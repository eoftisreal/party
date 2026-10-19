#include "party/signaling.hpp"
#include <iostream>
#include <thread>
#include <stdexcept>
namespace {
void expect(bool v,const char* m){if(!v)throw std::runtime_error(m);}
std::string id(char c){return std::string(32,c);}
party::Json message(char c,const std::string& type="offer",char gen='a'){
    return {{"messageId",id(c)},{"generation",id(gen)},{"from","host"},{"type",type},{"payload",party::Json::object()}};
}
}
int main(){using namespace party;try{
    SignalDelivery d;d.authorize(id('a'),"host",false);
    d.ingest(Json::array({message('1',"candidate"),message('2')}),0);
    expect(d.acknowledgements().empty(),"Fetched messages acknowledged prematurely");
    auto ready=d.ready(0);expect(ready.size()==2 && ready[0].at("type")=="offer","Offer must precede ICE");
    d.processed(id('2'),false,0);d.processed(id('1'),false,0);
    expect(d.acknowledgements().empty(),"Failed application acknowledged");
    expect(d.ready(100).empty(),"Retry backoff missing");
    ready=d.ready(300000);expect(ready.size()==2,"Failed signal was lost");
    d.processed(id('2'),true,300000);d.processed(id('1'),true,300000);
    expect(d.acknowledgements().size()==2,"Applied messages not acknowledged");
    d.ingest(Json::array({message('2')}),400000);expect(d.ready(400000).empty(),"Lost ACK response reapplied SDP");
    auto acks=d.acknowledgements();d.acknowledged(acks,400000);
    d.ingest(Json::array({message('2')}),500000);expect(d.ready(500000).empty(),"Duplicate delivered after ACK");
    d.authorize(id('b'),"host",false);d.ingest(Json::array({message('3')}),600000);
    expect(d.ready(600000).empty() && d.acknowledgements().size()==1,"Stale generation was applied");
    d.ingest(Json::array({message('4',"offer",'b')}),600000);expect(d.ready(600000).size()==1,"New generation missing");
    auto changed=message('4',"offer",'b');changed["payload"]={{"bad",true}};
    bool rejected=false;try{d.ingest(Json::array({changed}),600001);}catch(const std::invalid_argument&){rejected=true;}
    expect(rejected,"Conflicting duplicate identifier accepted");
    // Independent viewer generations coexist; replacing one leaves the other active.
    SignalDelivery multi;
    multi.authorizeConnections({{"viewer-a",id('a'),false},{"viewer-b",id('b'),false}});
    auto va=message('7',"answer",'a'), vb=message('8',"answer",'b');
    va["from"]="viewer-a"; vb["from"]="viewer-b";
    multi.ingest(Json::array({va,vb}),0);
    expect(multi.ready(0).size()==2,"Both viewers must receive signals");
    multi.authorizeConnections({{"viewer-a",id('c'),false},{"viewer-b",id('b'),false}});
    expect(!multi.current(va) && multi.current(vb),"Replaced viewer invalidated another viewer");

    std::atomic<unsigned> polls{0},ackCalls{0};std::atomic<bool> removed{false};
    Json room={{"roomId",id('c')},{"sessionId",id('9')},{"yourUserId","viewer"},{"hostId","host"},
        {"connection",{{"generation",id('a')},{"viewerId","viewer"},{"state","offered"}}}};
    Signaling s([&](const std::string&,const std::string& path,const Json&)->Json{
        if(path.ends_with("/sync")){++polls;return {{"room",room},{"messages",removed?Json::array():Json::array({message('5')})}};}
        if(path.ends_with("/ack")){removed=true;if(++ackCalls==1)throw std::runtime_error("Lost ACK response");return {{"acknowledged",true}};}
        return room;
    },room,false,{20000,100000,2000000,4000000});
    s.start();bool applied=false;unsigned delivered=0;const auto start=nowUs();
    while(nowUs()-start<2000000){
        for(const auto& event:s.events())if(event.at("event")=="signal"){
            ++delivered;expect(ackCalls==0,"Worker acknowledged before application");
            s.processed(event.at("messageId"),true);applied=true;
        }
        if(applied && ackCalls>=2)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    s.stop();expect(delivered==1 && ackCalls>=2 && polls>0,"Lost ACK response broke delivery worker");
    auto until=[&](Signaling& worker,const std::function<bool(const Json&)>& predicate){
        const auto deadline=nowUs()+4000000;bool found=false;
        while(nowUs()<deadline && !found){for(const auto& e:worker.events())if(predicate(e))found=true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        expect(found,"Signaling recovery event timed out");
    };
    std::atomic<unsigned> joins{0},hearts{0};
    Signaling expired([&](const std::string&,const std::string& path,const Json&)->Json{
        if(path.ends_with("/heartbeat") && ++hearts==1) throw HttpError(403,"PEER_EXPIRED");
        if(path.ends_with("/join"))++joins;
        if(path.ends_with("/sync"))return {{"room",room},{"messages",Json::array()}};
        return room;
    },room,false,{20000,20000,2000000,4000000});
    expired.start();until(expired,[](const Json& e){return e.at("event")=="rejoined";});expired.stop();
    expect(joins==1,"Expired membership was not rejoined exactly once");
    joins=0;hearts=0;Json renewalInput;bool renewalCommitted=false;
    Signaling lostRenewal([&](const std::string&,const std::string& path,const Json& input)->Json{
        if(path.ends_with("/heartbeat") && ++hearts==1)throw HttpError(403,"PEER_EXPIRED");
        if(path.ends_with("/join")){
            ++joins;
            if(!renewalCommitted){renewalInput=input;renewalCommitted=true;throw std::runtime_error("Lost renewal response");}
            expect(input==renewalInput,"Renewal retry changed request identifier");
            Json next=room;next["sessionId"]=id('b');return next;
        }
        if(renewalCommitted && joins<2 && path.ends_with("/heartbeat"))throw HttpError(403,"STALE_SESSION");
        if(path.ends_with("/sync"))return {{"room",room},{"messages",Json::array()}};return room;
    },room,false,{20000,20000,2000000,6000000});
    lostRenewal.start();until(lostRenewal,[](const Json& e){return e.at("event")=="rejoined";});lostRenewal.stop();
    expect(joins==2,"Lost renewal response broke recovery");
    joins=0;
    Signaling replaced([&](const std::string&,const std::string& path,const Json&)->Json{
        if(path.ends_with("/join"))++joins;
        if(path.ends_with("/heartbeat"))throw HttpError(403,"STALE_SESSION");return room;
    },room,false,{20000,20000,2000000,4000000});
    replaced.start();until(replaced,[](const Json& e){return e.at("event")=="closed" && e.at("code")=="STALE_SESSION";});replaced.stop();
    expect(joins==0,"Replaced instance fought the new session by rejoining");
    std::atomic<bool> offline{true};
    Signaling outage([&](const std::string&,const std::string& path,const Json&)->Json{
        if(offline && !path.ends_with("/leave"))throw std::runtime_error("offline");
        if(path.ends_with("/sync"))return {{"room",room},{"messages",Json::array()}};return room;
    },room,false,{20000,20000,100000,3000000});
    outage.start();until(outage,[](const Json& e){return e.at("event")=="coordination-lost";});offline=false;
    until(outage,[](const Json& e){return e.at("event")=="coordination-restored";});outage.stop();
    std::atomic<bool> entered{false},release{false};unsigned blockedHearts=0;
    Signaling blocked([&](const std::string&,const std::string& path,const Json&)->Json{
        if(path.ends_with("/heartbeat") && ++blockedHearts==1){entered=true;while(!release)std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        if(path.ends_with("/sync"))return {{"room",room},{"messages",Json::array()}};return room;
    },room,false,{20000,20000,200000,4000000});
    blocked.start();while(!entered)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    const bool staleDuringRequest=!blocked.leaseHealthy();release=true;
    until(blocked,[](const Json& e){return e.at("event")=="coordination-restored";});blocked.stop();
    expect(staleDuringRequest,"Blocked HTTPS worker extended the media authorization lease");
    Json hostRoom=room;hostRoom["yourUserId"]="host";
    hostRoom["connections"]=Json::array({room.at("connection")});std::string requestId;unsigned authorizations=0;
    Signaling retry([&](const std::string&,const std::string& path,const Json& input)->Json{
        if(path.ends_with("/peers")){
            if(requestId.empty())requestId=input.at("requestId");else expect(requestId==input.at("requestId").get<std::string>(),"Authorization retry changed request identifier");
            if(++authorizations==1)throw std::runtime_error("Lost authorization response");
            return {{"generation",id('a')},{"viewerId","viewer"}};
        }
        if(path.ends_with("/sync"))return {{"room",hostRoom},{"messages",Json::array()}};
        if(authorizations==2 && !path.ends_with("/heartbeat") && !path.ends_with("/leave"))std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return hostRoom;
    },hostRoom,true,{20000,20000,2000000,4000000});
    retry.authorize("viewer",true);retry.start();until(retry,[&](const Json& e){
        if(e.at("event")!="authorized")return false;
        expect(e.at("requestId").get<std::string>()==requestId,"Authorization result lost its request correlation");
        expect(retry.current({{"generation",e.at("generation")},{"from",e.at("viewerId")},{"type","answer"}}),"Authorization event preceded authoritative generation refresh");return true;
    });retry.stop();
    expect(authorizations==2,"Authorization retry did not complete");
    // The server has one shared bounded inbox for both directions. A rejected
    // outgoing POST must not prevent receiving and ACKing the queued offer.
    for(const auto status:{429L,503L,409L,0L}) {
        std::atomic<unsigned> posts{0},incomingPolls{0},drained{0},sent{0};
        Json original;std::atomic<bool> sameId{true};
        Signaling congested([&](const std::string& method,const std::string& path,const Json& input)->Json {
            if(path.ends_with("/signals") && method=="POST") {
                ++posts;if(original.is_null())original=input;else if(original!=input)sameId=false;
                if(!drained) {
                    if(status==0)throw std::runtime_error("Lost HTTP response");
                    throw HttpError(status,status==429?"INBOX_FULL":status==409?"RETRY":"UNAVAILABLE");
                }
                ++sent;return {{"accepted",true}};
            }
            if(path.ends_with("/sync")) {++incomingPolls;return {{"room",room},{"messages",drained?Json::array():Json::array({message('6')})}};}
            if(path.ends_with("/ack")){++drained;return {{"acknowledged",true}};}
            return room;
        },room,false,{20000,20000,2000000,4000000});
        congested.send("host",id('a'),{{"type","candidate"}});congested.start();
        const auto deadline=nowUs()+3000000;unsigned applications=0;
        while(nowUs()<deadline && !sent) {
            for(const auto& e:congested.events())if(e.at("event")=="signal") {
                ++applications;congested.processed(e.at("messageId"),true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        congested.stop();
        expect(sent==1 && drained>0 && incomingPolls>0 && applications==1,"Outgoing retry starved inbox processing/ACKs");
        expect(posts==2 && sameId,"Command retry spun or changed message identifier");
    }
    // v3.1 folds outbound messages, post-application ACKs and heartbeat into
    // exchanges. A lost committed response must retain original message IDs.
    Json v3Room=hostRoom;
    v3Room["protocol"]={{"major",3},{"features",Json::array({"sync-exchange","peer-snapshot"})}};
    std::atomic<unsigned> exchangePosts{0},exchangeAcks{0},legacyWrites{0},extraReads{0};
    std::atomic<bool> batchRetried{false};Json firstBatch;std::string firstAction;
    Signaling exchange([&](const std::string& method,const std::string& path,const Json& input)->Json {
        if(firstAction.empty())firstAction=path;
        if(path.ends_with("/peers"))return {{"generation",id('a')},{"viewerId","viewer"},{"room",v3Room}};
        if(path.ends_with("/signals") || path.ends_with("/ack") || path.ends_with("/heartbeat"))++legacyWrites;
        if(path.ends_with("/sync")) {
            Json incoming=message('e',"answer");incoming["from"]="viewer";
            Json result={{"room",v3Room},{"messages",exchangeAcks?Json::array():Json::array({incoming})}};
            if(method=="POST") {
                ++exchangePosts;
                if(!input.at("messages").empty()) {
                    if(firstBatch.is_null()){firstBatch=input.at("messages");throw std::runtime_error("Lost exchange response");}
                    expect(firstBatch==input.at("messages"),"Exchange retry changed messages or identifiers");batchRetried=true;
                }
                exchangeAcks+=unsigned(input.at("acknowledgements").size());
                Json accepted=Json::array();for(const auto& m:input.at("messages"))accepted.push_back(m.at("messageId"));
                result["accepted"]=accepted;result["acknowledged"]=true;result["heartbeat"]=input.at("heartbeat");
            }
            return result;
        }
        if(!path.ends_with("/leave"))++extraReads;
        return v3Room;
    },v3Room,true,{20000,20000,2000000,4000000});
    exchange.send("viewer",id('a'),{{"type","offer"}});
    exchange.send("viewer",id('a'),{{"type","candidate"}});
    exchange.authorize("viewer");exchange.start();
    const auto exchangeDeadline=nowUs()+3000000;unsigned applications=0;
    while(nowUs()<exchangeDeadline && !(batchRetried && exchangeAcks>0)) {
        for(const auto& e:exchange.events())if(e.at("event")=="signal") {++applications;exchange.processed(e.at("messageId"),true);}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    exchange.stop();
    expect(firstAction.ends_with("/peers"),"Negotiation did not take priority over coordination");
    expect(batchRetried && firstBatch.size()==2 && exchangeAcks>0 && applications==1,"Exchange response loss broke reliable batching");
    expect(legacyWrites==0 && extraReads==0,"Exchange used redundant heartbeat, ACK or authorization snapshot requests");
    std::atomic<unsigned> setupPolls{0};
    Signaling fast([&](const std::string&,const std::string& path,const Json&)->Json {
        if(path.ends_with("/sync")) { ++setupPolls; return {{"room",room},{"messages",Json::array()}}; }
        return room;
    },room,false,{2000000,10000000,120000000,300000000});
    fast.start();
    const auto fastDeadline=nowUs()+1500000;
    while(setupPolls<3 && nowUs()<fastDeadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool accelerated=setupPolls>=3;
    fast.connecting(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto beforeIdle=setupPolls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto afterIdle=setupPolls.load(); fast.stop();
    expect(accelerated,"Connection setup still waits for idle polling interval");
    expect(beforeIdle==afterIdle,"Fast polling continued after connection");
    expect(SignalingOptions{}.recoveryUs==1800000000LL,"Default reconnect window is not 30 minutes");
    std::atomic<unsigned> leaves{0};
    std::atomic<bool> temporaryOffline{true};
    Signaling guestOutage([&](const std::string&,const std::string& path,const Json&)->Json {
        if(path.ends_with("/leave")){++leaves;return Json::object();}
        if(temporaryOffline)throw std::runtime_error("weak connection");
        if(path.ends_with("/sync"))return {{"room",room},{"messages",Json::array()}};
        return hostRoom;
    },hostRoom,true,{20000,20000,100000,3000000});
    guestOutage.start();until(guestOutage,[](const Json& e){return e.at("event")=="coordination-lost";});
    expect(leaves==0,"A weak connection closed the host room");temporaryOffline=false;
    until(guestOutage,[](const Json& e){return e.at("event")=="coordination-restored";});
    guestOutage.stop();expect(leaves==1,"Explicit shutdown did not leave the room");
    leaves=0;
    Signaling timedOut([&](const std::string&,const std::string& path,const Json&)->Json {
        if(path.ends_with("/leave")){++leaves;return Json::object();}
        throw std::runtime_error("offline");
    },hostRoom,true,{20000,20000,100000,150000});
    timedOut.start();until(timedOut,[](const Json& e){return e.at("event")=="closed" && e.at("code")=="COORDINATION_UNAVAILABLE";});
    timedOut.stop();expect(leaves==0,"Recovery timeout sent a destructive leave request");
    Signaling bounded([](const auto&,const auto&,const auto&){return Json::object();},room,false);
    for(unsigned i=0;i<128;++i)bounded.send("host",id('a'),{{"type","candidate"}});
    rejected=false;try{bounded.send("host",id('a'),{{"type","candidate"}});}catch(const std::runtime_error&){rejected=true;}
    expect(rejected,"Outgoing signaling queue unbounded");
    std::cout<<"Signaling post-apply ACK, retries, deduplication and generation checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
