#pragma once

// What the executables share: configuration from the environment, a stop flag set
// by SIGINT/SIGTERM, and a log line on stderr. Secrets come by *name*: the
// variable `NPK_CONTROL_SECRET_VAR` (default `NPK_CONTROL_SECRET`) holds the name
// of the variable that holds the secret, so a stack's .env can pass any name and
// the secret itself never appears in a command line or a compose file.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

namespace npk::app {

inline volatile std::sig_atomic_t g_stop = 0;
inline void on_signal(int) { g_stop = 1; }
inline void install_signal_handlers() {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);
}
inline bool stop_requested() { return g_stop != 0; }

inline std::string env_string(const char* name, std::string fallback = {}) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::move(fallback);
}

// An integer in [lo, hi] from the environment; the fallback if unset. Sets *ok false for a malformed or out-of-range value.
inline long env_int(const char* name, long fallback, long lo, long hi, bool* ok) {
  const char* v = std::getenv(name);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 10);
  if (*end != '\0' || n < lo || n > hi) {
    *ok = false;
    std::fprintf(stderr, "%s: expected an integer from %ld to %ld\n", name, lo, hi);
    return fallback;
  }
  return n;
}

// The secret named by NPK_CONTROL_SECRET_VAR (default NPK_CONTROL_SECRET). Empty if unset.
inline std::string control_secret(std::string* var_name = nullptr) {
  const std::string name = env_string("NPK_CONTROL_SECRET_VAR", "NPK_CONTROL_SECRET");
  if (var_name) *var_name = name;
  return env_string(name.c_str());
}

// One line on stderr. Never pass it a secret or a token.
inline void log_line(std::string_view who, std::string_view what) {
  const std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  std::fprintf(stderr, "%s %.*s: %.*s\n", buf, static_cast<int>(who.size()), who.data(), static_cast<int>(what.size()), what.data());
}

}  // namespace npk::app
