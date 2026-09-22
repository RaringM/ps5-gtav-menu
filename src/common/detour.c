#include "gtavmenu/detour.h"

#include "gtavmenu/log.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

static size_t page_size(void) {
#if GTAV_DETOUR_SCE_MPROTECT
  return 0x4000u;
#else
  long value = sysconf(_SC_PAGESIZE);
  return value > 0 ? (size_t)value : 0x4000u;
#endif
}

// When built as an in-game MODULE (the etaHEN lane), patching the game's .text
// needs Sony's sceKernelMprotect -- libc mprotect does not flip code pages on PS5.
// In-game .text patching must go through sceKernelMprotect for exactly this reason.
// Host/ps5debug builds leave this off and use libc mprotect.
#if GTAV_DETOUR_SCE_MPROTECT
extern int sceKernelMprotect(const void* addr, size_t len, int prot);
#endif

static int protect_range(uintptr_t address, size_t len, int prot) {
  size_t page = page_size();
  uintptr_t start = address & ~(uintptr_t)(page - 1);
  uintptr_t end = (address + len + page - 1) & ~(uintptr_t)(page - 1);
  size_t size = end - start;

#if GTAV_DETOUR_SCE_MPROTECT
  if (sceKernelMprotect((const void*)start, size, prot) == 0) {
    return 0;
  }
  gtav_logf("sceKernelMprotect failed at 0x%llx len=0x%llx prot=0x%x", (unsigned long long)start,
            (unsigned long long)size, prot);
  return -1;
#else
  if (mprotect((void*)start, size, prot) != 0) {
    gtav_logf("mprotect failed at 0x%llx len=0x%llx errno=%d", (unsigned long long)start,
              (unsigned long long)size, errno);
    return -1;
  }
  return 0;
#endif
}

static void dispose_gateway(GtavDetour* detour) {
  if (!detour || !detour->gateway || !detour->gateway_length) return;
  munmap(detour->gateway, detour->gateway_length);
  detour->gateway = NULL;
  detour->gateway_length = 0;
}

typedef struct GtavDetourAtomic16 {
  uint64_t low;
  uint64_t high;
} GtavDetourAtomic16;

static int cmpxchg16b_store(uintptr_t address, const uint8_t* expected, const uint8_t* desired) {
#if defined(__x86_64__) || defined(__amd64__)
  uint64_t expected_low;
  uint64_t expected_high;
  uint64_t desired_low;
  uint64_t desired_high;
  unsigned char ok;

  if (!address || !expected || !desired || (address & 0x0fu)) {
    return -1;
  }

  memcpy(&expected_low, expected, sizeof(expected_low));
  memcpy(&expected_high, expected + sizeof(expected_low), sizeof(expected_high));
  memcpy(&desired_low, desired, sizeof(desired_low));
  memcpy(&desired_high, desired + sizeof(desired_low), sizeof(desired_high));

  __asm__ __volatile__(
      "lock cmpxchg16b %1\n\t"
      "sete %0"
      : "=q"(ok), "+m"(*(volatile GtavDetourAtomic16*)address), "+a"(expected_low),
        "+d"(expected_high)
      : "b"(desired_low), "c"(desired_high)
      : "cc", "memory");

  return ok ? 0 : -1;
#else
  (void)address;
  (void)expected;
  (void)desired;
  return -1;
#endif
}

static int write_jump_patch(uintptr_t address, size_t patch_len, const uint8_t* original,
                            const uint8_t* jump) {
  (void)original;
  if (!address || !jump) {
    return -1;
  }

  // Atomic install whenever the site is 16-byte aligned and the jump fits in the
  // patch window. A single 16-byte cmpxchg16b store replaces the prologue with no
  // tear window, which is mandatory when hooking a hot function the game calls
  // from other threads every frame (a multi-byte memcpy there crashes the moment
  // another thread fetches the half-written bytes). For a patch shorter than 16
  // bytes we preserve the trailing bytes [patch_len, 16) so only the stolen
  // window changes; bytes past the jump are dead (the jump skips them).
  if ((address & 0x0fu) == 0 && patch_len >= GTAV_MENU_DETOUR_JUMP_LEN) {
    const size_t head = patch_len < 16u ? patch_len : 16u;
    uint8_t expected16[16];
    uint8_t desired16[16];
    memcpy(expected16, (const void*)address, sizeof(expected16));
    memcpy(desired16, expected16, sizeof(desired16));
    memcpy(desired16, jump, head);  // jump is NOP-padded; head <= 16 stays in bounds
    if (cmpxchg16b_store(address, expected16, desired16) != 0) {
      gtav_logf("atomic detour install failed at 0x%llx len=%llu", (unsigned long long)address,
                (unsigned long long)patch_len);
      return -1;
    }
    // Bytes [16, patch_len) are dead padding inside the stolen window (the jump
    // skips them); leave them untouched, matching the pre-existing >16 behaviour.
    __builtin___clear_cache((char*)address, (char*)(address + 16u));
    return 0;
  }

  memcpy((void*)address, jump, patch_len);
  __builtin___clear_cache((char*)address, (char*)(address + patch_len));
  return 0;
}

