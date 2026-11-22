/*
 * port/3ds/audio_3ds.c
 *
 * Nintendo 3DS Miles Sound System (AIL_*) backend using libctru 24-channel ndsp.
 *
 * Channel Allocation (24 hardware DSP channels):
 *   - Channels  0..15 (16 voices): Simultaneous IMA-ADPCM / PCM SFX (audio.bag)
 *   - Channel      16 (1 stream) : Seamless zero-click looping IMA-ADPCM Music (music\*.wav)
 *   - Channel      17 (1 stream) : MP3-in-WAV (minimp3.h) / PCM Dialog (dialog\*.wav)
 *   - Channels 18..23 (6 spare)  : Overflow pool for overlapping opens / extra voices
 *
 * Memory & Cache Coherency:
 *   - All ndspWaveBuf PCM buffers live in linear memory allocated via linearAlloc().
 *   - DSP_FlushDataCache() is called immediately before every ndspChnWaveBufAdd().
 *   - stream_adpcm_seek() allocates its temporary decode buffer on the heap (never stack).
 *
 * OpenNox Backports:
 *   - PR #376: Synchronous main-thread timer dispatch & audio pumping in AIL_serve().
 *   - PR #502: Strict magic-tagged handle validation & error/bounds guards on all AIL_* APIs.
 *
 * Hardware Diagnostics:
 *   - Checks for sdmc:/3ds/dspfirm.cdc before calling ndspInit().
 *   - If missing, bypasses ndspInit(), logs a clear warning to console/stderr, and boots silently.
 */

#include <3ds.h>

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "platform_3ds.h"
#include "../../src/windows.h"
#include "../../src/compat_mss.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "../../src/minimp3.h"

/* =========================================================================
 * Constants & Channel Map
 * ========================================================================= */
#define AUDIO_3DS_DSP_FIRM_PATH      "sdmc:/3ds/dspfirm.cdc"

#define AUDIO_3DS_MAX_CHANNELS       24
#define AUDIO_3DS_SFX_CH_BASE        0
#define AUDIO_3DS_SFX_CH_COUNT       16   /* Channels 0..15 */
#define AUDIO_3DS_MUSIC_CH           16   /* Channel  16    */
#define AUDIO_3DS_DIALOG_CH          17   /* Channel  17    */
#define AUDIO_3DS_EXTRA_CH_BASE      18
#define AUDIO_3DS_EXTRA_CH_COUNT     6    /* Channels 18..23 */

/* Triple-buffered SFX (8 KB each -> 24 KB/voice, 384 KB total for 16 SFX voices) */
#define AUDIO_3DS_SFX_WBUFS          3
#define AUDIO_3DS_SFX_WBUF_SAMPLES   4096u /* int16_t elements; fits 2048B stereo ADPCM (4082) */
#define AUDIO_3DS_SFX_WBUF_BYTES     (AUDIO_3DS_SFX_WBUF_SAMPLES * sizeof(int16_t))

/* Quad-buffered Streams (16 KB each -> 64 KB/stream, 128 KB total for Music + Dialog) */
#define AUDIO_3DS_STREAM_WBUFS       4
#define AUDIO_3DS_STREAM_WBUF_SAMPLES 8192u /* int16_t elements; >= 2048 stereo frames + MP3 headroom */
#define AUDIO_3DS_STREAM_WBUF_BYTES  (AUDIO_3DS_STREAM_WBUF_SAMPLES * sizeof(int16_t))

#define AUDIO_3DS_MAX_TIMERS         16
#define AUDIO_3DS_MAX_PREFS          64
#define AUDIO_3DS_DECLICK_FRAMES     32u

/* Miles Sound System stream/sample status values */
#define AIL_SMP_FREE                 1
#define AIL_SMP_DONE                 2
#define AIL_SMP_PLAYING              4
#define AIL_SMP_STOPPED              8

/* Handle magic tags for OpenNox PR #502 validation */
#define AUDIO_3DS_MAGIC_DIG          0x44494733u /* "DIG3" */
#define AUDIO_3DS_MAGIC_SAMPLE       0x534D5033u /* "SMP3" */
#define AUDIO_3DS_MAGIC_STREAM       0x53545233u /* "STR3" */
#define AUDIO_3DS_MAGIC_TIMER        0x544D5233u /* "TMR3" */
#define AUDIO_3DS_MAGIC_FREED        0xDEAD3D50u

typedef enum {
    STREAM_ROLE_MUSIC = 0,
    STREAM_ROLE_DIALOG = 1,
    STREAM_ROLE_GENERIC = 2
} audio_stream_role_t;

/* =========================================================================
 * Internal Data Structures
 * ========================================================================= */
struct _DIG_DRIVER
{
    uint32_t         magic;
    nox_3ds_mutex_t *mutex;
    HSAMPLE          sample_head;
    HSTREAM          stream_head;
    bool             released;
    struct _DIG_DRIVER *next_driver;
};

struct _SAMPLE_BUFFER
{
    const void  *buffer;
    unsigned int length;
    unsigned int position;
};

struct _SAMPLE
{
    uint32_t     magic;
    HDIGDRIVER   dig;
    HSAMPLE      next;

    int          dsp_ch;
    bool         owns_linear_pool;
    uint8_t     *linear_pool;
    ndspWaveBuf  wbufs[AUDIO_3DS_SFX_WBUFS];
    int          next_wbuf;

    struct _SAMPLE_BUFFER buffers[2];
    BYTE         current;
    BYTE         ready;

    BYTE         stereo;
    BYTE         adpcm;
    BYTE         bit16;

    unsigned int playback_rate;
    unsigned int block_size;
    int          volume; /* 0..127 */
    int          pan;    /* 0..127 */
    bool         format_dirty;
    bool         rate_dirty;
    bool         mix_dirty;

    unsigned int playing;
    unsigned int paused;
    unsigned int input_exhausted;

    AILSAMPLECB  eob;
    AILSAMPLECB  eos;
    S32          user[8];
};

struct _STREAM
{
    uint32_t            magic;
    HDIGDRIVER          dig;
    HSTREAM             next;
    audio_stream_role_t role;

    int                 dsp_ch;
    bool                owns_linear_pool;
    uint8_t            *linear_pool;
    ndspWaveBuf         wbufs[AUDIO_3DS_STREAM_WBUFS];
    int                 next_wbuf;

    BYTE                stereo;
    unsigned int        playback_rate;
    int                 volume; /* 0..127 */
    unsigned int        playing;
    unsigned int        paused;
    unsigned int        eof;
    bool                loop_seamless;
    bool                loop_declick_pending;
    bool                have_last_frame;
    int16_t             last_frame[2];

    FILE               *file;
    unsigned int        file_size;
    unsigned int        chunk_size;
    unsigned int        chunk_pos;
    unsigned int        buffered;

    union
    {
        struct
        {
            WAVEFORMAT   wf;
            unsigned int position;
        } pcm;
        struct
        {
            WAVEFORMAT   wf;
            unsigned int position;
            unsigned int samples;
            unsigned int samples_per_block;
        } adpcm;
        struct
        {
            MPEGLAYER3WAVEFORMAT wf;
            mp3dec_t             dec;
        } mp3;
    };

    /* Dynamically allocated on heap when ADPCM seek lands mid-block (zero stack usage) */
    int16_t            *adpcm_seek_pcm;
    unsigned int        adpcm_seek_pcm_cap;
    unsigned int        adpcm_seek_pcm_samples;
    unsigned int        adpcm_seek_pcm_pos;

    unsigned int (*decode)(HSTREAM, int16_t *, unsigned int);
    void         (*seek)(HSTREAM, unsigned int);
    unsigned int (*tell)(HSTREAM);

    BYTE                buffer[16 * 1024];
};

struct _TIMER
{
    uint32_t     magic;
    bool         in_use;
    bool         running;
    unsigned int interval_ms;
    uint32_t     next_fire_ms;
    AILTIMERCB   cb;
    U32          user;
};

/* =========================================================================
 * Global Subsystem State
 * ========================================================================= */
static bool             g_audio_inited = false;
static bool             g_dsp_available = false;
static bool             g_in_serve = false;
static nox_3ds_mutex_t *g_global_mutex = NULL;
static HDIGDRIVER       g_driver_head = NULL;

static bool             g_ch_in_use[AUDIO_3DS_MAX_CHANNELS];
static uint8_t         *g_sfx_linear_pool = NULL;    /* 16 * 3 * 8 KB  = 384 KB */
static uint8_t         *g_stream_linear_pool = NULL; /*  2 * 4 * 16 KB = 128 KB */

static struct _TIMER    g_timers[AUDIO_3DS_MAX_TIMERS];
static S32              g_prefs[AUDIO_3DS_MAX_PREFS];
static bool             g_prefs_inited = false;
static char             g_last_error[256] = "No error";

/* =========================================================================
 * OpenNox PR #502 Handle Validation & Error Helpers
 * ========================================================================= */
