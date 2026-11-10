// BattleWear - graded, always-attached garment damage for NUCC / XFBIN costume mods.
//
// HARD INVARIANTS (enforced by invariants.cpp, asserted in tests):
//   * Clothing is NEVER removed, hidden, detached, unloaded or fully faded.
//   * Every logical panel always keeps its base mesh + base material bound.
//   * A "hole" is a local cutout backed by an underlayer (lining fabric), never skin.
//   * Damage is monotone within one battle: no spontaneous healing / recovery.
//   * State is decoupled from any fixed costume asset; unknown costume mods use the
//     generic fallback binding and still receive full graded damage.
//   * Structural damage does NOT require authored assets: when a costume ships no
//     damage variant, the tear, the hole and the dangling strips are generated
//     procedurally. There is no "supported costume" gate.
//   * Fragments may be cut away (a sleeve can lose its distal half) but the garment
//     body is never unbound: torso / waist core panels are never stripped.
//
// The logic layer has no game/engine dependency. All engine I/O goes through
// plugin/game_adapter.h, so this core is deterministic and unit-testable.
#ifndef BATTLEWEAR_H
#define BATTLEWEAR_H

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace bw {

constexpr int kMaxPanels = 32;
constexpr int kMaxNeighbors = 12;
constexpr int kMaxHoles = 8;
constexpr int kMaxHitTypes = 16;
constexpr int kMaxCommands = 512;
constexpr int kMaxCostumes = 64;   // registry entries (heap-backed)
constexpr int kMaxStageVariants = 4;
constexpr int kNameLen = 128;
constexpr int kShortLen = 32;
constexpr double kNoHoleTime = -1.0e9;

// ---------------------------------------------------------------- utilities

inline double Clamp(double v, double lo, double hi) {
    if (!(v >= lo)) return lo;   // also catches NaN
    return v > hi ? hi : v;
}
inline double Finite(double v, double fallback = 0.0) {
    return (v == v && v <= 1.0e300 && v >= -1.0e300) ? v : fallback;  // NaN / Inf guard
}
inline double SafePow(double base, double exp) {
    if (!(base > 0.0)) return 0.0;               // catches 0, negatives and NaN
    if (!Finite(exp)) return 0.0;
    return ::pow(base, exp);
}
inline double Monotone(double previous, double candidate) {
    double c = Finite(candidate, previous);
    return c > previous ? c : previous;
}
inline void CopyName(char* dst, const char* src, int cap = kNameLen) {
    if (!dst) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= static_cast<size_t>(cap)) n = static_cast<size_t>(cap) - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}
inline uint64_t Hash64(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t h = a * 0x9E3779B97F4A7C15ULL ^ (b + 0xBF58476D1CE4E5B9ULL) ^ (c << 21);
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27; h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return h;
}
inline double Rand01(uint64_t seed, uint32_t panel, uint32_t site, uint32_t lane) {
    return static_cast<double>(Hash64(seed, (static_cast<uint64_t>(panel) << 32) | site,
                                     lane) >> 11) / 9007199254740992.0;
}

// ---------------------------------------------------------------- config

struct Constraints {
    bool  allowGarmentRemoval = false;        // MUST stay false
    // MUST stay false: structural damage is generated procedurally, so no costume has to
    // ship pre-authored damage assets ("starred" costumes) to be destructible.
    bool  destructionRequiresAssets = false;
    double minCoverageRatio = 0.50;           // garment body may be torn down to half
    double maxHoleRatioPerPanel = 0.42;
    double maxTotalLossRatio = 0.45;
    int   maxHolesPerPanel = 4;
    bool  alwaysBackedByUnderlayer = true;
    int   maxStage = 3;
};

