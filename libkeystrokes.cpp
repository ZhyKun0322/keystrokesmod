#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <algorithm>

#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "Keystrokes"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

#define VERSION "1.3.0"

// ═══════════════════════════════════════════════════════════════════════════════
//  MoveInputComponent layout (from "I love Tacos" struct dump, MCBE 1.21.x)
//
//  Offset  │ Type                        │ Field
//  ────────┼─────────────────────────────┼──────────────────────────────────────
//  0x00    │ MoveInputState              │ mInputState     (processed, 0x10 bytes)
//  0x10    │ MoveInputState              │ mRawInputState  (raw,       0x10 bytes)
//  0x60    │ brstd::bitset<11, ushort>   │ mFlagValues     (2 bytes)
//  0x62    │ std::array<_Bool, 2>        │ mIsPaddling
//
//  MoveInputState (0x10 bytes assumed layout):
//  +0x00  float  forwardMovement   (+1 = W/forward, -1 = S/backward)
//  +0x04  float  sidewaysMovement  (+1 = D/right,   -1 = A/left)
//
//  mFlagValues bit indices (standard MCBE ordering):
//  Bit 0 : forward  (W)
//  Bit 1 : backward (S)
//  Bit 2 : left     (A)
//  Bit 3 : right    (D)
//  Bit 4 : jump     (Space)
//  Bit 5 : sneak
//  ...up to bit 10
//
//  ┌─────────────────────────────────────────────────────────────────────────┐
//  │  HOW TO FIND MOVE_INPUT_COMP_OFFSET USING LEVILAUNCHER MEMORY EDITOR:  │
//  │                                                                         │
//  │  1. Build and load this mod, go in-game                                │
//  │  2. Open LeviLauncher memory editor                                    │
//  │  3. Attach to com.mojang.minecraftpe                                   │
//  │  4. The settings panel logs g_playerAddr (also shown in HUD)           │
//  │  5. Navigate to that address in the memory editor                      │
//  │  6. Walk forward in-game — scan for a float changing between 0 and 1  │
//  │     within the next ~0x300 bytes of the player struct                  │
//  │  7. That float at +0x00 of the component = forwardMovement             │
//  │  8. offset = (component address) - (player address)                   │
//  │  9. Set MOVE_INPUT_COMP_OFFSET below and rebuild                       │
//  │                                                                         │
//  │  Common values seen in MCBE 1.20–1.21: 0x1B8, 0x1C0, 0x1D0           │
//  └─────────────────────────────────────────────────────────────────────────┘
// ═══════════════════════════════════════════════════════════════════════════════

#define MOVE_INPUT_COMP_OFFSET   0x1B8   // <-- find with LeviLauncher memory editor
#define MOVEMENT_THRESHOLD       0.3f    // analog stick dead zone for touch joystick

// Offsets within MoveInputComponent (from struct dump, should be stable)
#define OFF_FORWARD_MOVEMENT     0x00    // float in mInputState
#define OFF_SIDEWAYS_MOVEMENT    0x04    // float in mInputState
#define OFF_FLAG_VALUES          0x60    // brstd::bitset<11, ushort>

// Bit positions in mFlagValues
#define FLAG_BIT_FORWARD         0
#define FLAG_BIT_BACKWARD        1
#define FLAG_BIT_LEFT            2
#define FLAG_BIT_RIGHT           3
#define FLAG_BIT_JUMP            4

// ── Key state ─────────────────────────────────────────────────────────────────

struct KeyState {
    bool w = false, a = false, s = false, d = false;
    bool space = false, lmb = false, rmb = false;
};

static KeyState   g_keys;
static std::mutex g_keymutex;

// Physical keyboard keycodes (controller / Bluetooth keyboard fallback)
#define AKEYCODE_W     51
#define AKEYCODE_A     29
#define AKEYCODE_S     47
#define AKEYCODE_D     32
#define AKEYCODE_SPACE 62

// ── Player state ──────────────────────────────────────────────────────────────

static uintptr_t g_playerAddr = 0;

// ── Renderer state ────────────────────────────────────────────────────────────

static bool       g_initialized   = false;
static int        g_width         = 0, g_height = 0;
static float      g_uiscale       = 1.0f;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

