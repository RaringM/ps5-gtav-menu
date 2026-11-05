#pragma once

// Boot-time pool override stub assembler (design: the custom-pack limits plan, §5.2 and §6).
//
// Replaces the 5-byte `call CGame_LoadGameConfig` in CGame_Init with a `call` to this stub in
// the .text tail-padding cave. The stub calls the original function, then applies a table of
// guarded i32 overrides on the game thread, between gameconfig parsing and streaming init:
//
//   node entry: find the CConfigManager map node for a joaat hash and rewrite node+4
//               (PoolSizes, e.g. MetaDataStore 0x983a6ef3)
//   cfg entry:  rewrite the parsed config struct field [mgr+0x18]+offset
//               (ConfigModelInfo caps at +0x318 ped, +0x31c vehicle, +0x320 weapon)
//
// An entry is written only when its current value equals the pinned stock value; otherwise it
// records the value it found and leaves it alone. Entries are independent: one failure does not
// skip the rest. No syscalls, no allocation, no stop.
//
// Header-only and free of SDK/game dependencies: tests/test_boot_pool_stub.py checks the
// encoding and executes the stub on an x86-64 host against a synthetic config map.
//
// Code at `cave`:
//
//   mov byte [rip+entered], 1            first: the loader never clears a cave that was entered
//   push rbx; push r12; push r13         three pushes: rsp is 16-byte aligned at the call
//   movabs r11, LoadGameConfig; call r11 arguments pass through untouched
//   mov rbx, [rip+g_ConfigManager]
//   lea r12, [rip+table]; mov r13d, count
//  .entry:
//   cmp byte [r12+kind], CFG; je .cfg
//   node: rbx null -> 2; bucket count 0 -> 4; buckets null -> 5; hash % count bucket,
//         bounded chain walk (CHAIN_MAX) comparing node[0]: end -> 6, limit -> 7; rdi = node+4
//   .cfg: rbx null -> 2; cfg null -> 3; rdi = cfg + offset
//  .write:
//   old = [rdi]; entry.old = old; old != entry.expected -> 8; [rdi] = entry.new; result 1
//  .publish:
//   entry.result = cl; r12 += ENTRY_SIZE; dec r13d; jnz .entry
//   mov byte [rip+done], 1; pop r13; pop r12; pop rbx; ret
//
// Data after the code (8-byte aligned): `count` entries of GTAV_BOOT_POOL_ENTRY_SIZE bytes, then
// the done byte (set last; the loader polls it) and the entered byte (set first).
//
//   entry +0  u8  kind          GTAV_BOOT_POOL_KIND_*
//         +1  u8  result        GTAV_BOOT_POOL_RESULT_* (0 = not run)
//         +4  u32 key           joaat hash (node) or byte offset into cfg (cfg)
//         +8  i32 expected_old  pinned stock value
//         +12 i32 new_value
//         +16 i32 old_value     value found by the stub
//
// Clobbers only caller-saved registers (rax, rcx, rdx, rdi, r11) and flags. The 01.011.001 call
// site (n0x4ba261) reloads rax, rcx and rdx right after the call.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GTAV_BOOT_POOL_STUB_MAX 1024u
#define GTAV_BOOT_POOL_ENTRIES_MAX 16u
#define GTAV_BOOT_POOL_CHAIN_MAX 16u
#define GTAV_BOOT_POOL_ENTRY_SIZE 24u
#define GTAV_BOOT_POOL_CFG_OFFSET_MAX 0x1000u

#define GTAV_BOOT_POOL_ENTRY_KIND 0u
#define GTAV_BOOT_POOL_ENTRY_RESULT 1u
#define GTAV_BOOT_POOL_ENTRY_KEY 4u
#define GTAV_BOOT_POOL_ENTRY_EXPECTED 8u
#define GTAV_BOOT_POOL_ENTRY_NEW 12u
#define GTAV_BOOT_POOL_ENTRY_OLD 16u

enum {
  GTAV_BOOT_POOL_KIND_NODE = 1,
  GTAV_BOOT_POOL_KIND_CFG = 2,
};

