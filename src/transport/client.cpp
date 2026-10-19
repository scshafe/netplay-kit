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
  started_ = now_ = last_heard_ = now;
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
  } else if (state_ == State::Connected && now - last_heard_ > cfg_.idle_timeout) {
    state_ = State::Closed;
    close_reason_ = DisconnectReason::Timeout;
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
      last_heard_ = now_;
      state_ = State::Connected;
      break;
    }
    case PacketType::Data: {
      if (state_ != State::Connected || p.header.connection != id_) return;
      last_heard_ = now_;
      FrameReader r(p.body);
      Frame f;
      while (!r.done()) {
        if (r.next(f) != ParseError::None) break;
        if (f.kind == FrameKind::Close) {
          state_ = State::Closed;
          close_reason_ = f.close_reason <= 3 ? static_cast<DisconnectReason>(f.close_reason)
                                              : DisconnectReason::Closed;
          return;
        }
      }
      break;
    }
    default: break;
  }
}

void Client::disconnect(DisconnectReason reason) {
  if (state_ == State::Connected) {
    PacketBuilder b(Header{PacketType::Data, id_, send_seq_++});
    b.add_close(static_cast<std::uint8_t>(reason));
    link_.send(server_, b.bytes());
    close_reason_ = DisconnectReason::Closed;
  }
  if (state_ == State::Connected || state_ == State::Connecting) state_ = State::Closed;
}

}  // namespace npk::transport
