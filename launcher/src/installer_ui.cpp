#include "installer_ui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "config_manager.h"
#include "installer_payload.h"
#include "installer_windows.h"
#include "localization.h"
#include "ui.h"  // LauncherUI::SetupTheme (shared visual identity)
#include "xiso_process.h"

using enum TextId;

namespace {

constexpr int kPathBufSize = 512;
constexpr const char* kVersion = "1.0.0";

enum class Step { Destination = 0, Iso, Progress, Finish, Failed };
enum class UnStep { Confirm, Done };
enum class IsoState { Idle, Checking, Valid, Invalid, NoXex };
enum Phase {
  kPhaseNone = 0,
  kPhasePreparing,
  kPhaseRemoving,
  kPhaseExtracting,
  kPhaseCopying,
  kPhaseConfig,
  kPhaseRegister,
};

Step g_step = Step::Destination;
bool g_uninstall_mode = false;
UnStep g_un_step = UnStep::Confirm;

char g_dest_buf[kPathBufSize] = "";
char g_iso_buf[kPathBufSize] = "";
bool g_bufs_initialized = false;
std::string g_last_checked_iso;
bool g_shortcut_desktop = true;

std::string g_error;

// Dialog plumbing (same pattern as the launcher UI).
std::string g_dialog_selected_path;
bool g_dialog_updated = false;
bool g_dialog_is_iso = false;

void SDLCALL DialogCallback(void* userdata, const char* const* filelist, int) {
  if (filelist && filelist[0]) {
    auto* out_str = static_cast<std::string*>(userdata);
    if (out_str) {
      *out_str = filelist[0];
      g_dialog_updated = true;
    }
  }
}

// ---------------------------------------------------------------------------
// ISO validation: runs "extract-xiso -l" (metadata only, fast) in a thread.
// ---------------------------------------------------------------------------

struct IsoCheck {
  std::atomic<bool> running{false};
  std::atomic<bool> done{false};
  std::atomic<bool> valid{false};
  std::atomic<bool> has_xex{false};
  std::atomic<uint32_t> files{0};
  std::atomic<uint64_t> total_bytes{0};
  std::string error;
  std::thread thread;
};

IsoCheck g_iso_check;

IsoState GetIsoCheckState() {
  if (g_iso_check.running && !g_iso_check.done) return IsoState::Checking;
  if (g_iso_check.done && g_iso_check.valid) {
    return g_iso_check.has_xex ? IsoState::Valid : IsoState::NoXex;
  }
  if (g_iso_check.done && !g_iso_check.valid) return IsoState::Invalid;
  return IsoState::Idle;
}

void JoinIsoCheck() {
  if (g_iso_check.thread.joinable()) {
    g_iso_check.thread.join();
  }
}

void StartIsoCheck(const std::string& iso) {
  JoinIsoCheck();
  g_iso_check.running = true;
  g_iso_check.done = false;
  g_iso_check.valid = false;
  g_iso_check.has_xex = false;
  g_iso_check.files = 0;
  g_iso_check.total_bytes = 0;
  g_iso_check.error.clear();
  g_last_checked_iso = iso;

  std::filesystem::path xiso_exe = installer_payload::Resolve("extract-xiso.exe");
  std::filesystem::path iso_path = iso;
  g_iso_check.thread = std::thread(
      [iso_path = std::move(iso_path), xiso_exe = std::move(xiso_exe)]() {
        xiso::Progress progress;
        xiso::RunResult res = xiso::RunList(xiso_exe, iso_path, progress);
        if (res.ok) {
          g_iso_check.valid = true;
          g_iso_check.has_xex = xiso::OutputHasDefaultXex(res.output);
          g_iso_check.files = res.files_total;
          g_iso_check.total_bytes = res.total_bytes;
        } else {
          g_iso_check.error = res.error;
        }
        g_iso_check.running = false;
        g_iso_check.done = true;
      });
}

// ---------------------------------------------------------------------------
// Install worker
// ---------------------------------------------------------------------------

struct InstallWorker {
  std::atomic<bool> running{false};
  std::atomic<bool> done{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> canceled{false};
  std::atomic<int> phase{kPhaseNone};
  std::atomic<uint32_t> copy_done{0};
  std::atomic<uint32_t> copy_total{0};
  std::string error;
  xiso::Progress progress;  // shared with extract-xiso (and the copy phase)
  std::thread thread;
};

InstallWorker g_worker;
std::string g_install_dest;

void JoinInstallWorker() {
  if (g_worker.thread.joinable()) {
    g_worker.thread.join();
  }
}

void WorkerFail(const std::string& error) {
  g_worker.error = error;
  g_worker.failed = true;
  g_worker.running = false;
  g_worker.done = true;
}

bool CopyPayloadTree(const std::filesystem::path& src_root,
                     const std::filesystem::path& dst_root) {
  namespace fs = std::filesystem;
  std::error_code ec;

  uint32_t total = 0;
  for (fs::recursive_directory_iterator it(src_root), end; it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) ++total;
  }
  g_worker.copy_total = total;
  g_worker.copy_done = 0;

