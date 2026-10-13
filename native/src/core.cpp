#include "party/core.hpp"
#include <openssl/rand.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
namespace party {
int64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string randomId() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof bytes) != 1) throw std::runtime_error("Secure random generator failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string out; out.reserve(32);
    for (auto b : bytes) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}
std::string env(const char* key) { const auto* value = std::getenv(key); return value ? value : ""; }
Json PlaybackState::json() const { return {{"type", "state"}, {"epoch", epoch}, {"cutoffUs", cutoffUs}, {"hostNowUs", hostNowUs}, {"movieUs", movieUs}, {"playing", playing}, {"stopped", stopped}}; }
PlaybackState PlaybackState::parse(const Json& j) {
    if (!j.at("epoch").is_number_unsigned()) throw std::invalid_argument("Invalid playback epoch");
    PlaybackState s{j.at("epoch").get<uint64_t>(), j.at("cutoffUs").get<int64_t>(), j.at("hostNowUs").get<int64_t>(), j.at("movieUs").get<int64_t>(), j.at("playing").get<bool>(), j.at("stopped").get<bool>()};
    if (s.movieUs < 0 || s.movieUs > 365LL*86400*1000000 || s.hostNowUs < 0 || s.hostNowUs > 9000000000000000LL || s.cutoffUs < 0 || s.cutoffUs > 9000000000000000LL || s.cutoffUs > s.hostNowUs + 5000000) throw std::invalid_argument("Invalid playback clock");
    return s;
}
bool ClockEstimator::sample(int64_t t0, int64_t t1, int64_t t2, int64_t t3) {
    constexpr int64_t maxClock = 9000000000000000LL;
    if (t3 < t0 || t2 < t1 || t0 < 0 || t1 < 0 || t0 > maxClock || t1 > maxClock || t2 > maxClock || t3 > maxClock) return false;
    const auto rtt = (t3 - t0) - (t2 - t1);
    if (rtt < 0 || rtt > 5000000) return false;
    std::lock_guard lock(mutex_);
    if (!ready_ || rtt <= rtt_) { offset_ = ((t1 - t0) + (t2 - t3)) / 2; rtt_ = rtt; ready_ = true; }
    return true;
}
int64_t ClockEstimator::localTime(int64_t hostUs) const { std::lock_guard lock(mutex_); return hostUs - offset_; }
int64_t ClockEstimator::offsetUs() const { std::lock_guard lock(mutex_); return offset_; }
int64_t ClockEstimator::rttUs() const { std::lock_guard lock(mutex_); return rtt_; }
bool ClockEstimator::ready() const { std::lock_guard lock(mutex_); return ready_; }
int64_t unwrapTimestamp(uint32_t timestamp, uint32_t rate, int64_t anchorUs) {
    if (!rate || anchorUs < 0) throw std::invalid_argument("Invalid RTP clock");
    const int64_t anchorTicks = (anchorUs / 1000000) * rate + (anchorUs % 1000000) * rate / 1000000;
    const int32_t delta = static_cast<int32_t>(timestamp - static_cast<uint32_t>(anchorTicks));
    return anchorUs + static_cast<int64_t>(delta) * 1000000 / rate;
}
Json NetworkStats::json() const { return {{"rtpPackets", received.load()}, {"lost", lost.load()}, {"duplicates", duplicates.load()}, {"nacks", nacks.load()}, {"malformed", malformed.load()}, {"frames", frames.load()}, {"jitterUs", jitterUs.load()}}; }
unsigned BitrateController::update(double loss, int64_t jitter, int64_t queue) {
    if (!std::isfinite(loss) || loss < 0 || loss > 1) return bitrate_;
    if (loss > .02 || jitter > 60000 || queue > 400000) { bitrate_ = std::max(300000u, bitrate_ * 3 / 4); clean_ = 0; }
    else if (++clean_ >= 3) { bitrate_ = std::min(5000000u, bitrate_ + 150000); clean_ = 0; }
    return bitrate_;
}
}
