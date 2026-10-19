// The HTTP server and client over real loopback sockets, one thread: both are
// driven by update(), here in a loop on the real clock.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <functional>
#include <thread>

#include "check.hpp"
#include "npk/http/client.hpp"
#include "npk/http/server.hpp"
#include "npk/json/json.hpp"

using namespace npk;
using namespace npk::http;
using transport::Address;
using transport::Micros;
using transport::steady_now_us;

namespace {

// Runs `step` until `done()` or the real-time limit. Returns whether done() held.
bool pump(const std::function<void(Micros)>& step, const std::function<bool()>& done, int limit_ms = 5000) {
  const Micros end = steady_now_us() + limit_ms * 1000;
  while (steady_now_us() < end) {
    step(steady_now_us());
    if (done()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// A bare TCP connection to the server that the test writes to byte by byte.
struct Raw {
  explicit Raw(const Address& to) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to.port);
    std::memcpy(&sa.sin_addr, &to.ip[12], 4);
    CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  }
  ~Raw() { ::close(fd); }
  void send(std::string_view s) { ::send(fd, s.data(), s.size(), MSG_NOSIGNAL); }
  // Reads what has arrived; sets closed when the server closed the connection.
  void read() {
    char buf[4096];
    for (;;) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n > 0) got.append(buf, static_cast<std::size_t>(n));
      else {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) closed = true;
        return;
      }
    }
  }
  int fd;
  std::string got;
  bool closed = false;
};

std::unique_ptr<Server> make_server(Server::Handler h, ServerConfig cfg = {}) {
  std::string err;
  auto s = Server::listen(Address::loopback_v4(0), std::move(h), cfg, &err);
  CHECK(s != nullptr);
  return s;
}

void echo_round_trip() {
  auto server = make_server([](const Request& r, const Address&, Responder rs) {
    json::Value v;
    v.set("method", r.method);
    v.set("path", r.path);
    v.set("query", r.query);
    v.set("body", r.body);
    v.set("auth", std::string(bearer_token(r)));
    rs.send(Response::json(r.method == "POST" ? 201 : 200, json::dump(v)));
  });
  Client client;
  std::optional<Result> got;
  OutgoingRequest q;
  q.method = "POST";
  q.path = "/echo/1?a=b";
  q.body = "{\"hello\":\"w\xc3\xb6rld\"}";
  q.headers.emplace_back("Authorization", "Bearer sekrit");
  CHECK(client.request(server->local_address(), "localhost", q, steady_now_us(), [&](const Result& r) { got = r; }));
  CHECK(pump([&](Micros n) { server->update(n); client.update(n); }, [&] { return got.has_value(); }));
  CHECK(got->ok && got->status == 201);
  auto v = json::parse(got->body);
  CHECK(v && *v->find("method")->as_string() == "POST" && *v->find("path")->as_string() == "/echo/1");
  CHECK(*v->find("query")->as_string() == "a=b" && *v->find("auth")->as_string() == "sekrit");
  CHECK(*v->find("body")->as_string() == q.body);
  pump([&](Micros n) { server->update(n); }, [&] { return server->connection_count() == 0; }, 500);
  CHECK(server->connection_count() == 0 && client.in_flight() == 0);
}

void many_at_once() {
  int served = 0;
  auto server = make_server([&](const Request&, const Address&, Responder rs) {
    ++served;
    rs.send(Response::json(200, "{}"));
  });
  Client client({.max_in_flight = 30});
  int done = 0;
  for (int i = 0; i < 30; ++i) {
    CHECK(client.request(server->local_address(), "x", {}, steady_now_us(), [&](const Result& r) { done += r.ok; }));
  }
  CHECK(!client.request(server->local_address(), "x", {}, steady_now_us(), [](const Result&) {}));  // over max_in_flight
  CHECK(pump([&](Micros n) { server->update(n); client.update(n); }, [&] { return done == 30; }));
  CHECK(served == 30);
}

void deferred_answers() {
  std::vector<Responder> waiting;
  auto server = make_server([&](const Request&, const Address&, Responder rs) { waiting.push_back(rs); });
  Client client;
  std::optional<Result> got;
  client.request(server->local_address(), "x", {}, steady_now_us(), [&](const Result& r) { got = r; });
  CHECK(pump([&](Micros n) { server->update(n); client.update(n); }, [&] { return !waiting.empty(); }));
  CHECK(!got && waiting[0].alive());
  CHECK(waiting[0].send(Response::json(200, "{\"late\":true}")));
  CHECK(!waiting[0].send(Response::json(200, "{}")));  // only the first counts
  CHECK(pump([&](Micros n) { server->update(n); client.update(n); }, [&] { return got.has_value(); }));
  CHECK(got->ok && got->body == "{\"late\":true}");
}

