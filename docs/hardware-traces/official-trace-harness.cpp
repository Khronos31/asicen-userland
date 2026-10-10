// Bounded initialization/lock trace for the isolated Linux guest.
// Link against libPlexLib_W3U3.a with the two --wrap flags documented in
// HARDWARE-VALIDATION.md. All diagnostics go to stderr.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "Data_define.h"
#pragma GCC diagnostic pop

// Data_define.h ships this public SDK type under #if 0, so it is not an active
// declaration. Reproduce the SDK's documented 58-byte layout here; keep the
// field order and names from that disabled declaration.
struct Customer_Info {
    UCHAR bUseCustomerInfo;
    UCHAR InfoID[2];
    UCHAR VID[2];
    UCHAR PID[2];
    UCHAR Manufact_Str[10];
    UCHAR Product_Str[16];
    UCHAR HID_Str[15];
    UCHAR Remote_Ctrl_Num;
    UCHAR Customer_Def_Data[8];
    UCHAR Support_Feature;
};
typedef Customer_Info* PCustomerInfo;

static_assert(sizeof(Customer_Info) == 58, "SDK Customer_Info ABI size changed");
static_assert(offsetof(Customer_Info, VID) == 3, "SDK Customer_Info VID offset changed");
static_assert(offsetof(Customer_Info, PID) == 5, "SDK Customer_Info PID offset changed");

extern "C" BOOL TF_DTV_DevCreate(PVOID*, char*) asm("_Z16TF_DTV_DevCreatePPvPc");
extern "C" BOOL TF_AssignDevExt_0(PVOID) asm("_Z17TF_AssignDevExt_0Pv");
extern "C" BOOL TF_bGetCusInfo(PVOID, PCustomerInfo) asm("_Z14TF_bGetCusInfoPvP13Customer_Info");
extern "C" int TF_DTV_Init(PVOID, BYTE, BOOL, BYTE) asm("_Z11TF_DTV_InitPvhhh");
extern "C" int TF_DTV_UnInit(PVOID, BYTE) asm("_Z13TF_DTV_UnInitPvh");
extern "C" int TF_DTV_SetTunerFreq(PVOID, BYTE, ULONG, BYTE) asm("_Z19TF_DTV_SetTunerFreqPvhmh");
extern "C" int TF_DTV_TunerLockCheck(PVOID, BYTE) asm("_Z21TF_DTV_TunerLockCheckPvh");
extern "C" int TF_DTV_GenEncSeed(PVOID, BYTE, BYTE*, BYTE, BYTE*,
                                 BYTE) asm("_Z17TF_DTV_GenEncSeedPvhPhhS0_h");
extern "C" BOOL TF_bGetBufLen(PVOID, BYTE, ULONG*) asm("_Z13TF_bGetBufLenPvhPm");
extern "C" int TF_DTV_StreamDataRead(PVOID, BYTE, BYTE*,
                                     unsigned int) asm("_Z21TF_DTV_StreamDataReadPvhPhj");
extern "C" BOOL TF_DTV_DevClose(PVOID) asm("_Z15TF_DTV_DevClosePv");
extern "C" BOOL Tnim_AcquireFrequency(PVOID, ULONG,
                                      BYTE) asm("_Z21Tnim_AcquireFrequencyP13_STnimControlmh");
extern "C" BOOL Tnim_IsLocked(PVOID) asm("_Z13Tnim_IsLockedP13_STnimControl");

extern "C" int real_ucSetGPIO(BYTE, BYTE, int) asm("__real__Z9ucSetGPIOhhi");
extern "C" BYTE Key1[];
extern "C" BYTE Key2[];

static unsigned long gpio_calls;
static unsigned long gpio_lnb_masked;
static unsigned long gpio_fully_skipped;
static unsigned long gpioex_skipped;
static volatile sig_atomic_t stop_requested;

static void request_stop(int)
{
    stop_requested = 1;
}

