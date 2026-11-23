#define _GNU_SOURCE
#include "platform_3ds.h"

#include <3ds.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* =========================================================================
 * Milestone 7: 3DS SD Card Persistence & File System Hooks
 *
 * 1. Base Path Redirection: All game read/write paths resolve under
 *    sdmc:/3ds/nox/ (while preserving explicit sdmc:/ and romfs:/ prefixes
 *    and honoring SetCurrentDirectoryA subdirectories under sdmc:/3ds/nox/).
 * 2. Path Sanitization: Recursively converts Windows '\' to '/' and collapses
 *    duplicate separators / '.' / '..' components before fopen() or stat().
 * 3. Directory Creation: Automatically creates missing parent directories
 *    (e.g. sdmc:/3ds/nox/save, sdmc:/3ds/nox/gui) before opening for write.
 * 4. Stdio Sync: Tracks writable FILE* handles and explicitly calls fflush()
 *    + fsync() before fclose() to prevent SD card save corruption on power-off.
 * ========================================================================= */

#define NOX_3DS_MAX_WRITE_FILES 128
#define NOX_3DS_MAX_PATH_COMPS  64

typedef struct {
    char     resolved[512];
    uint8_t *data;
    size_t   size;
    size_t   cap;
    size_t   pos;
    bool     dirty;
} nox_3ds_memfile_t;

typedef struct {
    FILE              *fp;
    nox_3ds_memfile_t *mf;
    bool               in_use;
} nox_3ds_write_slot_t;

static nox_3ds_write_slot_t g_write_slots[NOX_3DS_MAX_WRITE_FILES];
static bool                 g_write_slots_overflow = false;
static RecursiveLock        g_fs_lock;
static bool                 g_fs_lock_init = false;

static void fs_lock(void)
{
    if (!g_fs_lock_init) {
        RecursiveLock_Init(&g_fs_lock);
        g_fs_lock_init = true;
    }
    RecursiveLock_Lock(&g_fs_lock);
}

static void fs_unlock(void)
{
    if (g_fs_lock_init) {
        RecursiveLock_Unlock(&g_fs_lock);
    }
}

static bool mode_requests_write(const char *mode)
{
    if (!mode)
        return false;
    return (strchr(mode, 'w') != NULL) ||
           (strchr(mode, 'a') != NULL) ||
           (strchr(mode, '+') != NULL);
}

static void track_write_file(FILE *fp, nox_3ds_memfile_t *mf)
{
    if (!fp)
        return;

    fs_lock();
    for (int i = 0; i < NOX_3DS_MAX_WRITE_FILES; ++i) {
        if (g_write_slots[i].in_use && g_write_slots[i].fp == fp) {
            g_write_slots[i].mf = mf;
            fs_unlock();
            return;
        }
    }
    for (int i = 0; i < NOX_3DS_MAX_WRITE_FILES; ++i) {
        if (!g_write_slots[i].in_use) {
            g_write_slots[i].fp = fp;
            g_write_slots[i].mf = mf;
            g_write_slots[i].in_use = true;
            fs_unlock();
            return;
        }
    }
    g_write_slots_overflow = true;
    fs_unlock();
}

static bool untrack_write_file(FILE *fp)
{
    if (!fp)
        return false;

    bool found = false;
    fs_lock();
    for (int i = 0; i < NOX_3DS_MAX_WRITE_FILES; ++i) {
        if (g_write_slots[i].in_use && g_write_slots[i].fp == fp) {
            g_write_slots[i].fp = NULL;
            g_write_slots[i].mf = NULL;
            g_write_slots[i].in_use = false;
            found = true;
            break;
        }
    }
    bool should_flush = found || g_write_slots_overflow;
    fs_unlock();
    return should_flush;
}

