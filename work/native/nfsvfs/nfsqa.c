/*
 * nfsqa: QA catalog / cheat actions for NFS Most Wanted 1.3.128 (arm32 path).
 *
 * Replays engine sequences that already exist in libapp.so instead of inventing new
 * game logic (evidence: work/hidden_content.md HC-01/HC-02, IDA c73y):
 *   cash grant  = StoreScreen_OnUpdate_Grant 0x19FF14..0x19FFC0:
 *                 profile=0x77B70(); ChangeSet ctor 0x251D58(&cs, profile);
 *                 sym=0x406644("store"); AddCash 0x256FF8(cs.ptr, amount, &sym);
 *                 Apply 0x257F84(cs.ptr); Save 0x25FF7C(0x77680(), &dword_AD49C4); release cs.holder
 *   unlock cars = 0x248184() (Metagame/Profile "Car/Unlock/All"; applies its own ChangeSet)
 *   unlock events = 0x25002C(profile) ("Events/Unlock All")
 *   award all   = 0x229E9C(awards=0x192724()) ("Metagame/Awards/Award All")
 *   debug flags = DebugOptions_Get 0x33FF1C() + byte offset (EverythingIsFree 53, DisableTraffic 72,
 *                 InfinitePlayerNitro 73, WeakenOpponents 74, AIDisableOpponents 40, ...)
 * Every entry point is checked against the expected libapp.so bytes before first use; on any
 * mismatch all actions refuse (rc -100). Callers must run on the game (GL) thread between frames.
 */
#include <android/log.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define TAG "NfsQa"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

enum {
    QA_GRANT_CASH = 1,
    QA_SET_DEBUG_BYTE = 2,
    QA_GET_DEBUG_BYTE = 3,
    QA_UNLOCK_ALL_CARS = 4,
    QA_UNLOCK_ALL_EVENTS = 5,
    QA_AWARD_ALL = 6,
    QA_GET_CASH = 7,
    QA_SAVE = 8,
};

typedef struct {
    uint32_t off;
    uint8_t bytes[8];
} code_sig_t;

static const code_sig_t k_sigs[] = {
    {0x077B70, {0xc0, 0x00, 0x9f, 0xe5, 0x00, 0x00, 0x9f, 0xe7}}, /* Profile::GetInstance */
    {0x077C38, {0x20, 0xae, 0xa5, 0x00, 0, 0, 0, 0}},             /* its literal -> dword_AD299C (4 bytes) */
    {0x077680, {0xc0, 0x00, 0x9f, 0xe5, 0x00, 0x00, 0x9f, 0xe7}}, /* SaveManager::GetInstance */
    {0x077748, {0x0c, 0xb3, 0xa5, 0x00, 0, 0, 0, 0}},             /* literal -> dword_AD2998 */
    {0x192724, {0xc0, 0x00, 0x9f, 0xe5, 0x00, 0x00, 0x9f, 0xe7}}, /* Awards::GetInstance */
    {0x1927EC, {0x68, 0x1a, 0x94, 0x00, 0, 0, 0, 0}},             /* literal -> dword_AD4198 */
    {0x251D58, {0x70, 0x4c, 0x2d, 0xe9, 0x10, 0xb0, 0x8d, 0xe2}}, /* ChangeSet ctor */
    {0x406644, {0x10, 0x4c, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}}, /* symbol from C string */
    {0x256FF8, {0xf0, 0x4d, 0x2d, 0xe9, 0x18, 0xb0, 0x8d, 0xe2}}, /* ChangeSet::AddCash */
    {0x257F84, {0x70, 0x4c, 0x2d, 0xe9, 0x10, 0xb0, 0x8d, 0xe2}}, /* ChangeSet::Apply */
    {0x25FF7C, {0xf0, 0x48, 0x2d, 0xe9, 0x10, 0xb0, 0x8d, 0xe2}}, /* SaveManager::Save */
    {0x33FF1C, {0x30, 0x48, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}}, /* DebugOptions_Get */
    {0x248184, {0xf0, 0x4f, 0x2d, 0xe9, 0x1c, 0xb0, 0x8d, 0xe2}}, /* Car/Unlock/All */
    {0x25002C, {0xf0, 0x4f, 0x2d, 0xe9, 0x1c, 0xb0, 0x8d, 0xe2}}, /* Events/Unlock All */
    {0x229E9C, {0x30, 0x48, 0x2d, 0xe9, 0x08, 0xb0, 0x8d, 0xe2}}, /* Award All */
};
static const uint8_t k_sig_len[] = {8, 4, 8, 4, 8, 4, 8, 8, 8, 8, 8, 8, 8, 8, 8};

#define DEBUG_OPTIONS_SIZE 0x7C  /* malloc size in DebugOptions_Get */
#define SAVE_ARG_OFF 0xAD49C4    /* dword_AD49C4 (value of GOT slot 0xAC61E4) */
#define PROFILE_CASH_OFF 4       /* Profile_DebugAddCash: *(profile+4) += amount */