// All vendor request-0x08 GPIO writes from FUSBDTV.o reach this undefined
// reference. Keep the value and mask bit 0x20 clear before entering WDM_cmd.o.
extern "C" int wrap_ucSetGPIO(BYTE value, BYTE mask, int fd) asm("__wrap__Z9ucSetGPIOhhi");
extern "C" int wrap_ucSetGPIO(BYTE value, BYTE mask, int fd)
{
    ++gpio_calls;
    const BYTE safe_mask = static_cast<BYTE>(mask & 0xdf);
    const BYTE safe_value = static_cast<BYTE>(value & 0xdf);
    if ((mask & 0x20) != 0)
        ++gpio_lnb_masked;
    if (safe_mask == 0) {
        ++gpio_fully_skipped;
        fprintf(stderr, "GPIO guard: request blocked (value=%02x mask=%02x; remaining mask=00)\n",
                value, mask);
        // The only caller in the archive, FUSBDTV_Cmd_Set_GPIO, ignores this
        // byte and returns success unconditionally. Avoid issuing an empty ioctl.
        return safe_value;
    }
    fprintf(stderr, "GPIO guard: value=%02x mask=%02x -> value=%02x mask=%02x\n", value, mask,
            safe_value, safe_mask);
    return real_ucSetGPIO(safe_value, safe_mask, fd);
}

// GPIOEx uses a distinct request-0x10 command whose board-level effects are
// not proven. The official FUSBDTV wrapper also ignores this helper's result.
extern "C" int wrap_ucSetGPIOEx(BYTE value, BYTE mask, int fd) asm("__wrap__Z11ucSetGPIOExhhi");
extern "C" int wrap_ucSetGPIOEx(BYTE value, BYTE mask, int fd)
{
    (void)fd;
    ++gpioex_skipped;
    fprintf(stderr, "GPIOEx guard: request skipped (value=%02x mask=%02x)\n", value, mask);
    return value;
}

