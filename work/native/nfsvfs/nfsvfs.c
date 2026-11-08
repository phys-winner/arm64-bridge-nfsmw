/*
 * nfsvfs: read-only virtual file for the embedded NFS Most Wanted OBB (arm32 path).
 *
 * The IronMonkey engine opens its OBB zip through im::PosixFileSystem::open(),
 * i.e. stat() for the size and fopen(path, "rb") + fread/fseek/ftell for the data
 * (libapp.so 1.3.128: sub_582794, FileStream vtable 0xAB11D4). When the OBB is
 * shipped as a STORED asset inside an APK, Java registers the asset's
 * (fd, offset, length) for one virtual path and this library redirects exactly
 * that path: stat() reports a regular read-only file of the asset length and
 * fopen() returns a funopen() stream that preads from the APK range. Nothing is
 * extracted to disk. All other paths pass through to libc unchanged.
 *
 * Hooks are installed by rewriting libapp.so's R_ARM_JUMP_SLOT/GLOB_DAT/ABS32
 * GOT entries for "fopen" and "stat" (BIND_NOW + RELRO, so the page is made
 * writable just for the store and then restored).
 */
#define _GNU_SOURCE 1 /* bionic declares funopen() only with BSD/GNU extensions */
#include <android/log.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "NfsVfs"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_vpath[512];
static int g_fd = -1;
static off64_t g_off;
static off64_t g_len;
static volatile int g_open_count;
static volatile long long g_read_bytes;

typedef struct {
    off64_t pos;
} vfile_t;

static int vfs_read(void *cookie, char *buf, int n) {
    vfile_t *f = (vfile_t *)cookie;
    if (n <= 0) return 0;
    off64_t rem = g_len - f->pos;
    if (rem <= 0) return 0;
    if ((off64_t)n > rem) n = (int)rem;
    ssize_t r = pread64(g_fd, buf, (size_t)n, g_off + f->pos);
    if (r > 0) {
        f->pos += r;
        __atomic_add_fetch(&g_read_bytes, (long long)r, __ATOMIC_RELAXED);
    }
    return (int)r;
}

static fpos_t vfs_seek(void *cookie, fpos_t off, int whence) {
    vfile_t *f = (vfile_t *)cookie;
    off64_t np;
    switch (whence) {
    case SEEK_SET: np = (off64_t)off; break;
    case SEEK_CUR: np = f->pos + (off64_t)off; break;
    case SEEK_END: np = g_len + (off64_t)off; break;
    default: errno = EINVAL; return (fpos_t)-1;
    }
    if (np < 0 || np > 0x7fffffffLL) {
        errno = EINVAL;
        return (fpos_t)-1;
    }
    f->pos = np;
    return (fpos_t)np;
}

static int vfs_close(void *cookie) {
    free(cookie);
    return 0;
}

static int is_vpath(const char *path) {
    return path && g_fd >= 0 && g_vpath[0] && strcmp(path, g_vpath) == 0;
}

static FILE *(*real_fopen)(const char *, const char *) = fopen;
static int (*real_stat)(const char *, struct stat *) = stat;

static FILE *hook_fopen(const char *path, const char *mode) {
    if (!is_vpath(path)) return real_fopen(path, mode);
    if (!mode || mode[0] != 'r' || strchr(mode, '+') || strchr(mode, 'w') || strchr(mode, 'a')) {
        errno = EROFS;
        return NULL;
    }
    vfile_t *f = (vfile_t *)calloc(1, sizeof(*f));
    if (!f) {
        errno = ENOMEM;
        return NULL;
    }
    FILE *fp = funopen(f, vfs_read, NULL, vfs_seek, vfs_close);
    if (!fp) {
        free(f);
        return NULL;
    }
    int n = __atomic_add_fetch(&g_open_count, 1, __ATOMIC_RELAXED);
    LOGI("fopen(%s) -> embedded range off=%lld len=%lld (open #%d)", path, (long long)g_off, (long long)g_len, n);
    return fp;
}

static int hook_stat(const char *path, struct stat *st) {
    if (!is_vpath(path)) return real_stat(path, st);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = g_len;
    st->st_blksize = 4096;
    st->st_blocks = (g_len + 511) / 512;
    return 0;
}

typedef struct {
    const char *name;
    void *hook;
    int patched;
} hook_t;

static hook_t g_hooks[] = {
    {"fopen", (void *)hook_fopen, 0},
    {"stat", (void *)hook_stat, 0},
};

static int patch_slot(uintptr_t addr, void *value) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = addr & ~((uintptr_t)page - 1);
    if (mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect RW %p failed: %s", (void *)start, strerror(errno));
        return -1;
    }
    *(void **)addr = value;
    __builtin___clear_cache((char *)addr, (char *)addr + sizeof(void *));
    if (mprotect((void *)start, (size_t)page, PROT_READ) != 0)
        LOGE("mprotect R %p failed: %s", (void *)start, strerror(errno));
    return 0;
}

