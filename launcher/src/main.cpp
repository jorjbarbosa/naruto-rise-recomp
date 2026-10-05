#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_process.h>
#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>

#if defined(_WIN32)
#include <windows.h>
#include <backends/imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#endif

#include "config_manager.h"
#include "game_paths.h"
#include "localization.h"
#include "texture_loader.h"
#include "ui.h"
#include "theme.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

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
#endif

int main(int argc, char* argv[]) {
  (void)argc;
  (void)argv;

  // Resolve base directory and configuration path
  const char* base_path_str = SDL_GetBasePath();
  std::filesystem::path base_dir = base_path_str ? std::filesystem::u8path(base_path_str)
                                               : std::filesystem::current_path();
  std::filesystem::path toml_path = base_dir / "narutorise.toml";

  GameConfig config;
  ConfigManager::Load(toml_path, config);

  // Initialize localization (English by default, or loaded from config)
  Localization::SetLanguageCode(config.launcher_language);

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
    std::cerr << "Failed to initialize SDL3: " << SDL_GetError() << std::endl;
    return 1;
  }

  const int window_w = 1060;
  const int window_h = 720;
  SDL_Window* window = SDL_CreateWindow(
      Tr(TextId::WindowTitle),
      window_w, window_h,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);

  if (!window) {
    std::cerr << "Failed to create SDL_Window: " << SDL_GetError() << std::endl;
    SDL_Quit();
    return 1;
  }

  SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  SDL_SetWindowMinimumSize(window, 1000, 680);

#if defined(_WIN32)
  HWND hwnd = (HWND)SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
  if (hwnd) {
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (icon) {
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
  }
  if (!hwnd || !CreateDeviceD3D(hwnd)) {
    std::cerr << "Failed to initialize Direct3D 11 device." << std::endl;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // Load cover artwork texture
  LoadedTexture cover_tex = LoadTextureFromFile(g_pd3dDevice, base_dir / "assets" / "cover.jpg");
  if (!cover_tex.srv) {
    cover_tex = LoadTextureFromFile(g_pd3dDevice, base_dir / "cover.jpg");
  }
#endif

  // Setup ImGui
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr; // Do not save layout .ini to disk

  LauncherUI::SetupTheme();
  launcher_theme::LoadFonts(base_dir);

#if defined(_WIN32)
  ImGui_ImplSDL3_InitForD3D(window);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
#endif

  bool running = true;
  bool request_launch = false;
  std::string status_msg = "";

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

#if defined(_WIN32)
    LauncherUI::Render(window, base_dir, config, request_launch, status_msg, cover_tex.srv,
                       cover_tex.width, cover_tex.height);
#else
    LauncherUI::Render(window, base_dir, config, request_launch, status_msg, nullptr);
#endif

    ImGui::Render();
#if defined(_WIN32)
    const float clear_color[4] = { 0.09f, 0.09f, 0.11f, 1.00f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0); // VSync enabled
#endif

    if (request_launch) {
      ConfigManager::Save(toml_path, config);
      const std::filesystem::path game_data = game_paths::Find(base_dir, config.custom_game_root);
      if (game_data.empty()) {
        status_msg = Tr(TextId::FilesMissingDesc);
        request_launch = false;
        continue;
      }

#if defined(_WIN32)
      std::filesystem::path game_exe = base_dir / "narutorise.exe";
      if (std::filesystem::exists(game_exe)) {
        std::wstring cmdline = L"\"" + game_exe.wstring() + L"\"";
        cmdline += L" --game_data_root=\"" + game_data.wstring() + L"\"";

        std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
        cmd_buf.push_back(L'\0');

        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring cwd = base_dir.wstring();
        BOOL success = CreateProcessW(
            nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi);

        if (success) {
          CloseHandle(pi.hThread);
          CloseHandle(pi.hProcess);
          status_msg = Tr(TextId::LaunchSuccessMsg);
          // Exit launcher once the game process is spawned
          running = false;
        } else {
          status_msg = std::string(Tr(TextId::LaunchFailedMsg)) + " Win32 Error " + std::to_string(GetLastError());
        }
      } else {
        status_msg = std::string(Tr(TextId::ExeNotFoundMsg)) + game_exe.string();
      }
#else
      std::filesystem::path game_exe = base_dir / "narutorise";
      if (std::filesystem::exists(game_exe)) {
        std::string exe_str = game_exe.string();
        std::string arg_root = "--game_data_root=" + game_data.string();
        const char* args[3];
        args[0] = exe_str.c_str();
        args[1] = arg_root.c_str();
        args[2] = nullptr;

        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        std::string cwd_str = base_dir.string();
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cwd_str.c_str());

        SDL_Process* proc = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (proc) {
          SDL_DestroyProcess(proc);
          status_msg = Tr(TextId::LaunchSuccessMsg);
          running = false;
        } else {
          status_msg = std::string(Tr(TextId::LaunchFailedMsg)) + SDL_GetError();
        }
      } else {
        status_msg = std::string(Tr(TextId::ExeNotFoundMsg)) + game_exe.string();
      }
#endif
      request_launch = false;
    }
  }

  // Save settings before closing
  ConfigManager::Save(toml_path, config);

#if defined(_WIN32)
  ImGui_ImplDX11_Shutdown();
#endif
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();

#if defined(_WIN32)
  cover_tex.Release();
  CleanupDeviceD3D();
#endif
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
