#define _GNU_SOURCE
#include "platform_3ds.h"

#include <3ds.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
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
    if (fd < 0 && errno == ENOENT) {
        nox_3ds_ensure_parent_dirs(mf->resolved);
        fd = open(mf->resolved, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    }
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

    /* Fast path: try mkdir() directly (1 IPC when parent exists, vs 3 IPCs
     * for ensure_parent_dirs + stat + mkdir). */
    if (mkdir(resolved, 0777) == 0 || errno == EEXIST)
        return true;

    if (errno == ENOENT) {
        if (!nox_3ds_ensure_parent_dirs(resolved))
            return false;
        if (mkdir(resolved, 0777) == 0 || errno == EEXIST)
            return true;
    }

    struct stat st;
    if (stat(resolved, &st) == 0)
        return S_ISDIR(st.st_mode);

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
        nox_3ds_crypt_cache_invalidate(resolved);

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
    if (!fp && wants_write && errno == ENOENT) {
        nox_3ds_ensure_parent_dirs(resolved);
        fp = fopen(resolved, mode);
    }
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

/* =========================================================================
 * Fast -O3 Blowfish & RAM-Cached Read-Only Cryptfile Reader
 *
 * Original Nox's sub_408E40 / sub_409050 read encrypted .map and .plr files
 * directly from FILE* in 1-8 byte chunks, re-decrypting the same 8-byte
 * Blowfish block up to 8 times per block and executing ftell()+fseek() for
 * every object during client map load (sub_4AC2B0 -> sub_4AC610). With a
 * 64 KB stdio buffer on 3DS SDMC, each fseek invalidates the buffer and
 * forces ARM9 FS IPC + redundant 64 KB reads.
 *
 * Here we:
 *  1. Provide a -O3 unrolled Blowfish block transform matching sub_409860.
 *  2. Demand-load read-only encrypted files: initial 8 KB static prefix
 *     (zero malloc for header peeks like sub_4CFE10 / sub_4CF470), promoting
 *     to a single 128 KB-chunked sequential read + single-pass decrypt on the
 *     first read past 8 KB (pumping audio between chunks).
 *  3. Keep a 2-slot LRU decrypted RAM cache for .map and .plr files so
 *     server load (sub_4CF5F0), client check (sub_4CF470), and client load
 *     (sub_4AC2B0) share one SDMC read and one Blowfish pass.
 * ========================================================================= */

#define NOX_3DS_CRYPT_PREFIX_SZ        8192u
#define NOX_3DS_CRYPT_PREFIX_SLOTS     4
#define NOX_3DS_CRYPT_READ_CHUNK       (128u * 1024u)
#define NOX_3DS_CRYPT_MAX_FILE         (8u * 1024u * 1024u)
#define NOX_3DS_CRYPT_CACHE_SLOTS      2
#define NOX_3DS_CRYPT_CACHE_MAX_TOTAL  (6u * 1024u * 1024u)

typedef struct {
    bool     valid;
    int      key_id;
    char     path[512];
    uint8_t *data;
    size_t   size;
    uint32_t last_use;
} nox_3ds_crypt_cache_slot_t;

typedef struct {
    bool     valid;
    int      key_id;
    char     path[512];
    size_t   file_size;
    size_t   prefix_len;
    uint32_t last_use;
    uint8_t  data[NOX_3DS_CRYPT_PREFIX_SZ];
} nox_3ds_crypt_prefix_slot_t;

typedef struct {
    bool            active;
    bool            from_cache;
    bool            from_prefix_cache;
    bool            is_raw;
    bool            owns_buf;
    int             cache_slot;
    int             key_id;
    FILE           *fp;
    FILE           *real_fp;
    char            path[512];
    uint8_t        *data;
    size_t          size;
    size_t          loaded;
    size_t          pos;
    const uint32_t *P_dec;
    const uint32_t *S;
    void          (*pump_cb)(void);
} nox_3ds_crypt_reader_t;

static nox_3ds_crypt_cache_slot_t  g_crypt_cache[NOX_3DS_CRYPT_CACHE_SLOTS];
static nox_3ds_crypt_prefix_slot_t g_crypt_prefix_cache[NOX_3DS_CRYPT_PREFIX_SLOTS];
static uint32_t                    g_crypt_cache_clock = 0;
static nox_3ds_crypt_reader_t      g_crypt_reader;
static uint8_t                     g_crypt_prefix_buf[NOX_3DS_CRYPT_PREFIX_SZ];
static FILE                        g_crypt_virtual_fp;
static FILE                       *g_crypt_open_fp = NULL;
static FILE                       *g_crypt_open_real_fp = NULL;
static bool                        g_crypt_open_is_virtual = false;
static char                        g_crypt_open_path[512];
static size_t                      g_crypt_open_size = 0;

int nox_3ds_blowfish_crypt(uint8_t *dst, const uint8_t *src, int len,
                           const uint32_t *P, const uint32_t *S)
{
    if (!src || len <= 0 || !P || !S)
        return 0;
    if (!dst)
        dst = (uint8_t *)src;

    const uint32_t *S0 = S;
    const uint32_t *S1 = S + 256;
    const uint32_t *S2 = S + 512;
    const uint32_t *S3 = S + 768;

    const uint32_t p0  = P[0],  p1  = P[1],  p2  = P[2],  p3  = P[3];
    const uint32_t p4  = P[4],  p5  = P[5],  p6  = P[6],  p7  = P[7];
    const uint32_t p8  = P[8],  p9  = P[9],  p10 = P[10], p11 = P[11];
    const uint32_t p12 = P[12], p13 = P[13], p14 = P[14], p15 = P[15];
    const uint32_t p16 = P[16], p17 = P[17];

    int nblocks = len / 8;
    const uint8_t *in = src;
    uint8_t *out = dst;

#define NOX_BF_F(x) (((S0[(x) >> 24] + S1[((x) >> 16) & 0xFFu]) ^ S2[((x) >> 8) & 0xFFu]) + S3[(x) & 0xFFu])
#define NOX_BF_PAIR(pa, pb) do { \
    L ^= (pa); \
    R ^= (pb) ^ NOX_BF_F(L); \
    L ^= NOX_BF_F(R); \
} while (0)

    for (int i = 0; i < nblocks; ++i) {
        uint32_t rawL, rawR;
        memcpy(&rawL, in, 4);
        memcpy(&rawR, in + 4, 4);
        uint32_t L = __builtin_bswap32(rawL);
        uint32_t R = __builtin_bswap32(rawR);

        NOX_BF_PAIR(p0,  p1);
        NOX_BF_PAIR(p2,  p3);
        NOX_BF_PAIR(p4,  p5);
        NOX_BF_PAIR(p6,  p7);
        NOX_BF_PAIR(p8,  p9);
        NOX_BF_PAIR(p10, p11);
        NOX_BF_PAIR(p12, p13);
        NOX_BF_PAIR(p14, p15);

        uint32_t outL = __builtin_bswap32(R ^ p17);
        uint32_t outR = __builtin_bswap32(L ^ p16);
        memcpy(out, &outL, 4);
        memcpy(out + 4, &outR, 4);

        in += 8;
        out += 8;
    }

#undef NOX_BF_PAIR
#undef NOX_BF_F

    int done = nblocks * 8;
    if (done < len) {
        memmove(out, in, (size_t)(len - done));
    }
    return done;
}

