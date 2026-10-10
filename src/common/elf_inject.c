// SDK-native ELF injector. See include/gtavmenu/elf_inject.h.
//
// Depends only on the process-control backend (proc_backend.h); it never includes a
// PS5 SDK header, so the host test drives it with a stub backend over a fake target
// buffer and the real feature-menu ELF. The parsing/relocation math mirrors the
// host-side ELF map plan tool used during development.

#include "gtavmenu/elf_inject.h"

#include "gtavmenu/log.h"
#include "gtavmenu/proc_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// A freshly-mapped target page may not be resident the instant we write it, so a
// cross-process write can transiently EFAULT on PS5 (seen during relocation/segment
// writes into the alloc_exec'd image). Fault the page in with a read and retry a few
// times before giving up -- mirrors the loader's broker-write retry.
static int inject_instance_matches(int pid, uint64_t expected_app_id) {
  uint64_t current;
  if (expected_app_id == 0) {
    return 1;  // explicit guard-off mode for the host harness / generic standalone callers
  }
  current = gtav_proc_app_id(pid);
  if (current == 0 || current != expected_app_id) {
    gtav_logf("elf_inject: app instance changed/unknown pid=%d token=0x%llx->0x%llx", pid,
              (unsigned long long)expected_app_id, (unsigned long long)current);
    return 0;
  }
  return 1;
}

// Read a write back before believing it. Both transfer paths can report success without having
// changed anything: the debug-memory service does exactly that for memory with no backing, which
// cost a wrong conclusion once (docs/ptrace-free-injection.md). Verifying doubles the traffic, so
// it is a build gate rather than the default -- the cave lane, which writes a whole image into
// pages the game has never touched, turns it on. Compares in bounded chunks so the stack cost is
// fixed regardless of how large a segment write is.
#ifndef GTAV_ELF_INJECT_VERIFY_WRITES
#define GTAV_ELF_INJECT_VERIFY_WRITES 0
#endif

#if GTAV_ELF_INJECT_VERIFY_WRITES
static int inject_write_readback_matches(int pid, uintptr_t addr, const void* buf, size_t n) {
  const uint8_t* want = (const uint8_t*)buf;
  size_t done = 0;
  while (done < n) {
    uint8_t check[256];
    size_t chunk = n - done;
    if (chunk > sizeof(check)) {
      chunk = sizeof(check);
    }
    memset(check, 0, sizeof(check));
    if (gtav_proc_read(pid, addr + done, check, chunk) != 0) {
      gtav_logf("elf_inject: readback FAILED at 0x%llx (+%zu)", (unsigned long long)addr, done);
      return 0;
    }
    if (memcmp(check, want + done, chunk) != 0) {
      gtav_logf("elf_inject: readback MISMATCH at 0x%llx (+%zu); the write did not stick",
                (unsigned long long)addr, done);
      return 0;
    }
    done += chunk;
  }
  return 1;
}
#endif

static int inject_write(int pid, uint64_t expected_app_id, uintptr_t addr, const void* buf,
                        size_t n) {
  int attempt;
  for (attempt = 0; attempt < 6; ++attempt) {
    uint8_t fault[8];
    size_t fn;
    if (!inject_instance_matches(pid, expected_app_id)) return -1;
    if (gtav_proc_write(pid, addr, buf, n) == 0) {
#if GTAV_ELF_INJECT_VERIFY_WRITES
      if (!inject_instance_matches(pid, expected_app_id)) return -1;
      if (inject_write_readback_matches(pid, addr, buf, n)) {
        return 0;
      }
#else
      return 0;
#endif
    }
    fn = n < sizeof(fault) ? n : sizeof(fault);
    if (!inject_instance_matches(pid, expected_app_id)) return -1;
    gtav_proc_read(pid, addr, fault, fn);
    usleep(100000);
  }
  return -1;
}

#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PF_X 0x1u

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_JMPREL 23

#define SHT_SYMTAB 2
#define SHT_DYNSYM 11
#define SHN_UNDEF 0

#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define ELF_PAGE 0x4000ULL
#define MAX_NEEDED 16

