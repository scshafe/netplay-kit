#include "npk/http/http.hpp"

#include <charconv>

namespace npk::http {

namespace {

bool is_token_char(char c) {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

bool is_value_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u == '\t' || (u >= 0x20 && u != 0x7f);  // obs-text (>= 0x80) is allowed in values
}

bool is_target_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u > 0x20 && u < 0x7f;
}

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

ParseResult error(int status) {
  ParseResult r;
  r.state = Parse::Error;
  r.error_status = status;
  return r;
}

// Finds the end of the head ("\r\n\r\n") within the limit. Returns its offset,
// npos if not yet, and sets too_big when the limit passed without one.
std::size_t find_head_end(std::string_view data, std::size_t limit, bool& too_big) {
  too_big = false;
  const std::size_t at = data.find("\r\n\r\n");
  if (at == std::string_view::npos) {
    too_big = data.size() > limit;
    return std::string_view::npos;
  }
  if (at + 4 > limit) {
    too_big = true;
    return std::string_view::npos;
  }
  return at;
}

// Parses header lines (each ends in CRLF) into out; the last line's CRLF is included in `lines`.
// Returns 0 on success, else the status to answer with.
int parse_headers(std::string_view lines, const Limits& limits, Headers& out) {
  while (!lines.empty()) {
    const std::size_t eol = lines.find("\r\n");
    if (eol == std::string_view::npos) return 400;
    const std::string_view line = lines.substr(0, eol);
    lines.remove_prefix(eol + 2);
    if (line.empty()) return 400;
    if (line.front() == ' ' || line.front() == '\t') return 400;  // obsolete line folding
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return 400;
    const std::string_view name = line.substr(0, colon);
    for (char c : name) {
      if (!is_token_char(c)) return 400;
    }
    const std::string_view value = trim(line.substr(colon + 1));
    for (char c : value) {
      if (!is_value_char(c)) return 400;
    }
    if (out.size() >= limits.max_headers) return 431;
    out.emplace_back(lower(name), std::string(value));
  }
  return 0;
}

const std::string* find_header(const Headers& h, std::string_view name) {
  for (const auto& kv : h) {
    if (kv.first == name) return &kv.second;
  }
  return nullptr;
}

// The Content-Length of the headers: -1 if absent, -2 if invalid or conflicting.
long long content_length(const Headers& h, std::size_t max) {
  long long found = -1;
  for (const auto& kv : h) {
    if (kv.first != "content-length") continue;
    if (kv.second.empty() || kv.second.size() > 10) return -2;
    unsigned long long v = 0;
    auto r = std::from_chars(kv.second.data(), kv.second.data() + kv.second.size(), v);
    if (r.ec != std::errc() || r.ptr != kv.second.data() + kv.second.size()) return -2;
    if (found >= 0 && static_cast<unsigned long long>(found) != v) return -2;
    found = v > max ? static_cast<long long>(max) + 1 : static_cast<long long>(v);
  }
  return found;
}

}  // namespace

const std::string* Request::header(std::string_view name) const { return find_header(headers, name); }
const std::string* ClientResponse::header(std::string_view name) const { return find_header(headers, name); }

ParseResult parse_request(std::string_view data, const Limits& limits, Request& out) {
  bool too_big;
  const std::size_t end = find_head_end(data, limits.max_header_bytes, too_big);
  if (end == std::string_view::npos) {
    if (!too_big) {
      // A request line that is already too long need not wait for its end.
      const std::size_t eol = data.find("\r\n");
      if (eol == std::string_view::npos && data.size() > limits.max_request_line) return error(414);
      return {};
    }
    const std::size_t eol = data.find("\r\n");
    return error(eol == std::string_view::npos || eol > limits.max_request_line ? 414 : 431);
  }
  const std::string_view head = data.substr(0, end + 2);  // includes the last header's CRLF
  const std::size_t line_end = head.find("\r\n");
  const std::string_view line = head.substr(0, line_end);
  if (line.size() > limits.max_request_line) return error(414);

  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0) return error(400);
  const std::string_view method = line.substr(0, sp1);
  const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view version = line.substr(sp2 + 1);
  for (char c : method) {
    if (!is_token_char(c)) return error(400);
  }
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return error(version.substr(0, 5) == "HTTP/" ? 505 : 400);
  if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE") return error(405);
  if (target.empty() || target.front() != '/') return error(400);
  for (char c : target) {
    if (!is_target_char(c)) return error(400);
  }
  const std::size_t q = target.find('?');
  const std::string_view path = target.substr(0, q);
  if (path.find('%') != std::string_view::npos || path.find("..") != std::string_view::npos ||
      path.find('\\') != std::string_view::npos || path.find("//") != std::string_view::npos)
    return error(400);

  Request req;
  req.method = std::string(method);
  req.path = std::string(path);
  if (q != std::string_view::npos) req.query = std::string(target.substr(q + 1));
  if (const int bad = parse_headers(head.substr(line_end + 2), limits, req.headers)) return error(bad);
  if (find_header(req.headers, "transfer-encoding")) return error(501);
  if (find_header(req.headers, "expect")) return error(417);
  const long long len = content_length(req.headers, limits.max_body);
  if (len == -2) return error(400);
  if (len > static_cast<long long>(limits.max_body)) return error(413);
  if (len < 0 && (req.method == "POST" || req.method == "PUT")) return error(411);
  const std::size_t body_len = len < 0 ? 0 : static_cast<std::size_t>(len);
  const std::size_t total = end + 4 + body_len;
  if (data.size() < total) return {};
  req.body = std::string(data.substr(end + 4, body_len));
  out = std::move(req);
  ParseResult r;
  r.state = Parse::Done;
  r.consumed = total;
  return r;
}

