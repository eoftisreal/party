#include "party/signaling.hpp"
#include "party/protocol.hpp"
#include <curl/curl.h>
#include <sstream>
#include <stdexcept>
namespace party {
namespace {
struct Response { std::string body; };
size_t writeBody(char* ptr, size_t size, size_t count, void* userdata) {
    const auto n = size * count;
    auto& r = *static_cast<Response*>(userdata);
    if (r.body.size() + n > 2*1024*1024) return 0;
    r.body.append(ptr, n); return n;
}
Json http(const std::string& url, const std::string& method, const Json& data, const std::vector<std::string>& headers) {
    static const int initialized = [] { if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("curl initialization failed"); return 1; }(); (void)initialized;
    using Curl = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    thread_local Curl curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl allocation failed");
    curl_easy_reset(curl.get()); // Retains connection/DNS/TLS caches; removes prior headers and body.
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    list = curl_slist_append(list, "Content-Type: application/json");
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> holder(list, curl_slist_free_all);
    Response response; const auto payload = data.dump();
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    const auto ca=env("PARTY_CA_BUNDLE"); if(!ca.empty()) curl_easy_setopt(curl.get(),CURLOPT_CAINFO,ca.c_str());
    // No redirects or TLS override: capabilities stay at the configured origin.
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    if (method != "GET") { curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str()); curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, long(payload.size())); }
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) throw std::runtime_error("HTTPS request failed");
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    Json parsed = Json::parse(response.body, nullptr, false);
    if (status < 200 || status >= 300) {
        std::string code = "HTTP_ERROR";
        if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) code = parsed["error"].value("code", code);
        int index = -1;
        if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object()) {
            const auto& error = parsed["error"];
            if (error.contains("messageIndex") && error["messageIndex"].is_number_integer()) {
                const auto value = error["messageIndex"].get<int64_t>();
                if (value >= 0 && value < int64_t(protocol::signalBatch)) index = int(value);
            }
        }
        throw HttpError(status, code, index);
    }
    if (parsed.is_discarded()) throw std::runtime_error("Invalid API JSON");
    return parsed;
}
void requireHttps(const std::string& value) {
    if (!value.starts_with("https://") || value.size() <= 8 || value.find_first_of("@?#\r\n\t ") != std::string::npos)
        throw std::invalid_argument("Party backend must be an HTTPS origin without credentials, query or fragment");
}
}
PartyApi::PartyApi() : functionUrl_(env("PARTY_FUNCTION_URL")) {
    if (functionUrl_.empty()) functionUrl_ = "https://party.sgp.appwrite.run";
    requireHttps(functionUrl_);
    while (functionUrl_.ends_with('/')) functionUrl_.pop_back();
}
Json PartyApi::request(const std::string& method, const std::string& path, const Json& data) {
    if ((!path.starts_with("/v1/rooms") && !path.starts_with("/v3/rooms")) || path.find_first_of("?#\r\n\t ") != std::string::npos)
        throw std::invalid_argument("Invalid Party API path");
    std::vector<std::string> headers{"X-Party-Protocol: " + std::to_string(protocol::major)};
    if (!partyToken_.empty()) headers.push_back("Authorization: Bearer " + partyToken_);
    if (!sessionId.empty()) headers.push_back("X-Party-Session: " + sessionId);
    auto input = data;
    const bool bootstrap = partyToken_.empty() && method == "POST" && (path == "/v1/rooms" || path == "/v3/rooms" || path.ends_with("/join"));
    auto wirePath = path;
    if (canonical_ && wirePath.starts_with("/v1/")) wirePath.replace(0, 3, "/v3");
    if (bootstrap) input["requestKey"] = randomId() + randomId();
    Json result;
    // Retry uncertain bootstrap responses with the same private key: no duplicate members.
    for (unsigned attempt = 0;; ++attempt) {
        try { result = http(functionUrl_ + wirePath, method, input, headers); break; }
        catch (const HttpError& e) { if (!bootstrap || attempt == 2 || (e.status < 500 && e.code != "RETRY")) throw; }
        catch (const std::exception&) { if (!bootstrap || attempt == 2) throw; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * (attempt + 1)));
    }
    if (result.contains("protocol")) {
        const auto& metadata = result.at("protocol");
        if (!metadata.is_object() || metadata.value("name", "") != "coin-party" ||
            metadata.value("major", 0) != protocol::major || !metadata.contains("features") || !metadata.at("features").is_array())
            throw std::runtime_error("Unsupported Party protocol response");
        canonical_ = true;
    }
    if (result.contains("partyToken")) {
        const auto token = result.at("partyToken").get<std::string>();
        if (token.size() != 70 || !token.starts_with("party_") ||
            token.substr(6).find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid party credential response");
        partyToken_ = token;
        result.erase("partyToken"); // Never expose private capabilities in events/diagnostics.
    }
    if (result.contains("sessionId")) sessionId = result.at("sessionId").get<std::string>();
    if (result.contains("roomId")) roomId = result.at("roomId").get<std::string>();
    return result;
}
Signaling::Signaling(Request request, Json room, bool host, SignalingOptions options)
 : request_(std::move(request)), options_(options), room_(std::move(room)), host_(host) { room_.erase("inviteCode"); room_.erase("partyToken"); }