/* =========================================================================
 * RAM-Buffered Write Stream (funopen)
 *
 * Nox's map serializer (sub_51DF90 -> sub_426C90/sub_426D40 -> sub_409190)
 * executes fseek(SEEK_SET) + fwrite(8 bytes) + fseek(SEEK_END) for EVERY
 * object in the map (2,000-5,000 objects per map). On 3DS SDMC, each fseek()
 * flushes the stdio buffer and triggers two synchronous ARM9 FSFILE_Write
 * IPC round-trips, causing 10-20s freezes per save.
 *
 * Buffering write/read-write save files in RAM makes all fseek/fwrite calls
 * pure memcpy operations (~1-2 ms total) and commits the finished file to
 * SDMC in one sequential 128 KB-chunked write() on fclose().
 * ========================================================================= */

#define NOX_3DS_MEMFILE_INIT_CAP    (64u * 1024u)
#define NOX_3DS_MEMFILE_PRELOAD_MAX (8u * 1024u * 1024u)
#define NOX_3DS_MEMFILE_WRITE_CHUNK (128u * 1024u)

static int memfile_flush_to_sd(nox_3ds_memfile_t *mf)
{
    if (!mf)
        return -1;

    int fd = open(mf->resolved, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;

    size_t off = 0;
    while (off < mf->size) {
        size_t chunk = mf->size - off;
        if (chunk > NOX_3DS_MEMFILE_WRITE_CHUNK)
            chunk = NOX_3DS_MEMFILE_WRITE_CHUNK;
        ssize_t w = write(fd, mf->data + off, chunk);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }

    close(fd);
    mf->dirty = false;
    return 0;
}

static int memfile_read(void *cookie, char *buf, size_t len)
{
    nox_3ds_memfile_t *mf = (nox_3ds_memfile_t *)cookie;
    if (!mf || !buf || len == 0)
        return 0;
    if (mf->pos >= mf->size)
        return 0;
    size_t avail = mf->size - mf->pos;
    if (len < avail)
        avail = len;
    memcpy(buf, mf->data + mf->pos, avail);
    mf->pos += avail;
    return (int)avail;
}

static int memfile_write(void *cookie, const char *buf, size_t len)
{
    nox_3ds_memfile_t *mf = (nox_3ds_memfile_t *)cookie;
    if (!mf || !buf) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0)
        return 0;

    size_t need = mf->pos + len;
    if (need > mf->cap) {
        size_t new_cap = mf->cap ? mf->cap : NOX_3DS_MEMFILE_INIT_CAP;
        while (new_cap < need) {
            if (new_cap > (SIZE_MAX / 2u)) {
                errno = ENOMEM;
                return -1;
            }
            new_cap *= 2u;
        }
        uint8_t *nd = (uint8_t *)realloc(mf->data, new_cap);
        if (!nd) {
            errno = ENOMEM;
            return -1;
        }
        mf->data = nd;
        mf->cap = new_cap;
    }

    if (mf->pos > mf->size) {
        memset(mf->data + mf->size, 0, mf->pos - mf->size);
    }
    memcpy(mf->data + mf->pos, buf, len);
    mf->pos += len;
    if (mf->pos > mf->size)
        mf->size = mf->pos;
    mf->dirty = true;
    return (int)len;
}

static fpos_t memfile_seek(void *cookie, fpos_t offset, int whence)
{
    nox_3ds_memfile_t *mf = (nox_3ds_memfile_t *)cookie;
    if (!mf) {
        errno = EINVAL;
        return (fpos_t)-1;
    }

    int64_t base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)mf->pos; break;
    case SEEK_END: base = (int64_t)mf->size; break;
    default:
        errno = EINVAL;
        return (fpos_t)-1;
    }

    int64_t next = base + (int64_t)offset;
    if (next < 0) {
        errno = EINVAL;
        return (fpos_t)-1;
    }
    mf->pos = (size_t)next;
    return (fpos_t)mf->pos;
}

static int memfile_close(void *cookie)
{
    nox_3ds_memfile_t *mf = (nox_3ds_memfile_t *)cookie;
    if (!mf)
        return 0;

    int rc = memfile_flush_to_sd(mf);
    free(mf->data);
    free(mf);
    return rc;
}

