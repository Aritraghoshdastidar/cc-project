#define _GNU_SOURCE
#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <libgen.h>
#include <linux/limits.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct mini_unionfs_state *) fuse_get_context()->private_data)

static void build_full_path(char *out, size_t out_sz, const char *base, const char *path) {
    if (snprintf(out, out_sz, "%s%s", base, path) >= (int)out_sz) {
        if (out_sz > 0) out[0] = '\0';
    }
}

static int build_whiteout_path(const char *path, char *out, size_t out_sz) {
    char upper_path[PATH_MAX];
    build_full_path(upper_path, sizeof(upper_path), UNIONFS_DATA->upper_dir, path);

    char tmp[PATH_MAX];
    strncpy(tmp, upper_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *dir = dirname(tmp);

    char tmp2[PATH_MAX];
    strncpy(tmp2, upper_path, sizeof(tmp2) - 1);
    tmp2[sizeof(tmp2) - 1] = '\0';
    char *base = basename(tmp2);

    if (snprintf(out, out_sz, "%s/.wh.%s", dir, base) >= (int)out_sz) {
        return -ENAMETOOLONG;
    }
    return 0;
}

static int mkdir_p(const char *dir_path, mode_t mode) {
    char tmp[PATH_MAX];
    size_t len;

    if (!dir_path || !*dir_path) return -EINVAL;
    if (strlen(dir_path) >= sizeof(tmp)) return -ENAMETOOLONG;

    strcpy(tmp, dir_path);
    len = strlen(tmp);

    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) == -1 && errno != EEXIST) return -errno;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) == -1 && errno != EEXIST) return -errno;
    return 0;
}

static int ensure_upper_parent_dirs(const char *path) {
    char upper_full[PATH_MAX];
    build_full_path(upper_full, sizeof(upper_full), UNIONFS_DATA->upper_dir, path);

    char tmp[PATH_MAX];
    strncpy(tmp, upper_full, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *parent = dirname(tmp);
    return mkdir_p(parent, 0755);
}

int is_whiteout(const char *path) {
    char wh_path[PATH_MAX];
    int rc = build_whiteout_path(path, wh_path, sizeof(wh_path));
    if (rc < 0) return rc;

    struct stat st;
    if (lstat(wh_path, &st) == 0) return 1;
    if (errno == ENOENT) return 0;
    return -errno;
}

int resolve_path(const char *path, char *resolved_path, int *is_upper) {
    int wh = is_whiteout(path);
    if (wh < 0) return wh;
    if (wh == 1) return -ENOENT;

    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    struct stat st;

    build_full_path(upper_path, sizeof(upper_path), UNIONFS_DATA->upper_dir, path);
    if (lstat(upper_path, &st) == 0) {
        strncpy(resolved_path, upper_path, PATH_MAX - 1);
        resolved_path[PATH_MAX - 1] = '\0';
        if (is_upper) *is_upper = 1;
        return 0;
    }
    if (errno != ENOENT) return -errno;

    build_full_path(lower_path, sizeof(lower_path), UNIONFS_DATA->lower_dir, path);
    if (lstat(lower_path, &st) == 0) {
        strncpy(resolved_path, lower_path, PATH_MAX - 1);
        resolved_path[PATH_MAX - 1] = '\0';
        if (is_upper) *is_upper = 0;
        return 0;
    }
    if (errno != ENOENT) return -errno;

    return -ENOENT;
}

static int copy_file_to_upper(const char *path, mode_t mode) {
    char lower_src[PATH_MAX];
    char upper_dst[PATH_MAX];
    build_full_path(lower_src, sizeof(lower_src), UNIONFS_DATA->lower_dir, path);
    build_full_path(upper_dst, sizeof(upper_dst), UNIONFS_DATA->upper_dir, path);

    int rc = ensure_upper_parent_dirs(path);
    if (rc < 0) return rc;

    int in_fd = open(lower_src, O_RDONLY);
    if (in_fd < 0) return -errno;

    int out_fd = open(upper_dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out_fd < 0) {
        int e = -errno;
        close(in_fd);
        return e;
    }

    char buf[8192];
    ssize_t rd;
    while ((rd = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < rd) {
            ssize_t wr = write(out_fd, buf + off, (size_t)(rd - off));
            if (wr < 0) {
                int e = -errno;
                close(in_fd);
                close(out_fd);
                return e;
            }
            off += wr;
        }
    }

    if (rd < 0) {
        int e = -errno;
        close(in_fd);
        close(out_fd);
        return e;
    }

    close(in_fd);
    if (close(out_fd) < 0) return -errno;
    return 0;
}

static int unionfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    char resolved[PATH_MAX];
    int is_upper = 0;
    int rc = resolve_path(path, resolved, &is_upper);
    (void)is_upper;
    if (rc < 0) return rc;

    if (lstat(resolved, stbuf) == -1) return -errno;
    return 0;
}