enum {
  GTAV_BOOT_POOL_RESULT_NOT_RUN = 0,
  GTAV_BOOT_POOL_RESULT_WRITTEN = 1,
  GTAV_BOOT_POOL_RESULT_NO_MANAGER = 2,
  GTAV_BOOT_POOL_RESULT_NO_CFG = 3,
  GTAV_BOOT_POOL_RESULT_NO_BUCKET_COUNT = 4,
  GTAV_BOOT_POOL_RESULT_NO_BUCKETS = 5,
  GTAV_BOOT_POOL_RESULT_NOT_FOUND = 6,
  GTAV_BOOT_POOL_RESULT_CHAIN_LIMIT = 7,
  GTAV_BOOT_POOL_RESULT_UNEXPECTED_OLD = 8,
};

typedef struct {
  uint8_t kind;
  uint32_t key;
  int32_t expected_old;
  int32_t new_value;
} GtavBootPoolEntry;

typedef struct {
  uint64_t table_addr;
  uint64_t done_addr;
  uint64_t entered_addr;
  uint32_t count;
  uint32_t size;
} GtavBootPoolStubLayout;

static inline const char* gtav_boot_pool_result_name(uint8_t result) {
  switch (result) {
    case GTAV_BOOT_POOL_RESULT_NOT_RUN:
      return "not-run";
    case GTAV_BOOT_POOL_RESULT_WRITTEN:
      return "written";
    case GTAV_BOOT_POOL_RESULT_NO_MANAGER:
      return "no-manager";
    case GTAV_BOOT_POOL_RESULT_NO_CFG:
      return "no-cfg";
    case GTAV_BOOT_POOL_RESULT_NO_BUCKET_COUNT:
      return "no-bucket-count";
    case GTAV_BOOT_POOL_RESULT_NO_BUCKETS:
      return "no-buckets";
    case GTAV_BOOT_POOL_RESULT_NOT_FOUND:
      return "not-found";
    case GTAV_BOOT_POOL_RESULT_CHAIN_LIMIT:
      return "chain-limit";
    case GTAV_BOOT_POOL_RESULT_UNEXPECTED_OLD:
      return "unexpected-old";
    default:
      return "unknown";
  }
}

// --- Minimal emitter with label fixups (range-checked; any error makes the build return 0) ---

enum {
  GTAV_BP_L_ENTRY,
  GTAV_BP_L_WALK,
  GTAV_BP_L_FOUND,
  GTAV_BP_L_CFG,
  GTAV_BP_L_WRITE,
  GTAV_BP_L_PUBLISH,
  GTAV_BP_L_F2,
  GTAV_BP_L_F3,
  GTAV_BP_L_F4,
  GTAV_BP_L_F5,
  GTAV_BP_L_F6,
  GTAV_BP_L_F7,
  GTAV_BP_L_F8,
  GTAV_BP_L_TABLE,
  GTAV_BP_L_DONE,
  GTAV_BP_L_ENTERED,
  GTAV_BP_L_COUNT,
};

#define GTAV_BP_FIXUPS_MAX 32u

typedef struct {
  uint32_t pos;
  uint32_t end;
  uint8_t label;
  uint8_t width;
} GtavBpFixup;

typedef struct {
  uint8_t* out;
  uint32_t n;
  uint32_t cap;
  int bad;
  int64_t label[GTAV_BP_L_COUNT];
  GtavBpFixup fix[GTAV_BP_FIXUPS_MAX];
  uint32_t nfix;
} GtavBpAsm;

static inline void gtav_bp_bytes(GtavBpAsm* a, const uint8_t* bytes, uint32_t len) {
  uint32_t i;
  if (a->bad || len > a->cap - a->n) {
    a->bad = 1;
    return;
  }
  for (i = 0; i < len; ++i) a->out[a->n++] = bytes[i];
}

static inline void gtav_bp_u8(GtavBpAsm* a, uint8_t v) {
  gtav_bp_bytes(a, &v, 1);
}

static inline void gtav_bp_u32(GtavBpAsm* a, uint32_t v) {
  const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  gtav_bp_bytes(a, b, 4);
}

static inline void gtav_bp_u64(GtavBpAsm* a, uint64_t v) {
  gtav_bp_u32(a, (uint32_t)v);
  gtav_bp_u32(a, (uint32_t)(v >> 32));
}