Signaling::Signaling(std::unique_ptr<PartyApi> api, Json room, bool host) : api_(std::move(api)), room_(std::move(room)), host_(host) {
    request_=[this](const auto& method,const auto& path,const auto& data){return api_->request(method,path,data);};
    room_.erase("inviteCode"); room_.erase("partyToken");
}
Signaling::~Signaling() { stop(); }
void Signaling::connecting(bool active) { fastPollUntilUs_ = active ? nowUs() + 35000000 : 0; }
void Signaling::start() { connecting(true); if(worker_.joinable()) throw std::logic_error("Signaling already started"); leaseDeadlineUs_=nowUs()+options_.leaseUs*5/6; worker_ = std::jthread([this](std::stop_token token) { run(token); }); }
void Signaling::send(const std::string& to, const std::string& generation, const Json& message) {
    Json input = message; input["to"] = to; input["generation"] = generation; input["messageId"] = randomId();
    if (Json{{"messages",Json::array({input})},{"acknowledgements",Json::array()},{"heartbeat",false}}.dump().size() > protocol::bodyBytes)
        throw std::invalid_argument("Signaling message exceeds request limit");
    std::lock_guard lock(mutex_);
    if (commands_.size() >= 128) throw std::runtime_error("Signaling queue overflow");
    commands_.push_back({{"action", "signals"}, {"input", input}});
}
std::string Signaling::authorize(const std::string& viewer, bool restart) {
    if (!host_) throw std::logic_error("Viewer cannot authorize stream");
    std::lock_guard lock(mutex_);
    if(commands_.size()>=128) throw std::runtime_error("Signaling queue overflow");
    const auto requestId = randomId();
    commands_.push_back({{"action", "peers"}, {"input", {{"viewerId", viewer}, {"restart", restart}, {"requestId", requestId}}}});
    return requestId;
}
std::vector<Json> Signaling::events() {
    std::lock_guard lock(mutex_); std::vector<Json> result(events_.begin(), events_.end()); events_.clear();
    auto incoming=delivery_.ready(nowUs()); result.insert(result.end(),incoming.begin(),incoming.end()); return result;
}
void Signaling::processed(const std::string& id,bool success) { std::lock_guard lock(mutex_); delivery_.processed(id,success,nowUs()); }
bool Signaling::current(const Json& message) { std::lock_guard lock(mutex_); return delivery_.current(message); }
void Signaling::run(std::stop_token stop) {
    const auto base="/v1/rooms/"+roomId();
    auto request=[&](const std::string& method,const std::string& path,const Json& data=Json::object()){return request_(method,base+path,data);};
    int64_t lastPoll=0,lastHeartbeat=nowUs(),ackRetryAt=0,leaseRefresh=nowUs();
    unsigned failures=0, commandFailures=0; bool suspended=false, eventOverflow=false, terminal=false;
    int64_t commandRetryAt=0, signalRetryAt=0;
    unsigned exchangeFailures=0;
    bool authorityReady=false;
    std::optional<Json> renewal;
    std::string currentSession=room_.value("sessionId","");
    const auto metadata = room_.value("protocol", Json::object());
    const auto features = metadata.value("features", Json::array());
    const bool exchangeEnabled = metadata.value("major", 0) == protocol::major && features.is_array() &&
        std::find(features.begin(), features.end(), "sync-exchange") != features.end();
    auto removeCommand = [&](const Json& command) {
        std::lock_guard lock(mutex_);
        const auto it = std::find(commands_.begin(), commands_.end(), command);
        if (it != commands_.end()) commands_.erase(it);
    };
    auto event=[&](Json value) {
        std::lock_guard lock(mutex_);
        const auto kind=value.at("event").get<std::string>();
        if(kind=="closed") events_.clear();
        if(kind=="room") std::erase_if(events_,[](const Json& e){return e.value("event","")=="room";});
        if(events_.size()>=128) {
            // A stalled application must fail closed without terminating this worker.
            events_.clear();events_.push_back({{"event","closed"},{"code","EVENT_QUEUE_FULL"}});
            eventOverflow=true;return;
        }
        events_.push_back(std::move(value));
    };
    int64_t iceRefreshAt = 0;
    if (room_.contains("iceServers") && env("PARTY_ICE_SERVERS").empty()) {
        auto settings = room_; settings["event"] = "ice"; event(settings);
        iceRefreshAt = nowUs() + 3540000000LL;
    }
    auto refreshIce = [&] {
        // Fetch on the HTTP worker before publishing room authority or offers.
        // Explicit local settings remain available for diagnostics and older backends.
        if (!api_ || !env("PARTY_ICE_SERVERS").empty() || nowUs() < iceRefreshAt) return;
        auto settings = request("GET", "/ice");
        const auto ttl = settings.at("ttlSeconds").get<int64_t>();
        if (!settings.at("iceServers").is_array() || ttl < 300 || ttl > 86400)
            throw std::runtime_error("Invalid backend ICE settings");
        settings["event"] = "ice"; event(settings);
        iceRefreshAt = nowUs() + (ttl - 60) * 1000000;
    };
    auto snapshot=[&](Json room) {
        std::vector<std::tuple<std::string,std::string,bool>> connections;
        if (host_) {
            if (auto it = room.find("connections"); it != room.end() && it->is_array()) {
                for (const auto& c : *it) {
                    if (!c.is_object()) continue;
                    const auto sender = c.value("viewerId", "");
                    const auto generation = c.value("generation", "");
                    if (!sender.empty() && !generation.empty())
                        connections.emplace_back(sender, generation, c.value("state", "") == "closed");
                }
            }
        } else {
            const auto connection = room.value("connection", Json());
            if (connection.is_object())
                connections.emplace_back(hostId(), connection.value("generation", ""), connection.value("state", "") == "closed");
        }
        {std::lock_guard lock(mutex_); delivery_.authorizeConnections(connections);}
        authorityReady=true;
        room["event"] = "room"; event(std::move(room));
    };
    auto renewSession=[&] {
        const auto joinedAt=nowUs();auto room=request("POST","/join",*renewal);
        currentSession=room.at("sessionId").get<std::string>();
        {std::lock_guard lock(mutex_);commands_.clear();delivery_.clear();}
        room["event"]="rejoined";event(room);suspended=false;leaseRefresh=joinedAt;
        leaseDeadlineUs_=joinedAt+options_.leaseUs*5/6;lastPoll=0;lastHeartbeat=joinedAt;
        iceRefreshAt=0;
        renewal.reset();failures=0;commandFailures=0;commandRetryAt=0;signalRetryAt=0;exchangeFailures=0;authorityReady=false;
    };
    while(!stop.stop_requested() && !eventOverflow) {
        try {
            if(renewal){renewSession();continue;}
            refreshIce(); // Obtain ICE settings before negotiating new peers.
            const auto now=nowUs();
            if(!exchangeEnabled && now-lastHeartbeat>=options_.heartbeatUs) {
                request("POST","/heartbeat"); lastHeartbeat=now; leaseRefresh=now;
                leaseDeadlineUs_=now+options_.leaseUs*5/6; // Request start is conservative under response latency.
                if(suspended){event({{"event","coordination-restored"}});suspended=false;}
            }
            Json acks;
            {std::lock_guard lock(mutex_);acks=delivery_.acknowledgements();}
            if(!exchangeEnabled && !acks.empty() && now>=ackRetryAt) {
                try {
                    request("POST","/ack",{{"messageIds",acks}});
                    std::lock_guard lock(mutex_);delivery_.acknowledged(acks,nowUs());
                } catch(const HttpError& e) {
                    if(e.status==401 || e.status==403 || e.status==410) throw;
                    ackRetryAt=now+options_.pollUs;
                } catch(const std::exception&) {ackRetryAt=now+options_.pollUs;}
            }
            std::optional<Json> command;
            {std::lock_guard lock(mutex_);
                if (exchangeEnabled) {
                    const auto it = std::find_if(commands_.begin(), commands_.end(), [](const Json& c){return c.at("action")=="peers";});
                    if (it != commands_.end()) command = *it;
                } else if(!commands_.empty()) command=commands_.front();
            }
            if(command && now>=commandRetryAt) {
                auto retryCommand=[&] {
                    // A full shared inbox needs the other direction to keep draining.
                    // Back off only this POST; never starve polling/ACKs/heartbeats.
                    const auto delay=std::min(10000u,250u*(1u<<std::min(++commandFailures,5u)));
                    commandRetryAt=nowUs()+int64_t(delay)*1000;
                };
                try {
                    auto result=request("POST","/"+command->at("action").get<std::string>(),command->at("input"));
                    if(command->at("action")=="peers") {
                        // Publish only after authority is refreshed; the application can consume events immediately.
                        snapshot(result.contains("room") ? result.at("room") : request("GET",""));
                        result.erase("room");
                        result["requestId"]=command->at("input").at("requestId");
                        result["event"]="authorized";event(result);
                    }
                    removeCommand(*command);
                    commandFailures=0;commandRetryAt=0;
                } catch(const HttpError& e) {
                    if(e.status==401 || (e.status==403 && e.code!="REMOTE_PEER_EXPIRED") || e.status==410 || e.status==426) throw;
                    if(e.status>=500 || e.status==429 || e.code=="RETRY") retryCommand();
                    else {
                        removeCommand(*command);
                        commandFailures=0;commandRetryAt=0;
                        Json rejected={{"event","error"},{"code",e.code}};
                        if(command->at("action")=="peers") {
                            rejected["viewerId"]=command->at("input").at("viewerId");
                            rejected["requestId"]=command->at("input").at("requestId");
                        }
                        event(std::move(rejected));
                    }
                } catch(const std::exception&) {retryCommand();}
            }
            if (exchangeEnabled && now >= ackRetryAt) {
                const bool pulse = now-lastHeartbeat >= options_.heartbeatUs;
                if (acks.size() > protocol::ackBatch) acks.erase(acks.begin()+protocol::ackBatch,acks.end());
                Json input = {{"messages",Json::array()},{"acknowledgements",Json::array()},{"heartbeat",pulse}};
                std::vector<Json> batch;
                {
                    std::lock_guard lock(mutex_);
                    // Superseded generations cannot poison an otherwise valid batch.
                    std::erase_if(commands_, [&](const Json& c){return authorityReady && c.at("action")=="signals" &&
                        !delivery_.current({{"from",c.at("input").at("to")},{"generation",c.at("input").at("generation")},{"type",c.at("input").at("type")}});});
                    for (const auto& c : commands_) if (authorityReady && now>=signalRetryAt && c.at("action")=="signals") {
                        input["messages"].push_back(c.at("input"));
                        if (input.dump().size() > protocol::bodyBytes) { input["messages"].erase(input["messages"].end()-1); break; }
                        batch.push_back(c);
                        if (batch.size() == protocol::signalBatch) break;
                    }
                }
                for (const auto& ack:acks) {
                    input["acknowledgements"].push_back(ack);
                    if (input.dump().size()>protocol::bodyBytes) {input["acknowledgements"].erase(input["acknowledgements"].end()-1);break;}
                }
                acks=input.at("acknowledgements");
                if (!batch.empty() || !acks.empty() || pulse) {
                    try {
                        const auto result = request("POST","/sync",input);
                        const auto expected = [&] { Json ids=Json::array(); for(const auto& c:batch)ids.push_back(c.at("input").at("messageId"));return ids; }();
                        if (result.at("accepted") != expected || !result.at("acknowledged").get<bool>() || result.at("heartbeat") != pulse)
                            throw std::runtime_error("Invalid coordination receipt");
                        snapshot(result.at("room"));
                        {std::lock_guard lock(mutex_); delivery_.ingest(result.at("messages"),nowUs()); delivery_.acknowledged(acks,nowUs());}
                        for (const auto& c:batch) removeCommand(c);
                        if(!batch.empty()){exchangeFailures=0;signalRetryAt=0;}
                        if (pulse) {
                            lastHeartbeat=now;leaseRefresh=now;leaseDeadlineUs_=now+options_.leaseUs*5/6;
                            if(suspended){event({{"event","coordination-restored"}});suspended=false;}
                        }
                        ackRetryAt=0;lastPoll=now;
                    } catch (const HttpError& e) {
                        if(e.status==401 || (e.status==403 && (e.messageIndex<0 || e.code=="PEER_EXPIRED" ||
                            e.code=="CREDENTIAL_REVOKED" || e.code=="STALE_SESSION" || e.code=="NOT_A_MEMBER")) || e.status==410 || e.status==426) throw;
                        if(e.status<500 && e.status!=429 && e.code!="RETRY" && e.messageIndex>=0 && size_t(e.messageIndex)<batch.size()) {
                            removeCommand(batch[size_t(e.messageIndex)]);
                            event({{"event","error"},{"code",e.code}});
                        }
                        if(!batch.empty()) {
                            // Rollback also undoes ACKs. Retry those independently
                            // while outbound messages back off, or two full inboxes
                            // could prevent each other from ever draining.
                            signalRetryAt=nowUs()+int64_t(std::min(10000u,250u*(1u<<std::min(++exchangeFailures,5u))))*1000;
                            ackRetryAt=0;
                        } else ackRetryAt=nowUs()+options_.pollUs;
                    } catch (const std::exception&) {
                        if(!batch.empty()) {
                            signalRetryAt=nowUs()+int64_t(std::min(10000u,250u*(1u<<std::min(++exchangeFailures,5u))))*1000;
                            ackRetryAt=0;
                        } else ackRetryAt=nowUs()+options_.pollUs;
                    }
                }
            }
            const auto pollUs = now < fastPollUntilUs_.load() ? std::min<int64_t>(options_.pollUs, 250000) : options_.pollUs;
            if(now-lastPoll>=pollUs) {
                const auto inbox=request("GET","/sync");
                snapshot(inbox.at("room"));
                {std::lock_guard lock(mutex_);delivery_.ingest(inbox.at("messages"),now);}
                lastPoll=now;
            }
            failures=0;
        } catch(const HttpError& e) {
            if(e.code=="PEER_EXPIRED") {
                if(!renewal)renewal={{"previousSession",currentSession},{"requestId",randomId()}};
                ++failures; // Retry own renewal before sending the old session header again.
            } else if(e.status==401 || e.status==403 || e.status==404 || e.status==410 || e.status==426) {
                // In particular, STALE_SESSION is terminal: never fight another instance by rejoining.
                terminal=true;event({{"event","closed"},{"code",e.code}});break;
            } else ++failures;
        } catch(const std::exception&) {++failures;}
        const auto outage=nowUs()-leaseRefresh;
        if(outage>options_.leaseUs*5/6 && !suspended) {event({{"event","coordination-lost"}});suspended=true;}
        if(outage>options_.recoveryUs){terminal=true;event({{"event","closed"},{"code","COORDINATION_UNAVAILABLE"}});break;}
        const auto delay=failures?std::min(10000u,250u*(1u<<std::min(failures,5u))):20u;
        for(unsigned slept=0;slept<delay && !stop.stop_requested();slept+=20)std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Network/auth failures must not implicitly end the host's room.
    // Only an explicit stop (normal player close / End Party) attempts to leave.
    if(stop.stop_requested() && !terminal && !eventOverflow) try{request("POST","/leave");}catch(const std::exception&){}
}
void Signaling::stop() { if (worker_.joinable()) { worker_.request_stop(); worker_.join(); } }
}
