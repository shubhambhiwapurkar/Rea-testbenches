#include "Sim/Rope.h"
#include "TestFramework.h"

using namespace summit::sim;

namespace rope_tests {

Anchor MakeAnchor(AnchorType type, double placement = 0.9, double ageDays = 0.0) {
  Anchor a;
  a.type = type;
  a.ratedStrengthKN = NominalAnchorStrengthKN(type);
  a.placementQuality01 = placement;
  a.ageDays = ageDays;
  return a;
}

double FailureRate(const FallScenario& sc, const RopeSpec& rope, const Anchor& anchor, double medium,
                   double tempC, int trials, std::uint64_t seed) {
  Pcg32 rng(seed);
  int failures = 0;
  for (int i = 0; i < trials; ++i) {
    if (ResolveFall(sc, rope, anchor, medium, tempC, rng).anchorFailed) ++failures;
  }
  return static_cast<double>(failures) / trials;
}

double MeanInjury(const FallScenario& sc, const RopeSpec& rope, const Anchor& anchor, int trials) {
  Pcg32 rng(99);
  double sum = 0.0;
  for (int i = 0; i < trials; ++i) sum += ResolveFall(sc, rope, anchor, 1.0, -5.0, rng).injurySeverity01;
  return sum / trials;
}

}  // namespace rope_tests

using namespace rope_tests;

TEST_CASE("Rope: fall factor is fall / rope out, capped at 2") {
  CHECK_NEAR(FallFactor(4.0, 10.0), 0.4, 1e-12);
  CHECK_NEAR(FallFactor(10.0, 5.0), 2.0, 1e-12);   // physical lead-fall maximum
  CHECK_NEAR(FallFactor(3.0, 0.0), 2.0, 1e-12);    // no rope out -> worst case
  CHECK_NEAR(FallFactor(-1.0, 5.0), 0.0, 1e-12);
}

TEST_CASE("Rope: stiffness inverted from the UIAA rated impact force") {
  RopeSpec rope;   // 8.8 kN
  const double k = RopeStiffnessN(rope);
  CHECK_RANGE(k, 18000.0, 28000.0);   // dynamic ropes: E*A ~ 20 kN
  RopeSpec soft = rope;
  soft.ratedImpactForceKN = 7.5;
  CHECK(RopeStiffnessN(soft) < k);
  RopeSpec worn = rope;
  worn.wear01 = 1.0;
  CHECK(RopeStiffnessN(worn) > k * 1.2);   // worn ropes absorb less energy
  RopeSpec staticRope = rope;
  staticRope.dynamic = false;
  CHECK_RANGE(RopeStiffnessN(staticRope) / k, 1.5, 3.0);
}

TEST_CASE("Rope: peak force reproduces the rated impact force at the UIAA standard drop") {
  RopeSpec rope;
  CHECK_NEAR(PeakImpactForceKN(80.0, 1.77, rope), 8.8, 0.05);
  RopeSpec r95 = rope;
  r95.ratedImpactForceKN = 9.5;
  CHECK_NEAR(PeakImpactForceKN(80.0, 1.77, r95), 9.5, 0.05);
}

TEST_CASE("Rope: peak force is monotonic in fall factor and mass") {
  RopeSpec rope;
  double prev = 0.0;
  for (double f = 0.0; f <= 2.0001; f += 0.1) {
    const double force = PeakImpactForceKN(80.0, f, rope);
    CHECK(force > prev);
    prev = force;
  }
  // A small fall factor is much gentler (FF 0.3 is roughly half of FF 1.77).
  CHECK(PeakImpactForceKN(80.0, 0.3, rope) < 0.6 * PeakImpactForceKN(80.0, 1.77, rope));
  CHECK_RANGE(PeakImpactForceKN(80.0, 0.3, rope), 3.0, 5.5);
  // Heavier climber (body + pack) -> higher force.
  CHECK(PeakImpactForceKN(100.0, 1.0, rope) > PeakImpactForceKN(80.0, 1.0, rope));
  CHECK(PeakImpactForceKN(60.0, 1.0, rope) < PeakImpactForceKN(80.0, 1.0, rope));
  CHECK_NEAR(PeakImpactForceKN(0.0, 1.0, rope), 0.0, 1e-12);
}

