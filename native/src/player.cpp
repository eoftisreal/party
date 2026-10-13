#include "party/player.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <stdexcept>
namespace party {
namespace {
void require(int r, const char* message) { if (r < 0) throw std::runtime_error(message); }
struct CodecDelete { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FrameDelete { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using Codec = std::unique_ptr<AVCodecContext, CodecDelete>;
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
Codec openDecoder(AVCodecID id) {
    const auto* codec = avcodec_find_decoder(id); if (!codec) throw std::runtime_error("Missing receive decoder");
    Codec ctx(avcodec_alloc_context3(codec)); if (!ctx) throw std::bad_alloc();
    ctx->pkt_timebase = AVRational{1,1000000};
    ctx->thread_count = 2; ctx->max_pixels = 1920*1080;
    if (id == AV_CODEC_ID_OPUS) {
        ctx->sample_rate = 48000; av_channel_layout_default(&ctx->ch_layout, 2);
        // Live Opus RTP has no Ogg container, so provide channel layout without container pre-skip.
        const uint8_t head[] = {'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0};
        ctx->extradata = static_cast<uint8_t*>(av_mallocz(sizeof(head) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx->extradata) throw std::bad_alloc();
        std::copy(std::begin(head), std::end(head), ctx->extradata); ctx->extradata_size = sizeof head;
    }
    require(avcodec_open2(ctx.get(), codec, nullptr), "Cannot open receive decoder"); return ctx;
}
}
struct Player::Impl {
    Player& owner;
    bool headless;
    Codec video = openDecoder(AV_CODEC_ID_H264), audio = openDecoder(AV_CODEC_ID_OPUS);
    SwrContext* swr = nullptr;
    SwsContext* sws = nullptr;
    SDL_AudioDeviceID device = 0;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int textureWidth = 0, textureHeight = 0;
    mutable std::mutex mutex;
    std::deque<EncodedFrame> incoming;
    std::deque<Json> controls;
    std::deque<int64_t> outstandingPings;
    size_t incomingBytes = 0;
    struct Video { Frame frame; int64_t pts; };
    struct Audio { std::vector<float> pcm; int64_t pts; };
    std::deque<Video> videos;
    std::deque<Audio> audios;
    PlaybackState state;
    bool haveState = false, buffering = true, waitingVideoKeyframe = true;
    std::atomic<bool> requestKeyframe{true};
    int64_t audioEndUs = 0, lastAudioPacketUs = 0, lastMediaUs = 0;
    uint64_t decodedVideo = 0, decodedAudio = 0, renderedVideo = 0, droppedVideo = 0, rebufferings = 0, decodeErrors = 0, concealedSamples = 0;
    int64_t avDriftUs = 0, maxAvDriftUs = 0;
    uint64_t videoChecksum = 0;
    double decodedAudioEnergy = 0;
    static constexpr int64_t bufferUs = 350000;
    Impl(Player& player, bool hidden) : owner(player), headless(hidden) {
        if (SDL_Init(SDL_INIT_AUDIO | (hidden ? 0 : SDL_INIT_VIDEO)) != 0) throw std::runtime_error(SDL_GetError());
        SDL_AudioSpec desired{}; desired.freq = 48000; desired.format = AUDIO_F32SYS; desired.channels = 2; desired.samples = 960;
        device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
        if (!device) { SDL_Quit(); throw std::runtime_error("Cannot open 48kHz stereo float audio output"); }
        if (!headless) {
            window = SDL_CreateWindow("Party — live WebRTC viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (window) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
            if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (!window || !renderer) {
                if (renderer) SDL_DestroyRenderer(renderer);
                if (window) SDL_DestroyWindow(window);
                SDL_CloseAudioDevice(device); SDL_Quit(); throw std::runtime_error("Cannot create viewer window");
            }
        }
        SDL_PauseAudioDevice(device, 0);
    }
    ~Impl() {
        SDL_CloseAudioDevice(device);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
                if (window) SDL_DestroyWindow(window);
        SDL_Quit(); swr_free(&swr); sws_freeContext(sws);
    }
    void flush() {
        SDL_ClearQueuedAudio(device); avcodec_flush_buffers(video.get()); avcodec_flush_buffers(audio.get());
        videos.clear(); audios.clear(); swr_free(&swr); audioEndUs = lastAudioPacketUs = 0; buffering = true; waitingVideoKeyframe = true; requestKeyframe = true;
    }
    void decode(EncodedFrame encoded) {
        if (!haveState || encoded.transportUs < state.cutoffUs || encoded.data.empty() || encoded.data.size() > 4*1024*1024) return;
        if (encoded.video && waitingVideoKeyframe) {
            bool idr = false;
            for (size_t i = 0; i + 4 < encoded.data.size(); ++i) {
                if (encoded.data[i] == 0 && encoded.data[i+1] == 0) {
                    const size_t header = encoded.data[i+2] == 1 ? i+3 : encoded.data[i+2] == 0 && encoded.data[i+3] == 1 ? i+4 : encoded.data.size();
                    if (header < encoded.data.size() && (encoded.data[header] & 31) == 5) { idr = true; break; }
                }
            }
            if (!idr) { requestKeyframe = true; return; }
            waitingVideoKeyframe = false;
        }
        auto* ctx = encoded.video ? video.get() : audio.get();
        Packet pkt(av_packet_alloc()); Frame output(av_frame_alloc()); if (!pkt || !output) throw std::bad_alloc();
        require(av_new_packet(pkt.get(), int(encoded.data.size())), "Packet allocation failed");
        std::copy(encoded.data.begin(), encoded.data.end(), pkt->data); pkt->pts = pkt->dts = encoded.transportUs;
        if (avcodec_send_packet(ctx, pkt.get()) < 0) { ++decodeErrors; requestKeyframe = true; if (encoded.video) waitingVideoKeyframe = true; return; }
        while (true) {
            const auto result = avcodec_receive_frame(ctx, output.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
            if (result < 0) { ++decodeErrors; requestKeyframe = true; if (encoded.video) waitingVideoKeyframe = true; break; }
            const auto pts = output->pts == AV_NOPTS_VALUE ? encoded.transportUs : output->pts;
            if (encoded.video) {
                if (output->width > 1920 || output->height > 1080 || output->width <= 0 || output->height <= 0) throw std::runtime_error("Unexpected receive dimensions");
                if (videos.size() >= 15) { videos.pop_front(); ++droppedVideo; }
                Frame clone(av_frame_clone(output.get())); if (!clone) throw std::bad_alloc();
                videos.push_back({std::move(clone), pts}); ++decodedVideo;
                // Content-derived evidence for tests: decoded luma, never simulated frame counters.
                for (int y = 0; y < output->height; y += 32) for (int x = 0; x < output->width; x += 32)
                    videoChecksum = videoChecksum * 131 + output->data[0][y*output->linesize[0]+x];
            } else {
                if (!swr) {
                    AVChannelLayout stereo; av_channel_layout_default(&stereo, 2);
                    const auto r = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &output->ch_layout, AVSampleFormat(output->format), output->sample_rate, 0, nullptr);
                    av_channel_layout_uninit(&stereo); require(r, "Receive resampler allocation failed"); require(swr_init(swr), "Receive resampler init failed");
                }
                if (output->nb_samples <= 0 || output->nb_samples > 5760) throw std::runtime_error("Invalid Opus audio size");
                const int capacity = swr_get_out_samples(swr, output->nb_samples);
                if (capacity <= 0 || capacity > 12000) throw std::runtime_error("Receive audio buffer overflow");
                std::vector<float> pcm(size_t(capacity)*2); uint8_t* dest = reinterpret_cast<uint8_t*>(pcm.data());
                const int count = swr_convert(swr, &dest, capacity, const_cast<const uint8_t**>(output->extended_data), output->nb_samples);
                require(count, "Receive audio resampling failed"); pcm.resize(size_t(count)*2);
                for (const auto sample : pcm) if (std::isfinite(sample)) decodedAudioEnergy += double(sample)*sample;
                if (audios.size() >= 50) { audios.pop_front(); ++rebufferings; }
                audios.push_back({std::move(pcm), pts}); ++decodedAudio;
            }
            lastMediaUs = nowUs(); av_frame_unref(output.get());
        }
    }
    int64_t audioClock() const { return audioEndUs - int64_t(SDL_GetQueuedAudioSize(device) / (sizeof(float)*2))*1000000/48000; }
    void render(Video& v) {
        if (!headless) {
            auto* f = v.frame.get();
            if (!texture || textureWidth != f->width || textureHeight != f->height) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
                if (!texture) throw std::runtime_error("Viewer texture allocation failed");
                textureWidth = f->width; textureHeight = f->height;
            }
            std::vector<uint8_t> rgba(size_t(f->width)*f->height*4);
            uint8_t* data = rgba.data(); const int stride = f->width*4;
            sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), f->width, f->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) throw std::runtime_error("Viewer scaler allocation failed");
            sws_scale(sws, f->data, f->linesize, 0, f->height, &data, &stride);
            SDL_UpdateTexture(texture, nullptr, rgba.data(), stride);
            int width = 0, height = 0; SDL_GetRendererOutputSize(renderer, &width, &height);
            const double scale = std::min(double(width)/f->width, double(height)/f->height);
            SDL_Rect target{(width-int(f->width*scale))/2, (height-int(f->height*scale))/2, int(f->width*scale), int(f->height*scale)};
            SDL_SetRenderDrawColor(renderer, 0,0,0,255); SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, nullptr, &target); SDL_RenderPresent(renderer);
        }
        ++renderedVideo;
    }
    bool tick() {
        SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) return false;
        std::deque<EncodedFrame> frames; std::deque<Json> messages;
        { std::lock_guard lock(mutex); frames.swap(incoming); messages.swap(controls); incomingBytes = 0; }
        for (const auto& j : messages) {
            try {
                const auto type = j.value("type", "");
                if (type == "pong") {
                    const int64_t t0 = j.at("t0"); bool expected = false;
                    { std::lock_guard lock(mutex); const auto found = std::find(outstandingPings.begin(), outstandingPings.end(), t0);
                      if (found != outstandingPings.end()) { outstandingPings.erase(found); expected = true; } }
                    if (expected) owner.clock.sample(t0, j.at("t1"), j.at("t2"), j.at("t3"));
                }
                else if (type == "state") {
                    const auto next = PlaybackState::parse(j);
                    if (haveState && next.epoch < state.epoch) continue;
                    if (!haveState || next.epoch != state.epoch) flush();
                    state = next; haveState = true; owner.hostAnchorUs = next.hostNowUs; owner.cutoffUs=next.cutoffUs;
                }
            } catch (const std::exception&) { /* malformed control cannot alter playback */ }
        }
        std::stable_sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) { return a.transportUs < b.transportUs; });
        for (auto& frame : frames) decode(std::move(frame));
        if (!haveState || !state.playing || !owner.clock.ready()) { SDL_ClearQueuedAudio(device); return true; }
        const auto now = nowUs();
        if (lastMediaUs && now-lastMediaUs > 1000000) { if (!buffering) ++rebufferings; buffering = true; requestKeyframe = true; }
        if (SDL_GetQueuedAudioSize(device) == 0 && !audios.empty()) {
            if (!buffering && audioEndUs) { ++rebufferings; buffering = true; }
            if (buffering && audios.size() >= 4 && !videos.empty()) {
                const auto target = owner.clock.localTime(audios.front().pts) + bufferUs;
                if (now >= target) { buffering = false; audioEndUs = 0; }
            }
        }
        if (buffering) return true;
        while (!audios.empty() && SDL_GetQueuedAudioSize(device) < 48000*2*sizeof(float)/5) {
            auto& block = audios.front();
            if (!audioEndUs && now < owner.clock.localTime(block.pts) + bufferUs) break;
            if (audioEndUs && block.pts < audioEndUs - 10000) { audios.pop_front(); continue; }
            if (audioEndUs && block.pts - audioEndUs > 100000) {
                SDL_ClearQueuedAudio(device); audioEndUs = 0; buffering = true; ++rebufferings; requestKeyframe = true; break;
            }
            if (audioEndUs && block.pts > audioEndUs + 1000) {
                const int gap = int((block.pts-audioEndUs)*48000/1000000);
                std::vector<float> silence(size_t(gap)*2);
                if (SDL_QueueAudio(device, silence.data(), uint32_t(silence.size()*sizeof(float))) < 0) throw std::runtime_error("Queue concealment audio failed");
                concealedSamples += uint64_t(gap);
            }
            if (SDL_QueueAudio(device, block.pcm.data(), uint32_t(block.pcm.size()*sizeof(float))) < 0) throw std::runtime_error("Queue audio output failed");
            audioEndUs = block.pts + int64_t(block.pcm.size()/2)*1000000/48000;
            lastAudioPacketUs = block.pts; audios.pop_front();
        }
        const auto master = audioClock();
        if (audioEndUs && swr) {
            const int64_t ideal = now + owner.clock.offsetUs() - bufferUs;
            const int64_t drift = master - ideal;
            if (std::abs(drift) > 20000 && std::abs(drift) < 250000) {
                // <=0.5% audio rate correction; no decoder restart for small drift.
                const int compensation = int(std::clamp<int64_t>(drift*48000/1000000, -240, 240));
                swr_set_compensation(swr, compensation, 48000);
            }
        }
        while (!videos.empty() && audioEndUs) {
            const int64_t drift = videos.front().pts - master;
            if (drift > 20000) break;
            if (drift < -60000) { videos.pop_front(); ++droppedVideo; continue; }
            avDriftUs = drift; maxAvDriftUs = std::max(maxAvDriftUs, std::abs(drift));
            render(videos.front()); videos.pop_front();
        }
        return true;
    }
};
Player::Player(bool hidden) : impl_(std::make_unique<Impl>(*this, hidden)) {}
Player::~Player() = default;
void Player::push(EncodedFrame frame) {
    std::lock_guard lock(impl_->mutex);
    if (frame.data.size() > 4*1024*1024 || impl_->incomingBytes + frame.data.size() > 8*1024*1024 || impl_->incoming.size() >= 256) { impl_->requestKeyframe = true; return; }
    impl_->incomingBytes += frame.data.size(); impl_->incoming.push_back(std::move(frame));
}
void Player::control(const Json& message) {
    std::lock_guard lock(impl_->mutex); if (impl_->controls.size() < 64) impl_->controls.push_back(message);
}
Json Player::ping() {
    const auto time = nowUs();
    std::lock_guard lock(impl_->mutex); if (impl_->outstandingPings.size() >= 8) impl_->outstandingPings.pop_front();
    impl_->outstandingPings.push_back(time); return {{"type", "ping"}, {"t0", time}};
}
void Player::reset() {
    { std::lock_guard lock(impl_->mutex); impl_->incoming.clear(); impl_->controls.clear(); impl_->outstandingPings.clear(); impl_->incomingBytes = 0; }
    impl_->flush(); impl_->haveState = false; hostAnchorUs = 0; cutoffUs=0; clock.reset();
}
bool Player::tick() { return impl_->tick(); }
bool Player::needsKeyframe() { return impl_->requestKeyframe.exchange(false); }
Json Player::stats() const {
    return {{"decodedVideoFrames", impl_->decodedVideo}, {"decodedAudioPackets", impl_->decodedAudio}, {"renderedVideoFrames", impl_->renderedVideo},
        {"droppedVideoFrames", impl_->droppedVideo}, {"rebufferings", impl_->rebufferings}, {"decodeErrors", impl_->decodeErrors}, {"concealedSamples", impl_->concealedSamples},
        {"audioEnergy", impl_->decodedAudioEnergy}, {"videoChecksum", impl_->videoChecksum}, {"avDriftUs", impl_->avDriftUs}, {"maxAvDriftUs", impl_->maxAvDriftUs}, {"audioQueuedBytes", SDL_GetQueuedAudioSize(impl_->device)},
        {"buffering", impl_->buffering}, {"clockOffsetUs", clock.offsetUs()}, {"clockRttUs", clock.rttUs()}, {"epoch", impl_->state.epoch}, {"movieUs", impl_->state.movieUs}};
}
}
