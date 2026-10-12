/*
 * usds — Unnatural Selection for dual-screen handhelds (RG DS Plus / ROCKNIX)
 *
 * A small libretro frontend around DOSBox Pure that:
 *   - shows the whole game on the top screen,
 *   - shows fullscreen / top-half / bottom-half (pannable) views on the bottom
 *     touch screen, sliced at the top edge of the DigiLife floppy drive,
 *   - turns touches and pad buttons into DOS mouse clicks and keys.
 *
 * Deliberately depends only on SDL2 (plus a handful of POSIX calls for
 * reading the touch panel directly), so it can be cross-built for aarch64
 * without a full sysroot.
 */
#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "libretro.h"
#include "font.h"
#include "sigs.h"

/* ---- the few libc calls we use directly (touch panel via evdev) ---------- */
#ifdef __linux__
int open(const char *path, int flags, ...);
long read(int fd, void *buf, unsigned long n);
int close(int fd);
int ioctl(int fd, unsigned long req, ...);
long readlink(const char *path, char *buf, unsigned long n);
#define O_RDONLY_ 0
#define O_NONBLOCK_ 04000
#define IOC_(dir, type, nr, size) (((unsigned long)(dir) << 30) | ((unsigned long)(size) << 16) | ((type) << 8) | (nr))
#define EVIOCGNAME_(len) IOC_(2, 'E', 0x06, len)
#define EVIOCGPROP_(len) IOC_(2, 'E', 0x09, len)
#define EVIOCGBIT_(ev, len) IOC_(2, 'E', 0x20 + (ev), len)
#define EVIOCGABS_(abs) IOC_(2, 'E', 0x40 + (abs), 24)
#endif

#define VERSION "1.0"

/* ========================================================================== */
/* small helpers                                                              */
/* ========================================================================== */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static char g_basedir[1024] = ".";
static char *path_join(char *out, size_t n, const char *a, const char *b)
{
    if (b[0] == '/' || !a || !a[0]) SDL_snprintf(out, n, "%s", b);
    else SDL_snprintf(out, n, "%s/%s", a, b);
    return out;
}

static char *slurp(const char *path, size_t *len)
{
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (!rw) return NULL;
    Sint64 sz = SDL_RWsize(rw);
    if (sz < 0) sz = 0;
    char *buf = (char *)SDL_malloc((size_t)sz + 1);
    size_t got = SDL_RWread(rw, buf, 1, (size_t)sz);
    buf[got] = 0;
    SDL_RWclose(rw);
    if (len) *len = got;
    return buf;
}

static int file_exists(const char *path)
{
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (!rw) return 0;
    SDL_RWclose(rw);
    return 1;
}

/* ========================================================================== */
/* config                                                                     */
/* ========================================================================== */

#define MAX_CFG 160
static struct { char key[64]; char val[256]; } g_cfg[MAX_CFG];
static int g_ncfg;

static void cfg_set(const char *k, const char *v)
{
    for (int i = 0; i < g_ncfg; i++)
        if (!SDL_strcmp(g_cfg[i].key, k)) { SDL_strlcpy(g_cfg[i].val, v, sizeof g_cfg[i].val); return; }
    if (g_ncfg >= MAX_CFG) return;
    SDL_strlcpy(g_cfg[g_ncfg].key, k, sizeof g_cfg[0].key);
    SDL_strlcpy(g_cfg[g_ncfg].val, v, sizeof g_cfg[0].val);
    g_ncfg++;
}

static const char *cfg_get(const char *k, const char *def)
{
    for (int i = 0; i < g_ncfg; i++)
        if (!SDL_strcmp(g_cfg[i].key, k)) return g_cfg[i].val;
    return def;
}
static int cfg_int(const char *k, int def) { const char *v = cfg_get(k, NULL); return v ? SDL_atoi(v) : def; }
static float cfg_float(const char *k, float def) { const char *v = cfg_get(k, NULL); return v ? (float)SDL_atof(v) : def; }

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + SDL_strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void cfg_load(const char *path)
{
    char *buf = slurp(path, NULL);
    if (!buf) return;
    char *line = buf;
    while (line && *line) {
        char *nl = SDL_strchr(line, '\n');
        if (nl) *nl = 0;
        char *l = trim(line);
        if (*l && *l != '#' && *l != ';') {
            char *eq = SDL_strchr(l, '=');
            if (eq) { *eq = 0; cfg_set(trim(l), trim(eq + 1)); }
        }
        line = nl ? nl + 1 : NULL;
    }
    SDL_free(buf);
    SDL_Log("config: loaded %s", path);
}

/* ========================================================================== */
/* libretro core                                                              */
/* ========================================================================== */

static struct {
    void *lib;
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*run)(void);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    struct retro_system_av_info av;
    enum retro_pixel_format fmt;
    retro_keyboard_event_t kb_cb;
    int shutdown;
} core;

static char g_sysdir[1024], g_savedir[1024];

/* DOS program currently running, from DOSBox Pure's status log */
static char g_prog[32];
static int g_game_started;
static volatile int g_quit_request;

static void note_program(const char *msg)
{
    const char *p = SDL_strstr(msg, "Program: ");
    if (!p) return;
    p += 9;
    size_t n = 0;
    while (p[n] && p[n] != ' ' && n < sizeof g_prog - 1) n++;
    SDL_memcpy(g_prog, p, n);
    g_prog[n] = 0;
    if (!SDL_strncasecmp(g_prog, "UNS", 3)) g_game_started = 1;
    /* The game has gone back to DOS: close the whole port instead of
     * leaving the player at DOSBox's own menu. */
    else if (g_game_started && cfg_int("exit_with_game", 1)) g_quit_request = 1;
}

/* core variables (DOSBox Pure options) */
static const char *core_var_get(const char *key)
{
    return cfg_get(key, NULL);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = SDL_strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    note_program(buf);
    if (level >= RETRO_LOG_INFO || cfg_int("debug", 0)) SDL_Log("core: %s", buf);
}

static bool core_environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_sysdir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_savedir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f == RETRO_PIXEL_FORMAT_XRGB8888 || f == RETRO_PIXEL_FORMAT_RGB565) { core.fmt = f; return true; }
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = (struct retro_variable *)data;
        v->value = core_var_get(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        core.kb_cb = ((struct retro_keyboard_callback *)data)->callback;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        core.av = *(const struct retro_system_av_info *)data;
        SDL_Log("core: av info %ux%u @ %.3f fps, %.0f Hz audio", core.av.geometry.base_width,
                core.av.geometry.base_height, core.av.timing.fps, core.av.timing.sample_rate);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        core.av.geometry = *(const struct retro_game_geometry *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        core.shutdown = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        const struct retro_message_ext *m = (const struct retro_message_ext *)data;
        SDL_Log("core message: %s", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        SDL_Log("core message: %s", ((const struct retro_message *)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        return true;
    default:
        return false;
    }
}

/* ---- video -------------------------------------------------------------- */
static Uint32 *g_frame;        /* XRGB8888, g_fw*g_fh */
static Uint32 *g_disp;         /* what is shown: g_frame, minus the parked cursor */
static int g_disp_serial = -1;
static int g_fw, g_fh, g_frame_serial;

static void core_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || w == 0 || h == 0) return; /* dupe */
    if ((int)w != g_fw || (int)h != g_fh) {
        SDL_free(g_frame);
        g_frame = (Uint32 *)SDL_malloc(sizeof(Uint32) * w * h);
        g_fw = (int)w; g_fh = (int)h;
        SDL_Log("video: frame size %dx%d", g_fw, g_fh);
    }
    const Uint8 *src = (const Uint8 *)data;
    for (unsigned y = 0; y < h; y++) {
        Uint32 *dst = g_frame + y * w;
        if (core.fmt == RETRO_PIXEL_FORMAT_XRGB8888) {
            SDL_memcpy(dst, src + y * pitch, w * 4);
        } else {
            const Uint16 *s = (const Uint16 *)(src + y * pitch);
            for (unsigned x = 0; x < w; x++) {
                Uint16 p = s[x];
                Uint32 r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                dst[x] = ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
            }
        }
    }
    g_frame_serial++;
}

/* ---- audio -------------------------------------------------------------- */
static SDL_AudioDeviceID g_audio;
static int g_audio_rate;

static size_t core_audio_batch(const int16_t *data, size_t frames)
{
    if (!g_audio) return frames;
    Uint32 queued = SDL_GetQueuedAudioSize(g_audio);
    Uint32 maxq = (Uint32)(g_audio_rate * 4 * cfg_int("audio_max_ms", 120) / 1000);
    if (queued < maxq) SDL_QueueAudio(g_audio, data, (Uint32)(frames * 4));
    return frames;
}

static void core_audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    core_audio_batch(s, 1);
}

/* ========================================================================== */
/* emulated mouse / keyboard state seen by the core                            */
/* ========================================================================== */

static float g_ptr_x = 160, g_ptr_y = 100; /* frame coordinates */
static int g_btn[2];                       /* left, right */
static Uint8 g_keys[RETROK_LAST];
static int g_frame_no;

static int16_t core_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0) return 0;
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_POINTER: {
        if (!g_fw || !g_fh) return 0;
        int v;
        if (id == RETRO_DEVICE_ID_POINTER_X) {
            v = (int)((g_ptr_x + 0.5f) / g_fw * 65534.0f) - 32767;
            if (v == 0) v = 1;
            return (int16_t)clampi(v, -32767, 32767);
        }
        if (id == RETRO_DEVICE_ID_POINTER_Y) {
            v = (int)((g_ptr_y + 0.5f) / g_fh * 65534.0f) - 32767;
            if (v == 0) v = 1;
            return (int16_t)clampi(v, -32767, 32767);
        }
        return 0;
    }
    case RETRO_DEVICE_MOUSE:
        if (id == RETRO_DEVICE_ID_MOUSE_LEFT) return (int16_t)g_btn[0];
        if (id == RETRO_DEVICE_ID_MOUSE_RIGHT) return (int16_t)g_btn[1];
        return 0;
    case RETRO_DEVICE_KEYBOARD:
        return id < RETROK_LAST ? g_keys[id] : 0;
    default:
        return 0;
    }
}

static void core_input_poll(void) {}

/* --- queued mouse/key events, applied one state change per emulated frame -- */
enum { EV_MOVE, EV_DOWN, EV_UP, EV_WAIT, EV_KDOWN, EV_KUP, EV_RESTORE };
typedef struct { int type; float x, y; int a; } QEvent;
#define QMAX 256
static QEvent g_q[QMAX];
static int g_qh, g_qt;
static int g_wait_until, g_btn_down_frame[2];
static float g_saved_x, g_saved_y;
static int g_parked; /* pointer is parked in the corner (cursor gets hidden) */

static void q_push(int type, float x, float y, int a)
{
    int n = (g_qt + 1) % QMAX;
    if (n == g_qh) return;
    g_q[g_qt].type = type; g_q[g_qt].x = x; g_q[g_qt].y = y; g_q[g_qt].a = a;
    g_qt = n;
}
static int q_empty(void) { return g_qh == g_qt; }

static void send_key(int key, int down)
{
    if (key <= 0 || key >= RETROK_LAST) return;
    if (g_keys[key] == (down ? 1 : 0)) return;
    g_keys[key] = down ? 1 : 0;
    Uint16 mods = 0;
    if (g_keys[RETROK_LSHIFT] || g_keys[RETROK_RSHIFT]) mods |= RETROKMOD_SHIFT;
    if (g_keys[RETROK_LCTRL] || g_keys[RETROK_RCTRL]) mods |= RETROKMOD_CTRL;
    if (g_keys[RETROK_LALT] || g_keys[RETROK_RALT]) mods |= RETROKMOD_ALT;
    uint32_t ch = 0;
    if (key >= 32 && key < 127) {
        ch = (uint32_t)key;
        if ((mods & RETROKMOD_SHIFT) && ch >= 'a' && ch <= 'z') ch -= 32;
    } else if (key == RETROK_RETURN) ch = '\r';
    else if (key == RETROK_BACKSPACE) ch = 8;
    else if (key == RETROK_ESCAPE) ch = 27;
    if (core.kb_cb) core.kb_cb(down, (unsigned)key, down ? ch : 0, mods);
}

