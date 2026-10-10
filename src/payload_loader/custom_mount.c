// Loader-side profile storage mount support. Every loader-created sandbox nullfs operation
// remains quarantined; custom assets require the game to see /data directly. The loader never
// copies custom assets into a sandbox or creates a fallback storage root.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include "gtavmenu/custom_mount.h"
#include "gtavmenu/log.h"
#include "gtavmenu/profile_storage.h"

#ifndef SANDBOX_ROOT
#define SANDBOX_ROOT "/mnt/sandbox"
#endif
#define MAX_SANDBOXES 64
#define SANDBOX_NAME_MAX 64

#define IOV(s) {(void*)(s), strlen(s) + 1}

typedef struct {
  char storage[MAX_SANDBOXES][SANDBOX_NAME_MAX];
  const char* names[MAX_SANDBOXES];
  int live[MAX_SANDBOXES];
  int count;
} SandboxList;

// Hardware 2026-10-04: leaving a loader-created nullfs mount inside GTA's sandbox until normal
// title teardown corrupted the sandbox walk. LncService descended through the retained namespace
// into other system sandboxes, ShellUI lost /user/appmeta, then crashed and could not remount
// /mnt/rnps. Keep every loader-created sandbox nullfs operation fail-closed until the loader owns
// a proven pre-ProcessTerm unmount signal. This is deliberately runtime-visible so the quarantined
// profile mount remains build-checked while no build flag can accidentally re-enable it.
static int sandbox_nullfs_allowed(void) {
  return 0;
}

static int list_sandboxes(SandboxList* list) {
  struct dirent* e;
  DIR* d = opendir(SANDBOX_ROOT);

  list->count = 0;
  if (!d) return -1;
  while ((e = readdir(d)) != NULL && list->count < MAX_SANDBOXES) {
    const size_t len = strlen(e->d_name);
    if (len >= SANDBOX_NAME_MAX) continue;
    memcpy(list->storage[list->count], e->d_name, len + 1u);
    list->names[list->count] = list->storage[list->count];
    list->live[list->count] = 0;
    list->count++;
  }
  closedir(d);
  // A running title's sandbox holds its app0; one kept alive only by our mount does not.
  for (int i = 0; i < list->count; ++i) {
    char app0[MAXPATHLEN];
    struct stat st;
    if (snprintf(app0, sizeof(app0), "%s/%s/app0", SANDBOX_ROOT, list->names[i]) <
            (int)sizeof(app0) &&
        stat(app0, &st) == 0) {
      list->live[i] = 1;
    }
  }
  return 0;
}

static int is_mount_from(const char* point, const char* source, int require_writable) {
  struct statfs st;

  if (statfs(point, &st) != 0) return 0;
  if (strcmp(st.f_fstypename, "nullfs") != 0 || strcmp(st.f_mntonname, point) != 0 ||
      strcmp(st.f_mntfromname, source) != 0) {
    return 0;
  }
  return !require_writable || !(st.f_flags & MNT_RDONLY);
}

// Unmount our nullfs mount at `point` (if it is ours) and remove the mountpoint directory.
static void remove_point(const char* point, const char* source, const char* label) {
  if (is_mount_from(point, source, 0)) {
    if (unmount(point, 0) != 0 && unmount(point, MNT_FORCE) != 0) {
      gtav_logf("%s mount: unmount %s failed: %s", label, point, strerror(errno));
      return;
    }
    gtav_logf("%s mount: removed mount %s", label, point);
  }
  rmdir(point);
}

// `sandbox` + `game_root`, or the prefix of it ending `levels` components from the end
// (levels 0 = the mountpoint itself, 1 = its parent, ...).
static int sandbox_path(const char* sandbox, const char* game_root, int levels, char* out,
                        size_t size) {
  if (snprintf(out, size, "%s%s", sandbox, game_root) >= (int)size) return -1;
  for (int i = 0; i < levels; ++i) {
    char* slash = strrchr(out, '/');
    if (!slash || slash == out) return -1;
    *slash = '\0';
  }
  return 0;
}

static int create_mount_parents(const char* sandbox, const char* game_root) {
  char dir[MAXPATHLEN];

  // All current mount roots are three components below the sandbox.
  for (int level = 2; level >= 0; --level) {
    if (sandbox_path(sandbox, game_root, level, dir, sizeof(dir)) != 0) return -1;
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) return -1;
  }
  return 0;
}

