#include "npk/transport/client.hpp"

#include "entropy.hpp"

namespace npk::transport {

Client::Client(Link& link, const Address& server, const ClientConfig& config)
    : link_(link), server_(server), cfg_(config), base_seed_(config.seed) {}

void Client::connect(Micros now) {
  if (state_ == State::Connecting || state_ == State::Connected) return;
  // A fresh nonce per attempt, so a reply to an earlier attempt is never mistaken for this one.
  Entropy e(base_seed_ == 0 ? 0 : base_seed_ + attempts_);
  ++attempts_;
  nonce_ = e.next();
  state_ = State::Connecting;
  have_challenge_ = false;
  resent_init_ = false;
  handshake_rtt_.reset();
  id_ = 0;
  conn_.reset();
  started_ = now_ = now;
  send_current(now);
}

void Client::send_current(Micros now) {
  PacketBuilder b(Header{have_challenge_ ? PacketType::Response : PacketType::Init});
  if (have_challenge_) {
    b.response(challenge_);
  } else {
    b.init(nonce_);
  }
  b.pad_to(kMinHandshakeRequest);  // so the server's reply can never be larger
  link_.send(server_, b.bytes());
  last_sent_ = now;
}

void Client::update(Micros now) {
  now_ = now;
  Address from;
  for (;;) {
    const auto n = link_.receive(from, rx_);
    if (!n) break;
    handle(from, {rx_.data(), *n});
  }
  if (state_ == State::Connecting) {
    if (now - started_ > cfg_.handshake_timeout) {
      state_ = State::Failed;
    } else if (now - last_sent_ >= cfg_.retry_interval) {
      if (!have_challenge_) resent_init_ = true;
      send_current(now);
    }
  } else if (state_ == State::Connected) {
    if (conn_->failed()) {
      conn_->send_close(now, link_, server_, DisconnectReason::ProtocolError);
      state_ = State::Closed;
      close_reason_ = DisconnectReason::ProtocolError;
    } else if (now - conn_->last_heard() > cfg_.idle_timeout) {
      state_ = State::Closed;
      close_reason_ = DisconnectReason::Timeout;
    } else {
      conn_->flush(now, link_, server_);
    }
  }
}

void Client::handle(const Address& from, std::span<const std::uint8_t> datagram) {
  Packet p;
  if (from != server_ || parse_packet(datagram, p) != ParseError::None) return;
  switch (p.header.type) {
    case PacketType::Challenge: {
      ChallengeBody c;
      if (state_ != State::Connecting || have_challenge_ || parse_challenge(p.body, c) != ParseError::None ||
          c.client_nonce != nonce_) {
        return;
      }
      if (!resent_init_) handshake_rtt_ = now_ - started_;
      challenge_ = c;
      have_challenge_ = true;
      send_current(now_);
      break;
    }
    case PacketType::Accept: {
      AcceptBody a;
      if (state_ != State::Connecting || !have_challenge_ || p.header.connection == 0 ||
          parse_accept(p.body, a) != ParseError::None || a.client_nonce != nonce_) {
        return;
      }
      id_ = p.header.connection;
      accept_server_time_ = a.server_time_us;
      accept_local_time_ = now_;
      conn_.emplace(id_, cfg_.connection, now_);
      if (handshake_rtt_) {
        // The Accept left the server at its time T and took about half the round trip.
        conn_->add_clock_sample(*handshake_rtt_, static_cast<Micros>(a.server_time_us) + *handshake_rtt_ / 2 - now_);
      }
      state_ = State::Connected;
      break;
    }
    case PacketType::Data: {
      if (state_ != State::Connected || p.header.connection != id_) return;
      conn_->on_packet(p, now_);
      if (conn_->peer_closed()) {
        state_ = State::Closed;
        close_reason_ = conn_->peer_close_reason();
      }
      break;
    }
    default: break;
  }
}

void Client::disconnect(DisconnectReason reason) {
  if (state_ == State::Connected) {
    conn_->send_close(now_, link_, server_, reason);
    close_reason_ = DisconnectReason::Closed;
  }
  if (state_ == State::Connected || state_ == State::Connecting) state_ = State::Closed;
}

bool Client::send_unreliable(std::span<const std::uint8_t> data) {
  return state_ == State::Connected && conn_->send_unreliable(data);
}

bool Client::send_reliable(std::span<const std::uint8_t> data) {
  return state_ == State::Connected && conn_->send_reliable(data);
}

std::optional<Message> Client::poll_message() { return conn_ ? conn_->poll_message() : std::nullopt; }

}  // namespace npk::transport
