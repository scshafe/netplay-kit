#include "npk/session/rollback.hpp"

#include <algorithm>
#include <limits>

namespace npk::session {

namespace {
constexpr std::size_t kMaxCheckpoints = 32;  // unread checkpoints kept; the oldest is dropped
}

Rollback::Rollback(Simulation& sim, const RollbackConfig& config)
    : sim_(sim),
      cfg_(config),
      players_(sim.player_count()),
      in_size_(sim.input_size()),
      frame_bytes_(players_ * in_size_) {
  cfg_.max_rollback = std::max<std::size_t>(cfg_.max_rollback, 1);
  cfg_.window = std::max(cfg_.window, cfg_.max_rollback + 9);
  slots_.resize(cfg_.window);
  prev_confirmed_.resize(frame_bytes_);
  for (std::size_t p = 0; p < players_; ++p)
    sim_.neutral_input(std::span(prev_confirmed_).subspan(p * in_size_, in_size_));
  store_state(0);
}

Rollback::Slot& Rollback::claim(Frame f) {
  Slot& s = slots_[index(f)];
  if (!s.claimed || s.tag != f) {
    s.tag = f;
    s.claimed = true;
    s.has_snapshot = s.has_used = s.has_canonical = false;
    s.has_prov.assign(players_, 0);
    s.provisional.assign(frame_bytes_, 0);
  }
  return s;
}

Rollback::Slot* Rollback::find(Frame f) {
  Slot& s = slots_[index(f)];
  return s.claimed && s.tag == f ? &s : nullptr;
}

const Rollback::Slot* Rollback::find(Frame f) const {
  const Slot& s = slots_[index(f)];
  return s.claimed && s.tag == f ? &s : nullptr;
}

void Rollback::mark_mismatch(Frame f) {
  if (f < frame_ && (!rollback_from_ || f < *rollback_from_)) rollback_from_ = f;
}

bool Rollback::set_provisional(PlayerIndex player, Frame f, std::span<const std::uint8_t> input) {
  if (player >= players_ || input.size() != in_size_ || !in_window(f) || !sim_.valid_input(input)) {
    ++stats_.rejected;
    return false;
  }
  Slot& s = claim(f);
  std::copy(input.begin(), input.end(), s.provisional.begin() + static_cast<std::ptrdiff_t>(player * in_size_));
  s.has_prov[player] = 1;
  if (f < frame_ && !s.has_canonical && s.has_used &&
      !std::equal(input.begin(), input.end(), s.used.begin() + static_cast<std::ptrdiff_t>(player * in_size_)))
    mark_mismatch(f);
  return true;
}

bool Rollback::add_canonical(Frame f, std::span<const std::uint8_t> inputs) {
  if (f < confirmed_) return true;
  if (!in_window(f) || inputs.size() != frame_bytes_) {
    ++stats_.rejected;
    return false;
  }
  for (std::size_t p = 0; p < players_; ++p) {
    if (!sim_.valid_input(inputs.subspan(p * in_size_, in_size_))) {
      ++stats_.rejected;
      return false;
    }
  }
  Slot& s = claim(f);
  if (s.has_canonical) return true;
  s.canonical.assign(inputs.begin(), inputs.end());
  s.has_canonical = true;
  if (f < frame_ && s.has_used && !std::equal(inputs.begin(), inputs.end(), s.used.begin())) mark_mismatch(f);
  return true;
}

bool Rollback::has_canonical(Frame f) const {
  if (f < confirmed_) return true;
  const Slot* s = find(f);
  return s && s->has_canonical;
}

Frame Rollback::next_canonical_needed() const {
  Frame f = confirmed_;
  while (f < window_end() && has_canonical(f)) ++f;
  return f;
}

void Rollback::resolve(Frame f, std::vector<std::uint8_t>& out) const {
  out.resize(frame_bytes_);
  const Slot* s = find(f);
  if (s && s->has_canonical) {
    out = s->canonical;
    return;
  }
  const Slot* prev = f > confirmed_ ? find(f - 1) : nullptr;
  for (std::size_t p = 0; p < players_; ++p) {
    auto dst = std::span(out).subspan(p * in_size_, in_size_);
    if (s && s->has_prov[p]) {
      std::copy_n(s->provisional.begin() + static_cast<std::ptrdiff_t>(p * in_size_), in_size_, dst.begin());
    } else {
      const auto& before = (prev && prev->has_used) ? prev->used : prev_confirmed_;
      sim_.predict_input(std::span<const std::uint8_t>(before).subspan(p * in_size_, in_size_), dst);
    }
  }
}

void Rollback::store_state(Frame f) {
  Slot& s = claim(f);
  sim_.snapshot(s.snapshot);
  s.has_snapshot = true;
  if (cfg_.hash_interval != 0 && f % cfg_.hash_interval == 0) s.hash = sim_.hash();
}

void Rollback::simulate(Frame f) {
  resolve(f, scratch_);
  Slot& s = claim(f);
  s.used = scratch_;
  s.has_used = true;
  sim_.step(f, scratch_);
  store_state(f + 1);
}

void Rollback::advance_confirmed() {
  while (confirmed_ < frame_) {
    Slot* s = find(confirmed_);
    if (!s || !s->has_canonical) break;
    prev_confirmed_ = s->canonical;
    s->claimed = false;
    ++confirmed_;
    if (cfg_.hash_interval != 0 && confirmed_ % cfg_.hash_interval == 0) {
      if (const Slot* c = find(confirmed_); c && c->has_snapshot) {
        if (checkpoints_.size() >= kMaxCheckpoints) checkpoints_.pop_front();
        checkpoints_.push_back({confirmed_, c->hash});
      }
    }
  }
}

void Rollback::correct() {
  if (!ok_) return;
  if (rollback_from_) {
    const Frame from = *rollback_from_;
    rollback_from_.reset();
    const Slot* base = find(from);
    if (from >= confirmed_ && from < frame_ && base && base->has_snapshot) {
      if (!sim_.restore(base->snapshot)) {
        ok_ = false;
        return;
      }
      const Frame end = frame_;
      for (Frame f = from; f < end; ++f) simulate(f);
      ++stats_.rollbacks;
      stats_.resimulated_frames += end - from;
      stats_.max_depth = std::max<std::uint64_t>(stats_.max_depth, end - from);
      since_rollback_ = 0;
    }
  }
  advance_confirmed();
}

bool Rollback::advance() {
  correct();
  if (!can_advance()) return false;
  simulate(frame_);
  ++frame_;
  if (since_rollback_ != std::numeric_limits<std::uint32_t>::max()) ++since_rollback_;
  advance_confirmed();
  return true;
}

bool Rollback::resync(Frame f, std::span<const std::uint8_t> snapshot) {
  if (!ok_ || !sim_.restore(snapshot)) {
    ok_ = false;
    return false;
  }
  ++stats_.resyncs;
  // What stays: canonical and provisional inputs for frames the new window covers.
  std::vector<Slot> old = std::move(slots_);
  slots_.assign(cfg_.window, Slot{});
  frame_ = confirmed_ = f;
  rollback_from_.reset();
  prev_confirmed_.assign(frame_bytes_, 0);
  for (std::size_t p = 0; p < players_; ++p)
    sim_.neutral_input(std::span(prev_confirmed_).subspan(p * in_size_, in_size_));
  for (Slot& o : old) {
    if (!o.claimed || o.tag < f || o.tag - f >= cfg_.window) continue;
    Slot& n = claim(o.tag);
    n.canonical = std::move(o.canonical);
    n.has_canonical = o.has_canonical;
    n.provisional = std::move(o.provisional);
    n.has_prov = std::move(o.has_prov);
  }
  store_state(f);
  checkpoints_.clear();
  since_rollback_ = 0;
  advance_confirmed();
  return true;
}

std::optional<Checkpoint> Rollback::take_checkpoint() {
  if (checkpoints_.empty()) return std::nullopt;
  const Checkpoint c = checkpoints_.front();
  checkpoints_.pop_front();
  return c;
}

}  // namespace npk::session
