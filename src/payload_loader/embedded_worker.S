#ifndef GTAV_EMBEDDED_WORKER_PATH
#error "GTAV_EMBEDDED_WORKER_PATH must name the worker ELF"
#endif

    .section .rodata.gtav_worker,"a",@progbits
    .balign 16
    .global gtav_embedded_worker_start
    .global gtav_embedded_worker_end
gtav_embedded_worker_start:
    .incbin GTAV_EMBEDDED_WORKER_PATH
gtav_embedded_worker_end:
    .section .note.GNU-stack,"",@progbits