struct name_list {
    char **items;
    size_t count;
    size_t cap;
};

static void name_list_free(struct name_list *nl) {
    for (size_t i = 0; i < nl->count; i++) free(nl->items[i]);
    free(nl->items);
    nl->items = NULL;
    nl->count = 0;
    nl->cap = 0;
}

static int name_list_contains(const struct name_list *nl, const char *name) {
    for (size_t i = 0; i < nl->count; i++) {
        if (strcmp(nl->items[i], name) == 0) return 1;
    }
    return 0;
}

static int name_list_add(struct name_list *nl, const char *name) {
    if (name_list_contains(nl, name)) return 0;
    if (nl->count == nl->cap) {
        size_t ncap = (nl->cap == 0) ? 16 : nl->cap * 2;
        char **nitems = realloc(nl->items, ncap * sizeof(char *));
        if (!nitems) return -ENOMEM;
        nl->items = nitems;
        nl->cap = ncap;
    }
    nl->items[nl->count] = strdup(name);
    if (!nl->items[nl->count]) return -ENOMEM;
    nl->count++;
    return 0;
}

static int is_name_whiteouted(const char *dirpath_virtual, const char *name) {
    char vpath[PATH_MAX];
    if (strcmp(dirpath_virtual, "/") == 0) {
        if (snprintf(vpath, sizeof(vpath), "/%s", name) >= (int)sizeof(vpath)) return 0;
    } else {
        if (snprintf(vpath, sizeof(vpath), "%s/%s", dirpath_virtual, name) >= (int)sizeof(vpath)) return 0;
    }
    int w = is_whiteout(vpath);
    return (w == 1);
}

