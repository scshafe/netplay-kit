#pragma once

// A small non-blocking HTTP server (docs/decisions/0004-http-and-json.md), driven
// by update(now) like the rest of the kit: no threads. One request per connection.
//
// Hostile-input rules: the whole request must arrive within `request_timeout`
// (slow senders are cut off), the head and body are bounded (http.hpp Limits),
// connections are bounded in total and per source address, and a connection is
// closed after its response. A handler may answer later (long-polling) through
// the Responder it was given; `response_timeout` bounds how long it may take.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "npk/http/http.hpp"
#include "npk/transport/address.hpp"
#include "npk/transport/time.hpp"

namespace npk::http {

class Server;

// A handle for answering one request, possibly later. Copyable; it must not
// outlive the Server. Sending to a connection that has since gone is a no-op.
class Responder {
 public:
  Responder() = default;
  // Answers the request. Only the first call counts. False if the connection is gone or already answered.
  bool send(Response r) const;
  bool alive() const;

 private:
  friend class Server;
  Responder(Server* s, std::uint64_t id) : server_(s), id_(id) {}
  Server* server_ = nullptr;
  std::uint64_t id_ = 0;
};

struct ServerConfig {
  Limits limits{};
  transport::Micros request_timeout = 10'000'000;   // from accept until the request is complete
  transport::Micros response_timeout = 40'000'000;  // from the request until the response is queued
  transport::Micros write_timeout = 10'000'000;     // to send the response out
  std::size_t max_connections = 256;
  std::size_t max_per_address = 32;
};

struct ServerStats {
  std::uint64_t accepted = 0;
  std::uint64_t refused = 0;        // over a connection limit
  std::uint64_t requests = 0;       // complete and handed to the handler
  std::uint64_t bad_requests = 0;   // answered with a parse error
  std::uint64_t timeouts = 0;
};

class Server {
 public:
  using Handler = std::function<void(const Request&, const transport::Address& peer, Responder)>;

  // Listens on `bind` (port 0 picks one). Null on failure, with the reason in *error.
  static std::unique_ptr<Server> listen(const transport::Address& bind, Handler handler,
                                        const ServerConfig& config = {}, std::string* error = nullptr);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void update(transport::Micros now);
  transport::Address local_address() const { return local_; }
  std::size_t connection_count() const { return conns_.size(); }
  const ServerStats& stats() const { return stats_; }

 private:
  friend class Responder;
  struct Conn;
  Server(int fd, transport::Address local, Handler h, const ServerConfig& c);
  bool respond(std::uint64_t id, Response r);
  bool alive(std::uint64_t id) const;
  void accept_new(transport::Micros now);
  void read_from(Conn& c, transport::Micros now);
  void write_to(Conn& c, transport::Micros now);
  void queue(Conn& c, Response r, transport::Micros now);
  void close(std::uint64_t id);

  int fd_;
  transport::Address local_;
  Handler handler_;
  ServerConfig cfg_;
  transport::Micros now_ = 0;
  std::uint64_t next_id_ = 1;
  std::unordered_map<std::uint64_t, std::unique_ptr<Conn>> conns_;
  std::unordered_map<transport::Address, std::size_t> per_address_;
  ServerStats stats_;
};

}  // namespace npk::http