static bool path_is_cacheable_cryptfile(const char *path)
{
    if (!path)
        return false;
    size_t n = strlen(path);
    if (n < 4)
        return false;
    const char *ext = path + n - 4;
    return (strcasecmp(ext, ".map") == 0) || (strcasecmp(ext, ".plr") == 0);
}

static size_t crypt_cache_total_bytes_locked(void)
{
    size_t total = 0;
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (g_crypt_cache[i].valid)
            total += g_crypt_cache[i].size;
    }
    return total;
}

static void crypt_cache_evict_slot_locked(int idx)
{
    if (idx < 0 || idx >= NOX_3DS_CRYPT_CACHE_SLOTS)
        return;
    nox_3ds_crypt_cache_slot_t *slot = &g_crypt_cache[idx];
    if (!slot->valid)
        return;

    if (g_crypt_reader.active && g_crypt_reader.from_cache && g_crypt_reader.cache_slot == idx) {
        /* Transfer buffer ownership to the active reader so it never dangles. */
        g_crypt_reader.from_cache = false;
        g_crypt_reader.owns_buf = true;
        g_crypt_reader.cache_slot = -1;
    } else {
        free(slot->data);
    }
    memset(slot, 0, sizeof(*slot));
}

static void crypt_cache_store(const char *resolved_path, int key_id, uint8_t *data, size_t size)
{
    if (!resolved_path || !data || size == 0 || size > NOX_3DS_CRYPT_CACHE_MAX_TOTAL) {
        free(data);
        return;
    }

    fs_lock();

    /* Replace any existing entry for the same path. */
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (g_crypt_cache[i].valid && strcasecmp(g_crypt_cache[i].path, resolved_path) == 0) {
            crypt_cache_evict_slot_locked(i);
        }
    }

    /* Ensure total cache size stays within budget. */
    while (crypt_cache_total_bytes_locked() + size > NOX_3DS_CRYPT_CACHE_MAX_TOTAL) {
        int oldest = -1;
        uint32_t oldest_clk = UINT32_MAX;
        for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
            if (g_crypt_cache[i].valid && g_crypt_cache[i].last_use <= oldest_clk) {
                oldest_clk = g_crypt_cache[i].last_use;
                oldest = i;
            }
        }
        if (oldest < 0)
            break;
        crypt_cache_evict_slot_locked(oldest);
    }

    /* Pick a free slot, or evict LRU slot. */
    int target = -1;
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (!g_crypt_cache[i].valid) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        uint32_t oldest_clk = UINT32_MAX;
        for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
            if (g_crypt_cache[i].last_use <= oldest_clk) {
                oldest_clk = g_crypt_cache[i].last_use;
                target = i;
            }
        }
        crypt_cache_evict_slot_locked(target);
    }

    nox_3ds_crypt_cache_slot_t *slot = &g_crypt_cache[target];
    slot->valid = true;
    slot->key_id = key_id;
    snprintf(slot->path, sizeof(slot->path), "%s", resolved_path);
    slot->data = data;
    slot->size = size;
    slot->last_use = ++g_crypt_cache_clock;

    /* Remove any stale prefix-only entry for the same path. */
    for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
        if (g_crypt_prefix_cache[i].valid &&
            strcasecmp(g_crypt_prefix_cache[i].path, resolved_path) == 0) {
            g_crypt_prefix_cache[i].valid = false;
        }
    }

    fs_unlock();
}

