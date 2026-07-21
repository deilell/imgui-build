// 编译参数必须加：
// -DIMGUI_IMPL_OPENGL_ES2 -DIMGUI_IMPL_OPENGL_NO_VAO -DIMGUI_IMPL_OPENGL_NO_SAMPLERS
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>
#include <cstdint>
#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <errno.h>

// ===================== Game Offsets =====================
const std::string LIB_NAME = "libunity.so";
constexpr uintptr_t BASE_BSS = 0x11110;
constexpr int OFF_ENTITY_LIST[] = {0x20, 0x28};
constexpr int OFF_ENTITY_LIST_CNT = sizeof(OFF_ENTITY_LIST) / sizeof(int);
constexpr int OFF_ENTITY_POS     = 0x1C;
constexpr int OFF_ENTITY_HEAD    = 0x40;
constexpr int OFF_ENTITY_ISPLAYER= 0x188;
constexpr int OFF_ENTITY_ISGHOST = 0x18A;
constexpr int OFF_ENTITY_ITEM    = 0x190;
constexpr int OFF_ENTITY_ANOMALY = 0x192;

// ESP Toggle Switches
bool g_ESP_Player   = false;
bool g_ESP_Ghost    = false;
bool g_ESP_Item     = false;
bool g_ESP_Anomaly  = false;

int g_ScrW = 800;
int g_ScrH = 600;
pid_t g_GamePid = 0;

// 3D Vector Struct
struct Vec3
{
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
};

// ===================== Memory Read Functions =====================
uintptr_t GetModuleBase(pid_t pid, const char* modName)
{
    char buf[1024];
    std::string path = "/proc/" + std::to_string(pid) + "/maps";
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return 0;
    uintptr_t base = 0;
    while (fgets(buf, sizeof(buf), fp))
    {
        if (strstr(buf, modName))
        {
            sscanf(buf, "%lx", &base);
            break;
        }
    }
    fclose(fp);
    return base;
}

uintptr_t ResolvePtr(pid_t pid, uintptr_t base, const int* offsets, int count)
{
    uintptr_t cur = base;
    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    int fd = open(memPath.c_str(), O_RDWR);
    if (fd < 0) return 0;
    for (int i = 0; i < count; i++)
    {
        lseek64(fd, cur, SEEK_SET);
        uint64_t val = 0;
        read(fd, &val, sizeof(uint64_t));
        if (!val) {
            close(fd);
            return 0;
        }
        cur = val + offsets[i];
    }
    close(fd);
    return cur;
}

uint64_t ReadU64(pid_t pid, uintptr_t addr)
{
    uint64_t res = 0;
    std::string memPath = "/proc/" + std::to_string(pid);
    int fd = open(memPath.c_str(), O_RDONLY);
    lseek64(fd, addr, SEEK_SET);
    read(fd, &res, 8);
    close(fd);
    return res;
}

uint8_t ReadU8(pid_t pid, uintptr_t addr)
{
    uint8_t res = 0;
    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    int fd = open(memPath.c_str(), O_RDONLY);
    lseek64(fd, addr, SEEK_SET);
    read(fd, &res, 1);
    close(fd);
    return res;
}

Vec3 ReadVec3(pid_t pid, uintptr_t addr)
{
    Vec3 v;
    std::string memPath = "/proc/" + std::to_string(pid) + "/mem";
    int fd = open(memPath.c_str(), O_RDONLY);
    lseek64(fd, addr, SEEK_SET);
    read(fd, &v, sizeof(Vec3));
    close(fd);
    return v;
}

// ===================== World To Screen Converter =====================
bool WorldToScreen(const Vec3& world, ImVec2& out)
{
    float halfW = g_ScrW * 0.5f;
    float halfH = g_ScrH * 0.5f;
    out.x = world.x * halfW + halfW;
    out.y = -world.z * halfH + halfH;
    return world.y > -15.0f;
}

