#include "party/media.hpp"
#include "party/player.hpp"
#include "party/signaling.hpp"
#include "party/transport.hpp"
#include <algorithm>
#include <csignal>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif
namespace {
struct ScopeExit {
    std::function<void()> action;
    ~ScopeExit() { try { action(); } catch (...) { /* retain the original error during unwinding */ } }
};
// The media pump runs in this process, not its launcher. On supported Windows
// versions a high-resolution waitable timer avoids relying on Sleep(2)'s
// coarse wakeup granularity. No busy-spin or global timer-resolution changes.
class LoopPacer {
public:
    LoopPacer() {
#ifdef _WIN32
#ifdef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
        highResolution_ = timer_ != nullptr;
#endif
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
#endif
    }
    ~LoopPacer() {
#ifdef _WIN32
        if (timer_) CloseHandle(timer_);
#endif
    }
    LoopPacer(const LoopPacer&) = delete;
    LoopPacer& operator=(const LoopPacer&) = delete;
    bool highResolution() const {
#ifdef _WIN32
        return highResolution_;
#else
        return false;
#endif
    }
    void wait() {
#ifdef _WIN32
        if (timer_) {
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -20000; // Relative 2 ms in 100 ns units.
            if (SetWaitableTimer(timer_, &dueTime, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0) return;
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
private:
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    bool highResolution_ = false;
#endif
};
std::atomic<bool> quitting{false};
void interrupt(int) { quitting = true; }
std::optional<std::string> command() {
#ifdef _WIN32
    static std::string line;
    const HANDLE handle=GetStdHandle(STD_INPUT_HANDLE);
    const bool console=GetFileType(handle)==FILE_TYPE_CHAR;
    while(true) {
        char c=0;
        if(console) {if(!_kbhit())break;c=char(_getch());}
        else {DWORD available=0,read=0;if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr)||!available)break;
            if(!ReadFile(handle,&c,1,&read,nullptr)||!read)break;}
        if(c=='\r'||c=='\n'){if(line.empty())continue;auto result=line;line.clear();if(console)std::cout<<'\n';return result;}
        if(c=='\b'){if(!line.empty())line.pop_back();}else if(c>=' '){line+=c;if(console)std::cout<<c<<std::flush;}
    }
#else
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    if (poll(&fd, 1, 0) > 0 && (fd.revents & POLLIN)) { std::string line; if (std::getline(std::cin, line)) return line; }
#endif
    return {};
}
}
int runParty(int argc, char** argv) {
    using namespace party;
    std::cout.setf(std::ios::unitbuf);
    try {
        if (argc < 2) throw std::invalid_argument("Usage: party host --file movie.mp4 [--room ID] | party viewer --room ID --invite CODE [--headless] [--preview] [--quality balanced|high|data-saver]");
        const bool host = std::string(argv[1]) == "host";
        if (!host && std::string(argv[1]) != "viewer") throw std::invalid_argument("Choose host or viewer");
        std::string file, roomId, invite, quality = "balanced"; bool headless = false, hardware = true, preview = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--headless") headless = true;
            else if (arg == "--preview") preview = true;
            else if (i+1 < argc && arg == "--quality") quality = argv[++i];
            else if (arg == "--software") hardware = false;
            else if (i+1 < argc && arg == "--file") file = argv[++i];
            else if (i+1 < argc && arg == "--room") roomId = argv[++i];
            else if (i+1 < argc && arg == "--invite") invite = argv[++i];
            else throw std::invalid_argument("Unknown/missing argument: " + arg);
        }
        if (host && file.empty()) throw std::invalid_argument("Host needs --file");
        if (!host && roomId.empty()) throw std::invalid_argument("Viewer needs --room");
        MediaOptions media;
        media.hardware = hardware;
        if (quality == "balanced") { media.bitrate = 4000000; media.maxBitrate = 8000000; }
        else if (quality == "high") { media.bitrate = 6000000; media.maxBitrate = 12000000; }
        else if (quality == "data-saver") { media.width = 1280; media.height = 720; media.bitrate = 1500000; media.maxBitrate = 3000000; }
        else throw std::invalid_argument("Quality must be balanced, high, or data-saver");
        rtc::InitLogger(rtc::LogLevel::Warning);
        const auto setupStarted = nowUs();
        auto ice = env("PARTY_ICE_SERVERS").empty() ? rtc::Configuration{} : iceConfigFromEnvironment();
        auto api = std::make_unique<AppwriteApi>();
        const auto room = roomId.empty() ? api->request("POST", "/v1/rooms", {{"title", "C++ movie night"}})
            : api->request("POST", "/v1/rooms/"+roomId+"/join", {{"inviteCode", invite}});
        const auto roomReadyMs = (nowUs()-setupStarted)/1000;
        if ((room.at("yourRole") == "host") != host) throw std::runtime_error("Room role does not match application mode");
        std::cout << "Room: " << room.at("roomId").get<std::string>() << '\n';
        if (room.contains("inviteCode")) std::cout << "Invite (share privately): " << room.at("inviteCode").get<std::string>() << '\n';
        Signaling signaling(std::move(api), room, host);
        struct HostSlot {
            std::shared_ptr<Peer> peer;
            std::string generation, sessionVersion;
            int64_t started = 0, disconnectedSince = 0;
            bool wasConnected = false;
        };
        // Host slots are owned by the main loop. Only this map is accessed by media
        // and WebRTC callback threads, and it is always protected by peerMutex.
        std::map<std::string, HostSlot> hostPeers;
        std::unordered_map<std::string, std::shared_ptr<Peer>> senderPeers;
        std::mutex peerMutex;
        std::shared_ptr<Peer> peer; // Viewer role has exactly one host connection.
        std::unique_ptr<Player> player;
        std::unique_ptr<MediaSource> source;
        std::string remoteUser, generation;
        std::set<std::string> authorizing;

        auto syncSenderPeers = [&] {
            std::lock_guard lock(peerMutex);
            senderPeers.clear();
            for (const auto& [viewer, slot] : hostPeers)
                if (slot.peer) senderPeers.emplace(viewer, slot.peer);
        };
        auto clearPeers = [&] {
            for (auto& [viewer, slot] : hostPeers) if (slot.peer) slot.peer->close();
            hostPeers.clear();
            syncSenderPeers();
            if (peer) peer->close();
            peer.reset();
        };
        std::atomic<int64_t> lastRemoteKeyframe{0};
        ScopeExit cleanup{[&] {
            if (source) source->pause();
            clearPeers();
            source.reset(); // Join producer before freeing Player or callback targets.
            signaling.stop();
            player.reset();
        }};
        signaling.start();
        if (!host || preview) player = std::make_unique<Player>(headless);
        if (host && player) { const auto time = nowUs(); player->clock.sample(time,time,time,time); }
        if (host) source = std::make_unique<MediaSource>(file, media, [&](EncodedFrame frame) {
            std::vector<std::shared_ptr<Peer>> targets;
            {
                std::lock_guard lock(peerMutex);
                targets.reserve(senderPeers.size());
                for (const auto& [viewer, target] : senderPeers) targets.push_back(target);
            }
            if (player) player->push(frame);
            for (const auto& target : targets) target->send(frame);
        });
        const auto mediaReadyMs = (nowUs()-setupStarted)/1000;
        auto createPeer = [&](std::string viewerId) {
            signaling.connecting(true);
            return Peer::create(host, ice, [&](EncodedFrame frame) {
                if (player) player->push(std::move(frame));
            }, [&, viewerId](Json message) {
                const auto type = message.value("type", "");
                if (host) {
                    if (type == "ping" && message.contains("t0") && message["t0"].is_number_integer()) {
                        const int64_t t1 = nowUs(), t0 = message["t0"];
                        if (t0 < 0) return;
                        std::shared_ptr<Peer> target;
                        { std::lock_guard lock(peerMutex); if (auto it = senderPeers.find(viewerId); it != senderPeers.end()) target = it->second; }
                        if (target) target->control({{"type", "pong"}, {"t0", t0}, {"t1", t1}, {"t2", nowUs()}});
                    } else if (type == "keyframe" && nowUs()-lastRemoteKeyframe.load() > 250000) {
                        lastRemoteKeyframe = nowUs(); source->keyframe();
                    }
                    // Never accept viewer playback or seek commands.
                } else if (type == "state" || type == "pong") {
                    if (type == "pong") message["t3"] = nowUs();
                    player->control(message);
                }
            }, media.bitrate, media.maxBitrate);
        };
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        std::cout << (host ? "Commands: play, pause, seek SECONDS, stop, stats, quit\n" : "Commands: stats, quit\n");
        int64_t peerStarted = 0, lastState = 0, lastPing = 0, lastStats = 0;
        bool wasConnected = false, wantsPlay = true, coordinationActive = true, restartRequired = false;
        bool streamingActive = false, negotiating = false;
        uint64_t sentEpoch = UINT64_MAX;
        LoopPacer loopPacer;
        int64_t lastLoopStart = 0, maxLoopIntervalUs = 0;
        uint64_t loopIntervals = 0, lateLoopIntervals = 0;
        while (!quitting) {
            const auto loopStart = nowUs();
            if (lastLoopStart) {
                const auto interval = loopStart - lastLoopStart;
                maxLoopIntervalUs = std::max(maxLoopIntervalUs, interval);
                if (interval > 15000) ++lateLoopIntervals;
                ++loopIntervals;
            }
            lastLoopStart = loopStart;
            auto updates=signaling.events();
            // Lease enforcement must not wait for a blocked HTTPS call on the worker.
            if(coordinationActive && !signaling.leaseHealthy())updates.insert(updates.begin(),Json{{"event","coordination-lost"}});
            else if(!coordinationActive && signaling.leaseHealthy())updates.push_back({{"event","coordination-restored"}});
            for (const auto& event : updates) {
                const auto kind = event.at("event").get<std::string>();
                if (kind == "ice") {
                    ice = iceConfigFromServers(event.at("iceServers"));
                    const bool relay = std::any_of(ice.iceServers.begin(), ice.iceServers.end(), [](const auto& s) { return s.type == rtc::IceServer::Type::Turn; });
                    const bool stun = std::any_of(ice.iceServers.begin(), ice.iceServers.end(), [](const auto& s) { return s.type == rtc::IceServer::Type::Stun; });
                    std::cout << "ICE servers: " << ice.iceServers.size() << ", STUN discovery: " << (stun ? "enabled" : "disabled")
                              << ", TURN relay: " << (relay ? "configured" : "not configured; using direct connections") << '\n';
                    if (!stun && !relay) std::cerr << "No public-address discovery configured; cross-network connections may fail.\n";
                }
                if (kind == "closed") {
                    std::cerr << "Room unavailable: " << event.value("code", "closed") << '\n';
                    quitting = true; break;
                }
                if (kind == "coordination-lost" || kind == "rejoined") {
                    if (source) source->pause();
                    clearPeers();
                    if (player) { player->reset(); if (host) { const auto t = nowUs(); player->clock.sample(t,t,t,t); } }
                    authorizing.clear(); remoteUser.clear(); generation.clear(); wasConnected = false;
                    streamingActive = false; negotiating = false;
                    restartRequired = true; coordinationActive = kind == "rejoined" && signaling.leaseHealthy();
                }
                if (kind == "coordination-restored") coordinationActive = signaling.leaseHealthy();
                if (kind == "error") {
                    std::cerr << "Signaling rejected: " << event.value("code", "error") << '\n';
                    // A retryable authorization failure must not block later room updates.
                    authorizing.clear();
                }
                if (kind == "room" && host && coordinationActive) {
                    const auto& members = event.at("participants");
                    const auto connections = event.value("connections", Json::array());
                    std::unordered_set<std::string> online;
                    for (const auto& member : members) {
                        const std::string viewer = member.at("userId");
                        if (viewer == signaling.userId() || member.at("online") != true) continue;
                        online.insert(viewer);
                        const auto version = member.value("sessionVersion", "");
                        if (auto it = hostPeers.find(viewer); it != hostPeers.end()) {
                            if (it->second.sessionVersion.empty()) it->second.sessionVersion = version;
                            bool invalid = it->second.sessionVersion != version;
                            for (const auto& c : connections) if (c.value("viewerId", "") == viewer &&
                                c.value("generation", "") == it->second.generation && c.value("state", "") == "closed")
                                invalid = true;
                            if (invalid) {
                                it->second.peer->close();
                                hostPeers.erase(it);
                                syncSenderPeers();
                            }
                        }
                        if (!hostPeers.contains(viewer) && !authorizing.contains(viewer)) {
                            authorizing.insert(viewer);
                            signaling.authorize(viewer, restartRequired);
                        }
                    }
                    for (auto it = hostPeers.begin(); it != hostPeers.end();) {
                        if (!online.contains(it->first)) {
                            it->second.peer->close();
                            it = hostPeers.erase(it);
                            syncSenderPeers();
                        } else ++it;
                    }
                    for (auto it = authorizing.begin(); it != authorizing.end();)
                        if (!online.contains(*it)) it = authorizing.erase(it);
                        else ++it;
                }
                if (kind == "authorized" && host && coordinationActive) {
                    const std::string viewer = event.at("viewerId");
                    authorizing.erase(viewer);
                    if (!signaling.current({{"generation", event.at("generation")},{"from",viewer},{"type","answer"}}))
                        continue;
                    if (auto it = hostPeers.find(viewer); it != hostPeers.end()) {
                        it->second.peer->close();
                        hostPeers.erase(it);
                    }
                    HostSlot slot;
                    slot.generation = event.at("generation");
                    // The next room snapshot will check the actual membership session.
                    // Capture the currently announced session version before creation.
                    slot.started = nowUs();
                    slot.peer = createPeer(viewer);
                    hostPeers.emplace(viewer, std::move(slot));
                    syncSenderPeers();
                    hostPeers.at(viewer).peer->offer();
                    restartRequired = false;
                }
                if (kind == "signal") {
                    const auto id = event.at("messageId").get<std::string>();
                    if (!signaling.current(event)) { signaling.processed(id, true); continue; }
                    bool applied = false;
                    try {
                        if (host) {
                            const std::string viewer = event.at("from");
                            if (auto it = hostPeers.find(viewer); it != hostPeers.end() &&
                                it->second.generation == event.at("generation").get<std::string>())
                                applied = it->second.peer->signal(event);
                        } else {
                            if (event.at("type") == "offer" && event.at("generation") != generation) {
                                if (peer) peer->close();
                                player->reset();
                                remoteUser = event.at("from");
                                generation = event.at("generation");
                                peer = createPeer(remoteUser); peerStarted = nowUs();
                            }
                            if (peer && event.at("generation") == generation && event.at("from") == remoteUser)
                                applied = peer->signal(event);
                        }
                    } catch (const std::exception&) { std::cerr << "Signal application failed; retained for retry\n"; }
                    signaling.processed(id, applied);
                }
            }
            if (coordinationActive && host) {
                bool anyConnected = false, anyNegotiating = !authorizing.empty();
                unsigned effectiveBitrate = media.maxBitrate;
                for (auto& [viewer, slot] : hostPeers) {
                    auto& current = slot.peer;
                    current->pump();
                    for (const auto& message : current->takeSignals())
                        signaling.send(viewer, slot.generation, message);
                    const bool connected = current->connected();
                    if (connected) {
                        anyConnected = true;
                        effectiveBitrate = std::min(effectiveBitrate, current->targetBitrate.load());
                        if (!slot.wasConnected) {
                            source->keyframe();
                            slot.disconnectedSince = 0;
                        }
                        if (current->forceKeyframe.exchange(false)) source->keyframe();
                    } else {
                        anyNegotiating = true;
                        if (current->failed() && !slot.disconnectedSince) slot.disconnectedSince = nowUs();
                        if (!authorizing.contains(viewer) &&
                            ((slot.disconnectedSince && nowUs()-slot.disconnectedSince > 3000000) ||
                             nowUs()-slot.started > 35000000)) {
                            authorizing.insert(viewer);
                            signaling.authorize(viewer, true);
                            slot.started = nowUs();
                            slot.disconnectedSince = 0;
                        }
                    }
                    slot.wasConnected = connected;
                }
                if (anyNegotiating != negotiating) {
                    signaling.connecting(anyNegotiating);
                    negotiating = anyNegotiating;
                }
                // Joining a second viewer must not seek/restart the movie for the first.
                if (anyConnected && !streamingActive) {
                    source->seek(source->state().movieUs);
                    if (wantsPlay) source->play();
                    streamingActive = true;
                    sentEpoch = UINT64_MAX;
                } else if (!anyConnected && streamingActive) {
                    source->pause();
                    streamingActive = false;
                }
                if (anyConnected) {
                    source->bitrate(effectiveBitrate);
                    if (nowUs()-lastState > 100000) {
                        const auto playback = source->state();
                        if (sentEpoch != playback.epoch) {
                            for (auto& [viewer, slot] : hostPeers) slot.peer->discardQueuedMedia();
                            sentEpoch = playback.epoch;
                        }
                        const auto state = playback.json();
                        for (auto& [viewer, slot] : hostPeers)
                            if (slot.peer->connected()) slot.peer->control(state);
                        if (player) player->control(state);
                        lastState = nowUs();
                    }
                }
            }
            if (coordinationActive && !host && peer) {
                peer->receiveCutoffUs = player->cutoffUs.load();
                peer->pump();
                for (const auto& message : peer->takeSignals()) signaling.send(remoteUser, generation, message);
                const bool connected = peer->connected();
                if (connected && !wasConnected) signaling.connecting(false);
                if (!connected && wasConnected) signaling.connecting(true);
                if (connected) {
                    peer->hostAnchorUs = player->hostAnchorUs.load();
                    if (nowUs()-lastPing > 1000000) { peer->control(player->ping()); lastPing = nowUs(); }
                    if (player->needsKeyframe()) peer->control({{"type","keyframe"}});
                }
                wasConnected = connected;
            }
            if (player && !player->tick()) quitting = true;
            if (source && !source->error().empty()) throw std::runtime_error(source->error());
            if (auto line = command()) {
                if (*line == "quit") quitting = true;
                else if (source && *line == "play") { wantsPlay = true; if (coordinationActive && streamingActive) source->play(); }
                else if (source && *line == "pause") { wantsPlay = false; source->pause(); }
                else if (source && *line == "stop") { wantsPlay = false; source->stop(); }
                else if (source && line->starts_with("seek ")) {
                    const double seconds = std::stod(line->substr(5));
                    if (!std::isfinite(seconds) || seconds < 0 || seconds > 86400*365) throw std::invalid_argument("Invalid seek position");
                    source->seek(int64_t(seconds*1000000));
                } else if (*line == "stats") lastStats = 0;
            }
            if (nowUs()-lastStats > 5000000) {
                Json stats;
                stats["loop"] = {{"intervals", loopIntervals}, {"maxIntervalUs", maxLoopIntervalUs},
                                 {"lateIntervalsOver15ms", lateLoopIntervals},
                                 {"highResolutionWaitableTimer", loopPacer.highResolution()}};
                stats["roomReadyMs"]=roomReadyMs; stats["mediaReadyMs"]=mediaReadyMs; stats["coordinationHealthy"]=signaling.leaseHealthy();stats["coordinationActive"]=coordinationActive;stats["authorizing"]=!authorizing.empty();
                if (host) {
                    stats["viewerCount"] = hostPeers.size();
                    Json viewerStats = Json::object();
                    unsigned connectedCount = 0;
                    for (const auto& [viewer, slot] : hostPeers) {
                        viewerStats[viewer] = {{"generation", slot.generation}, {"network", slot.peer->stats()}};
                        if (slot.peer->connected()) ++connectedCount;
                    }
                    stats["connectedViewerCount"] = connectedCount;
                    stats["viewers"] = std::move(viewerStats);
                    if (!hostPeers.empty()) {
                        stats["network"] = hostPeers.begin()->second.peer->stats();
                        stats["connectionGeneration"] = hostPeers.begin()->second.generation;
                    }
                } else if (peer) {
                    stats["network"] = peer->stats(); stats["connectionGeneration"] = generation;
                }
                if (source) stats["media"] = source->stats(); if (player) stats["player"] = player->stats();
                std::cout << stats.dump() << '\n'; lastStats = nowUs();
            }
            loopPacer.wait();
        }
        if (source) source->pause();
        clearPeers();
        source.reset(); signaling.stop();
        return 0;
    } catch (const std::exception& e) { std::cerr << "Party: " << e.what() << '\n'; return 1; }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    // MSVC narrow argv uses the system code page; FFmpeg's Windows file I/O uses UTF-8.
    std::vector<std::string> values;values.reserve(argc);
    for(int i=0;i<argc;++i) {
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
        if(size<=0){std::cerr<<"Invalid command-line Unicode\n";return 1;}
        std::string value(size,'\0');
        if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),size,nullptr,nullptr))return 1;
        value.resize(size-1);values.push_back(std::move(value));
    }
    std::vector<char*> arguments;arguments.reserve(argc+1);
    for(auto& value:values)arguments.push_back(value.data());
    arguments.push_back(nullptr);
    return runParty(argc,arguments.data());
}
#else
int main(int argc, char** argv) { return runParty(argc,argv); }
#endif