// ── HUD / settings ────────────────────────────────────────────────────────────

static float  g_keysize      = 50.0f;
static float  g_opacity      = 1.0f;
static float  g_rounding     = 8.0f;
static bool   g_locked       = false;
static bool   g_showsettings = false;
static ImVec2 g_hudpos       = ImVec2(100, 100);
static bool   g_posloaded    = false;

// ── Config paths ──────────────────────────────────────────────────────────────

static const char* SAVE_PATHS[] = {
    "/data/data/com.mojang.minecraftpe/files/keystrokes.cfg",
    "/data/data/com.mojang.minecraftpe.preview/files/keystrokes.cfg",
    nullptr
};

// ── CPS tracker ───────────────────────────────────────────────────────────────

static double nowsec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct CpsTracker {
    static const int MAX_CLICKS = 64;
    double times[MAX_CLICKS] = {};
    int    head = 0, count = 0;

    void click() {
        times[head] = nowsec();
        head = (head + 1) % MAX_CLICKS;
        if (count < MAX_CLICKS) count++;
    }

    int get() {
        double cutoff = nowsec() - 1.0;
        int n = 0;
        for (int i = 0; i < count; i++) {
            int idx = (head - 1 - i + MAX_CLICKS) % MAX_CLICKS;
            if (times[idx] >= cutoff) n++;
            else break;
        }
        return n;
    }
};

static CpsTracker g_lmbcps, g_rmbcps;
static bool       g_prevlmb = false, g_prevrmb = false;

// ── Touch scroll state (for settings panel) ───────────────────────────────────

static float g_lasttouchy = 0.0f;
static bool  g_touchdown  = false;

// ── Long-press detection ──────────────────────────────────────────────────────

static bool   g_pressing   = false;
static double g_pressstart = 0.0;
static const  double LONGPRESS_SEC = 0.5;

// ── Config I/O ────────────────────────────────────────────────────────────────

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
    FILE* f = fopen(getsavepath(), "w");
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

// ═══════════════════════════════════════════════════════════════════════════════
//  MoveInputComponent reader
//  Called every NormalTick from the game thread.
//  Reads touch joystick state from the component and writes g_keys.w/a/s/d.
//
//  Two strategies tried in order:
//   1. mFlagValues bitset at +0x60  (cleanest — single ushort read)
//   2. mInputState floats  at +0x00 (fallback — analog movement vector)
// ═══════════════════════════════════════════════════════════════════════════════

static void readMoveInputComponent(uintptr_t playerBase) {
    if (playerBase == 0 || playerBase < 0x10000) return;

    uintptr_t comp = playerBase + MOVE_INPUT_COMP_OFFSET;

    // Strategy 1: bitset flags
    uint16_t flags = *reinterpret_cast<const uint16_t*>(comp + OFF_FLAG_VALUES);
    bool fw = (flags >> FLAG_BIT_FORWARD)  & 1;
    bool bw = (flags >> FLAG_BIT_BACKWARD) & 1;
    bool lf = (flags >> FLAG_BIT_LEFT)     & 1;
    bool rt = (flags >> FLAG_BIT_RIGHT)    & 1;
    bool jp = (flags >> FLAG_BIT_JUMP)     & 1;

    if (fw || bw || lf || rt || jp) {
        std::lock_guard<std::mutex> lock(g_keymutex);
        g_keys.w = fw; g_keys.s = bw;
        g_keys.a = lf; g_keys.d = rt;
        g_keys.space = jp;
        return;
    }

    // Strategy 2: analog float movement vector
    float fwd  = *reinterpret_cast<const float*>(comp + OFF_FORWARD_MOVEMENT);
    float side = *reinterpret_cast<const float*>(comp + OFF_SIDEWAYS_MOVEMENT);

    // NaN / out-of-range guard — indicates a bad offset
    if (fwd  != fwd  || fwd  >  2.0f || fwd  < -2.0f) return;
    if (side != side || side >  2.0f || side < -2.0f) return;

    std::lock_guard<std::mutex> lock(g_keymutex);
    g_keys.w =  fwd  >  MOVEMENT_THRESHOLD;
    g_keys.s =  fwd  < -MOVEMENT_THRESHOLD;
    g_keys.a =  side < -MOVEMENT_THRESHOLD;
    g_keys.d =  side >  MOVEMENT_THRESHOLD;
    // space: not in the float path, leave it to keyboard hook
}

