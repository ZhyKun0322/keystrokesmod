#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "Keystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#define VERSION "1.2.2"

struct KeyState {
    bool w = false, a = false, s = false, d = false;
    bool space = false, lmb = false, rmb = false;
};

static KeyState g_keys;
static std::mutex g_keymutex;

#define AKEYCODE_W     51
#define AKEYCODE_A     29
#define AKEYCODE_S     47
#define AKEYCODE_D     32
#define AKEYCODE_SPACE 62

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static float g_uiscale = 1.0f;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static float g_keysize      = 50.0f;
static float g_opacity      = 1.0f;
static float g_rounding     = 8.0f;
static bool  g_locked       = false;
static bool  g_showsettings = false;
static ImVec2 g_hudpos      = ImVec2(100, 100);
static bool  g_posloaded    = false;

static const char* SAVE_PATHS[] = {
    "/data/data/com.mojang.minecraftpe/files/keystrokes.cfg",
    "/data/data/com.mojang.minecraftpe.preview/files/keystrokes.cfg",
    nullptr
};

static const char* consume_syms[] = {
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEb",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPNS_10InputEventE",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPj",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPjb",
    nullptr
};

static int g_consume_variant = -1;
static bool g_moverelative_hooked = false;

typedef int32_t (*consume_fn_0)(void*, void*, bool, long,      uint32_t*, AInputEvent**, bool);
typedef int32_t (*consume_fn_1)(void*, void*, bool, long,      uint32_t*, AInputEvent**);
typedef int32_t (*consume_fn_2)(void*, void*, bool, long long, uint32_t*, AInputEvent**);
typedef int32_t (*consume_fn_3)(void*, void*, bool, long long, AInputEvent**, uint32_t*);
typedef int32_t (*consume_fn_4)(void*, void*, bool, long long, AInputEvent**, uint32_t*, bool);

static consume_fn_0 orig_consume_0 = nullptr;
static consume_fn_1 orig_consume_1 = nullptr;
static consume_fn_2 orig_consume_2 = nullptr;
static consume_fn_3 orig_consume_3 = nullptr;
static consume_fn_4 orig_consume_4 = nullptr;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

typedef void (*moveRelative_fn)(void*, float, float, float, float);
static moveRelative_fn orig_moveRelative = nullptr;

// ── pattern scan ─────────────────────────────────────────────────────────────
static void* pattern_scan(uintptr_t base, size_t size,
                           const uint8_t* pattern, size_t patlen) {
    if (!base || !size || !pattern || !patlen) return nullptr;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(base);
    for (size_t i = 0; i + patlen <= size; i++) {
        if (memcmp(ptr + i, pattern, patlen) == 0)
            return ptr + i;
    }
    return nullptr;
}

// ── get libminecraftpe.so base and size ───────────────────────────────────────
static void getmcbasesize(uintptr_t& base, size_t& size) {
    base = 0; size = 0;
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return;
    char line[512];
    uintptr_t first_start = 0, last_end = 0;
    while (fgets(line, sizeof(line), maps)) {
        if (!strstr(line, "libminecraftpe.so")) continue;
        if (!strstr(line, "r-xp") && !strstr(line, "r--p")) continue;
        uintptr_t start = 0, end = 0;
        sscanf(line, "%lx-%lx", &start, &end);
        if (!first_start) first_start = start;
        if (end > last_end) last_end = end;
    }
    fclose(maps);
    if (first_start && last_end > first_start) {
        base = first_start;
        size = last_end - first_start;
    }
}

// ── moveRelative hook ─────────────────────────────────────────────────────────
static void hook_moveRelative(void* self, float strafe, float forward,
                               float vertical, float speed) {
    {
        std::lock_guard<std::mutex> lock(g_keymutex);
        g_keys.w = forward >  0.01f;
        g_keys.s = forward < -0.01f;
        g_keys.a = strafe  < -0.01f;
        g_keys.d = strafe  >  0.01f;
    }
    if (orig_moveRelative) orig_moveRelative(self, strafe, forward, vertical, speed);
}

