#include "Sim/Rope.h"

#include <algorithm>
#include <cmath>

#include "Sim/Constants.h"
#include "Sim/Math.h"

namespace summit::sim {

// Helpers live in a file-specific namespace (not an anonymous one) so that Unreal unity builds, which
// concatenate several .cpp files into one translation unit, cannot collide with other sim files.
namespace rope_detail {

// UIAA 101 / EN 892 standard drop: 80 kg mass, fall factor 1.77 (drop height and rope length fixed by
// the standard; the rated "impact force" is the peak force measured on the first drop).
constexpr double kStandardDropMassKg = 80.0;
constexpr double kStandardDropFallFactor = 1.77;

// Theoretical maximum fall factor in lead climbing: falling past the belay with all rope paid out.
constexpr double kMaxFallFactor = 2.0;

// Used ropes lose energy absorption: impact forces measured on retired ropes are typically 10-35 %
// above the new-rope rating (Beal / Mammut used-rope drop tests; DAV Sicherheitsforschung).
constexpr double kWearImpactIncrease = 0.35;

// Low-stretch (EN 1891 type A) static ropes and fixed lines: ~2x the stiffness of a dynamic rope with
// the same rating. EN 1891 allows <= 6 kN for 100 kg at fall factor 0.3, and 2x reproduces that
// (~6.3 kN) while giving ~9-10 kN for 80 kg at FF 1, in line with drop tests on static kernmantle
// (Petzl/Beal technical notes). Factor-2 falls still exceed 12 kN, so shock-loading a fixed line
// stays dangerous. Dyneema slings are far stiffer and are not modelled as rope.
constexpr double kStaticStiffnessMultiplier = 2.0;

// A dynamic belay / rope slipping through a tube device lowers the peak force by up to ~35 %
// (Petzl belay-device slip tests; Beal "dynamic belay" data: soft catches cut peak force by 30-40 %).
constexpr double kMaxDynamicBelayReduction = 0.35;

// Friction of the rope over the top carabiner: belayer-side tension is ~0.6-0.7 of the climber-side
// tension, so the top piece sees ~1.6-1.7x the climber force (Petzl, "Forces on the top anchor";
// Beal measurements). This is the "pulley effect".
constexpr double kTopPiecePulleyFactor = 1.66;

// Sliding/tumbling contact friction for a falling body on steep terrain (clothing on snow/rock).
constexpr double kTerrainContactFriction = 0.3;

// Standard normal CDF.
double NormalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// Log-normal scatter (sigma of ln strength) of real anchor strengths around the median, per type.
// Bolts are consistent; snow anchors and old fixed lines are notoriously variable (DAV and BD tests).
double AnchorStrengthLogSigma(AnchorType type) {
  switch (type) {
    case AnchorType::Bolt: return 0.12;
    case AnchorType::Cam: return 0.20;
    case AnchorType::Nut: return 0.25;
    case AnchorType::Piton: return 0.35;
    case AnchorType::IceScrew: return 0.25;
    case AnchorType::SnowPicket: return 0.35;
    case AnchorType::Deadman: return 0.40;
    case AnchorType::Sling: return 0.25;
    case AnchorType::FixedLine: return 0.50;
    case AnchorType::Count: break;
  }
  return 0.3;
}

// Warm ice is soft and screws melt out under load; very cold ice is brittle and "dinner-plates".
// Harmston (1998), Black Diamond ice screw strength tests: holding power drops sharply in wet,
// near-0 °C or aerated ice; very cold (< -15 °C) ice fractures around the threads.
double IceTemperatureFactor(double temperatureC) {
  const double warm = 1.0 - 0.55 * SmoothStep(-4.0, 1.0, temperatureC);
  const double brittle = 1.0 - 0.30 * SmoothStep(-12.0, -30.0, temperatureC);
  return warm * brittle;
}

// Wet, warming snow loses cohesion and anchors pull through (snow-anchor field tests, DAV).
double SnowTemperatureFactor(double temperatureC) {
  return 1.0 - 0.45 * SmoothStep(-3.0, 2.0, temperatureC);
}

// Melt-out of anything placed in snow or ice: fast when warm (days), slower in cold (weeks).
double MeltOutFactor(double ageDays, double temperatureC) {
  if (ageDays <= 0.0) return 1.0;
  const double tauDays = Lerp(3.0, 30.0, SmoothStep(0.0, -15.0, temperatureC));
  return 0.2 + 0.8 * std::exp(-ageDays / tauDays);
}

// Snow anchor placement: placementQuality01 spans a poor vertical placement (0), a good vertical
// placement (0.5) and a well-cut, buried T-slot (1). T-slot pickets hold ~1.6x a vertical picket.
double SnowPlacementFactor(double q) {
  q = Clamp01(q);
  return q < 0.5 ? Lerp(0.5, 1.0, q / 0.5) : Lerp(1.0, 1.6, (q - 0.5) / 0.5);
}

}  // namespace rope_detail

double FallFactor(double fallDistanceM, double ropeOutM) {
  // With no rope paid out the climber falls straight onto the belay: report the lead-climbing worst
  // case (factor 2) rather than dividing by zero. Values are capped to [0, 2] for the same reason.
  if (ropeOutM <= 0.0) return rope_detail::kMaxFallFactor;
  return Clamp(fallDistanceM / ropeOutM, 0.0, rope_detail::kMaxFallFactor);
}

double RopeStiffnessN(const RopeSpec& rope) {
  // Invert F = mg + sqrt((mg)^2 + 2 mg k f) for k:  (F - mg)^2 = (mg)^2 + 2 mg k f
  //   =>  k = F (F - 2 mg) / (2 mg f)
  // using the rated impact force at the UIAA 101 / EN 892 standard drop (80 kg, f = 1.77).
  // A new 8.8 kN rope gives k ~ 22.9 kN, consistent with published dynamic rope moduli (~20 kN).
  const double mg = rope_detail::kStandardDropMassKg * kGravity;
  const double ratedN =
      std::max(1.0, rope.ratedImpactForceKN) * 1000.0 *
      (1.0 + rope_detail::kWearImpactIncrease * Clamp01(rope.wear01));
  // The rated force must exceed 2 mg (a static load suddenly applied); clamp silly inputs.
  const double forceN = std::max(ratedN, 2.0 * mg + 100.0);
  double k = forceN * (forceN - 2.0 * mg) / (2.0 * mg * rope_detail::kStandardDropFallFactor);
  if (!rope.dynamic) k *= rope_detail::kStaticStiffnessMultiplier;
  return k;
}

double PeakImpactForceKN(double climberMassKg, double fallFactor, const RopeSpec& rope,
                         double belayDynamic01) {
  // Wexler (1950), "The theory of belaying", AAJ: energy balance of a mass on a linear-elastic rope,
  //   F = m g + sqrt((m g)^2 + 2 m g k f).
  // The force depends on the fall factor, not the fall length, because a longer rope stores more energy.
  const double m = std::max(0.0, climberMassKg);
  if (m <= 0.0) return 0.0;
  const double f = Clamp(fallFactor, 0.0, rope_detail::kMaxFallFactor);
  const double mg = m * kGravity;
  const double k = RopeStiffnessN(rope);
  const double forceN = mg + std::sqrt(mg * mg + 2.0 * mg * k * f);
  const double belayReduction = 1.0 - rope_detail::kMaxDynamicBelayReduction * Clamp01(belayDynamic01);
  return forceN * belayReduction / 1000.0;
}

double NominalAnchorStrengthKN(AnchorType type) {
  // Typical rated / laboratory strengths of the hardware itself (EN 568 ice screws >= 10 kN,
  // EN 12276 cams 10-14 kN, EN 12270 nuts 6-12 kN, EN 959 bolts >= 25 kN, EN 566 slings 22 kN).
  switch (type) {
    case AnchorType::IceScrew: return 10.0;
    case AnchorType::SnowPicket: return 12.0;
    case AnchorType::Deadman: return 12.0;
    case AnchorType::Cam: return 12.0;
    case AnchorType::Nut: return 9.0;
    case AnchorType::Piton: return 20.0;
    case AnchorType::Bolt: return 25.0;
    case AnchorType::Sling: return 22.0;
    case AnchorType::FixedLine: return 15.0;
    case AnchorType::Count: break;
  }
  return 10.0;
}

double AnchorEffectiveStrengthKN(const Anchor& anchor, double mediumQuality01, double temperatureC) {
  using namespace rope_detail;
  // Effective strength = min(hardware, what the medium can hold) x placement x ageing.
  // Hardware ratings are minimum breaking strengths; lab breaks typically come in ~25 % higher.
  const double medium = Clamp01(mediumQuality01);
  const double q = Clamp01(anchor.placementQuality01);
  const double age = std::max(0.0, anchor.ageDays);
  const double hardwareKN = std::max(0.0, anchor.ratedStrengthKN) * 1.25;

  double mediumKN = hardwareKN;
  double placement = 1.0;
  double ageing = 1.0;
  switch (anchor.type) {
    case AnchorType::IceScrew:
      // Good water ice: 12-22 kN (Harmston 1998 / Black Diamond; Petzl ice screw tests).
      // Aerated, rotten or wet ice: ~3-5 kN.
      mediumKN = 18.0 * Lerp(0.2, 1.0, medium) * IceTemperatureFactor(temperatureC);
      placement = Lerp(0.35, 1.0, q);   // angle, full depth, clean threads
      ageing = MeltOutFactor(age, temperatureC);
      break;
    case AnchorType::SnowPicket:
      // Vertical picket ~1.5-5 kN from soft to hard snow, T-slot ~2.4-8 kN (DAV snow-anchor tests,
      // Blackshaw & Marsh; Freedom of the Hills 9th ed. ch. 16).
      mediumKN = Lerp(1.5, 5.0, medium) * SnowPlacementFactor(q) * SnowTemperatureFactor(temperatureC);
      ageing = MeltOutFactor(age, temperatureC);
      break;
    case AnchorType::Deadman:
      // Snow fluke: ~2-6 kN depending on snow; must be buried at the right angle or it pulls out.
      mediumKN = Lerp(2.0, 6.0, medium) * SnowPlacementFactor(q) * SnowTemperatureFactor(temperatureC);
      ageing = MeltOutFactor(age, temperatureC);
      break;
    case AnchorType::Cam:
      // In sound rock the cam itself breaks (10-14 kN); flared or friable rock lets it walk/pull.
      mediumKN = 25.0 * Lerp(0.25, 1.0, medium);
      placement = Lerp(0.25, 1.0, q);
      break;
    case AnchorType::Nut:
      mediumKN = 20.0 * Lerp(0.25, 1.0, medium);
      placement = Lerp(0.25, 1.0, q);
      break;
    case AnchorType::Piton:
      // Old fixed pitons range from bomber to useless; corrosion and freeze-thaw loosen them.
      mediumKN = 20.0 * Lerp(0.2, 1.0, medium);
      placement = Lerp(0.3, 1.0, q);
      ageing = 0.4 + 0.6 * std::exp(-age / (365.0 * 10.0));
      break;
    case AnchorType::Bolt:
      // Modern bolts 20-25 kN in good rock (UIAA 123 / EN 959); placement barely matters (drilled).
      mediumKN = 25.0 * Lerp(0.3, 1.0, medium);
      placement = Lerp(0.75, 1.0, q);
      ageing = 0.6 + 0.4 * std::exp(-age / (365.0 * 25.0));
      break;
    case AnchorType::Sling:
      // Sling around a horn: limited by the horn (rock quality) and UV-aged webbing (DAV: slings left
      // in place lose roughly half their strength within a few seasons).
      mediumKN = 22.0 * Lerp(0.15, 1.0, medium);
      placement = Lerp(0.5, 1.0, q);
      ageing = 0.3 + 0.7 * std::exp(-age / 365.0);
      break;
    case AnchorType::FixedLine:
      // Expedition fixed lines: pickets/screws in snow and ice holding thin, often old rope. Anchors melt
      // out over days to weeks and the rope degrades under high-altitude UV; strength is unpredictable.
      mediumKN = 12.0 * Lerp(0.2, 1.0, medium) * IceTemperatureFactor(temperatureC);
      placement = Lerp(0.5, 1.0, q);
      ageing = MeltOutFactor(age, temperatureC) * (0.25 + 0.75 * std::exp(-age / 120.0));
      break;
    case AnchorType::Count:
      break;
  }
  return std::max(0.0, std::min(hardwareKN, mediumKN) * placement * ageing);
}

double UnprotectedFallInjury01(double fallDistanceM, double slopeAngleDeg) {
  // Unprotected falls: on steep ground (>~60°) falls beyond ~20-25 m are usually fatal (the free-fall
  // LD50 is roughly 4 storeys, ~12-15 m; Lapostolle et al. 2005). On easier slopes the body slides and
  // tumbles, so lethal distances grow to ~150 m (mountaineering accident reports, ANAM).
  const double d = std::max(0.0, fallDistanceM);
  const double steep = SmoothStep(25.0, 60.0, slopeAngleDeg);
  const double fatalM = Lerp(150.0, 25.0, steep);
  return Clamp01(std::pow(Clamp01(d / fatalM), 0.8));
}

double SelfArrestSuccessProbability(double slopeAngleDeg, bool iceOrHardSnow, double skill01,
                                    double reactionTimeS, double fatigue01) {
  // Self-arrest physics after Freedom of the Hills (9th ed., ch. 16) and AAI training notes: a
  // falling climber must get the pick in within the first second or two, before speed builds;
  // on hard snow/ice steeper than ~35-40° slides rarely stop.
  //   slide acceleration  a = g (sin θ - μ_slide cos θ),  v = a t_r = sqrt(2 g d (sin θ - μ cos θ))
  //   arrest deceleration b = g (μ_arrest cos θ - sin θ),  stopping distance s = v^2 / (2 b)
  const double theta = DegToRad(Clamp(slopeAngleDeg, 0.0, 89.0));
  const double skill = Clamp01(skill01);
  const double fatigue = Clamp01(fatigue01);
  const double reaction = std::max(0.0, reactionTimeS);

  // Sliding friction of clothing: ~0.1 on hard snow/ice, ~0.3 ploughing through soft snow.
  const double muSlide = iceOrHardSnow ? 0.10 : 0.30;
  // Effective braking friction of pick + body: ice-axe picks skate on hard surfaces.
  const double muArrest = (iceOrHardSnow ? 0.75 : 1.25) * Lerp(0.6, 1.1, skill) * (1.0 - 0.3 * fatigue);
  const double sinT = std::sin(theta);
  const double cosT = std::cos(theta);

  const double baseSkill = Lerp(0.6, 0.97, skill) * (1.0 - 0.35 * fatigue);
  const double slideAccel = kGravity * (sinT - muSlide * cosT);
  if (slideAccel <= 0.0) {
    // The slope is too gentle to keep sliding; the climber stops anyway.
    return Clamp01(0.9 + 0.1 * baseSkill);
  }
  const double v = slideAccel * reaction;
  const double reactionDistance = 0.5 * slideAccel * reaction * reaction;
  const double brakeMarginG = muArrest * cosT - sinT;
  if (brakeMarginG <= 0.0) {
    // Arrest cannot overcome gravity: only luck (a runout, soft patch) stops the slide.
    return iceOrHardSnow ? 0.02 : 0.05;
  }
  const double stopDistance = v * v / (2.0 * kGravity * brakeMarginG);
  // Long slides hit rocks or lose the axe; past the grip speed the axe is ripped from the hands.
  const double runoutScaleM = iceOrHardSnow ? 15.0 : 30.0;
  const double gripSpeedMS = (iceOrHardSnow ? 6.0 : 9.0) * Lerp(0.8, 1.2, skill);
  const double runout = std::exp(-(stopDistance + reactionDistance) / runoutScaleM);
  const double speed = 1.0 - SmoothStep(0.5 * gripSpeedMS, gripSpeedMS, v);
  return Clamp01(std::max(iceOrHardSnow ? 0.02 : 0.05, baseSkill * runout * speed));
}

FallOutcome ResolveFall(const FallScenario& scenario, const RopeSpec& rope, const Anchor& anchor,
                        double mediumQuality01, double temperatureC, Pcg32& rng) {
  using namespace rope_detail;
  FallOutcome out;
  const double mass = std::max(1.0, scenario.climberMassKg);
  const double ropeOut = std::max(0.0, scenario.ropeOutM);
  const double ropeForStretch = std::max(0.5, ropeOut);
  const double slope = Clamp(scenario.slopeAngleDeg, 0.0, 90.0);
  const double k = RopeStiffnessN(rope);

  // Below vertical the body slides/tumbles: only g (sin θ - μ cos θ) drives the fall, which lowers the
  // force on the rope (equivalent to a lighter climber in Wexler's energy balance) but adds contact.
  const double theta = DegToRad(slope);
  const double gravityShare =
      slope >= 89.0 ? 1.0
                    : Clamp(std::sin(theta) - kTerrainContactFriction * std::cos(theta), 0.05, 1.0);
  const double effMass = mass * gravityShare;

  auto resolveDrop = [&](double freeFallM, double& fallFactorOut, double& forceKNOut,
                         double& totalDistanceOut) {
    fallFactorOut = FallFactor(freeFallM, ropeOut);
    forceKNOut = PeakImpactForceKN(effMass, fallFactorOut, rope, scenario.belayDynamic01);
    // Rope stretch at peak: x = F L / k. A soft catch also lets rope slip through the device.
    const double stretch = forceKNOut * 1000.0 * ropeForStretch / k;
    const double slip = 0.5 * Clamp01(scenario.belayDynamic01) * fallFactorOut;
    totalDistanceOut = freeFallM + stretch + slip;
  };

  // Lead fall: twice the distance above the last piece plus slack in the system.
  const double freeFall = 2.0 * std::max(0.0, scenario.distanceAboveProtectionM) + std::max(0.0, scenario.slackM);
  double fallFactor = 0.0, forceKN = 0.0, distance = 0.0;
  resolveDrop(freeFall, fallFactor, forceKN, distance);

  // Top piece load: climber force plus belayer-side tension (pulley effect).
  const double anchorLoadKN = forceKN * kTopPiecePulleyFactor;
  const double strengthKN = AnchorEffectiveStrengthKN(anchor, mediumQuality01, temperatureC);
  // Log-normal strength distribution around the effective (median) strength: failure becomes likely
  // as the load approaches the strength, rather than at a hard threshold.
  double pFail = 1.0;
  if (strengthKN > 1e-6) {
    const double z = std::log(std::max(1e-6, anchorLoadKN) / strengthKN) / AnchorStrengthLogSigma(anchor.type);
    pFail = NormalCdf(z);
  }
  out.anchorFailed = rng.Chance(pFail);

  if (out.anchorFailed) {
    // The piece rips: the climber keeps falling past it and ends up on the belay (factor ~2 fall onto
    // the belay anchor, which is assumed to hold). Total drop = twice the height above the belay.
    const double secondFreeFall = 2.0 * (ropeOut) + std::max(0.0, scenario.slackM);
    resolveDrop(secondFreeFall, fallFactor, forceKN, distance);
    distance += 2.0 * std::max(0.0, scenario.distanceAboveProtectionM);
  }
  out.fallDistanceM = distance;
  out.fallFactor = fallFactor;
  out.peakForceKN = forceKN;

  // Rope cut: standard ropes can be cut over a sharp edge at a few kN (UIAA 101 sharp-edge test, 0.75 mm
  // edge); badly worn ropes can also fail under high force without an edge (core damage).
  double pCut = 0.0;
  if (scenario.sharpEdge) pCut += 0.9 * SmoothStep(3.0, 9.0, forceKN);
  const double wear = Clamp01(rope.wear01);
  pCut += 0.4 * wear * wear * SmoothStep(8.0, 14.0, forceKN);
  out.ropeCut = rng.Chance(Clamp01(pCut));

  // Injury from deceleration: UIAA limits rope impact to 12 kN as the tolerance limit for a harnessed
  // body (Wexler; military harness research); forces > 12 kN cause internal and spinal injury.
  const double decel = std::pow(Clamp01((forceKN - 6.0) / 16.0), 1.3);
  // Contact injury: falls on 30-85° terrain bounce and tumble (most leader-fall injuries are impacts,
  // not rope loads; Schöffl et al. 2013, climbing injury epidemiology). Vertical/overhanging falls
  // mostly swing into the wall.
  const double contactWeight =
      slope >= 88.0 ? 0.15 : SmoothStep(20.0, 45.0, slope) * (1.0 - 0.6 * SmoothStep(70.0, 88.0, slope));
  const double contact = contactWeight * (1.0 - std::exp(-distance / 20.0));
  double injury = 1.0 - (1.0 - decel) * (1.0 - contact);
  if (out.ropeCut) {
    // Unroped from here: assume terrain below is at least as long as the rope run plus a margin.
    injury = std::max(injury, UnprotectedFallInjury01(std::max(25.0, distance + ropeOut), slope));
  }
  // Natural variability of injuries (how you land), +-~15 %.
  injury *= std::exp(rng.Normal(0.0, 0.15));
  out.injurySeverity01 = Clamp01(injury);
  return out;
}

}  // namespace summit::sim