static void crypt_prefix_cache_store(const char *resolved_path, int key_id,
                                     size_t file_size, const uint8_t *prefix_data,
                                     size_t prefix_len)
{
    if (!resolved_path || !prefix_data || prefix_len == 0 ||
        prefix_len > NOX_3DS_CRYPT_PREFIX_SZ || file_size == 0)
        return;

    fs_lock();

    /* If already in the full-file cache, no need to store a prefix copy. */
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (g_crypt_cache[i].valid && strcasecmp(g_crypt_cache[i].path, resolved_path) == 0) {
            fs_unlock();
            return;
        }
    }

    int target = -1;
    for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
        if (g_crypt_prefix_cache[i].valid &&
            strcasecmp(g_crypt_prefix_cache[i].path, resolved_path) == 0) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
            if (!g_crypt_prefix_cache[i].valid) {
                target = i;
                break;
            }
        }
    }
    if (target < 0) {
        uint32_t oldest_clk = UINT32_MAX;
        for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
            if (g_crypt_prefix_cache[i].last_use <= oldest_clk) {
                oldest_clk = g_crypt_prefix_cache[i].last_use;
                target = i;
            }
        }
    }

    nox_3ds_crypt_prefix_slot_t *ps = &g_crypt_prefix_cache[target];
    ps->valid = true;
    ps->key_id = key_id;
    snprintf(ps->path, sizeof(ps->path), "%s", resolved_path);
    ps->file_size = file_size;
    ps->prefix_len = prefix_len;
    ps->last_use = ++g_crypt_cache_clock;
    memcpy(ps->data, prefix_data, prefix_len);

    fs_unlock();
}

void nox_3ds_crypt_cache_invalidate(const char *path)
{
    if (!path || !*path)
        return;

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0])
        return;

    fs_lock();
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (g_crypt_cache[i].valid && strcasecmp(g_crypt_cache[i].path, resolved) == 0) {
            crypt_cache_evict_slot_locked(i);
        }
    }
    for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
        if (g_crypt_prefix_cache[i].valid &&
            strcasecmp(g_crypt_prefix_cache[i].path, resolved) == 0) {
            g_crypt_prefix_cache[i].valid = false;
        }
    }
    fs_unlock();
}

void nox_3ds_crypt_cache_clear(void)
{
    fs_lock();
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        crypt_cache_evict_slot_locked(i);
    }
    for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
        g_crypt_prefix_cache[i].valid = false;
    }
    fs_unlock();
}

bool nox_3ds_crypt_is_virtual(FILE *fp)
{
    return fp == &g_crypt_virtual_fp;
}

