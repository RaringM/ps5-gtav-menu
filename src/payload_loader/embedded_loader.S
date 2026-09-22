#ifndef GTAV_EMBEDDED_LOADER_PATH
#error "GTAV_EMBEDDED_LOADER_PATH must name the loader daemon ELF"
#endif

    .section .rodata.gtav_loader,"a",@progbits
    .balign 16
    .global gtav_embedded_loader_start
    .global gtav_embedded_loader_end
gtav_embedded_loader_start:
    .incbin GTAV_EMBEDDED_LOADER_PATH
gtav_embedded_loader_end:
    .section .note.GNU-stack,"",@progbits
