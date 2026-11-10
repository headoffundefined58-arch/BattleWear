// BattleWear - visual layer: tier -> material / decal / geometry mapping.
//
// Two families of render ops exist, both additive or parameter-modifying:
//   * decals and material params - surface condition (wear, soot, roughness)
//   * geometry ops - real structure (cut polygons, dangling strips, native _dmg0N layer)
// There is no hide / detach / unload / full-alpha op anywhere in the system, so the base
// garment mesh and material stay bound on every panel in every frame.
//
// Universal destruction: a costume does NOT need authored damage assets. If the costume
// ships a native _dmg0N chain it is bound on top of the base mesh (higher fidelity), and
// the tear / hole / strips are generated procedurally in every other case.
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

void VisualMapper::Emit(VisualFrame& out, RenderOp op, int panel, const char* role,
                        const char* resource, double p0, double p1, double p2, double p3) const {
    if (out.commandCount >= kMaxCommands) return;
    if (!cfg_.IsCommandAllowed(RenderOpName(op))) return;
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

    // Hard budget for cut-away geometry, on top of the logical hole budget.
    double cutBudget = cfg_.constraints.maxTotalLossRatio;

    for (int i = 0; i < cfg_.panelCount; ++i) {
        const PanelState& ps = state.panels[i];
        const PanelBinding& pb = costume.panels[i];
        PanelVisual& v = out.panels[i];
        const bool core = cfg_.panels[i].core;
        const double area = std::max(1.0e-6, cfg_.panels[i].area);

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

        // --- structural damage: no authored asset required on any costume ------------
        // A native variant chain, when present, is an *additional* layer over the base
        // mesh; it never replaces the base binding.
        v.nativeVariantActive = state.stage >= 1 &&
            (costume.variantMask & (1u << (state.stage < 3 ? state.stage : 3u))) != 0;

        // Limbs and hems may lose more than the body: the outfit stays on, the fragment goes.
        const double cutScale = core ? 1.0 : 1.35;
        const double want = Clamp(ps.hole * cutScale * l3, 0.0,
                                  cfg_.visual.maxPanelRemoval);
        const double roomHere = std::max(0.0, cutBudget) / area;
        v.cutFraction = Clamp(std::min(want, std::min(cfg_.visual.maxPanelRemoval, roomHere)),
                              0.0, cfg_.visual.maxPanelRemoval);
        cutBudget -= area * v.cutFraction;

        v.stripped = cfg_.visual.proceduralStub && !core &&
                     ps.tear >= cfg_.visual.stripTearThreshold &&
                     v.cutFraction >= 0.5 * cfg_.visual.maxPanelRemoval;

        const bool proceduralOn = cfg_.visual.proceduralCutout || cfg_.visual.proceduralFray;
        if (v.cutFraction > 0.0 || v.fray > 0.02) {
            if (v.nativeVariantActive)
                v.tearSource = proceduralOn ? TearSource::HYBRID : TearSource::NATIVE_VARIANT;
            else
                v.tearSource = TearSource::PROCEDURAL;
        } else {
            v.tearSource = TearSource::NONE;
        }

        v.useAdditiveFray = (v.fray > 0.02) && (pb.hasFrayVariant || cfg_.visual.proceduralFray);
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
            if (v.useAdditiveFray) {
                const char* strip = pb.hasFrayVariant ? pb.frayVariant : cfg_.visual.procFrayMesh;
                Emit(out, RenderOp::BIND_ADDITIVE_MESH, i, "fray_strip", strip,
                     v.fray,
                     static_cast<double>(cfg_.visual.stripCountPerHole),
                     cfg_.visual.stripLength, cfg_.visual.stripDroop);
            }
        }

        if (v.nativeVariantActive) {
            Emit(out, RenderOp::BIND_DAMAGE_VARIANT, i, "damage_variant",
                 costume.stageVariant[state.stage < 3 ? state.stage : 3],
                 static_cast<double>(state.stage), out.stageBlend,
                 static_cast<double>(costume.variantMask), 0.0);
        }

        if (v.holeCutout > 0.0) {
            for (uint32_t k = 0; k < v.holeCount; ++k) {
                const HoleSite& h = v.holes[k];
                // 1) lining fabric first: it is always present, a hole never reaches skin.
                Emit(out, RenderOp::ADD_DECAL, i, "underlayer", cfg_.visual.decalUnderlayer,
                     h.u, h.v, h.radius, v.underlayer);
                if (v.useDedicatedHoleMask) {
                    // Authored mask (only when the modder shipped one).
                    Emit(out, RenderOp::ADD_DECAL, i, "hole_mask", pb.holeMask,
                         h.u, h.v, h.radius, v.holeCutout);
                }
                // 2) the opening itself. With an authored mask this is still a real cut,
                //    so the hole is see-through instead of painted on.
                if (cfg_.visual.proceduralCutout) {
                    const char* cutRes = cfg_.visual.procCutPolygon;
                    const char* role = v.useDedicatedHoleMask ? "cut_polygon" : "hole_mask";
                    Emit(out, RenderOp::CUT_GEOMETRY, i, role, cutRes,
                         h.u, h.v,
                         h.radius * (1.0 + cfg_.visual.cutJaggedness),
                         v.cutFraction);
                    if (cfg_.visual.stripCountPerHole > 0 && v.fray > 0.05) {
                        Emit(out, RenderOp::BIND_ADDITIVE_MESH, i, "hole_strip",
                             cfg_.visual.procFrayMesh, h.u, h.v, cfg_.visual.stripLength,
                             cfg_.visual.stripDroop);
                    }
                }
            }
        }

        if (v.stripped) {
            // Distal piece is gone: only the stub at the seam remains attached.
            Emit(out, RenderOp::BIND_ADDITIVE_MESH, i, "stub_hem", cfg_.visual.procStubMesh,
                 v.cutFraction, static_cast<double>(cfg_.visual.stripCountPerHole),
                 cfg_.visual.stripLength, cfg_.visual.stripDroop);
        }
    }
}

} // namespace bw