  for (fs::recursive_directory_iterator it(src_root), end; it != end; it.increment(ec)) {
    if (g_worker.progress.cancel.load()) return false;

    const fs::path entry = it->path();
    // The staged game_root only carries a notice file: real game files were
    // already extracted (or are already present) in the destination.
    if (entry.parent_path() == src_root && entry.filename() == L"game_root") {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;

    const fs::path rel = fs::relative(entry, src_root, ec);
    if (ec) {
      WorkerFail("failed to resolve " + entry.string());
      return false;
    }
    const fs::path dst = dst_root / rel;
    fs::create_directories(dst.parent_path(), ec);
    if (ec) {
      WorkerFail("failed to create " + dst.string() + ": " + ec.message());
      return false;
    }
    // Never copy a file onto itself (installer run straight from the target).
    if (fs::weakly_canonical(dst, ec) == fs::weakly_canonical(entry, ec)) continue;

    fs::copy_file(entry, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      WorkerFail("failed to copy " + rel.string() + ": " + ec.message());
      return false;
    }
    g_worker.copy_done.fetch_add(1);
    g_worker.progress.SetCurrentFile(rel.string());
  }
  return true;
}

void RunInstallWorker(std::string dest, std::string iso) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path dest_p(dest);
  const fs::path game_root = dest_p / "game_root";

  g_worker.phase = kPhasePreparing;
  g_worker.progress.AddLogLine(Tr(InstPreparing));
  fs::create_directories(dest_p, ec);
  if (ec) {
    WorkerFail("failed to create " + dest_p.string() + ": " + ec.message());
    return;
  }

  // Like the reference installer: extraction is skipped when the destination
  // already carries the game files (default.xex).
  bool have_files = fs::exists(game_root / "default.xex", ec);
  if (!iso.empty() && !have_files) {
    if (fs::exists(game_root, ec)) {
      g_worker.phase = kPhaseRemoving;
      g_worker.progress.AddLogLine(Tr(InstRemovingOld));
      fs::remove_all(game_root, ec);  // leftover partial extraction
    }
    g_worker.phase = kPhaseExtracting;
    g_worker.progress.AddLogLine(Tr(InstExtractingIso));
    const fs::path xiso_exe = installer_payload::Resolve("extract-xiso.exe");
    xiso::RunResult res = xiso::RunExtract(xiso_exe, iso, game_root, g_worker.progress);
    if (g_worker.progress.cancel.load() || res.canceled) {
      g_worker.canceled = true;
      g_worker.running = false;
      g_worker.done = true;
      return;
    }
    if (!res.ok) {
      WorkerFail(res.error);
      return;
    }
  } else if (!have_files) {
    WorkerFail("game files are missing (no ISO selected and no existing files)");
    return;
  }

  g_worker.phase = kPhaseCopying;
  g_worker.progress.AddLogLine(Tr(InstCopyingFiles));
  const fs::path payload_root = installer_payload::Resolve(".");
  const bool same_dir =
      fs::weakly_canonical(payload_root, ec) == fs::weakly_canonical(dest_p, ec);
  if (!same_dir && !CopyPayloadTree(payload_root, dest_p)) {
    if (g_worker.progress.cancel.load()) {
      g_worker.canceled = true;
      g_worker.running = false;
      g_worker.done = true;
    }
    return;  // CopyPayloadTree already reported the error
  }

  g_worker.phase = kPhaseConfig;
  g_worker.progress.AddLogLine(Tr(InstWritingConfig));
  GameConfig config;
  config.launcher_language = Localization::GetLanguageCode();
  ConfigManager::Save(dest_p / "narutorise.toml", config);

  g_worker.phase = kPhaseRegister;
  g_worker.progress.AddLogLine(Tr(InstRegistering));
  installer_windows::RegisterUninstallEntry(dest_p, kVersion,
                                            dest_p / "narutorise_installer.exe");

