#include "npk/session/simulation.hpp"

#include <algorithm>

namespace npk::session {

void Simulation::neutral_input(std::span<std::uint8_t> out) const {
  std::fill(out.begin(), out.end(), std::uint8_t{0});
}

void Simulation::predict_input(std::span<const std::uint8_t> previous, std::span<std::uint8_t> out) const {
  std::copy(previous.begin(), previous.end(), out.begin());
}

}  // namespace npk::session
