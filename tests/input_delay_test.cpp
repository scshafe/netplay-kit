#include <cstdio>

#include "check.hpp"
#include "npk/session/input_delay.hpp"

using namespace npk::session;

namespace {

constexpr npk::transport::Micros ms = 1000;

void the_rule() {
  InputDelayConfig c;  // 60 Hz: a frame is 16.667 ms
  CHECK(target_input_delay(0, 0, c) == 1);                // clamped up
  CHECK(target_input_delay(20 * ms, 0, c) == 1);          // 10 ms one way
  CHECK(target_input_delay(40 * ms, 0, c) == 2);          // 20 ms: 1.2 frames
  CHECK(target_input_delay(66 * ms, 0, c) == 2);          // 33 ms: just under 2 frames
  CHECK(target_input_delay(68 * ms, 0, c) == 3);          // 34 ms: just over
  CHECK(target_input_delay(40 * ms, 5 * ms, c) == 2);     // 20 + 10 = 30 ms
  CHECK(target_input_delay(40 * ms, 10 * ms, c) == 3);    // 20 + 20 = 40 ms
  CHECK(target_input_delay(900 * ms, 0, c) == 6);         // clamped down
  CHECK(target_input_delay(-5, -5, c) == 1);              // nonsense in, still in range
  InputDelayConfig c30;
  c30.tick_rate_hz = 30;  // a frame is 33.3 ms
  CHECK(target_input_delay(60 * ms, 0, c30) == 1);
  CHECK(target_input_delay(100 * ms, 0, c30) == 2);
}

void start_takes_the_target() {
  InputDelay d;
  d.start(80 * ms, 5 * ms);  // 40 + 10 = 50 ms: 3 frames
  CHECK(d.current() == 3);
}

void raises_after_a_streak_and_jumps_to_the_target() {
  InputDelay d;
  d.start(20 * ms, 0);
  CHECK(d.current() == 1);
  CHECK(d.evaluate(100 * ms, 0, 0) == 1);  // wants 3; once
  CHECK(d.evaluate(100 * ms, 0, 0) == 1);  // twice
  CHECK(d.evaluate(100 * ms, 0, 0) == 3);  // thrice: goes there at once
}

void a_dip_resets_the_streak() {
  InputDelay d;
  d.start(20 * ms, 0);
  d.evaluate(100 * ms, 0, 0);
  d.evaluate(100 * ms, 0, 0);
  d.evaluate(20 * ms, 0, 0);               // back to what it has: streak is gone
  CHECK(d.evaluate(100 * ms, 0, 0) == 1);  // starts again
  CHECK(d.evaluate(100 * ms, 0, 0) == 1);
  CHECK(d.evaluate(100 * ms, 0, 0) == 3);
}

void lowers_only_when_calm_and_by_one() {
  InputDelay d;
  d.start(100 * ms, 0);
  CHECK(d.current() == 3);
  for (int i = 0; i < 10; ++i) CHECK(d.evaluate(20 * ms, 0, 59) == 3);  // not calm: never
  CHECK(d.evaluate(20 * ms, 0, 60) == 3);
  CHECK(d.evaluate(20 * ms, 0, 60) == 3);
  CHECK(d.evaluate(20 * ms, 0, 60) == 2);  // one step
  for (int i = 0; i < 3; ++i) d.evaluate(20 * ms, 0, 60);
  CHECK(d.current() == 1);
  for (int i = 0; i < 9; ++i) d.evaluate(20 * ms, 0, 1000);
  CHECK(d.current() == 1);  // the floor
}

void a_noisy_link_does_not_flap() {
  // Alternating 40 ms and 70 ms of round trip (2 and 3 frames): the streak never builds.
  InputDelay d;
  d.start(40 * ms, 0);
  int changes = 0, last = d.current();
  for (int i = 0; i < 200; ++i) {
    int v = d.evaluate((i % 2 ? 70 : 40) * ms, 0, 1000);
    changes += v != last;
    last = v;
  }
  CHECK(changes == 0);
}

}  // namespace

int main() {
  the_rule();
  start_takes_the_target();
  raises_after_a_streak_and_jumps_to_the_target();
  a_dip_resets_the_streak();
  lowers_only_when_calm_and_by_one();
  a_noisy_link_does_not_flap();
  std::puts("input_delay_test: ok");
  return 0;
}
