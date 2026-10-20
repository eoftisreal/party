#include "party/rtp.hpp"
#include "party/transport.hpp"
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
        // Packets before a clock state must not bias jitter by an entire monotonic epoch.
        RtpReceiver cold(true,[](auto){},[]{});
        rtc::message_vector pre{packet(10,90000,true,{0x65,1})};cold.incoming(pre,[](auto){});cold.pump(nowUs());
        cold.setAnchor(anchor);rtc::message_vector post{packet(11,93000,true,{0x65,1})};cold.incoming(post,[](auto){});
        expect(cold.stats->jitterUs==0,"Missing clock anchor inflated jitter");
        // Clock state can arrive between the SPS/PPS and the final IDR slice.
        // Never publish the tail of that access unit to a fresh decoder.
        std::vector<EncodedFrame> anchoredFrames; unsigned anchorRequests=0;
        RtpReceiver lateClock(true,[&](auto frame){anchoredFrames.push_back(std::move(frame));},[&]{++anchorRequests;});
        auto clockInput=[&](rtc::message_ptr message){rtc::message_vector messages{message};lateClock.incoming(messages,[](auto){});lateClock.pump(nowUs());};
        clockInput(packet(100,90000,false,{0x67,1}));
        clockInput(packet(101,90000,false,{0x68,2}));
        lateClock.setAnchor(anchor);
        clockInput(packet(102,90000,true,{0x65,3}));
        expect(anchoredFrames.empty(),"Clock activation emitted an incomplete IDR access unit");
        clockInput(packet(103,93000,true,{0x65,4}));
        expect(anchoredFrames.size()==1 && anchorRequests>0,"Clock activation did not recover at the next access unit");
        expect(anchoredFrames.back().data==std::vector<uint8_t>({0,0,0,1,0x65,4}),"Clock activation corrupted the next access unit");
        clockInput(packet(104,96000,false,{0x67,5}));
        lateClock.setAnchor(0);
        clockInput(packet(105,96000,false,{0x68,6}));
        lateClock.setAnchor(anchor);
        clockInput(packet(106,96000,true,{0x65,7}));
        expect(anchoredFrames.size()==1,"Clock loss mixed access units across clock states");
        lateClock.setAnchor(0);
        clockInput(packet(107,99000,true,{0x65,8}));
        lateClock.setAnchor(anchor);
        clockInput(packet(108,102000,true,{0x65,9}));
        expect(anchoredFrames.size()==2,"A clockless marker unnecessarily discarded the next complete access unit");
        const auto count=frames.size();receiver.setCutoff(anchor+100000);
        input(packet(6,96000,true,{0x65,1}));receiver.pump(nowUs()+300000);
        expect(frames.size()==count,"Previous playback epoch leaked after seek");
        input(packet(50,108000,false,{0x7c,0x85,1}));
        input(packet(51,108000,true,{0x65,2}));receiver.pump(nowUs()+300000);
        expect(frames.size()==count,"Interrupted FU-A emitted corrupt frame");
        input(packet(52,111000,true,{0xe5,2}));receiver.pump(nowUs()+300000);
        expect(frames.size()==count,"H264 forbidden bit accepted");
        receiver.setReorderWindow(200000);
        input(packet(53,114000,false,{0x7c,0x85,1}));
        input(packet(55,114000,true,{0x7c,0x45,3}));receiver.pump(nowUs()+100000);
        input(packet(54,114000,false,{0x7c,0x05,2}));receiver.pump(nowUs()+210000);
        expect(frames.size()==count+1,"Retransmission discarded before longer RTT deadline");
        // More than 1024 missing packets must recover without replacing ICE.
        const auto beforeBurst=frames.size();const auto requestsBeforeBurst=requests;
        input(packet(56,117000,false,{0x7c,0x85,1}));
        input(packet(2056,120000,true,{0x65,9}));
        expect(frames.size()==beforeBurst+1 && requests>requestsBeforeBurst,"Long RTP loss burst pinned the receive sequence forever");
        expect(frames.back().data==std::vector<uint8_t>({0,0,0,1,0x65,9}),"Pre-gap partial access unit leaked into recovered IDR");
        input(packet(57,117000,true,{0x7c,0x45,2}));
        expect(frames.size()==beforeBurst+1,"Delayed pre-gap packet corrupted recovery");
        input(packet(2057,123000,true,{0x65,10}));
        expect(frames.size()==beforeBurst+2,"Post-burst RTP did not continue");
        // RFC 4585: one 16-byte PID/BLP NACK requests up to 17 missing
        // sequence numbers; verify exact network bytes across sequence wrap.
        std::vector<uint8_t> nackWire;
        RtpReceiver nackReceiver(true, [](auto){}, []{});
        nackReceiver.setAnchor(anchor);
        auto receiveForNack = [&](rtc::message_ptr message) {
            rtc::message_vector messages{message};
            nackReceiver.incoming(messages,[&](auto feedback){
                if (feedback->type != rtc::Message::Control) return;
                const auto* raw = reinterpret_cast<const uint8_t*>(feedback->data());
                nackWire.assign(raw, raw + feedback->size());
            });
            nackReceiver.pump(nowUs());
        };
        receiveForNack(packet(65530,90000,true,{0x65,1}));
        receiveForNack(packet(12,93000,true,{0x65,2}));
        expect(nackWire.size()==16, "NACK did not pack 17 missing packets into one FCI");
        expect((nackWire[0]&31)==1 && nackWire[1]==205, "Invalid RTCP Generic NACK header");
        expect(nackWire[12]==0xff && nackWire[13]==0xfb && nackWire[14]==0xff && nackWire[15]==0xff,
            "NACK PID/BLP wire format failed across sequence wrap");
        expect(nackReceiver.stats->nacks==17, "NACK packet request count changed");
        // A 12 Mbps sender must not be capped at 4800 bytes per 5ms pump.
        std::atomic<unsigned> pacingRate{12000000}; std::atomic<bool> keyframe{false};
        BoundedPacer pacer(pacingRate,keyframe); size_t transmitted=0;
        rtc::message_vector burst;
        for (int i=0;i<100;++i) burst.push_back(rtc::make_message(1000,rtc::Message::Binary));
        pacer.outgoing(burst,[&](auto packet){transmitted+=packet->size();});
        pacer.pump(1000000); transmitted=0;
        pacer.pump(1005000);
        expect(transmitted>=8000 && transmitted<=10000,"Pacer cannot sustain high bitrate at 5ms cadence");
        transmitted=0; pacer.pump(2000000);
        expect(transmitted<=18000,"Pacer emitted unbounded catch-up burst");
        pacer.clear(); expect(pacer.queueUs()==0,"Pacer clear retained old media");
        PacketGuard guard; rtc::message_vector bad{rtc::make_message(4,rtc::Message::Control)};
        guard.incoming(bad,[](auto){}); expect(bad.empty(),"Truncated RTCP passed guard");
        std::cout << "RTP reorder, loss, wrap and hostile-payload checks passed\n";
    } catch(const std::exception& e) { std::cerr << e.what() << '\n';return 1; }
}
