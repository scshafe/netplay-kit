// The home page: its text config, the files a site directory may serve, and
// the default page's files.

#include <cstdio>
#include <filesystem>
#include <fstream>

#include <unistd.h>

#include "check.hpp"
#include "npk/lobby/site.hpp"

using namespace npk::lobby;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& p, const std::string& content) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << content;
}

void config_parsing() {
  std::string err;
  auto c = parse_site_config("{}", &err);
  CHECK(c && c->title == "Game lobby" && !c->download && c->steps.size() == 3);
  c = parse_site_config(
      "{\"title\":\"Toy\",\"tagline\":\"t\",\"description\":\"d\",\"download\":{\"label\":\"Get it\",\"url\":\"https://x.example/a.dmg\",\"note\":\"macOS\"},"
      "\"steps\":[\"one\",\"two\"]}",
      &err);
  CHECK(c && c->title == "Toy" && c->download && c->download->url == "https://x.example/a.dmg" && c->steps.size() == 2);
  auto v = c->to_json();
  CHECK(*v.find("download")->find("label")->as_string() == "Get it" && v.find("steps")->as_array()->size() == 2);
  // A download with no label gets one.
  c = parse_site_config("{\"download\":{\"url\":\"http://h/p\"}}", &err);
  CHECK(c && c->download->label == "Download");

  for (const char* bad : {"", "[]", "{", "{\"titel\":\"typo\"}", "{\"title\":\"\"}", "{\"title\":5}", "{\"title\":\"a\\nb\"}",
                          "{\"download\":5}", "{\"download\":{}}", "{\"download\":{\"url\":\"javascript:alert(1)\"}}",
                          "{\"download\":{\"url\":\"ftp://h/f\"}}", "{\"download\":{\"url\":\"https://h/a b\"}}",
                          "{\"download\":{\"url\":\"https://h/\\\"onclick=\"}}", "{\"download\":{\"url\":\"https://h/<script>\"}}",
                          "{\"download\":{\"url\":\"https://h/\",\"extra\":1}}", "{\"steps\":[]}", "{\"steps\":\"x\"}", "{\"steps\":[1]}",
                          "{\"steps\":[\"\"]}"}) {
    CHECK(!parse_site_config(bad, &err));
    CHECK(!err.empty());
  }
  CHECK(!parse_site_config("{\"title\":\"" + std::string(61, 'x') + "\"}"));
  CHECK(parse_site_config("{\"title\":\"" + std::string(60, 'x') + "\"}"));
  CHECK(!parse_site_config("{\"tagline\":\"" + std::string(141, 'x') + "\"}"));
  CHECK(!parse_site_config("{\"description\":\"" + std::string(601, 'x') + "\"}"));
  std::string nine = "{\"steps\":[";
  for (int i = 0; i < 9; ++i) nine += std::string(i ? "," : "") + "\"s\"";
  CHECK(!parse_site_config(nine + "]}"));
  CHECK(!parse_site_config("{\"steps\":[\"" + std::string(201, 'x') + "\"]}"));
  CHECK(!parse_site_config("{\"title\":\"" + std::string(20000, 'x') + "\"}"));  // the document is too big
}

void default_files() {
  for (const char* p : {"/", "/index.html", "/style.css", "/app.js"}) CHECK(default_asset(p) && !default_asset(p)->body.empty());
  CHECK(default_asset("/") == default_asset("/index.html"));
  CHECK(!default_asset("/other") && !default_asset("") && !default_asset("/app.js/"));
  const std::string& html = default_asset("/")->body;
  const std::string& js = default_asset("/app.js")->body;
  const std::string& css = default_asset("/style.css")->body;
  CHECK(default_asset("/")->content_type.rfind("text/html", 0) == 0 && default_asset("/app.js")->content_type.rfind("text/javascript", 0) == 0);
  // The strict Content-Security-Policy forbids inline script and style, so none may be needed.
  CHECK(html.find("<script>") == std::string::npos && html.find("style=") == std::string::npos && html.find("onclick") == std::string::npos);
  CHECK(html.find("<script src=\"/app.js\">") != std::string::npos && html.find("href=\"/style.css\"") != std::string::npos);
  // Text from rooms and visitors only ever goes in as text.
  CHECK(js.find("innerHTML") == std::string::npos && js.find("outerHTML") == std::string::npos && js.find("insertAdjacentHTML") == std::string::npos &&
        js.find("document.write") == std::string::npos && js.find("eval(") == std::string::npos);
  CHECK(html.find("http://") == std::string::npos && html.find("https://") == std::string::npos && css.find("http") == std::string::npos &&
        css.find("@import") == std::string::npos && css.find("url(") == std::string::npos);
}

void site_directory() {
  const fs::path root = fs::temp_directory_path() / ("npk-site-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const fs::path dir = root / "site";
  write(dir / "index.html", "<html>mine</html>");
  write(dir / "extra.css", "body{}");
  write(dir / "img" / "logo.png", "PNGDATA");
  write(dir / ".hidden.html", "secret");
  write(dir / "README", "no extension");
  write(dir / "tool.exe", "binary");
  write(dir / "big.css", std::string(3 * 1024 * 1024, 'x'));
  write(dir / "small.css", std::string(100, 'x'));
  write(root / "outside.html", "outside the site");
  fs::create_symlink(root / "outside.html", dir / "link.html");
  fs::create_symlink(root, dir / "up");

  auto get = [&](const char* p) { return read_site_file(dir, p); };
  CHECK(get("/") && get("/")->body == "<html>mine</html>" && get("/")->content_type.rfind("text/html", 0) == 0);
  CHECK(get("/index.html") && get("/extra.css")->content_type.rfind("text/css", 0) == 0);
  CHECK(get("/img/logo.png") && get("/img/logo.png")->content_type == "image/png");
  CHECK(!get("/.hidden.html") && !get("/img/.x") && !get("/README") && !get("/tool.exe"));
  CHECK(!get("/big.css") && get("/small.css") && !read_site_file(dir, "/small.css", 50));
  CHECK(!get("/missing.html") && !get("/img") && !get("/img/") && !get("") && !get("index.html"));
  for (const char* p : {"/../outside.html", "/img/../index.html", "/%2e%2e/outside.html", "//index.html", "/a\\b.css", "/in dex.html",
                        "/index.html\x01", "/index.html:stream", "/up/outside.html", "/link.html", "/./index.html"}) {
    CHECK(!get(p));
  }
  CHECK(!read_site_file(root / "nothing-here", "/index.html"));
  fs::remove_all(root);
}

}  // namespace

int main() {
  config_parsing();
  default_files();
  site_directory();
  std::puts("site_test: ok");
  return 0;
}
