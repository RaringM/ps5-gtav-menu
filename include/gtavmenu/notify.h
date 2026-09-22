#pragma once

// On-screen system notification (the PS5 toast / blue banner), posted via the kernel.

#ifdef __cplusplus
extern "C" {
#endif

// Post a system notification carrying `message`. Returns the RAW kernel result of
// sceKernelSendNotificationRequest (0 on success, negative kernel error otherwise).
// NOTE: a deliberate exception to the project's 0/-1 convention -- the value is the
// unwrapped kernel code, not normalized to -1.
int gtav_notify(const char* message);

#ifdef __cplusplus
}
#endif