static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t* p) {
  uint64_t v = 0;
  int i;
  for (i = 0; i < 8; ++i) {
    v |= (uint64_t)p[i] << (8 * i);
  }
  return v;
}

// off + n fits within [0, len].
static int in_range(uint64_t off, uint64_t n, size_t len) {
  return off <= len && n <= (uint64_t)len - off;
}

static uint64_t round_down(uint64_t v, uint64_t a) {
  return v & ~(a - 1);
}

static uint64_t round_up(uint64_t v, uint64_t a) {
  return (v + a - 1) & ~(a - 1);
}

// Map a vaddr (a dynamic-section pointer) to its file offset via the PT_LOAD list.
static int vaddr_to_off(const uint8_t* elf, size_t len, uint64_t phoff, int phnum, int phentsize,
                        uint64_t vaddr, uint64_t* off_out) {
  int i;
  for (i = 0; i < phnum; ++i) {
    uint64_t ph = phoff + (uint64_t)i * (uint64_t)phentsize;
    uint64_t p_off;
    uint64_t p_vaddr;
    uint64_t p_filesz;
    if (!in_range(ph, 56, len)) {
      return -1;
    }
    if (rd32(elf + ph) != PT_LOAD) {
      continue;
    }
    p_off = rd64(elf + ph + 8);
    p_vaddr = rd64(elf + ph + 16);
    p_filesz = rd64(elf + ph + 32);
    if (vaddr >= p_vaddr && vaddr < p_vaddr + p_filesz) {
      *off_out = p_off + (vaddr - p_vaddr);
      return 0;
    }
  }
  return -1;
}

// A C string fully contained in the buffer and NUL-terminated before len.
static int cstr_ok(const uint8_t* elf, size_t len, uint64_t off) {
  uint64_t i;
  if (off >= len) {
    return 0;
  }
  for (i = off; i < len; ++i) {
    if (elf[i] == 0) {
      return 1;
    }
  }
  return 0;
}

#define FAILR(stage_no, ...)                               \
  do {                                                     \
    out->status = (stage_no);                              \
    snprintf(out->error, sizeof(out->error), __VA_ARGS__); \
    out->error[sizeof(out->error) - 1] = '\0';             \
    gtav_logf("elf_inject: %s", out->error);               \
    return -1;                                             \
  } while (0)

