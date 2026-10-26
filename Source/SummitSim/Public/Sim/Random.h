#pragma once

#include <cmath>
#include <cstdint>

namespace summit::sim {

// PCG32 (O'Neill 2014, pcg-random.org). Small, fast, deterministic across platforms.
// All simulation randomness must flow through this type with an explicit seed.
class Pcg32 {
 public:
  explicit Pcg32(std::uint64_t seed = 0x853c49e6748fea9bULL, std::uint64_t stream = 0xda3e39cb94b95bdbULL) {
    state_ = 0u;
    inc_ = (stream << 1u) | 1u;
    NextU32();
    state_ += seed;
    NextU32();
  }

  std::uint32_t NextU32() {
    const std::uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    const auto rot = static_cast<std::uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
  }

  // Uniform in [0, 1).
  double NextDouble() { return (NextU32() >> 5) * (1.0 / 134217728.0); }  // 27-bit mantissa
  double Uniform(double lo, double hi) { return lo + (hi - lo) * NextDouble(); }
  bool Chance(double probability) { return NextDouble() < probability; }

  // Standard normal via Box-Muller.
  double Normal(double mean = 0.0, double stddev = 1.0) {
    double u1 = NextDouble();
    if (u1 < 1e-300) u1 = 1e-300;
    const double u2 = NextDouble();
    return mean + stddev * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

 private:
  std::uint64_t state_;
  std::uint64_t inc_;
};

// Probability that a Poisson process with `ratePerSecond` fires at least once in dt.
inline double ProbabilityInInterval(double ratePerSecond, double dtSeconds) {
  return 1.0 - std::exp(-ratePerSecond * dtSeconds);
}

}  // namespace summit::sim
