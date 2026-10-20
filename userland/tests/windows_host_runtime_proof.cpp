// SPDX-License-Identifier: GPL-2.0-only

#include "px4/posix_ipc_contract.h"
#include "windows/windows_security.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

namespace {

bool file_identity(const wchar_t* path, std::uint64_t& volume,
                   std::uint64_t& index) noexcept
{
    const HANDLE handle = ::CreateFileW(
        path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const BOOL read = ::GetFileInformationByHandle(handle, &information);
    (void)::CloseHandle(handle);
    if (read == 0 || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return false;
    }
    volume = information.dwVolumeSerialNumber;
    index = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
            static_cast<std::uint64_t>(information.nFileIndexLow);
    return true;
}

class ScratchRoot final {
public:
    ScratchRoot()
    {
        std::array<wchar_t, MAX_PATH> profile{};
        const DWORD length = ::GetEnvironmentVariableW(
            L"USERPROFILE", profile.data(), static_cast<DWORD>(profile.size()));
        if (length == 0U || length >= profile.size()) {
            return;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::array<std::uint8_t, 5U> random{};
            std::array<wchar_t, 16U> token{};
            if (!px4::userland::windows_security::random_hex(
                    random.data(), random.size(), token.data(), token.size())) {
                return;
            }
            root_ = std::filesystem::path(profile.data()) /
                    (std::wstring(L"arw-") + token.data());
            std::error_code error;
            if (std::filesystem::exists(root_, error)) {
                continue;
            }
            if (!std::filesystem::create_directory(root_, error) || error) {
                return;
            }
            created_ = file_identity(root_.c_str(), volume_, index_);
            valid_ = created_ &&
                px4::userland::windows_security::set_current_user_owner_and_dacl(
                    root_.c_str());
            return;
        }
    }

    ~ScratchRoot()
    {
        if (!created_) {
            return;
        }
        std::uint64_t volume = 0U;
        std::uint64_t index = 0U;
        if (file_identity(root_.c_str(), volume, index) && volume == volume_ &&
            index == index_) {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }
    }

    ScratchRoot(const ScratchRoot&) = delete;
    ScratchRoot& operator=(const ScratchRoot&) = delete;

    bool valid() const noexcept { return valid_; }
    std::wstring wide() const { return root_.wstring(); }
    std::string utf8() const { return root_.u8string(); }

private:
    std::filesystem::path root_;
    std::uint64_t volume_ = 0U;
    std::uint64_t index_ = 0U;
    bool created_ = false;
    bool valid_ = false;
};

bool check(bool condition, const char* expression)
{
    if (!condition) {
        std::fprintf(stderr, "check failed: %s\n", expression);
    }
    return condition;
}

int run_proof()
{
    ScratchRoot root;
    if (!check(root.valid(), "scratch root created and secured")) {
        return 1;
    }
    const std::string runtime = root.utf8();
    const EndpointConfig first_config{runtime.c_str(), "RoomA", kControlEndpointName,
                                      EndpointAccess::private_user};
    auto listener_result = SocketListener::listen(first_config);
    if (!check(static_cast<bool>(listener_result), "ASICEN listener starts")) {
        return 1;
    }
    auto listener = std::move(listener_result.value());
    const std::wstring expected_suffix =
        L"\\asicen-userland\\v1\\RoomA\\control.sock";
    if (!check(std::wstring_view(listener.endpoint_path()).find(expected_suffix) !=
                   std::wstring_view::npos,
               "listener uses ASICEN v1 path and preserves RoomA")) {
        return 1;
    }

    const std::wstring p_root = root.wide() + L"\\px4-userland";
    if (!check(::GetFileAttributesW(p_root.c_str()) == INVALID_FILE_ATTRIBUTES,
               "no PX4 product namespace published")) {
        return 1;
    }

    auto discovery = discover_runtime_instances(runtime.c_str(),
                                                EndpointAccess::private_user);
    if (!check(static_cast<bool>(discovery), "ASICEN instance discovery succeeds")) {
        return 1;
    }
    bool found_room_a = false;
    for (std::size_t index = 0U; index < discovery.value().count; ++index) {
        if (std::string_view(discovery.value().instances[index].data()) == "RoomA") {
            found_room_a = true;
        }
    }
    if (!check(found_room_a, "discovery returns RoomA with original case")) {
        return 1;
    }

    auto client_result = SocketStream::connect(
        first_config, dtv::userland::Timeout{1000U});
    if (!check(static_cast<bool>(client_result), "ASICEN client connects")) {
        return 1;
    }
    auto client = std::move(client_result.value());
    client.close();

    constexpr std::string_view identity = "ASICEN-identity-20261020";
    auto first_lease = SerialEndpointLease::acquire_identity(first_config, identity);
    if (!check(static_cast<bool>(first_lease), "identity lease acquired")) {
        return 1;
    }
    auto held_lease = std::move(first_lease.value());
    const EndpointConfig other_config{runtime.c_str(), "RoomB", kControlEndpointName,
                                       EndpointAccess::private_user};
    auto duplicate = SerialEndpointLease::acquire_identity(other_config, identity);
    if (!check(!duplicate && duplicate.error() == dtv::userland::Error::BUSY,
               "same identity is exclusive across different instances")) {
        return 1;
    }
    held_lease.close();

    const std::wstring lock_path = root.wide() +
        L"\\.asicen-userland-ASICEN-identity-20261020.lock";
    if (!check(::GetFileAttributesW(lock_path.c_str()) == INVALID_FILE_ATTRIBUTES,
               "last identity lease removes its lock file")) {
        return 1;
    }
    auto reacquired = SerialEndpointLease::acquire_identity(other_config, identity);
    if (!check(static_cast<bool>(reacquired), "identity lease reacquired after close")) {
        return 1;
    }
    auto final_lease = std::move(reacquired.value());
    final_lease.close();
    listener.close();

    if (!check(::GetFileAttributesW(p_root.c_str()) == INVALID_FILE_ATTRIBUTES,
               "ASICEN proof never publishes into PX4 namespace")) {
        return 1;
    }
    std::puts("ASICEN Windows host runtime proof: PASS");
    return 0;
}

}  // namespace

int main()
{
    return run_proof();
}