// Apply one DT_RELA/DT_JMPREL relocation table. load_bias-relative writes go through
// the backend; import symbols are resolved by name against the needed libraries.
static int apply_relocs(int pid, uint64_t expected_app_id, const uint8_t* elf, size_t len,
                        uint64_t load_bias, uint64_t table_off, uint64_t table_sz, uint64_t ent,
                        uint64_t symtab_off, uint64_t syment, uint64_t strtab_off, uint64_t strsz,
                        const char* const* needed, int nneeded, GtavElfInjectResult* out) {
  uint64_t count;
  uint64_t i;
  if (table_off == 0 || table_sz == 0) {
    return 0;
  }
  if (ent < 24 || !in_range(table_off, table_sz, len)) {
    FAILR(40, "reloc table out of range off=0x%llx sz=0x%llx", (unsigned long long)table_off,
          (unsigned long long)table_sz);
  }
  count = table_sz / ent;
  for (i = 0; i < count; ++i) {
    const uint8_t* r = elf + table_off + i * ent;
    uint64_t r_offset = rd64(r);
    uint64_t r_info = rd64(r + 8);
    int64_t r_addend = (int64_t)rd64(r + 16);
    uint32_t type = (uint32_t)(r_info & 0xffffffffu);
    uint32_t symidx = (uint32_t)(r_info >> 32);
    uint64_t value;

    if (type == R_X86_64_RELATIVE) {
      value = load_bias + (uint64_t)r_addend;
      if (inject_write(pid, expected_app_id, load_bias + r_offset, &value, sizeof(value)) != 0) {
        FAILR(41, "RELATIVE write failed at 0x%llx", (unsigned long long)(load_bias + r_offset));
      }
      out->relocs_applied++;
      continue;
    }

    if (symidx == 0 ||
        (type != R_X86_64_64 && type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT)) {
      continue;  // not a kind we handle (and the menu ELF only emits these + RELATIVE)
    }

    {
      uint64_t sym = symtab_off + (uint64_t)symidx * syment;
      uint32_t st_name;
      uint16_t st_shndx;
      uint64_t st_value;
      if (!in_range(sym, 24, len)) {
        FAILR(42, "symbol %u out of range", symidx);
      }
      st_name = rd32(elf + sym);
      st_shndx = rd16(elf + sym + 6);
      st_value = rd64(elf + sym + 8);

      if (st_shndx == SHN_UNDEF) {
        uintptr_t resolved = 0;
        const char* name;
        if (strtab_off == 0 || !cstr_ok(elf, len, strtab_off + st_name)) {
          FAILR(43, "import name out of range (sym %u)", symidx);
        }
        name = (const char*)(elf + strtab_off + st_name);
        if (!inject_instance_matches(pid, expected_app_id)) {
          FAILR(46, "app instance changed before resolving import: %s", name);
        }
        if (gtav_proc_resolve_named(pid, needed, nneeded, name, &resolved) != 0) {
          FAILR(44, "unresolved import: %s", name);
        }
        value = (uint64_t)resolved + (type == R_X86_64_64 ? (uint64_t)r_addend : 0);
        out->imports_resolved++;
      } else {
        value = load_bias + st_value + (type == R_X86_64_64 ? (uint64_t)r_addend : 0);
        out->relocs_applied++;
      }
      if (inject_write(pid, expected_app_id, load_bias + r_offset, &value, sizeof(value)) != 0) {
        FAILR(45, "symbol reloc write failed at 0x%llx",
              (unsigned long long)(load_bias + r_offset));
      }
    }
    (void)strsz;
  }
  return 0;
}

// Scan .dynsym for a defined symbol by name, writing its st_value. Returns 0 on a
// hit, -1 if absent (no logging -- callers decide whether a miss is fatal).
static int scan_dynsym(const uint8_t* elf, size_t len, uint64_t shoff, int shnum, int shentsize,
                       const char* name, uint64_t* value_out) {
  int i;
  if (shoff == 0 || shnum == 0) {
    return -1;
  }
  for (i = 0; i < shnum; ++i) {
    uint64_t sh = shoff + (uint64_t)i * (uint64_t)shentsize;
    uint32_t sh_type;
    uint64_t sh_off;
    uint64_t sh_size;
    uint64_t sh_ent;
    uint32_t sh_link;
    uint64_t str_off;
    uint64_t str_size;
    uint64_t scount;
    uint64_t s;
    if (!in_range(sh, 64, len)) {
      continue;
    }
    sh_type = rd32(elf + sh + 4);
    if (sh_type != SHT_DYNSYM && sh_type != SHT_SYMTAB) {
      continue;
    }
    sh_off = rd64(elf + sh + 24);
    sh_size = rd64(elf + sh + 32);
    sh_link = rd32(elf + sh + 40);
    sh_ent = rd64(elf + sh + 56);
    if (sh_ent < 24 || sh_size == 0 || (uint64_t)sh_link >= (uint64_t)shnum) {
      continue;
    }
    {
      uint64_t strsh = shoff + (uint64_t)sh_link * (uint64_t)shentsize;
      if (!in_range(strsh, 64, len)) {
        continue;
      }
      str_off = rd64(elf + strsh + 24);
      str_size = rd64(elf + strsh + 32);
    }
    if (!in_range(sh_off, sh_size, len)) {
      continue;
    }
    scount = sh_size / sh_ent;
    for (s = 0; s < scount; ++s) {
      const uint8_t* sym = elf + sh_off + s * sh_ent;
      uint32_t st_name = rd32(sym);
      uint16_t st_shndx = rd16(sym + 6);
      uint64_t st_value = rd64(sym + 8);
      const char* sname;
      if (st_shndx == SHN_UNDEF || st_name == 0) {
        continue;
      }
      if (!cstr_ok(elf, len, str_off + st_name) || str_off + st_name >= str_off + str_size) {
        continue;
      }
      sname = (const char*)(elf + str_off + st_name);
      if (strcmp(sname, name) == 0) {
        *value_out = st_value;
        return 0;
      }
    }
  }
  return -1;
}

