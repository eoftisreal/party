#pragma once
#include "party/core.hpp"
#include <map>
namespace party {
// Used under Signaling's mutex. Only the application can mark a message applied.
class SignalDelivery {
public:
    void authorize(std::string generation, std::string sender, bool closed);
    void ingest(const Json& messages, int64_t time);
    std::vector<Json> ready(int64_t time);
    void processed(const std::string& id, bool success, int64_t time);
    Json acknowledgements() const;
    void acknowledged(const Json& ids, int64_t time);
    bool current(const Json& message) const;
    void clear();
private:
    enum class State { Pending, Processing, Applied, Acked };
    struct Entry { Json message; State state = State::Pending; int64_t readyAt = 0, expiresAt = 0; unsigned retries = 0; };
    std::map<std::string, Entry> entries_;
    std::string generation_, sender_;
    bool closed_ = true;
};
}
