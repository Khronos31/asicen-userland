// SPDX-License-Identifier: GPL-2.0-or-later
#include "../tools/asicend_identity_lease.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <windows/windows_security.h>

namespace {

namespace windows_security = px4::userland::windows_security;

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

bool to_utf8(const std::wstring& input, std::string& output)
{
    if (input.size() > static_cast<std::size_t>(INT_MAX)) return false;
    const int count = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        output.data(), count, nullptr, nullptr) == count;
}

class TemporaryRuntime final {
public:
    TemporaryRuntime()
    {
        std::array<wchar_t, MAX_PATH> temp{};
        std::array<wchar_t, MAX_PATH> candidate{};
        if (::GetTempPathW(static_cast<DWORD>(temp.size()), temp.data()) == 0U ||
            ::GetTempFileNameW(temp.data(), L"asl", 0U, candidate.data()) == 0U ||
            ::DeleteFileW(candidate.data()) == 0) return;
        if (!create_directory(candidate.data())) return;
        path_ = candidate.data();
    }

    ~TemporaryRuntime() noexcept
    {
        for (const std::wstring& child : children_) (void)::RemoveDirectoryW(child.c_str());
        if (!path_.empty()) (void)::RemoveDirectoryW(path_.c_str());
    }

    TemporaryRuntime(const TemporaryRuntime&) = delete;
    TemporaryRuntime& operator=(const TemporaryRuntime&) = delete;

    bool valid() const noexcept { return !path_.empty(); }

    bool child(const wchar_t* name, std::wstring& output)
    {
        output = path_ + L"\\" + name;
        if (!create_directory(output.c_str())) return false;
        children_.push_back(output);
        return true;
    }

private:
    static bool create_directory(const wchar_t* path)
    {
        SECURITY_ATTRIBUTES attributes{};
        SECURITY_DESCRIPTOR descriptor{};
        PACL acl = nullptr;
        windows_security::SidBuffer sid_storage{};
        if (!windows_security::build_current_user_security(
                attributes, descriptor, acl, sid_storage)) return false;
        const BOOL created = ::CreateDirectoryW(path, &attributes);
        (void)::LocalFree(acl);
        if (created == 0) return false;
        if (windows_security::verify_runtime_parent(path)) return true;
        (void)::RemoveDirectoryW(path);
        return false;
    }

    std::wstring path_;
    std::vector<std::wstring> children_;
};

bool lease_contract_for_path(std::string_view label, const std::string& path)
{
    auto first = asicen::cli::acquire_identity_lease(path, false, "usb-1-2");
    CHECK(first);
    const auto busy = asicen::cli::acquire_identity_lease(path, false, "usb-1-2");
    CHECK(!busy && busy.error() == dtv::userland::Error::BUSY);
    auto other = asicen::cli::acquire_identity_lease(path, false, "usb-1-3");
    CHECK(other);
    first.value().close();
    auto reacquired = asicen::cli::acquire_identity_lease(path, false, "usb-1-2");
    CHECK(reacquired);
    other.value().close();
    reacquired.value().close();
    std::printf("A1 PASS %.*s: acquire/BUSY/distinct/reacquire\n",
                static_cast<int>(label.size()), label.data());
    return true;
}

bool windows_valid_runtime_paths_support_identity_leases()
{
    TemporaryRuntime fixture;
    CHECK(fixture.valid());
    std::wstring wide_path;
    std::string utf8_path;

    CHECK(fixture.child(L"drive", wide_path));
    CHECK(to_utf8(wide_path, utf8_path));
    CHECK(lease_contract_for_path("drive-absolute", utf8_path));

    CHECK(fixture.child(L"extended", wide_path));
    std::wstring extended = L"\\\\?\\" + wide_path;
    CHECK(to_utf8(extended, utf8_path));
    CHECK(lease_contract_for_path("extended-drive-absolute", utf8_path));

    CHECK(fixture.child(L"日本語", wide_path));
    CHECK(to_utf8(wide_path, utf8_path));
    CHECK(lease_contract_for_path("non-ASCII", utf8_path));

    CHECK(fixture.child(L"localappdata-日本語", wide_path));
    ScopedLocalAppData restore;
    CHECK(::SetEnvironmentVariableW(L"LOCALAPPDATA", wide_path.c_str()) != FALSE);
    CHECK(lease_contract_for_path("LOCALAPPDATA-default", ""));
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
    if (!local_app_data_default_is_utf8() ||
        !windows_valid_runtime_paths_support_identity_leases() ||
        !windows_runtime_contract_is_preserved())
        return 1;
    std::puts("ASICEN Windows identity lease fixtures passed");
    return 0;
}
