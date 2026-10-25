// narutorise - ReXGlue Recompiled Project
//
// Input recording and replay for repeatable test boots.
//
// REX_NATIVE_INPUT_RECORD=<file> wraps the SDK's controller drivers (SDL, and
// the keyboard emulation of mnk_mode) and writes each device the game sees,
// every change of a device's state and every keystroke the game reads, with
// the read count of the device and the time on the input clock.
// REX_NATIVE_INPUT_REPLAY=<file> leaves the real devices out and gives the
// game the recorded ones (ReplayDriver: by input-clock time by default, by
// read count with REX_NATIVE_INPUT_REPLAY_CLOCK=reads), so the player's
// controller does not interfere. Neither the recording nor the replay depends on the window focus
// beyond what the SDK's drivers do: without the focus they read the
// controller as released, and that is what gets recorded.
//
// The input clock advances only while the game reads input, by at most 100 ms
// between two reads: a stretch without reads (a load that does not poll) does
// not shift what follows. The game is not deterministic (real time, the AI of
// the fights, load lengths), so a replay follows the recorded path, not the
// same frames.
//
// Text format, one line per event: "NRINPUT 2"; "D <id> <synthetic> <name>"
// (a device); "S <us> <read> <id> <packet> <buttons> <lt> <rt> <lx> <ly> <rx>
// <ry>" (a state, written when it changes); "K <us> <read> <id> <vk> <unicode>
// <flags> <user> <hid>" (a keystroke).
// Optional "P <us> <total device reads> <id> <port>" records preserve explicit
// native-port assignment and transfers; -1 disconnects the device. Old NRINPUT 2
// files without P records retain their original SlotAssignment behavior.

#include "input_replay.h"
#include "menu/input_gate.h"
#include "controls/runtime.h"
#include <rex/cvar.h>
#ifdef _WIN32
#include <rex/input/xinput/xinput_input_driver.h>
#endif

#include <algorithm>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/input/nop/nop_input_driver.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/logging.h>

namespace {

using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::InputDriver;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;
using rex::X_RESULT;  // The SDK's X_ERROR_* macros name them unqualified.
using rex::X_STATUS;

// Microseconds of input reads: each read adds the time since the previous
// one, at most kGapUs.
class InputClock {
 public:
  uint64_t Now() {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (started_) {
      const auto gap = std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count();
      elapsed_ += uint64_t(std::clamp<int64_t>(gap, 0, kGapUs));
    }
    started_ = true;
    last_ = now;
    return elapsed_;
  }

 private:
  static constexpr int64_t kGapUs = 100000;
  std::mutex mutex_;
  bool started_ = false;
  std::chrono::steady_clock::time_point last_;
  uint64_t elapsed_ = 0;
};

struct Pad {
  uint32_t packet = 0, buttons = 0, lt = 0, rt = 0;
  int32_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const Pad&) const = default;
};

Pad PadOf(const X_INPUT_STATE& s) {
  return {uint32_t(s.packet_number), uint32_t(uint16_t(s.gamepad.buttons)), s.gamepad.left_trigger,
          s.gamepad.right_trigger, int16_t(s.gamepad.thumb_lx), int16_t(s.gamepad.thumb_ly),
          int16_t(s.gamepad.thumb_rx), int16_t(s.gamepad.thumb_ry)};
}

void FillState(const Pad& p, X_INPUT_STATE* out) {
  std::memset(out, 0, sizeof(*out));
  out->packet_number = p.packet;
  out->gamepad.buttons = uint16_t(p.buttons);
  out->gamepad.left_trigger = uint8_t(p.lt);
  out->gamepad.right_trigger = uint8_t(p.rt);
  out->gamepad.thumb_lx = int16_t(p.lx);
  out->gamepad.thumb_ly = int16_t(p.ly);
  out->gamepad.thumb_rx = int16_t(p.rx);
  out->gamepad.thumb_ry = int16_t(p.ry);
}