bool nox_3ds_crypt_cache_has(const char *path)
{
    if (!path || !*path)
        return false;

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0])
        return false;

    bool found = false;
    fs_lock();
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (g_crypt_cache[i].valid && strcasecmp(g_crypt_cache[i].path, resolved) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
            if (g_crypt_prefix_cache[i].valid &&
                strcasecmp(g_crypt_prefix_cache[i].path, resolved) == 0) {
                found = true;
                break;
            }
        }
    }
    fs_unlock();
    return found;
}

void nox_3ds_crypt_reset(void)
{
    FILE *leaked_real = NULL;
    if (g_crypt_reader.active) {
        if (g_crypt_reader.real_fp && g_crypt_reader.real_fp != g_crypt_reader.fp) {
            leaked_real = g_crypt_reader.real_fp;
        }
        if (g_crypt_reader.owns_buf && g_crypt_reader.data &&
            g_crypt_reader.data != g_crypt_prefix_buf) {
            free(g_crypt_reader.data);
        }
        memset(&g_crypt_reader, 0, sizeof(g_crypt_reader));
    }
    if (g_crypt_open_is_virtual && g_crypt_open_real_fp && g_crypt_open_real_fp != leaked_real) {
        nox_3ds_fclose(g_crypt_open_real_fp);
    }
    if (leaked_real) {
        nox_3ds_fclose(leaked_real);
    }
    g_crypt_open_fp = NULL;
    g_crypt_open_real_fp = NULL;
    g_crypt_open_is_virtual = false;
    g_crypt_open_path[0] = '\0';
    g_crypt_open_size = 0;
}

FILE *nox_3ds_crypt_try_open_cached(const char *path)
{
    if (!path || !*path)
        return NULL;

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0])
        return NULL;

    size_t cached_size = 0;
    bool hit = false;

    fs_lock();
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        if (g_crypt_cache[i].valid && strcasecmp(g_crypt_cache[i].path, resolved) == 0) {
            cached_size = g_crypt_cache[i].size;
            hit = true;
            break;
        }
    }
    if (!hit) {
        for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
            if (g_crypt_prefix_cache[i].valid &&
                strcasecmp(g_crypt_prefix_cache[i].path, resolved) == 0) {
                cached_size = g_crypt_prefix_cache[i].file_size;
                hit = true;
                break;
            }
        }
    }
    fs_unlock();

    if (!hit)
        return NULL;

    nox_3ds_crypt_reset();
    g_crypt_open_fp = &g_crypt_virtual_fp;
    g_crypt_open_real_fp = NULL;
    g_crypt_open_is_virtual = true;
    snprintf(g_crypt_open_path, sizeof(g_crypt_open_path), "%s", resolved);
    g_crypt_open_size = cached_size;
    return &g_crypt_virtual_fp;
}

void nox_3ds_crypt_on_open(FILE *fp, const char *path, int mode)
{
    nox_3ds_crypt_reset();
    if (!path || !*path)
        return;

    char resolved[512];
    nox_3ds_resolve_path(path, resolved, sizeof(resolved));
    if (!resolved[0])
        return;

    if (mode != 0) {
        nox_3ds_crypt_cache_invalidate(resolved);
        return;
    }

    if (fp) {
        g_crypt_open_fp = fp;
        g_crypt_open_real_fp = fp;
        g_crypt_open_is_virtual = false;
        snprintf(g_crypt_open_path, sizeof(g_crypt_open_path), "%s", resolved);
    }
}

