#include "menu/controls.h"
#include "menu/text.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <rex/ui/keybinds.h>
#include <rex/cvar.h>
namespace narutorise::menu {
namespace {
std::string Escape(const std::string& value) {
  return Rml::StringUtilities::EncodeRml(value);
}
bool Within(Rml::Element* element, Rml::Element* container) {
  for (; element; element = element->GetParentNode())
    if (element == container) return true;
  return false;
}
std::string Button(const std::string& id, const std::string& label, const std::string& command,
                   int index = 0, int slot = 0, bool disabled = false,
                   const std::string& classes = "", int width = 0) {
  return "<button id='" + id + "' command='" + command + "' index='" + std::to_string(index) +
         "' slot='" + std::to_string(slot) + "' class='" + classes + (disabled ? " disabled" : "") +
         "'" + (disabled ? " disabled='disabled'" : "") +
         (width > 0 ? " style='width:" + std::to_string(width) + "dp'" : "") + ">" + Escape(label) +
         "</button>";
}
std::string Category(const std::string& id, const std::string& label, int page, bool selected,
                     bool disabled, const std::string& marker = "") {
  auto out =
      Button(id, label, "page", page, 0, disabled, selected ? "category selected" : "category");
  out.insert(out.size() - 9, "<span class='category-marker'>" +
                                 Escape(marker.empty() ? (selected ? "•" : "›") : marker) +
                                 "</span>");
  return out;
}
std::string Title(const std::string& title, const std::string& description) {
  return "<h2>" + Escape(title) + "</h2><p>" + Escape(description) + "</p>";
}
std::string ActionName(size_t action) {
  if (action < 4)
    return std::string(Text("c-button")) + " " + KeyButtonName(action);
  if (action == 8 || action == 9)
    return Text(action == 8 ? "c-left-click" : "c-right-click");
  if (action < 12)
    return KeyButtonName(action);
  constexpr TextId directions[] = {TextId::DirectionUp, TextId::DirectionDown,
                                   TextId::DirectionLeft, TextId::DirectionRight};
  return Tr(directions[(action - 12) % 4]);
}
std::string SourceName(controls::Source source) {
  if (source.kind == controls::SourceKind::None)
    return "—";
  if (source.kind == controls::SourceKind::Axis) {
    constexpr const char* axes[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    return std::string(axes[std::clamp(source.code, 0, 5)]) +
           (source.code < 4 ? (source.sign < 0 ? "−" : "+") : "");
  }
  auto defaults = controls::DefaultPad();
  for (size_t i = 0; i < 12; ++i)
    if (defaults[i] == source)
      return KeyButtonName(i);
  for (int i = 0; i < 4; ++i)
    if (source.code == (1 << i))
      return std::string("D-pad ") + ActionName(20 + i);
  return std::to_string(source.code);
}
}  // namespace
bool Controls::Show(Rml::Context& context, controls::Config config,
                    std::function<bool(const controls::Config&)> save, std::function<void()> close,
                    std::string menu_key, bool setup) {
  Hide();
  context_ = &context;
  config_ = std::move(config);
  save_ = std::move(save);
  close_ = std::move(close);
  menu_key_ = std::move(menu_key);
  inventory_ = controls::Hardware().Devices();
  player_ = 0;
  page_ = Devices;
  document_ = context.LoadDocument("controls.rml");
  if (!document_)
    return false;
  document_->SetClass("setup", setup);
  for (const char* event : {"click", "keydown", "mouseover", "focus"})
    document_->AddEventListener(event, this);
  Render("device-category");
  document_->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
  document_->GetElementById("device-category")->Focus();
  return true;
}
void Controls::Hide() {
  controls::Hardware().StopRumble();
  pending_ = {};
  capture_.Reset();
  held_keys_.clear();
  mouse_buttons_ = 0;
  mouse_position_ = false;
  pending_modifier_.clear();
  refresh_after_capture_ = false;
  rumble_active_ = false;
  if (document_) {
    for (const char* event : {"click", "keydown", "mouseover", "focus"})
      document_->RemoveEventListener(event, this);
    document_->Close();
  }
  document_ = nullptr;
  context_ = nullptr;
  save_ = {};
  close_ = {};
  message_.clear();
}
const controls::Device* Controls::Device() const {
  return inventory_ ? controls::FindDevice(*inventory_, config_.ports[player_].device) : nullptr;
}
void Controls::Tick(bool focused) {
  if (auto action = std::exchange(pending_, {}))
    action();
  if (!document_)
    return;
  if (!focused) {
    controls::Hardware().StopRumble();
    held_keys_.clear();
    mouse_buttons_ = 0;
  }
  auto inventory = controls::Hardware().Devices();
  if (inventory->revision != inventory_->revision) {
    inventory_ = std::move(inventory);
    if (capture_.BlocksNavigation())
      refresh_after_capture_ = true;
    else
      Render();
  } else
    inventory_ = std::move(inventory);
  if (page_ == Transfer) {
    auto* device = controls::FindDevice(*inventory_, transfer_device_);
    if (!device || device->generation != transfer_generation_ ||
        inventory_->revision != transfer_revision_ || transfer_owner_ < 0 ||
        config_.ports[size_t(transfer_owner_)].device != transfer_device_) {
      page_ = Devices;
      transfer_device_.clear();
      message_ = Text("c-device-changed");
      Render("device-category");
    }
  }
  if (capture_.BlocksNavigation()) {
    auto* device = controls::FindDevice(*inventory_, capture_.target().device);
    if (!focused || !device || device->generation != capture_.target().generation)
      capture_.Cancel();
    const bool neutral = focused && held_keys_.empty() && !mouse_buttons_ &&
                         std::all_of(inventory_->devices.begin(), inventory_->devices.end(),
                                     [](const auto& d) { return d.keyboard || d.raw.Neutral(); });
    std::optional<controls::Capture::Input> input;
    if (device && !device->keyboard && capture_.Armed()) {
      for (int bit = 0; bit < 16; ++bit)
        if (device->raw.buttons & (1u << bit)) {
          input = controls::Capture::Input{{controls::SourceKind::Button, 1 << bit}, {}};
          break;
        }
      if (!input)
        for (int axis = 0; axis < 6; ++axis)
          if (device->axes[axis] && std::abs(device->raw.axes[axis]) > .55) {
            input = controls::Capture::Input{
                {controls::SourceKind::Axis, axis, device->raw.axes[axis] < 0 ? -1 : 1}, {}};
            break;
          }
    }
    const bool was_armed = capture_.Armed();
    const bool was_blocking = capture_.BlocksNavigation();
    capture_.Poll(neutral, std::move(input));
    if (was_armed != capture_.Armed() || was_blocking != capture_.BlocksNavigation())
      SetGamepadHints(gamepad_);
    if (auto result = capture_.TakeResult()) {
      if (result->outcome != controls::Capture::Outcome::Canceled) {
        auto next = config_;
        auto target = capture_.target();
        auto& port = next.ports[target.player];
        if (target.keyboard)
          controls::BindKey(
              port, target.action, target.slot,
              result->outcome == controls::Capture::Outcome::Removed ? "" : result->input.key);
        else
          port.pad[target.action] = result->outcome == controls::Capture::Outcome::Removed
                                        ? controls::Source{}
                                        : result->input.pad;
        Commit(std::move(next),
               "bind-" + std::to_string(target.action) + "-" + std::to_string(target.slot));
      } else {
        message_ = device ? "" : Text("c-device-changed");
      }
      CaptureVisual();
    }
    if (!capture_.BlocksNavigation() && refresh_after_capture_) {
      refresh_after_capture_ = false;
      Render();
    }
  }
  bool active = controls::Hardware().RumbleActive();
  if (active != rumble_active_) {
    rumble_active_ = active;
    if (!active)
      message_.clear();
    Render();
  }
  UpdatePreview();
}
void Controls::Layout(int width, int height, float ratio) {
  if (!document_)
    return;
  const float w = std::max(360.0f, std::min(1040.0f, width / ratio - 48)),
              h = std::max(300.0f, std::min(586.667f, height / ratio - 32));
  auto* window = document_->GetElementById("controls-window");
  const auto dp = [](float x) { return fmt::format("{:.3f}dp", x); };
  window->SetProperty("width", dp(w));
  window->SetProperty("height", dp(h));
  window->SetProperty("left", dp((width / ratio - w) / 2));
  window->SetProperty("top", dp((height / ratio - h) / 2));
  const float left = w * .384f;
  document_->GetElementById("controls-sidebar")->SetProperty("width", dp(left - 36));
  document_->GetElementById("controls-divider")->SetProperty("left", dp(left));
  auto* detail = document_->GetElementById("controls-detail");
  detail->SetProperty("left", dp(left + 22));
  detail->SetProperty("right", "22dp");
  const auto dimensions = context_->GetDimensions();
  (void)dimensions;
}
void Controls::SetGamepadHints(bool gamepad) {
  gamepad_ = gamepad;
  if (document_) {
    const char* hint = capture_.Active()
                           ? (capture_.Armed() ? "c-capture-footer" : "c-capture-waiting")
                       : capture_.BlocksNavigation() ? "c-capture-release"
                       : gamepad ? (page_ == Deadzones ? "c-preview-pad-hints" : "c-pad-hints")
                                                     : "c-key-hints";
    std::string footer = Escape(Text(hint));
    for (const auto& [symbol, direction] : {std::pair{"←", "left"}, std::pair{"→", "right"}})
      for (auto at = footer.find(symbol); at != std::string::npos; at = footer.find(symbol, at + 1))
        footer.replace(
            at, std::string(symbol).size(),
            "<span class='key-arrow " + std::string(direction) +
                "'><span class='shaft'/><span class='tip-a'/><span class='tip-b'/></span>");
    document_->GetElementById("controls-footer")->SetInnerRML(footer);
  }
}
std::string Controls::Bindings(const std::vector<size_t>& actions) {
  const auto& port = config_.ports[player_];
  bool keyboard = port.device == "keyboard";
  std::string out = "<div class='mapping-head'><span>" + Escape(Text("c-game-command")) +
                    "</span><span>" + Escape(Text(keyboard ? "c-key-source" : "c-pad-source")) +
                    "</span></div>";
  for (size_t action : actions) {
    out += "<div class='binding-row'><span class='binding-label'>" + Escape(ActionName(action)) +
           "</span><div class='bindings'>";
    if (keyboard) {
      auto values = controls::Alternatives(port.keys[action]);
      values.resize(std::max(size_t(2), values.size()));
      for (size_t slot = 0; slot < values.size(); ++slot) {
        const bool sticks = page_ == Sticks;
        const int width =
            std::clamp(int(values[slot].size()) * 8 + (sticks ? 18 : 24),
                       sticks ? 40 : 60, sticks ? 160 : 280);
        out += Button("bind-" + std::to_string(action) + "-" + std::to_string(slot),
                      values[slot].empty() ? "—" : values[slot], "bind", int(action), int(slot),
                      !Device(), "binding", width);
      }
    } else
      out += Button("bind-" + std::to_string(action) + "-0", SourceName(port.pad[action]), "bind",
                    int(action), 0, !Device(), "binding");
    out += "</div></div>";
  }
  return out;
}
std::string Controls::Number(const std::string& field, const std::string& label, double value,
                             double scale) {
  const auto* d = Device();
  bool disabled = !d;
  if (d && field.starts_with("deadzone-")) {
    int i = field.back() - '0';
    disabled = d->keyboard || (i < 2 ? !(d->axes[i * 2] || d->axes[i * 2 + 1]) : !d->axes[i + 2]);
  }
  if (field.starts_with("rumble-"))
    disabled = !d || !d->rumble || controls::Hardware().RumbleActive();
  return "<div class='number-row'><span>" + Escape(label) + "</span><div class='number-control'>" +
         Button("minus-" + field, "−", "adjust", -1, 0, disabled, "step") + "<span id='number-" +
         field + "'>" + fmt::format("{:.0f}%", value * scale) + "</span>" +
         Button("plus-" + field, "+", "adjust", 1, 0, disabled, "step") + "</div></div>";
}
void Controls::Render(std::string focus) {
  if (!document_)
    return;
  if (focus.empty())
    if (auto* current = context_->GetFocusElement())
      focus = current->GetId();
  const auto& port = config_.ports[player_];
  const auto* device = Device();
  bool keyboard = port.device == "keyboard";
  document_->GetElementById("controls-back")->SetInnerRML("‹  " + Escape(Text("c-controls")));
  std::string tabs;
  for (size_t i = 0; i < 4; ++i)
    tabs += Button("player-" + std::to_string(i),
                   std::string(Text("c-player")) + " " + std::to_string(i + 1), "player", int(i), 0,
                   false, i == player_ ? "player selected" : "player");
  document_->GetElementById("players")->SetInnerRML(tabs);
  std::string name = device                ? (keyboard ? Text("keyboard-mouse") : device->name)
                     : port.device.empty() ? Text("c-none")
                                           : port.device_name + " · " + Text("not-connected");
  std::string side = "<div class='section'>" + Escape(Text("c-device")) + "</div>" +
                     Category("device-category", name, Devices, page_ == Devices, false);
  side += "<div class='section gap'>" + Escape(Text("c-mapping")) + "</div>";
  const std::pair<Page, const char*> categories[] = {
      {Buttons, "c-buttons"},
      {Triggers, "c-triggers"},
      {Sticks, keyboard ? "c-movement-camera" : "c-sticks"},
      {Rumble, "c-rumble"}};
  for (auto [page, key] : categories)
    side += Category("category-" + std::to_string(page), Text(key), page,
                     page_ == page || (page == Buttons && page_ == Directions),
                     !device || (page == Rumble && (!device->rumble || keyboard)));
  side += "<div class='section gap'>" + Escape(Text("c-options")) + "</div>";
  side += Category("category-deadzones", Text("c-use-deadzones"), Deadzones, page_ == Deadzones,
                   !device || keyboard, Text(port.deadzones ? "on" : "off"));
  side += Category("category-restore", Text("c-restore"), Restore, page_ == Restore, false);
  document_->GetElementById("controls-sidebar")->SetInnerRML(side);
  document_->GetElementById("controls-port")
      ->SetInnerRML(Escape(std::string(Text("c-player")) + " " + std::to_string(player_ + 1) +
                           " · " + Text("c-port-settings")));
  std::string out;
  if (page_ == Devices) {
    out = Title(Text("c-choose-device"), Text("c-device-help"));
    for (size_t i = 0; i < inventory_->devices.size(); ++i) {
      const auto& d = inventory_->devices[i];
      int owner = -1;
      for (size_t j = 0; j < 4; ++j)
        if (config_.ports[j].device == d.key)
          owner = int(j);
      std::string status = owner < 0 ? Text("c-available")
                                     : std::string(Text("c-in-use")) + " · " + Text("c-player") +
                                           " " + std::to_string(owner + 1);
      out += "<div class='device-row'>" +
             Button("device-" + std::to_string(i),
                    (d.keyboard ? Text("keyboard-mouse") : d.name) + "\n" + status, "device",
                    int(i), 0, !controls::FindDevice(*inventory_, d.key), "device-choice");
      if (d.rumble)
        out += Button("identify-" + std::to_string(i), Text("c-identify"), "identify", int(i), 0,
                      false, "identify");
      out += "</div>";
    }
    out += Button("device-none", Text("c-none"), "device", -1, 0, false, "category");
    out += "<p class='below'>" + Escape(Text("c-exclusive-help")) + "</p>";
    if (controls::Backend() == "xinput" || port.device.starts_with("sdl:session:") ||
        (device && device->transient))
      out += "<p>" + Escape(Text("c-identity-help")) + "</p>";
  } else if (page_ == Buttons || page_ == Directions) {
    out = Title(Text(page_ == Directions ? "c-directions"
                     : keyboard          ? "keyboard-mouse"
                                         : "c-buttons"),
                Text("c-bind-help"));
    out += Bindings(page_ == Directions ? std::vector<size_t>{20, 21, 22, 23, 8, 9}
                                        : std::vector<size_t>{0, 1, 2, 3, 4, 5, 11, 10});
    out += Button("mapping-more", Text(page_ == Directions ? "c-back-buttons" : "c-directions"),
                  "page", page_ == Directions ? Buttons : Directions, 0, false, "secondary below");
  } else if (page_ == Triggers) {
    out = Title(Text("c-triggers"), Text(keyboard ? "c-digital-triggers" : "c-triggers-help"));
    out += Bindings({6, 7});
    if (!keyboard) {
      out += Number("deadzone-2", "LT · " + std::string(Text("c-deadzone")), port.deadzone[2]);
      out += Number("deadzone-3", "RT · " + std::string(Text("c-deadzone")), port.deadzone[3]);
      out += Button("trigger-help", Text("c-use-deadzones"), "page", Deadzones, 0, false,
                    "secondary below");
    }
  } else if (page_ == Sticks) {
    out = Title(Text(keyboard ? "c-movement-camera" : "c-sticks"), Text("c-sticks-help"));
    out += "<div class='stick-columns'><div><div class='section'>" + Escape(Text("c-left-stick")) +
           "</div>" + Bindings({12, 13, 14, 15});
    if (!keyboard)
      out += Number("deadzone-0", Text("c-deadzone"), port.deadzone[0]);
    out += "</div><div><div class='section'>" + Escape(Text("c-right-stick")) + "</div>" +
           Bindings({16, 17, 18, 19});
    if (!keyboard)
      out += Number("deadzone-1", Text("c-deadzone"), port.deadzone[1]);
    out += "</div></div>";
    if (keyboard) {
      out += Number("sensitivity", Text("c-mouse-sensitivity"), port.sensitivity);
      out += Button("mouse-enabled",
                    std::string(Text("mouse-camera")) + ": " + Text(port.mouse ? "on" : "off"),
                    "mouse", 0, 0, false, "secondary");
    } else
      out += Button("sticks-help", Text("c-use-deadzones"), "page", Deadzones, 0, false,
                    "secondary below");
  } else if (page_ == Deadzones) {
    out = Title(Text("c-use-deadzones"), Text("c-deadzone-help"));
    out +=
        Button("deadzone-toggle",
               std::string(Text("c-apply-deadzones")) + ": " + Text(port.deadzones ? "on" : "off"),
               "deadzones", 0, 0, !device || keyboard, "category");
    out += "<div class='deadzone-layout'><div class='deadzone-adjustments'>";
    for (int i = 0; i < 4; ++i)
      out += Number("deadzone-" + std::to_string(i),
                    i < 2 ? Tr(i == 0 ? TextId::LeftStickName : TextId::RightStickName)
                          : (i == 2 ? "LT" : "RT"),
                    port.deadzone[i]);
    out +=
        "<p class='below'>" + Escape(Text("c-preview-legend")) + "</p></div><div class='graphs'>";
    if (device && !keyboard) {
      out += "<div class='stick-graphs'>";
      for (int i = 0; i < 2; ++i) {
        out += "<div class='graph-column'><div class='section'>" + std::string(i ? "RS" : "LS") +
               "</div>";
        if (device->axes[i * 2] || device->axes[i * 2 + 1])
          out += "<div class='stick-graph'><div id='zone-" + std::to_string(i) +
                 "' class='deadzone-region'/><div class='axis-horizontal'/><div "
                 "class='axis-vertical'/><div id='raw-" +
                 std::to_string(i) + "' class='raw-dot'/><div id='effective-" + std::to_string(i) +
                 "' class='effective-dot'/></div><div id='readout-" + std::to_string(i) +
                 "' class='readout'/>";
        else
          out += "<p>" + Escape(Text("c-unavailable")) + "</p>";
        out += "</div>";
      }
      out += "</div>";
      for (int i = 0; i < 2; ++i)
        if (device->axes[4 + i])
          out += "<div class='trigger-label'><span>" + std::string(i ? "RT" : "LT") +
                 "</span><span id='trigger-value-" + std::to_string(i) +
                 "'/></div><div class='trigger-graph'><div id='trigger-raw-" + std::to_string(i) +
                 "' class='trigger-input'/><div id='trigger-zone-" + std::to_string(i) +
                 "' class='trigger-zone'/><div id='trigger-effective-" + std::to_string(i) +
                 "' class='trigger-output'/></div>";
    } else
      out += "<p>" + Escape(Text("c-unavailable")) + "</p>";
    out += "</div></div>";
  } else if (page_ == Rumble) {
    out = Title(Text("c-rumble"), Text("c-rumble-help"));
    if (device && device->rumble) {
      out += Number("rumble-0", Text("c-low-motor"), port.rumble[0]);
      out += Number("rumble-1", Text("c-high-motor"), port.rumble[1]);
      out += Button("rumble-test",
                    Text(controls::Hardware().RumbleActive() ? "c-stop-test" : "c-start-test"),
                    "rumble", 0, 0, false, "primary below");
      out += "<p class='below'>" + Escape(Text("c-test-help")) + "</p>";
    } else
      out += "<p>" + Escape(Text("c-unavailable")) + "</p>";
  } else if (page_ == Restore) {
    out = Title(std::string(Text("c-restore")) + " · " + Text("c-player") + " " +
                    std::to_string(player_ + 1) + "?",
                Text("c-restore-intro"));
    out += "<div class='section below'>" + Escape(Text("c-restore-scope")) +
           "</div><p class='restore-list'>" + Escape(Text("c-restore-list")) + "</p><p>" +
           Escape(Text("c-restore-keep")) + "</p>";
    out += "<div class='confirmation restore-actions'>" +
           Button("restore-cancel", Text("cancel"), "page", Devices, 0, false, "secondary") +
           Button("restore-confirm", Text("c-restore-player"), "restore", 0, 0, false, "danger") +
           "</div>";
  } else if (page_ == Transfer) {
    out = Title(Text("c-transfer-title"), Text("c-transfer-help"));
    out += "<h2 class='below'>" + Escape(transfer_name_) + "</h2><div class='section below'>" +
           Escape(Text("c-in-use")) + "</div><h2>" +
           Escape(std::string(Text("c-player")) + " " + std::to_string(transfer_owner_ + 1)) +
           "</h2>";
    out += "<p class='below'>" +
           Escape(std::string(Text("c-transfer-detail")) + " " + Text("c-player") + " " +
                  std::to_string(player_ + 1) + ".") +
           "</p>";
    out +=
        "<div class='confirmation'>" +
        Button("transfer-cancel", Text("cancel"), "page", Devices, 0, false, "secondary") +
        Button("transfer-confirm", Text("c-transfer-confirm"), "transfer", 0, 0, false, "primary") +
        "</div>";
  }
  document_->GetElementById("controls-detail")->SetInnerRML(out);
  document_->GetElementById("controls-status")
      ->SetInnerRML(Escape(message_.empty() ? Text("c-autosave") : message_));
  document_->GetElementById("controls-status")
      ->SetClass("error", message_ == Text("save-failed") || message_ == Text("c-rumble-failed") ||
                              message_ == Text("c-reserved"));
  SetGamepadHints(gamepad_);
  auto size = context_->GetDimensions();
  Layout(size.x, size.y, context_->GetDensityIndependentPixelRatio());
  if (auto* selected = document_->GetElementById(focus))
    selected->Focus();
  else if (page_ == Transfer)
    document_->GetElementById("transfer-cancel")->Focus();
  UpdatePreview();
}
const char* Controls::SidebarId() const {
  switch (page_) {
    case Buttons: case Directions: return "category-1";
    case Triggers: return "category-3";
    case Sticks: return "category-4";
    case Rumble: return "category-5";
    case Deadzones: return "category-deadzones";
    case Restore: return "category-restore";
    default: return "device-category";
  }
}
void Controls::FocusContent() {
  // A stationary mouse must not reclaim focus after rebuilding the category.
  context_->ProcessMouseMove(-1, -1, 0);
  document_->UpdateDocument();
  // Follow document order: the tag lookup is breadth-first and would prefer
  // a footer button over bindings nested inside rows.
  const auto first = [&](auto&& self, Rml::Element* element) -> bool {
    if (element->GetTagName() == "button" && !element->HasAttribute("disabled") && element->Focus())
      return true;
    for (int i = 0; i < element->GetNumChildren(); ++i)
      if (self(self, element->GetChild(i))) return true;
    return false;
  };
  if (!first(first, document_->GetElementById("controls-detail"))) FocusSidebar();
}
void Controls::FocusSidebar() {
  controls::Hardware().StopRumble();
  context_->ProcessMouseMove(-1, -1, 0);
  auto* category = document_->GetElementById(SidebarId());
  if (!category || category->HasAttribute("disabled"))
    category = document_->GetElementById("device-category");
  category->Focus();
}
bool Controls::Commit(controls::Config next, std::string focus) {
  if (!controls::Valid(next) || !save_ || !save_(next)) {
    message_ = Text("save-failed");
    Render(std::move(focus));
    return false;
  }
  config_ = std::move(next);
  message_.clear();
  Render(std::move(focus));
  return true;
}
void Controls::Adjust(const std::string& field, int direction) {
  if (!Device())
    return;
  auto next = config_;
  auto& p = next.ports[player_];
  if (field.starts_with("rumble-") && controls::Hardware().RumbleActive())
    return;
  if (field.starts_with("deadzone-")) {
    int i = field.back() - '0';
    if (i < 0 || i >= 4)
      return;
    p.deadzone[i] = std::clamp(std::round(p.deadzone[i] * 100 + direction) / 100, 0.0, .95);
  } else if (field == "sensitivity")
    p.sensitivity = std::clamp(std::round(p.sensitivity * 100 + direction) / 100, .01, 10.0);
  else if (field.starts_with("rumble-")) {
    int i = field.back() - '0';
    if (i < 0 || i >= 2)
      return;
    p.rumble[i] = std::clamp(std::round(p.rumble[i] * 100 + direction) / 100, 0.0, 1.0);
  }
  Commit(std::move(next), (direction < 0 ? "minus-" : "plus-") + field);
}
void Controls::Activate(const std::string& command, int index, int slot) {
  if (capture_.BlocksNavigation())
    return;
  if (command == "close") {
    auto close = close_;
    if (close)
      close();
    return;
  }
  if (command == "player") {
    controls::Hardware().StopRumble();
    player_ = size_t((index + 4) % 4);
    const auto* device = Device();
    if (!device || page_ == Restore || page_ == Transfer || (page_ == Rumble && !device->rumble) ||
        (page_ == Deadzones && device->keyboard))
      page_ = Devices;
    message_.clear();
    Render(page_ == Devices ? "device-category" : "");
  } else if (command == "page") {
    controls::Hardware().StopRumble();
    page_ = Page(index);
    message_.clear();
    Render(page_ == Restore ? "restore-cancel" : "");
    FocusContent();
  } else if (command == "deadzones" || command == "mouse") {
    auto next = config_;
    if (command == "mouse")
      next.ports[player_].mouse = !next.ports[player_].mouse;
    else
      next.ports[player_].deadzones = !next.ports[player_].deadzones;
    Commit(std::move(next));
  } else if (command == "bind")
    BeginCapture(size_t(index), size_t(slot));
  else if (command == "device")
    ChooseDevice(index);
  else if (command == "transfer")
    ConfirmTransfer();
  else if (command == "restore" && page_ == Restore) {
    controls::Hardware().StopAllRumble();
    auto next = config_;
    controls::Restore(next.ports[player_]);
    if (Commit(std::move(next))) {
      page_ = Device() ? Buttons : Devices;
      Render(Device() ? "bind-0-0" : "device-category");
    }
  } else if (command == "identify" && index >= 0 && size_t(index) < inventory_->devices.size()) {
    const auto& d = inventory_->devices[size_t(index)];
    bool ok = controls::Hardware().Rumble(d.key, .35, .35, 250);
    message_ = Text(ok ? "c-identifying" : "c-rumble-failed");
    Render("identify-" + std::to_string(index));
  } else if (command == "rumble") {
    if (controls::Hardware().RumbleActive()) {
      controls::Hardware().StopRumble();
      message_.clear();
    } else if (const auto* d = Device()) {
      const auto& p = config_.ports[player_];
      message_ = Text(controls::Hardware().Rumble(d->key, p.rumble[0], p.rumble[1], 5000)
                          ? "c-testing"
                          : "c-rumble-failed");
    }
    Render("rumble-test");
  }
}
void Controls::UpdatePreview() {
  if (!document_ || page_ != Deadzones)
    return;
  auto* device = Device();
  if (!device || device->keyboard)
    return;
  const auto& port = config_.ports[player_];
  auto unfiltered = port;
  unfiltered.deadzones = false;
  const auto raw = controls::Transform(device->raw, unfiltered),
             effective = controls::Transform(device->raw, port);
  const auto percent = [](double value) { return fmt::format("{:.3f}%", value); };
  for (int i = 0; i < 2; ++i)
    if (auto* zone = document_->GetElementById("zone-" + std::to_string(i))) {
      const double size = port.deadzones ? port.deadzone[i] * 100 : 0;
      zone->SetProperty("width", percent(size));
      zone->SetProperty("height", percent(size));
      zone->SetProperty("left", percent((100 - size) / 2));
      zone->SetProperty("top", percent((100 - size) / 2));
      zone->SetProperty("border-radius", fmt::format("{:.3f}dp", size * .54));
      zone->SetProperty("display", size > 0 ? "block" : "none");
      for (int filtered = 0; filtered < 2; ++filtered) {
        auto* dot = document_->GetElementById(std::string(filtered ? "effective-" : "raw-") +
                                              std::to_string(i));
        const auto& input = filtered ? effective : raw;
        dot->SetProperty("left", percent(50 + input.axes[i * 2] * 48));
        dot->SetProperty("top", percent(50 - input.axes[i * 2 + 1] * 48));
      }
      document_->GetElementById("readout-" + std::to_string(i))
          ->SetInnerRML(fmt::format("{}% / {}%", std::lround(raw.axes[i * 2] * 100),
                                    std::lround(raw.axes[i * 2 + 1] * 100)));
    }
  for (int i = 0; i < 2; ++i)
    if (auto* value = document_->GetElementById("trigger-value-" + std::to_string(i))) {
      value->SetInnerRML(fmt::format("{:.0f}%", raw.axes[4 + i] * 100));
      document_->GetElementById("trigger-raw-" + std::to_string(i))
          ->SetProperty("width", percent(raw.axes[4 + i] * 100));
      document_->GetElementById("trigger-zone-" + std::to_string(i))
          ->SetProperty("width", percent(port.deadzones ? port.deadzone[2 + i] * 100 : 0));
      document_->GetElementById("trigger-effective-" + std::to_string(i))
          ->SetProperty("left", percent(effective.axes[4 + i] * 98));
    }
}
void Controls::ChooseDevice(int index) {
  controls::Hardware().StopRumble();
  if (index < 0) {
    auto next = config_;
    next.version = 1;
    controls::Assign(next, player_, {}, {});
    Commit(std::move(next), "device-category");
    return;
  }
  if (size_t(index) >= inventory_->devices.size())
    return;
  const auto selected = inventory_->devices[size_t(index)];
  auto now = controls::Hardware().Devices();
  auto* device = controls::FindDevice(*now, selected.key);
  if (!device || device->generation != selected.generation) {
    message_ = Text("c-device-changed");
    inventory_ = std::move(now);
    Render("device-category");
    return;
  }
  int owner = -1;
  for (size_t i = 0; i < 4; ++i)
    if (config_.ports[i].device == device->key)
      owner = int(i);
  if (owner >= 0 && size_t(owner) != player_) {
    transfer_device_ = device->key;
    transfer_name_ = device->keyboard ? Text("keyboard-mouse") : device->name;
    transfer_owner_ = owner;
    transfer_generation_ = device->generation;
    transfer_revision_ = now->revision;
    page_ = Transfer;
    Render("transfer-cancel");
    return;
  }
  controls::Hardware().StopRumble();
  auto next = config_;
  next.version = 1;
  controls::Assign(next, player_, device->key,
                   device->keyboard ? Text("keyboard-mouse") : device->name);
  Commit(std::move(next), "device-category");
}
void Controls::ConfirmTransfer() {
  auto now = controls::Hardware().Devices();
  auto* device = controls::FindDevice(*now, transfer_device_);
  if (page_ != Transfer || !device || device->generation != transfer_generation_ ||
      now->revision != transfer_revision_ || transfer_owner_ < 0 ||
      config_.ports[size_t(transfer_owner_)].device != transfer_device_) {
    page_ = Devices;
    message_ = Text("c-device-changed");
    inventory_ = std::move(now);
    Render("device-category");
    return;
  }
  controls::Hardware().StopRumble();
  auto next = config_;
  next.version = 1;
  controls::Assign(next, player_, device->key, device->name);
  // Persist both assignments together before changing either effective port.
  if (Commit(std::move(next))) {
    page_ = Devices;
    transfer_device_.clear();
    Render("device-category");
  }
}
void Controls::BeginCapture(size_t action, size_t slot) {
  auto* device = Device();
  if (!device || action >= kKeyActionCount)
    return;
  capture_.Begin({player_, action, slot, device->keyboard, device->key, device->generation});
  pending_modifier_.clear();
  message_.clear();
  CaptureVisual();
  context_->ProcessMouseMove(-1, -1, 0);
  // Clear RmlUi's activation state. Its synthetic clicks are ignored while
  // capture is active; the physical release still arms the raw capture.
  context_->ProcessKeyUp(Rml::Input::KI_RETURN, 0);
  context_->ProcessMouseButtonUp(0, 0);
}
void Controls::CaptureVisual() {
  if (!document_)
    return;
  auto target = capture_.target();
  auto* value = document_->GetElementById("bind-" + std::to_string(target.action) + "-" +
                                          std::to_string(target.slot));
  if (value) {
    std::string text;
    if (capture_.Active())
      text = Text("c-press-input");
    else if (target.keyboard) {
      auto values = controls::Alternatives(config_.ports[target.player].keys[target.action]);
      text = target.slot < values.size() ? values[target.slot] : "";
    } else
      text = SourceName(config_.ports[target.player].pad[target.action]);
    value->SetInnerRML(Escape(text.empty() ? "—" : text));
    value->SetClass("editing", capture_.Active());
    if (auto* row = value->GetParentNode()->GetParentNode())
      row->SetClass("editing", capture_.Active());
  }
  SetGamepadHints(gamepad_);
  document_->GetElementById("controls-status")
      ->SetInnerRML(Escape(message_.empty() ? Text("c-autosave") : message_));
  document_->GetElementById("controls-status")
      ->SetClass("error", !message_.empty() && message_ != Text("c-device-changed"));
}
void Controls::OfferKey(const std::string& key) {
  const auto plus = key.find_last_of('+');
  const auto base = plus == std::string::npos ? key : key.substr(plus + 1);
  if (IsReservedKey(base) || base == menu_key_) {
    message_ = Text("c-reserved");
    if (document_) {
      document_->GetElementById("controls-status")->SetInnerRML(Escape(message_));
      document_->GetElementById("controls-status")->SetClass("error", true);
    }
    return;
  }
  // Match the SDK's existing standalone-modifier policy: a modifier used as
  // a button is removed from the live chord mask, so such a chord cannot fire.
  auto proposed = config_.ports[player_];
  const auto& target = capture_.target();
  controls::BindKey(proposed, target.action, target.slot, key);
  if (controls::ModifierConflict(proposed.keys, key, rex::cvar::GetFlagByName("keybind_guide"))) {
    message_ = Text("c-modifier-conflict");
    if (document_) {
      document_->GetElementById("controls-status")->SetInnerRML(Escape(message_));
      document_->GetElementById("controls-status")->SetClass("error", true);
    }
    return;
  }
  capture_.Offer({{}, key});
}
void Controls::Key(rex::ui::VirtualKey key, bool down, bool repeat) {
  using rex::ui::VirtualKey;
  if (key == VirtualKey::kNone)
    return;
  if (down)
    held_keys_.insert(key);
  else
    held_keys_.erase(key);
  if (!capture_.Active() || repeat)
    return;
  if (down && key == VirtualKey::kEscape) {
    capture_.Cancel();
    return;
  }
  if (down && key == VirtualKey::kDelete) {
    capture_.Remove();
    return;
  }
  if (!capture_.Armed() || !capture_.target().keyboard)
    return;
  const bool modifier =
      key == VirtualKey::kShift || key == VirtualKey::kControl || key == VirtualKey::kMenu;
  if (modifier) {
    if (down)
      pending_modifier_ = rex::ui::VirtualKeyToString(key);
    else if (!pending_modifier_.empty()) {
      OfferKey(pending_modifier_);
      pending_modifier_.clear();
    }
    return;
  }
  if (down) {
    std::string name;
    if (held_keys_.contains(VirtualKey::kControl))
      name += "Control+";
    if (held_keys_.contains(VirtualKey::kShift))
      name += "Shift+";
    if (held_keys_.contains(VirtualKey::kMenu))
      name += "Alt+";
    auto base = rex::ui::VirtualKeyToString(key);
    if (base.empty())
      return;
    name += base;
    pending_modifier_.clear();
    OfferKey(name);
  }
}
void Controls::MouseButton(int button, bool down) {
  if (button < 1 || button > 5)
    return;
  if (down)
    mouse_buttons_ |= 1u << button;
  else
    mouse_buttons_ &= ~(1u << button);
  if (down && capture_.Armed() && capture_.target().keyboard) {
    constexpr const char* names[] = {"", "LMB", "RMB", "MMB", "X1MB", "X2MB"};
    OfferKey(names[button]);
  }
}
void Controls::MouseMove(int x, int y, float dx, float dy) {
  if (!mouse_position_) {
    mouse_position_ = true;
    mouse_x_ = x;
    mouse_y_ = y;
    return;
  }
  if (dx == 0 && dy == 0) {
    dx = float(x - mouse_x_);
    dy = float(y - mouse_y_);
  }
  mouse_x_ = x;
  mouse_y_ = y;
  if (capture_.Armed() && capture_.target().keyboard && capture_.target().action >= 12 &&
      capture_.target().action < 20 && std::max(std::abs(dx), std::abs(dy)) >= 6)
    OfferKey(std::abs(dx) > std::abs(dy) ? (dx < 0 ? "MouseLeft" : "MouseRight")
                                         : (dy < 0 ? "MouseUp" : "MouseDown"));
}
void Controls::ProcessEvent(Rml::Event& event) {
  if (capture_.BlocksNavigation()) {
    event.StopImmediatePropagation();
    return;
  }
  auto* target = event.GetTargetElement();
  while (target && target != document_ && !target->HasAttribute("command"))
    target = target->GetParentNode();
  if (!target)
    return;
  if (event.GetType() == "mouseover") {
    if (target->GetTagName() == "button" && !target->HasAttribute("disabled"))
      target->Focus();
    return;
  }
  if (event.GetType() == "focus") {
    target->ScrollIntoView(Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest});
    return;
  }
  if (target->HasAttribute("disabled"))
    return;
  if (event.GetType() == "keydown") {
    int key = event.GetParameter<int>("key_identifier", 0);
    if (key == Rml::Input::KI_ESCAPE) {
      const bool in_content = Within(target, document_->GetElementById("controls-detail"));
      pending_ = [this, in_content] {
        if (page_ == Restore || page_ == Transfer) {
          page_ = Devices;
          Render("device-category");
        } else if (in_content)
          FocusSidebar();
        else
          Activate("close", 0, 0);
      };
      event.StopPropagation();
    } else if (key == Rml::Input::KI_RIGHT &&
               Within(target, document_->GetElementById("controls-sidebar"))) {
      const int page = target->GetAttribute<int>("index", 0);
      pending_ = [this, page] { Activate("page", page, 0); };
      event.StopPropagation();
    } else if (key == Rml::Input::KI_PRIOR || key == Rml::Input::KI_NEXT) {
      pending_ = [this, key] {
        Activate("player", int(player_) + (key == Rml::Input::KI_NEXT ? 1 : -1), 0);
      };
      event.StopPropagation();
    } else if ((key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT) &&
               target->GetAttribute<std::string>("command", "") == "adjust") {
      auto id = target->GetId();
      auto field = id.substr(id.find('-') + 1);
      pending_ = [this, field, key] { Adjust(field, key == Rml::Input::KI_LEFT ? -1 : 1); };
      event.StopPropagation();
    }
    return;
  }
  if (event.GetType() != "click")
    return;
  std::string command = target->GetAttribute<std::string>("command", "");
  int index = target->GetAttribute<int>("index", 0), slot = target->GetAttribute<int>("slot", 0);
  if (command == "adjust") {
    auto id = target->GetId();
    auto field = id.substr(id.find('-') + 1);
    pending_ = [this, field, index] { Adjust(field, index); };
  } else
    pending_ = [this, command, index, slot] { Activate(command, index, slot); };
}
}  // namespace narutorise::menu
