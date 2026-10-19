// The lossy-link simulator: each condition does what it says, and a seed repeats.
#include <algorithm>
#include <vector>

#include "check.hpp"
#include "npk/transport/sim_link.hpp"

using namespace npk::transport;

namespace {

const Address kA = Address::v4(10, 0, 0, 1, 1000);
const Address kB = Address::v4(10, 0, 0, 2, 2000);
constexpr std::size_t kBuf = 2048;

struct Arrival {
  Micros at;
  std::uint32_t id;
};

std::vector<std::uint8_t> payload(std::uint32_t id, std::size_t size = 4) {
  std::vector<std::uint8_t> p(size, 0);
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(id >> (8 * i));
  return p;
}

std::uint32_t id_of(const std::vector<std::uint8_t>& p) {
  std::uint32_t id = 0;
  for (int i = 0; i < 4; ++i) id |= static_cast<std::uint32_t>(p[i]) << (8 * i);
  return id;
}

// Sends n numbered datagrams A -> B, one per millisecond, runs until `end` and
// returns what B received and when.
std::vector<Arrival> run(std::uint64_t seed, const LinkConditions& c, int n, Micros end = 5'000'000) {
  SimNetwork net(seed);
  net.set_default_conditions(c);
  auto a = net.make_link(kA);
  auto b = net.make_link(kB);
  std::vector<Arrival> got;
  std::vector<std::uint8_t> buf(kBuf);
  Address from;
  for (Micros t = 0; t < end; t += 1000) {
    if (t / 1000 < n) a->send(kB, payload(static_cast<std::uint32_t>(t / 1000)));
    net.advance(1000);
    while (auto len = b->receive(from, buf)) {
      CHECK(from == kA);
      got.push_back({net.now(), id_of({buf.begin(), buf.begin() + static_cast<long>(*len)})});
    }
  }
  return got;
}

void perfect_link_delivers_everything_in_order_after_the_delay() {
  LinkConditions c;
  c.delay = 20'000;
  const auto got = run(1, c, 100);
  CHECK(got.size() == 100);
  for (std::size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].id == i);
    CHECK(got[i].at >= static_cast<Micros>(i) * 1000 + 20'000);
    CHECK(got[i].at <= static_cast<Micros>(i) * 1000 + 21'000);  // within one tick
  }
}

void drop_rate_is_about_right() {
  LinkConditions c;
  c.drop = 0.3;
  const auto got = run(2, c, 4000);
  CHECK(got.size() > 2600 && got.size() < 3000);  // 70% of 4000, +-7%
  LinkConditions all;
  all.drop = 1;
  CHECK(run(2, all, 100).empty());
}

void jitter_stays_within_bounds() {
  LinkConditions c;
  c.delay = 10'000;
  c.jitter = 30'000;
  const auto got = run(3, c, 500);
  CHECK(got.size() == 500);
  bool reordered = false;
  for (std::size_t i = 0; i < got.size(); ++i) {
    CHECK(got[i].at >= static_cast<Micros>(got[i].id) * 1000 + 10'000);
    CHECK(got[i].at <= static_cast<Micros>(got[i].id) * 1000 + 41'000);
    if (i > 0 && got[i].id < got[i - 1].id) reordered = true;
  }
  CHECK(reordered);  // 30 ms of jitter on 1 ms spacing reorders
}

void reorder_holds_some_back_and_loses_none() {
  LinkConditions c;
  c.delay = 5'000;
  c.reorder = 0.2;
  c.reorder_delay = 50'000;
  const auto got = run(4, c, 1000);
  CHECK(got.size() == 1000);
  int inversions = 0;
  for (std::size_t i = 1; i < got.size(); ++i) inversions += got[i].id < got[i - 1].id;
  CHECK(inversions > 50);
  std::vector<std::uint32_t> ids;
  for (auto& g : got) ids.push_back(g.id);
  std::sort(ids.begin(), ids.end());
  for (std::uint32_t i = 0; i < ids.size(); ++i) CHECK(ids[i] == i);
}

void duplicate_delivers_extra_copies() {
  LinkConditions c;
  c.delay = 5'000;
  c.duplicate = 0.25;
  const auto got = run(5, c, 2000);
  CHECK(got.size() > 2400 && got.size() < 2600);
  LinkConditions all;
  all.duplicate = 1;
  CHECK(run(5, all, 10).size() == 20);
}

void same_seed_same_run_different_seed_differs() {
  LinkConditions c;
  c.drop = 0.2;
  c.delay = 10'000;
  c.jitter = 20'000;
  c.reorder = 0.1;
  c.reorder_delay = 30'000;
  c.duplicate = 0.1;
  const auto x = run(77, c, 500), y = run(77, c, 500), z = run(78, c, 500);
  CHECK(x.size() == y.size());
  for (std::size_t i = 0; i < x.size(); ++i) CHECK(x[i].at == y[i].at && x[i].id == y[i].id);
  bool differs = x.size() != z.size();
  for (std::size_t i = 0; !differs && i < x.size(); ++i) differs = x[i].at != z[i].at || x[i].id != z[i].id;
  CHECK(differs);
}

void per_path_conditions_and_oversize_and_limits() {
  SimNetwork net(9);
  auto a = net.make_link(kA);
  auto b = net.make_link(kB);
  LinkConditions slow;
  slow.delay = 100'000;
  net.set_conditions(kA, kB, slow);  // only A -> B is slow
  a->send(kB, payload(1));
  b->send(kA, payload(2));
  net.advance(1000);
  std::vector<std::uint8_t> buf(kBuf);
  Address from;
  CHECK(!b->receive(from, buf));
  CHECK(a->receive(from, buf) && from == kB);
  net.advance(100'000);
  CHECK(b->receive(from, buf).value_or(0) == 4);

  // A datagram over the path MTU vanishes; one at the limit passes.
  a->send(kB, payload(3, 1233));
  a->send(kB, payload(4, 1232));
  net.advance(200'000);
  CHECK(b->receive(from, buf).value_or(0) == 1232);
  CHECK(!b->receive(from, buf));

  // A full inbox drops, like a full socket buffer.
  net.set_inbox_limit(3);
  net.set_conditions(kA, kB, {});
  for (std::uint32_t i = 0; i < 10; ++i) a->send(kB, payload(i));
  net.advance(0);
  int n = 0;
  while (b->receive(from, buf)) ++n;
  CHECK(n == 3);

  // A short buffer cuts the datagram; a spoofed source arrives as given.
  const Address spoof = Address::v4(203, 0, 113, 9, 4444);
  net.inject(spoof, kB, payload(5, 100));
  net.advance(0);
  CHECK(b->receive(from, std::span<std::uint8_t>(buf.data(), 10)).value() == 10);
  CHECK(from == spoof);
  CHECK(b->stats().received_datagrams >= 1 && a->stats().sent_datagrams == 13);
}

}  // namespace

int main() {
  perfect_link_delivers_everything_in_order_after_the_delay();
  drop_rate_is_about_right();
  jitter_stays_within_bounds();
  reorder_holds_some_back_and_loses_none();
  duplicate_delivers_extra_copies();
  same_seed_same_run_different_seed_differs();
  per_path_conditions_and_oversize_and_limits();
  return 0;
}
