"""BattleWear - full-progression walkthrough, invariant verification and edge-case suite.

Run:  python tools/verify_flow.py
Outputs (out/): flow_trace.json, edge_cases.json, conformance_vectors.json, test_report.json
Exit code 0 only when every check passes.
"""
from __future__ import annotations

import copy
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from bw_runtime import (  # noqa: E402
    DamageModel, CostumeRegistry, VisualMapper, load_config,
    validate_state, validate_frame, validate_monotonicity, snapshot_state,
    DEFAULT_CONFIG_PATH,
)

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "out")
os.makedirs(OUT, exist_ok=True)

CHECK_COUNT = 0
FAILS = []


def check(ok, name):
    global CHECK_COUNT
    CHECK_COUNT += 1
    if not ok:
        FAILS.append(name)
        print("FAIL ", name)
    return bool(ok)


def check_clean(errs, name):
    # Per-frame invariant assertions are counted but not stored individually.
    if errs:
        return check(False, name + " [" + ",".join(errs) + "]")
    global CHECK_COUNT
    CHECK_COUNT += 1
    return True


CFG = load_config()
PANEL_KEYS = [p["key"] for p in CFG["panels"]]
HIT_NAMES = list(CFG["hitTypes"].keys())
BLUNT, SLASH, PIERCE, HEAT = (HIT_NAMES.index(x) for x in ("blunt", "slash", "pierce", "heat"))
N = len(PANEL_KEYS)


class Harness:
    def __init__(self, cfg=CFG, code="5nrt", variant="default", seed=0x5EED1234):
        self.cfg = cfg
        self.m = DamageModel(cfg, seed)
        self.reg = CostumeRegistry(cfg)
        bindings = os.path.join(ROOT, "config", "costume_bindings.example.json")
        if os.path.exists(bindings):
            self.reg.load_json(bindings)
        self.costume = self.reg.resolve(code, variant)
        self.vm = VisualMapper(cfg, self.m)
        self.trace = []
        self.notes = []
        self.frames = 0

    def hit(self, energy, panel, hit_type, critical=False, time=None):
        self.m.apply_hit(time=time, energy=energy, panel=panel,
                         hit_type=hit_type, critical=critical)

    def step(self, dt=1.0 / 60.0, record=True):
        before = snapshot_state(self.m)
        t_before = self.m.t
        self.m.tick(dt)
        check_clean(validate_state(self.cfg, self.m), "state invariants")
        check_clean(validate_monotonicity(before, snapshot_state(self.m),
                                          t_before, self.m.t), "monotonicity")
        frame = self.vm.build_frame(self.costume)
        check_clean(validate_frame(self.cfg, frame), "frame invariants")
        # Independent re-checks of the non-negotiable constraints.
        if not frame["garmentAttached"]:
            check(False, "garment attached")
        for v in frame["panels"]:
            if not v["baseMesh"] or not v["baseMaterial"]:
                check(False, "base mesh/material bound")
            if v["holeCutout"] > 0 and v["underlayer"] + 1e-9 < v["holeCutout"]:
                check(False, "cutout backed by lining")
        for c in frame["commands"]:
            if c["op"] not in ("SET_MATERIAL_PARAM", "ADD_DECAL", "BIND_ADDITIVE_MESH"):
                check(False, "no forbidden render op")
        self.frames += 1
        if record:
            s = self.m.snapshot()
            s["commands"] = len(frame["commands"])
            s["cutouts"] = sum(1 for c in frame["commands"] if c["role"] == "hole_mask")
            s["linings"] = sum(1 for c in frame["commands"] if c["role"] == "underlayer")
            self.trace.append(s)
        return frame

    def rebind(self, code, variant):
        self.costume = self.reg.resolve(code, variant)
        self.m.reset(0xBEEF42)
        self.notes.append(f"rebind {code}/{variant} -> pristine")


def dump(name, obj):
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)


def short(v):
    return round(float(v), 6)


# ================================================================== 1. config constraints
print("== 1. config and hard constraints ==")
check(CFG["constraints"]["allowGarmentRemoval"] is False, "garment removal disabled")
check(CFG["constraints"]["alwaysBackedByUnderlayer"] is True, "underlayer mandatory")
check(CFG["constraints"]["maxStage"] == 3, "no removal stage exists")
check(abs(sum(p["area"] for p in CFG["panels"]) - 1.0) < 1e-9, "panel areas normalized")
check(set(CFG["visual"]["allowedCommands"]) <=
      {"SET_MATERIAL_PARAM", "ADD_DECAL", "BIND_ADDITIVE_MESH"}, "render op whitelist")
