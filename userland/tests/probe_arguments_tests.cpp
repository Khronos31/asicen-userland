// SPDX-License-Identifier: GPL-2.0-only
#define main asicen_probe_entry_for_test
#include "../tools/asicen_probe.cpp"
#undef main

#include <cstdio>

namespace {
bool parse(std::initializer_list<const char*> words, Arguments* result = nullptr)
{
    std::vector<std::string> owned;
    for (const char* word : words)
        owned.emplace_back(word);
    std::vector<char*> pointers;
    for (auto& word : owned)
        pointers.push_back(word.data());
    Arguments local;
    return parse_arguments(static_cast<int>(pointers.size()), pointers.data(),
                           result != nullptr ? result : &local);
}
} // namespace

int main()
{
    unsigned failures = 0U;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            ++failures;
        }
    };
    check(parse({"probe"}), "default read-only list");
    check(parse({"probe", "--help"}), "help alone");
    check(!parse({"probe", "--help", "list"}), "help cannot bypass remaining args");
    check(!parse({"probe", "-h"}), "only documented help");
    check(!parse({"probe", "list", "list"}), "duplicate command rejected");
    check(!parse({"probe", "list", "describe"}), "multiple commands rejected");
    check(!parse({"probe", "--device", "1:2", "--device", "1:2", "describe"}),
          "duplicate selector rejected");
    check(!parse({"probe", "--device", "0x1:2", "describe"}), "hex selector rejected");
    check(!parse({"probe", "--device", " 1:2", "describe"}), "whitespace selector rejected");
    check(!parse({"probe", "--model", "w3u3", "list"}), "ignored model rejected");
    check(!parse({"probe", "--firmware", "path", "list"}), "ignored firmware rejected");
    check(
        !parse({"probe", "--device", "1:2", "--model", "w3u3", "--firmware", "", "load-firmware"}),
        "empty firmware rejected");
    check(!parse({"probe", "--claim"}), "claim requires explicit selector");
    check(!parse({"probe", "--device", "1:2", "--claim", "--claim"}), "duplicate claim rejected");
    Arguments claim;
    check(parse({"probe", "--device", "001:029", "--claim"}, &claim) && claim.command == "claim" &&
              claim.location.address == 29U,
          "explicit claim-only command");
    check(parse({"probe", "--device", "1:2", "--model", "w3u3", "--firmware", "file",
                 "load-firmware"}),
          "explicit cold upload syntax");
    check(parse({"probe", "--device", "1:2", "--model", "w3u3", "--firmware", "file",
                 "--initialize", "--require-cold"}),
          "explicit initialize/cold syntax");
    check(!parse({"probe", "--require-cold", "list"}), "cold requirement needs initialize");
    return failures == 0U ? 0 : 1;
}
