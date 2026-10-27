#include "diagnostics/incident.h"
#include "diagnostics/channel.h"
#include "data_paths.h"
#include "gpu/common/sha256.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace narutorise::incident {
namespace fs = std::filesystem;
using Json = nlohmann::json;
bool AttachClient(const fs::path&);
namespace {
constexpr size_t kLogLimit = 2 * 1024 * 1024;
constexpr uint64_t kStallMs = 120000;
std::string Hex(uint32_t n) { std::ostringstream out; out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << n; return out.str(); }
std::string Dump(const Json& value) { return value.dump(2, ' ', false, Json::error_handler_t::replace); }
Json Parse(std::string_view text) {
  auto value = Json::parse(text, nullptr, false);
  return value.is_discarded() ? Json{{"raw", text}, {"incomplete_or_invalid", true}} : value;
}
void Save(const fs::path& directory, const Json& value) {
  if (!AtomicText(directory / "manifest.json", Dump(value)))
    std::fprintf(stderr, "Incident manifest write failed: %s\n", Utf8(directory).c_str());
}
uint64_t Bytes(const fs::path& directory) {
  uint64_t size = 0;
  std::error_code error;
  for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
       !error && it != end; it.increment(error)) {
    if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
    if (it->is_regular_file(error)) size += it->file_size(error);
  }
  return size;
}
bool Alive(uint32_t pid) {
#ifdef _WIN32
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
  const bool live = WaitForSingleObject(process, 0) == WAIT_TIMEOUT; CloseHandle(process); return live;
#else
  return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}
