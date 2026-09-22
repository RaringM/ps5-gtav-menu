#include "gtavmenu/spawn_module_loader.h"

#include "gtavmenu/status.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// Kernel R/W (ucred authority/caps) for the privileged sceKernelLoadStartModule
// path. Guarded so host unit-test compiles (which build with the system cc) skip
// the PS5-only header; the PS5 payload build always has it.
#if !defined(GTAV_MENU_NO_ROOTDIR) && defined(__has_include) && __has_include(<ps5/kernel.h>)
#include <ps5/kernel.h>
#define GTAV_MENU_HAVE_PS5_KERNEL 1
#endif

extern int sceKernelLoadStartModule(const char* name, unsigned long argc, const void* argv,
                                    unsigned int flags, const void* opt, int* res);
#ifndef GTAV_MENU_SPAWN_MODULE_PATH
// /data root is the proven-resolvable load location (mirrors the reference
// /data/shell.prx): open() succeeds there with our SELF magic, so the remaining
// failure is the fake-self signature gate, not path resolution.
#define GTAV_MENU_SPAWN_MODULE_PATH "/data/gtav-menu-game.prx"
#endif
// DIAGNOSTIC ONLY. gtav_menu_load_spawn_module is a retained in-process probe of the
// privileged sceKernelLoadStartModule path -- it is NOT the shipping load lane. The
// injector restores GTA's ucred after injection, so this in-process attempt is expected
// to fail the fake-self signature gate (EACCES); the external ps5debug client owns the
// reliable elevation window. Kept only for diagnostic parity: both definitions below just
// emit status events and return the literal "load_module".
#ifndef GTAV_MENU_HAVE_PS5_KERNEL
// Host build (no PS5 kernel R/W): stub so the module compiles for unit tests.
const char* gtav_menu_load_spawn_module(const char* source) {
  (void)source;
  return "load_module";
}
#else
const char* gtav_menu_load_spawn_module(const char* source) {
  // The injector elevates ucred caps only DURING injection then restores them, so GTA now
  // has normal authority and sceKernelLoadStartModule rejects fake-signed .prx files with
  // EACCES (0x8002000d). The external ps5debug client now owns the reliable elevation
  // window; this in-process attempt is retained only as diagnostic parity for setups where
  // payload kernel writes are writable.
  pid_t pid = getpid();
  uint64_t saved_authid = kernel_get_ucred_authid(pid);
  // ucred attrs/caps are fixed-width kernel buffers, not scalars (see ps5/kernel.h). Snapshot
  // them so they can be restored after the diagnostic elevation; fold the low 8 bytes of attrs
  // into a uint64 only for the compact status print.
  uint8_t saved_attrs[32];
  int have_attrs = (kernel_get_ucred_attrs(pid, saved_attrs) == 0);
  uint8_t saved_caps[16];
  int have_caps = (kernel_get_ucred_caps(pid, saved_caps) == 0);
  uint8_t all_caps[16];
  memset(all_caps, 0xFF, sizeof(all_caps));

  // w = the write return code: 0 = copyin reported success (=> pipe-flag/no-effect bug if
  // el unchanged), -1 = copyin errored. Distinguishes the two write-failure modes.
  int w = kernel_set_ucred_authid(pid, 0x4801000000000013ULL);
  int wa = -1;
  if (have_attrs) {
    uint8_t el_attrs_set[32];
    memcpy(el_attrs_set, saved_attrs, sizeof(el_attrs_set));
    el_attrs_set[0] |= 0x80;
    wa = kernel_set_ucred_attrs(pid, el_attrs_set);
  }
  if (have_caps) kernel_set_ucred_caps(pid, all_caps);
  // Read back DURING elevation to prove what authority the kernel saw at load time.
  uint64_t el_authid = kernel_get_ucred_authid(pid);
  uint8_t el_attrs_buf[32];
  uint64_t el_attrs = 0;
  if (kernel_get_ucred_attrs(pid, el_attrs_buf) == 0)
    memcpy(&el_attrs, el_attrs_buf, sizeof(el_attrs));

  // Diagnostic: can GTA's (externally elevated + escaped) context even open() the
  // module path? This splits the ENOENT failure: if open() ALSO ENOENTs, the file
  // is not resolvable from our filedesc/location (escape not live / wrong path); if
  // open() SUCCEEDS but LoadStartModule still ENOENTs, the module loader uses a
  // different path root (sandbox prefix). Reports fd, errno, and the first 4 magic
  // bytes (expect 0x1d3d154f for a SELF) so we also confirm we opened the right file.
  int ofd = open(GTAV_MENU_SPAWN_MODULE_PATH, O_RDONLY);
  int oerr = ofd < 0 ? errno : 0;
  unsigned int omagic = 0;
  if (ofd >= 0) {
    if (read(ofd, &omagic, sizeof(omagic)) != (ssize_t)sizeof(omagic)) omagic = 0;
    close(ofd);
  }
  gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "LSMo fd=%d e=%d m=0x%x %s", ofd, oerr, omagic,
                     GTAV_MENU_SPAWN_MODULE_PATH);

  int res = -1;
  int handle = sceKernelLoadStartModule(GTAV_MENU_SPAWN_MODULE_PATH, 0, 0, 0, 0, &res);

  if (have_caps) kernel_set_ucred_caps(pid, saved_caps);
  if (have_attrs) kernel_set_ucred_attrs(pid, saved_attrs);
  kernel_set_ucred_authid(pid, saved_authid);

  // Compact to fit GTAV_MENU_STATUS_EVENT_LEN (80).
  gtav_status_eventf(GTAV_MENU_EVENT_COMMAND, "LSM h=0x%x w=%d wa=%d el=0x%llx at=0x%llx", handle,
                     w, wa, (unsigned long long)el_authid, (unsigned long long)el_attrs);
  (void)source;
  return "load_module";
}
#endif  // GTAV_MENU_HAVE_PS5_KERNEL