void answering_a_gone_peer_is_harmless() {
  std::vector<Responder> waiting;
  ServerConfig cfg;
  cfg.response_timeout = 100'000;
  auto server = make_server([&](const Request&, const Address&, Responder rs) { waiting.push_back(rs); }, cfg);
  {
    Raw raw(server->local_address());
    raw.send("GET / HTTP/1.1\r\n\r\n");
    CHECK(pump([&](Micros n) { server->update(n); }, [&] { return !waiting.empty(); }));
  }
  // The handler is too slow: the connection is dropped at the response timeout.
  CHECK(pump([&](Micros n) { server->update(n); }, [&] { return !waiting[0].alive(); }));
  CHECK(!waiting[0].send(Response::empty(200)));
  CHECK(server->stats().timeouts >= 1);
}

void refuses_bad_requests() {
  int handled = 0;
  auto server = make_server([&](const Request&, const Address&, Responder rs) { ++handled; rs.send(Response::empty(200)); });
  struct Case { std::string request; int status; };
  const Case cases[] = {
      {"GARBAGE\r\n\r\n", 400},
      {"PATCH / HTTP/1.1\r\n\r\n", 405},
      {"GET /../etc/passwd HTTP/1.1\r\n\r\n", 400},
      {"POST / HTTP/1.1\r\n\r\n", 411},
      {"POST / HTTP/1.1\r\nContent-Length: 999999\r\n\r\n", 413},
      {"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
      {"GET /" + std::string(5000, 'a') + " HTTP/1.1\r\n\r\n", 414},
      {"GET / HTTP/1.1\r\nX: " + std::string(9000, 'b') + "\r\n\r\n", 431},
      {"GET / HTTP/3\r\n\r\n", 505},
  };
  for (const Case& c : cases) {
    Raw raw(server->local_address());
    raw.send(c.request);
    CHECK(pump([&](Micros n) { server->update(n); raw.read(); }, [&] { return raw.closed; }));
    CHECK(raw.got.find("HTTP/1.1 " + std::to_string(c.status) + " ") == 0);
  }
  CHECK(handled == 0);
  // A request that is fine still works afterwards.
  Raw ok(server->local_address());
  ok.send("GET /fine HTTP/1.1\r\n\r\n");
  CHECK(pump([&](Micros n) { server->update(n); ok.read(); }, [&] { return ok.closed; }));
  CHECK(ok.got.find("HTTP/1.1 200 ") == 0 && handled == 1);
}

void a_request_in_pieces_works_and_a_slow_one_is_cut_off() {
  ServerConfig cfg;
  cfg.request_timeout = 300'000;
  int handled = 0;
  auto server = make_server([&](const Request& r, const Address&, Responder rs) {
    ++handled;
    rs.send(Response::json(200, r.body));
  }, cfg);
  const std::string req = "POST /x HTTP/1.1\r\nContent-Length: 6\r\n\r\nabcdef";
  {
    Raw raw(server->local_address());
    for (char c : req) {  // one byte at a time, quickly
      raw.send(std::string_view(&c, 1));
      server->update(steady_now_us());
    }
    CHECK(pump([&](Micros n) { server->update(n); raw.read(); }, [&] { return raw.closed; }));
    CHECK(raw.got.find("abcdef") != std::string::npos);
  }
  {
    Raw slow(server->local_address());  // a few bytes, then nothing
    slow.send("POST /x HTTP/1.1\r\nCont");
    CHECK(pump([&](Micros n) { server->update(n); slow.read(); }, [&] { return slow.closed; }));
    CHECK(slow.got.find("HTTP/1.1 408 ") == 0);
    Raw silent(server->local_address());  // not a byte
    CHECK(pump([&](Micros n) { server->update(n); silent.read(); }, [&] { return silent.closed; }));
    CHECK(silent.got.empty());
  }
  CHECK(handled == 1 && server->stats().timeouts >= 2);
}

void connection_limits() {
  ServerConfig cfg;
  cfg.max_connections = 6;
  cfg.max_per_address = 4;
  auto server = make_server([](const Request&, const Address&, Responder rs) { rs.send(Response::empty(200)); }, cfg);
  std::vector<std::unique_ptr<Raw>> held;
  for (int i = 0; i < 10; ++i) held.push_back(std::make_unique<Raw>(server->local_address()));
  pump([&](Micros n) { server->update(n); }, [&] { return server->stats().refused >= 6; }, 1000);
  CHECK(server->connection_count() == 4);  // all from 127.0.0.1: the per-address limit
  CHECK(server->stats().refused == 6);
  held.clear();
  pump([&](Micros n) { server->update(n); }, [&] { return server->connection_count() == 0; }, 1000);
  CHECK(server->connection_count() == 0);
  // The slots are free again.
  Raw again(server->local_address());
  again.send("GET / HTTP/1.1\r\n\r\n");
  CHECK(pump([&](Micros n) { server->update(n); again.read(); }, [&] { return again.closed; }));
  CHECK(again.got.find("200") != std::string::npos);
}

void client_failures() {
  Client client({.timeout = 300'000});
  // Nothing listening: connection refused.
  Address nowhere = Address::loopback_v4(1);
  std::optional<Result> got;
  client.request(nowhere, "x", {}, steady_now_us(), [&](const Result& r) { got = r; });
  CHECK(pump([&](Micros n) { client.update(n); }, [&] { return got.has_value(); }));
  CHECK(!got->ok && !got->error.empty());

  // A server that accepts and never answers: timeout.
  auto silent = make_server([](const Request&, const Address&, Responder) {});
  got.reset();
  client.request(silent->local_address(), "x", {}, steady_now_us(), [&](const Result& r) { got = r; });
  CHECK(pump([&](Micros n) { silent->update(n); client.update(n); }, [&] { return got.has_value(); }));
  CHECK(!got->ok && got->error == "timed out");

  // Bad requests never reach the network.
  OutgoingRequest bad;
  bad.path = "no-slash";
  got.reset();
  client.request(silent->local_address(), "x", bad, steady_now_us(), [&](const Result& r) { got = r; });
  CHECK(got && !got->ok && got->error == "bad request");
}

// A "server" that sends a client whatever bytes the test says.
void client_against_hostile_servers() {
  auto listener = Server::listen(Address::loopback_v4(0), [](const Request&, const Address&, Responder) {});
  CHECK(listener);
  // Use a plain socket as the hostile server.
  const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(::bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && ::listen(ls, 8) == 0);
  socklen_t sl = sizeof sa;
  ::getsockname(ls, reinterpret_cast<sockaddr*>(&sa), &sl);
  const Address where = Address::loopback_v4(ntohs(sa.sin_port));
  ::fcntl(ls, F_SETFL, ::fcntl(ls, F_GETFL, 0) | O_NONBLOCK);

  const std::string replies[] = {
      "garbage",
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab",                  // closes early
      "HTTP/1.1 200 OK\r\n\r\nno length",
      "HTTP/1.1 200 OK\r\nContent-Length: 99999999\r\n\r\n",
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
      "HTTP/1.1 200 OK\r\nX: " + std::string(20000, 'z') + "\r\n\r\n",
      std::string(200000, 'q'),
  };
  for (const std::string& reply : replies) {
    Client client({.timeout = 400'000});
    std::optional<Result> got;
    client.request(where, "x", {}, steady_now_us(), [&](const Result& r) { got = r; });
    int conn = -1;
    CHECK(pump([&](Micros n) {
      client.update(n);
      if (conn < 0) conn = ::accept(ls, nullptr, nullptr);
      if (conn >= 0 && reply.size()) {
        char buf[2048];
        ::recv(conn, buf, sizeof buf, MSG_DONTWAIT);
        std::string_view r = reply;
        ::send(conn, r.data(), r.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        ::close(conn);
        conn = -2;
      }
    }, [&] { return got.has_value(); }));
    if (conn >= 0) ::close(conn);
    CHECK(!got->ok);
  }
  ::close(ls);
}

void names_and_addresses() {
  CHECK(resolve("127.0.0.1", 80)->to_string() == "127.0.0.1:80");
  CHECK(resolve("localhost", 80).has_value());
  CHECK(!resolve("bad name", 80) && !resolve("a/b", 80) && !resolve("", 80));
  auto hp = parse_host_port("http://gameserver:9000/");
  CHECK(hp && hp->host == "gameserver" && hp->port == 9000);
  hp = parse_host_port("[::1]:8080");
  CHECK(hp && hp->host == "::1" && hp->port == 8080);
  hp = parse_host_port("lobby", 80);
  CHECK(hp && hp->port == 80);
  CHECK(!parse_host_port("lobby") && !parse_host_port("h:0") && !parse_host_port("h:99999") && !parse_host_port("h:x") &&
        !parse_host_port("") && !parse_host_port("http://h/path:80", 80) && !parse_host_port("[::1", 80));
}

}  // namespace

int main() {
  echo_round_trip();
  many_at_once();
  deferred_answers();
  answering_a_gone_peer_is_harmless();
  refuses_bad_requests();
  a_request_in_pieces_works_and_a_slow_one_is_cut_off();
  connection_limits();
  client_failures();
  client_against_hostile_servers();
  names_and_addresses();
  std::puts("http_test: ok");
  return 0;
}
