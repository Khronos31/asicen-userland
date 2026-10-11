// SPDX-License-Identifier: GPL-2.0-only
// ASICEN modification, 2026-10-10: serial-free identity tests and the native
// loader's malformed Unicode-path fixture when ASICEN_NATIVE_LOADER_TEST is set.
// Windows Phase 1 offline tests for the actual platform implementation:
// pointer-width handles, high-resolution pacing, BCrypt nonce, the AF_UNIX
// endpoint adapter, fail-closed group mode, stale endpoint cleanup, and the
// serial namespace lease. These run under Windows (native CI) and, where the
// kernel supports it, under Wine; AF_UNIX/DACL behavior that Wine lacks is
// reported separately rather than weakened here.
#if defined(ASICEN_NATIVE_LOADER_TEST)
#include "asicen/firmware.h"
#else
#include "px4/firmware.h"
#endif
#include "px4/platform_sleep.h"
#include "px4/posix_ipc.h"
#include "px4/windows_tuner_nonce.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

#include "windows/windows_security.h"

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

int failures = 0;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                               \
            ++failures;                                                             \
        }                                                                           \
    } while (false)

class TempRoot final {
public:
    explicit TempRoot(std::string_view name)
    {
        (void)name;
        std::array<wchar_t, MAX_PATH> base{};
        const DWORD base_length =
            ::GetEnvironmentVariableW(L"USERPROFILE", base.data(),
                                      static_cast<DWORD>(base.size()));
        if (base_length == 0U || base_length >= base.size()) {
            valid_ = false;
            return;
        }
        // Random short fresh directory; never blindly delete a preexisting
        // user folder. Keep the full endpoint path within the AF_UNIX limit.
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::array<std::uint8_t, 6U> random{};
            std::array<wchar_t, 20U> token{};
            if (!windows_security::random_hex(random.data(), random.size(),
                                              token.data(), token.size())) {
                valid_ = false;
                return;
            }
            root_ = std::filesystem::path(base.data()) /
                    (std::wstring(L"px4t-") + token.data());
            std::error_code error;
            if (std::filesystem::exists(root_, error)) {
                continue;
            }
            if (!std::filesystem::create_directories(root_, error) || error) {
                valid_ = false;
                return;
            }
            identity_valid_ = identity_of(root_.c_str(), identity_device_,
                                          identity_index_);
            valid_ = windows_security::set_current_user_owner_and_dacl(root_.c_str());
            return;
        }
        valid_ = false;
    }

    ~TempRoot()
    {
        if (!valid_) {
            return;
        }
        // Only remove the exact directory we created.
        std::uint64_t device = 0U;
        std::uint64_t index = 0U;
        if (identity_valid_ && identity_of(root_.c_str(), device, index) &&
            device == identity_device_ && index == identity_index_) {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }
    }

    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    bool valid() const noexcept { return valid_; }
    std::string utf8() const { return root_.u8string(); }
    std::wstring wide() const { return root_.wstring(); }

private:
    static bool identity_of(const wchar_t* path, std::uint64_t& device,
                            std::uint64_t& index) noexcept
    {
        const HANDLE handle = ::CreateFileW(
            path, FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        const BOOL ok = ::GetFileInformationByHandle(handle, &info);
        (void)::CloseHandle(handle);
        if (ok == 0) {
            return false;
        }
        device = info.dwVolumeSerialNumber;
        index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
                static_cast<std::uint64_t>(info.nFileIndexLow);
        return true;
    }

    std::filesystem::path root_;
    bool valid_ = false;
    bool identity_valid_ = false;
    std::uint64_t identity_device_ = 0U;
    std::uint64_t identity_index_ = 0U;
};

void test_socket_handle_width()
{
    static_assert(sizeof(NativeHandle) >= sizeof(void*),
                  "native socket/document handles are pointer width");
    static_assert(sizeof(PathChar) == sizeof(wchar_t),
                  "Windows paths are wide");
}

void test_pacing_sleep()
{
    const auto start = std::chrono::steady_clock::now();
    const bool high_resolution = platform::sleep_milliseconds(20U);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK(high_resolution);
    CHECK(elapsed.count() >= 20);
    CHECK(platform::sleep_milliseconds(0U));
}

void test_nonce_source()
{
    ipc::windows::WindowsTunerNonceSource source;
    const auto first = source.generate();
    const auto second = source.generate();
    CHECK(first && second);
    if (first && second) {
        CHECK(first.value().size() == ipc::kNonceLength);
        CHECK(std::memcmp(first.value().data(), second.value().data(),
                          ipc::kNonceLength) != 0);
    }
}

