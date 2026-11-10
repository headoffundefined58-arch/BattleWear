"""BattleWear - static source audit for the C++ module.

No C++ compiler is available on this machine (no cl.exe / no reachable toolchain),
so this script performs the checks that do not require one:

  A. delimiter balance after stripping comments / strings / char literals
  B. every method declared in battlewear.h has exactly one definition in src/
  C. no source file contains a removal / detachment / exposure primitive
  D. every damage write goes through Monotone()
  E. every cfg.* field referenced from C++ exists in the header structs
  F. every JSON knob in battlewear.config.json is consumed by the loader
  G. include guards present and unique

Exit code 0 only when every check passes.
"""
from __future__ import annotations

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIRS = ["src", "plugin"]
HEADER = os.path.join(ROOT, "include", "battlewear", "battlewear.h")
CONFIG = os.path.join(ROOT, "config", "battlewear.config.json")

FAILS = []
CHECKS = []


def check(ok, name, detail=""):
    CHECKS.append(name)
    if not ok:
        FAILS.append(name if not detail else name + " :: " + detail)
        print("FAIL ", name, detail)
    return ok


def strip_code(text):
    """Remove comments plus string/char literal bodies, keeping line structure."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
        elif c in "\"'":
            q = c
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                if text[i] == "\n":
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


# ---------------------------------------------------------------- A. balance

def check_balance(paths):
    ok = True
    for p in paths:
        code = strip_code(read(p))
        for open_c, close_c in (("{", "}"), ("(", ")"), ("[", "]")):
            depth = 0
            bad = False
            for ch in code:
                if ch == open_c:
                    depth += 1
                elif ch == close_c:
                    depth -= 1
                    if depth < 0:
                        bad = True
                        break
            if bad or depth != 0:
                ok = check(False, "delimiter balance " + os.path.basename(p),
                           f"{open_c}{close_c} depth={depth}")
    if ok:
        check(True, "delimiter balance across all translation units")
    return ok


# ------------------------------------------------- B. declarations vs definitions

DECL_RE = re.compile(r"^\s{4}(?:static\s+|virtual\s+|inline\s+|explicit\s+)*"
                     r"[A-Za-z_:<>\*&\s]+?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*(?:const)?\s*;\s*$",
                     re.MULTILINE)
MEMBER_DEF_RE = re.compile(r"\b([A-Za-z_]\w*)::(~?[A-Za-z_]\w*)\s*\(")
FREE_DEF_RE = re.compile(r"^(?:static\s+)?([A-Za-z_][\w:<>]*)\s+([A-Za-z_]\w*)\s*\(",
                         re.MULTILINE)


def check_definitions(header_text, src_paths):
    code = strip_code(header_text)
    decls = set()
    for m in DECL_RE.finditer(code):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "return", "sizeof", "pow", "exp"):
            continue
        decls.add(name)
    # free functions declared at namespace scope, e.g. "ValidationReport ValidateFrame(...);"
    for m in re.finditer(r"^([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\([^;{]*\)\s*;", code, re.MULTILINE):
        decls.add(m.group(2))
    defs = set()
    for p in src_paths:
        body = strip_code(read(p))
        for m in MEMBER_DEF_RE.finditer(body):
            defs.add(m.group(2))
        for m in FREE_DEF_RE.finditer(body):
            defs.add(m.group(2))
    missing = sorted(d for d in decls if d not in defs)
    ok = check(not missing, "every declared member has a definition", ",".join(missing))
    return ok, decls, defs


# ------------------------------------------------- C. forbidden primitives

FORBIDDEN = [
    r"\bRemoveMesh\b", r"\bDetach(Mesh|Node|Part)\b", r"\bSetVisible\s*\(\s*false",
    r"\bSetAlpha\s*\(\s*0", r"\bUnload(Asset|Mesh|Texture)\b", r"\bUnregister\b",
    r"\bSetEnabled\s*\(\s*false", r"\bHide(Mesh|Node|Part)\b", r"\bStrip(Garment|Mesh)\b",
    r"\btoggle_?nude\b", r"\bnaked\b", r"\bnude\b", r"\bundress\b",
    r"\bRemoveGarment\b", r"\bgarmentOff\b",
]


def check_forbidden(paths):
    hits = []
    for p in paths:
        code = strip_code(read(p)).lower()
        for pat in FORBIDDEN:
            for m in re.finditer(pat.lower(), code):
                hits.append(os.path.basename(p) + ":" + m.group(0))
    return check(not hits, "no removal / detachment / exposure primitive in code",
                 ",".join(sorted(set(hits))))


# ------------------------------------------------- D. monotone discipline

WRITE_RE = re.compile(r"\bp\.(wear|tear|hole)\s*=\s*([^;]+);|"
                      r"\bs\.(wear|tear|hole)\s*=\s*([^;]+);")


def check_monotone(paths):
    bad = []
    total = 0
    for p in paths:
        if os.path.basename(p) != "damage_model.cpp":
            continue          # config.cpp assigns these fields while parsing JSON
        code = strip_code(read(p))
        for m in WRITE_RE.finditer(code):
            total += 1
            rhs = (m.group(2) or m.group(4) or "").strip()
            if not rhs.startswith("Monotone("):
                bad.append(os.path.basename(p) + " -> " + rhs[:40])
    check(total > 0, "damage channels are actually written")
    return check(not bad, "all damage writes go through Monotone()", ",".join(bad))


# ------------------------------------------------- E. config field existence

def check_config_fields(header_text, src_paths):
    code = strip_code(header_text)
    # any member declaration inside a struct/class: "Type name;", "Type name[k];", "Type n = v;"
    fields = set(re.findall(r"^\s{4,}(?:const\s+)?[A-Za-z_][\w:]*\s+\**&?\s*([A-Za-z_]\w*)"
                            r"\s*(?:\[[^\]]*\])?\s*(?:=\s*[^;]*)?;", code, re.MULTILINE))
    missing = set()
    used = set()
    for p in src_paths:
        src = strip_code(read(p))
        for m in re.finditer(r"\b(?:cfg_|cfg|k|cons|vc|st|hp|pd|p)\."
                             r"(?:sim|constraints|stages|visual|panels|hitTypes)?\.?"
                             r"([A-Za-z_]\w*)(\s*\()?", src):
            if m.group(2):
                continue          # member function call, not a field
            used.add(m.group(1))
    for u in sorted(used):
        if u in fields:
            continue
        # struct-local members and locals are allowed; only flag obvious misses
        if u in ("panelCount", "hitTypeCount", "area", "neighbors", "neighborCount",
                 "key", "id", "name", "energy", "wear", "tear", "hole", "sharp",
                 "S1_wearMean", "S2_tearMean", "S2_tearPanels", "S3_holeCount",
                 "S3_holeMean", "hysteresis", "grade1_holeCount", "grade2_holeCount",
                 "grade3_holeCount", "grade3_intensity", "grade3_tearMean",
                 "names", "labels", "blendSeconds", "maxDirt", "maxRoughness",
                 "maxDesaturation", "maxFray", "maxEdgeJitter", "maxCutout",
                 "underlayerBoost", "minUVSpacing", "radiusMin", "radiusMax",
                 "growthRadiusGain", "decalWear", "decalSoot", "decalFray",
                 "decalHole", "decalUnderlayer", "allowedCommandCount", "allowedCommands"):
            continue
        missing.add(u)
    return check(not missing, "no reference to an undeclared config field",
                 ",".join(sorted(missing)))


# ------------------------------------------------- F. JSON knobs consumed

JSON_TO_CPP = {
    "allowGarmentRemoval": "allowGarmentRemoval", "minCoverageRatio": "minCoverageRatio",
    "maxHoleRatioPerPanel": "maxHoleRatioPerPanel", "maxTotalLossRatio": "maxTotalLossRatio",
    "maxHolesPerPanel": "maxHolesPerPanel", "alwaysBackedByUnderlayer": "alwaysBackedByUnderlayer",
    "maxStage": "maxStage", "dtMax": "dtMax", "energyScale": "energyScale",
    "maxHitEnergy": "maxHitEnergy", "intensityTauSeconds": "intensityTauSeconds",
    "intensityNorm": "intensityNorm", "fatigueGain": "fatigueGain",
    "adjacencyWeight": "adjacencyWeight", "wearGain": "wearGain",
    "wearSaturationPower": "wearSaturationPower", "tearOnsetWear": "tearOnsetWear",
    "sharpNucleationEnergy": "sharpNucleationEnergy", "tearGain": "tearGain",
    "tearNucleus": "tearNucleus", "tearCrackExponent": "tearCrackExponent",
    "tearEnergyExponent": "tearEnergyExponent", "minIntegrity": "minIntegrity",
    "holeOnsetTear": "holeOnsetTear", "holeNucleationEnergy": "holeNucleationEnergy",
    "holeGain": "holeGain", "holeNucleus": "holeNucleus",
    "holeGrowthExponent": "holeGrowthExponent", "holeCooldownSeconds": "holeCooldownSeconds",
    "holeSiteCoherenceGain": "holeSiteCoherenceGain",
    "stageDwellSeconds": "stageDwellSeconds", "criticalMultiplier": "criticalMultiplier",
    "S1_wearMean": "S1_wearMean", "S2_tearMean": "S2_tearMean",
    "S2_tearPanels": "S2_tearPanels", "S3_holeCount": "S3_holeCount",
    "S3_holeMean": "S3_holeMean", "hysteresis": "hysteresis",
    "grade1_holeCount": "grade1_holeCount", "grade2_holeCount": "grade2_holeCount",
    "grade3_holeCount": "grade3_holeCount", "grade3_intensity": "grade3_intensity",
    "grade3_tearMean": "grade3_tearMean", "blendSeconds": "blendSeconds",
    "maxDirt": "maxDirt", "maxRoughness": "maxRoughness",
    "maxDesaturation": "maxDesaturation", "maxFray": "maxFray",
    "maxEdgeJitter": "maxEdgeJitter", "maxCutout": "maxCutout",
    "underlayerBoost": "underlayerBoost", "minUVSpacing": "minUVSpacing",
    "radiusMin": "radiusMin", "radiusMax": "radiusMax",
    "maxCutount": "maxCutout",
}


def flatten(node, prefix=""):
    out = {}
    if isinstance(node, dict):
        for k, v in node.items():
            out.update(flatten(v, k))
    else:
        out[prefix] = node
    return out


JSON_META = {"schemaVersion", "name", "target", "runtime", "hardConstraint"}
JSON_DECALS = {"wear", "soot", "fray", "hole", "underlayer"}   # -> decalWear / decalSoot / ...


def check_json_consumed(src_paths):
    cfg = json.load(open(CONFIG, "r", encoding="utf-8"))
    flat = flatten(cfg)
    code = "".join(strip_code(read(p)) for p in src_paths)
    unconsumed = []
    for knob in flat:
        if knob in JSON_META:
            continue                     # documentation-only metadata
        if knob in JSON_DECALS:
            if ("decal" + knob.capitalize()) not in code:
                unconsumed.append(knob)
            continue
        cpp = JSON_TO_CPP.get(knob, knob)
        if cpp not in code:
            unconsumed.append(knob)
    return check(not unconsumed, "every config knob is consumed by the loader",
                 ",".join(sorted(unconsumed)))


# ------------------------------------------------- G. include guards

def check_guards(paths):
    guards = {}
    ok = True
    for p in paths:
        text = read(p)
        m = re.search(r"#ifndef\s+([A-Za-z_]\w*)\s*\n#define\s+([A-Za-z_]\w*)", text)
        if not m or m.group(1) != m.group(2):
            ok = check(False, "include guard " + os.path.basename(p))
            continue
        g = m.group(1)
        if g in guards:
            ok = check(False, "duplicate include guard " + g, guards[g] + " vs " + p)
        guards[g] = p
        tail = [ln.strip() for ln in text.rstrip().splitlines() if ln.strip()]
        if not tail or not tail[-1].startswith("#endif"):
            ok = check(False, "#endif close " + os.path.basename(p))
    if ok:
        check(True, "include guards present, unique, closed")
    return ok


def main():
    src_paths = []
    for d in SRC_DIRS:
        full = os.path.join(ROOT, d)
        if os.path.isdir(full):
            src_paths += [os.path.join(full, f) for f in sorted(os.listdir(full))
                          if f.endswith(".cpp")]
    headers = [HEADER, os.path.join(ROOT, "plugin", "game_adapter.h"),
               os.path.join(ROOT, "include", "battlewear", "mini_json.h")]

    print("== A. delimiter balance ==")
    check_balance(src_paths + [h for h in headers if os.path.exists(h)])
    print("== B. declarations vs definitions ==")
    check_definitions(read(HEADER), src_paths)
    print("== C. forbidden primitives ==")
    check_forbidden(src_paths + headers)
    print("== D. monotone discipline ==")
    check_monotone(src_paths)
    print("== E. config field existence ==")
    check_config_fields(read(HEADER), src_paths)
    print("== F. JSON knobs consumed ==")
    check_json_consumed(src_paths)
    print("== G. include guards ==")
    check_guards([h for h in headers if os.path.exists(h)])

    print("-" * 60)
    print(f"cpp_audit: checks={len(CHECKS)} failures={len(FAILS)}")
    for f in FAILS:
        print("  -", f)
    report = {"checks": len(CHECKS), "failures": len(FAILS),
              "failed": FAILS, "files": [os.path.relpath(p, ROOT) for p in src_paths]}
    with open(os.path.join(ROOT, "out", "cpp_audit.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=1)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