static double nowsec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct CpsTracker {
    static const int MAX_CLICKS = 64;
    double times[MAX_CLICKS] = {};
    int    head  = 0;
    int    count = 0;

    void click() {
        times[head] = nowsec();
        head = (head + 1) % MAX_CLICKS;
        if (count < MAX_CLICKS) count++;
    }

    int get() {
        double now    = nowsec();
        double cutoff = now - 1.0;
        int    n      = 0;
        for (int i = 0; i < count; i++) {
            int idx = (head - 1 - i + MAX_CLICKS) % MAX_CLICKS;
            if (times[idx] >= cutoff) n++;
            else break;
        }
        return n;
    }
};

static CpsTracker g_lmbcps, g_rmbcps;
static bool g_prevlmb = false, g_prevrmb = false;

static float g_lasttouchy = 0.0f;
static bool  g_touchdown  = false;

static const char* getsavepath() {
    for (int i = 0; SAVE_PATHS[i]; i++) {
        FILE* f = fopen(SAVE_PATHS[i], "r");
        if (f) { fclose(f); return SAVE_PATHS[i]; }
        f = fopen(SAVE_PATHS[i], "a");
        if (f) { fclose(f); return SAVE_PATHS[i]; }
    }
    return SAVE_PATHS[0];
}

static void savecfg() {
    const char* path = getsavepath();
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%f %f %f %f %d %f\n",
        g_hudpos.x, g_hudpos.y, g_keysize, g_opacity, (int)g_locked, g_rounding);
    fclose(f);
}

static void loadcfg() {
    if (g_posloaded) return;
    g_posloaded = true;
    for (int i = 0; SAVE_PATHS[i]; i++) {
        FILE* f = fopen(SAVE_PATHS[i], "r");
        if (!f) continue;
        int locked = 0;
        int read = fscanf(f, "%f %f %f %f %d %f",
            &g_hudpos.x, &g_hudpos.y, &g_keysize, &g_opacity, &locked, &g_rounding);
        fclose(f);
        if (read >= 5) {
            g_locked   = (locked != 0);
            g_keysize  = std::max(30.0f,  std::min(g_keysize,  120.0f));
            g_opacity  = std::max(0.1f,   std::min(g_opacity,  1.0f));
            g_rounding = std::max(0.0f,   std::min(g_rounding, 50.0f));
            return;
        }
    }
}

static bool   g_pressing   = false;
static double g_pressstart = 0.0;
static const double LONGPRESS_SEC = 0.5;

static void processinput(AInputEvent* event) {
    if (g_initialized) ImGui_ImplAndroid_HandleInputEvent(event);
    int32_t type = AInputEvent_getType(event);

    std::lock_guard<std::mutex> lock(g_keymutex);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action   = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        int32_t btnstate = AMotionEvent_getButtonState(event);

        bool newlmb = (btnstate & AMOTION_EVENT_BUTTON_PRIMARY)   != 0;
        bool newrmb = (btnstate & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
        if (newlmb && !g_prevlmb) g_lmbcps.click();
        if (newrmb && !g_prevrmb) g_rmbcps.click();
        g_prevlmb  = newlmb;
        g_prevrmb  = newrmb;
        g_keys.lmb = newlmb;
        g_keys.rmb = newrmb;

        if (g_initialized && g_showsettings) {
            float tx = AMotionEvent_getX(event, 0);
            float ty = AMotionEvent_getY(event, 0);
            ImGuiIO& io = ImGui::GetIO();

            if (action == AMOTION_EVENT_ACTION_DOWN) {
                g_lasttouchy    = ty;
                g_touchdown     = true;
                io.MousePos     = ImVec2(tx, ty);
                io.MouseDown[0] = true;
            } else if (action == AMOTION_EVENT_ACTION_MOVE && g_touchdown) {
                float dy     = ty - g_lasttouchy;
                g_lasttouchy = ty;
                io.MousePos  = ImVec2(tx, ty);
                io.MouseWheel += dy * -0.06f;
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
                g_touchdown     = false;
                io.MouseDown[0] = false;
            }
        }

    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t action  = AKeyEvent_getAction(event);
        int32_t keycode = AKeyEvent_getKeyCode(event);
        bool isPressed  = (action == AKEY_EVENT_ACTION_DOWN);
        switch (keycode) {
            case AKEYCODE_W:     g_keys.w     = isPressed; break;
            case AKEYCODE_A:     g_keys.a     = isPressed; break;
            case AKEYCODE_S:     g_keys.s     = isPressed; break;
            case AKEYCODE_D:     g_keys.d     = isPressed; break;
            case AKEYCODE_SPACE: g_keys.space = isPressed; break;
        }
    }
}

