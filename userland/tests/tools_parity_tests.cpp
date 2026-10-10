// SPDX-License-Identifier: GPL-2.0-only
#include "asicend_args.h"
#include "asicend_list_format.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

asicen::cli::DaemonArguments parse(std::initializer_list<const char*> options, bool mock = false)
{
    std::vector<const char*> arguments{"asicend"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return asicen::cli::parse_daemon_arguments(
        static_cast<int>(arguments.size()), arguments.data(), mock);
}

bool arguments_match_reference()
{
    for (const char* option : {"--help", "--list", "--list-json", "--models"}) {
        CHECK(parse({option}).valid);
        CHECK(!parse({option, option}).valid);
        CHECK(!parse({option, "--group"}).valid);
    }
    CHECK(!parse({"-h"}).valid);
    CHECK(!parse({"--socket", "/unused"}, true).valid &&
          !parse({"--socket", "/unused"}).valid);
    CHECK(!parse({"--usb-path", "1:2"}).valid);
    CHECK(parse({"--usb-path", "1:2", "--instance", "test"}).valid);
    CHECK(parse({"--fd", "3", "--instance", "test"}).valid);
    CHECK(parse({"--fd", "3"}).valid && parse({"--fd", "3"}).instance.empty());
    CHECK(parse({}, true).valid && parse({}, true).instance == "default");
    for (const char* selector : {"0:1", "1:0", "256:1", "1:256", "0x1:2", "1:2:3",
                                 "0-1", "1-0", "1-256", "1-1.", "1-1..2",
                                 "1-1.2.3.4.5.6.7.8.9"}) {
        CHECK(!parse({"--usb-path", selector, "--instance", "test"}).valid);
    }
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    CHECK(asicen::cli::parse_bus_address("010:008", &bus, &address) &&
          bus == 10U && address == 8U);
    std::vector<std::uint8_t> ports;
    CHECK(asicen::cli::parse_bus_port("010-008.001", &bus, &ports) &&
          bus == 10U && ports == std::vector<std::uint8_t>({8U, 1U}));
    CHECK(!parse({"--usb-path", "1:2", "--fd", "3", "--instance", "test"}).valid);
    CHECK(!parse({"--usb-path", "1:2", "--instance", "test", "--runtime-dir", ""}).valid);
    CHECK(!parse({"--usb-path", "1:2", "--instance", "test", "--firmware", "--group"}).valid);
    CHECK(!parse({"--instance", "test", "--model", "s3u", "--model", "s3u"}, true).valid);
    CHECK(!parse({"--instance", "test", "--group", "--group"}, true).valid);
    CHECK(!parse({"--instance", "test", "--runtime-dir", "x", "--runtime-dir", "x"}, true).valid);
    CHECK(!parse({"--instance", "12345678901234"}, true).valid);
    return true;
}

bool list_formats_share_observed_identity()
{
    using asicen::UsbFunctionObservation;
    const std::vector<UsbFunctionObservation> pair{
        {0x0b06, 0x0005, 1, {2, 2}, 18},
        {0x0b06, 0x0005, 1, {2, 1}, 17}};
    const std::string json = asicen::cli::format_device_list(pair, true);
    CHECK(json.find("\"serial\":null") != std::string::npos &&
          json.find("\"serial_unique\":false") != std::string::npos);
    CHECK(json.find("\"identity_kind\":\"usb-topology\",\"identity\":\"usb-1-2\"") != std::string::npos);
    CHECK(json.find("\"device\":1,\"serial\":null,\"bus\":1,\"address\":17,\"port\":\"1-2.1\"") != std::string::npos);
    CHECK(json.find("\"receiver\":0,\"device\":1,\"local\":0,\"system\":\"ISDB-S\",\"lnb_15v_supported\":true") != std::string::npos);
    CHECK(json.find("\"receiver\":1,\"device\":1,\"local\":1,\"system\":\"ISDB-T\",\"lnb_15v_supported\":false") != std::string::npos);
    const std::string text = asicen::cli::format_device_list(pair, false);
    CHECK(text.find("status=ready receivers=4 serial_unique=false") != std::string::npos &&
          text.find("dev1_bus=1 dev1_address=17 dev1_port=1-2.1") != std::string::npos);
    const auto* model = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(model != nullptr && asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {2U, 1U}));
    CHECK(!asicen::cli::runtime_matches_loader(pair[0], *model, 1U, {2U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 2U, {2U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {3U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {}));
    auto other_model = pair[1];
    other_model.pid = 0x0006U;
    CHECK(!asicen::cli::runtime_matches_loader(other_model, *model, 1U, {2U, 1U}));
    CHECK(asicen::cli::format_device_list({pair[1]}, true).find("\"status\":\"incomplete\"") != std::string::npos);
    auto ambiguous = pair;
    ambiguous.push_back({0x0b06, 0x0005, 1, {2, 3}, 19});
    const std::string duplicate = asicen::cli::format_device_list(ambiguous, true);
    CHECK(duplicate.find("\"status\":\"duplicate\"") != std::string::npos &&
          duplicate.find("\"devices\":[],\"candidates\":[{") != std::string::npos);
    for (const std::uint16_t pid : {0x0001U, 0x0003U}) {
        const std::string single = asicen::cli::format_device_list({{0x0b06, pid, 2, {4}, 9}}, true);
        CHECK(single.find("\"status\":\"ready\"") != std::string::npos &&
              single.find("\"identity\":\"usb-2-4\"") != std::string::npos &&
              single.find("\"receivers\":[{") != std::string::npos &&
              single.find("\"ungrouped_usb_devices\":[]") != std::string::npos);
    }
    CHECK(asicen::cli::format_device_list({}, true) ==
          "{\"enclosures\":[],\"ungrouped_usb_devices\":[]}\n");
    CHECK(asicen::cli::format_device_list({}, false).empty());
    return true;
}

}  // namespace

int main()
{
    if (!arguments_match_reference() || !list_formats_share_observed_identity()) {
        return 1;
    }
    std::puts("ASICEN tool parity tests passed (no USB access)");
    return 0;
}
