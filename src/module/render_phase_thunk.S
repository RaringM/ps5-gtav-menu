// Exact group-2 callback-object wrapper for the isolated phase experiment. SysV x86-64.
// Preserve the interrupted red zone, all GPRs/RFLAGS, x87/MMX/MXCSR and all 16 YMMs. The helper
// draws before the original script-update task. After restoring exact entry state, tail-jump to
// the original so exceptions never unwind through this image and the caller keeps its return PC.
#if GTAV_RENDER_PHASE_INTERCEPT && defined(__x86_64__)
#ifdef __APPLE__
#define SYM(x) _##x
#else
#define SYM(x) x
#endif
.text
.p2align 4
.globl SYM(gtav_render_phase_wrapper)
SYM(gtav_render_phase_wrapper):
  leaq -128(%rsp), %rsp
  pushfq
  pushq %rax
  pushq %rcx
  pushq %rdx
  pushq %rbx
  pushq %rbp
  pushq %rsi
  pushq %rdi
  pushq %r8
  pushq %r9
  pushq %r10
  pushq %r11
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  movq %rsp, %r12
  andq $-64, %rsp
  subq $1024, %rsp
  fxsave64 (%rsp)
  vmovdqu %ymm0, 512(%rsp)
  vmovdqu %ymm1, 544(%rsp)
  vmovdqu %ymm2, 576(%rsp)
  vmovdqu %ymm3, 608(%rsp)
  vmovdqu %ymm4, 640(%rsp)
  vmovdqu %ymm5, 672(%rsp)
  vmovdqu %ymm6, 704(%rsp)
  vmovdqu %ymm7, 736(%rsp)
  vmovdqu %ymm8, 768(%rsp)
  vmovdqu %ymm9, 800(%rsp)
  vmovdqu %ymm10, 832(%rsp)
  vmovdqu %ymm11, 864(%rsp)
  vmovdqu %ymm12, 896(%rsp)
  vmovdqu %ymm13, 928(%rsp)
  vmovdqu %ymm14, 960(%rsp)
  vmovdqu %ymm15, 992(%rsp)
  cld
  movq 64(%r12), %rdi
  call SYM(gtav_render_phase_wrapper_body)
  vmovdqu 512(%rsp), %ymm0
  vmovdqu 544(%rsp), %ymm1
  vmovdqu 576(%rsp), %ymm2
  vmovdqu 608(%rsp), %ymm3
  vmovdqu 640(%rsp), %ymm4
  vmovdqu 672(%rsp), %ymm5
  vmovdqu 704(%rsp), %ymm6
  vmovdqu 736(%rsp), %ymm7
  vmovdqu 768(%rsp), %ymm8
  vmovdqu 800(%rsp), %ymm9
  vmovdqu 832(%rsp), %ymm10
  vmovdqu 864(%rsp), %ymm11
  vmovdqu 896(%rsp), %ymm12
  vmovdqu 928(%rsp), %ymm13
  vmovdqu 960(%rsp), %ymm14
  vmovdqu 992(%rsp), %ymm15
  fxrstor64 (%rsp)
  movq %r12, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %r11
  popq %r10
  popq %r9
  popq %r8
  popq %rdi
  popq %rsi
  popq %rbp
  popq %rbx
  popq %rdx
  popq %rcx
  popq %rax
  popfq
  leaq 128(%rsp), %rsp
  jmp *SYM(gtav_render_phase_original)(%rip)
#endif
#ifndef __APPLE__
.section .note.GNU-stack,"",@progbits
#endif
