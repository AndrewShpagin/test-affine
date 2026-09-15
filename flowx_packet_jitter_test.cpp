#include "flowx_packet_jitter.h"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace {
using Jitter = flowx::PacketJitter;
using Bytes = std::vector<unsigned char>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
auto timeAt(int ms) { return Jitter::Time{}+std::chrono::milliseconds(ms); }
std::vector<std::pair<int,int>> trial(std::uint32_t seed) {
    Jitter jitter; jitter.reset(10,90,seed);
    std::vector<Bytes> packets;
    for (unsigned i=0;i<200;++i) {
        packets.push_back(Bytes{static_cast<unsigned char>(i),0,255,42});
        require(jitter.enqueue(packets.back(),timeAt(0)),"enqueue failed");
    }
    Bytes out;
    std::vector<std::pair<int,int>> delivered;
    for (int ms=0;ms<=90;++ms) while (jitter.popReady(timeAt(ms),out)) {
        require(ms>=10 && ms<=90,"delay outside range");
        require(out==packets.at(out[0]),"packet bytes changed");
        delivered.emplace_back(ms,out[0]);
    }
    require(delivered.size()==packets.size(),"packets lost or deadline not drained");
    std::vector<int> order;
    for (auto [ms,id]:delivered) order.push_back(id);
    require(!std::is_sorted(order.begin(),order.end()),"random delays did not reorder the burst");
    std::sort(order.begin(),order.end());
    for (unsigned i=0;i<order.size();++i) require(order[i]==int(i),"duplicate or missing packet");
    require(jitter.stats().scheduled==200 && jitter.stats().delivered==200 && !jitter.stats().queued &&
            !jitter.stats().overflow && !jitter.nextDue(),"bad drained stats");
    return delivered;
}
}

int main() {
    try {
        require(trial(1)==trial(1),"same seed is not reproducible");
        require(trial(1)!=trial(2),"different seed made no difference");
        Jitter jitter; Bytes out;
        jitter.reset(50,50,7);
        for (unsigned i=0;i<10;++i) jitter.enqueue(Bytes{static_cast<unsigned char>(i)},timeAt(100));
        require(jitter.nextDue()==timeAt(150) && !jitter.popReady(timeAt(149),out),"constant delay too short");
        for (unsigned i=0;i<10;++i)
            require(jitter.popReady(timeAt(150),out) && out==Bytes{static_cast<unsigned char>(i)},"equal deadlines lost FIFO order");
        jitter.reset(1000,1000,0);
        for (unsigned i=0;i<Jitter::kMaxPackets;++i) require(jitter.enqueue(Bytes{1},timeAt(0)),"early count overflow");
        require(!jitter.enqueue(Bytes{2},timeAt(0)) && jitter.stats().overflow==1,"packet bound not enforced");
        jitter.reset(1000,1000,0);
        require(jitter.enqueue(Bytes(Jitter::kMaxBytes,1),timeAt(0)),"byte limit rejected exact bound");
        require(!jitter.enqueue(Bytes{2},timeAt(0)) && jitter.stats().queued==1,"byte bound not enforced");
        jitter.reset(0,0,1);
        require(!jitter.enabled() && !jitter.nextDue() && !jitter.stats().overflow,"reset retained state");
        jitter.enqueue(Bytes{3},timeAt(123));
        require(jitter.popReady(timeAt(123),out) && out==Bytes{3},"zero delay not immediate");
        bool rejected=false;
        try { jitter.reset(20,10,1); } catch (const std::invalid_argument&) { rejected=true; }
        require(rejected,"invalid jitter range accepted");
        std::cout << "PASS: UDP delay bounds, seeded reordering, byte preservation, fixed/zero latency, bounded queue, reset\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