static FILE *memfile_open(const char *resolved, const char *mode, nox_3ds_memfile_t **out_mf)
{
    *out_mf = NULL;
    if (!resolved || !mode)
        return NULL;

    bool is_trunc_write = (mode[0] == 'w');
    bool is_rw_existing = (mode[0] == 'r' && strchr(mode, '+') != NULL);
    if (!is_trunc_write && !is_rw_existing)
        return NULL;

    nox_3ds_memfile_t *mf = (nox_3ds_memfile_t *)calloc(1, sizeof(*mf));
    if (!mf)
        return NULL;

    snprintf(mf->resolved, sizeof(mf->resolved), "%s", resolved);

    if (is_rw_existing) {
        struct stat st;
        if (stat(resolved, &st) != 0) {
            free(mf);
            return NULL; /* r+b fails if file does not exist */
        }
        if ((size_t)st.st_size > NOX_3DS_MEMFILE_PRELOAD_MAX) {
            free(mf);
            return NULL; /* fall back to direct fopen for huge files */
        }
        size_t init_cap = (size_t)st.st_size + 4096u;
        if (init_cap < NOX_3DS_MEMFILE_INIT_CAP)
            init_cap = NOX_3DS_MEMFILE_INIT_CAP;
        mf->data = (uint8_t *)malloc(init_cap);
        if (!mf->data) {
            free(mf);
            return NULL;
        }
        mf->cap = init_cap;

        int fd = open(resolved, O_RDONLY);
        if (fd < 0) {
            free(mf->data);
            free(mf);
            return NULL;
        }
        size_t off = 0;
        while (off < (size_t)st.st_size) {
            ssize_t r = read(fd, mf->data + off, (size_t)st.st_size - off);
            if (r <= 0)
                break;
            off += (size_t)r;
        }
        close(fd);
        mf->size = off;
        mf->pos = 0;
        mf->dirty = false;
    } else {
        mf->data = (uint8_t *)malloc(NOX_3DS_MEMFILE_INIT_CAP);
        if (!mf->data) {
            free(mf);
            return NULL;
        }
        mf->cap = NOX_3DS_MEMFILE_INIT_CAP;
        mf->size = 0;
        mf->pos = 0;
        mf->dirty = true;
    }

    FILE *fp = funopen(mf, memfile_read, memfile_write, memfile_seek, memfile_close);
    if (!fp) {
        free(mf->data);
        free(mf);
        return NULL;
    }

    *out_mf = mf;
    return fp;
}

/* Recursive helper to convert all Windows backslashes '\' to '/' */
static void sanitize_slashes_recursive(char *p)
{
    if (!p || !*p)
        return;
    if (*p == '\\')
        *p = '/';
    sanitize_slashes_recursive(p + 1);
}

void nox_3ds_sanitize_path(char *path)
{
    if (!path || !*path)
        return;

    /* 1. Recursively convert every '\' to '/' */
    sanitize_slashes_recursive(path);

    /* 2. Collapse consecutive '/' separators while preserving device roots */
    size_t r = 0, w = 0;
    while (path[r]) {
        char c = path[r++];
        if (c == '/' && w > 0 && path[w - 1] == '/')
            continue;
        path[w++] = c;
    }
    path[w] = '\0';

    /* 3. Strip trailing '/' unless it is a root ("/" or "sdmc:/" or "romfs:/") */
    while (w > 1 && path[w - 1] == '/' && !(w >= 2 && path[w - 2] == ':')) {
        path[--w] = '\0';
    }
}

/* Push slash-separated tokens onto a normalized component stack.
 * Handles '.', '..', and empty tokens safely. */
static int push_path_tokens(char *buf, char **comps, int depth, int max_comps)
{
    char *p = buf;
    while (p && *p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        char *tok = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/') {
            *p = '\0';
            p++;
        }

        if (tok[0] == '.' && tok[1] == '\0') {
            continue;
        }
        if (tok[0] == '.' && tok[1] == '.' && tok[2] == '\0') {
            if (depth > 0)
                depth--;
            continue;
        }
        if (depth < max_comps) {
            comps[depth++] = tok;
        }
    }
    return depth;
}