struct Simulation {
    double dtMax = 0.10;
    double energyScale = 1.0;
    double maxHitEnergy = 25.0;
    double intensityTauSeconds = 1.20;
    double intensityNorm = 2.50;
    double fatigueGain = 0.55;
    double adjacencyWeight = 0.18;
    double wearGain = 0.055;
    double wearSaturationPower = 1.35;
    double tearOnsetWear = 0.12;
    double sharpNucleationEnergy = 1.40;
    double tearGain = 0.030;
    double tearNucleus = 0.045;
    double tearCrackExponent = 0.65;
    double tearEnergyExponent = 0.85;
    double minIntegrity = 0.28;
    double holeOnsetTear = 0.32;
    double holeNucleationEnergy = 2.20;
    double holeGain = 0.020;
    double holeNucleus = 0.030;
    double holeGrowthExponent = 0.55;
    double holeCooldownSeconds = 0.45;
    double holeSiteCoherenceGain = 0.35;
    double stageDwellSeconds = 0.25;
    double criticalMultiplier = 1.35;
};

struct HitProfile {
    char   name[kShortLen] = {};
    double energy = 1.0;
    double wear = 0.5;
    double tear = 0.4;
    double hole = 0.1;
    bool   sharp = false;
};

struct PanelDef {
    int    id = 0;
    char   key[kShortLen] = {};
    double area = 0.0;
    // A core panel is the body of the garment (torso / waist). Core panels may be cut but
    // are never stripped off entirely; limbs and hems may lose their distal piece.
    bool   core = false;
    int    neighbors[kMaxNeighbors] = {};
    int    neighborCount = 0;
};

struct Stages {
    char   names[4][kShortLen] = {};
    char   labels[4][kShortLen] = {};
    double S1_wearMean = 0.10;
    double S2_tearMean = 0.16;
    int    S2_tearPanels = 2;
    int    S3_holeCount = 2;
    double S3_holeMean = 0.045;
    double hysteresis = 0.80;
    int    grade1_holeCount = 3;
    int    grade2_holeCount = 5;
    int    grade3_holeCount = 8;
    double grade3_intensity = 0.55;
    double grade3_tearMean = 0.55;
};

struct VisualConfig {
    double blendSeconds = 0.35;
    double maxDirt = 0.85;
    double maxRoughness = 0.55;
    double maxDesaturation = 0.30;
    double maxFray = 0.95;
    double maxEdgeJitter = 0.40;
    double maxCutout = 1.0;
    double underlayerBoost = 1.0;
    double minUVSpacing = 0.16;
    double radiusMin = 0.045;
    double radiusMax = 0.115;
    double growthRadiusGain = 0.055;
    // --- procedural destruction (no authored asset required on any costume) ---
    bool   proceduralCutout = true;    // real geometry cut instead of a dark decal
    bool   proceduralFray = true;      // generated fray strips on every costume
    bool   proceduralStub = true;      // distal piece (sleeve / hem) may be torn off
    int    stripCountPerHole = 4;      // dangling cloth strips per hole rim
    double stripLength = 0.24;         // strip length, panel-relative
    double stripWidth = 0.035;         // strip width, panel-relative
    double stripDroop = 0.65;          // 0 = stiff, 1 = hangs fully
    double maxPanelRemoval = 0.55;     // hard cap of cut-away area on one panel
    double cutJaggedness = 0.35;       // irregularity of the cut polygon
    double stripTearThreshold = 0.72;  // tear needed before a panel may be stripped
    char   decalWear[kNameLen] = {};
    char   decalSoot[kNameLen] = {};
    char   decalFray[kNameLen] = {};
    char   decalHole[kNameLen] = {};
    char   decalUnderlayer[kNameLen] = {};
    char   procFrayMesh[kNameLen] = {};
    char   procCutPolygon[kNameLen] = {};
    char   procStubMesh[kNameLen] = {};
    int    allowedCommandCount = 0;
    char   allowedCommands[8][kShortLen] = {};
};

struct Config {
    Constraints constraints;
    Simulation sim;
    HitProfile hitTypes[kMaxHitTypes];
    int        hitTypeCount = 0;
    PanelDef   panels[kMaxPanels];
    int        panelCount = 0;
    Stages     stages;
    VisualConfig visual;

