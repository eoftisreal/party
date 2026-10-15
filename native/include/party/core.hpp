#pragma once
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace party {
using Json = nlohmann::json;
int64_t nowUs();
std::string randomId();
std::string env(const char* key);
struct EncodedFrame {
    bool video = false;
    int64_t transportUs = 0;
    std::vector<uint8_t> data;
};
struct PlaybackState {
    uint64_t epoch = 0;
    int64_t cutoffUs = 0;
    int64_t hostNowUs = 0;
    int64_t movieUs = 0;
    bool playing = false;
    bool stopped = false;
    Json json() const;
    static PlaybackState parse(const Json& j);
};
// NTP four-timestamp estimator; keeps the lowest-RTT sample over a renewing window, with bounded clock correction.
class ClockEstimator {
public:
    bool sample(int64_t t0, int64_t t1, int64_t t2, int64_t t3);
    int64_t localTime(int64_t hostUs) const;
    int64_t offsetUs() const;
    int64_t rttUs() const;
    bool ready() const;
    void reset();
private:
    mutable std::mutex mutex_;
    int64_t offset_ = 0, rtt_ = 0, lastBest_ = 0;
    bool ready_ = false;
};
// Map a wrapping RTP timestamp to the nearest timestamp around an authenticated host-clock anchor.
int64_t unwrapTimestamp(uint32_t timestamp, uint32_t clockRate, int64_t anchorUs);
struct NetworkStats {
    std::atomic<uint64_t> received{0}, lost{0}, duplicates{0}, nacks{0}, malformed{0}, frames{0};
    std::atomic<int64_t> jitterUs{0};
    Json json() const;
};
// Arrival variation measured on audio timestamps; caller serializes access.
class PlayoutBudget {
public:
    void observe(int64_t arrivalUs, int64_t presentationUs);
    int64_t targetUs() const;
    void reset();
private:
    int64_t arrival_ = 0, presentation_ = 0, variation_ = 0;
    bool ready_ = false;
};
class BitrateController {
public:
    explicit BitrateController(unsigned initial = 2500000, unsigned maximum = 5000000);
    unsigned update(double loss, int64_t jitterUs, int64_t queueUs);
    unsigned bitrate() const { return bitrate_; }
private:
    unsigned bitrate_ = 2500000;
    unsigned maximum_ = 5000000;
    unsigned clean_ = 0;
};
}
