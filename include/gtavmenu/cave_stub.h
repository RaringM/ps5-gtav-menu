#pragma once

// Bootstrap stub assembler for the ptrace-free injection route (docs/ptrace-free-injection.md).
//
// PT_ATTACH permanently breaks GTA's streaming I/O, so the loader cannot use a remote mmap driven
// by ptrace register injection to obtain memory. Instead it writes this stub into the executable
// segment's tail padding -- a code cave -- over mdbg, detours PLAYER_PED_ID to it, and lets the
// stub obtain memory from inside the process on the game thread. Verified on hardware: a counter
// stub written this way runs ~270x per frame and the install/uninstall cycle leaves the game
// intact.
//
// Header-only and free of SDK/game dependencies on purpose: the bytes are hand-assembled x86-64
// written straight into a live game's .text, where one wrong byte crashes it on the next frame, so
// the whole thing is exercised on the host by tests/test_cave_stub.py.
//
// Layout produced at `cave` (legacy offsets shown; the exact-split body starts five bytes earlier
// after its shorter atomic gate; all addresses are absolute because the eboot has no ASLR):
//
//   +0                      one-shot claim gate           legacy: cmp/jne/mov byte; exact-split:
//                                                         aligned lock bts dword + jc. The latter
//                                                         is atomic because a concurrent fire must
//                                                         not double-allocate.
//   +20                     push rdi/rsi/rdx/rcx/r8/r9/r10/r11/rax
//                                                         the detour sits at a function ENTRY, so
//                                                         the argument registers are live and every
//                                                         caller-saved register must survive
//   +33                     xor  edi, edi                 mmap(NULL, size, RW, ANON|PRIVATE, -1, 0)
//                           mov  esi, size
//                           mov  edx, 3
//                           mov  r10d, 0x1002             syscall ABI: 4th argument is r10, not rcx
//                           mov  r8d, -1
//                           xor  r9d, r9d
//                           mov  eax, SYS_mmap
//                           call gadget                   libkernel's `syscall; ret`, not our own
//                                                         syscall instruction, which PS5 rejects
//                           <plausibility guard on rax>    negative, small, or unaligned => skip
//                           mov  rcx, rax                 PREFAULT: one store per 4 KB page, so
//                           mov  edx, size                 every page of the region is RESIDENT
//                    .touch: mov  byte [rcx], 0            before the loader writes the worker
//                           add  rcx, 0x1000               image into it. The loader's kernel
//                           sub  edx, 0x1000               write path walks the target's page
//                           jnz  .touch                    tables and copies through the direct
//                                                          map, which reaches resident pages ONLY
//                                                          -- a demand-zero MAP_ANON region has no
//                                                          PTEs until something touches it.
//                           mov  [rip+result], rax        published AFTER the prefault, so the
//                                                         loader cannot start writing into a
//                                                         half-resident region
//                           [split lane only: preserve the allocation on the stack, call
//                            mprotect(first page,R), and replace an INT64_MIN status sentinel]
//                           pop  ... (reverse order)
//   .tail                   <stolen prologue, relocated>
//                           jmp  [rip+disp] -> continuation
//                           <u64 continuation>
//                           <u64 result>                  the allocation or raw syscall error value
//                           <i64 split result>            optional; INT64_MIN until mprotect
//                                                         returns
//                           <u8/u32 done>                 legacy entry byte / atomic split claim
//                           <u8 split completion>         optional; after saved-register restore
//
// Stack alignment: at a function entry rsp%16 == 8. Nine pushes move it by 72 (72%16 == 8), so rsp
// is 16-byte aligned at the `call`, which is what the ABI requires.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// FreeBSD/PS5 mmap constants and syscall number.
#define GTAV_CAVE_SYS_MMAP 477
#define GTAV_CAVE_SYS_MPROTECT 74
#define GTAV_CAVE_PROT_READ 0x1
#define GTAV_CAVE_PROT_RW 0x3
#define GTAV_CAVE_MAP_ANON_PRIVATE 0x1002
#ifndef GTAV_CAVE_ALLOC_HINT
#define GTAV_CAVE_ALLOC_HINT 0
#endif

