#pragma once

// The Papyrus "hands" (SBKO_Helper.psc) - one C++ function per Papyrus signature, Ints for
// FormIDs, argument counts EXACTLY as in the .psc (a wrong count logs one VM line and does nothing; a missing script is
// a crash, hence VmCall's presence guard). ⛔ Call these from an SKSE task only - never from an event sink, never with
// the knockout mutex held. Every reply function runs on a Papyrus VM thread: it may log and TaskRelay::Add, nothing else.
//
//   Float Function KnockOut(Int aiFormId, Int aiNotify) Global                                      -> saved HealRate, or -1.0
//   Function WakeUp(Int aiFormId, Float afSavedHealRate, Int aiComingTo, Int aiNotify, Float afWakeHealth, Float afWakeFrac) Global
//                                                                    (-2.0 = base HealRate; u2d: Health raised to >= afWakeHealth)
//   Function SetRank(Int aiFormId, Int aiRank) Global
//   Float Function HealthPct(Int aiFormId) Global                                                   -> 0..1
//   Function NarrateWake(Int aiFormId, Int aiHours, Int aiNotify, Int aiNarrate) Global              (M1c: timer wake only)
// U1, the guarded unconscious:
//   Float Function KnockOutU(Int aiFormId, Int aiNotify, Int aiFromBleedout, Int aiNoFdcRoll) Global -> saved HealRate, or -1.0
//                                    (u6 fix 2: aiNoFdcRoll 1 = FDC_SBKO_RolledFaction rank 1 BEFORE rank 10 - a hit knock-out
//                                     never gets Follower Death Chance's roll; the DLL sends 1 only when FDC_SBKO_Patch.esp is loaded)
//   Int Function PotionsOff(Int aiFormId) Global                                                     -> mask 1|2|4 removed
//   Function PotionsBack(Int aiFormId, Int aiMask) Global
//   Float Function HoldHealthTo(Int aiFormId, Float afTargetHealth) Global     (fix 1: ABSOLUTE Health) -> Health after, or -1.0
//   Function HoldDown(Int aiFormId) Global
//   Function FinishWake(Int aiFormId, Float afSavedHealRate, Int aiRank, Int aiPotionMask, Int aiNotify, Float afWakeHealth,
//                       Float afWakeFrac) Global      (u2d: Health raised to >= afWakeHealth = fWakeHealthFraction x max, never lowered)
//   Function TestDrop(Int aiFormId) Global / Function TestMeddle(Int aiFormId) Global                (sbkou only)
// U2a / U2b (U2b: no bleedout window):
//   Function LetGo(Int aiFormId, Float afSavedHealRate, Int aiPotionMask, Int aiFlags) Global
//                                     (another mod took her; flags 1 NoBleedoutRecovery off, 2 unconscious off, 4 Paralysis 0,
//                                      8 HealRate + potions + stealth meter back; always rank 0)
//   Float Function ReAdopt(Int aiFormId, Int aiShove, Float afCarried, Int aiNoFdcRoll) Global   (u2a fix 1: released, still
//                                     unconscious -> held; u6 fix 2: + aiNoFdcRoll, as KnockOutU, for a re-adopted hit knock-out)
//   Function NarrateHealWake(Int aiFormId, Int aiNotify, Int aiNarrate) Global
//   Function SayTooInjured(Int aiFormId, Int aiNotify) Global         (U2b: "<name> is too injured to wake up." - another mod's wake)
// U2c: KnockOutU / ReAdopt / HoldDown turn NoBleedoutRecovery ON; FinishWake / LetGo / WakeUp / ClearMark turn it OFF
// (FinishWake's aiClearNoBleedout and LetGo's flag bit 1 are no longer read). No QueueNiNodeUpdate in any wake.
//   Function ClearMark(Int aiFormId, Int aiRank) Global              (a death / a stale rank-10 mark: NoBleedoutRecovery off + rank)
// u6c, the Sleep spell:
//   Function SetSleepMark(Int aiFormId, Int aiMark, Int aiNotify) Global   (SBKO_SleepFaction 0x80A rank: 2 put to sleep and she saw
//                                     the caster, 1 not seen, 0 any other knock-out / the coming-to ended; a non-member with 0 is left
//                                     alone; aiNotify 1 + aiMark > 0 = HUD "<name> falls into a deep sleep.")
// u7a:
//   Function FdcHit(Int aiFormId, String asHit, Int aiDeathOnly) Global     (U4: akNpc.SendModEvent("SBKO_FdcHit", asHit, deathOnly) -
//                                     the FDC x SBKO patch's running effect on her re-rolls; sent from Papyrus, never a C++ mod event
//                                     inside an SKSE task)
//   Function NarrateWoken(Int aiFormId, Int aiKind, Int aiMode, Int aiNotify, Int aiNarrate) Global
//                                    (M2: kind 1 a natural sleeper woken by the player to talk, 2 a Sleep-spell sleeper woken by the
//                                     player (Diary of Mine's Wake); mode 1 DirectNarration (she answers), 0 RegisterPersistentEvent)
// and SBKO_SeverCompat.psc (only when SeverActions.esp is loaded AND that script is installed):
//   Int Function Guard(Int aiFormId) Global                                -> -1 / -2 nothing done, 0 already excluded, >0 snapshot
//   Int Function Unguard(Int aiFormId, Int aiPacked) Global                -> -1 nothing done, 1 restored, 2 already included
namespace SBKO::Helper
{
    inline constexpr const char* kScript      = "SBKO_Helper";
    inline constexpr const char* kSeverScript = "SBKO_SeverCompat";

