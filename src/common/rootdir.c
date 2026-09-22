#include "gtavmenu/rootdir.h"

#ifdef GTAV_MENU_NO_ROOTDIR

int gtav_rootdir_enter(GtavRootdirGuard* guard) {
  if (guard) {
    guard->active = 0;
  }
  return -1;
}

void gtav_rootdir_leave(GtavRootdirGuard* guard) {
  if (guard) {
    guard->active = 0;
  }
}

#else

#include <ps5/kernel.h>
#include <string.h>
#include <unistd.h>

int gtav_rootdir_enter(GtavRootdirGuard* guard) {
  intptr_t root;

  if (!guard) return -1;
  memset(guard, 0, sizeof(*guard));

  guard->pid = getpid();
  guard->previous_root = kernel_get_proc_rootdir(guard->pid);
  root = kernel_get_root_vnode();
  if (!guard->previous_root || !root) {
    return -1;
  }

  if (kernel_set_proc_rootdir(guard->pid, root) != 0) {
    return -1;
  }

  guard->active = 1;
  return 0;
}

void gtav_rootdir_leave(GtavRootdirGuard* guard) {
  if (!guard || !guard->active) return;
  kernel_set_proc_rootdir(guard->pid, guard->previous_root);
  guard->active = 0;
}

#endif