// Resolve an exported symbol's vaddr from the .dynsym section (its st_value).
static int find_export(const uint8_t* elf, size_t len, uint64_t shoff, int shnum, int shentsize,
                       const char* name, uint64_t* value_out, GtavElfInjectResult* out) {
  if (scan_dynsym(elf, len, shoff, shnum, shentsize, name, value_out) != 0) {
    FAILR(51, "export %s not found in .dynsym", name);
  }
  return 0;
}

int gtav_elf_symbol_value(const uint8_t* elf, size_t len, const char* name, uint64_t* value_out) {
  if (elf == NULL || name == NULL || value_out == NULL || len < 0x40) {
    return -1;
  }
  return scan_dynsym(elf, len, rd64(elf + 0x28), rd16(elf + 0x3c), rd16(elf + 0x3a), name,
                     value_out);
}

// Parsed ELF header geometry (filled by parse_elf_headers).
typedef struct {
  uint64_t phoff;
  uint64_t shoff;
  int phnum;
  int phentsize;
  int shnum;
  int shentsize;
  uint64_t image_min;
  uint64_t image_max;
  uint64_t dyn_off;
  uint64_t dyn_sz;
} ElfImage;

// Dynamic-section tables resolved to file offsets (filled by parse_dynamic).
typedef struct {
  uint64_t strtab_off;
  uint64_t strsz;
  uint64_t symtab_off;
  uint64_t syment;
  uint64_t rela_off;
  uint64_t relasz;
  uint64_t relaent;
  uint64_t jmprel_off;
  uint64_t pltrelsz;
  const char* needed[MAX_NEEDED];
  int nneeded;
} ElfDynamic;

// Validate the ELF header and scan the program headers (pass 1) for the loadable
// image extent (page-rounded image_min..image_max) and the PT_DYNAMIC location.
static int parse_elf_headers(const uint8_t* elf, size_t len, ElfImage* img,
                             GtavElfInjectResult* out) {
  int i;
  if (len < 0x40 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') {
    FAILR(2, "not an ELF");
  }
  if (elf[4] != 2 || elf[5] != 1) {
    FAILR(3, "not little-endian ELF64");
  }
  if (rd16(elf + 0x12) != 62) {
    FAILR(4, "not x86-64");
  }

  memset(img, 0, sizeof(*img));
  img->phoff = rd64(elf + 0x20);
  img->shoff = rd64(elf + 0x28);
  img->phentsize = rd16(elf + 0x36);
  img->phnum = rd16(elf + 0x38);
  img->shentsize = rd16(elf + 0x3a);
  img->shnum = rd16(elf + 0x3c);
  img->image_min = ~0ULL;
  if (img->phentsize < 56) {
    FAILR(5, "bad phentsize %d", img->phentsize);
  }

  for (i = 0; i < img->phnum; ++i) {
    uint64_t ph = img->phoff + (uint64_t)i * (uint64_t)img->phentsize;
    uint32_t p_type;
    uint64_t p_vaddr;
    uint64_t p_memsz;
    if (!in_range(ph, 56, len)) {
      FAILR(6, "phdr %d out of range", i);
    }
    p_type = rd32(elf + ph);
    p_vaddr = rd64(elf + ph + 16);
    p_memsz = rd64(elf + ph + 40);
    if (p_type == PT_LOAD && p_memsz > 0) {
      uint64_t lo = round_down(p_vaddr, ELF_PAGE);
      uint64_t hi = round_up(p_vaddr + p_memsz, ELF_PAGE);
      if (lo < img->image_min) {
        img->image_min = lo;
      }
      if (hi > img->image_max) {
        img->image_max = hi;
      }
    } else if (p_type == PT_DYNAMIC) {
      img->dyn_off = rd64(elf + ph + 8);
      img->dyn_sz = rd64(elf + ph + 32);
    }
  }
  if (img->image_max <= img->image_min) {
    FAILR(7, "no loadable segments");
  }
  out->image_size = (uint32_t)(img->image_max - img->image_min);
  return 0;
}