  g_worker.running = false;
  g_worker.done = true;
}

void StartInstallWorker() {
  JoinInstallWorker();
  g_worker.running = true;
  g_worker.done = false;
  g_worker.failed = false;
  g_worker.canceled = false;
  g_worker.phase = kPhaseNone;
  g_worker.copy_done = 0;
  g_worker.copy_total = 0;
  g_worker.error.clear();
  g_install_dest = g_dest_buf;

  std::string dest(g_dest_buf);
  std::string iso(g_iso_buf);
  g_worker.thread = std::thread(
      [dest = std::move(dest), iso = std::move(iso)]() {
        RunInstallWorker(dest, iso);
      });
}

const char* PhaseLabel() {
  switch (g_worker.phase.load()) {
    case kPhasePreparing: return Tr(InstPreparing);
    case kPhaseRemoving: return Tr(InstRemovingOld);
    case kPhaseExtracting: return Tr(InstExtractingIso);
    case kPhaseCopying: return Tr(InstCopyingFiles);
    case kPhaseConfig: return Tr(InstWritingConfig);
    case kPhaseRegister: return Tr(InstRegistering);
    default: return "";
  }
}

// ---------------------------------------------------------------------------
// Shared widgets (single-column layout, mirroring the reference installer)
// ---------------------------------------------------------------------------

void DrawLanguageCombo(float content_w) {
  const float combo_w = 160.0f;
  ImGui::SetNextItemWidth(combo_w);
  ImGui::SameLine(content_w - combo_w);
  const auto& languages = Localization::GetLanguages();
  if (ImGui::BeginCombo("##LangSelect", languages[static_cast<size_t>(Localization::GetLanguage())].name)) {
    for (const auto& option : languages) {
      const bool selected = Localization::GetLanguage() == option.language;
      if (ImGui::Selectable(option.name, selected)) Localization::SetLanguage(option.language);
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
}

void DrawStepTitle(const char* text) {
  ImGui::SetWindowFontScale(1.15f);
  ImGui::Text("%s", text);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Spacing();
}

void DrawHint(const char* text) {
  ImGui::SetWindowFontScale(0.85f);
  ImGui::PushTextWrapPos();
  ImGui::TextDisabled("%s", text);
  ImGui::PopTextWrapPos();
  ImGui::SetWindowFontScale(1.0f);
}

bool NextEnabled() {
  switch (g_step) {
    case Step::Destination: {
      std::string dest(g_dest_buf);
      return !dest.empty() && installer_payload::Available();
    }
    case Step::Iso: {
      if (GetIsoCheckState() == IsoState::Valid) return true;
      std::error_code ec;
      return std::filesystem::exists(
          std::filesystem::path(g_dest_buf) / "game_root" / "default.xex", ec);
    }
    default:
      return false;
  }
}

const char* NextLabel() {
  if (g_step == Step::Iso) return Tr(InstInstall);
  if (g_step == Step::Finish) return Tr(InstFinish);
  return Tr(InstNext);
}

// ---------------------------------------------------------------------------
// Steps
// ---------------------------------------------------------------------------

void DrawDestinationStep() {
  DrawStepTitle(Tr(InstDestTitle));
  const float browse_w = ImGui::CalcTextSize(Tr(InstBrowse)).x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
  ImGui::InputText("##DestPath", g_dest_buf, kPathBufSize);
  ImGui::SameLine();
  if (ImGui::Button(Tr(InstBrowse), ImVec2(browse_w, 0))) {
    g_dialog_is_iso = false;
    SDL_ShowOpenFolderDialog(DialogCallback, &g_dialog_selected_path, nullptr,
                             nullptr, false);
  }
  ImGui::Spacing();
  DrawHint(Tr(InstDestHint));
}

void DrawIsoStep() {
  DrawStepTitle(Tr(InstIsoTitle));
  const float browse_w = ImGui::CalcTextSize(Tr(InstBrowse)).x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
  ImGui::InputText("##IsoPath", g_iso_buf, kPathBufSize);
  ImGui::SameLine();
  if (ImGui::Button(Tr(InstBrowse), ImVec2(browse_w, 0))) {
    g_dialog_is_iso = true;
    SDL_DialogFileFilter filters[] = {{Tr(InstIsoTitle), "iso"}};
    SDL_ShowOpenFileDialog(DialogCallback, &g_dialog_selected_path, nullptr, filters,
                            1, nullptr, false);
  }
  ImGui::Spacing();
  DrawHint(Tr(InstIsoHint));

  ImGui::Spacing();
  switch (GetIsoCheckState()) {
    case IsoState::Checking:
      DrawHint(Tr(InstIsoChecking));
      break;
    case IsoState::Valid: {
      char buf[128];
      double gb = static_cast<double>(g_iso_check.total_bytes.load()) / (1000.0 * 1000.0 * 1000.0);
      snprintf(buf, sizeof(buf), Tr(InstIsoValid), g_iso_check.files.load(), gb);
      ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.35f, 1.0f), "%s", buf);
      break;
    }
    case IsoState::NoXex:
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "%s", Tr(InstIsoNoXex));
      break;
    case IsoState::Invalid:
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "%s", Tr(InstIsoInvalid));
      break;
    case IsoState::Idle:
    default:
      if (!NextEnabled()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f), "%s", Tr(InstIsoRequired));
      }
      break;
  }
}

