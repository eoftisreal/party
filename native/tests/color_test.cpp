#include "party/color.hpp"
extern "C" {
#include <libavutil/pixfmt.h>
}
#include <cmath>
#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
void expect(bool test, const char* error) {
    if (!test) throw std::runtime_error(error);
}
AVFrame* create(AVPixelFormat format, AVColorSpace matrix, AVColorRange range) {
    auto* f = av_frame_alloc();
    if (!f) throw std::bad_alloc();
    f->format = format; f->width = 64; f->height = 64;
    f->colorspace = matrix; f->color_range = range;
    if (av_frame_get_buffer(f, 32) < 0) throw std::runtime_error("Test frame allocation failed");
    return f;
}
void fill(AVFrame* f, uint8_t y, uint8_t u, uint8_t v) {
    for (int row = 0; row < f->height; ++row)
        for (int col = 0; col < f->width; ++col) f->data[0][row*f->linesize[0]+col] = y;
    for (int row = 0; row < f->height/2; ++row)
        for (int col = 0; col < f->width/2; ++col) {
            f->data[1][row*f->linesize[1]+col] = u;
            f->data[2][row*f->linesize[2]+col] = v;
        }
}
}
int main() {
    try {
        party::SdrColorConverter converter;
        auto* source = create(AV_PIX_FMT_YUV420P, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG);
        auto* output = create(AV_PIX_FMT_YUV420P, AVCOL_SPC_UNSPECIFIED, AVCOL_RANGE_UNSPECIFIED);
        fill(source, 100, 90, 180);
        expect(!converter.convert(source, output), "BT709 limited should keep the fast scaling path");
        expect(output->data[0][0] == 100 && output->data[1][0] == 90 && output->data[2][0] == 180,
               "BT709 limited source pixels changed unexpectedly");
        expect(output->colorspace == AVCOL_SPC_BT709 && output->color_range == AVCOL_RANGE_MPEG,
               "Output metadata does not match limited BT709 pixels");

        // Full range black and white must become 16/235 in limited range.
        source->color_range = AVCOL_RANGE_JPEG;
        fill(source, 0, 128, 128);
        expect(converter.convert(source, output), "Full-range input did not trigger normalization");
        expect(std::abs(int(output->data[0][0])-16) <= 2, "Full-range black was not mapped to video black");
        fill(source, 255, 128, 128);
        expect(converter.convert(source, output), "Full-range white was not normalized");
        expect(std::abs(int(output->data[0][0])-235) <= 2, "Full-range white was not mapped to video white");

        // BT601 colored pixels must not silently be *relabeled* as BT709.
        source->colorspace = AVCOL_SPC_SMPTE170M;
        source->color_range = AVCOL_RANGE_MPEG;
        fill(source, 100, 90, 180);
        expect(converter.convert(source, output), "BT601 needs a real pixel-matrix conversion");
        expect(std::abs(int(output->data[0][0])-100) > 2,
               "BT601 matrix conversion only changed tags, not pixels");
        expect(output->colorspace == AVCOL_SPC_BT709, "Converted frame matrix is not BT709");
        av_frame_free(&source); av_frame_free(&output);
        std::cout << "SDR color matrix/range regression checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