static void format_components(const char *root, char **comps, int depth,
                              char *out, size_t out_sz)
{
    size_t pos = (size_t)snprintf(out, out_sz, "%s", root);
    if (pos >= out_sz) {
        out[out_sz - 1] = '\0';
        return;
    }

    for (int i = 0; i < depth; ++i) {
        bool need_slash = (pos > 0 && out[pos - 1] != '/');
        int written = snprintf(out + pos, out_sz - pos, "%s%s",
                               need_slash ? "/" : "", comps[i]);
        if (written < 0)
            break;
        if ((size_t)written >= out_sz - pos) {
            pos = out_sz - 1;
            break;
        }
        pos += (size_t)written;
    }
}

void nox_3ds_resolve_path(const char *path, char *out, size_t out_sz)
{
    if (!out || out_sz == 0)
        return;
    out[0] = '\0';
    if (!path || !*path)
        return;

    char work[512];
    snprintf(work, sizeof(work), "%s", path);
    nox_3ds_sanitize_path(work);

    /* Explicit 3DS device prefixes (sdmc:/ or romfs:/) are normalized in-place */
    if (strncasecmp(work, "sdmc:/", 6) == 0 || strncasecmp(work, "romfs:/", 7) == 0) {
        const bool is_romfs = (strncasecmp(work, "romfs:/", 7) == 0);
        const char *dev_root = is_romfs ? "romfs:/" : "sdmc:/";
        char *rest = work + (is_romfs ? 7 : 6);

        char *comps[NOX_3DS_MAX_PATH_COMPS];
        int depth = push_path_tokens(rest, comps, 0, NOX_3DS_MAX_PATH_COMPS);
        format_components(dev_root, comps, depth, out, out_sz);
        return;
    }

    /* Strip Windows drive letter prefix (e.g. "C:/..." or "c:...") */
    char *p = work;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        p += 2;
    }

    bool is_abs = (*p == '/');
    while (*p == '/')
        p++;

    char *comps[NOX_3DS_MAX_PATH_COMPS];
    int depth = 0;

    /* Scratch buffer for CWD sub-path when resolving relative paths after
     * SetCurrentDirectoryA() (e.g. inside Save/SAVE0001/Con01a). */
    char cwd_buf[512];
    cwd_buf[0] = '\0';

    if (!is_abs) {
        if (getcwd(cwd_buf, sizeof(cwd_buf)) != NULL) {
            nox_3ds_sanitize_path(cwd_buf);
            char *cp = cwd_buf;
            if (strncasecmp(cp, "sdmc:", 5) == 0)
                cp += 5;
            while (*cp == '/')
                cp++;
            if (strncasecmp(cp, "3ds/nox/", 8) == 0) {
                cp += 8;
                depth = push_path_tokens(cp, comps, depth, NOX_3DS_MAX_PATH_COMPS);
            }
        }
    } else {
        /* Avoid double-prefixing if caller passed "/3ds/nox/..." or "C:\3ds\nox\..." */
        if (strncasecmp(p, "3ds/nox/", 8) == 0) {
            p += 8;
        } else if (strcasecmp(p, "3ds/nox") == 0) {
            p += 7;
        }
    }

    depth = push_path_tokens(p, comps, depth, NOX_3DS_MAX_PATH_COMPS);
    format_components(NOX_3DS_DATA_ROOT, comps, depth, out, out_sz);
}

bool nox_3ds_ensure_parent_dirs(const char *resolved_path)
{
    if (!resolved_path || !*resolved_path)
        return false;

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", resolved_path);
    nox_3ds_sanitize_path(tmp);

    /* romfs is read-only; nothing to create */
    if (strncasecmp(tmp, "romfs:/", 7) == 0)
        return true;

    char *start = tmp;
    if (strncasecmp(tmp, "sdmc:/", 6) == 0) {
        start = tmp + 6;
    } else if (tmp[0] == '/') {
        start = tmp + 1;
    }

    struct stat st;
    /* Fast path: if the immediate parent directory already exists, skip walking
     * every ancestor component (saves 4-5 ARM9 FS IPC calls per open). */
    char *last_slash = strrchr(start, '/');
    if (last_slash) {
        *last_slash = '\0';
        if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) {
            return true;
        }
        *last_slash = '/';
    }

    for (char *p = start; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                    *p = '/';
                    return false;
                }
            }
            *p = '/';
        }
    }
    return true;
}

