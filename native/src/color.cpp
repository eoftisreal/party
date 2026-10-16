#include "party/color.hpp"
extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#include <stdexcept>
namespace party {
namespace {
void scaled(SwsContext* ctx, const AVFrame* from, AVFrame* to) {
    if (sws_scale(ctx, from->data, from->linesize, 0, from->height,
                  to->data, to->linesize) != to->height)
        throw std::runtime_error("SDR color conversion failed");
}
bool rgbPixels(const AVFrame* f) {
    const auto* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
}
}
AVColorSpace sdrMatrix(const AVFrame* f, AVColorSpace fallback) {
    if (f->colorspace != AVCOL_SPC_UNSPECIFIED && f->colorspace != AVCOL_SPC_RGB)
        return f->colorspace;
    if (fallback != AVCOL_SPC_UNSPECIFIED && fallback != AVCOL_SPC_RGB) return fallback;
    return f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
}
AVColorRange sdrRange(const AVFrame* f, AVColorRange fallback) {
    if (f->color_range != AVCOL_RANGE_UNSPECIFIED) return f->color_range;
    if (rgbPixels(f)) return AVCOL_RANGE_JPEG;
    // Legacy FFmpeg JPEG YUV formats imply full range even when not tagged.
    switch (AVPixelFormat(f->format)) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
        return AVCOL_RANGE_JPEG;
    default: break;
    }
    return fallback == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : fallback;
}
int swsMatrixId(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
    default: throw std::invalid_argument("Unsupported SDR YUV color matrix");
    }
}
SdrColorConverter::~SdrColorConverter() {
    sws_freeContext(direct_);
    sws_freeContext(toRgb_);
    sws_freeContext(toYuv_);
    av_frame_free(&rgb_);
}
bool SdrColorConverter::convert(const AVFrame* source, AVFrame* destination,
                                AVColorSpace fallbackMatrix, AVColorRange fallbackRange) {
    if (!source || !destination || source->width <= 0 || source->height <= 0 ||
        destination->width <= 0 || destination->height <= 0)
        throw std::invalid_argument("Invalid SDR frame dimensions");
    if (source->color_trc == AVCOL_TRC_SMPTE2084 || source->color_trc == AVCOL_TRC_ARIB_STD_B67)
        throw std::invalid_argument("HDR input needs tone mapping, not SDR conversion");
    const auto matrix = sdrMatrix(source, fallbackMatrix);
    const auto range = sdrRange(source, fallbackRange);
    const bool needsConversion = rgbPixels(source) || matrix != AVCOL_SPC_BT709 || range != AVCOL_RANGE_MPEG;
    constexpr int flags = SWS_BILINEAR;
    if (!needsConversion) {
        direct_ = sws_getCachedContext(direct_, source->width, source->height, AVPixelFormat(source->format),
                                       destination->width, destination->height, AVPixelFormat(destination->format),
                                       flags, nullptr, nullptr, nullptr);
        if (!direct_) throw std::runtime_error("Cannot allocate BT.709 scaler");
        scaled(direct_, source, destination);
    } else {
        // swscale YUV->YUV conversion may preserve the old YUV matrix even if
        // colorspace details are set. Decode through RGB to perform the real
        // BT.601/full -> BT.709/limited pixel transformation.
        if (!rgb_ || rgb_->width != destination->width || rgb_->height != destination->height) {
            av_frame_free(&rgb_);
            rgb_ = av_frame_alloc();
            if (!rgb_) throw std::bad_alloc();
            rgb_->format = AV_PIX_FMT_RGB24;
            rgb_->width = destination->width;
            rgb_->height = destination->height;
            if (av_frame_get_buffer(rgb_, 32) < 0) throw std::runtime_error("Cannot allocate color intermediate");
        }
        if (av_frame_make_writable(rgb_) < 0) throw std::runtime_error("Color intermediate is not writable");
        toRgb_ = sws_getCachedContext(toRgb_, source->width, source->height, AVPixelFormat(source->format),
                                      rgb_->width, rgb_->height, AV_PIX_FMT_RGB24, flags, nullptr, nullptr, nullptr);
        toYuv_ = sws_getCachedContext(toYuv_, rgb_->width, rgb_->height, AV_PIX_FMT_RGB24,
                                      destination->width, destination->height, AVPixelFormat(destination->format),
                                      flags, nullptr, nullptr, nullptr);
        if (!toRgb_ || !toYuv_) throw std::runtime_error("Cannot allocate SDR colorspace converters");
        const auto* bt709 = sws_getCoefficients(SWS_CS_ITU709);
        const auto* input = sws_getCoefficients(swsMatrixId(matrix));
        if (sws_setColorspaceDetails(toRgb_, input, range == AVCOL_RANGE_JPEG, bt709, 1, 0, 1<<16, 1<<16) < 0 ||
            sws_setColorspaceDetails(toYuv_, bt709, 1, bt709, 0, 0, 1<<16, 1<<16) < 0)
            throw std::runtime_error("Cannot configure SDR colorspace coefficients");
        scaled(toRgb_, source, rgb_);
        scaled(toYuv_, rgb_, destination);
    }
    // These tags must describe actual output samples, including after a seek
    // or encoder reopen. The encoder itself is configured with the same tags.
    destination->colorspace = AVCOL_SPC_BT709;
    destination->color_range = AVCOL_RANGE_MPEG;
    destination->color_primaries = AVCOL_PRI_BT709;
    destination->color_trc = AVCOL_TRC_BT709;
    destination->chroma_location = AVCHROMA_LOC_LEFT;
    return needsConversion;
}
}