class Recorder {
 public:
  explicit Recorder(const std::string& path) : out_(path, std::ios::binary | std::ios::trunc) {
    out_ << "NRINPUT 2\n";
    out_.flush();
  }
  bool ok() const { return bool(out_); }
  void Poll() {
    if (narutorise::menu::input_gate_enabled.load() && narutorise::controls::Current()->version)
      clock_.Now();
  }
  void Device(const DeviceInfo& info) {
    auto config = narutorise::controls::Current();
    const bool routing = narutorise::menu::input_gate_enabled.load() && config->version;
    int port = -1;
    if (routing) for (size_t i=0;i<config->ports.size();++i)
      if (!info.key.empty() && config->ports[i].device==info.key) port=int(i);
    std::lock_guard lock(mutex_);
    if (devices_.insert(uint64_t(info.id)).second)
      out_ << "D " << uint64_t(info.id) << ' ' << int(info.synthetic) << ' ' << info.name << '\n';
    if (routing) PortLocked(info.id,port);
    out_.flush();
  }
  void Disconnected(DeviceId id) {
    if (!narutorise::menu::input_gate_enabled.load() || !narutorise::controls::Current()->version) return;
    std::lock_guard lock(mutex_);PortLocked(id,-1);out_.flush();
  }
  // One state read of a device; state is null when the read failed.
  void State(DeviceId id, const X_INPUT_STATE* state) {
    const uint64_t t = clock_.Now();
    std::lock_guard lock(mutex_);
    const uint64_t read = state_reads_[uint64_t(id)]++;
    if (!state) return;
    const Pad pad = PadOf(*state);
    auto [it, fresh] = last_.try_emplace(uint64_t(id), pad);
    if (!fresh && it->second == pad) return;
    it->second = pad;
    out_ << "S " << t << ' ' << read << ' ' << uint64_t(id) << ' ' << pad.packet << ' ' << pad.buttons << ' '
         << pad.lt << ' ' << pad.rt << ' ' << pad.lx << ' ' << pad.ly << ' ' << pad.rx << ' ' << pad.ry << '\n';
    out_.flush();
  }
  // One keystroke read of a device; k is null when it returned none.
  void Keystroke(DeviceId id, const X_INPUT_KEYSTROKE* k) {
    const uint64_t t = clock_.Now();
    std::lock_guard lock(mutex_);
    const uint64_t read = keystroke_reads_[uint64_t(id)]++;
    if (!k) return;
    out_ << "K " << t << ' ' << read << ' ' << uint64_t(id) << ' ' << uint16_t(k->virtual_key) << ' '
         << uint16_t(k->unicode) << ' ' << uint16_t(k->flags) << ' ' << int(k->user_index) << ' ' << int(k->hid_code)
         << '\n';
    out_.flush();
  }

 private:
  void PortLocked(DeviceId id,int port) {
    auto [it,fresh]=ports_.try_emplace(uint64_t(id),port);
    if (!fresh && it->second==port) return;
    it->second=port;
    out_ << "P " << clock_.Now() << ' ' << state_reads_[uint64_t(id)]+keystroke_reads_[uint64_t(id)]
         << ' ' << uint64_t(id) << ' ' << port << '\n';
  }
  InputClock clock_;
  std::mutex mutex_;
  std::ofstream out_;
  std::set<uint64_t> devices_;
  std::map<uint64_t, Pad> last_;
  std::map<uint64_t, uint64_t> state_reads_, keystroke_reads_;
  std::map<uint64_t,int> ports_;
};

