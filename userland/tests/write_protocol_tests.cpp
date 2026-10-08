#include "asicen/write_protocol.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {
int failures=0;
void check(bool value,const char* name){ if(!value){ std::cerr<<"FAIL: "<<name<<'\n'; ++failures; } }
}

int main(){
    const std::array<std::uint8_t,5> data{0x11,0x22,0x33,0x44,0x55};

    asicen::ControlTransfer t{};
    check(asicen::make_i2c_write_chunk(0x60,0x20,data.data(),2,false,&t),"i2c write build");
    check(t.request==asicen::Request::I2cWrite,"i2c write request");
    check(t.value==0x2060,"i2c write value");
    check(t.index==0x2211,"i2c write index");
    check(t.length==3 && t.direction==asicen::Direction::In,"i2c write response");

    check(asicen::make_i2c_write_chunk(0x60,0x21,data.data(),1,true,&t),"i2c nostop build");
    check(t.request==asicen::Request::I2cWriteNoStop,"i2c nostop request");
    check(t.index==0x0011 && t.length==2,"i2c nostop packing");
    check(!asicen::make_i2c_write_chunk(0x60,0,data.data(),3,false,&t),"reject >2 normal write");

    check(asicen::make_i2c_buffer_fill(3,data.data(),3,&t),"buffer fill build");
    check(t.request==asicen::Request::I2cBufferFill,"buffer fill request");
    check(t.value==0x1103 && t.index==0x3322 && t.length==4,"buffer fill packing");

    t=asicen::make_i2c_buffer_send(0xa8,7,true);
    check(t.request==asicen::Request::I2cBufferSend,"buffer send request");
    check(t.value==0x01a8 && t.length==8,"buffer send packing");

    check(asicen::make_sysctrl_write(0x42,data.data(),3,&t),"sysctrl write build");
    check(t.request==asicen::Request::SysCtrlWrite,"sysctrl request");
    check(t.value==0x1142 && t.index==0x3322 && t.length==4,"sysctrl packing");

    t=asicen::make_gpio_set(0x12,0xf0);
    check(t.request==asicen::Request::Gpio && t.value==0xf012,"gpio packing");
    t=asicen::make_gpio_ex_set(0x34,0x0f);
    check(t.request==asicen::Request::GpioExSet && t.value==0x0f34,"gpioex packing");
    t=asicen::make_gpio_ex_get();
    check(t.request==asicen::Request::GpioExGet && t.length==1,"gpioex get");
    check(asicen::make_dsc_control(1,false).request==asicen::Request::DscStart,"dsc start");
    check(asicen::make_dsc_control(1,true).request==asicen::Request::DscStop,"dsc stop");
    for (std::uint8_t local : std::array<std::uint8_t, 2>{0, 1}) {
      const auto start = asicen::make_dsc_control(local, false);
      check(static_cast<std::uint8_t>(start.request) == 0x06 &&
                asicen::bm_request_type(start.direction) == 0xc0 &&
                start.value == local && start.index == 0 && start.length == 1,
            "literal DSC start request06 mapping for each local");
      const auto stop = asicen::make_dsc_control(local, true);
      check(static_cast<std::uint8_t>(stop.request) == 0x07 &&
                asicen::bm_request_type(stop.direction) == 0xc0 &&
                stop.value == local && stop.index == 0 && stop.length == 1,
            "literal DSC stop request07 mapping for each local");
    }
    check(asicen::make_reset_channel(2,7).value==0x0702,"reset packing");

    const auto normal=asicen::build_i2c_write_sequence(0x60,0x20,data.data(),5,0);
    check(normal.size()==3,"normal write sequence size");
    if(normal.size()==3){
      check(normal[0].value==0x2060 && normal[0].index==0x2211,"normal seq first");
      check(normal[1].value==0x2260 && normal[1].index==0x4433,"normal seq second");
      check(normal[2].value==0x2460 && normal[2].index==0x0055,"normal seq third");
    }

    const auto odd=asicen::build_i2c_write_sequence(0x60,0x21,data.data(),3,4);
    check(odd.size()==2,"odd nostop sequence size");
    if(odd.size()==2){
      check(odd[0].request==asicen::Request::I2cWriteNoStop && odd[0].index==0x0011,"odd first");
      check(odd[1].value==0x2260 && odd[1].index==0x3322,"odd second");
    }

    std::array<std::uint8_t,35> large{};
    for(std::size_t i=0;i<large.size();++i) large[i]=static_cast<std::uint8_t>(i);
    const auto ext=asicen::build_i2c_write_sequence(0xa8,0,large.data(),large.size(),3);
    // 32-byte batch: 11 fill transfers + send. 3-byte batch: 1 fill + send.
    check(ext.size()==14,"extended write sequence size");
    if(ext.size()==14){
      check(ext[0].request==asicen::Request::I2cBufferFill && ext[0].value==0x0000,"extended fill zero");
      check(ext[10].request==asicen::Request::I2cBufferFill,"extended fill last");
      check(ext[11].request==asicen::Request::I2cBufferSend && ext[11].length==33,"extended first send");
      check(ext[12].request==asicen::Request::I2cBufferFill && ext[12].value==0x2000,"extended second batch resets offset");
      check(ext[13].request==asicen::Request::I2cBufferSend && ext[13].length==4,"extended second send");
    }

    const auto ext_nostop=asicen::build_i2c_write_sequence(0xa8,0,data.data(),5,2);
    check(ext_nostop.size()==3,"extended mode2 size");
    if(ext_nostop.size()==3) check((ext_nostop.back().value>>8)==0,"extended mode2 no stop");

    return failures==0?0:1;
}
