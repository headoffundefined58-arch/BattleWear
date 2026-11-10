"""BattleWear - executable reference runtime (Python toolchain / conformance oracle).

Mirrors the C++17 production module in src/*.cpp formula-for-formula, using the same
config/battlewear.config.json. It is used offline to:
  * validate configs and generate the costume variant table,
  * reproduce the full pristine -> heavy damage walkthrough headlessly,
  * assert every hard invariant per frame and emit conformance vectors / traces.

Hard invariants (never violated):
  * garment always attached (base mesh + base material on every panel, every frame)
  * no removal / detach / unload / full-alpha state exists
  * every local cutout is backed by a lining-fabric layer
  * damage is monotone within a battle
  * coverage >= minCoverageRatio, total loss <= maxTotalLossRatio
"""
from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass, field

MASK64 = (1 << 64) - 1
NO_HOLE_TIME = -1.0e9
MAX_HOLES = 8
MAX_PANELS = 32


# ------------------------------------------------------------------ utilities

def clamp(v, lo, hi):
    if not (v >= lo):          # also catches NaN
        return lo
    return hi if v > hi else v


def finite(v, fallback=0.0):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return fallback
    if math.isnan(f) or math.isinf(f) or abs(f) > 1.0e300:
        return fallback
    return f


def safe_pow(base, exp):
    if not (base > 0.0):
        return 0.0
    return math.pow(base, exp)


def monotone(previous, candidate):
    return max(previous, finite(candidate, previous))


def hash64(a, b, c):
    h = ((a * 0x9E3779B97F4A7C15) ^ (b + 0xBF58476D1CE4E5B9) ^ (c << 21)) & MASK64
    h ^= h >> 30
    h = (h * 0xBF58476D1CE4E5B9) & MASK64
    h ^= h >> 27
    h = (h * 0x94D049BB133111EB) & MASK64
    h ^= h >> 31
    return h


def rand01(seed, panel, site, lane):
    return (hash64(seed, (panel << 32) | site, lane) >> 11) / 9007199254740992.0


# ------------------------------------------------------------------ config

DEFAULT_CONFIG_PATH = os.path.join(os.path.dirname(__file__), "..", "config",
                                   "battlewear.config.json")


def load_config(path=DEFAULT_CONFIG_PATH):
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    # Config-level hard constraints are enforced regardless of file contents.
    c = cfg["constraints"]
    c["allowGarmentRemoval"] = False
    c["alwaysBackedByUnderlayer"] = True
    c["maxStage"] = 3
    allowed = {"SET_MATERIAL_PARAM", "ADD_DECAL", "BIND_ADDITIVE_MESH"}
    cfg["visual"]["allowedCommands"] = [x for x in cfg["visual"]["allowedCommands"]
                                        if x in allowed]
    panels = cfg["panels"]
    total = sum(p["area"] for p in panels)
    for p in panels:
        p["area"] = p["area"] / total
    return cfg


# ------------------------------------------------------------------ logic layer

@dataclass
class HoleSite:
    u: float = 0.0
    v: float = 0.0
    radius: float = 0.0
    born_tick: int = 0


@dataclass
class PanelState:
    wear: float = 0.0
    tear: float = 0.0
    hole: float = 0.0
    accumulated_energy: float = 0.0
    last_hit_time: float = 0.0
    last_hole_time: float = NO_HOLE_TIME
    hit_count: int = 0
    hole_count: int = 0
    integrity: float = 1.0
    holes: list = field(default_factory=lambda: [HoleSite() for _ in range(MAX_HOLES)])