static inline void gtav_bp_mark(GtavBpAsm* a, uint8_t label) {
  a->label[label] = (int64_t)a->n;
}

// Reserve a `width`-byte displacement at the current position, relative to `end_extra` bytes past
// the displacement (the trailing immediate of a rip-relative store, else 0).
static inline void gtav_bp_ref(GtavBpAsm* a, uint8_t label, uint8_t width, uint32_t end_extra) {
  GtavBpFixup* f;
  if (a->bad || a->nfix >= GTAV_BP_FIXUPS_MAX) {
    a->bad = 1;
    return;
  }
  f = &a->fix[a->nfix++];
  f->pos = a->n;
  f->end = a->n + width + end_extra;
  f->label = label;
  f->width = width;
  if (width == 1) {
    gtav_bp_u8(a, 0);
  } else {
    gtav_bp_u32(a, 0);
  }
}

// Conditional jumps: cc 4 = z/e, 5 = nz/ne.
static inline void gtav_bp_jcc8(GtavBpAsm* a, uint8_t cc, uint8_t label) {
  gtav_bp_u8(a, (uint8_t)(0x70u | cc));
  gtav_bp_ref(a, label, 1, 0);
}

static inline void gtav_bp_jcc32(GtavBpAsm* a, uint8_t cc, uint8_t label) {
  const uint8_t op[2] = {0x0F, (uint8_t)(0x80u | cc)};
  gtav_bp_bytes(a, op, 2);
  gtav_bp_ref(a, label, 4, 0);
}

static inline void gtav_bp_jmp8(GtavBpAsm* a, uint8_t label) {
  gtav_bp_u8(a, 0xEB);
  gtav_bp_ref(a, label, 1, 0);
}

static inline void gtav_bp_jmp32(GtavBpAsm* a, uint8_t label) {
  gtav_bp_u8(a, 0xE9);
  gtav_bp_ref(a, label, 4, 0);
}

static inline int gtav_bp_resolve(GtavBpAsm* a) {
  uint32_t i;
  uint32_t k;
  if (a->bad) return -1;
  for (i = 0; i < a->nfix; ++i) {
    const GtavBpFixup* f = &a->fix[i];
    int64_t rel;
    if (a->label[f->label] < 0) return -1;
    rel = a->label[f->label] - (int64_t)f->end;
    if (f->width == 1) {
      if (rel < -128 || rel > 127) return -1;
      a->out[f->pos] = (uint8_t)(int8_t)rel;
    } else {
      for (k = 0; k < 4; ++k) a->out[f->pos + k] = (uint8_t)((uint32_t)(int32_t)rel >> (8u * k));
    }
  }
  return 0;
}

