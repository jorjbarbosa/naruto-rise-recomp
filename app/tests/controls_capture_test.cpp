#include "controls/capture.h"
#include <iostream>
#include <stdexcept>
using namespace narutorise::controls;
void Check(bool b, const char* why) {
  if (!b)
    throw std::runtime_error(why);
}
int main() {
  try {
    Capture capture;
    Capture::Target target{2, 0, 1, false, "fixture:pad", 7};
    for (int button : {0x2000, 0x100, 0x200, 1, 2, 4, 8, 0x40, 0x80}) {
      capture.Begin(target);
      capture.Poll(false, Capture::Input{{SourceKind::Button, 0x1000}, {}});
      Check(!capture.Armed() && !capture.TakeResult(), "initiator must release");
      capture.Poll(true);
      Check(capture.Armed(), "arm on neutral");
      capture.Poll(false, Capture::Input{{SourceKind::Button, button}, {}});
      auto result = capture.TakeResult();
      Check(
          result && result->outcome == Capture::Outcome::Bound && result->input.pad.code == button,
          "B LB RB and remaining buttons captured");
      Check(!capture.Active() && capture.BlocksNavigation(), "consume captured hold");
      capture.Poll(false);
      Check(capture.BlocksNavigation() && !capture.TakeResult(), "no repeated capture/navigation");
      capture.Poll(true);
      Check(!capture.BlocksNavigation(), "release resumes navigation");
    }
    for (int axis = 0; axis < 6; ++axis) {
      capture.Begin(target);
      capture.Poll(true);
      capture.Poll(false, Capture::Input{{SourceKind::Axis, axis, axis < 4 ? -1 : 1}, {}});
      Check(capture.TakeResult()->input.pad.code == axis, "axis capture");
      capture.Poll(true);
    }
    capture.Begin(target);
    capture.Cancel();
    Check(capture.TakeResult()->outcome == Capture::Outcome::Canceled,
          "Escape cancels before arming");
    capture.Poll(true);
    capture.Begin(target);
    capture.Poll(true);
    capture.Remove();
    Check(capture.TakeResult()->outcome == Capture::Outcome::Removed, "Delete removes");
    capture.Poll(true);
    target.keyboard = true;
    capture.Begin(target);
    capture.Poll(true);
    capture.Offer({{}, "Control+G"});
    Check(capture.TakeResult()->input.key == "Control+G", "keyboard input");
    capture.Poll(true);
    capture.Begin(target);
    capture.Poll(true);
    capture.Offer({{}, "MouseUp"});
    Check(capture.TakeResult()->input.key == "MouseUp", "mouse input");
    capture.Reset();
    Check(!capture.BlocksNavigation() && !capture.TakeResult(), "shutdown clears session");
    std::cout << "PASS: initiator release, inline session, B/LB/RB, buttons/axes/keyboard/mouse, "
                 "cancel/delete, release suppression\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