void DrawProgressStep(float content_w) {
  DrawStepTitle(Tr(InstProgressTitle));
  ImGui::Text("%s", PhaseLabel());
  ImGui::Spacing();

  const float bar_w = content_w;
  const int phase = g_worker.phase.load();
  if (phase == kPhaseExtracting) {
    // extract-xiso only reports totals at the end: animated (barber-pole) bar.
    float t = fmodf(static_cast<float>(ImGui::GetTime()) * 0.35f, 1.0f);
    char label[160];
    snprintf(label, sizeof(label), "%s (%u)", g_worker.progress.GetCurrentFile().c_str(),
             g_worker.progress.files_done.load());
    ImGui::ProgressBar(t, ImVec2(bar_w, 20), label);
  } else if (phase == kPhaseCopying) {
    uint32_t total = g_worker.copy_total.load();
    uint32_t done = g_worker.copy_done.load();
    float frac = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
    char label[160];
    snprintf(label, sizeof(label), "%u / %u", done, total);
    ImGui::ProgressBar(frac, ImVec2(bar_w, 20), label);
  } else {
    // Preparing / removing / config / register phases: animated bar.
    float t = fmodf(static_cast<float>(ImGui::GetTime()) * 0.35f, 1.0f);
    ImGui::ProgressBar(t, ImVec2(bar_w, 20), nullptr);
  }

  ImGui::Spacing();
  ImGui::BeginChild("LogBox", ImVec2(bar_w, 200), true);
  std::vector<std::string> lines = g_worker.progress.GetRecentLines();
  size_t first = lines.size() > 14 ? lines.size() - 14 : 0;
  for (size_t i = first; i < lines.size(); ++i) {
    ImGui::TextDisabled("%s", lines[i].c_str());
  }
  ImGui::EndChild();
}

void DrawFinishStep() {
  DrawStepTitle(Tr(InstFinishTitle));
  ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.35f, 1.0f), "[OK] %s", Tr(InstComplete));
  ImGui::Spacing();
  ImGui::Text(Tr(InstInstalledTo), g_install_dest.c_str());
  ImGui::Text(Tr(InstGameFilesAt), (std::filesystem::path(g_install_dest) / "game_root").string().c_str());
  ImGui::Spacing();
  ImGui::Checkbox(Tr(InstShortcutDesktop), &g_shortcut_desktop);
}

void DrawFailedStep() {
  DrawStepTitle(Tr(InstFailedTitle));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.55f, 0.45f, 1.0f));
  ImGui::TextWrapped("%s", g_error.c_str());
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Uninstall flow
// ---------------------------------------------------------------------------

void RunUninstall(bool keep_game_files, bool& request_exit) {
  namespace fs = std::filesystem;
  std::error_code ec;

  installer_windows::RemoveUninstallEntry();
  fs::remove(installer_windows::DesktopShortcutPath(), ec);
  fs::remove_all(installer_windows::StartMenuGroupDir(), ec);

  const fs::path install_dir = installer_payload::InstallerDir();
  if (!keep_game_files) {
    installer_windows::ScheduleDelayedRemove(install_dir, true);
  } else {
    const fs::path own_exe = installer_payload::OwnExePath();
    for (fs::directory_iterator it(install_dir), end; it != end; it.increment(ec)) {
      const fs::path entry = it->path();
      if (entry.filename() == L"game_root") continue;
      if (!own_exe.empty() && entry == own_exe) continue;
      fs::remove_all(entry, ec);
    }
    if (!own_exe.empty()) {
      installer_windows::ScheduleDelayedRemove(own_exe, false);
    }
  }
  g_un_step = UnStep::Done;
  (void)request_exit;
}

