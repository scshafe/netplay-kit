// npk-lobby: the lobby and the home of the game servers' registrations.
//
//   NPK_CONTROL_SECRET_VAR  name of the variable holding the shared secret (default NPK_CONTROL_SECRET)
//   NPK_HTTP_PORT           default 8080
//   NPK_BIND                address to listen on, default 0.0.0.0
//   NPK_MAX_ROOMS           default 256
//   NPK_SITE_FILE           path of the home page's text (site.json: title, tagline, description, download, steps)
//   NPK_SITE_DIR            directory of files that replace or add to the default home page

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "npk/app/env.hpp"
#include "npk/lobby/lobby.hpp"

int main() {
  using namespace npk;
  using namespace npk::app;
  install_signal_handlers();
  bool ok = true;
  lobby::LobbyConfig cfg;
  std::string var;
  cfg.control_secret = control_secret(&var);
  if (cfg.control_secret.size() < lobby::kMinSecret) {
    std::fprintf(stderr, "the control secret (variable %s) is missing or shorter than %zu bytes\n", var.c_str(), lobby::kMinSecret);
    return 2;
  }
  const long port = env_int("NPK_HTTP_PORT", 8080, 1, 65535, &ok);
  cfg.max_rooms = static_cast<std::size_t>(env_int("NPK_MAX_ROOMS", 256, 1, 100000, &ok));
  if (!ok) return 2;
  if (const std::string file = env_string("NPK_SITE_FILE"); !file.empty()) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string why;
    auto site = lobby::parse_site_config(text.str(), &why);
    if (!in || !site) {
      std::fprintf(stderr, "NPK_SITE_FILE %s: %s\n", file.c_str(), in ? why.c_str() : "cannot read it");
      return 2;
    }
    cfg.site = std::move(*site);
  }
  cfg.site_dir = env_string("NPK_SITE_DIR");
  const auto bind = transport::Address::parse(env_string("NPK_BIND", "0.0.0.0"), static_cast<std::uint16_t>(port));
  if (!bind) {
    std::fprintf(stderr, "NPK_BIND is not an IP address\n");
    return 2;
  }
  cfg.bind = *bind;
  cfg.log = [](std::string_view s) { log_line("lobby", s); };
  std::string err;
  auto lobby = lobby::Lobby::create(cfg, &err);
  if (!lobby) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  log_line("lobby", "listening on " + lobby->address().to_string());
  while (!stop_requested()) {
    lobby->update(transport::steady_now_us());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  log_line("lobby", "stopping");
  return 0;
}
