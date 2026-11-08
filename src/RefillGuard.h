#pragma once

// RefillGuard (2.0.0-u2e): stops the ENGINE's full heal of essential / protected NPCs.
//
// The engine (static RE of SkyrimVR.exe 1.4.15):
//   * Actor::StopCombat (+0x62E790) calls the "Health back to full" routine +0x5DECE0 (= the Health half of Papyrus ResetHealthAndLimbs)
//     at +0x62E96D for every actor that had a combat controller, NoBleedoutRecovery off, Essential OR Protected, life state not
//     EssentialDown - the "followers jump to full the moment a fight ends". The TESCombatEvent (None) is sent AFTER it, in the same call.
//     Papyrus SetUnconscious(true) runs StopCombat first, so SBKO's own knock-out set it off too (a KNOCKED-OUT NPC lay at 99 % Health).
//   * Actor::SetNoBleedoutRecovery(false) (+0x62C950) tail-jumps to the same routine at +0x62C9B9 (flag was on, out of combat, Essential /
//     Protected, not EssentialDown) - so SBKO's own WakeUp / FinishWake / LetGo / ClearMark refilled her to full before WakeHealth ran.
//
// Two patches, VR 1.4.15 only, each installed only when its bytes are exactly the expected ones (fail closed: log, change nothing):
//   1. write_call at +0x62E96D: skipped for every essential / protected NPC (iCombatEndRefill 2), for followers only (1), or
//      for nobody (0) - NPCs only (ActorTypeNPC, SBKO's scope; creatures keep it) - and in every mode for an NPC on whom SBKO's own
//      knock-out / hold call is running.
//   2. write_branch at +0x62C9B9: skipped only for an NPC on whom SBKO's own wake / let-go / clear call is running (LetGo then raises a
//      living NPC to fWakeHealthFraction, like every wake - SBKO_Helper.pex u2e).
// Thread rule: both thunks run on the game thread AND on Papyrus VM threads; they take only this file's own lock, for a lookup.
namespace SBKO::RefillGuard
{
    // kDataLoaded, after Settings::Load (reads iCombatEndRefill).
    void Install();

    // SBKO's own Papyrus call on this NPC starts (Helper: KnockOutU, HoldDown, WakeUp, FinishWake, LetGo, ClearMark) / has finished
    // or was not dispatched. Counted, so two overlapping calls on one NPC are both covered.
    void Enter(RE::FormID a_id);
    void Leave(RE::FormID a_id);

    // kPreLoadGame: a call still running dies with the old game (its reply may never come).
    void Reset();

    // u6h (the console test 'sbko refill'): what Install() did, and the combat-end patch's own decision for one actor - the SAME
    // function its thunk asks (nullptr = the engine would refill her, if the engine's own conditions hold: essential / protected,
    // NoBleedoutRecovery off, not EssentialDown, a combat controller). Reads only.
    struct InstallState
    {
        bool        stopPatch = false;   // Actor::StopCombat's full heal is patched (+0x62E96D)
        bool        flagPatch = false;   // SetNoBleedoutRecovery(false)'s full heal is patched (+0x62C9B9)
        const char* why       = "";      // why a patch is missing (a string literal; "" when both are in)
    };
    [[nodiscard]] InstallState GetInstallState();
    [[nodiscard]] const char*  SkipReason(RE::Actor* a_actor);
    [[nodiscard]] bool         CallRunning(RE::FormID a_id);
}
