#include "gtavmenu/notify.h"

#include <stddef.h>
#include <string.h>

typedef struct notify_request {
  // Layout of the kernel's notification request: a 45-byte header (request type, target
  // user, icon uri, ...) that we leave zeroed, followed by the UTF-8 message buffer. The
  // header is required padding -- the kernel reads `message` at this fixed offset, so the
  // size must not change even though we only populate the text.
  char header[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

int gtav_notify(const char* message) {
  notify_request_t req;

  if (!message) {
    message = "";
  }

  memset(&req, 0, sizeof(req));
  strncpy(req.message, message, sizeof(req.message) - 1);
  return sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
