#include "npk/http/client.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>

#include "../transport/sockaddr.hpp"

namespace npk::http {

using transport::Address;
using transport::Micros;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

struct Client::Call {
  int fd = -1;
  bool connected = false;
  std::string out;
  std::size_t written = 0;
  std::string in;
  Micros deadline = 0;
  Callback done;
};

Client::Client(const ClientConfig& config) : cfg_(config) {}

Client::~Client() {
  for (Call& c : calls_) {
    if (c.fd >= 0) ::close(c.fd);
  }
}

bool Client::request(const Address& to, std::string_view host, const OutgoingRequest& req, Micros now, Callback done) {
  if (calls_.size() >= cfg_.max_in_flight) return false;
  std::string bytes;
  if (!serialize_request(req, host, bytes) || bytes.size() > cfg_.limits.max_header_bytes + cfg_.limits.max_body) {
    done(Result::failure("bad request"));
    return true;
  }
  const bool v6 = !to.is_v4();
  const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  const int flags = fd < 0 ? -1 : ::fcntl(fd, F_GETFL, 0);
  if (fd < 0 || flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    if (fd >= 0) ::close(fd);
    done(Result::failure("socket failed"));
    return true;
  }
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  sockaddr_storage ss;
  const socklen_t len = transport::to_sockaddr(to, v6, ss);
  const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&ss), len);
  if (r < 0 && errno != EINPROGRESS) {
    ::close(fd);
    done(Result::failure("connect failed"));
    return true;
  }
  Call c;
  c.fd = fd;
  c.connected = r == 0;
  c.out = std::move(bytes);
  c.deadline = now + cfg_.timeout;
  c.done = std::move(done);
  calls_.push_back(std::move(c));
  return true;
}

void Client::finish(std::list<Call>::iterator it, Result r) {
  Callback cb = std::move(it->done);
  if (it->fd >= 0) ::close(it->fd);
  calls_.erase(it);
  cb(r);  // last: the callback may start another request
}

void Client::update(Micros now) {
  for (auto it = calls_.begin(); it != calls_.end();) {
    auto cur = it++;
    Call& c = *cur;
    if (now >= c.deadline) {
      finish(cur, Result::failure("timed out"));
      continue;
    }
    pollfd p{c.fd, static_cast<short>(c.connected ? POLLIN : POLLOUT), 0};
    if (::poll(&p, 1, 0) <= 0) continue;
    if (!c.connected) {
      int err = 0;
      socklen_t el = sizeof err;
      ::getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &el);
      if (err != 0) {
        finish(cur, Result::failure("connect failed"));
        continue;
      }
      c.connected = true;
    }
    while (c.written < c.out.size()) {
      const ssize_t n = ::send(c.fd, c.out.data() + c.written, c.out.size() - c.written, kSendFlags);
      if (n > 0) c.written += static_cast<std::size_t>(n);
      else if (n < 0 && errno == EINTR) continue;
      else break;
    }
    if (c.written < c.out.size() && !(errno == EAGAIN || errno == EWOULDBLOCK || c.written > 0)) {
      finish(cur, Result::failure("send failed"));
      continue;
    }
    if (c.written < c.out.size()) continue;
    // Request is out: read the response.
    bool closed = false;
    char buf[4096];
    for (;;) {
      const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
      if (n > 0) {
        if (c.in.size() + static_cast<std::size_t>(n) > cfg_.limits.max_header_bytes + cfg_.limits.max_body + 8) {
          closed = true;
          c.in.assign(1, '\0');  // too much: fails below as malformed
          break;
        }
        c.in.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n == 0) closed = true;
      else if (errno == EINTR) continue;
      else if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true;
      break;
    }
    ClientResponse resp;
    const ParseResult pr = parse_response(c.in, cfg_.limits, resp);
    if (pr.state == Parse::Done) {
      finish(cur, Result::success(resp.status, std::move(resp.body), std::move(resp.headers)));
    } else if (pr.state == Parse::Error || closed) {
      finish(cur, Result::failure(pr.state == Parse::Error || !c.in.empty() ? "bad response" : "connection closed"));
    }
  }
}

std::optional<Address> resolve(std::string_view host, std::uint16_t port) {
  if (auto a = Address::parse(host, port)) return a;
  if (host.empty() || host.size() > 253) return std::nullopt;
  for (char c : host) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok) return std::nullopt;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string h(host);
  if (::getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
  std::optional<Address> best;
  for (addrinfo* p = res; p; p = p->ai_next) {
    if (p->ai_family != AF_INET && p->ai_family != AF_INET6) continue;
    sockaddr_storage ss;
    std::memset(&ss, 0, sizeof ss);
    std::memcpy(&ss, p->ai_addr, std::min<std::size_t>(p->ai_addrlen, sizeof ss));
    Address a = transport::from_sockaddr(ss);
    a.port = port;
    if (!best || (a.is_v4() && !best->is_v4())) best = a;
  }
  ::freeaddrinfo(res);
  return best;
}

std::optional<HostPort> parse_host_port(std::string_view t, std::uint16_t default_port) {
  if (t.substr(0, 7) == "http://") t.remove_prefix(7);
  if (!t.empty() && t.back() == '/') t.remove_suffix(1);
  HostPort hp;
  std::string_view port_text;
  if (!t.empty() && t.front() == '[') {
    const std::size_t close = t.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    hp.host = std::string(t.substr(1, close - 1));
    t.remove_prefix(close + 1);
    if (!t.empty()) {
      if (t.front() != ':') return std::nullopt;
      port_text = t.substr(1);
    }
  } else {
    const std::size_t colon = t.rfind(':');
    hp.host = std::string(t.substr(0, colon));
    if (colon != std::string_view::npos) port_text = t.substr(colon + 1);
  }
  if (hp.host.empty() || hp.host.find('/') != std::string::npos) return std::nullopt;
  if (port_text.empty()) {
    if (default_port == 0) return std::nullopt;
    hp.port = default_port;
    return hp;
  }
  unsigned v = 0;
  auto r = std::from_chars(port_text.data(), port_text.data() + port_text.size(), v);
  if (r.ec != std::errc() || r.ptr != port_text.data() + port_text.size() || v == 0 || v > 65535) return std::nullopt;
  hp.port = static_cast<std::uint16_t>(v);
  return hp;
}

}  // namespace npk::http