int nox_3ds_crypt_try_cache(FILE *fp, int key_id)
{
    if (!fp || fp != g_crypt_open_fp || key_id < 0)
        return -1;

    size_t fsize = g_crypt_open_size;
    if (!g_crypt_open_is_virtual) {
        int fd = fileno(fp);
        if (fd < 0)
            return -1;

        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > NOX_3DS_CRYPT_MAX_FILE)
            return -1;

        fsize = (size_t)st.st_size;
        g_crypt_open_size = fsize;
    }

    fs_lock();
    for (int i = 0; i < NOX_3DS_CRYPT_CACHE_SLOTS; ++i) {
        nox_3ds_crypt_cache_slot_t *slot = &g_crypt_cache[i];
        if (slot->valid &&
            slot->key_id == key_id &&
            (g_crypt_open_is_virtual || slot->size == fsize) &&
            strcasecmp(slot->path, g_crypt_open_path) == 0) {
            slot->last_use = ++g_crypt_cache_clock;
            memset(&g_crypt_reader, 0, sizeof(g_crypt_reader));
            g_crypt_reader.active = true;
            g_crypt_reader.from_cache = true;
            g_crypt_reader.from_prefix_cache = false;
            g_crypt_reader.is_raw = false;
            g_crypt_reader.owns_buf = false;
            g_crypt_reader.cache_slot = i;
            g_crypt_reader.key_id = key_id;
            g_crypt_reader.fp = fp;
            g_crypt_reader.real_fp = g_crypt_open_is_virtual ? NULL : fp;
            snprintf(g_crypt_reader.path, sizeof(g_crypt_reader.path), "%s", slot->path);
            g_crypt_reader.data = slot->data;
            g_crypt_reader.size = slot->size;
            g_crypt_reader.loaded = slot->size;
            g_crypt_reader.pos = 0;
            fs_unlock();
            if (slot->size >= 64u * 1024u) {
                nox_3ds_load_log("crypt CACHE HIT '%s' (%u KB)",
                                 g_crypt_reader.path, (unsigned)(slot->size / 1024u));
            }
            return 1;
        }
    }

    for (int i = 0; i < NOX_3DS_CRYPT_PREFIX_SLOTS; ++i) {
        nox_3ds_crypt_prefix_slot_t *ps = &g_crypt_prefix_cache[i];
        if (ps->valid &&
            ps->key_id == key_id &&
            (g_crypt_open_is_virtual || ps->file_size == fsize) &&
            strcasecmp(ps->path, g_crypt_open_path) == 0) {
            ps->last_use = ++g_crypt_cache_clock;
            memcpy(g_crypt_prefix_buf, ps->data, ps->prefix_len);
            memset(&g_crypt_reader, 0, sizeof(g_crypt_reader));
            g_crypt_reader.active = true;
            g_crypt_reader.from_cache = false;
            g_crypt_reader.from_prefix_cache = true;
            g_crypt_reader.is_raw = false;
            g_crypt_reader.owns_buf = false;
            g_crypt_reader.cache_slot = -1;
            g_crypt_reader.key_id = key_id;
            g_crypt_reader.fp = fp;
            g_crypt_reader.real_fp = g_crypt_open_is_virtual ? NULL : fp;
            snprintf(g_crypt_reader.path, sizeof(g_crypt_reader.path), "%s", ps->path);
            g_crypt_reader.data = g_crypt_prefix_buf;
            g_crypt_reader.size = ps->file_size;
            g_crypt_reader.loaded = ps->prefix_len;
            g_crypt_reader.pos = 0;
            fs_unlock();
            return 2;
        }
    }
    fs_unlock();

    /* Virtual open missed (e.g. different key_id or concurrent eviction): lazily open real file. */
    if (g_crypt_open_is_virtual) {
        FILE *rfp = nox_3ds_fopen(g_crypt_open_path, "rb");
        if (!rfp)
            return -1;
        int fd = fileno(rfp);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 ||
            (size_t)st.st_size > NOX_3DS_CRYPT_MAX_FILE) {
            nox_3ds_fclose(rfp);
            return -1;
        }
        g_crypt_open_real_fp = rfp;
        g_crypt_open_size = (size_t)st.st_size;
    }

    return 0;
}

void nox_3ds_crypt_set_promote_ctx(FILE *fp,
                                   const uint32_t *P_dec, const uint32_t *S,
                                   void (*pump_cb)(void))
{
    if (!g_crypt_reader.active || g_crypt_reader.fp != fp)
        return;
    g_crypt_reader.P_dec = P_dec;
    g_crypt_reader.S = S;
    g_crypt_reader.pump_cb = pump_cb;
}

bool nox_3ds_crypt_arm_raw(FILE *fp)
{
    if (!fp || fp != g_crypt_open_fp || !g_crypt_open_is_virtual)
        return false;

    FILE *rfp = g_crypt_open_real_fp;
    if (!rfp) {
        rfp = nox_3ds_fopen(g_crypt_open_path, "rb");
        if (!rfp)
            return false;
        g_crypt_open_real_fp = rfp;
    }

    int fd = fileno(rfp);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (size_t)st.st_size > NOX_3DS_CRYPT_MAX_FILE) {
        return false;
    }

    size_t fsize = (size_t)st.st_size;
    size_t want = (fsize > NOX_3DS_CRYPT_PREFIX_SZ) ? NOX_3DS_CRYPT_PREFIX_SZ : fsize;
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, g_crypt_prefix_buf + got, want - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }

    memset(&g_crypt_reader, 0, sizeof(g_crypt_reader));
    g_crypt_reader.active = true;
    g_crypt_reader.from_cache = false;
    g_crypt_reader.from_prefix_cache = false;
    g_crypt_reader.is_raw = true;
    g_crypt_reader.owns_buf = false;
    g_crypt_reader.cache_slot = -1;
    g_crypt_reader.key_id = -1;
    g_crypt_reader.fp = fp;
    g_crypt_reader.real_fp = rfp;
    snprintf(g_crypt_reader.path, sizeof(g_crypt_reader.path), "%s", g_crypt_open_path);
    g_crypt_reader.data = g_crypt_prefix_buf;
    g_crypt_reader.size = (got < want) ? got : fsize;
    g_crypt_reader.loaded = got;
    g_crypt_reader.pos = 0;
    return true;
}