static void set_last_error(const char *msg)
{
    if (!msg)
        msg = "Unknown audio error";
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

static inline bool is_valid_ptr(const void *p)
{
    return p != NULL && p != (const void *)(intptr_t)-1;
}

static inline bool valid_dig(HDIGDRIVER d)
{
    return is_valid_ptr(d) && d->magic == AUDIO_3DS_MAGIC_DIG;
}

static inline bool valid_sample(HSAMPLE s)
{
    return is_valid_ptr(s) && s->magic == AUDIO_3DS_MAGIC_SAMPLE && valid_dig(s->dig);
}

static inline bool valid_stream(HSTREAM st)
{
    return is_valid_ptr(st) && st->magic == AUDIO_3DS_MAGIC_STREAM && valid_dig(st->dig);
}

static inline bool valid_timer(HTIMER t)
{
    if (!is_valid_ptr(t))
        return false;
    if (t < &g_timers[0] || t >= &g_timers[AUDIO_3DS_MAX_TIMERS])
        return false;
    return t->in_use && t->magic == AUDIO_3DS_MAGIC_TIMER;
}

static void init_default_prefs(void)
{
    if (g_prefs_inited)
        return;
    memset(g_prefs, 0, sizeof(g_prefs));
    /* Default Miles Sound System preferences expected by Nox:
     * 15 = DIG_USE_WAVEOUT (0 initially)
     * 34 = AIL_SERVE / mix latency divisor (non-zero so sub_43F140 works)
     * 35 = max serve rate (125)
     * 42 = current service rate
     */
    g_prefs[34] = 16;
    g_prefs[35] = 125;
    g_prefs[42] = 60;
    g_prefs_inited = true;
}

/* =========================================================================
 * IMA-ADPCM Decoder (Pure Integer C, Zero Stack Overhead)
 * ========================================================================= */
static const int k_ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int k_ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t sat16(int x)
{
    if (x < INT16_MIN) return INT16_MIN;
    if (x > INT16_MAX) return INT16_MAX;
    return (int16_t)x;
}

static inline int satindex(int x)
{
    if (x < 0) return 0;
    if (x > 88) return 88;
    return x;
}

static inline int16_t decode_nibble(int16_t predictor, unsigned int nibble, unsigned int index)
{
    int step = k_ima_step_table[index];
    int diff = step >> 3;
    if (nibble & 4u) diff += step;
    if (nibble & 2u) diff += step >> 1;
    if (nibble & 1u) diff += step >> 2;
    if (nibble & 8u)
        return sat16((int)predictor - diff);
    return sat16((int)predictor + diff);
}

static unsigned int decode_adpcm_mono(int16_t *out, unsigned int max_out,
                                      const BYTE *data, unsigned int size)
{
    if (!out || !data || size < 4 || max_out == 0)
        return 0;

    int16_t predictor = (int16_t)(data[0] | ((unsigned int)data[1] << 8));
    int index = satindex((int)data[2]);
    unsigned int samples = 0;

    out[samples++] = predictor;

    for (unsigned int i = 4; i < size && samples + 2 <= max_out; ++i) {
        unsigned int b  = (unsigned int)data[i];
        unsigned int n0 = b & 0x0Fu;
        unsigned int n1 = (b >> 4) & 0x0Fu;

        predictor = decode_nibble(predictor, n0, (unsigned int)index);
        index = satindex(index + k_ima_index_table[n0]);
        out[samples++] = predictor;

        predictor = decode_nibble(predictor, n1, (unsigned int)index);
        index = satindex(index + k_ima_index_table[n1]);
        out[samples++] = predictor;
    }

    return samples;
}

static unsigned int decode_adpcm_stereo(int16_t *out, unsigned int max_out,
                                        const BYTE *data, unsigned int size)
{
    if (!out || !data || size < 8 || max_out < 2)
        return 0;

    int16_t lpredictor = (int16_t)(data[0] | ((unsigned int)data[1] << 8));
    int lindex = satindex((int)data[2]);
    int16_t rpredictor = (int16_t)(data[4] | ((unsigned int)data[5] << 8));
    int rindex = satindex((int)data[6]);
    unsigned int samples = 0;

    out[samples++] = lpredictor;
    out[samples++] = rpredictor;

    for (unsigned int i = 8; i + 7 < size; i += 8) {
        for (unsigned int j = 0; j < 4; ++j) {
            if (samples + 4 > max_out)
                return samples;

            unsigned int bl = (unsigned int)data[i + j];
            unsigned int br = (unsigned int)data[i + j + 4];
            unsigned int l0 = bl & 0x0Fu;
            unsigned int l1 = (bl >> 4) & 0x0Fu;
            unsigned int r0 = br & 0x0Fu;
            unsigned int r1 = (br >> 4) & 0x0Fu;

            lpredictor = decode_nibble(lpredictor, l0, (unsigned int)lindex);
            lindex = satindex(lindex + k_ima_index_table[l0]);
            out[samples++] = lpredictor;

            rpredictor = decode_nibble(rpredictor, r0, (unsigned int)rindex);
            rindex = satindex(rindex + k_ima_index_table[r0]);
            out[samples++] = rpredictor;

            lpredictor = decode_nibble(lpredictor, l1, (unsigned int)lindex);
            lindex = satindex(lindex + k_ima_index_table[l1]);
            out[samples++] = lpredictor;

            rpredictor = decode_nibble(rpredictor, r1, (unsigned int)rindex);
            rindex = satindex(rindex + k_ima_index_table[r1]);
            out[samples++] = rpredictor;
        }
    }

    return samples;
}

/* =========================================================================
 * DSP Channel Allocation & Configuration Helpers
 * ========================================================================= */
static int alloc_sfx_channel(void)
{
    for (int ch = AUDIO_3DS_SFX_CH_BASE; ch < AUDIO_3DS_SFX_CH_BASE + AUDIO_3DS_SFX_CH_COUNT; ++ch) {
        if (!g_ch_in_use[ch]) {
            g_ch_in_use[ch] = true;
            return ch;
        }
    }
    for (int ch = AUDIO_3DS_EXTRA_CH_BASE; ch < AUDIO_3DS_MAX_CHANNELS; ++ch) {
        if (!g_ch_in_use[ch]) {
            g_ch_in_use[ch] = true;
            return ch;
        }
    }
    return -1;
}

static int alloc_stream_channel(audio_stream_role_t role)
{
    if (role == STREAM_ROLE_MUSIC && !g_ch_in_use[AUDIO_3DS_MUSIC_CH]) {
        g_ch_in_use[AUDIO_3DS_MUSIC_CH] = true;
        return AUDIO_3DS_MUSIC_CH;
    }
    if (role == STREAM_ROLE_DIALOG && !g_ch_in_use[AUDIO_3DS_DIALOG_CH]) {
        g_ch_in_use[AUDIO_3DS_DIALOG_CH] = true;
        return AUDIO_3DS_DIALOG_CH;
    }
    for (int ch = AUDIO_3DS_EXTRA_CH_BASE; ch < AUDIO_3DS_MAX_CHANNELS; ++ch) {
        if (!g_ch_in_use[ch]) {
            g_ch_in_use[ch] = true;
            return ch;
        }
    }
    /* Last resort if reserved partner slot is free */
    if (!g_ch_in_use[AUDIO_3DS_MUSIC_CH]) {
        g_ch_in_use[AUDIO_3DS_MUSIC_CH] = true;
        return AUDIO_3DS_MUSIC_CH;
    }
    if (!g_ch_in_use[AUDIO_3DS_DIALOG_CH]) {
        g_ch_in_use[AUDIO_3DS_DIALOG_CH] = true;
        return AUDIO_3DS_DIALOG_CH;
    }
    return -1;
}

static void release_dsp_channel(int ch)
{
    if (ch < 0 || ch >= AUDIO_3DS_MAX_CHANNELS)
        return;
    if (g_dsp_available) {
        ndspChnWaveBufClear(ch);
        ndspChnSetPaused(ch, false);
        ndspChnReset(ch);
    }
    g_ch_in_use[ch] = false;
}

static void apply_channel_mix(int ch, int volume, int pan)
{
    if (!g_dsp_available || ch < 0 || ch >= AUDIO_3DS_MAX_CHANNELS)
        return;

    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;

    float vol = (float)volume / 127.0f;
    /* Miles balance law: center (63/64) = 1.0/1.0, hard left (0) = 1.0/0.0, hard right (127) = 0.0/1.0 */
    float l_gain = (pan <= 64) ? 1.0f : (float)(127 - pan) / 63.0f;
    float r_gain = (pan >= 63) ? 1.0f : (float)pan / 63.0f;

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = vol * l_gain;
    mix[1] = vol * r_gain;
    ndspChnSetMix(ch, mix);
}

static bool wbuf_is_reusable(const ndspWaveBuf *wb)
{
    return wb->status == NDSP_WBUF_FREE || wb->status == NDSP_WBUF_DONE;
}

static bool any_wbuf_active(const ndspWaveBuf *wbufs, int count)
{
    for (int i = 0; i < count; ++i) {
        if (wbufs[i].status == NDSP_WBUF_QUEUED || wbufs[i].status == NDSP_WBUF_PLAYING)
            return true;
    }
    return false;
}

static void reset_wbufs(ndspWaveBuf *wbufs, int count, uint8_t *pool, size_t stride_bytes)
{
    for (int i = 0; i < count; ++i) {
        memset(&wbufs[i], 0, sizeof(wbufs[i]));
        wbufs[i].data_vaddr = (const void *)(pool + (size_t)i * stride_bytes);
        wbufs[i].status = NDSP_WBUF_FREE;
        wbufs[i].looping = false;
    }
}

static void submit_wbuf(int ch, ndspWaveBuf *wb, unsigned int total_int16_samples, bool stereo)
{
    if (!g_dsp_available || ch < 0 || ch >= AUDIO_3DS_MAX_CHANNELS || !wb || !wb->data_vaddr)
        return;
    if (total_int16_samples == 0)
        return;

    unsigned int channels = stereo ? 2u : 1u;
    unsigned int frames = total_int16_samples / channels;
    if (frames == 0)
        return;

    u32 byte_len = (u32)(frames * channels * sizeof(int16_t));
    wb->nsamples = (u32)frames;
    wb->offset = 0;
    wb->looping = false;
    wb->status = NDSP_WBUF_FREE;

    /* ARM11 L1/L2 caches are not coherent with DSP1: flush linear buffer before submission. */
    DSP_FlushDataCache(wb->data_vaddr, byte_len);
    ndspChnWaveBufAdd(ch, wb);
}

/* =========================================================================
 * Public 3DS Audio Lifecycle (platform_3ds.h)
 * ========================================================================= */
bool nox_3ds_audio_init(void)
{
    if (g_audio_inited)
        return g_dsp_available;

    init_default_prefs();
    if (!g_global_mutex)
        g_global_mutex = nox_3ds_mutex_create();

    memset(g_ch_in_use, 0, sizeof(g_ch_in_use));
    memset(g_timers, 0, sizeof(g_timers));

    /* Hardware Diagnostics: verify DSP firmware dump exists before calling ndspInit(). */
    struct stat st;
    if (stat(AUDIO_3DS_DSP_FIRM_PATH, &st) != 0 || !S_ISREG(st.st_mode)) {
        const char *warn =
            "[audio3ds] WARNING: sdmc:/3ds/dspfirm.cdc not found!\n"
            "[audio3ds] 3DS DSP firmware is required for audio (dump via DSP1 or GodMode9).\n"
            "[audio3ds] Bypassing ndspInit(); Nox will boot and run silently.\n";
        fputs(warn, stdout);
        fputs(warn, stderr);
        fflush(stdout);
        fflush(stderr);

        set_last_error("sdmc:/3ds/dspfirm.cdc missing; audio disabled");
        g_audio_inited = true;
        g_dsp_available = false;
        nox_3ds_state_mut()->dsp_audio_ready = false;
        return false;
    }

    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        fprintf(stdout, "[audio3ds] WARNING: ndspInit() failed (rc=0x%08lX); running silently.\n",
                (unsigned long)rc);
        fprintf(stderr, "[audio3ds] WARNING: ndspInit() failed (rc=0x%08lX); running silently.\n",
                (unsigned long)rc);
        fflush(stdout);
        fflush(stderr);

        set_last_error("ndspInit() failed");
        g_audio_inited = true;
        g_dsp_available = false;
        nox_3ds_state_mut()->dsp_audio_ready = false;
        return false;
    }

    /* Pre-allocate contiguous linear RAM pools for reserved channels (16 SFX + 2 Streams = 512 KB). */
    size_t sfx_pool_bytes    = (size_t)AUDIO_3DS_SFX_CH_COUNT * AUDIO_3DS_SFX_WBUFS * AUDIO_3DS_SFX_WBUF_BYTES;
    size_t stream_pool_bytes = 2u * AUDIO_3DS_STREAM_WBUFS * AUDIO_3DS_STREAM_WBUF_BYTES;

    g_sfx_linear_pool = (uint8_t *)linearAlloc(sfx_pool_bytes);
    g_stream_linear_pool = (uint8_t *)linearAlloc(stream_pool_bytes);
    if (!g_sfx_linear_pool || !g_stream_linear_pool) {
        fprintf(stderr, "[audio3ds] ERROR: linearAlloc failed for audio pools (%u + %u bytes)\n",
                (unsigned)sfx_pool_bytes, (unsigned)stream_pool_bytes);
        if (g_sfx_linear_pool) { linearFree(g_sfx_linear_pool); g_sfx_linear_pool = NULL; }
        if (g_stream_linear_pool) { linearFree(g_stream_linear_pool); g_stream_linear_pool = NULL; }
        ndspExit();
        set_last_error("linearAlloc failed for ndsp wave buffers");
        g_audio_inited = true;
        g_dsp_available = false;
        nox_3ds_state_mut()->dsp_audio_ready = false;
        return false;
    }

    memset(g_sfx_linear_pool, 0, sfx_pool_bytes);
    memset(g_stream_linear_pool, 0, stream_pool_bytes);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetClippingMode(NDSP_CLIP_SOFT);
    ndspSetMasterVol(1.0f);

    for (int ch = 0; ch < AUDIO_3DS_MAX_CHANNELS; ++ch) {
        ndspChnReset(ch);
        ndspChnInitParams(ch);
        ndspChnSetInterp(ch, (ch == AUDIO_3DS_MUSIC_CH || ch == AUDIO_3DS_DIALOG_CH)
                             ? NDSP_INTERP_POLYPHASE
                             : NDSP_INTERP_LINEAR);
        ndspChnSetRate(ch, 22050.0f);
        ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
        apply_channel_mix(ch, 127, 63);
    }

    g_audio_inited = true;
    g_dsp_available = true;
    nox_3ds_state_mut()->dsp_audio_ready = true;

    printf("[audio3ds] ndspInit() OK: 24 ch (0..15 SFX, 16 Music, 17 Dialog, 18..23 spare) | linear=%u KB\n",
           (unsigned)((sfx_pool_bytes + stream_pool_bytes) / 1024u));
    return true;
}