static void q_process(void)
{
    int minhold = cfg_int("min_click_frames", 4);
    while (!q_empty()) {
        if (g_frame_no < g_wait_until) return;
        QEvent *e = &g_q[g_qh];
        switch (e->type) {
        case EV_MOVE:
            g_ptr_x = e->x; g_ptr_y = e->y;
            g_parked = e->a == 1;
            break;
        case EV_RESTORE:
            g_ptr_x = g_saved_x; g_ptr_y = g_saved_y;
            break;
        case EV_WAIT:
            g_wait_until = g_frame_no + e->a;
            g_qh = (g_qh + 1) % QMAX;
            return;
        case EV_DOWN:
            g_btn[e->a] = 1;
            g_btn_down_frame[e->a] = g_frame_no;
            g_qh = (g_qh + 1) % QMAX;
            return; /* one button change per frame */
        case EV_UP:
            if (g_frame_no < g_btn_down_frame[e->a] + minhold) return; /* keep it down long enough */
            g_btn[e->a] = 0;
            g_qh = (g_qh + 1) % QMAX;
            return;
        case EV_KDOWN:
            send_key(e->a, 1);
            g_btn_down_frame[0] = g_frame_no; /* reuse as key timer */
            g_qh = (g_qh + 1) % QMAX;
            return;
        case EV_KUP:
            if (g_frame_no < g_btn_down_frame[0] + minhold) return;
            send_key(e->a, 0);
            g_qh = (g_qh + 1) % QMAX;
            return;
        }
        g_qh = (g_qh + 1) % QMAX;
    }
}

static int g_inject_park; /* an injected click wants the pointer parked afterwards */

/* scripted click somewhere on the game screen. The pointer is left there by
 * default: the game can take a while to act on a click (e.g. at Zoom 2), and
 * moving the pointer away too soon makes the click land somewhere else. */
static void inject_click(float fx, float fy, int button)
{
    float lastx = g_ptr_x, lasty = g_ptr_y;
    for (int i = g_qh; i != g_qt; i = (i + 1) % QMAX)
        if (g_q[i].type == EV_MOVE) { lastx = g_q[i].x; lasty = g_q[i].y; }
    g_saved_x = lastx; g_saved_y = lasty;
    int hold = cfg_int("inject_hold_frames", 8);
    q_push(EV_MOVE, fx, fy, 0);
    q_push(EV_WAIT, 0, 0, cfg_int("press_delay_frames", 10));
    q_push(EV_DOWN, 0, 0, button);
    q_push(EV_WAIT, 0, 0, hold);
    q_push(EV_UP, 0, 0, button);
    if (cfg_int("restore_pointer", 0)) {
        q_push(EV_WAIT, 0, 0, cfg_int("restore_delay_frames", 30));
        q_push(EV_RESTORE, 0, 0, 0);
    }
    g_inject_park = 1;
}

static void tap_key(int key)
{
    q_push(EV_KDOWN, 0, 0, key);
    q_push(EV_KUP, 0, 0, key);
}

/* ========================================================================== */
/* views                                                                      */
/* ========================================================================== */

enum { VIEW_FULL, VIEW_TOP, VIEW_BOTTOM };
static int g_view = VIEW_FULL;
static float g_pan[3];          /* per view, frame x of the left edge */
static float g_pan_target[3];
static Uint32 g_view_changed_at; /* shows the pan bar briefly */

/* game screen "units": the game is 320x200; scale to whatever the core gives */
static float ux(float x) { return x * (g_fw ? g_fw : 320) / 320.0f; }
static float uy(float y) { return y * (g_fh ? g_fh : 200) / 200.0f; }

static int slice_row(void)
{
    /* top edge of the floppy-drive housing: row 120 of 200 */
    return (int)(uy((float)cfg_int("slice_row", 120)) + 0.5f);
}

/* pixel aspect (width/height) of one frame pixel when the frame fills 4:3 */
static float pixel_aspect(void)
{
    if (!g_fw || !g_fh) return 0.8333f;
    return (4.0f / 3.0f) * (float)g_fh / (float)g_fw;
}

typedef struct { float x, y, w, h; } FRect;

/* For a viewport of size vw x vh and a view, compute the frame region shown
 * (src, in frame pixels) and where it goes (dst, in viewport pixels). */
static void view_geometry(int view, int vw, int vh, FRect *src, FRect *dst)
{
    float fw = (float)(g_fw ? g_fw : 320), fh = (float)(g_fh ? g_fh : 200);
    float pa = pixel_aspect();
    if (view == VIEW_FULL) {
        /* frame displayed at 4:3, fit inside the viewport */
        float dispw = fw * pa, disph = fh;
        float s = SDL_min(vw / dispw, vh / disph);
        dst->w = dispw * s; dst->h = disph * s;
        dst->x = (vw - dst->w) / 2; dst->y = (vh - dst->h) / 2;
        src->x = 0; src->y = 0; src->w = fw; src->h = fh;
        return;
    }
    int sr = slice_row();
    float r0 = view == VIEW_TOP ? 0.0f : (float)sr;
    float r1 = view == VIEW_TOP ? (float)sr : fh;
    float rows = r1 - r0;
    float sy = vh / rows;          /* screen px per frame row */
    float sx = sy * pa;            /* screen px per frame column */
    float visw = vw / sx;
    src->y = r0; src->h = rows;
    dst->y = 0; dst->h = (float)vh;
    if (visw >= fw) {
        src->x = 0; src->w = fw;
        dst->w = fw * sx; dst->x = (vw - dst->w) / 2;
    } else {
        float maxpan = fw - visw;
        g_pan[view] = clampf(g_pan[view], 0, maxpan);
        src->x = g_pan[view]; src->w = visw;
        dst->x = 0; dst->w = (float)vw;
    }
}

static float max_pan(int view)
{
    if (view == VIEW_FULL) return 0;
    FRect s, d;
    float save = g_pan[view];
    g_pan[view] = 0;
    view_geometry(view, 1024, 768, &s, &d);
    g_pan[view] = save;
    float fw = (float)(g_fw ? g_fw : 320);
    return SDL_max(0.0f, fw - s.w);
}

/* What the game is showing; decides how the two screens are laid out. */
enum { MODE_OTHER, MODE_LAB, MODE_BATTLE, MODE_DESK, MODE_CONSOLE };
static int g_mode = MODE_OTHER;
static int is_play_mode(void) { return g_mode == MODE_LAB || g_mode == MODE_BATTLE; }

static Uint32 frame_px(float gx, float gy)
{
    int x = clampi((int)ux(gx), 0, g_fw - 1), y = clampi((int)uy(gy), 0, g_fh - 1);
    return g_frame[y * g_fw + x];
}

/* fit a region (game units) into the viewport, keeping the pixel shape */
static void fit_region(float gx, float gy, float gw, float gh, int vw, int vh, FRect *src, FRect *dst)
{
    float pa = pixel_aspect();
    src->x = ux(gx); src->y = uy(gy); src->w = ux(gw); src->h = uy(gh);
    float dw = src->w * pa, dh = src->h;
    float sc = SDL_min(vw / dw, vh / dh);
    dst->w = dw * sc; dst->h = dh * sc;
    dst->x = (vw - dst->w) / 2; dst->y = (vh - dst->h) / 2;
}

/* Briefing / credits console: picture monitor with its bezel on top, text
 * monitor with its bezel on the bottom (game units, 320x200). */
static const float CONSOLE_TOP[4] = {78, 6, 166, 104};
static const float CONSOLE_BOTTOM[4] = {78, 112, 166, 88};

/* geometry for one screen (role 0 = top, 1 = bottom) in the current mode */
static void screen_geometry(int role, int vw, int vh, FRect *src, FRect *dst)
{
    if (g_mode == MODE_CONSOLE) {
        const float *r = role ? CONSOLE_BOTTOM : CONSOLE_TOP;
        fit_region(r[0], r[1], r[2], r[3], vw, vh, src, dst);
        return;
    }
    view_geometry(role && is_play_mode() ? g_view : VIEW_FULL, vw, vh, src, dst);
}

/* ========================================================================== */
/* screens / rendering                                                        */
/* ========================================================================== */

typedef struct {
    SDL_Renderer *ren;
    SDL_Texture *frame_tex, *sharp_tex, *font_tex, *title_tex;
    int tex_w, tex_h, sharp_k;
    int uploaded_serial, title_serial;
} RCtx;

/* frozen title card for the top screen while the desk is showing */
static Uint32 *g_title;
static int g_title_w, g_title_h, g_title_serial;

typedef struct {
    SDL_Window *win;
    RCtx *rc;
    SDL_Rect vp;  /* area of the window used by this screen */
    int role;     /* 0 = top (display only), 1 = bottom (touch) */
} Screen;

static RCtx g_rc[2];
static Screen g_scr[2];
static int g_single_window;
static int g_swap;

/* font atlas: 96 glyphs, 8x8 cells */
static int glyph_index(char c)
{
    if (c >= 'a' && c <= 'z' && c != 'x') c = (char)(c - 32);
    for (int i = 0; FONT[i].rows; i++) if (FONT[i].ch == c) return i;
    return -1;
}
static int glyph_width(int gi) { const char *r = FONT[gi].rows; int w = 0; while (r[w] && r[w] != '|') w++; return w; }

