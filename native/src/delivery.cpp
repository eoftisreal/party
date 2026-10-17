#include "party/delivery.hpp"
#include <algorithm>
#include <stdexcept>
namespace party {
namespace {
bool hexId(const Json& value) {
    if (!value.is_string()) return false;
    const auto& s = value.get_ref<const std::string&>();
    return s.size() == 32 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
}
void SignalDelivery::authorize(std::string generation, std::string sender, bool closed) {
    authorizeConnections({{std::move(sender), std::move(generation), closed}});
}
void SignalDelivery::authorizeConnections(const std::vector<std::tuple<std::string, std::string, bool>>& connections) {
    connections_.clear();
    for (const auto& [sender, generation, closed] : connections)
        if (!sender.empty() && !generation.empty())
            connections_[sender] = {generation, closed};
    for (auto& [id, entry] : entries_)
        if (!current(entry.message) && entry.state != State::Acked)
            entry.state = State::Applied;
}
bool SignalDelivery::current(const Json& message) const {
    if (!message.is_object() || !message.contains("generation") || !message.at("generation").is_string() ||
        !message.contains("from") || !message.at("from").is_string() ||
        !message.contains("type") || !message.at("type").is_string()) return false;
    const auto it = connections_.find(message.at("from").get<std::string>());
    return it != connections_.end() && message.at("generation") == it->second.first &&
        (!it->second.second || message.at("type") == "disconnect");
}
void SignalDelivery::ingest(const Json& messages, int64_t time) {
    if (!messages.is_array() || messages.size()>64) throw std::invalid_argument("Invalid signaling inbox");
    std::erase_if(entries_, [time](const auto& item){ return item.second.state == State::Acked && time > item.second.expiresAt; });
    for (const auto& message : messages) {
        if (!message.is_object() || !message.contains("messageId") || !hexId(message.at("messageId")) ||
            !message.contains("from") || !message.at("from").is_string() || message.at("from").get_ref<const std::string&>().size()>36 ||
            !message.contains("type") || !message.at("type").is_string() ||
            (message.at("type")!="offer" && message.at("type")!="answer" && message.at("type")!="candidate" && message.at("type")!="disconnect") ||
            !message.contains("generation") || !hexId(message.at("generation")) || !message.contains("payload") || !message.at("payload").is_object())
            throw std::invalid_argument("Invalid signaling message");
        const auto id = message.at("messageId").get<std::string>();
        if (auto it = entries_.find(id); it != entries_.end()) {
            if (it->second.message != message) throw std::invalid_argument("Conflicting signaling message ID");
            continue;
        }
        if (entries_.size() >= 256) {
            auto it = std::find_if(entries_.begin(),entries_.end(),[](const auto& item){return item.second.state==State::Acked;});
            if (it == entries_.end()) throw std::runtime_error("Signaling delivery queue full");
            entries_.erase(it);
        }
        Entry e{message, current(message) ? State::Pending : State::Applied, time, time+300000000, 0};
        entries_.emplace(id,std::move(e));
    }
}
std::vector<Json> SignalDelivery::ready(int64_t time) {
    std::vector<Json> result;
    for (auto& [id,e] : entries_) if (e.state==State::Pending && time>=e.readyAt) {
        if (!current(e.message)) { e.state=State::Applied; continue; }
        e.state=State::Processing; auto message=e.message; message["event"]="signal"; result.push_back(std::move(message));
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a,const auto& b){return (a.at("type")=="candidate")<(b.at("type")=="candidate");});
    return result;
}
void SignalDelivery::processed(const std::string& id, bool success, int64_t time) {
    const auto it=entries_.find(id); if(it==entries_.end() || it->second.state!=State::Processing) return;
    auto& e=it->second;
    if(success || !current(e.message)) e.state=State::Applied;
    else { e.state=State::Pending; e.readyAt=time+std::min<int64_t>(2000000,100000LL<<std::min(++e.retries,4u)); }
}
Json SignalDelivery::acknowledgements() const {
    Json ids=Json::array(); for(const auto& [id,e]:entries_) if(e.state==State::Applied && ids.size()<64) ids.push_back(id); return ids;
}
void SignalDelivery::acknowledged(const Json& ids,int64_t time) {
    for(const auto& id:ids) if(auto it=entries_.find(id.get<std::string>());it!=entries_.end() && it->second.state==State::Applied) {
        it->second.state=State::Acked; it->second.expiresAt=time+300000000;
    }
}
void SignalDelivery::clear() { entries_.clear(); connections_.clear(); }
}
