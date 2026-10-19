#pragma once

// One established connection, as seen from one end: packet sequence numbers and
// acks, an unreliable channel and a reliable ordered channel. A Server holds one
// per client and a Client holds one; neither is used on its own.
//
// Packets. Every Data packet has a sequence number (starting at 1). Its header
// acks the newest packet received and, in a 32-bit field, the 32 before it. A
// packet seen before, or too old to tell, is dropped whole, so a duplicate on
// the wire is never delivered twice. A packet whose frames do not parse is
// dropped before it is acked, so an ack always means "every frame in it was
// taken".
//
// Unreliable channel. Messages that fit one packet, sent once, delivered at most
// once, in any order. The send queue is bounded; the oldest message is dropped
// for a new one (a stale input is worth less than the newest).
//
// Reliable ordered channel. Messages up to max_message_size, cut into fragments,
// each with a sequence number; delivered once, in order. The sender keeps at most
// kWindow fragments in flight and resends one when the packet that carried it
// goes unacked for a retransmission timeout (doubling each time, up to 16x), or at
// once when three packets sent after it have been acked. The receiver
// holds kWindow slots, which the sender's window guarantees are enough, so a
// fragment outside them is a protocol error. All buffers are bounded; a peer that
// breaks a bound fails the connection (failed()).

#include <array>
#include <deque>
#include <optional>
#include <vector>

#include "npk/transport/link.hpp"
#include "npk/transport/rtt.hpp"
#include "npk/transport/time.hpp"
#include "npk/transport/types.hpp"
#include "npk/transport/wire.hpp"

namespace npk::transport {

enum class Channel : std::uint8_t { Unreliable, Reliable };

struct Message {
  Channel channel;
  std::vector<std::uint8_t> data;
};

struct ConnectionConfig {
  std::size_t max_message_size = 64 * 1024;       // reliable
  std::size_t max_send_buffer = 256 * 1024;       // reliable bytes queued or unacked
  std::size_t max_unreliable_queue = 64;          // messages waiting to be sent
  std::size_t max_inbox_unreliable = 256;         // messages waiting to be taken
  std::size_t max_inbox_reliable_bytes = 1 << 20;
  Micros keepalive_interval = 1'000'000;          // an empty packet when nothing else is sent
};

struct ConnectionStats {
  std::uint64_t packets_sent = 0;
  std::uint64_t packets_received = 0;     // accepted
  std::uint64_t duplicates_dropped = 0;   // seen before, or too old
  std::uint64_t malformed_dropped = 0;
  std::uint64_t fragments_resent = 0;
  std::uint64_t unreliable_dropped_send = 0;
  std::uint64_t unreliable_dropped_recv = 0;
};

class Connection {
 public:
  static constexpr std::size_t kWindow = 128;  // reliable fragments in flight / buffered
  // The largest unreliable message: a packet minus header and frame header.
  static constexpr std::size_t kMaxUnreliable = kMaxDatagram - kHeaderSize - 3;

  Connection(ConnectionId id, const ConnectionConfig& config = {}, Micros now = 0);

  // False when the message is too big or the buffer is full (reliable: try later).
  bool send_unreliable(std::span<const std::uint8_t> data);
  bool send_reliable(std::span<const std::uint8_t> data);

  std::optional<Message> poll_message();

  // Takes one packet that the owner has matched to this connection (right id,
  // right address). Returns whether it was accepted.
  bool on_packet(const Packet& p, Micros now);

  // Sends whatever is due: acks, fragments, queued messages, a keepalive.
  void flush(Micros now, Link& link, const Address& to);

  // Sends a packet that tells the peer the connection is over.
  void send_close(Micros now, Link& link, const Address& to, DisconnectReason reason);

  bool peer_closed() const { return peer_closed_; }
  DisconnectReason peer_close_reason() const { return peer_close_reason_; }
  bool failed() const { return failed_; }
  Micros last_heard() const { return last_heard_; }
  const RttEstimator& rtt() const { return rtt_; }
  const ConnectionStats& stats() const { return stats_; }
  ConnectionId id() const { return id_; }

 private:
  struct SendFragment {
    std::uint16_t seq;
    std::uint8_t flags;
    std::vector<std::uint8_t> data;
    bool acked = false;
    std::uint32_t sent_count = 0;
    Micros last_sent = 0;
    std::uint16_t packet_seq = 0;  // the newest packet that carried it
  };
  struct SentPacket {
    bool valid = false;
    bool acked = false;
    bool eliciting = false;  // carried frames, so the peer acks it promptly
    std::uint16_t seq = 0;
    Micros sent = 0;
    std::vector<std::uint16_t> fragments;
  };
  struct RecvSlot {
    bool valid = false;
    std::uint16_t seq = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> data;
  };

  bool seen(std::uint16_t seq) const;
  void mark_received(std::uint16_t seq);
  void process_acks(std::uint16_t ack, std::uint32_t bits, Micros now);
  void ack_packet(std::uint16_t seq, Micros now, bool direct);
  void accept_fragment(const Frame& f);
  void deliver_in_order();
  void fill_window();
  bool fragment_due(const SendFragment& f, Micros now) const;
  Header next_header();
  void record_sent(std::uint16_t seq, Micros now, bool eliciting, std::vector<std::uint16_t>&& fragments);
  void fail() { failed_ = true; }

  ConnectionId id_;
  ConnectionConfig cfg_;
  RttEstimator rtt_;
  ConnectionStats stats_;

  // Packet numbers.
  std::uint16_t send_seq_ = 1;
  bool have_received_ = false;
  std::uint16_t recv_latest_ = 0;
  std::uint32_t recv_bits_ = 0;
  bool ack_pending_ = false;
  Micros last_heard_ = 0;
  Micros last_sent_ = 0;
  std::array<SentPacket, 256> sent_;
  bool have_largest_acked_ = false;
  std::uint16_t largest_acked_ = 0;

  // Sending.
  std::deque<std::vector<std::uint8_t>> unreliable_out_;
  std::deque<std::vector<std::uint8_t>> reliable_queue_;
  std::size_t queue_offset_ = 0;       // bytes of reliable_queue_.front() already cut into fragments
  std::size_t send_buffer_bytes_ = 0;  // queued + unacked
  std::deque<SendFragment> window_;
  std::uint16_t next_fragment_seq_ = 0;

  // Receiving.
  std::array<RecvSlot, kWindow> slots_;
  std::uint16_t recv_next_ = 0;
  std::vector<std::uint8_t> assembling_;
  bool assembling_active_ = false;
  std::deque<Message> inbox_;
  std::size_t inbox_unreliable_ = 0;
  std::size_t inbox_reliable_bytes_ = 0;

  bool peer_closed_ = false;
  DisconnectReason peer_close_reason_ = DisconnectReason::Closed;
  bool failed_ = false;
};

}  // namespace npk::transport