static int write_jump_patch_prevalidated(GtavDetour* detour, const uint8_t* jump) {
  if (!detour || !detour->address || !jump || detour->length != 16u ||
      (detour->address & 0x0fu) != 0) {
    return -1;
  }

  memcpy(detour->patched, jump, detour->length);
  if (cmpxchg16b_store(detour->address, detour->original, detour->patched) != 0) {
    gtav_logf("prevalidated atomic detour install failed at 0x%llx len=%llu",
              (unsigned long long)detour->address, (unsigned long long)detour->length);
    return -1;
  }
  __builtin___clear_cache((char*)detour->address, (char*)(detour->address + detour->length));
  return 0;
}

static int restore_jump_patch(GtavDetour* detour) {
  uint8_t current[16];

  if (!detour || !detour->address || !detour->length) {
    return -1;
  }

  if (detour->prevalidated_no_read) {
    if ((detour->address & 0x0fu) != 0 || detour->length != 16u) {
      return -1;
    }
    if (cmpxchg16b_store(detour->address, detour->patched, detour->original) != 0) {
      gtav_logf("prevalidated atomic detour restore failed at 0x%llx len=%llu",
                (unsigned long long)detour->address, (unsigned long long)detour->length);
      return -1;
    }
    __builtin___clear_cache((char*)detour->address, (char*)(detour->address + detour->length));
    return 0;
  }

  if ((detour->address & 0x0fu) == 0 && detour->length >= GTAV_MENU_DETOUR_JUMP_LEN) {
    if (detour->length > 16u) {
      memcpy((void*)(detour->address + 16u), detour->original + 16u, detour->length - 16u);
      __builtin___clear_cache((char*)(detour->address + 16u),
                              (char*)(detour->address + detour->length));
    }
    // Restore the original head bytes with a single atomic store; trailing bytes
    // [length, 16) of the window were never changed, so carry the current values
    // through unchanged.
    const size_t head = detour->length < 16u ? detour->length : 16u;
    uint8_t desired[16];
    memcpy(current, (const void*)detour->address, sizeof(current));
    memcpy(desired, current, sizeof(desired));
    memcpy(desired, detour->original, head);
    if (memcmp(current, desired, sizeof(current)) == 0) {
      return 0;
    }
    if (cmpxchg16b_store(detour->address, current, desired) != 0) {
      gtav_logf("atomic detour restore failed at 0x%llx len=%llu",
                (unsigned long long)detour->address, (unsigned long long)detour->length);
      return -1;
    }
    __builtin___clear_cache((char*)detour->address, (char*)(detour->address + 16u));
    return 0;
  }

  memcpy((void*)detour->address, detour->original, detour->length);
  __builtin___clear_cache((char*)detour->address, (char*)(detour->address + detour->length));
  return 0;
}

int gtav_detour_validate(uintptr_t address, const uint8_t* expected, size_t expected_len) {
  if (!address) {
    return -1;
  }
  if (!expected_len) {
    return 0;
  }
  if (!expected || expected_len > GTAV_MENU_DETOUR_MAX_LEN) {
    return -1;
  }
  return memcmp((const void*)address, expected, expected_len) == 0 ? 0 : -1;
}

int gtav_detour_encode_abs_jump(uint8_t* out, size_t out_len, void* destination) {
  if (!out || !destination || out_len < GTAV_MENU_DETOUR_JUMP_LEN) {
    return -1;
  }

  memset(out, 0x90, out_len);
  out[0] = 0xFF;
  out[1] = 0x25;
  out[2] = 0x00;
  out[3] = 0x00;
  out[4] = 0x00;
  out[5] = 0x00;
  memcpy(out + 6, &destination, sizeof(destination));
  return 0;
}

static int modrm_is_rip_relative(uint8_t modrm) {
  return (modrm & 0xC7u) == 0x05u;
}