static void patch_rel_table(ElfW(Addr) base, const ElfW(Rel) *rel, size_t count, const ElfW(Sym) *symtab,
                            const char *strtab) {
    for (size_t i = 0; i < count; ++i) {
        unsigned type = ELF32_R_TYPE(rel[i].r_info);
        unsigned symi = ELF32_R_SYM(rel[i].r_info);
        if (!symi || (type != R_ARM_JUMP_SLOT && type != R_ARM_GLOB_DAT && type != R_ARM_ABS32)) continue;
        const char *name = strtab + symtab[symi].st_name;
        for (size_t h = 0; h < sizeof(g_hooks) / sizeof(g_hooks[0]); ++h) {
            if (strcmp(name, g_hooks[h].name) != 0) continue;
            uintptr_t slot = (uintptr_t)(base + rel[i].r_offset);
            if (patch_slot(slot, g_hooks[h].hook) == 0) {
                g_hooks[h].patched++;
                LOGI("hooked %s slot=%p type=%u", name, (void *)slot, type);
            }
        }
    }
}

typedef struct {
    const char *soname;
    int found;
} find_ctx_t;

static int phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    find_ctx_t *ctx = (find_ctx_t *)data;
    const char *name = info->dlpi_name;
    if (!name) return 0;
    size_t nl = strlen(name), sl = strlen(ctx->soname);
    if (nl < sl || strcmp(name + nl - sl, ctx->soname) != 0) return 0;
    if (nl > sl && name[nl - sl - 1] != '/') return 0;
    ElfW(Addr) base = info->dlpi_addr;
    const ElfW(Dyn) *dyn = NULL;
    for (int i = 0; i < info->dlpi_phnum; ++i)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(base + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) return 0;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    const ElfW(Rel) *jmprel = NULL, *rel = NULL;
    size_t pltrelsz = 0, relsz = 0;
    for (; dyn->d_tag != DT_NULL; ++dyn) {
        switch (dyn->d_tag) {
        case DT_SYMTAB: symtab = (const ElfW(Sym) *)(base + dyn->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)(base + dyn->d_un.d_ptr); break;
        case DT_JMPREL: jmprel = (const ElfW(Rel) *)(base + dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
        case DT_REL: rel = (const ElfW(Rel) *)(base + dyn->d_un.d_ptr); break;
        case DT_RELSZ: relsz = dyn->d_un.d_val; break;
        default: break;
        }
    }
    if (!symtab || !strtab) return 0;
    if (jmprel) patch_rel_table(base, jmprel, pltrelsz / sizeof(ElfW(Rel)), symtab, strtab);
    if (rel) patch_rel_table(base, rel, relsz / sizeof(ElfW(Rel)), symtab, strtab);
    ctx->found = 1;
    LOGI("patched %s at base=%p", info->dlpi_name, (void *)base);
    return 1;
}

JNIEXPORT jint JNICALL Java_com_ea_ironmonkey_port_NfsVfs_nativeInstall(JNIEnv *env, jclass cls, jstring jvpath,
                                                                         jint fd, jlong offset, jlong length) {
    (void)cls;
    const char *vp = (*env)->GetStringUTFChars(env, jvpath, NULL);
    if (!vp) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = 0;
    if (g_fd >= 0) {
        LOGI("already installed for %s", g_vpath);
        rc = strcmp(vp, g_vpath) == 0 ? 0 : -2;
        goto out;
    }
    if (strlen(vp) >= sizeof(g_vpath) || fd < 0 || offset < 0 || length <= 0) {
        rc = -3;
        goto out;
    }
    int dfd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (dfd < 0) {
        LOGE("dup(%d) failed: %s", fd, strerror(errno));
        rc = -4;
        goto out;
    }
    unsigned char eocd[4] = {0};
    if (pread64(dfd, eocd, 4, offset + length - 22) != 4 || eocd[0] != 'P' || eocd[1] != 'K' || eocd[2] != 5 ||
        eocd[3] != 6) {
        LOGE("embedded range is not a zip with a plain EOCD (off=%lld len=%lld)", (long long)offset, (long long)length);
        close(dfd);
        rc = -5;
        goto out;
    }
    strcpy(g_vpath, vp);
    g_off = offset;
    g_len = length;
    g_fd = dfd;
    find_ctx_t ctx = {"libapp.so", 0};
    dl_iterate_phdr(phdr_cb, &ctx);
    if (!ctx.found || !g_hooks[0].patched || !g_hooks[1].patched) {
        LOGE("libapp.so hooks incomplete: found=%d fopen=%d stat=%d", ctx.found, g_hooks[0].patched, g_hooks[1].patched);
        rc = -6;
    } else {
        LOGI("installed vpath=%s off=%lld len=%lld", g_vpath, (long long)g_off, (long long)g_len);
    }
out:
    pthread_mutex_unlock(&g_lock);
    (*env)->ReleaseStringUTFChars(env, jvpath, vp);
    return rc;
}

JNIEXPORT jlongArray JNICALL Java_com_ea_ironmonkey_port_NfsVfs_nativeStats(JNIEnv *env, jclass cls) {
    (void)cls;
    jlongArray arr = (*env)->NewLongArray(env, 3);
    if (!arr) return NULL;
    jlong v[3] = {g_open_count, g_read_bytes, g_fd >= 0 ? 1 : 0};
    (*env)->SetLongArrayRegion(env, arr, 0, 3, v);
    return arr;
}
