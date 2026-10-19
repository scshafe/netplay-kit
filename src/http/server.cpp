#include "npk/http/server.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

#include "../transport/sockaddr.hpp"

namespace npk::http {

using transport::Address;
using transport::Micros;

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set on each socket
#endif

struct Server::Conn {
  enum class State { Reading, Handling, Writing } state = State::Reading;
  std::uint64_t id = 0;
  int fd = -1;
  Address peer;
  Address peer_ip;  // port zeroed
  std::string in, out;
  std::size_t written = 0;
  Micros deadline = 0;
};

bool Responder::send(Response r) const { return server_ && server_->respond(id_, std::move(r)); }
bool Responder::alive() const { return server_ && server_->alive(id_); }

namespace {
bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
void set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}
}  // namespace

Server::Server(int fd, Address local, Handler h, const ServerConfig& c)
    : fd_(fd), local_(local), handler_(std::move(h)), cfg_(c) {}

Server::~Server() {
  for (auto& kv : conns_) ::close(kv.second->fd);
  ::close(fd_);
}

std::unique_ptr<Server> Server::listen(const Address& bind, Handler handler, const ServerConfig& config,
                                       std::string* error) {
  auto fail = [&](const char* what) -> std::unique_ptr<Server> {
    if (error) *error = std::string(what) + ": " + std::strerror(errno);
    return nullptr;
  };
  const bool v6 = !bind.is_v4();
  const int fd = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return fail("socket");
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (v6) {
    const int off = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
  }
  sockaddr_storage ss;
  const socklen_t len = transport::to_sockaddr(bind, v6, ss);
  if (!set_nonblocking(fd) || ::bind(fd, reinterpret_cast<sockaddr*>(&ss), len) < 0 || ::listen(fd, 128) < 0) {
    const int e = errno;
    ::close(fd);
    errno = e;
    return fail("listen");
  }
  sockaddr_storage bound;
  socklen_t blen = sizeof bound;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen);
  return std::unique_ptr<Server>(new Server(fd, transport::from_sockaddr(bound), std::move(handler), config));
}

void Server::close(std::uint64_t id) {
  const auto it = conns_.find(id);
  if (it == conns_.end()) return;
  Conn& c = *it->second;
  ::close(c.fd);
  if (auto p = per_address_.find(c.peer_ip); p != per_address_.end() && --p->second == 0) per_address_.erase(p);
  conns_.erase(it);
}

void Server::accept_new(Micros now) {
  for (int n = 0; n < 64; ++n) {
    sockaddr_storage ss;
    socklen_t len = sizeof ss;
    const int fd = ::accept(fd_, reinterpret_cast<sockaddr*>(&ss), &len);
    if (fd < 0) return;  // EAGAIN: nothing waiting; other errors: try again next update
    Address peer = transport::from_sockaddr(ss);
    Address ip = peer;
    ip.port = 0;
    if (conns_.size() >= cfg_.max_connections || per_address_[ip] >= cfg_.max_per_address || !set_nonblocking(fd)) {
      if (auto p = per_address_.find(ip); p != per_address_.end() && p->second == 0) per_address_.erase(p);
      ::close(fd);
      ++stats_.refused;
      continue;
    }
    set_nosigpipe(fd);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto c = std::make_unique<Conn>();
    c->id = next_id_++;
    c->fd = fd;
    c->peer = peer;
    c->peer_ip = ip;
    c->deadline = now + cfg_.request_timeout;
    ++per_address_[ip];
    ++stats_.accepted;
    conns_[c->id] = std::move(c);
  }
}

void Server::queue(Conn& c, Response r, Micros now) {
  c.out = serialize(r);
  c.written = 0;
  c.state = Conn::State::Writing;
  c.deadline = now + cfg_.write_timeout;
  c.in.clear();
  c.in.shrink_to_fit();
}

bool Server::respond(std::uint64_t id, Response r) {
  const auto it = conns_.find(id);
  if (it == conns_.end() || it->second->state != Conn::State::Handling) return false;
  queue(*it->second, std::move(r), now_);
  write_to(*it->second, now_);
  return true;
}

bool Server::alive(std::uint64_t id) const {
  const auto it = conns_.find(id);
  return it != conns_.end() && it->second->state == Conn::State::Handling;
}

void Server::read_from(Conn& c, Micros now) {
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
    if (n > 0) {
      // Never hold more than a complete request can be: head limit plus body limit plus slack.
      if (c.in.size() + static_cast<std::size_t>(n) > cfg_.limits.max_header_bytes + cfg_.limits.max_body + 8) {
        queue(c, Response::empty(413), now);
        ++stats_.bad_requests;
        return;
      }
      c.in.append(buf, static_cast<std::size_t>(n));
      Request req;
      const ParseResult pr = parse_request(c.in, cfg_.limits, req);
      if (pr.state == Parse::Error) {
        queue(c, Response::empty(pr.error_status), now);
        ++stats_.bad_requests;
        return;
      }
      if (pr.state == Parse::Done) {
        c.state = Conn::State::Handling;
        c.deadline = now + cfg_.response_timeout;
        ++stats_.requests;
        const std::uint64_t id = c.id;
        c.in.clear();
        handler_(req, c.peer, Responder(this, id));
        return;
      }
      continue;
    }
    if (n == 0) {  // the peer closed before finishing
      c.state = Conn::State::Writing;
      c.out.clear();
      c.written = 0;
      c.deadline = now;
      return;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      c.out.clear();
      c.state = Conn::State::Writing;
      c.deadline = now;
    }
    return;
  }
}

void Server::write_to(Conn& c, Micros now) {
  while (c.written < c.out.size()) {
    const ssize_t n = ::send(c.fd, c.out.data() + c.written, c.out.size() - c.written, kSendFlags);
    if (n > 0) {
      c.written += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    c.out.clear();  // broken: give up
    c.written = 0;
    break;
  }
  (void)now;
  c.deadline = 0;  // done: closed in the next sweep
}

void Server::update(Micros now) {
  now_ = now;
  accept_new(now);
  std::vector<std::uint64_t> to_close;
  for (auto& kv : conns_) {
    Conn& c = *kv.second;
    // Look for input only while reading (to notice a handler's peer leaving is not needed).
    if (c.state == Conn::State::Reading) {
      pollfd p{c.fd, POLLIN, 0};
      if (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) read_from(c, now);
    } else if (c.state == Conn::State::Writing && c.written < c.out.size()) {
      write_to(c, now);
    }
    if (c.state == Conn::State::Writing && c.written >= c.out.size()) {
      to_close.push_back(c.id);
    } else if (now >= c.deadline) {
      if (c.state == Conn::State::Reading || c.state == Conn::State::Handling) {
        ++stats_.timeouts;
        if (c.state == Conn::State::Reading && !c.in.empty()) {
          queue(c, Response::empty(408), now);  // best effort, short deadline
          write_to(c, now);
          to_close.push_back(c.id);
          continue;
        }
      }
      to_close.push_back(c.id);
    }
  }
  for (const auto id : to_close) close(id);
}

}  // namespace npk::http