class DamageModel:
    def __init__(self, cfg, seed=0x5EED1234):
        self.cfg = cfg
        self.sim = cfg["simulation"]
        self.cons = cfg["constraints"]
        self.stages = cfg["stages"]
        self.visual = cfg["visual"]
        self.panels = cfg["panels"]
        self.n = len(self.panels)
        self.hit_types = list(cfg["hitTypes"].items())
        self.cap = min(self.cons["maxHoleRatioPerPanel"],
                       self.cons["maxTotalLossRatio"])
        self.reset(seed)

    def reset(self, seed=0x5EED1234):
        self.t = 0.0
        self.intensity = 0.0
        self.last_intensity_time = 0.0
        self.total_hits = 0
        self.total_hole_sites = 0
        self.tick_index = 0
        self.stage = 0
        self.prev_stage = 0
        self.pending_stage = 0
        self.pending_stage_time = 0.0
        self.stage_blend = 1.0
        self.heavy_grade = 0
        self.seed = seed or 0x5EED1234
        self.active = True
        self.ps = [PanelState() for _ in range(self.n)]
        self.history = []

    # ---- aggregates
    def wear_mean(self): return sum(p["area"] * s.wear for p, s in zip(self.panels, self.ps))
    def tear_mean(self): return sum(p["area"] * s.tear for p, s in zip(self.panels, self.ps))
    def hole_mean(self): return sum(p["area"] * s.hole for p, s in zip(self.panels, self.ps))
    def tear_panel_count(self): return sum(1 for s in self.ps if s.tear >= 0.20)
    def hole_count(self): return sum(s.hole_count for s in self.ps)
    def total_loss(self): return sum(p["area"] * s.hole for p, s in zip(self.panels, self.ps))
    def coverage(self): return 1.0 - self.total_loss()

    def decay_intensity_to(self, t):
        dt = t - self.last_intensity_time
        if dt > 0:
            tau = max(0.01, self.sim["intensityTauSeconds"])
            self.intensity = clamp(self.intensity * math.exp(-dt / tau), 0.0, 1.0)
        self.last_intensity_time = t

    def _neighbor_hole_fraction(self, idx):
        nb = self.panels[idx]["neighbors"]
        if not nb:
            return 0.0
        return sum(1 for q in nb if 0 <= q < self.n and self.ps[q].hole_count > 0) / len(nb)

    def _distribute_hole_sites(self, s, idx):
        ratio = clamp(s.hole / self.cap, 0.0, 1.0)
        vc = self.visual["hole"]
        for k in range(min(s.hole_count, MAX_HOLES)):
            jitter = 0.60 + 0.40 * rand01(self.seed, idx, k, 7)
            s.holes[k].radius = clamp(
                vc["radiusMin"] + ratio * (vc["radiusMax"] - vc["radiusMin"]) * jitter,
                vc["radiusMin"], vc["radiusMax"])

    def _ensure_hole_sites(self, s, idx, time):
        if s.hole_count == 0:
            return
        allowed = min(MAX_HOLES, self.cons["maxHolesPerPanel"])
        step = self.cap / max(1, allowed)
        desired = int(clamp(int(s.hole / step) + 1, 1, allowed))
        vc = self.visual["hole"]
        while s.hole_count < desired:
            if time - s.last_hole_time < self.sim["holeCooldownSeconds"]:
                break
            k = s.hole_count
            best_u, best_v, best_d = 0.5, 0.5, -1.0
            for attempt in range(24):
                u = 0.12 + 0.76 * rand01(self.seed, idx, k * 32 + attempt, 1)
                v = 0.12 + 0.76 * rand01(self.seed, idx, k * 32 + attempt, 2)
                d = min([math.hypot(u - s.holes[j].u, v - s.holes[j].v)
                         for j in range(s.hole_count)] or [1.0])
                if d > best_d:
                    best_d, best_u, best_v = d, u, v
                if d >= vc["minUVSpacing"]:
                    break
            site = s.holes[k]
            site.u, site.v, site.born_tick = best_u, best_v, self.tick_index
            s.last_hole_time = time
            s.hole_count += 1
            self.total_hole_sites += 1
        self._distribute_hole_sites(s, idx)

    def _apply_to_panel(self, idx, time, E, hp, I):
        if idx < 0 or idx >= self.n or not (E > 0.0):
            return
        s = self.ps[idx]
        k = self.sim
        Ew, Et, Eh = E * hp["wear"], E * hp["tear"], E * hp["hole"]
        G = clamp(1.0 - 0.45 * s.wear - 0.35 * s.tear - 0.55 * s.hole,
                  max(0.05, k["minIntegrity"]), 1.0)
        s.integrity = G

        # 1) surface abrasion: saturating, diminishing returns
        dW = k["wearGain"] * Ew * safe_pow(max(0.0, 1.0 - s.wear),
                                           k["wearSaturationPower"])
        s.wear = monotone(s.wear, clamp(s.wear + dW, 0.0, 1.0))

        # 2) edge tear: crack-tip (Paris-like) propagation
        nucleates = s.wear >= k["tearOnsetWear"] or (
            hp["sharp"] and Et >= k["sharpNucleationEnergy"])
        if nucleates:
            crack_tip = safe_pow(s.tear + k["tearNucleus"], k["tearCrackExponent"])
            dT = (k["tearGain"] * safe_pow(Et, k["tearEnergyExponent"]) *
                  crack_tip / G * max(0.0, 1.0 - s.tear))
            s.tear = monotone(s.tear, clamp(s.tear + dT, 0.0, 1.0))

        # 3) local rupture: global loss budget by construction (areas sum to 1)
        loss_elsewhere = sum(self.panels[j]["area"] * self.ps[j].hole
                             for j in range(self.n) if j != idx)
        room = self.cons["maxTotalLossRatio"] - loss_elsewhere
        cap_here = min(self.cap, max(0.0, room) / max(1e-6, self.panels[idx]["area"]))
        if not (cap_here > s.hole):
            cap_here = s.hole
        coherence = 1.0 - k["holeSiteCoherenceGain"] * I * self._neighbor_hole_fraction(idx)
        fatigue_relief = 1.0 - 0.55 * I
        need = k["holeNucleationEnergy"] * max(0.20, coherence) * max(0.20, fatigue_relief)
        cyclic_drive = 0.35 * E * I * s.tear
        drive = Eh + cyclic_drive
        can_rupture = s.tear >= k["holeOnsetTear"] and drive >= need
        if can_rupture:
            if s.hole_count == 0:
                s.hole = monotone(s.hole, min(k["holeNucleus"], cap_here))
                s.hole_count = 1
                s.last_hole_time = time
                self.total_hole_sites += 1
                s.holes[0].u = 0.30 + 0.40 * rand01(self.seed, idx, 0, 3)
                s.holes[0].v = 0.30 + 0.40 * rand01(self.seed, idx, 0, 4)
                s.holes[0].born_tick = self.tick_index
                self._distribute_hole_sites(s, idx)
            else:
                room_factor = max(0.0, 1.0 - s.hole / max(1e-6, cap_here))
                dH = (k["holeGain"] * safe_pow(drive, 0.90) *
                      safe_pow(s.hole + k["holeNucleus"], k["holeGrowthExponent"]) /
                      G * room_factor)
                s.hole = monotone(s.hole, clamp(s.hole + dH, 0.0, cap_here))
            self._ensure_hole_sites(s, idx, time)

        s.accumulated_energy = monotone(s.accumulated_energy,
                                        s.accumulated_energy + E)
        s.last_hit_time = time
        s.hit_count += 1

    def apply_hit(self, time=None, energy=0.0, panel=0, hit_type=0, critical=False,
                  attacker_id=0):
        if not self.active:
            self.reset(self.seed)
        raw = finite(self.t if time is None else time, self.t)
        t = max(raw, self.last_intensity_time)
        t = min(t, self.t + 10.0)
        while t > self.t + 1e-12:
            self.tick(min(t - self.t, self.sim["dtMax"]))

        idx = int(clamp(finite(panel, 0.0), 0.0, float(self.n - 1)))
        if hit_type < 0 or hit_type >= len(self.hit_types):
            hit_type = 0
        hp = self.hit_types[hit_type][1]
        energy = clamp(finite(energy, 0.0), 0.0, self.sim["maxHitEnergy"])
        E = energy * self.sim["energyScale"] * hp["energy"]
        if critical:
            E *= self.sim["criticalMultiplier"]

        tau = max(0.01, self.sim["intensityTauSeconds"])
        norm = max(0.01, self.sim["intensityNorm"])
        if E > 0.0:
            self.decay_intensity_to(min(t, self.t))
            self.intensity = clamp(self.intensity + E / (norm * tau), 0.0, 1.0)
        I = self.intensity
        fatigue = 1.0 + self.sim["fatigueGain"] * I
        Eeff = E * fatigue
        self.total_hits += 1

        self._apply_to_panel(idx, t, Eeff, hp, I)
        for q in self.panels[idx]["neighbors"]:
            if q != idx and 0 <= q < self.n:
                self._apply_to_panel(q, t, Eeff * self.sim["adjacencyWeight"], hp, I)

    def compute_target_stage(self):
        st = self.stages
        w, t, h = self.wear_mean(), self.tear_mean(), self.hole_mean()
        tp, holes = self.tear_panel_count(), self.hole_count()
        cand = 0
        if w >= st["S1_wearMean"]:
            cand = 1
        if t >= st["S2_tearMean"] and tp >= st["S2_tearPanels"]:
            cand = 2
        if holes >= st["S3_holeCount"] and h >= st["S3_holeMean"]:
            cand = 3
        if cand < self.stage:
            f = st["hysteresis"]
            if self.stage >= 3 and holes >= st["S3_holeCount"] and h >= st["S3_holeMean"] * f:
                cand = 3
            elif self.stage >= 2 and t >= st["S2_tearMean"] * f and tp >= st["S2_tearPanels"]:
                cand = 2
            elif self.stage >= 1 and w >= st["S1_wearMean"] * f:
                cand = 1
        return int(clamp(cand, 0, self.cons["maxStage"]))

    def compute_heavy_grade(self):
        st = self.stages["heavyGrade"]
        holes, I, t = self.hole_count(), self.intensity, self.tear_mean()
        g = 0
        if holes >= st["grade1_holeCount"]:
            g = 1
        if holes >= st["grade2_holeCount"] or (holes >= st["grade1_holeCount"] and I >= 0.50):
            g = 2
        if holes >= st["grade3_holeCount"] or (
                I >= st["grade3_intensity"] and t >= st["grade3_tearMean"]
                and holes >= st["grade2_holeCount"]):
            g = 3
        return int(clamp(g, 0, 3))

    def tick(self, dt):
        d = clamp(finite(dt, 0.0), 0.0, max(0.001, self.sim["dtMax"]))
        self.t += d
        self.tick_index += 1
        self.decay_intensity_to(self.t)
        if self.stage_blend < 1.0:
            self.stage_blend = clamp(
                self.stage_blend + d / max(0.001, self.visual["blendSeconds"]), 0.0, 1.0)
        target = self.compute_target_stage()
        if target != self.stage:
            if target != self.pending_stage:
                self.pending_stage, self.pending_stage_time = target, 0.0
            self.pending_stage_time += d
            if self.pending_stage_time >= self.sim["stageDwellSeconds"]:
                self.prev_stage = self.stage
                self.stage = target
                self.stage_blend = 0.0
                self.pending_stage, self.pending_stage_time = target, 0.0
                self.history.append((self.t, target))
        else:
            self.pending_stage, self.pending_stage_time = self.stage, 0.0
        self.heavy_grade = self.compute_heavy_grade()

    def snapshot(self):
        return {
            "t": round(self.t, 6), "stage": self.stage, "grade": self.heavy_grade,
            "wear": self.wear_mean(), "tear": self.tear_mean(), "hole": self.hole_mean(),
            "intensity": self.intensity, "coverage": self.coverage(),
            "holes": self.hole_count(), "loss": self.total_loss(),
            "blend": self.stage_blend,
        }