// ═══════════════════════════════════════════════════════════════════════════════
//  LocalPlayer::normalTick hook
//  Game thread, ~20 Hz. Captures player pointer and reads movement every tick.
// ═══════════════════════════════════════════════════════════════════════════════

typedef void (*NormalTick_t)(void* self);
static NormalTick_t orig_NormalTick = nullptr;

static void hook_NormalTick(void* player) {
    if (player) {
        g_playerAddr = reinterpret_cast<uintptr_t>(player);
        readMoveInputComponent(g_playerAddr);
    }
    if (orig_NormalTick) orig_NormalTick(player);
}

// ── InputConsumer::consume (mouse CPS + physical keyboard fallback) ────────────

static const char* consume_syms[] = {
    // Android 13+ (trailing bool)
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEb",
    // Android 11-12
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
    // Android 10 (nsecs_t = long long)
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPjPPNS_10InputEventE",
    // Android 9 (swapped args)
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPj",
    // Android 9 vendor ROM
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEbxPPNS_10InputEventEPjb",
    nullptr
};

static int g_consume_variant = -1;

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

static void processinput(AInputEvent* event) {
    if (g_initialized) ImGui_ImplAndroid_HandleInputEvent(event);

    int32_t type = AInputEvent_getType(event);
    std::lock_guard<std::mutex> lock(g_keymutex);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        // Mouse buttons → CPS counter only
        // Touch joystick movement is handled by readMoveInputComponent
        int32_t action   = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        int32_t btnstate = AMotionEvent_getButtonState(event);

        bool newlmb = (btnstate & AMOTION_EVENT_BUTTON_PRIMARY)   != 0;
        bool newrmb = (btnstate & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
        if (newlmb && !g_prevlmb) g_lmbcps.click();
        if (newrmb && !g_prevrmb) g_rmbcps.click();
        g_prevlmb = newlmb; g_prevrmb = newrmb;
        g_keys.lmb = newlmb; g_keys.rmb = newrmb;

        // Settings panel touch scroll
        if (g_initialized && g_showsettings) {
            float tx = AMotionEvent_getX(event, 0);
            float ty = AMotionEvent_getY(event, 0);
            ImGuiIO& io = ImGui::GetIO();
            if (action == AMOTION_EVENT_ACTION_DOWN) {
                g_lasttouchy = ty; g_touchdown = true;
                io.MousePos = ImVec2(tx, ty); io.MouseDown[0] = true;
            } else if (action == AMOTION_EVENT_ACTION_MOVE && g_touchdown) {
                float dy = ty - g_lasttouchy; g_lasttouchy = ty;
                io.MousePos = ImVec2(tx, ty); io.MouseWheel += dy * -0.06f;
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
                g_touchdown = false; io.MouseDown[0] = false;
            }
        }

    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        // Physical keyboard / controller override
        int32_t action  = AKeyEvent_getAction(event);
        int32_t keycode = AKeyEvent_getKeyCode(event);
        bool    pressed = (action == AKEY_EVENT_ACTION_DOWN);
        switch (keycode) {
            case AKEYCODE_W:     g_keys.w     = pressed; break;
            case AKEYCODE_A:     g_keys.a     = pressed; break;
            case AKEYCODE_S:     g_keys.s     = pressed; break;
            case AKEYCODE_D:     g_keys.d     = pressed; break;
            case AKEYCODE_SPACE: g_keys.space = pressed; break;
        }
    }
}