// Reserve the executable image in the target and copy PT_LOAD file contents (pass 2;
// BSS stays zero from the anonymous mapping). Returns the base + load_bias.
static int map_segments(int pid, uint64_t expected_app_id, const uint8_t* elf, size_t len,
                        const ElfImage* img, int stop_after_alloc, uintptr_t preallocated,
                        uint64_t reserved, uintptr_t* base_out, uint64_t* load_bias_out,
                        GtavElfInjectResult* out) {
  uintptr_t base = 0;
  uint64_t load_bias;
  int allocation_may_exist = 0;
  int alloc_rc;
  int i;

  if (!inject_instance_matches(pid, expected_app_id)) {
    FAILR(9, "app instance changed before alloc_exec");
  }
  if (preallocated != 0) {
    // The caller already owns memory in the target -- on this firmware that comes from the
    // ptrace-free cave bootstrap, because gtav_proc_alloc_exec must PT_ATTACH and that permanently
    // breaks the game's streaming I/O (docs/ptrace-free-injection.md). Nothing here allocates, so
    // there is no ambiguous allocation to quarantine either.
    // Bound-check BEFORE the first byte goes out. Checking afterwards reports a failure for writes
    // that have already landed past the end of the region.
    if (reserved != 0 && (uint64_t)out->image_size > reserved) {
      FAILR(10, "image 0x%x exceeds the reserved 0x%llx", out->image_size,
            (unsigned long long)reserved);
    }
    base = preallocated;
    alloc_rc = 0;
  } else {
    alloc_rc = gtav_proc_alloc_exec(pid, (size_t)(img->image_max - img->image_min), &base,
                                    &allocation_may_exist);
  }
  out->base = base;
  // Publish this independently of base: ptrace can lose RAX after dispatching mmap, leaving a
  // possibly-live allocation whose address is unknowable. The loader must still quarantine it.
  out->allocation_may_exist = allocation_may_exist != 0;
  if (alloc_rc != 0) {
    FAILR(10, "alloc_exec(0x%x) failed: %s", out->image_size, gtav_proc_last_error());
  }
  if (!inject_instance_matches(pid, expected_app_id)) {
    FAILR(13, "app instance changed during alloc_exec");
  }
  load_bias = (uint64_t)base - img->image_min;
  if (stop_after_alloc) {
    // Measurement lane: the reserve exists and is RWX, but not one image byte has been written.
    *base_out = base;
    *load_bias_out = load_bias;
    return 0;
  }

  for (i = 0; i < img->phnum; ++i) {
    uint64_t ph = img->phoff + (uint64_t)i * (uint64_t)img->phentsize;
    uint32_t p_type = rd32(elf + ph);
    uint64_t p_off;
    uint64_t p_vaddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    if (p_type != PT_LOAD) {
      continue;
    }
    p_off = rd64(elf + ph + 8);
    p_vaddr = rd64(elf + ph + 16);
    p_filesz = rd64(elf + ph + 32);
    p_memsz = rd64(elf + ph + 40);
    if (p_memsz == 0 || p_filesz == 0) {
      continue;
    }
    if (!in_range(p_off, p_filesz, len)) {
      FAILR(11, "segment %d file range out of range", i);
    }
    if (inject_write(pid, expected_app_id, load_bias + p_vaddr, elf + p_off, (size_t)p_filesz) !=
        0) {
      FAILR(12, "segment %d write failed", i);
    }
  }

  *base_out = base;
  *load_bias_out = load_bias;
  return 0;
}

