// The HTTP request and response parsers: valid requests, each refusal with its
// status, incremental arrival (every split point), the limits, and random and
// mutated input.

#include <cstdio>
#include <random>

#include "check.hpp"
#include "npk/http/http.hpp"

using namespace npk::http;

namespace {

ParseResult parse(std::string_view s, Request& r, const Limits& l = {}) { return parse_request(s, l, r); }

int status_of(std::string_view s, const Limits& l = {}) {
  Request r;
  const auto p = parse(s, r, l);
  return p.state == Parse::Error ? p.error_status : p.state == Parse::Done ? 0 : -1;
}

void valid() {
  Request r;
  auto p = parse("GET /api/rooms?x=1&y=2 HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer abc\r\n\r\n", r);
  CHECK(p.state == Parse::Done && r.method == "GET" && r.path == "/api/rooms" && r.query == "x=1&y=2");
  CHECK(r.body.empty() && *r.header("host") == "a" && bearer_token(r) == "abc");
  const std::string post = "POST /a HTTP/1.1\r\nContent-Length: 5\r\nContent-Type: application/json\r\n\r\nhello";
  p = parse(post, r);
  CHECK(p.state == Parse::Done && r.body == "hello" && p.consumed == post.size());
  p = parse(post + "GET / HTTP/1.1\r\n\r\n", r);  // anything after the body is left alone
  CHECK(p.state == Parse::Done && p.consumed == post.size());
  CHECK(parse("DELETE /x HTTP/1.0\r\n\r\n", r).state == Parse::Done);
  CHECK(parse("GET / HTTP/1.1\r\nX-Odd:   spaced value \t\r\n\r\n", r).state == Parse::Done && *r.header("x-odd") == "spaced value");
  CHECK(parse("GET / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc", r).state == Parse::Done);
  // Header names are case-insensitive.
  CHECK(parse("GET / HTTP/1.1\r\nCONTENT-TYPE: x\r\n\r\n", r).state == Parse::Done && r.header("content-type"));
  CHECK(bearer_token(r).empty());
}

void every_split_point() {
  const std::string post = "POST /rooms HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\n{\"a\":[1,2]}";
  for (std::size_t cut = 0; cut < post.size(); ++cut) {
    Request r;
    CHECK(parse(std::string_view(post).substr(0, cut), r).state == Parse::NeedMore);
  }
  Request r;
  CHECK(parse(post, r).state == Parse::Done && r.body == "{\"a\":[1,2]}");
}

void refusals() {
  CHECK(status_of("GET / HTTP/1.1\r\nHost: a\r\n\r\n") == 0);
  CHECK(status_of("GET\r\n\r\n") == 400);
  CHECK(status_of("GET /\r\n\r\n") == 400);
  CHECK(status_of("GET / HTTP/1.1 extra\r\n\r\n") == 400 || status_of("GET / HTTP/1.1 extra\r\n\r\n") == 505);
  CHECK(status_of(" GET / HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("GET / HTTP/2.0\r\n\r\n") == 505);
  CHECK(status_of("GET / FTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("PATCH / HTTP/1.1\r\n\r\n") == 405);
  CHECK(status_of("get / HTTP/1.1\r\n\r\n") == 405);
  CHECK(status_of("GET x HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("GET http://evil/ HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("GET /a/../b HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("GET /a%2e%2e/b HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("GET /a//b HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of("GET /a\\b HTTP/1.1\r\n\r\n") == 400);
  CHECK(status_of(std::string("GET /a\x01 HTTP/1.1\r\n\r\n")) == 400);
  CHECK(status_of("GET / HTTP/1.1\nHost: a\n\n") == -1);  // bare LF: never completes, times out
  CHECK(status_of("GET / HTTP/1.1\r\nBad Name: x\r\n\r\n") == 400);
  CHECK(status_of("GET / HTTP/1.1\r\n: x\r\n\r\n") == 400);
  CHECK(status_of("GET / HTTP/1.1\r\nNoColon\r\n\r\n") == 400);
  CHECK(status_of("GET / HTTP/1.1\r\n folded: x\r\n\r\n") == 400);
  CHECK(status_of(std::string("GET / HTTP/1.1\r\nX: a\x01" "b\r\n\r\n")) == 400);
  CHECK(status_of(std::string("GET / HTTP/1.1\r\nX: a") + std::string(1, '\0') + "b\r\n\r\n") == 400);
  CHECK(status_of("POST / HTTP/1.1\r\n\r\n") == 411);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: x\r\n\r\n") == 400);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n") == 400);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n") == 400);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd") == 400);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 70000\r\n\r\n") == 413);
  CHECK(status_of("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == 501);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc") == 501);
  CHECK(status_of("POST / HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n") == 417);
}

void limits() {
  Limits l;
  l.max_request_line = 40;
  l.max_header_bytes = 100;
  l.max_headers = 3;
  l.max_body = 10;
  CHECK(status_of("GET /" + std::string(60, 'a') + " HTTP/1.1\r\n\r\n", l) == 414);
  CHECK(status_of("GET /" + std::string(60, 'a'), l) == 414);  // not even finished: already too long
  CHECK(status_of("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n\r\n", l) == 431);
  CHECK(status_of("GET / HTTP/1.1\r\nA: " + std::string(120, 'x') + "\r\n\r\n", l) == 431);
  CHECK(status_of("GET / HTTP/1.1\r\nA: " + std::string(120, 'x'), l) == 431);  // head never ends within the limit
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n", l) == 413);
  CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n0123456789", l) == 0);
  CHECK(status_of("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", l) == 0);
}

void responses() {
  ClientResponse c;
  Limits l;
  auto p = parse_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi", l, c);
  CHECK(p.state == Parse::Done && c.status == 200 && c.body == "hi" && *c.header("connection") == "close");
  CHECK(parse_response("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n", l, c).state == Parse::Done);
  CHECK(parse_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhi", l, c).state == Parse::NeedMore);
  for (const char* bad : {"HTTP/1.1 200 OK\r\n\r\nhi", "HTTP/1.1 100 Continue\r\nContent-Length: 0\r\n\r\n",
                          "HTTP/1.1 20 OK\r\nContent-Length: 0\r\n\r\n", "HTTP/2 200 OK\r\nContent-Length: 0\r\n\r\n",
                          "HTTP/1.1 200OK\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 999 X\r\nContent-Length: 0\r\n\r\n",
                          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
                          "HTTP/1.1 200 OK\r\nContent-Length: 70000\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab",
                          "garbage\r\n\r\n"}) {
    CHECK(parse_response(bad, l, c).state == Parse::Error);
  }
  // A response's head that never ends.
  CHECK(parse_response("HTTP/1.1 200 OK\r\nX: " + std::string(9000, 'y'), l, c).state == Parse::Error);
}

void output() {
  Response r = Response::json(201, "{\"ok\":true}");
  r.headers.emplace_back("X-Thing", "a\r\nInjected: yes");  // refused
  r.headers.emplace_back("X-Good", "fine");
  const std::string s = serialize(r);
  CHECK(s.find("Injected") == std::string::npos && s.find("X-Good: fine\r\n") != std::string::npos);
  CHECK(s.find("HTTP/1.1 201 Created\r\n") == 0 && s.find("Content-Length: 11\r\n") != std::string::npos);
  CHECK(s.find("Connection: close\r\n") != std::string::npos && s.substr(s.size() - 11) == "{\"ok\":true}");
  ClientResponse c;
  CHECK(parse_response(s, {}, c).state == Parse::Done && c.status == 201);

  std::string out;
  OutgoingRequest q;
  q.method = "POST";
  q.path = "/rooms";
  q.body = "{}";
  q.headers.emplace_back("Authorization", "Bearer s3cret");
  CHECK(serialize_request(q, "game:9000", out));
  Request back;
  CHECK(parse_request(out, {}, back).state == Parse::Done && back.body == "{}" && bearer_token(back) == "s3cret" &&
        *back.header("host") == "game:9000");
  q.path = "/a b";
  CHECK(!serialize_request(q, "h", out));
  q.path = "/ok";
  q.headers.emplace_back("X", "a\nb");
  CHECK(!serialize_request(q, "h", out));
  q.headers.pop_back();
  q.method = "TRACE";
  CHECK(!serialize_request(q, "h", out));

  CHECK(constant_time_equal("abc", "abc") && !constant_time_equal("abc", "abd") && !constant_time_equal("abc", "ab") &&
        !constant_time_equal("", "a") && constant_time_equal("", ""));
}

void garbage() {
  std::mt19937_64 rng(11);
  const std::string seeds[] = {
      "POST /api/rooms/3/join?x=1 HTTP/1.1\r\nHost: lobby\r\nAuthorization: Bearer tok\r\nContent-Length: 13\r\n\r\n{\"name\":\"bob\"}",
      "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"};
  const char alphabet[] = "GETPOSDLHTP/1.\r\n :;,-%?&=0123456789abcxyz\xff\x80";
  Limits l;
  l.max_body = 64;
  for (int i = 0; i < 300000; ++i) {
    std::string t;
    if (i % 4 == 0) {
      t.resize(rng() % 80);
      for (auto& c : t) c = alphabet[rng() % (sizeof alphabet - 1)];
    } else {
      t = seeds[rng() % 2];
      for (int k = 0, n = 1 + static_cast<int>(rng() % 4); k < n && !t.empty(); ++k) {
        const std::size_t at = rng() % t.size();
        switch (rng() % 3) {
          case 0: t[at] = alphabet[rng() % (sizeof alphabet - 1)]; break;
          case 1: t.erase(at, 1 + rng() % 3); break;
          default: t.insert(at, 1, alphabet[rng() % (sizeof alphabet - 1)]);
        }
      }
    }
    Request r;
    const auto p = parse_request(t, l, r);
    if (p.state == Parse::Done) {
      CHECK(p.consumed <= t.size() && r.path.size() > 0 && r.path[0] == '/' && r.body.size() <= l.max_body);
      CHECK(r.path.find("..") == std::string::npos && r.path.find('%') == std::string::npos);
    }
    if (p.state == Parse::Error) CHECK(p.error_status >= 400 && p.error_status < 600);
    ClientResponse c;
    (void)parse_response(t, l, c);
  }
}

}  // namespace

int main() {
  valid();
  every_split_point();
  refusals();
  limits();
  responses();
  output();
  garbage();
  std::puts("http_parse_test: ok");
  return 0;
}
