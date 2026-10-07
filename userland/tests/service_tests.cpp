#include "asicen/enclosure.h"
#include "asicen/receiver_service.h"
#include "asicen/stream_queue.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {
int failures=0;
void check(bool value,const char* name){ if(!value){ std::cerr<<"FAIL: "<<name<<'\n'; ++failures; } }
}

int main(){
    asicen::ReceiverAddress addr{};
    check(asicen::w3u3_receiver_address(0,&addr),"receiver 0 map");
    check(addr.function_index==0 && addr.local_lane==0 &&
          addr.system==asicen::BroadcastSystem::IsdbS,"receiver 0 fields");
    check(asicen::w3u3_receiver_address(1,&addr) &&
          addr.function_index==0 && addr.local_lane==1 &&
          addr.system==asicen::BroadcastSystem::IsdbT,"receiver 1 fields");
    check(asicen::w3u3_receiver_address(2,&addr) &&
          addr.function_index==1 && addr.local_lane==0,"receiver 2 fields");
    check(asicen::w3u3_receiver_address(3,&addr) &&
          addr.function_index==1 && addr.local_lane==1,"receiver 3 fields");
    check(!asicen::w3u3_receiver_address(4,&addr),"reject receiver 4");

    asicen::ReceiverLeaseTable leases(4);
    const auto lease0=leases.acquire(0);
    check(lease0.has_value() && *lease0!=0,"acquire receiver");
    check(!leases.acquire(0).has_value(),"busy receiver");
    check(leases.status(0).state==asicen::ReceiverState::Leased,"leased status");
    check(leases.set_streaming(0,*lease0,true),"set streaming");
    check(leases.status(0).state==asicen::ReceiverState::Streaming,"streaming status");
    check(!leases.release(0,*lease0+1),"reject wrong lease");
    check(leases.set_streaming(0,*lease0,false),"stop streaming");
    check(leases.release(0,*lease0),"release receiver");
    check(leases.status(0).state==asicen::ReceiverState::Free,"free status");

    asicen::StreamQueue queue(8);
    const std::array<std::uint8_t,5> a{1,2,3,4,5};
    const std::array<std::uint8_t,4> b{6,7,8,9};
    check(queue.push(a.data(),a.size()),"queue first");
    check(!queue.push(b.data(),b.size()),"queue overflow");
    check(queue.dropped_chunks()==1 && queue.dropped_bytes()==4,"queue drop stats");
    auto first=queue.pop(3);
    check(first.size()==3 && first[0]==1 && first[2]==3,"partial pop");
    check(queue.queued_bytes()==2,"queue partial remaining");
    check(queue.push(b.data(),b.size()),"queue after pop");
    auto rest=queue.pop(16);
    check(rest.size()==6 && rest[0]==4 && rest[1]==5 && rest[2]==6 && rest[5]==9,
          "queue order");
    queue.close();
    check(queue.closed(),"queue closed");
    check(!queue.push(a.data(),a.size()),"reject push closed");

    return failures==0?0:1;
}
