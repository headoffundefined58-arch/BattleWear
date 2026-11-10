// BattleWear - exhaustive flow + edge-case verification.
// Builds a deterministic battle, validates EVERY frame against all hard invariants,
// and writes out/trace.json + out/test_report.json as reproducible evidence.
#include "battlewear/battlewear.h"
#include "battlewear/mini_json.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace bw;

static int g_checks = 0;
static int g_fails = 0;
static std::vector<std::string> g_failNames;

static void Check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) { ++g_fails; g_failNames.push_back(name); std::printf("FAIL  %s\n", name); }
}
static void CheckReport(const ValidationReport& r, const char* name) {
    if (r.Ok()) { ++g_checks; return; }
    ++g_checks; ++g_fails;
    std::string s = name;
    s += " [";
    for (int i = 0; i < r.violationCount; ++i) {
        if (i) s += ",";
        s += ViolationName(r.codes[i]);
    }
    s += "]";
    g_failNames.push_back(s);
    std::printf("FAIL  %s\n", s.c_str());
}

static Config LoadConfig() {
    Config c = Config::Defaults();
    const char* paths[] = {
        "../config/battlewear.config.json",
        "config/battlewear.config.json",
        "../../config/battlewear.config.json",
        "E:/NARUTO X BORUTO/BattleWear/config/battlewear.config.json"
    };
    for (const char* p : paths) {
        std::string err;
        Config tmp = Config::Defaults();
        if (tmp.LoadFile(p, err)) {
            std::printf("config loaded: %s\n", p);
            return tmp;
        }
    }
    std::printf("config file not found - using built-in defaults (identical values)\n");
    return c;
}

struct FrameTrace {
    double time = 0;
    uint32_t stage = 0, grade = 0;
    double wear = 0, tear = 0, hole = 0, intensity = 0, coverage = 0;
    uint32_t holes = 0;
    uint32_t commands = 0;
};

struct Harness {
    Config cfg;
    DamageModel model;
    CostumeRegistry registry;
    VisualMapper mapper;
    CostumeHandle costume;
    std::vector<FrameTrace> trace;
    std::vector<std::string> notes;
    int invariantFrames = 0;

    explicit Harness(const Config& c, const char* code = "5nrt", const char* variant = "default")
        : cfg(c), model(c), registry(c), mapper(c) {
        registry.Resolve(code, variant, costume);
    }

    void Hit(double energy, int panel, int type, bool critical = false) {
        HitEvent e;
        e.time = model.State().simTime;
        e.energy = energy;
        e.panel = panel;
        e.hitType = type;
        e.critical = critical;
        model.ApplyHit(e);
    }

    void Step(double dt) {
        DamageState before = model.State();
        model.Tick(dt);
        CheckReport(ValidateMonotonicity(before, model.State()), "monotonicity");
        CheckReport(ValidateState(cfg, model.State()), "state invariants");
        VisualFrame f;
        mapper.BuildFrame(model.State(), costume, f);
        CheckReport(ValidateFrame(cfg, f), "frame invariants");
        // Explicit hard constraints, checked independently of the validator.
        if (!f.garmentAttached) Check(false, "garmentAttached");
        for (int i = 0; i < f.panelCount; ++i) {
            if (f.panels[i].baseMesh[0] == 0 || f.panels[i].baseMaterial[0] == 0)
                Check(false, "base mesh/material always bound");
            if (f.panels[i].holeCutout > 0.0 &&
                f.panels[i].underlayer + 1e-9 < f.panels[i].holeCutout)
                Check(false, "hole always backed by underlayer");
        }
        for (int i = 0; i < f.commandCount; ++i)
            if (static_cast<int>(f.commands[i].op) >= kRenderOpCount)
                Check(false, "no forbidden render op");
        ++invariantFrames;
        FrameTrace t;
        t.time = model.State().simTime;
        t.stage = model.State().stage;
        t.grade = model.State().heavyGrade;
        t.wear = model.WearMean();
        t.tear = model.TearMean();
        t.hole = model.HoleMean();
        t.intensity = model.State().intensity;
        t.coverage = model.CoverageRatio();
        t.holes = model.HoleCount();
        t.commands = static_cast<uint32_t>(f.commandCount);
        trace.push_back(t);
    }

