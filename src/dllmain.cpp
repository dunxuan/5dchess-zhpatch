// zhpatch - 5D Chess Chinese translation patch
// SDL2.dll proxy: forwards all exports to SDL2_orig.dll and hooks the game's
// text pipeline (measure / draw / font-bake) to substitute English with Chinese.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

// ---------------------------------------------------------------------------
// Game RVAs (image base 0x140000000, verified via IDA)
// ---------------------------------------------------------------------------
static const uint64_t RVA_MEASURE = 0x26640;   // sub_140026640: text layout/measure
static const uint64_t RVA_DRAW    = 0xC45D0;   // sub_1400C45D0: glyph draw
static const uint64_t RVA_BAKER   = 0x25B20;   // sub_140025B20: font style baker
static const uint64_t RVA_PARA_JUST   = 0xC4BB0;  // wrapped justified paragraph draw
static const uint64_t RVA_PARA_CENTER = 0xC55A0;  // wrapped centered paragraph draw

static const size_t   STOLEN_LEN  = 19;        // 8 pushes + sub rsp,imm32
static const uint8_t  PROLOGUE_PREFIX[12] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x53
};

// Style struct: pixel size is a float at +48 (set by the baker).
static const size_t   STYLE_OFF_PIXEL_SIZE = 48;

// ---------------------------------------------------------------------------
// Config / state
// ---------------------------------------------------------------------------
static std::wstring g_gameDir;
static std::string  g_transPath, g_logPath;
static int   g_cfgMaxGlyphs = 6000;     // hard cap on injected glyphs
static int   g_cfgDebug     = 1;
static int   g_cfgMinSize   = 8;        // below this px, never inject
static int   g_cfgMaxSize   = 64;       // above this px, never inject

static std::unordered_map<std::string, std::string> g_map;   // en -> zh (stable after init)
static std::unordered_map<std::string_view, const char*> g_view; // en -> zh cstr
static std::vector<uint16_t> g_injectCps;                     // extra codepoints for baker
static int  g_injectCount = 0;
static bool g_ready = false;
static std::once_flag g_once;

static FILE* g_log = nullptr;
static std::unordered_set<std::string> g_logged;   // once-per-string logging

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_logCs;
static bool g_logCsInit = false;

static void log_line(const char* s) {
    if (!g_cfgDebug || !g_log) return;
    if (g_logCsInit) EnterCriticalSection(&g_logCs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, s);
    fflush(g_log);
    if (g_logCsInit) LeaveCriticalSection(&g_logCs);
}
static void logf(const char* fmt, ...) {
    if (!g_cfgDebug || !g_log) return;
    char buf[1024]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    log_line(buf);
}
static void log_once(const std::string& key, const char* fmt, ...) {
    if (!g_cfgDebug || !g_log) return;
    if (!g_logged.insert(key).second) return;
    char buf[1024]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    log_line(buf);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string unescape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
        char c = s[++i];
        if (c == 'n') o += '\n';
        else if (c == 't') o += '\t';
        else if (c == 'r') o += '\r';
        else o += c;
    }
    return o;
}

static void append_utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// Capacity model: shared by baker (whether to inject) and draw (whether the
// injected set is present). Atlas max = 2048x2048 (baker retries up to 2^11).
// ---------------------------------------------------------------------------
static bool inject_allowed(int pixelSize, int glyphCount) {
    if (pixelSize < g_cfgMinSize || pixelSize > g_cfgMaxSize) return false;
    if (glyphCount <= 0 || glyphCount > g_cfgMaxGlyphs) return false;
    double cell = (double)pixelSize + 6.0;      // padding + alignment
    double need = (double)glyphCount * cell * cell;
    return need <= 4194304.0 * 0.85;
}

// ---------------------------------------------------------------------------
// Init: config + translation + injection list
// ---------------------------------------------------------------------------
static std::wstring getExeDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
    std::wstring p(buf);
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(0, k);
}

static void load_config() {
    std::string cfg = g_transPath; size_t k = cfg.find_last_of("\\/");
    cfg = (k==std::string::npos? "": cfg.substr(0,k+1)) + "config.ini";
    FILE* f = fopen(cfg.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int v; char key[64];
        if (sscanf(line, "max_glyphs = %d", &v) == 1) g_cfgMaxGlyphs = v;
        else if (sscanf(line, "debug = %d", &v) == 1) g_cfgDebug = v;
        else if (sscanf(line, "min_size = %d", &v) == 1) g_cfgMinSize = v;
        else if (sscanf(line, "max_size = %d", &v) == 1) g_cfgMaxSize = v;
        (void)key;
    }
    fclose(f);
}

static void decode_utf8_cps(const std::string& s, std::unordered_set<uint16_t>& out) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = ((c & 0xF) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 7) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); n = 4;
        } else { i++; continue; }
        i += n;
        if (cp <= 0xFFFF) out.insert((uint16_t)cp);
    }
}

static void do_init() {
    InitializeCriticalSection(&g_logCs);
    g_logCsInit = true;
    g_gameDir = getExeDir();
    g_transPath.clear();
    {
        char u[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_ACP, 0, g_gameDir.c_str(), -1, u, sizeof u, 0, 0);
        g_transPath = std::string(u) + "\\zhpatch\\translation.tsv";
        g_logPath   = std::string(u) + "\\zhpatch\\hook.log";
    }
    load_config();
    if (g_cfgDebug) g_log = fopen(g_logPath.c_str(), "w");
    log_line("zhpatch init");

    FILE* f = fopen(g_transPath.c_str(), "rb");
    if (!f) { logf("translation not found: %s", g_transPath.c_str()); }
    else {
        char buf[8192];
        int n = 0;
        while (fgets(buf, sizeof buf, f)) {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty() && (unsigned char)line[0] == 0xEF) { /* BOM */ line.erase(0, 3); }
            size_t tab = line.find('\t');
            if (tab == std::string::npos || tab == 0) continue;
            std::string en = unescape(line.substr(0, tab));
            std::string zh = unescape(line.substr(tab + 1));
            if (en.empty()) continue;
            auto it = g_map.find(en);
            if (it != g_map.end()) continue;         // keep first
            g_map.emplace(std::move(en), std::move(zh));
            n++;
        }
        fclose(f);
        logf("translation entries: %d", n);
    }

    // view index (pointers into g_map values - stable: g_map never rehashes after this)
    for (auto& kv : g_map) g_view.emplace(std::string_view(kv.first), kv.second.c_str());

    // injection codepoints: ASCII 32..126 + all CJK/other of translations.
    // NOTE: the baker's extras loop aborts on the first glyph that fails to
    // bake, so this list must only contain codepoints the font actually has —
    // never control characters (cp<32) or unpaired surrogates.
    std::unordered_set<uint16_t> cps;
    for (int c = 32; c <= 126; c++) cps.insert((uint16_t)c);
    for (auto& kv : g_map) {
        std::unordered_set<uint16_t> tmp;
        decode_utf8_cps(kv.second, tmp);
        for (uint16_t c : tmp)
            if (c >= 128 && !(c >= 0xD800 && c <= 0xDFFF)) cps.insert(c);
    }
    g_injectCps.assign(cps.begin(), cps.end());
    std::sort(g_injectCps.begin(), g_injectCps.end());
    g_injectCount = (int)g_injectCps.size();
    logf("injection codepoints: %d", g_injectCount);

    g_ready = true;
}

