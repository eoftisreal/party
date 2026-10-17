#include "party/transport.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace party {
namespace {
const char* candidateTypeName(rtc::Candidate::Type type) {
    switch (type) {
    case rtc::Candidate::Type::Host: return "host";
    case rtc::Candidate::Type::ServerReflexive: return "srflx";
    case rtc::Candidate::Type::PeerReflexive: return "prflx";
    case rtc::Candidate::Type::Relayed: return "relay";
    default: return "unknown";
    }
}
}
void BoundedPacer::outgoing(rtc::message_vector& messages, const rtc::message_callback& send) {
    std::lock_guard lock(mutex_); send_ = send;
    rtc::message_vector controls;
    size_t batchBytes = 0;
    for (const auto& m : messages) if (m->type != rtc::Message::Control) batchBytes += m->size();
    // Reserve for a whole access unit: dropping its first fragments creates a corrupt frame.
    // 512KiB is a hard bound; VBV in the encoder limits IDR bursts below this.
    constexpr size_t budget = 512*1024;
    if (bytes_ + batchBytes > budget) {
        dropped += queue_.size(); queue_.clear(); bytes_ = 0; keyframe_ = true;
    }
    for (auto& m : messages) {
        if (m->type == rtc::Message::Control) { controls.push_back(m); continue; }
        if (batchBytes > budget) { ++dropped; keyframe_ = true; continue; }
        bytes_ += m->size(); queue_.push_back(m);
    }
    messages.swap(controls);
}
void BoundedPacer::pump(int64_t now) {
    std::deque<rtc::message_ptr> ready;
    rtc::message_callback send;
    {
        std::lock_guard lock(mutex_);
        if (!last_) last_ = now;
        const double bytesPerSecond = rate_.load() * 1.15 / 8.0;
        // Cover a normal 2–5 ms pump cadence at high bitrate, while bounding bursts.
        const double burst = std::clamp(bytesPerSecond * .01, 4800.0, 32768.0);
        tokens_ = std::min(burst, tokens_ + std::max<int64_t>(0, now - last_) * bytesPerSecond / 1000000.0);
        last_ = now;
        while (!queue_.empty() && double(queue_.front()->size()) <= tokens_) {
            auto m = queue_.front(); queue_.pop_front(); bytes_ -= m->size(); tokens_ -= double(m->size()); ready.push_back(m);
        }
        send = send_;
    }
    if (send) for (auto& m : ready) { send(m); bytesSent += m->size(); }
}
int64_t BoundedPacer::queueUs() const { std::lock_guard lock(mutex_); return int64_t(bytes_) * 8 * 1000000 / rate_.load(); }
std::shared_ptr<Peer> Peer::create(bool host, const rtc::Configuration& config, FrameCallback frame, std::function<void(Json)> control, unsigned initialBitrate, unsigned maxBitrate) {
    BitrateController validate(initialBitrate, maxBitrate);
    auto p = std::shared_ptr<Peer>(new Peer(host)); p->initialBitrate_ = initialBitrate; p->maxBitrate_ = maxBitrate; p->targetBitrate = initialBitrate; p->init(config, std::move(frame), std::move(control)); return p;
}
void Peer::init(const rtc::Configuration& config, FrameCallback frame, std::function<void(Json)> control) {
    onControl_ = std::move(control);
    for (const auto& server : config.iceServers) {
        if (server.type == rtc::IceServer::Type::Stun) ++stunServers_;
        if (server.type == rtc::IceServer::Type::Turn) ++turnServers_;
    }
    pc_ = std::make_shared<rtc::PeerConnection>(config);
    const auto weak = weak_from_this();
    pc_->onStateChange([weak](auto state) { if (auto self = weak.lock()) { self->state_ = state; if (state == rtc::PeerConnection::State::Connected) { int64_t unset = 0; self->connectedUs_.compare_exchange_strong(unset, nowUs()); } } });
    pc_->onLocalDescription([weak](const rtc::Description& d) { if (auto self = weak.lock()) {
        std::lock_guard lock(self->mutex_); self->outbound_.push_back({{"type", d.typeString()}, {"payload", {{"sdp", std::string(d)}}}});
    }});
    pc_->onLocalCandidate([weak](const rtc::Candidate& c) { if (auto self = weak.lock()) {
        std::lock_guard lock(self->mutex_);
        auto& count = self->localCandidateCounts_[candidateTypeName(c.type())];
        count = count.get<unsigned>() + 1;
        if(self->outbound_.size()>=128){++self->sendFailures;self->state_=rtc::PeerConnection::State::Failed;return;}
        self->outbound_.push_back({{"type", "candidate"}, {"payload", {{"candidate", std::string(c)}, {"mid", c.mid()}}}});
    }});
    if (host_) {
        auto v = rtc::Description::Video("video", rtc::Description::Direction::SendOnly);
        v.addH264Codec(96, "profile-level-id=42e028;packetization-mode=1;level-asymmetry-allowed=1");
        v.addSSRC(1, "party", "party", "video");
        v.addAttribute("rtcp-fb:96 nack"); v.addAttribute("rtcp-fb:96 nack pli"); v.addAttribute("rtcp-fb:96 goog-remb");
        video_ = pc_->addTrack(v);
        auto cfg = std::make_shared<rtc::RtpPacketizationConfig>(1, "party", 96, 90000);
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, cfg, 1100);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(cfg));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(1024));
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak] { if (auto self = weak.lock()) self->forceKeyframe = true; }));
        packetizer->addToChain(std::make_shared<rtc::RembHandler>([weak](unsigned bitrate) { if (auto self = weak.lock()) self->targetBitrate = std::clamp(bitrate, 300000u, self->maxBitrate_); }));
        pacer_ = std::make_shared<BoundedPacer>(targetBitrate, forceKeyframe);
        packetizer->addToChain(pacer_);
        auto vg = std::make_shared<PacketGuard>(); vg->addToChain(packetizer); video_->setMediaHandler(vg);
        auto a = rtc::Description::Audio("audio", rtc::Description::Direction::SendOnly);
        a.addOpusCodec(111); a.addSSRC(2, "party", "party", "audio");
        a.addAttribute("rtcp-fb:111 nack"); audio_ = pc_->addTrack(a);
        auto acfg = std::make_shared<rtc::RtpPacketizationConfig>(2, "party", 111, 48000);
        auto ap = std::make_shared<rtc::OpusRtpPacketizer>(acfg);
        ap->addToChain(std::make_shared<rtc::RtcpSrReporter>(acfg));
        ap->addToChain(std::make_shared<rtc::RtcpNackResponder>(128));
        auto ag = std::make_shared<PacketGuard>(); ag->addToChain(ap); audio_->setMediaHandler(ag);
        setupChannel(pc_->createDataChannel("party-control"));
    } else {
        pc_->onTrack([weak, frame](auto track) { if (auto self = weak.lock()) self->receiverTrack(track, frame); });
        pc_->onDataChannel([weak](auto dc) { if (auto self = weak.lock()) {
            if (dc->label() != "party-control") { dc->close(); return; }
            self->setupChannel(dc);
        }});
    }
}
void Peer::setupChannel(std::shared_ptr<rtc::DataChannel> dc) {
    { std::lock_guard lock(mutex_); dc_ = dc; }
    const auto weak = weak_from_this();
    dc->onMessage(nullptr, [weak](std::string message) { if (auto self = weak.lock()) {
        if (message.size() > 8192) return;
        try { const auto j = Json::parse(message);
            std::lock_guard callbackLock(self->callbackMutex_);
            if (!self->closed_ && j.is_object() && j.contains("type")) {
                if (j.at("type") == "stream-config") {
                    if (!self->host_ && j.at("initialBitrate").is_number_unsigned() && j.at("maxBitrate").is_number_unsigned()) {
                        const auto initial = j.at("initialBitrate").get<uint64_t>(), maximum = j.at("maxBitrate").get<uint64_t>();
                        if (initial >= 300000 && initial <= maximum && maximum <= 12000000) {
                            self->remoteConfig_ = (initial << 32) | maximum;
                        }
                    }
                } else self->onControl_(j);
            } }
        catch (const std::exception&) { /* malformed peer control is rejected */ }
    }});
}
void Peer::receiverTrack(std::shared_ptr<rtc::Track> track, FrameCallback frame) {
    const bool video = track->description().type() == "video";
    if (track->direction() != rtc::Description::Direction::RecvOnly || (!video && track->description().type() != "audio")) { track->close(); return; }
    const auto weak = weak_from_this();
    auto receiver = std::make_shared<RtpReceiver>(video, [weak,frame=std::move(frame)](EncodedFrame value) {
        if(auto self=weak.lock()) {std::lock_guard callbackLock(self->callbackMutex_);
            if(!self->closed_)frame(std::move(value));}
    }, [weak] {
        if (auto self = weak.lock()) {
            // Called from pump on the application thread; rate-limit PLI.
            if (nowUs() - self->lastPli_ > 250000) {
                self->lastPli_ = nowUs();
                std::shared_ptr<rtc::Track> video; { std::lock_guard lock(self->mutex_); video = self->video_; }
                if (video) video->requestKeyframe();
            }
        }
    });
    auto guard = std::make_shared<PacketGuard>();
    guard->addToChain(std::make_shared<rtc::RtcpReceivingSession>()); guard->addToChain(receiver);
    track->setMediaHandler(guard);
    std::lock_guard lock(mutex_);
    if (video) { if (video_) { track->close(); return; } video_ = track; videoReceiver_ = receiver; }
    else { if (audio_) { track->close(); return; } audio_ = track; audioReceiver_ = receiver; }
}
void Peer::offer() { if (!host_) throw std::logic_error("Only host can offer"); pc_->setLocalDescription(rtc::Description::Type::Offer); }
bool Peer::signal(const Json& message) {
    const auto type=message.at("type").get<std::string>();const auto& payload=message.at("payload");
    if(type=="candidate") {
        if(!pc_->remoteDescription()) return false;
        const auto candidate=payload.at("candidate").get<std::string>(),mid=payload.at("mid").get<std::string>();
        const auto key=mid+"\n"+candidate;
        if(appliedCandidates_.contains(key)) return true;
        if(appliedCandidates_.size()>=128) throw std::runtime_error("Too many ICE candidates");
        pc_->addRemoteCandidate(rtc::Candidate(candidate,mid));appliedCandidates_.insert(key);++remoteTrickleCandidatesApplied_;
    } else if((type=="offer" && !host_) || (type=="answer" && host_)) {
        const auto sdp=payload.at("sdp").get<std::string>();
        if(sdp.size()>60000) throw std::invalid_argument("SDP too large");
        if(!remoteSdp_.empty() && remoteSdp_!=sdp) throw std::invalid_argument("Conflicting SDP within connection generation");
        if(remoteSdp_.empty()) {pc_->setRemoteDescription(rtc::Description(sdp,type));remoteSdp_=sdp;}
        if(!host_ && !pc_->localDescription()) pc_->setLocalDescription(rtc::Description::Type::Answer);
    } else if(type=="disconnect") close();
    else throw std::invalid_argument("Unauthorized signal direction");
    return true;
}
void BoundedPacer::clear() { std::lock_guard lock(mutex_);discarded+=queue_.size();queue_.clear();bytes_=0; }
void Peer::discardQueuedMedia() { if(pacer_) pacer_->clear();forceKeyframe=true; }
std::vector<Json> Peer::takeSignals() {
    std::lock_guard lock(mutex_); std::vector<Json> result(outbound_.begin(), outbound_.end()); outbound_.clear();
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return (a.at("type") == "candidate") < (b.at("type") == "candidate"); });
    return result;
}
void Peer::send(const EncodedFrame& frame) {
    if (!host_ || !connected()) return;
    const uint32_t rate = frame.video ? 90000 : 48000;
    const uint32_t timestamp = uint32_t((frame.transportUs / 1000000) * rate + (frame.transportUs % 1000000) * rate / 1000000);
    const auto track = frame.video ? video_ : audio_;
    try {
        if (track && track->isOpen()) track->sendFrame(reinterpret_cast<const std::byte*>(frame.data.data()), frame.data.size(), rtc::FrameInfo(timestamp));
    } catch (const std::exception&) { ++sendFailures; state_ = rtc::PeerConnection::State::Failed; }
}
bool Peer::control(const Json& message) {
    std::shared_ptr<rtc::DataChannel> dc; { std::lock_guard lock(mutex_); dc = dc_; }
    if (!dc || !dc->isOpen() || dc->bufferedAmount() > 65536) return false;
    try { return dc->send(message.dump()); } catch (const std::exception&) { ++sendFailures; return false; }
}
void Peer::pump() {
    if (closed_ || state_==rtc::PeerConnection::State::Failed) return;
    try {
    if(host_ && ((video_ && video_->isClosed()) || (audio_ && audio_->isClosed()))) {
        ++sendFailures;state_=rtc::PeerConnection::State::Failed;if(pacer_)pacer_->clear();return;
    }
    if (host_ && connected() && !configSent_)
        configSent_ = control({{"type", "stream-config"}, {"initialBitrate", initialBitrate_}, {"maxBitrate", maxBitrate_}});
    const auto remoteConfig = remoteConfig_.load();
    if (!host_ && !configApplied_ && remoteConfig) {
        controller_ = BitrateController(unsigned(remoteConfig >> 32), unsigned(remoteConfig & 0xffffffffu)); configApplied_ = true;
    }
    if (pacer_) pacer_->pump(nowUs());
    std::shared_ptr<RtpReceiver> v, a; std::shared_ptr<rtc::Track> vt;
    { std::lock_guard lock(mutex_); v = videoReceiver_; a = audioReceiver_; vt = video_; }
    const auto cutoff=receiveCutoffUs.load();
    if(cutoff!=appliedCutoffUs_) {if(v)v->setCutoff(cutoff);if(a)a->setCutoff(cutoff);appliedCutoffUs_=cutoff;}
    const auto rtt=pc_->rtt();
    const auto wait=rtt?std::clamp<int64_t>(2*rtt->count()*1000+20000,60000,250000):60000;
    if (v) v->setReorderWindow(wait);
    if (a) a->setReorderWindow(wait);
    if (v) { v->setAnchor(hostAnchorUs); v->pump(nowUs()); }
    if (a) { a->setAnchor(hostAnchorUs); a->pump(nowUs()); }
    if (v && vt && connected() && nowUs() - lastFeedback_ >= 2000000) {
        const auto received = v->stats->received.load(), lost = v->stats->lost.load();
        const auto n = received - previousReceived_, l = lost - previousLost_;
        const double loss = n+l ? double(l)/double(n+l) : 0;
        // Paused or idle streams provide no evidence of spare bandwidth.
        if (n+l) vt->requestBitrate(controller_.update(loss, v->stats->jitterUs.load(), 0));
        previousReceived_ = received; previousLost_ = lost; lastFeedback_ = nowUs();
    }
    } catch(const std::exception&) {
        // Track closure can race a pacing/NACK/REMB callback. Reconnect, do not kill the room.
        ++sendFailures;state_=rtc::PeerConnection::State::Failed;
        if(pacer_)pacer_->clear();
    }
}
bool Peer::connected() const { return state_ == rtc::PeerConnection::State::Connected; }
bool Peer::failed() const { const auto state = state_.load(); return state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Closed; }
Json Peer::stats() const {
    Json result{{"iceState", int(pc_->iceState())}, {"sendFailures", sendFailures.load()}, {"connectionState", int(state_.load())}, {"connected", connected()}, {"targetBitrate", targetBitrate.load()}, {"dataBytesSent", pc_->bytesSent()}, {"dataBytesReceived", pc_->bytesReceived()}};
    result["connectMs"] = connectedUs_.load() ? (connectedUs_.load()-createdUs_)/1000 : -1;
    result["maxBitrate"] = maxBitrate_;
    if (const auto rtt = pc_->rtt()) result["sctpRttMs"] = rtt->count();
    result["stunServersConfigured"] = stunServers_;
    result["turnServersConfigured"] = turnServers_;
    result["iceGatheringComplete"] = pc_->gatheringState() == rtc::PeerConnection::GatheringState::Complete;
    result["connectionPath"] = "unselected";
    rtc::Candidate local, remote;
    if (pc_->getSelectedCandidatePair(&local, &remote)) {
        result["candidateTypes"] = {int(local.type()), int(remote.type())};
        result["selectedCandidateTypes"] = {candidateTypeName(local.type()), candidateTypeName(remote.type())};
        if (local.type() == rtc::Candidate::Type::Relayed || remote.type() == rtc::Candidate::Type::Relayed)
            result["connectionPath"] = "relay";
        else if (local.type() != rtc::Candidate::Type::Unknown && remote.type() != rtc::Candidate::Type::Unknown)
            result["connectionPath"] = "direct";
    }
    std::lock_guard lock(mutex_);
    result["localCandidates"] = localCandidateCounts_;
    result["remoteTrickleCandidatesApplied"] = remoteTrickleCandidatesApplied_.load();
    if (videoReceiver_) result["video"] = videoReceiver_->stats->json();
    if (audioReceiver_) result["audio"] = audioReceiver_->stats->json();
    if (pacer_) { result["rtpVideoBytesSent"] = pacer_->bytesSent.load(); result["pacerQueueUs"] = pacer_->queueUs(); result["pacerDroppedPackets"] = pacer_->dropped.load(); result["pacerEpochDiscardedPackets"]=pacer_->discarded.load(); }
    return result;
}
void Peer::close() {
    if (closed_.exchange(true)) return;
    // Drain external callbacks before callers release Player/MediaSource callback targets.
    {std::lock_guard callbackLock(callbackMutex_);}
    std::shared_ptr<rtc::DataChannel> dc; { std::lock_guard lock(mutex_); dc = dc_; }
    if (dc) dc->resetCallbacks();
    pc_->resetCallbacks(); pc_->close();
}
Peer::~Peer() { close(); }
rtc::Configuration iceConfigFromServers(const Json& servers) {
    rtc::Configuration cfg; cfg.disableAutoNegotiation = true;
    cfg.mtu = 1200; cfg.maxMessageSize = 16384;
    if(const auto bind=env("PARTY_BIND_ADDRESS");!bind.empty()) cfg.bindAddress=bind;
    {
        if (!servers.is_array() || servers.size() > 8) throw std::invalid_argument("Invalid ICE server configuration");
        unsigned turns = 0, stuns = 0;
        for (const auto& s : servers) {
            rtc::IceServer server(s.at("url").get<std::string>());
            if (server.type == rtc::IceServer::Type::Turn) {
                if (server.relayType != rtc::IceServer::RelayType::TurnUdp || ++turns > 2) throw std::invalid_argument("Default libjuice build supports up to two UDP TURN servers");
                server.username = s.at("username").get<std::string>(); server.password = s.at("credential").get<std::string>();
                if (server.username.empty() || server.password.empty()) throw std::invalid_argument("TURN credentials required");
            }
            if (server.type == rtc::IceServer::Type::Stun && ++stuns > 1) throw std::invalid_argument("Default libjuice build supports one STUN server");
            cfg.iceServers.push_back(server);
        }
    }
    if (env("PARTY_RELAY_ONLY") == "1") {
        if (std::none_of(cfg.iceServers.begin(), cfg.iceServers.end(), [](const auto& s) { return s.type == rtc::IceServer::Type::Turn; })) throw std::invalid_argument("Relay-only mode needs a TURN server");
        cfg.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }
    return cfg;
}
rtc::Configuration iceConfigFromEnvironment() {
    const auto raw = env("PARTY_ICE_SERVERS");
    return iceConfigFromServers(raw.empty() ? Json::array() : Json::parse(raw));
}
}