// Minimal length decoder for the subset of x86-64 instructions that legitimately
// appear in a function prologue AND are position-independent (safe to copy into a
// gateway verbatim). Returns the instruction length (>0) for a recognised, safe
// instruction; -2 (and sets *rip_relative) for a RIP-relative instruction; -1 for
// anything unrecognised or position-dependent (relative jmp/call/Jcc, etc.) so the
// caller treats it as unsafe and refuses to steal that window. We deliberately do
// NOT relocate anything: register-relative memory operands are fine because they do
// not depend on the instruction's address, but RIP-relative and relative branches
// would break when moved, so they are rejected rather than fixed up.
static int stolen_instruction_len(const uint8_t* code, size_t remaining, int* rip_relative) {
  if (!code || !remaining || !rip_relative) {
    return -1;
  }
  *rip_relative = 0;

  size_t i = 0;
  // Optional REX prefix (0x40-0x4F).
  if ((code[i] & 0xF0u) == 0x40u) {
    i += 1;
    if (i >= remaining) return -1;
  }

  const uint8_t op = code[i];

  // Single-byte push/pop r64 (0x50-0x5F), with the REX.B above selecting r8-r15.
  if (op >= 0x50u && op <= 0x5Fu) {
    return (int)(i + 1u);
  }

  // Opcodes that take a ModRM byte. imm_bytes is the trailing immediate size.
  size_t imm_bytes = 0;
  switch (op) {
    // mov / lea / common ALU reg<-r/m and r/m<-reg forms, test.
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8B:
    case 0x8D:
    case 0x01:
    case 0x03:
    case 0x09:
    case 0x0B:
    case 0x11:
    case 0x13:
    case 0x19:
    case 0x1B:
    case 0x21:
    case 0x23:
    case 0x29:
    case 0x2B:
    case 0x31:
    case 0x33:
    case 0x39:
    case 0x3B:
    case 0x84:
    case 0x85:
      imm_bytes = 0;
      break;
    case 0x83:
      imm_bytes = 1;
      break;  // grp1 r/m64, imm8 (sub/add/and rsp,imm8 ...)
    case 0x81:
      imm_bytes = 4;
      break;  // grp1 r/m64, imm32
    case 0xC6:
      imm_bytes = 1;
      break;  // mov r/m8, imm8
    case 0xC7:
      imm_bytes = 4;
      break;  // mov r/m32, imm32
    default:
      return -1;  // unknown / position-dependent (E8/E9/EB/70-7F/0F.., ...)
  }

  if (i + 1u >= remaining) return -1;
  const uint8_t modrm = code[i + 1u];
  const uint8_t mod = (uint8_t)(modrm >> 6);
  const uint8_t rm = (uint8_t)(modrm & 0x07u);
  size_t len = i + 2u;  // through the ModRM byte

  if (modrm_is_rip_relative(modrm)) {
    *rip_relative = 1;
    return -2;
  }

  // SIB byte (rm == 100b) for memory forms (mod != 11b).
  if (mod != 3u && rm == 4u) {
    if (len >= remaining) return -1;
    const uint8_t sib = code[len];
    len += 1u;
    // mod==00 with SIB base==101b encodes a disp32 with no base register.
    if (mod == 0u && (sib & 0x07u) == 5u) {
      len += 4u;
    }
  }

  // Displacement from the ModRM mod field.
  if (mod == 1u) {
    len += 1u;
  } else if (mod == 2u) {
    len += 4u;
  }

  len += imm_bytes;
  if (len > remaining) return -1;
  return (int)len;
}

int gtav_detour_stolen_window_is_safe(const uint8_t* code, size_t len) {
  size_t offset = 0;

  if (!code || len < GTAV_MENU_DETOUR_JUMP_LEN || len > GTAV_MENU_DETOUR_MAX_LEN) {
    return -1;
  }

  while (offset < len) {
    int rip_relative = 0;
    int instruction_len = stolen_instruction_len(code + offset, len - offset, &rip_relative);
    if (rip_relative || instruction_len <= 0 || offset + (size_t)instruction_len > len) {
      return -1;
    }
    offset += (size_t)instruction_len;
  }

  return offset == len ? 0 : -1;
}