static int32_t hook_consume_0(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent, bool a6) {
    int32_t result = orig_consume_0 ? orig_consume_0(thiz, a1, a2, a3, a4, outEvent, a6) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_1(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent) {
    int32_t result = orig_consume_1 ? orig_consume_1(thiz, a1, a2, a3, a4, outEvent) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_2(void* thiz, void* a1, bool a2, long long a3, uint32_t* a4, AInputEvent** outEvent) {
    int32_t result = orig_consume_2 ? orig_consume_2(thiz, a1, a2, a3, a4, outEvent) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_3(void* thiz, void* a1, bool a2, long long a3, AInputEvent** outEvent, uint32_t* a4) {
    int32_t result = orig_consume_3 ? orig_consume_3(thiz, a1, a2, a3, outEvent, a4) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}
static int32_t hook_consume_4(void* thiz, void* a1, bool a2, long long a3, AInputEvent** outEvent, uint32_t* a4, bool a6) {
    int32_t result = orig_consume_4 ? orig_consume_4(thiz, a1, a2, a3, outEvent, a4, a6) : 0;
    if (result == 0 && outEvent && *outEvent) processinput(*outEvent);
    return result;
}

struct glstate {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog); glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.atex);  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo); glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc); glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bdst);
    s.blend = glIsEnabled(GL_BLEND); s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST); s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void restoregl(const glstate& s) {
    glUseProgram(s.prog); glActiveTexture(s.atex); glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao); glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]); glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bsrc, s.bdst);
    s.blend   ? glEnable(GL_BLEND)        : glDisable(GL_BLEND);
    s.cull    ? glEnable(GL_CULL_FACE)    : glDisable(GL_CULL_FACE);
    s.depth   ? glEnable(GL_DEPTH_TEST)   : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void drawkey(const char* label, bool pressed, ImVec2 size) {
    float a = g_opacity;
    ImVec4 color     = pressed ? ImVec4(0.85f, 0.85f, 0.85f, 0.95f*a)
                               : ImVec4(0.18f, 0.20f, 0.22f, 0.88f*a);
    ImVec4 textcolor = pressed ? ImVec4(0.05f, 0.05f, 0.05f, a)
                               : ImVec4(0.90f, 0.90f, 0.90f, a);
    ImGui::PushStyleColor(ImGuiCol_Button,        color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color);
    ImGui::PushStyleColor(ImGuiCol_Text,          textcolor);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void drawkeycps(const char* label, bool pressed, ImVec2 size, int cps) {
    float a = g_opacity;
    ImVec4 color     = pressed ? ImVec4(0.85f, 0.85f, 0.85f, 0.95f*a)
                               : ImVec4(0.18f, 0.20f, 0.22f, 0.88f*a);
    ImVec4 textcolor = pressed ? ImVec4(0.05f, 0.05f, 0.05f, a)
                               : ImVec4(0.90f, 0.90f, 0.90f, a);

    ImGui::PushStyleColor(ImGuiCol_Button,        color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Button("##cpskey", size);

    ImDrawList* dl      = ImGui::GetWindowDrawList();
    ImFont*     fn      = ImGui::GetFont();
    float       fs      = ImGui::GetFontSize();
    float       smallfs = fs * 0.75f;

    ImVec2 labelSz = fn->CalcTextSizeA(fs,      FLT_MAX, 0.0f, label);
    char   cpsbuf[16];
    snprintf(cpsbuf, sizeof(cpsbuf), "%d CPS", cps);
    ImVec2 cpsSz   = fn->CalcTextSizeA(smallfs, FLT_MAX, 0.0f, cpsbuf);

    float gap      = 3.0f;
    float blockH   = labelSz.y + gap + cpsSz.y;
    float blockTop = pos.y + (size.y - blockH) * 0.5f;

    float lx = pos.x + (size.x - labelSz.x) * 0.5f;
    dl->AddText(fn, fs, ImVec2(lx, blockTop),
                ImGui::ColorConvertFloat4ToU32(textcolor), label);

    float  cx     = pos.x + (size.x - cpsSz.x) * 0.5f;
    float  cy     = blockTop + labelSz.y + gap;
    ImVec4 dimcol = ImVec4(textcolor.x, textcolor.y, textcolor.z, textcolor.w * 0.70f);
    dl->AddText(fn, smallfs, ImVec2(cx, cy),
                ImGui::ColorConvertFloat4ToU32(dimcol), cpsbuf);

    ImGui::PopStyleColor(3);
}

static void drawsettings(ImVec2 hudpos) {
    float sw = g_width  * 0.26f;
    float sh = g_height * 0.62f;
    sw = std::max(sw, 220.0f);
    sh = std::max(sh, 340.0f);

    float ks      = g_keysize;
    float spacing = ks * 0.04f;
    float hudW    = ks * 3 + spacing * 2;

    float px = hudpos.x + hudW + 8.0f;
    float py = hudpos.y;
    if (px + sw > g_width)  px = hudpos.x - sw - 8.0f;
    if (py + sh > g_height) py = g_height - sh - 8.0f;
    if (px < 0) px = 8.0f;
    if (py < 0) py = 8.0f;

    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.10f, 0.12f, 0.16f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.16f, 0.20f, 0.28f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,       ImVec4(0.22f, 0.28f, 0.38f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,           ImVec4(0.35f, 0.65f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,     ImVec4(0.50f, 0.80f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            ImVec4(0.35f, 0.65f, 1.00f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.25f, 0.32f, 0.45f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(0.08f, 0.10f, 0.14f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.30f, 0.50f, 0.80f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.40f, 0.60f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(0.50f, 0.75f, 1.00f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(12.0f, 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(6.0f, 11.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,  6.0f);

    ImGui::Begin("##cfg", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize   |
        ImGuiWindowFlags_NoMove);

    float ctrlw = sw - 24.0f;

    ImGui::TextColored(ImVec4(0.85f, 0.92f, 1.00f, 1.0f), "KEYSTROKES  v" VERSION);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // show both hook statuses
    if (g_consume_variant >= 0)
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "KB Hook: variant %d (OK)", g_consume_variant);
    else
        ImGui::TextColored(ImVec4(1.00f, 0.35f, 0.35f, 1.0f), "KB Hook: NOT FOUND");

    if (g_moverelative_hooked)
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "Touch Hook: OK");
    else
        ImGui::TextColored(ImVec4(1.00f, 0.35f, 0.35f, 1.0f), "Touch Hook: NOT FOUND");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "KEY SIZE");
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.00f, 1.0f), "%.0f dp", g_keysize);
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::SliderFloat("##sz", &g_keysize, 30.0f, 120.0f, "")) savecfg();

    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "OPACITY");
    float op = g_opacity * 100.0f;
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.00f, 1.0f), "%.0f%%", op);
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::SliderFloat("##op", &op, 10.0f, 100.0f, "")) {
        g_opacity = op / 100.0f;
        savecfg();
    }

    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "CORNER RADIUS");
    ImGui::TextColored(ImVec4(0.90f, 0.93f, 1.00f, 1.0f), "%.0f dp", g_rounding);
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::SliderFloat("##rnd", &g_rounding, 0.0f, 50.0f, "")) savecfg();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "LOCK POSITION");
    bool locked = g_locked;
    if (ImGui::Checkbox("##lk", &locked)) { g_locked = locked; savecfg(); }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.62f, 0.75f, 1.0f), "Prevent drag & accidental move");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "RESET");
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.30f, 0.55f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.42f, 0.72f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.35f, 0.55f, 0.90f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.88f, 0.93f, 1.00f, 1.00f));
    if (ImGui::Button("Reset to Default", ImVec2(ctrlw, 0))) {
        g_keysize  = 50.0f;
        g_opacity  = 1.0f;
        g_rounding = 8.0f;
        g_locked   = false;
        g_hudpos   = ImVec2(100, 100);
        savecfg();
    }
    ImGui::PopStyleColor(4);

    float remaining = ImGui::GetContentRegionAvail().y
                    - ImGui::GetTextLineHeightWithSpacing() * 2.5f;
    if (remaining > 0) ImGui::Dummy(ImVec2(0, remaining));

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f, 0.50f, 0.65f, 1.0f), "Made by");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.50f, 0.75f, 1.00f, 1.0f), "ZhyKun");

    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(11);

    ImGuiIO& io = ImGui::GetIO();
    bool outsideclick = ImGui::IsMouseClicked(0) &&
        (io.MousePos.x < px || io.MousePos.x > px + sw ||
         io.MousePos.y < py || io.MousePos.y > py + sh);
    if (outsideclick) g_showsettings = false;
}