TEST_CASE("Rope: dynamic belay lowers peak force by 30-40 %") {
  RopeSpec rope;
  const double locked = PeakImpactForceKN(80.0, 1.0, rope, 0.0);
  const double soft = PeakImpactForceKN(80.0, 1.0, rope, 1.0);
  CHECK_RANGE(1.0 - soft / locked, 0.30, 0.40);
  CHECK(PeakImpactForceKN(80.0, 1.0, rope, 0.5) < locked);
}

TEST_CASE("Rope: static rope matches EN 1891 and shock-loads hard at high fall factors") {
  RopeSpec staticRope;
  staticRope.dynamic = false;
  CHECK_RANGE(PeakImpactForceKN(100.0, 0.3, staticRope), 5.0, 7.0);  // EN 1891 type A: <= ~6 kN
  CHECK_RANGE(PeakImpactForceKN(80.0, 1.0, staticRope), 8.5, 11.0);
  CHECK(PeakImpactForceKN(80.0, 2.0, staticRope) > 12.0);
  RopeSpec dynamicRope;
  CHECK(PeakImpactForceKN(80.0, 1.0, staticRope) > PeakImpactForceKN(80.0, 1.0, dynamicRope) + 2.0);
  RopeSpec rope;
  CHECK(PeakImpactForceKN(80.0, 1.0, rope) < 9.0);
}

TEST_CASE("Rope: wear raises the effective impact force") {
  RopeSpec rope;
  RopeSpec worn = rope;
  worn.wear01 = 0.8;
  CHECK(PeakImpactForceKN(80.0, 1.0, worn) > PeakImpactForceKN(80.0, 1.0, rope));
}

TEST_CASE("Anchor: ice screws hold 10-20 kN in good ice, little in warm rotten ice") {
  const Anchor screw = MakeAnchor(AnchorType::IceScrew, 1.0);
  const double good = AnchorEffectiveStrengthKN(screw, 1.0, -8.0);
  CHECK_RANGE(good, 10.0, 20.0);
  const double rotten = AnchorEffectiveStrengthKN(screw, 0.3, 0.0);
  CHECK_RANGE(rotten, 2.0, 6.0);
  // Very cold ice is brittle, warm ice is soft: both are weaker than ideal -8 °C ice.
  Anchor strongScrew = screw;
  strongScrew.ratedStrengthKN = 20.0;   // remove the hardware cap to see the ice effect
  const double ideal = AnchorEffectiveStrengthKN(strongScrew, 1.0, -8.0);
  CHECK(AnchorEffectiveStrengthKN(strongScrew, 1.0, -30.0) < ideal);
  CHECK(AnchorEffectiveStrengthKN(strongScrew, 1.0, 0.0) < ideal);
}

TEST_CASE("Anchor: snow pickets 2-8 kN, T-slot beats vertical, hard beats soft snow") {
  const Anchor vertical = MakeAnchor(AnchorType::SnowPicket, 0.5);
  const Anchor tslot = MakeAnchor(AnchorType::SnowPicket, 1.0);
  const double hardT = AnchorEffectiveStrengthKN(tslot, 1.0, -8.0);
  const double hardV = AnchorEffectiveStrengthKN(vertical, 1.0, -8.0);
  const double softV = AnchorEffectiveStrengthKN(vertical, 0.0, -8.0);
  CHECK_RANGE(hardT, 5.0, 8.5);
  CHECK_RANGE(hardV, 3.0, 6.0);
  CHECK_RANGE(softV, 0.5, 3.0);
  CHECK(hardT > hardV);
  CHECK(hardV > softV);
  // Wet, warm snow weakens the anchor.
  CHECK(AnchorEffectiveStrengthKN(tslot, 1.0, 1.0) < hardT);
}

TEST_CASE("Anchor: bolts 20-25 kN and placement quality matters for gear") {
  const double bolt = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::Bolt, 1.0), 1.0, -5.0);
  CHECK_RANGE(bolt, 20.0, 25.5);
  const double camGood = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::Cam, 1.0), 1.0, -5.0);
  const double camPoor = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::Cam, 0.2), 1.0, -5.0);
  CHECK_RANGE(camGood, 10.0, 16.0);
  CHECK(camPoor < 0.5 * camGood);
  // Poor medium (friable rock) lowers strength.
  CHECK(AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::Cam, 1.0), 0.0, -5.0) < camGood);
}

