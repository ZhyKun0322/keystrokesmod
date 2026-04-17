#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <mutex>

#include "pl/Gloss.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_android.h"

#define LOG_TAG "MobileKeystrokes"

// --- TACO'S STRUCT ---
struct Vec2 { float x, y; };
struct MoveInputComponent {
    char filler0[0x24];
    Vec2 mMove;
};

struct KeyState { bool w=0, a=0, s=0, d=0; };
static KeyState g_keys;
static std::mutex g_keymutex;
static bool g_initialized = false;
static uintptr_t g_playerAddr = 0; // For debugging

// --- THE BRAIN (With Safety Shield) ---
typedef void (*NormalTick)(void* self);
static NormalTick orig_NormalTick = nullptr;

void hook_NormalTick(void* player) {
    if (player) {
        g_playerAddr = (uintptr_t)player; // Save address to show on screen

        // SAFETY SHIELD: We check if the memory address is valid before reading
        // Offset 0x10A8 is our guess. We will check it carefully.
        uintptr_t mic_ptr_addr = (uintptr_t)player + 0x10A8; 
        
        // Basic pointer validation (must be in a high memory range)
        if (mic_ptr_addr > 0x1000000) { 
            MoveInputComponent** mic_pp = (MoveInputComponent**)mic_ptr_addr;
            
            if (mic_pp && *mic_pp) {
                MoveInputComponent* mic = *mic_pp;
                
                // Final safety check before reading X/Y
                if ((uintptr_t)mic > 0x1000000) {
                    std::lock_guard<std::mutex> lock(g_keymutex);
                    g_keys.a = (mic->mMove.x < -0.1f);
                    g_keys.d = (mic->mMove.x > 0.1f);
                    g_keys.w = (mic->mMove.y > 0.1f);
                    g_keys.s = (mic->mMove.y < -0.1f);
                }
            }
        }
    }
    if (orig_NormalTick) orig_NormalTick(player);
}

// --- DEBUG UI ---
void render_hud() {
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(250, 200), ImGuiCond_Always);
    ImGui::Begin("##HUD", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
    
    // Debug Info: Shows your Player Address on screen
    ImGui::Text("Player: 0x%lX", g_playerAddr);

    float ks = 60.0f;
    ImGui::SetCursorPosX(75);
    auto drawkey = [](const char* lbl, bool act, float size) {
        ImVec4 col = act ? ImVec4(1,1,1,0.8f) : ImVec4(0,0,0,0.4f);
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::Button(lbl, ImVec2(size, size));
        ImGui::PopStyleColor();
    };

    drawkey("W", g_keys.w, ks);
    ImGui::SetCursorPosX(10);
    drawkey("A", g_keys.a, ks); ImGui::SameLine();
    drawkey("S", g_keys.s, ks); ImGui::SameLine();
    drawkey("D", g_keys.d, ks);
    
    ImGui::End();
}

// --- STANDARD HOOKS ---
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    if (!g_initialized) { ImGui::CreateContext(); g_initialized = true; }
    ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplAndroid_NewFrame(w, h);
    ImGui::NewFrame();
    render_hud();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return orig_eglSwapBuffers(dpy, surf);
}

void* main_thread(void*) {
    sleep(15);
    GlossInit(true);
    GHandle hegl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    GHandle hmc = GlossOpen("libminecraftpe.so");
    if (hmc) {
        void* tick = (void*)GlossSymbol(hmc, "_ZN11LocalPlayer10normalTickEv", nullptr);
        if (tick) GlossHook(tick, (void*)hook_NormalTick, (void**)&orig_NormalTick);
    }
    return nullptr;
}
__attribute__((constructor)) void init() { pthread_t t; pthread_create(&t, nullptr, main_thread, nullptr); }