#ifdef _WIN32
std::wstring Quote(const std::wstring& value) {
  std::wstring out = L"\""; size_t slashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') { ++slashes; continue; }
    out.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
    if (c == L'"') out += L'\\'; out += c;
  }
  out.append(slashes * 2, L'\\'); return out + L'"';
}
bool Start(const fs::path& exe, const std::vector<std::string>& args, PROCESS_INFORMATION& process,
           HANDLE output = nullptr) {
  std::wstring command = Quote(exe.wstring());
  for (size_t i = 1; i < args.size(); ++i) { command += L' '; command += Quote(fs::u8path(args[i]).wstring()); }
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
  HANDLE input = nullptr;
  std::vector<unsigned char> attributes;
  if (output) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) return false;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdError = startup.StartupInfo.hStdOutput = output;
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size); attributes.resize(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) { CloseHandle(input); return false; }
    HANDLE handles[]{input, output};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof(handles), nullptr, nullptr)) {
      DeleteProcThreadAttributeList(startup.lpAttributeList); CloseHandle(input); return false;
    }
  }
  const bool ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, output != nullptr,
      CREATE_NO_WINDOW | (output ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
      &startup.StartupInfo, &process) != 0;
  if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (input) CloseHandle(input);
  return ok;
}
int DumpWorker(uint32_t pid, const fs::path& path) {
  HANDLE target = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
  if (!target) return int(GetLastError());
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) { CloseHandle(target); return int(GetLastError()); }
  HMODULE library = LoadLibraryW(L"dbghelp.dll");
  using Writer = decltype(&MiniDumpWriteDump);
  auto write = library ? reinterpret_cast<Writer>(GetProcAddress(library, "MiniDumpWriteDump")) : nullptr;
  const bool ok = write && write(target, pid, file, MINIDUMP_TYPE(MiniDumpWithThreadInfo |
      MiniDumpWithUnloadedModules), nullptr, nullptr, nullptr);
  const int result = ok ? 0 : int(GetLastError() ? GetLastError() : ERROR_PROC_NOT_FOUND);
  CloseHandle(file); CloseHandle(target); if (library) FreeLibrary(library); return result;
}
#endif
Json Snapshot(uint32_t pid, const fs::path& directory, const fs::path& executable, uint32_t sequence) {
  const std::string name = "snapshot-" + std::to_string(sequence) + ".dmp";
  const fs::path path = directory / name;
#ifdef _WIN32
  PROCESS_INFORMATION process{};
  const std::vector<std::string> args{Utf8(executable), "--incident-dump", std::to_string(pid), Utf8(path)};
  if (!Start(executable, args, process)) return Json{{"present", false}, {"reason", "snapshot-worker-start-failed"}, {"error", GetLastError()}};
  CloseHandle(process.hThread);
  if (WaitForSingleObject(process.hProcess, 15000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 1000); CloseHandle(process.hProcess);
    return Json{{"present", false}, {"reason", "snapshot-worker-timeout"}};
  }
  DWORD status = 0; GetExitCodeProcess(process.hProcess, &status); CloseHandle(process.hProcess);
  return Json{{"present", status == 0}, {"path", name}, {"error", status}, {"kind", "live-thread-snapshot"}};
#else
  (void)executable;
  // gdb remains optional. Native exception capture uses Crashpad; this is the
  // explicit live snapshot path, with a recorded limitation when unavailable.
  if (!fs::is_regular_file("/usr/bin/gdb")) return Json{{"present", false}, {"reason", "gdb-unavailable"}};
  const std::string core = Utf8(directory / ("snapshot-" + std::to_string(sequence) + ".core"));
  const std::string target = std::to_string(pid), command = "generate-core-file snapshot-" + std::to_string(sequence) + ".core";
  const pid_t worker = fork();
  if (worker == 0) {
    if (chdir(directory.c_str())) _exit(126);
    const struct rlimit limit{128ull * 1024 * 1024, 128ull * 1024 * 1024}; setrlimit(RLIMIT_FSIZE, &limit);
    const int null = open("/dev/null", O_RDWR); dup2(null, 1); dup2(null, 2);
    execl("/usr/bin/gdb", "gdb", "--batch", "--nx", "-p", target.c_str(), "-ex", command.c_str(), "-ex", "detach", nullptr);
    _exit(127);
  }
  if (worker < 0) return Json{{"present", false}, {"reason", "snapshot-fork-failed"}, {"error", errno}};
  int status = 0; const auto deadline = Now() + 15000;
  while (waitpid(worker, &status, WNOHANG) == 0) {
    if (Now() >= deadline) { kill(worker, SIGKILL); waitpid(worker, &status, 0); return Json{{"present", false}, {"reason", "snapshot-worker-timeout"}}; }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return Json{{"present", WIFEXITED(status) && WEXITSTATUS(status) == 0 && fs::exists(core)},
              {"path", fs::path(core).filename().string()}, {"wait_status", status},
              {"reason", WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "captured" : "gdb-failed-or-core-size-limit"},
              {"kind", "live-thread-snapshot"}};
#endif
}
void Artifacts(const fs::path& directory, Json& manifest) {
  // Own the local evidence independently of Crashpad's upload/cache pruning.
  // Its database remains private transport state and is not the exported copy.
  std::error_code copy_error;
  const auto capture = directory / "capture";
  if (fs::is_directory(capture, copy_error)) {
    for (fs::recursive_directory_iterator it(capture, fs::directory_options::skip_permission_denied, copy_error), end;
         !copy_error && it != end; it.increment(copy_error)) {
      if (it->is_symlink(copy_error)) { it.disable_recursion_pending(); continue; }
      if (!it->is_regular_file(copy_error) || it->path().extension() != ".dmp") continue;
      const auto destination = directory / "native-dumps" / it->path().filename();
      if (fs::exists(destination, copy_error)) continue;
      fs::create_directories(destination.parent_path(), copy_error);
      auto temporary = destination; temporary += ".partial";
      if (!copy_error) fs::copy_file(it->path(), temporary, fs::copy_options::none, copy_error);
      if (!copy_error) fs::rename(temporary, destination, copy_error);
      if (copy_error) {
        manifest["dump_copy_error"] = copy_error.message(); copy_error.clear();
      }
    }
  }
  manifest["artifacts"] = Json::array(); bool dump = false;
  std::error_code error;
  for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
       !error && it != end; it.increment(error)) {
    if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
    if (it->is_directory(error) && it.depth() == 0 && it->path().filename() == "capture") {
      it.disable_recursion_pending(); continue;
    }
    if (!it->is_regular_file(error)) continue;
    const auto name = it->path().filename().string();
    const auto extension = it->path().extension();
    if (extension != ".dmp" && extension != ".core" && name != "events.jsonl" && name != "stderr.log" && name != "session.log" && name != "guest-context.json") continue;
    const bool crash = extension == ".dmp" && name.rfind("snapshot-", 0) != 0;
    const auto hash = gpu::Sha256File(it->path());
    dump |= crash && !hash.empty();
    const auto relative_name = fs::relative(it->path(), directory).generic_u8string();
    manifest["artifacts"].push_back(Json{{"path", std::string(relative_name.begin(), relative_name.end())},
        {"size", it->file_size(error)}, {"sha256", hash}, {"present", !hash.empty()},
        {"error", hash.empty() ? Json("artifact-read-failed") : Json(nullptr)}, {"exception_dump", crash}});
  }
  manifest["exception_dump_present"] = dump;
  if (!dump) manifest["dump_absent_reason"] = manifest["collector"] == "ready"
      ? "No exception dump observed; explicit exit, hang or external termination may not run a crash handler"
      : "Exception collector was not confirmed ready; see collector events";
}
}

