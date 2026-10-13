#pragma once
#include "party/core.hpp"
#include "party/rtp.hpp"
#include <deque>
#include <functional>
#include <memory>
#include <set>
namespace party {
class BoundedPacer final : public rtc::MediaHandler {
public:
    explicit BoundedPacer(std::atomic<unsigned>& rate, std::atomic<bool>& keyframe) : rate_(rate), keyframe_(keyframe) {}
    void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override;
    void pump(int64_t now);
    int64_t queueUs() const;
    void clear();
    std::atomic<uint64_t> bytesSent{0}, dropped{0};
private:
    std::atomic<unsigned>& rate_;
    std::atomic<bool>& keyframe_;
    mutable std::mutex mutex_;
    std::deque<rtc::message_ptr> queue_;
    size_t bytes_ = 0;
    double tokens_ = 2400;
    int64_t last_ = 0;
    rtc::message_callback send_;
};
class Peer final : public std::enable_shared_from_this<Peer> {
public:
    using FrameCallback = std::function<void(EncodedFrame)>;
    static std::shared_ptr<Peer> create(bool host, const rtc::Configuration& config, FrameCallback frame, std::function<void(Json)> control);
    ~Peer();
    void offer();
    bool signal(const Json& message); // false: prerequisites not yet applied; caller must retry.
    void discardQueuedMedia();
    std::atomic<int64_t> receiveCutoffUs{0};
    std::vector<Json> takeSignals();
    void send(const EncodedFrame& frame);
    bool control(const Json& message);
    void pump();
    void close();
    bool connected() const;
    bool failed() const;
    Json stats() const;
    std::atomic<unsigned> targetBitrate{2500000};
    std::atomic<bool> forceKeyframe{true};
    std::atomic<uint64_t> sendFailures{0};
    std::atomic<int64_t> hostAnchorUs{0};
    // Attach instrumentation before offering; do not mutate media chains while streaming.
    std::shared_ptr<rtc::Track> videoTrack() { std::lock_guard lock(mutex_); return video_; }
    std::shared_ptr<rtc::PeerConnection> connection() { return pc_; }
private:
    explicit Peer(bool host) : host_(host) {}
    void init(const rtc::Configuration&, FrameCallback, std::function<void(Json)>);
    void setupChannel(std::shared_ptr<rtc::DataChannel>);
    void receiverTrack(std::shared_ptr<rtc::Track>, FrameCallback);
    bool host_;
    std::atomic<bool> closed_{false};
    std::atomic<rtc::PeerConnection::State> state_{rtc::PeerConnection::State::New};
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> video_, audio_;
    std::shared_ptr<rtc::DataChannel> dc_;
    std::shared_ptr<RtpReceiver> videoReceiver_, audioReceiver_;
    std::shared_ptr<BoundedPacer> pacer_;
    mutable std::mutex mutex_;
    std::deque<Json> outbound_;
    std::string remoteSdp_;
    std::set<std::string> appliedCandidates_;
    int64_t appliedCutoffUs_=0;
    std::function<void(Json)> onControl_;
    BitrateController controller_;
    int64_t lastFeedback_ = 0, lastPli_ = 0;
    uint64_t previousReceived_ = 0, previousLost_ = 0;
};
rtc::Configuration iceConfigFromEnvironment();
}