    // Is SBKO_Helper.pex installed? (cached per load; the absence is logged once, in plain words)
    [[nodiscard]] bool Present();
    // u7a (U7): the CACHED answer only - never TypeIsValid (safe inside a Papyrus native). -1 = not asked since the last load (the
    // check asks Present() every check, so it is known from the first check after a load), 0 = absent, 1 = present.
    [[nodiscard]] int PresentCached();

    // a_ok = the stack returned a Float (or an Int); a_value = that number. Runs on a VM thread.
    using FloatReply = std::function<void(bool a_ok, float a_value)>;
    using IntReply   = std::function<void(bool a_ok, std::int32_t a_value)>;   // U1: an Int kept exact (a packed snapshot)
    using DoneReply  = std::function<void()>;

    // Each returns false when nothing was dispatched (the reply will then never run).
    bool KnockOut(RE::FormID a_id, bool a_notify, FloatReply a_reply);
    bool WakeUp(RE::FormID a_id, float a_savedHealRate, std::int32_t a_comingTo, bool a_notify, float a_wakeHealth, float a_wakeFrac,
                DoneReply a_done);
    bool SetRank(RE::FormID a_id, std::int32_t a_rank);
    bool SetSleepMark(RE::FormID a_id, std::int32_t a_mark, bool a_notify);   // u6c: the Sleep spell's prompt marker
    bool HealthPct(RE::FormID a_id, FloatReply a_reply);

    // M1c: the timer-wake narration. a_narrate = SkyrimNetPresent() (the helper hands the line to SkyrimNet only then).
    bool NarrateWake(RE::FormID a_id, std::int32_t a_hours, bool a_notify, bool a_narrate);

    // M1c: does the VM know the script `SkyrimNetApi`? TypeIsValid, cached until ClearCache (kNewGame / kPostLoadGame);
    // the answer is logged once per cache fill.
    [[nodiscard]] bool SkyrimNetPresent();
    void               ClearCache();

    // ---- U1 ----
    bool KnockOutU(RE::FormID a_id, bool a_notify, bool a_fromBleedout, bool a_noFdcRoll, FloatReply a_reply);   // u6 fix 2: + a_noFdcRoll
    bool PotionsOff(RE::FormID a_id, IntReply a_reply);
    bool PotionsBack(RE::FormID a_id, std::int32_t a_mask);
    bool HoldHealthTo(RE::FormID a_id, float a_targetHealth, FloatReply a_reply);
    bool HoldDown(RE::FormID a_id, DoneReply a_done);
    bool FinishWake(RE::FormID a_id, float a_savedHealRate, std::int32_t a_rank, std::int32_t a_potionMask, bool a_notify,
                    float a_wakeHealth, float a_wakeFrac, DoneReply a_done);
    bool TestDrop(RE::FormID a_id);
    bool TestMeddle(RE::FormID a_id);

    // ---- U2a ----
    // u2e: + a_wakeHealth / a_wakeFrac (WakeHealth: a living NPC leaves SBKO with at least fWakeHealthFraction) - SBKO_Helper.pex u2e+
    bool LetGo(RE::FormID a_id, float a_savedHealRate, std::int32_t a_potionMask, std::int32_t a_flags, float a_wakeHealth,
               float a_wakeFrac, DoneReply a_done);
    bool ReAdopt(RE::FormID a_id, bool a_shove, float a_carriedHealRate, bool a_noFdcRoll, FloatReply a_reply);   // u6 fix 2
    bool NarrateHealWake(RE::FormID a_id, bool a_notify, bool a_narrate);
    bool SayTooInjured(RE::FormID a_id, bool a_notify);

    // 2.1.0 (bLootKnockedOut): SBKO_Helper.LootInventory - her inventory opens for the player (OpenInventory(true), free looting).
    // false = not dispatched (SBKO_Helper.pex without LootInventory, or the VM not ready)
    bool LootInventory(RE::FormID a_id);

    // ---- u7a ----
    bool FdcHit(RE::FormID a_id, const std::string& a_hit, bool a_deathOnly);                        // U4
    bool NarrateWoken(RE::FormID a_id, std::int32_t a_kind, std::int32_t a_mode, bool a_notify, bool a_narrate);   // M2

    // ---- u2c ----
    bool ClearMark(RE::FormID a_id, std::int32_t a_rank);   // NoBleedoutRecovery off (+ HealRate / stealth on a living NPC) + rank

    // SBKO_SeverCompat.psc. Call these ONLY when SeverActions.esp is loaded (the script links against SeverActions' types);
    // SeverPresent() is the VmCall presence guard for the script itself.
    [[nodiscard]] bool SeverPresent();
    bool SeverGuard(RE::FormID a_id, IntReply a_reply);
    bool SeverUnguard(RE::FormID a_id, std::int32_t a_packed, IntReply a_reply);
}