static void drawmenu() {
    KeyState k;
    { std::lock_guard<std::mutex> lock(g_keymutex); k = g_keys; }

    int lmbcps = g_lmbcps.get();
    int rmbcps = g_rmbcps.get();

    float ks      = g_keysize;
    float spacing = ks * 0.04f;
    float hudW    = ks * 3 + spacing * 2;
    float hudH    = ks * 3     + spacing * 2
                  + ks * 1.5f + spacing
                  + ks * 0.7f + spacing;

    ImGuiIO& io = ImGui::GetIO();
    bool isInside = (io.MousePos.x >= g_hudpos.x && io.MousePos.x <= g_hudpos.x + hudW &&
                     io.MousePos.y >= g_hudpos.y && io.MousePos.y <= g_hudpos.y + hudH);

    if (isInside && io.MouseDown[0] && !g_pressing && !g_showsettings) {
        g_pressing   = true;
        g_pressstart = nowsec();
    }
    if (!io.MouseDown[0]) g_pressing = false;

    if (g_pressing && (nowsec() - g_pressstart) >= LONGPRESS_SEC) {
        g_showsettings = true;
        g_pressing     = false;
    }

    if (!g_locked && !g_showsettings && isInside && ImGui::IsMouseDragging(0)) {
        g_hudpos.x += io.MouseDelta.x;
        g_hudpos.y += io.MouseDelta.y;
        g_hudpos.x = std::max(0.0f, std::min(g_hudpos.x, (float)g_width  - hudW));
        g_hudpos.y = std::max(0.0f, std::min(g_hudpos.y, (float)g_height - hudH));
        savecfg();
    }

    if (g_showsettings) drawsettings(g_hudpos);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowPos(g_hudpos, ImGuiCond_Always);
    ImGui::Begin("##ks", nullptr,
        ImGuiWindowFlags_NoTitleBar       |
        ImGuiWindowFlags_NoBackground     |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove           |
        ImGuiWindowFlags_NoInputs);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, g_rounding);

    ImGui::SetCursorPosX(ks + spacing);
    drawkey("W", k.w, ImVec2(ks, ks));

    drawkey("A", k.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", k.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", k.d, ImVec2(ks, ks));

    float half = (hudW - spacing) / 2.0f;
    drawkeycps("LMB", k.lmb, ImVec2(half, ks * 1.5f), lmbcps); ImGui::SameLine();
    drawkeycps("RMB", k.rmb, ImVec2(half, ks * 1.5f), rmbcps);

    drawkey("_____", k.space, ImVec2(hudW, ks * 0.7f));

    ImGui::PopStyleVar(3);
    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;
    loadcfg();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    int minside = std::min(g_width, g_height);
    int maxside = std::max(g_width, g_height);

    float dpscale = (float)minside / 480.0f;
    dpscale = std::max(0.85f, std::min(dpscale, 2.8f));
    if (maxside > 2400) dpscale = std::min(dpscale * 1.15f, 2.8f);

    g_uiscale = dpscale;

    float fontsize = std::max(14.0f, 15.0f * dpscale);
    ImFontConfig cfg;
    cfg.SizePixels = fontsize;
    io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpscale);
    style.WindowBorderSize = 0.0f;

    float ks      = g_keysize;
    float spacing = ks * 0.04f;
    float hudW    = ks * 3 + spacing * 2;
    float hudH    = ks * 4 + spacing * 3;
    g_hudpos.x = std::max(0.0f, std::min(g_hudpos.x, (float)g_width  - hudW));
    g_hudpos.y = std::max(0.0f, std::min(g_hudpos.y, (float)g_height - hudH));

    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;
    glstate s; savegl(s);
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    drawmenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    restoregl(s);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);
    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    if (ctx == g_targetcontext && surf == g_targetsurface) { g_width = w; g_height = h; setup(); render(); }
    return orig_eglswapbuffers(dpy, surf);
}