// Assemble the stub into `out`. Returns the total length (code + table + done byte), or 0 on any
// invalid argument, an entry the stub cannot apply safely, or an `out_cap` too small.
//
// cave:                  live install address in .text tail padding
// load_game_config:      live address of CGame_LoadGameConfig
// g_config_manager_ptr:  live address of the g_ConfigManager global pointer (within ±2 GiB)
// entries/count:         1..GTAV_BOOT_POOL_ENTRIES_MAX overrides; cfg offsets 4-aligned and
//                        below GTAV_BOOT_POOL_CFG_OFFSET_MAX
static inline uint32_t gtav_boot_pool_stub_build(uint64_t cave, uint64_t load_game_config,
                                                 uint64_t g_config_manager_ptr,
                                                 const GtavBootPoolEntry* entries, uint32_t count,
                                                 uint8_t* out, uint32_t out_cap,
                                                 GtavBootPoolStubLayout* layout) {
  GtavBpAsm a;
  uint32_t i;
  int64_t mgr_disp;
  uint8_t code;

  if (out == NULL || layout == NULL || entries == NULL || cave == 0 || load_game_config == 0 ||
      g_config_manager_ptr == 0 || count == 0 || count > GTAV_BOOT_POOL_ENTRIES_MAX) {
    return 0;
  }
  for (i = 0; i < count; ++i) {
    if (entries[i].kind == GTAV_BOOT_POOL_KIND_CFG) {
      if (entries[i].key >= GTAV_BOOT_POOL_CFG_OFFSET_MAX || (entries[i].key & 3u) != 0) return 0;
    } else if (entries[i].kind != GTAV_BOOT_POOL_KIND_NODE) {
      return 0;
    }
  }

  a.out = out;
  a.n = 0;
  a.cap = out_cap;
  a.bad = 0;
  a.nfix = 0;
  for (i = 0; i < GTAV_BP_L_COUNT; ++i) a.label[i] = -1;

  {
    // mov byte [rip+entered], 1
    static const uint8_t k[] = {0xC6, 0x05};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_ref(&a, GTAV_BP_L_ENTERED, 4, 1);
    gtav_bp_u8(&a, 0x01);
  }
  {
    // push rbx; push r12; push r13
    static const uint8_t k[] = {0x53, 0x41, 0x54, 0x41, 0x55};
    gtav_bp_bytes(&a, k, sizeof(k));
  }
  {
    // movabs r11, load_game_config; call r11
    static const uint8_t mov[] = {0x49, 0xBB};
    static const uint8_t call[] = {0x41, 0xFF, 0xD3};
    gtav_bp_bytes(&a, mov, sizeof(mov));
    gtav_bp_u64(&a, load_game_config);
    gtav_bp_bytes(&a, call, sizeof(call));
  }
  {
    // mov rbx, [rip+g_ConfigManager]
    static const uint8_t k[] = {0x48, 0x8B, 0x1D};
    gtav_bp_bytes(&a, k, sizeof(k));
    mgr_disp = (int64_t)g_config_manager_ptr - (int64_t)(cave + a.n + 4u);
    if (mgr_disp < INT32_MIN || mgr_disp > INT32_MAX) return 0;
    gtav_bp_u32(&a, (uint32_t)(int32_t)mgr_disp);
  }
  {
    // lea r12, [rip+table]; mov r13d, count
    static const uint8_t lea[] = {0x4C, 0x8D, 0x25};
    static const uint8_t mov[] = {0x41, 0xBD};
    gtav_bp_bytes(&a, lea, sizeof(lea));
    gtav_bp_ref(&a, GTAV_BP_L_TABLE, 4, 0);
    gtav_bp_bytes(&a, mov, sizeof(mov));
    gtav_bp_u32(&a, count);
  }

  gtav_bp_mark(&a, GTAV_BP_L_ENTRY);
  {
    // cmp byte [r12], CFG; je .cfg
    static const uint8_t k[] = {0x41, 0x80, 0x3C, 0x24, GTAV_BOOT_POOL_KIND_CFG};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_CFG);
  }
  {
    // test rbx, rbx; jz .f2
    static const uint8_t k[] = {0x48, 0x85, 0xDB};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_F2);
  }
  {
    // movzx ecx, word [rbx+0x10]; test ecx, ecx; jz .f4
    static const uint8_t k[] = {0x0F, 0xB7, 0x4B, 0x10, 0x85, 0xC9};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_F4);
  }
  {
    // mov eax, [r12+key]; xor edx, edx; div ecx      (edx = hash % bucket count)
    // mov rax, [rbx+8]; test rax, rax; jz .f5
    static const uint8_t k[] = {
        0x41, 0x8B, 0x44, 0x24, GTAV_BOOT_POOL_ENTRY_KEY, 0x31, 0xD2, 0xF7, 0xF1, 0x48, 0x8B, 0x43,
        0x08, 0x48, 0x85, 0xC0};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_F5);
  }
  {
    // mov rdi, [rax+rdx*8]; mov eax, [r12+key]; mov ecx, CHAIN_MAX
    static const uint8_t k[] = {
        0x48, 0x8B, 0x3C, 0xD0, 0x41, 0x8B, 0x44, 0x24, GTAV_BOOT_POOL_ENTRY_KEY, 0xB9};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_u32(&a, GTAV_BOOT_POOL_CHAIN_MAX);
  }
  gtav_bp_mark(&a, GTAV_BP_L_WALK);
  {
    // test rdi, rdi; jz .f6; cmp [rdi], eax; je .found
    static const uint8_t t[] = {0x48, 0x85, 0xFF};
    static const uint8_t c[] = {0x39, 0x07};
    gtav_bp_bytes(&a, t, sizeof(t));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_F6);
    gtav_bp_bytes(&a, c, sizeof(c));
    gtav_bp_jcc8(&a, 4, GTAV_BP_L_FOUND);
  }
  {
    // mov rdi, [rdi+8]; dec ecx; jnz .walk; jmp .f7
    static const uint8_t k[] = {0x48, 0x8B, 0x7F, 0x08, 0xFF, 0xC9};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc8(&a, 5, GTAV_BP_L_WALK);
    gtav_bp_jmp32(&a, GTAV_BP_L_F7);
  }
  gtav_bp_mark(&a, GTAV_BP_L_FOUND);
  {
    // add rdi, 4; jmp .write
    static const uint8_t k[] = {0x48, 0x83, 0xC7, 0x04};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jmp8(&a, GTAV_BP_L_WRITE);
  }
  gtav_bp_mark(&a, GTAV_BP_L_CFG);
  {
    // test rbx, rbx; jz .f2; mov rdi, [rbx+0x18]; test rdi, rdi; jz .f3
    static const uint8_t t[] = {0x48, 0x85, 0xDB};
    static const uint8_t m[] = {0x48, 0x8B, 0x7B, 0x18, 0x48, 0x85, 0xFF};
    gtav_bp_bytes(&a, t, sizeof(t));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_F2);
    gtav_bp_bytes(&a, m, sizeof(m));
    gtav_bp_jcc32(&a, 4, GTAV_BP_L_F3);
  }
  {
    // mov eax, [r12+key]; add rdi, rax
    static const uint8_t k[] = {0x41, 0x8B, 0x44, 0x24, GTAV_BOOT_POOL_ENTRY_KEY, 0x48, 0x01, 0xC7};
    gtav_bp_bytes(&a, k, sizeof(k));
  }
  gtav_bp_mark(&a, GTAV_BP_L_WRITE);
  {
    // mov eax, [rdi]; mov [r12+old], eax; cmp eax, [r12+expected]; jne .f8
    static const uint8_t k[] = {0x8B,
                                0x07,
                                0x41,
                                0x89,
                                0x44,
                                0x24,
                                GTAV_BOOT_POOL_ENTRY_OLD,
                                0x41,
                                0x3B,
                                0x44,
                                0x24,
                                GTAV_BOOT_POOL_ENTRY_EXPECTED};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc8(&a, 5, GTAV_BP_L_F8);
  }
  {
    // mov eax, [r12+new]; mov [rdi], eax; mov cl, WRITTEN; jmp .publish
    static const uint8_t k[] = {0x41,
                                0x8B,
                                0x44,
                                0x24,
                                GTAV_BOOT_POOL_ENTRY_NEW,
                                0x89,
                                0x07,
                                0xB1,
                                GTAV_BOOT_POOL_RESULT_WRITTEN};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jmp8(&a, GTAV_BP_L_PUBLISH);
  }
  // .f2-.f8: mov cl, code; jmp .publish
  for (code = GTAV_BOOT_POOL_RESULT_NO_MANAGER; code <= GTAV_BOOT_POOL_RESULT_UNEXPECTED_OLD;
       ++code) {
    gtav_bp_mark(&a, (uint8_t)(GTAV_BP_L_F2 + (code - GTAV_BOOT_POOL_RESULT_NO_MANAGER)));
    gtav_bp_u8(&a, 0xB1);
    gtav_bp_u8(&a, code);
    if (code != GTAV_BOOT_POOL_RESULT_UNEXPECTED_OLD) gtav_bp_jmp8(&a, GTAV_BP_L_PUBLISH);
  }
  gtav_bp_mark(&a, GTAV_BP_L_PUBLISH);
  {
    // mov [r12+result], cl; add r12, ENTRY_SIZE; dec r13d; jnz .entry
    static const uint8_t k[] = {0x41,
                                0x88,
                                0x4C,
                                0x24,
                                GTAV_BOOT_POOL_ENTRY_RESULT,
                                0x49,
                                0x83,
                                0xC4,
                                GTAV_BOOT_POOL_ENTRY_SIZE,
                                0x41,
                                0xFF,
                                0xCD};
    gtav_bp_bytes(&a, k, sizeof(k));
    gtav_bp_jcc32(&a, 5, GTAV_BP_L_ENTRY);
  }
  {
    // mov byte [rip+done], 1; pop r13; pop r12; pop rbx; ret
    static const uint8_t mov[] = {0xC6, 0x05};
    static const uint8_t tail[] = {0x41, 0x5D, 0x41, 0x5C, 0x5B, 0xC3};
    gtav_bp_bytes(&a, mov, sizeof(mov));
    gtav_bp_ref(&a, GTAV_BP_L_DONE, 4, 1);
    gtav_bp_u8(&a, 0x01);
    gtav_bp_bytes(&a, tail, sizeof(tail));
  }

  // Data: 8-byte aligned entry table, then the done byte.
  while ((a.n & 7u) != 0 && !a.bad) gtav_bp_u8(&a, 0x00);
  gtav_bp_mark(&a, GTAV_BP_L_TABLE);
  for (i = 0; i < count; ++i) {
    uint8_t rec[GTAV_BOOT_POOL_ENTRY_SIZE] = {0};
    uint32_t k;
    rec[GTAV_BOOT_POOL_ENTRY_KIND] = entries[i].kind;
    for (k = 0; k < 4; ++k) {
      rec[GTAV_BOOT_POOL_ENTRY_KEY + k] = (uint8_t)(entries[i].key >> (8u * k));
      rec[GTAV_BOOT_POOL_ENTRY_EXPECTED + k] =
          (uint8_t)((uint32_t)entries[i].expected_old >> (8u * k));
      rec[GTAV_BOOT_POOL_ENTRY_NEW + k] = (uint8_t)((uint32_t)entries[i].new_value >> (8u * k));
    }
    gtav_bp_bytes(&a, rec, sizeof(rec));
  }
  gtav_bp_mark(&a, GTAV_BP_L_DONE);
  gtav_bp_u8(&a, 0x00);
  gtav_bp_mark(&a, GTAV_BP_L_ENTERED);
  gtav_bp_u8(&a, 0x00);

  if (gtav_bp_resolve(&a) != 0) return 0;

  layout->table_addr = cave + (uint64_t)a.label[GTAV_BP_L_TABLE];
  layout->done_addr = cave + (uint64_t)a.label[GTAV_BP_L_DONE];
  layout->entered_addr = cave + (uint64_t)a.label[GTAV_BP_L_ENTERED];
  layout->count = count;
  layout->size = a.n;
  return a.n;
}

