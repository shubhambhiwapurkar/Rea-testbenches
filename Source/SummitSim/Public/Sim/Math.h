#pragma once

#include <algorithm>
#include <cmath>

#include "Sim/Constants.h"

namespace summit::sim {

struct Vec3 {
  double x = 0.0;  // right (m)
  double y = 0.0;  // forward (m)
  double z = 0.0;  // up (m)

  constexpr Vec3() = default;
  constexpr Vec3(double inX, double inY, double inZ) : x(inX), y(inY), z(inZ) {}

  constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
  constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  double Length() const { return std::sqrt(x * x + y * y + z * z); }
};

inline constexpr double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline constexpr double Clamp01(double v) { return Clamp(v, 0.0, 1.0); }
inline constexpr double Lerp(double a, double b, double t) { return a + (b - a) * t; }
inline constexpr double DegToRad(double d) { return d * kPi / 180.0; }
inline constexpr double RadToDeg(double r) { return r * 180.0 / kPi; }

// Smooth 0..1 ramp between edge0 and edge1 (Hermite), like GLSL smoothstep.
inline double SmoothStep(double edge0, double edge1, double x) {
  const double t = Clamp01((x - edge0) / (edge1 - edge0));
  return t * t * (3.0 - 2.0 * t);
}

// Exponential approach of `current` toward `target` with time constant tau (s).
inline double ApproachExp(double current, double target, double dtSeconds, double tauSeconds) {
  if (tauSeconds <= 0.0) return target;
  return target + (current - target) * std::exp(-dtSeconds / tauSeconds);
}

}  // namespace summit::sim
