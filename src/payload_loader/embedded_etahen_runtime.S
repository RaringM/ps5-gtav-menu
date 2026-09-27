#ifndef GTAV_EMBEDDED_ETAHEN_RUNTIME_PATH
#error "GTAV_EMBEDDED_ETAHEN_RUNTIME_PATH must name the etaHEN runtime plugin"
#endif

    .section .rodata.gtav_etahen_runtime,"a",@progbits
    .balign 16
    .global gtav_embedded_etahen_runtime_start
    .global gtav_embedded_etahen_runtime_end
gtav_embedded_etahen_runtime_start:
    .incbin GTAV_EMBEDDED_ETAHEN_RUNTIME_PATH
gtav_embedded_etahen_runtime_end:
    .section .note.GNU-stack,"",@progbits
