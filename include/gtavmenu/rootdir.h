#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GtavRootdirGuard {
  pid_t pid;
  intptr_t previous_root;
  int active;
} GtavRootdirGuard;

int gtav_rootdir_enter(GtavRootdirGuard* guard);
void gtav_rootdir_leave(GtavRootdirGuard* guard);

#ifdef __cplusplus
}
#endif
