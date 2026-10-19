#pragma once

// The client end of a match: a transport Client, a Rollback over the game's
// Simulation, and the clock that ties them to the server's.
//
// After the handshake the server sends Start: this client's seat, the tick rate
// and the server-clock time at which frame 0 begins. From the measured clock
// offset the client knows which frame is "now" (the *wall frame* W). Each wall
// frame it samples the local input (the InputSource), labels it for frame
// W + delay, uses it at once as the provisional input for that frame and sends
// the latest few labelled inputs to the server, along with the first canonical
// frame it still needs. It simulates every frame up to W it can (at most
// `max_steps_per_update` per update, and never more than `max_rollback` past the
// confirmed ones), taking canonical inputs from the server as they arrive and
// rolling back when they differ from what was simulated.

#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "npk/session/input_delay.hpp"
#include "npk/session/messages.hpp"
#include "npk/session/rollback.hpp"
#include "npk/transport/client.hpp"

namespace npk::session {

struct ClientSessionConfig {
  transport::ClientConfig transport{};
  RollbackConfig rollback{};
  InputDelayConfig delay{};  // tick_rate_hz is replaced by the server's
  std::size_t max_steps_per_update = 4;
  transport::Micros delay_eval_interval = 500'000;
  std::size_t checkpoint_log = 64;  // checkpoints kept for inspection
  // For a game server that hosts many matches: the room and the token the lobby
  // issued. Sent as the first message once connected.
  std::optional<msg::Join> join{};
};

struct ClientSessionStats {
  std::uint64_t frames_simulated = 0;       // including re-simulated ones: see Rollback::stats
  std::uint64_t messages_malformed = 0;
  std::uint64_t stalls = 0;                 // updates where the simulation could not catch up to the wall frame
  std::uint64_t delay_changes = 0;
  std::uint64_t resyncs = 0;                // state transfers from the server taken
};

class ClientSession {
 public:
  enum class State : std::uint8_t { Idle, Connecting, Waiting, Running, Ended };
  // Called once per wall frame; fills `out` (the game's input_size() bytes) with
  // the local player's input right now.
  using InputSource = std::function<void(Frame wall_frame, std::span<std::uint8_t> out)>;

  ClientSession(transport::Link& link, const transport::Address& server, Simulation& sim,
                InputSource input, const ClientSessionConfig& config = {});

  void connect(transport::Micros now);
  void update(transport::Micros now);
  void disconnect();

  State state() const { return state_; }
  // Why the session ended, if it did.
  transport::DisconnectReason end_reason() const { return end_reason_; }
  PlayerIndex player() const { return player_; }
  int input_delay() const { return delay_ ? delay_->current() : 0; }
  const Rollback& rollback() const { return rb_; }
  const transport::Client& transport() const { return client_; }
  const ClientSessionStats& stats() const { return stats_; }
  // Recent checkpoints (frame, hash), oldest first.
  const std::deque<Checkpoint>& checkpoints() const { return checkpoint_log_; }

 private:
  void on_message(const transport::Message& m);
  void on_start(const msg::Start& s);
  void on_frames(const msg::Frames& f);
  void on_resync(const transport::Message& m);
  void end(transport::DisconnectReason why);
  void label_inputs(Frame wall);
  void send_inputs();

  ClientSessionConfig cfg_;
  Simulation& sim_;
  transport::Client client_;
  Rollback rb_;
  InputSource source_;
  std::size_t in_size_;
  State state_ = State::Idle;
  transport::DisconnectReason end_reason_ = transport::DisconnectReason::Closed;
  PlayerIndex player_ = 0;
  int tick_rate_ = 60;
  transport::Micros start_time_ = 0;  // server clock
  std::optional<InputDelay> delay_;
  transport::Micros next_delay_eval_ = 0;
  std::int64_t last_wall_ = -1;
  std::int64_t wall_hint_ = -1;
  std::int64_t last_label_ = -1;  // highest frame given a local input
  std::deque<std::pair<Frame, std::vector<std::uint8_t>>> local_;  // labelled inputs not yet canonical
  std::vector<std::uint8_t> cur_input_, out_;
  std::deque<Checkpoint> checkpoint_log_;
  ClientSessionStats stats_;
};

}  // namespace npk::session
