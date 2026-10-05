#pragma once

#include <filesystem>
#include <string>

struct SDL_Window;
struct ID3D11ShaderResourceView;

class InstallerUI {
 public:
  // --uninstall: switches the wizard to the uninstaller flow.
  static void EnterUninstallMode();

  // Renders one frame. Set request_exit to close the app.
  static bool Render(SDL_Window* window, bool& request_exit);

  // Cancels and joins any running worker threads (call before shutdown).
  static void Shutdown();

  // Headless (tests/automation): runs the full install worker — ISO
  // extraction into <dest>\game_root, payload copy, narutorise.toml and the
  // Add/Remove Programs entry — without the wizard. Returns 0 on success,
  // 1 on failure, 2 on cancellation.
  static int RunInstallHeadless(const std::string& dest, const std::string& iso);
};