bool nox_3ds_crypt_arm_reader(FILE *fp, int key_id,
                              const uint32_t *P_dec, const uint32_t *S,
                              void (*pump_cb)(void))
{
    if (!fp || fp != g_crypt_open_fp || key_id < 0 || !P_dec || !S || g_crypt_open_size == 0)
        return false;

    FILE *rfp = g_crypt_open_is_virtual ? g_crypt_open_real_fp : fp;
    if (!rfp)
        return false;

    int fd = fileno(rfp);
    if (fd < 0)
        return false;

    size_t want = g_crypt_open_size;
    if (want > NOX_3DS_CRYPT_PREFIX_SZ)
        want = NOX_3DS_CRYPT_PREFIX_SZ;

    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, g_crypt_prefix_buf + got, want - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }

    if (got >= 8u) {
        nox_3ds_blowfish_crypt(g_crypt_prefix_buf, g_crypt_prefix_buf, (int)(got & ~7u), P_dec, S);
    }

    memset(&g_crypt_reader, 0, sizeof(g_crypt_reader));
    g_crypt_reader.active = true;
    g_crypt_reader.from_cache = false;
    g_crypt_reader.from_prefix_cache = false;
    g_crypt_reader.is_raw = false;
    g_crypt_reader.owns_buf = false;
    g_crypt_reader.cache_slot = -1;
    g_crypt_reader.key_id = key_id;
    g_crypt_reader.fp = fp;
    g_crypt_reader.real_fp = rfp;
    snprintf(g_crypt_reader.path, sizeof(g_crypt_reader.path), "%s", g_crypt_open_path);
    g_crypt_reader.data = g_crypt_prefix_buf;
    g_crypt_reader.size = (got < want) ? got : g_crypt_open_size;
    g_crypt_reader.loaded = got;
    g_crypt_reader.pos = 0;
    g_crypt_reader.P_dec = P_dec;
    g_crypt_reader.S = S;
    g_crypt_reader.pump_cb = pump_cb;
    return true;
}

static bool crypt_reader_promote_full(void)
{
    if (!g_crypt_reader.active)
        return false;
    if (g_crypt_reader.loaded >= g_crypt_reader.size)
        return true;

    int fd = -1;
    bool owned_fd = false;
    if (g_crypt_reader.real_fp) {
        fd = fileno(g_crypt_reader.real_fp);
        if (fd < 0)
            return false;
        if (g_crypt_reader.from_prefix_cache) {
            if (lseek(fd, (off_t)g_crypt_reader.loaded, SEEK_SET) < 0)
                return false;
        }
    } else {
        fd = open(g_crypt_reader.path, O_RDONLY);
        if (fd < 0)
            return false;
        owned_fd = true;
        if (g_crypt_reader.loaded > 0 &&
            lseek(fd, (off_t)g_crypt_reader.loaded, SEEK_SET) < 0) {
            close(fd);
            return false;
        }
    }

    uint32_t t0 = nox_3ds_get_ticks_ms();
    uint8_t *buf = (uint8_t *)malloc(g_crypt_reader.size);
    if (!buf) {
        nox_3ds_crypt_cache_clear();
        buf = (uint8_t *)malloc(g_crypt_reader.size);
        if (!buf) {
            if (owned_fd) close(fd);
            return false;
        }
    }

    if (g_crypt_reader.loaded > 0) {
        memcpy(buf, g_crypt_prefix_buf, g_crypt_reader.loaded);
    }

    size_t off = g_crypt_reader.loaded;
    size_t dec_off = g_crypt_reader.loaded & ~7u;
    if (dec_off < off) {
        /* Prefix was a non-multiple of 8 only on unexpected short read; decrypt from dec_off */
        dec_off = off & ~7u;
    }

    while (off < g_crypt_reader.size) {
        size_t chunk = g_crypt_reader.size - off;
        if (chunk > NOX_3DS_CRYPT_READ_CHUNK)
            chunk = NOX_3DS_CRYPT_READ_CHUNK;
        ssize_t r = read(fd, buf + off, chunk);
        if (r <= 0)
            break;
        off += (size_t)r;

        if (!g_crypt_reader.is_raw) {
            size_t avail_blocks = (off - dec_off) & ~7u;
            if (avail_blocks > 0) {
                nox_3ds_blowfish_crypt(buf + dec_off, buf + dec_off, (int)avail_blocks,
                                       g_crypt_reader.P_dec, g_crypt_reader.S);
                dec_off += avail_blocks;
            }
        }
        if (g_crypt_reader.pump_cb) {
            g_crypt_reader.pump_cb();
        }
    }

    if (owned_fd) {
        close(fd);
    }

    g_crypt_reader.data = buf;
    g_crypt_reader.owns_buf = true;
    g_crypt_reader.from_prefix_cache = false;
    g_crypt_reader.loaded = off;
    g_crypt_reader.size = off;

    uint32_t dt = nox_3ds_get_ticks_ms() - t0;
    if (off >= 64u * 1024u) {
        nox_3ds_load_log("crypt MISS+READ '%s' (%u KB, %u ms)",
                         g_crypt_reader.path, (unsigned)(off / 1024u), (unsigned)dt);
    }
    return true;
}

