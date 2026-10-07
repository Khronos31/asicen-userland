#include "asicen/enclosure_grouping.h"

#include <iostream>
#include <vector>

namespace {
int failures=0;
void check(bool value,const char* name){ if(!value){ std::cerr<<"FAIL: "<<name<<'\n'; ++failures; } }
}

int main(){
    using asicen::UsbFunctionObservation;
    const UsbFunctionObservation a{0x0b06,0x0005,1,{2,4,1}};
    const UsbFunctionObservation b{0x0b06,0x0005,1,{2,4,2}};
    const UsbFunctionObservation c{0x0b06,0x0005,1,{2,5,1}};
    const UsbFunctionObservation d{0x0b06,0x0005,2,{2,4,2}};

    check(asicen::same_usb_parent(a,b),"same parent");
    check(!asicen::same_usb_parent(a,c),"different parent");
    check(!asicen::same_usb_parent(a,d),"different bus");
    check(!asicen::same_usb_parent(
        UsbFunctionObservation{0x0b06,0x0005,1,{2}},
        UsbFunctionObservation{0x0b06,0x0005,1,{3}}),"root children not grouped");

    const std::vector<UsbFunctionObservation> obs{a,b,c,d};
    const auto groups=asicen::group_dual_function_enclosures(obs,0x0b06,0x0005);
    check(groups.size()==1,"one enclosure group");
    if(groups.size()==1) check(groups[0].first==0 && groups[0].second==1,"group indexes");

    const std::vector<UsbFunctionObservation> ambiguous{
      {0x0b06,0x0005,1,{7,1,1}},
      {0x0b06,0x0005,1,{7,1,2}},
      {0x0b06,0x0005,1,{7,1,3}},
    };
    check(asicen::group_dual_function_enclosures(ambiguous,0x0b06,0x0005).empty(),
          "ambiguous siblings rejected");

    return failures==0?0:1;
}