static void ensure_init() { std::call_once(g_once, do_init); }

// ---------------------------------------------------------------------------
// Substitution
// ---------------------------------------------------------------------------
static const char* translate(const char* s, uint64_t style) {
    ensure_init();
    if (!s || !g_ready) return s;
    // style-based capability check
    int px = 0;
    if (style) px = (int)(*(float*)((char*)style + STYLE_OFF_PIXEL_SIZE));
    bool capable = inject_allowed(px, g_injectCount);

    auto it = g_view.find(std::string_view(s));
    if (it == g_view.end()) {
        if (g_cfgDebug && s[0] && s[0] >= 0x20) {
            char line[600];
            snprintf(line, sizeof line, "UNTRANSLATED size=%d : %.500s", px, s);
            log_once(std::string(s), "%s", line);
        }
        return s;
    }
    if (!capable) {
        log_once(std::string("!cap:") + s, "NOT-ABLE size=%d (style lacks glyphs): %.200s", px, s);
        return s;
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
typedef uint64_t (*baker_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void*    (*gmalloc_t)(size_t);
typedef void*    (*grealloc_t)(void*, size_t);

static baker_t   o_baker   = nullptr;
static gmalloc_t g_gmalloc = nullptr;    // game's __malloc_base (static CRT heap)
static grealloc_t g_grealloc = nullptr;

// ---------------------------------------------------------------------------
// Style / glyph record view (layout verified via IDA decompilation)
//   style+0  : u32 base codepoint, u32 main count
//   style+8  : Rec[main count]   (28 bytes each)
//              u16 x0,y0,x1,y1 (atlas px); f32 bx@8, by@12, adv@16, w@20, h@24
//   style+16 : u32 extra count, u32 extra cap
//   style+24 : Extra[extra count] (32 bytes each) — same 28B layout + u16 codepoint@28
//   style+40 : u32 atlas_w, +44 atlas_h
//   style+48 : f32 pixel size
//   style+52,+56,+60 : f32 line metrics
// ---------------------------------------------------------------------------
struct Rec {
    uint16_t x0, y0, x1, y1;
    float bx, by, adv, w, h;
};

static inline int style_base(uint64_t st)   { return *(int*)(st + 0); }
static inline int style_count(uint64_t st)  { return *(int*)(st + 4); }
static inline Rec* style_main(uint64_t st)  { return (Rec*)(*(uintptr_t*)(st + 8)); }
static inline int style_excount(uint64_t st){ return *(int*)(st + 16); }
static inline Rec* style_extra(uint64_t st) { return (Rec*)(*(uintptr_t*)(st + 24)); }

// returns glyph record or nullptr when absent
static const Rec* find_rec(uint64_t st, uint32_t cp) {
    if (!st) return nullptr;
    int base = style_base(st), n = style_count(st);
    if (cp >= (uint32_t)base && cp < (uint32_t)base + (uint32_t)n)
        return &style_main(st)[cp - base];
    int en = style_excount(st);
    const uint8_t* ex = (const uint8_t*)style_extra(st);
    if (ex && en > 0) {
        // extras: 32-byte stride, codepoint u16 at +28, metrics layout same as main (0..27)
        for (int i = 0; i < en; i++) {
            const uint8_t* e = ex + (size_t)i * 32;
            if (*(const uint16_t*)(e + 28) == (uint16_t)cp)
                return (const Rec*)e;
        }
    }
    return nullptr;
}

// decode one UTF-8 codepoint; advances p & consumed (budget in bytes)
static uint32_t utf8_next(const unsigned char** p, int* consumed, int budget) {
    const unsigned char* s = *p;
    unsigned char c = s[0];
    uint32_t cp; int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 6 && *consumed + 1 < budget) { cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
    else if ((c >> 4) == 14 && *consumed + 2 < budget) { cp = ((c & 0xF) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3; }
    else if ((c >> 3) == 30 && *consumed + 3 < budget) {
        cp = ((c & 7) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4;
    } else { cp = c; n = 1; }
    *p += n; *consumed += n;
    return cp;
}

// ---------------------------------------------------------------------------
// measure replacement (original: sub_140026640) — UTF-8 aware
// out = [minx, miny, maxx-minx, maxy-miny]
// ---------------------------------------------------------------------------
static float* our_measure(float* out, uint64_t st, const char* str, int start, int lenArg) {
    int total = lenArg;
    if (lenArg == -1) total = (int)strlen(str);
    int budget = total - start;
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    if (budget > 0 && str) {
        const unsigned char* p = (const unsigned char*)str + start;
        int consumed = 0;
        float penx = 0, peny = 0;
        while (consumed < budget) {
            uint32_t cp = utf8_next(&p, &consumed, budget);
            if (cp == '\n') {
                peny += (*(float*)(st + 52) - *(float*)(st + 56)) + *(float*)(st + 60);
                penx = 0;
                continue;
            }
            if (cp == '\t') cp = ' ';
            const Rec* r = find_rec(st, cp);
            if (!r) continue;
            float rx = (float)(int)floorf(penx + r->bx + 0.5f);
            float ry = (float)(int)floorf(peny + r->by + 0.5f);
            penx += r->adv;
            minx = fminf(minx, rx);
            miny = fminf(miny, ry);
            maxx = fmaxf(penx, fmaxf((r->w + rx) - r->bx, maxx));
            maxy = fmaxf(peny, fmaxf((r->h + ry) - r->by, maxy));
        }
    }
    out[0] = minx; out[1] = miny; out[2] = maxx - minx; out[3] = maxy - miny;
    return out;
}

// ---------------------------------------------------------------------------
// draw replacement (original: sub_1400C45D0) — UTF-8 aware quad emitter.
// a1: vert state {int count, int cap; float* data}
// a2: idx  state {int count, int cap; uint32_t* data}
// a3: style; a4: string; a5: color (16B); a6: origin f32[3];
// a7: horiz dir f32[3]; a8: vert dir f32[3]; a9: scale
// ---------------------------------------------------------------------------
static uint64_t our_draw(uint64_t vertS, uint64_t idxS, uint64_t st, const char* str,
                         uint64_t colorP, uint64_t originP, uint64_t hdirP, uint64_t vdirP, float scaleA9) {
    float* verts = (float*)vertS;
    float* idxs  = (float*)idxS;
    const float* color  = (const float*)colorP;
    const float* origin = (const float*)originP;
    const float* hdir   = (const float*)hdirP;
    const float* vdir   = (const float*)vdirP;

    float k = scaleA9 / *(float*)(st + 48);
    int n = (int)strlen(str);

    int vcount = *(int*)verts, vcap = *((int*)verts + 1);
    int need = vcount + 4 * n;
    if (vcap < need) {
        *((int*)verts + 1) = need;
        ((void**)verts)[1] = g_grealloc(((void**)verts)[1], (size_t)36 * need);
    }
    int icount = *(int*)idxs, icap = *((int*)idxs + 1);
    int ineed = icount + 6 * n;
    if (icap < ineed) {
        *((int*)idxs + 1) = ineed;
        ((void**)idxs)[1] = g_grealloc(((void**)idxs)[1], (size_t)4 * ineed);
    }

    float aw = (float)*(int*)(st + 40);
    float ah = (float)*(int*)(st + 44);
    float invW = 1.0f / aw, invH = 1.0f / ah;

    if (n > 0) {
        const unsigned char* p = (const unsigned char*)str;
        int consumed = 0;
        float penx = 0, peny = 0;
        while (consumed < n) {
            uint32_t cp = utf8_next(&p, &consumed, n);
            if (cp == '\n') {
                peny += (*(float*)(st + 52) - *(float*)(st + 56)) + *(float*)(st + 60);
                penx = 0;
                continue;
            }
            if (cp == '\t') cp = ' ';
            const Rec* r = find_rec(st, cp);
            if (!r) continue;

            float bx = r->bx, by = r->by;
            float rx = (float)(int)floorf(penx + bx + 0.5f);
            float ry = (float)(int)floorf(peny + by + 0.5f);
            float right = r->w + rx - bx;
            float bottom = r->h + ry - by;

            float u0 = (float)r->x0 * invW;
            float v0 = (float)r->y0 * invH;
            float u1 = (float)r->x1 * invW;
            float v1 = (float)r->y1 * invH;

            // axes (faithful transcription of original math)
            float vx = vdir[0] * k, vy = vdir[1] * k, vz = vdir[2] * k;
            float hy = hdir[1] * k, hz = hdir[2] * k;
            float hx = hdir[0] * k;
            float negBottom = -bottom, negRy = -ry;

            // corners: pos = origin + hdir*X + vdir*Y  (Y as negative edges, per original)
            // A (rx, negBottom) uv(u0,v1) | B (right, negBottom) uv(u1,v1)
            // C (right, negRy)   uv(u1,v0) | D (rx, negRy)     uv(u0,v0)
            float Ax = origin[0] + hx * rx        + vx * negBottom;
            float Ay = origin[1] + hy * rx        + vy * negBottom;
            float Az = origin[2] + hz * rx        + vz * negBottom;
            float Bx = origin[0] + hx * right     + vx * negBottom;
            float By = origin[1] + hy * right     + vy * negBottom;
            float Bz = origin[2] + hz * right     + vz * negBottom;
            float Cx = origin[0] + hx * right     + vx * negRy;
            float Cy = origin[1] + hy * right     + vy * negRy;
            float Cz = origin[2] + hz * right     + vz * negRy;
            float Dx = origin[0] + hx * rx        + vx * negRy;
            float Dy = origin[1] + hy * rx        + vy * negRy;
            float Dz = origin[2] + hz * rx        + vz * negRy;

            penx += r->adv;

            // grow vertex buffer if needed
            int vc = *(int*)verts, vcp = *((int*)verts + 1);
            if (vc + 4 > vcp) {
                int nc = vc + 4;
                if (nc < 2 * vcp) nc = 2 * vcp;
                if (vcp < nc) {
                    *((int*)verts + 1) = nc;
                    ((void**)verts)[1] = g_grealloc(((void**)verts)[1], (size_t)36 * nc);
                }
            }
            float* vb = ((float**)verts)[1];
            int b0 = *(int*)verts;
            // vertex layout: pos(3f) uv(2f as u64 bits) color(4f) = 9 floats
            struct V { float x, y, z; float u, v; float c[4]; };
            V* vp = (V*)vb + b0;
            vp[0] = { Ax, Ay, Az, u0, v1, {color[0], color[1], color[2], color[3]} };
            vp[1] = { Bx, By, Bz, u1, v1, {color[0], color[1], color[2], color[3]} };
            vp[2] = { Cx, Cy, Cz, u1, v0, {color[0], color[1], color[2], color[3]} };
            vp[3] = { Dx, Dy, Dz, u0, v0, {color[0], color[1], color[2], color[3]} };
            *(int*)verts = vc + 4;

            // indices
            int ic = *(int*)idxs, icp = *((int*)idxs + 1);
            if (ic + 6 > icp) {
                int nc = ic + 6;
                if (nc < 2 * icp) nc = 2 * icp;
                if (icp < nc) {
                    *((int*)idxs + 1) = nc;
                    ((void**)idxs)[1] = g_grealloc(((void**)idxs)[1], (size_t)4 * nc);
                }
            }
            uint32_t* ib = ((uint32_t**)idxs)[1];
            ib[ic + 0] = b0 + 0;
            ib[ic + 1] = b0 + 1;
            ib[ic + 2] = b0 + 2;
            ib[ic + 3] = b0 + 2;
            ib[ic + 4] = b0 + 3;
            ib[ic + 5] = b0 + 0;
            *(int*)idxs = ic + 6;
        }
    }
    return 0;
}

static uint64_t (*o_measure_orig)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static uint64_t (*o_draw_orig)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static uint64_t h_measure(uint64_t a1, uint64_t a2, uint64_t str, uint64_t a4, uint64_t a5) {
    const char* s = translate((const char*)str, a2);
    if (g_cfgDebug && s && s[0]) {
        char line[760];
        snprintf(line, sizeof line, "MEAS start=%d len=%d slen=%d ra=%p str=%.110s",
                 (int)a4, (int)a5, (int)strlen(s), _ReturnAddress(), s);
        log_once(std::string("m:") + s, "%s", line);
    }
    our_measure((float*)a1, a2, s, (int)a4, (int)a5);
    if (g_cfgDebug && s && s[0] && (unsigned char)s[0] >= 0x80) {
        char line[400];
        snprintf(line, sizeof line, "MEASOUT x=%.1f y=%.1f w=%.1f h=%.1f str=%.60s",
                 ((float*)a1)[0], ((float*)a1)[1], ((float*)a1)[2], ((float*)a1)[3], s);
        log_once(std::string("o:") + s, "%s", line);
    }
    return a1;
}

static uint64_t h_draw(uint64_t a1, uint64_t a2, uint64_t style, uint64_t str,
                       uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9) {
    const char* s = translate((const char*)str, style);
    // a9 arrives as float bits in the 9th (stack) argument
    float scale;
    memcpy(&scale, &a9, sizeof scale);
    if (g_cfgDebug && s && s[0]) {
        // count glyph resolution for this string
        int found = 0, miss = 0;
        const unsigned char* p = (const unsigned char*)s;
        int consumed = 0, n = (int)strlen(s);
        while (consumed < n) {
            uint32_t cp = utf8_next(&p, &consumed, n);
            if (cp < 32) continue;
            if (find_rec(style, cp)) found++; else miss++;
        }
        char line[700];
        snprintf(line, sizeof line, "DRAW scale=%.2f found=%d miss=%d str=%.120s",
                 scale, found, miss, s);
        log_once(std::string("d:") + s, "%s", line);
    }
    our_draw(a1, a2, style, s, a5, a6, a7, a8, scale);
    return 0;
}

static uint64_t h_baker(uint64_t out, uint64_t font, uint64_t size, uint64_t sample,
                        uint64_t extra, uint64_t count) {
    ensure_init();
    int px = (int)(size & 0xFFFFFFFF);
    uint64_t ret;
    if (g_ready && inject_allowed(px, g_injectCount)) {
        logf("BAKER inject size=%d sample=%.40s cps=%d", px, (const char*)sample, g_injectCount);
        // Also replace the sample string with the full printable-ASCII set so the
        // main 95-glyph array has every ASCII glyph (originals bake only their
        // sample's characters).
        static char asciiSample[96];
        static bool initAscii = false;
        if (!initAscii) {
            int j = 0;
            for (int c = 32; c <= 126; c++) asciiSample[j++] = (char)c;
            asciiSample[j] = 0;
            initAscii = true;
        }
        ret = o_baker(out, font, size, (uint64_t)(uintptr_t)asciiSample,
                      (uint64_t)(uintptr_t)g_injectCps.data(), (uint64_t)(uint32_t)g_injectCount);
        // verify extras record layout: codepoint should sit at +28, stride 32
        int en = *(int*)(out + 16);
        uintptr_t ep = *(uintptr_t*)(out + 24);
        if (en > 0 && ep) {
            uint16_t first = *(uint16_t*)(ep + 28);
            uint16_t last  = *(uint16_t*)(ep + (size_t)(en - 1) * 32 + 28);
            logf("extras: count=%d first_cp=%u (expect %u) last_cp=%u (expect %u)",
                 en, first, g_injectCps.empty() ? 0 : g_injectCps.front(),
                 last, g_injectCps.empty() ? 0 : g_injectCps.back());
            // dump one CJK entry and one main-array 'A' entry for layout compare
            for (int i = 0; i < en; i++) {
                uint16_t c = *(uint16_t*)(ep + (size_t)i * 32 + 28);
                if (c > 126) {
                    const unsigned char* b = (const unsigned char*)(ep + (size_t)i * 32);
                    logf("EXTRA cp=%u bytes: %02x %02x %02x %02x %02x %02x %02x %02x | f32: %.2f %.2f %.2f %.2f %.2f",
                         c, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                         *(float*)(b+8), *(float*)(b+12), *(float*)(b+16), *(float*)(b+20), *(float*)(b+24));
                    break;
                }
            }
            // main 'A'
            uintptr_t mp = *(uintptr_t*)(out + 8);
            int base = *(int*)out;
            if (mp && base) {
                const unsigned char* b = (const unsigned char*)(mp + (size_t)('A' - base) * 28);
                logf("MAIN 'A' bytes: %02x %02x %02x %02x %02x %02x %02x %02x | f32: %.2f %.2f %.2f %.2f %.2f",
                     b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
                     *(float*)(b+8), *(float*)(b+12), *(float*)(b+16), *(float*)(b+20), *(float*)(b+24));
            }
        } else {
            logf("extras: count=%d ptr=%p", en, (void*)ep);
        }
    } else {
        logf("BAKER skip size=%d sample=%.40s", px, (const char*)sample);
        ret = o_baker(out, font, size, sample, extra, count);
    }
    return ret;
}

// ---------------------------------------------------------------------------
// Inline detour (14-byte abs jmp [rip+0])
// ---------------------------------------------------------------------------
struct Detour {
    void* target;
    void* tramp;          // original bytes + jmp back
    uint8_t* patch;
};

static Detour g_dMeasure, g_dDraw, g_dBaker, g_dParaJust, g_dParaCenter;

static void h_para_just(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8,
                        uint64_t a9bits, uint64_t a10bits, uint64_t a11);
static void h_para_center(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8,
                          uint64_t a9bits, uint64_t a10bits, uint64_t a11);

static bool make_detour(void* target, void* hook, void** origOut, Detour* rec) {
    uint8_t* p = (uint8_t*)target;
    if (memcmp(p, PROLOGUE_PREFIX, sizeof PROLOGUE_PREFIX) != 0) {
        logf("detour: prologue mismatch at %p", target);
        return false;
    }
    // trampoline: stolen bytes + jmp [rip+0];abs
    uint8_t* tr = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    memcpy(tr, p, STOLEN_LEN);
    uint8_t* back = tr + STOLEN_LEN;
    // jmp qword ptr [rip+0]  (6-byte insn) followed by the absolute target
    back[0] = 0xFF; back[1] = 0x25;
    back[2] = back[3] = back[4] = back[5] = 0;
    *(uint64_t*)(back + 6) = (uint64_t)(p + STOLEN_LEN);

    DWORD old;
    if (!VirtualProtect(p, 32, PAGE_EXECUTE_READWRITE, &old)) return false;
    // patch: jmp qword ptr [rip+0]; absolute hook address (14 bytes)
    p[0] = 0xFF; p[1] = 0x25;
    p[2] = p[3] = p[4] = p[5] = 0;
    *(uint64_t*)(p + 6) = (uint64_t)hook;
    for (int i = 14; i < STOLEN_LEN; i++) p[i] = 0x90;
    DWORD tmp; VirtualProtect(p, 32, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), p, STOLEN_LEN);

    *origOut = tr;
    rec->target = target; rec->tramp = tr; rec->patch = p;
    return true;
}

static void install() {
    HMODULE exe = GetModuleHandleW(NULL);
    if (!exe) { logf("no exe module"); return; }
    uint64_t base = (uint64_t)exe;
    logf("exe base = 0x%llX", (unsigned long long)base);

    g_gmalloc  = (gmalloc_t)(base + 0xE3D78);    // j__malloc_base
    g_grealloc = (grealloc_t)(base + 0xE6A64);   // j__realloc_base

    static void* dummy = nullptr;
    bool ok = make_detour((void*)(base + RVA_MEASURE), (void*)h_measure, &dummy, &g_dMeasure);
    ok = make_detour((void*)(base + RVA_DRAW), (void*)h_draw, &dummy, &g_dDraw) && ok;
    ok = make_detour((void*)(base + RVA_BAKER), (void*)h_baker, (void**)&o_baker, &g_dBaker) && ok;
    ok = make_detour((void*)(base + RVA_PARA_JUST), (void*)h_para_just, &dummy, &g_dParaJust) && ok;
    ok = make_detour((void*)(base + RVA_PARA_CENTER), (void*)h_para_center, &dummy, &g_dParaCenter) && ok;
    logf("hooks installed: %s", ok ? "OK" : "FAILED");
}

// ---------------------------------------------------------------------------
// SDL2 forwarding stubs are provided by the .def; we only need DllMain.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Paragraph (wrapped) text — replaces sub_1400C4BB0 (justified) and
// sub_1400C55A0 (centered). Both: split by '\n', then by ' ', greedy-wrap to
// width a10, emit glyph quads. Original walks raw bytes (no UTF-8, main array
// only) — this port decodes UTF-8, looks up extras, and translates the string.
// Alignment: justify adds round(wordIdx * gap) per word; center adds one
// round((wrap-used)*0.5) offset per line. Both emit a phantom space after each
// word (center skips it after the final word, matching the original).
// ---------------------------------------------------------------------------
struct PSeg { int s, e; bool pad; };

static void seg_split(const char* t, int lo, int hi, char delim, std::vector<PSeg>& out) {
    out.clear();
    int start = lo;
    for (int i = lo; i < hi; i++) {
        if (t[i] == delim) { out.push_back({start, i, true}); start = i + 1; }
    }
    out.push_back({start, hi, true});   // trailing segment (mirrors sub_140026D30)
}

static float measure_w(uint64_t st, const char* t, int s, int e) {
    float o[4];
    our_measure(o, st, t, s, e);
    return o[2];
}

static void emit_glyph_quad(uint64_t vertS, uint64_t idxS, const Rec* r, float penx, float peny,
                            float xoff, uint64_t st, const float* color, const float* origin,
                            const float* hdir, const float* vdir, float k) {
    float invW = 1.0f / (float)*(int*)(st + 40);
    float invH = 1.0f / (float)*(int*)(st + 44);
    float u0 = (float)r->x0 * invW, v0 = (float)r->y0 * invH;
    float u1 = (float)r->x1 * invW, v1 = (float)r->y1 * invH;
    float rx = (float)(int)floorf(penx + r->bx + 0.5f);
    float ry = (float)(int)floorf(peny + r->by + 0.5f);
    float right = r->w + rx - r->bx;
    float bottom = r->h + ry - r->by;
    float hx = hdir[0] * k, hy = hdir[1] * k, hz = hdir[2] * k;
    float vx = vdir[0] * k, vy = vdir[1] * k, vz = vdir[2] * k;
    float nb = -bottom, nr = -ry;
    float ox = origin[0] + hx * xoff, oy = origin[1] + hy * xoff, oz = origin[2] + hz * xoff;
    float Ax = ox + hx * rx + vx * nb, Ay = oy + hy * rx + vy * nb, Az = oz + hz * rx + vz * nb;
    float Bx = ox + hx * right + vx * nb, By = oy + hy * right + vy * nb, Bz = oz + hz * right + vz * nb;
    float Cx = ox + hx * right + vx * nr, Cy = oy + hy * right + vy * nr, Cz = oz + hz * right + vz * nr;
    float Dx = ox + hx * rx + vx * nr, Dy = oy + hy * rx + vy * nr, Dz = oz + hz * rx + vz * nr;

    int* verts = (int*)vertS;
    if (verts[0] + 4 > verts[1]) {
        int nc = verts[0] + 4;
        if (nc < 2 * verts[1]) nc = 2 * verts[1];
        if (verts[1] < nc) {
            verts[1] = nc;
            ((void**)verts)[1] = g_grealloc(((void**)verts)[1], (size_t)36 * nc);
        }
    }
    int* idxs = (int*)idxS;
    if (idxs[0] + 6 > idxs[1]) {
        int nc = idxs[0] + 6;
        if (nc < 2 * idxs[1]) nc = 2 * idxs[1];
        if (idxs[1] < nc) {
            idxs[1] = nc;
            ((void**)idxs)[1] = g_grealloc(((void**)idxs)[1], (size_t)4 * nc);
        }
    }
    struct V { float x, y, z; float u, v; float c[4]; };
    V* vp = (V*)((float**)verts)[1] + verts[0];
    vp[0] = { Ax, Ay, Az, u0, v1, {color[0], color[1], color[2], color[3]} };
    vp[1] = { Bx, By, Bz, u1, v1, {color[0], color[1], color[2], color[3]} };
    vp[2] = { Cx, Cy, Cz, u1, v0, {color[0], color[1], color[2], color[3]} };
    vp[3] = { Dx, Dy, Dz, u0, v0, {color[0], color[1], color[2], color[3]} };
    int b0 = verts[0];
    verts[0] += 4;
    uint32_t* ib = ((uint32_t**)idxs)[1];
    int ic = idxs[0];
    ib[ic + 0] = b0 + 0; ib[ic + 1] = b0 + 1; ib[ic + 2] = b0 + 2;
    ib[ic + 3] = b0 + 2; ib[ic + 4] = b0 + 3; ib[ic + 5] = b0 + 0;
    idxs[0] += 6;
}


static void para_impl(uint64_t vertS, uint64_t idxS, uint64_t st, const char* raw,
                      uint64_t colorP, uint64_t originP, uint64_t hdirP, uint64_t vdirP,
                      float scale, float wrapW, uint64_t outLinesP, bool justify) {
    ensure_init();
    if (wrapW > 12.0f) wrapW -= 12.0f;   // caller passes ~12px more than the panel inner width
    const char* t = translate((const char*)raw, st);
    if (g_cfgDebug && t && t[0]) {
        char ln[300];
        snprintf(ln, sizeof ln, "PARA %s wrapW=%.1f scale=%.1f px=%.0f len=%d str=%.80s",
                 justify ? "JUST" : "CEN", wrapW, scale,
                 *(float*)((char*)st + 48), (int)strlen(t), t);
        log_once(std::string("P:") + t, "%s", ln);
    }
    const float* color = (const float*)colorP;
    const float* origin = (const float*)originP;
    const float* hdir = (const float*)hdirP;
    const float* vdir = (const float*)vdirP;
    float k = scale / *(float*)(st + 48);
    int n = (int)strlen(t);
    bool emit = (vertS != 0 && idxS != 0);

    if (emit) {
        int* verts = (int*)vertS;
        int need = verts[0] + 4 * n;
        if (verts[1] < need) {
            verts[1] = need;
            ((void**)verts)[1] = g_grealloc(((void**)verts)[1], (size_t)36 * need);
        }
        int* idxs = (int*)idxS;
        int ineed = idxs[0] + 6 * n;
        if (idxs[1] < ineed) {
            idxs[1] = ineed;
            ((void**)idxs)[1] = g_grealloc(((void**)idxs)[1], (size_t)4 * ineed);
        }
    }

    float spaceW;
    { float o[4]; our_measure(o, st, " ", 0, -1); spaceW = o[2]; }
    float lineH = (*(float*)(st + 52) - *(float*)(st + 56)) + *(float*)(st + 60);

    std::vector<PSeg> lines, words;
    seg_split(t, 0, n, '\n', lines);
    float y = 0.0f;
    int lineCount = 0;

    for (const PSeg& ln : lines) {
        seg_split(t, ln.s, ln.e, ' ', words);
        int wn = (int)words.size();
        int consumed = 0;
        while (consumed < wn) {
            // greedy wrap: used = sum(w) + spaceW * count (mirrors the original loop)
            int count = 0;
            float used = 0.0f;
            int i = consumed;
            bool trace = false;
            if (g_cfgDebug) {
                static const char pat[] = { (char)0xe6, (char)0x9d, (char)0xa5,
                                             (char)0xe4, (char)0xb8, (char)0x80 };
                for (int q = 0; q + 6 <= ln.e - ln.s; q++)
                    if (!memcmp(t + ln.s + q, pat, 6)) { trace = true; break; }
            }
            if (trace) {
                static int traceBudget = 4;
                if (traceBudget > 0) {
                    traceBudget--;
                    char ln[900];
                    int n2 = snprintf(ln, sizeof ln, "  ITER consumed=%d wn=%d wrapW=%.0f words:",
                                      consumed, wn, wrapW);
                    for (int wi = consumed; wi < wn && wi < consumed + 8; wi++) {
                        n2 += snprintf(ln + n2, sizeof ln - n2, " [%d]len=%d w=%.0f",
                                       wi, words[wi].e - words[wi].s,
                                       measure_w(st, t, words[wi].s, words[wi].e));
                    }
                    log_line(ln);
                }
            }
            for (; i < wn; i++) {
                float w = measure_w(st, t, words[i].s, words[i].e);
                float addW = words[i].pad ? spaceW : 0.0f;
                if (g_cfgDebug && !g_logged.count(std::string("W:") + t)) {
                    char ln[220];
                    snprintf(ln, sizeof ln, "  WRAPDBG i=%d used=%.0f w=%.0f wordlen=%d pad=%d wrapW=%.0f",
                             i, used, w, words[i].e - words[i].s, (int)words[i].pad, wrapW);
                    g_logged.insert(std::string("W:") + t);
                    log_line(ln);
                }
                if (wrapW < used + w) {
                    // (A) doesn't fit. If it's an over-long CJK blob and the line
                    // already has content, steal a character prefix so the line
                    // fills naturally instead of justifying two tiny words apart.
                    if (w > wrapW && used > 0.0f) {
                        int cs = words[i].s, ce = words[i].e;
                        int blen = ce - cs, pb = 0, lastFit = 0;
                        const unsigned char* pp = (const unsigned char*)t + cs;
                        while (pb < blen) {
                            int s0 = pb;
                            utf8_next(&pp, &pb, blen);
                            float cw = measure_w(st, t, cs + s0, cs + pb);
                            if (used + cw > wrapW + 0.5f) break;
                            used += cw;
                            lastFit = pb;
                        }
                        if (lastFit > 0 && lastFit < blen) {
                            // piece lands at index i, remainder at i+1;
                            // include the piece in this line, leave the
                            // REMAINDER (truncated) for the next iteration.
                            words.insert(words.begin() + i, {cs, cs + lastFit, false});
                            words[i + 1].s = cs + lastFit;   // truncate blob
                            wn = (int)words.size();          // refresh count!
                            count = i - consumed + 1;
                            break;
                        }
                    }
                    break;
                }
                used += addW + w;
                count = i - consumed + 1;
                if (wrapW < used) { i++; break; }            // (B) fits only without its space
            }
            if (count == 0) {
                // No word fits: for multi-byte / long tokens (CJK paragraphs have
                // no spaces), hard-split the word at character boundaries and retry.
                const PSeg& wd = words[consumed];
                if (wd.e - wd.s > 1) {
                    std::vector<PSeg> flat;
                    flat.insert(flat.end(), words.begin(), words.begin() + consumed);
                    int cs = wd.s;
                    const unsigned char* q = (const unsigned char*)t + wd.s;
                    int qb = 0;
                    int pieces = 0;
                    { const unsigned char* qc = (const unsigned char*)t + wd.s; int qb2 = 0;
                      while (qb2 < wd.e - wd.s) { utf8_next(&qc, &qb2, wd.e - wd.s); pieces++; } }
                    int pi = 0;
                    while (qb < wd.e - wd.s) {
                        utf8_next(&q, &qb, wd.e - wd.s);
                        pi++;
                        flat.push_back({cs, wd.s + qb, pi == pieces});
                        cs = wd.s + qb;
                    }
                    flat.insert(flat.end(), words.begin() + consumed + 1, words.end());
                    words.swap(flat);
                    wn = (int)words.size();
                    continue;
                }
                count = 1;   // single unbreakable char/token
                used = (words[consumed].pad ? spaceW : 0.0f)
                     + measure_w(st, t, words[consumed].s, words[consumed].e);
                i = consumed + 1;
            }
            bool lastLine = (i >= wn);                        // no more words in this \n-line

            // justify: per-word round(idx * raw_gap); zero on last line / single word
            float gap = 0.0f;
            if (justify && count > 1 && !lastLine)
                gap = (wrapW - used) / (float)(count - 1);
            float center = 0.0f;
            if (!justify)
                center = roundf((wrapW - used) * 0.5f);

            float penx = 0.0f;
            for (int wi = 0; wi < count; wi++) {
                const PSeg& wd = words[consumed + wi];
                float xoff = center + (justify ? roundf((float)wi * gap) : 0.0f);
                bool lastWord = (wi == count - 1);
                int budget = wd.e - wd.s;
                const unsigned char* p = (const unsigned char*)t + wd.s;
                int got = 0;
                while (got < budget) {
                    uint32_t cp = utf8_next(&p, &got, budget);
                    if (justify && cp == '@') {               // paragraph spacer (original C4BB0)
                        y -= lineH * 0.5f;
                        continue;
                    }
                    const Rec* r = find_rec(st, cp);
                    if (r) {
                        if (emit)
                            emit_glyph_quad(vertS, idxS, r, penx, y, xoff,
                                            st, color, origin, hdir, vdir, k);
                        penx += r->adv;
                    }
                }
                // phantom space after every word (original walks one byte past the word);
                // the centered variant skips it after the final word of the output line
                bool skipPhantom = (!wd.pad) || (!justify && lastWord);
                if (!skipPhantom) {
                    const Rec* sp = find_rec(st, ' ');
                    if (sp) {
                        if (emit)
                            emit_glyph_quad(vertS, idxS, sp, penx, y, xoff,
                                            st, color, origin, hdir, vdir, k);
                        penx += sp->adv;
                    }
                }
            }
            if (trace) {
                static int emitBudget = 6;
                if (emitBudget > 0) {
                    emitBudget--;
                    char ln[200];
                    snprintf(ln, sizeof ln, "  EMIT count=%d used=%.0f lastLine=%d lines=%d",
                             count, used, (int)lastLine, lineCount + 1);
                    log_line(ln);
                }
            }
            consumed += count;
            y += lineH;
            lineCount++;
        }
    }
    if (outLinesP) *(int*)outLinesP = lineCount;
}

// --- paragraph wrappers (hook entry points) ----------------------------------
static void h_para_just(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                        uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8,
                        uint64_t a9bits, uint64_t a10bits, uint64_t a11) {
    float scale, wrapW;
    memcpy(&scale, &a9bits, 4);
    memcpy(&wrapW, &a10bits, 4);
    para_impl(a1, a2, a3, (const char*)a4, a5, a6, a7, a8, scale, wrapW, a11, true);
}

static void h_para_center(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8,
                          uint64_t a9bits, uint64_t a10bits, uint64_t a11) {
    float scale, wrapW;
    memcpy(&scale, &a9bits, 4);
    memcpy(&wrapW, &a10bits, 4);
    para_impl(a1, a2, a3, (const char*)a4, a5, a6, a7, a8, scale, wrapW, a11, false);
}

// ---------------------------------------------------------------------------
// SDL event pump wrappers (manual exports; forward.cpp skips these)
// logs each event type once so we can see whether input reaches the game
// ---------------------------------------------------------------------------
typedef int    (__cdecl *PFN_PollEvent)(void*);
typedef int    (__cdecl *PFN_WaitEvent)(void*);
typedef int    (__cdecl *PFN_WaitEventTO)(void*, unsigned);
typedef int    (__cdecl *PFN_Peep)(void*, int, int, unsigned, unsigned);
typedef void   (__cdecl *PFN_Pump)(void);

static HMODULE     g_sdlOrig = nullptr;
static PFN_PollEvent   g_Poll = nullptr;
static PFN_WaitEvent   g_Wait = nullptr;
static PFN_WaitEventTO g_WaitTO = nullptr;
static PFN_Peep        g_Peep = nullptr;
static PFN_Pump        g_Pump = nullptr;
static unsigned    g_evtSeen = 0;   // bitmask for types 0..31 relative to 0x300
static int         g_afterBtn = 0;  // polls remaining to dump game input state

static void resolve_sdl() {
    if (g_Poll) return;
    if (!g_sdlOrig) g_sdlOrig = GetModuleHandleW(L"SDL2_orig.dll");
    if (!g_sdlOrig) return;
    g_Poll   = (PFN_PollEvent)GetProcAddress(g_sdlOrig, "SDL_PollEvent");
    g_Wait   = (PFN_WaitEvent)GetProcAddress(g_sdlOrig, "SDL_WaitEvent");
    g_WaitTO = (PFN_WaitEventTO)GetProcAddress(g_sdlOrig, "SDL_WaitEventTimeout");
    g_Peep   = (PFN_Peep)GetProcAddress(g_sdlOrig, "SDL_PeepEvents");
    g_Pump   = (PFN_Pump)GetProcAddress(g_sdlOrig, "SDL_PumpEvents");
}

static void log_evt(const char* via, const void* e) {
    if (!g_cfgDebug || !e) return;
    const unsigned char* b = (const unsigned char*)e;
    unsigned type = *(const unsigned*)b;
    // event types: KEY 0x300-0x3FF, MOUSE 0x400-0x40F
    unsigned slot = 0;
    bool known = false;
    if (type >= 0x300 && type < 0x320) { slot = type - 0x300; known = true; }
    else if (type >= 0x400 && type < 0x420) { slot = 0x20 + (type - 0x400); known = true; }
    if (known) {
        unsigned bit = 1u << (slot & 31);
        if (g_evtSeen & bit) return;
        g_evtSeen |= bit;
    } else {
        static unsigned other = 0;
        if (++other > 24) return;
    }
    char line[200];
    if (type == 0x400) {  // mouse motion: x@20 y@24
        snprintf(line, sizeof line, "EVT %s motion x=%d y=%d", via, *(const int*)(b + 20), *(const int*)(b + 24));
    } else if (type == 0x401 || type == 0x402) {  // button: button@16, x@20, y@24
        unsigned btn = b[16];
        int x = *(const int*)(b + 20), y = *(const int*)(b + 24);
        // also dump the game's own input globals (RVAs):
        //   leftDown @ +0x147FC8, cursor qword @ +0x147F84
        HMODULE exe = GetModuleHandleW(NULL);
        unsigned char leftDown = 0;
        int curX = 0, curY = 0;
        if (exe) {
            leftDown = *((unsigned char*)exe + 0x147FC8);
            curX = *(int*)((unsigned char*)exe + 0x147F84);
            curY = *(int*)((unsigned char*)exe + 0x147F84 + 4);
        }
        snprintf(line, sizeof line, "EVT %s btn%s b=%u x=%d y=%d | game: leftDown=%u cursor=(%d,%d)",
                 via, type == 0x401 ? "DOWN" : "UP", btn, x, y, leftDown, curX, curY);
    } else {
        snprintf(line, sizeof line, "EVT %s type=0x%X", via, type);
    }
    log_line(line);
    if (type == 0x401 || type == 0x402) g_afterBtn = 40;
}

static void dump_game_input(const char* tag) {
    if (!g_cfgDebug || !g_log || g_afterBtn <= 0) return;
    g_afterBtn--;
    HMODULE exe = GetModuleHandleW(NULL);
    if (!exe) return;
    unsigned char* p = (unsigned char*)exe;
    char line[160];
    snprintf(line, sizeof line, "  post-%s leftDown=%u leftUp=%u cursor=(%d,%d)",
             tag, p[0x147FC8], p[0x147FCC],
             *(int*)(p + 0x147F84), *(int*)(p + 0x147F84 + 4));
    log_line(line);
}

extern "C" {

#pragma comment(linker, "/export:SDL_PollEvent")
#pragma comment(linker, "/export:SDL_WaitEvent")
#pragma comment(linker, "/export:SDL_WaitEventTimeout")
#pragma comment(linker, "/export:SDL_PeepEvents")
#pragma comment(linker, "/export:SDL_PumpEvents")

int __cdecl SDL_PollEvent(void* e) {
    resolve_sdl();
    if (!g_Poll) return 0;
    int r = g_Poll(e);
    if (r == 1) log_evt("poll", e);
    if (r == 0) dump_game_input("idle");
    else {
        const unsigned char* b = (const unsigned char*)e;
        if (b && *(const unsigned*)b != 0x401 && *(const unsigned*)b != 0x402)
            dump_game_input("after");
    }
    return r;
}

int __cdecl SDL_WaitEvent(void* e) {
    resolve_sdl();
    if (!g_Wait) return 0;
    int r = g_Wait(e);
    if (r == 1) log_evt("wait", e);
    return r;
}

int __cdecl SDL_WaitEventTimeout(void* e, unsigned timeout) {
    resolve_sdl();
    if (!g_WaitTO) return 0;
    int r = g_WaitTO(e, timeout);
    if (r == 1) log_evt("waitTO", e);
    return r;
}

int __cdecl SDL_PeepEvents(void* evs, int numevents, int action, unsigned minType, unsigned maxType) {
    resolve_sdl();
    if (!g_Peep) return -1;
    int r = g_Peep(evs, numevents, action, minType, maxType);
    if (r > 0 && evs) log_evt("peep", evs);
    return r;
}

void __cdecl SDL_PumpEvents(void) {
    resolve_sdl();
    if (g_Pump) g_Pump();
    log_evt("pump-dummy", nullptr);   // no-op; keep symbol exported
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        // Make the process DPI-aware BEFORE any window/cursor usage.
        // On scaled displays (e.g. 125%) the game otherwise mixes logical and
        // physical coordinates and all mouse hit-tests miss.
        {
            typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            if (u32) {
                PFN_SetProcessDpiAwarenessContext fn =
                    (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
                // (HANDLE)-4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
                if (fn) fn((HANDLE)(intptr_t)-4);
                else {
                    typedef BOOL (WINAPI *PFN_SetProcessDPIAware)();
                    PFN_SetProcessDPIAware fn2 = (PFN_SetProcessDPIAware)GetProcAddress(u32, "SetProcessDPIAware");
                    if (fn2) fn2();
                }
            }
        }
        // bootstrap log: written before anything else can fail
        {
            wchar_t buf[MAX_PATH * 2] = {};
            GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
            std::wstring p(buf);
            size_t k = p.find_last_of(L"\\/");
            std::wstring logp = (k == std::wstring::npos ? L"" : p.substr(0, k)) + L"\\zhpatch\\boot.log";
            FILE* bf = _wfopen(logp.c_str(), L"a");
            if (bf) {
                fprintf(bf, "attach, proxy=%p\n", (void*)h);
                fflush(bf);
                fclose(bf);
            }
        }
        // ZH_NOHOOK=1 disables hook installation (isolation experiments)
        DWORD envLen = GetEnvironmentVariableW(L"ZH_NOHOOK", NULL, 0);
        bool nohook = (envLen != 0);
        {
            wchar_t buf[MAX_PATH * 2] = {};
            GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
            std::wstring p(buf);
            size_t k = p.find_last_of(L"\\/");
            std::wstring logp = (k == std::wstring::npos ? L"" : p.substr(0, k)) + L"\\zhpatch\\boot.log";
            FILE* bf = _wfopen(logp.c_str(), L"a");
            if (bf) { fprintf(bf, "branch: envLen=%lu nohook=%d pid=%lu\n", envLen, (int)nohook, GetCurrentProcessId()); fclose(bf); }
        }
        if (!nohook) {
            install();
        }
        // marker after install
        {
            wchar_t buf[MAX_PATH * 2] = {};
            GetModuleFileNameW(NULL, buf, MAX_PATH * 2);
            std::wstring p(buf);
            size_t k = p.find_last_of(L"\\/");
            std::wstring logp = (k == std::wstring::npos ? L"" : p.substr(0, k)) + L"\\zhpatch\\boot.log";
            FILE* bf = _wfopen(logp.c_str(), L"a");
            if (bf) {
                fprintf(bf, "install done (baker hook=%p)\n", (void*)o_baker);
                fclose(bf);
            }
        }
    }
    return TRUE;
}