static bool read_deadline_expired(const timespec& deadline)
{
    if (stop_requested != 0) {
        return true;
    }
    timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return true;
    return now.tv_sec > deadline.tv_sec ||
           (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

struct RunState {
    PVOID extension;
    bool device_may_exist;
    bool gpio_snapshot_valid;
    BYTE initial_gpio;
    bool lane_init_attempted[2];
    bool lane_uninit_done[2];
};

static bool cleanup(RunState& state)
{
    bool ok = true;
    if (state.extension != 0) {
        for (int lane = 1; lane >= 0; --lane) {
            if (state.lane_init_attempted[lane] && !state.lane_uninit_done[lane]) {
                int rc = TF_DTV_UnInit(state.extension, static_cast<BYTE>(lane));
                state.lane_uninit_done[lane] = true;
                fprintf(stderr, "UnInit local%d rc=%d\n", lane, rc);
                if (rc != 1)
                    ok = false;
            }
        }
        if (state.device_may_exist) {
            BOOL rc = TF_DTV_DevClose(state.extension);
            fprintf(stderr, "DevClose rc=%u\n", static_cast<unsigned>(rc));
            if (!rc)
                ok = false;
            state.device_may_exist = false;
        }
        state.extension = 0;
    }
    fprintf(
        stderr,
        "GPIO guard totals: calls=%lu bit20_masked=%lu empty_mask_skipped=%lu GPIOEx_skipped=%lu\n",
        gpio_calls, gpio_lnb_masked, gpio_fully_skipped, gpioex_skipped);
    if (state.gpio_snapshot_valid) {
        fprintf(stderr,
                "Initial GPIO snapshot=%02x; parent must restore non-LNB bits with mask df after "
                "DevClose\n",
                state.initial_gpio);
    }
    if (fflush(stderr) != 0) {
        ok = false;
    }
    return ok;
}

static bool fill_random16(BYTE* bytes)
{
    if (bytes == nullptr)
        return false;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t offset = 0;
    while (offset < 16) {
        ssize_t count = read(fd, bytes + offset, 16 - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            close(fd);
            memset(bytes, 0, 16);
            return false;
        }
        offset += static_cast<size_t>(count);
    }
    if (close(fd) != 0) {
        memset(bytes, 0, 16);
        return false;
    }
    return true;
}

static bool write_all(int fd, const BYTE* bytes, size_t length, const timespec& deadline)
{
    size_t offset = 0;
    while (offset < length) {
        if (read_deadline_expired(deadline)) {
            return false;
        }
        ssize_t count = write(fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

static bool run_seed_capture(RunState& state, const char* output_path, bool use_key2)
{
    const BYTE mask[16] = {0x1f, 0xc5, 0x62, 0xb9, 0xb3, 0x36, 0x4c, 0x38,
                           0x86, 0xe5, 0x21, 0x1e, 0x94, 0x4e, 0xce, 0x3a};
    BYTE ap_seed[16] = {};
    BYTE pc_key[16] = {};
    BYTE stream_buffer[188 * 64] = {};
    bool ok = false;
    int output_fd = -1;
    PVOID control = 0;
    PVOID back_reference = 0;
    const BYTE* ctrl_bytes = 0;
    const BYTE* selected_key = use_key2 ? Key2 : Key1;
    unsigned int init_count = 0;

    const BYTE* ext_bytes = static_cast<const BYTE*>(state.extension);
    memcpy(&control, ext_bytes + 0x460, sizeof(control));
    if (control == 0) {
        fprintf(stderr, "Seed capture rejected: local1 control pointer is null\n");
        goto done;
    }
    ctrl_bytes = static_cast<const BYTE*>(control);
    memcpy(&back_reference, ctrl_bytes + 0x1d38, sizeof(back_reference));
    memcpy(&init_count, ctrl_bytes + 0x1d40, sizeof(init_count));
    if (back_reference != state.extension || ctrl_bytes[0x1d30] != 1 || init_count != 1 ||
        ctrl_bytes[0x15d8] != 1 || ctrl_bytes[0x15ec] != 0 || ctrl_bytes[0x30d60] != 1 ||
        ctrl_bytes[0x30d61] != 1) {
        fprintf(stderr, "Seed capture rejected: local1 control/init/controller/identify/gate/row0 "
                        "validation failed\n");
        goto done;
    }

    // The distributed archive exports Key1 and Key2 as 64 fixed-width rows.
    // Init marks row0 eligible; direct identify compares APEncSeed against the
    // selected row XOR the library's fixed local 16-byte mask. Never log bytes.
    for (size_t i = 0; i < 16; ++i)
        ap_seed[i] = static_cast<BYTE>(selected_key[i] ^ mask[i]);
    if (!fill_random16(pc_key)) {
        fprintf(stderr, "Seed capture failed: could not obtain 16 random PCKey bytes\n");
        goto done;
    }

    {
        int seed_rc = TF_DTV_GenEncSeed(state.extension, 1, ap_seed, 16, pc_key, 16);
        const BYTE gate_after_seed = ctrl_bytes[0x30d60];
        const BYTE app_key_state = ctrl_bytes[0x30da1];
        const BYTE expected_app_key_state = use_key2 ? 1 : 0;
        fprintf(stderr,
                "Seed API local1 selection=Key%u rc=%d readiness_gate_after=%u app_key_state=%u\n",
                use_key2 ? 2U : 1U, seed_rc, static_cast<unsigned>(gate_after_seed),
                static_cast<unsigned>(app_key_state));
        // A controller-state early return can report 1 without reaching the
        // output path. Require the source-observed readiness transition as well.
        if (seed_rc != 1 || gate_after_seed != 0 || app_key_state != expected_app_key_state)
            goto done;
    }

    {
        int tune_rc = TF_DTV_SetTunerFreq(state.extension, 1, 557142UL, 6);
        const BYTE gate_after_tune = ctrl_bytes[0x30d60];
        fprintf(stderr,
                "Seed capture tune local1 freq_khz=557142 bw=6 rc=%d readiness_gate_after=%u\n",
                tune_rc, static_cast<unsigned>(gate_after_tune));
        if (tune_rc != 1 || gate_after_tune != 0)
            goto done;
    }

    {
        timespec lock_deadline;
        if (clock_gettime(CLOCK_MONOTONIC, &lock_deadline) != 0)
            goto done;
        lock_deadline.tv_sec += 5;
        bool lock_seen = false;
        do {
            int lock_rc = TF_DTV_TunerLockCheck(state.extension, 1);
            fprintf(stderr, "Seed capture RF lock local1 rc=%d\n", lock_rc);
            if (lock_rc == 1) {
                lock_seen = true;
                break;
            }
            if (lock_rc < 0 || read_deadline_expired(lock_deadline))
                break;
            struct timespec pause = {0, 100000000};
            while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
            }
        } while (!read_deadline_expired(lock_deadline));
        if (!lock_seen) {
            fprintf(stderr, "Seed capture stopped: RF lock not observed within 5 seconds\n");
            goto done;
        }
    }

    output_fd =
        open(output_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (output_fd < 0) {
        fprintf(stderr, "Seed capture output create failed (errno=%d)\n", errno);
        goto done;
    }

    {
        timespec capture_deadline;
        if (clock_gettime(CLOCK_MONOTONIC, &capture_deadline) != 0)
            goto done;
        capture_deadline.tv_sec += 20;
        unsigned long long total_bytes = 0;
        unsigned int read_calls = 0;
        while (!read_deadline_expired(capture_deadline) && read_calls < 4096) {
            ULONG available = 0;
            BOOL length_rc = TF_bGetBufLen(state.extension, 1, &available);
            if (!length_rc || available < sizeof(stream_buffer)) {
                struct timespec pause = {0, 100000000};
                while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
                }
                continue;
            }
            int actual =
                TF_DTV_StreamDataRead(state.extension, 1, stream_buffer, sizeof(stream_buffer));
            ++read_calls;
            if (actual < 0 || static_cast<size_t>(actual) > sizeof(stream_buffer)) {
                fprintf(stderr, "Seed capture read failed: rc=%d\n", actual);
                goto done;
            }
            if (actual == 0) {
                struct timespec pause = {0, 100000000};
                while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
                }
                continue;
            }
            if (!write_all(output_fd, stream_buffer, static_cast<size_t>(actual),
                           capture_deadline)) {
                fprintf(stderr, "Seed capture output write failed (errno=%d)\n", errno);
                goto done;
            }
            total_bytes += static_cast<unsigned int>(actual);
        }
        if (fsync(output_fd) != 0) {
            fprintf(stderr, "Seed capture output sync failed (errno=%d)\n", errno);
            goto done;
        }
        fprintf(stderr, "Seed capture finished: bytes=%llu read_calls=%u\n", total_bytes,
                read_calls);
        ok = total_bytes != 0;
    }

done:
    if (output_fd >= 0 && close(output_fd) != 0) {
        fprintf(stderr, "Seed capture output close failed (errno=%d)\n", errno);
        ok = false;
    }
    memset(ap_seed, 0, sizeof(ap_seed));
    memset(pc_key, 0, sizeof(pc_key));
    memset(stream_buffer, 0, sizeof(stream_buffer));
    return ok;
}

int main(int argc, char** argv)
{
    const bool customer_only = argc == 4 && strcmp(argv[3], "--customer-only") == 0;
    const bool rf_diagnostic = argc == 4 && strcmp(argv[3], "--rf-diagnostic") == 0;
    const bool seed_capture = argc == 5 && strcmp(argv[3], "--seed-capture") == 0;
    const bool seed_key2_capture = argc == 5 && strcmp(argv[3], "--seed-key2-capture") == 0;
    if ((argc != 3 && !customer_only && !rf_diagnostic && !seed_capture && !seed_key2_capture) ||
        argv[1][0] != '/' || strlen(argv[2]) != 2 ||
        ((seed_capture || seed_key2_capture) && argv[4][0] != '/')) {
        fprintf(stderr,
                "usage: %s /dev/as11usbdtv0 initial_gpio_hex "
                "[--customer-only|--rf-diagnostic|--seed-capture|--seed-key2-capture "
                "/absolute/output.ts]\n",
                argv[0]);
        return 2;
    }
    auto hex_nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    int hi = hex_nibble(argv[2][0]);
    int lo = hex_nibble(argv[2][1]);
    if (hi < 0 || lo < 0) {
        fprintf(stderr, "initial_gpio_hex must be exactly two hex digits\n");
        return 2;
    }

    if (signal(SIGINT, request_stop) == SIG_ERR) {
        return 10;
    }
    if (signal(SIGTERM, request_stop) == SIG_ERR) {
        (void)signal(SIGINT, SIG_DFL);
        return 10;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        (void)signal(SIGINT, SIG_DFL);
        (void)signal(SIGTERM, SIG_DFL);
        return 10;
    }

    RunState state = {};
    state.gpio_snapshot_valid = true;
    state.initial_gpio = static_cast<BYTE>((hi << 4) | lo);
    char* device_path = argv[1];
    BOOL create_rc = TF_DTV_DevCreate(&state.extension, device_path);
    // DTV_DevCreate may have opened the node and allocated an extension before
    // DTV_Start reports failure, so a non-null extension still needs DevClose.
    state.device_may_exist = state.extension != 0;
    fprintf(stderr, "DevCreate rc=%u extension=%p\n", static_cast<unsigned>(create_rc),
            state.extension);
    if (!create_rc || state.extension == 0) {
        cleanup(state);
        return 1;
    }

    BOOL assign_rc = TF_AssignDevExt_0(state.extension);
    fprintf(stderr, "AssignDevExt_0 rc=%u\n", static_cast<unsigned>(assign_rc));
    if (!assign_rc) {
        cleanup(state);
        return 1;
    }

    Customer_Info customer_info = {};
    BOOL customer_rc = TF_bGetCusInfo(state.extension, &customer_info);
    const unsigned int customer_vid =
        (static_cast<unsigned int>(customer_info.VID[0]) << 8) | customer_info.VID[1];
    const unsigned int customer_pid =
        (static_cast<unsigned int>(customer_info.PID[0]) << 8) | customer_info.PID[1];
    const BYTE* extension_bytes = static_cast<const BYTE*>(state.extension);
    const unsigned int cached_vid =
        (static_cast<unsigned int>(extension_bytes[0x476b]) << 8) | extension_bytes[0x476c];
    const unsigned int cached_pid =
        (static_cast<unsigned int>(extension_bytes[0x476d]) << 8) | extension_bytes[0x476e];
    fprintf(stderr, "CustomerInfo rc=%u vid=%04x pid=%04x cached_vid=%04x cached_pid=%04x\n",
            static_cast<unsigned>(customer_rc), customer_vid, customer_pid, cached_vid, cached_pid);
    const bool customer_id_valid = customer_rc == 1 && customer_vid == 0x0b06 &&
                                   (customer_pid == 0x0004 || customer_pid == 0x0005) &&
                                   cached_vid == customer_vid && cached_pid == customer_pid;
    if (!customer_id_valid) {
        fprintf(stderr, "CustomerInfo validation failed; skipping Init\n");
        cleanup(state);
        return 1;
    }
    if (customer_only) {
        fprintf(stderr, "CustomerInfo-only phase complete; skipping Init/tune/lock\n");
        return cleanup(state) ? 0 : 1;
    }

    // TF_DTV_Init's second argument selects the lane object; its third bool
    // maps to the internal tuner selector 0/2. The fourth byte is the DTV_Init
    // mode. Use paired values 0/0 then 1/0 for the terrestrial lanes; never use
    // a value above 1, which enters the key-generation path.
    for (BYTE lane = 0; lane < 2; ++lane) {
        state.lane_init_attempted[lane] = true;
        int rc = TF_DTV_Init(state.extension, lane, lane, 0);
        fprintf(stderr, "Init local%u rc=%d\n", static_cast<unsigned>(lane), rc);
        if (rc < 0) {
            cleanup(state);
            return 1;
        }
    }

    if (seed_capture || seed_key2_capture) {
        const bool capture_ok = run_seed_capture(state, argv[4], seed_key2_capture);
        const bool cleanup_ok = cleanup(state);
        if (stop_requested != 0) {
            return 9;
        }
        return capture_ok && cleanup_ok ? 0 : 1;
    }

    int tune_rc = TF_DTV_SetTunerFreq(state.extension, 1, 557142UL, 6);
    fprintf(stderr, "Terrestrial tune local1 freq_khz=557142 bw=6 rc=%d\n", tune_rc);
    if (tune_rc != 1) {
        cleanup(state);
        return 1;
    }

    timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        cleanup(state);
        return 1;
    }
    deadline.tv_sec += 5;
    int lock_rc = 0;
    bool lock_seen = false;
    do {
        lock_rc = TF_DTV_TunerLockCheck(state.extension, 1);
        fprintf(stderr, "Terrestrial lock local1 rc=%d\n", lock_rc);
        if (lock_rc < 0)
            break;
        if (lock_rc == 1 && !lock_seen) {
            lock_seen = true;
            if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
                lock_rc = -1;
                break;
            }
            deadline.tv_sec += 5;
            fprintf(stderr, "Holding started stream for 5 seconds after lock\n");
        }
        if (read_deadline_expired(deadline))
            break;
        struct timespec pause = {0, 100000000};
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
        }
    } while (!read_deadline_expired(deadline));

    // StreamThreadRun was started by the official Init wrapper; deliberately
    // omit StreamDataRead because its bulk-read ioctl has no proven 5s timeout.
    fprintf(stderr, "StreamDataRead omitted; public_api_lock_seen=%d public_api_lock_final=%d\n",
            lock_seen ? 1 : 0, lock_rc);

    bool rf_lock_seen = false;
    int rf_acquire_rc = 0;
    if (rf_diagnostic) {
        // This opt-in path is a lower-level RF diagnostic, not the normal public
        // receive chain. Resolve the active local1 control object read-only and
        // reject unexpected object identity, lifecycle, or tune scratch state.
        const BYTE* ext_bytes = static_cast<const BYTE*>(state.extension);
        PVOID control = 0;
        memcpy(&control, ext_bytes + 0x460, sizeof(control));
        if (control == 0) {
            fprintf(stderr, "RF diagnostic rejected: local1 control pointer is null\n");
            cleanup(state);
            return 1;
        }
        const BYTE* ctrl_bytes = static_cast<const BYTE*>(control);
        PVOID back_reference = 0;
        memcpy(&back_reference, ctrl_bytes + 0x1d38, sizeof(back_reference));
        const BYTE control_lane = ctrl_bytes[0x1d30];
        unsigned int init_count = 0;
        memcpy(&init_count, ctrl_bytes + 0x1d40, sizeof(init_count));
        const BYTE stream_gate = ctrl_bytes[0x30d60];
        unsigned int acquire_state = 0;
        memcpy(&acquire_state, ctrl_bytes + 0x1c70, sizeof(acquire_state));
        const size_t lane_scratch = 0x4ea0 + 1 * 0x50;
        ULONG scratch0 = 0, scratch1 = 0, scratch2 = 0;
        memcpy(&scratch0, ext_bytes + lane_scratch, sizeof(scratch0));
        memcpy(&scratch1, ext_bytes + lane_scratch + 8, sizeof(scratch1));
        memcpy(&scratch2, ext_bytes + lane_scratch + 16, sizeof(scratch2));
        fprintf(stderr,
                "RF diagnostic state: control=%p backref_match=%d lane=%u init_count=%u "
                "stream_gate=%u acquire_state_nonzero=%d scratch=%lx/%lx/%lx\n",
                control, back_reference == state.extension ? 1 : 0,
                static_cast<unsigned>(control_lane), init_count, static_cast<unsigned>(stream_gate),
                acquire_state != 0 ? 1 : 0, static_cast<unsigned long>(scratch0),
                static_cast<unsigned long>(scratch1), static_cast<unsigned long>(scratch2));
        if (back_reference != state.extension || control_lane != 1 || init_count != 1 ||
            stream_gate != 1 || scratch0 != 0 || scratch1 != 0 || scratch2 != 0) {
            fprintf(
                stderr,
                "RF diagnostic rejected: control identity/lifecycle/scratch validation failed\n");
            cleanup(state);
            return 1;
        }

        // The public tune wrapper short-circuits in the observed post-stream
        // state. Call the vendor's lower-level acquisition primitive once without
        // altering state flags or seed inputs, then use its RF lock primitive.
        const int rf_lock_before = Tnim_IsLocked(control);
        fprintf(stderr, "RF diagnostic Tnim_IsLocked before acquire local1 rc=%d\n",
                rf_lock_before);
        rf_acquire_rc = Tnim_AcquireFrequency(control, 557142UL, 6);
        fprintf(stderr, "RF diagnostic Tnim_AcquireFrequency local1 freq_khz=557142 bw=6 rc=%d\n",
                rf_acquire_rc);
        fprintf(stderr, "RF diagnostic stream_gate_after_acquire=%u\n",
                static_cast<unsigned>(ctrl_bytes[0x30d60]));
        timespec rf_deadline;
        if (clock_gettime(CLOCK_MONOTONIC, &rf_deadline) != 0) {
            cleanup(state);
            return 1;
        }
        rf_deadline.tv_sec += 5;
        int rf_lock_rc = 0;
        bool rf_lock_hold_started = false;
        do {
            rf_lock_rc = Tnim_IsLocked(control);
            fprintf(stderr, "RF diagnostic Tnim_IsLocked local1 rc=%d\n", rf_lock_rc);
            if (rf_lock_rc == 1 && !rf_lock_hold_started) {
                rf_lock_seen = true;
                rf_lock_hold_started = true;
                if (clock_gettime(CLOCK_MONOTONIC, &rf_deadline) != 0)
                    break;
                rf_deadline.tv_sec += 5;
                fprintf(stderr, "RF lock held observation started for 5 seconds\n");
            }
            if (read_deadline_expired(rf_deadline))
                break;
            struct timespec pause = {0, 100000000};
            while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
            }
        } while (!read_deadline_expired(rf_deadline));
        fprintf(stderr, "RF diagnostic result: lock_seen=%d\n", rf_lock_seen ? 1 : 0);
    }

    bool cleanup_ok = cleanup(state);
    if (fflush(stderr) != 0)
        cleanup_ok = false;
    if (stop_requested != 0) {
        return 9;
    }
    if (rf_diagnostic)
        return rf_acquire_rc == 1 && rf_lock_seen && cleanup_ok ? 0 : 1;
    return lock_seen && lock_rc == 1 && cleanup_ok ? 0 : 1;
}
