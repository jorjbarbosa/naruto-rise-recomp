#include "diagnostics/incident.h"
#include "diagnostics/channel.h"
#include "gpu/common/sha256.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace narutorise::incident {
namespace fs = std::filesystem;
namespace {
uint32_t Crc(const fs::path& path) {
  uint32_t crc = 0xffffffffu; std::ifstream in(path, std::ios::binary); char buffer[65536];
  if (!in) throw std::runtime_error("Cannot open artifact");
  while (in.read(buffer, sizeof(buffer)) || in.gcount())
    for (std::streamsize i = 0; i < in.gcount(); ++i) {
      crc ^= uint8_t(buffer[i]);
      for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
  if (in.bad()) throw std::runtime_error("Cannot read artifact");
  return crc ^ 0xffffffffu;
}
void Word(std::ostream& out, uint32_t value, int bytes = 4) {
  for (int i = 0; i < bytes; ++i) out.put(char(value >> (i * 8)));
}
bool Allowed(const fs::path& relative) {
  if (relative.empty() || relative.is_absolute() || relative.has_root_name()) return false;
  for (const auto& part : relative) if (part == ".." || part == ".") return false;
  const auto name = relative.filename();
  return name == "events.jsonl" || name == "session.log" || name == "stderr.log" ||
      name == "guest-context.json" || relative.extension() == ".dmp" || relative.extension() == ".core";
}
}
bool Export(const fs::path& session, const fs::path& destination, std::string& error) {
  auto temporary = destination; temporary += ".partial";
  bool created = false;
  try {
    if (fs::exists(destination) || fs::is_symlink(destination) || fs::exists(temporary) || fs::is_symlink(temporary)) throw std::runtime_error("Export destination already exists");
    const auto manifest = nlohmann::json::parse(ReadText(session / "manifest.json", 1024 * 1024));
    if (manifest.value("state", "") != "complete") throw std::runtime_error("Session is still being collected");
    if (fs::is_symlink(session) || fs::is_symlink(session / "manifest.json")) throw std::runtime_error("Symlink session is not exportable");
    struct Entry { fs::path path; std::string name; uint32_t crc, size, offset = 0; };
    std::vector<Entry> entries;
    uint64_t bytes = 0;
    auto add = [&](const fs::path& relative) {
      const auto path = session / relative;
      const auto resolved = fs::weakly_canonical(path).lexically_relative(fs::canonical(session));
      if (resolved.empty() || resolved.is_absolute()) throw std::runtime_error("Artifact resolves outside its session");
      for (const auto& part : resolved) if (part == "..") throw std::runtime_error("Artifact resolves outside its session");
      auto ancestor = session;
      for (const auto& part : relative) { ancestor /= part; if (fs::is_symlink(ancestor)) throw std::runtime_error("Symlink artifact is not exportable"); }
      if (!fs::is_regular_file(path)) throw std::runtime_error("Manifest artifact is missing");
      const auto size = fs::file_size(path); bytes += size;
      if (bytes > 256ull * 1024 * 1024) throw std::runtime_error("Export exceeds 256 MiB; use the incident folder");
      const auto name = relative.generic_u8string();
      entries.push_back({path, std::string(name.begin(), name.end()), Crc(path), uint32_t(size)});
    };
    add("manifest.json");
    for (const auto& artifact : manifest.at("artifacts")) {
      const auto text = artifact.at("path").get<std::string>();
      const auto relative = fs::path(std::u8string(text.begin(), text.end()));
      if (!Allowed(relative)) throw std::runtime_error("Manifest contains an unsupported artifact path");
      if (!artifact.value("present", true)) continue; // Manifest retains the capture error.
      add(relative);
      if (gpu::Sha256File(entries.back().path) != artifact.at("sha256").get<std::string>()) throw std::runtime_error("Artifact hash differs from manifest");
    }
    fs::create_directories(destination.parent_path());
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create export");
    created = true;
    // ZIP store method: portable, bounded streaming, no runtime ZIP library.
    for (auto& entry : entries) {
      entry.offset = uint32_t(std::streamoff(out.tellp()));
      Word(out, 0x04034b50); Word(out, 20, 2); Word(out, 0x800, 2); Word(out, 0, 2);
      Word(out, 0, 2); Word(out, 0x21, 2); Word(out, entry.crc); Word(out, entry.size); Word(out, entry.size);
      Word(out, uint32_t(entry.name.size()), 2); Word(out, 0, 2); out << entry.name;
      std::ifstream in(entry.path, std::ios::binary); char buffer[65536];
      if (!in) throw std::runtime_error("Cannot reopen artifact");
      while (in.read(buffer, sizeof(buffer)) || in.gcount()) out.write(buffer, in.gcount());
      if (in.bad() || !out) throw std::runtime_error("Artifact copy failed");
    }
    const uint32_t central = uint32_t(std::streamoff(out.tellp()));
    for (const auto& entry : entries) {
      Word(out, 0x02014b50); Word(out, 20, 2); Word(out, 20, 2); Word(out, 0x800, 2); Word(out, 0, 2);
      Word(out, 0, 2); Word(out, 0x21, 2); Word(out, entry.crc); Word(out, entry.size); Word(out, entry.size);
      Word(out, uint32_t(entry.name.size()), 2); Word(out, 0, 2); Word(out, 0, 2); Word(out, 0, 2); Word(out, 0, 2);
      Word(out, 0); Word(out, entry.offset); out << entry.name;
    }
    const uint32_t end = uint32_t(std::streamoff(out.tellp()));
    Word(out, 0x06054b50); Word(out, 0, 2); Word(out, 0, 2);
    Word(out, uint32_t(entries.size()), 2); Word(out, uint32_t(entries.size()), 2);
    Word(out, end - central); Word(out, central); Word(out, 0, 2);
    out.close(); if (!out) throw std::runtime_error("Export write failed");
    fs::rename(temporary, destination); return true;
  } catch (const std::exception& e) {
    error = e.what(); std::error_code ignored; if (created) fs::remove(temporary, ignored); return false;
  }
}
}  // namespace narutorise::incident