# ------------------------------------------------------------------ costume binding

class CostumeRegistry:
    def __init__(self, cfg):
        self.cfg = cfg
        self.panel_keys = [p["key"] for p in cfg["panels"]]
        self.entries = {}
        self.fallback_count = 0

    @staticmethod
    def _generic(code, variant, n):
        return {
            "characterCode": code, "variantId": variant,
            "baseMesh": "auto:whole_garment", "baseMaterial": "auto:whole_garment",
            "dedicated": False, "fallbackUsed": True,
            "panels": [{"mesh": "auto:whole_garment", "material": "auto:whole_garment",
                        "uvSet": "uv0", "frayVariant": "", "holeMask": ""} for _ in range(n)],
        }

    def load_json(self, path):
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
        for item in doc.get("costumes", []):
            code = item.get("characterCode", "unknown")
            variant = item.get("variantId", "default")
            h = self._generic(code, variant, len(self.panel_keys))
            h["baseMesh"] = item.get("baseMesh", h["baseMesh"])
            h["baseMaterial"] = item.get("baseMaterial", h["baseMaterial"])
            h["dedicated"] = item.get("damageAssets") == "dedicated"
            h["fallbackUsed"] = False
            for p in h["panels"]:
                p["mesh"], p["material"] = h["baseMesh"], h["baseMaterial"]
            for key, val in item.get("panels", {}).items():
                if key not in self.panel_keys:
                    continue
                b = h["panels"][self.panel_keys.index(key)]
                b["mesh"] = val.get("mesh", b["mesh"])
                b["material"] = val.get("material", b["material"])
                b["uvSet"] = val.get("uv", b["uvSet"])
                b["frayVariant"] = val.get("frayVariant", "")
                b["holeMask"] = val.get("holeMask", "")
            self.entries[(code, variant)] = h

    def resolve(self, code="unknown", variant="default"):
        key = (code, variant)
        if key in self.entries:
            h = json.loads(json.dumps(self.entries[key]))
            h["fallbackUsed"] = False
            return h
        self.fallback_count += 1
        return self._generic(code, variant, len(self.panel_keys))