bool nox_3ds_audio_is_available(void)
{
    return g_dsp_available;
}

void nox_3ds_audio_shutdown(void)
{
    if (!g_audio_inited)
        return;

    if (g_global_mutex)
        nox_3ds_mutex_lock(g_global_mutex);

    /* Stop all timers */
    for (int i = 0; i < AUDIO_3DS_MAX_TIMERS; ++i) {
        g_timers[i].running = false;
        g_timers[i].in_use = false;
        g_timers[i].magic = AUDIO_3DS_MAGIC_FREED;
    }

    /* Close all open drivers, samples, and streams */
    while (g_driver_head) {
        HDIGDRIVER dig = g_driver_head;
        g_driver_head = dig->next_driver;

        while (dig->sample_head) {
            HSAMPLE s = dig->sample_head;
            dig->sample_head = s->next;
            release_dsp_channel(s->dsp_ch);
            if (s->owns_linear_pool && s->linear_pool) {
                linearFree(s->linear_pool);
                s->linear_pool = NULL;
            }
            s->magic = AUDIO_3DS_MAGIC_FREED;
            free(s);
        }

        while (dig->stream_head) {
            HSTREAM st = dig->stream_head;
            dig->stream_head = st->next;
            release_dsp_channel(st->dsp_ch);
            if (st->owns_linear_pool && st->linear_pool) {
                linearFree(st->linear_pool);
                st->linear_pool = NULL;
            }
            if (st->file) {
                fclose(st->file);
                st->file = NULL;
            }
            free(st->adpcm_seek_pcm);
            st->magic = AUDIO_3DS_MAGIC_FREED;
            free(st);
        }

        if (dig->mutex) {
            nox_3ds_mutex_destroy(dig->mutex);
            dig->mutex = NULL;
        }
        dig->magic = AUDIO_3DS_MAGIC_FREED;
        free(dig);
    }

    if (g_dsp_available) {
        for (int ch = 0; ch < AUDIO_3DS_MAX_CHANNELS; ++ch) {
            ndspChnWaveBufClear(ch);
            ndspChnReset(ch);
            g_ch_in_use[ch] = false;
        }
        ndspExit();
        g_dsp_available = false;
    }

    /* Free linear pools strictly AFTER ndspExit() so DSP1 cannot read freed memory. */
    if (g_sfx_linear_pool) {
        linearFree(g_sfx_linear_pool);
        g_sfx_linear_pool = NULL;
    }
    if (g_stream_linear_pool) {
        linearFree(g_stream_linear_pool);
        g_stream_linear_pool = NULL;
    }

    nox_3ds_state_mut()->dsp_audio_ready = false;
    g_audio_inited = false;

    if (g_global_mutex) {
        nox_3ds_mutex_unlock(g_global_mutex);
        nox_3ds_mutex_destroy(g_global_mutex);
        g_global_mutex = NULL;
    }
}

/* =========================================================================
 * Stream Format Helpers (RIFF WAV: PCM16, IMA-ADPCM, MP3-in-WAV)
 * ========================================================================= */
static void stream_find_data(HSTREAM stream)
{
    char tmp[8];

    stream->chunk_size = 0;
    stream->chunk_pos = 0;
    if (!stream->file)
        return;

    while (1) {
        long pos = ftell(stream->file);
        if (pos < 0 || (unsigned long)pos >= (unsigned long)stream->file_size)
            break;
        if (fread(tmp, 8, 1, stream->file) != 1)
            break;
        unsigned int size = nox_read_u32(tmp + 4);
        unsigned int skip = size + (size & 1u); /* RIFF chunks are word-aligned */
        if (memcmp(tmp, "data", 4) == 0) {
            stream->chunk_size = size;
            stream->chunk_pos = 0;
            break;
        }
        if (fseek(stream->file, (long)skip, SEEK_CUR) != 0)
            break;
    }
}

static unsigned int stream_pcm_decode(HSTREAM stream, int16_t *out, unsigned int max_samples)
{
    unsigned int channels = stream->stereo ? 2u : 1u;
    unsigned int remaining = stream->chunk_size - stream->chunk_pos;

    if (!remaining) {
        stream_find_data(stream);
        remaining = stream->chunk_size - stream->chunk_pos;
        if (!remaining) {
            stream->eof = 1;
            return 0;
        }
    }

    unsigned int max_bytes = max_samples * sizeof(int16_t);
    if (remaining > max_bytes)
        remaining = max_bytes;

    unsigned int frame_bytes = 2u * channels;
    unsigned int frames_to_read = remaining / frame_bytes;
    if (frames_to_read == 0) {
        stream->eof = 1;
        return 0;
    }

    size_t got = fread(out, frame_bytes, frames_to_read, stream->file);
    if (got == 0) {
        stream->eof = 1;
        return 0;
    }

    stream->chunk_pos += (unsigned int)(got * frame_bytes);
    stream->pcm.position += (unsigned int)got;
    return (unsigned int)(got * channels);
}

static void stream_pcm_seek(HSTREAM stream, unsigned int position)
{
    unsigned int channels = stream->stereo ? 2u : 1u;
    unsigned int frame_bytes = 2u * channels;
    unsigned int remaining_frames = position;

    stream->pcm.position = 0;
    stream->chunk_size = 0;
    stream->chunk_pos = 0;
    stream->buffered = 0;

    while (1) {
        stream_find_data(stream);
        if (stream->chunk_size == 0)
            return;

        unsigned int chunk_frames = stream->chunk_size / frame_bytes;
        if (remaining_frames < chunk_frames) {
            unsigned int byte_off = remaining_frames * frame_bytes;
            fseek(stream->file, (long)byte_off, SEEK_CUR);
            stream->chunk_pos += byte_off;
            stream->pcm.position += remaining_frames;
            return;
        }

        fseek(stream->file, (long)stream->chunk_size, SEEK_CUR);
        stream->pcm.position += chunk_frames;
        remaining_frames -= chunk_frames;
    }
}

static unsigned int stream_pcm_tell(HSTREAM stream)
{
    return stream->pcm.position;
}

static bool stream_adpcm_rewind_for_loop(HSTREAM stream)
{
    if (!stream->file)
        return false;

    stream->buffered = 0;
    stream->chunk_size = 0;
    stream->chunk_pos = 0;
    stream->adpcm.position = 0;
    stream->adpcm_seek_pcm_samples = 0;
    stream->adpcm_seek_pcm_pos = 0;

    if (fseek(stream->file, 12, SEEK_SET) != 0)
        return false;

    stream_find_data(stream);
    if (stream->chunk_size == 0)
        return false;

    stream->loop_declick_pending = true;
    return true;
}