// Per-variant trampolines
static int32_t hook_consume_0(void* t,void* a1,bool a2,long a3,uint32_t* a4,AInputEvent** o,bool a6){int32_t r=orig_consume_0?orig_consume_0(t,a1,a2,a3,a4,o,a6):0;if(r==0&&o&&*o)processinput(*o);return r;}
static int32_t hook_consume_1(void* t,void* a1,bool a2,long a3,uint32_t* a4,AInputEvent** o)      {int32_t r=orig_consume_1?orig_consume_1(t,a1,a2,a3,a4,o)  :0;if(r==0&&o&&*o)processinput(*o);return r;}
static int32_t hook_consume_2(void* t,void* a1,bool a2,long long a3,uint32_t* a4,AInputEvent** o) {int32_t r=orig_consume_2?orig_consume_2(t,a1,a2,a3,a4,o)  :0;if(r==0&&o&&*o)processinput(*o);return r;}
static int32_t hook_consume_3(void* t,void* a1,bool a2,long long a3,AInputEvent** o,uint32_t* a4) {int32_t r=orig_consume_3?orig_consume_3(t,a1,a2,a3,o,a4)  :0;if(r==0&&o&&*o)processinput(*o);return r;}
static int32_t hook_consume_4(void* t,void* a1,bool a2,long long a3,AInputEvent** o,uint32_t* a4,bool a6){int32_t r=orig_consume_4?orig_consume_4(t,a1,a2,a3,o,a4,a6):0;if(r==0&&o&&*o)processinput(*o);return r;}

// ── GL state save / restore ───────────────────────────────────────────────────

struct glstate {
    GLint prog,tex,atex,abuf,ebuf,vao,fbo,vp[4],sc[4],bsrc,bdst;
    GLboolean blend,cull,depth,scissor;
};
static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM,&s.prog);glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&s.atex);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&s.ebuf);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s.fbo);glGetIntegerv(GL_VIEWPORT,s.vp);
    glGetIntegerv(GL_SCISSOR_BOX,s.sc);glGetIntegerv(GL_BLEND_SRC_ALPHA,&s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA,&s.bdst);
    s.blend=glIsEnabled(GL_BLEND);s.cull=glIsEnabled(GL_CULL_FACE);
    s.depth=glIsEnabled(GL_DEPTH_TEST);s.scissor=glIsEnabled(GL_SCISSOR_TEST);
}
static void restoregl(const glstate& s) {
    glUseProgram(s.prog);glActiveTexture(s.atex);glBindTexture(GL_TEXTURE_2D,s.tex);
    glBindBuffer(GL_ARRAY_BUFFER,s.abuf);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s.ebuf);
    glBindVertexArray(s.vao);glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);
    glViewport(s.vp[0],s.vp[1],s.vp[2],s.vp[3]);glScissor(s.sc[0],s.sc[1],s.sc[2],s.sc[3]);
    glBlendFunc(s.bsrc,s.bdst);
    s.blend  ?glEnable(GL_BLEND)       :glDisable(GL_BLEND);
    s.cull   ?glEnable(GL_CULL_FACE)   :glDisable(GL_CULL_FACE);
    s.depth  ?glEnable(GL_DEPTH_TEST)  :glDisable(GL_DEPTH_TEST);
    s.scissor?glEnable(GL_SCISSOR_TEST):glDisable(GL_SCISSOR_TEST);
}

// ── Key drawing ───────────────────────────────────────────────────────────────

static void drawkey(const char* label, bool pressed, ImVec2 size) {
    float a = g_opacity;
    ImVec4 col  = pressed ? ImVec4(0.85f,0.85f,0.85f,0.95f*a) : ImVec4(0.18f,0.20f,0.22f,0.88f*a);
    ImVec4 tcol = pressed ? ImVec4(0.05f,0.05f,0.05f,a)       : ImVec4(0.90f,0.90f,0.90f,a);
    ImGui::PushStyleColor(ImGuiCol_Button,col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,col);
    ImGui::PushStyleColor(ImGuiCol_Text,tcol);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void drawkeycps(const char* label, bool pressed, ImVec2 size, int cps) {
    float a = g_opacity;
    ImVec4 col  = pressed ? ImVec4(0.85f,0.85f,0.85f,0.95f*a) : ImVec4(0.18f,0.20f,0.22f,0.88f*a);
    ImVec4 tcol = pressed ? ImVec4(0.05f,0.05f,0.05f,a)       : ImVec4(0.90f,0.90f,0.90f,a);
    ImGui::PushStyleColor(ImGuiCol_Button,col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,col);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,col);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Button("##cpskey", size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* fn = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), sfs = fs * 0.75f;
    ImVec2 lsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, label);
    char buf[16]; snprintf(buf, sizeof(buf), "%d CPS", cps);
    ImVec2 csz = fn->CalcTextSizeA(sfs, FLT_MAX, 0, buf);
    float gap = 3.0f, bh = lsz.y + gap + csz.y;
    float top = pos.y + (size.y - bh) * 0.5f;
    dl->AddText(fn, fs, ImVec2(pos.x+(size.x-lsz.x)*0.5f, top),
                ImGui::ColorConvertFloat4ToU32(tcol), label);
    ImVec4 dim = ImVec4(tcol.x,tcol.y,tcol.z,tcol.w*0.70f);
    dl->AddText(fn, sfs, ImVec2(pos.x+(size.x-csz.x)*0.5f, top+lsz.y+gap),
                ImGui::ColorConvertFloat4ToU32(dim), buf);
    ImGui::PopStyleColor(3);
}

