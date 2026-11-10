// BattleWear - hard-constraint validator. Called every frame in debug builds and by tests.
#include "battlewear/battlewear.h"

#include <algorithm>

namespace bw {

static void Add(ValidationReport& r, Violation v) {
    for (int i = 0; i < r.violationCount; ++i) if (r.codes[i] == v) return;  // dedupe
    if (r.violationCount < 32) r.codes[r.violationCount++] = v;
}
static bool Bad(double x) { return !(x == x) || x > 1.0e300 || x < -1.0e300; }
static bool OutOf01(double x) { return Bad(x) || x < -1.0e-9 || x > 1.0 + 1.0e-9; }

const char* ViolationName(Violation v) {
    switch (v) {
        case Violation::NONE: return "NONE";
        case Violation::GARMENT_REMOVED: return "GARMENT_REMOVED";
        case Violation::BASE_MESH_MISSING: return "BASE_MESH_MISSING";
        case Violation::BASE_MATERIAL_MISSING: return "BASE_MATERIAL_MISSING";
        case Violation::FORBIDDEN_RENDER_OP: return "FORBIDDEN_RENDER_OP";
        case Violation::UNDERLAYER_MISSING: return "UNDERLAYER_MISSING";
        case Violation::COVERAGE_BELOW_MIN: return "COVERAGE_BELOW_MIN";
        case Violation::NON_MONOTONE: return "NON_MONOTONE";
        case Violation::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case Violation::NON_FINITE: return "NON_FINITE";
        case Violation::HOLE_CAP_EXCEEDED: return "HOLE_CAP_EXCEEDED";
        case Violation::TOTAL_LOSS_EXCEEDED: return "TOTAL_LOSS_EXCEEDED";
        case Violation::STAGE_OUT_OF_RANGE: return "STAGE_OUT_OF_RANGE";
        case Violation::FRAME_OVERFLOW: return "FRAME_OVERFLOW";
    }
    return "UNKNOWN";
}

static const char* OpNameFor(RenderOp op) {
    switch (op) {
        case RenderOp::SET_MATERIAL_PARAM: return "SET_MATERIAL_PARAM";
        case RenderOp::ADD_DECAL: return "ADD_DECAL";
        case RenderOp::BIND_ADDITIVE_MESH: return "BIND_ADDITIVE_MESH";
    }
    return "UNKNOWN";  // anything outside the enum is a forbidden op
}

ValidationReport ValidateFrame(const Config& cfg, const VisualFrame& frame) {
    ValidationReport r;
    if (!frame.garmentAttached) Add(r, Violation::GARMENT_REMOVED);
    if (frame.stage > static_cast<uint32_t>(cfg.constraints.maxStage)) Add(r, Violation::STAGE_OUT_OF_RANGE);
    if (frame.heavyGrade > 3) Add(r, Violation::OUT_OF_RANGE);
    if (OutOf01(frame.stageBlend)) Add(r, Violation::OUT_OF_RANGE);
    if (frame.commandCount < 0 || frame.commandCount > kMaxCommands) Add(r, Violation::FRAME_OVERFLOW);
    if (frame.coverageRatio + 1.0e-9 < cfg.constraints.minCoverageRatio)
        Add(r, Violation::COVERAGE_BELOW_MIN);

    int n = std::min(frame.panelCount, cfg.panelCount);
    for (int i = 0; i < n; ++i) {
        const PanelVisual& v = frame.panels[i];
        if (v.baseMesh[0] == 0) Add(r, Violation::BASE_MESH_MISSING);
        if (v.baseMaterial[0] == 0) Add(r, Violation::BASE_MATERIAL_MISSING);
        double vals[] = {v.dirt, v.roughnessBoost, v.desaturation, v.fray,
                         v.edgeJitter, v.holeCutout, v.underlayer};
        for (double x : vals) {
            if (Bad(x)) Add(r, Violation::NON_FINITE);
            else if (OutOf01(x)) Add(r, Violation::OUT_OF_RANGE);
        }
        if (v.holeCount > static_cast<uint32_t>(cfg.constraints.maxHolesPerPanel))
            Add(r, Violation::HOLE_CAP_EXCEEDED);
        // Every cutout must be backed by a lining fabric of at least equal strength.
        if (v.holeCutout > 0.0 && v.underlayer + 1.0e-9 < v.holeCutout)
            Add(r, Violation::UNDERLAYER_MISSING);
        for (uint32_t k = 0; k < v.holeCount && k < kMaxHoles; ++k) {
            const HoleSite& h = v.holes[k];
            if (Bad(h.u) || Bad(h.v) || Bad(h.radius)) Add(r, Violation::NON_FINITE);
            else if (OutOf01(h.u) || OutOf01(h.v) || OutOf01(h.radius))
                Add(r, Violation::OUT_OF_RANGE);
        }
    }

    for (int i = 0; i < frame.commandCount && i < kMaxCommands; ++i) {
        const RenderCommand& c = frame.commands[i];
        if (c.op > RenderOp::BIND_ADDITIVE_MESH || !cfg.IsCommandAllowed(OpNameFor(c.op)))
            Add(r, Violation::FORBIDDEN_RENDER_OP);
        if (c.resource[0] == 0) Add(r, Violation::BASE_MATERIAL_MISSING);
        if (Bad(c.p0) || Bad(c.p1) || Bad(c.p2) || Bad(c.p3)) Add(r, Violation::NON_FINITE);
    }
    return r;
}

ValidationReport ValidateState(const Config& cfg, const DamageState& s) {
    ValidationReport r;
    if (s.stage > static_cast<uint32_t>(cfg.constraints.maxStage)) Add(r, Violation::STAGE_OUT_OF_RANGE);
    if (s.heavyGrade > 3) Add(r, Violation::OUT_OF_RANGE);
    if (OutOf01(s.intensity)) Add(r, Violation::OUT_OF_RANGE);
    if (Bad(s.simTime)) Add(r, Violation::NON_FINITE);

    double cap = std::min(cfg.constraints.maxHoleRatioPerPanel,
                          cfg.constraints.maxTotalLossRatio);
    double loss = 0.0;
    for (int i = 0; i < cfg.panelCount; ++i) {
        const PanelState& p = s.panels[i];
        double vals[] = {p.wear, p.tear, p.hole, p.integrity};
        for (double x : vals) {
            if (Bad(x)) Add(r, Violation::NON_FINITE);
            else if (OutOf01(x)) Add(r, Violation::OUT_OF_RANGE);
        }
        if (p.hole > cap + 1.0e-9) Add(r, Violation::HOLE_CAP_EXCEEDED);
        if (p.holeCount > static_cast<uint32_t>(cfg.constraints.maxHolesPerPanel))
            Add(r, Violation::HOLE_CAP_EXCEEDED);
        if (Bad(p.accumulatedEnergy) || p.accumulatedEnergy < 0.0) Add(r, Violation::NON_FINITE);
        loss += cfg.panels[i].area * p.hole;
    }
    if (loss > cfg.constraints.maxTotalLossRatio + 1.0e-9)
        Add(r, Violation::TOTAL_LOSS_EXCEEDED);
    if (1.0 - loss + 1.0e-9 < cfg.constraints.minCoverageRatio)
        Add(r, Violation::COVERAGE_BELOW_MIN);
    return r;
}

ValidationReport ValidateMonotonicity(const DamageState& before, const DamageState& after) {
    ValidationReport r;
    // A reset is the only legal way for damage to decrease.
    if (after.simTime < before.simTime - 1.0e-9) return r;
    for (int i = 0; i < kMaxPanels; ++i) {
        const PanelState& a = before.panels[i];
        const PanelState& b = after.panels[i];
        if (b.wear + 1.0e-9 < a.wear || b.tear + 1.0e-9 < a.tear ||
            b.hole + 1.0e-9 < a.hole || b.holeCount < a.holeCount ||
            b.hitCount < a.hitCount || b.accumulatedEnergy + 1.0e-9 < a.accumulatedEnergy) {
            Add(r, Violation::NON_MONOTONE);
            break;
        }
    }
    if (after.totalHoleSites < before.totalHoleSites || after.stage < before.stage)
        Add(r, Violation::NON_MONOTONE);
    return r;
}

} // namespace bw