static unsigned int stream_adpcm_decode(HSTREAM stream, int16_t *out, unsigned int max_samples)
{
    unsigned int block_size = stream->adpcm.wf.nBlockAlign;
    unsigned int channels = stream->stereo ? 2u : 1u;
    bool wrapped_this_call = false;

    if (block_size < (channels * 4u) || block_size > sizeof(stream->buffer)) {
        stream->eof = 1;
        return 0;
    }

    /* Serve cached tail from intra-block seek first */
    if (stream->adpcm_seek_pcm && stream->adpcm_seek_pcm_pos < stream->adpcm_seek_pcm_samples) {
        unsigned int available = stream->adpcm_seek_pcm_samples - stream->adpcm_seek_pcm_pos;
        unsigned int to_copy = available;
        if (to_copy > max_samples)
            to_copy = max_samples;
        to_copy -= (to_copy % channels);
        if (to_copy == 0)
            return 0;

        memcpy(out, stream->adpcm_seek_pcm + stream->adpcm_seek_pcm_pos, to_copy * sizeof(int16_t));
        stream->adpcm_seek_pcm_pos += to_copy;

        if (stream->adpcm_seek_pcm_pos >= stream->adpcm_seek_pcm_samples) {
            stream->adpcm_seek_pcm_pos = 0;
            stream->adpcm_seek_pcm_samples = 0;
        }
        return to_copy;
    }

retry_after_wrap:
    if (stream->adpcm.samples > 0 && stream->adpcm.position >= stream->adpcm.samples) {
        if (stream->loop_seamless && !wrapped_this_call && stream_adpcm_rewind_for_loop(stream)) {
            wrapped_this_call = true;
            goto retry_after_wrap;
        }
        stream->eof = 1;
        return 0;
    }

    /* Ensure we have a full ADPCM block buffered */
    while (stream->buffered < block_size) {
        unsigned int remaining = stream->chunk_size - stream->chunk_pos;
        if (!remaining) {
            stream_find_data(stream);
            remaining = stream->chunk_size - stream->chunk_pos;
            if (!remaining) {
                if (stream->loop_seamless && !wrapped_this_call && stream_adpcm_rewind_for_loop(stream)) {
                    wrapped_this_call = true;
                    goto retry_after_wrap;
                }
                stream->eof = 1;
                return 0;
            }
        }

        unsigned int want = block_size - stream->buffered;
        if (remaining > want)
            remaining = want;

        size_t got = fread(stream->buffer + stream->buffered, 1, remaining, stream->file);
        if (got == 0) {
            if (stream->loop_seamless && !wrapped_this_call && stream_adpcm_rewind_for_loop(stream)) {
                wrapped_this_call = true;
                goto retry_after_wrap;
            }
            stream->eof = 1;
            return 0;
        }
        stream->buffered += (unsigned int)got;
        stream->chunk_pos += (unsigned int)got;
    }

    unsigned int samples = stream->stereo
        ? decode_adpcm_stereo(out, max_samples, stream->buffer, block_size)
        : decode_adpcm_mono(out, max_samples, stream->buffer, block_size);

    stream->buffered -= block_size;
    if (stream->buffered)
        memmove(stream->buffer, stream->buffer + block_size, stream->buffered);

    if (samples == 0) {
        stream->eof = 1;
        return 0;
    }

    unsigned int frames = samples / channels;
    if (stream->adpcm.samples > 0 && frames + stream->adpcm.position > stream->adpcm.samples) {
        frames = stream->adpcm.samples - stream->adpcm.position;
        samples = frames * channels;
    }
    stream->adpcm.position += frames;
    return samples;
}

static void stream_adpcm_seek(HSTREAM stream, unsigned int position)
{
    unsigned int block_size = stream->adpcm.wf.nBlockAlign;
    unsigned int channels = stream->stereo ? 2u : 1u;
    unsigned int samples_per_block = stream->adpcm.samples_per_block;

    stream->buffered = 0;
    stream->chunk_size = 0;
    stream->chunk_pos = 0;
    stream->adpcm.position = 0;
    stream->adpcm_seek_pcm_samples = 0;
    stream->adpcm_seek_pcm_pos = 0;

    if (block_size < (channels * 4u) || block_size > sizeof(stream->buffer))
        return;
    if (samples_per_block == 0)
        samples_per_block = (block_size / channels - 4u) * 2u + 1u;
    if (samples_per_block == 0)
        return;

    unsigned int target_block = position / samples_per_block;
    unsigned int intra_block_samples = position % samples_per_block;
    unsigned int remaining_blocks = target_block;

    for (;;) {
        stream_find_data(stream);
        if (stream->chunk_size == 0)
            return;

        unsigned int chunk_blocks = stream->chunk_size / block_size;
        if (remaining_blocks < chunk_blocks) {
            unsigned int byte_off = remaining_blocks * block_size;
            fseek(stream->file, (long)byte_off, SEEK_CUR);
            stream->chunk_pos += byte_off;
            stream->adpcm.position += remaining_blocks * samples_per_block;
            break;
        }

        fseek(stream->file, (long)stream->chunk_size, SEEK_CUR);
        stream->adpcm.position += chunk_blocks * samples_per_block;
        remaining_blocks -= chunk_blocks;
    }

    if (intra_block_samples == 0)
        return;

    if (stream->chunk_pos + block_size > stream->chunk_size)
        return;

    if (fread(stream->buffer, 1, block_size, stream->file) != block_size)
        return;
    stream->chunk_pos += block_size;

    /* Allocate temporary decode buffer on the heap (NEVER on the 1 MB 3DS thread stack). */
    unsigned int max_block_int16 = (samples_per_block + 4u) * channels;
    int16_t *tmp = (int16_t *)malloc(max_block_int16 * sizeof(int16_t));
    if (!tmp) {
        set_last_error("Out of heap memory in stream_adpcm_seek");
        return;
    }

    unsigned int decoded_samples = stream->stereo
        ? decode_adpcm_stereo(tmp, max_block_int16, stream->buffer, block_size)
        : decode_adpcm_mono(tmp, max_block_int16, stream->buffer, block_size);

    unsigned int skip_samples = intra_block_samples * channels;
    if (skip_samples > decoded_samples)
        skip_samples = decoded_samples;

    unsigned int keep_samples = decoded_samples - skip_samples;
    if (keep_samples > 0) {
        if (stream->adpcm_seek_pcm_cap < keep_samples) {
            int16_t *new_buf = (int16_t *)realloc(stream->adpcm_seek_pcm, keep_samples * sizeof(int16_t));
            if (new_buf) {
                stream->adpcm_seek_pcm = new_buf;
                stream->adpcm_seek_pcm_cap = keep_samples;
            } else {
                keep_samples = 0;
            }
        }
        if (keep_samples > 0 && stream->adpcm_seek_pcm) {
            memcpy(stream->adpcm_seek_pcm, tmp + skip_samples, keep_samples * sizeof(int16_t));
        }
    }

    free(tmp);

    stream->adpcm_seek_pcm_samples = keep_samples;
    stream->adpcm_seek_pcm_pos = 0;
    stream->adpcm.position += intra_block_samples;
}

static unsigned int stream_adpcm_tell(HSTREAM stream)
{
    return stream->adpcm.position;
}

