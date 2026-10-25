#include "controls/capture.h"
#include <utility>
namespace narutorise::controls {
void Capture::Begin(Target target) {
  if (BlocksNavigation())
    return;
  target_ = std::move(target);
  phase_ = Phase::Waiting;
  result_.reset();
}
void Capture::Poll(bool neutral, std::optional<Input> input) {
  if (phase_ == Phase::Waiting) {
    if (neutral)
      phase_ = Phase::Armed;
    return;
  }
  if (phase_ == Phase::Suppress) {
    if (neutral)
      phase_ = Phase::Idle;
    return;
  }
  if (phase_ == Phase::Armed && input)
    Offer(std::move(*input));
}
void Capture::Offer(Input input) {
  if (Armed())
    Finish(Outcome::Bound, std::move(input));
}
void Capture::Cancel() {
  if (Active())
    Finish(Outcome::Canceled);
}
void Capture::Remove() {
  if (Active())
    Finish(Outcome::Removed);
}
void Capture::Finish(Outcome outcome, Input input) {
  result_ = Result{outcome, std::move(input)};
  phase_ = Phase::Suppress;
}
void Capture::Reset() {
  phase_ = Phase::Idle;
  result_.reset();
}
std::optional<Capture::Result> Capture::TakeResult() {
  return std::exchange(result_, {});
}
}  // namespace narutorise::controls
