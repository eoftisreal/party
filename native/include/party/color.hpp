#pragma once
extern "C" {
#include <libavutil/frame.h>
}
struct SwsContext;
namespace party {
// Resolve coded SDR metadata. Unspecified SD is assumed BT.601 and HD BT.709;
// explicit frame metadata always wins over the container/decoder fallback.
AVColorSpace sdrMatrix(const AVFrame* frame, AVColorSpace fallback = AVCOL_SPC_UNSPECIFIED);
AVColorRange sdrRange(const AVFrame* frame, AVColorRange fallback = AVCOL_RANGE_UNSPECIFIED);
int swsMatrixId(AVColorSpace matrix);

// Convert SDR source pixels into *actual* limited-range BT.709 pixels before
// feeding H.264, not merely relabeling BT.601 or full-range input as BT.709.
class SdrColorConverter {
public:
    SdrColorConverter() = default;
    ~SdrColorConverter();
    SdrColorConverter(const SdrColorConverter&) = delete;
    SdrColorConverter& operator=(const SdrColorConverter&) = delete;
    // Destination must have allocated YUV420P/NV12 buffers matching the encoder.
    // Returns true when an explicit RGB intermediate color conversion was needed.
    bool convert(const AVFrame* source, AVFrame* destination,
                 AVColorSpace fallbackMatrix = AVCOL_SPC_UNSPECIFIED,
                 AVColorRange fallbackRange = AVCOL_RANGE_UNSPECIFIED);
private:
    SwsContext* direct_ = nullptr;
    SwsContext* toRgb_ = nullptr;
    SwsContext* toYuv_ = nullptr;
    AVFrame* rgb_ = nullptr;
};
}
