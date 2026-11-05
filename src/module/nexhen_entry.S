.text
.macro GTAV_NX_ENTRY public, implementation
    .p2align 4
    .globl \public
    .type \public,@function
\public:
    lea \implementation(%rip), %r11
    jmp gtav_nexhen_entry_guard
    .size \public, .-\public
.endm
GTAV_NX_ENTRY plugin_load, gtav_nexhen_plugin_load
GTAV_NX_ENTRY plugin_unload, gtav_nexhen_plugin_unload
GTAV_NX_ENTRY _init, gtav_nexhen_module_init
GTAV_NX_ENTRY _fini, gtav_nexhen_module_fini
.hidden _init
.hidden _fini

.type gtav_nexhen_entry_guard,@function
gtav_nexhen_entry_guard:
    lea -128(%rsp), %rsp
    pushfq
    cld
    push %rbp
    mov %rsp, %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rdi
    push %rsi
    push %rdx
    push %r11
    xor %r14d, %r14d
    mov $1, %eax
    cpuid
    and $0x0c000000, %ecx
    cmp $0x0c000000, %ecx
    jne .Lgtav_fx
    xor %ecx, %ecx
    xgetbv
    mov %eax, %r12d
    mov %edx, %r13d
    mov $13, %eax
    xor %ecx, %ecx
    cpuid
    cmp $16384, %ebx
    ja .Lgtav_unsupported
    cmp $576, %ebx
    jb .Lgtav_unsupported
    mov $1, %r14d
    jmp .Lgtav_allocate
.Lgtav_fx:
    mov $512, %ebx
.Lgtav_allocate:
    sub %rbx, %rsp
    and $-64, %rsp
    mov %rsp, %r15
    mov %rsp, %rdi
    mov %ebx, %ecx
    xor %eax, %eax
    rep stosb
    test %r14d, %r14d
    jz .Lgtav_savefx
    mov %r12d, %eax
    mov %r13d, %edx
    xsave64 (%r15)
    jmp .Lgtav_invoke
.Lgtav_savefx:
    fxsave64 (%r15)
.Lgtav_invoke:
    mov -48(%rbp), %rdi
    mov -56(%rbp), %rsi
    mov -64(%rbp), %rdx
    call *-72(%rbp)
    mov %eax, %ebx
    test %r14d, %r14d
    jz .Lgtav_restorefx
    mov %r12d, %eax
    mov %r13d, %edx
    xrstor64 (%r15)
    jmp .Lgtav_result
.Lgtav_restorefx:
    fxrstor64 (%r15)
    jmp .Lgtav_result
.Lgtav_unsupported:
    mov $-1, %ebx
.Lgtav_result:
    mov %ebx, %eax
    lea -40(%rbp), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    popfq
    lea 128(%rsp), %rsp
    ret
.size gtav_nexhen_entry_guard, .-gtav_nexhen_entry_guard
.section .note.GNU-stack,"",@progbits