for p in CFG["panels"]:
    for q in p["neighbors"]:
        check(0 <= q < N and q != p["id"], f"adjacency valid: {p['key']}->{q}")
for name, hp in CFG["hitTypes"].items():
    check(abs(hp["wear"] + hp["tear"] + hp["hole"] - 1.0) < 1e-9,
          f"hit channel partition: {name}")

# Try to enable removal / whitelist a hide op inside a config file: must be refused.
evil = copy.deepcopy(CFG)
evil["constraints"]["allowGarmentRemoval"] = True
evil["constraints"]["maxStage"] = 4
evil["visual"]["allowedCommands"] = ["HIDE_MESH", "ADD_DECAL"]
tmp = os.path.join(OUT, "_evil_config.json")
dump("_evil_config.json", evil)
try:
    evil_loaded = load_config(tmp)
    check(evil_loaded["constraints"]["allowGarmentRemoval"] is False,
          "config cannot enable removal")
    check(evil_loaded["constraints"]["maxStage"] == 3, "config cannot add removal stage")
    check("HIDE_MESH" not in evil_loaded["visual"]["allowedCommands"],
          "config cannot whitelist a forbidden op")
finally:
    os.remove(tmp)

# ================================================================== 2. dynamic costume binding
print("== 2. dynamic costume binding ==")
reg = CostumeRegistry(CFG)
bfile = os.path.join(ROOT, "config", "costume_bindings.example.json")
reg.load_json(bfile)
unknown = reg.resolve("totally_new_mod_author", "costume_v12")
check(unknown["fallbackUsed"] is True, "unknown mod -> generic fallback")
check(all(p["mesh"] and p["material"] for p in unknown["panels"]),
      "fallback binds a base mesh+material on every panel")
dedicated = reg.resolve("7brt", "community_jacket_v3")
check(dedicated["dedicated"] and not dedicated["fallbackUsed"], "dedicated costume resolved")
check(dedicated["panels"][0]["frayVariant"] != "", "dedicated fray asset present")
check(dedicated["panels"][2]["mesh"] == dedicated["baseMesh"],
      "unspecified panel inherits base binding")
check(reg.resolve("7brt", "unregistered_variant")["fallbackUsed"],
      "same character / unknown variant -> fallback")

# ================================================================== 3. pristine
print("== 3. initial pristine state ==")
h = Harness()
h.notes.append("S0 pristine")
f0 = h.step()
check(h.m.stage == 0 and h.m.heavy_grade == 0, "starts at stage 0 / grade 0")
check(h.m.wear_mean() == 0 and h.m.tear_mean() == 0 and h.m.hole_mean() == 0,
      "starts with zero damage")
check(abs(h.m.coverage() - 1.0) < 1e-12, "starts at 100% coverage")
check(h.m.hole_count() == 0, "starts with no holes")
check(len(f0["commands"]) == N and
      all(c["role"] == "base_keep" for c in f0["commands"]),
      "pristine frame emits only base-binding commands")

# ================================================================== 4. full progression
print("== 4. full progression S0 -> S1 -> S2 -> S3 -> heavy ==")


def settle(h, frames=30):
    """Let the tier cross-fade finish; a freshly committed stage starts at blend=0."""
    for _ in range(frames):
        h.step()


def drive_until(h, target, max_frames, energy, hit_type, stride, critical, label):
    frames = 0
    while h.m.stage < target and frames < max_frames:
        h.step()
        if frames % stride == 0:
            h.hit(energy, (frames * 7) % N, hit_type,
                  critical and (frames % 5 == 0))
        frames += 1
    settle(h)
    s = h.m.snapshot()
    h.notes.append(f"{label}: reached stage {h.m.stage} at t={short(s['t'])}s "
                   f"after {frames} frames, wear={short(s['wear'])}, "
                   f"tear={short(s['tear'])}, holes={s['holes']}")
    return frames, s


f1, s1 = drive_until(h, 1, 6000, 0.85, BLUNT, 3, False, "S0->S1 轻微磨损")
check(f1 < 6000, "light abrasion reaches S1 in bounded time")
check(s1["stage"] >= 1, "S1 reached")
check(s1["holes"] == 0 and s1["hole"] == 0.0,
      "light abrasion produces no holes (rupture precursor missing)")
check(s1["coverage"] == 1.0, "S1 keeps full coverage")

