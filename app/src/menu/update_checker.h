#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace launcher_updates {

struct Release {
  std::string version;
  std::string page_url;
  std::string installer_url;
  std::string portable_url;
  std::string installer_sha256;
  std::uint64_t installer_size = 0;
};

// GitHub's prerelease flag determines eligibility, not a suffix in the tag.
std::optional<Release> ParseRelease(std::string_view json);
std::optional<int> CompareVersions(std::string_view left, std::string_view right);
std::optional<bool> IsNewerRelease(std::string_view installed, std::string_view available);
std::optional<std::string> FetchLatestRelease();

enum class Status { Checking, Current, Available, Failed, UnknownVersion };

// All methods run on the UI thread. The network worker owns only its mailbox;
// destroying the checker never waits for the network or leaves a UI reference.
class Checker {
 public:
  using Fetch = std::function<std::optional<std::string>()>;
  Checker(std::string installed, std::filesystem::path cache_file,
          Fetch fetch = FetchLatestRelease);
  void Poll();
  bool CanCheck() const;
  void CheckNow();
  void Ignore();
  bool IsIgnored() const;
  Status status() const { return status_; }
  const Release& release() const { return release_; }

 private:
  struct Mailbox;
  void Start();
  void Apply(const Release& release);
  void Save() const;
  std::string installed_;
  std::filesystem::path cache_file_;
  Fetch fetch_;
  std::shared_ptr<Mailbox> pending_;
  std::chrono::steady_clock::time_point manual_after_{};
  std::int64_t next_check_ = 0;
  std::string payload_;
  std::string ignored_;
  Release release_;
  Status status_ = Status::Failed;
};

}  // namespace launcher_updates