static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t off, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags) {
    (void) off;
    (void) fi;
    (void) flags;

    if (strcmp(path, "/") != 0) {
        char dir_resolved[PATH_MAX];
        int is_upper = 0;
        int rc = resolve_path(path, dir_resolved, &is_upper);
        if (rc < 0) return rc;

        struct stat st;
        if (lstat(dir_resolved, &st) == -1) return -errno;
        if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    }

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    struct name_list merged = {0};

    char upper_dir[PATH_MAX], lower_dir[PATH_MAX];
    build_full_path(upper_dir, sizeof(upper_dir), UNIONFS_DATA->upper_dir, path);
    build_full_path(lower_dir, sizeof(lower_dir), UNIONFS_DATA->lower_dir, path);

    DIR *ud = opendir(upper_dir);
    if (ud) {
        struct dirent *de;
        while ((de = readdir(ud)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;

            int rc = name_list_add(&merged, de->d_name);
            if (rc < 0) {
                closedir(ud);
                name_list_free(&merged);
                return rc;
            }
        }
        closedir(ud);
    } else if (errno != ENOENT) {
        return -errno;
    }

    DIR *ld = opendir(lower_dir);
    if (ld) {
        struct dirent *de;
        while ((de = readdir(ld)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (is_name_whiteouted(path, de->d_name)) continue;

            int rc = name_list_add(&merged, de->d_name);
            if (rc < 0) {
                closedir(ld);
                name_list_free(&merged);
                return rc;
            }
        }
        closedir(ld);
    } else if (errno != ENOENT) {
        name_list_free(&merged);
        return -errno;
    }

    for (size_t i = 0; i < merged.count; i++) {
        filler(buf, merged.items[i], NULL, 0, 0);
    }

    name_list_free(&merged);
    return 0;
}

static int unionfs_open(const char *path, struct fuse_file_info *fi) {
    char resolved[PATH_MAX];
    int is_upper = 0;
    int rc = resolve_path(path, resolved, &is_upper);
    if (rc < 0) return rc;

    int flags = fi->flags;
    bool wants_write = ((flags & O_ACCMODE) != O_RDONLY) || (flags & O_TRUNC) || (flags & O_APPEND);

    if (!is_upper && wants_write) {
        struct stat st;
        if (lstat(resolved, &st) == -1) return -errno;
        if (!S_ISREG(st.st_mode)) return -EISDIR;

        rc = copy_file_to_upper(path, st.st_mode & 0777);
        if (rc < 0) return rc;

        build_full_path(resolved, sizeof(resolved), UNIONFS_DATA->upper_dir, path);
    }

    int fd = open(resolved, flags);
    if (fd < 0) return -errno;

    fi->fh = (uint64_t)fd;
    return 0;
}

static int unionfs_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    if (close((int)fi->fh) == -1) return -errno;
    return 0;
}

static int unionfs_read(const char *path, char *buf, size_t size, off_t offset,
                        struct fuse_file_info *fi) {
    (void) path;
    int fd = (int)fi->fh;
    ssize_t res = pread(fd, buf, size, offset);
    if (res < 0) return -errno;
    return (int)res;
}

static int unionfs_write(const char *path, const char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {
    (void) path;
    int fd = (int)fi->fh;
    ssize_t res = pwrite(fd, buf, size, offset);
    if (res < 0) return -errno;
    return (int)res;
}

static int unionfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int rc = ensure_upper_parent_dirs(path);
    if (rc < 0) return rc;

    char wh[PATH_MAX];
    rc = build_whiteout_path(path, wh, sizeof(wh));
    if (rc < 0) return rc;
    unlink(wh);

    char upath[PATH_MAX];
    build_full_path(upath, sizeof(upath), UNIONFS_DATA->upper_dir, path);

    int fd = open(upath, fi->flags | O_CREAT, mode);
    if (fd < 0) return -errno;
    fi->fh = (uint64_t)fd;
    return 0;
}

static int create_whiteout_for_path(const char *path) {
    int rc = ensure_upper_parent_dirs(path);
    if (rc < 0) return rc;

    char wh[PATH_MAX];
    rc = build_whiteout_path(path, wh, sizeof(wh));
    if (rc < 0) return rc;

    int fd = open(wh, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

static int unionfs_unlink(const char *path) {
    char upath[PATH_MAX], lpath[PATH_MAX];
    struct stat st_up, st_lo;

    build_full_path(upath, sizeof(upath), UNIONFS_DATA->upper_dir, path);
    build_full_path(lpath, sizeof(lpath), UNIONFS_DATA->lower_dir, path);

    bool in_upper = (lstat(upath, &st_up) == 0);
    bool in_lower = (lstat(lpath, &st_lo) == 0);

    if (in_upper) {
        if (unlink(upath) == -1) return -errno;
        if (in_lower) return create_whiteout_for_path(path);
        return 0;
    }

    if (in_lower) {
        return create_whiteout_for_path(path);
    }

    return -ENOENT;
}

static int unionfs_mkdir(const char *path, mode_t mode) {
    int rc = ensure_upper_parent_dirs(path);
    if (rc < 0) return rc;

    char upath[PATH_MAX];
    build_full_path(upath, sizeof(upath), UNIONFS_DATA->upper_dir, path);

    if (mkdir(upath, mode) == -1) return -errno;

    char wh[PATH_MAX];
    rc = build_whiteout_path(path, wh, sizeof(wh));
    if (rc == 0) unlink(wh);

    return 0;
}

static int unionfs_rmdir(const char *path) {
    char upath[PATH_MAX], lpath[PATH_MAX];
    struct stat st_up, st_lo;

    build_full_path(upath, sizeof(upath), UNIONFS_DATA->upper_dir, path);
    build_full_path(lpath, sizeof(lpath), UNIONFS_DATA->lower_dir, path);

    bool in_upper = (lstat(upath, &st_up) == 0) && S_ISDIR(st_up.st_mode);
    bool in_lower = (lstat(lpath, &st_lo) == 0) && S_ISDIR(st_lo.st_mode);

    if (in_upper) {
        if (rmdir(upath) == -1) return -errno;
        if (in_lower) return create_whiteout_for_path(path);
        return 0;
    }

    if (in_lower) {
        return create_whiteout_for_path(path);
    }

    return -ENOENT;
}

static const struct fuse_operations unionfs_oper = {
    .getattr = unionfs_getattr,
    .readdir = unionfs_readdir,
    .open    = unionfs_open,
    .read    = unionfs_read,
    .write   = unionfs_write,
    .create  = unionfs_create,
    .unlink  = unionfs_unlink,
    .mkdir   = unionfs_mkdir,
    .rmdir   = unionfs_rmdir,
    .release = unionfs_release,
};

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <lower_dir> <upper_dir> <mountpoint> [FUSE options]\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }

    struct mini_unionfs_state *state = calloc(1, sizeof(*state));
    if (!state) {
        perror("calloc");
        return 1;
    }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        perror("realpath");
        free(state->lower_dir);
        free(state->upper_dir);
        free(state);
        return 1;
    }

    int fuse_argc = argc - 2;
    char **fuse_argv = calloc((size_t)fuse_argc, sizeof(char *));
    if (!fuse_argv) {
        perror("calloc");
        free(state->lower_dir);
        free(state->upper_dir);
        free(state);
        return 1;
    }

    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[3];
    for (int i = 4; i < argc; i++) {
        fuse_argv[i - 2] = argv[i];
    }

    int ret = fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);

    free(fuse_argv);
    free(state->lower_dir);
    free(state->upper_dir);
    free(state);
    return ret;
}
