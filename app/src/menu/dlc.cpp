// Content paths and package operations come from the launcher's existing model.
#include "menu/dlc.h"
#include "menu/ui.h"
#include "menu/text.h"
#include "menu/game_choice.h"
#include "data_paths.h"
#include "../menu/content_manager.h"
#include "../menu/folders.h"
#include <rex/logging.h>
namespace narutorise::menu {
namespace {
std::string Utf8(const std::filesystem::path& path) { const auto s=path.u8string(); return {s.begin(),s.end()}; }
}
void Dlc::Open(Ui& ui, std::filesystem::path directory, std::filesystem::path user_root, void* owner,
               std::function<void(std::function<void()>)> defer, std::function<void()> closed) {
  ui_ = &ui; owner_ = owner; directory_ = std::move(directory); defer_ = std::move(defer); closed_ = std::move(closed);
  ui_->HideDialog();
  launcher_content::SetUserDataRoot(std::move(user_root)); Redraw();
}
void Dlc::Redraw() {
  std::vector<PanelRow> rows;
  const auto installed = launcher_content::ListInstalledDlc();
  if (installed.empty()) rows.push_back({"dlc-empty", Tr(TextId::DlcNoInstalled), "", Tr(TextId::DlcIntro)});
  for (size_t i = 0; i < installed.size(); ++i) {
    const auto& item = installed[i];
    PanelRow row{"dlc-" + std::to_string(i), item.name, Text("installed"), Utf8(item.path), false, false, {}, 0, {},
        [path = item.path] { launcher_ui::OpenFolder(path); }};
    row.good = true; rows.push_back(std::move(row));
  }
  for (const auto& item : launcher_content::ListPendingDlc(directory_)) {
    PanelRow row{"staged-" + std::to_string(rows.size()), item.name, Text("install-on-restart"),
        Tr(TextId::DlcStfsStaged), true};
    rows.push_back(std::move(row));
  }
  if (!rows.empty()) rows.front().section = Text("installed-list");
  PanelRow add{"dlc-install", Text("add-dlc"), "", Tr(TextId::DlcInstallLooseHint),
      false, false, {}, 0, {}, [this] { if (!work_.valid()) defer_([this] { ChooseFolder(); }); }};
  add.centered = true; rows.push_back(std::move(add));
  PanelRow folder{"dlc-folder", Tr(TextId::DlcOpenContentFolder), "", Utf8(launcher_content::GetDlcRoot()), false, false, {}, 0, {},
      [this] {
        std::string error;
        if (launcher_content::EnsureDlcFolders(error)) launcher_ui::OpenFolder(launcher_content::GetDlcRoot());
        else ui_->ShowDialog(Text("dlc"), Tr(TextId::DlcInstallFailed), {{Text("cancel"), {}}});
      }};
  folder.centered = true; rows.push_back(std::move(folder));
  ui_->ShowPanel(Text("dlc"), {Text("dlc")}, 0, std::move(rows), {},
      [this] { ui_->HidePanel(); if (closed_) closed_(); }, "dlc-install");
  view_ = ui_->ViewRevision();
}
void Dlc::ChooseFolder() {
  if (!ui_ || view_ != ui_->ViewRevision()) return;
  const auto choice = ChooseFile(owner_, true);
  if (!choice.error.empty()) { ui_->ShowDialog(Text("picker-failed"), choice.error, {{Text("cancel"), {}}}); return; }
  if (choice.path.empty()) return;
  InstallFolder(choice.path);
}
void Dlc::InstallFolder(const std::filesystem::path& path) {
  if (!ui_ || work_.valid() || view_ != ui_->ViewRevision()) return;
  // A selected ancestor of the target would recursively copy itself.
  const auto source = data::CanonicalPath(path);
  const auto target = data::CanonicalPath(launcher_content::GetDlcRoot());
  const auto relative = target.lexically_relative(source);
  if (relative.empty() || (!relative.is_absolute() && *relative.begin() != "..")) {
    ui_->ShowDialog(Text("dlc"), Tr(TextId::DlcInstallFailed), {{Text("cancel"), {}}}); return;
  }
  job_view_ = view_;
  work_ = std::async(std::launch::async, [source, directory = directory_] {
    Result result;
    try { result.staged = launcher_content::InstallDlc(source, directory, result.error); }
    catch (const std::exception& e) { result.error = e.what(); }
    return result;
  });
  ui_->ShowDialog(Text("dlc"), Text("working"));
}
void Dlc::Poll() {
  if (!work_.valid() || work_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const auto result = work_.get();
  if (!result.error.empty()) REXLOG_WARN("[narutorise] DLC installation: {}", result.error);
  if (!ui_ || job_view_ != ui_->ViewRevision()) return;
  ui_->HideDialog(); Redraw();
  ui_->ShowDialog(Text("dlc"), Tr(result.staged < 0 ? TextId::DlcInstallFailed :
      result.staged ? TextId::DlcStfsStaged : TextId::DlcInstallSuccess), {{result.staged < 0 ? Text("cancel") : Tr(TextId::KeysDone), {}}});
}
}  // namespace narutorise::menu
