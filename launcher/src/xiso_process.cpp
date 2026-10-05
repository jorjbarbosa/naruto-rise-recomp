#include "xiso_process.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <thread>

namespace xiso {
namespace {

// Parses one stdout line into progress updates (best-effort, cosmetic).
// extract-xiso prints lines like:
//   extracting <name> (<size> bytes)... [OK]
//   creating <dir>/ [OK]
//   <name> (<size> bytes)          (list mode)
//   N files in <iso> total M bytes
void ParseLine(const std::string& line, bool extract_mode, Progress& progress,
              RunResult& result) {
  if (line.empty()) return;

  const std::string kExtracting = "extracting ";
  const std::string kCreating = "creating ";

  if (line.rfind(kExtracting, 0) == 0) {
    std::string name = line.substr(kExtracting.size());
    size_t cut = name.find(" (");
    if (cut != std::string::npos) name.resize(cut);
    progress.SetCurrentFile(name);
    if (extract_mode && cut != std::string::npos) progress.files_done.fetch_add(1);
    return;
  }
  if (line.rfind(kCreating, 0) == 0) {
    std::string name = line.substr(kCreating.size());
    size_t cut = name.find("/ ");
    if (cut != std::string::npos) name.resize(cut);
    progress.SetCurrentFile(name);
    return;
  }

  // Final summary: "N files in <iso> total M bytes"
  size_t files_pos = line.find(" files in ");
  size_t total_pos = line.find(" total ");
  if (files_pos != std::string::npos && total_pos != std::string::npos &&
      line.find(" bytes") != std::string::npos && total_pos > files_pos) {
    result.files_total =
        static_cast<uint32_t>(strtoul(line.substr(0, files_pos).c_str(), nullptr, 10));
    std::string bytes_str = line.substr(total_pos + 7);
    result.total_bytes = strtoull(bytes_str.c_str(), nullptr, 10);
    return;
  }

  // List mode file line: "<name> (<size> bytes)". Directory entries report
  // "(0 bytes)" and are not counted.
  if (!extract_mode && line.size() > 8 && line.compare(line.size() - 8, 8, " bytes)") == 0) {
    if (line.size() >= 12 && line.compare(line.size() - 12, 12, " (0 bytes)") != 0) {
      std::string name = line;
      size_t cut = name.rfind(" (");
      if (cut != std::string::npos) {
        name.resize(cut);
        progress.SetCurrentFile(name);
        progress.files_done.fetch_add(1);
      }
    }
  }
}

#if defined(_WIN32)

struct ReaderState {
  HANDLE read_end = nullptr;
  Progress* progress = nullptr;
  RunResult* result = nullptr;
  bool extract_mode = false;
};

void ReaderThread(ReaderState state) {
  std::string pending_output;
  std::string line;
  char buffer[4096];
  DWORD read_bytes = 0;

  for (;;) {
    if (!ReadFile(state.read_end, buffer, sizeof(buffer), &read_bytes, nullptr) ||
        read_bytes == 0) {
      break;
    }
    pending_output.append(buffer, buffer + read_bytes);
    for (;;) {
      size_t nl = pending_output.find('\n');
      if (nl == std::string::npos) break;
      line = pending_output.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      pending_output.erase(0, nl + 1);
      state.result->output += line;
      state.result->output += '\n';
      state.progress->AddLogLine(line);
      ParseLine(line, state.extract_mode, *state.progress, *state.result);
    }
  }
  // Flush a trailing line without a newline.
  if (!pending_output.empty()) {
    line = pending_output;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    state.result->output += line;
    state.result->output += '\n';
    state.progress->AddLogLine(line);
    ParseLine(line, state.extract_mode, *state.progress, *state.result);
  }
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

RunResult SpawnExtractXiso(const std::wstring& cmdline, bool extract_mode,
                           Progress& progress) {
  RunResult result;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE pipe_read = nullptr;
  HANDLE pipe_write = nullptr;
  if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
    result.error = "CreatePipe failed";
    return result;
  }
  // The parent's read end must not be inherited by the child.
  SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = pipe_write;
  si.hStdError = pipe_write;   // merge stderr into stdout
  si.hStdInput = nullptr;

  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
  cmd_buf.push_back(L'\0');

  BOOL spawned = CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!spawned) {
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    result.error = "failed to start extract-xiso.exe (Win32 error " +
                   std::to_string(GetLastError()) + ")";
    return result;
  }
  // The parent must close its write end so EOF propagates to the reader.
  CloseHandle(pipe_write);