int gtav_profile_storage_mount(const char* title) {
  SandboxList list;
  char newest[SANDBOX_NAME_MAX];
  char sandbox[MAXPATHLEN];
  char point[MAXPATHLEN];
  struct stat st;

  if (!sandbox_nullfs_allowed()) {
    (void)title;
    gtav_logf("profile mount: quarantined until pre-exit unmount is proven");
    return -1;
  }

  if (list_sandboxes(&list) != 0) {
    gtav_logf("profile mount: cannot list %s: %s", SANDBOX_ROOT, strerror(errno));
    return -1;
  }
  if (gtav_custom_pick_sandbox(list.names, list.live, list.count, title, newest, sizeof(newest)) !=
      0) {
    gtav_logf("profile mount: no sandbox for %s under %s", title, SANDBOX_ROOT);
    return -1;
  }
  for (int i = 0; i < list.count; ++i) {
    if (!gtav_custom_sandbox_index(list.names[i], title, NULL)) continue;
    if (snprintf(sandbox, sizeof(sandbox), "%s/%s", SANDBOX_ROOT, list.names[i]) >=
        (int)sizeof(sandbox)) {
      continue;
    }
    if (strcmp(list.names[i], newest) != 0 &&
        sandbox_path(sandbox, GTAV_PROFILE_STORAGE_ROOT, 0, point, sizeof(point)) == 0) {
      remove_point(point, GTAV_PROFILE_STORAGE_ROOT, "profile");
    }
  }

  if (snprintf(sandbox, sizeof(sandbox), "%s/%s", SANDBOX_ROOT, newest) >= (int)sizeof(sandbox) ||
      sandbox_path(sandbox, GTAV_PROFILE_STORAGE_ROOT, 0, point, sizeof(point)) != 0) {
    return -1;
  }
  if (is_mount_from(point, GTAV_PROFILE_STORAGE_ROOT, 1)) {
    gtav_logf("profile mount: %s already mounted read-write", point);
    return 0;
  }
  mkdir("/data", 0777);
  mkdir("/data/GTAVMenu", 0777);
  mkdir(GTAV_PROFILE_STORAGE_ROOT, 0777);
  if (stat(GTAV_PROFILE_STORAGE_ROOT, &st) != 0 || !S_ISDIR(st.st_mode)) {
    gtav_logf("profile mount: %s is not a directory", GTAV_PROFILE_STORAGE_ROOT);
    return -1;
  }
  if (chmod(GTAV_PROFILE_STORAGE_ROOT, 0777) != 0) {
    gtav_logf("profile mount: chmod %s failed: %s", GTAV_PROFILE_STORAGE_ROOT, strerror(errno));
    return -1;
  }
  if (access(GTAV_PROFILE_STORAGE_PATH, F_OK) != 0 &&
      access(GTAV_PROFILE_STORAGE_LEGACY_PATH, F_OK) == 0) {
    if (rename(GTAV_PROFILE_STORAGE_LEGACY_PATH, GTAV_PROFILE_STORAGE_PATH) != 0) {
      gtav_logf("profile mount: could not migrate legacy profile: %s", strerror(errno));
    } else {
      gtav_logf("profile mount: migrated legacy profile to %s", GTAV_PROFILE_STORAGE_PATH);
    }
  }
  if (access(GTAV_PROFILE_STORAGE_PATH, F_OK) == 0 && chmod(GTAV_PROFILE_STORAGE_PATH, 0666) != 0) {
    gtav_logf("profile mount: chmod %s failed: %s", GTAV_PROFILE_STORAGE_PATH, strerror(errno));
    return -1;
  }
  if (create_mount_parents(sandbox, GTAV_PROFILE_STORAGE_ROOT) != 0) {
    gtav_logf("profile mount: mkdir %s failed: %s", point, strerror(errno));
    return -1;
  }
  {
    struct iovec iov[] = {
        IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(GTAV_PROFILE_STORAGE_ROOT),
        IOV("fspath"), IOV(point),
    };
    if (nmount(iov, sizeof(iov) / sizeof(iov[0]), 0) != 0) {
      gtav_logf("profile mount: nullfs %s -> %s failed: %s", GTAV_PROFILE_STORAGE_ROOT, point,
                strerror(errno));
      rmdir(point);
      return -1;
    }
  }
  if (!is_mount_from(point, GTAV_PROFILE_STORAGE_ROOT, 1)) {
    gtav_logf("profile mount: %s mounted but does not read back as ours", point);
    unmount(point, MNT_FORCE);
    rmdir(point);
    return -1;
  }
  gtav_logf("profile mount: mounted %s read-write at %s (game path %s)", GTAV_PROFILE_STORAGE_ROOT,
            point, GTAV_PROFILE_STORAGE_ROOT);
  return 0;
}