bool nox_3ds_crypt_read(FILE *fp, void *dst, int elem_size, int count,
                        int *out_pos, int *out_eof, int *out_ret)
{
    if (!g_crypt_reader.active || g_crypt_reader.fp != fp)
        return false;

    if (elem_size <= 0 || count <= 0) {
        if (out_ret) *out_ret = 0;
        return true;
    }

    size_t total = (size_t)elem_size * (size_t)count;
    if (g_crypt_reader.pos + total > g_crypt_reader.loaded &&
        g_crypt_reader.loaded < g_crypt_reader.size) {
        if (!crypt_reader_promote_full()) {
            if (out_eof) *out_eof = 1;
            if (out_ret) *out_ret = 0;
            return true;
        }
    }

    if (g_crypt_reader.pos >= g_crypt_reader.size) {
        if (out_pos) *out_pos = (int)g_crypt_reader.pos;
        if (out_eof) *out_eof = 1;
        if (out_ret) *out_ret = 0;
        return true;
    }

    size_t avail = g_crypt_reader.size - g_crypt_reader.pos;
    if (avail >= total) {
        memcpy(dst, g_crypt_reader.data + g_crypt_reader.pos, total);
        g_crypt_reader.pos += total;
        if (out_pos) *out_pos = (int)g_crypt_reader.pos;
        if (out_ret) *out_ret = count;
        return true;
    }

    /* Partial read at EOF */
    memcpy(dst, g_crypt_reader.data + g_crypt_reader.pos, avail);
    g_crypt_reader.pos += avail;
    if (out_pos) *out_pos = (int)g_crypt_reader.pos;
    if (out_eof) *out_eof = 1;
    if (out_ret) *out_ret = (int)(avail / (size_t)elem_size);
    return true;
}

bool nox_3ds_crypt_seek(FILE *fp, int offset, int whence,
                        int *out_pos, int *out_eof)
{
    if (!g_crypt_reader.active || g_crypt_reader.fp != fp)
        return false;

    if (out_eof) *out_eof = 0;

    int64_t base = 0;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)g_crypt_reader.pos; break;
    case SEEK_END: base = (int64_t)g_crypt_reader.size; break;
    default:
        return true;
    }

    int64_t next = base + (int64_t)offset;
    if (next < 0)
        next = 0;
    if ((uint64_t)next > (uint64_t)g_crypt_reader.size)
        next = (int64_t)g_crypt_reader.size;

    g_crypt_reader.pos = (size_t)next;
    if (out_pos) *out_pos = (int)g_crypt_reader.pos;
    return true;
}

