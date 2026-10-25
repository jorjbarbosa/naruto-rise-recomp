#include "controls/service.h"
#include <iostream>
int main() {
  using namespace narutorise::controls;
  auto& service = Hardware();
  service.Initialize();
  service.Poll("sdl", true);
  auto first = service.Devices();
  service.Poll("sdl", true);
  auto second = service.Devices();
  if (first->revision != second->revision || first->devices.empty() ||
      !first->devices.back().keyboard)
    return 1;
  std::cout << "Real SDL inventory: " << first->devices.size() - 1 << " gamepads\n";
  for (const auto& d : first->devices) {
    if (d.key.empty() || (d.keyboard && d.rumble))
      return 2;
    std::cout << d.name << "; rumble=" << d.rumble << "; transient=" << d.transient << '\n';
  }
  if (service.Rumble("not-a-real-device", 1, 1, 50))
    return 3;
  service.Poll("xinput", true);
  auto xinput = service.Devices();
  std::cout << "Real XInput inventory: " << xinput->devices.size() - 1 << " gamepads\n";
  for (const auto& d : xinput->devices)
    if (!d.keyboard && d.xinput < 0)
      return 4;
  service.Shutdown();
  if (!service.Devices()->devices.empty())
    return 5;
  RawState raw;
  if (!raw.Neutral())
    return 6;
  raw.axes[4] = .8;
  if (raw.Neutral())
    return 7;
  Inventory units;
  Device a, b;
  a.key = "serial:first";
  b.key = "serial:second";
  a.name = b.name = "Same model";
  units.devices = {a, b};
  if (!FindDevice(units, a.key) || FindDevice(units, "removed"))
    return 8;
  units.devices[1].key = a.key;
  if (FindDevice(units, a.key))
    return 9;
  std::cout << "PASS: actual backend inventory, stable snapshots, capabilities, invalid rumble, "
               "shutdown\n";
}