// One SDK driver with its devices, states and keystrokes recorded.
class RecordingDriver final : public InputDriver {
 public:
  RecordingDriver(std::unique_ptr<InputDriver> inner, std::shared_ptr<Recorder> recorder)
      : InputDriver(nullptr, 0), inner_(std::move(inner)), recorder_(std::move(recorder)) {
    // The input system sets the active callback on this driver; the inner one asks it.
    inner_->set_is_active_callback([this] { return is_active(); });
  }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }  // The inner driver is already set up.
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    recorder_->Poll();
    const size_t first = out.size();
    inner_->EnumerateDevices(out);
    std::set<DeviceId> present;
    for (size_t i = first; i < out.size(); ++i) {
      auto recorded=out[i];
      if(!recorded.key.empty()&&std::count_if(out.begin()+first,out.end(),[&](const auto& d){return d.key==recorded.key;})!=1)
        recorded.key.clear(); // The effective assignment rejects ambiguous identities too.
      recorder_->Device(recorded);present.insert(out[i].id);
    }
    for (auto id:devices_) if(!present.contains(id))recorder_->Disconnected(id);
    devices_=std::move(present);
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    const X_RESULT result = inner_->GetDeviceState(id, out_state);
    recorder_->State(id, result == X_ERROR_SUCCESS ? out_state : nullptr);
    return result;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags, X_INPUT_CAPABILITIES* out_caps) override {
    return inner_->GetDeviceCapabilities(id, flags, out_caps);
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override {
    return inner_->SetDeviceVibration(id, vibration);
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke) override {
    const X_RESULT result = inner_->GetDeviceKeystroke(id, flags, out_keystroke);
    recorder_->Keystroke(id, result == X_ERROR_SUCCESS ? out_keystroke : nullptr);
    return result;
  }
  void OnWindowAvailable(rex::ui::Window* window) override { inner_->OnWindowAvailable(window); }

 private:
  std::unique_ptr<InputDriver> inner_;
  std::shared_ptr<Recorder> recorder_;
  std::set<DeviceId> devices_;
};