void Prune(const fs::path& reports) {
  struct Candidate { fs::path path; fs::file_time_type time; uint64_t bytes; };
  std::vector<Candidate> candidates; uint64_t total = 0;
  std::error_code error;
  for (fs::directory_iterator it(reports, error), end; !error && it != end; it.increment(error)) {
    if (it->is_symlink(error) || !it->is_directory(error) || it->path().filename().string().rfind("session-", 0) != 0) continue;
    auto manifest = Parse(ReadText(it->path() / "manifest.json", 1024 * 1024));
    if (!manifest.is_object() || manifest.value("schema_version", 0) != 1) continue;
    const uint32_t owner = manifest.value("supervisor_pid", 0u);
    if (owner && Alive(owner)) continue; // Never delete an active capture, even at the quota.
    if (manifest.value("state", "") != "complete") {
      manifest["state"] = "complete";
      manifest["classification"] = "interruption-undetermined";
      manifest["supervisor_observation"] = "previous supervisor is no longer alive; final exit status unavailable";
      Artifacts(it->path(), manifest); Save(it->path(), manifest);
    }
    const auto size = Bytes(it->path()); total += size;
    candidates.push_back({it->path(), it->last_write_time(error), size});
  }
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
  size_t count = candidates.size();
  for (const auto& item : candidates) {
    if (count <= 32 && total <= 256ull * 1024 * 1024 && fs::file_time_type::clock::now() - item.time <= std::chrono::hours(24 * 14)) continue;
    // Only direct children accepted above. Symlinks never enter this list.
    const auto root = fs::weakly_canonical(reports, error);
    if (error) continue;
    const auto target = fs::weakly_canonical(item.path, error);
    if (error || target.parent_path() != root) continue;
    fs::remove_all(target, error);
    if (!error) { --count; total -= item.bytes; }
  }
}