    static Config Defaults();
    // Overlay JSON onto defaults. Missing keys keep defaults; returns false + error on parse failure.
    bool LoadJson(const std::string& text, std::string& error);
    bool LoadFile(const std::string& path, std::string& error);
    int  FindHitType(const char* name) const;  // -1 if unknown -> caller falls back to index 0
    int  FindPanel(const char* key) const;
    double TotalPanelArea() const;
    bool IsCommandAllowed(const char* name) const;
};

// ---------------------------------------------------------------- logic state

struct HitEvent {
    double   time = 0.0;         // seconds since battle start
    double   energy = 0.0;       // normalized impact energy
    int      panel = 0;          // logical panel index
    int      hitType = 0;        // index into Config::hitTypes
    bool     critical = false;
    uint32_t attackerId = 0;
};

struct HoleSite {
    double u = 0.0, v = 0.0;    // deterministic UV centre
    double radius = 0.0;        // UV radius
    uint32_t bornTick = 0;
};

struct PanelState {
    double wear = 0.0;              // 0..1 surface abrasion / soiling
    double tear = 0.0;              // 0..1 normalized edge tear length
    double hole = 0.0;              // 0..cap local cutout area ratio of THIS panel
    double accumulatedEnergy = 0.0;
    double lastHitTime = 0.0;
    double lastHoleTime = kNoHoleTime;
    uint32_t hitCount = 0;
    uint32_t holeCount = 0;
    double integrity = 1.0;
    HoleSite holes[kMaxHoles];
};

struct DamageState {
    double   simTime = 0.0;
    double   intensity = 0.0;      // 0..1 leaky-integrated combat intensity
    double   lastIntensityTime = 0.0;
    uint32_t totalHits = 0;
    uint32_t totalHoleSites = 0;
    uint32_t tickIndex = 0;
    uint32_t stage = 0;            // 0 pristine, 1 light wear, 2 edge tearing, 3 local holes
    uint32_t prevStage = 0;
    uint32_t pendingStage = 0;
    double   pendingStageTime = 0.0;
    double   stageBlend = 1.0;     // 0..1 cross-fade after a committed stage change
    uint32_t heavyGrade = 0;       // 0..3, intensity of the heavy-damage presentation
    uint32_t panelCount = 0;
    uint64_t battleSeed = 0;
    bool     battleActive = false;
    PanelState panels[kMaxPanels];
};

class DamageModel {
public:
    explicit DamageModel(const Config& cfg);

    // Explicit reset only: battle start/end, costume change, character switch.
    void Reset(uint64_t seed = 0x5EED1234ULL);
    // Apply one hit at its own timestamp; hits must arrive in non-decreasing time order
    // (out-of-order timestamps are clamped to the model clock, never rewound).
    void ApplyHit(const HitEvent& ev);
    // Advance simulation clock, decay intensity, resolve stage promotion with dwell/debounce.
    void Tick(double dt);

    const DamageState& State() const { return s_; }
    const Config&      GetConfig() const { return cfg_; }

    // Aggregate metrics (area weighted), exposed for the visual layer and tests.
    double WearMean() const;
    double TearMean() const;
    double HoleMean() const;
    uint32_t TearPanelCount() const;   // panels with tear >= 0.20
    uint32_t HoleCount() const;
    double TotalLossRatio() const;     // sum(area_i * hole_i)
    double CoverageRatio() const;      // 1 - TotalLossRatio
    uint32_t ComputeHeavyGrade() const;

private:
    Config cfg_;
    DamageState s_;
    double effectiveHoleCap_ = 0.0;

    void DecayIntensityTo(double t);
    void ApplyToPanel(int panel, double time, double energyScaleAmount, const HitProfile& hp,
                      double localIntensity, bool sharp);
    void EnsureHoleSites(PanelState& p, int panelIndex, double time);
    void DistributeHoleSites(PanelState& p, int panelIndex);
    uint32_t ComputeTargetStage() const;
    void CommitStage(uint32_t target);
};

// ---------------------------------------------------------------- costume binding