// The recorded devices, as one driver. By default a read gets the state the
// device had at the same input-clock time. With
// REX_NATIVE_INPUT_REPLAY_CLOCK=reads a device's n-th read gets the state of
// its n-th recorded read instead; in a village test that
// drifted already in the menus (the reads before the main menu vary from boot
// to boot) and the replay ended in Fight Mode, while the clock kept the menus,
// the load and the arrival in the village.
class ReplayAssignment final : public rex::input::DeviceAssignment {
 public:
  explicit ReplayAssignment(std::function<void(uint32_t,std::vector<DeviceId>&)> resolve):resolve_(std::move(resolve)){}
  void OnDevicesChanged(const std::vector<DeviceInfo>&) override {}
  void DevicesForUser(uint32_t user,std::vector<DeviceId>& out) const override { resolve_(user,out); }
 private:
  std::function<void(uint32_t,std::vector<DeviceId>&)> resolve_;
};
class ReplayDriver final : public InputDriver {
 public:
  explicit ReplayDriver(bool by_time) : InputDriver(nullptr, 0), by_time_(by_time), lifetime_(std::make_shared<std::atomic<ReplayDriver*>>(this)) {}
  ~ReplayDriver() override { lifetime_->store(nullptr); }
  std::unique_ptr<rex::input::DeviceAssignment> Assignment() {
    if(!has_ports_)return std::make_unique<rex::input::SlotAssignment>();
    return std::make_unique<ReplayAssignment>([lifetime=lifetime_](uint32_t user,std::vector<DeviceId>& out){
      out.clear();if(auto* driver=lifetime->load())driver->ResolvePorts(user,out);
    });
  }
  bool Load(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind("NRINPUT 2", 0) != 0) {
      error = "not an input recording (NRINPUT 2)";
      return false;
    }
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::istringstream f(line);
      char kind = 0;
      f >> kind;
      if (kind == 'D') {
        uint64_t id = 0;
        int synthetic = 0;
        f >> id >> synthetic;
        std::string name;
        std::getline(f >> std::ws, name);
        DeviceInfo info;
        info.id = DeviceId(id);
        info.name = "Replay: " + name;
        info.synthetic = synthetic != 0;
        devices_.push_back(info);
        tracks_[id];
      } else if (kind == 'S') {
        uint64_t t = 0, read = 0, id = 0;
        Pad p;
        f >> t >> read >> id >> p.packet >> p.buttons >> p.lt >> p.rt >> p.lx >> p.ly >> p.rx >> p.ry;
        if (!f || !tracks_.count(id)) continue;
        tracks_[id].states.push_back({by_time_ ? t : read, p});
        end_us_ = std::max(end_us_, t);
      } else if (kind == 'P') {
        uint64_t time=0,reads=0,id=0;int user=-1;f>>time>>reads>>id>>user;
        if(!f||!tracks_.count(id)||user< -1||user>=int(narutorise::controls::kPorts))continue;
        tracks_[id].ports.push_back({by_time_?time:reads,user});has_ports_=true;end_us_=std::max(end_us_,time);
      } else if (kind == 'K') {
        uint64_t t = 0, read = 0, id = 0;
        unsigned vk = 0, unicode = 0, flags = 0, user = 0, hid = 0;
        f >> t >> read >> id >> vk >> unicode >> flags >> user >> hid;
        if (!f || !tracks_.count(id)) continue;
        X_INPUT_KEYSTROKE k{};
        k.virtual_key = uint16_t(vk);
        k.unicode = uint16_t(unicode);
        k.flags = uint16_t(flags);
        k.user_index = uint8_t(user);
        k.hid_code = uint8_t(hid);
        tracks_[id].keystrokes.push_back({by_time_ ? t : read, k});
        end_us_ = std::max(end_us_, t);
      }
    }
    if (devices_.empty()) {
      error = "no devices";
      return false;
    }
    return true;
  }
  size_t devices() const { return devices_.size(); }
  uint64_t end_us() const { return end_us_; }

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    out.insert(out.end(), devices_.begin(), devices_.end());
  }
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    const uint64_t t = clock_.Now();
    std::lock_guard lock(mutex_);
    auto it = tracks_.find(uint64_t(id));
    if (it == tracks_.end()) return X_ERROR_DEVICE_NOT_CONNECTED;
    Track& track = it->second;
    const uint64_t at = by_time_ ? t : track.state_reads++;
    while (track.next_state < track.states.size() && track.states[track.next_state].first <= at) ++track.next_state;
    if (out_state) FillState(track.next_state ? track.states[track.next_state - 1].second : Pad{}, out_state);
    NoteEnd(track.next_state == track.states.size());
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (!tracks_.count(uint64_t(id))) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {  // A standard gamepad, as the SDK's NOP driver reports it.
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
      out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return tracks_.count(uint64_t(id)) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out_keystroke) override {
    const uint64_t t = clock_.Now();
    std::lock_guard lock(mutex_);
    auto it = tracks_.find(uint64_t(id));
    if (it == tracks_.end()) return X_ERROR_DEVICE_NOT_CONNECTED;
    Track& track = it->second;
    const uint64_t at = by_time_ ? t : track.keystroke_reads++;
    if (track.next_keystroke >= track.keystrokes.size() || track.keystrokes[track.next_keystroke].first > at)
      return X_ERROR_EMPTY;
    if (out_keystroke) *out_keystroke = track.keystrokes[track.next_keystroke].second;
    ++track.next_keystroke;
    return X_ERROR_SUCCESS;
  }

 private:
  struct Track {
    std::vector<std::pair<uint64_t, Pad>> states;  // By read or input-clock time.
    std::vector<std::pair<uint64_t, X_INPUT_KEYSTROKE>> keystrokes;
    size_t next_state = 0, next_keystroke = 0;
    uint64_t state_reads = 0, keystroke_reads = 0;
    std::vector<std::pair<uint64_t,int>> ports;
    size_t next_port=0;
    int port=-1;
  };
  void ResolvePorts(uint32_t user,std::vector<DeviceId>& out) {
    const uint64_t time=clock_.Now();std::lock_guard lock(mutex_);
    if(user>=narutorise::controls::kPorts)return;
    for(auto& [id,track]:tracks_){
      const uint64_t at=by_time_?time:track.state_reads+track.keystroke_reads;
      while(track.next_port<track.ports.size()&&track.ports[track.next_port].first<=at)
        track.port=track.ports[track.next_port++].second;
      if(track.port==int(user))out.push_back(DeviceId(id));
    }
  }
  // Logs once when every device has played its last recorded state.
  void NoteEnd(bool device_done) {
    if (ended_ || !device_done) return;
    for (const auto& [id, track] : tracks_)
      if (track.next_state < track.states.size() || track.next_port < track.ports.size()) return;
    ended_ = true;
    REXLOG_INFO("[narutorise] input replay: end of the recording ({:.1f} s recorded)", end_us_ / 1e6);
  }
  const bool by_time_;
  InputClock clock_;
  std::mutex mutex_;
  std::vector<DeviceInfo> devices_;
  std::map<uint64_t, Track> tracks_;
  uint64_t end_us_ = 0;
  bool ended_ = false;
  bool has_ports_=false;
  std::shared_ptr<std::atomic<ReplayDriver*>> lifetime_;
};