// Decode one entry of a table read back from the cave (`table` holds layout.count records).
static inline void gtav_boot_pool_entry_read(const uint8_t* table, uint32_t index,
                                             uint8_t* result_out, int32_t* old_out) {
  const uint8_t* rec = table + (size_t)index * GTAV_BOOT_POOL_ENTRY_SIZE;
  *result_out = rec[GTAV_BOOT_POOL_ENTRY_RESULT];
  *old_out = (int32_t)((uint32_t)rec[GTAV_BOOT_POOL_ENTRY_OLD] |
                       ((uint32_t)rec[GTAV_BOOT_POOL_ENTRY_OLD + 1] << 8) |
                       ((uint32_t)rec[GTAV_BOOT_POOL_ENTRY_OLD + 2] << 16) |
                       ((uint32_t)rec[GTAV_BOOT_POOL_ENTRY_OLD + 3] << 24));
}

// Build the 5-byte call replacement: E8 + rel32 targeting `cave` from `call_site`.
// Returns 5, or 0 when the displacement does not fit in int32 or an argument is invalid.
static inline uint32_t gtav_boot_pool_call_build(uint64_t call_site, uint64_t cave, uint8_t* out,
                                                 uint32_t out_cap) {
  int64_t disp64;
  uint32_t i;
  if (out == NULL || out_cap < 5u || call_site == 0 || cave == 0) return 0;
  disp64 = (int64_t)(cave - (call_site + 5u));
  if (disp64 < (int64_t)INT32_MIN || disp64 > (int64_t)INT32_MAX) return 0;
  out[0] = 0xE8;
  for (i = 0; i < 4; ++i) out[1 + i] = (uint8_t)((uint32_t)(int32_t)disp64 >> (8u * i));
  return 5;
}

#ifdef __cplusplus
}
#endif