# ------------------------------------------------------------------ visual layer

def tier_level(stage, prev_stage, blend, tier):
    if stage > tier:
        return 1.0
    if stage < tier:
        return 0.0
    if prev_stage < tier:
        return clamp(blend, 0.0, 1.0)
    return 1.0


class VisualMapper:
    def __init__(self, cfg, model):
        self.cfg = cfg
        self.model = model
        self.vc = cfg["visual"]
        self.allowed = set(cfg["visual"]["allowedCommands"])

    def _emit(self, commands, op, panel, role, resource, p0=0, p1=0, p2=0, p3=0):
        if len(commands) >= 512 or op not in self.allowed:
            return
        commands.append({"op": op, "panel": panel, "role": role,
                         "resource": resource, "p": [p0, p1, p2, p3]})

    def build_frame(self, costume):
        m, vc = self.model, self.vc
        l1 = tier_level(m.stage, m.prev_stage, m.stage_blend, 1)
        l2 = tier_level(m.stage, m.prev_stage, m.stage_blend, 2)
        l3 = tier_level(m.stage, m.prev_stage, m.stage_blend, 3)
        grade_boost = 1.0 + 0.12 * m.heavy_grade
        cap = min(m.cons["maxHoleRatioPerPanel"], m.cons["maxTotalLossRatio"])
        decals = vc["genericDecals"]
        commands, panels = [], []
        for i, (pd, ps, pb) in enumerate(zip(m.panels, m.ps, costume["panels"])):
            v = {
                "baseMesh": pb["mesh"] or "auto:whole_garment",
                "baseMaterial": pb["material"] or "auto:whole_garment",
                "dirt": clamp(ps.wear * vc["wear"]["maxDirt"] * l1, 0, 1),
                "roughnessBoost": clamp(ps.wear * vc["wear"]["maxRoughness"] * l1, 0, 1),
                "desaturation": clamp(ps.wear * vc["wear"]["maxDesaturation"] * l1, 0, 1),
                "fray": clamp(ps.tear * vc["tear"]["maxFray"] * l2 * grade_boost, 0, 1),
                "edgeJitter": clamp(ps.tear * vc["tear"]["maxEdgeJitter"] * l2, 0, 1),
                "holeCutout": clamp(ps.hole / cap * vc["hole"]["maxCutout"] * l3, 0, 1),
                "fallbackActive": costume["fallbackUsed"],
            }
            v["underlayer"] = max(v["holeCutout"],
                                  clamp(v["holeCutout"] * vc["hole"]["underlayerBoost"], 0, 1))
            sites = []
            for k in range(min(ps.hole_count, MAX_HOLES)):
                s = ps.holes[k]
                sites.append({"u": s.u, "v": s.v,
                              "radius": clamp(s.radius * grade_boost,
                                              vc["hole"]["radiusMin"], vc["hole"]["radiusMax"])})
            v["holes"] = sites
            v["holeCount"] = len(sites)
            v["useAdditiveFray"] = bool(pb["frayVariant"]) and v["fray"] > 0.02
            v["useDedicatedHoleMask"] = bool(pb["holeMask"]) and v["holeCutout"] > 0.02
            panels.append(v)

            self._emit(commands, "SET_MATERIAL_PARAM", i, "base_keep", v["baseMaterial"],
                       v["dirt"], v["roughnessBoost"], v["desaturation"], v["fray"])
            if v["dirt"] > 0.01:
                self._emit(commands, "ADD_DECAL", i, "wear", decals["wear"], v["dirt"])
            if ps.wear > 0.30:
                self._emit(commands, "ADD_DECAL", i, "soot", decals["soot"], v["dirt"] * 0.60)
            if v["fray"] > 0.01:
                self._emit(commands, "ADD_DECAL", i, "fray_edge", decals["fray"],
                           v["fray"], v["edgeJitter"])
                if v["useAdditiveFray"]:
                    self._emit(commands, "BIND_ADDITIVE_MESH", i, "fray_strip",
                               pb["frayVariant"], v["fray"])
            if v["holeCutout"] > 0.0:
                mask = pb["holeMask"] if v["useDedicatedHoleMask"] else decals["hole"]
                for s in sites:
                    self._emit(commands, "ADD_DECAL", i, "underlayer", decals["underlayer"],
                               s["u"], s["v"], s["radius"], v["underlayer"])
                    self._emit(commands, "ADD_DECAL", i, "hole_mask", mask,
                               s["u"], s["v"], s["radius"], v["holeCutout"])
        return {
            "stage": m.stage, "prevStage": m.prev_stage, "stageBlend": m.stage_blend,
            "heavyGrade": m.heavy_grade, "globalWear": m.wear_mean(),
            "coverageRatio": m.coverage(), "garmentAttached": True,
            "panels": panels, "commands": commands,
        }


