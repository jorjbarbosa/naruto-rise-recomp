#include "config.h"
#include <algorithm>
#include <charconv>
#include <cmath>
namespace narutorise::controls {
namespace {
std::string Identity(std::string key) {
  bool control = false, shift = false, alt = false;
  for (;;) {
    const auto plus = key.find('+');
    if (plus == std::string::npos)
      break;
    const auto prefix = key.substr(0, plus);
    if (prefix == "Control" || prefix == "Ctrl")
      control = true;
    else if (prefix == "Shift")
      shift = true;
    else if (prefix == "Alt")
      alt = true;
    else
      break;
    key.erase(0, plus + 1);
  }
  if (key == "NumpadEnter")
    key = "Return";
  return std::string(control ? "Control+" : "") + (shift ? "Shift+" : "") + (alt ? "Alt+" : "") +
         key;
}
}  // namespace
std::vector<std::string> Alternatives(const std::string& text) {
  std::vector<std::string> out;
  size_t at = 0;
  do {
    const auto end = text.find(',', at);
    auto value = text.substr(at, end == std::string::npos ? end : end - at);
    const auto first = value.find_first_not_of(' '), last = value.find_last_not_of(' ');
    out.push_back(first == std::string::npos ? "" : value.substr(first, last - first + 1));
    if (end == std::string::npos)
      break;
    at = end + 1;
  } while (true);
  return out;
}
std::string Join(const std::vector<std::string>& values) {
  size_t count = values.size();
  while (count && values[count - 1].empty())
    --count;
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    if (i)
      out += ',';
    out += values[i];
  }
  return out;
}
void BindKey(Port& port, size_t action, size_t slot, const std::string& key) {
  if (action >= kKeyActionCount)
    return;
  if (slot >= std::max(size_t(2), Alternatives(port.keys[action]).size()))
    return;
  if (!key.empty())
    for (auto& bind : port.keys) {
      auto alternatives = Alternatives(bind);
      for (auto& value : alternatives)
        if (Identity(value) == Identity(key))
          value.clear();
      bind = Join(alternatives);
    }
  auto alternatives = Alternatives(port.keys[action]);
  alternatives.resize(std::max(alternatives.size(), slot + 1));
  alternatives[slot] = key;
  port.keys[action] = Join(alternatives);
}
bool ModifierConflict(const KeyBinds& bindings, const std::string& key, const std::string& guide) {
  const auto candidate = Identity(key);
  const auto uses = [](std::string value, const std::string& modifier) {
    for (;;) {
      const auto plus = value.find('+');
      if (plus == std::string::npos) return false;
      const auto head = value.substr(0, plus);
      if (head != "Control" && head != "Shift" && head != "Alt") return false;
      if (head == modifier) return true;
      value.erase(0, plus + 1);
    }
  };
  const auto conflict = [&](const std::string& binding) {
    for (const auto& value : Alternatives(binding)) {
      const auto existing = Identity(value);
      for (const auto* modifier : {"Control", "Shift", "Alt"}) {
        if ((candidate == modifier && uses(existing, modifier)) ||
            (existing == modifier && uses(candidate, modifier))) return true;
      }
    }
    return false;
  };
  return conflict(guide) || std::any_of(bindings.begin(), bindings.end(), conflict);
}
void Restore(Port& port) {
  auto device = port.device, name = port.device_name;
  port = Port{};
  port.device = std::move(device);
  port.device_name = std::move(name);
}
void Assign(Config& config, size_t port, const std::string& device, const std::string& name) {
  if (port >= kPorts)
    return;
  for (auto& p : config.ports)
    if (!device.empty() && p.device == device) {
      p.device.clear();
      p.device_name.clear();
    }
  config.ports[port].device = device;
  config.ports[port].device_name = name;
}
bool AvailableForAutomatic(const Config& config, const std::string& device) {
  return !device.empty() && device != kAutomaticDevice &&
         std::none_of(config.ports.begin(), config.ports.end(),
                      [&](const Port& p) { return p.device == device; });
}
std::string AutomaticPad(const Config& config, const std::vector<std::string>& keys) {
  std::string selected;
  for (const auto& key : keys)
    if (key != "keyboard" && AvailableForAutomatic(config, key) &&
        std::count(keys.begin(), keys.end(), key) == 1 && (selected.empty() || key < selected))
      selected = key;
  return selected;
}
bool Valid(const Config& config) {
  if (config.version < 0 || config.version > 1)
    return false;
  for (size_t i = 0; i < kPorts; ++i) {
    const auto& p = config.ports[i];
    if (!std::isfinite(p.sensitivity) || p.sensitivity < .01 || p.sensitivity > 10)
      return false;
    for (double d : p.deadzone)
      if (!std::isfinite(d) || d < 0 || d > .95)
        return false;
    for (double r : p.rumble)
      if (!std::isfinite(r) || r < 0 || r > 1)
        return false;
    for (auto source : p.pad) {
      if (source.sign != 1 && source.sign != -1)
        return false;
      if (source.kind == SourceKind::Axis && (source.code < 0 || source.code >= 6))
        return false;
      if (source.kind == SourceKind::Button &&
          (source.code <= 0 || source.code > 0xffff || (source.code & (source.code - 1))))
        return false;
    }
    for (size_t j = 0; j < i; ++j)
      if (!p.device.empty() && p.device == config.ports[j].device)
        return false;
  }
  return true;
}
std::string Encode(Source source) {
  if (source.kind == SourceKind::None)
    return "";
  return std::string(source.kind == SourceKind::Button ? "button:" : "axis:") +
         std::to_string(source.code) +
         (source.kind == SourceKind::Axis ? (source.sign < 0 ? "-" : "+") : "");
}
Source Decode(const std::string& text) {
  Source source;
  const bool axis = text.starts_with("axis:"), button = text.starts_with("button:");
  if (!axis && !button)
    return source;
  auto first = text.data() + (axis ? 5 : 7), end = text.data() + text.size();
  if (axis) {
    if (first == end || (end[-1] != '+' && end[-1] != '-'))
      return {};
    source.sign = end[-1] == '-' ? -1 : 1;
    --end;
  }
  auto [after, error] = std::from_chars(first, end, source.code);
  if (error != std::errc{} || after != end)
    return {};
  source.kind = axis ? SourceKind::Axis : SourceKind::Button;
  return source;
}
}  // namespace narutorise::controls
