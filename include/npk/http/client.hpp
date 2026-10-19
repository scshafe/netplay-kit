#pragma once

// A small non-blocking HTTP client for the lobby and the game servers' calls to
// each other. One request per connection, answered through a callback from
// update(). Every request has a deadline for the whole exchange; a response is
// bounded like a request (http.hpp Limits). Addresses are IP literals or names
// resolved with `resolve()` (which can block briefly on DNS).

#include <functional>
#include <list>
#include <string>

#include "npk/http/http.hpp"
#include "npk/transport/address.hpp"
#include "npk/transport/time.hpp"

namespace npk::http {

struct Result {
  bool ok = false;     // a complete, well-formed response arrived
  int status = 0;      // when ok
  std::string body;    // when ok
  std::string error;   // when not ok: "connect failed", "timed out", ...

  static Result failure(std::string why) { return Result{false, 0, {}, std::move(why)}; }
  static Result success(int status, std::string body) { return Result{true, status, std::move(body), {}}; }
};

struct ClientConfig {
  Limits limits{};
  transport::Micros timeout = 5'000'000;
  std::size_t max_in_flight = 16;
};

class Client {
 public:
  using Callback = std::function<void(const Result&)>;

  explicit Client(const ClientConfig& config = {});
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Starts a request; the callback runs from a later update() (or this call, if it fails at once).
  // `host` goes in the Host header. False if too many are in flight.
  bool request(const transport::Address& to, std::string_view host, const OutgoingRequest& req,
               transport::Micros now, Callback done);
  void update(transport::Micros now);
  std::size_t in_flight() const { return calls_.size(); }

 private:
  struct Call;
  void finish(std::list<Call>::iterator it, Result r);
  ClientConfig cfg_;
  std::list<Call> calls_;
};

// Resolves a host name or IP literal (IPv4 preferred). Blocks for the lookup; the
// name must be a plain host name (letters, digits, '-', '.').
std::optional<transport::Address> resolve(std::string_view host, std::uint16_t port);

// "host:port" or "[v6]:port" or "http://host:port[/]" into its parts. No other URL forms.
struct HostPort {
  std::string host;
  std::uint16_t port = 0;
};
std::optional<HostPort> parse_host_port(std::string_view text, std::uint16_t default_port = 0);

}  // namespace npk::http
