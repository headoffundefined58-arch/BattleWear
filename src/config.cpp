// BattleWear - config loading (data-driven, no hard-coded costume assets).
#include "battlewear/battlewear.h"
#include "battlewear/mini_json.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace bw {

static void SetNames4(char names[4][kShortLen], const char* a, const char* b,
                      const char* c, const char* d) {
    CopyName(names[0], a, kShortLen);
    CopyName(names[1], b, kShortLen);
    CopyName(names[2], c, kShortLen);
    CopyName(names[3], d, kShortLen);
}

static const mj::Value* Get(const mj::Value& root, const char* key) { return root.Find(key); }
static double Num(const mj::Value& root, const char* key, double def) {
    const mj::Value* v = Get(root, key);
    return v ? v->Number(def) : def;
}
static int Int(const mj::Value& root, const char* key, int def) {
    const mj::Value* v = Get(root, key);
    return v ? v->Integer(def) : def;
}
static bool Boo(const mj::Value& root, const char* key, bool def) {
    const mj::Value* v = Get(root, key);
    return v ? v->Bool(def) : def;
}
static void Str(const mj::Value& root, const char* key, char* dst, int cap) {
    const mj::Value* v = Get(root, key);
    if (v && v->type == mj::Value::Type::String) CopyName(dst, v->text.c_str(), cap);
}

static const char* kDefaultPanelKeys[10] = {
    "torso_front", "torso_back", "abdomen", "shoulder_l", "shoulder_r",
    "arm_l", "arm_r", "hip", "leg_l", "leg_r"
};
static const double kDefaultPanelAreas[10] = {
    0.16, 0.16, 0.10, 0.07, 0.07, 0.08, 0.08, 0.10, 0.09, 0.09
};
static const int kDefaultNeighbors[10][4] = {
    {1, 2, 3, 4}, {0, 2, 3, 4}, {0, 1, 7, -1}, {0, 1, 5, -1}, {0, 1, 6, -1},
    {3, -1, -1, -1}, {4, -1, -1, -1}, {2, 8, 9, -1}, {7, -1, -1, -1}, {7, -1, -1, -1}
};

