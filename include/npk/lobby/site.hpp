#pragma once

// The home page: what the game is, where to download the client, how to join, and
// the rooms that are open now. The lobby serves it (docs/design.md, "Home page").
//
// The kit ships a default site (an HTML page, a style sheet and a script, all
// from the lobby's own origin, so a strict Content-Security-Policy holds). It is
// game-agnostic: what it says comes from a small JSON file the game provides
// (`SiteConfig`), and a game that wants its own look points `site_dir` at a
// directory of files that replace or add to the defaults.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "npk/json/json.hpp"

namespace npk::lobby {

struct SiteConfig {
  std::string title = "Game lobby";
  std::string tagline = "Rooms and matches";
  std::string description;
  struct Download {
    std::string label, url, note;
  };
  std::optional<Download> download;  // the client; url is http(s)
  std::vector<std::string> steps = {
      "Open the game and choose Online.",
      "Pick a room from the list below, or create one.",
      "Take a seat and press Ready. The match starts when every seat is taken and ready.",
  };

  json::Value to_json() const;
};

// Parses and validates the game's site.json: {"title", "tagline", "description",
// "download": {"label", "url", "note"}, "steps": [..]}. Every field is optional,
// unknown fields are an error (they are usually typos), lengths are bounded and a
// download URL must be http or https. On failure returns nothing and says why.
std::optional<SiteConfig> parse_site_config(std::string_view text, std::string* error = nullptr);

struct Asset {
  std::string content_type;
  std::string body;
};

// The default site's files by path ("/", "/style.css", "/app.js").
const Asset* default_asset(std::string_view path);

// A file under `dir` for a request path: only plain names ([A-Za-z0-9._-], none
// starting with '.'), only known file types, at most max_bytes, and only if it
// really lies inside `dir` once symlinks are resolved. "/" means index.html.
std::optional<Asset> read_site_file(const std::filesystem::path& dir, std::string_view request_path,
                                    std::size_t max_bytes = 2 * 1024 * 1024);

}  // namespace npk::lobby