// Upper bound on the assembled stub, generous enough for the layout above plus its data slots and
// room to grow: the cave is 9.4 KB of segment-tail padding, so headroom here costs nothing.
#define GTAV_CAVE_STUB_MAX 256u

// Maximum stolen prologue this assembler will relocate.
#define GTAV_CAVE_STOLEN_MAX 32u

// Where the loader finds the stub's data slots, as absolute addresses.
typedef struct GtavCaveStubLayout {
  uint64_t result_addr;          // u64: mmap's return value, 0 until the stub has run
  uint64_t protect_result_addr;  // optional i64: INT64_MIN pending, then split mprotect result
  uint64_t done_addr;            // legacy u8 or split u32: claimed before the first syscall
  uint64_t completion_addr;      // optional u8: split winner restored all saved registers
  uint64_t tail_addr;            // first byte of the relocated prologue
  uint32_t size;                 // total assembled length
} GtavCaveStubLayout;

// One rip-relative operand inside the stolen prologue that must be recomputed for its new home.
// PLAYER_PED_ID's prologue on 01.010.002 has exactly one, at offset 6 (`mov rax,[rip+disp32]`);
// patching at the wrong offset corrupts the ModRM byte, which is why the offset is an explicit
// input rather than something this code guesses.
typedef struct GtavCaveRiprel {
  uint32_t offset;  // byte offset of the instruction within the stolen prologue
  uint32_t length;  // instruction length, used to find the next-instruction address
} GtavCaveRiprel;