struct PanelBinding {
    char mesh[kNameLen] = {};           // ALWAYS bound (base garment stays attached)
    char material[kNameLen] = {};       // ALWAYS bound
    char uvSet[kShortLen] = {};
    char frayVariant[kNameLen] = {};    // optional ADDITIVE edge geometry
    char holeMask[kNameLen] = {};       // optional dedicated cutout decal
    bool hasFrayVariant = false;
    bool hasHoleMask = false;
};

struct CostumeHandle {
    char characterCode[kShortLen] = {};
    char variantId[kNameLen] = {};
    char baseMesh[kNameLen] = {};
    char baseMaterial[kNameLen] = {};
    bool dedicated = false;      // true = modder supplied per-panel damage assets
    bool fallbackUsed = false;   // true = generic binding built for an unknown mod
    // Every costume is destructible. There is deliberately no "supports damage" flag:
    // when no authored asset exists the damage is generated procedurally.
    bool destructible = true;
    // Optional native damage-variant chain shipped by the game / the mod:
    // stageVariant[1] = "..._dmg01.xfbin", [2] = "..._dmg02.xfbin", ...
    char     stageVariant[kMaxStageVariants][kNameLen] = {};
    uint32_t variantMask = 0;    // bit N set = native variant for stage N available
    int  panelCount = 0;
    PanelBinding panels[kMaxPanels];
};

class CostumeRegistry {
public:
    explicit CostumeRegistry(const Config& cfg);
    bool LoadJson(const std::string& text, std::string& error);  // optional binding file
    void Register(const CostumeHandle& handle);
    // Never fails: an unregistered costume produces a generic, fully functional binding.
    bool Resolve(const char* characterCode, const char* variantId, CostumeHandle& out);
    int  RegisteredCount() const { return count_; }
    int  FallbackCount() const { return fallbackCount_; }
private:
    Config cfg_;
    // 用 vector 而非 CostumeHandle[64]：单个 CostumeHandle ≈18 KB，64 个 ≈1.1 MB，
    // 放在栈上会直接撞穿 MSVC 默认 1 MB 栈（0xC00000FD）。注册只在载入期发生，堆分配无妨。
    std::vector<CostumeHandle> entries_;
    int count_ = 0;
    int fallbackCount_ = 0;
    void BuildGeneric(CostumeHandle& out) const;
};

// ---------------------------------------------------------------- visual layer

enum class RenderOp : uint8_t {
    SET_MATERIAL_PARAM = 0,   // modify base material params (never replace/remove it)
    ADD_DECAL = 1,            // additive UV-space overlay
    BIND_ADDITIVE_MESH = 2,   // additive fray strip / dangling cloth on top of the base mesh
    CUT_GEOMETRY = 3,         // drop the triangles inside one hole polygon (real opening)
    BIND_DAMAGE_VARIANT = 4   // bind the native _dmg0N mesh layer over the base mesh
    // NOTE: there is deliberately NO hide / detach / unload / full-alpha op. CUT_GEOMETRY
    // removes triangles inside a hole, it never unbinds the garment: the base mesh and the
    // base material stay bound on every panel in every frame.
};
constexpr RenderOp kLastRenderOp = RenderOp::BIND_DAMAGE_VARIANT;
constexpr int kRenderOpCount = 5;

inline const char* RenderOpName(RenderOp op) {
    switch (op) {
        case RenderOp::SET_MATERIAL_PARAM: return "SET_MATERIAL_PARAM";
        case RenderOp::ADD_DECAL: return "ADD_DECAL";
        case RenderOp::BIND_ADDITIVE_MESH: return "BIND_ADDITIVE_MESH";
        case RenderOp::CUT_GEOMETRY: return "CUT_GEOMETRY";
        case RenderOp::BIND_DAMAGE_VARIANT: return "BIND_DAMAGE_VARIANT";
    }
    return "UNKNOWN";
}

