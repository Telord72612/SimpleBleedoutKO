#pragma once

// SBKO 2.0 M1: the knockout, rebuilt in C++ - behaviour = v10's SBKO_DownWatcherScript.psc (221 lines,
// the per-NPC SPID ability), now one plugin that watches the engine's bleedout event and the loaded actors instead of a
// script on every NPC. "C++ brain, Papyrus hands": every engine write is done by SBKO_Helper.psc (Helper.h).
// Design and thread model: see the header comment of Knockout.cpp.
//
// U1: the GUARDED UNCONSCIOUS - a protected / essential NPC who enters bleedout is made unconscious and held
// (Paralysis, Health hold, potions off) until a heal or the game-hour timer wakes her.
//
namespace SBKO::Knockout
{
    inline constexpr std::uint32_t kSerializationID = 'SBKO';   // SKSE co-save unique ID (record 'STAT' v4 since U1; v1-v3 read;
                                                                 // u2c: + 'LREF' v1 - light-plugin ids by plugin name)

    // u2c P1: the number of log lines written (bumped by the counting sink main.cpp adds to the logger; read by the PERF line)
    extern std::atomic<std::uint64_t> g_logLines;

    void OnDataLoaded();                  // forms, event sinks, the check thread (once)
    void AcquirePlanck();                 // u6 fix 1 (F5): PLANCK's interface (kDataLoaded, after the ini; bHeadHit only)
    void OnPreLoadGame();                 // a load begins: stop checking, drop work queued for the old session
    void OnGameReady(bool a_postLoad);    // kPostLoadGame (true: arm the v10-leftover adoption) / kNewGame (false)

    // SKSE co-save callbacks (registered in SKSEPluginLoad)
    void OnSave(SKSE::SerializationInterface* a_intf);
    void OnLoad(SKSE::SerializationInterface* a_intf);
    void OnRevert(SKSE::SerializationInterface* a_intf);

    // U1 console tests (TestCmd.cpp). Called on the console's thread with a living, loaded, non-player actor; each returns
    // the ONE HUD line (under 15 words) and dispatches its Papyrus with no lock of ours held.
    std::string ConsoleU(RE::Actor* a_actor);                            // sbkou (!! drops a protected NPC SBKO does not hold to 0 HP)
    std::string ConsoleT(RE::Actor* a_actor, const char* a_cmd = "sbkot");   // sbkot / sbko timer

    // u6h console tests ('sbko <test>', TestCmd.cpp): the SAME task code the game runs, called synchronously on the console's thread with
    // a non-player NPC (status: also a dead / unloaded one), no lock of ours held. Each returns ONE line under 15 words; the detail is logged.
    [[nodiscard]] const char* ConsoleNotNpc(RE::Actor* a_actor);   // nullptr = an NPC (ActorTypeNPC); else the refusal's wording
    std::string ConsoleHelp();                                      // sbko help: the list (log) + one line
    std::string ConsoleStatus(RE::Actor* a_actor);                  // sbko status / sbkos: reads only
    std::string ConsoleSleep(RE::Actor* a_actor);                   // sbko sleep: OnSleepTask as if the player cast Sleep
    std::string ConsoleHeadKO(RE::Actor* a_actor, bool a_blunt, bool a_open);   // sbko headko [blunt] [open]: OnHitTask with a simulated hit
    std::string ConsoleRoll(RE::Actor* a_actor, bool a_apply);      // sbko roll [ko]: OnChanceKOTask (dry unless 'ko')
    std::string ConsoleHitWake(RE::Actor* a_actor);                 // sbko hitwake: OnSleepWakeTask with a simulated hit
    std::string ConsoleToy();                                       // sbko toy: the player's hands (no NPC needed)
    std::string ConsoleRefill(RE::Actor* a_actor);                  // sbko refill: RefillGuard's state + its decision for her

    // u7a console tests (same rules as u6h's): U4 FDC re-roll, U7 take-over, M2 woken-to-talk
    std::string ConsoleFdc(RE::Actor* a_actor);                     // sbko fdc: her FDC state (reads only)
    std::string ConsoleFdcHit(RE::Actor* a_actor);                  // sbko fdchit: OnFdcHitTask with a simulated qualifying hit
    std::string ConsoleTakeOver(RE::Actor* a_actor, float a_seconds);   // sbko takeover [seconds]: the TakeOver native's path + its task
    std::string ConsoleHolds(RE::Actor* a_actor);                   // sbko holds: the Holds native's answer
    std::string ConsoleRelease(RE::Actor* a_actor, float a_seconds);    // sbko release [seconds]: the ChokeReleased native's path + its task
    std::string ConsoleWoken(RE::Actor* a_actor);                   // sbko woken: NarrateWoken(kind 1) for her now
    std::string ConsoleSleepState(RE::Actor* a_actor);              // sbko sleepstate: her sit-sleep state and the M2 rules (reads only)

    // u7a (U7): the SBKO_API natives' work (Api.cpp). Called on a Papyrus thread (or the console's, for the tests): reads, SBKO's held
    // set (g_watchMtx), a log line, TaskRelay - NEVER the knockout mutex, never a VM call.
    bool ApiTakeOver(RE::Actor* a_target, RE::Actor* a_aggressor, std::string_view a_source, float a_seconds);
    bool ApiHolds(RE::Actor* a_target);
    void ApiChokeReleased(RE::Actor* a_victim, float a_seconds, const char* a_from);

    // m34 (build 3): the ONE heal rule (the U1 refined test, IsHeal) for Marks.cpp's choke-mark heal - a pure read of the effect form
    [[nodiscard]] bool IsHealEffect(const RE::EffectSetting* a_m);
}