static SDL_Texture *make_font_tex(SDL_Renderer *ren)
{
    int n = 0;
    while (FONT[n].rows) n++;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, n * 8, 8, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_FillRect(s, NULL, 0);
    for (int i = 0; i < n; i++) {
        const char *p = FONT[i].rows;
        int x = 0, y = 0;
        for (; *p; p++) {
            if (*p == '|') { y++; x = 0; continue; }
            if (*p == '#') ((Uint32 *)s->pixels)[y * (s->pitch / 4) + i * 8 + x] = 0xFFFFFFFF;
            x++;
        }
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
    SDL_FreeSurface(s);
    return t;
}

static int text_width(const char *s, int k)
{
    int w = 0;
    for (; *s; s++) { int gi = glyph_index(*s); w += ((gi >= 0 ? glyph_width(gi) : 3) + 1) * k; }
    return w ? w - k : 0;
}

static void draw_text(RCtx *rc, int x, int y, int k, const char *s, SDL_Color c)
{
    SDL_SetTextureColorMod(rc->font_tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(rc->font_tex, c.a);
    for (; *s; s++) {
        int gi = glyph_index(*s);
        if (gi < 0) { x += 4 * k; continue; }
        int gw = glyph_width(gi);
        SDL_Rect src = {gi * 8, 0, gw, 7}, dst = {x, y, gw * k, 7 * k};
        SDL_RenderCopy(rc->ren, rc->font_tex, &src, &dst);
        x += (gw + 1) * k;
    }
}

static void fill(SDL_Renderer *r, int x, int y, int w, int h, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 a)
{
    SDL_SetRenderDrawColor(r, cr, cg, cb, a);
    SDL_Rect rc = {x, y, w, h};
    SDL_RenderFillRect(r, &rc);
}

/* a button in the game's style: lavender bevel, dark pixel text, optional light */
static void draw_game_button(RCtx *rc, int x, int y, int w, int h, int k, const char *label, int on, int light, Uint8 alpha)
{
    SDL_Renderer *r = rc->ren;
    fill(r, x, y, w, h, 23, 22, 21, alpha);
    if (on) fill(r, x + k, y + k, w - 2 * k, h - 2 * k, 126, 124, 160, alpha);
    else fill(r, x + k, y + k, w - 2 * k, h - 2 * k, 156, 154, 196, alpha);
    Uint8 hi[3] = {198, 214, 238}, sh[3] = {117, 120, 147};
    if (on) { Uint8 t; for (int i = 0; i < 3; i++) { t = hi[i]; hi[i] = sh[i]; sh[i] = t; } }
    fill(r, x + k, y + k, w - 2 * k, k, hi[0], hi[1], hi[2], alpha);
    fill(r, x + k, y + k, k, h - 2 * k, hi[0], hi[1], hi[2], alpha);
    fill(r, x + k, y + h - 2 * k, w - 2 * k, k, sh[0], sh[1], sh[2], alpha);
    fill(r, x + w - 2 * k, y + k, k, h - 2 * k, sh[0], sh[1], sh[2], alpha);
    int ind = light ? 6 * k : 0;
    int tw = text_width(label, k);
    int tx = x + (w - tw - (light ? ind + 3 * k : 0)) / 2 + (on ? k : 0);
    int ty = y + (h - 7 * k) / 2 + (on ? k : 0);
    SDL_Color shc = {198, 200, 228, alpha}, tc = {34, 34, 46, alpha};
    if (on) { shc = (SDL_Color){150, 150, 180, alpha}; tc = (SDL_Color){62, 62, 80, alpha}; }
    draw_text(rc, tx + k, ty + k, k, label, shc);
    draw_text(rc, tx, ty, k, label, tc);
    if (light) {
        int ix = tx + tw + 4 * k, iy = y + (h - ind) / 2;
        if (on) { fill(r, ix, iy, ind, ind, 35, 194, 3, alpha); fill(r, ix + k, iy + k, ind - 2 * k, ind - 2 * k, 72, 236, 64, alpha); }
        else { fill(r, ix, iy, ind, ind, 6, 30, 0, alpha); fill(r, ix + k, iy + k, ind - 2 * k, ind - 2 * k, 9, 51, 0, alpha); }
    }
}

static void rc_init(RCtx *rc, SDL_Window *win)
{
    Uint32 flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
    rc->ren = SDL_CreateRenderer(win, -1, flags);
    if (!rc->ren) {
        SDL_Log("renderer: accelerated failed (%s), using software", SDL_GetError());
        rc->ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_RendererInfo info;
    if (rc->ren && SDL_GetRendererInfo(rc->ren, &info) == 0) SDL_Log("renderer: %s", info.name);
    SDL_SetRenderDrawBlendMode(rc->ren, SDL_BLENDMODE_BLEND);
    rc->font_tex = make_font_tex(rc->ren);
    rc->uploaded_serial = -1;
}

static void rc_upload(RCtx *rc)
{
    if (!g_frame) return;
    const char *filter = cfg_get("filter", "sharp");
    int want_sharp = !SDL_strcmp(filter, "sharp");
    if (!rc->frame_tex || rc->tex_w != g_fw || rc->tex_h != g_fh) {
        if (rc->frame_tex) SDL_DestroyTexture(rc->frame_tex);
        if (rc->sharp_tex) SDL_DestroyTexture(rc->sharp_tex);
        rc->sharp_tex = NULL;
        rc->frame_tex = SDL_CreateTexture(rc->ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, g_fw, g_fh);
        SDL_SetTextureScaleMode(rc->frame_tex, !SDL_strcmp(filter, "linear") ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        rc->tex_w = g_fw; rc->tex_h = g_fh;
        if (want_sharp) {
            int k = cfg_int("sharp_scale", 0);
            if (k <= 0) k = clampi(1280 / g_fw, 1, 8);
            rc->sharp_k = k;
            rc->sharp_tex = SDL_CreateTexture(rc->ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET, g_fw * k, g_fh * k);
            if (rc->sharp_tex) SDL_SetTextureScaleMode(rc->sharp_tex, SDL_ScaleModeLinear);
            else SDL_Log("renderer: no target textures, using nearest scaling");
        }
        rc->uploaded_serial = -1;
    }
    if (rc->uploaded_serial == g_frame_serial) return;
    SDL_UpdateTexture(rc->frame_tex, NULL, g_disp ? g_disp : g_frame, g_fw * 4);
    if (rc->sharp_tex) {
        SDL_SetRenderTarget(rc->ren, rc->sharp_tex);
        SDL_RenderSetViewport(rc->ren, NULL);
        SDL_RenderCopy(rc->ren, rc->frame_tex, NULL, NULL);
        SDL_SetRenderTarget(rc->ren, NULL);
    }
    rc->uploaded_serial = g_frame_serial;
}

/* draw frame region src (frame px) into dst (viewport px) with float precision */
static void draw_frame_region(RCtx *rc, FRect src, FRect dst)
{
    SDL_Texture *t = rc->sharp_tex ? rc->sharp_tex : rc->frame_tex;
    if (!t) return;
    float u0 = src.x / g_fw, v0 = src.y / g_fh, u1 = (src.x + src.w) / g_fw, v1 = (src.y + src.h) / g_fh;
    SDL_Vertex v[4];
    SDL_Color white = {255, 255, 255, 255};
    v[0].position = (SDL_FPoint){dst.x, dst.y};                 v[0].tex_coord = (SDL_FPoint){u0, v0};
    v[1].position = (SDL_FPoint){dst.x + dst.w, dst.y};         v[1].tex_coord = (SDL_FPoint){u1, v0};
    v[2].position = (SDL_FPoint){dst.x + dst.w, dst.y + dst.h}; v[2].tex_coord = (SDL_FPoint){u1, v1};
    v[3].position = (SDL_FPoint){dst.x, dst.y + dst.h};         v[3].tex_coord = (SDL_FPoint){u0, v1};
    for (int i = 0; i < 4; i++) v[i].color = white;
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(rc->ren, t, v, 4, idx, 6);
}

/* ---- OSD ---------------------------------------------------------------- */
static Uint32 g_pan_at;
static void osd(const char *msg) { if (cfg_int("debug", 0)) SDL_Log("osd: %s", msg); }

static Uint8 fade_alpha(Uint32 since, Uint32 hold, Uint32 fadems)
{
    Uint32 now = SDL_GetTicks();
    if (!since) return 0;
    Uint32 d = now - since;
    if (d < hold) return 255;
    if (d > hold + fadems) return 0;
    return (Uint8)(255 - (d - hold) * 255 / fadems);
}

/* ---- on-screen keyboard -------------------------------------------------- */
static int g_osk;
static int g_osk_shift;
typedef struct { const char *label; int key; float w; } OskKey;
static const OskKey OSK_ROWS[5][12] = {
    {{"1", '1', 1}, {"2", '2', 1}, {"3", '3', 1}, {"4", '4', 1}, {"5", '5', 1}, {"6", '6', 1}, {"7", '7', 1}, {"8", '8', 1}, {"9", '9', 1}, {"0", '0', 1}, {"-", '-', 1}, {0, 0, 0}},
    {{"Q", 'q', 1}, {"W", 'w', 1}, {"E", 'e', 1}, {"R", 'r', 1}, {"T", 't', 1}, {"Y", 'y', 1}, {"U", 'u', 1}, {"I", 'i', 1}, {"O", 'o', 1}, {"P", 'p', 1}, {"<-", RETROK_BACKSPACE, 1}, {0, 0, 0}},
    {{"A", 'a', 1}, {"S", 's', 1}, {"D", 'd', 1}, {"F", 'f', 1}, {"G", 'g', 1}, {"H", 'h', 1}, {"J", 'j', 1}, {"K", 'k', 1}, {"L", 'l', 1}, {"ENTER", RETROK_RETURN, 2}, {0, 0, 0}},
    {{"Z", 'z', 1}, {"X", 'x', 1}, {"C", 'c', 1}, {"V", 'v', 1}, {"B", 'b', 1}, {"N", 'n', 1}, {"M", 'm', 1}, {",", ',', 1}, {".", '.', 1}, {"/", '/', 1}, {"^", RETROK_UP, 1}, {0, 0, 0}},
    {{"SHIFT", RETROK_LSHIFT, 2}, {"ESC", RETROK_ESCAPE, 1.5f}, {"SPACE", ' ', 3.5f}, {"<", RETROK_LEFT, 1}, {"v", RETROK_DOWN, 1}, {">", RETROK_RIGHT, 1}, {"HIDE", -1, 1.5f}, {0, 0, 0}},
};

typedef struct { SDL_Rect r; const OskKey *k; } OskHit;
static OskHit g_osk_hits[64];
static int g_osk_nhits;
static SDL_Rect g_osk_area;
static int g_osk_pressed = -1;
static Uint32 g_osk_pressed_at;

static void osk_layout(int vw, int vh)
{
    int ah = vh * 46 / 100;
    g_osk_area = (SDL_Rect){0, vh - ah, vw, ah};
    int pad = vw / 128, rows = 5;
    int rh = (ah - pad * (rows + 1)) / rows;
    g_osk_nhits = 0;
    for (int r = 0; r < rows; r++) {
        float units = 0;
        int n = 0;
        while (OSK_ROWS[r][n].label) { units += OSK_ROWS[r][n].w; n++; }
        float uw = (vw - pad * (n + 1)) / 11.0f;
        if (uw * units + pad * (n + 1) > vw) uw = (vw - pad * (n + 1)) / units;
        float total = uw * units + pad * (n - 1);
        float x = (vw - total) / 2;
        for (int i = 0; i < n; i++) {
            int w = (int)(uw * OSK_ROWS[r][i].w + pad * (OSK_ROWS[r][i].w - 1));
            g_osk_hits[g_osk_nhits].r = (SDL_Rect){(int)x, g_osk_area.y + pad + r * (rh + pad), w, rh};
            g_osk_hits[g_osk_nhits].k = &OSK_ROWS[r][i];
            g_osk_nhits++;
            x += w + pad;
        }
    }
}

static void osk_draw(RCtx *rc, int vw, int vh)
{
    osk_layout(vw, vh);
    int k = SDL_max(2, vh / 256);
    fill(rc->ren, g_osk_area.x, g_osk_area.y, g_osk_area.w, g_osk_area.h, 80, 80, 96, 235);
    fill(rc->ren, g_osk_area.x, g_osk_area.y, g_osk_area.w, k, 139, 133, 159, 255);
    for (int i = 0; i < g_osk_nhits; i++) {
        OskHit *h = &g_osk_hits[i];
        int on = (i == g_osk_pressed && SDL_GetTicks() - g_osk_pressed_at < 150) ||
                 (h->k->key == RETROK_LSHIFT && g_osk_shift);
        int light = h->k->key == RETROK_LSHIFT;
        draw_game_button(rc, h->r.x, h->r.y, h->r.w, h->r.h, k, h->k->label, on, light, 255);
    }
}

static int osk_tap(int vx, int vy)
{
    if (!g_osk || vy < g_osk_area.y) return 0;
    for (int i = 0; i < g_osk_nhits; i++) {
        SDL_Rect *r = &g_osk_hits[i].r;
        if (vx >= r->x && vx < r->x + r->w && vy >= r->y && vy < r->y + r->h) {
            const OskKey *k = g_osk_hits[i].k;
            g_osk_pressed = i; g_osk_pressed_at = SDL_GetTicks();
            if (k->key == -1) { g_osk = 0; osd("KEYBOARD OFF"); }
            else if (k->key == RETROK_LSHIFT) { g_osk_shift = !g_osk_shift; send_key(RETROK_LSHIFT, g_osk_shift); }
            else tap_key(k->key);
        }
    }
    return 1; /* inside keyboard area: swallow */
}

/* ---- drawing a whole screen ------------------------------------------- */
static void draw_screen(Screen *s)
{
    RCtx *rc = s->rc;
    SDL_Renderer *r = rc->ren;
    SDL_RenderSetViewport(r, &s->vp);
    SDL_RenderSetClipRect(r, NULL);
    fill(r, 0, 0, s->vp.w, s->vp.h, 0, 0, 0, 255);
    if (!g_frame) return;
    int vw = s->vp.w, vh = s->vp.h;
    int k = SDL_max(2, vh / 192);
    FRect src, dst;
    if (g_mode == MODE_CONSOLE) {
        /* letterbox with the console's own grey (follows the game's fades) */
        Uint32 c = frame_px(3, 30);
        fill(r, 0, 0, vw, vh, (c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
    }
    if (s->role == 0 && g_mode == MODE_DESK && g_title && cfg_int("desk_title", 1)) {
        if (!rc->title_tex || rc->title_serial != g_title_serial) {
            if (rc->title_tex) SDL_DestroyTexture(rc->title_tex);
            rc->title_tex = SDL_CreateTexture(rc->ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STATIC, g_title_w, g_title_h);
            SDL_SetTextureScaleMode(rc->title_tex, SDL_ScaleModeLinear);
            SDL_UpdateTexture(rc->title_tex, NULL, g_title, g_title_w * 4);
            rc->title_serial = g_title_serial;
        }
        view_geometry(VIEW_FULL, vw, vh, &src, &dst);
        SDL_Rect d = {(int)dst.x, (int)dst.y, (int)dst.w, (int)dst.h};
        SDL_RenderCopy(r, rc->title_tex, NULL, &d);
        return;
    }
    screen_geometry(s->role, vw, vh, &src, &dst);
    draw_frame_region(rc, src, dst);

    if (s->role == 0) {
        return;
    }

    /* bottom screen overlays */
    Uint8 pa = fade_alpha(SDL_max(g_pan_at, g_view_changed_at), 1200, 500);
    if (pa && is_play_mode() && g_view != VIEW_FULL) {
        float mp = max_pan(g_view);
        if (mp > 0) {
            int tw = vw / 2, tx = (vw - tw) / 2, ty = vh - 6 * k - (g_osk ? g_osk_area.h : 0), th = 2 * k;
            float fw = (float)g_fw;
            float frac = src.w / fw;
            int thw = (int)(tw * frac), thx = tx + (int)((tw - thw) * (g_pan[g_view] / mp));
            fill(r, tx - k, ty - k, tw + 2 * k, th + 2 * k, 23, 22, 21, pa);
            fill(r, tx, ty, tw, th, 9, 51, 0, pa);
            fill(r, thx, ty, thw, th, 72, 236, 64, pa);
        }
    }
    if (g_osk) osk_draw(rc, vw, vh);
}

/* ========================================================================== */
/* game-specific UI knowledge                                                 */
/* ========================================================================== */

/* The three zoom buttons (game units, 320x200). Lab and battle differ. */
static const float ZOOM_BTN[2][3][2] = {
    {{123, 72}, {123, 87}, {123, 101}}, /* lab */
    {{303, 49}, {303, 64}, {303, 79}},  /* battle */
};
static const float DISK_POS[2] = {310, 138};

/* classify the area around a zoom arrow: 2 = lit (bright green),
 * 1 = unlit (dark green), 0 = not an arrow */
static int arrow_state(float gx, float gy)
{
    int bright = 0, green = 0;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            Uint32 p = frame_px(gx + dx, gy + dy);
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (g > r + 40 && g > b + 40) { green++; if (g > 200) bright++; }
        }
    return bright >= 2 ? 2 : green >= 2 ? 1 : 0;
}

/* returns the lit zoom index for a layout, or -1 if the layout doesn't match */
static int zoom_state(int layout)
{
    int lit = -1, nlit = 0;
    for (int i = 0; i < 3; i++) {
        int st = arrow_state(ZOOM_BTN[layout][i][0], ZOOM_BTN[layout][i][1]);
        if (st == 0) return -1;
        if (st == 2) { lit = i; nlit++; }
    }
    return nlit == 1 ? lit : -1;
}

static int detect_layout(void)
{
    if (!g_frame) return -1;
    for (int l = 0; l < 2; l++) if (zoom_state(l) >= 0) return l;
    return -1;
}

static int detect_zoom(int layout)
{
    return layout < 0 ? -1 : zoom_state(layout);
}

static int g_last_layout = -1;

/* average-colour 16x10 thumbnail of the current frame */
static void frame_grid(unsigned char out[480])
{
    for (int gy = 0; gy < 10; gy++)
        for (int gx = 0; gx < 16; gx++) {
            int x0 = gx * g_fw / 16, x1 = (gx + 1) * g_fw / 16, y0 = gy * g_fh / 10, y1 = (gy + 1) * g_fh / 10;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2) {
                    Uint32 p = g_frame[y * g_fw + x];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                }
            unsigned char *o = out + (gy * 16 + gx) * 3;
            o[0] = (unsigned char)(r / n); o[1] = (unsigned char)(g / n); o[2] = (unsigned char)(b / n);
        }
}

/* share of thumbnail cells that match a reference within a tolerance */
static float grid_match(const unsigned char *a, const unsigned char *b)
{
    int ok = 0;
    for (int i = 0; i < 480; i += 3) {
        int d = SDL_abs(a[i] - b[i]) + SDL_abs(a[i + 1] - b[i + 1]) + SDL_abs(a[i + 2] - b[i + 2]);
        if (d < 90) ok++;
    }
    return ok / 160.0f;
}

/* briefing/credits console: the four outer edges share one bluish grey */
static int is_console(void)
{
    Uint32 c = frame_px(3, 30);
    if (frame_px(3, 170) != c || frame_px(160, 3) != c || frame_px(160, 197) != c) return 0;
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    return b > r + 8 && SDL_abs(r - g) < 6 && r > 40;
}

static void title_save(void)
{
    char path[1100];
    SDL_snprintf(path, sizeof path, "%s/title.bmp", g_savedir);
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormatFrom(g_title, g_title_w, g_title_h, 32, g_title_w * 4, SDL_PIXELFORMAT_RGB888);
    if (sf) { if (SDL_SaveBMP(sf, path) == 0) SDL_Log("title card saved to %s", path); SDL_FreeSurface(sf); }
}

static void title_load(void)
{
    char path[1100];
    SDL_snprintf(path, sizeof path, "%s/title.bmp", g_savedir);
    SDL_Surface *raw = SDL_LoadBMP(path);
    if (!raw) return;
    SDL_Surface *sf = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGB888, 0);
    SDL_FreeSurface(raw);
    if (!sf) return;
    g_title_w = sf->w; g_title_h = sf->h;
    g_title = (Uint32 *)SDL_malloc((size_t)g_title_w * g_title_h * 4);
    for (int y = 0; y < g_title_h; y++) SDL_memcpy(g_title + y * g_title_w, (Uint8 *)sf->pixels + y * sf->pitch, g_title_w * 4);
    SDL_FreeSurface(sf);
    g_title_serial++;
    SDL_Log("title card loaded from %s", path);
}

/* during the credits the picture monitor always shows the game's logo */
static int g_credits;
static int is_credits_picture(void)
{
    int ok = 0;
    for (int gy = 0; gy < 8; gy++)
        for (int gx = 0; gx < 12; gx++) {
            int x0 = (int)ux(89 + gx * 12), y0 = (int)uy(14 + gy * 89 / 8.0f);
            int x1 = (int)ux(89 + (gx + 1) * 12), y1 = (int)uy(14 + (gy + 1) * 89 / 8.0f);
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    Uint32 p = g_frame[y * g_fw + x];
                    r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                }
            if (!n) continue;
            const unsigned char *ref = SIG_CREDITS + (gy * 12 + gx) * 3;
            int d = SDL_abs((int)(r / n) - ref[0]) + SDL_abs((int)(g / n) - ref[1]) + SDL_abs((int)(b / n) - ref[2]);
            if (d < 90) ok++;
        }
    return ok >= 67; /* 70% of 96 cells */
}

static int g_title_captured;
static int g_desk_seen;
static int g_mode_candidate = -1, g_mode_votes;
static const char *MODE_NAMES[] = {"other", "lab", "battle", "desk", "console"};

/* look at the frame and decide what's on screen (called every few frames) */
static void classify_screen(void)
{
    if (!g_frame) return;
    unsigned char grid[480];
    frame_grid(grid);
    int dark = 0;
    for (int i = 0; i < 480; i += 3) if (grid[i] + grid[i + 1] + grid[i + 2] < 36) dark++;
    if (dark > 150) return; /* fade to black: keep the current layout */

    int m;
    int layout = detect_layout();
    if (layout == 0) m = MODE_LAB;
    else if (layout == 1) m = MODE_BATTLE;
    else if (is_console()) { m = MODE_CONSOLE; g_credits = is_credits_picture(); }
    else if (grid_match(grid, SIG_DESK) >= 0.7f) m = MODE_DESK;
    else {
        m = MODE_OTHER;
        if (!g_title_captured && grid_match(grid, SIG_TITLE) >= 0.85f) {
            if (!g_title || g_title_w != g_fw || g_title_h != g_fh) {
                SDL_free(g_title);
                g_title = (Uint32 *)SDL_malloc((size_t)g_fw * g_fh * 4);
                g_title_w = g_fw; g_title_h = g_fh;
            }
            SDL_memcpy(g_title, g_frame, (size_t)g_fw * g_fh * 4);
            g_title_serial++;
            g_title_captured = 1;
            title_save();
        }
    }
    if (m == g_mode) { g_mode_candidate = -1; return; }
    if (m != g_mode_candidate) { g_mode_candidate = m; g_mode_votes = 1; return; }
    if (++g_mode_votes >= 2) {
        SDL_Log("screen: %s -> %s (program %s)", MODE_NAMES[g_mode], MODE_NAMES[m], g_prog);
        g_mode = m;
        g_mode_candidate = -1;
        if (m == MODE_DESK) g_desk_seen = 1;
        if (m == MODE_LAB || m == MODE_BATTLE) g_last_layout = m == MODE_LAB ? 0 : 1;
    }
}

/* The game's arrow cursor (hotspot at row 0, column 0). k = black, W = white. */
static const char *const CURSOR_ROWS[] = {
    "k", "kk", "kWk", "kWWk", "kWWWk", "kWWWWk", "kWWWWWk", "kWWWWWWk", "kWWWWWWWk",
    "kWWWWWWWWk", "kWWWWWWkkk", "kkkkWWWWk", "....kWWWk", ".....kkkk", NULL};

/* does the cursor sprite sit at (hx,hy)? returns the number of visible
 * sprite pixels that match, or 0 if any visible pixel doesn't */
static int cursor_at(const Uint32 *f, int hx, int hy)
{
    int n = 0;
    for (int r = 0; CURSOR_ROWS[r]; r++) {
        int y = hy + r;
        if (y >= g_fh) break;
        for (int c = 0; CURSOR_ROWS[r][c]; c++) {
            char ch = CURSOR_ROWS[r][c];
            int x = hx + c;
            if (ch == '.' || x >= g_fw) continue;
            Uint32 p = f[y * g_fw + x] & 0xFFFFFF;
            if (p != (ch == 'W' ? 0xFFFFFFu : 0u)) return 0;
            n++;
        }
    }
    return n;
}

/* Copy the frame for display. While the pointer is parked, find the cursor in
 * the bottom-right corner and paint it out with the colours beside it. */
static void update_display(void)
{
    if (!g_frame || g_disp_serial == g_frame_serial) return;
    static int cap;
    if (!g_disp || cap != g_fw * g_fh) { SDL_free(g_disp); cap = g_fw * g_fh; g_disp = (Uint32 *)SDL_malloc((size_t)cap * 4); }
    SDL_memcpy(g_disp, g_frame, (size_t)cap * 4);
    g_disp_serial = g_frame_serial;
    if (!g_parked || !cfg_int("hide_parked_cursor", 1) || g_fw != 320) return;
    int bx = -1, by = -1, best = 3;
    for (int hy = 150; hy < g_fh; hy++)
        for (int hx = 270; hx < g_fw; hx++) {
            int n = cursor_at(g_frame, hx, hy);
            if (n > best) { best = n; bx = hx; by = hy; }
        }
    if (bx < 0) return;
    for (int r = 0; CURSOR_ROWS[r] && by + r < g_fh; r++) {
        int y = by + r, x0 = -1, x1 = -1;
        for (int c = 0; CURSOR_ROWS[r][c]; c++)
            if (CURSOR_ROWS[r][c] != '.' && bx + c < g_fw) { if (x0 < 0) x0 = bx + c; x1 = bx + c; }
        if (x0 < 0) continue;
        Uint32 fillc = x0 > 0 ? g_frame[y * g_fw + x0 - 1] : (x1 + 1 < g_fw ? g_frame[y * g_fw + x1 + 1] : 0);
        for (int x = x0; x <= x1; x++) g_disp[y * g_fw + x] = fillc;
    }
}

/* a game menu or dialog is open in the monitor (yellow backdrop) */
static int game_menu_open(void)
{
    if (!is_play_mode()) return 0;
    int yellow = 0, n = 0;
    for (int y = 16; y < 100; y += 8)
        for (int x = 20; x < 300; x += 10) {
            Uint32 p = frame_px((float)x, (float)y);
            int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            if (r > 180 && g > 180 && b < 150) yellow++;
            n++;
        }
    return yellow * 4 > n;
}

/* Pointer parking: the game draws its own cursor, so after a tap the pointer
 * is moved out of the way once nothing depends on where it is. */
static int g_park_due;
static void park_later(int frames)
{
    if (cfg_int("park_pointer", 1)) g_park_due = g_frame_no + frames;
}
static void action_disk(void)
{
    inject_click(ux(DISK_POS[0]), uy(DISK_POS[1]), 0);
    osd("DISK DRIVE");
}

static void set_view(int v)
{
    g_view = v;
    g_view_changed_at = SDL_GetTicks();
    osd(v == VIEW_TOP ? "TOP HALF" : v == VIEW_BOTTOM ? "BOTTOM HALF" : "FULL SCREEN");
}

static void action_half(int half)
{
    set_view(g_view == half ? VIEW_FULL : half);
}

/* ========================================================================== */
/* pad input                                                                  */
/* ========================================================================== */

enum {
    A_TOP, A_BOTTOM, A_PANL, A_PANR, A_DISK, A_RIGHTMOD, A_OSK, A_SHIFT, A_RETURN, A_ESCAPE,
    A_QUITMOD, A_SWAP, A_COUNT
};
static const char *ACTION_NAMES[A_COUNT] = {
    "top", "bottom", "pan_left", "pan_right", "disk", "right_click", "keyboard", "shift", "enter", "escape",
    "hotkey", "swap_screens"
};
/* Controls are mirrored left/right (D-pad side and face-button side) so either
 * hand can work the buttons while the other holds the stylus.
 * Defaults for the RG DS on ROCKNIX. SDL names: a = bottom button (B),
 * b = right button (A), x = top button (X), y = left button (Y). */
static const char *ACTION_DEFAULTS[A_COUNT] = {
    "dpup,x",                        /* top half: D-pad up or X */
    "dpdown,a",                      /* bottom half: D-pad down or B */
    "leftshoulder,rightshoulder",    /* L1 / R1: pan left */
    "lefttrigger,righttrigger",      /* L2 / R2: pan right */
    "start,leftstick",               /* Start or L3: click the floppy disk */
    "b,dpright",                     /* hold A or D-pad right + tap = right click (or tap with two fingers) */
    "back,key:F1,BTN_BACK",          /* Select or BACK: on-screen keyboard (ROCKNIX sends BACK as F1) */
    "y,dpleft",                      /* hold Y or D-pad left: Shift (vertical walls) */
    "",                              /* Enter: on-screen keyboard */
    "",                              /* Escape: on-screen keyboard */
    "guide",                         /* Menu button as hotkey */
    "",
};

#define MAXB 8
static int g_bind[A_COUNT][MAXB]; /* SDL button ids, triggers as 100+axis */
static int g_nbind[A_COUNT];
static int g_held[A_COUNT];
static Uint32 g_held_since[A_COUNT];
static int g_pad_btn[SDL_CONTROLLER_BUTTON_MAX + 2];

/* Input ids: SDL controller buttons (0..), triggers (100+axis), keyboard
 * scancodes (ID_KEY+scancode) and raw evdev key codes (ID_EVDEV+code). */
#define ID_KEY   0x10000
#define ID_EVDEV 0x20000

static int evdev_code_from_name(const char *t)
{
    if (!SDL_strncmp(t, "evdev:", 6)) t += 6;
    static const struct { const char *n; int c; } names[] = {
        {"BTN_BACK", 0x116}, {"BTN_MODE", 0x13c}, {"BTN_SELECT", 0x13a}, {"BTN_START", 0x13b},
        {"KEY_PLAYPAUSE", 164}, {"KEY_BACK", 158}, {"KEY_HOMEPAGE", 172}, {NULL, 0}};
    for (int i = 0; names[i].n; i++) if (!SDL_strcasecmp(t, names[i].n)) return names[i].c;
    return SDL_atoi(t);
}

/* held state of inputs that aren't SDL controller buttons or triggers */
static struct { int id, down; } g_other_held[16];
static int other_held(int id)
{
    for (int i = 0; i < 16; i++) if (g_other_held[i].id == id) return g_other_held[i].down;
    return 0;
}
static void set_other_held(int id, int down)
{
    for (int i = 0; i < 16; i++) if (g_other_held[i].id == id) { g_other_held[i].down = down; return; }
    for (int i = 0; i < 16; i++) if (!g_other_held[i].id) { g_other_held[i].id = id; g_other_held[i].down = down; return; }
}

static void binds_init(void)
{
    for (int a = 0; a < A_COUNT; a++) {
        char key[64], buf[256];
        SDL_snprintf(key, sizeof key, "bind_%s", ACTION_NAMES[a]);
        SDL_strlcpy(buf, cfg_get(key, ACTION_DEFAULTS[a]), sizeof buf);
        g_nbind[a] = 0;
        char *save = NULL;
        for (char *t = SDL_strtokr(buf, ", ", &save); t && g_nbind[a] < MAXB; t = SDL_strtokr(NULL, ", ", &save)) {
            if (!SDL_strcmp(t, "lefttrigger")) g_bind[a][g_nbind[a]++] = 100 + SDL_CONTROLLER_AXIS_TRIGGERLEFT;
            else if (!SDL_strcmp(t, "righttrigger")) g_bind[a][g_nbind[a]++] = 100 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            else if (!SDL_strncmp(t, "key:", 4)) {
                SDL_Scancode sc = SDL_GetScancodeFromName(t + 4);
                if (sc != SDL_SCANCODE_UNKNOWN) g_bind[a][g_nbind[a]++] = ID_KEY + sc;
                else SDL_Log("config: unknown key '%s' for %s", t + 4, key);
            }
            else if (!SDL_strncmp(t, "evdev:", 6) || !SDL_strncmp(t, "BTN_", 4) || !SDL_strncmp(t, "KEY_", 4)) {
                int code = evdev_code_from_name(t);
                if (code > 0) g_bind[a][g_nbind[a]++] = ID_EVDEV + code;
                else SDL_Log("config: unknown input code '%s' for %s", t, key);
            }
            else {
                SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(t);
                if (b != SDL_CONTROLLER_BUTTON_INVALID) g_bind[a][g_nbind[a]++] = b;
                else SDL_Log("config: unknown button '%s' for %s", t, key);
            }
        }
    }
}

static int g_trigger_state[2];
static int g_running = 1;

static void swap_screens(void)
{
    g_swap = !g_swap;
    for (int i = 0; i < 2; i++) g_scr[i].role = g_single_window ? i : (i ^ g_swap);
    osd(g_swap ? "SCREENS SWAPPED" : "SCREENS NORMAL");
}

static void action_press(int a)
{
    if (g_held[A_QUITMOD] && a != A_QUITMOD) {
        /* hotkey combos */
        if (a == A_DISK) { g_running = 0; return; }       /* hotkey + start: quit */
        if (a == A_OSK) { swap_screens(); return; }        /* hotkey + select: swap screens */
    }
    switch (a) {
    case A_TOP: if (is_play_mode()) action_half(VIEW_TOP); break;
    case A_BOTTOM: if (is_play_mode()) action_half(VIEW_BOTTOM); break;
    case A_PANL:
    case A_PANR:
        if (is_play_mode() && g_view != VIEW_FULL) {
            float step = cfg_float("pan_step", 32);
            g_pan_target[g_view] = clampf(g_pan_target[g_view] + (a == A_PANL ? -step : step), 0, max_pan(g_view));
            g_pan_at = SDL_GetTicks();
        }
        break;
    case A_DISK:
        if (g_held[A_OSK]) { g_running = 0; return; }       /* select + start: quit */
        action_disk();
        break;
    case A_OSK:
        if (g_held[A_DISK]) { g_running = 0; return; }
        g_osk = !g_osk; osd(g_osk ? "KEYBOARD" : "KEYBOARD OFF");
        break;
    case A_SHIFT: send_key(RETROK_LSHIFT, 1); osd("SHIFT"); break;
    case A_RETURN: send_key(RETROK_RETURN, 1); break;
    case A_ESCAPE: tap_key(RETROK_ESCAPE); break;
    case A_RIGHTMOD: osd("RIGHT CLICK"); break;
    case A_SWAP: swap_screens(); break;
    default: break;
    }
}

static void action_release(int a)
{
    switch (a) {
    case A_SHIFT: if (!g_osk_shift) send_key(RETROK_LSHIFT, 0); break;
    case A_RETURN: send_key(RETROK_RETURN, 0); break;
    default: break;
    }
}

static void action_set(int a, int down)
{
    if (down == g_held[a]) return;
    g_held[a] = down;
    if (down) { g_held_since[a] = SDL_GetTicks(); action_press(a); }
    else action_release(a);
}

/* button id (or 100+axis) changed: update every action bound to it */
static int input_bound(int id)
{
    for (int a = 0; a < A_COUNT; a++)
        for (int i = 0; i < g_nbind[a]; i++) if (g_bind[a][i] == id) return 1;
    return 0;
}

static void pad_input(int id, int down)
{
    if (id >= ID_KEY) set_other_held(id, down);
    for (int a = 0; a < A_COUNT; a++)
        for (int i = 0; i < g_nbind[a]; i++)
            if (g_bind[a][i] == id) {
                /* an action is held while any of its bound inputs is held */
                int any = down;
                if (!down)
                    for (int j = 0; j < g_nbind[a]; j++) {
                        int o = g_bind[a][j];
                        if (o == id) continue;
                        if (o >= ID_KEY ? other_held(o) : o >= 100 ? g_trigger_state[o - 100 - SDL_CONTROLLER_AXIS_TRIGGERLEFT] : g_pad_btn[o]) any = 1;
                    }
                action_set(a, any);
            }
}

static float g_stick[2];

/* continuous pan while held, pointer from the left stick */
static void update_continuous(float dt)
{
    Uint32 now = SDL_GetTicks();
    if (g_held[A_QUITMOD] && now - g_held_since[A_QUITMOD] > (Uint32)cfg_int("home_hold_ms", 1000)) {
        SDL_Log("home button held: quitting");
        g_running = 0;
    }
    if (is_play_mode() && g_view != VIEW_FULL) {
        float speed = cfg_float("pan_speed", 240); /* frame px per second */
        float dir = 0;
        if (g_held[A_PANL] && now - g_held_since[A_PANL] > 220) dir -= 1;
        if (g_held[A_PANR] && now - g_held_since[A_PANR] > 220) dir += 1;
        if (dir != 0) {
            g_pan_target[g_view] = clampf(g_pan_target[g_view] + dir * speed * dt, 0, max_pan(g_view));
            g_pan_at = now;
        }
        /* ease towards target */
        float d = g_pan_target[g_view] - g_pan[g_view];
        float k = 1.0f - SDL_powf(0.001f, dt * 2.5f);
        g_pan[g_view] = (SDL_fabsf(d) < 0.25f) ? g_pan_target[g_view] : g_pan[g_view] + d * k;
    }
    if (SDL_fabsf(g_stick[0]) > 0.2f || SDL_fabsf(g_stick[1]) > 0.2f) {
        float sp = cfg_float("stick_speed", 160) * dt;
        float nx = clampf(g_ptr_x + g_stick[0] * sp, 0, (float)g_fw - 1);
        float ny = clampf(g_ptr_y + g_stick[1] * sp * 0.83f, 0, (float)g_fh - 1);
        if (q_empty()) { g_ptr_x = nx; g_ptr_y = ny; g_parked = 0; }
    }
}

/* ========================================================================== */
/* touch                                                                      */
/* ========================================================================== */

static int g_touch_down;
static int g_touch_btn;
static int g_touch_in_osk;

/* bottom-screen normalized coords -> frame coords; returns 0 if off-picture */
static int bottom_to_frame(float nx, float ny, float *fx, float *fy)
{
    Screen *s = g_scr[0].role == 1 ? &g_scr[0] : &g_scr[1];
    int vw = s->vp.w, vh = s->vp.h;
    FRect src, dst;
    screen_geometry(1, vw, vh, &src, &dst);
    float px = nx * vw, py = ny * vh;
    float u = (px - dst.x) / dst.w, v = (py - dst.y) / dst.h;
    int inside = u >= 0 && u <= 1 && v >= 0 && v <= 1;
    u = clampf(u, 0, 0.9999f); v = clampf(v, 0, 0.9999f);
    *fx = src.x + u * src.w;
    *fy = src.y + v * src.h;
    return inside;
}

static Screen *bottom_screen(void) { return g_scr[0].role == 1 ? &g_scr[0] : &g_scr[1]; }

static void touch_down(float nx, float ny)
{
    if (g_touch_down) return;
    Screen *s = bottom_screen();
    if (g_osk) {
        osk_layout(s->vp.w, s->vp.h);
        int vx = (int)(nx * s->vp.w), vy = (int)(ny * s->vp.h);
        if (osk_tap(vx, vy)) { g_touch_down = 1; g_touch_in_osk = 1; return; }
    }
    /* First run only: hold taps during the intro until the title card has
     * been seen once, so the desk can show it on the top screen */
    if (!g_title && g_mode == MODE_OTHER && !g_desk_seen && !SDL_strncasecmp(g_prog, "UNS_PLOT", 8) && cfg_int("desk_title", 1)) {
        g_touch_down = 1; g_touch_in_osk = 1;
        return;
    }
    /* The credits after EXIT TO DOS only respond to Escape */
    if (g_mode == MODE_CONSOLE && g_credits) {
        tap_key(RETROK_ESCAPE);
        g_touch_down = 1; g_touch_in_osk = 1;
        return;
    }
    float fx, fy;
    if (!bottom_to_frame(nx, ny, &fx, &fy) && !(g_mode == MODE_CONSOLE || g_mode == MODE_OTHER)) return;
    g_touch_down = 1;
    g_touch_in_osk = 0;
    g_touch_btn = g_held[A_RIGHTMOD] ? 1 : 0;
    /* Move first, press a few frames later: the game updates hover state
     * (menu highlights, buttons) on its own schedule and applies a press to
     * whatever it last saw under the pointer. */
    q_push(EV_MOVE, fx, fy, 0);
    q_push(EV_WAIT, 0, 0, cfg_int("press_delay_frames", 10));
    q_push(EV_DOWN, 0, 0, g_touch_btn);
}

static void touch_move(float nx, float ny)
{
    if (!g_touch_down || g_touch_in_osk) return;
    float fx, fy;
    bottom_to_frame(nx, ny, &fx, &fy);
    if (q_empty()) { g_ptr_x = fx; g_ptr_y = fy; }
    else q_push(EV_MOVE, fx, fy, 0);
}

/* a second finger landed: make this touch a right click if the press hasn't
 * reached the game yet */
static void touch_second_finger(void)
{
    if (!g_touch_down || g_touch_in_osk || g_touch_btn == 1) return;
    for (int i = g_qh; i != g_qt; i = (i + 1) % QMAX)
        if (g_q[i].type == EV_DOWN) { g_q[i].a = 1; g_touch_btn = 1; return; }
}

static void touch_up(void)
{
    if (!g_touch_down) return;
    g_touch_down = 0;
    if (g_touch_in_osk) { g_touch_in_osk = 0; return; }
    q_push(EV_UP, 0, 0, g_touch_btn);
    park_later(cfg_int("park_delay_frames", 45));
}

/* ---- direct evdev touch (bypasses compositor seat/mapping quirks) ------- */
static int g_evfd = -1;
static int g_ev_min[2], g_ev_max[2], g_ev_cur[2], g_ev_touch, g_ev_was;

#ifdef __linux__
static int test_bit(const unsigned char *bits, int b) { return (bits[b / 8] >> (b % 8)) & 1; }

static void evdev_open(void)
{
    const char *src = cfg_get("touch_source", "auto");
    if (!SDL_strcmp(src, "sdl")) return;
    const char *forced = cfg_get("touch_device", "");
    const char *match = cfg_get("touch_match", "fe5e0000");
    int best = -1, bestfd = -1, bestmatch = 0;
    char bestname[128] = "";
    for (int i = 0; i < 32; i++) {
        char path[256];
        SDL_snprintf(path, sizeof path, "/dev/input/event%d", i);
        if (forced[0]) { if (i) break; SDL_strlcpy(path, forced, sizeof path); }
        int fd = open(path, O_RDONLY_ | O_NONBLOCK_);
        if (fd < 0) { if (forced[0]) SDL_Log("touch: cannot open %s", path); continue; }
        unsigned char absbits[8] = {0}, props[4] = {0};
        char name[128] = "";
        ioctl(fd, EVIOCGNAME_(sizeof name), name);
        ioctl(fd, EVIOCGBIT_(3, sizeof absbits), absbits);
        ioctl(fd, EVIOCGPROP_(sizeof props), props);
        int has_mt = test_bit(absbits, 0x35) && test_bit(absbits, 0x36);
        int direct = test_bit(props, 1);
        if (!forced[0] && !(has_mt && direct)) { close(fd); continue; }
        char link[512] = "", sys[96];
        SDL_snprintf(sys, sizeof sys, "/sys/class/input/event%d", i);
        long n = readlink(sys, link, sizeof link - 1);
        if (n > 0) link[n] = 0;
        int m = match[0] && SDL_strstr(link, match) != NULL;
        SDL_Log("touch: candidate %s '%s' %s%s", path, name, link, m ? " (match)" : "");
        if (best < 0 || (m && !bestmatch)) {
            if (bestfd >= 0) close(bestfd);
            best = i; bestfd = fd; bestmatch = m;
            SDL_strlcpy(bestname, name, sizeof bestname);
        } else close(fd);
    }
    if (bestfd < 0) { SDL_Log("touch: no evdev touchscreen found, using SDL touch events"); return; }
    int info[6];
    for (int a = 0; a < 2; a++) {
        SDL_memset(info, 0, sizeof info);
        ioctl(bestfd, EVIOCGABS_(0x35 + a), info);
        g_ev_min[a] = info[1]; g_ev_max[a] = info[2];
        if (g_ev_max[a] <= g_ev_min[a]) { /* no range reported: use the panel size */
            g_ev_min[a] = 0;
            g_ev_max[a] = a ? cfg_int("touch_size_y", 768) - 1 : cfg_int("touch_size_x", 1024) - 1;
        }
    }
    g_evfd = bestfd;
    SDL_Log("touch: using %s '%s' x %d..%d y %d..%d", forced[0] ? forced : "evdev", bestname, g_ev_min[0], g_ev_max[0], g_ev_min[1], g_ev_max[1]);
}

static void evdev_norm(float *nx, float *ny)
{
    float x = (float)(g_ev_cur[0] - g_ev_min[0]) / (g_ev_max[0] - g_ev_min[0]);
    float y = (float)(g_ev_cur[1] - g_ev_min[1]) / (g_ev_max[1] - g_ev_min[1]);
    if (cfg_int("touch_swap_xy", 0)) { float t = x; x = y; y = t; }
    if (cfg_int("touch_invert_x", 0)) x = 1 - x;
    if (cfg_int("touch_invert_y", 0)) y = 1 - y;
    *nx = clampf(x, 0, 1); *ny = clampf(y, 0, 1);
}

static void evdev_poll(void)
{
    if (g_evfd < 0) return;
    struct { long sec, usec; unsigned short type, code; int value; } ev[64];
    long n;
    while ((n = read(g_evfd, ev, sizeof ev)) > 0) {
        int cnt = (int)(n / sizeof ev[0]);
        for (int i = 0; i < cnt; i++) {
            if (ev[i].type == 3) { /* EV_ABS */
                static int slot = 0;
                if (ev[i].code == 0x2f) slot = ev[i].value;                    /* ABS_MT_SLOT */
                else if (slot == 1 && ev[i].code == 0x39 && ev[i].value >= 0) touch_second_finger();
                else if (slot == 0 && ev[i].code == 0x35) g_ev_cur[0] = ev[i].value;
                else if (slot == 0 && ev[i].code == 0x36) g_ev_cur[1] = ev[i].value;
                else if (slot == 0 && ev[i].code == 0x39) g_ev_touch = ev[i].value >= 0; /* TRACKING_ID */
            } else if (ev[i].type == 1 && ev[i].code == 0x14a) { /* BTN_TOUCH */
                g_ev_touch = ev[i].value != 0;
            } else if (ev[i].type == 0 && ev[i].code == 0) { /* SYN_REPORT */
                float nx, ny;
                evdev_norm(&nx, &ny);
                if (g_ev_touch && !g_ev_was) touch_down(nx, ny);
                else if (g_ev_touch) touch_move(nx, ny);
                else if (g_ev_was) touch_up();
                g_ev_was = g_ev_touch;
            }
        }
    }
}
/* Extra buttons read straight from the kernel (e.g. the RG DS BACK key),
 * for bindings written as BTN_BACK / evdev:CODE. Works when no other
 * process has grabbed the device; otherwise the key:… binding covers it. */
static int g_keyfd[8], g_nkeyfd;
static void evkeys_open(void)
{
    int want[32], nwant = 0;
    for (int a = 0; a < A_COUNT; a++)
        for (int i = 0; i < g_nbind[a]; i++)
            if (g_bind[a][i] >= ID_EVDEV && nwant < 32) want[nwant++] = g_bind[a][i] - ID_EVDEV;
    if (!nwant) return;
    for (int i = 0; i < 32 && g_nkeyfd < 8; i++) {
        char path[64];
        SDL_snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY_ | O_NONBLOCK_);
        if (fd < 0) continue;
        if (fd == g_evfd) { close(fd); continue; }
        unsigned char keybits[96] = {0};
        char name[128] = "";
        ioctl(fd, EVIOCGBIT_(1, sizeof keybits), keybits);
        ioctl(fd, EVIOCGNAME_(sizeof name), name);
        int hit = 0;
        for (int w = 0; w < nwant; w++) if (want[w] < 768 && test_bit(keybits, want[w])) hit = 1;
        if (!hit) { close(fd); continue; }
        g_keyfd[g_nkeyfd++] = fd;
        SDL_Log("input: reading extra buttons from %s '%s'", path, name);
    }
}
static void evkeys_poll(void)
{
    struct { long sec, usec; unsigned short type, code; int value; } ev[32];
    for (int f = 0; f < g_nkeyfd; f++) {
        long n;
        while ((n = read(g_keyfd[f], ev, sizeof ev)) > 0)
            for (int i = 0; i < (int)(n / sizeof ev[0]); i++)
                if (ev[i].type == 1 && ev[i].value != 2) {
                    int id = ID_EVDEV + ev[i].code;
                    if (input_bound(id)) pad_input(id, ev[i].value != 0);
                }
    }
}
#else
static void evdev_open(void) {}
static void evdev_poll(void) {}
static void evkeys_open(void) {}
static void evkeys_poll(void) {}
#endif

/* ========================================================================== */
/* screenshots + test script                                                  */
/* ========================================================================== */

static void save_window(SDL_Window *win, RCtx *rc, const char *path)
{
    int w, h;
    SDL_GetRendererOutputSize(rc->ren, &w, &h);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_RenderSetViewport(rc->ren, NULL);
    SDL_RenderReadPixels(rc->ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    (void)win;
}

static void save_frame(const char *path)
{
    if (!g_frame) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(g_frame, g_fw, g_fh, 32, g_fw * 4, SDL_PIXELFORMAT_RGB888);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}

static char *g_script;
static char *g_script_pos;
static int g_script_wait;
static char g_pending_shot[512];
static int g_nothrottle;

static int key_from_name(const char *n)
{
    if (SDL_strlen(n) == 1) return (unsigned char)SDL_tolower(n[0]);
    if (!SDL_strcasecmp(n, "enter") || !SDL_strcasecmp(n, "return")) return RETROK_RETURN;
    if (!SDL_strcasecmp(n, "escape") || !SDL_strcasecmp(n, "esc")) return RETROK_ESCAPE;
    if (!SDL_strcasecmp(n, "space")) return ' ';
    if (!SDL_strcasecmp(n, "backspace")) return RETROK_BACKSPACE;
    if (!SDL_strcasecmp(n, "shift")) return RETROK_LSHIFT;
    return 0;
}

static int action_from_name(const char *n)
{
    for (int a = 0; a < A_COUNT; a++) if (!SDL_strcmp(n, ACTION_NAMES[a])) return a;
    return -1;
}

static void script_step(void)
{
    if (!g_script_pos) return;
    if (g_script_wait > 0) { g_script_wait--; return; }
    while (g_script_pos && *g_script_pos) {
        char line[512];
        char *nl = SDL_strchr(g_script_pos, '\n');
        size_t len = nl ? (size_t)(nl - g_script_pos) : SDL_strlen(g_script_pos);
        if (len >= sizeof line) len = sizeof line - 1;
        SDL_memcpy(line, g_script_pos, len); line[len] = 0;
        g_script_pos = nl ? nl + 1 : NULL;
        char *l = trim(line);
        if (!*l || *l == '#') continue;
        char cmd[32] = "", a1[256] = "", a2[64] = "", a3[64] = "";
        char *save = NULL, *t;
        t = SDL_strtokr(l, " ", &save); if (t) SDL_strlcpy(cmd, t, sizeof cmd);
        t = SDL_strtokr(NULL, " ", &save); if (t) SDL_strlcpy(a1, t, sizeof a1);
        t = SDL_strtokr(NULL, " ", &save); if (t) SDL_strlcpy(a2, t, sizeof a2);
        t = SDL_strtokr(NULL, " ", &save); if (t) SDL_strlcpy(a3, t, sizeof a3);
        SDL_Log("script: %s %s %s %s", cmd, a1, a2, a3);
        if (!SDL_strcmp(cmd, "wait")) { g_script_wait = SDL_atoi(a1); return; }
        else if (!SDL_strcmp(cmd, "tap")) {
            int right = !SDL_strcmp(a3, "right");
            if (right) g_held[A_RIGHTMOD] = 1;
            touch_down(SDL_atoi(a1) / 1024.0f, SDL_atoi(a2) / 768.0f);
            touch_up();
            if (right) g_held[A_RIGHTMOD] = 0;
        }
        else if (!SDL_strcmp(cmd, "touchdown")) touch_down(SDL_atoi(a1) / 1024.0f, SDL_atoi(a2) / 768.0f);
        else if (!SDL_strcmp(cmd, "touchmove")) touch_move(SDL_atoi(a1) / 1024.0f, SDL_atoi(a2) / 768.0f);
        else if (!SDL_strcmp(cmd, "touchup")) touch_up();
        else if (!SDL_strcmp(cmd, "finger2")) touch_second_finger();
        else if (!SDL_strcmp(cmd, "sdlkey")) { /* push a real key press+release through SDL */
            SDL_Event ev; SDL_zero(ev);
            ev.type = SDL_KEYDOWN; ev.key.keysym.scancode = SDL_GetScancodeFromName(a1);
            ev.key.keysym.sym = SDL_GetKeyFromScancode(ev.key.keysym.scancode); ev.key.state = SDL_PRESSED;
            SDL_PushEvent(&ev);
            ev.type = SDL_KEYUP; ev.key.state = SDL_RELEASED; SDL_PushEvent(&ev);
        }
        else if (!SDL_strcmp(cmd, "press")) { int a = action_from_name(a1); if (a >= 0) action_set(a, 1); }
        else if (!SDL_strcmp(cmd, "release")) { int a = action_from_name(a1); if (a >= 0) action_set(a, 0); }
        else if (!SDL_strcmp(cmd, "action")) { int a = action_from_name(a1); if (a >= 0) { action_set(a, 1); action_set(a, 0); } }
        else if (!SDL_strcmp(cmd, "click")) inject_click(ux((float)SDL_atof(a1)), uy((float)SDL_atof(a2)), !SDL_strcmp(a3, "right"));
        else if (!SDL_strcmp(cmd, "key")) tap_key(key_from_name(a1));
        else if (!SDL_strcmp(cmd, "move")) q_push(EV_MOVE, ux((float)SDL_atof(a1)), uy((float)SDL_atof(a2)), 0);
        else if (!SDL_strcmp(cmd, "shot")) { SDL_strlcpy(g_pending_shot, a1, sizeof g_pending_shot); return; }
        else if (!SDL_strcmp(cmd, "frame")) save_frame(a1);
        else if (!SDL_strcmp(cmd, "pan")) { if (g_view != VIEW_FULL) g_pan[g_view] = g_pan_target[g_view] = clampf((float)SDL_atof(a1), 0, max_pan(g_view)); }
        else if (!SDL_strcmp(cmd, "info")) SDL_Log("info: btn=%d%d credits=%d mode=%s prog=%s view=%d pan=%.1f layout=%d zoom=%d ptr=%.1f,%.1f frame=%dx%d",
                                                   g_btn[0], g_btn[1], g_credits, MODE_NAMES[g_mode], g_prog, g_view, g_pan[g_view], detect_layout(), detect_zoom(detect_layout()), g_ptr_x, g_ptr_y, g_fw, g_fh);
        else if (!SDL_strcmp(cmd, "quit")) { g_running = 0; return; }
        else SDL_Log("script: unknown command '%s'", cmd);
    }
}

/* ========================================================================== */
/* setup                                                                      */
/* ========================================================================== */

static void *sym(const char *name)
{
    void *p = SDL_LoadFunction(core.lib, name);
    if (!p) SDL_Log("core: missing symbol %s", name);
    return p;
}

static int core_load(const char *path)
{
    core.lib = SDL_LoadObject(path);
    if (!core.lib) { SDL_Log("core: cannot load %s: %s", path, SDL_GetError()); return 0; }
    core.init = (void (*)(void))sym("retro_init");
    core.deinit = (void (*)(void))sym("retro_deinit");
    core.api_version = (unsigned (*)(void))sym("retro_api_version");
    core.get_system_info = (void (*)(struct retro_system_info *))sym("retro_get_system_info");
    core.get_system_av_info = (void (*)(struct retro_system_av_info *))sym("retro_get_system_av_info");
    core.set_environment = (void (*)(retro_environment_t))sym("retro_set_environment");
    core.set_video_refresh = (void (*)(retro_video_refresh_t))sym("retro_set_video_refresh");
    core.set_audio_sample = (void (*)(retro_audio_sample_t))sym("retro_set_audio_sample");
    core.set_audio_sample_batch = (void (*)(retro_audio_sample_batch_t))sym("retro_set_audio_sample_batch");
    core.set_input_poll = (void (*)(retro_input_poll_t))sym("retro_set_input_poll");
    core.set_input_state = (void (*)(retro_input_state_t))sym("retro_set_input_state");
    core.run = (void (*)(void))sym("retro_run");
    core.load_game = (bool (*)(const struct retro_game_info *))sym("retro_load_game");
    core.unload_game = (void (*)(void))sym("retro_unload_game");
    if (!core.init || !core.run || !core.load_game) return 0;
    struct retro_system_info si;
    SDL_memset(&si, 0, sizeof si);
    core.get_system_info(&si);
    SDL_Log("core: %s %s (api %u) from %s", si.library_name, si.library_version, core.api_version(), path);
    core.fmt = RETRO_PIXEL_FORMAT_0RGB1555;
    core.set_environment(core_environment);
    core.set_video_refresh(core_video);
    core.set_audio_sample(core_audio_sample);
    core.set_audio_sample_batch(core_audio_batch);
    core.set_input_poll(core_input_poll);
    core.set_input_state(core_input_state);
    core.init();
    return 1;
}

static const char *find_core(char *buf, size_t n)
{
    const char *c = cfg_get("core", "");
    if (c[0]) { path_join(buf, n, g_basedir, c); if (file_exists(buf)) return buf; }
    const char *cands[] = {
        "dosbox_pure_libretro.so",
        "/usr/lib/libretro/dosbox_pure_libretro.so",
        "/tmp/cores/dosbox_pure_libretro.so",
        "/storage/.config/retroarch/cores/dosbox_pure_libretro.so",
        "/usr/lib64/libretro/dosbox_pure_libretro.so",
        "/usr/lib/x86_64-linux-gnu/libretro/dosbox_pure_libretro.so",
        NULL};
    for (int i = 0; cands[i]; i++) {
        path_join(buf, n, g_basedir, cands[i]);
        if (file_exists(buf)) return buf;
    }
    return NULL;
}

static void default_options(void)
{
    /* frontend */
    const char *defs[][2] = {
        {"content", "gamedata/uns.exe"},
        {"dosbox_pure_mouse_input", "direct"},
        {"dosbox_pure_on_screen_keyboard", "false"},
        {"dosbox_pure_auto_mapping", "false"},
        {"dosbox_pure_bind_unused", "false"},
        {"dosbox_pure_mouse_wheel", "none"},
        {"dosbox_pure_menu_time", "0"},
        {"dosbox_pure_savestate", "disabled"},
        {"dosbox_pure_sblaster_conf", "A220 I7 D1 H5"},
        {NULL, NULL}};
    for (int i = 0; defs[i][0]; i++) cfg_set(defs[i][0], defs[i][1]);
}

static void create_screens(void)
{
    int ndisp = SDL_GetNumVideoDisplays();
    const char *mode = cfg_get("window_mode", "auto");
    g_single_window = !SDL_strcmp(mode, "single") || (SDL_strcmp(mode, "dual") && ndisp < 2);
    for (int i = 0; i < ndisp; i++) {
        SDL_Rect b;
        SDL_GetDisplayBounds(i, &b);
        SDL_Log("display %d: %s %dx%d at %d,%d", i, SDL_GetDisplayName(i), b.w, b.h, b.x, b.y);
    }
    if (g_single_window) {
        int w = cfg_int("window_width", 512), h = cfg_int("window_height", 768);
        Uint32 fl = SDL_WINDOW_RESIZABLE;
        SDL_Window *win = SDL_CreateWindow("Unnatural Selection DS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, fl);
        rc_init(&g_rc[0], win);
        for (int i = 0; i < 2; i++) { g_scr[i].win = win; g_scr[i].rc = &g_rc[0]; g_scr[i].role = i; }
        SDL_Log("window: single stacked window %dx%d", w, h);
        return;
    }
    int top = cfg_int("top_display", 0), bot = cfg_int("bottom_display", 1);
    if (top >= ndisp) top = 0;
    if (bot >= ndisp) bot = ndisp > 1 ? 1 : 0;
    /* The bottom window's title contains "Bottom": ROCKNIX's sway config moves
     * such windows to the second panel and powers it on. */
    const char *titles[2] = {"Unnatural Selection", "Unnatural Selection - Bottom"};
    int disp[2] = {top, bot};
    for (int i = 0; i < 2; i++) {
        SDL_Rect b;
        SDL_GetDisplayBounds(disp[i], &b);
        SDL_Window *win = SDL_CreateWindow(titles[i], SDL_WINDOWPOS_UNDEFINED_DISPLAY(disp[i]), SDL_WINDOWPOS_UNDEFINED_DISPLAY(disp[i]),
                                           b.w, b.h, SDL_WINDOW_FULLSCREEN_DESKTOP);
        if (!win) { SDL_Log("window %d: %s", i, SDL_GetError()); continue; }
        rc_init(&g_rc[i], win);
        g_scr[i].win = win; g_scr[i].rc = &g_rc[i]; g_scr[i].role = i;
    }
    if (cfg_int("swap_screens", 0)) swap_screens();
}

static void update_viewports(void)
{
    if (g_single_window) {
        int w, h;
        SDL_GetRendererOutputSize(g_rc[0].ren, &w, &h);
        g_scr[0].vp = (SDL_Rect){0, 0, w, h / 2};
        g_scr[1].vp = (SDL_Rect){0, h / 2, w, h - h / 2};
    } else {
        for (int i = 0; i < 2; i++) {
            if (!g_scr[i].rc) continue;
            int w, h;
            SDL_GetRendererOutputSize(g_scr[i].rc->ren, &w, &h);
            g_scr[i].vp = (SDL_Rect){0, 0, w, h};
        }
    }
}

/* PC keyboard: function keys drive frontend actions, the rest goes to the game */
static int sdl_to_retro_key(SDL_Keycode k)
{
    if (k >= 32 && k < 127) return (int)k;
    switch (k) {
    case SDLK_RETURN: return RETROK_RETURN;
    case SDLK_ESCAPE: return RETROK_ESCAPE;
    case SDLK_BACKSPACE: return RETROK_BACKSPACE;
    case SDLK_TAB: return RETROK_TAB;
    case SDLK_LSHIFT: return RETROK_LSHIFT;
    case SDLK_RSHIFT: return RETROK_RSHIFT;
    case SDLK_LCTRL: return RETROK_LCTRL;
    case SDLK_RCTRL: return RETROK_RCTRL;
    case SDLK_LALT: return RETROK_LALT;
    case SDLK_RALT: return RETROK_RALT;
    case SDLK_UP: return RETROK_UP;
    case SDLK_DOWN: return RETROK_DOWN;
    case SDLK_LEFT: return RETROK_LEFT;
    case SDLK_RIGHT: return RETROK_RIGHT;
    case SDLK_DELETE: return RETROK_DELETE;
    default: return 0;
    }
}

static void handle_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: g_running = 0; break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_CLOSE) g_running = 0;
        break;
    case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController *gc = SDL_GameControllerOpen(e->cdevice.which);
        if (gc) SDL_Log("pad: %s", SDL_GameControllerName(gc));
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        int b = e->cbutton.button, d = e->type == SDL_CONTROLLERBUTTONDOWN;
        if (b >= 0 && b < SDL_CONTROLLER_BUTTON_MAX) {
            if (d && !input_bound(b)) SDL_Log("input: pad button %s (not bound)", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b));
            g_pad_btn[b] = d; pad_input(b, d);
        }
        break;
    }
    case SDL_CONTROLLERAXISMOTION: {
        int ax = e->caxis.axis, v = e->caxis.value;
        if (ax == SDL_CONTROLLER_AXIS_TRIGGERLEFT || ax == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            int i = ax - SDL_CONTROLLER_AXIS_TRIGGERLEFT, d = v > 12000;
            if (d != g_trigger_state[i]) { g_trigger_state[i] = d; pad_input(100 + ax, d); }
        } else if (ax == SDL_CONTROLLER_AXIS_LEFTX) g_stick[0] = v / 32767.0f;
        else if (ax == SDL_CONTROLLER_AXIS_LEFTY) g_stick[1] = v / 32767.0f;
        break;
    }
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP:
        if (g_evfd >= 0 || g_single_window) break; /* evdev is authoritative */
        if (e->type == SDL_FINGERDOWN && g_touch_down) { touch_second_finger(); break; }
        if (e->tfinger.fingerId != 0 && e->type != SDL_FINGERUP && g_touch_down) break;
        if (e->type == SDL_FINGERDOWN) touch_down(e->tfinger.x, e->tfinger.y);
        else if (e->type == SDL_FINGERMOTION) touch_move(e->tfinger.x, e->tfinger.y);
        else touch_up();
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION: {
        /* desktop testing: the mouse acts as the stylus on the bottom screen */
        if (!g_single_window) break;
        int mx = e->type == SDL_MOUSEMOTION ? e->motion.x : e->button.x;
        int my = e->type == SDL_MOUSEMOTION ? e->motion.y : e->button.y;
        int ww, wh, ow, oh;
        SDL_GetWindowSize(g_scr[0].win, &ww, &wh);
        SDL_GetRendererOutputSize(g_rc[0].ren, &ow, &oh);
        float px = mx * (float)ow / ww, py = my * (float)oh / wh;
        Screen *b = bottom_screen();
        float nx = (px - b->vp.x) / b->vp.w, ny = (py - b->vp.y) / b->vp.h;
        if (e->type == SDL_MOUSEBUTTONDOWN) {
            if (ny < 0) break;
            int saved = g_held[A_RIGHTMOD];
            if (e->button.button == SDL_BUTTON_RIGHT) g_held[A_RIGHTMOD] = 1;
            touch_down(nx, ny);
            g_held[A_RIGHTMOD] = saved;
        } else if (e->type == SDL_MOUSEBUTTONUP) touch_up();
        else touch_move(clampf(nx, 0, 1), clampf(ny, 0, 1));
        break;
    }
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        int d = e->type == SDL_KEYDOWN;
        if (e->key.repeat) break;
        SDL_Keycode k = e->key.keysym.sym;
        int kid = ID_KEY + e->key.keysym.scancode;
        if (input_bound(kid)) { pad_input(kid, d); break; }
        if (d) SDL_Log("input: key %s (not bound)", SDL_GetScancodeName(e->key.keysym.scancode));
        int a = -1;
        switch (k) {
        case SDLK_F2: a = A_BOTTOM; break;
        case SDLK_F3: a = A_PANL; break;
        case SDLK_F4: a = A_PANR; break;
        case SDLK_F6: a = A_DISK; break;
        case SDLK_F7: a = A_OSK; break;
        case SDLK_F8: a = A_RIGHTMOD; break;
        case SDLK_F12: if (d) g_running = 0; break;
        default: break;
        }
        if (a >= 0) action_set(a, d);
        else send_key(sdl_to_retro_key(k), d);
        break;
    }
    default: break;
    }
}

