#include "npk/session/server_session.hpp"

#include <algorithm>

namespace npk::session {

using transport::Micros;

ServerSession::ServerSession(transport::Link& link, Simulation& sim, const ServerSessionConfig& config)
    : cfg_(config),
      sim_(sim),
      server_(link, config.transport),
      players_(sim.player_count()),
      in_size_(sim.input_size()),
      frame_bytes_(players_ * in_size_) {
  cfg_.history = std::max<Frame>(cfg_.history, 128);
  if (!msg::shape_supported(players_, in_size_)) {
    state_ = State::Finished;  // this game does not fit the protocol; nobody is seated
    return;
  }
  seats_.resize(players_);
  for (Seat& s : seats_) {
    s.pending.assign(kPending * in_size_, 0);
    s.pending_tag.assign(kPending, 0);
    s.pending_has.assign(kPending, 0);
  }
  previous_.resize(frame_bytes_);
  for (std::size_t p = 0; p < players_; ++p)
    sim_.neutral_input(std::span(previous_).subspan(p * in_size_, in_size_));
  history_.assign(static_cast<std::size_t>(cfg_.history) * frame_bytes_, 0);
}

std::size_t ServerSession::seated() const {
  std::size_t n = 0;
  for (const Seat& s : seats_) n += s.connected;
  return n;
}

std::optional<transport::RttEstimator> ServerSession::rtt(std::size_t seat) const {
  return seat < seats_.size() && seats_[seat].connected ? server_.rtt(seats_[seat].id) : std::nullopt;
}

Micros ServerSession::frame_start(Frame f) const {
  return start_time_ + static_cast<Micros>(f) * 1'000'000 / cfg_.tick_rate_hz;
}

std::int64_t ServerSession::wall_frame(Micros now) const {
  return now < start_time_ ? -1 : (now - start_time_) * cfg_.tick_rate_hz / 1'000'000;
}

void ServerSession::update(Micros now) {
  if (state_ == State::Finished && seats_.empty()) return;
  server_.update(now);
  while (auto e = server_.poll_event()) {
    if (e->type == transport::ServerEvent::Type::Connected) on_connected(*e, now);
    else on_disconnected(*e);
  }
  while (auto m = server_.poll_message()) on_message(*m);
  if (state_ == State::Waiting && seated() == players_) start(now);
  if (state_ == State::Running) {
    finalise(now);
    send_frames(now);
    if (seated() == 0) state_ = State::Finished;
  }
}

void ServerSession::on_connected(const transport::ServerEvent& e, Micros) {
  if (state_ == State::Waiting) {
    for (Seat& s : seats_) {
      if (s.connected) continue;
      s.id = e.id;
      s.connected = true;
      s.ack = s.sent_to = 0;
      s.last_send = 0;
      s.bad = 0;
      std::fill(s.pending_has.begin(), s.pending_has.end(), 0);
      return;
    }
  }
  server_.disconnect(e.id, transport::DisconnectReason::Closed);  // no seat
}

void ServerSession::on_disconnected(const transport::ServerEvent& e) {
  for (Seat& s : seats_) {
    if (s.connected && s.id == e.id) s.connected = false;
  }
}

void ServerSession::drop(Seat& s, transport::DisconnectReason why) {
  ++stats_.clients_dropped;
  server_.disconnect(s.id, why);
  s.connected = false;
}

void ServerSession::count_bad(Seat& s) {
  if (++s.bad > cfg_.malformed_limit && s.connected) drop(s, transport::DisconnectReason::ProtocolError);
}

void ServerSession::start(Micros now) {
  state_ = State::Running;
  start_time_ = now + cfg_.start_delay;
  for (std::size_t i = 0; i < seats_.size(); ++i) {
    msg::Start st{.player = static_cast<std::uint8_t>(i),
                  .players = static_cast<std::uint8_t>(players_),
                  .input_size = static_cast<std::uint8_t>(in_size_),
                  .tick_rate = static_cast<std::uint16_t>(cfg_.tick_rate_hz),
                  .start_time = static_cast<std::uint64_t>(start_time_)};
    msg::encode_start(st, out_);
    server_.send_reliable(seats_[i].id, out_);
  }
}

bool ServerSession::has_input(const Seat& s, Frame f) const {
  const std::size_t slot = f % kPending;
  return s.pending_has[slot] && s.pending_tag[slot] == f;
}

void ServerSession::on_message(const transport::ServerMessage& m) {
  Seat* seat = nullptr;
  for (Seat& s : seats_) {
    if (s.connected && s.id == m.id) seat = &s;
  }
  if (!seat || state_ != State::Running) return;
  if (m.message.channel != transport::Channel::Unreliable || msg::type_of(m.message.data) != msg::Type::Inputs) {
    ++stats_.messages_malformed;
    count_bad(*seat);
    return;
  }
  msg::Inputs in;
  if (!msg::decode_inputs(m.message.data, in_size_, in)) {
    ++stats_.messages_malformed;
    count_bad(*seat);
    return;
  }
  seat->ack = std::min(in.canonical_ack, next_final_);
  for (std::size_t i = 0; i < in.count; ++i) {
    const Frame f = in.first + static_cast<Frame>(i);
    const auto input = in.data.subspan(i * in_size_, in_size_);
    if (f < next_final_) { ++stats_.inputs_stale; continue; }
    if (f - next_final_ >= kPending) { ++stats_.inputs_far; continue; }
    if (!sim_.valid_input(input)) {
      ++stats_.inputs_invalid;
      count_bad(*seat);
      if (!seat->connected) return;
      continue;
    }
    const std::size_t slot = f % kPending;
    if (has_input(*seat, f)) continue;  // the first wins
    std::copy(input.begin(), input.end(), seat->pending.begin() + static_cast<std::ptrdiff_t>(slot * in_size_));
    seat->pending_tag[slot] = f;
    seat->pending_has[slot] = 1;
    ++stats_.inputs_accepted;
  }
}

void ServerSession::finalise(Micros now) {
  std::vector<std::uint8_t> frame(frame_bytes_);
  for (;;) {
    const Frame f = next_final_;
    if (static_cast<std::int64_t>(f) > wall_frame(now) + cfg_.max_lead_frames) return;
    bool all = true;
    for (const Seat& s : seats_) all = all && (!s.connected || has_input(s, f));
    if (!all && now < frame_start(f + cfg_.grace_frames)) return;
    for (std::size_t p = 0; p < players_; ++p) {
      Seat& s = seats_[p];
      auto dst = std::span(frame).subspan(p * in_size_, in_size_);
      if (!s.connected) {
        sim_.neutral_input(dst);
      } else if (has_input(s, f)) {
        const std::size_t slot = f % kPending;
        std::copy_n(s.pending.begin() + static_cast<std::ptrdiff_t>(slot * in_size_), in_size_, dst.begin());
      } else {
        ++stats_.frames_missing;
        std::copy_n(previous_.begin() + static_cast<std::ptrdiff_t>(p * in_size_), in_size_, dst.begin());
      }
    }
    previous_ = frame;
    std::copy(frame.begin(), frame.end(),
              history_.begin() + static_cast<std::ptrdiff_t>((f % cfg_.history) * frame_bytes_));
    sim_.step(f, frame);
    ++next_final_;
    if (cfg_.on_canonical) cfg_.on_canonical(f, frame);
  }
}

void ServerSession::send_frames(Micros now) {
  const Micros frame_us = 1'000'000 / cfg_.tick_rate_hz;
  const std::size_t per_msg = std::min<std::size_t>(msg::kMaxFramesBytes / frame_bytes_, 255);
  for (Seat& s : seats_) {
    if (!s.connected || s.ack >= next_final_) continue;
    if (s.sent_to == next_final_ && now - s.last_send < frame_us) continue;
    Frame first = s.ack;
    if (next_final_ > cfg_.history && first < next_final_ - cfg_.history) first = next_final_ - cfg_.history;
    const std::size_t count = std::min<std::size_t>(next_final_ - first, per_msg);
    std::vector<std::uint8_t> data;
    data.reserve(count * frame_bytes_);
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t at = ((first + i) % cfg_.history) * frame_bytes_;
      data.insert(data.end(), history_.begin() + static_cast<std::ptrdiff_t>(at),
                  history_.begin() + static_cast<std::ptrdiff_t>(at + frame_bytes_));
    }
    msg::encode_frames(first, count, data, out_);
    server_.send_unreliable(s.id, out_);
    s.sent_to = next_final_;
    s.last_send = now;
  }
}

}  // namespace npk::session