f2, s2 = drive_until(h, 2, 12000, 4.0, SLASH, 2, False, "S1->S2 边缘撕裂")
check(f2 < 12000, "sharp hits reach S2 in bounded time")
check(s2["stage"] >= 2 and s2["tear"] > 0.10, "S2 reached with accumulated tearing")
check(h.m.tear_panel_count() >= 2, "tearing is coherent across multiple panels")
frame2 = h.vm.build_frame(h.costume)
check(any(c["role"] == "fray_edge" for c in frame2["commands"]), "S2 emits edge fray")
check(any(c["role"] == "wear" for c in frame2["commands"]), "S1 wear persists into S2")
check(all(c["role"] != "hole_mask" for c in frame2["commands"]) or h.m.hole_count() > 0,
      "no hole decal without a rupture event")

f3, s3 = drive_until(h, 3, 18000, 6.0, PIERCE, 1, True, "S2->S3 局部破洞")
check(f3 < 18000, "heavy puncture reaches S3 in bounded time")
check(s3["stage"] == 3 and s3["holes"] >= CFG["stages"]["S3_holeCount"],
      "S3 reached with local holes")
check(s3["coverage"] >= CFG["constraints"]["minCoverageRatio"], "S3 coverage still >= 80%")
frame3 = h.vm.build_frame(h.costume)
cut = sum(1 for c in frame3["commands"] if c["role"] == "hole_mask")
line = sum(1 for c in frame3["commands"] if c["role"] == "underlayer")
check(cut > 0 and line == cut, "every cutout has exactly one lining layer")
check(frame3["garmentAttached"], "garment attached at S3")
check(all(v["baseMesh"] for v in frame3["panels"]), "S3 keeps every base mesh")

# High-intensity sustained fight -> significant, coherent heavy damage.
h.notes.append("high-intensity sustained phase")
phase4_start = len(h.trace)
for i in range(900):
    h.step()
    h.hit(6.0, (i * 5) % N, PIERCE if i % 2 else SLASH, True)
    if i % 4 == 0:
        h.hit(5.0, (i * 11) % N, PIERCE)
s4 = h.m.snapshot()
h.notes.append(f"high intensity: grade={s4['grade']}, holes={s4['holes']}, "
               f"tear={short(s4['tear'])}, coverage={short(s4['coverage'])}")
check(s4["grade"] >= 2, "high intensity yields heavy grade >= 2")
check(s4["holes"] >= CFG["stages"]["heavyGrade"]["grade2_holeCount"],
      "high intensity yields many coherent holes")
check(s4["tear"] > 0.30, "heavy damage includes substantial edge tearing")
check(s4["coverage"] >= CFG["constraints"]["minCoverageRatio"], "coverage bound holds")
check(s4["loss"] <= CFG["constraints"]["maxTotalLossRatio"] + 1e-9, "total loss capped")
frame4 = h.vm.build_frame(h.costume)
check(len(frame4["commands"]) <= 512, "command buffer never overflows")
check(len(frame4["commands"]) > N, "heavy damage emits additional visual layers")
check(all(v["holeCount"] <= CFG["constraints"]["maxHolesPerPanel"]
          for v in frame4["panels"]), "per-panel hole count capped")

# Stage transitions must have happened in order and been cross-faded.
transitions = [(short(t), st) for t, st in h.m.history]
check([st for _, st in transitions] == sorted(st for _, st in transitions),
      "stage transitions are monotone / ordered")
check(transitions[0][1] == 1 and transitions[-1][1] == 3,
      "walkthrough covers 1 -> 2 -> 3")
check(all(0.0 <= x["blend"] <= 1.0 for x in h.trace),
      "stage cross-fade blend stays within [0,1] on every frame")

# ================================================================== 5. long idle
print("== 5. long idle / no healing ==")
h.notes.append("long idle (12s)")
w_before, t_before, stage_before = h.m.wear_mean(), h.m.tear_mean(), h.m.stage
for _ in range(120):
    h.step(0.10)
check(h.m.intensity < 0.02, "intensity decays to ~0 when idle")
check(h.m.wear_mean() >= w_before - 1e-9 and h.m.tear_mean() >= t_before - 1e-9,
      "no spontaneous healing during idle")
check(h.m.stage == stage_before, "stage does not regress during idle")
check(h.m.coverage() >= CFG["constraints"]["minCoverageRatio"], "coverage stable during idle")

# ================================================================== 6. costume hot-swap
print("== 6. costume hot-swap ==")
h.rebind("7brt", "unregistered_variant")
fr = h.step()
check(h.m.stage == 0 and h.m.hole_count() == 0, "new garment starts intact")
check(h.costume["fallbackUsed"], "unknown variant still resolves")
check_clean(validate_frame(CFG, fr), "frame valid after rebind")