void test_shared_group_fails_closed()
{
    TempRoot root("group");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    const EndpointConfig config{runtime.c_str(), "grp", kControlEndpointName,
                                EndpointAccess::shared_group};
    const auto listener = SocketListener::listen(config);
    CHECK(!listener);
    if (!listener) {
        CHECK(listener.error() == Error::UNSUPPORTED);
    }
}

void test_endpoint_round_trip_and_stale_cleanup()
{
    TempRoot root("rt");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    const EndpointConfig config{runtime.c_str(), "rt", kControlEndpointName,
                                EndpointAccess::private_user};

    {
        auto listener = SocketListener::listen(config);
        CHECK(listener);
        if (!listener) {
            std::fprintf(stderr, "listen error=%d\n",
                         static_cast<int>(listener.error()));
            return;
        }
        CHECK(listener.value().endpoint_path()[0] != L'\0');

        // A second listener on the same live endpoint must fail busy.
        const auto duplicate = SocketListener::listen(config);
        CHECK(!duplicate);
        if (!duplicate) {
            CHECK(duplicate.error() == Error::BUSY);
        }

        const auto client = SocketStream::connect(config, Timeout{2000U});
        CHECK(client);
        if (client) {
            const auto accepted = listener.value().accept(Timeout{2000U});
            CHECK(accepted);
        }
    }

    // The listener cleaned up its endpoint; a fresh listen must succeed and
    // must not be confused by the removed socket.
    const auto recreated = SocketListener::listen(config);
    CHECK(recreated);
}

void test_serial_lease_exclusivity()
{
    TempRoot root("lease");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    constexpr std::string_view serial = "000000000000001";
    const EndpointConfig config{runtime.c_str(), serial.data(),
                                kControlEndpointName,
                                EndpointAccess::private_user};

    {
        const auto first = SerialEndpointLease::acquire(config, serial);
        if (!first) {
            std::fprintf(stderr, "stage lease first error=%d runtime=%s\n",
                         static_cast<int>(first.error()), runtime.c_str());
        }
        CHECK(first);
        if (first) {
            const auto second = SerialEndpointLease::acquire(config, serial);
            CHECK(!second);
            if (!second) {
                CHECK(second.error() == Error::BUSY);
            }
        }
    }
    const auto after_release = SerialEndpointLease::acquire(config, serial);
    if (!after_release) {
        std::fprintf(stderr, "stage lease after-release error=%d runtime=%s\n",
                     static_cast<int>(after_release.error()), runtime.c_str());
    }
    CHECK(after_release);
}

void test_identity_lease_exclusivity()
{
    TempRoot root("identity");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    const EndpointConfig first{runtime.c_str(), "left", kControlEndpointName};
    const EndpointConfig second{runtime.c_str(), "right", kControlEndpointName};
    constexpr std::string_view identity = "usb1-2.1";
    for (const std::string_view invalid : {"", ".", "..", "../usb", "a/b", "a\\b", "a:b"}) {
        CHECK(SerialEndpointLease::acquire_identity(first, invalid).error() ==
              Error::INVALID_ARGUMENT);
    }
    const std::string embedded("usb\0other", 9U);
    CHECK(SerialEndpointLease::acquire_identity(first, embedded).error() ==
          Error::INVALID_ARGUMENT);
    auto lease = SerialEndpointLease::acquire_identity(first, identity);
    CHECK(lease);
    if (!lease) return;
    CHECK(SerialEndpointLease::acquire_identity(second, identity).error() == Error::BUSY);
    auto other = SerialEndpointLease::acquire_identity(second, "usb1-3.1");
    CHECK(other);
    lease.value().close();
    lease.value().close();
    CHECK(SerialEndpointLease::acquire_identity(second, identity));
}