// ===================== ESP Draw Utilities =====================
void DrawBox(ImDrawList* draw, Vec3 min, Vec3 max, ImU32 color)
{
    ImVec2 p[8];
    WorldToScreen(Vec3(min.x, min.y, min.z), p[0]);
    WorldToScreen(Vec3(max.x, min.y, min.z), p[1]);
    WorldToScreen(Vec3(max.x, min.y, max.z), p[2]);
    WorldToScreen(Vec3(min.x, min.y, max.z), p[3]);
    WorldToScreen(Vec3(min.x, max.y, min.z), p[4]);
    WorldToScreen(Vec3(max.x, max.y, min.z), p[5]);
    WorldToScreen(Vec3(max.x, max.y, max.z), p[6]);
    WorldToScreen(Vec3(min.x, max.y, max.z), p[7]);

    draw->AddLine(p[0], p[1], color, 2.0f);
    draw->AddLine(p[1], p[2], color, 2.0f);
    draw->AddLine(p[2], p[3], color, 2.0f);
    draw->AddLine(p[3], p[0], color, 2.0f);
    draw->AddLine(p[4], p[5], color, 2.0f);
    draw->AddLine(p[5], p[6], color, 2.0f);
    draw->AddLine(p[6], p[7], color, 2.0f);
    draw->AddLine(p[7], p[4], color, 2.0f);
    draw->AddLine(p[0], p[4], color, 2.0f);
    draw->AddLine(p[1], p[5], color, 2.0f);
    draw->AddLine(p[2], p[6], color, 2.0f);
    draw->AddLine(p[3], p[7], color, 2.0f);
}

void DrawAntenna(ImDrawList* draw, Vec3 foot, Vec3 head, ImU32 col)
{
    ImVec2 p1, p2;
    if (WorldToScreen(foot, p1) && WorldToScreen(head, p2))
        draw->AddLine(p1, p2, col, 3.0f);
}

void DrawBone(ImDrawList* draw, Vec3 pos, ImU32 col, float rad = 4.0f)
{
    ImVec2 p;
    if (WorldToScreen(pos, p))
        draw->AddCircleFilled(p, rad, col);
}

// ===================== SDL Global Vars =====================
static SDL_Window* g_Window = nullptr;
static SDL_GLContext g_GLContext;
static bool g_Running = true;
static ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

