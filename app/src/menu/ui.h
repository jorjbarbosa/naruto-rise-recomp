// The RmlUi of the game, on the SDK's UI thread: one context the size of the
// render target, the fonts and documents of the menus (assets.h), drawn
// through a rex::ui::ImmediateDrawer. In the native mode that is the UI
// layer's (gpu/ui) and the app draws a frame per Swap while a document is
// shown (per Present of the UI screen before the game); with the Xenos
// renderer it is the SDK's, and the presenter draws the Ui (its UIDrawer)
// while a document is shown. Sizes are in dp, one dp a pixel at 720 lines and
// more above. The documents: the frame rate, and the setup screen before the
// game, which takes the keyboard and the mouse of the window while it shows.
#pragma once

#include "menu/setup_screen.h"
#include "menu/panel.h"
#include "menu/pad_input.h"
#include "menu/controls.h"
#include "menu/text.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/window_listener.h>

namespace Rml {
class Context;
class ElementDocument;
}  // namespace Rml
namespace rex::ui {
class ImmediateDrawer;
class Presenter;
}  // namespace rex::ui

namespace narutorise::menu {
class AssetFiles;
class RenderInterface;

class Ui final : public rex::ui::UIDrawer, public rex::ui::WindowInputListener {
 public:
  Ui();
  ~Ui();
  // The UI thread. presenter: the Xenos presenter, or null in the native mode.
  bool Initialize(rex::ui::ImmediateDrawer& drawer, rex::ui::Presenter* presenter);
  void Shutdown();
  // The frame rate of the game: the frames CountFrame counts, unless the
  // source gives the rate (fps, ms) itself (the Xenos guest rate).
  void SetRateSource(std::function<bool(double& fps, double& ms)> source) { rate_source_ = std::move(source); }
  void CountFrame() { frames_.fetch_add(1, std::memory_order_relaxed); }  // Any thread.
  // The frame rate document (UI thread). It hides while the setup screen shows.
  void SetFpsVisible(bool visible);
  bool FpsVisible() const { return fps_visible_.load(); }
  // The setup screen before the game (setup_screen.h), UI thread.
  bool ShowSetup(const SetupScreen::State& state, SetupScreen::Actions actions);
  void RefreshSetup(const SetupScreen::State& state);
  void SetSetupFilesNotice(std::string notice) { setup_.SetFilesNotice(std::move(notice)); }
  void HideSetup();
  // Modal actions run after the document event. Cancel is focused by default.
  void ShowDialog(const std::string& title, const std::string& message,
                  std::vector<std::pair<std::string, std::function<void()>>> actions = {});
  void HideDialog();
  bool DialogShown() const { return dialog_shown_.load(); }
  void SetTick(std::function<void()> tick) { tick_ = std::move(tick); }
  void SetInputActive(std::function<bool()> active) { input_active_ = std::move(active); }
  void ShowPanel(std::string title, std::vector<std::string> tabs, int tab, std::vector<PanelRow> rows,
                 std::function<void(int)> select_tab, std::function<void()> close, std::string focus = {});
  void HidePanel();
  std::string PanelFocus() const { return panel_.FocusId(); }
  bool PanelShown() const { return panel_.Shown() || controls_.Shown(); }
  // UI thread: asynchronous results belong to the view that started them.
  uint64_t ViewRevision() const { return view_revision_; }
  void ShowControls(controls::Config config, std::function<bool(const controls::Config&)> save,
                    std::function<void()> close, std::string menu_key);
  void CaptureKey(std::function<void(std::string)> captured);
  void CancelKeyCapture() { key_capture_ = {}; pending_key_.clear(); }
  void EnableGameMenu(std::string key, std::string chord, std::function<void()> toggle);
  bool GameMenuEnabled() const { return game_menu_enabled_.load(); }
  void PollInput();
  void ShowOverlay(std::function<void()> settings, std::function<void()> dlc,
                   std::function<void()> quit, std::function<void()> close);
  void HideOverlay();
  bool OverlayShown() const { return overlay_shown_.load(); }
  void Notify(std::string message, std::string action_label = {}, std::function<void()> action = {}, std::string title = {});
  void ShowShortcutHint(std::string message, std::chrono::milliseconds delay = {});
  bool HintShown() const { return hint_shown_; }
  void SetShadersVisible(bool visible, uint32_t pending = 0, float progress = -1);
  // A document is shown (any thread): the native mode draws frames then.
  bool Shown() const { return fps_visible_.load() || setup_shown_.load() || panel_shown_.load() ||
                              controls_shown_.load() || overlay_shown_.load() || status_shown_.load() ||
                              dialog_shown_.load(); }
  // A frame of the UI, on the UI thread: the app's call in the native mode,
  // the presenter's with Xenos.
  void Draw(rex::ui::UIDrawContext& context) override;
  // The window's keyboard and mouse (UI thread): taken while the setup screen
  // shows, except the keys with Alt (the window's own).
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