# ------------------------------------------------------------------ invariants

def bad(x):
    return not isinstance(x, (int, float)) or math.isnan(x) or math.isinf(x)


def out01(x):
    return bad(x) or x < -1e-9 or x > 1.0 + 1e-9


def validate_state(cfg, m):
    errs = []
    if m.stage > cfg["constraints"]["maxStage"]:
        errs.append("STAGE_OUT_OF_RANGE")
    if m.heavy_grade > 3 or out01(m.intensity) or out01(m.stage_blend):
        errs.append("OUT_OF_RANGE")
    cap = min(cfg["constraints"]["maxHoleRatioPerPanel"],
              cfg["constraints"]["maxTotalLossRatio"])
    for s in m.ps:
        for x in (s.wear, s.tear, s.hole, s.integrity):
            if bad(x):
                errs.append("NON_FINITE")
            elif out01(x):
                errs.append("OUT_OF_RANGE")
        if s.hole > cap + 1e-9 or s.hole_count > cfg["constraints"]["maxHolesPerPanel"]:
            errs.append("HOLE_CAP_EXCEEDED")
    if m.total_loss() > cfg["constraints"]["maxTotalLossRatio"] + 1e-9:
        errs.append("TOTAL_LOSS_EXCEEDED")
    if m.coverage() + 1e-9 < cfg["constraints"]["minCoverageRatio"]:
        errs.append("COVERAGE_BELOW_MIN")
    return sorted(set(errs))