int RunSupervised(std::vector<std::string> arguments, const fs::path& executable,
                  const std::function<int(std::vector<std::string>&)>& run) {
  if (arguments.size() == 4 && arguments[1] == "--incident-export") {
    std::string error;
    if (Export(fs::u8path(arguments[2]), fs::u8path(arguments[3]), error)) return 0;
    std::fprintf(stderr, "Incident export: %s\n", error.c_str()); return 2;
  }
  if (arguments.size() == 3 && arguments[1] == "--incident-request-snapshot") {
    const auto path = fs::u8path(arguments[2]);
    const auto manifest = Parse(ReadText(path / "manifest.json", 1024 * 1024));
    if (!manifest.is_object() || manifest.value("state", "") != "running") return 2;
    return AtomicText(path / "snapshot.request", "manual\n") ? 0 : 2;
  }
#ifdef _WIN32
  if (arguments.size() == 4 && arguments[1] == "--incident-dump")
    return DumpWorker(uint32_t(std::stoul(arguments[2])), fs::u8path(arguments[3]));
#endif
  constexpr std::string_view prefix = "--incident-child=";
  for (size_t i = 1; i < arguments.size(); ++i) {
    if (!arguments[i].starts_with(prefix)) continue;
    const auto path = fs::u8path(arguments[i].substr(prefix.size()));
    arguments.erase(arguments.begin() + std::ptrdiff_t(i));
    if (!AttachClient(path)) { std::fputs("Cannot attach incident supervisor channel\n", stderr); return 3; }
#ifndef _WIN32
    prctl(PR_SET_PTRACER, getppid(), 0, 0, 0);
#endif
    try {
      const int result = run(arguments);
      if (result == 0) ExpectedExit("application-return");
      else FirstFailure("application-startup-return", std::to_string(result));
      return result;
    } catch (const std::exception& error) {
      FirstFailure("unhandled-cpp-exception", error.what()); throw;
    } catch (...) { FirstFailure("unhandled-cpp-exception", "non-std exception"); throw; }
  }
  auto environment = data::CaptureEnvironment(executable.parent_path());
  if (!environment) { std::fprintf(stderr, "Diagnostics bootstrap: %s\n", environment.error.c_str()); return run(arguments); }
  data::Overrides cli;
  for (size_t i = 1; i < arguments.size(); ++i) {
    for (auto [name, target] : {std::pair{"data_root", &cli.data_root}, std::pair{"user_data_root", &cli.user_data_root}, std::pair{"cache_root", &cli.cache_root}}) {
      const std::string flag = "--" + std::string(name);
      if (arguments[i].starts_with(flag + "=")) *target = arguments[i].substr(flag.size() + 1);
      else if (arguments[i] == flag && i + 1 < arguments.size()) *target = arguments[i + 1];
    }
  }
  const auto paths = data::ResolvePaths(*environment.value, cli);
  if (!paths) { std::fprintf(stderr, "Diagnostics bootstrap: %s\n", paths.error.c_str()); return run(arguments); }
  try {
    const auto root = paths.value->data_root / "incidents";
    fs::create_directories(root);
    Prune(root);
    const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto directory = root / ("session-" + std::to_string(wall) + "-" + std::to_string(Pid()));
    if (!fs::create_directory(directory)) throw std::runtime_error("Session directory already exists");
#ifndef _WIN32
    fs::permissions(directory, fs::perms::owner_all);
#endif
    Mapping mapping;
    if (!mapping.Open(directory / "channel.bin", true)) throw std::runtime_error("Cannot create diagnostics channel");
    return Observe(mapping, directory, executable, arguments);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Local incident collection degraded: %s\n", error.what());
#ifdef _WIN32
    MessageBoxW(nullptr, L"Local diagnostics could not start. Check write access to the selected data folder. The game will continue without the incident supervisor.", L"Naruto Rise diagnostics", MB_OK | MB_ICONWARNING);
#endif
    return run(arguments);
  }
}

int Observe(Mapping& mapping, const fs::path& directory, const fs::path& executable,
            const std::vector<std::string>& arguments) {
  Channel& channel = *mapping.channel;
  Json manifest{{"schema_version", 1}, {"diagnostics_channel_version", kChannelVersion}, {"session_id", Utf8(directory.filename())},
      {"incident_id", Utf8(directory.filename())}, {"supervisor_pid", Pid()}, {"state", "starting"},
      {"started_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
      {"executable_sha256", gpu::Sha256File(executable)}, {"binary", Utf8(executable.filename())},
      {"collector", "not-confirmed"}, {"metadata", Json::object()}, {"operations_in_progress", Json::object()},
      {"snapshots", Json::array()}, {"transport", {{"state", "not-initialized"}}}};
  Save(directory, manifest);
  auto child_arguments = arguments;
  child_arguments.push_back("--incident-child=" + Utf8(directory));
#ifdef _WIN32
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &security, 0)) throw std::runtime_error("Cannot create diagnostic stderr pipe");
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  PROCESS_INFORMATION process{};
  const bool started = Start(executable, child_arguments, process, write);
  const DWORD start_error = GetLastError(); CloseHandle(write);
  if (!started) {
    CloseHandle(read); manifest["state"] = "complete"; manifest["classification"] = "process-start-failed";
    manifest["process_start_error"] = start_error; Save(directory, manifest); return 3;
  }
  CloseHandle(process.hThread); const uint32_t pid = process.dwProcessId;
