// SPDX-License-Identifier: GPL-2.0-or-later
#include "../tools/asicend_identity_lease.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class ScopedLocalAppData final {
public:
    ScopedLocalAppData() noexcept
    {
        const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0U);
        if (required == 0U) return;
        previous_.resize(required);
        const DWORD length = ::GetEnvironmentVariableW(
            L"LOCALAPPDATA", previous_.data(), required);
        if (length == 0U || length >= required) {
            previous_.clear();
            return;
        }
        previous_.resize(length);
        had_previous_ = true;
    }

    ~ScopedLocalAppData() noexcept
    {
        if (had_previous_) {
            (void)::SetEnvironmentVariableW(L"LOCALAPPDATA", previous_.c_str());
        } else {
            (void)::SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
        }
    }

private:
    std::wstring previous_;
    bool had_previous_ = false;
};

bool local_app_data_default_is_utf8()
{
    ScopedLocalAppData restore;
    const wchar_t* path = L"C:\\Users\\\x65e5\x672c\\AppData\\Local";
    CHECK(::SetEnvironmentVariableW(L"LOCALAPPDATA", path) != FALSE);
    std::string converted;
    CHECK(asicen::cli::local_app_data_utf8(converted));
    CHECK(converted == "C:\\Users\\\xE6\x97\xA5\xE6\x9C\xAC\\AppData\\Local");
    return true;
}

bool windows_runtime_contract_is_preserved()
{
    const std::array<std::string_view, 9U> invalid_paths{
        "runtime", ".\\runtime", "C:runtime", "C:", "\\runtime",
        "\\\\server\\share", "\\\\?\\UNC\\server\\share",
        "\\\\?\\C:", "\\\\?\\C:runtime"};
    for (const std::string_view path : invalid_paths) {
        dtv::userland::host_listener_context::IdentityLeaseDiagnostic diagnostic{};
        const auto lease = asicen::cli::acquire_identity_lease(
            path, false, "usb-1-2", &diagnostic);
        CHECK(!lease && lease.error() == dtv::userland::Error::INVALID_ARGUMENT);
        CHECK(diagnostic.reason ==
              dtv::userland::host_listener_context::IdentityLeaseDiagnosticReason::runtime_directory);
    }
    const auto group = asicen::cli::acquire_identity_lease(
        "C:\\Users\\test\\AppData\\Local", true, "usb-1-2");
    CHECK(!group && group.error() == dtv::userland::Error::UNSUPPORTED);
    return true;
}

}  // namespace

int main()
{
    if (!local_app_data_default_is_utf8() || !windows_runtime_contract_is_preserved())
        return 1;
    std::puts("ASICEN Windows identity lease fixtures passed");
    return 0;
}
