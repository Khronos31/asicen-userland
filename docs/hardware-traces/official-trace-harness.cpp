// Bounded initialization/lock trace for the isolated Linux guest.
// Link against libPlexLib_W3U3.a with the two --wrap flags documented in
// HARDWARE-VALIDATION.md. All diagnostics go to stderr.

#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
typedef Customer_Info *PCustomerInfo;

static_assert(sizeof(Customer_Info) == 58, "SDK Customer_Info ABI size changed");
static_assert(offsetof(Customer_Info, VID) == 3, "SDK Customer_Info VID offset changed");
static_assert(offsetof(Customer_Info, PID) == 5, "SDK Customer_Info PID offset changed");

extern "C" BOOL TF_DTV_DevCreate(PVOID *, char *) asm("_Z16TF_DTV_DevCreatePPvPc");
extern "C" BOOL TF_AssignDevExt_0(PVOID) asm("_Z17TF_AssignDevExt_0Pv");
extern "C" BOOL TF_bGetCusInfo(PVOID, PCustomerInfo)
    asm("_Z14TF_bGetCusInfoPvP13Customer_Info");
extern "C" int TF_DTV_Init(PVOID, BYTE, BOOL, BYTE) asm("_Z11TF_DTV_InitPvhhh");
extern "C" int TF_DTV_UnInit(PVOID, BYTE) asm("_Z13TF_DTV_UnInitPvh");
extern "C" int TF_DTV_SetTunerFreq(PVOID, BYTE, ULONG, BYTE) asm("_Z19TF_DTV_SetTunerFreqPvhmh");
extern "C" int TF_DTV_TunerLockCheck(PVOID, BYTE) asm("_Z21TF_DTV_TunerLockCheckPvh");
extern "C" BOOL TF_DTV_DevClose(PVOID) asm("_Z15TF_DTV_DevClosePv");

extern "C" int real_ucSetGPIO(BYTE, BYTE, int) asm("__real__Z9ucSetGPIOhhi");

static unsigned long gpio_calls;
static unsigned long gpio_lnb_masked;
static unsigned long gpio_fully_skipped;
static unsigned long gpioex_skipped;

// All vendor request-0x08 GPIO writes from FUSBDTV.o reach this undefined
// reference. Keep the value and mask bit 0x20 clear before entering WDM_cmd.o.
extern "C" int wrap_ucSetGPIO(BYTE value, BYTE mask, int fd)
    asm("__wrap__Z9ucSetGPIOhhi");
extern "C" int wrap_ucSetGPIO(BYTE value, BYTE mask, int fd) {
  ++gpio_calls;
  const BYTE safe_mask = static_cast<BYTE>(mask & 0xdf);
  const BYTE safe_value = static_cast<BYTE>(value & 0xdf);
  if ((mask & 0x20) != 0) ++gpio_lnb_masked;
  if (safe_mask == 0) {
    ++gpio_fully_skipped;
    fprintf(stderr,
            "GPIO guard: request blocked (value=%02x mask=%02x; remaining mask=00)\n",
            value, mask);
    // The only caller in the archive, FUSBDTV_Cmd_Set_GPIO, ignores this
    // byte and returns success unconditionally. Avoid issuing an empty ioctl.
    return safe_value;
  }
  fprintf(stderr, "GPIO guard: value=%02x mask=%02x -> value=%02x mask=%02x\n",
          value, mask, safe_value, safe_mask);
  return real_ucSetGPIO(safe_value, safe_mask, fd);
}

// GPIOEx uses a distinct request-0x10 command whose board-level effects are
// not proven. The official FUSBDTV wrapper also ignores this helper's result.
extern "C" int wrap_ucSetGPIOEx(BYTE value, BYTE mask, int fd)
    asm("__wrap__Z11ucSetGPIOExhhi");
extern "C" int wrap_ucSetGPIOEx(BYTE value, BYTE mask, int fd) {
  (void)fd;
  ++gpioex_skipped;
  fprintf(stderr, "GPIOEx guard: request skipped (value=%02x mask=%02x)\n",
          value, mask);
  return value;
}