#else
  int output[2];
  if (pipe(output)) throw std::runtime_error("Cannot create diagnostic stderr pipe");
  std::vector<char*> argv; for (auto& a : child_arguments) argv.push_back(a.data()); argv.push_back(nullptr);
  const pid_t pid = fork();
  if (!pid) {
    close(output[0]); dup2(output[1], STDERR_FILENO); dup2(output[1], STDOUT_FILENO); close(output[1]);
    execv(executable.c_str(), argv.data()); _exit(127);
  }
  close(output[1]);
  if (pid < 0) { close(output[0]); throw std::runtime_error("Cannot start game process"); }
  fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK);
#endif
  manifest["child_pid"] = uint32_t(pid); manifest["state"] = "running"; Save(directory, manifest);
  std::ofstream events(directory / "events.jsonl", std::ios::binary);
  std::ofstream stderr_file(directory / "stderr.log", std::ios::binary);
  uint64_t stderr_bytes = 0, cursor = 0, last_save = 0, started_at = Now(), last_progress = started_at;
  uint64_t previous_failure = 0, previous_guest = 0;
  uint64_t incomplete_cursor = uint64_t(-1), incomplete_since = 0;
  uint32_t snapshot_request = 0, snapshot_sequence = 0;
  bool stall_captured = false, exited = false;
  // The startup menu holds the runtime's start for the player: no stall while
  // it shows, and the operations it held count from its close.
  bool setup_open = false;
  uint64_t setup_closed_at = 0;
  std::array<uint64_t, 5> counts{};
  int exit_status = 0;
  auto append = [&](Json event) { events << event.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n'; events.flush(); };
  append(Json{{"operation", "bootstrap"}, {"state", "supervised"}, {"thread", 0}, {"time_ms", started_at}});
  for (;;) {
    char buffer[8192];
    // Always drain, even after reaching the retention limit. A child writing
    // stderr must never deadlock on a full pipe.
    for (int chunks = 0; chunks < 64; ++chunks) {
#ifdef _WIN32
      DWORD available = 0, size = 0;
      if (!PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) || !available ||
          !ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &size, nullptr) || !size) break;
#else
      const ssize_t size = ::read(output[0], buffer, sizeof(buffer)); if (size <= 0) break;
#endif
      const size_t keep = std::min(size_t(size), kLogLimit - std::min(stderr_bytes, uint64_t(kLogLimit)));
      stderr_file.write(buffer, std::streamsize(keep)); stderr_bytes += uint64_t(size);
    }
    const uint64_t next = channel.next.load(std::memory_order_acquire);
    if (next > cursor + kSlots) { manifest["events_overwritten"] = manifest.value("events_overwritten", uint64_t(0)) + next - cursor - kSlots; cursor = next - kSlots; }
    for (; cursor < next; ++cursor) {
      uint64_t version = 0, time = 0, thread = 0; std::string text;
      const auto& slot = channel.events[cursor % kSlots];
      if (!ReadSlot(slot, version, time, thread, text) || version != (cursor + 1) * 2) {
        if (incomplete_cursor != cursor) { incomplete_cursor = cursor; incomplete_since = Now(); }
        if (exited || Now() - incomplete_since > 200) {
          manifest["events_incomplete"] = manifest.value("events_incomplete", uint64_t(0)) + 1;
          continue; // Ignore an incomplete slot without losing later evidence.
        }
        break;
      }
      auto value = Parse(text); value["time_ms"] = time; value["thread"] = thread; value["sequence"] = cursor;
      append(value);
      if (value.contains("operation") && value.contains("state")) {
        const auto operation_id = value.value("operation_id", uint64_t(0));
        const std::string key = operation_id ? "id:" + std::to_string(operation_id) :
            std::to_string(thread) + ":" + value["operation"].get<std::string>();
        const auto state = value["state"].get<std::string>();
        manifest["last_phase"] = value;
        if (state == "begin") manifest["operations_in_progress"][key] = value;
        else if (state == "end" || state == "failed") manifest["operations_in_progress"].erase(key);
        else if (state == "metadata") manifest["metadata"][value["operation"].get<std::string>()] = value["detail"];
        if (value["operation"] == "transport") manifest["transport"] = value;
        if (value["operation"] == "setup-screen") {
          setup_open = state == "begin";
          if (!setup_open) setup_closed_at = time;
        }
      }
      last_progress = Now();
    }
    for (const auto& [slot, field, previous] : {std::tuple{&channel.first_failure, "first_failure", &previous_failure},
                                               std::tuple{&channel.guest_fault, "guest_context", &previous_guest}}) {
      uint64_t version = 0, time = 0, thread = 0; std::string text;
      if (ReadSlot(*slot, version, time, thread, text) && version != *previous) {
        *previous = version;
        manifest[field] = Json{{"data", Parse(text)}, {"time_ms", time}, {"host_thread", thread}};
        if (std::string_view(field) == "guest_context") {
          std::istringstream input(text); std::array<uint64_t, 40> words{}; bool valid = true;
          for (auto& word : words) { input >> std::hex >> word; if (!input) { valid = false; break; } }
          if (valid) {
            Json context{{"native_pc", words[0]}, {"fault_address", words[1]}, {"sdk_exception_kind", words[2]},
                {"guest_thread", words[3]}, {"guest_memory_base", words[4]}, {"lr", words[5]}, {"ctr", words[6]},
                {"guest_context_available", words[4] != 0},
                {"register_origin", "published PPCContext; compiler-local values are not reconstructed"},
                {"access_operation", words[39] == 1 ? "read" : words[39] == 2 ? "write" : "unknown"},
                {"fatal_correlation", "not-confirmed; match native PC and host thread against the exception dump"},
                {"guest_stack_bounds", "not read on the fatal path; r1/r13 and memory base are provided"},
                {"lr_is_native_pc", false}, {"gpr", Json::array()}};
            for (size_t i = 7; i < 39; ++i) context["gpr"].push_back(words[i]);
            manifest[field]["data"] = context;
            AtomicText(directory / "guest-context.json", Dump(context));
          }
        }
        append(Json{{"operation", field}, {"state", "captured"}, {"time_ms", time}, {"thread", thread}, {"data", Parse(text)}});
        Save(directory, manifest);
      }
    }
    constexpr const char* roles[]{"ui", "guest", "translator", "compiler", "gpu"};
    for (size_t i = 0; i < counts.size(); ++i) {
      const auto current = channel.counts[i].load(std::memory_order_relaxed);
      if (current != counts[i]) { last_progress = Now(); counts[i] = current; }
      manifest["progress"][roles[i]] = Json{{"count", current}, {"last_time_ms", channel.progress[i].load(std::memory_order_relaxed)}};
    }
    manifest["collector"] = channel.collector.load(std::memory_order_acquire) == 1 ? "ready" :
        channel.collector.load(std::memory_order_acquire) == 2 ? "degraded" : "not-confirmed";
    if (exited) break; // Drain publications made immediately before death.
    const uint32_t request = channel.snapshot.load(std::memory_order_acquire);
    std::error_code file_error;
    const bool manual = request != snapshot_request || fs::exists(directory / "snapshot.request", file_error);
    // No timeout kills the game. This is a diagnostic snapshot of suspected
    // inactivity, not a causal assertion. UI heartbeats alone do not clear a
    // stuck compiler/translator: each active operation retains its own start.
    bool stale_operation = false;
    for (size_t i = 0; i < counts.size(); ++i) {
      const auto active = channel.active[i].load(std::memory_order_acquire);
      const auto since = channel.active_since[i].load(std::memory_order_acquire);
      manifest["progress"][roles[i]]["active"] = active;
      if (active && since && Now() - std::max(since, channel.progress[i].load(std::memory_order_relaxed)) > kStallMs)
        stale_operation = true;
    }
    for (const auto& operation : manifest["operations_in_progress"]) {
      auto time = std::max(operation.value("time_ms", Now()), setup_closed_at);
      if (operation.value("operation", "") == "pipeline-precompile")
        time = std::max(time, channel.progress[3].load(std::memory_order_relaxed));
      stale_operation |= Now() - time > kStallMs;
    }
    const bool stalled = !stall_captured && !setup_open && (stale_operation || Now() - last_progress > kStallMs);
    if (manual || stalled) {
      snapshot_request = request; if (stalled) stall_captured = true;
      fs::remove(directory / "snapshot.request", file_error);
      auto result = Snapshot(uint32_t(pid), directory, executable, ++snapshot_sequence);
      result["trigger"] = manual ? "manual" : "suspected-stall";
      result["time_ms"] = Now(); manifest["snapshots"].push_back(result);
      append(Json{{"operation", "live-snapshot"}, {"state", "result"}, {"detail", result}});
      Save(directory, manifest);
    }
    if (Now() - last_save >= 1000) {
      manifest["events_dropped"] = channel.dropped.load(); manifest["events_truncated"] = channel.truncated.load();
      manifest["stderr_truncated"] = stderr_bytes > kLogLimit; Save(directory, manifest); last_save = Now();
    }
