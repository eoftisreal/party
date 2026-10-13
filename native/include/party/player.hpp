#pragma once
#include "party/core.hpp"
#include <memory>
namespace party {
class Player {
public:
    explicit Player(bool headless = false);
    ~Player();
    void push(EncodedFrame frame);
    void control(const Json& message);
    Json ping();
    void reset();
    bool tick(); // false when the user closes the SDL window
    Json stats() const;
    bool needsKeyframe();
    ClockEstimator clock;
    std::atomic<int64_t> hostAnchorUs{0};
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
