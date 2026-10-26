#include "Sim/Balance.h"

#include <algorithm>
#include <cmath>

#include "Sim/Constants.h"

namespace summit::sim {

// Model overview (sources cited at each constant):
//  - Each axis (lateral, forward) is an inverted pendulum pivoting at the feet with natural divergence
//    rate w0 = sqrt(g / L), L = combined CoM height (Winter 1995, "Human balance and posture control
//    during standing and walking", Gait & Posture 3:193-214).
//  - Corrective control acts through the centre of pressure (CoP), which can only move within the base
//    of support. This saturation is what makes balance physically limited (Hof, Gazendam & Sinke 2005,
//    "The condition for dynamic stability", J Biomech 38:1-8: extrapolated CoM, XcoM = x + v / w0).
//  - The automatic postural controller is a delayed PD controller (Peterka 2002, "Sensorimotor
//    integration in human postural control", J Neurophysiol 88:1097-1118: Kp ~1.5-2 mgh/rad,
//    Kd ~0.3-0.5 mgh s/rad, loop delay ~0.1-0.2 s). Fatigue and hypoxia lower gains and add delay.
//  - Player lean (L2/R2) shifts the target posture and adds a voluntary weight shift (hip strategy,
//    Horak & Nashner 1986), as in Death Stranding: a well-timed lean makes heavy, lopsided loads
//    manageable, over-leaning tips you over the other way.

// File-specific namespace (not anonymous) so Unreal unity builds cannot collide with other sim files.
namespace balance_detail {

constexpr double kSubStepS = 1.0 / 120.0;
constexpr double kMaxFrameS = 0.25;        // larger hitches are clamped (no spiral of death)

// Base of support. Foot width ~9-10 cm; CoP can travel ~12 cm toward the toes and ~8 cm toward the
// heel from the resting CoM projection (Winter 1995).
constexpr double kFootHalfWidthM = 0.045;
constexpr double kFootFrontM = 0.12;
constexpr double kFootBackM = 0.08;
// During gait the lateral margin is set by single-stance and foot placement; mean mediolateral margins
// of stability while walking are well below those of quiet standing (Hof et al. 2007; McAndrew Young
// & Dingwell 2012). Modelled as a shrinking effective stance width with speed.
constexpr double kWalkingLateralShrink = 0.4;
// Stepping forward catches a forward fall: walking is "controlled falling" (Winter 1995).
constexpr double kWalkingForwardReachM = 0.25;
constexpr double kWalkingBackReachM = 0.08;   // a shortened step catches a backward lurch
// Trekking poles / axe widen the effective support (Jacobson et al. 2000; Bohne & Abendroth-Smith 2007,
// poles reduce postural sway under load). They carry modest loads, so the gain is partial.
constexpr double kPoleLateralM = 0.12;
constexpr double kPoleForwardM = 0.10;
// Bracing: wide, crouched stance.
constexpr double kBraceWidthFactor = 1.4;
constexpr double kBraceComFactor = 0.85;
constexpr double kBraceMaxSpeedMS = 0.15;

// Cross-slope coupling: on a cross slope part of the body follows the surface normal (ankle and foot
// alignment), producing a downhill toppling tendency; mediolateral stability margins shrink on
// cross-slopes (Dixon & Pearsall 2010, "Gait dynamics on a cross-slope walking surface", J Appl
// Biomech 26:17-25). The coupling fraction is a gameplay calibration.
constexpr double kSlopeCoupling = 0.2;

// Automatic compensation: people lean forward under a backpack almost fully (Bloom & Woodhull-McNeal
// 1987, "Postural adjustments while standing with two types of loaded backpack", Ergonomics 30), but
// only partially lean away from a lopsided load; the player supplies the rest.
constexpr double kAutoCargoCompLat = 0.25;
constexpr double kAutoCargoCompFwd = 0.85;
constexpr double kAutoDisturbanceComp = 0.6;  // leaning into steady wind / slope
constexpr double kBraceAutoComp = 0.9;

// Player authority: maximum voluntary lean (~12-13°) and hip-strategy weight shift (in units of CoP
// displacement / CoM height).
constexpr double kMaxVoluntaryLeanRad = 0.22;
constexpr double kPlayerShiftAuthority = 0.07;
constexpr double kAutoHipAuthority = 0.03;     // automatic hip strategy when near or past the edge

// Delayed PD controller (Peterka 2002).
constexpr double kKp = 1.8;      // in units of (CoP / L) per rad of error
constexpr double kKd = 0.45;     // per rad/s
constexpr double kBaseDelayS = 0.12;
constexpr double kDisturbanceEstimateTauS = 1.5;   // the controller only anticipates slow disturbances

// Stumble and fall.
constexpr double kStumbleAfterS = 0.3;
constexpr double kFallTiltRad = 27.0 * kPi / 180.0;
// Reactive stepping: latency ~0.2 s, lateral (crossover) reach ~0.2 m, forward ~0.3 m (Maki & McIlroy
// 1997, "The role of limb movements in maintaining upright stance: the change-in-support strategy").
constexpr double kStepLatencyS = 0.2;
constexpr double kStepReachLatM = 0.2;
constexpr double kStepReachFwdM = 0.3;
constexpr double kStepMarginM = 0.03;
constexpr double kStepCooldownS = 0.6;
constexpr double kMaxRecoverySteps = 3.0;
constexpr double kRecentStepDecayS = 4.0;
constexpr double kStaggerS = 0.4;
constexpr double kSlipStumbleS = 0.5;

// Gait: step length ~0.7 m at normal speeds.
constexpr double kStepLengthM = 0.7;

// Wind drag on a standing person: Cd ~1.0-1.2 (Penwarden 1973, "Acceptable wind speeds in towns",
// Building Science 8:259-267; people are blown over in gusts above ~20 m/s). Side-on body area ~0.45 m^2;
// a pack adds area roughly with its load.
constexpr double kDragCoefficient = 1.1;
constexpr double kBodySideAreaM2 = 0.45;
constexpr double kPackAreaPerKgM2 = 0.005;
constexpr double kPackAreaMaxM2 = 0.4;
constexpr double kGustTauS = 2.0;           // gust correlation time
constexpr double kGustIntensity = 0.6;      // peak/mean gust factors of ~1.5-2 at gustiness 1

// Friction demand: the required coefficient of friction of level walking is ~0.17-0.22 (Redfern et al.
// 2001, "Biomechanics of slips", Ergonomics 44:1138-1166), rising with load and speed.
constexpr double kGaitRcof = 0.2;

double Sign(double v) { return v > 0.0 ? 1.0 : (v < 0.0 ? -1.0 : 0.0); }

struct Footing {
  double demand = 0.0;
  double friction = 1.0;
  double ratio = 0.0;     // demand / friction; > 1 means slipping
  double slipRisk01 = 0.0;
};

Footing EvaluateFooting(const BalanceInputs& in, double speedMS, double loadRatio) {
  Footing f;
  const double tanAlong = std::tan(DegToRad(Clamp(in.slopeAlongDeg, -80.0, 80.0)));
  const double tanAcross = std::tan(DegToRad(Clamp(in.slopeAcrossDeg, -80.0, 80.0)));
  const double slopeTan = std::sqrt(tanAlong * tanAlong + tanAcross * tanAcross);
  f.demand = slopeTan + kGaitRcof * SmoothStep(0.0, 1.3, speedMS) * (1.0 + 0.3 * Clamp01(loadRatio));
  f.friction = std::max(0.02, SurfaceFriction(in.terrain, in.crampons, in.temperatureC));
  f.ratio = f.demand / f.friction;
  f.slipRisk01 = SmoothStep(0.6, 1.1, f.ratio);
  return f;
}

}  // namespace balance_detail

BalanceModel::BalanceModel(const BodyGeometry& body, std::uint64_t seed) : body_(body), rng_(seed) {
  RecomputeCargo();
}

void BalanceModel::RecomputeCargo() {
  const double bodyMass = std::max(1.0, body_.bodyMassKg);
  double mass = bodyMass;
  Vec3 moment(0.0, 0.0, body_.comHeightM * bodyMass);
  cargoMassKg_ = 0.0;
  unstrappedMassKg_ = 0.0;
  for (const CargoItem& item : cargo_) {
    const double m = std::max(0.0, item.massKg);
    const Vec3 pos(item.offsetM.x, item.offsetM.y, body_.packAttachHeightM + item.offsetM.z);
    moment += pos * m;
    mass += m;
    cargoMassKg_ += m;
    if (!item.strapped) unstrappedMassKg_ += m;
  }
  combinedCoM_ = moment / mass;
}

void BalanceModel::SetCargo(const std::vector<CargoItem>& cargo) {
  // The CoM angle relative to the feet is kept continuous (the body re-poses as the load is shouldered);
  // the controller then drifts toward the new, partially compensated target posture.
  cargo_ = cargo;
  RecomputeCargo();
}

double BalanceModel::CargoMassKg() const { return cargoMassKg_; }

Vec3 BalanceModel::CombinedCoM() const { return combinedCoM_; }

double BalanceModel::CargoLateralBiasRad() const {
  // Positive when the load pulls the CoM to the right (the climber must lean left to compensate).
  return std::atan2(combinedCoM_.x, std::max(0.1, combinedCoM_.z));
}

void BalanceModel::Step(double dtSeconds, const BalanceInputs& in) {
  using namespace balance_detail;
  if (!(dtSeconds > 0.0)) return;
  // Fixed sub-step with an accumulator: identical results for any frame rate whose frames are whole
  // multiples of the sub-step (1/30, 1/60, 1/120 ...), and at most one sub-step of lag otherwise.
  timeAccumulatorS_ += std::min(dtSeconds, kMaxFrameS);
  while (timeAccumulatorS_ >= kSubStepS - 1e-9) {
    SubStep(kSubStepS, in);
    timeAccumulatorS_ -= kSubStepS;
  }
}

void BalanceModel::SubStep(double h, const BalanceInputs& in) {
  using namespace balance_detail;
  if (state_.fallen) return;

  const double fatigue = Clamp01(in.fatigue01);
  const double cognitive = Clamp01(in.cognitiveImpairment01);
  const double lean = Clamp(in.leanInput, -1.0, 1.0);
  const double roughness = Clamp01(in.roughness01);
  const double speed = in.bracing ? std::min(std::max(0.0, in.speedMS), kBraceMaxSpeedMS) : std::max(0.0, in.speedMS);
  const double bodyMass = std::max(1.0, body_.bodyMassKg);
  const double totalMass = bodyMass + cargoMassKg_;

  // --- Geometry ---
  double comHeight = std::max(0.3, combinedCoM_.z);
  if (in.bracing) comHeight *= kBraceComFactor;
  const double w0sq = kGravity / comHeight;       // natural frequency squared, sqrt(g/h)
  const double w0 = std::sqrt(w0sq);
  const double biasLat = CargoLateralBiasRad();
  const double biasFwd = std::atan2(combinedCoM_.y, std::max(0.1, combinedCoM_.z));

  // --- Footing and base of support ---
  const Footing footing = EvaluateFooting(in, speed, cargoMassKg_ / bodyMass);
  const double cosAcross = std::cos(DegToRad(Clamp(in.slopeAcrossDeg, -80.0, 80.0)));
  const double cosAlong = std::cos(DegToRad(Clamp(in.slopeAlongDeg, -80.0, 80.0)));
  const double footingShrink = 1.0 - 0.35 * footing.slipRisk01;
  const double walking = SmoothStep(0.2, 1.5, speed);
  double halfWidth = (std::max(0.0, body_.stanceHalfWidthM) * (1.0 - kWalkingLateralShrink * walking) + kFootHalfWidthM) *
                     cosAcross * footingShrink;
  double front = (kFootFrontM + kWalkingForwardReachM * SmoothStep(0.0, 1.2, speed)) * cosAlong * footingShrink;
  double back = (kFootBackM + kWalkingBackReachM * walking) * cosAlong * footingShrink;
  if (in.bracing) {
    halfWidth *= kBraceWidthFactor;
    front *= kBraceWidthFactor;
    back *= kBraceWidthFactor;
  }
  if (in.usingPoles) {
    halfWidth += kPoleLateralM;
    front += kPoleForwardM;
    back += kPoleForwardM;
  }

  // --- Disturbances (angular accelerations, rad/s^2) ---
  // Slope: gravity component across / along the fall line. The slope is seen and felt, so the
  // controller anticipates it fully (feed-forward); only wind and sway must be estimated.
  const double slopeLat = w0sq * kSlopeCoupling * std::sin(DegToRad(Clamp(in.slopeAcrossDeg, -80.0, 80.0)));
  const double slopeFwd = -w0sq * kSlopeCoupling * std::sin(DegToRad(Clamp(in.slopeAlongDeg, -80.0, 80.0)));
  double distLat = slopeLat;
  double distFwd = slopeFwd;

  // Wind: drag 0.5 rho Cd A v|v|, with gusts as an Ornstein-Uhlenbeck process.
  if (in.gustiness01 > 0.0 && in.lateralWindMS != 0.0) {
    gustState_ += -gustState_ * h / kGustTauS + std::sqrt(2.0 * h / kGustTauS) * rng_.Normal();
  }
  const double windEff = in.lateralWindMS * (1.0 + kGustIntensity * Clamp01(in.gustiness01) * gustState_);
  if (windEff != 0.0) {
    const double packArea = std::min(kPackAreaMaxM2, kPackAreaPerKgM2 * cargoMassKg_);
    const double area = kBodySideAreaM2 + packArea;
    const double rho = in.airDensityKgM3 > 0.0 ? in.airDensityKgM3 : 0.74;
    const double force = 0.5 * rho * kDragCoefficient * area * windEff * std::fabs(windEff);
    // Centre of pressure: ~0.55 of body height for the body, pack at its attachment height.
    const double arm = (kBodySideAreaM2 * 0.55 * body_.heightM + packArea * body_.packAttachHeightM) / area;
    const double braceArm = in.bracing ? kBraceComFactor : 1.0;
    distLat += force * arm * braceArm / (totalMass * comHeight * comHeight);
  }

  // Unstrapped cargo swings at stride frequency, shifting the combined CoM.
  const double stepFreqHz = speed / kStepLengthM;
  if (unstrappedMassKg_ > 0.0) {
    swayPhase_ += 2.0 * kPi * 0.5 * std::max(stepFreqHz, 0.3) * h;
    if (swayPhase_ > 2.0 * kPi) swayPhase_ -= 2.0 * kPi;
    const double swayAmpM = 0.12 * (SmoothStep(0.0, 1.4, speed) + 0.5 * roughness);
    const double shift = unstrappedMassKg_ * swayAmpM / (totalMass * comHeight);
    distLat += w0sq * shift * std::sin(swayPhase_);
    distFwd += w0sq * 0.5 * shift * std::cos(2.0 * swayPhase_);
  }
  const double unstrappedFrac = unstrappedMassKg_ / totalMass;

  // --- Discrete events: footsteps (roughness perturbations) and slips ---
  const double pSlip = SmoothStep(0.9, 1.4, footing.ratio);
  bool slipNow = false;
  stepPhase_ += stepFreqHz * h;
  if (stepPhase_ >= 1.0) {
    stepPhase_ -= std::floor(stepPhase_);
    if (roughness > 0.0) {
      // Uneven ground: each footstep lands slightly off, a random step perturbation scaled by speed.
      state_.lateralVelRadS += rng_.Normal() * roughness * (0.03 + 0.05 * speed) * (1.0 + 2.0 * unstrappedFrac);
      state_.forwardVelRadS += rng_.Normal() * roughness * (0.04 + 0.06 * speed);
    }
    if (pSlip > 0.0 && rng_.Chance(pSlip)) slipNow = true;
  }
  if (speed < 0.1 && pSlip > 0.0 && rng_.Chance(ProbabilityInInterval(0.8 * pSlip, h))) slipNow = true;
  if (slipNow) {
    // The foot skids downhill: lateral kick toward the fall line; going up you pitch forward onto your
    // face, going down your feet shoot out and you sit back.
    const double severity = (0.06 + 0.45 * pSlip) * rng_.Uniform(0.7, 1.3);
    const double dirLat = in.slopeAcrossDeg != 0.0 ? Sign(in.slopeAcrossDeg) : (rng_.Chance(0.5) ? 1.0 : -1.0);
    state_.lateralVelRadS += dirLat * severity;
    state_.forwardVelRadS += (in.slopeAlongDeg >= 0.0 ? 1.0 : -1.0) * 0.6 * severity;
    slipTimerS_ = kSlipStumbleS;
  }

  // --- Automatic postural control (delayed PD through the CoP) ---
  const double delayS = kBaseDelayS + 0.10 * cognitive + 0.05 * fatigue;
  const int delaySteps = std::min(kDelayBufferSize - 1, static_cast<int>(std::lround(delayS / h)));
  delayLat_[delayHead_] = comLatRad_;
  delayLatVel_[delayHead_] = state_.lateralVelRadS;
  delayFwd_[delayHead_] = comFwdRad_;
  delayFwdVel_[delayHead_] = state_.forwardVelRadS;
  const int delayedIdx = (delayHead_ - delaySteps + kDelayBufferSize) % kDelayBufferSize;
  delayHead_ = (delayHead_ + 1) % kDelayBufferSize;
  const double seenLat = delayLat_[delayedIdx];
  const double seenLatVel = delayLatVel_[delayedIdx];
  const double seenFwd = delayFwd_[delayedIdx];
  const double seenFwdVel = delayFwdVel_[delayedIdx];

  const double impairment = (1.0 - 0.2 * fatigue) * (1.0 - 0.25 * cognitive);
  double gain = impairment * (in.bracing ? 1.3 : 1.0) * (state_.stumbling ? 1.2 : 1.0);
  const double kp = kKp * gain;
  const double kd = kKd * gain;

  distEstLat_ = ApproachExp(distEstLat_, distLat - slopeLat, h, kDisturbanceEstimateTauS);
  distEstFwd_ = ApproachExp(distEstFwd_, distFwd - slopeFwd, h, kDisturbanceEstimateTauS);
  const double expectLat = slopeLat + distEstLat_;
  const double expectFwd = slopeFwd + distEstFwd_;

  const double adapt = (1.0 - 0.5 * fatigue) * (1.0 - 0.5 * cognitive);
  const double cargoCompLat = in.bracing ? kBraceAutoComp : kAutoCargoCompLat * adapt;
  const double cargoCompFwd = in.bracing ? kBraceAutoComp : kAutoCargoCompFwd * adapt;
  const double distComp = in.bracing ? kBraceAutoComp : kAutoDisturbanceComp * adapt;

  const double playerAccel = lean * kPlayerShiftAuthority * w0sq;
  const double targetLat = (1.0 - cargoCompLat) * biasLat + lean * kMaxVoluntaryLeanRad -
                           distComp * std::asin(Clamp(expectLat / w0sq, -0.5, 0.5));
  const double targetFwd = (1.0 - cargoCompFwd) * biasFwd - distComp * std::asin(Clamp(expectFwd / w0sq, -0.5, 0.5));

  const double copLatDes = comHeight * (std::sin(targetLat) + (expectLat + playerAccel) / w0sq +
                                        kp * (seenLat - targetLat) + kd * seenLatVel);
  const double copFwdDes = comHeight * (std::sin(targetFwd) + expectFwd / w0sq + kp * (seenFwd - targetFwd) + kd * seenFwdVel);
  const double copLat = Clamp(copLatDes, -halfWidth, halfWidth);
  const double copFwd = Clamp(copFwdDes, -back, front);

  // Effective ("gravito-inertial") CoM projection: where gravity plus external forces through the CoM
  // meet the ground; equals the CoP required to hold the current posture.
  auto effectiveLat = [&]() { return comHeight * (std::sin(comLatRad_) + (distLat + playerAccel) / w0sq); };
  auto effectiveFwd = [&]() { return comHeight * (std::sin(comFwdRad_) + distFwd / w0sq); };

  // Automatic hip strategy kicks in once the CoP is saturated at the edge.
  const double xLat0 = effectiveLat();
  const double xFwd0 = effectiveFwd();
  const double hipLat = std::fabs(xLat0) > halfWidth ? -Sign(xLat0) * kAutoHipAuthority * w0sq * impairment : 0.0;
  const double hipFwd = (xFwd0 > front || xFwd0 < -back) ? -Sign(xFwd0) * kAutoHipAuthority * w0sq * impairment : 0.0;

  // --- Integrate (semi-implicit Euler) ---
  const double accLat = w0sq * (std::sin(comLatRad_) - copLat / comHeight) + distLat + playerAccel + hipLat;
  const double accFwd = w0sq * (std::sin(comFwdRad_) - copFwd / comHeight) + distFwd + hipFwd;
  state_.lateralVelRadS += accLat * h;
  state_.forwardVelRadS += accFwd * h;
  comLatRad_ += state_.lateralVelRadS * h;
  comFwdRad_ += state_.forwardVelRadS * h;

  // --- Support check, stumble, recovery step, fall ---
  double xLat = effectiveLat();
  double xFwd = effectiveFwd();
  const bool outLat = std::fabs(xLat) > halfWidth;
  const bool outFwd = xFwd > front || xFwd < -back;
  if (outLat || outFwd) {
    state_.secondsOutsideSupport += h;
  } else {
    state_.secondsOutsideSupport = std::max(0.0, state_.secondsOutsideSupport - h);
  }
  stepCooldownS_ = std::max(0.0, stepCooldownS_ - h);
  slipTimerS_ = std::max(0.0, slipTimerS_ - h);
  recentSteps_ *= std::exp(-h / kRecentStepDecayS);

  // Recovery step (stepping strategy): when the extrapolated CoM leaves the base of support, plant a
  // foot just beyond it (Hof 2005 foot-placement rule: CoP at XcoM plus a small margin). Reach is limited
  // and shrinks with fatigue, hypoxia and poor footing; repeated steps in a few seconds exhaust it
  // (a stagger that cannot be stopped ends in a fall).
  {
    const double xcLat = xLat + comHeight * std::cos(comLatRad_) * state_.lateralVelRadS / w0;
    const double xcFwd = xFwd + comHeight * std::cos(comFwdRad_) * state_.forwardVelRadS / w0;
    const bool xcOutLat = std::fabs(xcLat) > halfWidth;
    const bool xcOutFwd = xcFwd > front || xcFwd < -back;
    xcomOutsideS_ = (xcOutLat || xcOutFwd) ? xcomOutsideS_ + h : 0.0;
    if (xcomOutsideS_ > kStepLatencyS && stepCooldownS_ <= 0.0 && recentSteps_ < kMaxRecoverySteps && !slipNow) {
      const double ability = (1.0 - 0.5 * fatigue) * (1.0 - 0.3 * cognitive) * (1.0 - 0.5 * footing.slipRisk01);
      if (xcOutLat) {
        const double excess = std::fabs(xcLat) - halfWidth + kStepMarginM;
        const double shift = Sign(xcLat) * std::min(kStepReachLatM * ability, excess);
        comLatRad_ = std::asin(Clamp(std::sin(comLatRad_) - shift / comHeight, -0.99, 0.99));
      }
      if (xcOutFwd) {
        const double excess = (xcFwd > front ? xcFwd - front : -back - xcFwd) + kStepMarginM;
        const double shift = Sign(xcFwd) * std::min(kStepReachFwdM * ability, excess);
        comFwdRad_ = std::asin(Clamp(std::sin(comFwdRad_) - shift / comHeight, -0.99, 0.99));
      }
      stepCooldownS_ = kStepCooldownS;
      recentSteps_ += 1.0;
      xcomOutsideS_ = 0.0;
      slipTimerS_ = std::max(slipTimerS_, kStaggerS);   // a recovery step reads as a stumble
      xLat = effectiveLat();
      xFwd = effectiveFwd();
    }
  }

  state_.stumbling = state_.secondsOutsideSupport > kStumbleAfterS || slipTimerS_ > 0.0;
  const double recoveryWindowS = 1.2 - 0.4 * fatigue - 0.1 * cognitive;
  if (state_.secondsOutsideSupport > recoveryWindowS || std::fabs(comLatRad_) > kFallTiltRad ||
      std::fabs(comFwdRad_) > kFallTiltRad) {
    state_.fallen = true;
    state_.stumbling = false;
  }

  // --- Outputs ---
  state_.lateralTiltRad = comLatRad_ - biasLat;   // body (trunk) lean
  state_.forwardTiltRad = comFwdRad_ - biasFwd;
  state_.slipRisk01 = footing.slipRisk01;
  // Stability from the extrapolated-CoM margin (Hof 2005): XcoM = x + v / w0.
  const double xcomLat = xLat + comHeight * std::cos(comLatRad_) * state_.lateralVelRadS / w0;
  const double xcomFwd = xFwd + comHeight * std::cos(comFwdRad_) * state_.forwardVelRadS / w0;
  const double marginLat = (halfWidth - std::fabs(xcomLat)) / halfWidth;
  const double marginFwd = xcomFwd >= 0.0 ? (front - xcomFwd) / front : (back + xcomFwd) / back;
  // Both axes' margins count (a product, so any loss of margin shows on the gauge).
  double target = Clamp01(marginLat) * Clamp01(marginFwd) * (1.0 - 0.4 * footing.slipRisk01);
  if (state_.stumbling) target = std::min(target, 0.35);
  if (state_.fallen) target = 0.0;
  state_.stability01 = state_.fallen ? 0.0 : ApproachExp(state_.stability01, target, h, 0.08);
}

void BalanceModel::Recover() {
  // Back on your feet, CoM centred over the feet, body leaning to hold the load. The RNG keeps its
  // sequence so replays stay deterministic.
  state_ = BalanceState{};
  comLatRad_ = 0.0;
  comFwdRad_ = 0.0;
  gustState_ = 0.0;
  stepPhase_ = 0.0;
  slipTimerS_ = 0.0;
  stepCooldownS_ = 0.0;
  xcomOutsideS_ = 0.0;
  recentSteps_ = 0.0;
  distEstLat_ = 0.0;
  distEstFwd_ = 0.0;
  delayLat_.fill(0.0);
  delayLatVel_.fill(0.0);
  delayFwd_.fill(0.0);
  delayFwdVel_.fill(0.0);
  state_.lateralTiltRad = -CargoLateralBiasRad();
  state_.forwardTiltRad = -std::atan2(combinedCoM_.y, std::max(0.1, combinedCoM_.z));
}

double BalanceModel::SafeSpeedMS(const BalanceInputs& in) const {
  using namespace balance_detail;
  if (state_.fallen) return 0.0;
  // Tobler (1993) hiking function: v = 6 exp(-3.5 |tan θ + 0.05|) km/h, ~1.4 m/s on the flat, fastest
  // on a slight descent. Normalised to a 1.4 m/s preferred walking speed.
  const double tanAlong = std::tan(DegToRad(Clamp(in.slopeAlongDeg, -80.0, 80.0)));
  double v = 1.4 * std::exp(-3.5 * (std::fabs(tanAlong + 0.05) - 0.05));

  const double bodyMass = std::max(1.0, body_.bodyMassKg);
  const Footing footing = EvaluateFooting(in, std::min(v, 1.4), cargoMassKg_ / bodyMass);
  v *= 1.0 - 0.75 * footing.slipRisk01;                        // bad footing
  const double tanAcross = std::tan(DegToRad(Clamp(in.slopeAcrossDeg, -80.0, 80.0)));
  const double slopeDeg = RadToDeg(std::atan(std::sqrt(tanAlong * tanAlong + tanAcross * tanAcross)));
  if (slopeDeg > MaxWalkableSlopeDeg(in.terrain, in.crampons)) v *= 0.25;   // hands and tools needed
  v *= 1.0 - 0.5 * SmoothStep(10.0, 35.0, std::fabs(in.slopeAcrossDeg));      // traversing
  v *= 1.0 - 0.6 * LoadPenalty01(cargoMassKg_, bodyMass);                     // Knapik 2004
  // Walking becomes difficult above ~15 m/s and dangerous above ~20 m/s (Penwarden 1973; Beaufort).
  const double gustWind = std::fabs(in.lateralWindMS) * (1.0 + 0.5 * Clamp01(in.gustiness01));
  v *= 1.0 - 0.7 * SmoothStep(10.0, 25.0, gustWind);
  v *= 1.0 - 0.5 * Clamp01(in.fatigue01);
  v *= 1.0 - 0.3 * Clamp01(in.cognitiveImpairment01);
  v *= 1.0 - 0.5 * Clamp01(in.roughness01);
  if (unstrappedMassKg_ > 0.0) v *= 0.85;
  if (state_.stumbling) v *= 0.5;
  if (in.bracing) v = std::min(v, kBraceMaxSpeedMS);
  return std::max(0.05, v);
}

double LoadPenalty01(double loadKg, double bodyMassKg) {
  // Knapik et al. 2004: little penalty below ~20 % of body mass, sharply rising injury, stumble and
  // energy costs beyond 30-35 %, and loads of 60-70 % are near the practical limit for sustained travel.
  if (bodyMassKg <= 0.0) return 1.0;
  return SmoothStep(0.2, 0.65, std::max(0.0, loadKg) / bodyMassKg);
}

}  // namespace summit::sim
