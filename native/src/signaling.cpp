#include "party/signaling.hpp"
#include <curl/curl.h>
#include <fstream>
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
    Curl curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl allocation failed");
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
    // Deliberately no redirects and no TLS-verification override: JWTs stay at configured origin.
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
        throw HttpError(status, code);
    }
    if (parsed.is_discarded()) throw std::runtime_error("Invalid API JSON");
    return parsed;
}
std::string readJwtFile() {
    const auto path = env("PARTY_JWT_FILE");
    if (path.empty()) return env("PARTY_JWT");
    std::ifstream file(path); if (!file) throw std::runtime_error("Cannot read JWT file");
    std::string jwt; std::getline(file, jwt); return jwt;
}
void requireHttps(const std::string& value) { if (!value.starts_with("https://")) throw std::invalid_argument("Appwrite endpoints must use HTTPS"); }
}
AppwriteApi::AppwriteApi() : functionUrl_(env("PARTY_FUNCTION_URL")), endpoint_(env("PARTY_APPWRITE_ENDPOINT")), project_(env("PARTY_PROJECT_ID")), sessionSecret_(env("PARTY_SESSION_SECRET")) {
    requireHttps(functionUrl_); while (functionUrl_.ends_with('/')) functionUrl_.pop_back();
    if (!sessionSecret_.empty()) { requireHttps(endpoint_); if (project_.empty()) throw std::invalid_argument("Appwrite project required for session auth"); }
}
std::string AppwriteApi::jwt() {
    if (sessionSecret_.empty()) jwt_ = readJwtFile();
    else if (nowUs() >= jwtExpiryUs_) {
        jwt_ = http(endpoint_ + "/account/jwts", "POST", Json::object(), {"X-Appwrite-Project: " + project_, "X-Appwrite-Session: " + sessionSecret_}).at("jwt").get<std::string>();
        jwtExpiryUs_ = nowUs() + 600000000;
    }
    if (jwt_.empty() || jwt_.size() > 8192 || jwt_.find_first_of("\r\n") != std::string::npos) throw std::invalid_argument("Valid PARTY_JWT(_FILE) or PARTY_SESSION_SECRET required");
    return jwt_;
}
Json AppwriteApi::request(const std::string& method, const std::string& path, const Json& data) {
    auto result = http(functionUrl_ + path, method, data, {"Authorization: Bearer " + jwt(), "X-Party-Session: " + sessionId});
    if (result.contains("sessionId")) sessionId = result.at("sessionId").get<std::string>();
    if (result.contains("roomId")) roomId = result.at("roomId").get<std::string>();
    return result;
}
Signaling::Signaling(std::unique_ptr<AppwriteApi> api, Json room, bool host) : api_(std::move(api)), room_(std::move(room)), host_(host) {}
Signaling::~Signaling() { stop(); }
void Signaling::start() { worker_ = std::jthread([this](std::stop_token token) { run(token); }); }
void Signaling::send(const std::string& to, const std::string& generation, const Json& message) {
    Json input = message; input["to"] = to; input["generation"] = generation; input["messageId"] = randomId();
    std::lock_guard lock(mutex_);
    if (commands_.size() >= 128) throw std::runtime_error("Signaling queue overflow");
    commands_.push_back({{"action", "signals"}, {"input", input}});
}
void Signaling::authorize(const std::string& viewer, bool restart) {
    if (!host_) throw std::logic_error("Viewer cannot authorize stream");
    std::lock_guard lock(mutex_); commands_.push_back({{"action", "peers"}, {"input", {{"viewerId", viewer}, {"restart", restart}}}});
}
std::vector<Json> Signaling::events() { std::lock_guard lock(mutex_); std::vector<Json> result(events_.begin(), events_.end()); events_.clear(); return result; }
void Signaling::notifyRealtime() {
    const auto endpoint = env("PARTY_APPWRITE_ENDPOINT"), project = env("PARTY_PROJECT_ID"), database = env("PARTY_DATABASE_ID");
    if (endpoint.empty() || project.empty() || database.empty()) return;
    requireHttps(endpoint);
    auto url = endpoint; url.replace(0, 5, "wss");
    // Current official SDK uses project-only URL + subscribe/authentication messages.
    // Session authentication avoids exposing JWTs in URLs. Polling works if older servers reject this protocol.
    if (env("PARTY_SESSION_SECRET").empty()) return;
    rtc::WebSocketConfiguration cfg; cfg.maxMessageSize = 65536; cfg.connectionTimeout = std::chrono::seconds(5); cfg.pingInterval = std::chrono::seconds(20);
    auto ws = std::make_shared<rtc::WebSocket>(cfg); const auto weak = std::weak_ptr<rtc::WebSocket>(ws);
    const auto wake = wake_;
    const auto secret = env("PARTY_SESSION_SECRET");
    const auto channel = "databases." + database + ".collections." + (env("PARTY_NOTIFICATIONS_COLLECTION_ID").empty() ? "notifications" : env("PARTY_NOTIFICATIONS_COLLECTION_ID")) + ".documents";
    ws->onMessage(nullptr, [weak, wake, secret, channel](std::string text) {
        try {
            const auto message = Json::parse(text); const auto type = message.value("type", "");
            if (auto socket = weak.lock(); socket && type == "connected") {
                socket->send(Json{{"type", "authentication"}, {"data", {{"session", secret}}}}.dump());
                socket->send(Json{{"type", "subscribe"}, {"data", Json::array({{{"subscriptionId", "party"}, {"channels", Json::array({channel})}, {"queries", Json::array()}}})}}.dump());
            }
            if (type == "event") wake->store(true);
        } catch (const std::exception&) { /* polling is authoritative */ }
    });
    ws->onError([](std::string) { /* no secret-containing URL/error logging */ });
    ws->open(url + "/realtime?project=" + project); realtime_ = ws;
}
void Signaling::run(std::stop_token stop) {
    const auto base = "/v1/rooms/" + roomId();
    int64_t lastPoll = 0, lastRoom = 0, lastHeartbeat = 0, lastRealtime = 0;
    unsigned failures = 0;
    int64_t leaseRefresh = nowUs();
    auto event = [&](Json value) { std::lock_guard lock(mutex_); if (events_.size() >= 256) throw std::runtime_error("Signaling event overflow"); events_.push_back(std::move(value)); };
    while (!stop.stop_requested()) {
        try {
            const auto now = nowUs();
            if ((!realtime_ || realtime_->isClosed()) && now - lastRealtime > 10000000) { lastRealtime = now; notifyRealtime(); }
            if (now - lastHeartbeat >= 10000000) { api_->request("POST", base + "/heartbeat"); lastHeartbeat = now; leaseRefresh = nowUs(); }
            std::optional<Json> command;
            { std::lock_guard lock(mutex_); if (!commands_.empty()) command = commands_.front(); }
            if (command) {
                try {
                    auto result = api_->request("POST", base + "/" + command->at("action").get<std::string>(), command->at("input"));
                    if (command->at("action") == "peers") { result["event"] = "authorized"; event(result); }
                    std::lock_guard lock(mutex_); commands_.pop_front();
                } catch (const HttpError& e) {
                    if (e.status >= 500 || e.status == 429 || e.code == "RETRY") throw;
                    { std::lock_guard lock(mutex_); commands_.pop_front(); }
                    event({{"event", "error"}, {"code", e.code}});
                }
            }
            if (now - lastRoom >= 2000000) {
                auto result = api_->request("GET", base); result["event"] = "room"; event(result); lastRoom = now;
            }
            if (now - lastPoll >= 2000000 || wake_->exchange(false)) {
                const auto inbox = api_->request("GET", base + "/signals");
                Json ids = Json::array();
                for (auto message : inbox.at("messages")) { ids.push_back(message.at("messageId")); message["event"] = "signal"; event(message); }
                if (!ids.empty()) api_->request("POST", base + "/ack", {{"messageIds", ids}});
                lastPoll = now;
            }
            failures = 0;
        } catch (const HttpError& e) {
            if (e.status == 401 || e.status == 403 || e.status == 404 || e.status == 410) {
                event({{"event", "closed"}, {"code", e.code}}); break;
            }
            ++failures;
        } catch (const std::exception&) { ++failures; }
        if (nowUs() - leaseRefresh > 120000000) { event({{"event", "closed"}, {"code", "COORDINATION_UNAVAILABLE"}}); break; }
        const auto delay = failures ? std::min(10000u, 250u * (1u << std::min(failures, 5u))) : 20u;
        for (unsigned slept = 0; slept < delay && !stop.stop_requested(); slept += 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (realtime_) { realtime_->resetCallbacks(); realtime_->close(); }
    try { api_->request("POST", base + "/leave"); } catch (const std::exception&) { /* expired/closed rooms already revoke streaming */ }
}
void Signaling::stop() { if (worker_.joinable()) { worker_.request_stop(); worker_.join(); } }
}
