#include "update_checker.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif

namespace launcher_updates {
namespace {
constexpr size_t kMaxResponse = 1024 * 1024;
constexpr std::string_view kReleaseRoot =
    "https://github.com/jorjbarbosa/naruto-rise-recomp/releases/";

std::int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

bool Digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int CompareNumber(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return a.size() > b.size() ? 1 : -1;
  return a == b ? 0 : a > b ? 1 : -1;
}

struct Version {
  std::array<std::string, 3> numbers;
  std::vector<std::string> pre;
};

std::optional<Version> ParseVersion(std::string_view value) {
  if (value.empty() || value.size() > 64) return {};
  if (value.front() == 'v') value.remove_prefix(1);
  static const std::regex pattern(
      R"(^([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)");
  std::smatch match;
  const std::string text(value);
  if (!std::regex_match(text, match, pattern)) return {};
  Version version;
  for (size_t i = 0; i < 3; ++i) {
    version.numbers[i] = match[i + 1].str();
    if (version.numbers[i].size() > 1 && version.numbers[i][0] == '0') return {};
  }
  const std::string pre = match[4].str();
  for (size_t start = 0; start < pre.size();) {
    const auto end = pre.find('.', start);
    auto part = pre.substr(start, end - start);
    if (Digits(part) && part.size() > 1 && part[0] == '0') return {};
    version.pre.push_back(std::move(part));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return version;
}

std::string EncodeTag(std::string_view tag) {
  std::string result;
  for (char c : tag) result += c == '+' ? "%2B" : std::string(1, c);
  return result;
}

std::string ReadBounded(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::string text(kMaxResponse + 1, '\0');
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(file.gcount()));
  return text.size() <= kMaxResponse ? text : std::string{};
}
}  // namespace

std::optional<int> CompareVersions(std::string_view left, std::string_view right) {
  const auto a = ParseVersion(left), b = ParseVersion(right);
  if (!a || !b) return {};
  for (size_t i = 0; i < 3; ++i) {
    const int order = CompareNumber(a->numbers[i], b->numbers[i]);
    if (order) return order;
  }
  if (a->pre.empty() != b->pre.empty()) return a->pre.empty() ? 1 : -1;
  for (size_t i = 0; i < std::min(a->pre.size(), b->pre.size()); ++i) {
    const auto& x = a->pre[i];
    const auto& y = b->pre[i];
    if (x == y) continue;
    const bool xn = Digits(x), yn = Digits(y);
    if (xn != yn) return xn ? -1 : 1;
    return xn ? CompareNumber(x, y) : x > y ? 1 : -1;
  }
  return a->pre.size() == b->pre.size() ? 0 : a->pre.size() > b->pre.size() ? 1 : -1;
}

std::optional<bool> IsNewerRelease(std::string_view installed, std::string_view available) {
  if (installed.starts_with('v')) installed.remove_prefix(1);
  if (available.starts_with('v')) available.remove_prefix(1);
  // Match the narrowly scoped numbering reset accepted by installer.iss.
  const auto legacy = [](auto v) {
    return v == "1.0.0-beta.1" || v == "1.0.0" || v == "1.0.1" || v == "1.0.2";
  };
  const auto renumbered = [](auto v) {
    return v == "0.1.0-beta.2" || v == "0.1.0-beta.3" ||
           v == "0.2.0-beta.1" || v == "0.2.0-beta.2" || v == "0.3.0-beta";
  };
  if (legacy(installed) && renumbered(available)) return true;
  if (renumbered(installed) && legacy(available)) return false;
  const auto order = CompareVersions(available, installed);
  return order ? std::optional<bool>(*order > 0) : std::nullopt;
}

std::optional<Release> ParseRelease(std::string_view text) {
  if (text.size() > kMaxResponse) return {};
  try {
    const auto json = nlohmann::json::parse(text);
    if (json.at("draft").get<bool>() || json.at("prerelease").get<bool>()) return {};
    const auto tag = json.at("tag_name").get<std::string>();
    if (!ParseVersion(tag)) return {};
    Release release;
    release.version = tag.starts_with('v') ? tag.substr(1) : tag;
    // Construct links from a validated tag and our fixed repository, never from
    // arbitrary HTML/URLs in the release description or a local cache file.
    release.page_url = std::string(kReleaseRoot) + "tag/" + EncodeTag(tag);
    const auto& assets = json.at("assets");
    if (!assets.is_array()) return {};
    for (const auto& asset : assets) {
      if (!asset.is_object() || asset.value("state", "") != "uploaded") continue;
      const auto name = asset.value("name", "");
      if (name != "NarutoRiseInstaller.exe" && name != "naruto-rise-recomp-win-amd64.zip") continue;
      const auto url = std::string(kReleaseRoot) + "download/" + EncodeTag(tag) + "/" + name;
      if (asset.value("browser_download_url", "") != url) continue;
      if (name == "NarutoRiseInstaller.exe") {
        release.installer_url = url;
        const auto digest = asset.value("digest", nlohmann::json{});
        const auto size = asset.value("size", nlohmann::json{});
        if (digest.is_string() && size.is_number_unsigned()) {
          const auto value = digest.get<std::string>();
          if (value.starts_with("sha256:") && value.size() == 71 &&
              std::all_of(value.begin() + 7, value.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
              }) && size.get<std::uint64_t>() > 0 && size.get<std::uint64_t>() <= 1024ULL * 1024 * 1024) {
            release.installer_sha256 = value.substr(7);
            release.installer_size = size.get<std::uint64_t>();
          }
        }
      }
      else release.portable_url = url;
    }
    return release;
  } catch (const nlohmann::json::exception&) {
    return {};
  }
}

