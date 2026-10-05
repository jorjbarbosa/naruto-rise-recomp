#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#if defined(_WIN32)
#include <windows.h>
#include <backends/imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#endif

#include "config_manager.h"
#include "installer_payload.h"
#include "installer_ui.h"
#include "installer_windows.h"
#include "localization.h"
#include "theme.h"
#include "xiso_process.h"

using enum TextId;

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static void CreateRenderTarget() {
  ID3D11Texture2D* pBackBuffer = nullptr;
  if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
  }
}

static void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

static bool CreateDeviceD3D(HWND hWnd) {
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };

  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED) {
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  }
  if (FAILED(res)) {
    return false;
  }

  CreateRenderTarget();
  return true;
}

static void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}
#endif  // _WIN32

namespace {

// Wires stdout/stderr for headless diagnostics:
//  * launched with redirected handles (pipelines, PowerShell capture): the
//    existing stdout already works;
//  * launched from a console (cmd) without handles: attach to the parent
//    console and reopen CONOUT$.
void SetupConsoleIo() {
#if defined(_WIN32)
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  const bool redirected = (out != nullptr) && (out != INVALID_HANDLE_VALUE);
  if (!redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
  }
#endif
}

// Hidden diagnostics (also used by the packaging tests):
//   narutorise_installer --iso-list <iso>
//   narutorise_installer --iso-extract <iso> <dest>
int HeadlessIso(const char* iso, const char* dest) {
  SetupConsoleIo();

  std::filesystem::path xiso_exe = installer_payload::Resolve("extract-xiso.exe");
  if (!std::filesystem::exists(xiso_exe)) {
    std::fprintf(stderr, "extract-xiso.exe not found at: %s\n",
                 xiso_exe.string().c_str());
    return 2;
  }

  xiso::Progress progress;
  xiso::RunResult res;
  if (dest) {
    res = xiso::RunExtract(xiso_exe, iso, dest, progress);
  } else {
    res = xiso::RunList(xiso_exe, iso, progress);
  }

  std::fprintf(stdout, "%s", res.output.c_str());
  std::fprintf(stdout, "\n[result] ok=%d exit=%d files=%u bytes=%llu xex=%d error=%s\n",
               res.ok ? 1 : 0, res.exit_code, res.files_total,
               static_cast<unsigned long long>(res.total_bytes),
               xiso::OutputHasDefaultXex(res.output) ? 1 : 0, res.error.c_str());
  return res.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Hidden headless modes (no window): diagnostics + automated tests.
  if (argc >= 3 && std::strcmp(argv[1], "--iso-list") == 0) {
    return HeadlessIso(argv[2], nullptr);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--iso-extract") == 0) {
    return HeadlessIso(argv[2], argv[3]);
  }
  if (argc >= 4 && std::strcmp(argv[1], "--install-test") == 0) {
    SetupConsoleIo();
    int rc = InstallerUI::RunInstallHeadless(argv[2], argv[3]);
    std::fprintf(stdout, "[install-test] result code: %d\n", rc);
    return rc;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--payload-info") == 0) {
    SetupConsoleIo();
    std::string err;
    bool embedded = installer_payload::ExtractEmbedded(err);
    std::fprintf(stdout, "embedded payload: %s\n", embedded ? "yes" : "no");
    std::fprintf(stdout, "payload available: %s\n",
                 installer_payload::Available() ? "yes" : "no");
    if (!err.empty()) {
      std::fprintf(stdout, "payload error: %s\n", err.c_str());
      return 1;
    }
    return 0;
  }

  bool uninstall_mode = false;
  if (argc >= 2 && std::strcmp(argv[1], "--uninstall") == 0) {
    uninstall_mode = true;
  }

  // Default the wizard language to the system language.
#if defined(_WIN32)
  if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE) {
    Localization::SetLanguageCode("pt_BR");
  }
#endif

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    std::fprintf(stderr, "Failed to initialize SDL3: %s\n", SDL_GetError());
    return 1;
  }

  const char* title = uninstall_mode ? Tr(UnWindowTitle) : Tr(InstWindowTitle);
  SDL_Window* window = SDL_CreateWindow(title, 740, 520, SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "Failed to create SDL_Window: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

#if defined(_WIN32)
  HWND hwnd = (HWND)SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (!hwnd || !CreateDeviceD3D(hwnd)) {
    std::fprintf(stderr, "Failed to initialize Direct3D 11 device.\n");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
#endif

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  launcher_theme::Apply();  // shared visual identity with the launcher

#if defined(_WIN32)
  ImGui_ImplSDL3_InitForD3D(window);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
#endif

  installer_windows::CoInitializeForUi();

  if (uninstall_mode) {
    InstallerUI::EnterUninstallMode();
  }

  bool running = true;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      }
      if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
          event.window.windowID == SDL_GetWindowID(window)) {
        running = false;
      }
#if defined(_WIN32)
      if (event.type == SDL_EVENT_WINDOW_RESIZED &&
          event.window.windowID == SDL_GetWindowID(window)) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
      }
#endif
    }

#if defined(_WIN32)
    ImGui_ImplDX11_NewFrame();
#endif
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    InstallerUI::Render(window, running);

    ImGui::Render();
#if defined(_WIN32)
    const float clear_color[4] = {0.09f, 0.09f, 0.11f, 1.00f};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
#endif
  }

  InstallerUI::Shutdown();
  installer_windows::CoUninitializeForUi();

#if defined(_WIN32)
  ImGui_ImplDX11_Shutdown();
#endif
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

#if defined(_WIN32)
  CleanupDeviceD3D();
#endif
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
