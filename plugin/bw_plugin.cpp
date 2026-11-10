// BattleWear - JAPI / ModdingAPI DLL plugin entry points.
//
// Build this file into a native x64 DLL alongside the core sources. The exported
// BW_* functions are loader-agnostic; a JAPI plugin just forwards its on-frame
// callback to BW_Tick.
//
// Integration with the existing MOD workflow:
//   1. Put the generic decals / NUD fray strips in a BattleWear CPK (or let
//      JAPI's CPK Loader load it). No vanilla file is replaced.
//   2. NSC Mod Manager / costume_bindings.json registers (characterCode,
//      variantId) -> per-panel mesh/material roles. Unknown costume mods need no
//      registration: CostumeRegistry produces a generic fallback binding.
//   3. IGameAdapter::ApplyFrame maps logical render commands to NUT material
//      parameters, UV decals and additive NUD geometry at runtime.
#include "battlewear/battlewear.h"
#include "game_adapter.h"

#include <fstream>
#include <iterator>
#include <new>
#include <string>

#ifdef _WIN32
#define BW_EXPORT __declspec(dllexport)
#else
#define BW_EXPORT __attribute__((visibility("default")))
#endif

namespace bw {
namespace plugin {

static std::string ReadText(const char* path) {
    std::ifstream in(path ? path : "", std::ios::binary);
    if (!in) return std::string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class BattleWearPlugin {
public:
    BattleWearPlugin() : model_(cfg_), registry_(cfg_), mapper_(cfg_) {}

    bool Initialize(const char* configPath, const char* bindingsPath, IGameAdapter& adapter) {
        adapter_ = &adapter;
        std::string err;
        std::string configText = ReadText(configPath);
        if (!configText.empty() && !cfg_.LoadJson(configText, err)) {
            adapter_->Log(("BattleWear: config parse failed, using defaults: " + err).c_str());
            cfg_ = Config::Defaults();
        }
        model_ = DamageModel(cfg_);
        registry_ = CostumeRegistry(cfg_);
        mapper_ = VisualMapper(cfg_);

        std::string bindingText = ReadText(bindingsPath);
        if (!bindingText.empty() && !registry_.LoadJson(bindingText, err))
            adapter_->Log(("BattleWear: binding file ignored: " + err).c_str());

        char code[kShortLen] = "unknown";
        char variant[kNameLen] = "default";
        model_.Reset(0x5EED1234ULL);
        registry_.Resolve(code, variant, costume_);
        VisualFrame frame;
        mapper_.BuildFrame(model_.State(), costume_, frame);
        adapter_->ApplyFrame(frame, costume_);
        return true;
    }

    void OnFrame(double dt) {
        if (!adapter_) return;

        BattleEvent be;
        while (adapter_->PollBattleEvent(be)) {
            if (be.kind == BattleEventKind::BattleStart ||
                be.kind == BattleEventKind::CharacterSwitch) {
                model_.Reset(Hash64(model_.State().battleSeed,
                                    static_cast<uint64_t>(model_.State().tickIndex), 0xA5A5ULL));
            }
        }
        CostumeChangeEvent ce;
        while (adapter_->PollCostumeChange(ce))
            OnCostumeChange(ce.characterCode, ce.variantId);

        HitTelemetry ht;
        while (adapter_->PollHit(ht)) {
            HitEvent e;
            e.time = model_.State().simTime;   // clamp to the model clock
            e.energy = ht.energy;
            e.panel = ht.panel;
            e.hitType = ht.hitType;
            e.critical = ht.critical;
            e.attackerId = ht.attackerId;
            model_.ApplyHit(e);
        }
        model_.Tick(dt);

        VisualFrame frame;
        mapper_.BuildFrame(model_.State(), costume_, frame);
#ifndef NDEBUG
        if (!ValidateState(cfg_, model_.State()).Ok() ||
            !ValidateFrame(cfg_, frame).Ok() || !frame.garmentAttached) {
            adapter_->Log("BattleWear: invariant violation - frame discarded");
            return;
        }
#endif
        adapter_->ApplyFrame(frame, costume_);
    }

    void OnCostumeChange(const char* code, const char* variant) {
        registry_.Resolve(code ? code : "unknown", variant ? variant : "default", costume_);
        // A different garment is an intact garment; no damage carries over.
        model_.Reset(Hash64(model_.State().battleSeed, 0xC0FFEEULL, 0x1234ULL));
    }

    const DamageState& State() const { return model_.State(); }

private:
    Config cfg_ = Config::Defaults();
    DamageModel model_;
    CostumeRegistry registry_;
    VisualMapper mapper_;
    CostumeHandle costume_;
    IGameAdapter* adapter_ = nullptr;
};

} // namespace plugin
} // namespace bw

using bw::plugin::BattleWearPlugin;

extern "C" {

BW_EXPORT void* BW_Create(const char* configPath, const char* bindingsPath, void* adapter) {
    if (!adapter) return nullptr;
    BattleWearPlugin* p = new (std::nothrow) BattleWearPlugin();
    if (!p) return nullptr;
    if (!p->Initialize(configPath, bindingsPath,
                       *static_cast<bw::plugin::IGameAdapter*>(adapter))) {
        delete p;
        return nullptr;
    }
    return p;
}

BW_EXPORT void BW_Destroy(void* handle) { delete static_cast<BattleWearPlugin*>(handle); }

BW_EXPORT void BW_Tick(void* handle, double dt) {
    if (handle) static_cast<BattleWearPlugin*>(handle)->OnFrame(dt);
}

BW_EXPORT void BW_OnCostumeChange(void* handle, const char* code, const char* variant) {
    if (handle) static_cast<BattleWearPlugin*>(handle)->OnCostumeChange(code, variant);
}

BW_EXPORT int BW_Stage(void* handle) {
    if (!handle) return 0;
    return static_cast<int>(static_cast<BattleWearPlugin*>(handle)->State().stage);
}

} // extern "C"