ParseResult parse_response(std::string_view data, const Limits& limits, ClientResponse& out) {
  bool too_big;
  const std::size_t end = find_head_end(data, limits.max_header_bytes, too_big);
  if (end == std::string_view::npos) return too_big ? error(502) : ParseResult{};
  const std::string_view head = data.substr(0, end + 2);
  const std::size_t line_end = head.find("\r\n");
  const std::string_view line = head.substr(0, line_end);
  // "HTTP/1.x SSS reason"
  if (line.size() < 12 || line.substr(0, 7) != "HTTP/1." || (line[7] != '0' && line[7] != '1') || line[8] != ' ' ||
      (line.size() > 12 && line[12] != ' '))
    return error(502);
  int status = 0;
  for (std::size_t i = 9; i < 12; ++i) {
    if (line[i] < '0' || line[i] > '9') return error(502);
    status = status * 10 + (line[i] - '0');
  }
  if (status < 200 || status > 599) return error(502);  // no 1xx: nothing sends Expect
  ClientResponse resp;
  resp.status = status;
  if (parse_headers(head.substr(line_end + 2), limits, resp.headers) != 0) return error(502);
  if (find_header(resp.headers, "transfer-encoding")) return error(502);
  const long long len = content_length(resp.headers, limits.max_body);
  if (len < 0 || len > static_cast<long long>(limits.max_body)) return error(502);
  const std::size_t total = end + 4 + static_cast<std::size_t>(len);
  if (data.size() < total) return {};
  resp.body = std::string(data.substr(end + 4, static_cast<std::size_t>(len)));
  out = std::move(resp);
  ParseResult r;
  r.state = Parse::Done;
  r.consumed = total;
  return r;
}

const char* reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 417: return "Expectation Failed";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
  }
}

namespace {
bool clean(std::string_view s) {
  for (char c : s) {
    if (c == '\r' || c == '\n' || c == '\0') return false;
  }
  return true;
}
}  // namespace

std::string serialize(const Response& r) {
  std::string o = "HTTP/1.1 " + std::to_string(r.status) + " " + reason_phrase(r.status) + "\r\n";
  if (clean(r.content_type) && !r.body.empty()) o += "Content-Type: " + r.content_type + "\r\n";
  o += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n";
  for (const auto& kv : r.headers) {
    if (clean(kv.first) && clean(kv.second) && !kv.first.empty()) o += kv.first + ": " + kv.second + "\r\n";
  }
  o += "\r\n";
  o += r.body;
  return o;
}

bool serialize_request(const OutgoingRequest& r, std::string_view host, std::string& out) {
  if (r.method != "GET" && r.method != "POST" && r.method != "PUT" && r.method != "DELETE") return false;
  if (r.path.empty() || r.path.front() != '/') return false;
  for (char c : r.path) {
    if (!is_target_char(c)) return false;
  }
  if (!clean(host) || host.empty() || !clean(r.content_type)) return false;
  std::string o = r.method + " " + r.path + " HTTP/1.1\r\nHost: " + std::string(host) + "\r\nConnection: close\r\n";
  for (const auto& kv : r.headers) {
    if (kv.first.empty() || !clean(kv.first) || !clean(kv.second)) return false;
    for (char c : kv.first) {
      if (!is_token_char(c)) return false;
    }
    o += kv.first + ": " + kv.second + "\r\n";
  }
  if (!r.body.empty() || r.method == "POST" || r.method == "PUT") {
    o += "Content-Type: " + r.content_type + "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\n";
  }
  o += "\r\n";
  o += r.body;
  out = std::move(o);
  return true;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
  // The length is not secret (the secret's length is configuration, not content), the bytes are.
  unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
  const std::size_t n = a.size() < b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

std::string_view bearer_token(const Request& r) {
  const std::string* h = r.header("authorization");
  if (!h || h->size() < 8) return {};
  const std::string_view v = *h;
  if (lower(v.substr(0, 7)) != "bearer ") return {};
  const std::string_view token = trim(v.substr(7));
  for (char c : token) {
    if (c == ' ' || c == '\t') return {};
  }
  return token;
}

}  // namespace npk::http
