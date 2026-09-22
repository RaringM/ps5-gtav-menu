// Worker-side kernel-log writer (crash-capture sink).
//
// History of two dead ends (both crashed the injected worker on the first log line):
//   1. Reusing the crt's klog_puts -- it snprintf()s through a pointer __klog_init() fills at
//      crt _start, which the injected worker (started as a bare thread at its ELF entry) never
//      runs, so the pointer is NULL.
//   2. A raw `syscall` instruction -- PS5 requires the syscall instruction to originate from
//      inside libkernel (the SDK's __crt_syscall literally jumps to libkernel's syscall site,
//      crt/syscall.c "jump directly to the syscall instruction"). A `syscall` in our injected
//      code is at a non-libkernel address, so the kernel kills the process.
//
// The blessed path for injected code is the libkernel export sceKernelDebugOutText(channel,
// text): it routes through libkernel (valid syscall origin) and is what injected game plugins
// use for debug output (see ref/etaHEN-Plugins Game_Plugin_Loader). It resolves from the same
// libkernel.so stub that gives notify.c its sceKernelSendNotificationRequest -- which already
// works from this worker -- so it links implicitly and is safe to call at init.
//
// Why the kernel log at all: the worker's other sink is the in-memory ring (log_ring.c), which
// lives in GTA's process memory and dies with the process -- it can never hold the breadcrumbs
// immediately before a fatal crash. The kernel log is kernel-side and survives. Stream it from
// a PC with `./menu-ctl.sh logs --kernel` (ps5debug klog forwarder, port 3232).

#include <stddef.h>

#if defined(__PROSPERO__) || defined(__ORBIS__)

// libkernel debug output (NID 9JYNqN6jAKI). Declared locally like notify.c's libkernel import;
// resolves from libkernel.so, which the prospero toolchain links implicitly.
extern int sceKernelDebugOutText(int channel, const char* text);

void gtav_worker_klog(const char* message) {
  if (!message) {
    return;
  }
  // Copy + append a newline so consecutive lines don't run together. Manual (no libc dep);
  // the message already carries the "GTAVMenu" tag that `logs --kernel` filters on.
  char line[512];
  size_t i = 0;
  for (; message[i] != '\0' && i < sizeof(line) - 2; ++i) {
    line[i] = message[i];
  }
  line[i++] = '\n';
  line[i] = '\0';
  (void)sceKernelDebugOutText(0, line);
}

#else  // Host build (static tests, Linux cc): no libkernel -- inert stub.

void gtav_worker_klog(const char* message) {
  (void)message;
}

#endif
