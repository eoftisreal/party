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

    std::atomic<unsigned> polls{0},ackCalls{0};std::atomic<bool> removed{false};
    Json room={{"roomId",id('c')},{"yourUserId","viewer"},{"hostId","host"},
        {"connection",{{"generation",id('a')},{"viewerId","viewer"},{"state","offered"}}}};
    Signaling s([&](const std::string&,const std::string& path,const Json&)->Json{
        if(path.ends_with("/signals")){++polls;return {{"messages",removed?Json::array():Json::array({message('5')})}};}
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
    std::cout<<"Signaling post-apply ACK, retries, deduplication and generation checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
