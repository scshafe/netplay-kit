#include "npk/transport/connection.hpp"

#include <algorithm>

namespace npk::transport {
namespace {

constexpr std::size_t kMaxQueuedMessages = 4096;  // reliable messages not yet cut into fragments

}  // namespace

Connection::Connection(ConnectionId id, const ConnectionConfig& config, Micros now)
    : id_(id), cfg_(config), next_ping_(now), last_heard_(now), last_sent_(now) {}

// ---- packet numbers ---------------------------------------------------------

bool Connection::seen(std::uint16_t seq) const {
  if (!have_received_) return false;
  if (seq == recv_latest_) return true;
  if (seq_newer(seq, recv_latest_)) return false;
  const std::uint16_t offset = static_cast<std::uint16_t>(recv_latest_ - seq);
  if (offset > 32) return true;  // too old to tell: drop it
  return (recv_bits_ >> (offset - 1)) & 1u;
}

void Connection::mark_received(std::uint16_t seq) {
  if (!have_received_) {
    have_received_ = true;
    recv_latest_ = seq;
    recv_bits_ = 0;
  } else if (seq_newer(seq, recv_latest_)) {
    const std::uint16_t d = static_cast<std::uint16_t>(seq - recv_latest_);
    if (d > 32) {
      recv_bits_ = 0;
    } else {
      recv_bits_ = (d < 32 ? recv_bits_ << d : 0u) | (1u << (d - 1));
    }
    recv_latest_ = seq;
  } else {
    const std::uint16_t offset = static_cast<std::uint16_t>(recv_latest_ - seq);
    recv_bits_ |= 1u << (offset - 1);
  }
}

void Connection::process_acks(std::uint16_t ack, std::uint32_t bits, Micros now) {
  ack_packet(ack, now, true);
  for (std::uint16_t i = 0; i < 32; ++i) {
    if ((bits >> i) & 1u) ack_packet(static_cast<std::uint16_t>(ack - 1 - i), now, false);
  }
  while (!window_.empty() && window_.front().acked) {
    send_buffer_bytes_ -= window_.front().data.size();
    window_.pop_front();
  }
  fill_window();
}

void Connection::ack_packet(std::uint16_t seq, Micros now, bool direct) {
  SentPacket& rec = sent_[seq & 255];
  // Only a packet we really sent can be acked; anything else is ignored.
  if (!rec.valid || rec.seq != seq || rec.acked) return;
  rec.acked = true;
  if (!have_largest_acked_ || seq_newer(seq, largest_acked_)) {
    have_largest_acked_ = true;
    largest_acked_ = seq;
  }
  // The newest ack of a packet that asked for one (the peer acks those at once) is a
  // clean round-trip sample; acks of the rest can wait for the peer's next packet.
  if (direct && rec.eliciting) rtt_.sample(now - rec.sent);
  const std::uint16_t base = window_.empty() ? next_fragment_seq_ : window_.front().seq;
  for (const std::uint16_t fseq : rec.fragments) {
    const std::uint16_t idx = static_cast<std::uint16_t>(fseq - base);
    if (idx < window_.size() && window_[idx].seq == fseq) window_[idx].acked = true;
  }
}

// ---- sending ----------------------------------------------------------------

bool Connection::send_unreliable(std::span<const std::uint8_t> data) {
  if (data.size() > kMaxUnreliable) return false;
  if (unreliable_out_.size() >= cfg_.max_unreliable_queue) {
    unreliable_out_.pop_front();
    ++stats_.unreliable_dropped_send;
  }
  unreliable_out_.emplace_back(data.begin(), data.end());
  return true;
}

bool Connection::send_reliable(std::span<const std::uint8_t> data) {
  if (data.size() > cfg_.max_message_size) return false;
  if (send_buffer_bytes_ + data.size() > cfg_.max_send_buffer) return false;
  if (reliable_queue_.size() >= kMaxQueuedMessages) return false;
  send_buffer_bytes_ += data.size();
  reliable_queue_.emplace_back(data.begin(), data.end());
  fill_window();
  return true;
}

void Connection::fill_window() {
  while (window_.size() < kWindow && !reliable_queue_.empty()) {
    const auto& msg = reliable_queue_.front();
    const std::size_t remaining = msg.size() - queue_offset_;
    const std::size_t n = std::min(remaining, kMaxFragment);
    SendFragment f;
    f.seq = next_fragment_seq_++;
    f.flags = static_cast<std::uint8_t>((queue_offset_ == 0 ? kFragBegin : 0) | (n == remaining ? kFragEnd : 0));
    f.data.assign(msg.begin() + static_cast<std::ptrdiff_t>(queue_offset_),
                  msg.begin() + static_cast<std::ptrdiff_t>(queue_offset_ + n));
    window_.push_back(std::move(f));
    queue_offset_ += n;
    if (n == remaining) {
      reliable_queue_.pop_front();
      queue_offset_ = 0;
    }
  }
}

Connection::Due Connection::fragment_due(const SendFragment& f, Micros now) const {
  if (f.acked) return Due::No;
  if (f.sent_count == 0) return Due::Now;
  // Lost, by the look of it: packets sent three or more after its carrier got through.
  if (have_largest_acked_ && seq_newer(largest_acked_, f.packet_seq) &&
      static_cast<std::uint16_t>(largest_acked_ - f.packet_seq) >= 3) {
    return Due::Now;
  }
  const std::uint32_t shift = std::min<std::uint32_t>(f.sent_count - 1, 4);
  const Micros timeout = std::min(rtt_.rto() << shift, rtt_.max_rto());
  return now - f.last_sent >= timeout ? Due::Probe : Due::No;
}

Header Connection::next_header() {
  return Header{PacketType::Data, id_, send_seq_, recv_latest_, recv_bits_};
}

void Connection::record_sent(std::uint16_t seq, Micros now, bool eliciting,
                             std::vector<std::uint16_t>&& fragments) {
  SentPacket& r = sent_[seq & 255];
  r.valid = true;
  r.acked = false;
  r.eliciting = eliciting;
  r.seq = seq;
  r.sent = now;
  r.fragments = std::move(fragments);
}

void Connection::flush(Micros now, Link& link, const Address& to) {
  fill_window();
  bool first = true;
  for (int guard = 0; guard < 256; ++guard) {
    PacketBuilder b(next_header());
    std::vector<std::uint16_t> carried;
    bool full = false;
    bool control = false;
    if (first) {
      for (std::size_t k = 0; k < kMaxPongsPerFlush && !pending_pongs_.empty(); ++k) {
        const PendingPong pp = pending_pongs_.front();
        pending_pongs_.pop_front();
        // t3 is stamped now; the peer subtracts the time we held its ping.
        b.add_pong(pp.id, pp.t1, static_cast<std::uint64_t>(pp.received), static_cast<std::uint64_t>(now));
        control = true;
      }
      pending_pongs_.clear();  // the rest is stale by the next flush
      if (now >= next_ping_) {
        PingRecord& rec = pings_[next_ping_id_ % pings_.size()];
        rec = {true, next_ping_id_, now};
        b.add_ping(next_ping_id_, static_cast<std::uint64_t>(now));
        ++next_ping_id_;
        ++stats_.pings_sent;
        next_ping_ = now + (pings_started_++ < cfg_.warmup_pings ? cfg_.warmup_interval : cfg_.ping_interval);
        control = true;
      }
    }
    std::size_t unacked_before = 0;  // fragments ahead of this one still waiting for an ack
    for (SendFragment& f : window_) {
      if (f.acked) continue;
      const Due due = fragment_due(f, now);
      const std::size_t ahead = unacked_before++;
      if (due == Due::No) continue;
      // A timeout alone says little (the path may be dead): only the oldest few
      // fragments are resent for it. The ack that comes back lets the loss rule
      // resend the rest.
      if (due == Due::Probe && ahead >= kProbeFragments) continue;
      if (!b.add_reliable(f.seq, f.flags, f.data)) {
        full = true;
        break;
      }
      if (f.sent_count > 0) ++stats_.fragments_resent;
      ++f.sent_count;
      f.last_sent = now;
      f.packet_seq = send_seq_;
      carried.push_back(f.seq);
    }
    bool unreliable = false;
    while (!unreliable_out_.empty()) {
      if (!b.add_unreliable(unreliable_out_.front())) {
        full = true;
        break;
      }
      unreliable_out_.pop_front();
      unreliable = true;
    }
    const bool asks_for_ack = !carried.empty() || unreliable || control;
    const bool ack_only = first && ack_pending_;
    const bool keepalive = first && now - last_sent_ >= cfg_.keepalive_interval;
    if (!asks_for_ack && !ack_only && !keepalive) break;

    link.send(to, b.bytes());
    record_sent(send_seq_, now, asks_for_ack, std::move(carried));
    ++send_seq_;
    ++stats_.packets_sent;
    ack_pending_ = false;
    last_sent_ = now;
    first = false;
    if (!full) break;
  }
}

void Connection::send_close(Micros now, Link& link, const Address& to, DisconnectReason reason) {
  PacketBuilder b(next_header());
  b.add_close(static_cast<std::uint8_t>(reason));
  link.send(to, b.bytes());
  record_sent(send_seq_, now, false, {});
  ++send_seq_;
  ++stats_.packets_sent;
  last_sent_ = now;
}

// ---- receiving --------------------------------------------------------------

bool Connection::on_packet(const Packet& p, Micros now) {
  if (seen(p.header.seq)) {
    ++stats_.duplicates_dropped;
    return false;
  }
  // First pass: the whole body must parse before anything is acked or applied.
  {
    FrameReader r(p.body);
    Frame f;
    while (!r.done()) {
      if (r.next(f) != ParseError::None ||
          (f.kind == FrameKind::Reliable && f.data.size() > kMaxFragment)) {
        ++stats_.malformed_dropped;
        return false;
      }
    }
  }
  mark_received(p.header.seq);
  ++stats_.packets_received;
  last_heard_ = now;
  if (!p.body.empty()) ack_pending_ = true;
  process_acks(p.header.ack, p.header.ack_bits, now);

  FrameReader r(p.body);
  Frame f;
  while (!r.done() && r.next(f) == ParseError::None) {
    switch (f.kind) {
      case FrameKind::Unreliable:
        if (inbox_unreliable_ >= cfg_.max_inbox_unreliable) {
          ++stats_.unreliable_dropped_recv;
        } else {
          inbox_.push_back({Channel::Unreliable, {f.data.begin(), f.data.end()}});
          ++inbox_unreliable_;
        }
        break;
      case FrameKind::Reliable: accept_fragment(f); break;
      case FrameKind::Close:
        peer_closed_ = true;
        peer_close_reason_ = f.close_reason <= static_cast<std::uint8_t>(DisconnectReason::ProtocolError)
                                 ? static_cast<DisconnectReason>(f.close_reason)
                                 : DisconnectReason::Closed;
        break;
      case FrameKind::Ping:
        if (pending_pongs_.size() < kMaxPongsPerFlush) {
          pending_pongs_.push_back({f.ping_id, f.t1, now});
        } else {
          ++stats_.pings_unanswered;
        }
        break;
      case FrameKind::Pong: accept_pong(f, now); break;
    }
  }
  return true;
}

void Connection::accept_pong(const Frame& f, Micros now) {
  PingRecord& rec = pings_[f.ping_id % pings_.size()];
  const Micros t1 = static_cast<Micros>(f.t1), t2 = static_cast<Micros>(f.t2), t3 = static_cast<Micros>(f.t3);
  // Only a ping we sent, answered once, with our own timestamp echoed back.
  if (!rec.valid || rec.id != f.ping_id || rec.sent != t1 || t3 < t2) {
    ++stats_.pongs_rejected;
    return;
  }
  rec.valid = false;
  const Micros held = t3 - t2;
  const Micros rtt = (now - t1) - held;
  constexpr Micros kMaxRtt = 60'000'000;
  if (rtt < 0 || rtt > kMaxRtt) {
    ++stats_.pongs_rejected;
    return;
  }
  clock_.add(rtt, ((t2 - t1) + (t3 - now)) / 2);
  ++stats_.pongs_accepted;
}

void Connection::accept_fragment(const Frame& f) {
  if (seq_newer(recv_next_, f.reliable_seq)) return;  // already delivered
  const std::uint16_t offset = static_cast<std::uint16_t>(f.reliable_seq - recv_next_);
  if (offset >= kWindow) {  // an honest sender's window cannot reach this far
    fail();
    return;
  }
  RecvSlot& slot = slots_[f.reliable_seq % kWindow];
  if (slot.valid) return;  // a duplicate
  slot.valid = true;
  slot.seq = f.reliable_seq;
  slot.flags = f.flags;
  slot.data.assign(f.data.begin(), f.data.end());
  deliver_in_order();
}

void Connection::deliver_in_order() {
  for (;;) {
    RecvSlot& slot = slots_[recv_next_ % kWindow];
    if (!slot.valid || slot.seq != recv_next_) return;
    const bool begin = slot.flags & kFragBegin, end = slot.flags & kFragEnd;
    if (assembling_active_ == begin) {  // a begin inside a message, or a middle with no begin
      fail();
      return;
    }
    if (begin) {
      assembling_active_ = true;
      assembling_.clear();
    }
    if (assembling_.size() + slot.data.size() > cfg_.max_message_size) {
      fail();
      return;
    }
    assembling_.insert(assembling_.end(), slot.data.begin(), slot.data.end());
    slot.valid = false;
    slot.data.clear();
    ++recv_next_;
    if (end) {
      assembling_active_ = false;
      if (inbox_reliable_bytes_ + assembling_.size() > cfg_.max_inbox_reliable_bytes) {
        fail();
        return;
      }
      inbox_reliable_bytes_ += assembling_.size();
      inbox_.push_back({Channel::Reliable, std::move(assembling_)});
      assembling_.clear();
    }
  }
}

std::optional<Message> Connection::poll_message() {
  if (inbox_.empty()) return std::nullopt;
  Message m = std::move(inbox_.front());
  inbox_.pop_front();
  if (m.channel == Channel::Unreliable) {
    --inbox_unreliable_;
  } else {
    inbox_reliable_bytes_ -= m.data.size();
  }
  return m;
}

}  // namespace npk::transport
