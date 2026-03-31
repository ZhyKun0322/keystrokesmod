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
#include <algorithm>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define VERSION "1.0.7"

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
static bool  g_locked       = false;
static bool  g_showsettings = false;
static ImVec2 g_hudpos      = ImVec2(100, 100);
static bool  g_posloaded    = false;

static const char* SAVE_PATH = "/data/data/com.mojang.minecraftpe/files/keystrokes.cfg";

static bool g_usenewconsume = false;

static const char* consume_syms[] = {
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEb",
    "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
    nullptr
};

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;
static int32_t (*orig_consume)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t (*orig_consume_new)(void*, void*, bool, long, uint32_t*, AInputEvent**, bool) = nullptr;

static void savecfg() {
    FILE* f = fopen(SAVE_PATH, "w");
    if (!f) return;
    fprintf(f, "%f %f %f %f %d\n", g_hudpos.x, g_hudpos.y, g_keysize, g_opacity, (int)g_locked);
    fclose(f);
}

static void loadcfg() {
    if (g_posloaded) return;
    g_posloaded = true;
    FILE* f = fopen(SAVE_PATH, "r");
    if (!f) return;
    int locked = 0;
    fscanf(f, "%f %f %f %f %d", &g_hudpos.x, &g_hudpos.y, &g_keysize, &g_opacity, &locked);
    g_locked = (locked != 0);
    fclose(f);
}

static double nowsec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static bool   g_pressing   = false;
static double g_pressstart = 0.0;
static const double LONGPRESS_SEC = 0.5;

