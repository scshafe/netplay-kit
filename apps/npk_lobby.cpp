// npk-lobby: the lobby and the home of the game servers' registrations.
//
//   NPK_CONTROL_SECRET_VAR  name of the variable holding the shared secret (default NPK_CONTROL_SECRET)
//   NPK_HTTP_PORT           default 8080
//   NPK_BIND                address to listen on, default 0.0.0.0
//   NPK_MAX_ROOMS           default 256

#include <chrono>
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
