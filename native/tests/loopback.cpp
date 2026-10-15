#include "party/media.hpp"
#include "party/player.hpp"
#include "party/transport.hpp"
#include <iostream>
#include <thread>
#include <mutex>
#include <set>
namespace {
class Impairment final : public rtc::MediaHandler {
public:
    std::atomic<uint64_t> dropped{0}, reordered{0};
    void outgoing(rtc::message_vector& messages, const rtc::message_callback&) override {
        rtc::message_vector result;
        for (auto& message : messages) {
            if (message->type != rtc::Message::Binary) { result.push_back(message); continue; }
            auto packet = party::parseRtp({reinterpret_cast<const uint8_t*>(message->data()), message->size()});
            if (!packet || packet->ssrc != 1) { result.push_back(message); continue; }
            const uint64_t key = (uint64_t(packet->timestamp)<<16)|packet->sequence;
            if (packet->sequence % 97 == 0 && once.insert(key).second) { ++dropped; continue; }
            if (held) { result.push_back(message); result.push_back(std::move(held)); ++reordered; }
            else if (packet->sequence % 31 == 0) held = message;
            else result.push_back(message);
            if (once.size() > 2048) once.erase(once.begin());
        }
        messages.swap(result);
    }
private:
    std::set<uint64_t> once;
    rtc::message_ptr held;
};
void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); } }
int main(int argc, char** argv) {
    using namespace party;
    if (argc != 2) { std::cerr << "Usage: party_loopback generated-local-video.mp4\n"; return 2; }
    try {
        rtc::InitLogger(rtc::LogLevel::Warning);
        const bool highQuality = env("PARTY_TEST_HIGH_QUALITY") == "1";
        const unsigned initialRate = highQuality ? 6000000 : 2500000;
        const unsigned maximumRate = highQuality ? 12000000 : 5000000;
        Player player(env("PARTY_TEST_WINDOW") != "1");
        std::shared_ptr<Peer> host, viewer;
        std::mutex peerMutex;
        MediaSource source(argv[1], MediaOptions{1920,1080,env("PARTY_TEST_HARDWARE") == "1",initialRate,maximumRate}, [&](EncodedFrame frame) {
            std::shared_ptr<Peer> current; { std::lock_guard lock(peerMutex); current = host; }
            if (current) current->send(frame);
        });
        const bool impair = env("PARTY_TEST_IMPAIR") == "1";
        std::shared_ptr<Impairment> impairment;
        std::vector<Json> toViewer,toHost;uint64_t deferredIce=0,duplicateSdp=0,sentEpoch=UINT64_MAX;
        int64_t connectBegan=0;
        auto connect = [&] {
            toViewer.clear();toHost.clear();sentEpoch=UINT64_MAX;connectBegan=nowUs();
            rtc::Configuration config; config.disableAutoNegotiation = true; config.mtu = 1200; config.bindAddress = "127.0.0.1";
            const auto h = Peer::create(true, config, [](auto) {}, [&](Json j) {
                if (j.value("type", "") == "ping") {
                    std::shared_ptr<Peer> current; { std::lock_guard lock(peerMutex); current = host; }
                    if (current) current->control({{"type", "pong"}, {"t0", j.at("t0")}, {"t1", nowUs()}, {"t2", nowUs()}});
                } else if (j.value("type", "") == "keyframe") source.keyframe();
            }, initialRate, maximumRate);
            { std::lock_guard lock(peerMutex); host = h; }
            viewer = Peer::create(false, config, [&](EncodedFrame frame) { player.push(std::move(frame)); }, [&](Json j) {
                if (j.value("type", "") == "pong") j["t3"] = nowUs(); player.control(j);
            });
            if (impair) {
                impairment = std::make_shared<Impairment>();
                auto chain = host->videoTrack()->getMediaHandler();
                while (chain->next() && !std::dynamic_pointer_cast<BoundedPacer>(chain->next())) chain = chain->next();
                require(bool(chain->next()), "Sender pacing chain missing");
                impairment->setNext(chain->next()); chain->setNext(impairment);
            }
            host->offer();
        };
        connect();
        const auto start = nowUs();
        int64_t lastState=0, lastPing=0, connectedAt=0;
        bool played=false, paused=false, resumed=false, sought=false, backward=false, forwardAgain=false, reconnected=false, secondPlayed=false, stopped=false;
        uint64_t videoBeforePause=0, videoBeforeReconnect=0;
        while (nowUs()-start < 22000000) {
            auto transfer=[&](auto& pending,auto from,auto to,bool holdOffer){
                const auto outgoing=from->takeSignals();pending.insert(pending.end(),outgoing.begin(),outgoing.end());
                const bool candidates=std::any_of(pending.begin(),pending.end(),[](const Json& m){return m.at("type")=="candidate";});
                std::stable_sort(pending.begin(),pending.end(),[](const Json& a,const Json& b){return (a.at("type")=="candidate")>(b.at("type")=="candidate");});
                std::erase_if(pending,[&](const Json& m){
                    if(holdOffer && m.at("type")=="offer" && !candidates && nowUs()-connectBegan<250000)return false;
                    if(!to->signal(m)){++deferredIce;return false;}
                    require(to->signal(m),"Duplicate applied SDP/ICE failed");
                    if(m.at("type")!="candidate")++duplicateSdp;
                    return true;
                });
            };
            transfer(toViewer,host,viewer,true);transfer(toHost,viewer,host,false);
            host->pump(); viewer->hostAnchorUs=player.hostAnchorUs.load();viewer->receiveCutoffUs=player.cutoffUs.load();viewer->pump();
            // Genuine received media accumulates while the output thread is delayed.
            if(!connectedAt || nowUs()-connectedAt<250000 || nowUs()-connectedAt>1750000)player.tick();
            if (host->connected() && viewer->connected()) {
                if (!connectedAt) connectedAt = nowUs();
                if (!played) { source.play(); played=true; }
                if (reconnected && !secondPlayed) { source.seek(source.state().movieUs); source.play(); secondPlayed=true; }
                if (nowUs()-lastState > 50000) { const auto state=source.state();
                    if(sentEpoch!=state.epoch){host->discardQueuedMedia();sentEpoch=state.epoch;}
                    host->control(state.json()); lastState=nowUs(); }
                if (nowUs()-lastPing > 250000) { viewer->control(player.ping()); lastPing=nowUs(); }
                if (host->forceKeyframe.exchange(false)) source.keyframe();
                source.bitrate(host->targetBitrate);
                if (player.needsKeyframe()) viewer->control({{"type", "keyframe"}});
                const auto elapsed = nowUs()-connectedAt;
                if (elapsed > 3000000 && !paused) { require(player.stats().at("renderedVideoFrames").get<uint64_t>()>15,"Delayed output startup never recovered");source.pause(); paused=true; videoBeforePause=player.stats().at("renderedVideoFrames"); }
                if (elapsed > 4000000 && !resumed) {
                    require(player.stats().at("renderedVideoFrames").get<uint64_t>() <= videoBeforePause+3, "Viewer continues rendering while paused");
                    source.play(); resumed=true;
                }
                if (elapsed > 6000000 && !sought) { source.seek(12000000); sought=true; }
                if(elapsed>7500000 && !backward){source.seek(3000000);backward=true;}
                if(elapsed>8500000 && !forwardAgain){source.seek(15000000);forwardAgain=true;}
                if (elapsed > 10000000 && !reconnected) {
                    require(player.stats().at("decodedVideoFrames").get<uint64_t>() > 60, "Real video was not decoded");
                    require(player.stats().at("decodedAudioPackets").get<uint64_t>() > 100, "Real audio was not decoded");
                    require(source.error().empty(), "Source error before reconnection");
                    videoBeforeReconnect = player.stats().at("renderedVideoFrames");
                    source.pause();host->videoTrack()->close();host->pump();require(host->failed(),"Closed media track did not trigger recoverable transport failure");host->close(); viewer->close(); player.reset();
                    connect(); reconnected=true;
                }
                if (elapsed > 15000000 && secondPlayed && !stopped) { source.stop(); stopped=true; }
                if (elapsed > 16000000 && stopped) break;
            }
            require(source.error().empty(), source.error().c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(played && paused && resumed && sought && backward && forwardAgain && reconnected && secondPlayed && stopped, "Control/reconnection sequence did not complete");
        require(deferredIce>0 && duplicateSdp>=4,"Real candidate-before-SDP retry/duplicate scenario did not run");
        const auto stats = player.stats();
        require(stats.at("renderedVideoFrames").get<uint64_t>() > videoBeforeReconnect+20, "Video did not recover after new ICE connection");
        require(stats.at("audioEnergy").get<double>() > 1.0, "Decoded audio is silent");
        require(stats.at("videoChecksum").get<uint64_t>() != 0, "Decoded content checksum is empty");
        require(stats.at("maxAvDriftUs").get<int64_t>() <= 60000, "A/V scheduling drift exceeded 60ms");
        if (impair) { require(impairment->dropped.load() > 0 && impairment->reordered.load() > 0, "Impairment did not act on real RTP"); require(viewer->stats().at("video").at("nacks").get<uint64_t>() > 0, "No NACK feedback under loss"); }
        if (env("PARTY_TEST_WINDOW") == "1") require(stats.at("directYuvFrames").get<uint64_t>() > 0, "Windowed streaming did not exercise YUV upload");
        if (highQuality && !impair) require(host->targetBitrate.load() > 5000000, "High-quality negotiation fell back to legacy ceiling");
        require(source.state().stopped && !source.state().playing, "Stop did not stop source");
        std::cout << Json{{"test", "real-loopback"}, {"deferredIce",deferredIce}, {"duplicateSdpApplications",duplicateSdp}, {"durationUs", nowUs()-start}, {"impaired", impair}, {"injectedDrops", impairment ? impairment->dropped.load() : 0}, {"injectedReordering", impairment ? impairment->reordered.load() : 0}, {"source", source.stats()}, {"host", host->stats()}, {"viewer", viewer->stats()}, {"player", stats}}.dump(2) << '\n';
        source.pause(); host->close(); viewer->close();
        return 0;
    } catch (const std::exception& e) { std::cerr << "Loopback failed: " << e.what() << '\n'; return 1; }
}
