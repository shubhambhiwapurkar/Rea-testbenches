#pragma once

namespace summit::sim {

// Barometric pressure at altitude.
// Base model: West JB (1996) "Prediction of barometric pressures at high altitudes with the use of
// model atmospheres", J Appl Physiol 81:1850-1854:
//   PB[Torr] = exp(6.63268 - 0.1112 h - 0.00149 h^2), h in km.
// This fits subtropical Himalayan observations (Everest summit ~253 Torr / 33.7 kPa in May), whereas
// ISA underestimates pressure there. Latitude and season corrections are applied on top: pressure is
// lower at high latitude (Denali) and in winter.
double BarometricPressureKPa(double altitudeM, double latitudeDeg = 28.0, int dayOfYear = 140);

// ICAO standard atmosphere (troposphere + lower stratosphere), for reference and comparison.
double IsaPressureKPa(double altitudeM);

// Dry-air density from the ideal gas law.
double AirDensityKgM3(double pressureKPa, double temperatureC);

// Inspired O2 partial pressure in the trachea: PiO2 = FiO2 * (PB - 47 mmHg).
double InspiredPO2MmHg(double pressureKPa, double fio2 = 0.2093);

// Boiling point of water (deg C) at a given pressure (Antoine equation). Affects cooking and melting snow.
double WaterBoilingPointC(double pressureKPa);

}  // namespace summit::sim