int gtav_detour_safe_patch_len(const uint8_t* code, size_t max_len) {
  // Walk whole instructions until the accumulated length covers a 14-byte
  // absolute jump, then return that length so the caller can steal exactly that
  // many bytes. Returns -1 if an unsafe / RIP-relative / unrecognised instruction
  // is hit before reaching the jump length, or the prologue is too short.
  if (!code) return -1;
  if (max_len > GTAV_MENU_DETOUR_MAX_LEN) {
    max_len = GTAV_MENU_DETOUR_MAX_LEN;
  }
  size_t offset = 0;
  while (offset < GTAV_MENU_DETOUR_JUMP_LEN) {
    int rip_relative = 0;
    int instruction_len = stolen_instruction_len(code + offset, max_len - offset, &rip_relative);
    if (rip_relative || instruction_len <= 0) {
      return -1;
    }
    offset += (size_t)instruction_len;
    if (offset > max_len) {
      return -1;
    }
  }
  return (int)offset;
}

int gtav_detour_build_gateway(uint8_t* out, size_t out_len, uintptr_t address,
                              const uint8_t* stolen, size_t stolen_len) {
  void* return_address = (void*)(address + stolen_len);

  if (!out || !address || !stolen || stolen_len > GTAV_MENU_DETOUR_MAX_LEN) {
    return -1;
  }
  if (out_len < stolen_len + GTAV_MENU_DETOUR_JUMP_LEN) {
    return -1;
  }
  if (gtav_detour_stolen_window_is_safe(stolen, stolen_len) != 0) {
    return -1;
  }

  memcpy(out, stolen, stolen_len);
  return gtav_detour_encode_abs_jump(out + stolen_len, out_len - stolen_len, return_address);
}

int gtav_detour_install_abs_jump(GtavDetour* detour, uintptr_t address, void* destination,
                                 size_t patch_len, const uint8_t* expected, size_t expected_len,
                                 int dry_run) {
  uint8_t jump[GTAV_MENU_DETOUR_MAX_LEN];

  if (!detour || !address || !destination) return -1;
  if (patch_len < GTAV_MENU_DETOUR_JUMP_LEN || patch_len > GTAV_MENU_DETOUR_MAX_LEN) return -1;
  if (expected_len && expected_len > patch_len) return -1;

  memset(detour, 0, sizeof(*detour));

  if (gtav_detour_validate(address, expected, expected_len) != 0) {
    gtav_logf("detour validation failed at 0x%llx", (unsigned long long)address);
    return -1;
  }

  memcpy(detour->original, (const void*)address, patch_len);
  detour->address = address;
  detour->length = patch_len;

  if (dry_run) {
    gtav_logf("detour dry-run passed at 0x%llx len=%llu dest=%p", (unsigned long long)address,
              (unsigned long long)patch_len, destination);
    return 0;
  }

  if (gtav_detour_encode_abs_jump(jump, patch_len, destination) != 0) {
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return -1;
  }

  if (write_jump_patch(address, patch_len, detour->original, jump) != 0) {
    protect_range(address, patch_len, PROT_READ | PROT_EXEC);
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_EXEC) != 0) {
    return -1;
  }

  detour->installed = 1;
  gtav_logf("detour installed at 0x%llx len=%llu dest=%p", (unsigned long long)address,
            (unsigned long long)patch_len, destination);
  return 0;
}

int gtav_detour_install_abs_jump_prevalidated(GtavDetour* detour, uintptr_t address,
                                              void* destination, size_t patch_len,
                                              const uint8_t* original, size_t original_len,
                                              int dry_run) {
  uint8_t jump[GTAV_MENU_DETOUR_MAX_LEN];

  if (!detour || !address || !destination || !original) return -1;
  if (patch_len != 16u || patch_len > GTAV_MENU_DETOUR_MAX_LEN) return -1;
  if ((address & 0x0fu) != 0) return -1;
  if (original_len < patch_len) return -1;

  memset(detour, 0, sizeof(*detour));
  memcpy(detour->original, original, patch_len);
  detour->address = address;
  detour->length = patch_len;
  detour->prevalidated_no_read = 1;

  if (dry_run) {
    gtav_logf("prevalidated detour dry-run passed at 0x%llx len=%llu dest=%p",
              (unsigned long long)address, (unsigned long long)patch_len, destination);
    return 0;
  }

  if (gtav_detour_encode_abs_jump(jump, patch_len, destination) != 0) {
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return -1;
  }

  if (write_jump_patch_prevalidated(detour, jump) != 0) {
    protect_range(address, patch_len, PROT_READ | PROT_EXEC);
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_EXEC) != 0) {
    return -1;
  }

  detour->installed = 1;
  gtav_logf("prevalidated detour installed at 0x%llx len=%llu dest=%p", (unsigned long long)address,
            (unsigned long long)patch_len, destination);
  return 0;
}

