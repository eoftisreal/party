#include "party/core.hpp"
#include "party/rtp.hpp"
#include "party/transport.hpp"
#include <iostream>
#include <stdexcept>
namespace { void expect(bool v, const char* what) { if (!v) throw std::runtime_error(what); } }
int main() {
    using namespace party;
    try {
        const auto ice = iceConfigFromServers(Json::array({
            {{"url", "stun:relay.example:3478"}},
            {{"url", "turn:relay.example:3478?transport=udp"}, {"username", "temporary"}, {"credential", "temporary-secret"}}
        }));
        expect(ice.iceServers.size() == 2 && ice.disableAutoNegotiation, "backend ICE configuration lost");
        expect(ice.iceServers[1].username == "temporary" && ice.iceServers[1].password == "temporary-secret", "TURN credentials lost");
        for (const auto& servers : {Json::object(), Json::array({{{"url", "turn:relay.example:3478"}, {"username", ""}, {"credential", ""}}})}) {
            bool rejected = false;
            try { iceConfigFromServers(servers); } catch (const std::exception&) { rejected = true; }
            expect(rejected, "invalid backend ICE configuration accepted");
        }
        ClockEstimator clock;
        expect(clock.sample(1000000,1051000,1052000,1004000), "clock sample");
        expect(clock.offsetUs() == 49500 && clock.rttUs() == 3000, "NTP estimator arithmetic");
        expect(!clock.sample(9,10,11,8), "invalid clock order");
        expect(clock.sample(1000000,1060000,1061000,1020000) && clock.offsetUs() == 49500, "reject worse queueing bias");
        expect(clock.sample(32000000,32101000,32102000,32020000), "clock refresh after network path changes");
        expect(clock.offsetUs()==54500, "clock correction must be bounded and escape stale best RTT");
        clock.reset();expect(!clock.ready(), "old connection clock survived reset");
        expect(clock.sample(1000000,1051000,1052000,1004000) && clock.offsetUs()==49500,"new connection clock did not initialize");
        const int64_t anchor = 60000000000;
        for (const auto rate : {48000u, 90000u}) {
            const auto ticks = uint32_t((anchor/1000000)*rate);
            expect(std::abs(unwrapTimestamp(ticks+rate, rate, anchor)-(anchor+1000000)) <= 1, "RTP wrap forward");
            expect(std::abs(unwrapTimestamp(ticks-rate, rate, anchor)-(anchor-1000000)) <= 1, "RTP wrap backward");
        }
        BitrateController rate;
        expect(rate.update(.1,0,0) < 2500000, "loss reduces bitrate");
        for (int i=0; i<40; ++i) rate.update(1,0,0);
        expect(rate.bitrate() == 300000, "bitrate floor");
        for (int i=0; i<200; ++i) rate.update(0,0,0);
        expect(rate.bitrate() == 5000000, "bitrate ceiling");
        BitrateController high(6000000, 12000000);
        for (int i=0; i<60; ++i) high.update(0,0,0);
        expect(high.bitrate() == 12000000, "high quality ceiling");
        expect(high.update(.1,0,0) == 9000000, "high quality still backs off on loss");
        bool invalidRate = false;
        try { BitrateController invalid(6000000, 5000000); } catch (const std::invalid_argument&) { invalidRate = true; }
        expect(invalidRate, "invalid bitrate configuration accepted");
        PlayoutBudget budget;
        for (int64_t i=0; i<100; ++i) budget.observe(i*20000, i*20000);
        expect(budget.targetUs() == 350000, "steady arrivals unnecessarily inflate latency");
        int64_t arrival=2000000, pts=2000000;
        for (int i=0; i<100; ++i) { arrival+=200000; pts+=20000; budget.observe(arrival,pts); }
        expect(budget.targetUs() == 600000, "jitter budget not bounded/adaptive");
        budget.reset(); expect(budget.targetUs() == 350000, "playout reset retained old jitter");
        PlayoutBudget bursts;
        int64_t burstArrival = 0, burstPts = 0;
        for (int i = 0; i < 160; ++i) {
            burstPts += 20000;
            burstArrival += (i % 8 == 0) ? 140000 : 20000;
            bursts.observe(burstArrival, burstPts);
        }
        expect(bursts.targetUs() >= 500000, "recurring arrival bursts should grow playout budget");
        bursts.reset();
        for (int64_t i = 0; i < 160; ++i) bursts.observe(i * 20000, i * 20000);
        expect(bursts.targetUs() == 350000, "burst histogram must reset on a new epoch");
        std::vector<uint8_t> packet{0x80,0xe0,0xff,0xfe,0,0,0,7,0,0,0,1,0x65,0x11};
        auto parsed = parseRtp(packet); expect(parsed && parsed->sequence == 65534 && parsed->marker && parsed->payload.size()==2, "RTP header parsing");
        packet[0] = 0x90; expect(!parseRtp(packet), "truncated extension rejected");
        packet[0] = 0xa0; packet.back() = 250; expect(!parseRtp(packet), "invalid padding rejected");
        packet.resize(3); expect(!parseRtp(packet), "truncated header rejected");
        expect(randomId().size()==32 && randomId()!=randomId(), "secure IDs");
        std::cout << "core unit checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