# ================================================================== 7. edge cases
print("== 7. edge cases ==")
EDGE = []


def edge(name, ok, detail=""):
    EDGE.append({"case": name, "passed": bool(ok), "detail": detail})
    check(ok, "edge: " + name)


def fresh():
    return Harness()


# numeric abuse
e = fresh()
e.step()
nan, inf = float("nan"), float("inf")
for energy in (0.0, -5.0, nan, inf, 1e300, 1e12):
    e.hit(energy, 0, PIERCE)
for dt in (-1.0, 0.0, nan, inf, 1e9):
    e.step(dt)
edge("zero/negative/NaN/Inf/oversized hit energy", not validate_state(CFG, e.m))
edge("negative/zero/NaN/Inf/oversized dt", e.m.t < 10.0 and e.m.coverage() >= 0.8)
e.hit(3.0, -7, 999, time=-100.0)
e.hit(3.0, 9999, -5, time=1e12)
e.step()
edge("out-of-order / pathological future timestamps",
     not validate_state(CFG, e.m) and e.m.t < 100.0)
edge("invalid panel / hit-type indices", e.m.total_hits >= 2)

# debounce + hysteresis
d = fresh()
d.hit(20.0, 0, PIERCE, True)
d.step()
edge("single burst does not instantly promote (debounce)", d.m.stage == 0)
for _ in range(40):
    d.step()
s = d.m.stage
for _ in range(200):
    d.step(0.05)
edge("stage never demotes (hysteresis + monotone damage)", d.m.stage >= s)

# hit-location diffusion
loc = fresh()
for _ in range(60):
    loc.step()
    loc.hit(2.0, 0, SLASH)
edge("hit-location primary dominance",
     loc.m.ps[0].wear > loc.m.ps[8].wear)
edge("stress diffusion to adjacent panels",
     any(loc.m.ps[q].wear > 0 for q in CFG["panels"][0]["neighbors"]))

# determinism
def run_seed(seed):
    x = fresh()
    x.m.reset(seed)
    for i in range(300):
        x.step()
        x.hit(5.0, (i * 3) % N, PIERCE, True)
    return x


a, b, c = run_seed(12345), run_seed(12345), run_seed(999)
same = all(abs(a.m.ps[i].hole - b.m.ps[i].hole) < 1e-12 and
           all(abs(a.m.ps[i].holes[k].u - b.m.ps[i].holes[k].u) < 1e-12
               for k in range(min(a.m.ps[i].hole_count, b.m.ps[i].hole_count)))
           for i in range(N))
diff = any(abs(a.m.ps[i].holes[0].u - c.m.ps[i].holes[0].u) > 1e-9
           for i in range(N) if a.m.ps[i].hole_count and c.m.ps[i].hole_count)
edge("same seed -> identical damage and hole placement", same)
edge("different seed -> different hole placement", diff)

# frame-rate independence (hits every 0.1 s)
def run_rate(dt, frames_per_hit):
    x = fresh()
    for frame in range(frames_per_hit * 240):
        x.step(dt)
        if frame % frames_per_hit == 0:
            i = frame // frames_per_hit
            x.hit(4.0, (i * 5) % N, SLASH if i % 2 else PIERCE)
    return x


r30, r60 = run_rate(1 / 30, 3), run_rate(1 / 60, 6)
fps_ok = abs(r30.m.t - r60.m.t) < 1e-6
for i in range(N):
    for attr in ("wear", "tear", "hole"):
        if abs(getattr(r30.m.ps[i], attr) - getattr(r60.m.ps[i], attr)) > 2e-6:
            fps_ok = False
edge("frame-rate independence (30 vs 60 fps)", fps_ok)
edge("stage is frame-rate independent", r30.m.stage == r60.m.stage)

# saturation stress
sat = fresh()
for i in range(3000):
    sat.step()
    sat.hit(25.0, i % N, PIERCE, True)
fsat = sat.vm.build_frame(sat.costume)
edge("3000 max-energy hits: coverage >= 80%",
     sat.m.coverage() >= CFG["constraints"]["minCoverageRatio"])
edge("3000 max-energy hits: loss cap respected",
     sat.m.total_loss() <= CFG["constraints"]["maxTotalLossRatio"] + 1e-9)
edge("3000 max-energy hits: no removal stage", sat.m.stage <= 3)
edge("3000 max-energy hits: frame still valid",
     not validate_state(CFG, sat.m) and not validate_frame(CFG, fsat))
