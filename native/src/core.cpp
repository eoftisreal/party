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
    if (!ready_ || rtt <= rtt_ || t3-lastBest_>30000000) {
        const auto next=((t1-t0)+(t2-t3))/2;
        offset_=ready_?offset_+std::clamp<int64_t>(next-offset_,-5000,5000):next;
        rtt_=rtt;lastBest_=t3;ready_=true;
    }
    return true;
}
void ClockEstimator::reset() {std::lock_guard lock(mutex_);offset_=rtt_=lastBest_=0;ready_=false;}
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
void PlayoutBudget::observe(int64_t arrival, int64_t presentation) {
    if (ready_ && presentation > presentation_ && arrival >= arrival_) {
        const auto elapsed = arrival - arrival_, media = presentation - presentation_;
        // Ignore discontinuities, pauses and duplicate/reordered timestamps.
        if (elapsed < 1000000 && media < 1000000) {
            const auto delta = std::min<int64_t>(250000, std::abs(elapsed - media));
            variation_ += (delta - variation_) / 16;
            constexpr double decay = 0.97; // Approx. 33 packet effective window.
            for (auto& weight : delayBuckets_) weight *= decay;
            const auto bucket = std::min<size_t>(delayBuckets_.size() - 1, size_t(delta / 8000));
            delayBuckets_[bucket] += 1.0;
            delayWeight_ = delayWeight_ * decay + 1.0;
            if (delaySamples_ < 32) ++delaySamples_;
        }
    }
    if (!ready_ || presentation > presentation_) { arrival_ = arrival; presentation_ = presentation; ready_ = true; }
}
int64_t PlayoutBudget::targetUs() const {
    int64_t burstUs = 0;
    // Wait for a representative sample window; a single startup packet must
    // not force the maximum delay. Audio packets arrive roughly every 20 ms.
    if (delaySamples_ >= 32 && delayWeight_ > 0.0) {
        const auto threshold = 0.95 * delayWeight_;
        double cumulative = 0.0;
        for (size_t i = 0; i < delayBuckets_.size(); ++i) {
            cumulative += delayBuckets_[i];
            if (cumulative >= threshold) { burstUs = int64_t(i) * 8000; break; }
        }
    }
    // Retain the existing movie-friendly 350-600 ms bounds. Only select a
    // new target while buffering; uninterrupted playback keeps its anchor.
    return std::clamp<int64_t>(350000 + std::max(4 * variation_, 2 * burstUs), 350000, 600000);
}
void PlayoutBudget::reset() {
    arrival_ = presentation_ = variation_ = 0;
    delayBuckets_.fill(0.0);
    delayWeight_ = 0.0;
    delaySamples_ = 0;
    ready_ = false;
}
BitrateController::BitrateController(unsigned initial, unsigned maximum) : bitrate_(initial), maximum_(maximum) {
    if (initial < 300000 || initial > maximum || maximum > 12000000)
        throw std::invalid_argument("Bitrate must satisfy 300000 <= initial <= maximum <= 12000000");
}
unsigned BitrateController::update(double loss, int64_t jitter, int64_t queue) {
    if (!std::isfinite(loss) || loss < 0 || loss > 1) return bitrate_;
    if (loss > .02 || jitter > 60000 || queue > 400000) { bitrate_ = std::max(300000u, bitrate_ * 3 / 4); clean_ = 0; }
    else if (++clean_ >= 3) { bitrate_ = std::min(maximum_, bitrate_ + std::max(150000u, bitrate_ / 10)); clean_ = 0; }
    return bitrate_;
}
}
