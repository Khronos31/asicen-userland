#include "asicen/ts_framer.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
int failures=0;
void check(bool value,const char* name){ if(!value){ std::cerr<<"FAIL: "<<name<<'\n'; ++failures; } }

std::vector<std::uint8_t> packets(std::size_t count, std::uint8_t sync) {
    std::vector<std::uint8_t> data(count * asicen::kMpegTsPacketSize, 0xff);
    for(std::size_t i=0;i<count;++i) {
        data[i*asicen::kMpegTsPacketSize]=sync;
        data[i*asicen::kMpegTsPacketSize+1]=0x1f;
        data[i*asicen::kMpegTsPacketSize+2]=0xff;
        data[i*asicen::kMpegTsPacketSize+3]=static_cast<std::uint8_t>(0x10|(i&0x0f));
    }
    return data;
}
}

int main(){
    check(asicen::ts_sync_candidate(0x47,asicen::TransportMode::AsicenMode3),
          "mode3 plain sync");
    check(asicen::ts_sync_candidate(0xc7,asicen::TransportMode::AsicenMode3),
          "mode3 marked sync");
    check(!asicen::ts_sync_candidate(0xc7,asicen::TransportMode::Plain),
          "plain rejects marked sync");

    auto marked=packets(10,0xc7);
    std::size_t off=999;
    check(asicen::find_ts_alignment(marked.data(),marked.size(),
                                    asicen::TransportMode::AsicenMode3,&off) &&
          off==0,"find marked alignment");

    std::vector<std::uint8_t> garbage(13,0x55);
    garbage.insert(garbage.end(),marked.begin(),marked.end());
    check(asicen::find_ts_alignment(garbage.data(),garbage.size(),
                                    asicen::TransportMode::AsicenMode3,&off) &&
          off==13,"find alignment after garbage");

    asicen::TsFramer framer(asicen::TransportMode::AsicenMode3);
    auto first=framer.push(garbage.data(),500);
    check(first.empty(),"fragment insufficient for sync proof");
    auto second=framer.push(garbage.data()+500,garbage.size()-500);
    check(second.size()==10*asicen::kMpegTsPacketSize,"framer emits packets");
    check(second[0]==0x47 && second[188]==0x47,"framer normalizes sync bit");
    check(framer.discarded_bytes()==13,"framer tracks garbage");

    auto plain=packets(8,0x47);
    asicen::TsFramer plain_framer(asicen::TransportMode::Plain);
    auto p=plain_framer.push(plain.data(),plain.size());
    check(p.size()==plain.size() && p[0]==0x47,"plain framing");

    asicen::TsFramer bounded(asicen::TransportMode::Plain,188*8);
    std::vector<std::uint8_t> junk(188*20,0);
    check(bounded.push(junk.data(),junk.size()).empty(),"bounded junk no output");
    check(bounded.pending_bytes()<=188*8,"bounded pending");
    check(bounded.discarded_bytes()>0,"bounded discard count");

    return failures==0?0:1;
}
