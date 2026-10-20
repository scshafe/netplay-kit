#include "npk/lobby/site.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace npk::lobby {

namespace {

bool plain_text(std::string_view s, std::size_t max) {  // printable, no control characters, not empty
  if (s.empty() || s.size() > max) return false;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) return false;
  }
  return true;
}

bool fail(std::string* error, std::string why) {
  if (error) *error = std::move(why);
  return false;
}

bool text_field(const json::Value& v, std::string_view key, std::size_t max, std::string& out, std::string* error) {
  const json::Value* f = v.find(key);
  if (!f) return true;
  if (!f->as_string() || !plain_text(*f->as_string(), max)) return fail(error, std::string(key) + " must be 1 to " + std::to_string(max) + " printable characters");
  out = *f->as_string();
  return true;
}

bool known_keys(const json::Value& v, std::initializer_list<std::string_view> keys, std::string_view where, std::string* error) {
  for (const auto& kv : *v.as_object()) {
    if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) return fail(error, "unknown field in " + std::string(where) + ": " + kv.first);
  }
  return true;
}

}  // namespace

json::Value SiteConfig::to_json() const {
  json::Value v;
  v.set("title", title);
  v.set("tagline", tagline);
  v.set("description", description);
  if (download) {
    json::Value d;
    d.set("label", download->label);
    d.set("url", download->url);
    d.set("note", download->note);
    v.set("download", d);
  }
  json::Value s = json::Value(json::Value::Array{});
  for (const auto& step : steps) s.push_back(step);
  v.set("steps", s);
  return v;
}

std::optional<SiteConfig> parse_site_config(std::string_view text, std::string* error) {
  json::Limits l;
  l.max_bytes = 16 * 1024;
  std::string why;
  auto doc = json::parse(text, l, &why);
  if (!doc || !doc->is_object()) {
    fail(error, doc ? "site config is not a JSON object" : "site config: " + why);
    return std::nullopt;
  }
  SiteConfig c;
  if (!known_keys(*doc, {"title", "tagline", "description", "download", "steps"}, "the site config", error)) return std::nullopt;
  if (!text_field(*doc, "title", 60, c.title, error) || !text_field(*doc, "tagline", 140, c.tagline, error) ||
      !text_field(*doc, "description", 600, c.description, error))
    return std::nullopt;
  if (const json::Value* d = doc->find("download")) {
    if (!d->is_object() || !known_keys(*d, {"label", "url", "note"}, "download", error)) {
      if (error && error->empty()) *error = "download must be an object";
      return std::nullopt;
    }
    SiteConfig::Download dl;
    dl.label = "Download";
    if (!text_field(*d, "label", 40, dl.label, error) || !text_field(*d, "url", 300, dl.url, error) ||
        !text_field(*d, "note", 100, dl.note, error))
      return std::nullopt;
    const bool web = dl.url.rfind("https://", 0) == 0 || dl.url.rfind("http://", 0) == 0;
    if (!web || dl.url.find_first_of(" \"'<>\\") != std::string::npos) {
      fail(error, "download.url must be an http or https URL");
      return std::nullopt;
    }
    c.download = std::move(dl);
  }
  if (const json::Value* s = doc->find("steps")) {
    if (!s->as_array() || s->as_array()->empty() || s->as_array()->size() > 8) {
      fail(error, "steps must be a list of 1 to 8 strings");
      return std::nullopt;
    }
    c.steps.clear();
    for (const json::Value& step : *s->as_array()) {
      if (!step.as_string() || !plain_text(*step.as_string(), 200)) {
        fail(error, "each step must be 1 to 200 printable characters");
        return std::nullopt;
      }
      c.steps.push_back(*step.as_string());
    }
  }
  return c;
}

// ---- files from a directory ----

namespace {

const char* content_type_for(std::string_view ext) {
  struct E { const char* ext; const char* type; };
  static const E table[] = {
      {"html", "text/html; charset=utf-8"}, {"css", "text/css; charset=utf-8"},
      {"js", "text/javascript; charset=utf-8"}, {"json", "application/json"},
      {"txt", "text/plain; charset=utf-8"}, {"svg", "image/svg+xml"}, {"png", "image/png"},
      {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"webp", "image/webp"},
      {"ico", "image/x-icon"}, {"woff2", "font/woff2"},
  };
  for (const E& e : table) {
    if (ext == e.ext) return e.type;
  }
  return nullptr;
}

bool plain_name(std::string_view n) {
  if (n.empty() || n.size() > 100 || n.front() == '.') return false;
  for (const char c : n) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

}  // namespace

std::optional<Asset> read_site_file(const std::filesystem::path& dir, std::string_view request_path, std::size_t max_bytes) {
  namespace fs = std::filesystem;
  if (request_path.empty() || request_path.front() != '/') return std::nullopt;
  std::string rel(request_path.substr(1));
  if (rel.empty()) rel = "index.html";
  fs::path p = dir;
  std::size_t i = 0;
  std::string last;
  while (i <= rel.size()) {
    const std::size_t j = rel.find('/', i);
    const std::string_view part = std::string_view(rel).substr(i, j == std::string::npos ? std::string::npos : j - i);
    if (!plain_name(part)) return std::nullopt;
    p /= std::string(part);
    last = std::string(part);
    if (j == std::string::npos) break;
    i = j + 1;
  }
  const std::size_t dot = last.rfind('.');
  if (dot == std::string::npos) return std::nullopt;
  const char* type = content_type_for(last.substr(dot + 1));
  if (!type) return std::nullopt;
  std::error_code ec;
  const fs::path root = fs::canonical(dir, ec);
  if (ec) return std::nullopt;
  const fs::path real = fs::canonical(p, ec);  // resolves symlinks
  if (ec || !fs::is_regular_file(real, ec) || ec) return std::nullopt;
  // The resolved file must be inside the resolved directory.
  auto r = root.begin();
  auto f = real.begin();
  for (; r != root.end(); ++r, ++f) {
    if (f == real.end() || *f != *r) return std::nullopt;
  }
  const auto size = fs::file_size(real, ec);
  if (ec || size > max_bytes) return std::nullopt;
  std::ifstream in(real, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream body;
  body << in.rdbuf();
  return Asset{type, body.str()};
}

}  // namespace npk::lobby