    void Rebind(const char* code, const char* variant) {
        registry.Resolve(code, variant, costume);
        model.Reset(0xBEEF42ULL);   // new garment = intact garment
        notes.push_back(std::string("rebind ") + code + "/" + variant);
    }
};

static void WriteTraceJson(const Harness& h, const char* path) {
    std::ofstream o(path);
    o << std::fixed << std::setprecision(6);
    o << "{\n  \"frames\": [\n";
    for (size_t i = 0; i < h.trace.size(); ++i) {
        const FrameTrace& t = h.trace[i];
        o << "    {\"t\":" << t.time << ",\"stage\":" << t.stage << ",\"grade\":" << t.grade
          << ",\"wear\":" << t.wear << ",\"tear\":" << t.tear << ",\"hole\":" << t.hole
          << ",\"intensity\":" << t.intensity << ",\"coverage\":" << t.coverage
          << ",\"holes\":" << t.holes << ",\"commands\":" << t.commands << "}";
        if (i + 1 < h.trace.size()) o << ",";
        o << "\n";
    }
    o << "  ],\n  \"notes\": [";
    for (size_t i = 0; i < h.notes.size(); ++i) {
        if (i) o << ",";
        o << "\"" << h.notes[i] << "\"";
    }
    o << "]\n}\n";
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    Config cfg = LoadConfig();
    std::printf("BattleWear verification - %d panels, %d hit types\n",
                cfg.panelCount, cfg.hitTypeCount);

    // ---------------------------------------------------------------- 1. config
    Check(cfg.constraints.allowGarmentRemoval == false, "removal disabled by default");
    Check(cfg.constraints.alwaysBackedByUnderlayer == true, "underlayer mandatory");
    Check(cfg.constraints.maxStage == 3, "max stage is 3 (no removal tier)");
    Check(std::fabs(cfg.TotalPanelArea() - 1.0) < 1e-9, "panel areas normalized to 1");
    Check(cfg.IsCommandAllowed("SET_MATERIAL_PARAM"), "material param op allowed");
    Check(cfg.IsCommandAllowed("ADD_DECAL"), "decal op allowed");
    Check(cfg.IsCommandAllowed("BIND_ADDITIVE_MESH"), "additive mesh op allowed");
    Check(cfg.IsCommandAllowed("CUT_GEOMETRY"), "geometry cut op allowed");
    Check(cfg.IsCommandAllowed("BIND_DAMAGE_VARIANT"), "damage variant op allowed");
    Check(!cfg.constraints.destructionRequiresAssets,
          "destruction never requires authored assets (no starred-costume gate)");
    Check(!cfg.IsCommandAllowed("HIDE_MESH"), "hide mesh op rejected");
    Check(!cfg.IsCommandAllowed("DETACH_COSTUME"), "detach op rejected");

    // Config cannot enable removal, and forbidden ops are dropped even if requested.
    {
        std::string evil = "{\"constraints\":{\"allowGarmentRemoval\":true},"
                           "\"visual\":{\"allowedCommands\":[\"HIDE_MESH\",\"ADD_DECAL\"]}}";
        Config c = Config::Defaults(); std::string err;
        c.LoadJson(evil, err);
        Check(!c.constraints.allowGarmentRemoval, "config cannot enable removal");
        Check(!c.IsCommandAllowed("HIDE_MESH"), "config cannot whitelist a hide op");
        Check(c.IsCommandAllowed("ADD_DECAL"), "legit op preserved");
        Check(c.constraints.maxStage == 3, "config cannot add a removal stage");
    }
    {
        std::string partial = "{\"simulation\":{\"wearGain\":0.09}}";
        Config c = Config::Defaults(); std::string err;
        Check(c.LoadJson(partial, err), "partial config parses");
        Check(std::fabs(c.sim.wearGain - 0.09) < 1e-12, "partial config overrides one key");
        Check(std::fabs(c.sim.tearGain - 0.030) < 1e-12, "partial config keeps other defaults");
    }
    {
        std::string bad = "{\"simulation\": ";
        Config c = Config::Defaults(); std::string err;
        Check(!c.LoadJson(bad, err), "malformed config rejected");
    }
    {
        std::string utf8 = "{\"stages\":{\"labels\":[\"完好\",\"轻微磨损\",\"边缘撕裂\",\"局部破洞\"]}}";
        Config c = Config::Defaults(); std::string err;
        Check(c.LoadJson(utf8, err), "utf-8 config parses");
        Check(strcmp(c.stages.labels[3], "局部破洞") == 0, "utf-8 labels preserved");
    }

    // ---------------------------------------------------------------- 2. costume registry
    {
        CostumeRegistry reg(cfg);
        CostumeHandle a;
        Check(reg.Resolve("nonexistent_mod_author", "v99", a), "unknown costume resolves");
        Check(a.fallbackUsed, "unknown costume uses generic fallback");
        Check(!a.dedicated, "fallback has no dedicated assets");
        for (int i = 0; i < cfg.panelCount; ++i) {
            Check(a.panels[i].mesh[0] != 0, "fallback panel has base mesh");
            Check(a.panels[i].material[0] != 0, "fallback panel has base material");
        }
        Check(reg.FallbackCount() >= 1, "fallback counted");

        std::string json =
            "{\"costumes\":[{\"characterCode\":\"7brt\",\"variantId\":\"jacket_v3\","
            "\"baseMesh\":\"cmn/7brt/jacket.nud\",\"baseMaterial\":\"cmn/7brt/jacket.nut\","
            "\"damageAssets\":\"dedicated\",\"panels\":{"
            "\"torso_front\":{\"mesh\":\"front.nud\",\"material\":\"front.nut\","
            "\"frayVariant\":\"front_fray.nud\",\"holeMask\":\"front_hole.dds\"}}}]}";
        std::string err;
        Check(reg.LoadJson(json, err), "binding file parses");
        CostumeHandle b;
        Check(reg.Resolve("7brt", "jacket_v3", b), "dedicated costume resolves");
        Check(b.dedicated && !b.fallbackUsed, "dedicated flag set");
        Check(b.panels[0].hasFrayVariant && b.panels[0].hasHoleMask, "dedicated assets present");
        Check(b.panels[1].mesh[0] != 0 && b.panels[1].material[0] != 0,
              "unspecified panel inherits base binding");
        CostumeHandle c2;
        reg.Resolve("7brt", "other_variant", c2);
        Check(c2.fallbackUsed, "same character, unknown variant -> fallback");
    }

    // ---------------------------------------------------------------- 3. pristine state
    Harness h(cfg);
    int blunt = cfg.FindHitType("blunt");
    int slash = cfg.FindHitType("slash");
    int pierce = cfg.FindHitType("pierce");
    int heat = cfg.FindHitType("heat");
    Check(blunt >= 0 && slash >= 0 && pierce >= 0 && heat >= 0, "hit type table resolved");
    h.notes.push_back("S0 pristine");
    h.Step(1.0 / 60.0);
    Check(h.model.State().stage == 0, "starts at stage 0");
    Check(h.model.WearMean() == 0.0 && h.model.TearMean() == 0.0 &&
          h.model.HoleMean() == 0.0, "starts with zero damage");
    Check(std::fabs(h.model.CoverageRatio() - 1.0) < 1e-12, "starts at full coverage");
    Check(h.model.HoleCount() == 0, "starts with no holes");
    Check(h.model.State().heavyGrade == 0, "starts at heavy grade 0");
    {
        VisualFrame f; h.mapper.BuildFrame(h.model.State(), h.costume, f);
        bool onlyBase = true;
        for (int i = 0; i < f.commandCount; ++i)
            if (strcmp(f.commands[i].role, "base_keep") != 0) onlyBase = false;
        Check(onlyBase, "pristine frame emits only base-binding commands");
        Check(f.commandCount == cfg.panelCount, "one base command per panel");
        Check(f.coverageRatio == 1.0, "pristine coverage 1.0");
    }

    // ---------------------------------------------------------------- 4-6. full progression
    // Each phase drives the model with the physically appropriate stimulus until the
    // next tier is reached, so the walkthrough is not tied to brittle hit counts.
    auto DriveUntil = [&](uint32_t targetStage, int maxFrames, double energy, int type,
                          int stride, bool critical, const char* label) {
        int frames = 0;
        while (h.model.State().stage < targetStage && frames < maxFrames) {
            h.Step(1.0 / 60.0);
            if (frames % stride == 0)
                h.Hit(energy, (frames * 7) % cfg.panelCount, type, critical && (frames % 5 == 0));
            ++frames;
        }
        std::ostringstream n;
        n << label << " -> stage " << h.model.State().stage << " after " << frames
          << " frames @" << std::fixed << std::setprecision(2) << h.model.State().simTime << "s";
        h.notes.push_back(n.str());
        return frames;
    };

    // 阶段刚提交的那一帧 stageBlend = 0，是新层级的交叉淡入起点：此时新层级视觉强度
    // 为 0（旧层级的贴花仍然在画，所以画面不会"掉档"）。断言稳态表现前先让淡入走完，
    // 否则测到的是淡入第一帧，必然看不到该层级的贴花。
    auto SettleFade = [&](int maxFrames) {
        int n = 0;
        while (h.model.State().stageBlend < 1.0 && n < maxFrames) { h.Step(1.0 / 60.0); ++n; }
        return n;
    };

    h.notes.push_back("S1 light abrasion phase");
    int f1 = DriveUntil(1, 6000, 0.85, blunt, 3, false, "S0->S1");
    Check(f1 < 6000, "light abrasion reaches S1 in bounded time");
    Check(h.model.State().stage >= 1, "light hits promote to S1 (light wear)");
    Check(h.model.WearMean() >= cfg.stages.S1_wearMean * 0.7, "wear mean approaches S1 threshold");
    Check(h.model.HoleCount() == 0, "no holes from light abrasion");
    Check(h.model.HoleMean() == 0.0, "no cutout from light abrasion");

    h.notes.push_back("S2 edge tearing phase");
    int f2 = DriveUntil(2, 12000, 4.0, slash, 2, false, "S1->S2");
    Check(f2 < 12000, "sharp hits reach S2 in bounded time");
    Check(h.model.State().stage >= 2, "sharp hits promote to S2 (edge tearing)");
    Check(h.model.TearMean() > 0.10, "tear accumulated");
    Check(h.model.TearPanelCount() >= 2, "tearing is coherent across panels");
    SettleFade(120);
    {
        VisualFrame f; h.mapper.BuildFrame(h.model.State(), h.costume, f);
        bool anyFray = false, anyWear = false;
        for (int i = 0; i < f.commandCount; ++i) {
            if (strcmp(f.commands[i].role, "fray_edge") == 0) anyFray = true;
            if (strcmp(f.commands[i].role, "wear") == 0) anyWear = true;
        }
        Check(anyFray, "S2 emits edge fray decals");
        Check(anyWear, "S1 wear decals persist into S2");
    }

    h.notes.push_back("S3 local holes phase");
    int f3 = DriveUntil(3, 18000, 6.0, pierce, 1, true, "S2->S3");
    Check(f3 < 18000, "heavy puncture reaches S3 in bounded time");
    Check(h.model.State().stage == 3, "heavy fight reaches S3 (local holes)");
    Check(h.model.HoleCount() >= static_cast<uint32_t>(cfg.stages.S3_holeCount),
          "at least S3 hole count reached");
    Check(h.model.HoleMean() > 0.0, "cutout area accumulated");
    Check(h.model.State().heavyGrade >= 1, "heavy grade escalated");
    Check(h.model.CoverageRatio() >= cfg.constraints.minCoverageRatio, "coverage still >= 80%");
    SettleFade(120);
    {
        VisualFrame f; h.mapper.BuildFrame(h.model.State(), h.costume, f);
        int under = 0, cut = 0;
        for (int i = 0; i < f.commandCount; ++i) {
            if (strcmp(f.commands[i].role, "underlayer") == 0) ++under;
            if (strcmp(f.commands[i].role, "hole_mask") == 0) ++cut;
        }
        Check(under > 0 && cut > 0, "holes drawn with lining + local cutout");
        Check(under == cut, "every cutout has exactly one lining layer");
        Check(f.garmentAttached, "garment attached at S3");
        for (int i = 0; i < f.panelCount; ++i)
            Check(f.panels[i].baseMesh[0] != 0, "S3 panel still has base mesh");
    }

    // ---------------------------------------------------------------- 7. high-intensity coherent heavy damage
    h.notes.push_back("high-intensity sustained phase");
    for (int i = 0; i < 900; ++i) {
        h.Step(1.0 / 60.0);
        h.Hit(6.0, (i * 5) % cfg.panelCount, (i % 2) ? pierce : slash, true);
        if (i % 4 == 0) h.Hit(5.0, (i * 11) % cfg.panelCount, pierce);
    }
    Check(h.model.State().heavyGrade >= 2, "high intensity yields heavy grade >= 2");
    Check(h.model.HoleCount() >= static_cast<uint32_t>(cfg.stages.grade2_holeCount),
          "high intensity yields many coherent holes");
    Check(h.model.CoverageRatio() >= cfg.constraints.minCoverageRatio,
          "coverage bound holds under maximum stress");
    Check(h.model.TotalLossRatio() <= cfg.constraints.maxTotalLossRatio + 1e-9,
          "total fabric loss capped");
    Check(h.model.HoleCount() <= static_cast<uint32_t>(cfg.panelCount * cfg.constraints.maxHolesPerPanel),
          "hole count bounded");
    {
        VisualFrame f; h.mapper.BuildFrame(h.model.State(), h.costume, f);
        Check(f.commandCount <= kMaxCommands, "command buffer never overflows");
        Check(f.commandCount > cfg.panelCount, "heavy damage emits additional visual layers");
        for (int i = 0; i < f.panelCount; ++i) {
            Check(f.panels[i].holeCount <= static_cast<uint32_t>(cfg.constraints.maxHolesPerPanel),
                  "per-panel hole count capped");
            Check(f.panels[i].underlayer + 1e-9 >= f.panels[i].holeCutout,
                  "lining >= cutout on every panel");
        }
    }

    // ---------------------------------------------------------------- 8. long idle: no healing, no flicker
    h.notes.push_back("long idle phase");
    double wearBefore = h.model.WearMean(), tearBefore = h.model.TearMean();
    uint32_t stageBefore = h.model.State().stage;
    for (int i = 0; i < 120; ++i) h.Step(0.10);
    Check(h.model.State().intensity < 0.02, "intensity decays to ~0 when idle");
    Check(h.model.WearMean() >= wearBefore - 1e-9, "wear does not heal during idle");
    Check(h.model.TearMean() >= tearBefore - 1e-9, "tear does not heal during idle");
    Check(h.model.State().stage == stageBefore, "stage does not regress during idle");
    Check(h.model.CoverageRatio() >= cfg.constraints.minCoverageRatio, "coverage unchanged by idle");

    // ---------------------------------------------------------------- 9. numeric edge cases
    h.notes.push_back("numeric edge cases");
    {
        Harness e(cfg);
        e.Step(1.0 / 60.0);
        double nan = std::nan(""), inf = std::numeric_limits<double>::infinity();
        (void)inf;
        e.Hit(0.0, 0, blunt);
        e.Hit(-5.0, 0, blunt);
        e.Hit(nan, 0, blunt);
        e.Hit(1.0e300, 0, blunt);   // Inf -> guarded to 0
        e.Hit(1e12, 0, pierce);     // clamped to maxHitEnergy
        e.Step(-1.0);
        e.Step(0.0);
        e.Step(nan);
        e.Step(1e9);                // clamped to dtMax
        CheckReport(ValidateState(cfg, e.model.State()), "state valid after numeric abuse");
        Check(e.model.WearMean() >= 0.0 && e.model.WearMean() <= 1.0, "wear bounded after abuse");
        Check(e.model.CoverageRatio() >= cfg.constraints.minCoverageRatio,
              "coverage bound after extreme energy");
        Check(e.model.HoleMean() <= cfg.constraints.maxTotalLossRatio + 1e-9,
              "loss cap after extreme energy");

        HitEvent bad;
        bad.time = -100.0; bad.energy = 3.0; bad.panel = -7; bad.hitType = 999;
        e.model.ApplyHit(bad);
        bad.time = 1e12; bad.panel = 9999; bad.hitType = -5; bad.energy = 3.0;
        e.model.ApplyHit(bad);
        e.Step(1.0 / 60.0);
        CheckReport(ValidateState(cfg, e.model.State()), "state valid after invalid indices/times");
        Check(e.model.State().simTime < 100.0, "pathological future timestamp bounded");
    }

    // ---------------------------------------------------------------- 10. determinism
    {
        auto run = [&](uint64_t seed) {
            Harness x(cfg);
            x.model.Reset(seed);
            for (int i = 0; i < 300; ++i) {
                x.Step(1.0 / 60.0);
                x.Hit(5.0, (i * 3) % cfg.panelCount, pierce, true);
            }
            return x.model.State();
        };
        DamageState a = run(12345), b = run(12345), c = run(999);
        bool same = true, different = false;
        for (int i = 0; i < cfg.panelCount; ++i) {
            if (std::fabs(a.panels[i].hole - b.panels[i].hole) > 1e-12) same = false;
            for (uint32_t k = 0; k < a.panels[i].holeCount && k < kMaxHoles; ++k)
                if (std::fabs(a.panels[i].holes[k].u - b.panels[i].holes[k].u) > 1e-12) same = false;
        }
        for (int i = 0; i < cfg.panelCount; ++i)
            for (uint32_t k = 0; k < a.panels[i].holeCount && k < c.panels[i].holeCount && k < kMaxHoles; ++k)
                if (std::fabs(a.panels[i].holes[k].u - c.panels[i].holes[k].u) > 1e-9) different = true;
        Check(same, "same seed -> identical damage and hole placement");
        Check(different, "different seed -> different hole placement");
    }

    // ---------------------------------------------------------------- 11. frame-rate independence
    {
        auto run = [&](double dt, int framesPerHit) {
            Harness x(cfg);
            for (int frame = 0; frame < framesPerHit * 240; ++frame) {
                x.Step(dt);
                if (frame % framesPerHit == 0) {
                    int i = frame / framesPerHit;
                    x.Hit(4.0, (i * 5) % cfg.panelCount, (i % 2) ? slash : pierce);
                }
            }
            return x.model.State();
        };
        DamageState a = run(1.0 / 30.0, 3);   // hit every 0.1s
        DamageState b = run(1.0 / 60.0, 6);   // hit every 0.1s
        bool ok = std::fabs(a.simTime - b.simTime) < 1e-6;
        for (int i = 0; i < cfg.panelCount; ++i) {
            if (std::fabs(a.panels[i].wear - b.panels[i].wear) > 2e-6) ok = false;
            if (std::fabs(a.panels[i].tear - b.panels[i].tear) > 2e-6) ok = false;
            if (std::fabs(a.panels[i].hole - b.panels[i].hole) > 2e-6) ok = false;
        }
        Check(ok, "damage is frame-rate independent (30 vs 60 fps)");
        Check(a.stage == b.stage, "stage is frame-rate independent");
    }

    // ---------------------------------------------------------------- 12. debounce / hysteresis
    {
        Harness d(cfg);
        // A single huge burst must not instantly flip the visual tier.
        d.Hit(20.0, 0, pierce, true);
        d.Step(1.0 / 60.0);
        Check(d.model.State().stage == 0, "single burst does not instantly promote (debounce)");
        for (int i = 0; i < 40; ++i) d.Step(1.0 / 60.0);
        Check(d.model.State().stage >= 1, "sustained state eventually promotes");
        uint32_t s = d.model.State().stage;
        for (int i = 0; i < 200; ++i) d.Step(0.05);
        Check(d.model.State().stage >= s, "stage never demotes (monotone damage)");
    }

    // ---------------------------------------------------------------- 13. hit-location diffusion
    {
        Harness loc(cfg);
        for (int i = 0; i < 60; ++i) { loc.Step(1.0 / 60.0); loc.Hit(2.0, 0, slash); }
        Check(loc.model.State().panels[0].wear > loc.model.State().panels[8].wear,
              "primary hit panel dominates");
        bool neighborWorn = false;
        for (int j = 0; j < cfg.panels[0].neighborCount; ++j)
            if (loc.model.State().panels[cfg.panels[0].neighbors[j]].wear > 0.0) neighborWorn = true;
        Check(neighborWorn, "stress diffuses to adjacent panels (coherent damage)");
        Check(loc.model.State().panels[0].wear <= 1.0, "wear stays in range");
    }

    // ---------------------------------------------------------------- 14. costume hot-swap
    h.notes.push_back("costume hot-swap");
    h.Rebind("7brt", "jacket_v3");
    h.Step(1.0 / 60.0);
    Check(h.model.State().stage == 0, "costume change resets to pristine");
    Check(h.model.WearMean() == 0.0 && h.model.HoleCount() == 0, "new garment is intact");
    Check(h.costume.fallbackUsed, "unknown variant still resolves via fallback");
    CheckReport(ValidateFrame(cfg, [&]() {
        VisualFrame f; h.mapper.BuildFrame(h.model.State(), h.costume, f); return f;
    }()), "frame valid after rebind");
    {
        CostumeRegistry reg(cfg);
        std::string json = "{\"costumes\":[{\"characterCode\":\"7brt\",\"variantId\":\"jacket_v3\","
                           "\"baseMesh\":\"m\",\"baseMaterial\":\"t\",\"damageAssets\":\"dedicated\","
                           "\"panels\":{\"torso_front\":{\"frayVariant\":\"f.nud\",\"holeMask\":\"h.dds\"}}}]}";
        std::string err; reg.LoadJson(json, err);
        CostumeHandle ch; reg.Resolve("7brt", "jacket_v3", ch);
        Harness x(cfg, "7brt", "jacket_v3");
        (void)x;
        // Bind the dedicated handle explicitly for the visual check.
        x.costume = ch;
        for (int i = 0; i < 400; ++i) { x.Step(1.0 / 60.0); x.Hit(6.0, 0, pierce, true); }
        VisualFrame f; x.mapper.BuildFrame(x.model.State(), x.costume, f);
        bool additive = false, dedicatedMask = false;
        for (int i = 0; i < f.commandCount; ++i) {
            if (f.commands[i].op == RenderOp::BIND_ADDITIVE_MESH) additive = true;
            if (strcmp(f.commands[i].role, "hole_mask") == 0 &&
                strcmp(f.commands[i].resource, "h.dds") == 0) dedicatedMask = true;
        }
        Check(additive, "dedicated fray variant bound additively");
        Check(dedicatedMask, "dedicated hole mask used when available");
        Check(f.garmentAttached, "dedicated costume still always attached");
        for (int i = 0; i < f.panelCount; ++i)
            Check(f.panels[i].baseMesh[0] != 0, "dedicated costume keeps every base mesh");
    }

    // ------------------------------------------- 14b. universal destruction (no ★ gate)
    {
        // A costume that ships no damage asset at all must still break structurally.
        Harness u(cfg, "totally_new_mod_author", "costume_v12");
        Check(u.costume.fallbackUsed, "unknown costume falls back to the generic binding");
        Check(u.costume.destructible, "generic costume is destructible");
        Check(u.costume.variantMask == 0, "generic costume ships no native variant");
        for (int i = 0; i < 900; ++i) { u.Step(1.0 / 60.0); u.Hit(14.0, 5, pierce, true); }
        VisualFrame f; u.mapper.BuildFrame(u.model.State(), u.costume, f);
        bool cut = false, strip = false;
        int baseKeep = 0;
        for (int i = 0; i < f.commandCount; ++i) {
            if (f.commands[i].op == RenderOp::CUT_GEOMETRY) cut = true;
            if (f.commands[i].op == RenderOp::BIND_ADDITIVE_MESH ||
                f.commands[i].op == RenderOp::BIND_DAMAGE_VARIANT) strip = true;
            if (strcmp(f.commands[i].role, "base_keep") == 0) ++baseKeep;
        }
        Check(u.model.State().stage >= 2, "asset-free costume still reaches tearing");
        Check(cut, "asset-free costume gets real geometry cuts");
        Check(strip, "asset-free costume gets generated cloth strips");
        Check(baseKeep == cfg.panelCount, "every panel still emits its base binding");
        bool coreStripped = false, procSource = true;
        for (int i = 0; i < f.panelCount; ++i) {
            if (f.panels[i].stripped && cfg.panels[i].core) coreStripped = true;
            if (f.panels[i].tearSource != TearSource::NONE &&
                f.panels[i].tearSource != TearSource::PROCEDURAL) procSource = false;
        }
        Check(!coreStripped, "garment body (core panels) is never stripped");
        Check(procSource, "no native variant -> procedural tear source");
        CheckReport(ValidateFrame(cfg, f), "asset-free frame invariants");
    }
    {
        // A costume that DOES ship a native _dmg0N chain: bound as an extra layer only.
        std::string json = "{\"costumes\":[{\"characterCode\":\"2nrx\","
                           "\"variantId\":\"naruto_road\",\"baseMesh\":\"m\","
                           "\"baseMaterial\":\"t\",\"damageVariants\":[\"a_dmg01.xfbin\","
                           "\"a_dmg02.xfbin\",\"a_dmg03.xfbin\"]}]}";
        CostumeRegistry reg(cfg);
        std::string err;
        Check(reg.LoadJson(json, err), "binding file with native variants parses");
        CostumeHandle ch;
        reg.Resolve("2nrx", "naruto_road", ch);
        Check(ch.variantMask == 0xEu, "native variant mask covers stages 1..3");
        Check(ch.destructible, "costume with native variants is destructible");
        Harness n(cfg);
        n.costume = ch;
        for (int i = 0; i < 400; ++i) { n.Step(1.0 / 60.0); n.Hit(9.0, 0, pierce, true); }
        VisualFrame f; n.mapper.BuildFrame(n.model.State(), n.costume, f);
        bool variantBound = false, hybrid = false;
        int baseKeep = 0;
        for (int i = 0; i < f.commandCount; ++i) {
            if (f.commands[i].op == RenderOp::BIND_DAMAGE_VARIANT) variantBound = true;
            if (strcmp(f.commands[i].role, "base_keep") == 0) ++baseKeep;
        }
        for (int i = 0; i < f.panelCount; ++i)
            if (f.panels[i].tearSource == TearSource::HYBRID) hybrid = true;
        Check(variantBound, "native damage variant bound as an extra layer");
        Check(baseKeep == cfg.panelCount, "native variant never replaces the base binding");
        Check(hybrid, "native + procedural -> hybrid source");
        CheckReport(ValidateFrame(cfg, f), "native variant frame invariants");
    }

    // ---------------------------------------------------------------- 15. stress saturation
    {
        Harness s(cfg);
        for (int i = 0; i < 3000; ++i) {
            s.Step(1.0 / 60.0);
            s.Hit(25.0, i % cfg.panelCount, pierce, true);
        }
        Check(s.model.CoverageRatio() >= cfg.constraints.minCoverageRatio,
              "3000 max hits: coverage still >= 80%");
        Check(s.model.TotalLossRatio() <= cfg.constraints.maxTotalLossRatio + 1e-9,
              "3000 max hits: total loss capped");
        Check(s.model.State().stage <= 3, "3000 max hits: no removal stage");
        CheckReport(ValidateState(cfg, s.model.State()), "state valid at saturation");
        VisualFrame f; s.mapper.BuildFrame(s.model.State(), s.costume, f);
        CheckReport(ValidateFrame(cfg, f), "frame valid at saturation");
        Check(f.commandCount <= kMaxCommands, "command buffer at saturation");
    }

    // ---------------------------------------------------------------- outputs
    WriteTraceJson(h, "../out/trace.json");
    {
        std::ofstream o("../out/test_report.json");
        o << "{\n  \"checks\": " << g_checks << ",\n  \"failures\": " << g_fails
          << ",\n  \"invariantFrames\": " << h.invariantFrames
          << ",\n  \"traceFrames\": " << h.trace.size() << ",\n  \"failed\": [";
        for (size_t i = 0; i < g_failNames.size(); ++i) {
            if (i) o << ",";
            o << "\"" << g_failNames[i] << "\"";
        }
        o << "]\n}\n";
    }
    std::printf("\n---------------------------------------------\n");
    std::printf("checks=%d failures=%d invariantFrames=%d traceFrames=%zu\n",
                g_checks, g_fails, h.invariantFrames, h.trace.size());
    std::printf("final stage=%u grade=%u wear=%.4f tear=%.4f hole=%.4f coverage=%.4f holes=%u\n",
                h.model.State().stage, h.model.State().heavyGrade, h.model.WearMean(),
                h.model.TearMean(), h.model.HoleMean(), h.model.CoverageRatio(),
                h.model.HoleCount());
    return g_fails == 0 ? 0 : 1;
}