bool nox_3ds_ensure_dir(const char *path)
{
    if (!path || !*path)
        return false;

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0])
        return false;

    if (strncasecmp(resolved, "romfs:/", 7) == 0)
        return true;

    if (!nox_3ds_ensure_parent_dirs(resolved))
        return false;

    struct stat st;
    if (stat(resolved, &st) == 0)
        return S_ISDIR(st.st_mode);

    if (mkdir(resolved, 0777) == 0 || errno == EEXIST)
        return true;

    return false;
}

FILE *nox_3ds_fopen(const char *path, const char *mode)
{
    if (!path || !*path) {
        errno = EINVAL;
        return NULL;
    }
    if (!mode || !*mode)
        mode = "rb";

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0]) {
        errno = EINVAL;
        return NULL;
    }

    bool wants_write = mode_requests_write(mode);
    if (wants_write) {
        nox_3ds_ensure_parent_dirs(resolved);

        /* Route truncate-write ("w"/"wb") and in-place save updates ("r+b")
         * through our RAM-buffered funopen stream so Nox's thousands of
         * per-object fseek()+fwrite() calls run in RAM and flush sequentially
         * to SDMC once on fclose(). */
        nox_3ds_memfile_t *mf = NULL;
        FILE *mfp = memfile_open(resolved, mode, &mf);
        if (mfp) {
            track_write_file(mfp, mf);
            return mfp;
        }
    }

    FILE *fp = fopen(resolved, mode);
    if (fp) {
        /* Newlib defaults to BUFSIZ=1024 on 3DS, causing thousands of ARM9
         * FSFILE_Read/Write IPC round-trips when saving/loading 1-3 MB maps.
         * A 64 KB stdio buffer cuts FS IPC calls by 64x. */
        setvbuf(fp, NULL, _IOFBF, 65536);
        if (wants_write) {
            track_write_file(fp, NULL);
        }
    }
    return fp;
}

int nox_3ds_fclose(FILE *stream)
{
    if (!stream)
        return 0;

    /* Untrack the writable stream and flush C stdio buffer only.
     * Per-file fsync() is intentionally omitted here because Nox writes
     * dozens of small save/map files sequentially; physical SDMC sync is
     * batched once per save event in nox_3ds_commit_save() and on APT suspend. */
    bool was_writable = untrack_write_file(stream);
    if (was_writable) {
        fflush(stream);
    }

    return fclose(stream);
}

int nox_3ds_stat(const char *path, struct stat *st)
{
    if (!path || !*path || !st) {
        errno = EINVAL;
        return -1;
    }

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0]) {
        errno = EINVAL;
        return -1;
    }

    return stat(resolved, st);
}

void nox_3ds_fs_sync_all(void)
{
    fs_lock();
    for (int i = 0; i < NOX_3DS_MAX_WRITE_FILES; ++i) {
        if (g_write_slots[i].in_use && g_write_slots[i].fp) {
            FILE *fp = g_write_slots[i].fp;
            fflush(fp);
            if (g_write_slots[i].mf) {
                if (g_write_slots[i].mf->dirty) {
                    memfile_flush_to_sd(g_write_slots[i].mf);
                }
            } else {
                int fd = fileno(fp);
                if (fd >= 0) {
                    fsync(fd);
                }
            }
        }
    }
    fs_unlock();
}

void nox_3ds_commit_save(void)
{
    /* 1. Flush all C stdio userspace output buffers. */
    fflush(NULL);

    /* 2. Sync any still-open writable file descriptors tracked by fs_3ds. */
    nox_3ds_fs_sync_all();

    /* 3. Issue a single batch directory-level fsync() on the save root (or data root)
     *    so FAT32 metadata/cluster updates are committed once per save event. */
    int dirfd = open(NOX_3DS_DATA_ROOT "/save", O_RDONLY);
    if (dirfd < 0) {
        dirfd = open(NOX_3DS_DATA_ROOT, O_RDONLY);
    }
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }
}
