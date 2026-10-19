#include "npk/session/client_session.hpp"

#include <algorithm>

namespace npk::session {

using transport::Micros;

ClientSession::ClientSession(transport::Link& link, const transport::Address& server, Simulation& sim,
                             InputSource input, const ClientSessionConfig& config)
    : cfg_(config),
      sim_(sim),
      client_(link, server, config.transport),
      rb_(sim, config.rollback),
      source_(std::move(input)),
      in_size_(sim.input_size()) {
  cur_input_.resize(in_size_);
}

void ClientSession::connect(Micros now) {
  if (!msg::shape_supported(sim_.player_count(), in_size_)) {
    end_reason_ = transport::DisconnectReason::ProtocolError;
    state_ = State::Ended;
    return;
  }
  state_ = State::Connecting;
  client_.connect(now);
}

void ClientSession::disconnect() {
  client_.disconnect();
  if (state_ != State::Ended) state_ = State::Ended;
}

void ClientSession::end(transport::DisconnectReason why) {
  if (state_ == State::Ended) return;
  state_ = State::Ended;
  end_reason_ = why;
  if (client_.state() == transport::Client::State::Connected) {
    client_.disconnect(why);
  }
}

void ClientSession::on_start(const msg::Start& s) {
  if (state_ != State::Waiting) return;  // a second Start is ignored
  if (s.players != sim_.player_count() || s.input_size != in_size_ || s.player >= s.players ||
      s.tick_rate < 10 || s.tick_rate > 240 || s.hash_interval != cfg_.rollback.hash_interval) {
    end(transport::DisconnectReason::ProtocolError);
    return;
  }
  player_ = s.player;
  tick_rate_ = s.tick_rate;
  start_time_ = static_cast<Micros>(s.start_time);
  InputDelayConfig dc = cfg_.delay;
  dc.tick_rate_hz = tick_rate_;
  delay_.emplace(dc);
  if (auto c = client_.clock()) delay_->start(c->rtt(), c->jitter());
  next_delay_eval_ = 0;
  // Frames before the delay have no input of their own: they are neutral.
  std::vector<std::uint8_t> neutral(in_size_);
  sim_.neutral_input(neutral);
  for (int f = 0; f < delay_->current(); ++f) {
    if (rb_.set_provisional(player_, static_cast<Frame>(f), neutral)) {
      local_.emplace_back(static_cast<Frame>(f), neutral);
      last_label_ = f;
    }
  }
  state_ = State::Running;
}

void ClientSession::on_frames(const msg::Frames& f) {
  const std::size_t frame_bytes = sim_.player_count() * in_size_;
  for (std::size_t i = 0; i < f.count; ++i) {
    const Frame frame = f.first + static_cast<Frame>(i);
    if (!rb_.add_canonical(frame, f.data.subspan(i * frame_bytes, frame_bytes))) {
      // Beyond the window: the server sends it again once there is room. An
      // invalid input from the server is a protocol error, though.
      if (frame >= rb_.confirmed() && frame < rb_.window_end()) end(transport::DisconnectReason::ProtocolError);
      return;
    }
  }
}

void ClientSession::on_resync(const transport::Message& m) {
  msg::Resync r;
  if (m.channel != transport::Channel::Reliable || state_ != State::Running ||
      !msg::decode_resync(m.data, cfg_.transport.connection.max_message_size, r)) {
    ++stats_.messages_malformed;
    end(transport::DisconnectReason::ProtocolError);
    return;
  }
  // Not from the past (further back than the window) or the far future.
  const std::int64_t window = static_cast<std::int64_t>(rb_.window_end() - rb_.confirmed());
  if (static_cast<std::int64_t>(r.frame) + window < rb_.confirmed() ||
      static_cast<std::int64_t>(r.frame) > wall_hint_ + window || !rb_.resync(r.frame, r.snapshot)) {
    end(transport::DisconnectReason::ProtocolError);
    return;
  }
  ++stats_.resyncs;
}

void ClientSession::on_message(const transport::Message& m) {
  const auto type = msg::type_of(m.data);
  if (type == msg::Type::Start && m.channel == transport::Channel::Reliable) {
    msg::Start s;
    if (msg::decode_start(m.data, s)) on_start(s);
    else { ++stats_.messages_malformed; end(transport::DisconnectReason::ProtocolError); }
  } else if (type == msg::Type::Frames && m.channel == transport::Channel::Unreliable) {
    msg::Frames f;
    if (state_ != State::Running) return;
    if (msg::decode_frames(m.data, sim_.player_count() * in_size_, f)) on_frames(f);
    else ++stats_.messages_malformed;
  } else if (type == msg::Type::Resync) {
    on_resync(m);
  } else {
    ++stats_.messages_malformed;
  }
}

void ClientSession::label_inputs(Frame wall) {
  const int d = delay_->current();
  const std::int64_t target = static_cast<std::int64_t>(wall) + d;
  source_(wall, cur_input_);
  if (!sim_.valid_input(cur_input_)) sim_.neutral_input(cur_input_);
  // After a long stall or a resync the confirmed frames are already past the last label.
  if (last_label_ + 1 < static_cast<std::int64_t>(rb_.confirmed())) last_label_ = static_cast<std::int64_t>(rb_.confirmed()) - 1;
  // Raising the delay leaves frames without an input: they repeat the last one.
  // Lowering it makes a frame come up twice: the first label stands.
  while (last_label_ < target) {
    const Frame f = static_cast<Frame>(last_label_ + 1);
    const std::vector<std::uint8_t>& value = (last_label_ + 1 == target || local_.empty()) ? cur_input_ : local_.back().second;
    if (!rb_.set_provisional(player_, f, value)) break;
    local_.emplace_back(f, value);
    last_label_ = f;
  }
  while (!local_.empty() && local_.front().first < rb_.confirmed()) local_.pop_front();
}

void ClientSession::send_inputs() {
  if (local_.empty()) return;
  const std::size_t fit = (transport::Connection::kMaxUnreliable - 10) / in_size_;
  const std::size_t count = std::min({local_.size(), msg::kMaxInputFrames, fit});
  std::vector<std::uint8_t> data;
  data.reserve(count * in_size_);
  const std::size_t skip = local_.size() - count;
  for (std::size_t i = skip; i < local_.size(); ++i) data.insert(data.end(), local_[i].second.begin(), local_[i].second.end());
  msg::encode_inputs(local_[skip].first, rb_.next_canonical_needed(), count, data, out_);
  client_.send_unreliable(out_);
}

void ClientSession::update(Micros now) {
  if (state_ == State::Idle || state_ == State::Ended) return;
  client_.update(now);
  // Where "now" is on the server's frame clock, for checking what the server sends.
  wall_hint_ = -1;
  if (state_ == State::Running) {
    if (const auto c = client_.clock(); c && c->to_peer(now) >= start_time_)
      wall_hint_ = (c->to_peer(now) - start_time_) * tick_rate_ / 1'000'000;
  }
  while (auto m = client_.poll_message()) on_message(*m);
  switch (client_.state()) {
    case transport::Client::State::Connected:
      if (state_ == State::Connecting) state_ = State::Waiting;
      break;
    case transport::Client::State::Failed:
    case transport::Client::State::Closed:
      end(client_.state() == transport::Client::State::Failed ? transport::DisconnectReason::Timeout
                                                                : client_.close_reason());
      return;
    default:
      break;
  }
  if (state_ != State::Running) return;
  const auto clock = client_.clock();
  if (!clock) return;
  if (!rb_.ok()) { end(transport::DisconnectReason::ProtocolError); return; }

  const Micros server_now = clock->to_peer(now);
  std::int64_t wall = server_now < start_time_ ? -1 : (server_now - start_time_) * tick_rate_ / 1'000'000;
  if (wall > last_wall_ && wall >= 0) {
    // Safe point: between frames, not inside a rollback. The delay is re-evaluated here.
    if (now >= next_delay_eval_) {
      const int before = delay_->current();
      if (delay_->evaluate(clock->rtt(), clock->jitter(), rb_.frames_since_rollback()) != before) ++stats_.delay_changes;
      next_delay_eval_ = now + cfg_.delay_eval_interval;
    }
    label_inputs(static_cast<Frame>(wall));
    last_wall_ = wall;
    send_inputs();
  }
  rb_.correct();
  std::size_t steps = 0;
  while (wall >= 0 && static_cast<std::int64_t>(rb_.frame()) <= wall && steps < cfg_.max_steps_per_update) {
    if (!rb_.advance()) break;
    ++steps;
  }
  stats_.frames_simulated += steps;
  if (wall >= 0 && static_cast<std::int64_t>(rb_.frame()) <= wall) ++stats_.stalls;
  while (auto c = rb_.take_checkpoint()) {
    if (checkpoint_log_.size() >= cfg_.checkpoint_log) checkpoint_log_.pop_front();
    checkpoint_log_.push_back(*c);
    msg::encode_hash({.frame = c->frame, .hash = c->hash}, out_);
    client_.send_reliable(out_);
  }
  if (!rb_.ok()) end(transport::DisconnectReason::ProtocolError);
}

}  // namespace npk::session
