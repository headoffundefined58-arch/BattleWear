// BattleWear - dynamic costume binding.
// Nothing here references a fixed costume asset. Unregistered / third-party costume MODs
// are resolved to a generic whole-garment binding that still supports all damage tiers.
#include "battlewear/battlewear.h"
#include "battlewear/mini_json.h"

#include <cstdio>

namespace bw {

CostumeRegistry::CostumeRegistry(const Config& cfg) : cfg_(cfg) {}

static bool KeyEquals(const CostumeHandle& h, const char* code, const char* variant) {
    return strcmp(h.characterCode, code) == 0 && strcmp(h.variantId, variant) == 0;
}

void CostumeRegistry::BuildGeneric(CostumeHandle& out) const {
    out = CostumeHandle();
    CopyName(out.baseMesh, "auto:whole_garment");
    CopyName(out.baseMaterial, "auto:whole_garment");
    out.dedicated = false;
    out.fallbackUsed = true;
    out.panelCount = cfg_.panelCount;
    for (int i = 0; i < cfg_.panelCount; ++i) {
        PanelBinding& b = out.panels[i];
        b = PanelBinding();
        CopyName(b.mesh, out.baseMesh);
        CopyName(b.material, out.baseMaterial);
        CopyName(b.uvSet, "uv0", kShortLen);
        b.hasFrayVariant = false;
        b.hasHoleMask = false;
    }
}

void CostumeRegistry::Register(const CostumeHandle& handle) {
    CostumeHandle h = handle;
    h.fallbackUsed = false;
    h.panelCount = cfg_.panelCount;
    // A base binding is mandatory for every panel: the garment always stays attached.
    for (int i = 0; i < cfg_.panelCount; ++i) {
        if (h.panels[i].mesh[0] == 0) CopyName(h.panels[i].mesh, h.baseMesh[0] ? h.baseMesh : "auto:whole_garment");
        if (h.panels[i].material[0] == 0) CopyName(h.panels[i].material, h.baseMaterial[0] ? h.baseMaterial : "auto:whole_garment");
        if (h.panels[i].uvSet[0] == 0) CopyName(h.panels[i].uvSet, "uv0", kShortLen);
        h.panels[i].hasFrayVariant = h.panels[i].frayVariant[0] != 0;
        h.panels[i].hasHoleMask = h.panels[i].holeMask[0] != 0;
    }
    for (int i = 0; i < count_; ++i) {
        if (KeyEquals(entries_[i], h.characterCode, h.variantId)) { entries_[i] = h; return; }
    }
    if (count_ < 64) entries_[count_++] = h;
}

bool CostumeRegistry::Resolve(const char* characterCode, const char* variantId, CostumeHandle& out) {
    const char* code = characterCode ? characterCode : "unknown";
    const char* var = variantId ? variantId : "default";
    for (int i = 0; i < count_; ++i) {
        if (KeyEquals(entries_[i], code, var)) {
            out = entries_[i];
            out.fallbackUsed = false;
            return true;
        }
    }
    BuildGeneric(out);
    CopyName(out.characterCode, code, kShortLen);
    CopyName(out.variantId, var, kNameLen);
    ++fallbackCount_;
    return true;   // NEVER fails: unknown costume mods are fully supported
}

bool CostumeRegistry::LoadJson(const std::string& text, std::string& error) {
    mj::Value root;
    if (!mj::Parse(text, root, error)) return false;
    const mj::Value* arr = root.Find("costumes");
    if (!arr || arr->type != mj::Value::Array) { error = "missing costumes array"; return false; }

    for (const auto& item : arr->Array()) {
        CostumeHandle h;
        const mj::Value* v = nullptr;
        v = item.Find("characterCode"); if (v) CopyName(h.characterCode, v->Text("unknown").c_str(), kShortLen);
        v = item.Find("variantId");    if (v) CopyName(h.variantId, v->Text("default").c_str(), kNameLen);
        v = item.Find("baseMesh");     if (v) CopyName(h.baseMesh, v->Text("auto:whole_garment").c_str(), kNameLen);
        v = item.Find("baseMaterial"); if (v) CopyName(h.baseMaterial, v->Text("auto:whole_garment").c_str(), kNameLen);
        v = item.Find("damageAssets");
        h.dedicated = v && v->Text("generic") == "dedicated";

        if (h.characterCode[0] == 0) CopyName(h.characterCode, "unknown", kShortLen);
        if (h.variantId[0] == 0) CopyName(h.variantId, "default", kNameLen);
        if (h.baseMesh[0] == 0) CopyName(h.baseMesh, "auto:whole_garment");
        if (h.baseMaterial[0] == 0) CopyName(h.baseMaterial, "auto:whole_garment");

        for (int i = 0; i < cfg_.panelCount; ++i) {
            CopyName(h.panels[i].mesh, h.baseMesh);
            CopyName(h.panels[i].material, h.baseMaterial);
            CopyName(h.panels[i].uvSet, "uv0", kShortLen);
        }
        const mj::Value* panels = item.Find("panels");
        if (panels && panels->type == mj::Value::Object) {
            for (const auto& kv : panels->Object()) {
                int idx = cfg_.FindPanel(kv.first.c_str());
                if (idx < 0) continue;
                const mj::Value& pv = kv.second;
                PanelBinding& b = h.panels[idx];
                const mj::Value* x = nullptr;
                x = pv.Find("mesh");       if (x) CopyName(b.mesh, x->Text(b.mesh).c_str());
                x = pv.Find("material");   if (x) CopyName(b.material, x->Text(b.material).c_str());
                x = pv.Find("uv");         if (x) CopyName(b.uvSet, x->Text("uv0").c_str(), kShortLen);
                x = pv.Find("frayVariant");if (x) CopyName(b.frayVariant, x->Text("").c_str());
                x = pv.Find("holeMask");   if (x) CopyName(b.holeMask, x->Text("").c_str());
            }
        }
        Register(h);
    }
    return true;
}

} // namespace bw
