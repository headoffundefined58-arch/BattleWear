// BattleWear - engine abstraction for the NUCC / XFBIN runtime.
//
// The logic and visual layers never call the game. Implement IGameAdapter once per
// loader (JAPI DLL plugin / ModdingAPI plugin) and the whole system drops in unchanged.
//
// Typical wiring on NSUNSC (Steam):
//   * Resource path remap: hook CPK/XFBIN resolve, translate logical roles
//     ("wear", "fray_edge", "hole_mask", "underlayer") to NUT decals / NUD additive
//     meshes from the BattleWear CPK, keyed by characterCode + variantId.
//   * prm / prm_load parameter reads supply hit energy and hit location.
//   * PollHit is fed from the combat event stream (damage, hitbox -> logical panel).
//   * ApplyFrame issues only additive material/decal/mesh changes; it must NEVER
//     hide, unload, detach or fade the base costume mesh.
#ifndef BATTLEWEAR_GAME_ADAPTER_H
#define BATTLEWEAR_GAME_ADAPTER_H

#include "battlewear/battlewear.h"

namespace bw {
namespace plugin {

enum class BattleEventKind : uint8_t { None = 0, BattleStart, BattleEnd, CharacterSwitch };

struct HitTelemetry {
    double time = 0.0;
    double energy = 0.0;       // map from in-game damage / impulse
    int    panel = 0;          // map from hitbox / bone to a logical garment panel
    int    hitType = 0;        // index into Config::hitTypes
    bool   critical = false;
    uint32_t attackerId = 0;
};

struct CostumeChangeEvent {
    char characterCode[kShortLen] = {};
    char variantId[kNameLen] = {};
};

struct BattleEvent {
    BattleEventKind kind = BattleEventKind::None;
    char characterCode[kShortLen] = {};
};

class IGameAdapter {
public:
    virtual ~IGameAdapter() {}
    virtual double Now() const = 0;
    virtual bool PollHit(HitTelemetry& out) = 0;
    virtual bool PollCostumeChange(CostumeChangeEvent& out) = 0;
    virtual bool PollBattleEvent(BattleEvent& out) = 0;
    // Called once per frame with a fully resolved frame. All ops are additive.
    virtual void ApplyFrame(const VisualFrame& frame, const CostumeHandle& costume) = 0;
    virtual void Log(const char* message) = 0;
};

} // namespace plugin
} // namespace bw

#endif // BATTLEWEAR_GAME_ADAPTER_H
