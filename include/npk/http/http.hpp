#pragma once

// HTTP/1.1 as far as the lobby and the control channel need it
// (docs/decisions/0004-http-and-json.md): one request per connection, bodies by
// Content-Length only, no chunked encoding, no upgrade, no TLS. The parsers here
// are pure functions over the bytes received so far, so they can be fuzzed; the
// server and the client (server.hpp, client.hpp) wrap them in sockets.
//
// Everything is bounded by Limits. CRLF line ends only. Header names are
// lowercased; a Content-Length given twice must agree; Transfer-Encoding and
// Expect are refused rather than half-supported.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace npk::http {

struct Limits {
  std::size_t max_request_line = 2048;   // method, target and version
  std::size_t max_header_bytes = 8192;   // the whole head, request line included
  std::size_t max_headers = 32;
  std::size_t max_body = 64 * 1024;
};

using Headers = std::vector<std::pair<std::string, std::string>>;  // names lowercased

struct Request {
  std::string method;  // GET, POST, PUT or DELETE
  std::string path;    // the target before '?': starts with '/', printable ASCII, no "..", no '%'
  std::string query;   // after '?', raw
  Headers headers;
  std::string body;
  const std::string* header(std::string_view lowercase_name) const;
};

struct Response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  Headers headers;  // extra; names and values must not contain CR or LF

  static Response empty(int status) { return Response{status, "application/json", {}, {}}; }
  static Response json(int status, std::string body) { return Response{status, "application/json", std::move(body), {}}; }
};

enum class Parse { NeedMore, Done, Error };

struct ParseResult {
  Parse state = Parse::NeedMore;
  int error_status = 400;     // Error: what a server answers with
  std::size_t consumed = 0;   // Done: bytes used (anything after is ignored: the connection closes)
};

// `data` is everything received so far on the connection.
ParseResult parse_request(std::string_view data, const Limits& limits, Request& out);

struct ClientResponse {
  int status = 0;
  Headers headers;
  std::string body;
  const std::string* header(std::string_view lowercase_name) const;
};
// A response needs a Content-Length (the kit's own server always sends one); one
// without is refused. The body limit is Limits::max_body.
ParseResult parse_response(std::string_view data, const Limits& limits, ClientResponse& out);

const char* reason_phrase(int status);
// "HTTP/1.1 200 OK", Content-Type, Content-Length, Connection: close, headers, body.
std::string serialize(const Response& r);

struct OutgoingRequest {
  std::string method = "GET";
  std::string path = "/";
  Headers headers;   // e.g. authorization
  std::string body;
  std::string content_type = "application/json";
};
// False if the method, path or a header holds a character that does not belong.
bool serialize_request(const OutgoingRequest& r, std::string_view host, std::string& out);

// Compares without stopping at the first difference (for secrets and tokens).
bool constant_time_equal(std::string_view a, std::string_view b);
// The token of an "Authorization: Bearer <token>" header, if there is exactly that.
std::string_view bearer_token(const Request& r);

}  // namespace npk::http