// Parse the PT_DYNAMIC section: collect DT_* tags, resolve the table vaddrs to file
// offsets, and resolve DT_NEEDED basenames to in-buffer C strings.
static int parse_dynamic(const uint8_t* elf, size_t len, const ElfImage* img, ElfDynamic* dyn,
                         GtavElfInjectResult* out) {
  uint64_t strtab_v = 0, symtab_v = 0, rela_v = 0, jmprel_v = 0;
  uint64_t needed_off[MAX_NEEDED];
  uint64_t n, k;
  int i;

  memset(dyn, 0, sizeof(*dyn));
  dyn->syment = 24;
  dyn->relaent = 24;

  if (img->dyn_off == 0 || img->dyn_sz == 0 || !in_range(img->dyn_off, img->dyn_sz, len)) {
    FAILR(20, "no dynamic section");
  }
  n = img->dyn_sz / 16;
  for (k = 0; k < n; ++k) {
    int64_t tag = (int64_t)rd64(elf + img->dyn_off + k * 16);
    uint64_t val = rd64(elf + img->dyn_off + k * 16 + 8);
    if (tag == DT_NULL) {
      break;
    }
    switch (tag) {
      case DT_STRTAB:
        strtab_v = val;
        break;
      case DT_STRSZ:
        dyn->strsz = val;
        break;
      case DT_SYMTAB:
        symtab_v = val;
        break;
      case DT_SYMENT:
        dyn->syment = val ? val : 24;
        break;
      case DT_RELA:
        rela_v = val;
        break;
      case DT_RELASZ:
        dyn->relasz = val;
        break;
      case DT_RELAENT:
        dyn->relaent = val ? val : 24;
        break;
      case DT_JMPREL:
        jmprel_v = val;
        break;
      case DT_PLTRELSZ:
        dyn->pltrelsz = val;
        break;
      case DT_NEEDED:
        if (dyn->nneeded < MAX_NEEDED) {
          needed_off[dyn->nneeded++] = val;
        }
        break;
      default:
        break;
    }
  }

  // Resolve dynamic-table vaddrs to file offsets.
  if (strtab_v && vaddr_to_off(elf, len, img->phoff, img->phnum, img->phentsize, strtab_v,
                               &dyn->strtab_off) != 0) {
    FAILR(21, "DT_STRTAB vaddr unmapped");
  }
  if (symtab_v && vaddr_to_off(elf, len, img->phoff, img->phnum, img->phentsize, symtab_v,
                               &dyn->symtab_off) != 0) {
    FAILR(22, "DT_SYMTAB vaddr unmapped");
  }
  if (rela_v &&
      vaddr_to_off(elf, len, img->phoff, img->phnum, img->phentsize, rela_v, &dyn->rela_off) != 0) {
    FAILR(23, "DT_RELA vaddr unmapped");
  }
  if (jmprel_v && vaddr_to_off(elf, len, img->phoff, img->phnum, img->phentsize, jmprel_v,
                               &dyn->jmprel_off) != 0) {
    FAILR(24, "DT_JMPREL vaddr unmapped");
  }

  // Resolve DT_NEEDED basenames to in-buffer C strings.
  for (i = 0; i < dyn->nneeded; ++i) {
    uint64_t so = dyn->strtab_off + needed_off[i];
    if (dyn->strtab_off == 0 || !cstr_ok(elf, len, so)) {
      FAILR(25, "DT_NEEDED name out of range");
    }
    dyn->needed[i] = (const char*)(elf + so);
  }
  return 0;
}

int gtav_elf_map_and_relocate_at(int pid, const uint8_t* elf, size_t len, uint64_t expected_app_id,
                                 uintptr_t base, uint64_t reserved, GtavElfInjectResult* out) {
  if (base == 0) {
    if (out != NULL) {
      memset(out, 0, sizeof(*out));
    }
    return -1;
  }
  return gtav_elf_map_and_relocate_into(pid, elf, len, expected_app_id, GTAV_ELF_MAP_FULL, base,
                                        reserved, out);
}

int gtav_elf_map_and_relocate_staged(int pid, const uint8_t* elf, size_t len,
                                     uint64_t expected_app_id, int stop_after,
                                     GtavElfInjectResult* out) {
  return gtav_elf_map_and_relocate_into(pid, elf, len, expected_app_id, stop_after, 0, 0, out);
}