const char* Variable(const char* name) {
  const char* value = std::getenv(name);
  return value && *value ? value : nullptr;
}

std::unique_ptr<rex::input::InputSystem> NativeInput(std::shared_ptr<Recorder> recorder = {}) {
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);
  const auto add = [&](std::unique_ptr<InputDriver> driver) {
    if (driver->Setup() != X_STATUS_SUCCESS) return;
    driver = narutorise::menu::GateInput(narutorise::controls::ConfigureInput(std::move(driver)));
    if (recorder) driver = std::make_unique<RecordingDriver>(std::move(driver), recorder);
    input->AddDriver(std::move(driver));
  };
#ifdef _WIN32
  if (rex::cvar::GetFlagByName("input_backend") == "xinput")
    add(std::make_unique<rex::input::xinput::XinputInputDriver>(nullptr, 0));
  else
#endif
    add(std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0));
  add(std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0));
  input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  input->SetDeviceAssignment(narutorise::controls::ConfiguredAssignment());
  return input;
}

}  // namespace

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
  const char* record = Variable("REX_NATIVE_INPUT_RECORD");
  const char* replay = Variable("REX_NATIVE_INPUT_REPLAY");
  if (tool_mode) return rex::input::CreateDefaultInputSystem(tool_mode);
  if (!record && !replay) return narutorise::menu::input_gate_enabled.load() ? NativeInput() : rex::input::CreateDefaultInputSystem(tool_mode);
  auto input = std::make_unique<rex::input::InputSystem>(nullptr);
  bool built = false;
  bool recorded_assignment = false;
  if (replay) {
    const char* clock = Variable("REX_NATIVE_INPUT_REPLAY_CLOCK");
    const bool by_time = !(clock && std::string(clock) == "reads");
    auto driver = std::make_unique<ReplayDriver>(by_time);
    std::string error;
    if (driver->Load(replay, error)) {
      REXLOG_INFO("[narutorise] input replay: {} ({} devices, {:.1f} s, by {}); the real devices are not read",
                  replay, driver->devices(), driver->end_us() / 1e6, by_time ? "time" : "read");
      auto assignment=driver->Assignment();
      input->AddDriver(narutorise::menu::GateInput(std::move(driver)));
      input->SetDeviceAssignment(std::move(assignment));recorded_assignment=true;
      built = true;
    } else {
      REXLOG_ERROR("[narutorise] input replay: {} not loaded ({}); using the real devices", replay, error);
    }
  } else {
    auto recorder = std::make_shared<Recorder>(record);
    if (recorder->ok()) {
      if (narutorise::menu::input_gate_enabled.load()) {
        REXLOG_INFO("[narutorise] input recording: {}", record);
        return NativeInput(recorder);
      }
      // The default drivers of the SDK (CreateDefaultInputSystem, SDL backend), recorded.
      auto sdl = std::make_unique<rex::input::sdl::SDLInputDriver>(nullptr, 0);
      if (sdl->Setup() == X_STATUS_SUCCESS)
        input->AddDriver(std::make_unique<RecordingDriver>(std::move(sdl), recorder));
      auto mnk = std::make_unique<rex::input::mnk::MnkInputDriver>(nullptr, 0);
      if (mnk->Setup() == X_STATUS_SUCCESS)
        input->AddDriver(std::make_unique<RecordingDriver>(std::move(mnk), recorder));
      REXLOG_INFO("[narutorise] input recording: {}", record);
      built = true;
    } else {
      REXLOG_ERROR("[narutorise] input recording: cannot write {}; not recording", record);
    }
  }
  if (!built) return narutorise::menu::input_gate_enabled.load() ? NativeInput() : rex::input::CreateDefaultInputSystem(tool_mode);
  input->AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  if(!recorded_assignment)input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  return input;
}