static void processinput(AInputEvent* event) {
    if (g_initialized) ImGui_ImplAndroid_HandleInputEvent(event);
    int32_t type = AInputEvent_getType(event);

    std::lock_guard<std::mutex> lock(g_keymutex);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t btnstate = AMotionEvent_getButtonState(event);
        g_keys.lmb = (btnstate & AMOTION_EVENT_BUTTON_PRIMARY)   != 0;
        g_keys.rmb = (btnstate & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
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

static int32_t hook_consume(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent) {
    int32_t result = orig_consume ? orig_consume(thiz, a1, a2, a3, a4, outEvent) : 0;
    if (result == 0 && outEvent && *outEvent)
        processinput(*outEvent);
    return result;
}

static int32_t hook_consume_new(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent, bool a6) {
    int32_t result = orig_consume_new ? orig_consume_new(thiz, a1, a2, a3, a4, outEvent, a6) : 0;
    if (result == 0 && outEvent && *outEvent)
        processinput(*outEvent);
    return result;
}

struct glstate {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog); glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.atex); glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.abuf);
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
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void drawkey(const char* label, bool pressed, ImVec2 size) {
    float a = g_opacity;
    ImVec4 color     = pressed ? ImVec4(1.0f, 1.0f, 1.0f, 0.95f*a) : ImVec4(0.15f, 0.15f, 0.15f, 0.7f*a);
    ImVec4 textcolor = pressed ? ImVec4(0.0f, 0.0f, 0.0f, a)       : ImVec4(1.0f,  1.0f,  1.0f,  a);
    ImGui::PushStyleColor(ImGuiCol_Button,        color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color);
    ImGui::PushStyleColor(ImGuiCol_Text,          textcolor);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void drawsettings(ImVec2 hudpos) {
    float sw = g_width  * 0.22f;
    float sh = g_height * 0.38f;
    sw = std::max(sw, 200.0f);
    sh = std::max(sh, 180.0f);

    float ks      = g_keysize;
    float spacing = ks * 0.12f;
    float hudW    = ks * 3 + spacing * 2;

    float px = hudpos.x + hudW + 8.0f;
    float py = hudpos.y;
    if (px + sw > g_width)  px = hudpos.x - sw - 8.0f;
    if (py + sh > g_height) py = g_height - sh - 8.0f;
    if (px < 0) px = 8.0f;
    if (py < 0) py = 8.0f;

    ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(sw, sh), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg,         ImVec4(0.10f, 0.10f, 0.10f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.20f, 0.20f, 0.20f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   ImVec4(0.28f, 0.28f, 0.28f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.30f, 0.80f, 0.50f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.35f, 1.00f, 0.60f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,        ImVec4(0.30f, 0.80f, 0.50f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(10.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(6.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,   ImVec2(6.0f, 4.0f));

    ImGui::Begin("##cfg", nullptr,
        ImGuiWindowFlags_NoTitleBar  |
        ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoMove);

    float labelw = sw * 0.52f;
    float ctrlw  = sw - labelw - 20.0f;

    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "KEYSTROKES");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Size: %.0fdp", g_keysize);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##sz", &g_keysize, 30.0f, 120.0f, "")) savecfg();

    ImGui::Spacing();

    float op = g_opacity * 100.0f;
    ImGui::Text("Opacity: %.0f%%", op);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##op", &op, 10.0f, 100.0f, "")) {
        g_opacity = op / 100.0f;
        savecfg();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Columns(2, "##lkcol", false);
    ImGui::SetColumnWidth(0, labelw);
    ImGui::Text("Lock Position");
    ImGui::NextColumn();
    ImGui::SetNextItemWidth(ctrlw);
    if (ImGui::Checkbox("##lk", &g_locked)) savecfg();
    ImGui::Columns(1);

    ImGui::Spacing();
    ImGui::TextDisabled("Prevent dragging and");
    ImGui::TextDisabled("accidental activation");

    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(6);

    ImGuiIO& io = ImGui::GetIO();
    bool outsideclick = ImGui::IsMouseClicked(0) &&
        (io.MousePos.x < px || io.MousePos.x > px + sw ||
         io.MousePos.y < py || io.MousePos.y > py + sh);
    if (outsideclick) g_showsettings = false;
}

static void drawmenu() {
    KeyState k;
    { std::lock_guard<std::mutex> lock(g_keymutex); k = g_keys; }

    float ks      = g_keysize;
    float spacing = ks * 0.12f;
    float hudW    = ks * 3 + spacing * 2;
    float hudH    = ks * 4 + spacing * 3;

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
        savecfg();
    }

    if (g_showsettings) drawsettings(g_hudpos);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowPos(g_hudpos, ImGuiCond_Always);
    ImGui::Begin("##ks", nullptr,
        ImGuiWindowFlags_NoTitleBar      |
        ImGuiWindowFlags_NoBackground    |
        ImGuiWindowFlags_AlwaysAutoResize|
        ImGuiWindowFlags_NoMove          |
        ImGuiWindowFlags_NoInputs);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    ImGui::SetCursorPosX(ks + spacing);
    drawkey("W", k.w, ImVec2(ks, ks));

    drawkey("A", k.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", k.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", k.d, ImVec2(ks, ks));
    drawkey("SPACE", k.space, ImVec2(hudW, ks * 0.7f));

    float half = (hudW - spacing) / 2.0f;
    drawkey("LMB", k.lmb, ImVec2(half, ks * 0.7f)); ImGui::SameLine();
    drawkey("RMB", k.rmb, ImVec2(half, ks * 0.7f));

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

    float dpscale = (float)minside / 480.0f;
    dpscale = std::max(0.8f, std::min(dpscale, 2.5f));
    g_uiscale = dpscale;

    float fontsize = std::max(13.0f, 14.0f * dpscale);

    ImFontConfig cfg;
    cfg.SizePixels = fontsize;
    io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpscale);
    style.WindowBorderSize = 0.0f;

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

    GHandle hegl = GlossOpen("libEGL.so");
    void* swap   = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) {
        GHandle hgles = GlossOpen("libGLESv2.so");
        swap = (void*)GlossSymbol(hgles, "eglSwapBuffers", nullptr);
    }
    if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);

    GHandle hlib     = GlossOpen("libinput.so");
    void* symconsume = nullptr;
    for (int i = 0; consume_syms[i]; i++) {
        symconsume = (void*)GlossSymbol(hlib, consume_syms[i], nullptr);
        if (symconsume) {
            g_usenewconsume = (i == 0);
            break;
        }
    }

    if (symconsume) {
        if (g_usenewconsume)
            GlossHook(symconsume, (void*)hook_consume_new, (void**)&orig_consume_new);
        else
            GlossHook(symconsume, (void*)hook_consume,     (void**)&orig_consume);
    }

    return nullptr;
}

__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
