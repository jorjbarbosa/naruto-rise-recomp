#pragma once
#include "controls/config.h"
#include <optional>
namespace narutorise::controls {
class Capture {
 public:
  struct Target {
    size_t player = 0, action = 0, slot = 0;
    bool keyboard = false;
    std::string device;
    uint64_t generation = 0;
  };
  struct Input {
    Source pad;
    std::string key;
  };
  enum class Outcome { Bound, Removed, Canceled };
  struct Result {
    Outcome outcome;
    Input input;
  };
  void Begin(Target target);
  void Poll(bool neutral, std::optional<Input> input = {});
  void Offer(Input input);
  void Cancel();
  void Remove();
  void Reset();
  bool Active() const { return phase_ == Phase::Waiting || phase_ == Phase::Armed; }
  bool Armed() const { return phase_ == Phase::Armed; }
  bool BlocksNavigation() const { return phase_ != Phase::Idle; }
  const Target& target() const { return target_; }
  std::optional<Result> TakeResult();

 private:
  enum class Phase { Idle, Waiting, Armed, Suppress };
  void Finish(Outcome outcome, Input input = {});
  Phase phase_ = Phase::Idle;
  Target target_;
  std::optional<Result> result_;
};
}  // namespace narutorise::controls
