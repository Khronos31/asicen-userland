// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/diagnostic_probe.h"

int main(int argc, char** argv)
{
    return asicen::run_diagnostic_probe(asicen::DiagnosticProbeRole::frontend, argc,
                                        const_cast<const char* const*>(argv));
}
