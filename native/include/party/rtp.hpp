#pragma once
#include "party/core.hpp"
#include <rtc/rtc.hpp>
#include <functional>
#include <algorithm>
#include <map>
#include <span>
namespace party {
struct RtpPacket {
    uint16_t sequence;
    uint32_t timestamp, ssrc;
    uint8_t payloadType;
    bool marker;
    std::span<const uint8_t> payload;
};
std::optional<RtpPacket> parseRtp(std::span<const uint8_t> bytes);
// Bounded reorder buffer + NACK + strict H264 RFC6184 depacketization. No disk I/O.
class RtpReceiver final : public rtc::MediaHandler {
public:
    RtpReceiver(bool video, std::function<void(EncodedFrame)> frame, std::function<void()> keyframe);
    void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void pump(int64_t timeUs);
    void setCutoff(int64_t cutoff);
    void setReorderWindow(int64_t us) { reorderUs_=std::clamp<int64_t>(us,60000,250000); }
    void setAnchor(int64_t hostUs) { anchorUs_.store(hostUs); }
    std::shared_ptr<NetworkStats> stats = std::make_shared<NetworkStats>();
private:
    struct Entry { rtc::message_ptr message; int64_t arrival; };
    void deliver(const RtpPacket& p);
    void discardFrame();
    bool video_, initialized_ = false, damaged_ = false, fragment_ = false;
    int64_t highest_ = 0, expected_ = 0, gapStart_ = 0, lastNack_ = 0, transit_ = 0;
    uint32_t ssrc_ = 0, frameTimestamp_ = 0;
    std::atomic<int64_t> anchorUs_{0}, reorderUs_{60000};
    int64_t cutoffUs_=0;
    std::mutex mutex_;
    std::map<int64_t, Entry> pending_;
    std::vector<uint8_t> frame_;
    std::function<void(EncodedFrame)> emit_;
    std::function<void()> keyframe_;
    rtc::message_callback send_;
};
// Guard libdatachannel's RTCP session before its packed-header casts.
class PacketGuard final : public rtc::MediaHandler {
public:
    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override;
};
}
