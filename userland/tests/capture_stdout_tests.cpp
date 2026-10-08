#include "asicen/stream_capture.h"

#include <iostream>
#include <sstream>

int main() {
    std::ostringstream normal;
    std::ostringstream diagnostic;

    asicen::write_command_summary(normal, diagnostic, true, "PX-W3U3", "1-2.1", 1);
    if (!normal.str().empty() ||
        diagnostic.str() != "model=\"PX-W3U3\" port=1-2.1 local=1\n") {
        std::cerr << "FAIL: capture command summary must stay off stdout\n";
        return 1;
    }

    normal.str("");
    normal.clear();
    diagnostic.str("");
    diagnostic.clear();
    asicen::write_command_summary(normal, diagnostic, false, "PX-W3U3", "1-2.1", 1);
    if (normal.str() != "model=\"PX-W3U3\" port=1-2.1 local=1\n" ||
        !diagnostic.str().empty()) {
        std::cerr << "FAIL: non-capture command summary remains normal text output\n";
        return 1;
    }

    return 0;
}
