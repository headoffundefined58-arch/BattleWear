// BattleWear - visual layer: tier -> material / decal / additive-mesh mapping.
//
// Only three render ops exist, all additive or parameter-modifying. There is no
// hide / detach / unload / full-alpha op anywhere in the system, so the base garment
// mesh and material stay bound on every panel in every frame.
// A hole is always drawn as: underlayer (lining fabric) decal first, then a local
// cutout decal on top. The lining is never omitted - a hole never reveals the body.
#include "battlewear/battlewear.h"

#include <algorithm>

namespace bw {

VisualMapper::VisualMapper(const Config& cfg) : cfg_(cfg) {}

double VisualMapper::TierLevel(uint32_t stage, uint32_t prevStage, double blend, uint32_t tier) const {
    if (stage > tier) return 1.0;
    if (stage < tier) return 0.0;
    if (prevStage < tier) return Clamp(blend, 0.0, 1.0);  // cross-fade on promotion
    return 1.0;
}

static const char* OpName(RenderOp op) {
    switch (op) {
        case RenderOp::SET_MATERIAL_PARAM: return "SET_MATERIAL_PARAM";
        case RenderOp::ADD_DECAL: return "ADD_DECAL";
        case RenderOp::BIND_ADDITIVE_MESH: return "BIND_ADDITIVE_MESH";
    }
    return "UNKNOWN";
}

void VisualMapper::Emit(VisualFrame& out, RenderOp op, int panel, const char* role,
                        const char* resource, double p0, double p1, double p2, double p3) const {
    if (out.commandCount >= kMaxCommands) return;
    if (!cfg_.IsCommandAllowed(OpName(op))) return;
    RenderCommand& c = out.commands[out.commandCount++];
    c = RenderCommand();
    c.op = op;
    c.panel = panel;
    CopyName(c.role, role, kShortLen);
    CopyName(c.resource, resource ? resource : "", kNameLen);
    c.p0 = p0; c.p1 = p1; c.p2 = p2; c.p3 = p3;
}

void VisualMapper::BuildFrame(const DamageState& state, const CostumeHandle& costume,
                              VisualFrame& out) const {
    out = VisualFrame();
    out.stage = state.stage;
    out.prevStage = state.prevStage;
    out.stageBlend = Clamp(state.stageBlend, 0.0, 1.0);
    out.heavyGrade = state.heavyGrade;
    out.panelCount = cfg_.panelCount;
    out.garmentAttached = true;   // hard invariant, never set false

    double loss = 0.0, wearSum = 0.0;
    for (int i = 0; i < cfg_.panelCount; ++i) {
        loss += cfg_.panels[i].area * state.panels[i].hole;
        wearSum += cfg_.panels[i].area * state.panels[i].wear;
    }
    out.coverageRatio = Clamp(1.0 - loss, 0.0, 1.0);
    out.globalWear = Clamp(wearSum, 0.0, 1.0);

    const double cap = std::min(cfg_.constraints.maxHoleRatioPerPanel,
                                cfg_.constraints.maxTotalLossRatio);
    const double invCap = 1.0 / std::max(1.0e-6, cap);
    const double l1 = TierLevel(state.stage, state.prevStage, state.stageBlend, 1);
    const double l2 = TierLevel(state.stage, state.prevStage, state.stageBlend, 2);
    const double l3 = TierLevel(state.stage, state.prevStage, state.stageBlend, 3);
    const double gradeBoost = 1.0 + 0.12 * state.heavyGrade;

    for (int i = 0; i < cfg_.panelCount; ++i) {
        const PanelState& ps = state.panels[i];
        const PanelBinding& pb = costume.panels[i];
        PanelVisual& v = out.panels[i];

        CopyName(v.baseMesh, pb.mesh[0] ? pb.mesh : "auto:whole_garment");
        CopyName(v.baseMaterial, pb.material[0] ? pb.material : "auto:whole_garment");
        v.fallbackActive = costume.fallbackUsed;

        v.dirt = Clamp(ps.wear * cfg_.visual.maxDirt * l1, 0.0, 1.0);
        v.roughnessBoost = Clamp(ps.wear * cfg_.visual.maxRoughness * l1, 0.0, 1.0);
        v.desaturation = Clamp(ps.wear * cfg_.visual.maxDesaturation * l1, 0.0, 1.0);
        v.fray = Clamp(ps.tear * cfg_.visual.maxFray * l2 * gradeBoost, 0.0, 1.0);
        v.edgeJitter = Clamp(ps.tear * cfg_.visual.maxEdgeJitter * l2, 0.0, 1.0);
        v.holeCutout = Clamp(ps.hole * invCap * cfg_.visual.maxCutout * l3, 0.0, 1.0);
        v.underlayer = Clamp(v.holeCutout * cfg_.visual.underlayerBoost, 0.0, 1.0);
        if (v.holeCutout > 0.0 && v.underlayer < v.holeCutout)
            v.underlayer = v.holeCutout;         // lining is never weaker than the cutout

        v.holeCount = std::min(static_cast<uint32_t>(kMaxHoles), ps.holeCount);
        for (uint32_t k = 0; k < v.holeCount; ++k) {
            v.holes[k] = ps.holes[k];
            v.holes[k].radius = Clamp(v.holes[k].radius * gradeBoost,
                                      cfg_.visual.radiusMin, cfg_.visual.radiusMax);
        }
        v.useAdditiveFray = pb.hasFrayVariant && v.fray > 0.02;
        v.useDedicatedHoleMask = pb.hasHoleMask && v.holeCutout > 0.02;

        // Base binding is always emitted first: proof that the garment is kept.
        Emit(out, RenderOp::SET_MATERIAL_PARAM, i, "base_keep", v.baseMaterial,
             v.dirt, v.roughnessBoost, v.desaturation, v.fray);

        if (v.dirt > 0.01)
            Emit(out, RenderOp::ADD_DECAL, i, "wear", cfg_.visual.decalWear, v.dirt, 0, 0, 0);
        if (ps.wear > 0.30)
            Emit(out, RenderOp::ADD_DECAL, i, "soot", cfg_.visual.decalSoot,
                 v.dirt * 0.60, 0, 0, 0);
        if (v.fray > 0.01) {
            Emit(out, RenderOp::ADD_DECAL, i, "fray_edge", cfg_.visual.decalFray,
                 v.fray, v.edgeJitter, 0, 0);
            if (v.useAdditiveFray)
                Emit(out, RenderOp::BIND_ADDITIVE_MESH, i, "fray_strip",
                     pb.frayVariant, v.fray, 0, 0, 0);
        }
        if (v.holeCutout > 0.0) {
            for (uint32_t k = 0; k < v.holeCount; ++k) {
                const HoleSite& h = v.holes[k];
                const char* mask = v.useDedicatedHoleMask ? pb.holeMask : cfg_.visual.decalHole;
                // 1) lining fabric (always present), 2) local cutout over the lining.
                Emit(out, RenderOp::ADD_DECAL, i, "underlayer", cfg_.visual.decalUnderlayer,
                     h.u, h.v, h.radius, v.underlayer);
                Emit(out, RenderOp::ADD_DECAL, i, "hole_mask", mask,
                     h.u, h.v, h.radius, v.holeCutout);
            }
        }
    }
}

} // namespace bw