static void RenderFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    g_ScrW = (int)io.DisplaySize.x;
    g_ScrH = (int)io.DisplaySize.y;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImDrawList* bgDraw = ImGui::GetBackgroundDrawList();

    // Render All ESP Entities
    if (g_GamePid > 0)
    {
        uintptr_t modBase = GetModuleBase(g_GamePid, LIB_NAME.c_str());
        if (modBase != 0)
        {
            uintptr_t entityRoot = ResolvePtr(g_GamePid, modBase, OFF_ENTITY_LIST, OFF_ENTITY_LIST_CNT);
            if (entityRoot == 0) goto skip_esp;

            for (int i = 0; i < 256; i++)
            {
                uint64_t entAddr = ReadU64(g_GamePid, entityRoot + (uint64_t)i * 8);
                if (entAddr == 0) continue;

                uint8_t bIsPlayer = ReadU8(g_GamePid, entAddr + OFF_ENTITY_ISPLAYER);
                uint8_t bIsGhost  = ReadU8(g_GamePid, entAddr + OFF_ENTITY_ISGHOST);
                uint8_t bItem     = ReadU8(g_GamePid, entAddr + OFF_ENTITY_ITEM);
                uint8_t bAnomaly  = ReadU8(g_GamePid, entAddr + OFF_ENTITY_ANOMALY);

                Vec3 pos = ReadVec3(g_GamePid, entAddr + OFF_ENTITY_POS);
                Vec3 headPos = ReadVec3(g_GamePid, entAddr + OFF_ENTITY_POS + OFF_ENTITY_HEAD);
                Vec3 foot(pos.x, pos.y - 1.8f, pos.z);

                // Player ESP Cyan Box + Bones + Antenna
                if (bIsPlayer && g_ESP_Player)
                {
                    ImU32 col = IM_COL32(0, 255, 255, 255);
                    DrawBox(bgDraw, Vec3(pos.x - 0.4f, foot.y, pos.z - 0.4f), Vec3(pos.x + 0.4f, pos.y, pos.z + 0.4f), col);
                    DrawAntenna(bgDraw, foot, headPos, col);
                    DrawBone(bgDraw, headPos, IM_COL32(255,255,255,255),5);
                    DrawBone(bgDraw, Vec3(pos.x-0.3f, pos.y-0.7f, pos.z), col);
                    DrawBone(bgDraw, Vec3(pos.x+0.3f, pos.y-0.7f, pos.z), col);
                    DrawBone(bgDraw, foot, col);
                }
                // Ghost / Monster Red ESP
                else if (bIsGhost && g_ESP_Ghost)
                {
                    ImU32 col = IM_COL32(255, 30, 30, 255);
                    DrawBox(bgDraw, Vec3(pos.x - 0.5f, foot.y, pos.z - 0.5f), Vec3(pos.x + 0.5f, pos.y + 1.0f, pos.z + 0.5f), col);
                    DrawAntenna(bgDraw, foot, headPos, col);
                    DrawBone(bgDraw, headPos, IM_COL32(255,255,255,255),5);
                }
                // Item Yellow ESP
                else if (bItem && g_ESP_Item)
                {
                    ImU32 col = IM_COL32(255, 220, 0, 255);
                    DrawBox(bgDraw, Vec3(pos.x - 0.2f, foot.y, pos.z - 0.2f), Vec3(pos.x + 0.2f, pos.y + 0.3f, pos.z + 0.2f), col);
                }
                // Anomaly / EMF Purple ESP
                else if (bAnomaly && g_ESP_Anomaly)
                {
                    ImU32 col = IM_COL32(200, 50, 255, 255);
                    DrawBox(bgDraw, Vec3(pos.x - 0.15f, foot.y, pos.z - 0.15f), Vec3(pos.x + 0.15f, pos.y + 0.2f, pos.z + 0.15f), col);
                }
            }
        }
    }
skip_esp:

    // ESP Control Panel (All English Text, No Chinese)
    ImGui::Begin("ESP Control Panel");
    char pidBuf[32];
    sprintf(pidBuf, "%d", g_GamePid);
    ImGui::InputText("Game PID", pidBuf, IM_ARRAYSIZE(pidBuf));
    if (ImGui::Button("Attach Process"))
        g_GamePid = atoi(pidBuf);
    ImGui::Separator();
    ImGui::Checkbox("Player Box & Bones & Antenna", &g_ESP_Player);
    ImGui::Checkbox("Ghost / Monster ESP", &g_ESP_Ghost);
    ImGui::Checkbox("Item ESP", &g_ESP_Item);
    ImGui::Checkbox("EMF & Anomaly ESP", &g_ESP_Anomaly);
    ImGui::Text("FPS: %.1f", io.Framerate);
    ImGui::End();

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_Window);
}

int main(int, char**)
{
    // SDL Init
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0)
    {
        printf("SDL Init Fail: %s\n", SDL_GetError());
        return 1;
    }

    // GLES2 Settings
    const char* glsl_version = "#version 100";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    g_Window = SDL_CreateWindow("The Ghost ESP", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, window_flags);
    if (!g_Window)
    {
        printf("Window create fail\n");
        SDL_Quit();
        return 1;
    }

    g_GLContext = SDL_GL_CreateContext(g_Window);
    SDL_GL_MakeCurrent(g_Window, g_GLContext);
    SDL_GL_SetSwapInterval(1);

    // ImGui Init (Removed Font Loading Code, No External TTF Needed)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Use Default Built-in Font Only (No ziti.ttf required)
    ImGui_ImplSDL2_InitForOpenGL(g_Window, g_GLContext);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Main Game Loop
    g_Running = true;
    while (g_Running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                g_Running = false;
        }
        RenderFrame();
    }

    // Clean Up Resources
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(g_GLContext);
    SDL_DestroyWindow(g_Window);
    SDL_Quit();
    return 0;
}