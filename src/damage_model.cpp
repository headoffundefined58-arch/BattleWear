// BattleWear - logic layer: physically-motivated, monotone damage accumulation.
//
// Model summary
//   I(t)  : leaky-integrated combat intensity,  dI = E/(norm*tau) per hit, decay exp(-dt/tau).
//           Frame-rate independent: evaluated analytically at each hit's own timestamp.
//   wear  : saturating surface abrasion, dW ~ Ew * (1-wear)^p. Diminishing returns.
//   tear  : Paris-like crack growth, dT ~ Et^m * (tear+nucleus)^q / integrity * (1-tear).
//           Nucleates only after fabric weakening (wear) or a sharp stress concentrator.
//   hole  : local rupture, requires a pre-existing tear + a per-hit rupture drive
//           (puncture energy + cyclic loading at the crack tip). Growth saturates at a hard cap.
//           Holes are backed by an underlayer in the visual layer - never skin.
//   stage : area-weighted aggregate with hysteresis + dwell debounce (no flicker).
//
// All writes go through Monotone(): damage never decreases inside one battle.
#include "battlewear/battlewear.h"

#include <algorithm>

namespace bw {

DamageModel::DamageModel(const Config& cfg) : cfg_(cfg) {
    if (cfg_.panelCount <= 0) {
        Config d = Config::Defaults();
        std::string err;
        (void)err;
        cfg_ = d;
    }
    effectiveHoleCap_ = std::min(cfg_.constraints.maxHoleRatioPerPanel,
                                 cfg_.constraints.maxTotalLossRatio);
    if (!(effectiveHoleCap_ > 0.0)) effectiveHoleCap_ = 0.18;
    Reset();
}

void DamageModel::Reset(uint64_t seed) {
    s_ = DamageState();
    s_.panelCount = static_cast<uint32_t>(cfg_.panelCount);
    s_.battleSeed = seed ? seed : 0x5EED1234ULL;
    s_.battleActive = true;
    s_.pendingStage = 0;
    for (int i = 0; i < cfg_.panelCount; ++i) {
        s_.panels[i].integrity = 1.0;
        s_.panels[i].lastHoleTime = kNoHoleTime;
    }
}

void DamageModel::DecayIntensityTo(double t) {
    double dt = t - s_.lastIntensityTime;
    if (!(dt > 0.0)) { s_.lastIntensityTime = t; return; }
    double tau = std::max(0.01, cfg_.sim.intensityTauSeconds);
    s_.intensity = Clamp(s_.intensity * std::exp(-dt / tau), 0.0, 1.0);
    s_.lastIntensityTime = t;
}

double DamageModel::WearMean() const {
    double m = 0.0;
    for (int i = 0; i < cfg_.panelCount; ++i) m += cfg_.panels[i].area * s_.panels[i].wear;
    return m;
}
double DamageModel::TearMean() const {
    double m = 0.0;
    for (int i = 0; i < cfg_.panelCount; ++i) m += cfg_.panels[i].area * s_.panels[i].tear;
    return m;
}
double DamageModel::HoleMean() const {
    double m = 0.0;
    for (int i = 0; i < cfg_.panelCount; ++i) m += cfg_.panels[i].area * s_.panels[i].hole;
    return m;
}
uint32_t DamageModel::TearPanelCount() const {
    uint32_t n = 0;
    for (int i = 0; i < cfg_.panelCount; ++i) if (s_.panels[i].tear >= 0.20) ++n;
    return n;
}
uint32_t DamageModel::HoleCount() const {
    uint32_t n = 0;
    for (int i = 0; i < cfg_.panelCount; ++i) n += s_.panels[i].holeCount;
    return n;
}
double DamageModel::TotalLossRatio() const {
    double t = 0.0;
    for (int i = 0; i < cfg_.panelCount; ++i)
        t += cfg_.panels[i].area * s_.panels[i].hole;
    return t;
}
double DamageModel::CoverageRatio() const { return 1.0 - TotalLossRatio(); }

static double NeighborHoleFraction(const DamageState& s, const Config& cfg, int idx) {
    const PanelDef& p = cfg.panels[idx];
    if (p.neighborCount == 0) return 0.0;
    int n = 0;
    for (int j = 0; j < p.neighborCount; ++j) {
        int q = p.neighbors[j];
        if (q >= 0 && q < cfg.panelCount && s.panels[q].holeCount > 0) ++n;
    }
    return static_cast<double>(n) / p.neighborCount;
}

void DamageModel::DistributeHoleSites(PanelState& p, int panelIndex) {
    for (uint32_t k = 0; k < p.holeCount && k < kMaxHoles; ++k) {
        HoleSite& site = p.holes[k];
        double ratio = Clamp(p.hole / effectiveHoleCap_, 0.0, 1.0);
        double jitter = 0.60 + 0.40 * Rand01(s_.battleSeed, static_cast<uint32_t>(panelIndex), k, 7);
        site.radius = Clamp(cfg_.visual.radiusMin +
                            ratio * (cfg_.visual.radiusMax - cfg_.visual.radiusMin) * jitter,
                            cfg_.visual.radiusMin, cfg_.visual.radiusMax);
    }
}

void DamageModel::EnsureHoleSites(PanelState& p, int panelIndex, double time) {
    if (p.holeCount == 0) return;
    const int allowed = std::min(static_cast<int>(kMaxHoles), cfg_.constraints.maxHolesPerPanel);
    double step = effectiveHoleCap_ / static_cast<double>(std::max(1, allowed));
    int desired = static_cast<int>(p.hole / step) + 1;
    desired = static_cast<int>(Clamp(desired, 1, allowed));
    while (p.holeCount < static_cast<uint32_t>(desired)) {
        if (time - p.lastHoleTime < cfg_.sim.holeCooldownSeconds) break;
        uint32_t k = p.holeCount;
        // Deterministic rejection sampling: keep holes visually separated and inside the panel.
        double bestU = 0.5, bestV = 0.5, bestDist = -1.0;
        for (int attempt = 0; attempt < 24; ++attempt) {
            double u = 0.12 + 0.76 * Rand01(s_.battleSeed, static_cast<uint32_t>(panelIndex), k * 32u + attempt, 1);
            double v = 0.12 + 0.76 * Rand01(s_.battleSeed, static_cast<uint32_t>(panelIndex), k * 32u + attempt, 2);
            double minDist = 1.0;
            for (uint32_t j = 0; j < p.holeCount; ++j) {
                double du = u - p.holes[j].u, dv = v - p.holes[j].v;
                minDist = std::min(minDist, std::sqrt(du * du + dv * dv));
            }
            if (minDist > bestDist) { bestDist = minDist; bestU = u; bestV = v; }
            if (minDist >= cfg_.visual.minUVSpacing) break;
        }
        HoleSite& site = p.holes[k];
        site.u = bestU;
        site.v = bestV;
        site.bornTick = s_.tickIndex;
        p.lastHoleTime = time;
        ++p.holeCount;
        ++s_.totalHoleSites;
    }
    DistributeHoleSites(p, panelIndex);
}

void DamageModel::ApplyToPanel(int idx, double time, double E, const HitProfile& hp,
                               double localIntensity, bool /*sharp*/) {
    if (idx < 0 || idx >= cfg_.panelCount || !(E > 0.0)) return;
    PanelState& p = s_.panels[idx];

    const Simulation& k = cfg_.sim;
    double Ew = E * hp.wear;
    double Et = E * hp.tear;
    double Eh = E * hp.hole;

    // Remaining fabric strength: damaged cloth carries load worse -> faster crack growth.
    double G = Clamp(1.0 - 0.45 * p.wear - 0.35 * p.tear - 0.55 * p.hole,
                     std::max(0.05, k.minIntegrity), 1.0);
    p.integrity = G;

    // --- channel 1: surface abrasion (saturating, diminishing returns) ---
    double dW = k.wearGain * Ew * SafePow(std::max(0.0, 1.0 - p.wear), k.wearSaturationPower);
    p.wear = Monotone(p.wear, Clamp(p.wear + dW, 0.0, 1.0));

    // --- channel 2: edge tear (Paris-like crack-tip propagation) ---
    bool nucleatesTear = (p.wear >= k.tearOnsetWear) ||
                         (hp.sharp && Et >= k.sharpNucleationEnergy);
    if (nucleatesTear) {
        double crackTip = SafePow(p.tear + k.tearNucleus, k.tearCrackExponent);
        double dT = k.tearGain * SafePow(Et, k.tearEnergyExponent) * crackTip /
                    G * std::max(0.0, 1.0 - p.tear);
        p.tear = Monotone(p.tear, Clamp(p.tear + dT, 0.0, 1.0));
    }

    // --- channel 3: local rupture (hole) ---
    // Global budget: total fabric loss can never exceed maxTotalLossRatio (areas sum to 1).
    double lossElsewhere = 0.0;
    for (int j = 0; j < cfg_.panelCount; ++j)
        if (j != idx) lossElsewhere += cfg_.panels[j].area * s_.panels[j].hole;
    double globalRoom = cfg_.constraints.maxTotalLossRatio - lossElsewhere;
    double areaHere = std::max(1.0e-6, cfg_.panels[idx].area);
    double capHere = std::min(effectiveHoleCap_, std::max(0.0, globalRoom) / areaHere);
    if (!(capHere > p.hole)) capHere = p.hole;   // never shrink -> strict monotonicity

    double coherence = 1.0 - k.holeSiteCoherenceGain * localIntensity *
                       NeighborHoleFraction(s_, cfg_, idx);
    double fatigueRelief = 1.0 - 0.55 * localIntensity;   // cyclic loading at the crack tip
    double need = k.holeNucleationEnergy * std::max(0.20, coherence) * std::max(0.20, fatigueRelief);
    double cyclicDrive = 0.35 * E * localIntensity * p.tear;
    double drive = Eh + cyclicDrive;

    bool canRupture = (p.tear >= k.holeOnsetTear) && (drive >= need);
    if (canRupture) {
        if (p.holeCount == 0) {
            p.hole = Monotone(p.hole, std::min(k.holeNucleus, capHere));
            p.holeCount = 1;
            p.lastHoleTime = time;
            ++s_.totalHoleSites;
            p.holes[0].u = 0.30 + 0.40 * Rand01(s_.battleSeed, static_cast<uint32_t>(idx), 0, 3);
            p.holes[0].v = 0.30 + 0.40 * Rand01(s_.battleSeed, static_cast<uint32_t>(idx), 0, 4);
            p.holes[0].bornTick = s_.tickIndex;
            DistributeHoleSites(p, idx);
        } else {
            double room = std::max(0.0, 1.0 - p.hole / std::max(1.0e-6, capHere));
            double dH = k.holeGain * SafePow(drive, 0.90) *
                        SafePow(p.hole + k.holeNucleus, k.holeGrowthExponent) / G * room;
            p.hole = Monotone(p.hole, Clamp(p.hole + dH, 0.0, capHere));
        }
        EnsureHoleSites(p, idx, time);
    }

    p.accumulatedEnergy = Monotone(p.accumulatedEnergy, p.accumulatedEnergy + E);
    p.lastHitTime = time;
    ++p.hitCount;
}

void DamageModel::ApplyHit(const HitEvent& ev) {
    if (!s_.battleActive) Reset(s_.battleSeed);

    double rawTime = Finite(ev.time, s_.simTime);
    double t = std::max(rawTime, s_.lastIntensityTime);       // no time rewind
    double maxAdvance = s_.simTime + 10.0;                    // bound for pathological gaps
    if (t > maxAdvance) t = maxAdvance;
    if (t > s_.simTime) {
        double remain = t - s_.simTime;
        int guard = 0;
        while (remain > 0.0 && guard++ < 256) {
            double step = std::min(remain, cfg_.sim.dtMax);
            Tick(step);
            remain -= step;
        }
    }

    int idx = static_cast<int>(Clamp(Finite(ev.panel, 0.0), 0.0,
                                     static_cast<double>(cfg_.panelCount - 1)));
    int ht = ev.hitType;
    if (ht < 0 || ht >= cfg_.hitTypeCount) ht = 0;
    const HitProfile& hp = cfg_.hitTypes[ht];

    double energy = Finite(ev.energy, 0.0);
    energy = Clamp(energy, 0.0, cfg_.sim.maxHitEnergy);
    double E = energy * cfg_.sim.energyScale * hp.energy;
    if (ev.critical) E *= cfg_.sim.criticalMultiplier;

    // Impulse into the leaky intensity integrator (before damage: current fight pressure
    // immediately affects how the fabric responds).
    double tau = std::max(0.01, cfg_.sim.intensityTauSeconds);
    double norm = std::max(0.01, cfg_.sim.intensityNorm);
    if (E > 0.0) {
        DecayIntensityTo(std::min(t, s_.simTime));
        s_.intensity = Clamp(s_.intensity + E / (norm * tau), 0.0, 1.0);
    }
    double localIntensity = s_.intensity;
    double fatigue = 1.0 + cfg_.sim.fatigueGain * localIntensity;  // rapid repeated loading
    double Eeff = E * fatigue;
    ++s_.totalHits;

    // Primary panel takes the full load; adjacent panels share stress across seams.
    ApplyToPanel(idx, t, Eeff, hp, localIntensity, hp.sharp);
    const PanelDef& pd = cfg_.panels[idx];
    for (int j = 0; j < pd.neighborCount; ++j) {
        int q = pd.neighbors[j];
        if (q == idx || q < 0 || q >= cfg_.panelCount) continue;
        ApplyToPanel(q, t, Eeff * cfg_.sim.adjacencyWeight, hp, localIntensity, hp.sharp);
    }
}

uint32_t DamageModel::ComputeTargetStage() const {
    const Stages& st = cfg_.stages;
    double w = WearMean(), t = TearMean(), h = HoleMean();
    uint32_t tearPanels = TearPanelCount();
    uint32_t holes = HoleCount();

    uint32_t cand = 0;
    if (w >= st.S1_wearMean) cand = 1;
    if (t >= st.S2_tearMean && tearPanels >= static_cast<uint32_t>(st.S2_tearPanels)) cand = 2;
    if (holes >= static_cast<uint32_t>(st.S3_holeCount) && h >= st.S3_holeMean) cand = 3;

    // Hysteresis only matters for demotion (config reload / externally edited state).
    if (cand < s_.stage) {
        double f = st.hysteresis;
        if (s_.stage >= 3 && holes >= static_cast<uint32_t>(st.S3_holeCount) &&
            h >= st.S3_holeMean * f) cand = 3;
        else if (s_.stage >= 2 && t >= st.S2_tearMean * f &&
                 tearPanels >= static_cast<uint32_t>(st.S2_tearPanels)) cand = 2;
        else if (s_.stage >= 1 && w >= st.S1_wearMean * f) cand = 1;
    }
    return static_cast<uint32_t>(Clamp(cand, 0, static_cast<double>(cfg_.constraints.maxStage)));
}

void DamageModel::CommitStage(uint32_t target) {
    s_.prevStage = s_.stage;
    s_.stage = target;
    s_.stageBlend = 0.0;
    s_.pendingStage = target;
    s_.pendingStageTime = 0.0;
}

uint32_t DamageModel::ComputeHeavyGrade() const {
    const Stages& st = cfg_.stages;
    uint32_t holes = HoleCount();
    double I = s_.intensity;
    double t = TearMean();
    uint32_t g = 0;
    if (holes >= static_cast<uint32_t>(st.grade1_holeCount)) g = 1;
    if (holes >= static_cast<uint32_t>(st.grade2_holeCount) ||
        (holes >= static_cast<uint32_t>(st.grade1_holeCount) && I >= 0.50)) g = 2;
    if (holes >= static_cast<uint32_t>(st.grade3_holeCount) ||
        (I >= st.grade3_intensity && t >= st.grade3_tearMean &&
         holes >= static_cast<uint32_t>(st.grade2_holeCount))) g = 3;
    return Clamp(g, 0, 3);
}

void DamageModel::Tick(double dt) {
    double d = Clamp(Finite(dt, 0.0), 0.0, std::max(0.001, cfg_.sim.dtMax));
    s_.simTime += d;
    ++s_.tickIndex;
    DecayIntensityTo(s_.simTime);

    if (s_.stageBlend < 1.0)
        s_.stageBlend = Clamp(s_.stageBlend + d / std::max(0.001, cfg_.visual.blendSeconds), 0.0, 1.0);

    uint32_t target = ComputeTargetStage();
    if (target != s_.stage) {
        if (target != s_.pendingStage) {
            s_.pendingStage = target;
            s_.pendingStageTime = 0.0;
        }
        s_.pendingStageTime += d;
        if (s_.pendingStageTime >= cfg_.sim.stageDwellSeconds) CommitStage(target);
    } else {
        s_.pendingStage = s_.stage;
        s_.pendingStageTime = 0.0;
    }
    s_.heavyGrade = ComputeHeavyGrade();
}

} // namespace bw