static void base_dir_from_argv0(const char *argv0)
{
    const char *env = SDL_getenv("USDS_DIR");
    if (env && env[0]) { SDL_strlcpy(g_basedir, env, sizeof g_basedir); return; }
    char *bp = SDL_GetBasePath();
    if (bp) {
        SDL_strlcpy(g_basedir, bp, sizeof g_basedir);
        size_t n = SDL_strlen(g_basedir);
        if (n > 1 && g_basedir[n - 1] == '/') g_basedir[n - 1] = 0;
        SDL_free(bp);
        return;
    }
    (void)argv0;
}

int main(int argc, char **argv)
{
    base_dir_from_argv0(argc > 0 ? argv[0] : "");
    default_options();
    char path[1024];
    const char *cfgpath = NULL;
    for (int i = 1; i < argc; i++) {
        if (!SDL_strcmp(argv[i], "--config") && i + 1 < argc) cfgpath = argv[++i];
        else if (!SDL_strcmp(argv[i], "--script") && i + 1 < argc) { g_script = slurp(argv[++i], NULL); g_script_pos = g_script; }
        else if (!SDL_strcmp(argv[i], "--nothrottle")) g_nothrottle = 1;
        else if (!SDL_strcmp(argv[i], "--set") && i + 1 < argc) {
            char kv[512]; SDL_strlcpy(kv, argv[++i], sizeof kv);
            char *eq = SDL_strchr(kv, '=');
            if (eq) { *eq = 0; cfg_set(kv, eq + 1); }
        }
    }
    cfg_load(cfgpath ? cfgpath : path_join(path, sizeof path, g_basedir, "usds.cfg"));
    for (int i = 1; i < argc; i++) /* --set wins over the file */
        if (!SDL_strcmp(argv[i], "--set") && i + 1 < argc) {
            char kv[512]; SDL_strlcpy(kv, argv[++i], sizeof kv);
            char *eq = SDL_strchr(kv, '=');
            if (eq) { *eq = 0; cfg_set(kv, eq + 1); }
        }
    SDL_Log("usds %s, base dir %s", VERSION, g_basedir);

    path_join(g_sysdir, sizeof g_sysdir, g_basedir, cfg_get("system_dir", "system"));
    path_join(g_savedir, sizeof g_savedir, g_basedir, cfg_get("save_dir", "saves"));

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return 1;
    }
    char corebuf[1024];
    const char *corepath = find_core(corebuf, sizeof corebuf);
    if (!corepath || !core_load(corepath)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Unnatural Selection",
                                 "DOSBox Pure core (dosbox_pure_libretro.so) not found.", NULL);
        return 1;
    }

    char content[1024];
    path_join(content, sizeof content, g_basedir, cfg_get("content", "gamedata/uns.exe"));
    if (!file_exists(content)) {
        SDL_Log("content not found: %s", content);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Unnatural Selection",
                                 "Game files not found. Copy the DOS game into the gamedata folder.", NULL);
        return 1;
    }
    struct retro_game_info gi;
    SDL_memset(&gi, 0, sizeof gi);
    gi.path = content;
    if (!core.load_game(&gi)) { SDL_Log("core: load_game failed for %s", content); return 1; }
    core.get_system_av_info(&core.av);
    SDL_Log("core: loaded %s, %ux%u @ %.3f fps, audio %.0f Hz", content, core.av.geometry.base_width,
            core.av.geometry.base_height, core.av.timing.fps, core.av.timing.sample_rate);

    create_screens();
    if (!g_scr[0].rc || !g_scr[1].rc) { SDL_Log("could not create both screens"); return 1; }
    if (!g_single_window || cfg_int("hide_cursor", 0)) SDL_ShowCursor(SDL_DISABLE);
    binds_init();
    evdev_open();
    evkeys_open();
    title_load();
    g_title_captured = g_title != NULL;

    g_audio_rate = (int)(core.av.timing.sample_rate > 0 ? core.av.timing.sample_rate : 48000);
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof want);
    want.freq = g_audio_rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else SDL_Log("audio: %s", SDL_GetError());

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { SDL_GameController *gc = SDL_GameControllerOpen(i); if (gc) SDL_Log("pad: %s", SDL_GameControllerName(gc)); }

    double fps = core.av.timing.fps > 1 ? core.av.timing.fps : 70.0;
    Uint64 freq = SDL_GetPerformanceFrequency();
    double acc = 0;
    Uint64 last = SDL_GetPerformanceCounter();
    int vsync = cfg_int("vsync", 1) && !g_nothrottle;
    if (vsync) SDL_RenderSetVSync(g_scr[0].rc->ren, 1);

    while (g_running && !core.shutdown && !g_quit_request) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        evdev_poll();
        evkeys_poll();

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - last) / freq;
        last = now;
        if (dt > 0.25) dt = 0.25;
        int runs;
        if (g_nothrottle) runs = 1;
        else {
            acc += dt * fps;
            runs = (int)acc;
            if (runs > 4) { runs = 4; acc = 0; }
            acc -= runs;
        }
        update_continuous(g_nothrottle ? (float)(1.0 / fps) : (float)dt);
        for (int i = 0; i < runs; i++) {
            script_step();
            q_process();
            core.run();
            g_frame_no++;
            if ((g_frame_no & 7) == 0) classify_screen();
            if (g_inject_park && q_empty()) { g_inject_park = 0; park_later(cfg_int("park_delay_frames", 45)); }
            if (g_park_due && g_frame_no >= g_park_due && q_empty() && !g_touch_down) {
                if (!game_menu_open() && SDL_fabsf(g_stick[0]) < 0.2f && SDL_fabsf(g_stick[1]) < 0.2f) {
                    q_push(EV_MOVE, ux(cfg_float("park_x", 319)), uy(cfg_float("park_y", 199)), 1);
                    g_park_due = 0;
                }
            }
        }
        update_display();
        update_viewports();
        for (int i = 0; i < 2; i++) {
            if (g_single_window && i == 1) break;
            rc_upload(g_scr[i].rc);
        }
        draw_screen(&g_scr[0]);
        if (!g_single_window) { SDL_RenderPresent(g_scr[0].rc->ren); }
        draw_screen(&g_scr[1]);
        if (g_pending_shot[0]) {
            char p[600];
            if (g_single_window) save_window(g_scr[0].win, g_scr[0].rc, g_pending_shot);
            else for (int i = 0; i < 2; i++) { SDL_snprintf(p, sizeof p, "%s.%s.bmp", g_pending_shot, g_scr[i].role ? "bottom" : "top"); save_window(g_scr[i].win, g_scr[i].rc, p); }
            g_pending_shot[0] = 0;
        }
        SDL_RenderPresent(g_scr[1].rc->ren);
        if (!vsync && !g_nothrottle) {
            double spare = (acc < 1 ? (1 - acc) / fps : 0) * 1000.0;
            if (spare > 2) SDL_Delay((Uint32)(spare - 1));
        }
    }

    SDL_Log("exiting");
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    core.unload_game();
    core.deinit();
    SDL_Quit();
    return 0;
}
