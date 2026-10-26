#pragma once

// Physical constants shared by the simulation core. SI units unless suffixed.

namespace summit::sim {

inline constexpr double kGravity = 9.80665;                   // m/s^2 (standard gravity)
inline constexpr double kSeaLevelPressureKPa = 101.325;       // ISA sea-level pressure
inline constexpr double kMmHgPerKPa = 7.500616827;            // 1 kPa in mmHg
inline constexpr double kBodyWaterVapourPressureMmHg = 47.0;  // saturated vapour at 37 °C
inline constexpr double kO2FractionDryAir = 0.2093;           // FiO2 of ambient air
inline constexpr double kGasConstantDryAir = 287.05;          // J/(kg·K)
inline constexpr double kCelsiusToKelvin = 273.15;
inline constexpr double kStefanBoltzmann = 5.670374419e-8;    // W/(m^2·K^4)
inline constexpr double kKcalToJoule = 4184.0;
inline constexpr double kSecondsPerHour = 3600.0;
inline constexpr double kSecondsPerDay = 86400.0;
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kDeathZoneAltitudeM = 8000.0;         // conventional threshold

}  // namespace summit::sim
