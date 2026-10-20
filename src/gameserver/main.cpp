#include "npk/gameserver/main.hpp"

#include "npk/app/env.hpp"
#include "npk/transport/udp_socket.hpp"

namespace npk::gameserver {

int run(SimulationFactory factory) {
  using namespace npk::app;
  install_signal_handlers();
  bool ok = true;
  GameServerConfig cfg;
  cfg.factory = std::move(factory);
  std::string var;
  cfg.control_secret = control_secret(&var);
  if (cfg.control_secret.size() < kMinSecret) {
    std::fprintf(stderr, "the control secret (variable %s) is missing or shorter than %zu bytes\n", var.c_str(), kMinSecret);
    return 2;
  }
  const long udp_port = env_int("NPK_UDP_PORT", 7777, 1, 65535, &ok);
  const long control_port = env_int("NPK_CONTROL_PORT", 9000, 1, 65535, &ok);
  cfg.max_matches = static_cast<std::size_t>(env_int("NPK_MAX_MATCHES", 16, 1, 10000, &ok));
  cfg.match.tick_rate_hz = static_cast<int>(env_int("NPK_TICK_RATE", 60, 10, 240, &ok));
  cfg.lobby = env_string("NPK_LOBBY");
  cfg.advertise_control = env_string("NPK_ADVERTISE_CONTROL");
  cfg.advertise_udp = env_string("NPK_ADVERTISE_UDP");
  if (!ok) return 2;
  if (!cfg.lobby.empty() && (cfg.advertise_control.empty() || cfg.advertise_udp.empty())) {
    std::fprintf(stderr, "NPK_LOBBY needs NPK_ADVERTISE_CONTROL and NPK_ADVERTISE_UDP\n");
    return 2;
  }
  const auto bind = transport::Address::parse(env_string("NPK_CONTROL_BIND", "0.0.0.0"), static_cast<std::uint16_t>(control_port));
  if (!bind) {
    std::fprintf(stderr, "NPK_CONTROL_BIND is not an IP address\n");
    return 2;
  }
  cfg.control_bind = *bind;
  cfg.log = [](std::string_view s) { log_line("gameserver", s); };

  std::string err;
  auto sock = transport::UdpSocket::bind(transport::Address::any_v6(static_cast<std::uint16_t>(udp_port)), &err);
  if (!sock) sock = transport::UdpSocket::bind(transport::Address::any_v4(static_cast<std::uint16_t>(udp_port)), &err);
  if (!sock) {
    std::fprintf(stderr, "udp port %ld: %s\n", udp_port, err.c_str());
    return 2;
  }
  auto server = GameServer::create(*sock, std::move(cfg), &err);
  if (!server) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  log_line("gameserver", "listening: udp " + sock->local_address().to_string() + ", control " + server->control_address().to_string());
  while (!stop_requested()) {
    server->update(transport::steady_now_us());
    sock->wait_readable(2);  // wakes at once for a datagram; otherwise the loop ticks every 2 ms
  }
  log_line("gameserver", "stopping");
  return 0;
}

}  // namespace npk::gameserver