// Precreate the product directory with a protected DACL that grants Everyone
// full access. Same-user verification must reject it even though it is a
// well-formed protected DACL.
void test_protected_broad_dacl_rejected()
{
    TempRoot root("acl");
    CHECK(root.valid());
    const std::wstring product =
        std::filesystem::path(root.wide()).wstring() + L"\\px4-userland";
    CHECK(::CreateDirectoryW(product.c_str(), nullptr) != 0);
    // Give the directory a well-formed protected DACL that still grants
    // Everyone full access, but with the current user as owner, so the test
    // exercises the ACE inspection (not an owner mismatch).
    CHECK(windows_security::set_current_user_owner_and_dacl(product.c_str()));

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    CHECK(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
              L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != 0);
    if (descriptor == nullptr) {
        return;
    }
    PACL dacl = nullptr;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    CHECK(::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != 0);
    const DWORD status = ::SetNamedSecurityInfoW(
        const_cast<wchar_t*>(product.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
        nullptr, dacl, nullptr);
    ::LocalFree(descriptor);
    CHECK(status == ERROR_SUCCESS);

    const std::string runtime = root.utf8();
    const EndpointConfig config{runtime.c_str(), "acl", kControlEndpointName,
                                EndpointAccess::private_user};
    const auto listener = SocketListener::listen(config);
    CHECK(!listener);
    if (!listener) {
        CHECK(listener.error() == Error::INVALID_ARGUMENT);
    }
}

// While the serial lease is held, the lock file must not be deletable or
// renamable (no FILE_SHARE_DELETE), which protects the namespace from a lock
// split via replacement.
void test_lease_rename_delete_protection()
{
    TempRoot root("leaseprot");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    constexpr std::string_view serial = "000000000000001";
    const EndpointConfig config{runtime.c_str(), serial.data(),
                                kControlEndpointName,
                                EndpointAccess::private_user};
    std::wstring lock_path = std::filesystem::path(root.wide()).wstring() +
                             L"\\.px4-userland-" +
                             std::wstring(serial.begin(), serial.end()) + L".lock";
    {
        auto lease = SerialEndpointLease::acquire(config, serial);
        if (!lease) {
            std::fprintf(stderr, "stage leaseprot acquire error=%d runtime=%s\n",
                         static_cast<int>(lease.error()), runtime.c_str());
        }
        CHECK(lease);
        if (lease) {
            const BOOL deleted = ::DeleteFileW(lock_path.c_str());
            CHECK(deleted == 0);
            CHECK(::GetLastError() == ERROR_SHARING_VIOLATION ||
                  ::GetLastError() == ERROR_ACCESS_DENIED);
            const std::wstring renamed = lock_path + L".renamed";
            const BOOL moved = ::MoveFileW(lock_path.c_str(), renamed.c_str());
            CHECK(moved == 0);
        }
    }
    // After release the lock file is no longer held open.
    const BOOL deleted_after = ::DeleteFileW(lock_path.c_str());
    CHECK(deleted_after != 0 || ::GetLastError() == ERROR_FILE_NOT_FOUND);
}

// The Windows lease close() mirrors the POSIX last-holder cleanup: the
// final holder's close removes the lock file, so a cleanly stopped daemon
// leaves an empty runtime directory; while another (shared custom-instance)
// holder still holds the lease, the lock file must remain for that holder's
// own close.
void test_serial_lease_last_holder_cleanup()
{
    TempRoot root("leaselast");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    constexpr std::string_view serial = "000000000000001";
    const std::wstring lock_path =
        std::filesystem::path(root.wide()).wstring() + L"\\.px4-userland-" +
        std::wstring(serial.begin(), serial.end()) + L".lock";

    // Single exclusive holder: an explicit close() removes the lock file.
    {
        const EndpointConfig config{runtime.c_str(), serial.data(),
                                    kControlEndpointName,
                                    EndpointAccess::private_user};
        auto lease = SerialEndpointLease::acquire(config, serial);
        if (!lease) {
            std::fprintf(stderr, "stage leaselast single error=%d runtime=%s\n",
                         static_cast<int>(lease.error()), runtime.c_str());
        }
        CHECK(lease);
        if (lease) {
            CHECK(::GetFileAttributesW(lock_path.c_str()) !=
                  INVALID_FILE_ATTRIBUTES);
            lease.value().close();
            CHECK(::GetFileAttributesW(lock_path.c_str()) ==
                  INVALID_FILE_ATTRIBUTES);
        }
    }

    // Two shared custom-instance holders: the first close keeps the lock
    // file for the still-active holder; the last close (via the destructor)
    // removes it.
    const EndpointConfig custom_a{runtime.c_str(), "insta", kControlEndpointName,
                                  EndpointAccess::private_user};
    const EndpointConfig custom_b{runtime.c_str(), "instb", kControlEndpointName,
                                  EndpointAccess::private_user};
    {
        auto first = SerialEndpointLease::acquire(custom_a, serial);
        auto second = SerialEndpointLease::acquire(custom_b, serial);
        if (!first || !second) {
            std::fprintf(stderr, "stage leaselast shared errors=%d/%d runtime=%s\n",
                         static_cast<int>(first.error()),
                         static_cast<int>(second.error()), runtime.c_str());
        }
        CHECK(first);
        CHECK(second);
        if (first && second) {
            first.value().close();
            CHECK(::GetFileAttributesW(lock_path.c_str()) !=
                  INVALID_FILE_ATTRIBUTES);
        }
    }
    // `second` is released by its destructor here; it is the last holder.
    CHECK(::GetFileAttributesW(lock_path.c_str()) ==
          INVALID_FILE_ATTRIBUTES);
}

// A NULL DACL (which grants everyone full access) must fail closed.
void test_null_dacl_rejected()
{
    TempRoot root("nulldacl");
    CHECK(root.valid());
    const std::wstring product =
        std::filesystem::path(root.wide()).wstring() + L"\\px4-userland";
    CHECK(::CreateDirectoryW(product.c_str(), nullptr) != 0);
    CHECK(windows_security::set_current_user_owner_and_dacl(product.c_str()));
    const DWORD status = ::SetNamedSecurityInfoW(
        const_cast<wchar_t*>(product.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr);
    CHECK(status == ERROR_SUCCESS);

    const std::string runtime = root.utf8();
    const EndpointConfig config{runtime.c_str(), "nulldacl", kControlEndpointName,
                                EndpointAccess::private_user};
    const auto listener = SocketListener::listen(config);
    CHECK(!listener);
    if (!listener) {
        CHECK(listener.error() == Error::INVALID_ARGUMENT);
    }
}

// Cleanup must not delete a replacement object that took over the endpoint
// path after the original socket was removed.
void test_identity_cleanup_protects_replacement()
{
    TempRoot root("repl");
    CHECK(root.valid());
    const std::string runtime = root.utf8();
    const EndpointConfig config{runtime.c_str(), "repl", kControlEndpointName,
                                EndpointAccess::private_user};
    auto listener = SocketListener::listen(config);
    CHECK(listener);
    if (!listener) {
        return;
    }
    const std::wstring endpoint(listener.value().endpoint_path());
    CHECK(::DeleteFileW(endpoint.c_str()) != 0);
    const HANDLE replacement =
        ::CreateFileW(endpoint.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(replacement != INVALID_HANDLE_VALUE);
    if (replacement != INVALID_HANDLE_VALUE) {
        ::CloseHandle(replacement);
    }
    listener.value().close();
    CHECK(::GetFileAttributesW(endpoint.c_str()) != INVALID_FILE_ATTRIBUTES);
    (void)::DeleteFileW(endpoint.c_str());
}

// A firmware path outside the active code page must still open (and then be
// rejected by content policy), proving the Windows wide file open. No real
// firmware bytes are used.
void test_windows_unicode_firmware_path()
{
    TempRoot root("fw");
    CHECK(root.valid());
    std::wstring path = root.wide() + L"\\firmware-\u0100.bin";
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0U, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::array<std::uint8_t, 2U> bytes{0x01U, 0x02U};
    DWORD written = 0U;
    CHECK(::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                      nullptr) != 0);
    ::CloseHandle(file);

    const int utf8_length = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    CHECK(utf8_length > 0);
    std::string utf8;
    if (utf8_length > 0) {
        utf8.resize(static_cast<std::size_t>(utf8_length - 1));
        CHECK(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.c_str(), -1,
                                    utf8.data(), utf8_length, nullptr,
                                    nullptr) == utf8_length);
    }
#if defined(ASICEN_NATIVE_LOADER_TEST)
    asicen::FirmwareProvider provider(utf8, asicen::ModelId::W3u3);
#else
    FirmwareProvider provider(utf8);
#endif
    const auto loaded = provider.load();
    if (loaded) {
        std::fprintf(stderr, "stage unicode firmware unexpectedly accepted\n");
    }
    CHECK(!loaded);
    if (!loaded) {
        CHECK(loaded.error() == Error::FIRMWARE_REJECTED);
    }
    (void)::DeleteFileW(path.c_str());
}

}  // namespace

int main()
{
    test_socket_handle_width();
    test_pacing_sleep();
    test_nonce_source();
    test_shared_group_fails_closed();
    test_endpoint_round_trip_and_stale_cleanup();
    test_serial_lease_exclusivity();
    test_identity_lease_exclusivity();
    test_protected_broad_dacl_rejected();
    test_lease_rename_delete_protection();
    test_serial_lease_last_holder_cleanup();
    test_null_dacl_rejected();
    test_identity_cleanup_protects_replacement();
    test_windows_unicode_firmware_path();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("windows platform tests: PASS\n");
    return 0;
}