// ── Settings panel ────────────────────────────────────────────────────────────

static void drawsettings(ImVec2 hudpos) {
    float sw = std::max(g_width  * 0.26f, 220.0f);
    float sh = std::max(g_height * 0.65f, 380.0f);
    float ks = g_keysize, sp = ks * 0.04f;
    float px = hudpos.x + ks*3 + sp*2 + 8.0f, py = hudpos.y;
    if (px + sw > g_width)  px = hudpos.x - sw - 8.0f;
    if (py + sh > g_height) py = g_height - sh - 8.0f;
    if (px < 0) px = 8.0f; if (py < 0) py = 8.0f;

    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,             ImVec4(0.10f,0.12f,0.16f,0.97f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,              ImVec4(0.16f,0.20f,0.28f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,       ImVec4(0.22f,0.28f,0.38f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,           ImVec4(0.35f,0.65f,1.00f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,     ImVec4(0.50f,0.80f,1.00f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,            ImVec4(0.35f,0.65f,1.00f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_Separator,            ImVec4(0.25f,0.32f,0.45f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          ImVec4(0.08f,0.10f,0.14f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        ImVec4(0.30f,0.50f,0.80f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.40f,0.60f,0.90f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  ImVec4(0.50f,0.75f,1.00f,1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(12.0f,12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(6.0f,11.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.0f,4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,  6.0f);

    ImGui::Begin("##cfg", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);

    float cw = sw - 24.0f;
    ImGui::TextColored(ImVec4(0.85f,0.92f,1.00f,1.0f), "KEYSTROKES  v" VERSION);
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── Input source status ───────────────────────────────────────────────────
    if (g_playerAddr != 0) {
        ImGui::TextColored(ImVec4(0.40f,0.80f,0.40f,1.0f),
            "NormalTick OK  (0x%lX)", (unsigned long)g_playerAddr);

        // Live readout of raw component values — great for finding the right offset
        uintptr_t comp = g_playerAddr + MOVE_INPUT_COMP_OFFSET;
        float fwd  = *reinterpret_cast<const float*>(comp + OFF_FORWARD_MOVEMENT);
        float side = *reinterpret_cast<const float*>(comp + OFF_SIDEWAYS_MOVEMENT);
        uint16_t fl = *reinterpret_cast<const uint16_t*>(comp + OFF_FLAG_VALUES);
        ImGui::TextColored(ImVec4(0.60f,0.85f,0.60f,1.0f),
            "fwd=%.2f side=%.2f flags=%04X", fwd, side, fl);
    } else {
        ImGui::TextColored(ImVec4(1.00f,0.35f,0.35f,1.0f), "NormalTick: waiting...");
    }

    if (g_consume_variant >= 0)
        ImGui::TextColored(ImVec4(0.40f,0.80f,0.40f,1.0f),
            "InputConsumer: variant %d", g_consume_variant);
    else
        ImGui::TextColored(ImVec4(0.80f,0.65f,0.20f,1.0f), "InputConsumer: not hooked");

    ImGui::TextColored(ImVec4(0.50f,0.65f,0.85f,1.0f),
        "MoveInput offset: 0x%X", (unsigned)MOVE_INPUT_COMP_OFFSET);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    // ── Size ──────────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.50f,0.75f,1.00f,1.0f), "KEY SIZE");
    ImGui::TextColored(ImVec4(0.90f,0.93f,1.00f,1.0f), "%.0f dp", g_keysize);
    ImGui::SetNextItemWidth(cw);
    if (ImGui::SliderFloat("##sz", &g_keysize, 30.0f, 120.0f, "")) savecfg();
    ImGui::Spacing();

    // ── Opacity ───────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.50f,0.75f,1.00f,1.0f), "OPACITY");
    float op = g_opacity * 100.0f;
    ImGui::TextColored(ImVec4(0.90f,0.93f,1.00f,1.0f), "%.0f%%", op);
    ImGui::SetNextItemWidth(cw);
    if (ImGui::SliderFloat("##op", &op, 10.0f, 100.0f, "")) { g_opacity = op/100.0f; savecfg(); }
    ImGui::Spacing();

    // ── Rounding ──────────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.50f,0.75f,1.00f,1.0f), "CORNER RADIUS");
    ImGui::TextColored(ImVec4(0.90f,0.93f,1.00f,1.0f), "%.0f dp", g_rounding);
    ImGui::SetNextItemWidth(cw);
    if (ImGui::SliderFloat("##rnd", &g_rounding, 0.0f, 50.0f, "")) savecfg();

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.50f,0.75f,1.00f,1.0f), "LOCK POSITION");
    bool lk = g_locked;
    if (ImGui::Checkbox("##lk", &lk)) { g_locked = lk; savecfg(); }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f,0.62f,0.75f,1.0f), "Prevent drag");

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f,0.30f,0.55f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f,0.42f,0.72f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.35f,0.55f,0.90f,1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.88f,0.93f,1.00f,1.00f));
    if (ImGui::Button("Reset to Default", ImVec2(cw, 0))) {
        g_keysize=50.0f; g_opacity=1.0f; g_rounding=8.0f;
        g_locked=false; g_hudpos=ImVec2(100,100); savecfg();
    }
    ImGui::PopStyleColor(4);

    float rem = ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing()*2.5f;
    if (rem > 0) ImGui::Dummy(ImVec2(0, rem));
    ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f,0.50f,0.65f,1.0f), "Made by");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.50f,0.75f,1.00f,1.0f), "ZhyKun");

    ImGui::End();
    ImGui::PopStyleVar(5);
    ImGui::PopStyleColor(11);

    ImGuiIO& io = ImGui::GetIO();
    bool outside = ImGui::IsMouseClicked(0) &&
        (io.MousePos.x < px || io.MousePos.x > px+sw ||
         io.MousePos.y < py || io.MousePos.y > py+sh);
    if (outside) g_showsettings = false;
}

// ── Main HUD ──────────────────────────────────────────────────────────────────

static void drawmenu() {
    KeyState k;
    { std::lock_guard<std::mutex> lock(g_keymutex); k = g_keys; }

    int lc = g_lmbcps.get(), rc = g_rmbcps.get();
    float ks = g_keysize, sp = ks * 0.04f;
    float hudW = ks*3 + sp*2;
    float hudH = ks*3 + sp*2 + ks*1.5f + sp + ks*0.7f + sp;

    ImGuiIO& io = ImGui::GetIO();
    bool inside = (io.MousePos.x >= g_hudpos.x && io.MousePos.x <= g_hudpos.x + hudW &&
                   io.MousePos.y >= g_hudpos.y && io.MousePos.y <= g_hudpos.y + hudH);

    if (inside && io.MouseDown[0] && !g_pressing && !g_showsettings) {
        g_pressing = true; g_pressstart = nowsec();
    }
    if (!io.MouseDown[0]) g_pressing = false;
    if (g_pressing && (nowsec() - g_pressstart) >= LONGPRESS_SEC) {
        g_showsettings = true; g_pressing = false;
    }

    if (!g_locked && !g_showsettings && inside && ImGui::IsMouseDragging(0)) {
        g_hudpos.x += io.MouseDelta.x; g_hudpos.y += io.MouseDelta.y;
        g_hudpos.x = std::max(0.0f, std::min(g_hudpos.x, (float)g_width  - hudW));
        g_hudpos.y = std::max(0.0f, std::min(g_hudpos.y, (float)g_height - hudH));
        savecfg();
    }

    if (g_showsettings) drawsettings(g_hudpos);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::SetNextWindowPos(g_hudpos, ImGuiCond_Always);
    ImGui::Begin("##ks", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoBackground|
        ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoInputs);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(sp, sp));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, g_rounding);

    ImGui::SetCursorPosX(ks + sp);
    drawkey("W", k.w, ImVec2(ks, ks));
    drawkey("A", k.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", k.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", k.d, ImVec2(ks, ks));

    float half = (hudW - sp) / 2.0f;
    drawkeycps("LMB", k.lmb, ImVec2(half, ks*1.5f), lc); ImGui::SameLine();
    drawkeycps("RMB", k.rmb, ImVec2(half, ks*1.5f), rc);
    drawkey("_____", k.space, ImVec2(hudW, ks*0.7f));

    ImGui::PopStyleVar(3);
    ImGui::End();
}

