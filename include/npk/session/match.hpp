#pragma once

// One match on the server: the authoritative host (docs/design.md, "Session
// layer"). It does not own a network connection: whoever hosts it (a
// ServerSession for one match, a GameServer for many) seats connections in it,
// passes it their messages, and gives it a MatchIo to send through. When every
// seat is taken the match starts: frame 0 begins at a time it announces, and
// frame F at start + F / tick rate.
//
// It collects each player's input for each frame and *finalises* a frame when
// every seated player's input has arrived or when the frame's start plus a
// grace period has passed, whichever is first (and never more than a few
// frames ahead of the clock). A missing input becomes the player's previous
// canonical one; an input that arrives after its frame was finalised is dropped.
// A player who has left plays neutral input. The canonical inputs go to every
// client, redundantly until acknowledged, and step the server's own simulation,
// which is the authority on what happened.

#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "npk/session/messages.hpp"
#include "npk/session/simulation.hpp"
#include "npk/transport/connection.hpp"
#include "npk/transport/types.hpp"

namespace npk::session {

struct MatchConfig {
  int tick_rate_hz = 60;
  transport::Micros start_delay = 500'000;  // from the last player seating to frame 0
  Frame grace_frames = 2;                   // how long past a frame's start a missing input is awaited
  Frame max_lead_frames = 8;                // frames finalised ahead of the clock, at most
  Frame history = 256;                      // canonical frames kept for re-sending; > the client window
  Frame hash_interval = 30;                 // must equal the clients' Rollback hash_interval; told to them in Start
  std::size_t max_resyncs = 5;              // desync repairs for one client before it is dropped
  std::size_t malformed_limit = 100;        // malformed or invalid messages before a client is dropped
  std::size_t max_message_size = 64 * 1024; // the transport's reliable message bound: a Resync must fit
  transport::Micros join_timeout = 60'000'000; // from the first update until all seats are taken, or the match is a no-show
  transport::Micros max_duration = 3600'000'000; // from the start; then the match ends as timed out
  transport::Micros flush_timeout = 3'000'000;   // after the end, how long to keep sending until every client has the last frames
  // Called with each frame's canonical inputs as it is finalised, in order.
  std::function<void(Frame, std::span<const std::uint8_t>)> on_canonical;
};

struct MatchStats {
  std::uint64_t inputs_accepted = 0;   // individual frame inputs taken
  std::uint64_t inputs_stale = 0;       // for a frame already finalised: mostly the redundant copies each message carries
  std::uint64_t inputs_far = 0;        // too far ahead of the server
  std::uint64_t inputs_invalid = 0;    // refused by Simulation::valid_input
  std::uint64_t messages_malformed = 0;
  std::uint64_t frames_missing = 0;    // player-frames finalised without the player's input
  std::uint64_t clients_dropped = 0;   // for too many bad messages, or a desync that would not stay repaired
  std::uint64_t hash_checked = 0;      // client checkpoints compared with the server's
  std::uint64_t hash_unknown = 0;      // reported for a frame the server holds no checkpoint for
  std::uint64_t desyncs = 0;           // reported hashes that differ from the server's
  std::uint64_t resyncs_sent = 0;      // state transfers: after a desync, or to a client too far behind
};

// How a Match sends. ConnectionIds are the host's.
class MatchIo {
 public:
  virtual ~MatchIo() = default;
  virtual bool send_unreliable(transport::ConnectionId id, std::span<const std::uint8_t> data) = 0;
  virtual bool send_reliable(transport::ConnectionId id, std::span<const std::uint8_t> data) = 0;
  virtual void disconnect(transport::ConnectionId id, transport::DisconnectReason why) = 0;
};

class Match {
 public:
  enum class State : std::uint8_t { Waiting, Running, Finished };
  // Why a Finished match ended.
  enum class End : std::uint8_t {
    None,
    Completed,  // the game said it is over
    Abandoned,  // everyone left after it started
    NoShow,     // not everyone arrived in time
    TimedOut,   // max_duration
  };

  Match(MatchIo& io, Simulation& sim, const MatchConfig& config = {});

  // Takes the connection into a seat (any free one when `seat` is npos). False if there is none free,
  // the match has started, or the seat is taken.
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);
  bool add_player(transport::ConnectionId id, std::size_t seat = npos);
  // The connection is gone (the host noticed). A player who leaves after the start plays neutral input.
  void remove_player(transport::ConnectionId id);
  // A message from a seated connection. Others are ignored.
  void on_message(transport::ConnectionId id, const transport::Message& m);
  void update(transport::Micros now);

  State state() const { return state_; }
  End end() const { return end_; }
  // Finished and every seated client has been sent the last frames (or flush_timeout passed): the host may let go.
  bool done() const { return state_ == State::Finished && flushed_; }
  std::size_t players() const { return players_; }
  bool seat_bound(std::size_t seat) const { return seat < seats_.size() && seats_[seat].joined; }
  // The next frame to finalise: frames below it are done.
  Frame next_frame() const { return next_final_; }
  transport::Micros start_time() const { return start_time_; }
  std::size_t seated() const;
  bool seat_connected(std::size_t seat) const { return seat < seats_.size() && seats_[seat].connected; }
  std::optional<transport::ConnectionId> connection(std::size_t seat) const;
  const MatchStats& stats() const { return stats_; }

 private:
  static constexpr std::size_t kPending = 64;  // frames ahead for which inputs are kept
  struct Seat {
    transport::ConnectionId id = 0;
    bool connected = false;
    bool joined = false;         // ever seated in this match
    Frame ack = 0;               // the first canonical frame the client still needs
    Frame sent_to = 0;           // next_final_ at the last send
    transport::Micros last_send = 0;
    std::size_t bad = 0;
    std::size_t desyncs = 0;     // repairs so far
    Frame resync_frame = 0;      // the checkpoint last sent; hashes for earlier frames are stale
    bool resynced = false;
    std::vector<std::uint8_t> pending;  // kPending * input_size
    std::vector<Frame> pending_tag;     // which frame a slot holds
    std::vector<std::uint8_t> pending_has;
  };

  void start(transport::Micros now);
  void finish(End why, transport::Micros now);
  void finalise(transport::Micros now);
  void send_frames(transport::Micros now);
  void drop(Seat& s, transport::DisconnectReason why);
  bool has_input(const Seat& s, Frame f) const;
  transport::Micros frame_start(Frame f) const;
  std::int64_t wall_frame(transport::Micros now) const;
  void count_bad(Seat& s);
  void on_hash(Seat& s, const msg::Hash& h);
  void resync(Seat& s, bool desync);
  void record_checkpoint();

  struct CheckpointRec {
    Frame frame;
    std::uint64_t hash;
    std::vector<std::uint8_t> snapshot;
  };
  static constexpr std::size_t kCheckpoints = 8;

  MatchConfig cfg_;
  MatchIo& io_;
  Simulation& sim_;
  std::size_t players_, in_size_, frame_bytes_;
  State state_ = State::Waiting;
  End end_ = End::None;
  bool flushed_ = false;
  transport::Micros created_ = -1, finished_at_ = 0;
  std::vector<Seat> seats_;
  transport::Micros start_time_ = 0;
  Frame next_final_ = 0;
  std::vector<std::uint8_t> previous_;  // last canonical frame
  std::vector<std::uint8_t> history_;   // cfg.history * frame_bytes
  std::vector<std::uint8_t> out_;
  std::deque<CheckpointRec> checkpoints_;
  MatchStats stats_;
};

}  // namespace npk::session
