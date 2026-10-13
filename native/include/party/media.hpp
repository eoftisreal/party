#pragma once
#include "party/core.hpp"
#include <functional>
#include <memory>
namespace party {
struct MediaOptions { int width = 1920, height = 1080; bool hardware = true; unsigned bitrate = 2500000; };
class MediaSource {
public:
    MediaSource(const std::string& path, MediaOptions options, std::function<void(EncodedFrame)> sink);
    ~MediaSource();
    void play();
    void pause();
    void seek(int64_t movieUs);
    void stop();
    void keyframe();
    void bitrate(unsigned bitsPerSecond);
    PlaybackState state() const;
    Json stats() const;
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
