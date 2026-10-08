// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>

int run_asicen_ts_research(int argc, char** argv);
int run_asicen_ts_portable(int argc, char** argv);

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0) {
            // Preserve the original explicitly addressed research protocol for
            // its existing offline workflow; product mode is the default.
            return run_asicen_ts_research(argc, argv);
        }
    }
    return run_asicen_ts_portable(argc, argv);
}
