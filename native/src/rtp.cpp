#include "party/rtp.hpp"
#include <algorithm>
#include <cstring>
namespace party {
namespace {
uint16_t u16(std::span<const uint8_t> b, size_t n) { return uint16_t((b[n] << 8) | b[n+1]); }
uint32_t u32(std::span<const uint8_t> b, size_t n) { return (uint32_t(b[n]) << 24) | (uint32_t(b[n+1]) << 16) | (uint32_t(b[n+2]) << 8) | b[n+3]; }
std::span<const uint8_t> bytes(const rtc::Message& m) { return {reinterpret_cast<const uint8_t*>(m.data()), m.size()}; }
}
std::optional<RtpPacket> parseRtp(std::span<const uint8_t> b) {
    if (b.size() < 12 || (b[0] >> 6) != 2) return {};
    size_t offset = 12 + 4 * (b[0] & 15);
    if (offset > b.size()) return {};
    if (b[0] & 16) { if (offset + 4 > b.size()) return {}; offset += 4 + 4 * u16(b, offset + 2); if (offset > b.size()) return {}; }
    size_t end = b.size();
    if (b[0] & 32) { const size_t padding = b.back(); if (!padding || padding > end - offset) return {}; end -= padding; }
    if (end <= offset) return {};
    return RtpPacket{u16(b, 2), u32(b, 4), u32(b, 8), uint8_t(b[1] & 127), bool(b[1] & 128), b.subspan(offset, end - offset)};
}
void PacketGuard::incoming(rtc::message_vector& messages, const rtc::message_callback&) {
    std::erase_if(messages, [](auto& m) {
        const auto b = bytes(*m);
        if (m->type == rtc::Message::Binary) return !parseRtp(b).has_value() || b.size() > 2048;
        if (m->type != rtc::Message::Control) return true;
        size_t offset = 0;
        while (offset < b.size()) {
            if (offset + 4 > b.size() || (b[offset] >> 6) != 2) return true;
            const size_t size = (size_t(u16(b, offset+2)) + 1) * 4;
            if (size < 8 || size > b.size() - offset) return true;
            const auto type = b[offset+1];
            const auto count = b[offset] & 31;
            if ((type == 200 && size < 28 + size_t(count)*24) || (type == 201 && size < 8 + size_t(count)*24) ||
                ((type == 205 || type == 206) && size < 12)) return true;
            offset += size;
        }
        return offset != b.size();
    });
}
RtpReceiver::RtpReceiver(bool video, std::function<void(EncodedFrame)> frame, std::function<void()> keyframe)
 : video_(video), emit_(std::move(frame)), keyframe_(std::move(keyframe)) {}
void RtpReceiver::incoming(rtc::message_vector& messages, const rtc::message_callback& send) {
    std::lock_guard lock(mutex_); send_ = send;
    for (auto& m : messages) {
        if (m->type != rtc::Message::Binary) continue;
        auto p = parseRtp(bytes(*m));
        if (!p || p->payloadType != (video_ ? 96 : 111)) { ++stats->malformed; continue; }
        const auto time = nowUs();
        const auto anchor=anchorUs_.load();
        const auto transport=anchor?unwrapTimestamp(p->timestamp,video_?90000:48000,anchor):0;
        if(anchor && transport<cutoffUs_) continue;
        if (!initialized_) { initialized_ = true; highest_ = expected_ = p->sequence; ssrc_ = p->ssrc; }
        if (p->ssrc != ssrc_) { ++stats->malformed; continue; }
        const int64_t seq = highest_ + int16_t(p->sequence - uint16_t(highest_));
        if (seq < expected_ || pending_.contains(seq)) { ++stats->duplicates; continue; }
        if (seq > expected_ + 1024 || pending_.size() >= 512) { ++stats->malformed; continue; }
        highest_ = std::max(highest_, seq);
        const auto transit = anchor?time-transport:0;
        if (transit_ && anchor) { const int64_t difference = std::abs(transit - transit_); const auto previous = stats->jitterUs.load(); stats->jitterUs = previous + (difference - previous) / 16; }
        transit_ = transit;
        pending_.emplace(seq, Entry{m, time}); ++stats->received;
    }
    messages.clear();
}
void RtpReceiver::pump(int64_t time) {
    std::lock_guard lock(mutex_);
    while (!pending_.empty()) {
        auto it = pending_.begin();
        if (it->first != expected_) {
            if (!gapStart_) gapStart_ = it->second.arrival;
            if (time - lastNack_ > 20000 && send_) {
                const auto missing = std::min<int64_t>(it->first - expected_, 16);
                auto message = rtc::make_message(rtc::RtcpNack::Size(unsigned(missing)), rtc::Message::Control);
                auto* nack = reinterpret_cast<rtc::RtcpNack*>(message->data());
                nack->preparePacket(ssrc_, unsigned(missing));
                for (int64_t k = 0; k < missing; ++k) { nack->parts[k].setPid(uint16_t(expected_ + k)); nack->parts[k].setBlp(0); }
                send_(message); lastNack_ = time; stats->nacks += uint64_t(missing);
            }
            if (time - gapStart_ < reorderUs_.load()) break;
            stats->lost += uint64_t(it->first - expected_); expected_ = it->first;
            if (video_) { damaged_ = true; fragment_ = false; keyframe_(); }
        }
        gapStart_ = 0;
        if (auto p = parseRtp(bytes(*it->second.message))) deliver(*p);
        pending_.erase(it); ++expected_;
    }
}
void RtpReceiver::setCutoff(int64_t cutoff) {
    std::lock_guard lock(mutex_);cutoffUs_=cutoff;pending_.clear();discardFrame();
    initialized_=false;gapStart_=lastNack_=transit_=0;
}
void RtpReceiver::discardFrame() { frame_.clear(); fragment_ = false; damaged_ = false; }
void RtpReceiver::deliver(const RtpPacket& p) {
    const auto anchor = anchorUs_.load();
    if (!anchor) return; // Wait for authenticated clock state; never guess a wrapping epoch.
    if (!video_) {
        emit_({false, unwrapTimestamp(p.timestamp, 48000, anchor), {p.payload.begin(), p.payload.end()}}); ++stats->frames; return;
    }
    if (!frame_.empty() && p.timestamp != frameTimestamp_) { discardFrame(); keyframe_(); }
    if (frame_.empty()) frameTimestamp_ = p.timestamp;
    const auto b = p.payload; const auto type = b[0] & 31;
    const auto separator = [&] { frame_.insert(frame_.end(), {0,0,0,1}); };
    if ((b[0]&128) || (fragment_ && type!=28)) damaged_=true;
    if (type > 0 && type < 24) { separator(); frame_.insert(frame_.end(), b.begin(), b.end()); }
    else if (type == 24) {
        size_t offset = 1;
        while (offset + 2 <= b.size()) {
            const auto size = u16(b, offset); offset += 2;
            if (!size || size > b.size() - offset || !(b[offset] & 31) || (b[offset] & 31) >= 24) { damaged_ = true; break; }
            separator(); frame_.insert(frame_.end(), b.begin()+offset, b.begin()+offset+size); offset += size;
        }
        if (offset != b.size()) damaged_ = true;
    } else if (type == 28 && b.size() >= 3) {
        const auto unit = b[1] & 31;
        if (!unit || unit >= 24 || (b[1] & 32) || ((b[1] & 192) == 192)) damaged_ = true;
        if (b[1] & 128) { if(fragment_) damaged_=true; separator(); frame_.push_back(uint8_t((b[0] & 224) | unit)); fragment_ = true; }
        if (!fragment_) damaged_ = true;
        if (!damaged_) frame_.insert(frame_.end(), b.begin()+2, b.end());
        if (b[1] & 64) fragment_ = false;
    } else damaged_ = true;
    if (frame_.size() > 4*1024*1024) { damaged_ = true; frame_.clear(); }
    if (p.marker) {
        if (!damaged_ && !fragment_ && !frame_.empty()) {
            emit_({true, unwrapTimestamp(p.timestamp, 90000, anchor), std::move(frame_)}); ++stats->frames;
        } else keyframe_();
        discardFrame();
    }
}
}
