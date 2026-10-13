#pragma once
#include "party/core.hpp"
#include <rtc/rtc.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
namespace party {
class HttpError final : public std::runtime_error {
public:
    HttpError(long status, std::string code) : std::runtime_error(code), status(status), code(std::move(code)) {}
    long status;
    std::string code;
};
class AppwriteApi {
public:
    AppwriteApi();
    Json request(const std::string& method, const std::string& path, const Json& data = Json::object());
    std::string jwt();
    std::string sessionId, roomId;
private:
    std::string functionUrl_, endpoint_, project_, jwt_, sessionSecret_;
    int64_t jwtExpiryUs_ = 0;
};
// All HTTP work happens on one worker. The render/network-pacer thread never blocks on Functions.
class Signaling {
public:
    Signaling(std::unique_ptr<AppwriteApi> api, Json room, bool host);
    ~Signaling();
    void start();
    void send(const std::string& to, const std::string& generation, const Json& message);
    void authorize(const std::string& viewerId, bool restart = false);
    std::vector<Json> events();
    void stop();
    std::string roomId() const { return room_.at("roomId"); }
    std::string userId() const { return room_.at("yourUserId"); }
    std::string hostId() const { return room_.at("hostId"); }
private:
    void run(std::stop_token);
    void notifyRealtime();
    std::unique_ptr<AppwriteApi> api_;
    Json room_;
    bool host_;
    std::jthread worker_;
    std::shared_ptr<rtc::WebSocket> realtime_;
    std::shared_ptr<std::atomic<bool>> wake_ = std::make_shared<std::atomic<bool>>(true);
    std::mutex mutex_;
    std::deque<Json> commands_, events_;
};
}
