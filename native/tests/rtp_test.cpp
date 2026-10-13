#include "party/rtp.hpp"
#include <iostream>
#include <stdexcept>
namespace {
void expect(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
rtc::message_ptr packet(uint16_t seq, uint32_t timestamp, bool marker, std::vector<uint8_t> payload) {
    rtc::binary bytes(12+payload.size()); auto* h = reinterpret_cast<rtc::RtpHeader*>(bytes.data());
    h->preparePacket(); h->setSeqNumber(seq); h->setTimestamp(timestamp); h->setSsrc(1); h->setPayloadType(96); h->setMarker(marker);
    std::copy(payload.begin(),payload.end(),reinterpret_cast<uint8_t*>(bytes.data())+12);
    return rtc::make_message(std::move(bytes));
}
}
int main() {
    using namespace party;
    try {
        std::vector<EncodedFrame> frames; unsigned requests=0,nacks=0;
        RtpReceiver receiver(true,[&](auto frame){frames.push_back(std::move(frame));},[&]{++requests;});
        const int64_t anchor=1000000; receiver.setAnchor(anchor);
        auto input=[&](rtc::message_ptr message){rtc::message_vector messages{message};receiver.incoming(messages,[&](auto m){if(m->type==rtc::Message::Control)++nacks;});receiver.pump(nowUs());};
        // A NAL unit spanning the 16-bit sequence wrap; fragment 0 arrives ahead of 65535.
        input(packet(65534,90000,false,{0x7c,0x85,0xaa}));
        input(packet(0,90000,true,{0x7c,0x45,0xcc}));
        input(packet(65535,90000,false,{0x7c,0x05,0xbb}));
        receiver.pump(nowUs()+100000);
        expect(frames.size()==1,"FU-A wrap/reordering reconstruction failed");
        expect(frames[0].data==std::vector<uint8_t>({0,0,0,1,0x65,0xaa,0xbb,0xcc}),"FU-A bytes corrupted");
        expect(frames[0].transportUs==anchor,"Wrong reconstructed RTP timestamp");
        expect(nacks>0 && receiver.stats->lost==0,"Reordered packet should NACK without unrecoverable loss");
        input(packet(1,93000,false,{0x7c,0x85,0xaa}));
        input(packet(3,93000,true,{0x7c,0x45,0xcc}));
        receiver.pump(nowUs()+100000);
        expect(frames.size()==1 && requests>0 && receiver.stats->lost==1,"Incomplete FU must be discarded with recovery request");
        input(packet(4,96000,true,{0x65,0xdd}));receiver.pump(nowUs()+100000);
        expect(frames.size()==2,"Receiver did not recover after missing fragment");
        input(packet(5,99000,true,{0x78,0,20,0x65}));
        expect(receiver.stats->frames==2,"Malformed STAP-A was emitted");
        PacketGuard guard; rtc::message_vector bad{rtc::make_message(4,rtc::Message::Control)};
        guard.incoming(bad,[](auto){}); expect(bad.empty(),"Truncated RTCP passed guard");
        std::cout << "RTP reorder, loss, wrap and hostile-payload checks passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n';return 1; }
}