// Assemble the stub. `stolen` is the prologue copied out of the target at `target`, `continuation`
// is where execution resumes (target + stolen_len). `riprel` may be NULL when the prologue contains
// no rip-relative operand. Returns the assembled length, or 0 on any invalid argument -- callers
// must treat 0 as "do not write anything".
static inline uint32_t gtav_cave_stub_build_split_readonly(
    uint64_t cave, uint64_t gadget, uint64_t target, const uint8_t* stolen, uint32_t stolen_len,
    uint64_t continuation, uint32_t alloc_size, uint32_t split_readonly_size,
    const GtavCaveRiprel* riprel, uint8_t* out, uint32_t out_cap, GtavCaveStubLayout* layout) {
  uint32_t n = 0;
  uint32_t tail_off;
  uint32_t data_off;
  uint32_t i;
  int32_t disp;
  // Positions of the rel8 displacement bytes that jump forward over the prefault loop, patched once
  // its end is known, plus the loop's own head for the backward branch.
  uint32_t skip_fix[3];
  uint32_t skip_fixes = 0;
  uint32_t touch_head;
  int32_t rel;

  if (out == NULL || stolen == NULL || layout == NULL || stolen_len == 0 ||
      stolen_len > GTAV_CAVE_STOLEN_MAX || out_cap < GTAV_CAVE_STUB_MAX || cave == 0 ||
      gadget == 0 || alloc_size == 0) {
    return 0;
  }
  if (riprel != NULL &&
      ((uint64_t)riprel->offset + riprel->length > stolen_len || riprel->length < 7)) {
    return 0;
  }
  // The prefault loop walks the region in 4 KB steps and stops on a zero counter, so a size that is
  // not a whole number of pages would run past the end of the mapping.
  if ((alloc_size & 0xFFFu) != 0) {
    return 0;
  }
  // The special cave-prefix lane asks the target's own vm_map_protect path to split the mapping by
  // removing write permission from its first 16-KiB page. The loader can then add execute to that
  // already-separate entry through kernel R/W without ever making the second (data) page
  // executable.
  if (split_readonly_size &&
      ((split_readonly_size & 0x3fffu) || split_readonly_size >= alloc_size)) {
    return 0;
  }
  // A non-fixed address hint only: mmap may choose another free address. Never add MAP_FIXED.
  if (GTAV_CAVE_ALLOC_HINT && ((uint64_t)GTAV_CAVE_ALLOC_HINT < 0x10000ull ||
                               (uint64_t)GTAV_CAVE_ALLOC_HINT > 0xffffc000ull ||
                               ((uint64_t)GTAV_CAVE_ALLOC_HINT & 0x3fffull))) {
    return 0;
  }

  // Data slots live after the code; compute their offsets first so the code can reference them.
  // Code length is fixed by construction: 33 legacy or 28 split bytes of claim gate + register
  // save, 65 bytes of call sequence and 47 bytes of prefault. The split variant then adds its
  // second call and post-restore completion store. The stolen bytes and a 6-byte jump follow. The
  // assert below refuses to emit anything if this drifts.
  tail_off = (split_readonly_size ? 28u : 33u) + 65u + 47u;
  if (GTAV_CAVE_ALLOC_HINT) tail_off += 3u;  // mov edi,imm32 replaces xor edi,edi
  if (split_readonly_size) tail_off += 49u;
  if (split_readonly_size) tail_off += 7u;  // completion store after saved-register restoration
  data_off = tail_off + stolen_len + 6u;
  data_off = (data_off + 7u) & ~7u;  // align the u64 slots
  if (data_off + 8u + 8u + (split_readonly_size ? 13u : 1u) > GTAV_CAVE_STUB_MAX) {
    return 0;
  }

  {
    const uint64_t protect_result = split_readonly_size ? cave + data_off + 16u : 0;
    const uint64_t done = cave + data_off + 16u + (split_readonly_size ? 8u : 0u);
    const uint64_t completion = split_readonly_size ? done + 4u : 0;
    const uint64_t result = cave + data_off + 8u;

    if (split_readonly_size) {
      // lock bts dword [rip+done],0 atomically elects one allocator without touching any incoming
      // register. CF carries the old bit; all losing or later entrants replay the stolen prologue.
      // The split-only encoding is five bytes shorter than the legacy cmp/jne/mov gate and keeps
      // the exact non-split bootstrap byte-for-byte unchanged.
      out[n++] = 0xf0;
      out[n++] = 0x0f;
      out[n++] = 0xba;
      out[n++] = 0x2d;
      disp = (int32_t)(int64_t)(done - (cave + n + 5u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0x00;
      out[n++] = 0x0f;  // jc rel32 -> tail
      out[n++] = 0x82;
      disp = (int32_t)(tail_off - (n + 4u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
    } else {
      // Preserve the previously hardware-validated non-split gate exactly.
      out[n++] = 0x80;  // cmp byte [rip+disp32],0
      out[n++] = 0x3d;
      disp = (int32_t)(int64_t)(done - (cave + n + 5u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0x00;
      out[n++] = 0x0f;  // jne rel32 -> tail
      out[n++] = 0x85;
      disp = (int32_t)(tail_off - (n + 4u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0xc6;  // mov byte [rip+disp32],1
      out[n++] = 0x05;
      disp = (int32_t)(int64_t)(done - (cave + n + 5u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0x01;
    }
    // push rdi,rsi,rdx,rcx,r8,r9,r10,r11,rax  (9 regs, 13 bytes: r8-r11 need a REX prefix)
    out[n++] = 0x57;
    out[n++] = 0x56;
    out[n++] = 0x52;
    out[n++] = 0x51;
    out[n++] = 0x41;
    out[n++] = 0x50;
    out[n++] = 0x41;
    out[n++] = 0x51;
    out[n++] = 0x41;
    out[n++] = 0x52;
    out[n++] = 0x41;
    out[n++] = 0x53;
    out[n++] = 0x50;

    if (GTAV_CAVE_ALLOC_HINT) {
      out[n++] = 0xBF;  // mov edi,imm32; zero-extended address hint, not a fixed mapping
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)((uint64_t)GTAV_CAVE_ALLOC_HINT >> (8u * i));
    } else {
      // Preserve the previously validated NULL-hint encoding byte for byte.
      out[n++] = 0x31;
      out[n++] = 0xFF;
    }
    // mov esi, alloc_size      (5)
    out[n++] = 0xBE;
    for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(alloc_size >> (8u * i));
    // mov edx, PROT_RW         (5)
    out[n++] = 0xBA;
    out[n++] = (uint8_t)GTAV_CAVE_PROT_RW;
    out[n++] = 0x00;
    out[n++] = 0x00;
    out[n++] = 0x00;
    // mov r10d, MAP_ANON|MAP_PRIVATE  (6)
    out[n++] = 0x41;
    out[n++] = 0xBA;
    out[n++] = (uint8_t)(GTAV_CAVE_MAP_ANON_PRIVATE & 0xFF);
    out[n++] = (uint8_t)((GTAV_CAVE_MAP_ANON_PRIVATE >> 8) & 0xFF);
    out[n++] = 0x00;
    out[n++] = 0x00;
    // mov r8d, -1              (6)
    out[n++] = 0x41;
    out[n++] = 0xB8;
    out[n++] = 0xFF;
    out[n++] = 0xFF;
    out[n++] = 0xFF;
    out[n++] = 0xFF;
    // xor r9d,r9d              (3)
    out[n++] = 0x45;
    out[n++] = 0x31;
    out[n++] = 0xC9;
    // mov eax, SYS_mmap        (5)
    out[n++] = 0xB8;
    out[n++] = (uint8_t)(GTAV_CAVE_SYS_MMAP & 0xFF);
    out[n++] = (uint8_t)((GTAV_CAVE_SYS_MMAP >> 8) & 0xFF);
    out[n++] = 0x00;
    out[n++] = 0x00;
    // mov r11, gadget          (10)  -- r11 is caller-saved and already pushed
    out[n++] = 0x49;
    out[n++] = 0xBB;
    for (i = 0; i < 8; ++i) out[n++] = (uint8_t)(gadget >> (8u * i));
    // call r11                 (3)
    out[n++] = 0x41;
    out[n++] = 0xFF;
    out[n++] = 0xD3;

    // Plausibility guard before treating rax as an address. A failed syscall through the gadget
    // comes back as an error value, not a pointer, and storing through one would fault the game on
    // the spot -- so the prefault runs only for something that can actually be an mmap result:
    // non-negative, above the first 64 KB, and page-aligned.
    // test rax, rax            (3)
    out[n++] = 0x48;
    out[n++] = 0x85;
    out[n++] = 0xC0;
    // js .publish              (2)
    out[n++] = 0x78;
    skip_fix[skip_fixes++] = n;
    out[n++] = 0x00;
    // cmp rax, 0xFFFF          (6)
    out[n++] = 0x48;
    out[n++] = 0x3D;
    out[n++] = 0xFF;
    out[n++] = 0xFF;
    out[n++] = 0x00;
    out[n++] = 0x00;
    // jbe .publish             (2)
    out[n++] = 0x76;
    skip_fix[skip_fixes++] = n;
    out[n++] = 0x00;
    // test rax, 0xFFF          (6)
    out[n++] = 0x48;
    out[n++] = 0xA9;
    out[n++] = 0xFF;
    out[n++] = 0x0F;
    out[n++] = 0x00;
    out[n++] = 0x00;
    // jnz .publish             (2)
    out[n++] = 0x75;
    skip_fix[skip_fixes++] = n;
    out[n++] = 0x00;
    // mov rcx, rax             (3)
    out[n++] = 0x48;
    out[n++] = 0x89;
    out[n++] = 0xC1;
    // mov edx, alloc_size      (5)
    out[n++] = 0xBA;
    for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(alloc_size >> (8u * i));
    touch_head = n;
    // mov byte [rcx], 0        (3)  -- the store, not a read: only a write faults the page in dirty
    out[n++] = 0xC6;
    out[n++] = 0x01;
    out[n++] = 0x00;
    // add rcx, 0x1000          (7)
    out[n++] = 0x48;
    out[n++] = 0x81;
    out[n++] = 0xC1;
    out[n++] = 0x00;
    out[n++] = 0x10;
    out[n++] = 0x00;
    out[n++] = 0x00;
    // sub edx, 0x1000          (6)
    out[n++] = 0x81;
    out[n++] = 0xEA;
    out[n++] = 0x00;
    out[n++] = 0x10;
    out[n++] = 0x00;
    out[n++] = 0x00;
    // jnz .touch               (2)
    out[n++] = 0x75;
    rel = (int32_t)touch_head - (int32_t)(n + 1u);
    if (rel < -128) {
      return 0;  // the loop grew past a short branch; refuse rather than emit a wild jump
    }
    out[n++] = (uint8_t)(int8_t)rel;
    if (split_readonly_size) {
      // Publish the allocation before entering the new syscall, then preserve it with two pushes.
      // The duplicate keeps the ABI's pre-call 16-byte alignment. Do not keep it in an argument
      // register: the live failure in pid 324 showed a started stub with a zero final result, and
      // the target syscall boundary has not proved those registers stable.
      out[n++] = 0x48;  // mov [rip+result],rax
      out[n++] = 0x89;
      out[n++] = 0x05;
      disp = (int32_t)(int64_t)(result - (cave + n + 4u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0x50;  // push rax (allocation)
      out[n++] = 0x50;  // push rax (alignment-preserving duplicate)
      out[n++] = 0x48;  // mov rdi,rax
      out[n++] = 0x89;
      out[n++] = 0xc7;
      out[n++] = 0xbe;  // mov esi,split_readonly_size
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(split_readonly_size >> (8u * i));
      out[n++] = 0xba;  // mov edx,PROT_READ
      out[n++] = GTAV_CAVE_PROT_READ;
      out[n++] = 0;
      out[n++] = 0;
      out[n++] = 0;
      out[n++] = 0xb8;  // mov eax,SYS_mprotect
      out[n++] = (uint8_t)(GTAV_CAVE_SYS_MPROTECT & 0xff);
      out[n++] = (uint8_t)((GTAV_CAVE_SYS_MPROTECT >> 8) & 0xff);
      out[n++] = 0;
      out[n++] = 0;
      // syscall clobbers r11, so reload the only authorized libkernel syscall gadget.
      out[n++] = 0x49;  // mov r11,gadget
      out[n++] = 0xbb;
      for (i = 0; i < 8; ++i) out[n++] = (uint8_t)(gadget >> (8u * i));
      out[n++] = 0x41;  // call r11
      out[n++] = 0xff;
      out[n++] = 0xd3;
      out[n++] = 0x48;  // mov [rip+protect_result],rax
      out[n++] = 0x89;
      out[n++] = 0x05;
      disp = (int32_t)(int64_t)(protect_result - (cave + n + 4u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0x58;  // pop rax (allocation duplicate)
      out[n++] = 0x58;  // pop rax (allocation)
    }
    // .publish -- resolve the three forward branches now that its offset is known.
    for (i = 0; i < skip_fixes; ++i) {
      rel = (int32_t)n - (int32_t)(skip_fix[i] + 1u);
      if (rel > 127) {
        return 0;
      }
      out[skip_fix[i]] = (uint8_t)(int8_t)rel;
    }
    // mov [rip+disp32], rax    (7)
    out[n++] = 0x48;
    out[n++] = 0x89;
    out[n++] = 0x05;
    disp = (int32_t)(int64_t)(result - (cave + n + 4u));
    for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
    // pop rax,r11,r10,r9,r8,rcx,rdx,rsi,rdi  (13)
    out[n++] = 0x58;
    out[n++] = 0x41;
    out[n++] = 0x5B;
    out[n++] = 0x41;
    out[n++] = 0x5A;
    out[n++] = 0x41;
    out[n++] = 0x59;
    out[n++] = 0x41;
    out[n++] = 0x58;
    out[n++] = 0x59;
    out[n++] = 0x5A;
    out[n++] = 0x5E;
    out[n++] = 0x5F;
    if (split_readonly_size) {
      // Only the elected winner reaches this store. All incoming registers have been restored;
      // the remaining cave path is the pinned stolen prologue and indirect continuation jump.
      out[n++] = 0xc6;
      out[n++] = 0x05;
      disp = (int32_t)(int64_t)(completion - (cave + n + 5u));
      for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));
      out[n++] = 0x01;
    }

    if (n != tail_off) {
      return 0;  // layout drift: refuse rather than write a stub whose jne lands mid-instruction
    }

    // The stolen prologue, with its one rip-relative operand recomputed for the cave.
    for (i = 0; i < stolen_len; ++i) {
      out[n + i] = stolen[i];
    }
    if (riprel != NULL) {
      const uint64_t orig_next = target + riprel->offset + riprel->length;
      int32_t orig_disp = 0;
      uint64_t effective;
      uint64_t new_next;
      for (i = 0; i < 4; ++i) {
        orig_disp |=
            (int32_t)((uint32_t)stolen[riprel->offset + riprel->length - 4 + i] << (8u * i));
      }
      effective = orig_next + (uint64_t)(int64_t)orig_disp;
      new_next = cave + n + riprel->offset + riprel->length;
      disp = (int32_t)(int64_t)(effective - new_next);
      for (i = 0; i < 4; ++i) {
        out[n + riprel->offset + riprel->length - 4 + i] = (uint8_t)(disp >> (8u * i));
      }
    }
    n += stolen_len;

    // jmp [rip+disp32] -> continuation. The displacement is computed, never assumed to be zero: the
    // u64 slots are 8-byte aligned, so there may be padding between this instruction and the
    // continuation, and a hardcoded [rip+0] would then jump through the padding instead.
    out[n++] = 0xFF;
    out[n++] = 0x25;
    disp = (int32_t)(int64_t)((uint64_t)data_off - (uint64_t)(n + 4u));
    for (i = 0; i < 4; ++i) out[n++] = (uint8_t)(disp >> (8u * i));

    while (n < data_off) {
      out[n++] = 0x00;
    }
    for (i = 0; i < 8; ++i) out[n++] = (uint8_t)(continuation >> (8u * i));
    for (i = 0; i < 8; ++i) out[n++] = 0x00;  // result
    if (split_readonly_size) {
      // INT64_MIN is not a possible raw syscall result here. The target replaces it only after
      // mprotect returns, so the host can distinguish an in-flight second syscall from success.
      for (i = 0; i < 7; ++i) out[n++] = 0x00;
      out[n++] = 0x80;
      for (i = 0; i < 4; ++i) out[n++] = 0x00;  // atomic entered/claim word
      out[n++] = 0x00;                          // winner completion
    } else {
      out[n++] = 0x00;  // legacy entered byte
    }

    layout->result_addr = result;
    layout->protect_result_addr = protect_result;
    layout->done_addr = done;
    layout->completion_addr = completion;
    layout->tail_addr = cave + tail_off;
    layout->size = n;
  }
  return n;
}

static inline uint32_t gtav_cave_stub_build(uint64_t cave, uint64_t gadget, uint64_t target,
                                            const uint8_t* stolen, uint32_t stolen_len,
                                            uint64_t continuation, uint32_t alloc_size,
                                            const GtavCaveRiprel* riprel, uint8_t* out,
                                            uint32_t out_cap, GtavCaveStubLayout* layout) {
  return gtav_cave_stub_build_split_readonly(cave, gadget, target, stolen, stolen_len, continuation,
                                             alloc_size, 0, riprel, out, out_cap, layout);
}

// The absolute 14-byte jump the loader writes over the target prologue: jmp [rip+0] + the address.
// Returns 14, or 0 if `out_cap` is too small.
static inline uint32_t gtav_cave_jump_build(uint64_t dest, uint8_t* out, uint32_t out_cap) {
  uint32_t i;
  uint32_t n = 0;
  if (out == NULL || out_cap < 14u) {
    return 0;
  }
  out[n++] = 0xFF;
  out[n++] = 0x25;
  out[n++] = 0x00;
  out[n++] = 0x00;
  out[n++] = 0x00;
  out[n++] = 0x00;
  for (i = 0; i < 8; ++i) out[n++] = (uint8_t)(dest >> (8u * i));
  return n;
}

#ifdef __cplusplus
}
#endif
