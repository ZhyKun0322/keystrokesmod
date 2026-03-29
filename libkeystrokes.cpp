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

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

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
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

// ─── Settings ────────────────────────────────────────────────────────────────
static float g_keysize    = 60.0f;
static float g_opacity    = 1.0f;
static bool  g_locked     = false;
static bool  g_showsettings = false;

// ─── Long press detection ─────────────────────────────────────────────────────
static bool  g_pressing        = false;
static double g_pressstart     = 0.0;
static const double LONGPRESS_SEC = 0.6;

static double nowsec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;
static int32_t (*orig_consume)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static int32_t hook_consume(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** outEvent) {
    int32_t result = orig_consume ? orig_consume(thiz, a1, a2, a3, a4, outEvent) : 0;

    if (result == 0 && outEvent && *outEvent) {
        AInputEvent* event = *outEvent;

        if (g_initialized)
            ImGui_ImplAndroid_HandleInputEvent(event);

        int32_t type = AInputEvent_getType(event);
        std::lock_guard<std::mutex> lock(g_keymutex);

        if (type == AINPUT_EVENT_TYPE_MOTION) {
            int32_t btnstate = AMotionEvent_getButtonState(event);
            g_keys.lmb = (btnstate & AMOTION_EVENT_BUTTON_PRIMARY)   != 0;
            g_keys.rmb = (btnstate & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
        }
        else if (type == AINPUT_EVENT_TYPE_KEY) {
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
    return result;
}

// ─── GL save/restore ─────────────────────────────────────────────────────────
struct glstate {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM,              &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,           &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE,               &s.atex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,         &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,         &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,          &s.fbo);
    glGetIntegerv(GL_VIEWPORT,                      s.vp);
    glGetIntegerv(GL_SCISSOR_BOX,                   s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,              &s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA,              &s.bdst);
    s.blend   = glIsEnabled(GL_BLEND);
    s.cull    = glIsEnabled(GL_CULL_FACE);
    s.depth   = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void restoregl(const glstate& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.atex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bsrc, s.bdst);
    s.blend   ? glEnable(GL_BLEND)        : glDisable(GL_BLEND);
    s.cull    ? glEnable(GL_CULL_FACE)    : glDisable(GL_CULL_FACE);
    s.depth   ? glEnable(GL_DEPTH_TEST)   : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

// ─── UI ──────────────────────────────────────────────────────────────────────

static void drawkey(const char* label, bool pressed, ImVec2 size) {
    float a = g_opacity;
    ImVec4 color     = pressed ? ImVec4(1.0f, 1.0f, 1.0f, 0.95f * a) : ImVec4(0.2f, 0.2f, 0.2f, 0.75f * a);
    ImVec4 textcolor = pressed ? ImVec4(0.0f, 0.0f, 0.0f, a)         : ImVec4(0.9f, 0.9f, 0.9f, a);
    ImGui::PushStyleColor(ImGuiCol_Button,        color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color);
    ImGui::PushStyleColor(ImGuiCol_Text,          textcolor);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void drawsettings(ImVec2 hudpos) {
    ImGui::SetNextWindowPos(ImVec2(hudpos.x, hudpos.y - 180), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260, 170), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);

    ImGui::Begin("##settings", nullptr,
        ImGuiWindowFlags_NoTitleBar    |
        ImGuiWindowFlags_NoResize      |
        ImGuiWindowFlags_NoMove        |
        ImGuiWindowFlags_NoScrollbar);

    // Size slider: 30–120 dp
    int sizeidp = (int)g_keysize;
    ImGui::Text("Size: %ddp", sizeidp);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##size", &sizeidp, 30, 120))
        g_keysize = (float)sizeidp;

    // Opacity slider: 0–100%
    int opacitypct = (int)(g_opacity * 100.0f);
    ImGui::Text("Opacity: %d%%", opacitypct);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##opacity", &opacitypct, 10, 100))
        g_opacity = opacitypct / 100.0f;

    // Lock position toggle
    ImGui::Text("Lock Position");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40);
    ImGui::Checkbox("##lock", &g_locked);
    ImGui::TextDisabled("Prevent dragging and accidental activation");

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

static void drawmenu() {
    KeyState k;
    {
        std::lock_guard<std::mutex> lock(g_keymutex);
        k = g_keys;
    }

    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar    |
                             ImGuiWindowFlags_NoBackground  |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoScrollbar;
    if (g_locked) flags |= ImGuiWindowFlags_NoMove;

    ImGui::Begin("Keystrokes HUD", nullptr, flags);

    ImVec2 winpos = ImGui::GetWindowPos();

    // ── Long press detection on the HUD window ──
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGuiIO& io  = ImGui::GetIO();
    bool mousedown = io.MouseDown[0];

    if (hovered && mousedown && !g_pressing) {
        g_pressing   = true;
        g_pressstart = nowsec();
    }
    if (!mousedown) {
        g_pressing = false;
    }
    if (g_pressing && (nowsec() - g_pressstart) >= LONGPRESS_SEC) {
        g_showsettings = !g_showsettings;
        g_pressing     = false;
    }

    // ── Drag ──
    if (!g_locked && hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 delta = io.MouseDelta;
        ImGui::SetWindowPos(ImVec2(winpos.x + delta.x, winpos.y + delta.y));
        winpos = ImGui::GetWindowPos();
    }

    float ks      = g_keysize;
    float spacing = ks * 0.1f;
    float rowwidth = ks * 3 + spacing * 2;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(spacing, spacing));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    // Row 1: W centered
    ImGui::SetCursorPosX(ks + spacing * 1.5f);
    drawkey("W", k.w, ImVec2(ks, ks));

    // Row 2: A S D
    drawkey("A", k.a, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("S", k.s, ImVec2(ks, ks)); ImGui::SameLine();
    drawkey("D", k.d, ImVec2(ks, ks));

    // Row 3: SPACE
    drawkey("SPACE", k.space, ImVec2(rowwidth, ks));

    // Row 4: LMB RMB
    float halfrow = (rowwidth - spacing) / 2.0f;
    drawkey("LMB", k.lmb, ImVec2(halfrow, ks)); ImGui::SameLine();
    drawkey("RMB", k.rmb, ImVec2(halfrow, ks));

    ImGui::PopStyleVar(2);
    ImGui::End();

    // ── Settings popup ──
    if (g_showsettings)
        drawsettings(winpos);
}

// ─── ImGui init & render ─────────────────────────────────────────────────────

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = (float)g_height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    ImFontConfig cfg;
    cfg.SizePixels = 28.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale);
    g_initialized = true;
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

// ─── EGL hook ────────────────────────────────────────────────────────────────

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);
    if (g_targetcontext == EGL_NO_CONTEXT) {
        g_targetcontext = ctx;
        g_targetsurface = surf;
    }
    if (ctx == g_targetcontext && surf == g_targetsurface) {
        g_width = w; g_height = h;
        setup(); render();
    }
    return orig_eglswapbuffers(dpy, surf);
}

// ─── Init ────────────────────────────────────────────────────────────────────

static void* mainthread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
    GHandle hlib = GlossOpen("libinput.so");
    void* symconsume = (void*)GlossSymbol(hlib,
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (symconsume) GlossHook(symconsume, (void*)hook_consume, (void**)&orig_consume);
    return nullptr;
}

__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
