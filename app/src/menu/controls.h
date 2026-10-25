#pragma once
#include "controls/runtime.h"
#include "controls/capture.h"
#include <rex/ui/virtual_key.h>
#include <set>
#include <RmlUi/Core/EventListener.h>
#include <functional>
#include <string>
namespace Rml {
class Context;
class ElementDocument;
}  // namespace Rml
namespace narutorise::menu {
class Controls final : public Rml::EventListener {
 public:
  bool Show(Rml::Context& context, controls::Config config,
            std::function<bool(const controls::Config&)> save, std::function<void()> close,
            std::string menu_key, bool setup);
  void Hide();
  bool Shown() const { return document_ != nullptr; }
  void Tick(bool focused);
  void Layout(int width, int height, float ratio);
  void SetGamepadHints(bool gamepad);
  bool BlocksNavigation() const { return capture_.BlocksNavigation(); }
  bool UseStickNavigation() const { return !Shown() || page_ != Deadzones; }
  void Key(rex::ui::VirtualKey key, bool down, bool repeat = false);
  void MouseButton(int button, bool down);
  void MouseMove(int x, int y, float dx, float dy);
  void ProcessEvent(Rml::Event& event) override;

 private:
  enum Page {
    Devices,
    Buttons,
    Directions,
    Triggers,
    Sticks,
    Rumble,
    Deadzones,
    Restore,
    Transfer
  };
  void Render(std::string focus = {});
  void Activate(const std::string& command, int index, int slot);
  bool Commit(controls::Config next, std::string focus = {});
  void Adjust(const std::string& field, int direction);
  std::string Bindings(const std::vector<size_t>& actions);
  std::string Number(const std::string& field, const std::string& label, double value,
                     double scale = 100);
  const controls::Device* Device() const;
  void BeginCapture(size_t action, size_t slot);
  void CaptureVisual();
  void OfferKey(const std::string& key);
  void ChooseDevice(int index);
  void ConfirmTransfer();
  void UpdatePreview();
  void FocusContent();
  void FocusSidebar();
  const char* SidebarId() const;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Context* context_ = nullptr;
  controls::Config config_;
  std::shared_ptr<const controls::Inventory> inventory_;
  size_t player_ = 0;
  Page page_ = Devices;
  bool gamepad_ = false;
  std::string menu_key_, message_;
  std::function<bool(const controls::Config&)> save_;
  std::function<void()> close_, pending_;
  controls::Capture capture_;
  std::set<rex::ui::VirtualKey> held_keys_;
  uint32_t mouse_buttons_ = 0;
  bool mouse_position_ = false;
  int mouse_x_ = 0, mouse_y_ = 0;
  std::string pending_modifier_;
  bool refresh_after_capture_ = false;
  std::string transfer_device_, transfer_name_;
  int transfer_owner_ = -1;
  uint64_t transfer_generation_ = 0, transfer_revision_ = 0;
  bool rumble_active_ = false;
};
}  // namespace narutorise::menu
