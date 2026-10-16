#include "party/media.hpp"
#include "party/color.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <condition_variable>
#include <cmath>
#include <iostream>
#include <thread>
#include <stdexcept>
namespace party {
namespace {
std::string avError(int result) { char buf[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result, buf, sizeof buf); return buf; }
void check(int result, const char* operation) { if (result < 0) throw std::runtime_error(std::string(operation) + ": " + avError(result)); }
struct CodecDeleter { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FrameDeleter { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FormatDeleter { void operator()(AVFormatContext* f) const { avformat_close_input(&f); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
FramePtr newFrame() { FramePtr f(av_frame_alloc()); if (!f) throw std::bad_alloc(); return f; }
PacketPtr newPacket() { PacketPtr p(av_packet_alloc()); if (!p) throw std::bad_alloc(); return p; }
CodecPtr decoder(AVStream* stream) {
    const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id); if (!codec) throw std::runtime_error("No input decoder");
    CodecPtr ctx(avcodec_alloc_context3(codec)); if (!ctx) throw std::bad_alloc();
    check(avcodec_parameters_to_context(ctx.get(), stream->codecpar), "decoder parameters");
    ctx->pkt_timebase = stream->time_base; ctx->thread_count = 2;
    check(avcodec_open2(ctx.get(), codec, nullptr), "open input decoder"); return ctx;
}
}
struct MediaSource::Impl {
    MediaOptions options;
    std::function<void(EncodedFrame)> sink;
    FormatPtr format;
    CodecPtr videoDecoder, audioDecoder, videoEncoder, audioEncoder;
    SdrColorConverter colorConverter;
    SwrContext* resampler = nullptr;
    AVAudioFifo* fifo = nullptr;
    FramePtr videoFrame = newFrame();
    int videoIndex = -1, audioIndex = -1;
    int64_t startUs = 0, durationUs = 0, movieBaseUs = 0, transportBaseUs = 0, fifoMovieUs = 0, minMovieUs = 0;
    int64_t lastVideoUs = -1, lastRateChangeUs = 0;
    mutable std::mutex mutex;
    std::condition_variable cv;
    PlaybackState playback;
    bool wantedPlaying = false, stopped = false;
    uint64_t commandVersion = 0, appliedVersion = 0;
    std::optional<int64_t> wantedSeek;
    std::string failure, encoderName;
    std::jthread thread;
    std::atomic<bool> forceKeyframe{true};
    std::atomic<unsigned> targetRate;
    std::atomic<unsigned> activeBitrate{0}, encoderOpens{0};
    std::atomic<uint64_t> videoFrames{0}, audioFrames{0}, encodedBytes{0}, colorConvertedFrames{0};
    Impl(const std::string& path, MediaOptions opts, std::function<void(EncodedFrame)> callback)
        : options(opts), sink(std::move(callback)), targetRate(opts.bitrate) {
        BitrateController validate(options.bitrate, options.maxBitrate);
        if (options.width < 2 || options.width > 1920 || options.height < 2 || options.height > 1080)
            throw std::invalid_argument("Media dimensions must fit 1920x1080");
        AVFormatContext* raw = nullptr;
        // Explicitly local files only: input protocols cannot turn the prototype into a remote fetch service.
        AVDictionary* dict = nullptr;
        av_dict_set(&dict, "protocol_whitelist", "file", 0);
        const auto result = avformat_open_input(&raw, path.c_str(), nullptr, &dict); av_dict_free(&dict);
        format.reset(raw); check(result, "open local movie");
        check(avformat_find_stream_info(format.get(), nullptr), "read movie streams");
        videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (videoIndex < 0 || audioIndex < 0) throw std::runtime_error("Prototype requires both video and audio streams");
        videoDecoder = decoder(format->streams[videoIndex]); audioDecoder = decoder(format->streams[audioIndex]);
        if (videoDecoder->color_trc == AVCOL_TRC_SMPTE2084 || videoDecoder->color_trc == AVCOL_TRC_ARIB_STD_B67)
            throw std::runtime_error("HDR tone mapping is not supported by the SDR prototype");
        startUs = format->start_time == AV_NOPTS_VALUE ? 0 : format->start_time;
        durationUs = format->duration == AV_NOPTS_VALUE ? 0 : format->duration;
        if (videoDecoder->width <= 0 || videoDecoder->height <= 0 || videoDecoder->width > 8192 || videoDecoder->height > 8192)
            throw std::runtime_error("Unsupported input dimensions");
        const auto scale = std::min({1.0, double(options.width) / videoDecoder->width, double(options.height) / videoDecoder->height});
        options.width = std::max(2, int(videoDecoder->width * scale) & ~1);
        options.height = std::max(2, int(videoDecoder->height * scale) & ~1);
        openVideoEncoder(); openAudioEncoder();
        playback.cutoffUs = nowUs(); playback.hostNowUs = nowUs();
        thread = std::jthread([this](std::stop_token token) {
            try { run(token); } catch (const std::exception& e) {
                std::lock_guard lock(mutex); failure = e.what(); playback.playing = false; playback.stopped = true;
            }
        });
    }
    ~Impl() {
        thread.request_stop(); cv.notify_all(); if (thread.joinable()) thread.join();
        swr_free(&resampler); if (fifo) av_audio_fifo_free(fifo);
    }
    void openVideoEncoder() {
        const std::vector<std::string> names = options.hardware ? std::vector<std::string>{"h264_nvenc", "h264_qsv", "h264_amf", "libx264"} : std::vector<std::string>{"libx264"};
        CodecPtr selected;
        for (const auto& name : names) {
            const auto* codec = avcodec_find_encoder_by_name(name.c_str()); if (!codec) continue;
            CodecPtr ctx(avcodec_alloc_context3(codec)); if (!ctx) throw std::bad_alloc();
            ctx->width = options.width; ctx->height = options.height;
            ctx->pix_fmt = name == "libx264" ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
            ctx->time_base = AVRational{1, 90000}; ctx->framerate = AVRational{30, 1};
            ctx->bit_rate = targetRate; ctx->rc_max_rate = targetRate; ctx->rc_buffer_size = int(targetRate / 4);
            ctx->thread_count = 4;
            ctx->gop_size = 30; ctx->max_b_frames = 0; ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP; ctx->profile = FF_PROFILE_H264_BASELINE; ctx->level = 40;
            ctx->color_range = AVCOL_RANGE_MPEG; ctx->color_primaries = AVCOL_PRI_BT709; ctx->color_trc = AVCOL_TRC_BT709; ctx->colorspace = AVCOL_SPC_BT709;
            if (name == "libx264") {
                av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0); av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->priv_data, "x264-params", "repeat-headers=1:scenecut=0:force-cfr=0", 0);
            } else if (name == "h264_nvenc") {
                av_opt_set(ctx->priv_data, "preset", "p1", 0); av_opt_set(ctx->priv_data, "tune", "ull", 0);
                av_opt_set(ctx->priv_data, "zerolatency", "1", 0); av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
            } else if (name == "h264_qsv") av_opt_set(ctx->priv_data, "async_depth", "1", 0);
            else av_opt_set(ctx->priv_data, "usage", "ultralowlatency", 0);
            if (avcodec_open2(ctx.get(), codec, nullptr) < 0) continue;
            selected = std::move(ctx); { std::lock_guard lock(mutex); encoderName = name; } break;
        }
        if (!selected) throw std::runtime_error("No usable H264 encoder (FFmpeg build needs libx264 fallback)");
        videoEncoder = std::move(selected);
        activeBitrate = unsigned(videoEncoder->bit_rate); ++encoderOpens;
        videoFrame = newFrame(); videoFrame->format = videoEncoder->pix_fmt; videoFrame->width = options.width; videoFrame->height = options.height;
        check(av_frame_get_buffer(videoFrame.get(), 32), "video encode buffer");
        forceKeyframe = true;
    }
    void openAudioEncoder() {
        const auto* codec = avcodec_find_encoder_by_name("libopus"); if (!codec) throw std::runtime_error("FFmpeg build needs libopus");
        audioEncoder.reset(avcodec_alloc_context3(codec)); if (!audioEncoder) throw std::bad_alloc();
        audioEncoder->sample_rate = 48000; audioEncoder->sample_fmt = AV_SAMPLE_FMT_FLT;
        av_channel_layout_default(&audioEncoder->ch_layout, 2);
        audioEncoder->bit_rate = 96000; audioEncoder->time_base = AVRational{1,48000};
        av_opt_set(audioEncoder->priv_data, "application", "audio", 0); av_opt_set(audioEncoder->priv_data, "frame_duration", "20", 0);
        av_opt_set(audioEncoder->priv_data, "packet_loss", "10", 0); av_opt_set(audioEncoder->priv_data, "fec", "1", 0);
        check(avcodec_open2(audioEncoder.get(), codec, nullptr), "open Opus encoder");
        if (fifo) av_audio_fifo_free(fifo);
        fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, 960); if (!fifo) throw std::bad_alloc();
        swr_free(&resampler);
    }
    bool interrupted(std::stop_token token) {
        std::lock_guard lock(mutex); return token.stop_requested() || commandVersion != appliedVersion;
    }
    bool pace(int64_t movieUs, std::stop_token token) {
        const auto target = transportBaseUs + movieUs - movieBaseUs;
        while (nowUs() < target) {
            std::unique_lock lock(mutex);
            if (token.stop_requested() || commandVersion != appliedVersion) return false;
            cv.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(10000, target - nowUs())));
        }
        return !interrupted(token);
    }
    void applyCommands() {
        std::optional<int64_t> seek;
        {
            std::lock_guard lock(mutex);
            if (commandVersion == appliedVersion) return;
            seek = wantedSeek; wantedSeek.reset();
            const auto current = playback.playing ? std::max<int64_t>(0, nowUs() - transportBaseUs + movieBaseUs) : playback.movieUs;
            movieBaseUs = seek.value_or(current);
            if (durationUs > 0) movieBaseUs = std::min(movieBaseUs, durationUs);
            transportBaseUs = nowUs() + 150000;
            playback.movieUs = movieBaseUs; playback.cutoffUs = transportBaseUs;
            playback.playing = wantedPlaying; playback.stopped = stopped;
            ++playback.epoch; appliedVersion = commandVersion;
        }
        if (seek) {
            const auto target = movieBaseUs + startUs;
            check(avformat_seek_file(format.get(), -1, INT64_MIN, target, target, AVSEEK_FLAG_BACKWARD), "seek movie");
            avcodec_flush_buffers(videoDecoder.get()); avcodec_flush_buffers(audioDecoder.get());
            openVideoEncoder(); openAudioEncoder(); minMovieUs = movieBaseUs; lastVideoUs = -1;
        } else {
            // Audio buffered before pause/resume must not be sent under the new clock epoch.
            av_audio_fifo_reset(fifo); swr_free(&resampler); forceKeyframe = true;
        }
    }
    int64_t movieTime(AVFrame* frame, int index) {
        if (frame->best_effort_timestamp == AV_NOPTS_VALUE) throw std::runtime_error("Input media has no usable timestamps");
        return av_rescale_q(frame->best_effort_timestamp, format->streams[index]->time_base, AV_TIME_BASE_Q) - startUs;
    }
    void encoded(AVCodecContext* encoder, bool video) {
        auto packet = newPacket();
        while (true) {
            const auto result = avcodec_receive_packet(encoder, packet.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            check(result, "encode media packet");
            if (packet->pts == AV_NOPTS_VALUE) throw std::runtime_error("Encoder returned missing PTS");
            const auto ptsUs = av_rescale_q(packet->pts, encoder->time_base, AV_TIME_BASE_Q);
            sink({video, ptsUs, {packet->data, packet->data + packet->size}});
            encodedBytes += uint64_t(packet->size); if (video) ++videoFrames; else ++audioFrames;
            av_packet_unref(packet.get());
        }
    }
    void video(AVFrame* frame, std::stop_token token) {
        const auto movieUs = movieTime(frame, videoIndex);
        if (movieUs < minMovieUs || movieUs < 0 || (lastVideoUs >= 0 && movieUs - lastVideoUs < 30000)) return;
        if (!pace(movieUs, token)) return;
        // Re-open to actually apply bitrate changes; many hardware encoders ignore live AVCodecContext writes.
        const auto requestedRate = targetRate.load();
        const auto activeRate = unsigned(videoEncoder->bit_rate);
        // Apply meaningful reductions quickly; coalesce small increases to avoid IDR/reopen churn.
        const bool reduce = requestedRate < activeRate && activeRate - requestedRate >= activeRate / 10;
        const bool increase = requestedRate > activeRate && requestedRate - activeRate >= activeRate / 5;
        if ((reduce && nowUs() - lastRateChangeUs > 1000000) ||
            (increase && nowUs() - lastRateChangeUs > 5000000)) { openVideoEncoder(); lastRateChangeUs = nowUs(); }
        check(av_frame_make_writable(videoFrame.get()), "writable video buffer");
        // Normalize the *pixel values* to limited-range BT.709 before tagging
        // the encoded stream. Source frame metadata overrides decoder defaults.
        if (colorConverter.convert(frame, videoFrame, videoDecoder->colorspace, videoDecoder->color_range))
            ++colorConvertedFrames;
        videoFrame->pts = av_rescale_q(transportBaseUs + movieUs - movieBaseUs, AV_TIME_BASE_Q, videoEncoder->time_base);
        videoFrame->pict_type = forceKeyframe.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        check(avcodec_send_frame(videoEncoder.get(), videoFrame.get()), "encode video"); encoded(videoEncoder.get(), true);
        lastVideoUs = movieUs;
    }
    void audio(AVFrame* frame, std::stop_token token) {
        auto movieUs = movieTime(frame, audioIndex);
        if (!resampler) {
            AVChannelLayout layout; av_channel_layout_default(&layout, 2);
            const int r = swr_alloc_set_opts2(&resampler, &layout, AV_SAMPLE_FMT_FLT, 48000, &frame->ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&layout); check(r, "audio resampler allocation"); check(swr_init(resampler), "audio resampler init");
        }
        const auto delay = swr_get_delay(resampler, frame->sample_rate);
        movieUs -= av_rescale(delay, 1000000, frame->sample_rate);
        const int capacity = int(av_rescale_rnd(delay + frame->nb_samples, 48000, frame->sample_rate, AV_ROUND_UP));
        if (capacity < 0 || capacity > 48000*2) throw std::runtime_error("Invalid decoded audio size");
        std::vector<float> samples(size_t(capacity)*2); uint8_t* output = reinterpret_cast<uint8_t*>(samples.data());
        const int count = swr_convert(resampler, &output, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        check(count, "resample audio");
        const int skip = int(std::clamp<int64_t>(av_rescale_rnd(std::max<int64_t>(0, minMovieUs-movieUs), 48000, 1000000, AV_ROUND_UP), 0, count));
        if (skip == count) return;
        if (av_audio_fifo_size(fifo) == 0) fifoMovieUs = movieUs + int64_t(skip)*1000000/48000;
        void* input = samples.data() + skip*2;
        if (av_audio_fifo_size(fifo) + count - skip > 96000) throw std::runtime_error("Audio FIFO overflow");
        check(av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + count - skip), "resize audio FIFO");
        if (av_audio_fifo_write(fifo, &input, count-skip) != count-skip) throw std::runtime_error("Audio FIFO write failed");
        while (av_audio_fifo_size(fifo) >= audioEncoder->frame_size) {
            if (!pace(fifoMovieUs, token)) return;
            auto out = newFrame(); out->nb_samples = audioEncoder->frame_size; out->format = audioEncoder->sample_fmt; out->sample_rate = 48000;
            check(av_channel_layout_copy(&out->ch_layout, &audioEncoder->ch_layout), "copy channel layout");
            check(av_frame_get_buffer(out.get(), 0), "audio encode buffer");
            if (av_audio_fifo_read(fifo, reinterpret_cast<void**>(out->data), out->nb_samples) != out->nb_samples) throw std::runtime_error("Audio FIFO read failed");
            // Offset encoder lookahead so the first Opus packet uses the same transport origin as video.
            out->pts = av_rescale_q(transportBaseUs + fifoMovieUs - movieBaseUs, AV_TIME_BASE_Q, audioEncoder->time_base) + audioEncoder->initial_padding;
            check(avcodec_send_frame(audioEncoder.get(), out.get()), "encode audio"); encoded(audioEncoder.get(), false);
            fifoMovieUs += int64_t(out->nb_samples)*1000000/48000;
        }
    }
    void run(std::stop_token token) {
        auto packet = newPacket(); auto frame = newFrame();
        while (!token.stop_requested()) {
            applyCommands();
            {
                std::unique_lock lock(mutex);
                if (!playback.playing) { cv.wait_for(lock, std::chrono::milliseconds(20)); continue; }
            }
            const auto result = av_read_frame(format.get(), packet.get());
            if (result == AVERROR_EOF) {
                // Flush delayed decoder frames and audio encoder; output remains paced.
                for (auto [context, index] : {std::pair{videoDecoder.get(), videoIndex}, std::pair{audioDecoder.get(), audioIndex}}) {
                    avcodec_send_packet(context, nullptr);
                    while (avcodec_receive_frame(context, frame.get()) >= 0) {
                        if (index == videoIndex) video(frame.get(), token); else audio(frame.get(), token); av_frame_unref(frame.get());
                    }
                }
                avcodec_send_frame(videoEncoder.get(), nullptr); encoded(videoEncoder.get(), true);
                avcodec_send_frame(audioEncoder.get(), nullptr); encoded(audioEncoder.get(), false);
                std::lock_guard lock(mutex); playback.movieUs = durationUs > 0 ? durationUs : std::max<int64_t>(0, nowUs()-transportBaseUs+movieBaseUs);
                playback.playing = wantedPlaying = false; playback.stopped = stopped = true; continue;
            }
            check(result, "read movie packet");
            AVCodecContext* context = packet->stream_index == videoIndex ? videoDecoder.get() : packet->stream_index == audioIndex ? audioDecoder.get() : nullptr;
            if (context) {
                check(avcodec_send_packet(context, packet.get()), "decode input packet");
                while (true) {
                    const auto decoded = avcodec_receive_frame(context, frame.get());
                    if (decoded == AVERROR(EAGAIN) || decoded == AVERROR_EOF) break;
                    check(decoded, "decode input frame");
                    if (context == videoDecoder.get()) video(frame.get(), token); else audio(frame.get(), token);
                    av_frame_unref(frame.get());
                }
            }
            av_packet_unref(packet.get());
        }
    }
};
MediaSource::MediaSource(const std::string& path, MediaOptions opts, std::function<void(EncodedFrame)> sink) : impl_(std::make_unique<Impl>(path, opts, std::move(sink))) {}
MediaSource::~MediaSource() = default;
void MediaSource::play() { std::lock_guard lock(impl_->mutex); if (impl_->stopped && !impl_->wantedSeek) impl_->wantedSeek = 0; impl_->wantedPlaying = true; impl_->stopped = false; ++impl_->commandVersion; impl_->cv.notify_all(); }
void MediaSource::pause() { std::lock_guard lock(impl_->mutex); impl_->wantedPlaying = false; ++impl_->commandVersion; impl_->cv.notify_all(); }
void MediaSource::seek(int64_t us) {
    if (us < 0) throw std::invalid_argument("Negative seek");
    std::lock_guard lock(impl_->mutex); impl_->wantedSeek = us; impl_->stopped = false; ++impl_->commandVersion; impl_->cv.notify_all();
}
void MediaSource::stop() { std::lock_guard lock(impl_->mutex); impl_->wantedSeek = 0; impl_->wantedPlaying = false; impl_->stopped = true; ++impl_->commandVersion; impl_->cv.notify_all(); }
void MediaSource::keyframe() { impl_->forceKeyframe = true; }
void MediaSource::bitrate(unsigned rate) { impl_->targetRate = std::clamp(rate, 300000u, impl_->options.maxBitrate); }
PlaybackState MediaSource::state() const {
    std::lock_guard lock(impl_->mutex); auto state = impl_->playback; state.hostNowUs = nowUs();
    if (state.playing) state.movieUs = std::max<int64_t>(0, state.hostNowUs - impl_->transportBaseUs + impl_->movieBaseUs);
    if (impl_->durationUs > 0) state.movieUs = std::min(state.movieUs, impl_->durationUs);
    return state;
}
Json MediaSource::stats() const {
    std::lock_guard lock(impl_->mutex);
    return {{"encoder", impl_->encoderName}, {"width", impl_->options.width}, {"height", impl_->options.height}, {"videoFrames", impl_->videoFrames.load()}, {"audioPackets", impl_->audioFrames.load()}, {"encodedBytes", impl_->encodedBytes.load()}, {"bitrate", impl_->targetRate.load()}, {"activeBitrate", impl_->activeBitrate.load()}, {"encoderOpens", impl_->encoderOpens.load()}, {"maxBitrate", impl_->options.maxBitrate},
            {"colorFormat", "bt709-limited"}, {"colorConvertedFrames", impl_->colorConvertedFrames.load()}};
}
std::string MediaSource::error() const { std::lock_guard lock(impl_->mutex); return impl_->failure; }
}