edge("3000 max-energy hits: command buffer bounded", len(fsat["commands"]) <= 512)

# dedicated assets, additive only
x = Harness(code="7brt", variant="community_jacket_v3")
for i in range(400):
    x.step()
    x.hit(6.0, 0, PIERCE, True)
fd = x.vm.build_frame(x.costume)
edge("dedicated fray variant bound additively",
     any(c["op"] == "BIND_ADDITIVE_MESH" for c in fd["commands"]))
edge("dedicated hole mask used when available",
     any(c["role"] == "hole_mask" and c["resource"].startswith("bw/7brt")
         for c in fd["commands"]))
edge("dedicated costume still keeps every base mesh",
     all(v["baseMesh"] for v in fd["panels"]) and fd["garmentAttached"])
edge("all render ops are whitelisted",
     set(c["op"] for c in fd["commands"]) <= set(CFG["visual"]["allowedCommands"]))

# missing dedicated resources -> graceful degradation
g = fresh()
fg = g.vm.build_frame(g.costume)
edge("generic fallback never fails to build a frame", len(fg["commands"]) == N)
edge("no hide/detach/unload/alpha-zero op exists in the frame vocabulary",
     all(c["op"] in ("SET_MATERIAL_PARAM", "ADD_DECAL", "BIND_ADDITIVE_MESH")
         for c in fg["commands"]))

# physical precursor ordering
p = fresh()
for _ in range(50):
    p.step()
    p.hit(0.2, 0, BLUNT)
edge("no tear before fabric weakening or sharp stress",
     p.m.ps[0].tear == 0.0 or p.m.ps[0].wear >= CFG["simulation"]["tearOnsetWear"])
edge("no hole before an edge tear precursor",
     p.m.hole_count() == 0)

# lining count always equals cutout count over the whole trace
bad_lining = [x for x in h.trace if x["linings"] != x["cutouts"]]
edge("every frame: lining count == cutout count", not bad_lining)
edge("every frame: coverage >= 0.80",
     all(x["coverage"] >= CFG["constraints"]["minCoverageRatio"] - 1e-9
         for x in h.trace))
edge("every frame: no frame exceeds 512 commands",
     all(x["commands"] <= 512 for x in h.trace))

# ================================================================== outputs
print("== 8. writing artifacts ==")
flow = {
    "config": os.path.relpath(DEFAULT_CONFIG_PATH, ROOT).replace("\\", "/"),
    "stages": CFG["stages"]["labels"],
    "phases": h.notes,
    "transitions": [{"t": t, "stage": st} for t, st in transitions],
    "final": {k: short(v) if isinstance(v, float) else v for k, v in s4.items()},
    "after_costume_hot_swap": {k: short(v) if isinstance(v, float) else v
                               for k, v in h.m.snapshot().items()},
    "trace": h.trace,
}
dump("flow_trace.json", flow)
dump("edge_cases.json", EDGE)
dump("conformance_vectors.json", {
    "config_sha_hint": "battlewear.config.json schemaVersion=1",
    "panels": N,
    "stage_thresholds": {k: CFG["stages"][k] for k in
                         ("S1_wearMean", "S2_tearMean", "S2_tearPanels",
                          "S3_holeCount", "S3_holeMean")},
    "flow_milestones": [s1, s2, s3, s4],
    "saturation": {k: short(v) if isinstance(v, float) else v
                   for k, v in sat.m.snapshot().items()},
    "determinism_samples": [short(a.m.ps[0].holes[0].u) if a.m.ps[0].hole_count else None,
                            short(b.m.ps[0].holes[0].u) if b.m.ps[0].hole_count else None,
                            short(c.m.ps[0].holes[0].u) if c.m.ps[0].hole_count else None],
})
dump("test_report.json", {
    "checks": CHECK_COUNT,
    "failures": len(FAILS),
    "failed": FAILS,
    "invariant_frames_checked": h.frames + e.frames + d.frames + loc.frames +
                                 a.frames + b.frames + c.frames + r30.frames +
                                 r60.frames + sat.frames + x.frames + g.frames,
    "trace_frames": len(h.trace),
    "edge_cases": len(EDGE),
    "edge_cases_passed": sum(1 for x in EDGE if x["passed"]),
})

print("-" * 60)
print(f"checks={CHECK_COUNT} failures={len(FAILS)} "
      f"traceFrames={len(h.trace)} edgeCases={len(EDGE)}")
print(f"transitions={transitions}")
print(f"final={flow['final']}")
sys.exit(0 if not FAILS else 1)