#ifdef _WIN32
    exited = WaitForSingleObject(process.hProcess, 100) == WAIT_OBJECT_0;
    if (exited) { DWORD code = 0; GetExitCodeProcess(process.hProcess, &code); exit_status = int(code); }
#else
    const auto waited = waitpid(pid, &exit_status, WNOHANG);
    exited = waited == pid;
    if (!exited) std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
  }
  // One final bounded drain after death: no inherited pipe writer can keep
  // the supervisor alive indefinitely.
  for (int n = 0; n < 256; ++n) {
    char buffer[8192];
#ifdef _WIN32
    DWORD available = 0, size = 0;
    if (!PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) || !available || !ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &size, nullptr)) break;
#else
    const ssize_t size = ::read(output[0], buffer, sizeof(buffer)); if (size <= 0) break;
#endif
    const auto keep = std::min(size_t(size), kLogLimit - std::min(stderr_bytes, uint64_t(kLogLimit)));
    stderr_file.write(buffer, std::streamsize(keep)); stderr_bytes += uint64_t(size);
  }
#ifdef _WIN32
  CloseHandle(read); CloseHandle(process.hProcess); const uint32_t raw_exit = uint32_t(exit_status);
#else
  close(output[0]); manifest["wait_status"] = exit_status;
  manifest["signal"] = WIFSIGNALED(exit_status) ? Json(WTERMSIG(exit_status)) : Json(nullptr);
  const uint32_t raw_exit = WIFEXITED(exit_status) ? uint32_t(WEXITSTATUS(exit_status)) : uint32_t(128 + WTERMSIG(exit_status));