int gtav_detour_install_trampoline(GtavDetour* detour, uintptr_t address, void* destination,
                                   size_t patch_len, const uint8_t* expected, size_t expected_len,
                                   void** original_out) {
  uint8_t jump[GTAV_MENU_DETOUR_MAX_LEN];
  size_t gateway_len = patch_len + GTAV_MENU_DETOUR_JUMP_LEN;
  void* gateway;

  if (original_out) {
    *original_out = NULL;
  }
  if (!detour || !address || !destination) return -1;
  if (patch_len < GTAV_MENU_DETOUR_JUMP_LEN || patch_len > GTAV_MENU_DETOUR_MAX_LEN) return -1;
  if (expected_len && expected_len > patch_len) return -1;

  memset(detour, 0, sizeof(*detour));

  if (gtav_detour_validate(address, expected, expected_len) != 0) {
    gtav_logf("trampoline validation failed at 0x%llx", (unsigned long long)address);
    return -1;
  }

  memcpy(detour->original, (const void*)address, patch_len);
  detour->address = address;
  detour->length = patch_len;

  if (gtav_detour_stolen_window_is_safe(detour->original, patch_len) != 0) {
    gtav_logf("trampoline stolen window unsafe at 0x%llx len=%llu", (unsigned long long)address,
              (unsigned long long)patch_len);
    return -1;
  }

  gateway = mmap(NULL, gateway_len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (gateway == MAP_FAILED) {
    gtav_logf("trampoline gateway mmap failed len=%llu errno=%d", (unsigned long long)gateway_len,
              errno);
    return -1;
  }

  if (gtav_detour_build_gateway((uint8_t*)gateway, gateway_len, address, detour->original,
                                patch_len) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }
  __builtin___clear_cache((char*)gateway, (char*)gateway + gateway_len);
  if (protect_range((uintptr_t)gateway, gateway_len, PROT_READ | PROT_EXEC) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }

  if (gtav_detour_encode_abs_jump(jump, patch_len, destination) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }

  if (write_jump_patch(address, patch_len, detour->original, jump) != 0) {
    protect_range(address, patch_len, PROT_READ | PROT_EXEC);
    munmap(gateway, gateway_len);
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_EXEC) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }

  detour->gateway = gateway;
  detour->gateway_length = gateway_len;
  detour->installed = 1;
  if (original_out) {
    *original_out = gateway;
  }
  gtav_logf("trampoline detour installed at 0x%llx len=%llu dest=%p gateway=%p",
            (unsigned long long)address, (unsigned long long)patch_len, destination, gateway);
  return 0;
}

static void publish_original_gateway(GtavDetourPublishOriginalFn publish_original,
                                     void* publish_context, void** original_out, void* gateway) {
  if (original_out) {
    *original_out = gateway;
  }
  if (publish_original) {
    publish_original(gateway, publish_context);
  }
}

int gtav_detour_prepare_trampoline_prevalidated(GtavDetour* detour, uintptr_t address,
                                                size_t patch_len, const uint8_t* original,
                                                size_t original_len, void** original_out) {
  size_t gateway_len = patch_len + GTAV_MENU_DETOUR_JUMP_LEN;
  void* gateway;

  if (original_out) {
    *original_out = NULL;
  }
  if (!detour || !address || !original) return -1;
  if (patch_len < GTAV_MENU_DETOUR_JUMP_LEN || patch_len > GTAV_MENU_DETOUR_MAX_LEN) return -1;
  if (original_len < patch_len) return -1;

  memset(detour, 0, sizeof(*detour));
  memcpy(detour->original, original, patch_len);
  detour->address = address;
  detour->length = patch_len;

  if (gtav_detour_stolen_window_is_safe(detour->original, patch_len) != 0) {
    gtav_logf("prevalidated trampoline stolen window unsafe at 0x%llx len=%llu",
              (unsigned long long)address, (unsigned long long)patch_len);
    return -1;
  }

  gateway = mmap(NULL, gateway_len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);
  if (gateway == MAP_FAILED) {
    gtav_logf("prevalidated trampoline gateway mmap failed len=%llu errno=%d",
              (unsigned long long)gateway_len, errno);
    return -1;
  }

  if (gtav_detour_build_gateway((uint8_t*)gateway, gateway_len, address, detour->original,
                                patch_len) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }
  __builtin___clear_cache((char*)gateway, (char*)gateway + gateway_len);
  if (protect_range((uintptr_t)gateway, gateway_len, PROT_READ | PROT_EXEC) != 0) {
    munmap(gateway, gateway_len);
    return -1;
  }

  detour->gateway = gateway;
  detour->gateway_length = gateway_len;
  if (original_out) {
    *original_out = gateway;
  }
  gtav_logf("prevalidated trampoline gateway prepared at 0x%llx len=%llu gateway=%p",
            (unsigned long long)address, (unsigned long long)patch_len, gateway);
  return 0;
}

