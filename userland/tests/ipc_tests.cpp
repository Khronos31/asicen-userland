#include "asicen/ipc.h"

#include <iostream>

namespace {
int failures=0;
void check(bool value,const char* name){ if(!value){ std::cerr<<"FAIL: "<<name<<'\n'; ++failures; } }
}

int main(){
    asicen::IpcRequest req{asicen::IpcCommand::Stream,3,1234};
    const auto bytes=asicen::encode_request(req);
    asicen::IpcRequest parsed{};
    check(asicen::decode_request(bytes.data(),bytes.size(),&parsed),"decode request");
    check(parsed.command==asicen::IpcCommand::Stream && parsed.receiver==3 &&
          parsed.packet_count==1234,"request round trip");

    auto bad=bytes;
    bad[0]=0;
    check(!asicen::decode_request(bad.data(),bad.size(),&parsed),"reject bad magic");

    asicen::IpcResponse rsp{asicen::IpcStatus::Ok,0x1122334455667788ULL,4};
    const auto rb=asicen::encode_response(rsp);
    asicen::IpcResponse rp{};
    check(asicen::decode_response(rb.data(),rb.size(),&rp),"decode response");
    check(rp.status==asicen::IpcStatus::Ok &&
          rp.lease_id==0x1122334455667788ULL && rp.receiver_count==4,
          "response round trip");
    return failures==0?0:1;
}