static void* mainthread(void*) {
    sleep(5);

    GlossInit(true);

    // Hook EGL swap
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap   = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) {
        GHandle hgles = GlossOpen("libGLESv2.so");
        swap = (void*)GlossSymbol(hgles, "eglSwapBuffers", nullptr);
    }
    if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);

    // Hook InputConsumer::consume (keyboard/mouse)
    GHandle hlib     = GlossOpen("libinput.so");
    void*   symconsume = nullptr;

    for (int i = 0; consume_syms[i]; i++) {
        symconsume = (void*)GlossSymbol(hlib, consume_syms[i], nullptr);
        if (symconsume) {
            g_consume_variant = i;
            LOGI("consume: matched variant %d -> %s", i, consume_syms[i]);
            break;
        }
        LOGI("consume: variant %d not found", i);
    }

    if (!symconsume) {
        LOGW("consume: no variant matched — keyboard/mouse keys will not light up");
    } else {
        switch (g_consume_variant) {
            case 0: GlossHook(symconsume, (void*)hook_consume_0, (void**)&orig_consume_0); break;
            case 1: GlossHook(symconsume, (void*)hook_consume_1, (void**)&orig_consume_1); break;
            case 2: GlossHook(symconsume, (void*)hook_consume_2, (void**)&orig_consume_2); break;
            case 3: GlossHook(symconsume, (void*)hook_consume_3, (void**)&orig_consume_3); break;
            case 4: GlossHook(symconsume, (void*)hook_consume_4, (void**)&orig_consume_4); break;
        }
    }

    // Hook moveRelative via pattern scan (touch dpad support)
    uintptr_t mc_base = 0;
    size_t    mc_size = 0;
    getmcbasesize(mc_base, mc_size);

    if (mc_base && mc_size) {
        // 32-byte pattern from libminecraftpe.so @ 0xe640f48
        static const uint8_t sig[] = {
            0xFD, 0x7B, 0xBA, 0xA9,
            0xFC, 0x6F, 0x01, 0xA9,
            0xFA, 0x67, 0x02, 0xA9,
            0xF8, 0x5F, 0x03, 0xA9,
            0xF6, 0x57, 0x04, 0xA9,
            0xF4, 0x4F, 0x05, 0xA9,
            0xFD, 0x03, 0x00, 0x91,
            0xFF, 0x03, 0x3E, 0xD1
        };
        void* fn = pattern_scan(mc_base, mc_size, sig, sizeof(sig));
        if (fn) {
            GlossHook(fn, (void*)hook_moveRelative, (void**)&orig_moveRelative);
            g_moverelative_hooked = true;
            LOGI("moveRelative hooked by pattern at %p", fn);
        } else {
            LOGW("moveRelative pattern not found — touch dpad will not light up");
        }
    } else {
        LOGW("moveRelative: could not find libminecraftpe.so in memory");
    }

    return nullptr;
}

__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