int gtav_detour_probe_patch_protection(uintptr_t address, size_t patch_len) {
  if (!address) return -1;
  if (patch_len < GTAV_MENU_DETOUR_JUMP_LEN || patch_len > GTAV_MENU_DETOUR_MAX_LEN) return -1;

  if (protect_range(address, patch_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return -1;
  }
  if (protect_range(address, patch_len, PROT_READ | PROT_EXEC) != 0) {
    return -1;
  }

  gtav_logf("patch protection probe passed at 0x%llx len=%llu", (unsigned long long)address,
            (unsigned long long)patch_len);
  return 0;
}

static int gtav_detour_install_trampoline_prevalidated_impl(
    GtavDetour* detour, uintptr_t address, void* destination, size_t patch_len,
    const uint8_t* original, size_t original_len, GtavDetourPublishOriginalFn publish_original,
    void* publish_context, void** original_out) {
  uint8_t jump[GTAV_MENU_DETOUR_MAX_LEN];
  int published = 0;

  if (original_out) {
    *original_out = NULL;
  }
  if (!destination) return -1;
  if (gtav_detour_prepare_trampoline_prevalidated(detour, address, patch_len, original,
                                                  original_len, original_out) != 0) {
    return -1;
  }
  if (gtav_detour_encode_abs_jump(jump, patch_len, destination) != 0) {
    dispose_gateway(detour);
    return -1;
  }

  if (publish_original) {
    publish_original_gateway(publish_original, publish_context, original_out, detour->gateway);
    published = 1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    if (published) {
      publish_original_gateway(publish_original, publish_context, original_out, NULL);
    }
    dispose_gateway(detour);
    return -1;
  }

  if (write_jump_patch(address, patch_len, detour->original, jump) != 0) {
    if (published) {
      publish_original_gateway(publish_original, publish_context, original_out, NULL);
    }
    protect_range(address, patch_len, PROT_READ | PROT_EXEC);
    dispose_gateway(detour);
    return -1;
  }

  if (protect_range(address, patch_len, PROT_READ | PROT_EXEC) != 0) {
    gtav_logf("prevalidated trampoline installed but restore protection failed at 0x%llx len=%llu",
              (unsigned long long)address, (unsigned long long)patch_len);
  }

  detour->installed = 1;
  if (!published && original_out) {
    *original_out = detour->gateway;
  }
  gtav_logf("prevalidated trampoline detour installed at 0x%llx len=%llu dest=%p gateway=%p",
            (unsigned long long)address, (unsigned long long)patch_len, destination,
            detour->gateway);
  return 0;
}

int gtav_detour_install_trampoline_prevalidated(GtavDetour* detour, uintptr_t address,
                                                void* destination, size_t patch_len,
                                                const uint8_t* original, size_t original_len,
                                                void** original_out) {
  return gtav_detour_install_trampoline_prevalidated_impl(
      detour, address, destination, patch_len, original, original_len, NULL, NULL, original_out);
}

int gtav_detour_install_trampoline_prevalidated_publish(
    GtavDetour* detour, uintptr_t address, void* destination, size_t patch_len,
    const uint8_t* original, size_t original_len, GtavDetourPublishOriginalFn publish_original,
    void* publish_context, void** original_out) {
  return gtav_detour_install_trampoline_prevalidated_impl(detour, address, destination, patch_len,
                                                          original, original_len, publish_original,
                                                          publish_context, original_out);
}

int gtav_detour_restore(GtavDetour* detour) {
  if (!detour || !detour->installed || !detour->address || !detour->length) {
    dispose_gateway(detour);
    return 0;
  }

  if (protect_range(detour->address, detour->length, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return -1;
  }

  if (restore_jump_patch(detour) != 0) {
    protect_range(detour->address, detour->length, PROT_READ | PROT_EXEC);
    return -1;
  }

  if (protect_range(detour->address, detour->length, PROT_READ | PROT_EXEC) != 0) {
    return -1;
  }

  gtav_logf("detour restored at 0x%llx", (unsigned long long)detour->address);
  detour->installed = 0;
  dispose_gateway(detour);
  return 0;
}