typedef void *(*fn_get_t)(void);
typedef void (*fn_cs_ctor_t)(void *cs, void *profile);
typedef void (*fn_sym_t)(void *sym, const char *text);
typedef void (*fn_add_cash_t)(void *cs, int amount, void *sym);
typedef void (*fn_apply_t)(void *cs);
typedef void (*fn_save_t)(void *save_mgr, void *arg);
typedef void (*fn_void_t)(void);
typedef void (*fn_obj_t)(void *obj);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t g_base;
static int g_state; /* 0 unknown, 1 verified, -1 mismatch */

static int find_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    const char *n = info->dlpi_name;
    if (!n) return 0;
    size_t l = strlen(n);
    if (l >= 10 && strcmp(n + l - 10, "/libapp.so") == 0) {
        *(uintptr_t *)data = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static int ensure_verified(void) {
    if (g_state) return g_state;
    uintptr_t base = 0;
    dl_iterate_phdr(find_cb, &base);
    if (!base) {
        LOGE("libapp.so not loaded");
        return 0;
    }
    for (size_t i = 0; i < sizeof(k_sigs) / sizeof(k_sigs[0]); ++i) {
        if (memcmp((const void *)(base + k_sigs[i].off), k_sigs[i].bytes, k_sig_len[i]) != 0) {
            LOGE("signature mismatch at 0x%06x: libapp.so is not the expected 1.3.128 build", k_sigs[i].off);
            g_state = -1;
            return g_state;
        }
    }
    g_base = base;
    g_state = 1;
    LOGI("libapp.so verified at %p", (void *)base);
    return g_state;
}

#define FN(type, off) ((type)(g_base + (off)))

static void release_holder(uint32_t *holder) {
    /* boost::shared_ptr counted base: [vtbl][use][weak][ptr]; dispose on use==0, destroy on weak==0 */
    if (!holder) return;
    if (--holder[1] == 0) {
        void (**vt)(void *) = (void (**)(void *))(uintptr_t)holder[0];
        vt[2](holder);
        if (--holder[2] == 0) vt[3](holder);
    }
}

static void save_game(void) {
    void *sm = FN(fn_get_t, 0x077680)();
    FN(fn_save_t, 0x25FF7C)(sm, (void *)(g_base + SAVE_ARG_OFF));
}

static int grant_cash(int amount) {
    if (amount <= 0 || amount > 100000000) return -2;
    void *profile = FN(fn_get_t, 0x077B70)();
    if (!profile) return -3;
    uint32_t cs[2] = {0, 0};
    FN(fn_cs_ctor_t, 0x251D58)(cs, profile);
    uint32_t sym = 0;
    FN(fn_sym_t, 0x406644)(&sym, "store");
    FN(fn_add_cash_t, 0x256FF8)((void *)(uintptr_t)cs[0], amount, &sym);
    FN(fn_apply_t, 0x257F84)((void *)(uintptr_t)cs[0]);
    save_game();
    release_holder((uint32_t *)(uintptr_t)cs[1]);
    return 0;
}

JNIEXPORT jint JNICALL Java_com_ea_ironmonkey_port_NfsQa_nativeAction(JNIEnv *env, jclass cls, jint action, jint arg) {
    (void)env;
    (void)cls;
    pthread_mutex_lock(&g_lock);
    int rc;
    if (ensure_verified() != 1) {
        rc = -100;
        goto out;
    }
    switch (action) {
    case QA_GRANT_CASH:
        rc = grant_cash(arg);
        LOGI("grant cash %d -> rc=%d", arg, rc);
        break;
    case QA_SET_DEBUG_BYTE: {
        int off = (arg >> 8) & 0xFF, val = arg & 0xFF;
        if (off >= DEBUG_OPTIONS_SIZE) {
            rc = -2;
            break;
        }
        uint8_t *opts = (uint8_t *)FN(fn_get_t, 0x33FF1C)();
        if (!opts) {
            rc = -3;
            break;
        }
        opts[off] = (uint8_t)val;
        LOGI("debug byte +%d = %d", off, val);
        rc = 0;
        break;
    }
    case QA_GET_DEBUG_BYTE: {
        int off = arg & 0xFF;
        uint8_t *opts = (uint8_t *)FN(fn_get_t, 0x33FF1C)();
        rc = (opts && off < DEBUG_OPTIONS_SIZE) ? opts[off] : -2;
        break;
    }
    case QA_UNLOCK_ALL_CARS:
        FN(fn_void_t, 0x248184)();
        save_game();
        LOGI("unlock all cars applied");
        rc = 0;
        break;
    case QA_UNLOCK_ALL_EVENTS: {
        void *profile = FN(fn_get_t, 0x077B70)();
        FN(fn_obj_t, 0x25002C)(profile);
        save_game();
        LOGI("unlock all events applied");
        rc = 0;
        break;
    }
    case QA_AWARD_ALL: {
        void *awards = FN(fn_get_t, 0x192724)();
        FN(fn_obj_t, 0x229E9C)(awards);
        save_game();
        LOGI("award all applied");
        rc = 0;
        break;
    }
    case QA_GET_CASH: {
        uint8_t *profile = (uint8_t *)FN(fn_get_t, 0x077B70)();
        rc = profile ? *(int32_t *)(profile + PROFILE_CASH_OFF) : -3;
        break;
    }
    case QA_SAVE:
        save_game();
        rc = 0;
        break;
    default:
        rc = -1;
    }
out:
    pthread_mutex_unlock(&g_lock);
    return rc;
}
