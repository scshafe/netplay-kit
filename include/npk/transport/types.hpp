#pragma once

#include <cstdint>

namespace npk::transport {

// Names one established connection on a server. Never 0 (0 means "none yet" on the wire).
using ConnectionId = std::uint32_t;

// Why a connection ended. The numbers are the Close frame's reason byte; a
// peer's Close always reads as Closed on the receiving side.
enum class DisconnectReason : std::uint8_t {
  Closed = 0,          // the peer or the application closed it normally
  Timeout = 1,         // nothing heard for too long
  Replaced = 2,        // the same address connected again
  ServerShutdown = 3,
};

}  // namespace npk::transport