Config Config::Defaults() {
    Config c;

    c.constraints.allowGarmentRemoval = false;
    c.constraints.minCoverageRatio = 0.80;
    c.constraints.maxHoleRatioPerPanel = 0.22;
    c.constraints.maxTotalLossRatio = 0.18;
    c.constraints.maxHolesPerPanel = 4;
    c.constraints.alwaysBackedByUnderlayer = true;
    c.constraints.maxStage = 3;

    c.sim.dtMax = 0.10;
    c.sim.energyScale = 1.0;
    c.sim.maxHitEnergy = 25.0;
    c.sim.intensityTauSeconds = 1.20;
    c.sim.intensityNorm = 2.50;
    c.sim.fatigueGain = 0.55;
    c.sim.adjacencyWeight = 0.18;
    c.sim.wearGain = 0.055;
    c.sim.wearSaturationPower = 1.35;
    c.sim.tearOnsetWear = 0.12;
    c.sim.sharpNucleationEnergy = 1.40;
    c.sim.tearGain = 0.030;
    c.sim.tearNucleus = 0.045;
    c.sim.tearCrackExponent = 0.65;
    c.sim.tearEnergyExponent = 0.85;
    c.sim.minIntegrity = 0.28;
    c.sim.holeOnsetTear = 0.32;
    c.sim.holeNucleationEnergy = 2.20;
    c.sim.holeGain = 0.020;
    c.sim.holeNucleus = 0.030;
    c.sim.holeGrowthExponent = 0.55;
    c.sim.holeCooldownSeconds = 0.45;
    c.sim.holeSiteCoherenceGain = 0.35;
    c.sim.stageDwellSeconds = 0.25;
    c.sim.criticalMultiplier = 1.35;

    struct { const char* n; double e, w, t, h; bool sharp; } hp[5] = {
        {"blunt",  0.70, 0.85, 0.13, 0.02, false},
        {"slash",  1.00, 0.42, 0.52, 0.06, true },
        {"pierce", 0.90, 0.22, 0.26, 0.52, true },
        {"heat",   1.10, 0.90, 0.08, 0.02, false},
        {"chakra", 1.00, 0.55, 0.32, 0.13, true }
    };
    c.hitTypeCount = 5;
    for (int i = 0; i < 5; ++i) {
        CopyName(c.hitTypes[i].name, hp[i].n, kShortLen);
        c.hitTypes[i].energy = hp[i].e;
        c.hitTypes[i].wear = hp[i].w;
        c.hitTypes[i].tear = hp[i].t;
        c.hitTypes[i].hole = hp[i].h;
        c.hitTypes[i].sharp = hp[i].sharp;
    }

    c.panelCount = 10;
    for (int i = 0; i < 10; ++i) {
        PanelDef& p = c.panels[i];
        p.id = i;
        CopyName(p.key, kDefaultPanelKeys[i], kShortLen);
        p.area = kDefaultPanelAreas[i];
        p.neighborCount = 0;
        for (int j = 0; j < 4; ++j) {
            if (kDefaultNeighbors[i][j] >= 0) p.neighbors[p.neighborCount++] = kDefaultNeighbors[i][j];
        }
    }

    SetNames4(c.stages.names, "pristine", "light_wear", "edge_tearing", "local_holes");
    CopyName(c.stages.labels[0], "完好", kShortLen);
    CopyName(c.stages.labels[1], "轻微磨损", kShortLen);
    CopyName(c.stages.labels[2], "边缘撕裂", kShortLen);
    CopyName(c.stages.labels[3], "局部破洞", kShortLen);
    c.stages.S1_wearMean = 0.10;
    c.stages.S2_tearMean = 0.16;
    c.stages.S2_tearPanels = 2;
    c.stages.S3_holeCount = 2;
    c.stages.S3_holeMean = 0.045;
    c.stages.hysteresis = 0.80;
    c.stages.grade1_holeCount = 3;
    c.stages.grade2_holeCount = 5;
    c.stages.grade3_holeCount = 8;
    c.stages.grade3_intensity = 0.55;
    c.stages.grade3_tearMean = 0.55;

    c.visual.blendSeconds = 0.35;
    c.visual.maxDirt = 0.85;
    c.visual.maxRoughness = 0.55;
    c.visual.maxDesaturation = 0.30;
    c.visual.maxFray = 0.95;
    c.visual.maxEdgeJitter = 0.40;
    c.visual.maxCutout = 1.0;
    c.visual.underlayerBoost = 1.0;
    c.visual.minUVSpacing = 0.16;
    c.visual.radiusMin = 0.045;
    c.visual.radiusMax = 0.115;
    c.visual.growthRadiusGain = 0.055;
    CopyName(c.visual.decalWear, "bw/generic/wear_uv");
    CopyName(c.visual.decalSoot, "bw/generic/soot_uv");
    CopyName(c.visual.decalFray, "bw/generic/fray_edge");
    CopyName(c.visual.decalHole, "bw/generic/hole_mask");
    CopyName(c.visual.decalUnderlayer, "bw/generic/underlayer");
    static const char* ops[3] = {"SET_MATERIAL_PARAM", "ADD_DECAL", "BIND_ADDITIVE_MESH"};
    c.visual.allowedCommandCount = 3;
    for (int i = 0; i < 3; ++i) CopyName(c.visual.allowedCommands[i], ops[i], kShortLen);

    return c;
}