def validate_frame(cfg, frame):
    errs = []
    allowed = set(cfg["visual"]["allowedCommands"])
    if not frame["garmentAttached"]:
        errs.append("GARMENT_REMOVED")
    if frame["coverageRatio"] + 1e-9 < cfg["constraints"]["minCoverageRatio"]:
        errs.append("COVERAGE_BELOW_MIN")
    for v in frame["panels"]:
        if not v["baseMesh"]:
            errs.append("BASE_MESH_MISSING")
        if not v["baseMaterial"]:
            errs.append("BASE_MATERIAL_MISSING")
        for x in (v["dirt"], v["roughnessBoost"], v["desaturation"], v["fray"],
                  v["edgeJitter"], v["holeCutout"], v["underlayer"]):
            if bad(x):
                errs.append("NON_FINITE")
            elif out01(x):
                errs.append("OUT_OF_RANGE")
        if v["holeCutout"] > 0.0 and v["underlayer"] + 1e-9 < v["holeCutout"]:
            errs.append("UNDERLAYER_MISSING")
        if v["holeCount"] > cfg["constraints"]["maxHolesPerPanel"]:
            errs.append("HOLE_CAP_EXCEEDED")
        for s in v["holes"]:
            for x in (s["u"], s["v"], s["radius"]):
                if bad(x):
                    errs.append("NON_FINITE")
                elif out01(x):
                    errs.append("OUT_OF_RANGE")
    for c in frame["commands"]:
        if c["op"] not in allowed:
            errs.append("FORBIDDEN_RENDER_OP")
        if not c["resource"]:
            errs.append("BASE_MATERIAL_MISSING")
        if any(bad(x) for x in c["p"]):
            errs.append("NON_FINITE")
    if len(frame["commands"]) > 512:
        errs.append("FRAME_OVERFLOW")
    return sorted(set(errs))


def snapshot_state(m):
    return [(s.wear, s.tear, s.hole, s.hole_count, s.hit_count) for s in m.ps]


def validate_monotonicity(before, after, before_time, after_time):
    if after_time < before_time - 1e-9:
        return []          # explicit reset
    errs = []
    for a, b in zip(before, after):
        if (b[0] + 1e-9 < a[0] or b[1] + 1e-9 < a[1] or b[2] + 1e-9 < a[2]
                or b[3] < a[3] or b[4] < a[4]):
            errs.append("NON_MONOTONE")
            break
    return errs