bool nox_3ds_crypt_close(FILE *fp)
{
    if (!g_crypt_reader.active || g_crypt_reader.fp != fp) {
        if (g_crypt_open_fp == fp) {
            FILE *rfp = g_crypt_open_is_virtual ? g_crypt_open_real_fp : NULL;
            bool was_virtual = g_crypt_open_is_virtual;
            g_crypt_open_fp = NULL;
            g_crypt_open_real_fp = NULL;
            g_crypt_open_is_virtual = false;
            g_crypt_open_path[0] = '\0';
            g_crypt_open_size = 0;
            if (rfp) {
                nox_3ds_fclose(rfp);
            }
            if (was_virtual) {
                return true;
            }
        }
        return (fp == &g_crypt_virtual_fp);
    }

    FILE *to_close = g_crypt_reader.real_fp;

    bool should_cache_full = (!g_crypt_reader.from_cache &&
                              !g_crypt_reader.is_raw &&
                              g_crypt_reader.loaded == g_crypt_reader.size &&
                              g_crypt_reader.size > 0 &&
                              g_crypt_reader.size <= NOX_3DS_CRYPT_CACHE_MAX_TOTAL &&
                              path_is_cacheable_cryptfile(g_crypt_reader.path));

    bool should_cache_prefix = (!should_cache_full &&
                                !g_crypt_reader.from_cache &&
                                !g_crypt_reader.from_prefix_cache &&
                                !g_crypt_reader.is_raw &&
                                g_crypt_reader.loaded > 0 &&
                                g_crypt_reader.loaded < g_crypt_reader.size &&
                                g_crypt_reader.loaded <= NOX_3DS_CRYPT_PREFIX_SZ &&
                                g_crypt_reader.data == g_crypt_prefix_buf &&
                                path_is_cacheable_cryptfile(g_crypt_reader.path));

    if (should_cache_full) {
        if (g_crypt_reader.owns_buf && g_crypt_reader.data != g_crypt_prefix_buf) {
            crypt_cache_store(g_crypt_reader.path, g_crypt_reader.key_id,
                              g_crypt_reader.data, g_crypt_reader.size);
            g_crypt_reader.data = NULL;
            g_crypt_reader.owns_buf = false;
        } else if (g_crypt_reader.data == g_crypt_prefix_buf) {
            uint8_t *copy = (uint8_t *)malloc(g_crypt_reader.size);
            if (copy) {
                memcpy(copy, g_crypt_prefix_buf, g_crypt_reader.size);
                crypt_cache_store(g_crypt_reader.path, g_crypt_reader.key_id,
                                  copy, g_crypt_reader.size);
            }
        }
    } else if (should_cache_prefix) {
        crypt_prefix_cache_store(g_crypt_reader.path, g_crypt_reader.key_id,
                                 g_crypt_reader.size, g_crypt_prefix_buf,
                                 g_crypt_reader.loaded);
    } else if (g_crypt_reader.owns_buf && g_crypt_reader.data &&
               g_crypt_reader.data != g_crypt_prefix_buf) {
        free(g_crypt_reader.data);
        g_crypt_reader.data = NULL;
    }

    memset(&g_crypt_reader, 0, sizeof(g_crypt_reader));
    g_crypt_open_fp = NULL;
    g_crypt_open_real_fp = NULL;
    g_crypt_open_is_virtual = false;
    g_crypt_open_path[0] = '\0';
    g_crypt_open_size = 0;

    if (to_close) {
        nox_3ds_fclose(to_close);
    }
    return true;
}

/* =========================================================================
 * Lightweight Campaign Load-Phase Timing Log (RAM-buffered -> load.log)
 * ========================================================================= */
#define NOX_3DS_LOAD_LOG_CAP 4096u
static char   g_load_log_buf[NOX_3DS_LOAD_LOG_CAP];
static size_t g_load_log_len = 0;
static bool   g_load_log_started = false;

void nox_3ds_load_log(const char *fmt, ...)
{
    if (!fmt)
        return;

    char msg[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    uint32_t ms = nox_3ds_get_ticks_ms();
    fprintf(stderr, "[load3ds %ums] %s\n", (unsigned)ms, msg);

    if (g_load_log_len + 1 < NOX_3DS_LOAD_LOG_CAP) {
        int n = snprintf(g_load_log_buf + g_load_log_len,
                         NOX_3DS_LOAD_LOG_CAP - g_load_log_len,
                         "[%ums] %s\n", (unsigned)ms, msg);
        if (n > 0) {
            size_t avail = NOX_3DS_LOAD_LOG_CAP - 1u - g_load_log_len;
            g_load_log_len += ((size_t)n < avail) ? (size_t)n : avail;
        }
    }
}

void nox_3ds_load_log_flush(void)
{
    if (g_load_log_len == 0)
        return;

    const char *mode = g_load_log_started ? "a" : "w";
    FILE *f = fopen(NOX_3DS_DATA_ROOT "/load.log", mode);
    if (f) {
        g_load_log_started = true;
        fwrite(g_load_log_buf, 1, g_load_log_len, f);
        fclose(f);
    }
    g_load_log_len = 0;
}
