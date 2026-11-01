// The first page of the setup screen (setup_screen.h).
#include "menu/setup_screen.h"
#include "menu/text.h"
#include "menu/render_interface.h"
#include <utility>
#include <RmlUi/Core.h>
#include <rex/logging.h>

namespace narutorise::menu {
namespace {
using game_image::Status;

// The state line under the menu: a title and a detail. A ready game shows
// the game files check there.
std::pair<const char*, const char*> StatusText(const SetupScreen::State& state, const std::string& files_notice) {
  switch (state.reason) {
    case SetupReason::kFirstRun: return {Text("setup-no-game"), nullptr};
    case SetupReason::kGameMissing: return {Text("missing"), Text("missing-detail")};
    case SetupReason::kGameNotReady:
      if (state.game.status == Status::kIncomplete) return {Text("incomplete"), Text("incomplete-detail")};
      return {Text(state.game.status == Status::kOtherVersion ? "unsupported" :
                   state.game.status == Status::kUnreadable ? "unreadable" : "unrecognized"), Text("game-hint")};
    case SetupReason::kStartFailed: return {Text("start-failed"), Text("start-detail")};
    default: return {Text("setup-ready"), files_notice.empty() ? nullptr : files_notice.c_str()};
  }
}
}  // namespace

bool SetupScreen::CreateModel(Rml::Context& context) {
  if (model_) return true;
  Rml::DataModelConstructor constructor = context.CreateDataModel("setup");
  if (!constructor) return false;
  constructor.Bind("game_ready", &game_ready_);
  constructor.Bind("start_failed", &start_failed_);
  constructor.Bind("files_warning", &files_warning_);
  constructor.Bind("logo_ready", &logo_ready_);
  constructor.Bind("first_label", &first_label_);
  constructor.Bind("status_title", &status_title_);
  constructor.Bind("status_detail", &status_detail_);
  constructor.Bind("version", &version_);
  const auto bind = [&](const char* name, std::function<void()> Actions::*action) {
    constructor.BindEventCallback(name, [this, action](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
      Run(actions_.*action);
    });
  };
  constructor.BindEventCallback("first", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    Run(game_ready_ ? actions_.play : actions_.choose_game);
  });
  bind("settings", &Actions::settings);
  bind("dlc", &Actions::dlc);
  bind("quit", &Actions::quit);
  // The mouse moves the focus, so one item is lit.
  constructor.BindEventCallback("hover", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
    if (Rml::Element* item = event.GetCurrentElement()) item->Focus();
  });
  model_ = constructor.GetModelHandle();
  return true;
}

bool SetupScreen::Show(Rml::Context& context, const State& state, Actions actions) {
  if (!CreateModel(context)) return false;
  actions_ = std::move(actions);
  game_ready_ = state.reason != SetupReason::kFirstRun && state.reason != SetupReason::kGameMissing &&
                state.reason != SetupReason::kGameNotReady;
  start_failed_ = state.reason == SetupReason::kStartFailed;
  logo_root_ = game_ready_ && state.game.status == Status::kReady ? state.game_root : std::filesystem::path{};
  disc_image_ = state.game.disc_image;
  if (logo_root_ != render_root_) logo_ready_ = false;
  first_label_ = game_ready_ ? Text("setup-play") : Text("choose");
  state_ = state;
  UpdateStatus();
  version_ = state.version.empty() ? "" : "v" + state.version;
  model_.DirtyAllVariables();
  if (!document_) document_ = context.LoadDocument("setup.rml");
  if (!document_) {
    REXLOG_WARN("[narutorise] UI: the setup document did not load");
    return false;
  }
  document_->Show(Rml::ModalFlag::None, Rml::FocusFlag::Document);
  SetGamepadHints(gamepad_hints_);
  // A failed start leads to the settings; otherwise to playing or choosing the game.
  if (Rml::Element* item = document_->GetElementById(start_failed_ ? "settings" : "first")) item->Focus(true);
  return true;
}

void SetupScreen::UpdateStatus() {
  const auto [title, detail] = StatusText(state_, files_notice_);
  status_title_ = title;
  status_detail_ = detail && state_.reason != SetupReason::kFirstRun ? detail : "";
  files_warning_ = game_ready_ && !files_notice_.empty();
}

void SetupScreen::SetFilesNotice(std::string notice) {
  files_notice_ = std::move(notice);
  if (!model_) return;
  UpdateStatus();
  for (const char* name : {"status_title", "status_detail", "files_warning"}) model_.DirtyVariable(name);
}

void SetupScreen::Hide() {
  logo_root_.clear();
  if (!document_) return;
  document_->Close();  // Its images go at the context's next update.
  document_ = nullptr;
  actions_ = {};
}
void SetupScreen::SetControlsOpen(bool open) { if(document_)document_->SetClass("controls-open",open); }

void SetupScreen::SetGamepadHints(bool gamepad) {
  gamepad_hints_ = gamepad;
  if (document_) {
    if (auto* hints = document_->GetElementById("setup-hints"))
      hints->SetInnerRML(Text(gamepad ? "setup-pad-hints" : "setup-key-hints"));
  }
}

void SetupScreen::UpdateLogo(RenderInterface& render) {
  if (render_root_ != logo_root_) {
    render.SetGameLogo({});
    render_root_ = logo_root_;
    logo_ready_ = false;
    logo_complete_ = false;
    if (model_) model_.DirtyVariable("logo_ready");
  }
  if (logo_task_.valid() && logo_task_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto image = logo_task_.get();
    // Hide/Refresh or another selection may have changed the requested origin.
    if (document_ && pending_root_ == logo_root_) {
      logo_complete_ = true;
      logo_ready_ = image.has_value();
      if (image) REXLOG_INFO("[narutorise] Setup logo: loaded {}x{} from local game", image->width, image->height);
      else REXLOG_INFO("[narutorise] Setup logo: unavailable; keeping text title");
      render.SetGameLogo(std::move(image));
      model_.DirtyVariable("logo_ready");
    }
  }
  if (document_ && !logo_root_.empty() && !logo_complete_ && !logo_task_.valid()) {
    pending_root_ = logo_root_;
    // Capture values only: closing the UI cannot leave a worker touching it.
    try {
      logo_task_ = std::async(std::launch::async, [root = logo_root_, disc = disc_image_] { return game_logo::Load(root, disc); });
    } catch (...) {
      logo_complete_ = true;
      REXLOG_WARN("[narutorise] Setup logo: worker unavailable; keeping text title");
    }
  }
}

void SetupScreen::Run(const std::function<void()>& action) {
  if (action) action();
}

}  // namespace narutorise::menu