int gtav_elf_map_and_relocate_into(int pid, const uint8_t* elf, size_t len,
                                   uint64_t expected_app_id, int stop_after, uintptr_t preallocated,
                                   uint64_t reserved, GtavElfInjectResult* out) {
  ElfImage img;
  ElfDynamic dyn;
  uintptr_t base = 0;
  uint64_t load_bias = 0;

  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  if (elf == NULL) {
    FAILR(1, "null argument");
  }

  if (parse_elf_headers(elf, len, &img, out) != 0) {
    return -1;
  }
  if (map_segments(pid, expected_app_id, elf, len, &img,
                   stop_after == GTAV_ELF_MAP_STOP_AFTER_ALLOC, preallocated, reserved, &base,
                   &load_bias, out) != 0) {
    return -1;
  }
  if (stop_after == GTAV_ELF_MAP_STOP_AFTER_ALLOC) {
    out->base = (uintptr_t)load_bias;
    out->status = 0;
    out->error[0] = '\0';
    return 0;
  }
  if (parse_dynamic(elf, len, &img, &dyn, out) != 0) {
    return -1;
  }

  // Apply relocations (DT_RELA, then DT_JMPREL if distinct).
  if (apply_relocs(pid, expected_app_id, elf, len, load_bias, dyn.rela_off, dyn.relasz, dyn.relaent,
                   dyn.symtab_off, dyn.syment, dyn.strtab_off, dyn.strsz, dyn.needed, dyn.nneeded,
                   out) != 0) {
    return -1;
  }
  if (dyn.jmprel_off != 0 && dyn.jmprel_off != dyn.rela_off &&
      apply_relocs(pid, expected_app_id, elf, len, load_bias, dyn.jmprel_off, dyn.pltrelsz,
                   dyn.relaent, dyn.symtab_off, dyn.syment, dyn.strtab_off, dyn.strsz, dyn.needed,
                   dyn.nneeded, out) != 0) {
    return -1;
  }

  out->base = (uintptr_t)load_bias;
  out->status = 0;
  out->error[0] = '\0';
  return 0;
}

int gtav_elf_map_and_relocate(int pid, const uint8_t* elf, size_t len, uint64_t expected_app_id,
                              GtavElfInjectResult* out) {
  return gtav_elf_map_and_relocate_staged(pid, elf, len, expected_app_id, GTAV_ELF_MAP_FULL, out);
}

int gtav_elf_start_thread(int pid, const uint8_t* elf, size_t len, const char* entry_symbol,
                          uintptr_t arg, uint64_t expected_app_id, GtavElfInjectResult* out) {
  ElfImage img;
  uint64_t entry_value = 0;

  if (out == NULL) {
    return -1;
  }
  if (out->base == 0) {
    FAILR(61, "start_thread called without prior map_and_relocate");
  }
  if (elf == NULL || entry_symbol == NULL) {
    FAILR(1, "null argument");
  }

  if (parse_elf_headers(elf, len, &img, out) != 0) {
    return -1;
  }
  if (find_export(elf, len, img.shoff, img.shnum, img.shentsize, entry_symbol, &entry_value, out) !=
      0) {
    return -1;
  }
  out->entry = (uintptr_t)(out->base + entry_value);

  gtav_logf("elf_inject: base=0x%lx entry=0x%lx relocs=%u imports=%u; starting thread",
            (unsigned long)out->base, (unsigned long)out->entry, out->relocs_applied,
            out->imports_resolved);

  if (!inject_instance_matches(pid, expected_app_id)) {
    FAILR(59, "app instance changed before start_thread");
  }
  if (gtav_proc_start_thread(pid, out->entry, arg, &out->tid) != 0) {
    FAILR(60, "start_thread failed: %s", gtav_proc_last_error());
  }
  out->status = 0;
  out->error[0] = '\0';
  return 0;
}

int gtav_elf_inject(int pid, const uint8_t* elf, size_t len, const char* entry_symbol,
                    uintptr_t arg, uint64_t expected_app_id, GtavElfInjectResult* out) {
  if (gtav_elf_map_and_relocate(pid, elf, len, expected_app_id, out) != 0) {
    return -1;
  }
  return gtav_elf_start_thread(pid, elf, len, entry_symbol, arg, expected_app_id, out);
}