static bool read_deadline_expired(const timespec &deadline) {
  timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return true;
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

static bool cleanup(RunState &state) {
  bool ok = true;
  if (state.extension != 0) {
    for (int lane = 1; lane >= 0; --lane) {
      if (state.lane_init_attempted[lane] && !state.lane_uninit_done[lane]) {
        int rc = TF_DTV_UnInit(state.extension, static_cast<BYTE>(lane));
        state.lane_uninit_done[lane] = true;
        fprintf(stderr, "UnInit local%d rc=%d\n", lane, rc);
        if (rc != 1) ok = false;
      }
    }
    if (state.device_may_exist) {
      BOOL rc = TF_DTV_DevClose(state.extension);
      fprintf(stderr, "DevClose rc=%u\n", static_cast<unsigned>(rc));
      if (!rc) ok = false;
      state.device_may_exist = false;
    }
    state.extension = 0;
  }
  fprintf(stderr,
          "GPIO guard totals: calls=%lu bit20_masked=%lu empty_mask_skipped=%lu GPIOEx_skipped=%lu\n",
          gpio_calls, gpio_lnb_masked, gpio_fully_skipped, gpioex_skipped);
  if (state.gpio_snapshot_valid) {
    fprintf(stderr,
            "Initial GPIO snapshot=%02x; parent must restore non-LNB bits with mask df after DevClose\n",
            state.initial_gpio);
  }
  return ok;
}

int main(int argc, char **argv) {
  const bool customer_only = argc == 4 && strcmp(argv[3], "--customer-only") == 0;
  if ((argc != 3 && !customer_only) || argv[1][0] != '/' || strlen(argv[2]) != 2) {
    fprintf(stderr,
            "usage: %s /dev/as11usbdtv0 initial_gpio_hex [--customer-only]\n",
            argv[0]);
    return 2;
  }
  auto hex_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int hi = hex_nibble(argv[2][0]);
  int lo = hex_nibble(argv[2][1]);
  if (hi < 0 || lo < 0) {
    fprintf(stderr, "initial_gpio_hex must be exactly two hex digits\n");
    return 2;
  }

  RunState state = {};
  state.gpio_snapshot_valid = true;
  state.initial_gpio = static_cast<BYTE>((hi << 4) | lo);
  char *device_path = argv[1];
  BOOL create_rc = TF_DTV_DevCreate(&state.extension, device_path);
  // DTV_DevCreate may have opened the node and allocated an extension before
  // DTV_Start reports failure, so a non-null extension still needs DevClose.
  state.device_may_exist = state.extension != 0;
  fprintf(stderr, "DevCreate rc=%u extension=%p\n",
          static_cast<unsigned>(create_rc), state.extension);
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
  const BYTE *extension_bytes = static_cast<const BYTE *>(state.extension);
  const unsigned int cached_vid =
      (static_cast<unsigned int>(extension_bytes[0x476b]) << 8) |
      extension_bytes[0x476c];
  const unsigned int cached_pid =
      (static_cast<unsigned int>(extension_bytes[0x476d]) << 8) |
      extension_bytes[0x476e];
  fprintf(stderr,
          "CustomerInfo rc=%u vid=%04x pid=%04x cached_vid=%04x cached_pid=%04x\n",
          static_cast<unsigned>(customer_rc), customer_vid, customer_pid,
          cached_vid, cached_pid);
  const bool customer_id_valid =
      customer_rc == 1 && customer_vid == 0x0b06 &&
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
    if (lock_rc < 0) break;
    if (lock_rc == 1 && !lock_seen) {
      lock_seen = true;
      if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        lock_rc = -1;
        break;
      }
      deadline.tv_sec += 5;
      fprintf(stderr, "Holding started stream for 5 seconds after lock\n");
    }
    if (read_deadline_expired(deadline)) break;
    struct timespec pause = {0, 100000000};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
  } while (!read_deadline_expired(deadline));

  // StreamThreadRun was started by the official Init wrapper; deliberately
  // omit StreamDataRead because its bulk-read ioctl has no proven 5s timeout.
  fprintf(stderr, "StreamDataRead omitted; lock_seen=%d lock_final=%d\n",
          lock_seen ? 1 : 0, lock_rc);
  bool cleanup_ok = cleanup(state);
  return lock_seen && lock_rc == 1 && cleanup_ok ? 0 : 1;
}