static unsigned int stream_mp3_decode(HSTREAM stream, int16_t *out, unsigned int max_samples)
{
    if (max_samples < (unsigned int)MINIMP3_MAX_SAMPLES_PER_FRAME)
        return 0;

    for (;;) {
        unsigned int remaining = stream->chunk_size - stream->chunk_pos;

        if (stream->buffered < sizeof(stream->buffer)) {
            if (!remaining) {
                stream_find_data(stream);
                remaining = stream->chunk_size - stream->chunk_pos;
            }

            if (remaining) {
                unsigned int space = (unsigned int)sizeof(stream->buffer) - stream->buffered;
                unsigned int to_read = (remaining < space) ? remaining : space;
                size_t got = fread(stream->buffer + stream->buffered, 1, to_read, stream->file);
                stream->buffered += (unsigned int)got;
                stream->chunk_pos += (unsigned int)got;
                remaining = stream->chunk_size - stream->chunk_pos;
            }
        }

        if (stream->buffered == 0) {
            stream->eof = 1;
            return 0;
        }

        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int frames = mp3dec_decode_frame(&stream->mp3.dec, stream->buffer, (int)stream->buffered, out, &info);

        if (info.frame_bytes > 0) {
            unsigned int consumed = (unsigned int)info.frame_bytes;
            if (consumed > stream->buffered)
                consumed = stream->buffered;
            stream->buffered -= consumed;
            if (stream->buffered)
                memmove(stream->buffer, stream->buffer + consumed, stream->buffered);
        }

        if (frames > 0) {
            unsigned int ch = (info.channels > 0) ? (unsigned int)info.channels : (stream->stereo ? 2u : 1u);
            if (ch > 2u) ch = 2u;
            /* Sync DSP channel rate/format if MP3 header differs from WAV container header */
            if (info.hz > 0 && (unsigned int)info.hz != stream->playback_rate) {
                stream->playback_rate = (unsigned int)info.hz;
                if (g_dsp_available && stream->dsp_ch >= 0)
                    ndspChnSetRate(stream->dsp_ch, (float)stream->playback_rate);
            }
            if ((ch > 1u) != (stream->stereo != 0)) {
                stream->stereo = (ch > 1u) ? 1 : 0;
                if (g_dsp_available && stream->dsp_ch >= 0)
                    ndspChnSetFormat(stream->dsp_ch, stream->stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
            }
            return (unsigned int)frames * ch;
        }

        /* No PCM produced yet: if frame_bytes == 0 and no more file data, hit EOF cleanly */
        if (info.frame_bytes == 0) {
            if (!remaining) {
                stream_find_data(stream);
                if (stream->chunk_size == 0) {
                    stream->eof = 1;
                    return 0;
                }
            } else if (stream->buffered >= sizeof(stream->buffer)) {
                /* Buffer full of unsyncable garbage; discard first half to guarantee forward progress */
                unsigned int drop = stream->buffered / 2u;
                stream->buffered -= drop;
                memmove(stream->buffer, stream->buffer + drop, stream->buffered);
            }
        }
    }
}

static void stream_mp3_seek(HSTREAM stream, unsigned int position)
{
    (void)position;
    mp3dec_init(&stream->mp3.dec);
    stream->buffered = 0;
    stream->chunk_pos = 0;
    stream->chunk_size = 0;
}

static unsigned int stream_mp3_tell(HSTREAM stream)
{
    (void)stream;
    return 0;
}

/* =========================================================================
 * Sample & Stream Pump Workers (Run Synchronously on Main Thread)
 * ========================================================================= */
static void sample_eos(HSAMPLE S)
{
    S->playing = 0;
    S->input_exhausted = 0;
    if (S->eos)
        S->eos(S);
}

static void sample_eob(HSAMPLE S)
{
    if (S->ready < 2)
        S->ready++;
    S->current = (BYTE)(!S->current);
    if (S->eob)
        S->eob(S);
}

static void sample_sync_dsp_params(HSAMPLE S)
{
    if (!g_dsp_available || S->dsp_ch < 0)
        return;

    if (S->format_dirty) {
        ndspChnSetFormat(S->dsp_ch, S->stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        S->format_dirty = false;
    }
    if (S->rate_dirty) {
        float rate = (S->playback_rate >= 4000u && S->playback_rate <= 96000u)
                     ? (float)S->playback_rate
                     : 22050.0f;
        ndspChnSetRate(S->dsp_ch, rate);
        S->rate_dirty = false;
    }
    if (S->mix_dirty) {
        apply_channel_mix(S->dsp_ch, S->volume, S->pan);
        S->mix_dirty = false;
    }
}

static void sample_work(HSAMPLE S)
{
    if (!S->playing || S->paused)
        return;

    sample_sync_dsp_params(S);

    /* If input buffers are exhausted, wait for queued DSP buffers to finish before firing EOS */
    if (S->input_exhausted) {
        if (!any_wbuf_active(S->wbufs, AUDIO_3DS_SFX_WBUFS)) {
            sample_eos(S);
        }
        return;
    }

    if (S->block_size == 0)
        S->block_size = S->stereo ? 2048u : 1024u;

    if (S->ready >= 2) {
        S->input_exhausted = 1;
        if (!any_wbuf_active(S->wbufs, AUDIO_3DS_SFX_WBUFS))
            sample_eos(S);
        return;
    }

    struct _SAMPLE_BUFFER *buf = &S->buffers[S->current];
    if (!buf->buffer || buf->length == 0) {
        sample_eob(S);
        buf = &S->buffers[S->current];
        if (!buf->buffer || buf->length == 0) {
            S->input_exhausted = 1;
            if (!any_wbuf_active(S->wbufs, AUDIO_3DS_SFX_WBUFS))
                sample_eos(S);
            return;
        }
    }

    for (int submitted = 0; submitted < AUDIO_3DS_SFX_WBUFS; ++submitted) {
        ndspWaveBuf *wb = &S->wbufs[S->next_wbuf];
        if (!wbuf_is_reusable(wb))
            break;

        buf = &S->buffers[S->current];
        if (!buf->buffer || buf->length == 0) {
            S->input_exhausted = 1;
            break;
        }

        unsigned int remaining = (buf->position < buf->length) ? (buf->length - buf->position) : 0u;
        unsigned int decoded_samples = 0;

        if (S->adpcm) {
            if (remaining < S->block_size) {
                /* Do not decode truncated ADPCM blocks; advance to next staging buffer. */
                buf->position = buf->length;
                sample_eob(S);
                buf = &S->buffers[S->current];
                if (!buf->buffer || buf->length == 0) {
                    S->input_exhausted = 1;
                    break;
                }
                continue;
            }

            const BYTE *src = (const BYTE *)buf->buffer + buf->position;
            decoded_samples = S->stereo
                ? decode_adpcm_stereo(wb->data_pcm16, AUDIO_3DS_SFX_WBUF_SAMPLES, src, S->block_size)
                : decode_adpcm_mono(wb->data_pcm16, AUDIO_3DS_SFX_WBUF_SAMPLES, src, S->block_size);
            buf->position += S->block_size;
        } else if (S->bit16) {
            unsigned int frame_bytes = S->stereo ? 4u : 2u;
            unsigned int max_bytes = AUDIO_3DS_SFX_WBUF_BYTES;
            unsigned int take = (remaining > max_bytes) ? max_bytes : remaining;
            take -= (take % frame_bytes);
            if (take > 0) {
                memcpy(wb->data_pcm16, (const BYTE *)buf->buffer + buf->position, take);
                decoded_samples = take / sizeof(int16_t);
                buf->position += take;
            } else {
                buf->position = buf->length;
            }
        } else {
            /* Unsigned 8-bit PCM -> signed 16-bit PCM */
            unsigned int channels = S->stereo ? 2u : 1u;
            unsigned int take = (remaining > AUDIO_3DS_SFX_WBUF_SAMPLES) ? AUDIO_3DS_SFX_WBUF_SAMPLES : remaining;
            take -= (take % channels);
            const uint8_t *src = (const uint8_t *)buf->buffer + buf->position;
            for (unsigned int i = 0; i < take; ++i)
                wb->data_pcm16[i] = (int16_t)(((int)src[i] - 128) << 8);
            decoded_samples = take;
            buf->position += take;
        }

        if (decoded_samples > 0) {
            submit_wbuf(S->dsp_ch, wb, decoded_samples, S->stereo != 0);
            S->next_wbuf = (S->next_wbuf + 1) % AUDIO_3DS_SFX_WBUFS;
        }

        if (buf->position >= buf->length) {
            sample_eob(S);
            buf = &S->buffers[S->current];
            if (!buf->buffer || buf->length == 0) {
                S->input_exhausted = 1;
                break;
            }
        }
    }

    if (S->input_exhausted && !any_wbuf_active(S->wbufs, AUDIO_3DS_SFX_WBUFS)) {
        sample_eos(S);
    }
}

static void apply_loop_declick(int16_t *pcm, unsigned int total_samples,
                               unsigned int channels, const int16_t prev[2])
{
    unsigned int frames = total_samples / channels;
    unsigned int ramp = (frames < AUDIO_3DS_DECLICK_FRAMES) ? frames : AUDIO_3DS_DECLICK_FRAMES;
    if (ramp == 0)
        return;

    for (unsigned int f = 0; f < ramp; ++f) {
        /* Smoothly blend from the last pre-loop sample into the looped waveform */
        int w_new = (int)(f + 1u);
        int w_old = (int)(ramp - f);
        int denom = (int)(ramp + 1u);
        for (unsigned int c = 0; c < channels; ++c) {
            int s_new = (int)pcm[f * channels + c];
            int s_old = (int)prev[c];
            pcm[f * channels + c] = sat16((s_old * w_old + s_new * w_new) / denom);
        }
    }
}

static void stream_work(HSTREAM stream)
{
    if (!stream->playing || stream->paused)
        return;

    if (stream->eof) {
        /* Wait until all queued DSP wave buffers finish playing before reporting SMP_DONE */
        if (!any_wbuf_active(stream->wbufs, AUDIO_3DS_STREAM_WBUFS)) {
            stream->playing = 0;
        }
        return;
    }

    unsigned int channels = stream->stereo ? 2u : 1u;
    /* Target ~90 ms per wave buffer, leaving headroom for one full MP3/ADPCM decode block */
    unsigned int target_samples = (stream->playback_rate * channels) / 11u;
    unsigned int max_fill = AUDIO_3DS_STREAM_WBUF_SAMPLES - (unsigned int)MINIMP3_MAX_SAMPLES_PER_FRAME;
    if (target_samples > max_fill)
        target_samples = max_fill;
    if (target_samples < 1024u * channels)
        target_samples = 1024u * channels;

    for (int filled = 0; filled < AUDIO_3DS_STREAM_WBUFS; ++filled) {
        ndspWaveBuf *wb = &stream->wbufs[stream->next_wbuf];
        if (!wbuf_is_reusable(wb))
            break;

        unsigned int offset = 0;
        while (offset < target_samples) {
            unsigned int space = AUDIO_3DS_STREAM_WBUF_SAMPLES - offset;
            if (stream->decode == stream_mp3_decode && space < (unsigned int)MINIMP3_MAX_SAMPLES_PER_FRAME)
                break;

            bool had_declick = stream->loop_declick_pending;
            unsigned int got = stream->decode(stream, wb->data_pcm16 + offset, space);
            if (got == 0)
                break;

            if (!had_declick && stream->loop_declick_pending && stream->have_last_frame) {
                /* Seamless loop wrap happened inside this decode call: de-click the stitch */
                int16_t stitch_prev[2];
                if (offset >= channels) {
                    stitch_prev[0] = wb->data_pcm16[offset - channels];
                    stitch_prev[1] = (channels > 1u) ? wb->data_pcm16[offset - channels + 1u] : stitch_prev[0];
                } else {
                    stitch_prev[0] = stream->last_frame[0];
                    stitch_prev[1] = stream->last_frame[1];
                }
                apply_loop_declick(wb->data_pcm16 + offset, got, channels, stitch_prev);
                stream->loop_declick_pending = false;
            } else if (stream->loop_declick_pending && stream->have_last_frame) {
                apply_loop_declick(wb->data_pcm16 + offset, got, channels, stream->last_frame);
                stream->loop_declick_pending = false;
            }

            offset += got;
            if (offset >= channels) {
                stream->last_frame[0] = wb->data_pcm16[offset - channels];
                stream->last_frame[1] = (channels > 1u) ? wb->data_pcm16[offset - channels + 1u] : stream->last_frame[0];
                stream->have_last_frame = true;
            }
        }

        if (offset == 0)
            break;

        submit_wbuf(stream->dsp_ch, wb, offset, stream->stereo != 0);
        stream->next_wbuf = (stream->next_wbuf + 1) % AUDIO_3DS_STREAM_WBUFS;
    }

    if (stream->eof && !any_wbuf_active(stream->wbufs, AUDIO_3DS_STREAM_WBUFS)) {
        stream->playing = 0;
    }
}

/* =========================================================================
 * Synchronous Audio Pump & OpenNox PR #376 Timer Dispatch
 * ========================================================================= */
void nox_3ds_audio_pump(void)
{
    if (!g_audio_inited || !g_dsp_available)
        return;

    if (g_global_mutex)
        nox_3ds_mutex_lock(g_global_mutex);

    for (HDIGDRIVER dig = g_driver_head; dig; dig = dig->next_driver) {
        if (!valid_dig(dig) || dig->released)
            continue;

        nox_3ds_mutex_lock(dig->mutex);

        for (HSAMPLE s = dig->sample_head; s; s = s->next) {
            if (valid_sample(s) && s->playing)
                sample_work(s);
        }

        for (HSTREAM st = dig->stream_head; st; st = st->next) {
            if (valid_stream(st) && st->playing)
                stream_work(st);
        }

        nox_3ds_mutex_unlock(dig->mutex);
    }

    if (g_global_mutex)
        nox_3ds_mutex_unlock(g_global_mutex);
}

static void dispatch_timers_sync(void)
{
    uint32_t now_ms = nox_3ds_get_ticks_ms();

    for (int i = 0; i < AUDIO_3DS_MAX_TIMERS; ++i) {
        struct _TIMER *t = &g_timers[i];
        if (!t->in_use || !t->running || t->magic != AUDIO_3DS_MAGIC_TIMER || !t->cb)
            continue;

        /* Signed difference handles 32-bit tick wrap cleanly */
        if ((int32_t)(now_ms - t->next_fire_ms) >= 0) {
            unsigned int interval = (t->interval_ms > 0) ? t->interval_ms : 33u;
            uint32_t behind = now_ms - t->next_fire_ms;
            if (behind > 200u) {
                /* Avoid runaway timer burst after loading screens */
                t->next_fire_ms = now_ms + interval;
            } else {
                t->next_fire_ms += interval;
            }

            AILTIMERCB cb = t->cb;
            U32 user = t->user;
            cb(user);
        }
    }
}

/* =========================================================================
 * Miles Sound System (AIL_*) Public API Implementation for 3DS
 * ========================================================================= */
DXDEC S32 AILCALL AIL_startup(void)
{
    init_default_prefs();
    if (!g_audio_inited)
        nox_3ds_audio_init();
    return 0;
}

DXDEC void AILCALL AIL_shutdown(void)
{
    nox_3ds_audio_shutdown();
}

DXDEC void AILCALL AIL_serve(void)
{
    if (g_in_serve)
        return;
    g_in_serve = true;

    /* OpenNox PR #376: Dispatch all Miles timers synchronously on the main thread,
     * then pump all 24 ndsp channels to refill wave buffers and fire EOB/EOS callbacks. */
    dispatch_timers_sync();
    nox_3ds_audio_pump();

    g_in_serve = false;
}

DXDEC char FAR *AILCALL AIL_last_error(void)
{
    return g_last_error;
}

DXDEC S32 AILCALL AIL_get_preference(U32 number)
{
    init_default_prefs();
    if (number < AUDIO_3DS_MAX_PREFS)
        return g_prefs[number];
    return 0;
}

DXDEC S32 AILCALL AIL_set_preference(U32 number, S32 value)
{
    init_default_prefs();
    if (number < AUDIO_3DS_MAX_PREFS) {
        S32 prev = g_prefs[number];
        g_prefs[number] = value;
        return prev;
    }
    return 0;
}

DXDEC S32 AILCALL AIL_waveOutOpen(HDIGDRIVER FAR *pdrvr, LPHWAVEOUT FAR *lphWaveOut,
                                  S32 wDeviceID, LPWAVEFORMAT lpFormat)
{
    (void)lphWaveOut;
    (void)wDeviceID;
    (void)lpFormat;

    if (!pdrvr) {
        set_last_error("AIL_waveOutOpen: NULL pdrvr");
        return -1;
    }
    *pdrvr = NULL;

    if (!g_audio_inited)
        nox_3ds_audio_init();

    if (!g_dsp_available) {
        /* DSP firmware missing or ndspInit failed: return failure cleanly so Nox boots silently. */
        set_last_error("3DS DSP unavailable (missing sdmc:/3ds/dspfirm.cdc)");
        return -1;
    }

    HDIGDRIVER dig = (HDIGDRIVER)calloc(1, sizeof(*dig));
    if (!dig) {
        set_last_error("AIL_waveOutOpen: out of memory");
        return -1;
    }

    dig->mutex = nox_3ds_mutex_create();
    if (!dig->mutex) {
        free(dig);
        set_last_error("AIL_waveOutOpen: mutex creation failed");
        return -1;
    }

    dig->magic = AUDIO_3DS_MAGIC_DIG;
    dig->released = false;

    if (g_global_mutex)
        nox_3ds_mutex_lock(g_global_mutex);
    dig->next_driver = g_driver_head;
    g_driver_head = dig;
    if (g_global_mutex)
        nox_3ds_mutex_unlock(g_global_mutex);

    *pdrvr = dig;
    return 0;
}

DXDEC void AILCALL AIL_waveOutClose(HDIGDRIVER drvr)
{
    if (!valid_dig(drvr))
        return;

    if (g_global_mutex)
        nox_3ds_mutex_lock(g_global_mutex);

    HDIGDRIVER *pp = &g_driver_head;
    while (*pp && *pp != drvr)
        pp = &(*pp)->next_driver;
    if (*pp == drvr)
        *pp = drvr->next_driver;

    if (g_global_mutex)
        nox_3ds_mutex_unlock(g_global_mutex);

    nox_3ds_mutex_lock(drvr->mutex);

    while (drvr->sample_head) {
        HSAMPLE s = drvr->sample_head;
        drvr->sample_head = s->next;
        release_dsp_channel(s->dsp_ch);
        if (s->owns_linear_pool && s->linear_pool) {
            linearFree(s->linear_pool);
            s->linear_pool = NULL;
        }
        s->magic = AUDIO_3DS_MAGIC_FREED;
        free(s);
    }

    while (drvr->stream_head) {
        HSTREAM st = drvr->stream_head;
        drvr->stream_head = st->next;
        release_dsp_channel(st->dsp_ch);
        if (st->owns_linear_pool && st->linear_pool) {
            linearFree(st->linear_pool);
            st->linear_pool = NULL;
        }
        if (st->file) {
            fclose(st->file);
            st->file = NULL;
        }
        free(st->adpcm_seek_pcm);
        st->magic = AUDIO_3DS_MAGIC_FREED;
        free(st);
    }

    nox_3ds_mutex_unlock(drvr->mutex);
    nox_3ds_mutex_destroy(drvr->mutex);
    drvr->mutex = NULL;
    drvr->magic = AUDIO_3DS_MAGIC_FREED;
    free(drvr);
}

DXDEC void AILCALL AIL_digital_configuration(HDIGDRIVER dig, S32 FAR *rate, S32 FAR *format, char FAR *string)
{
    if (rate)
        *rate = 22050;
    if (format)
        *format = 3; /* 16-bit stereo */
    if (string) {
        if (valid_dig(dig) && g_dsp_available)
            strcpy(string, "Nintendo 3DS NDSP 24-ch");
        else
            strcpy(string, "");
    }
}

DXDEC S32 AILCALL AIL_digital_handle_release(HDIGDRIVER drvr)
{
    if (!valid_dig(drvr))
        return 0;

    nox_3ds_mutex_lock(drvr->mutex);
    drvr->released = true;

    for (HSAMPLE s = drvr->sample_head; s; s = s->next) {
        if (!valid_sample(s))
            continue;
        s->playing = 0;
        s->paused = 0;
        s->input_exhausted = 0;
        if (g_dsp_available && s->dsp_ch >= 0) {
            ndspChnWaveBufClear(s->dsp_ch);
            reset_wbufs(s->wbufs, AUDIO_3DS_SFX_WBUFS, s->linear_pool, AUDIO_3DS_SFX_WBUF_BYTES);
            s->next_wbuf = 0;
        }
    }

    for (HSTREAM st = drvr->stream_head; st; st = st->next) {
        if (!valid_stream(st))
            continue;
        st->playing = 0;
        st->paused = 0;
        if (g_dsp_available && st->dsp_ch >= 0) {
            ndspChnWaveBufClear(st->dsp_ch);
            reset_wbufs(st->wbufs, AUDIO_3DS_STREAM_WBUFS, st->linear_pool, AUDIO_3DS_STREAM_WBUF_BYTES);
            st->next_wbuf = 0;
        }
    }

    nox_3ds_mutex_unlock(drvr->mutex);
    return 0;
}

DXDEC S32 AILCALL AIL_digital_handle_reacquire(HDIGDRIVER drvr)
{
    if (!valid_dig(drvr))
        return 0;

    nox_3ds_mutex_lock(drvr->mutex);
    drvr->released = false;
    nox_3ds_mutex_unlock(drvr->mutex);
    return 0;
}

/* =========================================================================
 * Sample (SFX) APIs (Reserved Channels 0..15)
 * ========================================================================= */
DXDEC HSAMPLE AILCALL AIL_allocate_sample_handle(HDIGDRIVER dig)
{
    if (!valid_dig(dig) || !g_dsp_available) {
        set_last_error("AIL_allocate_sample_handle: invalid driver or DSP inactive");
        return NULL;
    }

    nox_3ds_mutex_lock(dig->mutex);

    int ch = alloc_sfx_channel();
    if (ch < 0) {
        nox_3ds_mutex_unlock(dig->mutex);
        set_last_error("AIL_allocate_sample_handle: no free ndsp channels");
        return NULL;
    }

    HSAMPLE sample = (HSAMPLE)calloc(1, sizeof(*sample));
    if (!sample) {
        release_dsp_channel(ch);
        nox_3ds_mutex_unlock(dig->mutex);
        set_last_error("AIL_allocate_sample_handle: out of memory");
        return NULL;
    }

    size_t voice_bytes = (size_t)AUDIO_3DS_SFX_WBUFS * AUDIO_3DS_SFX_WBUF_BYTES;
    if (ch >= AUDIO_3DS_SFX_CH_BASE && ch < AUDIO_3DS_SFX_CH_BASE + AUDIO_3DS_SFX_CH_COUNT && g_sfx_linear_pool) {
        sample->linear_pool = g_sfx_linear_pool + (size_t)(ch - AUDIO_3DS_SFX_CH_BASE) * voice_bytes;
        sample->owns_linear_pool = false;
    } else {
        sample->linear_pool = (uint8_t *)linearAlloc(voice_bytes);
        if (!sample->linear_pool) {
            release_dsp_channel(ch);
            free(sample);
            nox_3ds_mutex_unlock(dig->mutex);
            set_last_error("AIL_allocate_sample_handle: linearAlloc failed");
            return NULL;
        }
        sample->owns_linear_pool = true;
    }

    sample->magic = AUDIO_3DS_MAGIC_SAMPLE;
    sample->dig = dig;
    sample->dsp_ch = ch;
    sample->playback_rate = 22050;
    sample->volume = 127;
    sample->pan = 63;
    sample->ready = 2;
    sample->format_dirty = true;
    sample->rate_dirty = true;
    sample->mix_dirty = true;

    ndspChnReset(ch);
    ndspChnInitParams(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, 22050.0f);
    ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
    apply_channel_mix(ch, 127, 63);
    reset_wbufs(sample->wbufs, AUDIO_3DS_SFX_WBUFS, sample->linear_pool, AUDIO_3DS_SFX_WBUF_BYTES);
    sample->next_wbuf = 0;

    sample->next = dig->sample_head;
    dig->sample_head = sample;

    nox_3ds_mutex_unlock(dig->mutex);
    return sample;
}

DXDEC void AILCALL AIL_release_sample_handle(HSAMPLE S)
{
    if (!valid_sample(S))
        return;

    HDIGDRIVER dig = S->dig;
    nox_3ds_mutex_lock(dig->mutex);

    S->playing = 0;
    S->paused = 0;
    release_dsp_channel(S->dsp_ch);
    S->dsp_ch = -1;

    HSAMPLE *pp = &dig->sample_head;
    while (*pp && *pp != S)
        pp = &(*pp)->next;
    if (*pp == S)
        *pp = S->next;

    nox_3ds_mutex_unlock(dig->mutex);

    if (S->owns_linear_pool && S->linear_pool) {
        linearFree(S->linear_pool);
        S->linear_pool = NULL;
    }

    S->magic = AUDIO_3DS_MAGIC_FREED;
    free(S);
}

DXDEC void AILCALL AIL_init_sample(HSAMPLE S)
{
    if (!valid_sample(S))
        return;

    nox_3ds_mutex_lock(S->dig->mutex);

    S->playing = 0;
    S->paused = 0;
    S->input_exhausted = 0;
    S->current = 0;
    S->ready = 2;
    S->volume = 127;
    S->pan = 63;
    S->buffers[0].buffer = NULL;
    S->buffers[0].length = 0;
    S->buffers[0].position = 0;
    S->buffers[1].buffer = NULL;
    S->buffers[1].length = 0;
    S->buffers[1].position = 0;
    S->mix_dirty = true;
    S->format_dirty = true;
    S->rate_dirty = true;

    if (g_dsp_available && S->dsp_ch >= 0) {
        ndspChnWaveBufClear(S->dsp_ch);
        ndspChnSetPaused(S->dsp_ch, false);
        reset_wbufs(S->wbufs, AUDIO_3DS_SFX_WBUFS, S->linear_pool, AUDIO_3DS_SFX_WBUF_BYTES);
        S->next_wbuf = 0;
        apply_channel_mix(S->dsp_ch, S->volume, S->pan);
        S->mix_dirty = false;
    }

    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC void AILCALL AIL_load_sample_buffer(HSAMPLE S, U32 buff_num, void const FAR *buffer, U32 len)
{
    if (!valid_sample(S) || buff_num >= 2u)
        return;
    if (len > 0 && !buffer)
        return;

    nox_3ds_mutex_lock(S->dig->mutex);
    S->buffers[buff_num].buffer = buffer;
    S->buffers[buff_num].length = len;
    S->buffers[buff_num].position = 0;
    if (len > 0) {
        S->playing = 1;
        S->input_exhausted = 0;
    }
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC S32 AILCALL AIL_sample_buffer_ready(HSAMPLE S)
{
    if (!valid_sample(S))
        return -1;

    if (S->ready == 0)
        return -1;
    if (S->ready-- == 2)
        return 0;
    return !S->current;
}

DXDEC void AILCALL AIL_end_sample(HSAMPLE S)
{
    if (!valid_sample(S))
        return;

    nox_3ds_mutex_lock(S->dig->mutex);
    if (g_dsp_available && S->dsp_ch >= 0) {
        ndspChnWaveBufClear(S->dsp_ch);
        ndspChnSetPaused(S->dsp_ch, false);
        reset_wbufs(S->wbufs, AUDIO_3DS_SFX_WBUFS, S->linear_pool, AUDIO_3DS_SFX_WBUF_BYTES);
        S->next_wbuf = 0;
    }
    S->paused = 0;
    if (S->playing) {
        sample_eob(S);
        sample_eos(S);
    }
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC void AILCALL AIL_stop_sample(HSAMPLE S)
{
    if (!valid_sample(S))
        return;

    nox_3ds_mutex_lock(S->dig->mutex);
    S->paused = 1;
    if (g_dsp_available && S->dsp_ch >= 0)
        ndspChnSetPaused(S->dsp_ch, true);
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC void AILCALL AIL_resume_sample(HSAMPLE S)
{
    if (!valid_sample(S))
        return;

    nox_3ds_mutex_lock(S->dig->mutex);
    S->paused = 0;
    if (g_dsp_available && S->dsp_ch >= 0)
        ndspChnSetPaused(S->dsp_ch, false);
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC AILSAMPLECB AILCALL AIL_register_EOB_callback(HSAMPLE S, AILSAMPLECB EOB)
{
    if (!valid_sample(S))
        return NULL;
    AILSAMPLECB prev = S->eob;
    S->eob = EOB;
    return prev;
}

DXDEC AILSAMPLECB AILCALL AIL_register_EOS_callback(HSAMPLE S, AILSAMPLECB EOS)
{
    if (!valid_sample(S))
        return NULL;
    AILSAMPLECB prev = S->eos;
    S->eos = EOS;
    return prev;
}

DXDEC S32 AILCALL AIL_sample_user_data(HSAMPLE S, U32 index)
{
    if (!valid_sample(S) || index >= 8u)
        return 0;
    return S->user[index];
}

DXDEC void AILCALL AIL_set_sample_user_data(HSAMPLE S, U32 index, S32 value)
{
    if (!valid_sample(S) || index >= 8u)
        return;
    S->user[index] = value;
}

DXDEC void AILCALL AIL_set_sample_type(HSAMPLE S, S32 format, U32 flags)
{
    (void)flags;
    if (!valid_sample(S))
        return;

    nox_3ds_mutex_lock(S->dig->mutex);
    S->bit16  = (BYTE)((format & 1) != 0);
    S->stereo = (BYTE)((format & 2) != 0);
    S->adpcm  = (BYTE)((format & 4) != 0);
    S->format_dirty = true;
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC void AILCALL AIL_set_sample_adpcm_block_size(HSAMPLE S, U32 blocksize)
{
    if (!valid_sample(S))
        return;

    unsigned int min_blk = S->stereo ? 8u : 4u;
    if (blocksize < min_blk) blocksize = min_blk;
    if (blocksize > 2048u)   blocksize = 2048u;
    S->block_size = blocksize;
}

DXDEC void AILCALL AIL_set_sample_volume(HSAMPLE S, S32 volume)
{
    if (!valid_sample(S))
        return;

    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    nox_3ds_mutex_lock(S->dig->mutex);
    S->volume = volume;
    apply_channel_mix(S->dsp_ch, S->volume, S->pan);
    S->mix_dirty = false;
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC void AILCALL AIL_set_sample_pan(HSAMPLE S, S32 pan)
{
    if (!valid_sample(S))
        return;

    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    nox_3ds_mutex_lock(S->dig->mutex);
    S->pan = pan;
    apply_channel_mix(S->dsp_ch, S->volume, S->pan);
    S->mix_dirty = false;
    nox_3ds_mutex_unlock(S->dig->mutex);
}

DXDEC void AILCALL AIL_set_sample_playback_rate(HSAMPLE S, S32 playback_rate)
{
    if (!valid_sample(S))
        return;

    if (playback_rate <= 0)
        playback_rate = 22050;
    if (playback_rate < 4000)
        playback_rate = 4000;
    if (playback_rate > 96000)
        playback_rate = 96000;

    nox_3ds_mutex_lock(S->dig->mutex);
    S->playback_rate = (unsigned int)playback_rate;
    if (g_dsp_available && S->dsp_ch >= 0) {
        ndspChnSetRate(S->dsp_ch, (float)S->playback_rate);
        S->rate_dirty = false;
    } else {
        S->rate_dirty = true;
    }
    nox_3ds_mutex_unlock(S->dig->mutex);
}

/* =========================================================================
 * Stream APIs (Channel 16 = Music ADPCM, Channel 17 = Dialog MP3/PCM)
 * ========================================================================= */
static bool path_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        if (strncasecmp(p, needle, nlen) == 0)
            return true;
    }
    return false;
}

static audio_stream_role_t classify_stream_role(const char *filename, WORD fmt_tag)
{
    if (path_contains_ci(filename, "music\\") || path_contains_ci(filename, "music/"))
        return STREAM_ROLE_MUSIC;
    if (path_contains_ci(filename, "dialog\\") || path_contains_ci(filename, "dialog/"))
        return STREAM_ROLE_DIALOG;
    if (fmt_tag == 0x11)
        return STREAM_ROLE_MUSIC;
    if (fmt_tag == 0x55 || fmt_tag == 0x01)
        return STREAM_ROLE_DIALOG;
    return STREAM_ROLE_GENERIC;
}

DXDEC HSTREAM AILCALL AIL_open_stream(HDIGDRIVER dig, char const FAR *filename, S32 stream_mem)
{
    (void)stream_mem;

    if (!valid_dig(dig) || !g_dsp_available || !filename || !filename[0]) {
        set_last_error("AIL_open_stream: invalid arguments or DSP inactive");
        return NULL;
    }

    FILE *f = fopen(filename, "rb"); /* routed through compat_fopen() */
    if (!f) {
        set_last_error("AIL_open_stream: failed to open file");
        return NULL;
    }

    /* Batch SDMC reads into 32 KB chunks to eliminate micro-stutters while streaming. */
    setvbuf(f, NULL, _IOFBF, 32 * 1024);

    BYTE tmp[256];
    HSTREAM stream = NULL;
    int ch = -1;

    if (fread(tmp, 12, 1, f) != 1)
        goto error;
    if (memcmp(tmp, "RIFF", 4) != 0 || memcmp(tmp + 8, "WAVE", 4) != 0)
        goto error;

    unsigned int file_size = nox_read_u32(tmp + 4) + 8u;

    /* Locate the 'fmt ' chunk (skipping any optional JUNK/LIST chunks before it) */
    unsigned int fmt_size = 0;
    for (;;) {
        if (fread(tmp, 8, 1, f) != 1)
            goto error;
        unsigned int csize = nox_read_u32(tmp + 4);
        if (memcmp(tmp, "fmt ", 4) == 0) {
            fmt_size = csize;
            break;
        }
        unsigned int skip = csize + (csize & 1u);
        if (fseek(f, (long)skip, SEEK_CUR) != 0)
            goto error;
    }

    if (fmt_size < 14u || fmt_size > sizeof(tmp))
        goto error;
    if (fread(tmp, fmt_size, 1, f) != 1)
        goto error;

    WAVEFORMAT wf;
    wf.wFormatTag      = nox_read_u16(tmp + 0);
    wf.nChannels       = nox_read_u16(tmp + 2);
    wf.nSamplesPerSec  = nox_read_u32(tmp + 4);
    wf.nAvgBytesPerSec = nox_read_u32(tmp + 8);
    wf.nBlockAlign     = nox_read_u16(tmp + 12);

    stream = (HSTREAM)calloc(1, sizeof(*stream));
    if (!stream)
        goto error;

    stream->dig = dig;
    stream->file = f;
    stream->file_size = file_size;
    stream->volume = 127;
    stream->dsp_ch = -1;
    stream->role = classify_stream_role(filename, wf.wFormatTag);

    if (wf.wFormatTag == 0x01) {
        /* Uncompressed PCM16 WAV (typically pre-converted dialog\*.wav) */
        stream->playback_rate = (wf.nSamplesPerSec > 0) ? wf.nSamplesPerSec : 22050u;
        stream->stereo = (wf.nChannels > 1) ? 1 : 0;
        stream->pcm.wf = wf;
        stream->decode = stream_pcm_decode;
        stream->seek = stream_pcm_seek;
        stream->tell = stream_pcm_tell;
        stream->loop_seamless = false;
    } else if (wf.wFormatTag == 0x11) {
        /* IMA-ADPCM WAV (music\*.wav) */
        stream->playback_rate = (wf.nSamplesPerSec > 0) ? wf.nSamplesPerSec : 22050u;
        stream->stereo = (wf.nChannels > 1) ? 1 : 0;
        stream->adpcm.wf = wf;
        stream->decode = stream_adpcm_decode;
        stream->seek = stream_adpcm_seek;
        stream->tell = stream_adpcm_tell;
        stream->loop_seamless = (stream->role == STREAM_ROLE_MUSIC);

        unsigned int channels = stream->stereo ? 2u : 1u;
        unsigned int spb = (fmt_size >= 20u) ? (unsigned int)nox_read_u16(tmp + 18) : 0u;
        if (spb == 0 && wf.nBlockAlign >= channels * 4u)
            spb = (wf.nBlockAlign / channels - 4u) * 2u + 1u;
        stream->adpcm.samples_per_block = spb;

        /* Read optional 'fact' chunk for exact total sample count */
        long after_fmt = ftell(f);
        if (fread(tmp, 8, 1, f) == 1 && memcmp(tmp, "fact", 4) == 0) {
            unsigned int fsize = nox_read_u32(tmp + 4);
            if (fsize >= 4u && fread(tmp, 4, 1, f) == 1) {
                stream->adpcm.samples = nox_read_u32(tmp);
                if (fsize > 4u)
                    fseek(f, (long)(fsize - 4u), SEEK_CUR);
            }
        } else if (after_fmt >= 0) {
            fseek(f, after_fmt, SEEK_SET);
        }
    } else if (wf.wFormatTag == 0x55) {
        /* MP3-in-WAV (dialog\*.wav) decoded via bundled minimp3.h */
        if (fmt_size < 30u)
            goto error;

        unsigned int channels = nox_read_u16(tmp + 2);
        unsigned int rate     = nox_read_u32(tmp + 4);
        unsigned int cbSize   = nox_read_u16(tmp + 16);
        if (cbSize != 12u)
            goto error;

        stream->playback_rate = (rate > 0) ? rate : 22050u;
        stream->stereo = (channels > 1u) ? 1 : 0;
        mp3dec_init(&stream->mp3.dec);
        stream->decode = stream_mp3_decode;
        stream->seek = stream_mp3_seek;
        stream->tell = stream_mp3_tell;
        stream->loop_seamless = false;
    } else {
        fprintf(stderr, "[audio3ds] Unsupported WAV format 0x%04X in %s\n",
                (unsigned)wf.wFormatTag, filename);
        goto error;
    }

    nox_3ds_mutex_lock(dig->mutex);

    ch = alloc_stream_channel(stream->role);
    if (ch < 0) {
        nox_3ds_mutex_unlock(dig->mutex);
        set_last_error("AIL_open_stream: no free ndsp stream channel");
        goto error;
    }

    size_t st_bytes = (size_t)AUDIO_3DS_STREAM_WBUFS * AUDIO_3DS_STREAM_WBUF_BYTES;
    if (ch == AUDIO_3DS_MUSIC_CH && g_stream_linear_pool) {
        stream->linear_pool = g_stream_linear_pool;
        stream->owns_linear_pool = false;
    } else if (ch == AUDIO_3DS_DIALOG_CH && g_stream_linear_pool) {
        stream->linear_pool = g_stream_linear_pool + st_bytes;
        stream->owns_linear_pool = false;
    } else {
        stream->linear_pool = (uint8_t *)linearAlloc(st_bytes);
        if (!stream->linear_pool) {
            release_dsp_channel(ch);
            nox_3ds_mutex_unlock(dig->mutex);
            set_last_error("AIL_open_stream: linearAlloc failed");
            goto error;
        }
        stream->owns_linear_pool = true;
    }

    stream->dsp_ch = ch;
    stream->magic = AUDIO_3DS_MAGIC_STREAM;

    ndspChnReset(ch);
    ndspChnInitParams(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(ch, (float)stream->playback_rate);
    ndspChnSetFormat(ch, stream->stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    apply_channel_mix(ch, stream->volume, 63);
    reset_wbufs(stream->wbufs, AUDIO_3DS_STREAM_WBUFS, stream->linear_pool, AUDIO_3DS_STREAM_WBUF_BYTES);
    stream->next_wbuf = 0;

    stream->next = dig->stream_head;
    dig->stream_head = stream;

    nox_3ds_mutex_unlock(dig->mutex);
    return stream;

error:
    if (f)
        fclose(f);
    if (stream) {
        free(stream->adpcm_seek_pcm);
        free(stream);
    }
    return NULL;
}

DXDEC void AILCALL AIL_close_stream(HSTREAM stream)
{
    if (!valid_stream(stream))
        return;

    HDIGDRIVER dig = stream->dig;
    nox_3ds_mutex_lock(dig->mutex);

    stream->playing = 0;
    stream->paused = 0;
    release_dsp_channel(stream->dsp_ch);
    stream->dsp_ch = -1;

    HSTREAM *pp = &dig->stream_head;
    while (*pp && *pp != stream)
        pp = &(*pp)->next;
    if (*pp == stream)
        *pp = stream->next;

    nox_3ds_mutex_unlock(dig->mutex);

    if (stream->owns_linear_pool && stream->linear_pool) {
        linearFree(stream->linear_pool);
        stream->linear_pool = NULL;
    }
    if (stream->file) {
        fclose(stream->file);
        stream->file = NULL;
    }
    free(stream->adpcm_seek_pcm);
    stream->adpcm_seek_pcm = NULL;
    stream->magic = AUDIO_3DS_MAGIC_FREED;
    free(stream);
}

DXDEC void AILCALL AIL_start_stream(HSTREAM stream)
{
    if (!valid_stream(stream))
        return;

    nox_3ds_mutex_lock(stream->dig->mutex);
    stream->playing = 1;
    stream->paused = 0;
    stream->eof = 0;
    if (g_dsp_available && stream->dsp_ch >= 0)
        ndspChnSetPaused(stream->dsp_ch, false);
    /* Prime initial wave buffers immediately for zero startup latency. */
    stream_work(stream);
    nox_3ds_mutex_unlock(stream->dig->mutex);
}

DXDEC void AILCALL AIL_pause_stream(HSTREAM stream, S32 onoff)
{
    if (!valid_stream(stream))
        return;

    nox_3ds_mutex_lock(stream->dig->mutex);
    if (onoff) {
        stream->paused = 1;
        if (g_dsp_available && stream->dsp_ch >= 0)
            ndspChnSetPaused(stream->dsp_ch, true);
    } else {
        stream->paused = 0;
        if (g_dsp_available && stream->dsp_ch >= 0)
            ndspChnSetPaused(stream->dsp_ch, false);
        if (stream->playing)
            stream_work(stream);
    }
    nox_3ds_mutex_unlock(stream->dig->mutex);
}

DXDEC void AILCALL AIL_set_stream_position(HSTREAM stream, S32 offset)
{
    if (!valid_stream(stream) || !stream->file || !stream->seek)
        return;
    if (offset < 0)
        offset = 0;

    nox_3ds_mutex_lock(stream->dig->mutex);

    if (g_dsp_available && stream->dsp_ch >= 0) {
        ndspChnWaveBufClear(stream->dsp_ch);
        reset_wbufs(stream->wbufs, AUDIO_3DS_STREAM_WBUFS, stream->linear_pool, AUDIO_3DS_STREAM_WBUF_BYTES);
        stream->next_wbuf = 0;
    }

    stream->chunk_size = 0;
    stream->chunk_pos = 0;
    stream->buffered = 0;
    stream->eof = 0;
    stream->loop_declick_pending = false;
    stream->have_last_frame = false;

    if (fseek(stream->file, 12, SEEK_SET) == 0)
        stream->seek(stream, (unsigned int)offset);

    nox_3ds_mutex_unlock(stream->dig->mutex);
}

DXDEC void AILCALL AIL_set_stream_volume(HSTREAM stream, S32 volume)
{
    if (!valid_stream(stream))
        return;

    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;

    nox_3ds_mutex_lock(stream->dig->mutex);
    stream->volume = volume;
    apply_channel_mix(stream->dsp_ch, stream->volume, 63);
    nox_3ds_mutex_unlock(stream->dig->mutex);
}

DXDEC S32 AILCALL AIL_stream_position(HSTREAM stream)
{
    if (!valid_stream(stream) || !stream->tell)
        return 0;
    return (S32)stream->tell(stream);
}

DXDEC S32 AILCALL AIL_stream_status(HSTREAM stream)
{
    if (!valid_stream(stream))
        return AIL_SMP_DONE;

    /* Pump synchronous timers/streams here as well so tight fade-wait loops
     * (e.g. sub_43DCC0 / draw.c:4479) advance volume faders and never hang. */
    if (!g_in_serve)
        AIL_serve();

    if (!valid_stream(stream))
        return AIL_SMP_DONE;
    if (stream->paused)
        return AIL_SMP_STOPPED;
    return stream->playing ? AIL_SMP_PLAYING : AIL_SMP_DONE;
}

/* =========================================================================
 * Synchronous Miles Timer APIs (OpenNox PR #376)
 * ========================================================================= */
DXDEC HTIMER AILCALL AIL_register_timer(AILTIMERCB fn)
{
    if (!fn)
        return (HTIMER)(intptr_t)-1;

    for (int i = 0; i < AUDIO_3DS_MAX_TIMERS; ++i) {
        if (!g_timers[i].in_use) {
            memset(&g_timers[i], 0, sizeof(g_timers[i]));
            g_timers[i].magic = AUDIO_3DS_MAGIC_TIMER;
            g_timers[i].in_use = true;
            g_timers[i].running = false;
            g_timers[i].interval_ms = 33; /* default ~30 Hz */
            g_timers[i].cb = fn;
            g_timers[i].user = 0;
            return &g_timers[i];
        }
    }

    set_last_error("AIL_register_timer: timer table full");
    return (HTIMER)(intptr_t)-1;
}

DXDEC void AILCALL AIL_set_timer_frequency(HTIMER timer, U32 hertz)
{
    if (!valid_timer(timer))
        return;
    unsigned int ms = (hertz > 0u) ? (1000u / hertz) : 33u;
    if (ms == 0u)
        ms = 1u;
    timer->interval_ms = ms;
}

DXDEC void AILCALL AIL_start_timer(HTIMER timer)
{
    if (!valid_timer(timer))
        return;
    timer->running = true;
    timer->next_fire_ms = nox_3ds_get_ticks_ms() + timer->interval_ms;
}

DXDEC void AILCALL AIL_stop_timer(HTIMER timer)
{
    if (!valid_timer(timer))
        return;
    timer->running = false;
}

DXDEC void AILCALL AIL_release_timer_handle(HTIMER timer)
{
    if (!valid_timer(timer))
        return;
    timer->running = false;
    timer->in_use = false;
    timer->cb = NULL;
    timer->magic = AUDIO_3DS_MAGIC_FREED;
}