void DrawUninstallPane(float w, float h, bool& request_exit) {
  const float pad = 20.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  ImGui::BeginChild("UninstallPane", ImVec2(w, h), false);

  ImGui::SetWindowFontScale(1.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.05f, 1.0f));
  ImGui::Text("NARUTO: RISE OF A NINJA");
  ImGui::PopStyleColor();
  ImGui::SetWindowFontScale(1.0f);
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  if (g_un_step == UnStep::Confirm) {
    DrawStepTitle(Tr(UnWindowTitle));
    ImGui::TextWrapped("%s", Tr(UnIntro));
    ImGui::Spacing();
    ImGui::Text(Tr(InstInstalledTo), installer_payload::InstallerDir().string().c_str());
    ImGui::Text(Tr(InstGameFilesAt),
                (installer_payload::InstallerDir() / "game_root").string().c_str());
    ImGui::Spacing();
    ImGui::Spacing();

    const float remove_w = std::max(200.0f, ImGui::CalcTextSize(Tr(UnRemoveAll)).x + ImGui::GetStyle().FramePadding.x * 2);
    const float keep_w = std::max(200.0f, ImGui::CalcTextSize(Tr(UnKeepFiles)).x + ImGui::GetStyle().FramePadding.x * 2);
    const float cancel_w = std::max(90.0f, ImGui::CalcTextSize(Tr(UnCancel)).x + ImGui::GetStyle().FramePadding.x * 2);
    if (ImGui::Button(Tr(UnRemoveAll), ImVec2(remove_w, 32))) {
      RunUninstall(false, request_exit);
    }
    ImGui::SameLine(0, 10.0f);
    if (ImGui::Button(Tr(UnKeepFiles), ImVec2(keep_w, 32))) {
      RunUninstall(true, request_exit);
    }
    ImGui::SameLine(0, 10.0f);
    if (ImGui::Button(Tr(UnCancel), ImVec2(cancel_w, 32))) {
      request_exit = true;
    }
  } else {
    DrawStepTitle(Tr(UnWindowTitle));
    ImGui::TextWrapped("%s", Tr(UnDone));
    ImGui::Spacing();
    if (ImGui::Button(Tr(InstFinish), ImVec2(90, 30))) {
      request_exit = true;
    }
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

}  // namespace

void InstallerUI::EnterUninstallMode() {
  g_uninstall_mode = true;
}

