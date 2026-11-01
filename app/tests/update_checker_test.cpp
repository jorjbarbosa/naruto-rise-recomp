#include "menu/update_checker.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace launcher_updates;
using namespace std::chrono_literals;

namespace {
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

nlohmann::json Fixture(std::string tag = "v0.2.0-beta.2") {
  const auto root = "https://github.com/jorjbarbosa/naruto-rise-recomp/releases/download/" + tag + "/";
  return {{"tag_name", tag}, {"draft", false}, {"prerelease", false},
    {"assets", nlohmann::json::array({
      {{"name", "NarutoRiseInstaller.exe"}, {"state", "uploaded"},
       {"browser_download_url", root + "NarutoRiseInstaller.exe"}},
      {{"name", "naruto-rise-recomp-win-amd64.zip"}, {"state", "uploaded"},
       {"browser_download_url", root + "naruto-rise-recomp-win-amd64.zip"}}})}};
}

void Finish(Checker& checker) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (checker.status() == Status::Checking && std::chrono::steady_clock::now() < deadline) {
    checker.Poll();
    std::this_thread::sleep_for(1ms);
  }
  Require(checker.status() != Status::Checking, "Worker did not complete");
}

struct TempCache {
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("narutorise-update-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path file = directory / "update-check.json";
  TempCache() { std::filesystem::create_directory(directory); }
  ~TempCache() {
    std::error_code error;
    std::filesystem::remove(file, error);
    std::filesystem::remove(directory, error);
  }
};
}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--live") {
      const auto payload = FetchLatestRelease();
      Require(payload.has_value(), "GitHub request failed");
      const auto release = ParseRelease(*payload);
      Require(release && !release->portable_url.empty(),
              "Latest GitHub release or its Windows assets were not recognized");
      std::cout << "GitHub latest: " << release->version << "; portable link recognized\n";
      return 0;
    }

    Require(CompareVersions("v0.2.0-beta.10", "0.2.0-beta.2") == 1, "Numeric beta ordering");
    Require(CompareVersions("0.2.0", "0.2.0-beta.10") == 1, "Final release ordering");
    Require(CompareVersions("1.10.0", "1.9.99") == 1, "Numeric minor ordering");
    Require(CompareVersions("1.0.0+abc", "v1.0.0+def") == 0, "Build metadata affects precedence");
    Require(CompareVersions("1.0.0-alpha", "1.0.0-alpha.1") == -1, "Prerelease length ordering");
    Require(CompareVersions("1.0.0-1", "1.0.0-alpha") == -1, "Numeric versus named suffix");
    Require(CompareVersions("1.0.0-beta", "1.0.0-alpha") == 1, "Named suffix ordering");
    Require(CompareVersions("99999999999999999999.0.0", "2.0.0") == 1, "Large version overflow");
    for (const auto* bad : {"", "unknown", "1.2", "01.2.3", "1.2.3-beta.01", "1.2.3/evil", "1.2.3\n"})
      Require(!CompareVersions(bad, "1.2.3"), "Accepted malformed version");
    Require(IsNewerRelease("1.0.0-beta.1", "v0.2.0-beta.2") == true, "Legacy migration missing");
    Require(IsNewerRelease("1.0.0", "v0.3.0-beta") == true, "Legacy migration to 0.3.0-beta missing");
    Require(IsNewerRelease("0.2.0-beta.2", "v0.3.0-beta") == true, "0.3.0-beta not newer than 0.2.0-beta.2");
    Require(IsNewerRelease("1.0.0", "v0.4.0-beta") == true, "Legacy migration to 0.4.0-beta missing");
    Require(IsNewerRelease("0.3.0-beta", "v0.4.0-beta") == true, "0.4.0-beta not newer than 0.3.0-beta");
    Require(IsNewerRelease("1.0.0", "v0.4.1-beta") == true, "Legacy migration to 0.4.1-beta missing");
    Require(IsNewerRelease("0.4.0-beta", "v0.4.1-beta") == true, "0.4.1-beta not newer than 0.4.0-beta");
    Require(IsNewerRelease("0.2.0-beta.2", "v1.0.0-beta.1") == false, "Reverse legacy migration allowed");
    Require(IsNewerRelease("2.0.0", "v0.2.0-beta.2") == false, "Downgrade offered");
    Require(IsNewerRelease("v0.2.0-beta.2", "0.2.0-beta.2") == false, "Same version offered");

    auto json = Fixture();
    auto release = ParseRelease(json.dump());
    Require(release && !release->installer_url.empty() && !release->portable_url.empty(),
            "Published beta-named release must be accepted");
    json["assets"].erase(json["assets"].begin());
    release = ParseRelease(json.dump());
    Require(release && release->installer_url.empty() && !release->portable_url.empty(),
            "ZIP-only release must work after the compatibility bridge is retired");
    json["prerelease"] = true;
    Require(!ParseRelease(json.dump()), "Pre-release accepted");
    json = Fixture(); json["draft"] = true;
    Require(!ParseRelease(json.dump()), "Draft accepted");
    json = Fixture(); json["assets"][0]["browser_download_url"] = "https://evil.example/setup.exe";
    json["html_url"] = "file:///C:/evil.exe";
    release = ParseRelease(json.dump());
    Require(release && release->installer_url.empty() && release->page_url.starts_with("https://github.com/"),
            "Untrusted link accepted");
    json = Fixture(); json["assets"][0]["state"] = "starter";
    Require(ParseRelease(json.dump())->installer_url.empty(), "Incomplete upload offered");
    json = Fixture(); json["assets"] = nlohmann::json::array();
    Require(ParseRelease(json.dump())->installer_url.empty(), "Missing asset invented");
    for (const auto* bad : {"{}", "[]", "not JSON", "{\"prerelease\":\"false\"}"})
      Require(!ParseRelease(bad), "Malformed API response accepted");
    Require(!ParseRelease(std::string(1024 * 1024 + 1, ' ')), "Oversized response accepted");

    const auto payload = Fixture().dump();
    TempCache cache;
    std::atomic<int> calls = 0;
    const auto fetch = [&]() -> std::optional<std::string> { ++calls; return payload; };
    {
      Checker checker("0.2.0-beta.1", cache.file, fetch);
      Finish(checker);
      Require(checker.status() == Status::Available, "New release not surfaced");
      checker.Ignore();
      Require(checker.IsIgnored(), "Ignore did not apply");
      checker.CheckNow();
      Require(calls == 1, "Repeated check bypassed cooldown");
    }
    {
      Checker checker("0.2.0-beta.1", cache.file, fetch);
      Require(checker.status() == Status::Available && checker.IsIgnored(), "Cache or ignore not restored");
      Require(calls == 1, "Fresh cache still fetched");
      checker.CheckNow();
      Finish(checker);
      Require(calls == 2, "Manual check did not bypass cache");
    }
    {
      Checker checker("0.2.0-beta.2", cache.file, fetch);
      Require(checker.status() == Status::Current, "Cache not compared against this installation");
      Checker unknown("dev", cache.file, fetch);
      Require(unknown.status() == Status::UnknownVersion, "Unknown version reported as up to date");
    }
    {
      std::ofstream(cache.file) << "broken cache";
      Checker checker("0.2.0-beta.1", cache.file, fetch);
      Finish(checker);
      Require(calls == 3, "Corrupt cache prevented check");
    }
    {
      TempCache failed_cache;
      Checker checker("0.2.0-beta.1", failed_cache.file, []() -> std::optional<std::string> { return {}; });
      Finish(checker);
      Require(checker.status() == Status::Failed, "Offline check reported success");
      Checker retry("0.2.0-beta.1", failed_cache.file, fetch);
      Require(retry.status() == Status::Failed && calls == 3, "Failed check retried on every startup");
    }
    {
      TempCache expired;
      std::ofstream(expired.file) << nlohmann::json{{"schema", 1}, {"next_check", 1},
          {"release", Fixture()}, {"ignored", ""}};
      Checker checker("0.2.0-beta.1", expired.file, fetch);
      Finish(checker);
      Require(calls == 4, "Expired cache prevented refresh");
    }
    {
      TempCache ignored_next;
      std::ofstream(ignored_next.file) << nlohmann::json{{"schema", 1}, {"next_check", 1},
          {"release", Fixture()}, {"ignored", "0.2.0-beta.2"}};
      Checker checker("0.2.0-beta.1", ignored_next.file, [] { return Fixture("v0.3.0").dump(); });
      Finish(checker);
      Require(checker.status() == Status::Available && !checker.IsIgnored(), "Ignoring one release hid the next");
    }
    {
      // A slow request must not hold up launch/exit or access a destroyed Checker.
      std::promise<void> unblock, entered, finished;
      auto ready = unblock.get_future().share();
      {
        Checker checker("0.2.0-beta.1", {}, [ready, &entered, &finished, payload] {
          entered.set_value();
          ready.wait();
          finished.set_value();
          return payload;
        });
        entered.get_future().wait();
        Require(checker.status() == Status::Checking, "Slow request blocked constructor");
        Require(!checker.CanCheck(), "Concurrent request allowed");
      }
      unblock.set_value();
      finished.get_future().wait();
    }
    std::cout << "Update checks passed: versions, releases, trusted links, cache, ignore, offline and worker lifetime\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