  ReaderState reader;
  reader.read_end = pipe_read;
  reader.progress = &progress;
  reader.result = &result;
  reader.extract_mode = extract_mode;
  std::thread reader_thread(ReaderThread, reader);

  for (;;) {
    DWORD wait = WaitForSingleObject(pi.hProcess, 200);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_FAILED) {
      TerminateProcess(pi.hProcess, 1);
      break;
    }
    if (progress.cancel.load()) {
      TerminateProcess(pi.hProcess, 1);
      result.canceled = true;
      break;
    }
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  result.exit_code = static_cast<int>(exit_code);
  result.ok = !result.canceled && exit_code == 0;

  reader_thread.join();
  CloseHandle(pipe_read);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (result.canceled) {
    result.error = "canceled";
  } else if (!result.ok) {
    result.error = "extract-xiso exited with code " + std::to_string(result.exit_code);
    if (!result.output.empty()) {
      result.error += " - " + result.output;
    }
  }
  return result;
}

RunResult RunList(const std::filesystem::path& extract_xiso_exe,
                  const std::filesystem::path& iso_path, Progress& progress) {
  std::wstring cmdline = L"\"" + extract_xiso_exe.wstring() + L"\" -l -s \"" +
                         iso_path.wstring() + L"\"";
  return SpawnExtractXiso(cmdline, false, progress);
}

RunResult RunExtract(const std::filesystem::path& extract_xiso_exe,
                     const std::filesystem::path& iso_path,
                     const std::filesystem::path& dest_dir, Progress& progress) {
  std::wstring cmdline = L"\"" + extract_xiso_exe.wstring() + L"\" -x -s -d \"" +
                         dest_dir.wstring() + L"\" \"" + iso_path.wstring() + L"\"";
  return SpawnExtractXiso(cmdline, true, progress);
}

#else  // !_WIN32

RunResult RunList(const std::filesystem::path&, const std::filesystem::path&,
                  Progress&) {
  RunResult result;
  result.error = "extract-xiso is only supported on Windows builds.";
  return result;
}

RunResult RunExtract(const std::filesystem::path&, const std::filesystem::path&,
                     const std::filesystem::path&, Progress&) {
  RunResult result;
  result.error = "extract-xiso is only supported on Windows builds.";
  return result;
}

#endif  // _WIN32

bool OutputHasDefaultXex(const std::string& input) {
  std::string output(input);
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  // Root entry lines look like "\default.xex (N bytes)" (or without the
  // leading separator): anything before the name must be path separators
  // only — a directory component ("\data\default.xex") disqualifies it.
  constexpr std::string_view kXexName = "default.xex";  // 11 chars
  size_t pos = 0;
  while ((pos = output.find(kXexName, pos)) != std::string::npos) {
    const size_t end = pos + kXexName.size();
    if (end >= output.size() || output[end] == '\n' || output[end] == ' ' ||
        output[end] == '\r' || output[end] == '(') {
      size_t line_start = output.rfind('\n', pos);
      line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
      bool only_separators = true;
      for (size_t i = line_start; i < pos; ++i) {
        if (output[i] != '\\' && output[i] != '/') {
          only_separators = false;
          break;
        }
      }
      if (only_separators) return true;
    }
    pos += kXexName.size();
  }
  return false;
}

}  // namespace xiso
