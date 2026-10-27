#pragma once

#include "Sim/Export.h"

namespace summit::sim {

// Barometric pressure at altitude.
// Base model: West JB (1996) "Prediction of barometric pressures at high altitudes with the use of
// model atmospheres", J Appl Physiol 81:1850-1854:
//   PB[Torr] = exp(6.63268 - 0.1112 h - 0.00149 h^2), h in km.
// This fits subtropical Himalayan observations (Everest summit ~253 Torr / 33.7 kPa in May), whereas
// ISA underestimates pressure there. Latitude and season corrections are applied on top: pressure is
// lower at high latitude (Denali) and in winter.
SUMMITSIM_API double BarometricPressureKPa(double altitudeM, double latitudeDeg = 28.0, int dayOfYear = 140);

// Inverse of the uncorrected West (1996) model (28°N, May): the altitude at which PB equals
// `pressureKPa`. Used for "physiological equivalent altitude".
SUMMITSIM_API double AltitudeForPressureKPa(double pressureKPa);

// ICAO standard atmosphere (troposphere + lower stratosphere), for reference and comparison.
SUMMITSIM_API double IsaPressureKPa(double altitudeM);

// Dry-air density from the ideal gas law.
SUMMITSIM_API double AirDensityKgM3(double pressureKPa, double temperatureC);

// Inspired O2 partial pressure in the trachea: PiO2 = FiO2 * (PB - 47 mmHg).
SUMMITSIM_API double InspiredPO2MmHg(double pressureKPa, double fio2 = 0.2093);

// Boiling point of water (deg C) at a given pressure (Antoine equation). Affects cooking and melting snow.
SUMMITSIM_API double WaterBoilingPointC(double pressureKPa);

}  // namespace summit::sim