 private:
  void UpdateDocuments();
  void UpdateRate();
  bool TakesInput() const { return context_ && (setup_.Shown() || panel_.Shown() || controls_.Shown() || dialog_ || overlay_); }
  struct MenuEvents;
  std::unique_ptr<MenuEvents> overlay_events_, status_events_;
  Rml::ElementDocument* overlay_ = nullptr;
  Rml::ElementDocument* status_ = nullptr;
  // A dialog can show alone over the game (the crash report question): its
  // answer runs on the next frame, so it keeps the frames coming too.
  std::atomic<bool> overlay_shown_{false}, status_shown_{false}, game_menu_enabled_{false}, dialog_shown_{false};
  std::function<void()> toggle_menu_;
  std::string menu_key_ = "F1";
  struct Notice { std::string text, action_label; std::function<void()> action; std::string title; };
  std::deque<Notice> notices_;
  std::chrono::steady_clock::time_point notice_until_{};
  bool notice_active_ = false, shaders_visible_ = false;
  bool notice_pressed_ = false;
  bool hint_shown_ = false, hint_pending_ = false;
  std::chrono::steady_clock::time_point hint_start_{}, hint_end_{};
  uint32_t shader_pending_ = 0;
  float shader_progress_ = -1;
  std::string shaders_text_;
  bool gamepad_input_ = false, dispatching_gamepad_ = false;
  void UseGamepad(bool gamepad);
  void UpdateStatus();
  struct DialogEvents;
  std::unique_ptr<DialogEvents> dialog_events_;
  Rml::ElementDocument* dialog_ = nullptr;
  std::function<void()> tick_;
  std::function<bool()> input_active_;
  std::function<void(std::string)> key_capture_;
  std::string pending_key_;
  std::chrono::steady_clock::time_point capture_started_{};
  std::unique_ptr<AssetFiles> files_;
  std::unique_ptr<RenderInterface> render_;
  rex::ui::Presenter* presenter_ = nullptr;
  bool in_presenter_ = false;
  Rml::Context* context_ = nullptr;
  Rml::ElementDocument* fps_ = nullptr;
  SetupScreen setup_;
  Panel panel_;
  Controls controls_;
  std::atomic<bool> controls_shown_{false};
  PadInput pad_;
  TextSystem text_;
  std::atomic<bool> panel_shown_{false};
  std::atomic<bool> fps_visible_{false}, setup_shown_{false};
  bool initialized_ = false;
  uint64_t view_revision_ = 0;
  int width_ = 0, height_ = 0;
  // The rate: the frames counted since the last update, every kRatePeriod.
  static constexpr std::chrono::milliseconds kRatePeriod{500};
  std::function<bool(double&, double&)> rate_source_;
  std::atomic<uint64_t> frames_{0};
  uint64_t rate_frames_ = 0;
  std::chrono::steady_clock::time_point rate_time_{};
};

}  // namespace narutorise::menu