int InstallerUI::RunInstallHeadless(const std::string& dest, const std::string& iso) {
  strncpy(g_dest_buf, dest.c_str(), kPathBufSize - 1);
  g_dest_buf[kPathBufSize - 1] = '\0';
  strncpy(g_iso_buf, iso.c_str(), kPathBufSize - 1);
  g_iso_buf[kPathBufSize - 1] = '\0';

  StartInstallWorker();
  while (!g_worker.done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  JoinInstallWorker();

  if (g_worker.canceled) return 2;
  if (g_worker.failed) return 1;
  return 0;
}

void InstallerUI::Shutdown() {
  g_worker.progress.cancel = true;
  JoinIsoCheck();
  JoinInstallWorker();
}

bool InstallerUI::Render(SDL_Window* window, bool& request_exit) {
  int w = 0, h = 0;
  SDL_GetWindowSize(window, &w, &h);
  const float fw = static_cast<float>(w);
  const float fh = static_cast<float>(h);
  const float pad = 20.0f;  // grid margin, like the reference installer

  if (!g_bufs_initialized) {
    g_bufs_initialized = true;
#ifdef _WIN32
    strncpy(g_dest_buf, "C:\\Games\\Naruto - Rise of a Ninja", kPathBufSize - 1);
#endif
  }

  // Consume dialog results.
  if (g_dialog_updated) {
    if (!g_dialog_selected_path.empty()) {
      if (g_dialog_is_iso) {
        strncpy(g_iso_buf, g_dialog_selected_path.c_str(), kPathBufSize - 1);
      } else {
        strncpy(g_dest_buf, g_dialog_selected_path.c_str(), kPathBufSize - 1);
      }
    }
    g_dialog_updated = false;
  }

  // Trigger ISO validation whenever the path changes.
  if (g_step == Step::Iso && !g_iso_check.running) {
    std::string iso_now(g_iso_buf);
    if (iso_now != g_last_checked_iso && !iso_now.empty()) {
      std::error_code ec;
      if (std::filesystem::exists(std::filesystem::path(iso_now), ec)) {
        StartIsoCheck(iso_now);
      }
    }
  }

  // Reap the finished install worker.
  if (g_step == Step::Progress && g_worker.done) {
    JoinInstallWorker();
    if (g_worker.failed) {
      g_error = g_worker.error;
      g_step = Step::Failed;
    } else if (g_worker.canceled) {
      g_step = Step::Iso;
    } else {
      g_step = Step::Finish;
    }
  }

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(fw, fh));
  ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoScrollbar;
  ImGui::Begin("InstallerRoot", nullptr, win_flags);

  if (g_uninstall_mode) {
    DrawUninstallPane(fw, fh, request_exit);
    ImGui::End();
    return true;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
  const float content_w = fw - pad * 2.0f;

  // Row 0: header — big accent title + language selector on the right.
  ImGui::SetWindowFontScale(1.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.05f, 1.0f));
  ImGui::Text("NARUTO: RISE OF A NINJA");
  ImGui::PopStyleColor();
  ImGui::SetWindowFontScale(1.0f);
  DrawLanguageCombo(content_w);
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // Row 1: step content, left-aligned stack.
  ImGui::BeginChild("WizardContent", ImVec2(content_w, fh - pad * 2.0f - 92.0f), false,
                    ImGuiWindowFlags_NoScrollbar);
  switch (g_step) {
    case Step::Destination: DrawDestinationStep(); break;
    case Step::Iso: DrawIsoStep(); break;
    case Step::Progress: DrawProgressStep(content_w); break;
    case Step::Finish: DrawFinishStep(); break;
    case Step::Failed: DrawFailedStep(); break;
  }
  ImGui::EndChild();

  // Row 2: footer buttons size to translated labels, bottom-right.
  float btn_w = 90.0f;
  for (const char* label : {Tr(InstBack), NextLabel(), Tr(UnCancel)}) {
    btn_w = std::max(btn_w, ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2);
  }
  const float btn_h = 30.0f;
  const float footer_y = fh - pad - btn_h;

  if (g_step == Step::Destination || g_step == Step::Iso) {
    if (g_step == Step::Iso) {
      ImGui::SetCursorPos(ImVec2(fw - pad - btn_w * 2.0f - 10.0f, footer_y));
      if (ImGui::Button(Tr(InstBack), ImVec2(btn_w, btn_h))) {
        g_step = Step::Destination;
      }
      ImGui::SameLine(0, 10.0f);
    } else {
      ImGui::SetCursorPos(ImVec2(fw - pad - btn_w, footer_y));
    }

    const bool next_enabled = NextEnabled();
    if (!next_enabled) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.38f, 0.05f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.50f, 0.12f, 1.00f));
    if (ImGui::Button(NextLabel(), ImVec2(btn_w, btn_h))) {
      g_step = static_cast<Step>(static_cast<int>(g_step) + 1);
      if (g_step == Step::Progress) {
        StartInstallWorker();
      }
    }
    ImGui::PopStyleColor(2);
    if (!next_enabled) ImGui::EndDisabled();
  } else if (g_step == Step::Finish) {
    ImGui::SetCursorPos(ImVec2(fw - pad - btn_w, footer_y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.38f, 0.05f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.50f, 0.12f, 1.00f));
    if (ImGui::Button(Tr(InstFinish), ImVec2(btn_w, btn_h))) {
      const std::filesystem::path dest_p(g_install_dest);
      if (g_shortcut_desktop) {
        installer_windows::CreateShortcut(dest_p / "narutorise_launcher.exe", dest_p,
                                          installer_windows::DesktopShortcutPath(),
                                          "Naruto: Rise of a Ninja PC Port");
      }
      request_exit = true;
    }
    ImGui::PopStyleColor(2);
  } else if (g_step == Step::Failed) {
    ImGui::SetCursorPos(ImVec2(fw - pad - btn_w * 2.0f - 10.0f, footer_y));
    if (ImGui::Button(Tr(UnCancel), ImVec2(btn_w, btn_h))) {
      request_exit = true;
    }
    ImGui::SameLine(0, 10.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.38f, 0.05f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.50f, 0.12f, 1.00f));
    if (ImGui::Button(Tr(InstBack), ImVec2(btn_w, btn_h))) {
      g_error.clear();
      g_step = Step::Iso;  // retry with a different ISO
    }
    ImGui::PopStyleColor(2);
  }

  ImGui::PopStyleVar();
  ImGui::End();
  return true;
}