TEST_CASE("Anchor: old fixed lines degrade over days to weeks") {
  const double fresh = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::FixedLine, 0.8, 0.0), 0.8, -10.0);
  const double week = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::FixedLine, 0.8, 7.0), 0.8, -10.0);
  const double month = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::FixedLine, 0.8, 30.0), 0.8, -10.0);
  CHECK(week < fresh);
  CHECK(month < 0.5 * fresh);
  // Warm weather melts anchors out faster.
  const double monthWarm = AnchorEffectiveStrengthKN(MakeAnchor(AnchorType::FixedLine, 0.8, 30.0), 0.8, 0.0);
  CHECK(monthWarm < month);
}

TEST_CASE("ResolveFall: geometry, fall factor and pulley effect") {
  FallScenario sc;   // 2 m above pro, 10 m out, 0.5 m slack, 70°
  RopeSpec rope;
  const Anchor bolt = MakeAnchor(AnchorType::Bolt, 1.0);
  Pcg32 rng(1);
  const FallOutcome out = ResolveFall(sc, rope, bolt, 1.0, -5.0, rng);
  CHECK(!out.anchorFailed);
  CHECK_NEAR(out.fallFactor, 0.45, 1e-9);
  CHECK(out.fallDistanceM > 4.5);   // includes rope stretch
  CHECK(out.fallDistanceM < 8.0);
  CHECK_RANGE(out.peakForceKN, 2.5, 6.0);
  CHECK_RANGE(out.injurySeverity01, 0.0, 0.6);
}

TEST_CASE("ResolveFall: anchor failure is probabilistic and grows with load") {
  RopeSpec rope;
  const Anchor screw = MakeAnchor(AnchorType::IceScrew, 0.7);
  FallScenario small;
  small.distanceAboveProtectionM = 0.5;
  small.ropeOutM = 20.0;
  small.slackM = 0.2;
  small.slopeAngleDeg = 85.0;
  FallScenario big = small;
  big.distanceAboveProtectionM = 4.0;
  big.ropeOutM = 5.0;
  big.belayDynamic01 = 0.0;
  // Medium-quality ice: effective ~6-8 kN, so the big fall (top piece load > 10 kN) often rips it.
  const double pSmall = FailureRate(small, rope, screw, 0.6, -6.0, 400, 11);
  const double pBig = FailureRate(big, rope, screw, 0.6, -6.0, 400, 11);
  CHECK(pSmall < 0.1);
  CHECK(pBig > pSmall + 0.3);
  // Bolts essentially never fail in an ordinary fall.
  CHECK(FailureRate(big, rope, MakeAnchor(AnchorType::Bolt, 1.0), 1.0, -6.0, 400, 5) < 0.01);
  // Rotten, warm ice almost always fails under a hard fall.
  CHECK(FailureRate(big, rope, screw, 0.1, 0.5, 400, 7) > 0.8);
}

TEST_CASE("ResolveFall: anchor failure lengthens the fall onto the belay") {
  RopeSpec rope;
  Anchor junk = MakeAnchor(AnchorType::SnowPicket, 0.0);
  FallScenario sc;
  sc.distanceAboveProtectionM = 3.0;
  sc.ropeOutM = 8.0;
  Pcg32 rng(3);
  const FallOutcome out = ResolveFall(sc, rope, junk, 0.0, 2.0, rng);
  CHECK(out.anchorFailed);
  CHECK(out.fallDistanceM > 2.0 * sc.ropeOutM);
  CHECK(out.fallFactor > 1.5);
  CHECK(out.injurySeverity01 > 0.4);
}

TEST_CASE("ResolveFall: deterministic for the same seed") {
  FallScenario sc;
  sc.distanceAboveProtectionM = 3.0;
  RopeSpec rope;
  const Anchor screw = MakeAnchor(AnchorType::IceScrew, 0.6);
  Pcg32 a(42), b(42);
  for (int i = 0; i < 50; ++i) {
    const FallOutcome x = ResolveFall(sc, rope, screw, 0.5, -3.0, a);
    const FallOutcome y = ResolveFall(sc, rope, screw, 0.5, -3.0, b);
    CHECK(x.anchorFailed == y.anchorFailed);
    CHECK(x.ropeCut == y.ropeCut);
    CHECK(x.injurySeverity01 == y.injurySeverity01);
  }
}