// ── ImGui init ────────────────────────────────────────────────────────────────

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;
    loadcfg();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    int mn = std::min(g_width, g_height), mx = std::max(g_width, g_height);
    float dp = std::max(0.85f, std::min((float)mn / 480.0f, 2.8f));
    if (mx > 2400) dp = std::min(dp * 1.15f, 2.8f);
    g_uiscale = dp;

    ImFontConfig cfg; cfg.SizePixels = std::max(14.0f, 15.0f * dp);
    io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dp);
    style.WindowBorderSize = 0.0f;

    float ks = g_keysize, sp = ks * 0.04f;
    g_hudpos.x = std::max(0.0f, std::min(g_hudpos.x, (float)g_width  - ks*3 - sp*2));
    g_hudpos.y = std::max(0.0f, std::min(g_hudpos.y, (float)g_height - ks*4 - sp*3));

    g_initialized = true;
    LOGI("ImGui ready. Screen=%dx%d dp=%.2f", g_width, g_height, dp);
}

static void render() {
    if (!g_initialized) return;
    glstate s; savegl(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    drawmenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    restoregl(s);
}

// ── eglSwapBuffers hook ───────────────────────────────────────────────────────

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);
    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    if (ctx == g_targetcontext && surf == g_targetsurface) {
        g_width = w; g_height = h; setup(); render();
    }
    return orig_eglswapbuffers(dpy, surf);
}

