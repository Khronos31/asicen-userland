#include "asicen/card_mailbox.h"

#include <iostream>

namespace {
int failures=0;
void check(bool value,const char* name){ if(!value){ std::cerr<<"FAIL: "<<name<<'\n'; ++failures; } }
}

int main(){
    check(asicen::build_card_mailbox_chunks(0).empty(),"zero length");

    const auto small=asicen::build_card_mailbox_chunks(5);
    check(small.size()==1,"small chunk count");
    if(small.size()==1){
      check(small[0].page==0 && small[0].register_address==0x40 &&
            small[0].buffer_offset==0 && small[0].length==5,"small chunk fields");
    }

    const auto exact=asicen::build_card_mailbox_chunks(64);
    check(exact.size()==1 && exact[0].length==64,"exact page");

    const auto split=asicen::build_card_mailbox_chunks(130);
    check(split.size()==3,"multi page count");
    if(split.size()==3){
      check(split[0].page==0 && split[0].register_address==0x40 &&
            split[0].length==64,"page0");
      check(split[1].page==1 && split[1].register_address==0x40 &&
            split[1].buffer_offset==64 && split[1].length==64,"page1");
      check(split[2].page==2 && split[2].register_address==0x40 &&
            split[2].buffer_offset==128 && split[2].length==2,"page2");
    }

    check(asicen::decode_card_mailbox_length(0x23,0)==0x23,"length low");
    check(asicen::decode_card_mailbox_length(0x23,1)==0x123,"length high bit");
    check(asicen::decode_card_mailbox_length(0x23,3)==0x123,"length masks high source");

    return failures==0?0:1;
}