std::optional<std::string> FetchLatestRelease() {
#if defined(_WIN32)
  struct Handle {
    HINTERNET value;
    ~Handle() { if (value) WinHttpCloseHandle(value); }
  };
  Handle session{WinHttpOpen(L"NarutoRiseLauncher-UpdateCheck/1.0",
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value || !WinHttpSetTimeouts(session.value, 3000, 3000, 3000, 3000)) return {};
  Handle connection{WinHttpConnect(session.value, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
  if (!connection.value) return {};
  Handle request{WinHttpOpenRequest(connection.value, L"GET",
      L"/repos/jorjbarbosa/naruto-rise-recomp/releases/latest", nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
  if (!request.value) return {};
  DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) return {};
  constexpr auto headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
  if (!WinHttpSendRequest(request.value, headers, -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.value, nullptr)) return {};
  DWORD status = 0, size = sizeof(status);
  if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return {};
  std::string body;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    char buffer[8192];
    DWORD read = 0;
    if (!WinHttpReadData(request.value, buffer, sizeof(buffer), &read)) return {};
    if (!read) return body;
    if (body.size() + read > kMaxResponse) return {};
    body.append(buffer, read);
  }
#endif
  return {};
}

struct Checker::Mailbox {
  std::mutex mutex;
  bool done = false;
  std::optional<std::string> payload;
};

Checker::Checker(std::string installed, std::filesystem::path cache_file, Fetch fetch)
    : installed_(std::move(installed)), cache_file_(std::move(cache_file)), fetch_(std::move(fetch)) {
  try {
    const auto cache = nlohmann::json::parse(ReadBounded(cache_file_));
    if (cache.at("schema") == 1) {
      ignored_ = cache.value("ignored", "");
      if (!ParseVersion(ignored_)) ignored_.clear();
      next_check_ = cache.value("next_check", std::int64_t{0});
      if (next_check_ > Now() + 6 * 60 * 60) next_check_ = 0;  // Clock rollback / corrupt cache.
      const auto payload = cache.value("release", nlohmann::json{}).dump();
      if (const auto release = ParseRelease(payload)) {
        payload_ = payload;
        Apply(*release);
      }
    }
  } catch (const nlohmann::json::exception&) {
    next_check_ = 0;
  }
  if (next_check_ <= Now()) Start();
}

void Checker::Apply(const Release& release) {
  release_ = release;
  const auto newer = IsNewerRelease(installed_, release.version);
  status_ = !newer ? Status::UnknownVersion : *newer ? Status::Available : Status::Current;
}

bool Checker::CanCheck() const {
  return !pending_ && std::chrono::steady_clock::now() >= manual_after_;
}

void Checker::CheckNow() {
  if (CanCheck()) Start();
}

void Checker::Start() {
  status_ = Status::Checking;
  manual_after_ = std::chrono::steady_clock::now() + std::chrono::minutes(1);
  next_check_ = Now() + 15 * 60;
  payload_.clear();
  Save();
  pending_ = std::make_shared<Mailbox>();
  try {
    std::thread([mailbox = pending_, fetch = fetch_] {
      std::optional<std::string> payload;
      try { payload = fetch(); } catch (...) { /* Network failure is nonfatal. */ }
      std::lock_guard lock(mailbox->mutex);
      mailbox->payload = std::move(payload);
      mailbox->done = true;
    }).detach();
  } catch (const std::system_error&) {
    pending_.reset();
    status_ = Status::Failed;
  }
}

void Checker::Poll() {
  if (!pending_) return;
  std::optional<std::string> payload;
  {
    std::lock_guard lock(pending_->mutex);
    if (!pending_->done) return;
    payload = std::move(pending_->payload);
  }
  pending_.reset();
  status_ = Status::Failed;
  if (payload) {
    if (const auto release = ParseRelease(*payload)) {
      Apply(*release);
      payload_ = std::move(*payload);
      next_check_ = Now() + 6 * 60 * 60;
    }
  }
  Save();
}

void Checker::Ignore() {
  if (status_ != Status::Available) return;
  ignored_ = release_.version;
  Save();
}

bool Checker::IsIgnored() const { return ignored_ == release_.version; }

void Checker::Save() const {
  if (cache_file_.empty()) return;
  // An unwritable cache must never prevent playing or checking for updates.
  std::error_code error;
  std::filesystem::create_directories(cache_file_.parent_path(), error);
  auto temporary = cache_file_;
  temporary += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
  const nlohmann::json cache = {{"schema", 1}, {"next_check", next_check_}, {"ignored", ignored_},
      {"release", payload_.empty() ? nlohmann::json{} : nlohmann::json::parse(payload_)}};
  {
    std::ofstream file(temporary, std::ios::binary);
    if (!file) return;
    file << cache.dump();
    file.close();
    if (!file) { std::filesystem::remove(temporary, error); return; }
  }
#if defined(_WIN32)
  if (MoveFileExW(temporary.c_str(), cache_file_.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
#else
  std::filesystem::rename(temporary, cache_file_, error);
  if (!error) return;
#endif
  std::filesystem::remove(temporary, error);
}
}  // namespace launcher_updates
