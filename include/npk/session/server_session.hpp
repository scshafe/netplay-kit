#pragma once

// The server end of a match: the authoritative host (docs/design.md, "Session
// layer"). It owns a transport Server, gives the first `player_count` clients
// that connect their seats in order, and when all are seated starts the match:
// frame 0 begins at a time it announces, and frame F at start + F / tick rate.
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
#include "npk/transport/server.hpp"

namespace npk::session {

struct ServerSessionConfig {
  transport::ServerConfig transport{};
  int tick_rate_hz = 60;
  transport::Micros start_delay = 500'000;  // from the last player seating to frame 0
  Frame grace_frames = 2;                   // how long past a frame's start a missing input is awaited
  Frame max_lead_frames = 8;                // frames finalised ahead of the clock, at most
  Frame history = 256;                      // canonical frames kept for re-sending; > the client window
  Frame hash_interval = 30;                 // must equal the clients' Rollback hash_interval; told to them in Start
  std::size_t max_resyncs = 5;              // desync repairs for one client before it is dropped
  std::size_t malformed_limit = 100;        // malformed or invalid messages before a client is dropped
  // Called with each frame's canonical inputs as it is finalised, in order.
  std::function<void(Frame, std::span<const std::uint8_t>)> on_canonical;
};

struct ServerSessionStats {
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

class ServerSession {
 public:
  enum class State : std::uint8_t { Waiting, Running, Finished };

  ServerSession(transport::Link& link, Simulation& sim, const ServerSessionConfig& config = {});

  void update(transport::Micros now);

  State state() const { return state_; }
  // The next frame to finalise: frames below it are done.
  Frame next_frame() const { return next_final_; }
  transport::Micros start_time() const { return start_time_; }
  std::size_t seated() const;
  bool seat_connected(std::size_t seat) const { return seat < seats_.size() && seats_[seat].connected; }
  std::optional<transport::RttEstimator> rtt(std::size_t seat) const;
  const ServerSessionStats& stats() const { return stats_; }
  const transport::Server& transport() const { return server_; }

 private:
  static constexpr std::size_t kPending = 64;  // frames ahead for which inputs are kept
  struct Seat {
    transport::ConnectionId id = 0;
    bool connected = false;
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

  void on_connected(const transport::ServerEvent& e, transport::Micros now);
  void on_disconnected(const transport::ServerEvent& e);
  void on_message(const transport::ServerMessage& m);
  void start(transport::Micros now);
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

  ServerSessionConfig cfg_;
  Simulation& sim_;
  transport::Server server_;
  std::size_t players_, in_size_, frame_bytes_;
  State state_ = State::Waiting;
  std::vector<Seat> seats_;
  transport::Micros start_time_ = 0;
  Frame next_final_ = 0;
  std::vector<std::uint8_t> previous_;  // last canonical frame
  std::vector<std::uint8_t> history_;   // cfg.history * frame_bytes
  std::vector<std::uint8_t> out_;
  std::deque<CheckpointRec> checkpoints_;
  ServerSessionStats stats_;
};

}  // namespace npk::session