TEST_CASE("ResolveFall: sharp edges cut ropes under high load, and cuts are severe") {
  RopeSpec rope;
  const Anchor bolt = MakeAnchor(AnchorType::Bolt, 1.0);
  FallScenario sc;
  sc.distanceAboveProtectionM = 5.0;
  sc.ropeOutM = 6.0;
  sc.slopeAngleDeg = 90.0;
  sc.belayDynamic01 = 0.0;
  sc.sharpEdge = true;
  Pcg32 rng(8);
  int cuts = 0;
  double cutInjury = 0.0;
  for (int i = 0; i < 200; ++i) {
    const FallOutcome o = ResolveFall(sc, rope, bolt, 1.0, -5.0, rng);
    if (o.ropeCut) {
      ++cuts;
      cutInjury += o.injurySeverity01;
    }
  }
  CHECK(cuts > 120);
  if (cuts > 0) CHECK(cutInjury / cuts > 0.8);
  // No edge, new rope: no cuts.
  sc.sharpEdge = false;
  Pcg32 rng2(8);
  int cleanCuts = 0;
  for (int i = 0; i < 200; ++i) cleanCuts += ResolveFall(sc, rope, bolt, 1.0, -5.0, rng2).ropeCut ? 1 : 0;
  CHECK(cleanCuts == 0);
}

TEST_CASE("ResolveFall: static line shock-loads and tumbling terrain injure more") {
  RopeSpec dyn;
  RopeSpec stat;
  stat.dynamic = false;
  const Anchor bolt = MakeAnchor(AnchorType::Bolt, 1.0);
  FallScenario sc;
  sc.distanceAboveProtectionM = 3.0;
  sc.ropeOutM = 3.5;  // near fall factor 2: e.g. falling past a fixed-line anchor on a short lanyard
  sc.slopeAngleDeg = 90.0;
  sc.belayDynamic01 = 0.0;
  CHECK(MeanInjury(sc, stat, bolt, 100) > MeanInjury(sc, dyn, bolt, 100) + 0.1);
  FallScenario tumble = sc;
  tumble.slopeAngleDeg = 60.0;
  CHECK(MeanInjury(tumble, dyn, bolt, 100) > MeanInjury(sc, dyn, bolt, 100));
}

TEST_CASE("Unprotected falls: long falls on steep ground are fatal") {
  CHECK(UnprotectedFallInjury01(30.0, 60.0) > 0.9);
  CHECK(UnprotectedFallInjury01(5.0, 20.0) < 0.2);
  CHECK(UnprotectedFallInjury01(20.0, 70.0) > UnprotectedFallInjury01(20.0, 30.0));
  CHECK(UnprotectedFallInjury01(10.0, 60.0) < UnprotectedFallInjury01(25.0, 60.0));
}

TEST_CASE("Self-arrest: easy on soft moderate snow, hopeless on steep hard snow") {
  CHECK(SelfArrestSuccessProbability(25.0, false, 0.8, 0.5, 0.0) > 0.75);
  CHECK(SelfArrestSuccessProbability(45.0, true, 0.8, 0.5, 0.0) < 0.1);
  CHECK(SelfArrestSuccessProbability(30.0, true, 0.8, 0.5, 0.0) >
        SelfArrestSuccessProbability(40.0, true, 0.8, 0.5, 0.0));
  // Slow reaction lets speed build up.
  CHECK(SelfArrestSuccessProbability(30.0, true, 0.8, 2.0, 0.0) <
        0.5 * SelfArrestSuccessProbability(30.0, true, 0.8, 0.5, 0.0));
  // Skill and fatigue.
  CHECK(SelfArrestSuccessProbability(30.0, false, 0.1, 0.8, 0.0) <
        SelfArrestSuccessProbability(30.0, false, 0.9, 0.8, 0.0));
  CHECK(SelfArrestSuccessProbability(30.0, false, 0.7, 0.8, 0.9) <
        SelfArrestSuccessProbability(30.0, false, 0.7, 0.8, 0.0));
  // Hard snow is worse than soft at the same angle.
  CHECK(SelfArrestSuccessProbability(35.0, true, 0.7, 0.8, 0.2) <
        SelfArrestSuccessProbability(35.0, false, 0.7, 0.8, 0.2));
  const double p = SelfArrestSuccessProbability(35.0, true, 0.5, 1.0, 0.5);
  CHECK_RANGE(p, 0.0, 1.0);
}
