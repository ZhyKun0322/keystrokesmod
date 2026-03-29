#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#include <mutex>

// ─── Key state ───────────────────────────────────────────────────────────────

struct KeyState {
    bool w = false, a = false, s = false, d = false;
    bool space = false, lmb = false, rmb = false;
};

static KeyState g_keys;
static std::mutex g_keymutex;

// Android keycodes
#define AKEYCODE_W     51
#define AKEYCODE_A     29
#define AKEYCODE_S     47
#define AKEYCODE_D     32
#define AKEYCODE_SPACE 62

// ─── EGL / ImGui state ───────────────────────────────────────────────────────

static bool       g_initialized    = false;
static int        g_width = 0,  g_height = 0;
static EGLContext g_targetcontext  = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface  = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;
static void       (*orig_input1)(void*, void*, void*)            = nullptr;
static int32_t    (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ─── Input hooks ─────────────────────────────────────────────────────────────

static void handleevent(AInputEvent* event) {
    if (!event) return;

    int32_t type = AInputEvent_getType(event);

    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        int32_t btnstate = AMotionEvent_getButtonState(event);
        std::lock_guard<std::mutex> lock(g_keymutex);
        g_keys.lmb = (btnstate & AMOTION_EVENT_BUTTON_PRIMARY)   != 0;
        g_keys.rmb = (btnstate & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
    }

    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t keycode = AKeyEvent_getKeyCode(event);
        int32_t action  = AKeyEvent_getAction(event);
        bool pressed    = (action == AKEY_EVENT_ACTION_DOWN);
        std::lock_guard<std::mutex> lock(g_keymutex);
        switch (keycode) {
            case AKEYCODE_W:     g_keys.w     = pressed; break;
            case AKEYCODE_A:     g_keys.a     = pressed; break;
            case AKEYCODE_S:     g_keys.s     = pressed; break;
            case AKEYCODE_D:     g_keys.d     = pressed; break;
            case AKEYCODE_SPACE: g_keys.space = pressed; break;
        }
    }
}

static void hook_input1(void* thiz, void* a1, void* a2) {
    if (orig_input1) orig_input1(thiz, a1, a2);
    if (thiz && g_initialized) {
        AInputEvent* event = (AInputEvent*)thiz;
        ImGui_ImplAndroid_HandleInputEvent(event);
        handleevent(event);
    }
}

static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
        handleevent(*event);
    }
    return result;
}

// ─── GL state save/restore ───────────────────────────────────────────────────

struct glstate {
    GLint prog, tex, atex, abuf, ebuf, vao, fbo, vp[4], sc[4], bsrc, bdst;
    GLboolean blend, cull, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM,               &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,            &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE,                &s.atex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,          &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,  &s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,          &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,           &s.fbo);
    glGetIntegerv(GL_VIEWPORT,                       s.vp);
    glGetIntegerv(GL_SCISSOR_BOX,                    s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,               &s.bsrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA,               &s.bdst);
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
    s.blend   ? glEnable(GL_BLEND)       : glDisable(GL_BLEND);
    s.cull    ? glEnable(GL_CULL_FACE)   : glDisable(GL_CULL_FACE);
    s.depth   ? glEnable(GL_DEPTH_TEST)  : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST): glDisable(GL_SCISSOR_TEST);
}

// ─── UI drawing ──────────────────────────────────────────────────────────────

static void drawkey(const char* label, bool pressed) {
    ImVec4 color = pressed
        ? ImVec4(1.0f, 1.0f, 1.0f, 0.95f)   // bright white when pressed
        : ImVec4(0.2f, 0.2f, 0.2f, 0.75f);   // dark when released
    ImVec4 textcolor = pressed
        ? ImVec4(0.0f, 0.0f, 0.0f, 1.0f)
        : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);

    ImVec2 size(60, 60);
    ImGui::PushStyleColor(ImGuiCol_Button,        color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color);
    ImGui::PushStyleColor(ImGuiCol_Text,          textcolor);
    ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
}

static void drawspacer(float width) {
    ImGui::InvisibleButton("##spacer", ImVec2(width, 60));
}

static void drawmenu() {
    KeyState k;
    {
        std::lock_guard<std::mutex> lock(g_keymutex);
        k = g_keys;
    }

    ImGui::SetNextWindowPos(ImVec2(10, 90), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##keystrokes", nullptr,
        ImGuiWindowFlags_NoTitleBar    |
        ImGuiWindowFlags_NoResize      |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoScrollbar   |
        ImGuiWindowFlags_NoBackground  |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);

    // Row 1: [W] centered over ASD
    drawspacer(66);
    ImGui::SameLine();
    drawkey("W", k.w);

    // Row 2: [A] [S] [D]
    drawkey("A", k.a);
    ImGui::SameLine();
    drawkey("S", k.s);
    ImGui::SameLine();
    drawkey("D", k.d);

    // Row 3: [  SPACE  ]
    drawkey("     SPACE     ", k.space);

    // Row 4: [LMB] [RMB]
    drawkey("LMB", k.lmb);
    ImGui::SameLine();
    drawkey("RMB", k.rmb);

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// ─── ImGui setup & render ────────────────────────────────────────────────────

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    float scale = (float)g_height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;

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

    glstate s;
    savegl(s);

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

// ─── EGL swap hook ───────────────────────────────────────────────────────────

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH,  &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_targetcontext = ctx;
            g_targetsurface = surf;
        }
    }

    if (ctx != g_targetcontext || surf != g_targetsurface)
        return orig_eglswapbuffers(dpy, surf);

    g_width  = w;
    g_height = h;
    setup();
    render();

    return orig_eglswapbuffers(dpy, surf);
}

// ─── Hook init ───────────────────────────────────────────────────────────────

static void hookinput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_input1, (void**)&orig_input1);
        if (h) return;
    }

    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_input2, (void**)&orig_input2);
        if (h) return;
    }
}

static void* mainthread(void*) {
    sleep(3);

    GlossInit(true);

    GHandle hegl = GlossOpen("libEGL.so");
    if (!hegl) return nullptr;

    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;

    GHook h = GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
    if (!h) return nullptr;

    hookinput();
    return nullptr;
}

__attribute__((constructor))
void keystrokes_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