// Where a panel's structural damage comes from. NATIVE_VARIANT = the costume ships a
// _dmg0N model, PROCEDURAL = BattleWear generates the tear itself. No costume is ever
// left at NONE once it has taken damage.
enum class TearSource : uint8_t {
    NONE = 0,
    NATIVE_VARIANT = 1,
    PROCEDURAL = 2,
    HYBRID = 3
};

struct RenderCommand {
    RenderOp op = RenderOp::SET_MATERIAL_PARAM;
    int      panel = 0;
    char     role[kShortLen] = {};   // logical decal / param / additive-mesh role
    char     resource[kNameLen] = {};
    double   p0 = 0.0, p1 = 0.0, p2 = 0.0, p3 = 0.0;
};

struct PanelVisual {
    char   baseMesh[kNameLen] = {};      // invariant: never empty
    char   baseMaterial[kNameLen] = {};  // invariant: never empty
    double dirt = 0.0;
    double roughnessBoost = 0.0;
    double desaturation = 0.0;
    double fray = 0.0;
    double edgeJitter = 0.0;
    double holeCutout = 0.0;
    double underlayer = 0.0;             // invariant: >= holeCutout when a hole exists
    double cutFraction = 0.0;            // geometry actually removed inside holes (0..cap)
    uint32_t holeCount = 0;
    HoleSite holes[kMaxHoles];
    bool   useAdditiveFray = false;
    bool   useDedicatedHoleMask = false;
    bool   fallbackActive = false;
    bool   nativeVariantActive = false;  // native _dmg0N layer currently bound
    bool   stripped = false;             // distal piece torn off, stub remains
    TearSource tearSource = TearSource::NONE;
};

struct VisualFrame {
    uint32_t stage = 0;
    uint32_t prevStage = 0;
    double   stageBlend = 1.0;
    uint32_t heavyGrade = 0;
    double   globalWear = 0.0;
    double   coverageRatio = 1.0;        // invariant: >= minCoverageRatio
    bool     garmentAttached = true;     // invariant: always true
    int      panelCount = 0;
    PanelVisual panels[kMaxPanels];
    int      commandCount = 0;
    RenderCommand commands[kMaxCommands];
};

class VisualMapper {
public:
    explicit VisualMapper(const Config& cfg);
    // Pure function of (damage state, costume handle). No engine calls, no allocation.
    void BuildFrame(const DamageState& state, const CostumeHandle& costume, VisualFrame& out) const;
private:
    Config cfg_;
    void Emit(VisualFrame& out, RenderOp op, int panel, const char* role,
              const char* resource, double p0, double p1, double p2, double p3) const;
    double TierLevel(uint32_t stage, uint32_t prevStage, double blend, uint32_t tier) const;
};

// ---------------------------------------------------------------- invariants

enum class Violation : uint16_t {
    NONE = 0,
    GARMENT_REMOVED = 1,
    BASE_MESH_MISSING = 2,
    BASE_MATERIAL_MISSING = 3,
    FORBIDDEN_RENDER_OP = 4,
    UNDERLAYER_MISSING = 5,
    COVERAGE_BELOW_MIN = 6,
    NON_MONOTONE = 7,
    OUT_OF_RANGE = 8,
    NON_FINITE = 9,
    HOLE_CAP_EXCEEDED = 10,
    TOTAL_LOSS_EXCEEDED = 11,
    STAGE_OUT_OF_RANGE = 12,
    FRAME_OVERFLOW = 13,
    CUTOUT_EXCEEDED = 14,
    CORE_PIECE_STRIPPED = 15,
    BASE_BINDING_MISSING = 16
};

struct ValidationReport {
    int violationCount = 0;
    Violation codes[32] = {};
    bool Ok() const { return violationCount == 0; }
};

ValidationReport ValidateFrame(const Config& cfg, const VisualFrame& frame);
ValidationReport ValidateState(const Config& cfg, const DamageState& state);
// Detects any decrease of damage between two consecutive states (only legal right after Reset).
ValidationReport ValidateMonotonicity(const DamageState& before, const DamageState& after);
const char* ViolationName(Violation v);

} // namespace bw

#endif // BATTLEWEAR_H