// ── Main thread: install hooks ────────────────────────────────────────────────

static void* mainthread(void*) {
    sleep(5);
    GlossInit(true);

    // EGL
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) {
        GHandle hgles = GlossOpen("libGLESv2.so");
        swap = (void*)GlossSymbol(hgles, "eglSwapBuffers", nullptr);
    }
    if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);

    // LocalPlayer::normalTick  →  captures player + reads MoveInputComponent
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        void* tick = (void*)GlossSymbol(hmc, "_ZN11LocalPlayer10normalTickEv", nullptr);
        if (tick) {
            GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick);
            LOGI("NormalTick hooked OK");
        } else {
            LOGW("NormalTick symbol not found — touch WASD will not light up");
        }
    }

    // InputConsumer::consume  →  mouse CPS + physical keyboard fallback
    GHandle hlib = GlossOpen("libinput.so");
    void* symconsume = nullptr;
    for (int i = 0; consume_syms[i]; i++) {
        symconsume = (void*)GlossSymbol(hlib, consume_syms[i], nullptr);
        if (symconsume) { g_consume_variant = i; LOGI("consume: variant %d OK", i); break; }
        LOGI("consume: variant %d not found", i);
    }
    if (!symconsume) LOGW("consume: no variant matched — LMB/RMB CPS disabled");
    else switch (g_consume_variant) {
        case 0: GlossHook(symconsume,(void*)hook_consume_0,(void**)&orig_consume_0); break;
        case 1: GlossHook(symconsume,(void*)hook_consume_1,(void**)&orig_consume_1); break;
        case 2: GlossHook(symconsume,(void*)hook_consume_2,(void**)&orig_consume_2); break;
        case 3: GlossHook(symconsume,(void*)hook_consume_3,(void**)&orig_consume_3); break;
        case 4: GlossHook(symconsume,(void*)hook_consume_4,(void**)&orig_consume_4); break;
    }

    return nullptr;
}

__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
