// A UDP echo server and probe, for measuring a network path before the game
// server exists (the UDP-over-tailnet check, design.md "Network path").
//
//   npk_echo serve [--port N] [--duration S]
//   npk_echo probe --host IP --port N [--count N] [--interval-ms N] [--sizes A,B,..]
//
// The server echoes a datagram back, unchanged, only if it starts with the probe
// magic, so it is no general reflector, and the reply is never larger than the
// request. The probe sends numbered datagrams of each size and reports loss,
// reordering, round-trip percentiles and jitter. Sizes up to 1200 are the
// transport's datagram limit.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "npk/transport/time.hpp"
#include "npk/transport/udp_socket.hpp"
#include "npk/transport/wire.hpp"

using namespace npk::transport;

namespace {

constexpr char kMagic[8] = {'N', 'P', 'K', 'E', 'C', 'H', 'O', '1'};
constexpr std::size_t kProbeHeader = 8 + 4 + 8;  // magic, seq, send time

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Args {
  std::string mode, host;
  long port = 7777, duration = 0, count = 200, interval_ms = 20;
  std::vector<std::size_t> sizes{64, 512, 1200};
};

bool parse_long(const char* s, long lo, long hi, long& out) {
  char* end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parse_args(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.mode = argv[1];
  for (int i = 2; i < argc; i += 2) {
    std::string_view k = argv[i];
    if (i + 1 >= argc) return false;
    const char* v = argv[i + 1];
    if (k == "--host") a.host = v;
    else if (k == "--port") { if (!parse_long(v, 0, 65535, a.port)) return false; }
    else if (k == "--duration") { if (!parse_long(v, 0, 86400, a.duration)) return false; }
    else if (k == "--count") { if (!parse_long(v, 1, 100000, a.count)) return false; }
    else if (k == "--interval-ms") { if (!parse_long(v, 1, 10000, a.interval_ms)) return false; }
    else if (k == "--sizes") {
      a.sizes.clear();
      std::string s = v;
      for (std::size_t p = 0; p <= s.size();) {
        std::size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        long n = 0;
        if (!parse_long(s.substr(p, q - p).c_str(), static_cast<long>(kProbeHeader),
                        static_cast<long>(kMaxDatagram), n))
          return false;
        a.sizes.push_back(static_cast<std::size_t>(n));
        p = q + 1;
      }
    } else return false;
  }
  return a.mode == "serve" || (a.mode == "probe" && !a.host.empty());
}

int serve(const Args& a) {
  std::string err;
  auto sock = UdpSocket::bind(Address::any_v6(static_cast<std::uint16_t>(a.port)), &err);
  if (!sock) sock = UdpSocket::bind(Address::any_v4(static_cast<std::uint16_t>(a.port)), &err);
  if (!sock) { std::fprintf(stderr, "bind failed: %s\n", err.c_str()); return 1; }
  std::printf("echo listening on %s\n", sock->local_address().to_string().c_str());
  std::fflush(stdout);
  auto start = std::chrono::steady_clock::now();
  std::uint64_t echoed = 0, ignored = 0;
  std::vector<std::uint8_t> buf(kMaxDatagram + 1);
  while (!g_stop) {
    if (a.duration > 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(a.duration)) break;
    if (!sock->wait_readable(200)) continue;
    Address from;
    while (auto n = sock->receive(from, buf)) {
      if (*n < kProbeHeader || *n > kMaxDatagram || std::memcmp(buf.data(), kMagic, 8) != 0) { ++ignored; continue; }
      sock->send(from, std::span(buf.data(), *n));
      ++echoed;
    }
  }
  std::printf("echoed %llu, ignored %llu\n", static_cast<unsigned long long>(echoed),
              static_cast<unsigned long long>(ignored));
  return 0;
}

double percentile(const std::vector<double>& sorted, double p) {
  auto i = static_cast<std::size_t>(std::lround(p * static_cast<double>(sorted.size() - 1)));
  return sorted[i];
}

int probe(const Args& a) {
  auto to = Address::parse(a.host, static_cast<std::uint16_t>(a.port));
  if (!to) { std::fprintf(stderr, "--host must be an IP address literal\n"); return 1; }
  std::string err;
  auto sock = UdpSocket::bind(to->is_v4() ? Address::any_v4(0) : Address::any_v6(0), &err);
  if (!sock) { std::fprintf(stderr, "bind failed: %s\n", err.c_str()); return 1; }
  std::printf("probing %s: %ld packets per size, every %ld ms\n", to->to_string().c_str(), a.count, a.interval_ms);
  std::printf("%6s %6s %6s %6s %8s %8s %8s %8s %8s %8s\n", "size", "sent", "recv", "loss%",
              "reorder", "min_ms", "p50_ms", "p95_ms", "max_ms", "jitter");
  std::vector<std::uint8_t> out(kMaxDatagram), in(kMaxDatagram + 1);
  for (std::size_t size : a.sizes) {
    std::vector<double> rtts;  // in arrival order, to compute jitter and reordering
    std::vector<double> sorted;
    std::vector<bool> seen(static_cast<std::size_t>(a.count), false);
    long recv = 0, reorder = 0, highest = -1;
    auto take = [&] {
      Address from;
      while (auto n = sock->receive(from, in)) {
        if (*n != size || from != *to || std::memcmp(in.data(), kMagic, 8) != 0) continue;
        std::uint32_t seq;
        std::int64_t sent;
        std::memcpy(&seq, in.data() + 8, 4);
        std::memcpy(&sent, in.data() + 12, 8);
        if (seq >= static_cast<std::uint32_t>(a.count) || seen[seq]) continue;  // stray or duplicate
        seen[seq] = true;
        ++recv;
        if (static_cast<long>(seq) < highest) ++reorder; else highest = static_cast<long>(seq);
        rtts.push_back(static_cast<double>(steady_now_us() - sent) / 1000.0);
      }
    };
    for (long i = 0; i < a.count && !g_stop; ++i) {
      std::memset(out.data(), 0, size);
      std::memcpy(out.data(), kMagic, 8);
      auto seq = static_cast<std::uint32_t>(i);
      std::int64_t now = steady_now_us();
      std::memcpy(out.data() + 8, &seq, 4);
      std::memcpy(out.data() + 12, &now, 8);
      if (!sock->send(*to, std::span(out.data(), size)))
        std::fprintf(stderr, "send of %zu bytes was refused\n", size);
      auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(a.interval_ms);
      while (std::chrono::steady_clock::now() < until) { sock->wait_readable(1); take(); }
    }
    for (int i = 0; i < 20; ++i) { sock->wait_readable(100); take(); }  // stragglers, up to 2 s
    long sent = a.count;
    if (rtts.empty()) {
      std::printf("%6zu %6ld %6ld %6.1f %8s\n", size, sent, recv, 100.0, "-");
      continue;
    }
    double jitter = 0;
    for (std::size_t i = 1; i < rtts.size(); ++i) jitter += std::fabs(rtts[i] - rtts[i - 1]);
    jitter /= static_cast<double>(std::max<std::size_t>(rtts.size() - 1, 1));
    sorted = rtts;
    std::sort(sorted.begin(), sorted.end());
    std::printf("%6zu %6ld %6ld %6.1f %8ld %8.2f %8.2f %8.2f %8.2f %8.2f\n", size, sent, recv,
                100.0 * static_cast<double>(sent - recv) / static_cast<double>(sent), reorder,
                sorted.front(), percentile(sorted, 0.5), percentile(sorted, 0.95), sorted.back(), jitter);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) {
    std::fprintf(stderr,
                 "usage: npk_echo serve [--port N] [--duration S]\n"
                 "       npk_echo probe --host IP --port N [--count N] [--interval-ms N] [--sizes A,B,..]\n");
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  return a.mode == "serve" ? serve(a) : probe(a);
}