#endif
  stderr_file.close(); events.close();
  manifest["events_write_failed"] = !events;
  manifest["stderr_write_failed"] = !stderr_file;
  // Capture may finish in Crashpad shortly after the client dies. The wait is
  // bounded and never depends on a network response.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  const auto log_path = fs::u8path(ReadText(directory / "log-path.txt"));
  if (!log_path.empty()) {
    std::ifstream log(log_path, std::ios::binary); std::ofstream copy(directory / "session.log", std::ios::binary);
    if (log && copy) {
      log.seekg(0, std::ios::end); const auto size = log.tellg();
      manifest["log_original_size"] = int64_t(size); manifest["log_truncated"] = size > std::streamoff(kLogLimit);
      if (size > std::streamoff(kLogLimit)) log.seekg(size - std::streamoff(kLogLimit)); else log.seekg(0);
      copy << log.rdbuf();
    } else manifest["log_error"] = "session-log-unavailable";
  } else manifest["log_error"] = "session-log-not-published";
  manifest["state"] = "complete"; manifest["raw_exit_code"] = raw_exit; manifest["raw_exit_hex"] = Hex(raw_exit);
  manifest["duration_ms"] = Now() - started_at;
  manifest["expected_exit_requested"] = channel.expected_exit.load() != 0;
  Artifacts(directory, manifest);
  if (!manifest.value("exception_dump_present", false) && (raw_exit == 0xC0000602u || raw_exit == 0xC0000409u)) {
    manifest["dump_absent_reason"] = "Status matches a Windows fast-fail family code; normal handlers may be bypassed and no WER dump was observed";
    manifest["fast_fail_capture"] = "not-observed";
  }
  manifest["classification"] = manifest.contains("first_failure") ? "explicit-failure" :
      manifest.value("exception_dump_present", false) ? "native-exception" :
      raw_exit == 0 && channel.expected_exit.load() ? "normal-exit" : "interruption-undetermined";
  manifest["events_uncollected"] = channel.next.load() - cursor;
  Save(directory, manifest);
  return int(raw_exit);
}
}  // namespace narutorise::incident