bool Config::LoadJson(const std::string& text, std::string& error) {
    mj::Value root;
    if (!mj::Parse(text, root, error)) return false;
    if (root.type != mj::Value::Type::Object) { error = "root must be an object"; return false; }

    const mj::Value* cons = Get(root, "constraints");
    if (cons) {
        // Hard constraint: removal can never be enabled by config.
        constraints.allowGarmentRemoval = false;
        constraints.minCoverageRatio = Clamp(Num(*cons, "minCoverageRatio", constraints.minCoverageRatio), 0.70, 1.0);
        constraints.maxHoleRatioPerPanel = Clamp(Num(*cons, "maxHoleRatioPerPanel", constraints.maxHoleRatioPerPanel), 0.001, 0.35);
        constraints.maxTotalLossRatio = Clamp(Num(*cons, "maxTotalLossRatio", constraints.maxTotalLossRatio), 0.001, 0.30);
        constraints.maxHolesPerPanel = static_cast<int>(Clamp(Int(*cons, "maxHolesPerPanel", constraints.maxHolesPerPanel), 1, kMaxHoles));
        constraints.alwaysBackedByUnderlayer = true;
        constraints.maxStage = 3;
    }

    const mj::Value* simv = Get(root, "simulation");
    if (simv) {
        sim.dtMax = Clamp(Num(*simv, "dtMax", sim.dtMax), 0.001, 0.25);
        sim.energyScale = Clamp(Num(*simv, "energyScale", sim.energyScale), 0.0, 100.0);
        sim.maxHitEnergy = Clamp(Num(*simv, "maxHitEnergy", sim.maxHitEnergy), 0.001, 1.0e6);
        sim.intensityTauSeconds = Clamp(Num(*simv, "intensityTauSeconds", sim.intensityTauSeconds), 0.01, 60.0);
        sim.intensityNorm = Clamp(Num(*simv, "intensityNorm", sim.intensityNorm), 0.01, 1.0e6);
        sim.fatigueGain = Clamp(Num(*simv, "fatigueGain", sim.fatigueGain), 0.0, 10.0);
        sim.adjacencyWeight = Clamp(Num(*simv, "adjacencyWeight", sim.adjacencyWeight), 0.0, 1.0);
        sim.wearGain = Clamp(Num(*simv, "wearGain", sim.wearGain), 0.0, 10.0);
        sim.wearSaturationPower = Clamp(Num(*simv, "wearSaturationPower", sim.wearSaturationPower), 0.05, 10.0);
        sim.tearOnsetWear = Clamp(Num(*simv, "tearOnsetWear", sim.tearOnsetWear), 0.0, 1.0);
        sim.sharpNucleationEnergy = Clamp(Num(*simv, "sharpNucleationEnergy", sim.sharpNucleationEnergy), 0.0, 100.0);
        sim.tearGain = Clamp(Num(*simv, "tearGain", sim.tearGain), 0.0, 10.0);
        sim.tearNucleus = Clamp(Num(*simv, "tearNucleus", sim.tearNucleus), 1.0e-6, 1.0);
        sim.tearCrackExponent = Clamp(Num(*simv, "tearCrackExponent", sim.tearCrackExponent), 0.0, 4.0);
        sim.tearEnergyExponent = Clamp(Num(*simv, "tearEnergyExponent", sim.tearEnergyExponent), 0.05, 4.0);
        sim.minIntegrity = Clamp(Num(*simv, "minIntegrity", sim.minIntegrity), 0.05, 1.0);
        sim.holeOnsetTear = Clamp(Num(*simv, "holeOnsetTear", sim.holeOnsetTear), 0.0, 1.0);
        sim.holeNucleationEnergy = Clamp(Num(*simv, "holeNucleationEnergy", sim.holeNucleationEnergy), 0.0, 100.0);
        sim.holeGain = Clamp(Num(*simv, "holeGain", sim.holeGain), 0.0, 10.0);
        sim.holeNucleus = Clamp(Num(*simv, "holeNucleus", sim.holeNucleus), 1.0e-6, 1.0);
        sim.holeGrowthExponent = Clamp(Num(*simv, "holeGrowthExponent", sim.holeGrowthExponent), 0.0, 4.0);
        sim.holeCooldownSeconds = Clamp(Num(*simv, "holeCooldownSeconds", sim.holeCooldownSeconds), 0.0, 30.0);
        sim.holeSiteCoherenceGain = Clamp(Num(*simv, "holeSiteCoherenceGain", sim.holeSiteCoherenceGain), 0.0, 1.0);
        sim.stageDwellSeconds = Clamp(Num(*simv, "stageDwellSeconds", sim.stageDwellSeconds), 0.0, 10.0);
        sim.criticalMultiplier = Clamp(Num(*simv, "criticalMultiplier", sim.criticalMultiplier), 1.0, 10.0);
    }

    const mj::Value* ht = Get(root, "hitTypes");
    if (ht && ht->type == mj::Value::Type::Object) {
        int i = 0;
        for (const auto& kv : ht->Object()) {
            if (i >= kMaxHitTypes) break;
            const mj::Value& v = kv.second;
            HitProfile& p = hitTypes[i];
            CopyName(p.name, kv.first.c_str(), kShortLen);
            p.energy = Clamp(Num(v, "energy", p.energy), 0.0, 100.0);
            p.wear = Clamp(Num(v, "wear", p.wear), 0.0, 1.0);
            p.tear = Clamp(Num(v, "tear", p.tear), 0.0, 1.0);
            p.hole = Clamp(Num(v, "hole", p.hole), 0.0, 1.0);
            p.sharp = Boo(v, "sharp", p.sharp);
            double sum = p.wear + p.tear + p.hole;
            if (sum > 0.0) { p.wear /= sum; p.tear /= sum; p.hole /= sum; }
            ++i;
        }
        if (i > 0) hitTypeCount = i;
    }

    const mj::Value* pv = Get(root, "panels");
    if (pv && pv->type == mj::Value::Type::Array && !pv->Array().empty()) {
        int n = 0;
        for (const auto& item : pv->Array()) {
            if (n >= kMaxPanels) break;
            PanelDef& p = panels[n];
            p.id = Int(item, "id", n);
            Str(item, "key", p.key, kShortLen);
            p.area = Clamp(Num(item, "area", 1.0 / pv->Array().size()), 1.0e-6, 1.0);
            p.neighborCount = 0;
            const mj::Value* nb = item.Find("neighbors");
            if (nb && nb->type == mj::Value::Type::Array) {
                for (const auto& x : nb->Array()) {
                    if (p.neighborCount >= kMaxNeighbors) break;
                    int id = x.Integer(-1);
                    if (id >= 0 && id < static_cast<int>(pv->Array().size()) && id != n)
                        p.neighbors[p.neighborCount++] = id;
                }
            }
            ++n;
        }
        panelCount = n;
        // Normalize areas to sum to exactly 1 so a per-panel cap implies a global cap.
        double total = TotalPanelArea();
        if (total > 0.0) for (int i = 0; i < panelCount; ++i) panels[i].area /= total;
    }

    const mj::Value* st = Get(root, "stages");
    if (st) {
        const mj::Value* names = st->Find("names");
        if (names && names->type == mj::Value::Type::Array) {
            for (int i = 0; i < 4 && i < static_cast<int>(names->Array().size()); ++i)
                if (names->Array()[i].type == mj::Value::Type::String)
                    CopyName(stages.names[i], names->Array()[i].text.c_str(), kShortLen);
        }
        const mj::Value* labels = st->Find("labels");
        if (labels && labels->type == mj::Value::Type::Array) {
            for (int i = 0; i < 4 && i < static_cast<int>(labels->Array().size()); ++i)
                if (labels->Array()[i].type == mj::Value::Type::String)
                    CopyName(stages.labels[i], labels->Array()[i].text.c_str(), kShortLen);
        }
        stages.S1_wearMean = Clamp(Num(*st, "S1_wearMean", stages.S1_wearMean), 0.0, 1.0);
        stages.S2_tearMean = Clamp(Num(*st, "S2_tearMean", stages.S2_tearMean), 0.0, 1.0);
        stages.S2_tearPanels = Int(*st, "S2_tearPanels", stages.S2_tearPanels);
        stages.S3_holeCount = Int(*st, "S3_holeCount", stages.S3_holeCount);
        stages.S3_holeMean = Clamp(Num(*st, "S3_holeMean", stages.S3_holeMean), 0.0, 1.0);
        stages.hysteresis = Clamp(Num(*st, "hysteresis", stages.hysteresis), 0.0, 1.0);
        const mj::Value* hg = st->Find("heavyGrade");
        if (hg) {
            stages.grade1_holeCount = Int(*hg, "grade1_holeCount", stages.grade1_holeCount);
            stages.grade2_holeCount = Int(*hg, "grade2_holeCount", stages.grade2_holeCount);
            stages.grade3_holeCount = Int(*hg, "grade3_holeCount", stages.grade3_holeCount);
            stages.grade3_intensity = Clamp(Num(*hg, "grade3_intensity", stages.grade3_intensity), 0.0, 1.0);
            stages.grade3_tearMean = Clamp(Num(*hg, "grade3_tearMean", stages.grade3_tearMean), 0.0, 1.0);
        }
    }

    const mj::Value* vi = Get(root, "visual");
    if (vi) {
        visual.blendSeconds = Clamp(Num(*vi, "blendSeconds", visual.blendSeconds), 0.001, 10.0);
        const mj::Value* w = vi->Find("wear");
        if (w) {
            visual.maxDirt = Clamp(Num(*w, "maxDirt", visual.maxDirt), 0.0, 1.0);
            visual.maxRoughness = Clamp(Num(*w, "maxRoughness", visual.maxRoughness), 0.0, 1.0);
            visual.maxDesaturation = Clamp(Num(*w, "maxDesaturation", visual.maxDesaturation), 0.0, 1.0);
        }
        const mj::Value* t = vi->Find("tear");
        if (t) {
            visual.maxFray = Clamp(Num(*t, "maxFray", visual.maxFray), 0.0, 1.0);
            visual.maxEdgeJitter = Clamp(Num(*t, "maxEdgeJitter", visual.maxEdgeJitter), 0.0, 1.0);
        }
        const mj::Value* h = vi->Find("hole");
        if (h) {
            visual.maxCutout = Clamp(Num(*h, "maxCutout", visual.maxCutout), 0.0, 1.0);
            visual.underlayerBoost = Clamp(Num(*h, "underlayerBoost", visual.underlayerBoost), 1.0, 4.0);
            visual.minUVSpacing = Clamp(Num(*h, "minUVSpacing", visual.minUVSpacing), 0.0, 1.0);
            visual.radiusMin = Clamp(Num(*h, "radiusMin", visual.radiusMin), 0.001, 1.0);
            visual.radiusMax = Clamp(Num(*h, "radiusMax", visual.radiusMax), 0.001, 1.0);
            visual.growthRadiusGain = Clamp(Num(*h, "growthRadiusGain", visual.growthRadiusGain), 0.0, 1.0);
        }
        const mj::Value* d = vi->Find("genericDecals");
        if (d) {
            Str(*d, "wear", visual.decalWear, kNameLen);
            Str(*d, "soot", visual.decalSoot, kNameLen);
            Str(*d, "fray", visual.decalFray, kNameLen);
            Str(*d, "hole", visual.decalHole, kNameLen);
            Str(*d, "underlayer", visual.decalUnderlayer, kNameLen);
        }
        const mj::Value* ops = vi->Find("allowedCommands");
        if (ops && ops->type == mj::Value::Type::Array) {
            int n = 0;
            for (const auto& x : ops->Array()) {
                if (n >= 8) break;
                if (x.type != mj::Value::Type::String) continue;
                const char* s = x.text.c_str();
                if (strcmp(s, "SET_MATERIAL_PARAM") && strcmp(s, "ADD_DECAL") &&
                    strcmp(s, "BIND_ADDITIVE_MESH")) continue;   // forbidden ops are dropped
                CopyName(visual.allowedCommands[n++], s, kShortLen);
            }
            if (n > 0) visual.allowedCommandCount = n;
        }
    }
    return true;
}

bool Config::LoadFile(const std::string& path, std::string& error) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) { error = "cannot open config file: " + path; return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    return LoadJson(ss.str(), error);
}

int Config::FindHitType(const char* name) const {
    if (!name) return -1;
    for (int i = 0; i < hitTypeCount; ++i)
        if (strcmp(hitTypes[i].name, name) == 0) return i;
    return -1;
}
int Config::FindPanel(const char* key) const {
    if (!key) return -1;
    for (int i = 0; i < panelCount; ++i)
        if (strcmp(panels[i].key, key) == 0) return i;
    return -1;
}
double Config::TotalPanelArea() const {
    double t = 0.0;
    for (int i = 0; i < panelCount; ++i) t += panels[i].area;
    return t;
}
bool Config::IsCommandAllowed(const char* name) const {
    for (int i = 0; i < visual.allowedCommandCount; ++i)
        if (strcmp(visual.allowedCommands[i], name) == 0) return true;
    return false;
}

} // namespace bw
