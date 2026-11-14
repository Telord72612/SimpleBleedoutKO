#include "PCH.h"
#include "Knockout.h"
#include "Helper.h"
#include "Marks.h"               // m34 (build 3): the skin marks - fed by the sinks / tasks below, ticked at the check's end
#include "PerkEntryRunEvent.h"   // u7a (M2): the perk-fragment event (the toolbox only declares it)
#include "RefillGuard.h"   // u6h: the console test 'sbko refill'
#include "Settings.h"
#include "TaskRelay.h"

#include <optional>        // u6h: the console status snapshot

// =====================================================================================================================
// THE KNOCKOUT (M1). v10 line numbers (psc:N) refer to the v10 Papyrus source
// SBKO_DownWatcherScript.psc as it was before M1.
//
// THREAD MODEL (house rules):
//   * Event sinks (bleedout / death / cell attach-detach / U1: magic effect, animation graph) copy FormIDs and hand off -
//     TaskRelay::Add, the cell inbox, or (footsteps) one counter under their own mutex. They never take g_mtx and never
//     call AddTask (SKSE VR runs tasks inside its queue lock).
//   * ALL knockout work runs inside SKSE tasks: the check tick (one queued at a time, never self-re-adding) and the
//     relayed sink / reply tasks. In VR those tasks may run off the main thread -> g_mtx guards every shared
//     thing; it is taken once per task scope, never re-entered (a second lock by the same thread = CTD).
//   * Papyrus is dispatched only from a task (or the console command) and only with g_mtx RELEASED: decide under the lock,
//     collect `Call`s, unlock, Run(). Replies run on a VM thread: they log or TaskRelay::Add - never g_mtx, never AddTask.
//   * U1 lock order: g_mtx -> g_watchMtx and g_mtx -> g_footMtx, never the reverse. g_watchMtx / g_footMtx are held only
//     for a set / map operation - never around an engine call. The animation-graph attach / detach (which takes the
//     graph's event-source lock) runs in Run(), with no lock of ours held.
//   * u6b: the DAMAGE HOOK (any game thread: main, AI workers, Papyrus) takes ONLY g_u5Mtx or g_watchMtx, one at a time, each for a map /
//     set operation, never g_mtx, never AddTask (TaskRelay only), never Papyrus, never a log line. Lock order g_mtx -> g_u5Mtx (Detect and
//     the check may take it inside g_mtx); nothing takes g_mtx while holding g_u5Mtx.
//
// M1c: healing or time are the only things that wake an NPC -> v10's OnCellDetach wake is
// GONE: a DOWN NPC wakes only by the heal check or the game-hour timer. A TIMER wake is narrated: WakeUp with its HUD line
// off, then (when WakeUp's stack has finished) SBKO_Helper.NarrateWake with the hours she was out.
//
// M1d: an NPC recognised as knocked out in an unloaded cell is made conscious, so as to not create
// conflict and a permanent state -> at a game LOAD only, the first unpaused check makes every handled NPC who is NOT
// loaded conscious at once (WakeUp(id, saved, 0, 0)) and drops her; one not even in memory waits in g_pendingConscious
// (co-save) until she is first seen loaded. A cell change during play still wakes nobody (M1c).
//
// U1: THE GUARDED UNCONSCIOUS (transitions T1/T5-T7/T6b/T14
// below). A protected / essential NPC who enters bleedout is made UNCONSCIOUS
// (KnockOutU: SetUnconscious first, Paralysis 1, the 0.001 self-shove, StopCombat, HealRate 0, rank 10) instead of being kept
// in bleedout; Smart NPC Potions' abilities come off; a follower is excluded from SeverActions' survival tracking (its
// feeding + IdleForceDefaultState made a ragdolled NPC "moonwalk"). Each check then: re-asserts unconscious / Paralysis if
// anything cleared them (shove), holds her Health after the iGraceChecks settle (a rise is put back with DamageActorValue),
// re-shoves a ragdoll whose graph sends footsteps, and wakes her on the game-hour timer (FinishWake: unconscious off FIRST,
// then Paralysis 0, potions / HealRate / SeverActions back, rank 3 -> the coming-to stages, NarrateWake). M1 "DOWN" entries
// from an older co-save keep M1's rules until they wake. No new timer, cooldown or cap: the settle is iGraceChecks; the
// "in flight" flags only wait for a dispatched Papyrus call to return.
//
// u1 fix round 1:
//   * the HOLD follows her Health DOWN (rule: no health rise while she is out): every settled check the hold becomes
//     min(hold, her Health); it never rises - not even after a re-settle (T7 keeps it);
//   * M-1: the time wake (and the VRTE let-go) waits while a HoldDown / HoldHealthTo is still on the Papyrus stack, so a late
//     SetUnconscious / Paralysis can never land after FinishWake;
//   * M-2: Health in C++ = current / (permanent + temporary modifier) = Actor::GetActorValueMax, the engine's maximum, and the
//     put-back target goes to Papyrus as ABSOLUTE Health (HoldHealthTo) - one basis on both sides;
//   * M-5: "in bleedout while UNCONSCIOUS" is tested BEFORE the timer wake (re-assert first, wake at a later check).
//
// U2a (a held NPC could stay down through any number of Healing Hands applications): THE HEAL WAKE.
//   * Trigger: the first HEAL (the refined rule, IsHeal) from any Actor - herself included (she cannot drink while out, so a
//     self-cast heal is a fed potion, e.g. Gift by Hand's) - on an NPC held UNCONSCIOUS (the settle included). A non-actor
//     caster never counts. During ENTERING it is kept (healPending, co-save flags bit1) and acted on at KnockOutU's reply.
//     Later heals do nothing. The wake obeys M-1 (waits for a HoldDown / HoldHealthTo still in flight).
//   * bHealWakeWindow 1 (THIS order):
//       (a) WAKING   - WakeStand: unconscious off, Paralysis 0 -> the engine's own get-up, Health still at the hold;
//       (b) DROPPING - at GetUpEnd / GetUpExit (the per-actor graph sink) or a check that reads her standing (not ragdolled,
//                      knock state Normal) - else at the 2nd check after (a) anyway: DropToBleedout = NoBleedoutRecovery on +
//                      Health -10 % of max -> the engine puts the protected / essential NPC into bleedout FROM STANDING;
//       (c) WINDOW   - from the moment she is seen bleeding out: fWindowSeconds (15) of unpaused check time, Health kept <= 0,
//                      potions / HealRate / SeverActions exclusion kept; Diary of Mine's DOMBeingCaptured -> LetGo. Then
//                      EndWindow (M1's WakeUp path + potions back) + Unguard -> coming to + NarrateHealWake. Fallbacks (logged):
//                      not bleeding out after the drop -> EndWindow "could not start"; out of bleedout early -> EndWindow "left
//                      early"; dead -> T14; a save loaded mid-wake -> EndWindow, not a restarted window.
//   * bHealWakeWindow 0: the heal wakes her straight up (FinishWake).
//   * A wake by ANOTHER mod (counted, bExternalWakeCounts): ExternalClear() is the ONE place that decides what a
//     cleared unconscious flag / Paralysis means. Settled, not bleeding out, alive, not within the first 2 checks after a load /
//     cell attach / 3D load -> a straight wake (FinishWake, coming to, "<name> is awake."). Otherwise -> re-assert (U1).
//   * The SeverActions guard asks SBKO_SeverCompat.Guard for every NPC (membership decided in Papyrus by SeverActions' own
//     roster and survival list - a precision change). The effect log keeps each kind ONCE per knock-out and counts repeats.
//   * ANOTHER MOD TAKES HER (Diary of Mine's DOMInteractPerk offers Wake /
//     Tie-up / Bag / Abduct on EVERY unconscious NPC): TakenBy() - DOMBeingCaptured (Abduct), DOMActionInContainer (Bag), a
//     restrained life state (Tie-up) or a disabled reference - in ANY held state -> LetGo: SBKO gives back what it set and forgets
//     her, never fights. DoM's Wake is the external-wake rule above.
//   * The window drops her to -10 % of max: the ~4.9 HP/s regen seen while unconscious must not leave her above 0. A heal that
//     still lifts her out = "left the window early" (not fought).
// u2a fix round 1:
//   * RELEASED WATCH: an NPC let go with her unconscious flag on (a captive, a tie, VRTE) is kept
//     in g_released (co-save record 'RELS'). Each check: alive, loaded, still unconscious and NO LONGER taken -> RE-ADOPTED as
//     UNCONSCIOUS (ReAdopt: Paralysis 1, HealRate 0 with her original value carried, potions off, the guard, rank 10). Conscious /
//     dead / deleted -> dropped (VRTE's deferred part - HealRate / potions / SeverActions - is given back then). No timer.
//   * Every load-safety wake and "unconscious off" LetGo for an NPC with no 3D, and EVERY bag LetGo, also leaves a one-shot BELT in
//     g_pendingConscious: when she is next seen loaded, still unconscious and not taken, the wake runs again.
//   * WINDOW: a rise above the drop level + 1 % with NO heal since the drop is put back to the drop level; EndWindow always ends
//     above 0.
//   * The external-wake rule exactly: re-assert only in the knock-out's settle, in bleedout (T7), within 2 checks of
//     a load / cell attach / 3D load, or for a Paralysis-only clear. An SBKO call still in flight is handled by the reply gate.
//   * TakenBy() - now also VRTE's rank 5 - is asked before every heal-wake step and in ANY held state -> LetGo.
//   * LetGo has a reply: not dispatched -> she is kept and it is retried at the next check. bLogEffects 0 silences the
//     EFFECT lines.
// U2b (no bleedout after unconscious - Diary of Mine's menu already works on an unconscious NPC):
//   * NO bleedout window: WAKING / DROPPING, DropToBleedout, EndWindow, the get-up relay, the window hold, bHealWakeWindow and
//     fWindowSeconds are gone. A heal wakes her STRAIGHT UP (FinishWake + NarrateHealWake, rank 3 coming to).
//   * "Wake" does NOT wake (they need healing): ExternalClear() re-asserts an external clear of the
//     unconscious flag and - when it is a real attempt (settled, alive, not in bleedout, not right after a load / attach / 3D load) -
//     SayTooInjured shows ONE line "<name> is too injured to wake up." per clear. bExternalWakeCounts is gone.
//   * Bag / Abduct / Tie-up / VRTE: SBKO lets go (TakenBy -> LetGo), the released watch and the belts stay as they were.
//   * State::kWindow survives only as a u2a-save leftover: such an entry is finished at the first check (FinishWake + NoBleedoutRecovery
//     off), never restarted.
// U2c (six fixes + a performance pass):
//   1. no QueueNiNodeUpdate in any wake (SBKO_Helper.psc) - the heal-wake vanish / pops.
//   2. light-plugin (FE) FormIDs: a new co-save record 'LREF' v1, written FIRST, holds the plugin name + local id of every FE id in
//      STAT / the pending list / RELS (their layouts are unchanged, so an older DLL still reads them). On load an FE id is rebuilt
//      from the plugin name; an FE id from an older save goes through ResolveFormID and is repaired if her plugin became a regular
//      one (skyrimvresl's formula keeps the old light slot in bits 12-23); FF ids stay raw.
//   3. the ORPHAN safety net: rank 10 + unconscious + no SBKO record + not taken -> adopted as UNCONSCIOUS (ReAdopt), checked on a
//      cell attach and once after a load (never a faction read for every actor every check); rank 10 on a conscious NPC with no
//      record -> ClearMark (rank 0). An adopt drops the "corrupted twins" (pending / released ids mangled from her FE id).
//   4. NoBleedoutRecovery ON while held (KnockOutU / ReAdopt / HoldDown), OFF on every exit (FinishWake, LetGo, WakeUp, ClearMark at a
//      death) - the essential regen stops at the source. Put-backs are counted per NPC in the PERF line.
//   5. the coming-to clock runs on unpaused check time whether or not she has 3D; a step taken while she is not in memory is written
//      when she is found (rankDirty). The load safety leaves a COMING_TO NPC to that clock.
//   6. log fixes: the cell-detach line, a real negative HP %, the wake's parity line after FinishWake (not racing it), and ONE line
//      for why an NPC in bleedout was not knocked out (once per NPC per bleedout).
//   P. performance: one PERF line per minute of play (measurement only - never a limit); bVerboseChecks 0 by default; no flush per
//      line (main.cpp); constant-effect (ability) re-applications at a 3D load are counted, not logged one by one; the graph
//      listener is re-attached only when her graph changed; the per-check name / effect strings are built only when logged.
// U2d: (1) the game setting fEssentialNonCombatHealRateBonus is written from the ini ([Regen], e.g. 0.1;
//   engine default 5.0) at data load and after every load (ApplyRegenSetting); (2) every wake raises her Health to AT LEAST
//   fWakeHealthFraction (0.25) of her max, never lowers it (WakeHealthTarget -> FinishWake / WakeUp); (3) a 1.5 s bleedout:
//   an NPC stays fBleedoutGraceSeconds (1.5) in the engine's own bleedout before SBKO knocks her out (g_grace; the check thread
//   wakes at the due time -> GraceTask -> ProcessGrace -> Detect re-checks everything) - a deliberate SEQUENCING so other mods that
//   act on bleedout (Follower Death Chance, SeverActions, DoM) get their turn first, not a limit; (5) the knock-out line logs
//   essential / protected for the reference and the base separately.
// U6 (rules R1-R8): THE HEAD-HIT / SNEAK
//   KNOCK-OUT inside SBKO (= the Knockout mod's rules + a blunt weapon to the head). A TESHitEvent sink copies the PLAYER's hits on
//   actors (target, source, the flags WORD at +0x18 and - for PLANCK's extended hit event - the hit node's name, whether it or one
//   of its first 3 parents is "NPC Head [Head]", the contact point) and relays them. The task (OnHitTask) decides: a punch or a bash
//   counts anywhere, a mace / warhammer (keyword by ID) only on PLANCK's head node; any NPC that is alive, not a ghost, not bleeding
//   out, not already down / held / declined by SBKO, not taken by another mod (TakenBy, incl. VRTE), not in a brawl or the Companions'
//   fist-fight, followers only with bHeadHitFollowers; a worn ArmorHelmet item = immune; then 100 % (no roll) when the hit is a
//   sneak attack, OR she does not detect the player (RequestDetectionLevel(player, kNormal) <= 0 - the exact test of Papyrus
//   IsDetectedBy, VR 0x985720), OR her Health <= fHeadHitHealthThreshold. The knock-out = SBKO's own (an Entry + KnockOutU with
//   aiFromBleedout 0: Paralysis, shove, then unconscious - VRTE's order); everything after it is the bleedout knock-out's (heal /
//   timer wake, potions, SeverActions guard, RefillGuard, FDC). ONE difference: the HOLD of a hit knock-out is her Health at the
//   knock-out (min(that, her Health at the settle), floor 1 %) - no fUnconsciousHealthFraction cap (Entry::hitKO, co-save flags
//   bit 0x20; an older save = a bleedout knock-out). No timer, roll, cap or cooldown anywhere (a standing rule).
// u6 fix round 1: F1 the hit flag survives the released watch (Released::hitKO, RELS flag 0x04) into a
//   re-adoption; F2 low Health <= threshold + 0.1 % (float tolerance); F3 detection is the LAST test; F5 PLANCK's GetCurrentHitEvent()
//   confirms the magic word when its interface is there; F7 / F8 log wording + a logged source-0 hit.
// u6 fix round 2 (two rule changes): (1) no punch knock-out - only a blunt weapon or a bash does it: a
//   punch never knocks out and makes no line (bHeadHitFists and F8's source-0 line are gone); a bash anywhere, a mace / warhammer on
//   the head. (2) a follower's hit knock-out gets no FDC roll (her HP is still over 0) - a hit knock-out (and its
//   re-adoption) is pre-marked FDC_SBKO_RolledFaction rank 1 (u7a: 2) BEFORE rank 10 (KnockOutU / ReAdopt aiNoFdcRoll 1, only with
//   FDC_SBKO_Patch.esp loaded), so the FDC x SBKO patch's Claim() says "already had its roll". Bleedout knock-outs still roll.
// u6 fix round 3: PLANCK's extended hit block is read ONLY when activeragdoll.dll is loaded (g_planckPresent,
//   decided once at kDataLoaded) - SE / AE / VR without PLANCK never follow the magic word alone; GetCurrentHitEvent() still confirms it.
// u6b (two rules):
//   (1) U6 without PLANCK (the Knockout mod works on every Skyrim version without PLANCK; PLANCK stays the more precise
//       VR path): with PLANCK loaded a mace / warhammer counts on the HEAD only (unchanged); WITHOUT it (SE / AE always,
//       VR without activeragdoll.dll) it counts ANYWHERE, like the Knockout mod (which has no hit-location test at all). Logged per hit.
//   (2) U5 THE CHANCE KNOCK-OUT of a NORMAL NPC (not essential, not protected, ActorTypeNPC, not a ghost): EVERY direct hit by anyone
//       that leaves her at 0 < Health <= fChanceKOHealthBelow (0.15) rolls the formula (helmet / boots / gloves / body / level
//       difference attacker - victim / this hit's damage) - won = SBKO's own hit knock-out (StartKnockOutU, hitKO: held at her Health,
//       no 25 % pull-down), lost = SBKO does nothing (the game decides: she may kneel in bleedout - "acting hurt" - or fight on).
//       A direct hit = a weapon / arrow / bolt / bash / a creature's claw or bite / a spell's impact / a concentration beam (one roll per
//       beam); NOT a tick (ShouldModifyOnUpdate: burning, poison, linger), a cloak's or a hazard's damage, a weapon enchantment / poison
//       on its own (it rides on the weapon hit), attacker-less damage, or a PUNCH (bare hands - the U6 punch removal, Injury Engine /
//       Diary of Mine). Where: a HandleHealthDamage hook on Character (vtable slot 0x104 SE / AE, 0x106 VR - CommonLib's
//       RelocateVirtual(0x104, 0x106)), PRE-call: Health is already written, the attacker is known; it chains to whatever was in the slot
//       (Injury Engine's own hook, installed at its plugin load) and never changes the damage. A thread_local "which value-modifier
//       effect is applying a value on this thread" scope (vfunc 0x20 on 6 effect vtables) tells a spell from a weapon, and the
//       TESHitEvent this thread sent just before the damage names the weapon (a punch). The hook only copies + relays; the armour read
//       and the roll run in an SKSE task (OnChanceKOTask). NORMAL NPCs' BLEEDOUT no longer knocks them out by itself: the roll of the
//       hit that started it decides (one roll per hit); a bleedout that no rolled hit started (a tick, no attacker, her Health above the
//       line after the hit) rolls ONCE at the bleedout (PickBleedout); one a punch started never rolls (she acts hurt). Essential /
//       protected NPCs: unchanged. Deaths are never touched (HP at 0 = dead).
// u6d: non-hostile weapons (torture whip / DoM slap) never roll / knock out (F1); an enchantment /
//   poison belongs to THIS thread's last swing (t_swing, F2); + 2 effect scopes, a spell-sourced hit event = a spell (F3); one hit event =
//   one damage (F4); summons never knocked out (F7); vtable slots checked before writing (F8); fewer misleading bleedout lines (F10).
// u6c (the Sleep spell): an effect carrying
//   SBKO_KnockOutEffect (ESP 0x804; SBKO_SleepEffect 0x805 = the Sleep spell / scroll) cast by the PLAYER on a valid NPC = SBKO's own
//   HIT knock-out, 100 % (a spell works 100 % if it hits: no roll, no detection or Health rule, no helmet test; U6's targets).
//   EffectSink tests it FIRST (caster = the player, effect in the sorted list built at kDataLoaded) and relays to OnSleepTask, which
//   reads her detection of the player ONCE (RequestDetectionLevel > 0 = Papyrus IsDetectedBy) only to pick the wake prompt:
//   SBKO_SleepFaction (0x80A) rank 2 = she saw the caster, 1 = she did not (SBKO_Helper.SetSleepMark). EVERY knock-out writes that
//   rank (0 for the bleedout / U6 / U5 ones) and the coming-to's end writes 0. Without the ESP records: off, one start line. [Sleep].
// u6f (a Sleep-spell sleeper could not be woken by a hit or by talking to her - it is just sleep - and a sneak hit could fail
//   because a cape or a hair item counted as a helmet - the log: "helmet YES: Leather Cape",
//   "helmet YES: [KS] Glow"):
//   (1) HELMET RULE A replaces the ArmorHelmet keyword test in U6's immunity AND U5's helmet factor (one
//       helper, HelmetRuleA): a helmet = a WORN armour covering biped slot 30 (head) or 31 (hair) that is Light or Heavy armour. Clothing
//       and every item off slots 30 / 31 never count, whatever keyword they carry; Devious Devices items are skipped. The lines name each
//       head-ish item that did NOT count and why.
//   (2) A SLEEP-SPELL sleeper (Entry::sleepMark >= 1 - co-saved since u6f, STAT flags 0x40 / 0x80, RELS 0x08 / 0x10) wakes STRAIGHT UP
//       (the heal wake's path: FinishWake rank 3, NarrateHealWake - "wakes up") on ANY TESHitEvent on her by anyone (bSleepWakeOnHit; the
//       Sleep spell's own projectile hit and her own self-shove excepted) or when the PLAYER activates / talks to her (TESActivateEvent,
//       bSleepWakeOnTalk) - also when another mod clears her unconscious flag on purpose (Diary of Mine's menu "Wake", the external-clear
//       rule's "real attempt"). The sinks test one set (g_sleepWatch, rebuilt by SyncCount; empty = an atomic early-out), copy FormIDs and
//       relay OnSleepWakeTask. A wake asked while KnockOutU has not replied or a hold call is in flight waits for it (sleepWakePending),
//       exactly as a heal does (M-1). Injury knock-outs (bleedout, U6, U5) are unchanged: only a heal or the timer wakes them.
// u6g (wig / hair mods such as KS Hairdos are excluded): [HeadHit] sHelmetIgnorePlugins = a comma list
//   of plugin file names (case-insensitive). A worn armour whose ORIGINATING file (TESForm::GetFile(0) = the first of its sourceFiles,
//   the plugin that defines the FormID - an override such as Wpn,Armor Synthesis.esp does not matter) is on the list is never a helmet
//   (HelmetWhy::kIgnoredPlugin, tested after the slot / type tests - so the log only names it when rule A would have counted it). One
//   helper, so U6's immunity and U5's helmet factor both follow it. Default = common wig / hair plugins that rule A
//   would count. Start line: the list, each name loaded or not (once).
// u6h (console tests + regen 0.01;
//   the stuck-KO gaps G3 / G4):
//   G3 a TESDeathEvent whose task finds her NOT dead any more (the death was cancelled / undone) keeps her: no ClearMark, nothing given
//      back, her entry stays held, a cleared flag is re-asserted quietly for 2 checks (the engine-reset rule), then the usual rules
//      (heal / timer wake). A death that is real when the task runs is handled as before (and the check still catches a missed one).
//   G4 the M1d rule (a knocked-out NPC in an unloaded cell is made conscious): an ORPHAN (rank 10 + in bleedout / unconscious /
//      paralysed - fix 1: bleedout too -, no record, not taken) that ARRIVES with a cell attach during play is WOKEN at once (MakeConscious:
//      FinishWake rank 0 - unconscious
//      off, Paralysis 0, HealRate, stealth, NoBleedoutRecovery off) instead of being held for a new 6-12 h. An orphan found right after a
//      LOAD in the loaded area (the after-load pass, or a cell that attached during the load) is still re-held, as before.
//   Console tests: 'sbko <test>' (TestCmd.cpp) runs the SAME task functions the sinks relay to, synchronously on the console's thread
//      (OnSleepTask, OnHitTask, OnChanceKOTask with a dry flag, OnSleepWakeTask), with the input the sink would have copied; each of
//      them now returns a TaskResult (the sinks ignore it) so the console can print ONE line. 'sbko status' is reads only.
// u7a (build 2:
//   U4, U7, M2):
//   U4 FOLLOWER DEATH CHANCE RE-ROLLS ON EVERY HIT (each hit is a re-roll for FDC to kill that NPC; only OTHER people's hits,
//      a miss = nothing, a protected / essential non-follower never dies while knocked out). A follower SBKO holds UNCONSCIOUS after a
//      BLEEDOUT knock-out (not a hit / sleep / choke one - g_fdcWatch, rebuilt by SyncCount) that takes a qualifying DIRECT hit (the U5
//      rules: a weapon, arrow, bash, creature attack, spell impact, one per concentration beam; never a tick, cloak, hazard, enchantment /
//      poison, punch, toy, 0-damage hit, attacker-less damage; never the player's own - bFdcRerollPlayerHits 0) -> OnFdcHitTask checks
//      rank 10, FDC_SBKO_RolledFaction rank 1 (FDC rolled once; 2 = a hit / sleep / choke knock-out, never) and the player in combat
//      (FDC's own gate) -> SBKO_Helper.FdcHit sends the mod event SBKO_FdcHit FROM PAPYRUS (never a C++ mod event inside a task);
//      the FDC x SBKO patch's running effect on her re-rolls with FDC's own settings. The damage hook is installed for U5 OR U4 now.
//   U7 VRTE HANDS A CHOKED-OUT NPC TO SBKO (handed over when SBKO is present, kept in VRTE when it is not; SBKO's
//      rules apply): the SBKO_API natives (Api.cpp). TakeOver = SBKO's own CHOKE knock-out (a hit-class one: held at her Health, no FDC
//      roll, KnockOutU in VRTE's order, quiet - VRTE's passout line speaks), cls kClsChoke, SBKO_SleepFaction rank 3 = "choked" for the
//      prompts; it wakes ONLY on a real heal or SBKO's 6-12 h timer (NOT the Sleep spell's light class - SBKO's rules); DoM's Wake =
//      "too injured". The seconds held are co-saved on the entry (record 'CHOK' v1) for build 3's neck mark; ChokeReleased(Actor, Float)
//      (an addition for the marks) records the TOTAL seconds when the player lets go - on her choke entry, or, with no
//      take-over (let go before the passout), in a small co-saved list build 3 reads.
//   M2 "WOKEN TO TALK" (SkyrimNet is told "(Player's name) just woke you to talk to you"): the player activating an NPC asleep
//      in a bed (TESActivateEvent, not sneaking, sit-sleep state 7 / 8) or picking Diary of Mine's menu Wake on her (TESPerkEntryRunEvent,
//      DOMInteractPerk fragment 0) -> pending; the next unpaused check that sees her no longer asleep -> SBKO_Helper.NarrateWoken
//      ("<player> has just woken <NPC> up to talk to <NPC>.", DirectNarration - she answers). Bag / Abduct / Untie / Tie-up cancel it;
//      still asleep after 2 checks = Cancel / pickpocket -> nothing. DoM's Wake on a Sleep-spell sleeper -> her wake is narrated "woken from
//      a deep sleep" by the player. Probe lines (bLogWokenProbe) settle the two UNVERIFIED VR event layouts.
// m34 (build 3 = M3 / M4 skin marks + the choke neck mark;
//   see Marks.cpp): the work is in Marks.cpp. This file only FEEDS it: HitSink names every actor hit
//   (Marks::NoteHit - the attribution of Injury Engine's ping), EffectSink hands effects on MARKED actors to Marks::NoteEffect (before its own
//   "nobody held" fast path), the death sink also relays while marked actors exist and OnDeathTask calls Marks::OnDeath, DrainCells calls
//   Marks::OnAttach / OnDetach (with no g_mtx held), OnTakeOverTask / OnChokeReleasedTask call Marks::OnChoke with the seconds held, the check
//   calls Marks::Tick(loaded) at its very end (no lock held), and the co-save writes / reads record 'MARK' v1 (its FE ids in 'LREF'). The
//   effect / hit sinks are also registered when the marks are on. Lock order: g_mtx -> Marks' own mutex (WriteLightRefs); never the reverse.
// =====================================================================================================================
namespace SBKO::Knockout
{
    std::atomic<std::uint64_t> g_logLines{ 0 };   // u2c P1: bumped by the counting sink main.cpp adds to the logger

    namespace
    {
        // ---- the SkyrimNet contract (prompts 0797-0799 read these ranks) - constants, never ini keys ----------------
        constexpr std::int32_t kRankDown     = 10;   // v10 RANK_DOWN (psc:30)
        constexpr std::int32_t kRankComingTo = 3;    // v10 COMING_TO (psc:31)

        // savedHealRate -2.0 = "the saved value is lost; WakeUp restores her BASE HealRate" (an NPC adopted from a v10
        // save, whose saved value lived in the old script; or one saved while KnockOut had not replied yet)
        constexpr float kHealRateUseBase = -2.0f;

        constexpr std::uint32_t kRecordType    = 'STAT';
        // M1c: v2 adds koHours; M1d: v3 adds the pendingConscious list; U1: v4 adds the unconscious fields (v1-v3 read).
        // U2a: the LAYOUT is unchanged (still v4): state byte 5 (WINDOW) and flags bit1 (healPending) are now used. U2b: the same
        // layout; state 5 is only a u2a heal-wake window leftover (finished at the first check, written back while unfinished).
        constexpr std::uint32_t kRecordVersion = 4;
        // u2a fix 1: the released watch lives in its OWN record (an older DLL skips an unknown type; STAT's layout is unchanged)
        constexpr std::uint32_t kReleasedType    = 'RELS';
        constexpr std::uint32_t kReleasedVersion = 1;
        // u2c fix 2: the light-plugin refs - for every FE id in STAT / pending / RELS: the raw id, the local id (& 0xFFF) and her
        // plugin's file name. Written BEFORE STAT and RELS (the reader needs it first); an older DLL skips the unknown type.
        constexpr std::uint32_t kLightRefType    = 'LREF';
        constexpr std::uint32_t kLightRefVersion = 1;
        // u7a (U7): the choke seconds - its OWN record (STAT / RELS layouts unchanged; an older DLL skips an unknown type). Written LAST.
        // count u32, then per item: id u32, where u8 (0 = her STAT entry, a choke knock-out; 1 = her RELS entry; 2 = a loose choke with no
        // knock-out), seconds f32, game hour f32
        constexpr std::uint32_t kChokeType    = 'CHOK';
        constexpr std::uint32_t kChokeVersion = 1;
        // u2c fix 6: "not read" for a Health fraction (a real one can be negative: an NPC below 0 HP in bleedout)
        constexpr float kNoRead = -1000.0f;

        constexpr RE::FormID kFactionLocalID = 0x803;          // SBKO_StateFaction (psc:190)
        constexpr const char* kPlugin        = "SimpleBleedoutKO_001.esp";
        constexpr RE::FormID kActorTypeNPC   = 0x00013794;     // Skyrim.esm keyword, by ID (no BGSDefaultObjectManager on the fork)
        // m1f brawl guard: DGIntimidateFaction (Skyrim.esm, flag SpecialCombat) - vanilla brawls and SeverActions' BrawlManager
        // route a brawler's damage into bleedout through it; a brawl loser is never knocked out.
        constexpr RE::FormID kBrawlFaction   = 0x0004CFA6;
        // U1: CurrentFollowerFaction (Skyrim.esm; winning override USSEP) - rank >= 0 = a player follower (U2a: log only)
        constexpr RE::FormID kFollowerFaction = 0x0005C84E;
        // U1: VRTE_ChokeStateFaction (VRTouchEvents.esp 0x804) - rank 5 = VRTE holds her in its own KO
        // (VRTouch_MainScript.psc :2373-2380). Soft lookup: absent mod = no rule.
        constexpr RE::FormID  kVrteFactionLocalID = 0x804;
        constexpr const char* kVrtePlugin         = "VRTouchEvents.esp";
        constexpr std::int32_t kVrteKoRank        = 5;
        // U1: Smart NPC Potions' drink-potion abilities (NPCpotions_Spell 0xD62, _SpellMage 0x80A,
        // _SpellAssassin 0x80B, all Ability; VRTE.psc:472-474). Mask bit i = kPotionLocal[i] (1, 2, 4 - VRTE's KOPotionsOff).
        constexpr const char* kPotionPlugin   = "Smart_NPC_Potions.esp";
        constexpr RE::FormID  kPotionLocal[3] = { 0xD62, 0x80A, 0x80B };
        // U1: SeverActions' survival script sits on quest SeverActions (000D62:SeverActions.esp, VMAD script 24)
        constexpr const char* kSeverPlugin     = "SeverActions.esp";
        constexpr RE::FormID  kSeverQuestLocal = 0xD62;
        // U2a: Diary of Mine's DOMBeingCaptured (1598FF:DiaryOfMine.esm; DOMAbductionMagicEffect.psc:20-23
        // rank 2, DOM_SlaveManager.psc:262-264 rank 1). Any membership during the window -> SBKO lets go. Soft lookup.
        constexpr RE::FormID  kDomFactionLocal = 0x1598FF;
        constexpr const char* kDomPlugin       = "DiaryOfMine.esm";
        // U2a: Diary of Mine's DOMActionInContainer (103D96:DiaryOfMine.esm) - rank 1 = she is in a sack
        // (DOM_Keys.psc:11074 / DOM_Actor.psc:17971 / DOM_PAH.psc:2635 set it BEFORE MoveTo(DOMTonyMarker); the release removes it,
        // DOM_Keys.psc:11198). A custom DOMBag spell goes through the same DOMDoPutInBagActor (DOMBag_SpellEffect.psc:18).
        constexpr RE::FormID  kDomBagFactionLocal = 0x103D96;
        // U2a: Diary of Mine's DOMActionTied (58AB49:DiaryOfMine.esm) - DOM_Interactor.TieupActor sets it (rank 3)
        // BEFORE SetRestrained. A belt: if SetRestrained does not change the life state in VR, the faction still says
        // "tied" - SBKO lets go and never fights the tie.
        constexpr RE::FormID  kDomTiedFactionLocal = 0x58AB49;
        // u2a fix 1: Diary of Mine's DOMActorFaction (58FC50:DiaryOfMine.esm) = a DoM slave. A RELEASED NPC
        // who is now a DoM slave is never re-adopted (DoM owns her - SBKO never fights DoM); the watch waits.
        constexpr RE::FormID  kDomSlaveFactionLocal = 0x58FC50;

        // ---- U6: the head-hit / sneak knock-out (Skyrim.esm forms by ID) ----
        constexpr RE::FormID    kActorTypeGhost   = 0x000D205E;   // keyword ActorTypeGhost (Knockout's listener filter: never a ghost)
        constexpr RE::FormID    kArmorHelmet      = 0x0006C0EE;   // keyword ArmorHelmet - u6f: NOT the helmet test any more (rule A, slot +
                                                                  // armour type); only named in the log when an item carries it but fails
        constexpr RE::FormID    kWeapTypeMace     = 0x0001E714;   // keyword WeapTypeMace (one-hand mace = blunt, R3)
        constexpr RE::FormID    kWeapTypeHammer   = 0x0006D930;   // keyword WeapTypeWarhammer (warhammers share kTwoHandAxe with
                                                                  // battleaxes, so the keyword is the test)
        constexpr RE::FormID    kQuestBrawl       = 0x00047AE6;   // DGIntimidateQuest - a vanilla brawl is running (Knockout's cloak)
        constexpr RE::FormID    kQuestC00         = 0x0004B2D9;   // C00 (Take Up Arms) - stage 20 = the fist-fight with Vilkas
        constexpr RE::FormID    kQuestC00Fight    = 0x000BC0BD;   // C00JorrvaskrFight - stage 1 = that fight
        constexpr std::uint16_t kC00FightStage      = 20;
        constexpr std::uint16_t kC00JorrvaskrStage  = 1;
        // TESHitEvent flags (TESHitEvent.h:15-19) - the LOW byte of the word at +0x18 (PLANCK keeps them there)
        constexpr std::uint32_t kHitPower = 1u << 0;
        constexpr std::uint32_t kHitSneak = 1u << 1;
        constexpr std::uint32_t kHitBash  = 1u << 2;
        // PLANCK's extended hit event (fork b04495f include/planckinterface001.h:11-35, src/main.cpp:6086-6101; PPB reads the same
        // layout live, RagFrame.cpp:564-580): TESHitEvent (0x20) + HitData* +0x20 + position +0x28 + velocity +0x34 + node +0x40 +
        // node name +0x48 + isLeft +0x50. Valid ONLY during the sink call. The magic sits in the top 3 bytes of the flags word.
        constexpr std::uint32_t kPlanckMagic   = 0x59914000u;
        constexpr const char*   kHeadNode      = "NPC Head [Head]";
        constexpr int           kHeadParents   = 3;      // also a body on a child of the head bone counts (the neck never walks up to it)
        constexpr std::int32_t  kNoDetectRecord = -1000; // RequestDetectionLevel's "no process / no detection record" (VR 0x6051D6)
        constexpr float         kLowHealthTolerance = 0.001f;   // u6 fix 1 (F2): float tolerance of the low-Health trigger (0.1 %)

        // the 1 % tolerance of the health lock: float noise only, not a gameplay limit
        constexpr float kLockTolerance = 0.01f;
        // the lowest lock a put-back may aim at: SBKO's own DamageActorValue must never take her to 0 (bleedout / death)
        constexpr float kLockFloor = 0.01f;
        // fix 1: a fall smaller than this (0.01 % of max) is float noise from SBKO's own put-back, not damage - the hold does not
        // follow it (a measurement tolerance, like kLockTolerance; not a gameplay limit)
        constexpr float kFallNoise = 0.0001f;
        // U2a (the rule for an external clear): within this many checks after a game load / cell attach / 3D load, a cleared
        // unconscious flag is an ENGINE RESET, not a deliberate wake - a correctness rule (the engine re-initialises the actor), not
        // a gameplay limit (the first 2 checks).
        constexpr std::int32_t kResetChecks = 2;

        enum class State : std::uint8_t
        {
            kEntering    = 0,   // KnockOut(U) dispatched, its reply not back yet = v10's bBusy (psc:39): no wake possible
            kDown        = 1,   // M1 knocked out = kept in bleedout (v10 bIsDown) - U1: legacy, from an older co-save only
            kComingTo    = 2,   // awake, rank 3 -> 2 -> 1 -> 0 (v10 iComingTo > 0)
            kUnconscious = 3,   // U1: unconscious + Paralysis 1 + ragdoll, Health held, HealRate 0, potions off
            kWindow      = 5,   // U2b: ONLY a u2a-save leftover (the old heal-wake window) - finished at the first check with
                                // FinishWake + NoBleedoutRecovery off; never entered any more (state 4 / 6 were never saved)
        };

        const char* StateName(State a_state)
        {
            switch (a_state) {
                case State::kEntering:    return "ENTERING";
                case State::kDown:        return "DOWN";
                case State::kComingTo:    return "COMING_TO";
                case State::kUnconscious: return "UNCONSCIOUS";
                case State::kWindow:      return "WINDOW (u2a leftover)";
            }
            return "?";
        }

        // a state in which SBKO still holds her (potions off, SeverActions exclusion kept, HealRate 0, effects watched)
        bool Held(State a_s)
        {
            return a_s == State::kEntering || a_s == State::kUnconscious || a_s == State::kWindow;
        }

        // U2a: what a COMING_TO entry's wake was (the save maps a wake still running by it)
        enum class WakeKind : std::uint8_t
        {
            kTime       = 0,   // the game-hour timer (FinishWake + NarrateWake)
            kStraight   = 2,   // a heal (FinishWake + NarrateHealWake), or finishing a u2a-save leftover
        };

        struct Entry
        {
            State         state         = State::kEntering;
            std::uint64_t serial        = 0;                  // changes on every transition that waits for a reply
            float         savedHealRate = kHealRateUseBase;   // v10 fSavedHealRate (psc:42), from KnockOut's reply
            float         recoverHours  = 0.0f;               // v10 fRecoverHours (psc:41), the rolled 6-12
            float         wakeAtHours   = 0.0f;               // game hours passed at which the timer wakes her
            float         dispatchHours = 0.0f;               // ENTERING: game hours when KnockOut was dispatched
            float         koHours       = 0.0f;               // M1c: game hours at the knock-out (adoption time for a v10 leftover)
            std::int32_t  comingToRank  = 0;                  // v10 iComingTo (psc:36)
            float         stageSeconds  = 0.0f;               // seconds of play on the current coming-to rank
            std::int32_t  downChecks    = 0;                  // v10 iDownTicks (psc:40); U1: the settle counts these too
            bool          awaitingWake  = false;              // COMING_TO: the wake's Papyrus stack has not finished
            std::chrono::steady_clock::time_point wakeDoneAt{};   // when it finished (re-knockout gate, see Detect)
            std::string   who;                                // "Name (0x........)" for the log

            // ---- U1 ----
            bool          unc            = false;   // this episode is a U1 knock-out (KnockOutU); false = M1 DOWN / v10 adoption
            std::uint64_t episode        = 0;       // the knock-out's own number: every step reply matches on it
            std::uint8_t  cls            = 0;       // 0 at-0-HP (the only class so far)
            std::int32_t  potionMask     = -1;      // Smart NPC Potions abilities taken (1|2|4); -1 = none taken
            bool          potionsInFlight = false;  // a PotionsOff call has not returned yet
            bool          severExcluded  = false;   // SBKO excluded her from SeverActions survival (Unguard at the wake)
            std::int32_t  severPacked    = 0;       // her needs snapshot (SBKO_SeverCompat.Guard's return)
            bool          severInFlight  = false;   // a Guard call has not returned yet
            float         lockFrac       = -1.0f;   // the HOLD (fraction of max = perm + temp); -1 = never settled yet. Fix 1:
                                                    // it only ever goes DOWN (follows damage), never up
            bool          holdInFlight   = false;   // a HoldDown call has not returned yet (no second one meanwhile)
            bool          healthInFlight = false;   // a HoldHealthTo call has not returned yet
            bool          shoveAfterLoad = false;   // loaded from the co-save: the first check shoves her once if not ragdolled
            float         windowSeconds  = 0.0f;    // co-save field (U2a's window; U2b: always written 0)
            std::string   lastEffect;               // the last magic effect seen on her (the put-back log line names it)

            // ---- U2a ----
            bool          healPending  = false;   // a HEAL landed; the wake starts when KnockOutU replied / nothing is in flight
            std::string   healTrigger;            // that heal, for the log
            WakeKind      wakeKind     = WakeKind::kTime;
            bool          leftoverWake = false;   // U2b: this COMING_TO wake finishes a u2a-save window leftover (NoBleedoutRecovery off)
            // effect bookkeeping (log hygiene): each mgef ONCE per knock-out, the rest counted
            std::unordered_map<RE::FormID, std::uint32_t> kinds;
            std::uint32_t effectsSinceCheck  = 0;
            std::uint32_t newKindsSinceCheck = 0;
            std::uint32_t healsSinceCheck    = 0;
            std::uint32_t healsTotal         = 0;
            bool          triggerLogged      = false;
            // external-clear context: checks since the knock-out / the last load / cell attach / 3D load
            std::int32_t  checksSince = 0;
            const char*   since       = "the knock-out";
            bool          had3D       = true;
            // ---- u2a fix round 1 ----
            bool          settledOnce = false;   // the knock-out's settle (iGraceChecks) is over - the external-clear rule's "settle"
            bool          resetActive = false;   // the 2-check engine-reset rule is armed (a load / cell attach / 3D load - never a KO)
            bool          releasing   = false;   // a LetGo is in flight (not dispatched -> retried at the next check)
            std::uint8_t  releaseKind = 0;       // its TakenKind
            bool          readopted   = false;   // this episode is a re-adoption (log only)
            // ---- u2c ----
            bool          orphan         = false;   // fix 3: this episode is an orphan adopt (log only)
            bool          rankDirty      = false;   // fix 5: the coming-to rank changed while she was not in memory - written when found
            std::uint32_t putBacks       = 0;       // fix 4: put-backs this knock-out (the first one is logged, the rest counted)
            std::uint32_t putBacksPerf   = 0;       // ... since the last PERF line
            std::uint32_t abilityApplies = 0;       // P2: constant-effect (ability) applications since the last check (not logged singly)
            // ---- U6 ----
            bool          hitKO  = false;   // this episode is a HIT knock-out (the player's bash / blunt head hit): the hold is
                                            // her Health at the knock-out, no fUnconsciousHealthFraction cap (co-save flags bit 0x20)
            float         hitPct = -1.0f;   // her Health fraction (current / max) when the hit knocked her out; -1 = not known (not
                                            // saved: a save inside the settle loads with her Health at the settle as the hold)
            // ---- u6c ----
            std::int32_t  sleepMark = 0;    // the Sleep spell put her down: 2 she saw the caster, 1 not, 0 another knock-out (the marker
                                            // itself is SBKO_SleepFaction's rank, kept by the save). u6f: CO-SAVED (STAT flags 0x40 / 0x80)
                                            // - it decides that a hit / the player's talk wakes her (a sleep, not an injury)
            // ---- u6f ----
            bool          sleepWakePending = false;   // a hit / talk / another mod's Wake came while KnockOutU had not replied or a hold call
                                                      // was in flight - the wake starts when that returns (the heal wake's M-1 rule)
            std::string   sleepWakeTrigger;           // what woke her (the log)
            // ---- u7a ----
            float         chokeSeconds = 0.0f;   // U7: a CHOKE knock-out (cls kClsChoke) - the seconds the choke was held: TakeOver's (at the
                                                 // passout), raised by ChokeReleased to the TOTAL when the player lets go (build 3's neck mark).
                                                 // Co-saved (record 'CHOK')
            float         chokeHours   = 0.0f;   // U7: the game hour that value was recorded (co-saved with it)
            bool          wokeByPlayer = false;  // M2: a Sleep-spell sleeper the player woke with Diary of Mine's Wake - her wake is narrated
                                                 // "woken by <player>" (not saved: a wake in flight across a save narrates the old way)
        };

        // u7a (U7): the entry's knock-out class (Entry::cls - the u8 STAT v4 has always saved; 0 = every older kind)
        constexpr std::uint8_t kClsChoke = 1;

        // M1d / U1 / U2a: a knocked-out NPC the load safety could not reach yet (not in memory at the load)
        struct Pending
        {
            float        healRate      = kHealRateUseBase;
            bool         unc           = false;   // U1 knock-out -> FinishWake (else M1 WakeUp)
            bool         window        = false;   // U2b: a u2a-save window leftover -> FinishWake + NoBleedoutRecovery off
            std::int32_t potionMask    = -1;
            bool         severExcluded = false;
            std::int32_t severPacked   = 0;
            bool         belt          = false;   // u2a fix 1: a SECOND run of a wake already dispatched to her while she had no
                                                  // 3D - only when she is loaded, still unconscious and not taken; dropped if awake
        };

        // u2a: who took her (TakenBy) - decides what LetGo gives back now and whether SBKO keeps watching her
        enum class TakenKind : std::uint8_t
        {
            kNone    = 0,
            kCaptive = 1,   // DoM capture / tie / a restrained life state: the flag stays (DoM owns her); Paralysis 0 + the rest back
            kBag     = 2,   // a disable: SBKO clears the flag it set too (+ the load-safety belt). u2b fix: DoM's bag is kCaptive now
            kVrte    = 3,   // VRTE's choke KO (rank 5): SBKO touches NOTHING of hers now; its own part comes back when VRTE wakes her
        };

        // u2a fix 1 (M-A): an NPC SBKO let go with her unconscious flag on (kCaptive / kVrte). Watched each check until she is
        // conscious (dropped), dead / deleted (dropped), or unconscious and no longer taken (RE-ADOPTED). Co-save record 'RELS'.
        struct Released
        {
            float        healRate      = kHealRateUseBase;   // her ORIGINAL HealRate (SBKO's saved value)
            std::int32_t potionMask    = -1;                 // kVrte: the abilities still off (deferred); kCaptive: -1 (given back)
            bool         severExcluded = false;              // kVrte: still excluded (deferred)
            std::int32_t severPacked   = 0;
            bool         deferred      = false;              // kVrte: HealRate / potions / SeverActions not given back yet
            bool         givingBack    = false;              // the deferred give-back is in flight
            bool         hitKO         = false;              // u6 fix 1 (F1): she was a HIT knock-out - a re-adoption keeps the
                                                             // no-cap hold (RELS flag 0x04; an older save / DLL = a bleedout knock-out)
            std::int32_t sleepMark     = 0;                  // u6f: she was a Sleep-spell sleeper (2 / 1) - a re-adoption stays one (a hit
                                                             // / talk still wakes her); RELS flags 0x08 / 0x10
            bool         choke         = false;              // u7a (U7): she was a CHOKE knock-out - a re-adoption stays one; RELS flag 0x20
            float        chokeSeconds  = 0.0f;               // u7a: its seconds held (record 'CHOK')
            float        chokeHours    = 0.0f;
        };

        // ---- forms: written once at kDataLoaded, read-only afterwards ---------------------------------------------
        RE::TESFaction* g_faction         = nullptr;
        RE::BGSKeyword* g_kwNPC           = nullptr;
        RE::TESFaction* g_brawlFaction    = nullptr;   // m1f
        RE::TESFaction* g_followerFaction = nullptr;   // U1 (U2a: log only)
        RE::TESFaction* g_vrteFaction     = nullptr;   // U1 (null = VRTouchEvents.esp not loaded)
        RE::SpellItem*  g_potion[3]       = {};        // U1 (null = Smart_NPC_Potions.esp not loaded)
        bool            g_severLoaded     = false;     // U1: SeverActions.esp loaded AND its survival quest found
        RE::TESFaction* g_domFaction      = nullptr;   // U2a (null = DiaryOfMine.esm not loaded)
        RE::TESFaction* g_domBagFaction   = nullptr;   // U2a: DOMActionInContainer
        RE::TESFaction* g_domTiedFaction  = nullptr;   // U2a: DOMActionTied
        RE::TESFaction* g_domSlaveFaction = nullptr;   // u2a fix 1: DOMActorFaction (released watch); u2b fix: also "taken" (TakenBy)
        RE::BGSKeyword* g_kwGhost         = nullptr;   // U6: ActorTypeGhost (null = not found - no ghost rule)
        RE::TESQuest*   g_questBrawl      = nullptr;   // U6: DGIntimidateQuest
        RE::TESQuest*   g_questC00        = nullptr;   // U6: C00
        RE::TESQuest*   g_questC00Fight   = nullptr;   // U6: C00JorrvaskrFight

        // u6 fix 1 (F5): PLANCK's mod-support interface - only to CONFIRM a PLANCK hit (GetCurrentHitEvent() == the event the sink
        // got) on top of the magic word. Vtable order = the 0.8.1 fork's include/planckinterface001.h (also PCVR's port
        // include/PlanckInterface.h): slot 12 = GetCurrentHitEvent, which exists only in the 0.8.x layout (PPB DismemberGuard:
        // 0.7.x has another order) -> used only for build >= 80000. Slots 1-11 are never called (placeholders keep the order).
        // GetCurrentHitEvent returns PLANCK's own global (set around its SendEvent) - a field read, safe from any thread.
        struct IPlanckInterface001
        {
            virtual std::uint32_t    GetBuildNumber()      = 0;   // 00
            virtual void             Deprecated1()         = 0;   // 01
            virtual void             Deprecated2()         = 0;   // 02
            virtual void             AddIgnoredActor()     = 0;   // 03
            virtual void             RemoveIgnoredActor()  = 0;   // 04
            virtual void             AddAggrIgnored()      = 0;   // 05
            virtual void             RemoveAggrIgnored()   = 0;   // 06
            virtual void             SetAggrLowTopic()     = 0;   // 07
            virtual void             SetAggrHighTopic()    = 0;   // 08
            virtual void             AddRagdollIgnored()   = 0;   // 09
            virtual void             RemoveRagdollIgnored() = 0;  // 10
            virtual void             GetLastHitData()      = 0;   // 11 (returns a struct by value - never called here)
            virtual RE::TESHitEvent* GetCurrentHitEvent()  = 0;   // 12 - valid only while a hit event is being dispatched
        };
        std::atomic<IPlanckInterface001*> g_planckApi{ nullptr };   // set once at kDataLoaded (before any game), read by HitSink
        // u6 fix round 3: is PLANCK (activeragdoll.dll - the fork's activeragdoll.vcxproj ProjectName; the mod
        // ships SKSE/Plugins/activeragdoll.dll) loaded in this process at all? Decided ONCE at kDataLoaded, before the hit sink is
        // registered. false (SE / AE always, VR without PLANCK) = the extended block is NEVER read - the magic word alone could be
        // stack padding (~1 in 16.7 M hits) and would send the sink after a garbage node pointer.
        std::atomic<bool>                 g_planckPresent{ false };

        // u6 fix round 2 (a follower's hit knock-out gets no FDC roll: she goes down with her HP still over 0, so there is
        // no chance of dying there): the FDC x SBKO patch (FDC_SBKO_Mark.psc) rolls once per SBKO
        // knock-out and skips an NPC whose FDC_SBKO_RolledFaction (FDC_SBKO_Patch.esp 0x800, an ESL) rank is >= 1. A HIT knock-out sets
        // that mark itself, BEFORE rank 10 (KnockOutU / ReAdopt aiNoFdcRoll 1). Found once at kDataLoaded (light-plugin aware); null =
        // the patch is not loaded -> aiNoFdcRoll is never sent, so the script never asks GetFormFromFile for a missing file.
        constexpr RE::FormID  kFdcRolledFactionLocal = 0x800;
        constexpr const char* kFdcPatchPlugin        = "FDC_SBKO_Patch.esp";
        RE::TESFaction*       g_fdcRolledFaction     = nullptr;

        // ---- u6c: the SLEEP SPELL (a spell works 100 % if it hits; seen / not seen -> a different wake
        // prompt). Records in SimpleBleedoutKO_001.esp (light): KYWD SBKO_KnockOutEffect 0x804, MGEF SBKO_SleepEffect 0x805 (Script
        // archetype, no script), FACT SBKO_SleepFaction 0x80A (the prompt marker). g_sleepMgefs = every effect carrying the keyword + 0x805,
        // SORTED, built ONCE at kDataLoaded BEFORE the effect sink is registered and never written again - the sink reads it with no lock.
        // An older ESP without the records = g_sleepOn false: the feature is off (one start line), no SetSleepMark is ever sent.
        constexpr RE::FormID   kSleepKeywordLocal = 0x804;
        constexpr RE::FormID   kSleepEffectLocal  = 0x805;
        constexpr RE::FormID   kSleepFactionLocal = 0x80A;
        constexpr RE::FormID   kSleepProjectileLocal = 0x809;   // u6f: the spell's touch projectile - its own hit never wakes the sleeper
        constexpr std::int32_t kSleepSeen         = 2;   // SBKO_SleepFaction rank: put to sleep, she detected the caster at the cast
        constexpr std::int32_t kSleepUnseen       = 1;   //                         put to sleep, she did not
        constexpr std::int32_t kSleepChoked       = 3;   // u7a (U7): CHOKED unconscious (VRTE's take-over) - the prompts' marker ONLY: the
                                                         //   entry's sleepMark stays 0 (a choke is not the Sleep spell's light class)
        RE::BGSKeyword*         g_sleepKeyword = nullptr;
        RE::EffectSetting*      g_sleepEffect  = nullptr;
        RE::TESFaction*         g_sleepFaction = nullptr;   // non-null = every knock-out writes the marker (0 for any other kind)
        std::vector<RE::FormID> g_sleepMgefs;
        std::atomic<bool>       g_sleepOn{ false };
        RE::BGSProjectile*      g_sleepProjectile = nullptr;   // u6f (null = not found: only the spell-source test applies)

        // ---- u7a U4: FOLLOWER DEATH CHANCE re-rolls on every hit ----------------------------------------------------------------------
        // g_fdcOn = bFdcRerollOnHit AND FDC_SBKO_Patch.esp loaded (g_fdcRolledFaction) AND the damage hook installed - decided ONCE at
        // kDataLoaded, before the hook is written; read by every hook call. g_fdcWatch = the NPCs SBKO holds UNCONSCIOUS after a BLEEDOUT
        // knock-out (not a hit / sleep / choke one) - rebuilt by SyncCount with g_watch, under g_watchMtx (the hook's allowed lock). Only a
        // follower can have FDC's effect; the task asks. g_fdcBeam (g_u5Mtx): EVERY concentration beam that already asked on her (one re-roll
        // per beam = per caster per cast; m34b: a SET, not one slot - two beams at once no longer re-roll every frame); pruned
        // by SyncCount when she leaves the watch, cleared at a load.
        std::atomic<bool>              g_fdcOn{ false };
        std::unordered_set<RE::FormID> g_fdcWatch;
        std::atomic<std::size_t>       g_fdcWatchCount{ 0 };
        constexpr RE::FormID           kFdcEffectLocal = 0x81E;                       // Inju3_MainEffect (FollowerDeathChance.esp) - log only
        constexpr const char*          kFdcPlugin      = "FollowerDeathChance.esp";
        RE::EffectSetting*             g_fdcEffect     = nullptr;
        struct FdcBeam
        {
            const void*   effect = nullptr;
            std::uint16_t uid    = 0;
            RE::FormID    spell  = 0;
        };
        std::unordered_map<RE::FormID, std::vector<FdcBeam>> g_fdcBeam;

        // ---- u7a U7: the take-over ---------------------------------------------------------------------------------------------------
        // g_takeOverPending (g_watchMtx): accepted by the TakeOver native, the task has not knocked her out yet -> Holds() says yes, a second
        // TakeOver says "already pending" (its seconds kept if larger). Erased by the task AFTER SyncCount put her in g_watch (no gap).
        std::unordered_map<RE::FormID, float> g_takeOverPending;
        // g_chokeLoose (g_mtx; co-saved in 'CHOK'): a choke the player let go of with NO take-over (before the passout, or SBKO declined) - the
        // seconds held + the game hour, for build 3's (lighter) neck mark. One per NPC (a new release replaces it); dropped at her death.
        struct ChokeLoose
        {
            float seconds = 0.0f;
            float hours   = 0.0f;
        };
        std::unordered_map<RE::FormID, ChokeLoose> g_chokeLoose;
        std::atomic<std::size_t>                   g_chokeLooseCount{ 0 };   // mirror of g_chokeLoose.size() (the death sink's early-out)

        // ---- u7a M2: "woken to talk" -------------------------------------------------------------------------------------------------
        // Diary of Mine's DOMInteractPerk (B14A55:DiaryOfMine.esm): its Activate entries' fragments
        // 0 Wake, 1 Bag, 2 Abduct, 3 Untie, 4 Tie-up (VMAD). g_domPerkId is written once at kDataLoaded, before the perk sink is registered.
        constexpr RE::FormID   kDomInteractPerkLocal = 0xB14A55;
        constexpr std::int32_t kDomFragWake          = 0;
        RE::BGSPerk*           g_domInteractPerk = nullptr;
        RE::FormID             g_domPerkId       = 0;
        std::atomic<bool>      g_wokenTalkOn{ false };   // the activate sink's M2 path
        std::atomic<bool>      g_wokenPerkOn{ false };   // the perk sink (DoM's Wake: a natural sleeper, or a Sleep-spell sleeper's narration)
        enum class WokenBy : std::uint8_t
        {
            kTalk    = 0,   // the player activated (talked to) her while she slept in a bed
            kDomWake = 1,   // the player picked Diary of Mine's menu Wake on her
        };
        struct Woken
        {
            WokenBy       by     = WokenBy::kTalk;
            std::int32_t  checks = 0;      // unpaused checks seen since the trigger (2 = the detection window: still asleep -> dropped)
            std::uint32_t state  = 0;      // her sit-sleep state at the trigger (7 / 8)
            std::string   who;
        };
        std::unordered_map<RE::FormID, Woken> g_woken;   // g_mtx
        constexpr std::int32_t kWokenNatural = 1;        // NarrateWoken kinds
        constexpr std::int32_t kWokenSpell   = 2;

        // ---- u6b (U5): the chance knock-out of a NORMAL NPC -------------------------------------------------------------------
        // State shared by the damage hook (any game thread) and the tasks. g_u5Mtx is held ONLY for a map operation - never around an
        // engine call, never while taking g_mtx (order g_mtx -> g_u5Mtx). No timer anywhere: "one roll per hit" is decided by WHICH
        // damage it is (a serial per roll), never by time.
        enum class RollState : std::uint8_t
        {
            kNone    = 0,
            kPending = 1,   // queued by the hook, the task has not rolled yet
            kWon     = 2,   // rolled, won, and SBKO's knock-out was dispatched
            kLost    = 3,   // rolled and lost - the game decides
            kSkipped = 4,   // ruled out in the task before the roll (taken, brawl, held ...), or won but not knocked out
        };
        // what started the bleedout she entered last (recorded by the hook right after that damage)
        enum class EntryKind : std::uint8_t
        {
            kNone    = 0,   // nothing recorded (a bleedout from before the load, or cleared once she was seen up)
            kNotAHit = 1,   // no rolled direct hit started it (a tick, no attacker, her Health above the line ...) -> ONE roll at the bleedout
            kNoRoll  = 2,   // a direct hit that never rolls (a punch) -> she acts hurt, no roll
            kRolled  = 3,   // the hit that started it rolled (or carried a roll: the same beam, a weapon enchantment) -> that roll decides
        };
        struct U5Victim
        {
            // the newest knock-out roll queued for her
            std::uint32_t rollSerial   = 0;
            RE::FormID    rollAttacker = 0;
            RollState     rollState    = RollState::kNone;
            float         rollChance   = -1.0f;
            float         rollValue    = -1.0f;
            // the concentration beams that already rolled on her (one roll per beam: the same effect instance). m34b: EVERY
            // beam, not only the last one - two beams at once (two casters, two hands) no longer roll on every frame. Cleared at a load.
            struct Beam
            {
                const void*   effect = nullptr;
                std::uint16_t uid    = 0;
                RE::FormID    spell  = 0;
                std::uint32_t roll   = 0;
                RollState     state  = RollState::kNone;
                float         chance = -1.0f;
                float         value  = -1.0f;
            };
            std::vector<Beam> beams;
            // the bleedout she entered last
            std::uint32_t bleedSerial   = 0;   // + 1 at every recorded bleedout; 0 = none recorded yet
            EntryKind     entry         = EntryKind::kNone;
            std::uint32_t entryRoll     = 0;
            RollState     entryState    = RollState::kNone;
            float         entryChance   = -1.0f;
            float         entryValue    = -1.0f;
            RE::FormID    entryAttacker = 0;
            float         entryDamage   = 0.0f;
            float         entryMax      = 0.0f;
            const char*   entryWhy      = "";   // always a string literal
            // Detect's decision for that bleedout (PickBleedout)
            bool          decided    = false;
            bool          decidedKO  = false;   // won her roll AT the bleedout: Detect knocks her out (after the grace, if any)
            std::uint32_t decidedFor = 0;       // the bleedSerial it was made for
        };
        std::mutex                               g_u5Mtx;
        std::unordered_map<RE::FormID, U5Victim> g_u5;
        std::atomic<std::uint32_t>               g_u5RollSerial{ 0 };
        std::atomic<bool>                        g_u5On{ false };   // the damage hooks are in (bChanceKO 1) - read by every hook call
        // cloak-associated and hazard spells (built once at kDataLoaded, read-only afterwards): their damage is a tick, never a hit
        std::unordered_set<RE::FormID>           g_tickSpells;
        // Devious Devices' item keywords (found by EditorID at kDataLoaded; null = DD absent): such worn items are not "armour"
        RE::BGSKeyword*                          g_kwDDLockable  = nullptr;
        RE::BGSKeyword*                          g_kwDDInventory = nullptr;
        // MEASUREMENT ONLY (the PERF line)
        std::atomic<std::uint64_t> g_perfDmgCalls{ 0 }, g_perfDmgLow{ 0 }, g_perfDmgLowNs{ 0 }, g_perfDmgRolls{ 0 }, g_perfDmgEntries{ 0 };

        // ---- u6b: thread-locals of the damage hook (each game thread its own; no lock) ----
        // the innermost value-modifier effect applying a value on this thread (vfunc 0x20 scope) and the actor it applies it to
        thread_local const RE::ValueModifierEffect* t_effect       = nullptr;
        thread_local const RE::Actor*               t_effectTarget = nullptr;
        // the last TESHitEvent this thread sent on an actor: the engine sends a physical hit's event BEFORE its damage, on the same thread
        struct HitTls
        {
            RE::FormID    target     = 0;
            RE::FormID    cause      = 0;
            RE::FormID    source     = 0;
            RE::FormID    projectile = 0;
            std::uint32_t flags      = 0;
        };
        thread_local HitTls t_hit{};
        // u6d (F2): THIS thread's last physical damage with an attacker (a weapon swing, an arrow, a bash ...) - a weapon enchantment /
        // poison applied right after it on the same thread belongs to it. Overwritten by the next physical damage; cleared by the next hit
        // event this thread sends. roll 0 = that damage did not roll (above the line / ruled out); noRollWhy non-null = it never rolls (a
        // punch, a non-hostile weapon) - the enchantment carries that too.
        struct SwingTls
        {
            RE::FormID    victim    = 0;
            RE::FormID    attacker  = 0;
            std::uint32_t roll      = 0;
            const char*   noRollWhy = nullptr;   // a string literal
            HitTls        hit{};                 // the hit event it matched (target 0 = none)
        };
        thread_local SwingTls t_swing{};
        std::atomic<std::uint64_t> g_perfDmgPhysical{ 0 };   // u6d (F3): rolls queued for a weapon hit with NO hit event (kPhysical)

        // ---- the map and everything it owns: guarded by g_mtx (tasks + co-save callbacks only) ---------------------
        std::mutex                            g_mtx;
        std::unordered_map<RE::FormID, Entry> g_map;
        std::unordered_set<RE::FormID>        g_adoptCandidates;   // actors attached since the last check (v10 leftovers)
        // u6h (G4): the candidates whose cell attached DURING a load or before the first check after it (in the loaded area at the load) -
        // an orphan among them is re-held as before; every other candidate ARRIVED from an unloaded cell during play and is woken
        std::unordered_set<RE::FormID>        g_adoptAtLoad;
        // m1b (m-3): NPCs whose KnockOut replied -1.0 or was refused by the VM. Detection skips them while they stay
        // in bleedout; a check that sees one NOT in bleedout (or her detach) removes her. No timer.
        // M1d: an NPC the load safety just made conscious joins it too, so neither the scan nor the adoption can knock her
        // out again before WakeUp has lifted her out of bleedout.
        std::unordered_set<RE::FormID>        g_declined;
        // M1d: knocked out at a load but NOT in memory then -> made conscious the first time she is seen loaded.
        // Saved in the co-save (record v3; U1 v4 adds the unconscious fields).
        std::unordered_map<RE::FormID, Pending> g_pendingConscious;
        std::unordered_map<RE::FormID, Released> g_released;   // u2a fix 1 (guarded by g_mtx; co-save record 'RELS')
        bool                                  g_loadSafety = false;  // M1d: kPostLoadGame -> next unpaused check runs the load safety
        bool                                  g_adoptAll = false;  // kPostLoadGame: next unpaused check looks at all loaded actors
        std::uint64_t                         g_serial   = 0;
        std::mt19937                          g_rng{ std::random_device{}() };
        bool                                  g_hasLastTick = false;   // the coming-to clock: real seconds between
        std::chrono::steady_clock::time_point g_lastTick{};            // two UNPAUSED checks

        // ---- lock-free flags -----------------------------------------------------------------------------------------
        std::atomic<std::size_t>   g_handled{ 0 };       // mirror of g_map.size(): the death sink returns at once on 0
        std::atomic<std::uint32_t> g_generation{ 0 };    // bumped at kPreLoadGame + revert: queued old-session work is dropped
        std::atomic<bool>          g_gameReady{ false }; // a game is loaded (kNewGame / kPostLoadGame .. kPreLoadGame)
        std::atomic<bool>          g_tickQueued{ false };// at most ONE check task waits in SKSE's queue
        std::atomic<bool>          g_started{ false };
        std::atomic<bool>          g_firstBleedoutEvent{ false };
        std::atomic<std::uint32_t> g_bleedoutEvents{ 0 };

        // ---- U1: the HELD set (every state but COMING_TO) for the magic-effect sink ----------------------------------
        // Rebuilt by SyncCount() under g_mtx; the sink only tests membership under g_watchMtx (lock order g_mtx -> g_watchMtx).
        std::mutex                     g_watchMtx;
        std::unordered_set<RE::FormID> g_watch;
        std::atomic<std::size_t>       g_watchCount{ 0 };
        // u6f: the Sleep-spell SLEEPERS SBKO holds (sleepMark >= 1, ENTERING / UNCONSCIOUS) - the hit / activate sinks' early-out. Rebuilt
        // by SyncCount with g_watch (same lock, g_watchMtx); count 0 = the sinks return after one atomic load.
        std::unordered_set<RE::FormID> g_sleepWatch;
        std::atomic<std::size_t>       g_sleepWatchCount{ 0 };

        // ---- U1 / U2a: the per-actor animation-graph watch of the held NPCs whose graph carries our sink ------------------
        // FormID -> footsteps since the last check (U2b: the U2a get-up relay is gone). g_footMtx is held only for the map operation
        // (the sink runs while the graph's event source holds its own lock - never call the engine while holding g_footMtx).
        struct GraphWatch
        {
            std::uint32_t steps  = 0;         // FootLeft / FootRight since the last check
            const void*   graph  = nullptr;   // u2c P3: the graph manager + first graph our sink was added to (re-attach only
            const void*   graph0 = nullptr;   //         when these change - a rebuilt graph - instead of every check)
        };
        std::mutex                                 g_footMtx;
        std::unordered_map<RE::FormID, GraphWatch> g_foot;
        std::atomic<std::size_t>                   g_footCount{ 0 };

        // ---- u2c P1: MEASUREMENT ONLY (never a limit, never a cap) - one PERF line per minute of unpaused play -------------
        // Sink counters are relaxed atomics (any thread); the check-side ones are touched by the check task only.
        std::atomic<std::uint64_t> g_perfEffectEvents{ 0 }, g_perfEffectRelayed{ 0 }, g_perfEffectNs{ 0 };
        std::atomic<std::uint64_t> g_perfEffectAbilities{ 0 };   // fix round 1: ability events of held NPCs dropped in the sink
        std::atomic<std::uint64_t> g_perfGraphEvents{ 0 }, g_perfGraphSteps{ 0 }, g_perfGraphNs{ 0 };
        std::atomic<std::uint64_t> g_perfBleedEvents{ 0 }, g_perfBleedNs{ 0 };
        std::atomic<std::uint64_t> g_perfCellEvents{ 0 }, g_perfCellActors{ 0 }, g_perfCellNs{ 0 };
        std::atomic<std::uint64_t> g_perfPutBacks{ 0 };
        std::atomic<std::uint64_t> g_perfHitEvents{ 0 }, g_perfHitRelayed{ 0 }, g_perfHitNs{ 0 };   // U6: the hit sink
        constexpr std::size_t      kPerfKinds = 32;   // > the number of Call kinds
        std::atomic<std::uint32_t> g_perfCalls[kPerfKinds]{};
        std::uint64_t              g_perfChecks = 0, g_perfCheckNs = 0, g_perfCheckMaxNs = 0, g_perfScanned = 0;
        float                      g_perfSeconds = 0.0f;   // unpaused play since the last PERF line
        std::uint64_t              g_perfLogBase = 0;      // log-line counter at the last PERF line

        inline std::uint64_t NsSince(std::chrono::steady_clock::time_point a_t0)
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a_t0).count());
        }

        // u2c fix 6: actors in bleedout SBKO did NOT knock out, already explained in the log for this bleedout (g_mtx). A new
        // TESEnterBleedoutEvent for her clears her id first, so the reason is logged once per NPC per bleedout.
        std::unordered_set<RE::FormID> g_silentLogged;

        // u2d (a 1.5 s delay): NPCs in the engine's bleedout waiting out fBleedoutGraceSeconds before SBKO knocks them out -
        // FormID -> the moment SBKO first saw her bleeding out (g_mtx). Every entry is looked at again at its due time (ProcessGrace).
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_grace;
        // the check thread's alarm for the earliest due grace (g_threadMtx; lock order g_mtx -> g_threadMtx, the thread never takes g_mtx)
        std::mutex                            g_threadMtx;
        std::condition_variable               g_threadCv;
        std::chrono::steady_clock::time_point g_graceDue = (std::chrono::steady_clock::time_point::max)();
        std::atomic<bool>                     g_graceQueued{ false };   // at most ONE grace task waits in SKSE's queue
        std::atomic<std::uint64_t>            g_perfGraceStarted{ 0 }, g_perfGraceKnockedOut{ 0 };

        // wake the check thread at a_due (or earlier, if an earlier grace is already armed)
        void ArmGraceWake(std::chrono::steady_clock::time_point a_due)
        {
            {
                std::scoped_lock lk(g_threadMtx);
                if (a_due < g_graceDue) g_graceDue = a_due;
            }
            g_threadCv.notify_one();
        }

        // ---- the cell inbox: attach/detach of ACTORS only, drained by one relayed task ------------------------------
        struct CellEvent
        {
            RE::FormID    id;
            bool          attached;
            std::uint32_t gen;
            bool          atLoad = false;   // u6h (G4): sent while no game was ready (between kPreLoadGame and kPostLoadGame)
        };
        std::mutex             g_inboxMtx;   // held ONLY for a push or a swap - never while calling anything
        std::vector<CellEvent> g_inbox;
        std::atomic<bool>      g_drainQueued{ false };

        // ---- small readers ------------------------------------------------------------------------------------------
        std::string NameOf(RE::Actor* a_actor)
        {
            if (!a_actor) return "(none)";
            const char* n = a_actor->GetDisplayFullName();
            if (!n || !*n) n = a_actor->GetName();
            return (n && *n) ? std::string(n) : std::string("(no name)");
        }

        std::string Who(RE::Actor* a_actor, RE::FormID a_id) { return std::format("{} (0x{:08X})", NameOf(a_actor), a_id); }

        // v10's Pct (psc:219-221): (afFraction * 100.0) as Int -> truncated, then "%"
        std::string Pct(float a_fraction) { return std::format("{}%", static_cast<int>(a_fraction * 100.0f)); }

        // Health % in C++: current / permanent (parity with Papyrus GetActorValuePercentage is logged).
        float HealthFraction(RE::Actor* a_actor, float* a_cur = nullptr, float* a_perm = nullptr)
        {
            auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
            if (!avo) return 0.0f;
            const float cur  = avo->GetActorValue(RE::ActorValue::kHealth);
            const float perm = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
            if (a_cur) *a_cur = cur;
            if (a_perm) *a_perm = perm;
            return perm > 0.0f ? cur / perm : 0.0f;
        }

        // u1 fix 1 (M-2): Health % against the ENGINE's maximum = permanent + temporary modifier, i.e. what Papyrus'
        // GetActorValuePercentage divides by (a Fortify Health raises it). Actor::GetActorValueMax (toolbox Actor.cpp:337-341) =
        // GetPermanentActorValue (ActorValueOwner vfunc, already used by M1) + Actor::GetActorValueModifier(kTemporary)
        // (Actor.cpp:343-348, RELOCATION_ID 37524 / 38469 -> VR 0x62A100 in version-1-4-15-0.csv). Used by every U1 decision;
        // M1's legacy DOWN rules keep HealthFraction (current / permanent) unchanged.
        float HealthFractionMax(RE::Actor* a_actor, float* a_cur = nullptr, float* a_max = nullptr)
        {
            auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
            if (!avo) return 0.0f;
            const float cur = avo->GetActorValue(RE::ActorValue::kHealth);
            const float mx  = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
            if (a_cur) *a_cur = cur;
            if (a_max) *a_max = mx;
            return mx > 0.0f ? cur / mx : 0.0f;
        }

        // one decimal, for the hold's own lines (a whole-number Pct would print 24% for a 24.99999% float)
        std::string Pct1(float a_fraction) { return std::format("{:.1f}%", a_fraction * 100.0f); }

        // u2d (wake up at 25 % HP): the ABSOLUTE Health a wake raises her to at least - fWakeHealthFraction x her max, the max read
        // as permanent + temporary (GetActorValueMax, the hold's own basis). -1 = not read (the script then uses her base Health).
        float WakeHealthTarget(RE::FormID a_id)
        {
            float mx = 0.0f;
            if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id)) HealthFractionMax(a, nullptr, &mx);
            return mx > 0.0f ? Settings::Get().wakeHealthFraction * mx : -1.0f;
        }

        // M1e: is her parent cell ATTACHED (part of the loaded area around the player)? TESObjectCELL::IsAttached() =
        // cellState == kAttached; cellState (0x44) and TESObjectREFR::parentCell (0x60) sit before the runtime-versioned
        // blocks, so the same inline read is right on SE / AE / VR (no address-library ID involved).
        bool InAttachedCell(RE::Actor* a_actor)
        {
            const auto* cell = a_actor ? a_actor->GetParentCell() : nullptr;
            return cell && cell->IsAttached();
        }

        bool IsBleedingOut(RE::Actor* a_actor)
        {
            const auto* st = a_actor ? a_actor->AsActorState() : nullptr;
            return st && st->IsBleedingOut();
        }

        // v10's filter = SPID's ActorTypeNPC line (SimpleBleedoutKO_DISTR.ini:12): the base OR the race carries it.
        bool IsNPC(RE::Actor* a_actor)
        {
            if (!a_actor || !g_kwNPC) return false;
            const auto* npc  = a_actor->GetActorBase();
            const auto* race = a_actor->GetRace();
            return (npc && npc->HasKeyword(g_kwNPC)) || (race && race->HasKeyword(g_kwNPC));
        }

        // U1: a player follower = IsPlayerTeammate (Actor.cpp:1110-1113, boolBits) OR CurrentFollowerFaction rank >= 0
        // (GetFactionRank, RELOCATION_ID 36668 / 37676, VR 0x600190). U2a: for the log only (SeverActions decides its guard).
        bool IsFollower(RE::Actor* a_actor)
        {
            if (!a_actor) return false;
            if (a_actor->IsPlayerTeammate()) return true;
            return g_followerFaction && a_actor->GetFactionRank(g_followerFaction, false) >= 0;
        }

        // U1: VRTE holds her in its own KO (rank 5) - SBKO never knocks out or re-asserts on her.
        bool VrteHolds(RE::Actor* a_actor)
        {
            return a_actor && g_vrteFaction && a_actor->GetFactionRank(g_vrteFaction, false) == kVrteKoRank;
        }

        // U1: protected / essential on the reference (Actor.cpp:977-980, :1122-1125 - boolFlags) or on the base
        // (TESActorBaseData.h:102, :106).
        // u2d (5): essential / protected, the reference's runtime flags and the base record's, apart (the engine bonus's test is
        // unknown - this shows whether a Protected-only NPC gets it)
        std::string EssFlags(RE::Actor* a_actor)
        {
            const auto* base = a_actor ? a_actor->GetActorBase() : nullptr;
            auto yn = [](bool b) { return b ? "YES" : "no"; };
            return std::format("essential ref {} / base {}, protected ref {} / base {}", yn(a_actor && a_actor->IsEssential()),
                               yn(base && base->IsEssential()), yn(a_actor && a_actor->IsProtected()), yn(base && base->IsProtected()));
        }

        bool IsProtectedOrEssential(RE::Actor* a_actor)
        {
            if (!a_actor) return false;
            if (a_actor->IsEssential() || a_actor->IsProtected()) return true;
            const auto* base = a_actor->GetActorBase();
            return base && (base->IsEssential() || base->IsProtected());
        }

        // 2.1.0 [WhoGetsKnockedOut] (SBKO_Setting.ini): nullptr = her group may be knocked out at bleedout; else why not (her group is
        // switched off - she keeps the game's own bleedout). A follower is judged as a follower first (whatever her flags), then a
        // protected / essential NPC, then an ordinary one. Reads only her flags / factions: safe under g_mtx.
        const char* WhoSwitchedOff(RE::Actor* a_actor, bool a_follower)
        {
            const auto& cfg = Settings::Get();
            if (a_follower)
                return cfg.koFollowers ? nullptr : "bKnockOutFollowers 0 - a follower keeps the game's own bleedout";
            if (IsProtectedOrEssential(a_actor))
                return cfg.koProtected ? nullptr : "bKnockOutProtected 0 - a protected / essential NPC keeps the game's own bleedout";
            return cfg.koNormal ? nullptr : "bKnockOutNormalNPCs 0 - an ordinary NPC keeps the game's own bleedout";
        }

        // U1: which of Smart NPC Potions' three abilities she has now (mask 1|2|4). Actor::HasSpell = RELOCATION_ID 37828 /
        // 38782 (Actor.cpp:864-869), VR 0x63BBE0 (checked in version-1-4-15-0.csv).
        std::int32_t PotionSpellsOn(RE::Actor* a_actor)
        {
            if (!a_actor) return 0;
            std::int32_t mask = 0;
            for (int i = 0; i < 3; ++i)
                if (g_potion[i] && a_actor->HasSpell(g_potion[i])) mask |= (1 << i);
            return mask;
        }

        float HoursNow()
        {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? cal->GetHoursPassed() : 0.0f;
        }

        // v10 RollRecoverHours (psc:202-207). Caller holds g_mtx (g_rng).
        float RollHours()
        {
            const auto& cfg = Settings::Get();
            if (cfg.recoverMaxHours <= cfg.recoverMinHours) return cfg.recoverMinHours;
            return std::uniform_real_distribution<float>(cfg.recoverMinHours, cfg.recoverMaxHours)(g_rng);
        }

        // caller holds g_mtx. U1: also rebuilds the HELD set the magic-effect sink tests (everything but COMING_TO).
        void SyncCount()
        {
            g_handled.store(g_map.size());
            std::scoped_lock lk(g_watchMtx);
            g_watch.clear();
            g_sleepWatch.clear();
            g_fdcWatch.clear();
            for (const auto& [id, e] : g_map) {
                if (e.state != State::kComingTo) g_watch.insert(id);
                if (e.sleepMark > 0 && (e.state == State::kEntering || e.state == State::kUnconscious)) g_sleepWatch.insert(id);   // u6f
                // u7a (U4): a BLEEDOUT knock-out SBKO holds unconscious (FDC rolled once at it) - a hit / sleep / choke one never re-rolls
                if (e.state == State::kUnconscious && e.unc && !e.hitKO && e.sleepMark == 0 && e.cls == 0 && !e.releasing) g_fdcWatch.insert(id);
            }
            g_watchCount.store(g_watch.size());
            g_sleepWatchCount.store(g_sleepWatch.size());
            g_fdcWatchCount.store(g_fdcWatch.size());
        }

        // m34b: a victim off the U4 watch forgets her counted beams. Caller holds g_mtx (order g_mtx -> g_u5Mtx; g_watchMtx is
        // not held here). The same test as SyncCount's watch, read from g_map.
        void PruneFdcBeams()
        {
            std::scoped_lock lk(g_u5Mtx);
            for (auto it = g_fdcBeam.begin(); it != g_fdcBeam.end();) {
                const auto m = g_map.find(it->first);
                const bool watched = m != g_map.end() && m->second.state == State::kUnconscious && m->second.unc && !m->second.hitKO &&
                                     m->second.sleepMark == 0 && m->second.cls == 0 && !m->second.releasing;
                it = watched ? std::next(it) : g_fdcBeam.erase(it);
            }
        }

        bool Watched(RE::FormID a_id)
        {
            std::scoped_lock lk(g_watchMtx);
            return g_watch.contains(a_id);
        }

        // u6f: is she one of SBKO's Sleep-spell sleepers? (the sinks' filter - the task decides again under g_mtx)
        bool Asleep(RE::FormID a_id)
        {
            std::scoped_lock lk(g_watchMtx);
            return g_sleepWatch.contains(a_id);
        }

        // u7a (U4): is she on the FDC re-roll watch? (the damage hook's filter - any game thread; the task decides again under g_mtx)
        bool FdcWatched(RE::FormID a_id)
        {
            std::scoped_lock lk(g_watchMtx);
            return g_fdcWatch.contains(a_id);
        }

        // u7a (U7): accepted by TakeOver, not knocked out yet (any thread)
        bool TakeOverPending(RE::FormID a_id)
        {
            std::scoped_lock lk(g_watchMtx);
            return g_takeOverPending.contains(a_id);
        }

        // U1 footsteps: take (and zero) her count. Caller may hold g_mtx (order g_mtx -> g_footMtx).
        std::uint32_t TakeFootsteps(RE::FormID a_id)
        {
            std::scoped_lock lk(g_footMtx);
            auto             it = g_foot.find(a_id);
            if (it == g_foot.end()) return 0;
            const std::uint32_t n = it->second.steps;
            it->second.steps      = 0;
            return n;
        }

        // a new knock-out: nothing from before it (footsteps while she was awake, an old arm) is hers now
        void ResetGraphWatch(RE::FormID a_id)
        {
            std::scoped_lock lk(g_footMtx);
            if (auto it = g_foot.find(a_id); it != g_foot.end()) it->second = GraphWatch{};
        }

        // the packed SeverActions needs snapshot (SBKO_SeverCompat.Guard): 2^30 + H*2^20 + F*2^10 + C, each in tenths
        std::string SeverNeeds(std::int32_t a_packed)
        {
            if (a_packed < (1 << 30)) return "(no snapshot)";
            const std::int32_t r = a_packed - (1 << 30);
            const float        h = static_cast<float>(r >> 20) / 10.0f;
            const float        f = static_cast<float>((r >> 10) & 0x3FF) / 10.0f;
            const float        c = static_cast<float>(r & 0x3FF) / 10.0f;
            return std::format("hunger {:.1f} fatigue {:.1f} cold {:.1f}", h, f, c);
        }

        // ---- U1: the refined heal rule (DDAB's IsHealingEffect WornDevices.cpp:5853-5864 with ONE change:
        // Recover disqualifies only a PeakValueModifier - that is a Fortify Health. SeverActions' HealOther effect 16023F is
        // ValueModifier + Recover + NoDeathDispel and must count). U2a: the first one from an Actor is the wake trigger.
        bool IsHeal(const RE::EffectSetting* a_m)
        {
            if (!a_m) return false;
            using Arch       = RE::EffectArchetypes::ArchetypeID;
            using F          = RE::EffectSetting::EffectSettingData::Flag;
            const auto arch  = a_m->GetArchetype();
            const bool value = arch == Arch::kValueModifier;
            const bool peak  = arch == Arch::kPeakValueModifier;
            if (!value && !peak) return false;
            if (a_m->data.primaryAV != RE::ActorValue::kHealth) return false;
            if (a_m->data.flags.any(F::kDetrimental, F::kHostile)) return false;
            if (a_m->data.castingType == RE::MagicSystem::CastingType::kConstantEffect) return false;
            if (peak && a_m->data.flags.any(F::kRecover)) return false;
            return true;
        }

        std::string EffectFlags(const RE::EffectSetting* a_m)
        {
            using F = RE::EffectSetting::EffectSettingData::Flag;
            static constexpr std::pair<F, const char*> kNames[] = {
                { F::kHostile, "Hostile" },         { F::kRecover, "Recover" },     { F::kDetrimental, "Detrimental" },
                { F::kNoHitEvent, "NoHitEvent" },   { F::kNoDuration, "NoDuration" }, { F::kNoMagnitude, "NoMagnitude" },
                { F::kFXPersist, "FXPersist" },     { F::kHideInUI, "HideInUI" },   { F::kNoRecast, "NoRecast" },
                { F::kPainless, "Painless" },       { F::kNoHitEffect, "NoHitEffect" }, { F::kNoDeathDispel, "NoDeathDispel" },
            };
            std::string out;
            for (const auto& [f, n] : kNames)
                if (a_m->data.flags.all(f)) out += (out.empty() ? "" : "|") + std::string(n);
            return out.empty() ? "none" : out;
        }

        const char* CastingName(RE::MagicSystem::CastingType a_c)
        {
            switch (a_c) {
                case RE::MagicSystem::CastingType::kConstantEffect: return "Constant";
                case RE::MagicSystem::CastingType::kFireAndForget:  return "FireAndForget";
                case RE::MagicSystem::CastingType::kConcentration:  return "Concentration";
                case RE::MagicSystem::CastingType::kScroll:         return "Scroll";
            }
            return "?";
        }

        std::string AvName(RE::ActorValue a_av)
        {
            switch (a_av) {
                case RE::ActorValue::kHealth:   return "Health";
                case RE::ActorValue::kMagicka:  return "Magicka";
                case RE::ActorValue::kStamina:  return "Stamina";
                case RE::ActorValue::kHealRate: return "HealRate";
                case RE::ActorValue::kNone:     return "none";
                default:                        return std::format("av{}", static_cast<int>(a_av));
            }
        }

        // The newest active effect of base `mgef` on her: duration / magnitude / the item it came from. MagicTarget::VisitEffects
        // = RELOCATION_ID 33756 / 34540 (MagicTarget.cpp:149-154), VR 0x558580 (version-1-4-15-0.csv); ActiveEffect's
        // fields have one layout on every runtime (ActiveEffect.h:104-120, static_assert 0x90; DDAB reads spell/effect/
        // usUniqueID through the same struct in VR).
        class FindEffect final : public RE::MagicTarget::ForEachActiveEffectVisitor
        {
        public:
            explicit FindEffect(RE::FormID a_mgef) : mgef(a_mgef) {}
            RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_ae) override
            {
                if (a_ae) {
                    const auto* b = a_ae->GetBaseObject();
                    if (b && b->GetFormID() == mgef && (!found || a_ae->elapsedSeconds < elapsed)) {
                        found   = true;
                        elapsed = a_ae->elapsedSeconds;
                        dur     = a_ae->duration;
                        mag     = a_ae->magnitude;
                        item    = a_ae->spell;
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;   // DDAB's rule: always continue (the list is short)
            }
            RE::FormID      mgef;
            bool            found   = false;
            float           elapsed = 0.0f, dur = 0.0f, mag = 0.0f;
            RE::MagicItem*  item    = nullptr;
        };

        // ---- the Papyrus calls a locked scope decided on; Run() dispatches them with NO lock held -------------------
        struct Call
        {
            enum class Kind
            {
                kKnockOut,
                kWake,
                kRank,
                kRestore,      // WakeUp(id, healRate, 0, 0): m1b m-2 v10 leftover (healRate -2.0) and the M1d load safety; no HUD line
                // ---- U1 ----
                kKnockOutU,    // KnockOutU(id, notify, fromBleedout 1 - U6: 0 for a hit knock-out) -> OnKnockOutUReply
                kPotionsOff,   // PotionsOff(id) -> OnPotionsOffReply
                kPotionsBack,  // PotionsBack(id, mask) - a late reply / death
                kSeverGuard,   // SBKO_SeverCompat.Guard(id) -> OnGuardReply
                kSeverUnguard, // SBKO_SeverCompat.Unguard(id, packed)
                kHoldHealth,   // HoldHealthTo(id, absolute Health) -> OnHoldHealthReply
                kHoldDown,     // HoldDown(id) -> OnHoldDownDone
                kFinishWake,   // the TIME wake: FinishWake(id, saved, 3, mask, 0) -> OnWakeDone (+ NarrateWake)
                kRestoreU,     // the load safety's FinishWake(id, saved, 0, mask, 0); no reply, no HUD
                kAttachFoot,   // Actor::AddAnimationGraphEventSink (engine call: no lock of ours held)
                kDetachFoot,   // Actor::RemoveAnimationGraphEventSink
                // ---- U2a ----
                kLetGo,        // LetGo(id, saved, mask) - Diary of Mine took her
                kStraightWake, // the heal wake / finishing a u2a leftover: FinishWake(id, saved, 3, mask, 0, flags&1) -> OnWakeDone
                kTooInjured,   // U2b: SayTooInjured(id, notify) - "<name> is too injured to wake up." after an external clear
                kReAdopt,      // u2a fix 1: ReAdopt(id, shove, carried HealRate) -> OnKnockOutUReply (same reply as KnockOutU)
                // ---- u2c ----
                kClearMark,    // ClearMark(id, rank): NoBleedoutRecovery off (+ HealRate / stealth on a living NPC) + rank - a death,
                               // a stale rank-10 mark
                // ---- u6c ----
                kSleepMark,    // SetSleepMark(id, rank, notify): SBKO_SleepFaction - 2 / 1 the Sleep spell (seen / not seen), 0 any other
                               // knock-out and the coming-to's end (the SkyrimNet wake prompt's marker); u7a: 3 = choked (VRTE's take-over)
                // ---- u7a ----
                kWokenNarrate, // M2: NarrateWoken(id, kind = rank, mode = narrate, notify, SkyrimNet present) - the player woke her
                kCount         // (the number of kinds - the PERF line's table size)
            };
            Kind          kind;
            RE::FormID    id;
            std::uint64_t serial   = 0;
            float         healRate = kHealRateUseBase;
            std::int32_t  rank     = 0;
            float         cppPct   = kNoRead;   // C++ health fraction when decided (kNoRead = not read; u2c: may be negative)
            std::string   who;
            std::string   reason;
            bool          timer    = false;   // M1c: a timer wake (WakeUp without its HUD line, then NarrateWake)
            std::int32_t  hoursOut = 0;       // M1c: game hours she was out (timer wake only)
            // ---- U1 ----
            std::int32_t  mask     = 0;       // potions mask (kPotionsBack / kFinishWake / kRestoreU / U2a wakes)
            std::int32_t  packed   = 0;       // SeverActions snapshot (kSeverUnguard)
            float         target   = 0.0f;    // fix 1: the ABSOLUTE Health to hold her at (kHoldHealth);
                                              // kLetGo: 1 = clear NoBleedoutRecovery
            bool          clearUnc = false;   // U2a kLetGo: clear the unconscious flag too (a bag / a disable - never a captive)
            // ---- U2a ----
            std::int32_t  narrate  = -1;      // OnWakeDone: > 0 NarrateWake(hours), -2 NarrateHealWake, -1 nothing
            std::int32_t  flags    = 0;       // kLetGo's flag bits (1 / 2 / 4 / 8, see SBKO_Helper.LetGo); kStraightWake / kRestoreU:
                                              // 1 = NoBleedoutRecovery off too (a u2a-save leftover)
            // ---- U6 ----
            bool          fromBleedout = true;   // kKnockOutU: false = a HIT knock-out of a standing NPC (KnockOutU aiFromBleedout 0:
                                                 // Paralysis, shove, then SetUnconscious - VRTE's order)
            // ---- u6b ----
            bool          hitKO = false;         // kKnockOutU: a HIT knock-out (U6 / U5) - pre-marked "no FDC roll". Apart from fromBleedout
                                                 // since u6b: a U5 roll can knock out an NPC who is already kneeling in bleedout
            // ---- u6c ----
            bool          quiet = false;         // kKnockOutU: no HUD line (the Sleep spell - its kSleepMark shows "falls into a deep sleep")
            // ---- u7a ----
            float         choke = 0.0f;          // kReAdopt: a re-adopted CHOKE knock-out's seconds held (flag bit 64 = a choke) - carried back to
                                                 // the released watch if the ReAdopt is not dispatched
        };

        constexpr std::int32_t kNarrateNone  = -1;
        constexpr std::int32_t kNarrateHeal  = -2;
        static_assert(static_cast<std::size_t>(Call::Kind::kCount) <= kPerfKinds);

        // u2c P1: the PERF line's names
        const char* KindName(Call::Kind a_k)
        {
            switch (a_k) {
                case Call::Kind::kKnockOut:     return "KnockOut";
                case Call::Kind::kWake:         return "WakeUp";
                case Call::Kind::kRank:         return "SetRank";
                case Call::Kind::kRestore:      return "Restore";
                case Call::Kind::kKnockOutU:    return "KnockOutU";
                case Call::Kind::kPotionsOff:   return "PotionsOff";
                case Call::Kind::kPotionsBack:  return "PotionsBack";
                case Call::Kind::kSeverGuard:   return "SeverGuard";
                case Call::Kind::kSeverUnguard: return "SeverUnguard";
                case Call::Kind::kHoldHealth:   return "HoldHealthTo";
                case Call::Kind::kHoldDown:     return "HoldDown";
                case Call::Kind::kFinishWake:   return "FinishWake(time)";
                case Call::Kind::kRestoreU:     return "FinishWake(load safety)";
                case Call::Kind::kAttachFoot:   return "AttachGraphSink(engine, real attaches)";
                case Call::Kind::kDetachFoot:   return "DetachGraphSink(engine)";
                case Call::Kind::kLetGo:        return "LetGo";
                case Call::Kind::kStraightWake: return "FinishWake(heal)";
                case Call::Kind::kTooInjured:   return "SayTooInjured";
                case Call::Kind::kReAdopt:      return "ReAdopt";
                case Call::Kind::kClearMark:    return "ClearMark";
                case Call::Kind::kSleepMark:    return "SetSleepMark";
                case Call::Kind::kWokenNarrate: return "NarrateWoken";
                case Call::Kind::kCount:        break;
            }
            return "?";
        }

        void OnKnockOutReply(RE::FormID a_id, std::uint64_t a_serial, bool a_ok, float a_value);
        void OnWakeDone(RE::FormID a_id, std::uint64_t a_serial, std::int32_t a_narrate);
        void OnKnockOutUReply(RE::FormID a_id, std::uint64_t a_serial, bool a_ok, float a_value);
        void OnPotionsOffReply(RE::FormID a_id, std::uint64_t a_episode, bool a_ok, std::int32_t a_mask);
        void OnGuardReply(RE::FormID a_id, std::uint64_t a_episode, bool a_ok, std::int32_t a_packed);
        void OnHoldHealthReply(RE::FormID a_id, std::uint64_t a_episode, bool a_ok, float a_hp);
        void OnHoldDownDone(RE::FormID a_id, std::uint64_t a_episode);
        void OnLetGoDone(RE::FormID a_id, std::uint64_t a_serial);

        // One HealthPct round trip: Papyrus' GetActorValuePercentage vs the C++ reading, logged on one line.
        void Parity(const char* a_when, RE::FormID a_id, const std::string& a_who, float a_cppAtDispatch)
        {
            const bool sent = Helper::HealthPct(a_id, [a_when, a_id, who = a_who, a_cppAtDispatch](bool a_ok, float a_pct) {
                // VM thread: a read-only look at the actor (what any Papyrus native does), then one log line
                float       cur = 0.0f, perm = 0.0f, now = -1.0f, mx = 0.0f, nowMax = -1.0f;
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id)) {
                    now    = HealthFraction(a, &cur, &perm);
                    nowMax = HealthFractionMax(a, nullptr, &mx);   // fix 1 (M-2): the basis U1 decides on
                }
                const std::string atDispatch = a_cppAtDispatch > kNoRead ? std::format("{:.2f}%", a_cppAtDispatch * 100.0f) : "not read";
                if (!a_ok)
                    logger::warn("[parity] {} {}: HealthPct returned no Float | C++ current/permanent = {:.2f}% ({:.1f} / {:.1f}) | "
                                 "current/max = {:.2f}% (max {:.1f})",
                                 a_when, who, now * 100.0f, cur, perm, nowMax * 100.0f, mx);
                else
                    logger::info("[parity] {} {}: Papyrus GetActorValuePercentage = {:.2f}% | C++ current/max(perm+temp) = {:.2f}% "
                                 "(max {:.1f}) | current/permanent = {:.2f}% ({:.1f} / {:.1f}) at the reply, {} C++ when asked",
                                 a_when, who, a_pct * 100.0f, nowMax * 100.0f, mx, now * 100.0f, cur, perm, atDispatch);
            });
            if (!sent) logger::warn("[parity] {} {}: HealthPct not dispatched", a_when, a_who);
        }

        // ---- U1: the footstep sink (one static object for the whole process - never a dead vtable; PPB's rule,
        // PushStep.cpp:1155-1186). It runs on whatever thread raised the animation event, holding that event source's lock:
        // compare the tag, bump one counter under g_footMtx, return. Always kContinue (kStop starves the sinks behind us).
        // (U2b: the U2a GetUpEnd / GetUpExit relay is gone - nothing needs it without the window.)
        class FootSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
                                                  RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
            {
                // only the graphs of held NPCs carry this sink; g_footCount 0 = nothing to count (fast path)
                if (!a_event || !a_event->holder || g_footCount.load(std::memory_order_relaxed) == 0) return RE::BSEventNotifyControl::kContinue;
                g_perfGraphEvents.fetch_add(1, std::memory_order_relaxed);   // u2c P1/P4
                const char* tag = a_event->tag.c_str();
                if (!tag) return RE::BSEventNotifyControl::kContinue;
                if (_stricmp(tag, "FootLeft") != 0 && _stricmp(tag, "FootRight") != 0) return RE::BSEventNotifyControl::kContinue;
                const auto       t0 = std::chrono::steady_clock::now();
                const RE::FormID id = a_event->holder->GetFormID();
                {
                    std::scoped_lock lk(g_footMtx);
                    if (auto it = g_foot.find(id); it != g_foot.end()) ++it->second.steps;
                }
                g_perfGraphSteps.fetch_add(1, std::memory_order_relaxed);
                g_perfGraphNs.fetch_add(NsSince(t0), std::memory_order_relaxed);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        FootSink g_footSink;

        // Actor::AddAnimationGraphEventSink (Actor.cpp:51-71) / RemoveAnimationGraphEventSink (Actor.cpp:1211-...): byte-identical
        // to CommonLibVR 4.14.0's, which PPB runs in VR (PushStep.cpp:1261; GetUpStart..GetUpExit seen in its log). Both take
        // graphs.front() when no sink matched - so an empty graph list is refused here first.
        bool HasGraph(RE::Actor* a_actor, const void** a_mgr = nullptr, const void** a_graph0 = nullptr)
        {
            if (!a_actor || !a_actor->Get3D()) return false;
            RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
            a_actor->GetAnimationGraphManager(mgr);
            if (!mgr || mgr->graphs.empty()) return false;
            if (a_mgr) *a_mgr = mgr.get();
            if (a_graph0) *a_graph0 = mgr->graphs.front().get();
            return true;
        }

        void Run(std::vector<Call>& a_calls)
        {
            const bool  notify   = Settings::Get().notify;
            const float wakeFrac = Settings::Get().wakeHealthFraction;   // u2d
            std::unordered_set<RE::FormID> koFailed;   // U1: a KnockOutU that was not dispatched -> skip its PotionsOff / Guard
            for (auto& c : a_calls) {
                // u2c P1: counted by kind (fix round 1: an attach is counted only when it really reaches the engine - below)
                if (const auto k = static_cast<std::size_t>(c.kind); k < kPerfKinds && c.kind != Call::Kind::kAttachFoot)
                    g_perfCalls[k].fetch_add(1, std::memory_order_relaxed);
                switch (c.kind) {
                    case Call::Kind::kKnockOut: {
                        const RE::FormID    id     = c.id;
                        const std::uint64_t serial = c.serial;
                        const bool sent = Helper::KnockOut(id, notify, [id, serial](bool a_ok, float a_value) {
                            TaskRelay::Add([id, serial, a_ok, a_value] { OnKnockOutReply(id, serial, a_ok, a_value); });
                        });
                        if (!sent) {
                            {
                                std::scoped_lock lk(g_mtx);
                                if (auto it = g_map.find(id); it != g_map.end() && it->second.serial == serial) {
                                    g_map.erase(it);
                                    SyncCount();
                                }
                                g_declined.insert(id);   // m-3: not retried while she stays in bleedout
                            }
                            logger::warn("[KO] {} KnockOut was NOT dispatched - SBKO lets go of her (not retried while she stays "
                                         "in bleedout); the game's own bleedout carries on", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kWake: {
                        const RE::FormID    id     = c.id;
                        const std::uint64_t serial = c.serial;
                        // M1c: a timer wake shows no "has recovered." line - NarrateWake speaks for it once WakeUp is done
                        const std::int32_t narrateHours = c.timer ? c.hoursOut : kNarrateNone;
                        const bool sent = Helper::WakeUp(id, c.healRate, kRankComingTo, c.timer ? false : notify, WakeHealthTarget(id),
                                                         wakeFrac, [id, serial, narrateHours] {
                                                             TaskRelay::Add([id, serial, narrateHours] { OnWakeDone(id, serial, narrateHours); });
                                                         });
                        if (!sent) {
                            // m1b (m-4): nothing woke her - back to DOWN (same saved HealRate), the checks go on
                            {
                                std::scoped_lock lk(g_mtx);
                                if (auto it = g_map.find(id); it != g_map.end() && it->second.serial == serial &&
                                                              it->second.state == State::kComingTo) {
                                    Entry& e       = it->second;
                                    e.state        = State::kDown;
                                    e.awaitingWake = false;
                                    e.comingToRank = 0;
                                    e.stageSeconds = 0.0f;
                                    e.serial       = ++g_serial;
                                    SyncCount();
                                }
                            }
                            logger::warn("[KO] {} WakeUp was NOT dispatched (wanted: {}) - she is DOWN again, the checks go on",
                                         c.who, c.reason);
                            break;
                        }
                        logger::info("[KO] {} RECOVERED -- {} (WakeUp: HealRate {}, rank {})", c.who, c.reason,
                                     c.healRate >= 0.0f ? std::format("back to {:.4f}", c.healRate)
                                                        : std::string("back to her BASE value (-2.0)"),
                                     kRankComingTo);
                        break;   // u2c fix 6: the parity line runs in OnWakeDone, after the wake (it raced it here)
                    }
                    case Call::Kind::kRank:
                        Helper::SetRank(c.id, c.rank);
                        break;
                    case Call::Kind::kSleepMark:   // u6c: the marker; u6d fix 2: NEVER the HUD line here - it is
                                                   // sent by OnKnockOutUReply once KnockOutU really knocked her out (not a declined one)
                        if (!koFailed.contains(c.id) || c.rank <= 0) {
                            if (!Helper::SetSleepMark(c.id, c.rank, false))
                                logger::warn("[KO] {} SetSleepMark (rank {}) was NOT dispatched - the Sleep spell's wake-prompt marker is not "
                                             "written (an older SBKO_Helper.pex?)", c.who, c.rank);
                        }
                        break;
                    case Call::Kind::kRestore:
                        if (!Helper::WakeUp(c.id, c.healRate, 0, false, WakeHealthTarget(c.id), wakeFrac, {}))
                            logger::warn("[KO] {} the conscious restore (WakeUp {:.4f}, rank 0) was NOT dispatched", c.who, c.healRate);
                        break;

                    // ---- U1 ----------------------------------------------------------------------------------------------
                    case Call::Kind::kKnockOutU: {
                        const RE::FormID    id     = c.id;
                        const std::uint64_t serial = c.serial;
                        // U6: a hit knock-out sends aiFromBleedout 0 (the psc's VRTE order for a standing NPC); u6b: a U5 hit knock-out of
                        // an NPC already in bleedout sends 1 (SetUnconscious first, else she snaps upright)
                        // u6 fix 2: a hit knock-out is pre-marked "already had its FDC roll" (only with FDC_SBKO_Patch.esp loaded);
                        // u6b: decided by c.hitKO (was !fromBleedout)
                        const bool noFdcRoll = c.hitKO && g_fdcRolledFaction;
                        const bool sent = Helper::KnockOutU(id, notify && !c.quiet, c.fromBleedout, noFdcRoll, [id, serial](bool a_ok, float a_value) {
                            TaskRelay::Add([id, serial, a_ok, a_value] { OnKnockOutUReply(id, serial, a_ok, a_value); });
                        });
                        if (!sent) {
                            koFailed.insert(id);
                            {
                                std::scoped_lock lk(g_mtx);
                                if (auto it = g_map.find(id); it != g_map.end() && it->second.serial == serial) {
                                    g_map.erase(it);
                                    SyncCount();
                                }
                                g_declined.insert(id);   // (a hit knock-out: dropped from it at the next check - she is not bleeding out)
                            }
                            if (!c.hitKO)
                                logger::warn("[KO] {} KnockOutU was NOT dispatched - SBKO lets go of her (not retried while she "
                                             "stays in bleedout); the game's own bleedout carries on", c.who);
                            else
                                logger::warn("[KO] {} KnockOutU (hit knock-out) was NOT dispatched - nothing was done to her; the "
                                             "next qualifying hit after the next check tries again", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kPotionsOff: {
                        if (koFailed.contains(c.id)) break;
                        const RE::FormID    id = c.id;
                        const std::uint64_t ep = c.serial;
                        const bool sent = Helper::PotionsOff(id, [id, ep](bool a_ok, std::int32_t a_mask) {
                            TaskRelay::Add([id, ep, a_ok, a_mask] { OnPotionsOffReply(id, ep, a_ok, a_mask); });
                        });
                        if (!sent) {
                            std::scoped_lock lk(g_mtx);
                            if (auto it = g_map.find(id); it != g_map.end() && it->second.episode == ep) it->second.potionsInFlight = false;
                            logger::warn("[KO] {} PotionsOff was NOT dispatched - her potion abilities stay on", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kPotionsBack:
                        if (c.mask > 0 && !Helper::PotionsBack(c.id, c.mask))
                            logger::warn("[KO] {} PotionsBack (mask {}) was NOT dispatched", c.who, c.mask);
                        break;
                    case Call::Kind::kSeverGuard: {
                        if (koFailed.contains(c.id)) break;
                        const RE::FormID    id = c.id;
                        const std::uint64_t ep = c.serial;
                        const bool sent = Helper::SeverGuard(id, [id, ep](bool a_ok, std::int32_t a_packed) {
                            TaskRelay::Add([id, ep, a_ok, a_packed] { OnGuardReply(id, ep, a_ok, a_packed); });
                        });
                        if (!sent) {
                            std::scoped_lock lk(g_mtx);
                            if (auto it = g_map.find(id); it != g_map.end() && it->second.episode == ep) it->second.severInFlight = false;
                            logger::warn("[KO] {} SeverActions guard was NOT dispatched - she stays in SeverActions' survival", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kSeverUnguard: {
                        // ⛔ only into SBKO_SeverCompat while SeverActions.esp is loaded (a co-save from a game that had it)
                        if (!g_severLoaded) {
                            logger::warn("[KO] {} was excluded from SeverActions survival by SBKO, but SeverActions.esp is not "
                                         "loaded now - nothing to give back", c.who);
                            break;
                        }
                        const std::string who  = c.who;
                        const std::int32_t pk  = c.packed;
                        const bool sent = Helper::SeverUnguard(c.id, pk, [who, pk](bool a_ok, std::int32_t a_r) {
                            // VM thread: one log line only
                            if (!a_ok)
                                logger::warn("[KO] {} SeverActions include: no answer from Unguard", who);
                            else if (a_r == 1)
                                logger::info("[KO] {} SeverActions: included in survival again, needs restored ({})", who, SeverNeeds(pk));
                            else if (a_r == 2)
                                logger::info("[KO] {} SeverActions: she was already included again (someone else did it) - her "
                                             "needs were left as they are", who);
                            else
                                logger::warn("[KO] {} SeverActions include: Unguard returned {} (no quest / no actor)", who, a_r);
                        });
                        if (!sent) {
                            {
                                std::scoped_lock lk(g_mtx);
                                if (auto it = g_map.find(c.id); it != g_map.end()) {   // keep it for a later try (the next wake / death)
                                    it->second.severExcluded = true;
                                    it->second.severPacked   = pk;
                                }
                            }
                            logger::warn("[KO] {} SeverActions include was NOT dispatched - she stays excluded (SeverActions' "
                                         "follower page can include her)", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kHoldHealth: {
                        const RE::FormID    id = c.id;
                        const std::uint64_t ep = c.serial;
                        const bool sent = Helper::HoldHealthTo(id, c.target, [id, ep](bool a_ok, float a_hp) {
                            TaskRelay::Add([id, ep, a_ok, a_hp] { OnHoldHealthReply(id, ep, a_ok, a_hp); });
                        });
                        if (!sent) {
                            std::scoped_lock lk(g_mtx);
                            if (auto it = g_map.find(id); it != g_map.end() && it->second.episode == ep) it->second.healthInFlight = false;
                            logger::warn("[KO] {} HoldHealthTo was NOT dispatched", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kHoldDown: {
                        const RE::FormID    id = c.id;
                        const std::uint64_t ep = c.serial;
                        const bool sent = Helper::HoldDown(id, [id, ep] { TaskRelay::Add([id, ep] { OnHoldDownDone(id, ep); }); });
                        if (!sent) {
                            std::scoped_lock lk(g_mtx);
                            if (auto it = g_map.find(id); it != g_map.end() && it->second.episode == ep) it->second.holdInFlight = false;
                            logger::warn("[KO] {} HoldDown was NOT dispatched", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kFinishWake:
                    case Call::Kind::kStraightWake: {
                        const bool          straight     = c.kind == Call::Kind::kStraightWake;
                        const RE::FormID    id           = c.id;
                        const std::uint64_t serial       = c.serial;
                        const std::int32_t  narrateCode  = straight ? c.narrate : (c.timer ? c.hoursOut : kNarrateNone);
                        const float wakeHp = WakeHealthTarget(id);   // u2d
                        const bool sent = Helper::FinishWake(id, c.healRate, kRankComingTo, c.mask, false, wakeHp, wakeFrac,
                                                             [id, serial, narrateCode] {
                                                                 TaskRelay::Add([id, serial, narrateCode] { OnWakeDone(id, serial, narrateCode); });
                                                             });
                        if (!sent) {
                            // nothing woke her - back to UNCONSCIOUS (same saved values), the checks go on (U2b: a u2a window
                            // leftover goes back to WINDOW - the next check tries the finish again)
                            {
                                std::scoped_lock lk(g_mtx);
                                if (auto it = g_map.find(id); it != g_map.end() && it->second.serial == serial &&
                                                              it->second.state == State::kComingTo) {
                                    Entry& e       = it->second;
                                    e.state        = e.leftoverWake ? State::kWindow : State::kUnconscious;
                                    e.awaitingWake = false;
                                    e.comingToRank = 0;
                                    e.stageSeconds = 0.0f;
                                    e.serial       = ++g_serial;
                                    SyncCount();
                                }
                            }
                            logger::warn("[KO] {} FinishWake was NOT dispatched (wanted: {}) - she is UNCONSCIOUS again, the checks "
                                         "go on", c.who, c.reason);
                            break;
                        }
                        logger::info("[KO] {} RECOVERED -- {} (FinishWake: Health to at least {} ({}% of max, never lowered), "
                                     "unconscious off, Paralysis 0, HealRate {}, potions back: mask {}, rank {})",
                                     c.who, c.reason, wakeHp > 0.0f ? std::format("{:.1f}", wakeHp) : std::string("her base x fraction"),
                                     std::lround(wakeFrac * 100.0f),
                                     c.healRate >= 0.0f ? std::format("back to {:.4f}", c.healRate)
                                                        : std::string("back to her BASE value (-2.0)"),
                                     c.mask, kRankComingTo);
                        break;   // u2c fix 6: the parity line runs in OnWakeDone, after FinishWake (it raced it here)
                    }
                    case Call::Kind::kRestoreU:
                        if (!Helper::FinishWake(c.id, c.healRate, 0, c.mask, false, WakeHealthTarget(c.id), wakeFrac, {}))
                            logger::warn("[KO] {} the conscious restore (FinishWake {:.4f}, potions mask {}, rank 0) was NOT dispatched",
                                         c.who, c.healRate, c.mask);
                        break;
                    case Call::Kind::kAttachFoot: {
                        auto*       a      = RE::TESForm::LookupByID<RE::Actor>(c.id);
                        const void* mgr    = nullptr;
                        const void* graph0 = nullptr;
                        if (!HasGraph(a, &mgr, &graph0)) break;
                        bool same = false;
                        {
                            std::scoped_lock lk(g_footMtx);
                            auto& w = g_foot[c.id];
                            same    = w.graph == mgr && w.graph0 == graph0;   // u2c P3: the same graph as last time - nothing to do
                            w.graph  = mgr;
                            w.graph0 = graph0;
                            g_footCount.store(g_foot.size());
                        }
                        if (same) break;
                        g_perfCalls[static_cast<std::size_t>(Call::Kind::kAttachFoot)].fetch_add(1, std::memory_order_relaxed);
                        if (a->AddAnimationGraphEventSink(&g_footSink))   // true only when it really added (de-duplicates)
                            logger::info("[KO] {} graph listener ATTACHED to her animation graph (footsteps counted per check)", c.who);
                        break;
                    }
                    case Call::Kind::kDetachFoot: {
                        {
                            std::scoped_lock lk(g_footMtx);
                            g_foot.erase(c.id);
                            g_footCount.store(g_foot.size());
                        }
                        auto* a = RE::TESForm::LookupByID<RE::Actor>(c.id);
                        if (HasGraph(a)) a->RemoveAnimationGraphEventSink(&g_footSink);
                        break;
                    }

                    // ---- U2a ---------------------------------------------------------------------------------------------
                    case Call::Kind::kTooInjured:
                        if (!Helper::SayTooInjured(c.id, notify))
                            logger::warn("[KO] {} SayTooInjured was NOT dispatched", c.who);
                        break;
                    case Call::Kind::kLetGo: {
                        const RE::FormID    id     = c.id;
                        const std::uint64_t serial = c.serial;
                        // u2e: + the wake Health floor (the engine's full heal at the flag-off is skipped now)
                        const bool sent = Helper::LetGo(id, c.healRate, c.mask, c.flags, WakeHealthTarget(id), wakeFrac, [id, serial] {
                            TaskRelay::Add([id, serial] { OnLetGoDone(id, serial); });
                        });
                        if (!sent) {
                            // u2a fix 1 (m-3): never forget her with Paralysis 1 / HealRate 0 - keep her, retry next check
                            std::scoped_lock lk(g_mtx);
                            if (serial == 0) {
                                if (auto it = g_released.find(id); it != g_released.end()) it->second.givingBack = false;
                            } else if (auto it = g_map.find(id); it != g_map.end() && it->second.serial == serial) {
                                it->second.releasing = false;
                            }
                            logger::warn("[KO] {} LetGo was NOT dispatched - SBKO keeps her and tries again at the next check", c.who);
                        }
                        break;
                    }
                    case Call::Kind::kReAdopt: {
                        const RE::FormID    id     = c.id;
                        const std::uint64_t serial = c.serial;
                        // u6 fix 2: a re-adopted HIT knock-out (flag bit 8) is pre-marked again - the mark was cleared at the let-go
                        const bool noFdcRoll = (c.flags & 8) != 0 && g_fdcRolledFaction;
                        const bool sent = Helper::ReAdopt(id, (c.flags & 1) != 0, c.healRate, noFdcRoll, [id, serial](bool a_ok, float a_value) {
                            TaskRelay::Add([id, serial, a_ok, a_value] { OnKnockOutUReply(id, serial, a_ok, a_value); });
                        });
                        if (!sent) {
                            bool rewatched = false;
                            {
                                std::scoped_lock lk(g_mtx);
                                if (auto it = g_map.find(id); it != g_map.end() && it->second.serial == serial) {
                                    g_map.erase(it);
                                    SyncCount();
                                    // u2a fix 2: back on the released watch with the same record - retried at the next check
                                    Released r;
                                    r.healRate      = c.healRate;
                                    r.potionMask    = c.mask;
                                    r.severPacked   = c.packed;
                                    r.deferred      = (c.flags & 2) != 0;
                                    r.severExcluded = (c.flags & 4) != 0;
                                    r.hitKO         = (c.flags & 8) != 0;   // u6 fix 1 (F1)
                                    r.sleepMark     = (c.flags & 16) ? ((c.flags & 32) ? kSleepSeen : kSleepUnseen) : 0;   // u6f
                                    r.choke         = (c.flags & 64) != 0;   // u7a (U7)
                                    r.chokeSeconds  = c.choke;
                                    r.chokeHours    = c.cppPct;   // (kReAdopt carries the hour in cppPct - see BeginReAdopt)
                                    g_released[id]  = r;
                                    rewatched       = true;
                                }
                            }
                            logger::warn("[KO] {} ReAdopt was NOT dispatched - {}", c.who,
                                         rewatched ? "back on the released watch, retried at the next check"
                                                   : "a newer state already owns her - nothing to put back");
                        }
                        break;
                    }
                    case Call::Kind::kClearMark:
                        if (!Helper::ClearMark(c.id, c.rank))
                            logger::warn("[KO] {} ClearMark (NoBleedoutRecovery off, rank {}) was NOT dispatched", c.who, c.rank);
                        break;
                    case Call::Kind::kWokenNarrate: {   // u7a (M2)
                        const bool skyrimNet = Helper::SkyrimNetPresent();
                        const bool sent      = Helper::NarrateWoken(c.id, c.rank, c.narrate, notify, skyrimNet);
                        if (Settings::Get().logWoken || !sent)
                            logger::info("[woken] {} NarrateWoken(kind {}, mode {} = {}, notify {}, SkyrimNet {}) {}", c.who, c.rank, c.narrate,
                                         c.narrate == 1 ? "she answers you" : "she only knows it", notify ? 1 : 0, skyrimNet ? 1 : 0,
                                         sent ? "dispatched" : "NOT dispatched (an older SBKO_Helper.pex?)");
                        break;
                    }
                    case Call::Kind::kCount:
                        break;
                }
            }
            a_calls.clear();
        }

        // ---- transitions (caller holds g_mtx; they only queue Calls) --------------------------------------------------

        // v10 EnterDown (psc:99-125), the C++ half: record, roll the timer, hand the engine writes to KnockOut.
        // U1: no longer used for a new knock-out (BeginKnockOutU is); kept for reading the M1 code paths together.
        [[maybe_unused]] void BeginKnockOut(Entry& e, RE::FormID a_id, const char* a_via, const char* a_note, std::vector<Call>& a_calls)
        {
            e.state         = State::kEntering;
            e.unc           = false;
            e.serial        = ++g_serial;
            e.recoverHours  = RollHours();
            e.dispatchHours = HoursNow();
            e.koHours       = e.dispatchHours;   // refined to the reply's moment in OnKnockOutReply
            e.downChecks    = 0;
            e.comingToRank  = 0;
            e.stageSeconds  = 0.0f;
            e.awaitingWake  = false;
            logger::info("[KO] {} is in bleedout (seen by {}){} - KnockOut dispatched; no wake until it replies", e.who, a_via, a_note);
            a_calls.push_back(Call{ .kind = Call::Kind::kKnockOut, .id = a_id, .serial = e.serial, .who = e.who });
        }

        // U1 T1: the guarded unconscious. `a_potions` = the Smart NPC Potions mask seen on her (C++ HasSpell, before the lock);
        // `a_guardSever` = SeverActions.esp is loaded, bSeverActionsGuard is on and SBKO_SeverCompat is installed (U2a: for EVERY
        // NPC - SBKO_SeverCompat.Guard decides by SeverActions' own lists whether she is tracked). KnockOutU, PotionsOff and Guard
        // go out together; each reply finds her by the episode.
        // the fields every new episode (a knock-out, a re-adoption) starts from
        void ResetEpisode(Entry& e, RE::FormID a_id)
        {
            e.state           = State::kEntering;
            e.unc             = true;
            e.serial          = ++g_serial;
            e.episode         = e.serial;
            e.cls             = 0;
            e.recoverHours    = RollHours();
            e.dispatchHours   = HoursNow();
            e.koHours         = e.dispatchHours;   // refined to the reply's moment in OnKnockOutUReply
            e.downChecks      = 0;
            e.comingToRank    = 0;
            e.stageSeconds    = 0.0f;
            e.awaitingWake    = false;
            e.potionMask      = -1;
            e.potionsInFlight = false;
            e.severExcluded   = false;
            e.severPacked     = 0;
            e.severInFlight   = false;
            e.lockFrac        = -1.0f;
            e.holdInFlight    = false;
            e.healthInFlight  = false;
            e.shoveAfterLoad  = false;
            e.windowSeconds   = 0.0f;
            e.lastEffect.clear();
            // U2a
            e.healPending        = false;
            e.healTrigger.clear();
            e.sleepWakePending   = false;   // u6f
            e.sleepWakeTrigger.clear();
            e.wakeKind           = WakeKind::kTime;
            e.leftoverWake       = false;
            e.kinds.clear();
            e.effectsSinceCheck  = 0;
            e.newKindsSinceCheck = 0;
            e.healsSinceCheck    = 0;
            e.healsTotal         = 0;
            e.triggerLogged      = false;
            e.checksSince        = 0;
            e.since              = "the knock-out";
            e.had3D              = true;
            e.settledOnce        = false;
            e.resetActive        = false;   // the engine-reset rule applies after a load / attach / 3D load - never a knock-out
            e.releasing          = false;
            e.releaseKind        = 0;
            e.readopted          = false;
            e.orphan             = false;
            e.rankDirty          = false;
            e.putBacks           = 0;
            e.abilityApplies     = 0;
            e.hitKO              = false;   // U6: a bleedout knock-out / re-adoption unless the hit path says otherwise
            e.hitPct             = -1.0f;
            e.sleepMark          = 0;       // u6c: set by OnSleepTask after StartKnockOutU
            e.chokeSeconds       = 0.0f;    // u7a: set by OnTakeOverTask after StartKnockOutU (cls is reset above)
            e.chokeHours         = 0.0f;
            e.wokeByPlayer       = false;   // u7a (M2)
            ResetGraphWatch(a_id);   // nothing from before this knock-out (footsteps while she was awake) is hers now
        }

        // The knock-out's calls, shared by the bleedout path (BeginKnockOutU) and U6's hit path (OnHitTask): a new episode, then
        // KnockOutU + PotionsOff + Guard together. Returns the log tail (essential / protected, potions, guard). Caller holds g_mtx.
        // u6b: a_hitKO (U6 / U5) is apart from a_fromBleedout (the Papyrus order) - the CALLER sets e.hitKO / e.hitPct after this.
        // u6c: a_sleepMark = SBKO_SleepFaction's rank for THIS knock-out (2 / 1 the Sleep spell, 0 every other kind) - written at EVERY
        // knock-out when the ESP has the faction, so a mark from an earlier spell episode never reaches a later knock-out's prompts.
        std::string StartKnockOutU(Entry& e, RE::FormID a_id, std::int32_t a_potions, bool a_follower, const std::string& a_ess,
                                   bool a_guardSever, bool a_fromBleedout, bool a_hitKO, std::vector<Call>& a_calls,
                                   std::int32_t a_sleepMark = 0)
        {
            const auto& cfg = Settings::Get();
            ResetEpisode(e, a_id);
            g_released.erase(a_id);   // a fresh knock-out supersedes a release watch
            std::string extra = "; " + a_ess;   // u2d (5): essential / protected, reference and base apart
            a_calls.push_back(Call{ .kind = Call::Kind::kKnockOutU, .id = a_id, .serial = e.serial, .who = e.who,
                                    .fromBleedout = a_fromBleedout, .hitKO = a_hitKO,
                                    // u6d fix 2: quiet ONLY when the marker can carry the HUD line (faction 0x80A found);
                                    // u7a: a CHOKE is always quiet (VRTE's passout line is the one the player sees)
                                    .quiet = (a_sleepMark > 0 && g_sleepFaction != nullptr) || a_sleepMark == kSleepChoked });
            if (g_sleepFaction)   // u6c: right after KnockOutU (a different faction from its rank 10 - the two stacks never race on one value)
                a_calls.push_back(Call{ .kind = Call::Kind::kSleepMark, .id = a_id, .rank = a_sleepMark, .who = e.who });
            if (cfg.potionsOff && a_potions > 0) {
                e.potionsInFlight = true;
                a_calls.push_back(Call{ .kind = Call::Kind::kPotionsOff, .id = a_id, .serial = e.episode, .who = e.who });
                extra += std::format("; Smart NPC Potions abilities seen (mask {}) -> PotionsOff", a_potions);
            }
            if (a_guardSever) {
                e.severInFlight = true;
                a_calls.push_back(Call{ .kind = Call::Kind::kSeverGuard, .id = a_id, .serial = e.episode, .who = e.who });
                extra += std::format("; SeverActions guard asked (SeverActions decides if it tracks her; follower by teammate / "
                                     "CurrentFollowerFaction: {})", a_follower ? "yes" : "no");
            } else {
                extra += std::format("; SeverActions guard {}", !cfg.severActionsGuard ? "OFF (bSeverActionsGuard 0)"
                                                                 : !g_severLoaded     ? "not needed (SeverActions.esp not loaded)"
                                                                                      : "NOT possible (SBKO_SeverCompat.pex missing)");
            }
            return extra;
        }

        void BeginKnockOutU(Entry& e, RE::FormID a_id, const char* a_via, const char* a_note, std::int32_t a_potions,
                            bool a_follower, const std::string& a_ess, bool a_guardSever, std::vector<Call>& a_calls)
        {
            const std::string extra = StartKnockOutU(e, a_id, a_potions, a_follower, a_ess, a_guardSever, true, false, a_calls);
            logger::info("[KO] {} is in bleedout (seen by {}){} - KnockOutU dispatched (unconscious + Paralysis + shove){}; no wake "
                         "until it replies",
                         e.who, a_via, a_note, extra);
        }

        // 2.1.0 looting (bLootKnockedOut, while sneaking): the NPC whose inventory SBKO asked to open (pending) and the one whose
        // inventory menu SBKO opened and is still open (open); 0 = none. Written by the loot task (an SKSE task) / the menu sink (UI
        // thread), read anywhere - atomics only. Both cleared at kPreLoadGame and at a revert.
        std::atomic<RE::FormID> g_lootPending{ 0 };
        std::atomic<RE::FormID> g_lootOpen{ 0 };

        // Task context (TaskRelay -> SKSE task; in VR an engine job thread inside SKSE's task lock, not the main thread). If the open
        // container menu is SBKO's loot menu of HER, it is hidden; another container now = left alone. No lock of ours.
        void HideOurLootMenu(RE::FormID a_id, const char* a_why)
        {
            if (g_lootOpen.load() != a_id) return;
            auto* ui = RE::UI::GetSingleton();
            if (!ui || !ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME)) return;
            const auto ref = RE::TESObjectREFR::LookupByHandle(RE::ContainerMenu::GetTargetRefHandle());
            if (!ref || ref->GetFormID() != a_id) return;   // the container menu shows someone else now - not ours
            if (auto* q = RE::UIMessageQueue::GetSingleton()) q->AddMessage(RE::ContainerMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
            logger::info("[loot] 0x{:08X} {} - closing her inventory menu", a_id, a_why);
        }

        // 2.1.0: she wakes / is let go / dies while SBKO's loot menu of HER is open -> that menu closes. A menu still on its way (asked
        // for, not open yet) is caught when it opens: the menu sink checks she is still knocked out. Callable with g_mtx held: an atomic
        // read and a TaskRelay push (TaskRelay's own lock is a leaf). a_why = a string literal.
        void CloseOurLoot(RE::FormID a_id, const char* a_why)
        {
            if (a_id == 0 || g_lootOpen.load() != a_id) return;
            TaskRelay::Add([a_id, a_why] { HideOurLootMenu(a_id, a_why); });
        }

        // v10 WakeUp (psc:127-156), the C++ half. `a_cppPct` < 0 = health not read. `a_timer` = the game-hour timer woke
        // her (M1c: narrated, `a_hoursOut` game hours out); otherwise the heal check did.
        void BeginWake(Entry& e, RE::FormID a_id, std::string a_reason, float a_cppPct, bool a_timer, std::int32_t a_hoursOut,
                       std::vector<Call>& a_calls)
        {
            CloseOurLoot(a_id, "wakes up");   // 2.1.0
            e.state        = State::kComingTo;
            e.comingToRank = kRankComingTo;   // psc:145
            e.stageSeconds = 0.0f;            // psc:146
            e.awaitingWake = true;
            e.serial       = ++g_serial;
            a_calls.push_back(Call{ .kind     = Call::Kind::kWake,
                                    .id       = a_id,
                                    .serial   = e.serial,
                                    .healRate = e.savedHealRate,
                                    .cppPct   = a_cppPct,
                                    .who      = e.who,
                                    .reason   = std::move(a_reason),
                                    .timer    = a_timer,
                                    .hoursOut = a_hoursOut });
        }

        // U1 T6b: the time wake of an UNCONSCIOUS NPC. FinishWake + (SBKO excluded her) Unguard + the graph listener off.
        // The potion mask stays in the entry until FinishWake's stack has finished (a failed dispatch goes back to UNCONSCIOUS).
        void BeginWakeU(Entry& e, RE::FormID a_id, std::string a_reason, float a_cppPct, std::int32_t a_hoursOut,
                        std::vector<Call>& a_calls)
        {
            CloseOurLoot(a_id, "wakes up");   // 2.1.0
            e.state        = State::kComingTo;
            e.comingToRank = kRankComingTo;
            e.stageSeconds = 0.0f;
            e.awaitingWake = true;
            e.serial       = ++g_serial;
            e.wakeKind     = WakeKind::kTime;
            e.leftoverWake = false;
            a_calls.push_back(Call{ .kind     = Call::Kind::kFinishWake,
                                    .id       = a_id,
                                    .serial   = e.serial,
                                    .healRate = e.savedHealRate,
                                    .cppPct   = a_cppPct,
                                    .who      = e.who,
                                    .reason   = std::move(a_reason),
                                    .timer    = true,
                                    .hoursOut = a_hoursOut,
                                    .mask     = (std::max)(0, e.potionMask) });
            if (e.severExcluded) {
                a_calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = a_id, .who = e.who, .packed = e.severPacked });
                e.severExcluded = false;
            }
            a_calls.push_back(Call{ .kind = Call::Kind::kDetachFoot, .id = a_id, .who = e.who });
        }

        // U2a / U2b: a STRAIGHT wake - the heal wake (NarrateHealWake) or finishing a u2a-save leftover (`a_clearNoBleedout`: the old
        // window had set NoBleedoutRecovery; no narration): FinishWake rank 3 + Unguard + the graph listener off.
        void BeginStraightWake(Entry& e, RE::FormID a_id, std::string a_reason, float a_cppPct, std::int32_t a_narrate,
                               bool a_clearNoBleedout, std::vector<Call>& a_calls)
        {
            CloseOurLoot(a_id, "wakes up");   // 2.1.0
            e.state        = State::kComingTo;
            e.comingToRank = kRankComingTo;
            e.stageSeconds = 0.0f;
            e.awaitingWake = true;
            e.serial       = ++g_serial;
            e.healPending  = false;
            e.sleepWakePending = false;   // u6f: whichever wake came first, this is THE wake
            e.wakeKind     = WakeKind::kStraight;
            e.leftoverWake = a_clearNoBleedout;
            a_calls.push_back(Call{ .kind     = Call::Kind::kStraightWake,
                                    .id       = a_id,
                                    .serial   = e.serial,
                                    .healRate = e.savedHealRate,
                                    .cppPct   = a_cppPct,
                                    .who      = e.who,
                                    .reason   = std::move(a_reason),
                                    .mask     = (std::max)(0, e.potionMask),
                                    .narrate  = a_narrate,
                                    .flags    = a_clearNoBleedout ? 1 : 0 });
            if (e.severExcluded) {
                a_calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = a_id, .who = e.who, .packed = e.severPacked });
                e.severExcluded = false;
            }
            a_calls.push_back(Call{ .kind = Call::Kind::kDetachFoot, .id = a_id, .who = e.who });
        }

        // U2a / U2b: the first HEAL -> the wake, STRAIGHT UP (U2b: no bleedout window). Caller made sure nothing of SBKO's is in flight
        // (M-1) and nobody has taken her.
        void BeginHealWake(Entry& e, RE::FormID a_id, const char* a_why, float a_cppPct, std::vector<Call>& a_calls)
        {
            e.healPending = false;
            logger::info("[KO] {} HEAL WAKE ({}): {} -> FinishWake: she gets up, coming to", e.who, a_why, e.healTrigger);
            BeginStraightWake(e, a_id, std::format("healed ({})", e.healTrigger), a_cppPct, kNarrateHeal, false, a_calls);
        }

        // u6f: a Sleep-spell SLEEPER woken by a hit / the player's talk / another mod's Wake - the heal wake's STRAIGHT path (FinishWake rank 3:
        // unconscious off, Paralysis 0, HealRate + potions back, Health raised to fWakeHealthFraction only if she is below it, never
        // lowered; the SeverActions include; NarrateHealWake, which says "<name> wakes up." for a sleeper). Her SBKO_SleepFaction rank stays
        // until the coming-to's end, so SkyrimNet's wake prompt still reads seen / not seen. Caller made sure nothing of SBKO's is in flight
        // (M-1) and nobody has taken her.
        void BeginSleepWake(Entry& e, RE::FormID a_id, float a_cppPct, std::vector<Call>& a_calls)
        {
            const std::string trigger = e.sleepWakeTrigger.empty() ? std::string("a hit or a talk") : e.sleepWakeTrigger;
            e.sleepWakePending        = false;
            logger::info("[KO] {} SLEEP WAKE ({}): she was only asleep (the Sleep spell) -> FinishWake: she gets up, coming to", e.who,
                         trigger);
            BeginStraightWake(e, a_id, std::format("woken from the Sleep spell's sleep ({})", trigger), a_cppPct, kNarrateHeal, false,
                              a_calls);
        }

        // U2a: ANOTHER MOD TOOK HER: SBKO stops holding her - no +50, no HUD line - and gives back
        // what it set (SBKO_Helper.LetGo flags): NoBleedoutRecovery off only when SBKO's window set it (DROPPING / WINDOW);
        //   kCaptive (DoM capture / slave / tie / bag / restrained): Paralysis 0 + HealRate / potions / SeverActions back, the flag STAYS;
        //   kBag (a disable): the same + unconscious off (SBKO set it, nothing else holds her);
        //   kVrte (VRTE's choke KO, rank 5): NOTHING of hers is touched now - VRTE owns Paralysis / unconscious and has just saved
        //   SBKO's HealRate 0 as "hers"; SBKO's part comes back when VRTE wakes her (the released watch).
        // u2a fix 1: the entry STAYS (releasing) until LetGo's reply - a LetGo that was not dispatched is retried at the next check
        // (m-3); OnLetGoDone then forgets her and keeps watching a kCaptive / kVrte NPC (g_released) or belts a kBag one.
        void LetGoNow(Entry& e, RE::FormID a_id, const char* a_why, TakenKind a_kind, std::vector<Call>& a_calls)
        {
            if (e.releasing) return;
            CloseOurLoot(a_id, "is let go");   // 2.1.0
            const bool clearBleed = true;   // u2c fix 4: SBKO set NoBleedoutRecovery on every hold (the psc clears it on every LetGo too)
            const bool vrte       = a_kind == TakenKind::kVrte;
            const bool clearUnc   = a_kind == TakenKind::kBag;
            const std::int32_t flags = (clearBleed ? 1 : 0) | (clearUnc ? 2 : 0) | (vrte ? 0 : 4) | (vrte ? 0 : 8);
            e.releasing   = true;
            e.releaseKind = static_cast<std::uint8_t>(a_kind);
            e.serial      = ++g_serial;
            logger::info("[KO] {} TAKEN BY ANOTHER MOD ({}) - SBKO lets go (LetGo: {}{}{}, rank 0) and stops holding her",
                         e.who, a_why,
                         vrte ? std::string("nothing of VRTE's touched; HealRate / potions / SeverActions come back when VRTE wakes her")
                              : std::format("Paralysis 0, HealRate {}, potions mask {}, SeverActions {}",
                                            e.savedHealRate >= 0.0f ? std::format("{:.4f}", e.savedHealRate) : std::string("base"),
                                            (std::max)(0, e.potionMask), e.severExcluded ? "include" : "-"),
                         clearBleed ? ", NoBleedoutRecovery off" : "",
                         vrte ? "" : clearUnc ? ", unconscious off" : ", unconscious left as it is (SBKO watches for her release)");
            a_calls.push_back(Call{ .kind     = Call::Kind::kLetGo,
                                    .id       = a_id,
                                    .serial   = e.serial,
                                    .healRate = e.savedHealRate,
                                    .who      = e.who,
                                    .mask     = vrte ? 0 : (std::max)(0, e.potionMask),
                                    .flags    = flags });
            if (!vrte && e.severExcluded) {
                a_calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = a_id, .who = e.who, .packed = e.severPacked });
                e.severExcluded = false;
            }
            a_calls.push_back(Call{ .kind = Call::Kind::kDetachFoot, .id = a_id, .who = e.who });
        }

        // u2a fix 1 (M-A): re-adopt a released NPC who is given back still unconscious. Caller holds g_mtx; `a_shove` = she
        // was not ragdolled; `a_guardSever` decided before the lock (SeverPresent asks the VM).
        void BeginReAdopt(Entry& e, RE::FormID a_id, const Released& a_rel, bool a_shove, std::int32_t a_potions, bool a_guardSever,
                          std::vector<Call>& a_calls, bool a_orphan = false)
        {
            const auto& cfg = Settings::Get();
            ResetEpisode(e, a_id);
            e.unc       = true;
            e.serial    = ++g_serial;
            e.episode   = e.serial;
            e.readopted = true;
            e.orphan    = a_orphan;
            e.recoverHours  = RollHours();
            e.dispatchHours = HoursNow();
            e.koHours       = e.dispatchHours;
            e.potionMask    = a_rel.deferred ? a_rel.potionMask : -1;   // a VRTE release still has its abilities off
            e.severExcluded = a_rel.deferred && a_rel.severExcluded;
            e.severPacked   = e.severExcluded ? a_rel.severPacked : 0;
            // u6 fix 1 (F1): a released HIT knock-out stays one - after ResetEpisode (which clears it). Her Health at the knock-out
            // is not carried (hitPct -1), so the hold = her Health at the re-adoption's settle, no cap (as a save loaded in the settle).
            // An orphan cannot know her kind: Released{} has hitKO false -> the bleedout knock-out's cap.
            e.hitKO         = a_rel.hitKO;
            e.hitPct        = -1.0f;
            e.sleepMark     = a_rel.sleepMark;   // u6f: a released sleeper stays one (her SBKO_SleepFaction rank was never touched)
            e.cls           = a_rel.choke ? kClsChoke : 0;   // u7a (U7): a released choke stays one (her SBKO_SleepFaction rank 3 too)
            e.chokeSeconds  = a_rel.chokeSeconds;
            e.chokeHours    = a_rel.chokeHours;
            std::string extra = !a_rel.hitKO ? std::string()
                                : std::format("; a HIT knock-out: held at her Health after the settle, no cap; FDC roll {}",
                                              g_fdcRolledFaction ? "skipped (FDC_SBKO_RolledFaction rank 2 before rank 10)"
                                                                 : "- (FDC_SBKO_Patch.esp not loaded)");
            // u2a fix 2: the call also carries the release record (mask / packed / flag bits 2-4; u6: bit 8 = a hit
            // knock-out), so a ReAdopt that cannot be dispatched puts her back on the released watch instead of forgetting her.
            if (a_rel.choke) extra += std::format("; a CHOKE knock-out (held {:.1f} s)", a_rel.chokeSeconds);
            a_calls.push_back(Call{ .kind = Call::Kind::kReAdopt, .id = a_id, .serial = e.serial, .healRate = a_rel.healRate,
                                    .cppPct = a_rel.chokeHours,   // u7a: the choke's hour rides here (only read back on a failed dispatch)
                                    .who = e.who, .mask = a_rel.potionMask, .packed = a_rel.severPacked,
                                    .flags = (a_shove ? 1 : 0) | (a_rel.deferred ? 2 : 0) | (a_rel.severExcluded ? 4 : 0) |
                                             (a_rel.hitKO ? 8 : 0) | (a_rel.sleepMark > 0 ? 16 : 0) |
                                             (a_rel.sleepMark == kSleepSeen ? 32 : 0) |   // u6f: 16 / 32 = a sleeper (seen)
                                             (a_rel.choke ? 64 : 0),                       // u7a: 64 = a choke
                                    .choke = a_rel.chokeSeconds });
            if (cfg.potionsOff && a_potions > 0) {
                e.potionsInFlight = true;
                a_calls.push_back(Call{ .kind = Call::Kind::kPotionsOff, .id = a_id, .serial = e.episode, .who = e.who });
                extra += std::format("; Smart NPC Potions abilities seen (mask {}) -> PotionsOff", a_potions);
            }
            if (!e.severExcluded && a_guardSever) {
                e.severInFlight = true;
                a_calls.push_back(Call{ .kind = Call::Kind::kSeverGuard, .id = a_id, .serial = e.episode, .who = e.who });
                extra += "; SeverActions guard asked";
            }
            if (a_orphan)
                logger::info("[KO] {} ORPHAN ADOPTED (rank 10 + unconscious, no record - a lost knock-out) - ReAdopt: rank 10, "
                             "NoBleedoutRecovery on, Paralysis 1{}, HealRate 0 (her current value saved; 0 = SBKO's own zero -> her base "
                             "value at the wake){}; settle, then the hold (u6: her knock-out's kind is unknown - held with the {}% cap)",
                             e.who, a_shove ? ", shoved (not ragdolled)" : " (already ragdolled - no shove)", extra,
                             std::lround(cfg.unconsciousHealthFraction * 100.0f));
            else
                logger::info("[KO] {} RE-ADOPTED (released to another mod, still unconscious, no longer taken) - ReAdopt: rank 10, "
                             "NoBleedoutRecovery on, Paralysis 1{}, HealRate 0 (her original {} carried){}",
                             e.who, a_shove ? ", shoved (not ragdolled)" : "",
                             a_rel.healRate >= 0.0f ? std::format("{:.4f}", a_rel.healRate) : std::string("base"), extra);
        }

        // U1: what a knocked-out NPC who DIES or is dropped leaves behind that belongs to another mod: her Smart NPC Potions
        // abilities and her SeverActions exclusion are given back (an ability on a corpse does nothing; a resurrected NPC
        // keeps nothing odd). HealRate / Paralysis stay as v10 OnDying left them. Caller holds g_mtx.
        void GiveBackOthers(Entry& e, RE::FormID a_id, std::vector<Call>& a_calls)
        {
            if (e.potionMask > 0) {
                a_calls.push_back(Call{ .kind = Call::Kind::kPotionsBack, .id = a_id, .who = e.who, .mask = e.potionMask });
                e.potionMask = -1;
            }
            if (e.severExcluded) {
                a_calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = a_id, .who = e.who, .packed = e.severPacked });
                e.severExcluded = false;
            }
            if (e.unc) a_calls.push_back(Call{ .kind = Call::Kind::kDetachFoot, .id = a_id, .who = e.who });
        }

        // U1: the potion re-check (VRTE KOPotionsRecheck :2212-2218): Smart NPC Potions' quest re-adds its abilities. U2a: also in
        // the WAKING / WINDOW states (a potion drunk in the window would lift her out of the bleedout). Caller holds g_mtx.
        void PotionRecheck(Entry& e, RE::Actor* a, RE::FormID a_id, std::vector<Call>& a_calls)
        {
            if (!Settings::Get().potionsOff || e.potionsInFlight) return;
            if (const std::int32_t again = PotionSpellsOn(a); again > 0) {
                logger::info("[KO] {} potion ability back on her (mask {}, Smart NPC Potions re-added it) - taken off again", e.who,
                             again);
                e.potionsInFlight = true;
                a_calls.push_back(Call{ .kind = Call::Kind::kPotionsOff, .id = a_id, .serial = e.episode, .who = e.who });
            }
        }

        // ---- U2a: has ANOTHER MOD taken her? (Diary of Mine's DOMInteractPerk Abduct / Tie-up / Bag; any disable;
        // u2a fix 1: VRTE's choke KO rank 5 too) ----
        // Returns the reason + the kind, or nullptr. Field reads + GetFactionRank (RELOCATION_ID 36668, VR 0x600190) - safe on an
        // actor without 3D (a bagged NPC is moved to DOMTonyMarker).
        const char* TakenBy(RE::Actor* a_actor, TakenKind& a_kind)
        {
            a_kind = TakenKind::kNone;
            if (!a_actor) return nullptr;
            a_kind = TakenKind::kCaptive;
            if (g_domFaction && a_actor->GetFactionRank(g_domFaction, false) >= 0)
                return "Diary of Mine is capturing her (DOMBeingCaptured)";   // Abduct - DoM owns her now; the flag stays
            // u2b fix: a FAST Abduct can finish between two checks (DOMBeingCaptured gone, her own
            // SetUnconscious(false) done) - a DoM slave is DoM's, never "woken by another mod" nor knocked out by SBKO
            if (g_domSlaveFaction && a_actor->GetFactionRank(g_domSlaveFaction, false) >= 0)
                return "a Diary of Mine slave (DOMActorFaction) - DoM owns her";
            const auto* st = a_actor->AsActorState();
            if (st && st->GetLifeState() == RE::ACTOR_LIFE_STATE::kRestrained)
                return "restrained (Diary of Mine's Tie-up, or another restraint)";   // never SetUnconscious(false) - it could untie her
            if (g_domTiedFaction && a_actor->GetFactionRank(g_domTiedFaction, false) >= 0)
                return "tied by Diary of Mine (DOMActionTied)";                        // the same: the flag stays as it is
            // u2b fix (they need healing): DoM's bag keeps her unconscious too - the flag stays, she is
            // watched (g_released), and RE-ADOPTED if she comes out of the sack still unconscious; no wake without a heal
            if (g_domBagFaction && a_actor->GetFactionRank(g_domBagFaction, false) >= 0)
                return "put in a bag (Diary of Mine DOMActionInContainer)";
            a_kind = TakenKind::kBag;
            if (a_actor->IsDisabled())
                return "disabled by another mod";
            a_kind = TakenKind::kVrte;
            if (VrteHolds(a_actor))
                return "VRTE holds her (VRTE_ChokeStateFaction rank 5)";
            a_kind = TakenKind::kNone;
            return nullptr;
        }

        // ---- THE ONE PLACE that decides what an external clear means (U2b: a "Wake" does not wake them, they need healing;
        // DoM's menu Wake gets "(NPC's name) is too injured to wake up") ---------------------------------------------------------
        // Called by the UNCONSCIOUS check only (SBKO's own clears happen in COMING_TO / the let-go, never here), and only when she is
        // NOT bleeding out (T7 owns that) and alive (T14), and no HoldDown of SBKO's is in flight. Every clear is RE-ASSERTED
        // (unconscious + Paralysis + shove); the decision is only whether the player is told:
        //   * Paralysis cleared, unconscious flag still TRUE        -> re-assert silently
        //   * the KNOCK-OUT's settle (iGraceChecks) is not over       -> re-assert silently (KnockOutU may still be taking effect)
        //   * within the first 2 checks after a game load / cell attach / 3D load (an engine reset - a correctness rule, not a
        //     gameplay limit; never armed by a knock-out) -> re-assert silently
        //   * the life state reads RESTRAINED (DoM's Tie-up = SetRestrained) -> TAKEN, LetGo (never a re-assert - it would fight /
        //     untie the captive); also caught earlier by TakenBy()
        //   * a life state other than Alive                         -> re-assert silently (logged)
        //   * otherwise (settled, alive, not in bleedout, not right after a load) = someone tried to WAKE her (Diary of Mine's menu
        //     "Wake": SetUnconscious(false) + PushActorAway(0.1), Paralysis left at 1) -> re-assert + ONE HUD line
        //     "<name> is too injured to wake up." (SayTooInjured). Only a heal wakes her.
        //   u6f: a Sleep-spell SLEEPER (sleepMark >= 1) on that last line is WOKEN instead (sleepWake - with DoM, talking to an
        //   unconscious NPC opens Diary of Mine's menu, whose "Wake" is this clear): no re-assert, no "too injured". Always (no switch).
        struct ClearDecision
        {
            bool        tooInjured = false;
            bool        letGo      = false;
            bool        sleepWake  = false;   // u6f
            const char* what       = "";
            std::string why;
        };

        ClearDecision ExternalClear(const Entry& e, bool a_unc, float a_para, RE::ACTOR_LIFE_STATE a_life)
        {
            ClearDecision d;
            d.what = (!a_unc && a_para < 0.5f) ? "the unconscious flag AND Paralysis" : !a_unc ? "the unconscious flag" : "Paralysis only";
            if (a_unc)
                d.why = "a Paralysis-only clear - re-asserted silently";
            else if (a_life == RE::ACTOR_LIFE_STATE::kRestrained) {
                d.letGo = true;
                d.why   = "restrained - taken by another mod";
            } else if (!e.settledOnce)
                d.why = "the knock-out's settle is not over (KnockOutU may still be taking effect)";
            else if (e.resetActive && e.checksSince <= kResetChecks)
                d.why = std::format("check {} after {} - an engine reset, not a deliberate wake", e.checksSince, e.since);
            else if (a_life != RE::ACTOR_LIFE_STATE::kAlive)
                d.why = std::format("her life state is {}, not Alive", static_cast<int>(a_life));
            else if (e.sleepMark > 0) {   // u6f: only asleep - another mod's deliberate Wake wakes her (not gated: bSleepWakeOnTalk 0 keeps
                                          // Diary of Mine's menu usable on a sleeper - Tie-up / Bag - and its Wake still works)
                d.sleepWake = true;
                d.why       = "someone woke her (another mod cleared her unconscious flag - e.g. Diary of Mine's menu Wake) - she was only "
                              "asleep (the Sleep spell): she wakes up";
            } else {
                d.tooInjured = true;
                d.why        = "someone tried to wake her - she is too injured: only a heal wakes her";
            }
            return d;
        }

        Entry& AdoptDown(RE::FormID a_id, std::string a_who)
        {
            Entry e;
            e.state         = State::kDown;
            e.serial        = ++g_serial;
            e.savedHealRate = kHealRateUseBase;
            e.recoverHours  = RollHours();
            e.koHours       = HoursNow();   // M1c: the true knock-out moment lived in v10's script - the adoption time stands in
            e.wakeAtHours   = e.koHours + e.recoverHours;
            e.who           = std::move(a_who);
            auto& ref       = g_map[a_id] = std::move(e);
            SyncCount();
            return ref;
        }

        // M1d: the load safety's wake - rank 0 = fully clear, no HUD, no narration. M1 DOWN: WakeUp(id, saved, 0, 0). U1
        // UNCONSCIOUS: FinishWake(id, saved, 0, mask, 0) + the SeverActions include. A u2a-save window leftover: the same FinishWake
        // + NoBleedoutRecovery off. She also joins g_declined so no knock-out / adoption looks at her until she is seen out of bleedout.
        void MakeConscious(RE::FormID a_id, const Pending& a_p, std::string a_who, std::vector<Call>& a_calls)
        {
            g_declined.insert(a_id);
            g_adoptCandidates.erase(a_id);
            g_adoptAtLoad.erase(a_id);   // u6h
            if (a_p.unc) {
                a_calls.push_back(Call{ .kind     = Call::Kind::kRestoreU,
                                        .id       = a_id,
                                        .healRate = a_p.healRate,
                                        .who      = a_who,
                                        .mask     = (std::max)(0, a_p.potionMask),
                                        .flags    = a_p.window ? 1 : 0 });   // a u2a leftover: NoBleedoutRecovery off too
                if (a_p.severExcluded)
                    a_calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = a_id, .who = a_who, .packed = a_p.severPacked });
            } else {
                a_calls.push_back(Call{ .kind = Call::Kind::kRestore, .id = a_id, .healRate = a_p.healRate, .who = std::move(a_who) });
            }
        }

        Pending PendingOf(const Entry& e)
        {
            Pending p;
            // U2b: only a u2a-save window leftover (still WINDOW, or its finishing FinishWake still running)
            p.window        = e.unc && (e.state == State::kWindow || (e.state == State::kComingTo && e.awaitingWake && e.leftoverWake));
            p.unc           = e.unc && (p.window || e.state == State::kEntering || e.state == State::kUnconscious ||
                                         (e.state == State::kComingTo && e.awaitingWake));
            p.healRate      = e.state == State::kEntering ? kHealRateUseBase : e.savedHealRate;
            p.potionMask    = e.potionMask;
            p.severExcluded = e.severExcluded;
            p.severPacked   = e.severPacked;
            return p;
        }

        // u2c fix 2 / 3: when the orphan net adopts `a_real`, a pending / released entry that is the SAME NPC under an id an older
        // co-save mangled is dropped: same low 24 bits with a regular top byte (rule 1: FE38E673 saved, 9038E673
        // restored), or - her plugin became a regular one - the old light slot left in bits 12-23 (9038E673 for the real 90000673).
        // FE / FF ids are never twins. Caller holds g_mtx.
        void DropTwins(RE::FormID a_real, const std::string& a_who)
        {
            // guard: rule 1 only for an FE NPC, rule 2 only for an id with nothing in bits 12-23, and never
            // an id that is a real actor now (a mangled id resolves to no form) - so another plugin's NPC is never taken for a twin
            const bool realIsLight = (a_real >> 24) == 0xFE;
            const bool realIsSmall = (a_real & 0x00FFF000) == 0;
            auto twin = [a_real, realIsLight, realIsSmall](RE::FormID x) {
                const std::uint32_t top = x >> 24;
                if (x == a_real || top == 0xFE || top == 0xFF) return false;
                const bool same24  = realIsLight && (x & 0x00FFFFFF) == (a_real & 0x00FFFFFF);
                const bool slotOff = realIsSmall && (x & 0x00FFF000) != 0 && (x & 0xFF000FFF) == a_real;
                return (same24 || slotOff) && !RE::TESForm::LookupByID<RE::Actor>(x);
            };
            for (auto it = g_pendingConscious.begin(); it != g_pendingConscious.end();) {
                if (twin(it->first)) {
                    logger::info("[KO] {}: dropped the load-safety entry 0x{:08X} - the same NPC under an id an older co-save mangled "
                                 "(a light-plugin FormID)", a_who, it->first);
                    it = g_pendingConscious.erase(it);
                } else
                    ++it;
            }
            for (auto it = g_released.begin(); it != g_released.end();) {
                if (twin(it->first)) {
                    logger::info("[KO] {}: dropped the released-watch entry 0x{:08X} - the same NPC under an id an older co-save "
                                 "mangled (a light-plugin FormID)", a_who, it->first);
                    it = g_released.erase(it);
                } else
                    ++it;
            }
        }

        // A loaded actor NOT in the map: is a knockout mark still on her? (the first unpaused check after kPostLoadGame, and the
        // check after her cell attached - u2c P3: never a faction read for every loaded actor every check). Ranks only - never
        // RemoveFromFaction. u2c fix 3, the ORPHAN net: rank 10 + unconscious + no record + not taken -> adopted as UNCONSCIOUS.
        // u6h (G4, the M1d rule: a knocked-out NPC in an unloaded cell is made conscious): a_arrived = her cell attached
        // during PLAY (she came from an unloaded cell - not the after-load pass, not a cell that attached during the load) -> such an
        // orphan is WOKEN at once (MakeConscious: FinishWake rank 0) instead of held for a new 6-12 h.
        void AdoptCheck(RE::Actor* a, const char* a_why, bool a_guardSever, std::vector<Call>& a_calls, bool a_arrived = false)
        {
            const RE::FormID id = a->GetFormID();
            if (!g_faction || g_map.contains(id) || g_released.contains(id)) return;   // u2c: the released watch has her
            // M1d: made conscious by the load safety (her rank 0 may not be written yet) / still waiting for it
            if (g_declined.contains(id) || g_pendingConscious.contains(id)) return;
            const std::int32_t rank = a->GetFactionRank(g_faction, false);   // -1 = not a member
            if (rank == kRankDown) {
                const auto* st = a->AsActorState();
                // u6h fix 1: the arriving orphan's state, read once - in bleedout, unconscious or paralysed = still
                // knocked out, whatever the engine shows after her cell attached (it may reset an unconscious NPC under its bleedout line
                // into bleedout)
                const bool bleedNow = IsBleedingOut(a);
                const bool uncNow   = st && st->IsUnconscious();
                const bool paraNow  = a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kParalysis) >= 0.5f;
                if (sbko::IsDeadNow(a)) {
                    std::string who = Who(a, id);
                    logger::info("[KO] {} has rank 10 but is dead ({}) - ClearMark: rank 0, NoBleedoutRecovery off", who, a_why);
                    a_calls.push_back(Call{ .kind = Call::Kind::kClearMark, .id = id, .rank = 0, .who = std::move(who) });
                } else if (a_arrived && (bleedNow || uncNow || paraNow)) {
                    // u6h (G4) + fix 1 (the M1d rule: a knocked-out NPC in an unloaded cell is made
                    // conscious): she ARRIVED from an unloaded cell still knocked out - in bleedout, unconscious or paralysed - and SBKO has
                    // no record of her -> woken now with the load safety's own wake (MakeConscious: FinishWake rank 0; it also declines her,
                    // so the bleedout scan never knocks her out again while she gets up). Her record is lost, so: HealRate = her base value
                    // when it reads 0 (SBKO's zero), else left as it is; potions: the mask is unknown - none given here (Smart NPC Potions
                    // re-adds its own abilities); SeverActions: no snapshot - not touched (its follower page can include her)
                    std::string       who  = Who(a, id);
                    TakenKind         kind = TakenKind::kNone;
                    const char* const why  = TakenBy(a, kind);   // DoM capture / slave / tie / bag, a disable, VRTE rank 5
                    if (why) {
                        logger::info("[KO] {} has rank 10 and is knocked out, no record ({}) - but taken by another mod ({}): left alone",
                                     who, a_why, why);
                        return;
                    }
                    DropTwins(id, who);
                    const float hr = a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealRate);
                    Pending     p;
                    p.unc        = true;
                    p.healRate   = hr > 0.0f ? hr : kHealRateUseBase;
                    p.potionMask = 0;
                    logger::info("[KO] {} has rank 10 and is {} with no SBKO record ({}: she ARRIVED from an unloaded cell) - a lost "
                                 "knock-out: WOKEN at once (u6h, his M1d rule: a knock-out found in an unloaded cell is made conscious) - "
                                 "FinishWake rank 0: NoBleedoutRecovery off, Health raised to at least {}% (never lowered), unconscious off, "
                                 "Paralysis 0, HealRate {}, stealth meter back; potions: mask unknown - none given back (Smart NPC Potions "
                                 "re-adds its own); SeverActions: no snapshot - not touched",
                                 who,
                                 bleedNow ? "IN BLEEDOUT (the engine reset her out of unconscious)"
                                 : uncNow ? "unconscious"
                                          : "paralysed (the unconscious flag reads off)",
                                 a_why, std::lround(Settings::Get().wakeHealthFraction * 100.0f),
                                 hr > 0.0f ? std::format("kept at {:.4f}", hr) : std::string("back to her BASE value (it read 0)"));
                    MakeConscious(id, p, who, a_calls);
                } else if (bleedNow) {
                    auto& e = AdoptDown(id, Who(a, id));
                    logger::info("[KO] {} ADOPTED as DOWN ({}): rank 10 and still in bleedout = a v10 knockout. HealRate comes back "
                                 "from her base value; wakes on a heal to {}%, or in ~{} game-hrs",
                                 e.who, a_why, std::lround(Settings::Get().wakeHealthFraction * 100.0f), static_cast<int>(e.recoverHours));
                } else if (uncNow || paraNow) {
                    // Paralysis 1 still on her counts as held too - right after a load / an attach the
                    // engine may have reset the unconscious flag (the U2a "engine reset"), and DoM's Wake leaves Paralysis 1; the
                    // UNCONSCIOUS check's settle re-asserts the flag (HoldDown). (u6h fix 1: an ARRIVING one never gets here - woken above)
                    std::string       who  = Who(a, id);
                    TakenKind         kind = TakenKind::kNone;
                    const char* const why  = TakenBy(a, kind);   // DoM capture / slave / tie / bag, a disable, VRTE rank 5
                    if (why) {
                        logger::info("[KO] {} has rank 10 and is unconscious, no record ({}) - but taken by another mod ({}): left alone",
                                     who, a_why, why);
                        return;
                    }
                    DropTwins(id, who);
                    const std::int32_t potions = Settings::Get().potionsOff ? PotionSpellsOn(a) : 0;
                    Released           r;
                    r.healRate = -1.0f;   // ReAdopt saves her current HealRate (0 = SBKO's own zero -> -2.0 = her base value)
                    Entry e;
                    e.who     = who;
                    auto& ref = g_map[id] = std::move(e);
                    logger::info("[KO] {} has rank 10 and is {} with no SBKO record ({}) - a lost knock-out", who,
                                 st && st->IsUnconscious() ? "unconscious" : "paralysed (the unconscious flag reads off)", a_why);
                    BeginReAdopt(ref, id, r, !a->IsInRagdollState(), potions, a_guardSever, a_calls, true);
                    g_declined.erase(id);
                    SyncCount();
                } else {
                    // a conscious NPC still marked rank 10, no record = a stale mark (a lost knock-out something else woke)
                    std::string who = Who(a, id);
                    logger::info("[KO] {} has rank 10 but is CONSCIOUS (no Paralysis) with no record ({}) - a stale mark: ClearMark "
                                 "(rank 0, NoBleedoutRecovery off, HealRate back if it reads 0)", who, a_why);
                    a_calls.push_back(Call{ .kind = Call::Kind::kClearMark, .id = id, .rank = 0, .who = std::move(who) });
                }
            } else if (rank >= 1 && rank <= kRankComingTo) {
                Entry e;
                e.state        = State::kComingTo;
                e.serial       = ++g_serial;
                e.comingToRank = rank;
                e.who          = Who(a, id);
                logger::info("[KO] {} ADOPTED as COMING TO at rank {} ({}) - a v10 coming-to; {} s of play per step", e.who, rank,
                             a_why, Settings::Get().stageSeconds);
                g_map[id] = std::move(e);
                SyncCount();
            }
        }

        // Detection (event or scan): v10 OnUpdate's `!bIsDown && IsBleedingOut() -> EnterDown()` (psc:63-65), with v10's
        // filter. Takes g_mtx itself - call it with NO lock held.
        // u2c fix 6: one line - once per NPC per bleedout - for an actor in bleedout that is NOT knocked out. Takes g_mtx.
        void SayNotKnockedOut(RE::Actor* a, const char* a_via, const char* a_why)
        {
            const RE::FormID id = a->GetFormID();
            {
                std::scoped_lock lk(g_mtx);
                if (!g_silentLogged.insert(id).second) return;
            }
            logger::info("[KO] {} in bleedout (seen by {}) - not knocked out: {}", Who(a, id), a_via, a_why);
        }

        // u6b (U5): what a NORMAL NPC's knock-out roll says about the bleedout she is in (PickBleedout, defined with the U5 code below;
        // it logs its own decision). Default kWait = do nothing now.
        enum class BleedVerdict : std::uint8_t
        {
            kWait     = 0,   // the hit's roll has not been made yet (or not asked) - nothing now, a later look decides
            kActHurt  = 1,   // lost / no roll (a punch) / ruled out: the game's own bleedout ("acting hurt") - SBKO does nothing
            kKnockOut = 2,   // won her roll AT the bleedout: SBKO's bleedout knock-out
            kHitKO    = 3,   // won at the hit: the hit's task knocks her out (Detect does nothing)
        };
        BleedVerdict PickBleedout(RE::Actor* a_actor, RE::FormID a_id);

        // u6d (F10): her CURRENT bleedout is already decided "acting hurt" (g_u5Mtx alone, a map lookup)
        bool U5ActingHurtDecided(RE::FormID a_id)
        {
            std::scoped_lock lk(g_u5Mtx);
            const auto       it = g_u5.find(a_id);
            return it != g_u5.end() && it->second.decided && !it->second.decidedKO && it->second.decidedFor == it->second.bleedSerial;
        }

        void Detect(RE::Actor* a, const char* a_via, std::vector<Call>& a_calls)
        {
            if (!a) return;
            if (a->IsPlayerRef()) {   // psc:46 - never the player's own bleedout
                SayNotKnockedOut(a, a_via, "the player");
                return;
            }
            if (!IsBleedingOut(a)) return;   // the event's task came after she left bleedout - nothing to explain
            const char* why = !a->Get3D()          ? "no 3D (not loaded)"
                              : sbko::IsDeadNow(a) ? "dead"
                              : !IsNPC(a)          ? "not an NPC (no ActorTypeNPC keyword - a creature)"
                              : !Helper::Present() ? "SBKO_Helper.pex is missing"
                                                   : nullptr;
            if (why) {
                SayNotKnockedOut(a, a_via, why);
                return;
            }
            // u6d (F10): a normal NPC whose bleedout is already decided "acting hurt" - nothing more to do or say while she kneels
            // (her ONE [chanceko] line was written when it was decided); no VM ask, no reads
            if (g_u5On.load(std::memory_order_relaxed) && !IsProtectedOrEssential(a) && U5ActingHurtDecided(a->GetFormID())) return;
            // 2.1.0 [WhoGetsKnockedOut]: her group is switched off and SBKO holds nothing of hers (not watched, no rank 10 / coming-to
            // rank) - every look below would end at the same gate; said once per bleedout, nothing more is read (no lock of ours held)
            if (const char* off = WhoSwitchedOff(a, IsFollower(a))) {
                const std::int32_t r = g_faction ? a->GetFactionRank(g_faction, false) : -1;
                if (!Watched(a->GetFormID()) && r != kRankDown && !(r >= 1 && r <= kRankComingTo)) {
                    SayNotKnockedOut(a, a_via, off);
                    return;
                }
            }
            const auto&        cfg   = Settings::Get();
            const RE::FormID   id    = a->GetFormID();
            const std::int32_t rank  = g_faction ? a->GetFactionRank(g_faction, false) : -1;
            const bool         brawl = g_brawlFaction && a->GetFactionRank(g_brawlFaction, false) >= 0;   // m1f, -1 = not a member
            const bool         vrte  = VrteHolds(a);                                                       // U1
            const bool         foll  = IsFollower(a);                                                      // U1 (U2a: log only)
            const std::string  ess   = EssFlags(a);                                                        // u2d (5)
            // U2a: the guard is asked for EVERY NPC (SeverActions decides); decided before the lock - SeverPresent asks the VM
            const bool         guardSever = cfg.severActionsGuard && g_severLoaded && Helper::SeverPresent();
            const std::int32_t potions    = cfg.potionsOff ? PotionSpellsOn(a) : 0;
            const char*        taken      = nullptr;                                                     // U2a (TakenBy)
            std::string        who   = Who(a, id);
            const auto         nowTp = std::chrono::steady_clock::now();
            const float        gap   = cfg.checkSeconds;

            TakenKind takenKind = TakenKind::kNone;
            taken = TakenBy(a, takenKind);   // U2a: another mod holds her (DoM capture / bag / restrained / disabled / VRTE)

            // u6b (U5: an NPC whose HP drops low gets a KO roll first, then goes into bleedout): a NORMAL NPC (not essential,
            // not protected) is no longer knocked out just for bleeding out - her knock-out roll decides (the roll of the hit that
            // started this bleedout, or ONE roll here when no rolled hit started it). Asked only when nothing below would stop anyway
            // (SBKO already holds her, another mod took her, a brawl, VRTE, declined, the load-safety list): those lines run as before.
            // Essential / protected NPCs: unchanged. bChanceKO 0 (no hook): unchanged for everyone.
            const bool   normalNpc = g_u5On.load(std::memory_order_relaxed) && !IsProtectedOrEssential(a);
            BleedVerdict verdict   = BleedVerdict::kWait;
            if (normalNpc && !taken && !brawl && !vrte && !Watched(id) && !WhoSwitchedOff(a, foll)) {   // 2.1.0: no roll for a switched-off group
                bool skip = false;
                {
                    std::scoped_lock lk0(g_mtx);
                    skip = g_declined.contains(id) || g_pendingConscious.contains(id);
                    // u6d (F10): not when the lines below cannot knock her out NOW anyway (a COMING_TO NPC still inside the wake gap
                    // or her wake's stack, a let-go in flight, a held entry, the v10 rank-10 adoption) - a "KNOCKED OUT" roll line with no
                    // knock-out after it misleads; a later look rolls
                    if (!skip) {
                        if (const auto it = g_map.find(id); it != g_map.end())
                            skip = it->second.releasing || it->second.state != State::kComingTo || it->second.awaitingWake ||
                                   std::chrono::duration<float>(nowTp - it->second.wakeDoneAt).count() < gap;
                        else
                            skip = rank == kRankDown;
                    }
                }
                if (!skip) verdict = PickBleedout(a, id);   // no lock of ours held (it reads her armour, may roll, logs)
            }

            std::scoped_lock lk(g_mtx);
            // u2d (a 1.5 s bleedout lets the other mods record it and do their job): before ANY knock-out she spends
            // fBleedoutGraceSeconds in the engine's own bleedout, so the mods that act on
            // bleedout (Follower Death Chance's roll, SeverActions, DoM's menu) get their turn first. A sequencing - not a limit.
            // true = not yet (the first sighting is logged and arms the check thread for the due moment).
            const float graceSecs  = cfg.bleedoutGraceSeconds;
            float       waitedSecs = 0.0f;
            auto inGrace = [&]() -> bool {
                if (graceSecs <= 0.0f) return false;
                auto [gi, fresh] = g_grace.try_emplace(id, nowTp);
                waitedSecs       = std::chrono::duration<float>(nowTp - gi->second).count();
                if (waitedSecs < graceSecs) {
                    if (fresh) {
                        g_perfGraceStarted.fetch_add(1, std::memory_order_relaxed);
                        logger::info("[KO] {} in bleedout (seen by {}; {}) - SBKO knocks her out in {:.1f} s if she is still bleeding out "
                                     "then: the engine's bleedout first, so other mods get their turn", who, a_via, ess, graceSecs);
                        ArmGraceWake(gi->second + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                      std::chrono::duration<float>(graceSecs)));
                    }
                    return true;
                }
                g_grace.erase(gi);
                g_perfGraceKnockedOut.fetch_add(1, std::memory_order_relaxed);
                return false;
            };
            auto graceNote = [&](const char* a_more) {
                return std::format("{}{}", graceSecs > 0.0f ? std::format(" after {:.2f} s in the engine's bleedout", waitedSecs)
                                                            : std::string(),
                                   a_more);
            };
            if (g_declined.contains(id)) {   // m-3: KnockOut declined / refused - not again while she stays in bleedout
                if (g_silentLogged.insert(id).second)   // u2c fix 6: once per bleedout
                    logger::info("[KO] {} in bleedout (seen by {}) - not knocked out: declined / let go earlier (KnockOut refused, the "
                                 "load safety woke her, or another mod took her) - skipped until she is seen out of bleedout",
                                 who, a_via);
                return;
            }
            if (taken) {
                // U2a: never knock out (or re-assert on) an NPC another mod has taken; one SBKO holds is let go by the check
                if (!g_map.contains(id)) {
                    g_declined.insert(id);
                    logger::info("[KO] {} is in bleedout but taken by another mod ({}, seen by {}) - no knockout; skipped until she is "
                                 "up", who, taken, a_via);
                }
                return;
            }
            if (auto it = g_map.find(id); it != g_map.end()) {
                Entry& e = it->second;
                if (e.releasing) return;   // u2a fix 1: a LetGo of SBKO's is in flight
                switch (e.state) {
                    case State::kUnconscious:
                        // U1 T7: an NPC SBKO holds UNCONSCIOUS is in bleedout (someone took her to 0 HP) - re-assert, unconscious
                        // FIRST (the engine restores some Health when she leaves bleedout; the settle starts again, the hold stays)
                        if (vrte || e.holdInFlight) return;
                        e.holdInFlight = true;
                        e.downChecks   = 0;   // fix 1: the settle starts again, the HOLD is kept (it never rises)
                        logger::info("[KO] {} found in BLEEDOUT while unconscious (seen by {}) - re-asserted (unconscious first, "
                                     "Paralysis 1, shove); the settle starts again, the hold stays {}",
                                     e.who, a_via, e.lockFrac >= 0.0f ? Pct1(e.lockFrac) : std::string("unset"));
                        a_calls.push_back(Call{ .kind = Call::Kind::kHoldDown, .id = id, .serial = e.episode, .who = e.who });
                        return;
                    case State::kWindow:     // a u2a-save leftover - finished by the check
                        return;
                    default:
                        break;
                }
            }
            if (brawl) {
                // m1f: a brawl's bleedout is the brawl's. She joins g_declined, so she stays skipped until a check sees her out
                // of bleedout - even if the brawl takes her out of the faction while she is still kneeling.
                g_declined.insert(id);
                g_silentLogged.insert(id);   // u2c: this line is the reason for this bleedout
                logger::info("[KO] {} is in a BRAWL (DGIntimidateFaction, seen by {}) - no knockout; skipped until she is up", who,
                             a_via);
                return;
            }
            if (vrte && !g_map.contains(id)) {
                // U1: VRTouchEvents' KO mark (VRTE_ChokeStateFaction rank 5) - VRTE holds her; never two owners of one flag
                g_declined.insert(id);
                logger::info("[KO] {} carries VRTE's KO mark (VRTE_ChokeStateFaction rank 5, seen by {}) - VRTE holds her, no "
                             "knockout; skipped until she is out of bleedout", who, a_via);
                return;
            }
            if (g_pendingConscious.contains(id)) return;   // M1d: the load safety looks at her first (next check)
            if (auto it = g_map.find(id); it != g_map.end()) {
                Entry& e = it->second;
                if (e.state != State::kComingTo || e.awaitingWake) return;   // ENTERING / DOWN: already hers
                // v10: a coming-to NPC found in bleedout again is knocked out again. v10's first look came one poll
                // (POLL_SECS) after its synchronous WakeUp; here WakeUp is async, so the same gap is counted from the
                // moment WakeUp's stack finished.
                if (std::chrono::duration<float>(nowTp - e.wakeDoneAt).count() < gap) return;
                if (vrte) return;
                if (normalNpc && verdict != BleedVerdict::kKnockOut) return;   // u6b (U5): her roll said no / not yet (PickBleedout logged it)
                // 2.1.0 [WhoGetsKnockedOut]: dropping again while coming to = a NEW automatic knock-out - her group's switch decides (off =
                // the game's own bleedout; her coming-to goes on as usual)
                if (const char* off = WhoSwitchedOff(a, foll)) {
                    if (g_silentLogged.insert(id).second)
                        logger::info("[KO] {} in bleedout again while coming to (seen by {}) - not knocked out again: {}", e.who, a_via, off);
                    return;
                }
                if (inGrace()) return;   // u2d: the bleedout grace first - every knock-out
                const std::string note = graceNote(normalNpc ? " - again, while coming to; she won her knock-out roll at the bleedout"
                                                             : " - again, while coming to");
                e.who = std::move(who);
                BeginKnockOutU(e, id, a_via, note.c_str(), potions, foll, ess, guardSever, a_calls);
                SyncCount();
                return;
            }
            if (rank == kRankDown) {
                // Rank 10 with no entry = a knockout nobody tracks (a v10 / M1 save): adopt it as M1 DOWN - knocking out
                // again would save the zeroed HealRate as hers.
                auto& e = AdoptDown(id, std::move(who));
                logger::info("[KO] {} ADOPTED as DOWN (seen by {}): rank 10 already, no entry = a v10 knockout. HealRate comes back "
                             "from her base value; ~{} game-hrs", e.who, a_via, static_cast<int>(e.recoverHours));
                return;
            }
            // 2.1.0 [WhoGetsKnockedOut]: only a NEW knock-out is asked (an NPC SBKO already holds, one coming to and a rank-10 adoption
            // were all handled above). Logged once per bleedout (g_silentLogged - under g_mtx here, so not SayNotKnockedOut).
            if (const char* off = WhoSwitchedOff(a, foll)) {
                if (g_silentLogged.insert(id).second)
                    logger::info("[KO] {} in bleedout (seen by {}) - not knocked out: {}", who, a_via, off);
                return;
            }
            if (normalNpc && verdict != BleedVerdict::kKnockOut) return;   // u6b (U5): her roll said no / not yet (PickBleedout logged it)
            if (inGrace()) return;   // u2d: the bleedout grace first - every knock-out
            const std::string note = graceNote(normalNpc ? " - she won her knock-out roll at the bleedout" : "");
            Entry e;
            e.who     = std::move(who);
            auto& ref = g_map[id] = std::move(e);
            BeginKnockOutU(ref, id, a_via, note.c_str(), potions, foll, ess, guardSever, a_calls);
            SyncCount();
        }

        // u2d: the bleedout graces that are due - Detect looks at her again (bleeding out, alive, an NPC with 3D, not taken, not a
        // brawler, not handled) and knocks her out, or says why not; a grace that ends without a knock-out is logged and forgotten.
        // Called by GraceTask (the check thread wakes at the due moment) and by every check. Call with NO lock held.
        void ProcessGrace(std::vector<Call>& a_calls)
        {
            const float             graceSecs = Settings::Get().bleedoutGraceSeconds;
            std::vector<RE::FormID> due;
            {
                std::scoped_lock lk(g_mtx);
                if (g_grace.empty()) return;
                const auto now = std::chrono::steady_clock::now();
                for (const auto& [gid, t] : g_grace)
                    if (std::chrono::duration<float>(now - t).count() >= graceSecs) due.push_back(gid);
            }
            for (const auto gid : due) {
                auto*      a     = RE::TESForm::LookupByID<RE::Actor>(gid);
                const bool dead  = a && sbko::IsDeadNow(a);
                const bool bleed = a && IsBleedingOut(a);
                if (a && bleed && !dead && a->Get3D()) Detect(a, "the end of her bleedout grace", a_calls);
                std::scoped_lock lk(g_mtx);
                if (g_grace.erase(gid)) {   // still there = Detect did not knock her out
                    const char* why = !a                ? "she is gone"
                                      : dead            ? "she died"
                                      : !bleed          ? "she left bleedout (another mod or the engine got her up)"
                                      : !a->Get3D()     ? "she has no 3D"
                                                        : "taken / declined / already handled - see the line above";
                    logger::info("[KO] {} - her bleedout grace ended without a knock-out: {}", a ? Who(a, gid) : std::format("0x{:08X}", gid),
                                 why);
                }
            }
            std::scoped_lock lk(g_mtx);
            if (!g_grace.empty()) {   // the next due moment
                auto earliest = (std::chrono::steady_clock::time_point::max)();
                for (const auto& [gid, t] : g_grace) earliest = (std::min)(earliest, t);
                ArmGraceWake(earliest + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(graceSecs)));
            }
        }

        // u2d: queued by the check thread at a grace's due moment (at most one queued). Paused / loading: the next check does it.
        void GraceTask()
        {
            g_graceQueued.store(false);
            if (!g_gameReady.load()) return;
            if (auto* ui = RE::UI::GetSingleton(); !ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;
            std::vector<Call> calls;
            ProcessGrace(calls);
            Run(calls);
        }

        // ---- replies (relayed into tasks) ----------------------------------------------------------------------------

        void OnKnockOutReply(RE::FormID a_id, std::uint64_t a_serial, bool a_ok, float a_value)
        {
            auto* a   = RE::TESForm::LookupByID<RE::Actor>(a_id);
            float cpp = a ? HealthFraction(a) : -1.0f;

            enum class Outcome { kLate, kDropped, kDown } outcome = Outcome::kLate;
            std::string who;
            float       saved = 0.0f, hours = 0.0f;
            {
                std::scoped_lock lk(g_mtx);
                auto             it = g_map.find(a_id);
                if (it != g_map.end() && it->second.serial == a_serial && it->second.state == State::kEntering) {
                    Entry& e = it->second;
                    who      = e.who;
                    if (a_ok && std::fabs(a_value + 1.0f) < 0.001f) {   // ONLY -1.0 = declined / gone (-2.0 is a value)
                        g_map.erase(it);
                        SyncCount();
                        g_declined.insert(a_id);   // m-3: not retried while she stays in bleedout
                        outcome = Outcome::kDropped;
                    } else {
                        e.state         = State::kDown;                          // v10: bBusy = false (psc:120)
                        e.savedHealRate = a_ok ? a_value : kHealRateUseBase;
                        e.koHours       = HoursNow();                            // M1c: the knock-out moment
                        e.wakeAtHours   = e.koHours + e.recoverHours;            // psc:119
                        e.downChecks    = 0;
                        saved           = e.savedHealRate;
                        hours           = e.recoverHours;
                        outcome         = Outcome::kDown;
                    }
                }
            }
            switch (outcome) {
                case Outcome::kLate:
                    logger::info("[KO] KnockOut reply for 0x{:08X} came after her entry changed (died / save loaded) - ignored", a_id);
                    return;
                case Outcome::kDropped:
                    logger::info("[KO] {} KnockOut replied -1 (gone or dead) - dropped; not retried while she stays in bleedout", who);
                    return;
                case Outcome::kDown:
                    if (!a_ok)
                        logger::warn("[KO] {} KnockOut returned no Float - her HealRate will come back from the base value", who);
                    logger::info("[KO] {} KNOCKED OUT (ragdoll, kept in bleedout) -- heal to {}% to wake, or ~{} game-hrs (saved HealRate {})",
                                 who, std::lround(Settings::Get().wakeHealthFraction * 100.0f), static_cast<int>(hours),
                                 saved >= 0.0f                      ? std::format("{:.4f}", saved)
                                 : std::fabs(saved + 2.0f) < 0.001f ? std::string("-2.0 = her BASE value on wake")
                                                                    : std::format("{:.4f}, passed to WakeUp unchanged", saved));
                    Parity("at knock-out", a_id, who, cpp);
                    return;
            }
        }

        // U1 T5: KnockOutU replied -> UNCONSCIOUS (the settle starts: iGraceChecks checks with no put-back). U2a: a heal that
        // landed while she was ENTERING (healPending) starts the heal wake now.
        void OnKnockOutUReply(RE::FormID a_id, std::uint64_t a_serial, bool a_ok, float a_value)
        {
            auto* a   = RE::TESForm::LookupByID<RE::Actor>(a_id);
            float cpp = a ? HealthFractionMax(a) : kNoRead;   // fix 1 (M-2): the U1 basis; u2c: may be negative (below 0 HP)
            TakenKind takenKind = TakenKind::kNone;
            const char* const taken = a ? TakenBy(a, takenKind) : nullptr;   // u2a fix 1: read before the lock

            enum class Outcome { kLate, kDropped, kUnc } outcome = Outcome::kLate;
            std::string       who;
            float             saved = 0.0f, hours = 0.0f;
            bool              readopt = false, orphan = false;
            bool              hitKO = false;     // U6
            float             hitPct = -1.0f;
            std::int32_t      sleepMark = 0;     // u6c
            bool              choke = false;     // u7a (U7)
            float             chokeSecs = 0.0f;
            std::vector<Call> calls;
            const auto&       cfg = Settings::Get();
            {
                std::scoped_lock lk(g_mtx);
                auto             it = g_map.find(a_id);
                if (it != g_map.end() && it->second.serial == a_serial && it->second.state == State::kEntering) {
                    Entry& e = it->second;
                    who      = e.who;
                    hitKO    = e.hitKO;
                    hitPct   = e.hitPct;
                    sleepMark = e.sleepMark;
                    choke     = e.cls == kClsChoke;
                    chokeSecs = e.chokeSeconds;
                    if (a_ok && std::fabs(a_value + 1.0f) < 0.001f) {   // ONLY -1.0 = declined / gone (-2.0 is a value)
                        GiveBackOthers(e, a_id, calls);                  // a PotionsOff / Guard that already landed
                        g_map.erase(it);
                        SyncCount();
                        g_declined.insert(a_id);
                        outcome = Outcome::kDropped;
                    } else {
                        e.state         = State::kUnconscious;
                        e.savedHealRate = a_ok ? a_value : kHealRateUseBase;
                        e.koHours       = HoursNow();
                        e.wakeAtHours   = e.koHours + e.recoverHours;
                        e.downChecks    = 0;
                        e.lockFrac      = -1.0f;
                        readopt         = e.readopted;
                        orphan          = e.orphan;
                        saved           = e.savedHealRate;
                        hours           = e.recoverHours;
                        outcome         = Outcome::kUnc;
                        if (e.healPending && cfg.healWake && !taken) {
                            BeginHealWake(e, a_id, "a heal landed while she was being knocked out", cpp, calls);
                            SyncCount();
                        } else if (e.sleepWakePending && !taken) {   // u6f: a hit / talk while she was still falling asleep
                            BeginSleepWake(e, a_id, cpp, calls);
                            SyncCount();
                        }
                    }
                }
            }
            if (outcome == Outcome::kUnc) {
                if (!a_ok)
                    logger::warn("[KO] {} KnockOutU returned no Float - her HealRate will come back from the base value", who);
                // U6: a hit knock-out holds her Health at the knock-out - no fUnconsciousHealthFraction cap
                const std::string holdRule =
                    hitKO ? std::format("her Health at the knock-out ({}; a HIT knock-out - no {}% cap; never above her Health then)",
                                        hitPct >= 0.0f ? Pct1(hitPct) : std::string("not known - her Health then"),
                                        std::lround(cfg.unconsciousHealthFraction * 100.0f))
                          : std::format("min({}%, her Health then)", std::lround(cfg.unconsciousHealthFraction * 100.0f));
                logger::info("[KO] {} {} (unconscious, ragdoll) HP {} -> settle ({} checks x {} s), then Health held at "
                             "{}; wakes on the first heal{} or in ~{} game-hrs; saved HealRate {}",
                             who, orphan ? "ORPHAN ADOPTED" : readopt ? "RE-ADOPTED"
                                  : sleepMark > 0 ? (sleepMark == kSleepSeen ? "PUT TO SLEEP BY THE SLEEP SPELL (she saw you)"
                                                                             : "PUT TO SLEEP BY THE SLEEP SPELL (she did not see you)")
                                  : choke ? std::format("CHOKED UNCONSCIOUS - taken over from VRTE (held {:.1f} s)", chokeSecs)
                                  : hitKO ? "KNOCKED OUT BY A HIT" : "KNOCKED OUT",
                             cpp > kNoRead ? Pct1(cpp) : std::string("not read"), cfg.graceChecks,
                             cfg.checkSeconds,
                             holdRule,
                             !cfg.healWake ? " (bHealWake 0: NO heal wake)" : " (straight up)",
                             static_cast<int>(hours),
                             saved >= 0.0f                      ? std::format("{:.4f}", saved)
                             : std::fabs(saved + 2.0f) < 0.001f ? std::string("-2.0 = her BASE value on wake")
                                                                : std::format("{:.4f}, passed to FinishWake unchanged", saved));
            }
            Run(calls);
            switch (outcome) {
                case Outcome::kLate:
                    logger::info("[KO] KnockOutU reply for 0x{:08X} came after her entry changed (died / save loaded) - ignored", a_id);
                    return;
                case Outcome::kDropped:
                    // u6 fix 1 (F7): worded by kind - a hit knock-out was never in bleedout
                    if (hitKO)
                        logger::info("[KO] {} KnockOutU (hit knock-out) replied -1 (gone or dead) - dropped; a later qualifying hit after "
                                     "the next check may try again", who);
                    else
                        logger::info("[KO] {} KnockOutU replied -1 (gone or dead) - dropped; not retried while she stays in bleedout", who);
                    return;
                case Outcome::kUnc:
                    // u6d fix 2: the Sleep spell's ONE HUD line, only now that the knock-out really happened (a
                    // KnockOutU that declined - she died meanwhile - never gets here). SetSleepMark writes the same rank again (idempotent).
                    // u6f: not for a RE-ADOPTED sleeper (her sleepMark is carried now - she did not just fall asleep)
                    if (sleepMark > 0 && !readopt && !orphan && g_sleepFaction && cfg.notify && !Helper::SetSleepMark(a_id, sleepMark, true))
                        logger::warn("[KO] {} SetSleepMark (the 'falls into a deep sleep.' line) was NOT dispatched", who);
                    Parity("at knock-out", a_id, who, cpp);
                    return;
            }
        }

        // U1: PotionsOff replied. Still the same knock-out and she is still held -> the mask is kept (OR'd: the re-check may
        // take one off again); otherwise (she woke / died / was dropped meanwhile) the abilities go back at once.
        void OnPotionsOffReply(RE::FormID a_id, std::uint64_t a_episode, bool a_ok, std::int32_t a_mask)
        {
            bool        back = false;
            std::string who  = std::format("0x{:08X}", a_id);
            std::int32_t total = 0;
            {
                std::scoped_lock lk(g_mtx);
                auto             it   = g_map.find(a_id);
                const bool       held = it != g_map.end() && it->second.episode == a_episode && Held(it->second.state);
                if (held) {
                    Entry& e          = it->second;
                    who               = e.who;
                    e.potionsInFlight = false;
                    if (a_ok && a_mask > 0) e.potionMask = (e.potionMask < 0 ? 0 : e.potionMask) | a_mask;
                    total = e.potionMask;
                } else {
                    if (it != g_map.end()) who = it->second.who;
                    back = a_ok && a_mask > 0;
                }
            }
            if (!a_ok) {
                logger::warn("[KO] {} PotionsOff returned no Int - nothing is known to give back", who);
                return;
            }
            if (back) {
                logger::info("[KO] {} potions OFF (mask {}) landed after she stopped being held - given back at once", who, a_mask);
                if (!Helper::PotionsBack(a_id, a_mask)) logger::warn("[KO] {} PotionsBack (mask {}) was NOT dispatched", who, a_mask);
                return;
            }
            logger::info("[KO] {} potions OFF: mask {} removed now (Smart NPC Potions: 1 base, 2 Mage, 4 Assassin); given back at the "
                         "wake: mask {}", who, a_mask, total);
        }

        // U1: SBKO_SeverCompat.Guard replied.
        void OnGuardReply(RE::FormID a_id, std::uint64_t a_episode, bool a_ok, std::int32_t a_packed)
        {
            bool        undo = false;
            std::string who  = std::format("0x{:08X}", a_id);
            {
                std::scoped_lock lk(g_mtx);
                auto             it   = g_map.find(a_id);
                const bool       held = it != g_map.end() && it->second.episode == a_episode && Held(it->second.state);
                if (held) {
                    Entry& e        = it->second;
                    who             = e.who;
                    e.severInFlight = false;
                    if (a_ok && a_packed > 0) {
                        e.severExcluded = true;
                        e.severPacked   = a_packed;
                    }
                } else {
                    if (it != g_map.end()) who = it->second.who;
                    undo = a_ok && a_packed > 0;
                }
            }
            if (!a_ok) {
                logger::warn("[KO] {} SeverActions guard: no answer from Guard - unknown whether she was excluded", who);
                return;
            }
            if (undo) {
                logger::info("[KO] {} SeverActions guard landed after she stopped being held - included again at once", who);
                std::vector<Call> calls{ Call{ .kind = Call::Kind::kSeverUnguard, .id = a_id, .who = who, .packed = a_packed } };
                Run(calls);
                return;
            }
            if (a_packed > 0)
                logger::info("[KO] {} SeverActions guard: EXCLUDED from survival tracking while she is out (needs kept for the wake: {})",
                             who, SeverNeeds(a_packed));
            else if (a_packed == 0)
                logger::info("[KO] {} SeverActions guard: she was ALREADY excluded by the user - SBKO touches nothing (now or at the "
                             "wake)", who);
            else if (a_packed == -2)
                logger::info("[KO] {} SeverActions guard: SeverActions survival is switched OFF - nothing to guard", who);
            else if (a_packed == -3)
                logger::info("[KO] {} SeverActions guard: SeverActions does not track her (not a teammate, not on its roster or its "
                             "survival list) - nothing done", who);
            else
                logger::warn("[KO] {} SeverActions guard: Guard returned {} (no actor / no survival quest) - nothing done", who, a_packed);
        }

        void OnHoldHealthReply(RE::FormID a_id, std::uint64_t a_episode, bool a_ok, float a_hp)
        {
            std::string   who      = std::format("0x{:08X}", a_id);
            std::uint32_t putBacks = 0;
            {
                std::scoped_lock lk(g_mtx);
                if (auto it = g_map.find(a_id); it != g_map.end() && it->second.episode == a_episode) {
                    it->second.healthInFlight = false;
                    who                       = it->second.who;
                    putBacks                  = it->second.putBacks;
                }
            }
            // u2c P2: the reply is logged with the knock-out's first put-back only (bVerboseChecks 1: every one); the rest are
            // counted in the PERF line
            if (a_ok && a_hp >= 0.0f && putBacks > 1 && !Settings::Get().verboseChecks) return;
            if (a_ok && a_hp >= 0.0f) {
                float mx = 0.0f;
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id)) HealthFractionMax(a, nullptr, &mx);
                logger::info("[KO] {} put-back done: HP now {:.1f} = {} of max {:.1f}", who, a_hp,
                             mx > 0.0f ? Pct1(a_hp / mx) : std::string("?"), mx);
            } else
                logger::warn("[KO] {} put-back: HoldHealthTo found no living actor", who);
        }

        void OnHoldDownDone(RE::FormID a_id, std::uint64_t a_episode)
        {
            std::scoped_lock lk(g_mtx);
            if (auto it = g_map.find(a_id); it != g_map.end() && it->second.episode == a_episode) it->second.holdInFlight = false;
        }

        // u2a fix 1: LetGo returned - forget her as held; a kCaptive / kVrte NPC stays watched (g_released), a kBag one is belted.
        // serial 0 = the deferred give-back of a VRTE release (VRTE woke her).
        void OnLetGoDone(RE::FormID a_id, std::uint64_t a_serial)
        {
            std::string who = std::format("0x{:08X}", a_id);
            const char* what = nullptr;
            {
                std::scoped_lock lk(g_mtx);
                if (a_serial == 0) {
                    if (auto it = g_released.find(a_id); it != g_released.end() && it->second.givingBack) {
                        g_released.erase(it);
                        what = "VRTE's release: SBKO's part given back (HealRate, potions, SeverActions) - no longer watched";
                    }
                } else if (auto it = g_map.find(a_id); it != g_map.end() && it->second.serial == a_serial && it->second.releasing) {
                    Entry&          e    = it->second;
                    const TakenKind kind = static_cast<TakenKind>(e.releaseKind);
                    who                  = e.who;
                    if (kind == TakenKind::kBag) {
                        // EVERY bag LetGo leaves the belt: the wake runs again when she is loaded, still unconscious
                        // and out of the sack
                        Pending p;
                        p.healRate   = e.savedHealRate;
                        p.unc        = true;
                        p.potionMask = 0;
                        p.belt       = true;
                        g_pendingConscious[a_id] = p;
                        what = "forgotten; a one-shot belt wakes her again if she comes back still unconscious";
                    } else {
                        Released r;
                        r.healRate      = e.savedHealRate;
                        r.deferred      = kind == TakenKind::kVrte;
                        r.potionMask    = r.deferred ? e.potionMask : -1;
                        r.severExcluded = r.deferred && e.severExcluded;
                        r.severPacked   = r.severExcluded ? e.severPacked : 0;
                        r.hitKO         = e.hitKO;   // u6 fix 1 (F1): a re-adoption keeps the hit knock-out's no-cap hold
                        r.sleepMark     = e.sleepMark;   // u6f: and a sleeper stays a sleeper
                        r.choke         = e.cls == kClsChoke;   // u7a (U7): and a choke a choke
                        r.chokeSeconds  = e.chokeSeconds;
                        r.chokeHours    = e.chokeHours;
                        g_released[a_id] = r;
                        what = r.deferred ? "released to VRTE; SBKO's part comes back when VRTE wakes her (watched)"
                                          : "released; watched - re-adopted if she is given back still unconscious";
                    }
                    g_declined.insert(a_id);   // the scan never knocks her out while she stays in bleedout (e.g. mid-capture)
                    g_map.erase(it);
                    SyncCount();
                }
            }
            if (what) logger::info("[KO] {} LetGo done - {}", who, what);
        }

        void OnWakeDone(RE::FormID a_id, std::uint64_t a_serial, std::int32_t a_narrate)
        {
            std::string who;
            bool        unc  = false;
            bool        wokeByPlayer = false;   // u7a (M2)
            {
                std::scoped_lock lk(g_mtx);
                auto             it = g_map.find(a_id);
                if (it == g_map.end() || it->second.serial != a_serial || it->second.state != State::kComingTo) return;
                wokeByPlayer            = it->second.wokeByPlayer && it->second.sleepMark > 0;
                it->second.awaitingWake = false;
                it->second.leftoverWake = false;
                it->second.wakeDoneAt   = std::chrono::steady_clock::now();
                who                     = it->second.who;
                unc                     = it->second.unc;
                if (unc) it->second.potionMask = -1;   // U1: FinishWake gave them back
            }
            logger::info("[KO] {} {} finished (coming to: stage {})", who,
                         !unc ? "WakeUp" : "FinishWake", kRankComingTo);
            {   // u2c fix 6: the parity line AFTER the wake (at dispatch it raced FinishWake's +50)
                auto*       aw  = RE::TESForm::LookupByID<RE::Actor>(a_id);
                const float now = aw ? HealthFractionMax(aw) : kNoRead;
                Parity("after the wake", a_id, who, now);
            }
            const bool notify = Settings::Get().notify;
            if (a_narrate > 0) {   // M1c: a timer wake - narrated after the wake, so she is already up when it is told
                const bool narrate = Helper::SkyrimNetPresent();
                const bool sent    = Helper::NarrateWake(a_id, a_narrate, notify, narrate);
                logger::info("[KO] {} NarrateWake(hours {}, notify {}, narrate {}) {}", who, a_narrate, notify ? 1 : 0,
                             narrate ? 1 : 0, sent ? "dispatched" : "NOT dispatched");
            } else if (a_narrate == kNarrateHeal && wokeByPlayer && Settings::Get().wokenNarration) {
                // u7a (M2): the player woke this Sleep-spell sleeper with Diary of Mine's Wake - "<player> has just woken <NPC> from a deep
                // sleep." (she answers the player), instead of "<NPC> wakes up from a deep sleep."
                const bool narrate = Helper::SkyrimNetPresent();
                const bool sent    = Helper::NarrateWoken(a_id, kWokenSpell, 1, notify, narrate);
                logger::info("[KO] {} NarrateWoken(kind {} = a Sleep-spell sleeper woken by the player, notify {}, narrate {}) {}", who, kWokenSpell,
                             notify ? 1 : 0, narrate ? 1 : 0, sent ? "dispatched" : "NOT dispatched");
            } else if (a_narrate == kNarrateHeal) {
                const bool narrate = Helper::SkyrimNetPresent();
                const bool sent    = Helper::NarrateHealWake(a_id, notify, narrate);
                logger::info("[KO] {} NarrateHealWake(notify {}, narrate {}) {}", who, notify ? 1 : 0, narrate ? 1 : 0,
                             sent ? "dispatched" : "NOT dispatched");
            }
        }

        // ---- relayed sink work -----------------------------------------------------------------------------------------

        void OnBleedoutTask(RE::FormID a_id, std::uint32_t a_gen, std::uint32_t a_n)
        {
            // u2c P4: the event's log lines are written here, not in the sink (copy + relay only there)
            if (a_n == 1)
                logger::info("[KO] FIRST TESEnterBleedoutEvent of the session arrived (0x{:08X}) - the engine event reaches SBKO", a_id);
            else if (Settings::Get().verboseChecks)
                logger::info("[KO] TESEnterBleedoutEvent #{} (0x{:08X})", a_n, a_id);
            {
                std::scoped_lock lk(g_mtx);
                g_silentLogged.erase(a_id);   // u2c fix 6: a new bleedout - its reason (if any) is logged once again
            }
            if (a_gen != g_generation.load() || !g_gameReady.load()) return;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id);
            if (!a) return;
            std::vector<Call> calls;
            Detect(a, "TESEnterBleedoutEvent", calls);
            Run(calls);
        }

        // v10 OnDying (psc:86-91): rank 0, forget her - nothing restored. Acts on ANY stage of the event (PPB: the
        // dead=true stage never arrived in VR). U1: another mod's state she carries goes back (GiveBackOthers).
        void OnDeathTask(RE::FormID a_id, bool a_dead, std::uint32_t a_gen)
        {
            if (a_gen != g_generation.load()) return;
            // u6h (G3): act ONLY if she is still dead when this task runs. A death that was cancelled or undone
            // (resurrected) between the event and now must not leave her unconscious + Paralysis 1 at rank 0 with no record - she is
            // kept as she is held. Not in memory = treated as dead (as before). (A real death the task misses is still caught by the
            // check: dead + 3D -> ClearMark, dropped.)
            auto* const       a       = RE::TESForm::LookupByID<RE::Actor>(a_id);
            const bool        deadNow = sbko::IsDeadNow(a);
            const int         life    = (a && a->AsActorState()) ? static_cast<int>(a->AsActorState()->GetLifeState()) : -1;
            if (deadNow) Marks::OnDeath(a_id);   // m34: the marks stay on the body, her marks record goes (no lock of ours held)
            if (deadNow) CloseOurLoot(a_id, "died");   // 2.1.0: the living-loot menu closes; her corpse is the game's own loot after
            std::string       who;
            State             was = State::kDown;
            std::vector<Call> calls;
            {
                std::scoped_lock lk(g_mtx);
                if (deadNow) {
                    g_adoptCandidates.erase(a_id);
                    g_adoptAtLoad.erase(a_id);   // u6h
                    g_chokeLoose.erase(a_id);    // u7a (U7): no neck mark on a corpse
                    g_chokeLooseCount.store(g_chokeLoose.size());
                    g_woken.erase(a_id);         // u7a (M2)
                }
                auto it = g_map.find(a_id);
                if (it == g_map.end()) return;
                who = it->second.who;
                was = it->second.state;
                if (!deadNow) {
                    // u6h (G3): keep holding her. A flag the cancelled death cleared is re-asserted QUIETLY for 2 checks (the engine-reset
                    // rule of ExternalClear - not "someone tried to wake her"); after that, the usual rules (heal / timer / sleep wake)
                    Entry& e = it->second;
                    if (e.state == State::kUnconscious) {
                        e.resetActive = true;
                        e.checksSince = 0;
                        e.since       = "a cancelled death";
                    }
                } else {
                    GiveBackOthers(it->second, a_id, calls);
                    g_map.erase(it);
                    SyncCount();
                }
            }
            if (!deadNow) {
                logger::info("[KO] {} TESDeathEvent (dead={}) - but she is NOT dead when SBKO looks (life state {}: the death was cancelled or "
                             "undone) - SBKO keeps holding her ({}): no ClearMark, nothing given back; {}",
                             who, a_dead, life, StateName(was),
                             was == State::kUnconscious ? "a cleared unconscious flag / Paralysis is re-asserted quietly for 2 checks, then the "
                                                          "usual rules (a heal or the timer wakes her)"
                             : was == State::kComingTo  ? "she goes on coming to"
                                                        : "the next steps go on as usual");
                return;
            }
            logger::info("[KO] {} died (TESDeathEvent, dead={}) while {} - ClearMark: rank 0, NoBleedoutRecovery off; dropped (v10 "
                         "OnDying){}", who, a_dead, StateName(was), calls.empty() ? "" : "; another mod's state (potions / SeverActions) back");
            if (!Helper::ClearMark(a_id, 0))   // u2c fix 4: nothing of SBKO's stays on her
                logger::warn("[KO] {} ClearMark (rank 0, NoBleedoutRecovery off) was NOT dispatched", who);
            Run(calls);
        }

        // U1 (bLogEffects) / U2a: a magic effect landed on an NPC SBKO holds. U2a: the first HEAL from an Actor is THE WAKE
        // TRIGGER; log hygiene - each mgef is logged fully ONCE per knock-out (the trigger heal too), repeats are only counted.
        void OnEffectTask(RE::FormID a_tid, RE::FormID a_cid, RE::FormID a_mgef, std::uint32_t a_gen)
        {
            if (a_gen != g_generation.load() || !g_gameReady.load()) return;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_tid);
            if (!a) return;
            auto*      m       = RE::TESForm::LookupByID<RE::EffectSetting>(a_mgef);
            const bool heal    = IsHeal(m);
            const bool ability = m && m->data.castingType == RE::MagicSystem::CastingType::kConstantEffect;   // never a heal (IsHeal)
            if (ability) return;   // u2c fix round 1: the sink drops these already (a belt - before any string / TakenBy work)
            const std::string name = m ? (m->GetName() && *m->GetName() ? std::string(m->GetName()) : std::string("(no name)"))
                                       : std::string("(not an effect)");
            bool        casterActor = false;
            std::string caster      = "(none)";
            if (a_cid == a_tid) {
                casterActor = true;   // herself - she cannot drink while out, so this is a FED potion (e.g. Gift by Hand's)
                caster      = "herself";
            } else if (a_cid != 0) {
                auto* cf    = RE::TESForm::LookupByID(a_cid);
                auto* ca    = cf ? cf->As<RE::Actor>() : nullptr;
                casterActor = ca != nullptr;
                caster      = ca ? Who(ca, a_cid)
                                 : std::format("{} (0x{:08X}, not an actor)", cf && cf->GetName() && *cf->GetName() ? cf->GetName() : "?", a_cid);
            }
            const bool  trigger = heal && casterActor;
            const auto& cfg     = Settings::Get();
            const float pct     = HealthFractionMax(a);
            TakenKind         takenKind = TakenKind::kNone;
            const char* const taken     = TakenBy(a, takenKind);   // u2a fix 1 (m-2): live, not a cached flag
            bool              logFull = false;
            std::string       who, state, action;
            std::uint32_t     seen = 0;
            std::vector<Call> calls;
            {
                std::scoped_lock lk(g_mtx);
                auto             it = g_map.find(a_tid);
                if (it == g_map.end() || it->second.state == State::kComingTo) return;
                Entry& e = it->second;
                who      = e.who;
                state    = StateName(e.state);
                ++e.effectsSinceCheck;
                auto [k, isNew] = e.kinds.try_emplace(a_mgef, 0u);
                seen            = ++k->second;
                if (isNew) ++e.newKindsSinceCheck;
                if (ability) ++e.abilityApplies;   // u2c P2: summarised at the next check, never one line each
                if (heal) {
                    ++e.healsSinceCheck;
                    ++e.healsTotal;
                }
                e.lastEffect = std::format("{} (mgef 0x{:08X}) from {} -> {}", name, a_mgef, caster, heal ? "HEAL" : "not a heal");
                if (trigger) {
                    logFull         = !e.triggerLogged;
                    e.triggerLogged = true;
                    if (!cfg.healWake)
                        action = "bHealWake 0 - logged only";
                    else if (e.releasing || taken)
                        action = std::format("taken by another mod ({}) - no SBKO wake", taken ? taken : "SBKO is letting go");
                    else if (e.state == State::kEntering) {
                        if (!e.healPending) {
                            e.healPending = true;
                            e.healTrigger = e.lastEffect;
                            action        = "THE WAKE TRIGGER - acted on when KnockOutU replies";
                        }
                    } else if (e.state == State::kUnconscious) {
                        if (!e.healPending) {
                            e.healPending = true;
                            e.healTrigger = e.lastEffect;
                            if (e.holdInFlight || e.healthInFlight)
                                action = std::format("THE WAKE TRIGGER - the wake waits for the running {} (next check)",
                                                     e.holdInFlight ? "HoldDown" : "HoldHealthTo");
                            else {
                                action = "THE WAKE TRIGGER - the wake starts";
                                BeginHealWake(e, a_tid, "a heal", pct, calls);
                                SyncCount();
                            }
                        }
                    } else if (e.state == State::kDown)
                        action = "an old M1 knock-out - M1's heal rule (Health >= fWakeHealthFraction) applies";
                    else
                        action = "she is already waking - counted";
                } else {
                    logFull = isNew && !ability;   // u2c P2: a 3D load re-applies every ability - no 50-line burst
                    if (heal && !casterActor) action = "a heal-type effect from a non-actor caster - never a wake";
                }
            }
            Run(calls);
            if (!logFull || !cfg.logEffects) return;   // u2a fix 1 (m-1): bLogEffects 0 silences the EFFECT lines
            std::string dur = "?", mag = "?", src = "?";
            if (auto* mt = a->AsMagicTarget()) {
                FindEffect v(a_mgef);
                mt->VisitEffects(v);
                if (v.found) {
                    dur = std::format("{:.1f}s", v.dur);
                    mag = std::format("{:.1f}", v.mag);
                    src = v.item ? std::format("{} (0x{:08X})", v.item->GetName() && *v.item->GetName() ? v.item->GetName() : "?",
                                               v.item->GetFormID())
                                 : std::string("?");
                } else {
                    dur = "gone (already finished when read)";
                }
            }
            std::string detail = m ? std::format("archetype={} av={} flags={} casting={}",
                                                 m->GetArchetypeAsString().c_str(), AvName(m->data.primaryAV), EffectFlags(m),
                                                 CastingName(m->data.castingType))
                                   : std::string("archetype=?");
            logger::info("[KO] EFFECT on {} ({}): mgef=0x{:08X} {} caster={} {} dur={} mag={} src={} -> {}{}", who, state, a_mgef, name,
                         caster, detail, dur, mag, src, heal ? "HEAL" : "not a heal",
                         !action.empty() ? " | " + action
                         : seen == 1     ? std::string(" (first time this knock-out; repeats are counted on the check line)")
                                         : std::string());
        }

        // u6h: what ONE run of a relayed task did - the console tests ('sbko headko / sleep / roll / hitwake', TestCmd.cpp) call the very
        // same task function and print this as their one line; the event sinks' relays ignore it.
        struct TaskResult
        {
            bool        acted  = false;   // knocked out / put to sleep / woken / a wake queued
            std::string what;             // the verdict ("KNOCKED OUT by ...", "no: <why>", ...)
            float       chance = -1.0f;   // U5: the chance rolled against (-1 = no roll)
            float       roll   = -1.0f;   // U5: the roll (-1 = none)
            bool        won    = false;   // U5: the roll was won (applied or not)
            std::string wonBut;           // u6h fix 1: U5 - won, but not knocked out: why (SBKO's lists)
        };

        // ---- U6: the head-hit / sneak knock-out -----------------------------------------------------------------------------
        // What the hit sink copies - plain values only (PLANCK's extended block is valid only during the sink call).
        struct HitCopy
        {
            const char*   test       = nullptr; // u6h: a console test's label (a string literal) - nullptr = a real hit from the sink
            RE::FormID    target     = 0;
            RE::FormID    source     = 0;       // the weapon / shield / torch / spell form of the hit (TESHitEvent::source)
            RE::FormID    projectile = 0;
            std::uint32_t flags      = 0;       // the 4-byte word at +0x18: low byte = TESHitEvent::Flag, top 3 bytes = PLANCK's magic
            bool          planck     = false;   // PLANCK's extended hit event (the player's physical melee hit in VR)
            bool          head       = false;   // PLANCK's hit node is NPC Head [Head], or one of its first kHeadParents parents is
            int           headUp     = -1;      // 0 = the node itself, 1..kHeadParents = that parent; -1 = not the head
            bool          isLeft     = false;
            bool          magicNoMatch = false; // u6 fix 1 (F5): the magic word was there but PLANCK's current event was another one
            bool          magicNoPlanck = false; // u6 fix 3: the magic word was there but PLANCK is not loaded - never read
            bool          hasDamage  = false;    // u6e: PLANCK's HitData was read (a PLANCK hit only)
            float         hitDamage  = 0.0f;     // u6e: its totalDamage (HitData +0x50) - this hit's damage before resistances are applied
            RE::NiPoint3  pos{};                // PLANCK's contact point (world units)
            std::string   node;                 // PLANCK's hit node name ("" = none: no ragdoll body was hit)
            std::uint32_t gen        = 0;
        };

        // ActorTypeGhost on the base or the race (IsNPC's own test; Knockout's listener filter: never a ghost)
        bool IsGhost(RE::Actor* a_actor)
        {
            if (!a_actor || !g_kwGhost) return false;
            const auto* npc  = a_actor->GetActorBase();
            const auto* race = a_actor->GetRace();
            return (npc && npc->HasKeyword(g_kwGhost)) || (race && race->HasKeyword(g_kwGhost));
        }

        // u6d fix round 2: a CHILD (her race's Child flag - the data Actor::IsChild reads; a field read) or a RIDER on
        // a mount (Actor::IsOnMount: not a mount + ExtraInteraction) is never knocked out by U5, U6 or the Sleep spell. nullptr = neither.
        const char* ChildOrRider(RE::Actor* a_actor)
        {
            if (!a_actor) return nullptr;
            const auto* race = a_actor->GetRace();
            if (race && race->data.flags.all(RE::RACE_DATA::Flag::kChild)) return "a child";
            if (a_actor->IsOnMount()) return "riding a mount";
            return nullptr;
        }

        // R7: a brawl or the Companions' fist-fight is running (Knockout's cloak conditions, which block every knock-out then):
        // DGIntimidateQuest running (its kEnabled flag - the toolbox's TESQuest::IsRunning is NOT used: it answers true for a
        // stopped quest), C00 at stage 20, C00JorrvaskrFight at stage 1. Field reads only.
        const char* FistFightRunning()
        {
            if (g_questBrawl && g_questBrawl->IsEnabled()) return "a brawl is running (DGIntimidateQuest)";
            if (g_questC00 && g_questC00->GetCurrentStageID() == kC00FightStage) return "the Companions' fist-fight is on (C00 stage 20)";
            if (g_questC00Fight && g_questC00Fight->GetCurrentStageID() == kC00JorrvaskrStage)
                return "the Companions' fist-fight is on (C00JorrvaskrFight stage 1)";
            return nullptr;
        }

        // ---- u6f: HELMET RULE A (replaces the ArmorHelmet keyword test of R5 and of U5's formula) ----
        // ONE worn piece is a HELMET when it covers biped slot 30 (head) or 31 (hair) AND its armour type is Light or Heavy
        // (BGSBipedObjectForm::HasPartOf / IsLightArmor / IsHeavyArmor - field reads). Clothing (hoods, circlets, cloth wigs, capes) never
        // counts; a piece off slots 30 / 31 never counts (a cape on slot 46 carrying ArmorHelmet included); a Devious Devices item
        // (zad_Lockable / zad_InventoryDevice) is skipped. The order of the tests gives the log's reason (slot first, then type).
        // u6g: then a piece DEFINED by a plugin on [HeadHit] sHelmetIgnorePlugins (the wig / hair plugins) is not a helmet either.
        enum class HelmetWhy : std::uint8_t
        {
            kHelmet      = 0,
            kNotHeadSlot = 1,   // covers neither slot 30 nor 31
            kClothing    = 2,   // slot 30 / 31, but Clothing
            kDevice      = 3,   // a Devious Devices item
            kIgnoredPlugin = 4, // u6g: slot 30 / 31 + Light / Heavy, but its originating plugin is on sHelmetIgnorePlugins
        };

        // u6g: the plugin that DEFINES the form (sourceFiles[0]; TESForm.h GetFile, field read - the same call LightFileOf uses), or null
        const RE::TESFile* OriginFile(const RE::TESForm* a_form)
        {
            return a_form ? a_form->GetFile(0) : nullptr;
        }

        // u6g: is this file on [HeadHit] sHelmetIgnorePlugins? (case-insensitive file-name compare; the list is read-only after load)
        bool IsHelmetIgnoredFile(const RE::TESFile* a_file)
        {
            if (!a_file) return false;
            const std::string_view fn = a_file->GetFilename();
            for (const auto& n : Settings::Get().helmetIgnorePlugins) {
                if (n.size() == fn.size() && _strnicmp(n.data(), fn.data(), fn.size()) == 0) return true;
            }
            return false;
        }

        HelmetWhy HelmetRuleA(const RE::TESObjectARMO* a_armo)
        {
            if ((g_kwDDLockable && a_armo->HasKeyword(g_kwDDLockable)) || (g_kwDDInventory && a_armo->HasKeyword(g_kwDDInventory)))
                return HelmetWhy::kDevice;
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            if (!a_armo->HasPartOf(Slot::kHead) && !a_armo->HasPartOf(Slot::kHair)) return HelmetWhy::kNotHeadSlot;
            if (!a_armo->IsLightArmor() && !a_armo->IsHeavyArmor()) return HelmetWhy::kClothing;
            if (IsHelmetIgnoredFile(OriginFile(a_armo))) return HelmetWhy::kIgnoredPlugin;   // u6g
            return HelmetWhy::kHelmet;
        }

        std::string ArmoName(const RE::TESObjectARMO* a_armo)
        {
            const char* n = a_armo->GetName();
            return std::format("{} 0x{:08X}", n && *n ? n : "(no name)", a_armo->GetFormID());
        }

        // the log's text for one piece: a helmet -> "Name 0x.. (heavy armour, slot 30 head + 31 hair)"; a piece that did NOT count -> its
        // reason, named only when it is "head-ish" (covers slot 30 / 31, or carries ArmorHelmet) - boots or a body armour are never noise
        std::string HelmetText(const RE::TESObjectARMO* a_armo, HelmetWhy a_why)
        {
            using Slot       = RE::BGSBipedObjectForm::BipedObjectSlot;
            const bool head  = a_armo->HasPartOf(Slot::kHead);
            const bool hair  = a_armo->HasPartOf(Slot::kHair);
            const bool kwHel = a_armo->HasKeywordID(kArmorHelmet);
            switch (a_why) {
                case HelmetWhy::kHelmet:
                    return std::format("{} ({} armour, slot {})", ArmoName(a_armo), a_armo->IsHeavyArmor() ? "heavy" : "light",
                                       head && hair ? "30 head + 31 hair" : head ? "30 head" : "31 hair");
                case HelmetWhy::kNotHeadSlot:
                    return kwHel ? std::format("{}: not a head / hair slot (its ArmorHelmet keyword is ignored)", ArmoName(a_armo))
                                 : std::string();
                case HelmetWhy::kClothing:
                    return std::format("{}: clothing (slot {}{})", ArmoName(a_armo), head ? "30 head" : "31 hair",
                                       kwHel ? ", its ArmorHelmet keyword is ignored" : "");
                case HelmetWhy::kDevice:
                    return head || hair || kwHel ? std::format("{}: a Devious Devices item", ArmoName(a_armo)) : std::string();
                case HelmetWhy::kIgnoredPlugin: {   // u6g
                    const auto* f = OriginFile(a_armo);
                    return std::format("{}: a listed hair / wig plugin ({})", ArmoName(a_armo),
                                       f ? std::string(f->GetFilename()) : std::string("?"));
                }
            }
            return {};
        }

        // R5 (u6f: rule A): her worn HELMET. One walk of her inventory changes (worn items always live there) - never an inventory copy
        // (Actor::GetWornArmor deep-copies) and never an inventory initialised from here (noInit). a_what = the helmet; a_notOnes = the
        // head-ish pieces that did NOT count and why ("" = none), for the log.
        bool WornHelmet(RE::Actor* a_actor, std::string& a_what, std::string& a_notOnes)
        {
            auto* changes = a_actor ? a_actor->GetInventoryChanges(true) : nullptr;
            if (!changes || !changes->entryList) return false;
            bool found = false;
            for (const auto* entry : *changes->entryList) {
                if (!entry || !entry->object || !entry->object->IsArmor() || !entry->IsWorn()) continue;
                const auto* armo = entry->object->As<RE::TESObjectARMO>();
                if (!armo) continue;
                const HelmetWhy why = HelmetRuleA(armo);
                if (why == HelmetWhy::kHelmet) {
                    if (!found) a_what = HelmetText(armo, why);
                    found = true;
                } else if (std::string t = HelmetText(armo, why); !t.empty()) {
                    a_notOnes += (a_notOnes.empty() ? "" : "; ") + t;
                }
            }
            return found;
        }

        // the [headhit] / [chanceko] helmet text: "YES: <helmet>" / "no", + the pieces that did not count
        std::string HelmetLogText(bool a_helmet, const std::string& a_what, const std::string& a_notOnes, const char* a_yes,
                                  const char* a_no)
        {
            std::string s = a_helmet ? std::format("{} {}", a_yes, a_what) : std::string(a_no);
            if (!a_notOnes.empty()) s += std::format(" (not a helmet: {})", a_notOnes);
            return s;
        }

        const char* ToyWeapon(const RE::TESObjectWEAP* a_w);   // u6d (F1) / u6e - defined with the U5 code below

        // U6 / u6b U5: why SBKO's OWN lists stop a hit knock-out ("" = they do not). Caller holds g_mtx.
        std::string HeldWhy(RE::FormID id)
        {
            const auto it = g_map.find(id);
            if (g_pendingConscious.contains(id)) return "on SBKO's load-safety list (she is made conscious first)";
            if (g_released.contains(id)) return "on SBKO's released watch (another mod had her)";
            if (g_declined.contains(id)) return "declined / let go by SBKO earlier (until a check sees her out of bleedout)";
            if (it != g_map.end() && it->second.releasing) return "SBKO is letting her go to another mod";
            if (it != g_map.end() && it->second.state != State::kComingTo)
                return std::format("already held by SBKO ({})", StateName(it->second.state));
            if (it != g_map.end() && it->second.awaitingWake) return "SBKO's wake is still running on her";
            return {};
        }

        // U6: ONE player hit on an actor, in an SKSE task (relayed by HitSink; in VR that task runs on an engine JOB thread inside SKSE's
        // queue lock, not on the main thread). Every engine read happens with no lock of ours; g_mtx is taken for a read of
        // SBKO's own lists and again (re-checked) for the knock-out; detection is the LAST test (u6 fix 1, F3); the lines are written
        // after the lock, then Run(). Rules R1-R8 of the design.
        TaskResult OnHitTask(const HitCopy& h)
        {
            if (h.gen != g_generation.load() || !g_gameReady.load()) return { false, "no: the game is loading" };
            auto* a      = RE::TESForm::LookupByID<RE::Actor>(h.target);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!a || !player || a->IsPlayerRef()) return { false, "no: no such NPC" };
            const auto& cfg = Settings::Get();

            // ---- the weapon kind (R2 / R3). u6 fix round 2 (no punch knock-out - punches are an issue
            // with Injury Engine and DoM - so only a blunt weapon or a bash does it): ONLY a bash or a blunt weapon makes a line - a
            // punch (the Unarmed / hand-to-hand weapon, whatever its source form), a blade, an arrow, a spell: silence, never a knock-out.
            // (PLANCK never flags a bare hand as a bash - utils.cpp:244 `!IsUnarmed(weapon)`; its shield / torch / bow hits always are.)
            const bool  bash   = (h.flags & kHitBash) != 0;
            const bool  sneak  = (h.flags & kHitSneak) != 0;
            const bool  power  = (h.flags & kHitPower) != 0;
            auto*       src    = h.source ? RE::TESForm::LookupByID(h.source) : nullptr;
            auto*       weap   = src ? src->As<RE::TESObjectWEAP>() : nullptr;
            const bool  unarmed = weap && weap->IsHandToHandMelee();   // never blunt (and never a knock-out on its own)
            const bool  mace   = weap && !unarmed && weap->HasKeywordID(kWeapTypeMace);
            const bool  hammer = weap && !unarmed && !mace && weap->HasKeywordID(kWeapTypeHammer);
            const bool  blunt  = mace || hammer;
            if (!bash && !blunt) return { false, "no: not a bash or a blunt weapon" };
            const char*       srcName = src && src->GetName() && *src->GetName() ? src->GetName() : "?";
            const std::string kind    = bash ? std::format("BASH ({}{})", srcName, blunt ? ", a blunt weapon" : "")
                                             : std::format("BLUNT {} ({})", mace ? "mace" : "warhammer", srcName);
            // u6b (no PLANCK required, as in the Knockout mod; PLANCK is kept for VR as it is more precise):
            // the Knockout mod has NO hit-location test, so WITHOUT PLANCK (SE / AE always, VR without activeragdoll.dll) a mace /
            // warhammer counts ANYWHERE; with PLANCK loaded it counts on PLANCK's head node only (R3, unchanged).
            const bool planckLoaded  = g_planckPresent.load(std::memory_order_relaxed);
            const bool anywhere      = bash && cfg.headHitBash;                                    // R2: a bash, anywhere
            const bool bluntAnywhere = !anywhere && blunt && cfg.headHitBlunt && !planckLoaded;    // u6b: no PLANCK - anywhere
            const bool headOnly      = !anywhere && blunt && cfg.headHitBlunt && planckLoaded;     // R3: PLANCK - the head only
            const char* bluntRule    = !blunt || anywhere ? ""
                                       : planckLoaded     ? " | rule: PLANCK loaded - a blunt weapon counts on the head only"
                                                          : " | rule: no PLANCK - a blunt weapon counts anywhere (the Knockout mod's rule)";

            // ---- the victim (R6 / R7 + SBKO's own guards): reads only, no lock of ours
            float       cur = 0.0f, mx = 0.0f;
            const float pct    = HealthFractionMax(a, &cur, &mx);   // current / (permanent + temporary) - the hold's basis
            const auto* st     = a->AsActorState();
            const auto  life   = st ? st->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
            const bool  foll   = IsFollower(a);
            TakenKind   takenKind = TakenKind::kNone;
            const char* taken  = TakenBy(a, takenKind);   // DoM capture / slave / tie / bag, restrained, disabled, VRTE rank 5
            const char* fight  = FistFightRunning();
            // u6b: with U5 on, a NORMAL NPC's bleedout no longer knocks her out by itself - so U6's 100 % rules may knock out a normal NPC
            // who is kneeling in the game's own bleedout ("acting hurt"; the bash itself may have put her there - its task runs after the
            // damage). KnockOutU then takes the psc's bleedout order (unconscious first). Essential / protected: unchanged (R6).
            const bool  bleeding = IsBleedingOut(a);
            const bool  u5Normal = g_u5On.load(std::memory_order_relaxed) && !IsProtectedOrEssential(a);
            std::string helmetWhat, helmetNot;   // u6f: rule A - the helmet, and the head-ish pieces that did not count (the log)
            const bool  helmet = WornHelmet(a, helmetWhat, helmetNot);
            // R4 low Health. u6 fix 1 (F2): + kLowHealthTolerance (a float measurement tolerance, not a gameplay limit) - Injury
            // Engine's torture floor stops a punched NPC at EXACTLY 0.1 x her permanent Health, and whether that read <= 0.10 must not
            // depend on float rounding of her own max. onTolerance = it fired only thanks to the tolerance (said in the line).
            const bool lowHp       = mx > 0.0f && pct <= cfg.headHitHealthThreshold + kLowHealthTolerance;
            const bool onTolerance = lowHp && pct > cfg.headHitHealthThreshold;
            const bool sneakBitOk  = cfg.headHitSneak && sneak;            // the game's sneak-attack bit: the first test
            const bool lowOk       = cfg.headHitLowHealth && lowHp;
            // R4 "she does not detect the player" = EXACTLY Papyrus IsDetectedBy (static RE of VR 0x985720: both actors have a process, then
            // target->RequestDetectionLevel(player, 3) > 0); -1000 = no process / no detection record = not detected. u6 fix 1
            // (F3): the engine call gets-or-creates her detection record for the player (VR 0x663110: her process's lock + an engine
            // allocation) and this task runs on an engine JOB thread in VR, so it is the LAST test - asked only when every
            // other rule passed, SBKO's own lists included, and neither the sneak bit nor low Health already said yes (detRead).
            bool         detRead  = false;
            std::int32_t det      = kNoDetectRecord;
            bool         detected = false;
            const std::string lowPart = cfg.headHitLowHealth
                                            ? std::format("her Health {:.2f}% is above {:.2f}%", pct * 100.0f, cfg.headHitHealthThreshold * 100.0f)
                                            : std::string("the low-Health trigger is off (bHeadHitLowHealth 0)");

            std::string why;   // empty = she qualifies (so far)
            if (const char* toy = ToyWeapon(weap))   // u6d (F1) / u6e: a toy (whip, slap, crop - its bash too) never knocks out
                why = std::format("{} - no knock-out", toy);
            else if (h.hasDamage && h.hitDamage <= 0.0f)   // u6e: a 0-damage hit (PLANCK's HitData says so) never knocks out
                why = std::format("a 0-damage hit (HitData damage {:.1f}) - no knock-out", h.hitDamage);
            else if (!anywhere && !headOnly && !bluntAnywhere) {
                std::string off;
                if (bash) off += "bHeadHitBash 0";
                if (blunt) off += std::string(off.empty() ? "" : ", ") + "bHeadHitBlunt 0";
                why = std::format("this kind of hit is switched off ({})", off);
            } else if (headOnly && !h.planck)
                why = "a blunt weapon counts only on the head, and this is not a PLANCK melee hit (no head data)";
            else if (headOnly && !h.head)
                why = std::format("a blunt weapon counts only on the head - PLANCK's hit node is '{}'", h.node.empty() ? "(none)" : h.node);
            else if (!IsNPC(a))
                why = "not an NPC (no ActorTypeNPC keyword - a creature)";
            else if (IsGhost(a))
                why = "a ghost (ActorTypeGhost)";
            else if (a->IsCommandedActor())   // u6d (F7)
                why = "a summoned / commanded actor";
            else if (const char* cr = ChildOrRider(a))   // u6d fix 2
                why = cr;
            else if (sbko::IsDeadNow(a))
                why = "dead";
            else if (bleeding && !u5Normal && !WhoSwitchedOff(a, foll))   // 2.1.0: a switched-off group kneels in the game's
                why = "bleeding out - SBKO's bleedout knock-out owns that"; //   bleedout - your own knock-out still takes her
            else if (cur <= 0.0f)
                why = "her Health is at or below 0 - the bleedout / death path owns her";
            else if (st && st->IsUnconscious())
                why = "already unconscious";
            else if (taken)
                why = std::format("taken by another mod ({})", taken);
            else if (life == RE::ACTOR_LIFE_STATE::kReanimate || life == RE::ACTOR_LIFE_STATE::kRecycle)
                why = std::format("not a living NPC (life state {})", static_cast<int>(life));
            else if (g_brawlFaction && a->GetFactionRank(g_brawlFaction, false) >= 0)
                why = "in a brawl (DGIntimidateFaction)";
            else if (fight)
                why = fight;
            else if (foll && !cfg.headHitFollowers)
                why = "a follower (bHeadHitFollowers 0)";
            else if (!a->Get3D())
                why = "no 3D (not loaded)";
            else if (!Helper::Present())
                why = "SBKO_Helper.pex is missing";
            else if (helmet && cfg.headHitHelmetProtects)
                why = std::format("she wears a helmet ({}) - immune (bHeadHitHelmetProtects 1)", helmetWhat);
            else if (!cfg.headHitSneak && !cfg.headHitLowHealth)
                why = "both triggers are switched off (bHeadHitSneak 0, bHeadHitLowHealth 0)";
            else if (!cfg.headHitSneak && !lowOk)
                why = std::format("the sneak / undetected trigger is off (bHeadHitSneak 0); {}", lowPart);

            // ---- SBKO's own lists (R6) - asked under g_mtx; a read here, asked again when the knock-out is made
            const RE::FormID id = h.target;
            auto listsWhy = [id]() -> std::string { return HeldWhy(id); };   // caller holds g_mtx (u6b: shared with U5)
            if (why.empty()) {
                std::scoped_lock lk(g_mtx);
                why = listsWhy();
            }
            // ---- the LAST test (u6 fix 1, F3): detection - only when everything else passed and nothing has said yes yet
            if (why.empty() && !sneakBitOk && !lowOk) {
                detRead  = !sbko::IsDeadNow(a) && a->Get3D();   // (both are already known true here - a belt)
                det      = detRead ? a->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kNormal) : kNoDetectRecord;
                detected = det > 0;
                if (!detRead)
                    why = "detection could not be read (dead / not loaded)";
                else if (detected)
                    why = std::format("not a sneak attack and she detects you (detection {}); {}", det, lowPart);
            }
            const bool sneakOk = sneakBitOk || (detRead && !detected);

            // ---- the knock-out (R8: SBKO's own knock-out path - an Entry, KnockOutU not from bleedout)
            const std::string who = Who(a, id);
            std::int32_t      potions    = 0;
            bool              guardSever = false;
            std::string       ess, koExtra;
            if (why.empty()) {
                potions    = cfg.potionsOff ? PotionSpellsOn(a) : 0;
                guardSever = cfg.severActionsGuard && g_severLoaded && Helper::SeverPresent();   // asks the VM - never under g_mtx
                ess        = EssFlags(a);
            }
            bool              knocked = false;
            std::vector<Call> calls;
            if (why.empty()) {
                std::scoped_lock lk(g_mtx);
                const auto it = g_map.find(id);
                if (std::string again = listsWhy(); !again.empty())
                    why = std::move(again);
                else {
                    // a new entry, or her COMING_TO entry (awake again) - a new episode either way, as the bleedout path does
                    Entry* const e = it != g_map.end() ? &it->second : &g_map[id];
                    e->who   = who;
                    koExtra  = StartKnockOutU(*e, id, potions, foll, ess, guardSever, bleeding, true, calls);
                    e->hitKO  = true;   // after StartKnockOutU (its ResetEpisode clears both)
                    e->hitPct = pct;
                    SyncCount();
                    knocked = true;
                }
            }

            const char*       what    = bash ? "bash" : bluntAnywhere ? "blunt hit (no PLANCK: anywhere)" : "head hit (blunt)";
            const std::string trigger = sneakOk ? (sneakBitOk ? std::string("a sneak attack (the game's sneak-attack bit)")
                                                              : std::format("she does not detect you (detection {})", det))
                                                : std::format("her Health {:.2f}% <= {:.2f}%{}", pct * 100.0f, cfg.headHitHealthThreshold * 100.0f,
                                                              onTolerance ? " (on the 0.1 % float tolerance - e.g. Injury Engine's 10 % floor)"
                                                                          : "");
            // u6 fix 1 (F3): the detection value, or why it was not asked
            const std::string detText = detRead ? std::format("{} ({})", det,
                                                              det == kNoDetectRecord ? "no detection record = not detected"
                                                              : detected             ? "detected"
                                                                                     : "not detected")
                                        : std::format("not read ({})", !cfg.headHitSneak ? "bHeadHitSneak 0"
                                                                       : !why.empty()    ? "another rule already says no"
                                                                       : sneakBitOk      ? "the sneak bit already says yes"
                                                                                         : "low Health already says yes");
            const std::string verdict = knocked ? std::format("KNOCKED OUT by a {}{} - {}", sneakOk ? "sneak " : "", what, trigger)
                                                : "no: " + why;
            if (cfg.logHeadHits || knocked) {
                const std::string planckText =
                    h.planck ? std::format("PLANCK yes, node '{}', head {}, contact ({:.0f}, {:.0f}, {:.0f}), {} hand, HitData damage {}",
                                           h.node.empty() ? "(none)" : h.node,
                                           !h.head ? std::string("no") : h.headUp == 0 ? std::string("YES")
                                                                                      : std::format("YES (parent {} of the node)", h.headUp),
                                           h.pos.x, h.pos.y, h.pos.z, h.isLeft ? "left" : "right",
                                           h.hasDamage ? std::format("{:.1f}", h.hitDamage) : std::string("not read"))
                             : h.magicNoMatch ? std::string("PLANCK no (the magic word was there, but PLANCK's current hit event was another "
                                                            "one - the extended block was NOT read)")
                             : h.magicNoPlanck ? std::string("PLANCK no (the magic word was there, but PLANCK is not loaded - the extended "
                                                             "block was NOT read)")
                                               : std::string("PLANCK no (no hit node)");
                logger::info("[headhit] {} <- player {}{} | {}{} | sneak bit {}{}{} | detection {} | helmet {} | HP {} ({:.1f} / {:.1f}) "
                             "| {} | {}",
                             who, kind, h.test ? std::format(" [console test '{}': a simulated hit through the real U6 rules]", h.test) : std::string(),
                             planckText, bluntRule, sneak ? "YES" : "no", power ? ", power attack" : "", bash ? ", bash bit" : "",
                             detText, HelmetLogText(helmet, helmetWhat, helmetNot, "YES:", "no"), Pct1(pct), cur, mx,
                             foll ? "a follower" : "not a follower", verdict);
            }
            if (knocked)
                logger::info("[KO] {} KNOCK-OUT BY A HIT ({}{} - {}) - KnockOutU dispatched ({}){}; her Health will be held at her Health "
                             "at the knock-out ({}; no {}% cap); Follower Death Chance roll: {}; no wake until it replies",
                             who, sneakOk ? "sneak " : "", what, trigger,
                             bleeding ? "she was kneeling in the game's bleedout: unconscious first, then Paralysis + shove"
                                      : "Paralysis + shove, then unconscious: not from bleedout",
                             koExtra, Pct1(pct),
                             std::lround(cfg.unconsciousHealthFraction * 100.0f),
                             g_fdcRolledFaction ? "NONE (FDC_SBKO_RolledFaction rank 2 set before rank 10 - her Health is above 0)"
                                                : "- (FDC_SBKO_Patch.esp not loaded)");
            Run(calls);
            return { knocked, verdict };
        }

        // =============================================================================================================================
        // u6c: THE SLEEP SPELL. One Sleep effect (an effect in g_sleepMgefs) cast by the PLAYER landed on an actor - relayed by EffectSink,
        // run here in an SKSE task (in VR an engine JOB thread inside SKSE's queue lock). The rules: a spell
        // works 100 % if it hits (no roll, no detection rule, no Health rule, NO helmet test); U6's targets; SBKO's own HIT knock-out (the
        // hit hold, the FDC no-roll pre-mark); whether she SAW the caster only picks the wake prompt (SBKO_SleepFaction rank 2 / 1). The
        // detection is read ONCE, after every other rule (u6 fix 1, F3: the call gets-or-creates her detection record under her process
        // lock). The victim chain is OnHitTask's (R6 / R7 + u6b's kneeling normal NPC, D8) minus PLANCK / helmet / triggers - written out
        // here so the reviewed U6 / U5 tasks stay untouched in this round. Engine reads with no lock of ours; g_mtx only for SBKO's lists.
        // =============================================================================================================================
        TaskResult OnSleepTask(RE::FormID a_tid, RE::FormID a_mgef, std::uint32_t a_gen, const char* a_test = nullptr)
        {
            if (a_gen != g_generation.load() || !g_gameReady.load()) return { false, "no: the game is loading" };
            auto* a      = RE::TESForm::LookupByID<RE::Actor>(a_tid);
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!a || !player || a->IsPlayerRef()) return { false, "no: no such NPC" };
            const auto&       cfg   = Settings::Get();
            const auto*       m     = RE::TESForm::LookupByID<RE::EffectSetting>(a_mgef);
            const std::string spell = std::format("{} 0x{:08X}", m && m->GetName() && *m->GetName() ? m->GetName() : "(no name)", a_mgef);

            // ---- the victim (U6's R6 / R7 + SBKO's own guards): reads only, no lock of ours
            float       cur = 0.0f, mx = 0.0f;
            const float pct       = HealthFractionMax(a, &cur, &mx);   // current / (permanent + temporary) - the hold's basis
            const auto* st        = a->AsActorState();
            const auto  life      = st ? st->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
            const bool  foll      = IsFollower(a);
            TakenKind   takenKind = TakenKind::kNone;
            const char* taken     = TakenBy(a, takenKind);   // DoM capture / slave / tie / bag, restrained, disabled, VRTE rank 5
            const char* fight     = FistFightRunning();
            // u6b D8, kept for the spell: with U5 on, a NORMAL NPC kneeling in the game's own bleedout ("acting hurt") can be put to sleep;
            // KnockOutU then takes the psc's bleedout order (unconscious first). Essential / protected in bleedout: SBKO's bleedout path.
            const bool bleeding = IsBleedingOut(a);
            const bool u5Normal = g_u5On.load(std::memory_order_relaxed) && !IsProtectedOrEssential(a);

            std::string why;   // empty = she qualifies (so far)
            if (!IsNPC(a))
                why = "not an NPC (no ActorTypeNPC keyword - a creature)";
            else if (IsGhost(a))
                why = "a ghost (ActorTypeGhost)";
            else if (a->IsCommandedActor())   // u6d (F7): a summon is never put to sleep
                why = "a summoned / commanded actor";
            else if (const char* cr = ChildOrRider(a))   // u6d fix 2: no child, no rider on a mount
                why = cr;
            else if (sbko::IsDeadNow(a))
                why = "dead";
            else if (bleeding && !u5Normal && !WhoSwitchedOff(a, foll))   // 2.1.0: a switched-off group kneels in the game's
                why = "bleeding out - SBKO's bleedout knock-out owns that"; //   bleedout - your own knock-out still takes her
            else if (cur <= 0.0f)
                why = "her Health is at or below 0 - the bleedout / death path owns her";
            else if (st && st->IsUnconscious())
                why = "already unconscious";
            else if (taken)
                why = std::format("taken by another mod ({})", taken);
            else if (life == RE::ACTOR_LIFE_STATE::kReanimate || life == RE::ACTOR_LIFE_STATE::kRecycle)
                why = std::format("not a living NPC (life state {})", static_cast<int>(life));
            else if (g_brawlFaction && a->GetFactionRank(g_brawlFaction, false) >= 0)
                why = "in a brawl (DGIntimidateFaction)";
            else if (fight)
                why = fight;
            else if (foll && !cfg.sleepFollowers)
                why = "a follower (bSleepFollowers 0)";
            else if (!a->Get3D())
                why = "no 3D (not loaded)";
            else if (!Helper::Present())
                why = "SBKO_Helper.pex is missing";
            if (why.empty()) {   // SBKO's own lists - a read here, asked again when the knock-out is made
                std::scoped_lock lk(g_mtx);
                why = HeldWhy(a_tid);
            }

            // ---- SEEN: only when everything passed (the call gets-or-creates her detection record - never asked for a "no")
            bool         detRead = false;
            std::int32_t det     = kNoDetectRecord;
            bool         seen    = false;
            if (why.empty()) {
                detRead = !sbko::IsDeadNow(a) && a->Get3D();   // (both already known true here - a belt)
                det     = detRead ? a->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kNormal) : kNoDetectRecord;
                seen    = detRead && det > 0;                   // = Papyrus IsDetectedBy; -1000 = no record = not seen
            }
            const std::int32_t mark = seen ? kSleepSeen : kSleepUnseen;

            // ---- the knock-out (SBKO's own HIT knock-out path - an Entry, KnockOutU; asked again under g_mtx)
            const std::string who = Who(a, a_tid);
            std::int32_t      potions    = 0;
            bool              guardSever = false;
            std::string       ess, koExtra;
            if (why.empty()) {
                potions    = cfg.potionsOff ? PotionSpellsOn(a) : 0;
                guardSever = cfg.severActionsGuard && g_severLoaded && Helper::SeverPresent();   // asks the VM - never under g_mtx
                ess        = EssFlags(a);
            }
            bool              knocked = false;
            std::vector<Call> calls;
            if (why.empty()) {
                std::scoped_lock lk(g_mtx);
                const auto it = g_map.find(a_tid);
                if (std::string again = HeldWhy(a_tid); !again.empty())
                    why = std::move(again);
                else {
                    // a new entry, or her COMING_TO entry (awake again) - a new episode either way, as the bleedout path does
                    Entry* const e = it != g_map.end() ? &it->second : &g_map[a_tid];
                    e->who      = who;
                    koExtra     = StartKnockOutU(*e, a_tid, potions, foll, ess, guardSever, bleeding, true, calls, mark);
                    e->hitKO     = true;   // after StartKnockOutU (its ResetEpisode clears these)
                    e->hitPct    = pct;
                    e->sleepMark = mark;
                    SyncCount();
                    knocked = true;
                }
            }

            const std::string detText = detRead ? std::format("{} ({})", det,
                                                              det == kNoDetectRecord ? "no detection record = she did not see you"
                                                              : seen                 ? "she saw you"
                                                                                     : "she did not see you")
                                                : std::string("not read (another rule already says no)");
            if (cfg.logSleep || knocked || a_test)
                logger::info("[sleep] {} <- player {}{} | seen {} | detection {} | HP {} ({:.1f} / {:.1f}) | {} | {}", who, spell,
                             a_test ? std::format(" [console test '{}': the Sleep spell's real path, as if you cast it]", a_test) : std::string(),
                             !detRead ? "-" : seen ? "YES" : "no", detText, Pct1(pct), cur, mx, foll ? "a follower" : "not a follower",
                             knocked ? std::format("KNOCKED OUT - put to sleep (wake prompt: {})", seen ? "SEEN" : "NOT SEEN")
                                     : "no: " + why);
            if (knocked)
                logger::info("[KO] {} PUT TO SLEEP BY THE SLEEP SPELL ({}) - KnockOutU dispatched ({}){}; SBKO_SleepFaction rank {}; her "
                             "Health will be held at her Health at the knock-out ({}; no {}% cap); Follower Death Chance roll: {}; no wake "
                             "until it replies",
                             who, seen ? "she saw you" : "she did not see you",
                             bleeding ? "she was kneeling in the game's bleedout: unconscious first, then Paralysis + shove"
                                      : "Paralysis + shove, then unconscious: not from bleedout",
                             koExtra, mark, Pct1(pct), std::lround(cfg.unconsciousHealthFraction * 100.0f),
                             g_fdcRolledFaction ? "NONE (FDC_SBKO_RolledFaction rank 2 set before rank 10 - her Health is above 0)"
                                                : "- (FDC_SBKO_Patch.esp not loaded)");
            Run(calls);
            return { knocked, knocked ? std::format("put to sleep ({})", seen ? "she saw you" : "she did not see you") : "no: " + why };
        }

        // =============================================================================================================================
        // u6f: A SLEEPER WAKES ON A HIT OR WHEN THE PLAYER TALKS TO HER (it is just sleep). Relayed by HitSink (any TESHitEvent on
        // her, by anyone) and ActivateSink (the player activating her), both filtered by g_sleepWatch; decided again here under g_mtx (the
        // entry must still be a Sleep-spell sleeper SBKO holds). Engine reads with no lock of ours; Papyrus via Run() after the lock.
        // =============================================================================================================================
        enum class SleepWakeBy : std::uint8_t
        {
            kHit  = 0,
            kTalk = 1,
        };

        // the hit came from the Sleep spell / scroll itself (a MagicItem whose effects include one of g_sleepMgefs) - its own projectile hit
        bool IsSleepItem(RE::FormID a_src)
        {
            if (!a_src) return false;
            auto* f  = RE::TESForm::LookupByID(a_src);
            auto* mi = f ? f->As<RE::MagicItem>() : nullptr;
            if (!mi) return false;
            for (const auto* eff : mi->effects)
                if (eff && eff->baseEffect && std::binary_search(g_sleepMgefs.begin(), g_sleepMgefs.end(), eff->baseEffect->GetFormID()))
                    return true;
            return false;
        }

        TaskResult OnSleepWakeTask(RE::FormID a_tid, RE::FormID a_cid, RE::FormID a_src, RE::FormID a_proj, std::uint32_t a_flags,
                                   SleepWakeBy a_by, std::uint32_t a_gen, const char* a_test = nullptr)
        {
            if (a_gen != g_generation.load() || !g_gameReady.load()) return { false, "no: the game is loading" };
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_tid);
            if (!a) return { false, "no: no such NPC" };
            // never a wake: the Sleep spell's own hit (its projectile's TESHitEvent - source = the spell / scroll) and her own self-shove
            // (KnockOutU / HoldDown push her with herself as the source - a belt: PushActorAway is not known to send a hit event)
            if (a_by == SleepWakeBy::kHit) {
                if (IsSleepItem(a_src) || (g_sleepProjectile && a_proj && a_proj == g_sleepProjectile->GetFormID()))
                    return { false, "no: the Sleep spell's own hit never wakes" };
                if (a_cid == a_tid) return { false, "no: her own hit never wakes" };
            }
            // the text: who / what woke her (reads only)
            std::string trigger;
            if (a_by == SleepWakeBy::kTalk) {
                trigger = "by the player talking to her (activated her)";
            } else {
                std::string attacker = "no attacker (a trap / the world)";
                if (a_cid) {
                    auto* cf = RE::TESForm::LookupByID(a_cid);
                    if (auto* ca = cf ? cf->As<RE::Actor>() : nullptr)
                        attacker = ca->IsPlayerRef() ? std::string("the player") : Who(ca, a_cid);
                    else
                        attacker = std::format("{} (0x{:08X})", cf && cf->GetName() && *cf->GetName() ? cf->GetName() : "?", a_cid);
                }
                auto*             sf  = a_src ? RE::TESForm::LookupByID(a_src) : nullptr;
                const std::string src = sf ? std::format("{} 0x{:08X}", sf->GetName() && *sf->GetName() ? sf->GetName() : "(no name)", a_src)
                                           : std::string("no weapon / spell form");
                trigger = std::format("by a hit from {} ({}{}{})", attacker, src, (a_flags & kHitBash) ? ", a bash" : "",
                                      (a_flags & kHitSneak) ? ", a sneak attack" : "");
            }
            if (a_test) trigger += std::format(" [console test '{}': a simulated hit through the real sleep-wake path]", a_test);
            const float       pct       = HealthFractionMax(a);
            const bool        dead      = sbko::IsDeadNow(a);
            TakenKind         takenKind = TakenKind::kNone;
            const char* const taken     = TakenBy(a, takenKind);
            std::string       who, action;
            bool              notWoken = false, wokeNow = false;
            std::vector<Call> calls;
            {
                std::scoped_lock lk(g_mtx);
                auto             it = g_map.find(a_tid);
                if (it == g_map.end() || it->second.sleepMark <= 0)
                    return { false, "no: not a Sleep-spell sleeper SBKO holds" };   // not (or no longer) a Sleep-spell sleeper of SBKO's
                Entry& e = it->second;
                who      = e.who;
                if (e.state == State::kComingTo || e.sleepWakePending || (e.healPending && e.state == State::kUnconscious))
                    return { false, "no: she is already waking" };   // she is already waking (or a wake is already waiting) - one line per sleep
                if (dead) {
                    notWoken = true;
                    action   = "she is dead - nothing to wake";
                } else if (e.releasing || taken) {
                    notWoken = true;
                    action   = std::format("taken by another mod ({}) - no SBKO wake", taken ? taken : "SBKO is letting her go");
                } else if (e.state == State::kEntering) {
                    e.sleepWakePending = true;
                    e.sleepWakeTrigger = trigger;
                    action = "she was still falling asleep (KnockOutU has not replied) - she wakes the moment it does";
                } else if (e.state == State::kUnconscious) {
                    e.sleepWakeTrigger = trigger;
                    if (e.holdInFlight || e.healthInFlight) {
                        e.sleepWakePending = true;
                        action = std::format("the wake waits for the running {} (next check)", e.holdInFlight ? "HoldDown" : "HoldHealthTo");
                    } else {
                        logger::info("[sleep] {} woken {} - she wakes up", who, trigger);   // before the wake's own [KO] lines
                        BeginSleepWake(e, a_tid, pct, calls);
                        SyncCount();
                        wokeNow = true;
                    }
                } else
                    return { false, "no: an old knock-out, never a sleeper" };   // an M1 DOWN / u2a WINDOW leftover - never a sleeper
            }
            if (!action.empty())
                logger::info("[sleep] {} {} {} - {}", who, notWoken ? "NOT woken" : "woken", trigger, action);
            Run(calls);
            return { !notWoken, wokeNow ? std::string("WOKEN - she wakes up") : notWoken ? "NOT woken: " + action : "wake queued: " + action };
        }

        // =============================================================================================================================
        // u7a U7: THE TAKE-OVER - VRTouchEvents' choke passes out (15 s) and hands her to SBKO (SBKO_API.TakeOver, Api.cpp). The idea:
        // a choked NPC knows she was choked, so she wakes up confused with a vague memory of the player's hand
        // around her throat and everything going black; SBKO's rules apply when SBKO is installed and takes over; the NPC is handed
        // to SBKO when SBKO is present and kept in VRTE when it is not. The native decides on reads (TakeOverDecide - no g_mtx,
        // no VM call), answers True at once and relays OnTakeOverTask: SBKO's own knock-out (StartKnockOutU, a HIT-class one: held at her
        // Health, no FDC roll - mark 2 -, KnockOutU in VRTE's own order under the live grip, quiet), cls kClsChoke, SBKO_SleepFaction rank 3
        // (the prompts' "choked" marker; the entry's sleepMark stays 0 - NOT the Sleep spell's light class: a hit / DoM's Wake do not wake
        // her, only a real heal or SBKO's 6-12 h timer), the seconds held co-saved for build 3's neck mark.
        // =============================================================================================================================
        struct TakeOverCopy
        {
            const char*   test      = nullptr;   // a console test's label (a string literal) - nullptr = the native
            RE::FormID    target    = 0;
            RE::FormID    aggressor = 0;
            std::string   source;
            float         seconds   = 0.0f;      // the seconds the choke was held when it was handed over
            std::uint32_t gen       = 0;
        };

        // the take-over is done (or dropped): she leaves the pending set. Caller may hold g_mtx (order g_mtx -> g_watchMtx); called AFTER
        // SyncCount put a knocked-out NPC in g_watch, so Holds() never sees a gap.
        void EndTakeOverPending(RE::FormID a_id)
        {
            std::scoped_lock lk(g_watchMtx);
            g_takeOverPending.erase(a_id);
        }

        std::string ActorText(RE::Actor* a_actor, RE::FormID a_id)
        {
            if (!a_actor) return a_id ? std::format("0x{:08X}", a_id) : std::string("nobody");
            return a_actor->IsPlayerRef() ? std::string("the player") : Who(a_actor, a_id);
        }

        TaskResult OnTakeOverTask(const TakeOverCopy& t)
        {
            const auto& cfg = Settings::Get();
            if (t.gen != g_generation.load() || !g_gameReady.load()) {
                EndTakeOverPending(t.target);
                return { false, "no: the game is loading" };
            }
            auto* a = RE::TESForm::LookupByID<RE::Actor>(t.target);
            if (!a || a->IsPlayerRef()) {
                EndTakeOverPending(t.target);
                return { false, "no: no such NPC" };
            }
            float secs = t.seconds;   // a second TakeOver while this one was pending may have brought more seconds
            {
                std::scoped_lock lk(g_watchMtx);
                if (const auto it = g_takeOverPending.find(t.target); it != g_takeOverPending.end()) secs = (std::max)(secs, it->second);
            }
            const std::string who = Who(a, t.target);
            auto*             ag  = t.aggressor ? RE::TESForm::LookupByID<RE::Actor>(t.aggressor) : nullptr;
            // ---- reads, no lock of ours
            float              cur = 0.0f, mx = 0.0f;
            const float        pct        = HealthFractionMax(a, &cur, &mx);
            const bool         dead       = sbko::IsDeadNow(a);
            const bool         has3D      = a->Get3D() != nullptr;
            const bool         bleeding   = IsBleedingOut(a);
            const bool         foll       = IsFollower(a);
            TakenKind          takenKind  = TakenKind::kNone;
            const char*        taken      = TakenBy(a, takenKind);
            const std::int32_t potions    = cfg.potionsOff ? PotionSpellsOn(a) : 0;
            const bool         guardSever = cfg.severActionsGuard && g_severLoaded && Helper::SeverPresent();   // asks the VM - never under g_mtx
            const std::string  ess        = EssFlags(a);
            const float        hours      = HoursNow();
            std::string        why, already, koExtra;
            bool               knocked = false;
            bool               alreadyChoke = false;   // m34: a second TakeOver on her choke knock-out (its mark is renewed)
            std::vector<Call>  calls;
            {
                std::scoped_lock lk(g_mtx);
                if (dead)
                    why = "she died before the knock-out landed - nothing to do (VRTE opened no slot; the death stands)";
                else if (!has3D)
                    why = "she has no 3D any more (unloaded) - nothing done";
                else if (taken)
                    why = std::format("taken by another mod meanwhile ({}) - nothing done", taken);
                else {
                    const auto it = g_map.find(t.target);
                    if (it != g_map.end() && it->second.releasing)
                        why = "SBKO is letting her go to another mod - nothing done";
                    else if (it != g_map.end() && it->second.state != State::kComingTo) {
                        Entry& e = it->second;
                        if (e.cls == kClsChoke && secs > e.chokeSeconds) {
                            e.chokeSeconds = secs;
                            e.chokeHours   = hours;
                        }
                        alreadyChoke = e.cls == kClsChoke;
                        already = std::format("already SBKO's ({}{}) - nothing to do{}", StateName(e.state),
                                              e.cls == kClsChoke ? ", choked" : e.sleepMark > 0 ? ", the Sleep spell" : e.hitKO ? ", a hit knock-out"
                                                                                                                         : ", a bleedout knock-out",
                                              e.cls == kClsChoke ? std::format("; choke seconds now {:.1f}", e.chokeSeconds) : std::string());
                    } else {
                        // VRTE decided: SBKO's own lists give way - this knock-out is a new episode (as the hit / sleep paths' COMING_TO rule)
                        g_declined.erase(t.target);
                        g_pendingConscious.erase(t.target);
                        g_adoptCandidates.erase(t.target);
                        g_adoptAtLoad.erase(t.target);
                        g_chokeLoose.erase(t.target);   // the entry carries the seconds from now on
                        g_chokeLooseCount.store(g_chokeLoose.size());
                        g_woken.erase(t.target);
                        Entry* const e = it != g_map.end() ? &it->second : &g_map[t.target];
                        e->who         = who;
                        koExtra        = StartKnockOutU(*e, t.target, potions, foll, ess, guardSever, bleeding, true, calls, kSleepChoked);
                        e->hitKO        = true;   // after StartKnockOutU (its ResetEpisode clears these): held at her Health, no FDC roll
                        e->hitPct       = pct;
                        e->cls          = kClsChoke;
                        e->chokeSeconds = secs;
                        e->chokeHours   = hours;
                        SyncCount();
                        knocked = true;
                    }
                }
                EndTakeOverPending(t.target);   // after SyncCount - she is in g_watch now (Holds() never sees a gap)
            }
            const std::string from = std::format("{} (by {}, choke held {:.1f} s){}", t.source, ActorText(ag, t.aggressor), secs,
                                                 t.test ? std::format(" [console test '{}': the TakeOver native's own path]", t.test) : std::string());
            if (knocked)
                logger::info("[KO] {} TAKEN OVER FROM {} - CHOKED UNCONSCIOUS - KnockOutU dispatched ({}){}; held at her Health ({}; a hit-class "
                             "knock-out - no {}% cap); Follower Death Chance roll: {}; SBKO_SleepFaction rank 3 (choked) {}; quiet (VRTE's passout "
                             "line speaks); wakes ONLY on a real heal or the timer ({:.0f}-{:.0f} game h) - DoM's Wake = too injured; no wake until it "
                             "replies",
                             who, from,
                             bleeding ? "she was in bleedout: unconscious first, then Paralysis + shove"
                                      : "Paralysis + shove, then unconscious: VRTE's own order, under the grip",
                             koExtra, Pct1(pct), std::lround(cfg.unconsciousHealthFraction * 100.0f),
                             g_fdcRolledFaction ? "NONE (FDC_SBKO_RolledFaction rank 2 set before rank 10 - her Health is above 0)"
                                                : "- (FDC_SBKO_Patch.esp not loaded)",
                             g_sleepFaction ? "written" : "NOT written (SBKO_SleepFaction missing - the prompts read an ordinary knock-out)",
                             cfg.recoverMinHours, cfg.recoverMaxHours);
            else if (cfg.logTakeOver || t.test)
                logger::info("[takeover] {} from {} -> {}", who, from, !already.empty() ? already : "NOT knocked out: " + why);
            Run(calls);
            // m34 (build 3): the choke hand mark - she passed out (the take-over fires at VRTE's passout); a second TakeOver on a choke
            // knock-out SBKO already holds brings the larger seconds (the mark keeps the stronger step / the longer life)
            if (knocked || alreadyChoke) Marks::OnChoke(t.target, secs, true, "VRTE's take-over at the passout");
            return { knocked, knocked ? std::string("taken over (choked) - SBKO holds her") : !already.empty() ? already : "not knocked out: " + why };
        }

        enum class TakeOverVerdict : std::uint8_t
        {
            kDeclined = 0,   // False: VRTE keeps her in its own slot
            kAlready  = 1,   // True: SBKO holds her already (the task updates a choke's seconds)
            kPending  = 2,   // True: an earlier TakeOver's task has not run yet (its seconds raised if larger)
            kAccepted = 3,   // True: put in g_takeOverPending - the caller relays OnTakeOverTask (the console runs it at once)
        };

        // The TakeOver native's decision (a Papyrus thread, or the console's): reads + g_watchMtx only - never g_mtx, never a VM call
        // (Helper::PresentCached, filled by every check). One [takeover] line.
        TakeOverVerdict TakeOverDecide(RE::Actor* a_target, RE::Actor* a_aggressor, std::string_view a_source, float a_seconds, const char* a_test,
                                       std::string* a_whyOut = nullptr)
        {
            const auto& cfg   = Settings::Get();
            const bool  known = a_source == "VRTouchEvents.choke" || a_source == "console.choke";
            std::string why;
            if (!cfg.takeOver)
                why = "bTakeOver 0";
            else if (!g_gameReady.load())
                why = "no game is ready (a load is running)";
            else if (const int hp = Helper::PresentCached(); hp != 1)
                why = hp < 0 ? "SBKO's first check after the load has not run yet (SBKO_Helper's presence is not known)" : "SBKO_Helper.pex is missing";
            else if (!a_target)
                why = "no actor (None)";
            else if (a_target->IsPlayerRef())
                why = "that is the player";
            else if (sbko::IsDeadNow(a_target))
                why = "she is dead";
            else if (!a_target->Get3D())
                why = "she is not loaded (no 3D)";
            else if (!IsNPC(a_target))
                why = "not an NPC (no ActorTypeNPC - a creature)";
            else if (const char* cr = ChildOrRider(a_target))
                why = cr;
            else if (!known)
                why = std::format("unknown source '{}' (known: VRTouchEvents.choke)", a_source);
            else {
                TakenKind kind = TakenKind::kNone;
                if (const char* t = TakenBy(a_target, kind)) why = std::format("taken by another mod ({})", t);
            }
            const RE::FormID  id  = a_target ? a_target->GetFormID() : 0;
            const std::string who = a_target ? Who(a_target, id) : std::string("None");
            const std::string from = std::format("{} (by {}, choke held {:.1f} s){}", a_source,
                                                 ActorText(a_aggressor, a_aggressor ? a_aggressor->GetFormID() : 0), a_seconds,
                                                 a_test ? std::format(" [console test '{}']", a_test) : std::string());
            if (!why.empty()) {
                if (cfg.logTakeOver || a_test) logger::info("[takeover] {} from {} -> DECLINED ({}) - VRTE keeps her in its own slot", who, from, why);
                if (a_whyOut) *a_whyOut = why;
                return TakeOverVerdict::kDeclined;
            }
            if (Watched(id)) {
                if (cfg.logTakeOver || a_test) logger::info("[takeover] {} from {} -> ACCEPTED: already SBKO's - nothing to knock out", who, from);
                return TakeOverVerdict::kAlready;
            }
            {
                std::scoped_lock lk(g_watchMtx);
                if (const auto it = g_takeOverPending.find(id); it != g_takeOverPending.end()) {
                    it->second = (std::max)(it->second, a_seconds);
                    if (cfg.logTakeOver || a_test)
                        logger::info("[takeover] {} from {} -> ACCEPTED: an earlier take-over is still landing - nothing more to do", who, from);
                    return TakeOverVerdict::kPending;
                }
                g_takeOverPending[id] = a_seconds;
            }
            if (cfg.logTakeOver || a_test) logger::info("[takeover] {} from {} -> ACCEPTED - the knock-out lands next frame", who, from);
            return TakeOverVerdict::kAccepted;
        }

        // ChokeReleased (Api.cpp): the player let go of the choke after a_seconds in all (after the passout or before it). On her choke
        // knock-out (held, coming to, or on the released watch) = its TOTAL seconds (the larger value kept); no take-over (let go before the
        // passout, or SBKO declined) = the loose-choke list. Both co-saved ('CHOK') for build 3's neck mark. An SKSE task (g_mtx).
        TaskResult OnChokeReleasedTask(RE::FormID a_id, float a_seconds, const char* a_from, std::uint32_t a_gen, const char* a_test = nullptr)
        {
            if (a_gen != g_generation.load() || !g_gameReady.load()) return { false, "no: the game is loading" };
            auto*             a     = RE::TESForm::LookupByID<RE::Actor>(a_id);
            const std::string who   = a ? Who(a, a_id) : std::format("0x{:08X}", a_id);
            const bool        dead  = a && sbko::IsDeadNow(a);
            const float       hours = HoursNow();
            std::string       where;
            bool              kept = true;
            int               markKind = 0;   // m34: 1 = her choke knock-out (a passout), 2 = loose (no take-over), 0 = no mark
            float             markSecs = a_seconds;
            {
                std::scoped_lock lk(g_mtx);
                if (const auto it = g_map.find(a_id); it != g_map.end() && it->second.cls == kClsChoke) {
                    Entry& e       = it->second;
                    e.chokeSeconds = (std::max)(e.chokeSeconds, a_seconds);
                    markKind       = 1;
                    markSecs       = e.chokeSeconds;
                    e.chokeHours   = hours;
                    where          = std::format("on her choke knock-out ({}): total {:.1f} s", StateName(e.state), e.chokeSeconds);
                } else if (const auto rt = g_released.find(a_id); rt != g_released.end() && rt->second.choke) {
                    rt->second.chokeSeconds = (std::max)(rt->second.chokeSeconds, a_seconds);
                    rt->second.chokeHours   = hours;
                    markKind                = 1;
                    markSecs                = rt->second.chokeSeconds;
                    where = std::format("on her choke knock-out (released to another mod, watched): total {:.1f} s", rt->second.chokeSeconds);
                } else if (dead || !a) {
                    kept  = false;
                    where = a ? "she is dead - nothing kept" : "no such actor - nothing kept";
                } else {
                    g_chokeLoose[a_id] = ChokeLoose{ a_seconds, hours };
                    g_chokeLooseCount.store(g_chokeLoose.size());
                    markKind           = 2;
                    where = std::format("no SBKO choke knock-out (let go before the passout, or the take-over was declined) - kept on the loose-choke "
                                        "list ({} NPC(s)) for build 3's neck mark", g_chokeLoose.size());
                }
            }
            if (Settings::Get().logTakeOver || a_test)
                logger::info("[takeover] {} choke RELEASED after {:.1f} s ({}{}) -> {}", who, a_seconds, a_from,
                             a_test ? std::format(" [console test '{}': the ChokeReleased native's own path]", a_test) : std::string(), where);
            // m34 (build 3): the choke hand mark with the TOTAL seconds - a choke knock-out = a passout; a loose release = before the passout
            // (Marks: bChokeMarkBeforePassout), unless it lasted fChokePassoutSeconds or more (she passed out in VRTE's own slot). The mark keeps
            // its own seconds / start / step in record 'MARK' - it fades on its own clock after she wakes or is released (u7a).
            if (markKind == 1)
                Marks::OnChoke(a_id, markSecs, true, a_test ? "console 'sbko release' - her choke knock-out" : "ChokeReleased - her choke knock-out");
            else if (markKind == 2)
                Marks::OnChoke(a_id, markSecs, false, a_test ? "console 'sbko release' - no take-over" : "ChokeReleased - no take-over");
            return { kept, std::format("released after {:.0f} s - {}", a_seconds, kept ? "recorded" : "nothing kept") };
        }

        // =============================================================================================================================
        // u7a M2: "WOKEN TO TALK" (a follow-up of DoM's Wake and / or talking to a sleeping NPC: SkyrimNet is told
        // "(Player's name) just woke you to talk to you"). The sinks copy (ActivateSink: the player activating an NPC; PerkRunSink:
        // Diary of Mine's DOMInteractPerk entry) and relay OnWokenEventTask: a natural sleeper (sit-sleep state 7 / 8 at the event, not
        // sneaking, not hostile, not held by SBKO) goes on g_woken; the next UNPAUSED check that reads her no longer asleep narrates it
        // (NarrateWoken kind 1 - she answers). DoM's Bag / Abduct / Untie / Tie-up cancel it; still asleep after 2 checks (Cancel, a
        // pickpocket, a blocked activation) = nothing said - a detection window like the 2-check reset rule, not a gameplay limit. DoM's
        // Wake on a Sleep-spell sleeper SBKO holds tags her entry (wokeByPlayer): her wake (the external-clear sleeper path) is narrated
        // "woken from a deep sleep" by the player (OnWakeDone).
        // =============================================================================================================================
        struct WokenCopy
        {
            const char*   test          = nullptr;   // a console test's label
            RE::FormID    target        = 0;
            WokenBy       by            = WokenBy::kTalk;
            std::int32_t  frag          = -1;        // the perk event's flag = DoM's fragment index (-1 = a talk)
            std::uint32_t state         = 0;         // her sit-sleep state at the event (C++ enum: 7 sleeping, 8 wants to wake)
            bool          sneaking      = false;     // the player's sneaking bit at the event
            bool          domPerk       = false;     // the perk event's perk IS Diary of Mine's DOMInteractPerk
            RE::FormID    perk          = 0;         // (probe) the perk event's perk FormID as read at +0x10
            std::uint32_t flag          = 0;         // (probe) its flag as read at +0x14
            bool          causeIsPlayer = false;     // (probe) cause pointer == the player (a compare, never a dereference)
            std::uint32_t gen           = 0;
        };

        const char* SitSleepName(std::uint32_t a_s)
        {
            switch (a_s) {
                case 0: return "normal";
                case 1: return "wants to sit";
                case 2: return "waiting for the sit animation";
                case 3: return "sitting / riding";
                case 4: return "wants to stand";
                case 5: return "wants to sleep";
                case 6: return "waiting for the sleep animation";
                case 7: return "SLEEPING";
                case 8: return "wants to wake";
            }
            return "?";
        }

        const char* DomFragName(std::int32_t a_f)
        {
            switch (a_f) {
                case 0: return "Wake";
                case 1: return "Bag";
                case 2: return "Abduct";
                case 3: return "Untie";
                case 4: return "Tie-up";
            }
            return "(another entry)";
        }

        constexpr std::uint32_t kSitSleeping   = static_cast<std::uint32_t>(RE::SIT_SLEEP_STATE::kIsSleeping);
        constexpr std::uint32_t kSitWantToWake = static_cast<std::uint32_t>(RE::SIT_SLEEP_STATE::kWantToWake);

        TaskResult OnWokenEventTask(const WokenCopy& w)
        {
            const auto& cfg = Settings::Get();
            if (w.gen != g_generation.load() || !g_gameReady.load()) return { false, "no: the game is loading" };
            auto*             a   = w.domPerk || w.by == WokenBy::kTalk ? RE::TESForm::LookupByID<RE::Actor>(w.target) : nullptr;
            const std::string who = a ? Who(a, w.target) : std::format("0x{:08X}", w.target);
            if (cfg.logWokenProbe && !w.test) {
                if (w.by == WokenBy::kTalk)
                    logger::info("[probe] TESActivateEvent the player -> {} | her sit-sleep state {} ({}) | you sneaking {}", who, w.state,
                                 SitSleepName(w.state), w.sneaking ? "YES" : "no");
                else {
                    auto*       pf   = w.perk ? RE::TESForm::LookupByID(w.perk) : nullptr;
                    auto*       pk   = pf ? pf->As<RE::BGSPerk>() : nullptr;
                    const char* pn   = pk && pk->GetName() && *pk->GetName() ? pk->GetName() : "(no name)";
                    logger::info("[probe] TESPerkEntryRunEvent perk 0x{:08X} ({}) | flag {} | cause {} | target {}",
                                 w.perk, pk ? std::format("a real perk: {}{}", pn, w.domPerk ? " = Diary of Mine's DOMInteractPerk" : "")
                                            : std::string("NOT a perk form - the assumed layout (+0x10) may be wrong"),
                                 w.flag, w.causeIsPlayer ? "= the player (pointer compare)" : "not the player",
                                 w.domPerk ? std::format("{} (sit-sleep state {} {})", who, w.state, SitSleepName(w.state))
                                           : std::string("not read (another perk)"));
                }
            }
            if (w.by == WokenBy::kDomWake && !w.domPerk) return { false, "probe only (another perk)" };
            if (!a) return { false, "no: no such NPC" };
            // a Diary of Mine action other than Wake: she is DoM's now - her pending wake is cancelled (no "to talk")
            if (w.by == WokenBy::kDomWake && w.frag != kDomFragWake) {
                bool had = false;
                {
                    std::scoped_lock lk(g_mtx);
                    had = g_woken.erase(w.target) != 0;
                }
                if (had && cfg.logWoken)
                    logger::info("[woken] {}: Diary of Mine's {} picked by the player - her pending wake is cancelled (DoM's) - nothing said", who,
                                 DomFragName(w.frag));
                return { false, std::format("Diary of Mine's {} - no 'woken' line", DomFragName(w.frag)) };
            }
            // ---- reads, no lock of ours
            const bool  dead    = sbko::IsDeadNow(a);
            const bool  npc     = IsNPC(a);
            const bool  has3D   = a->Get3D() != nullptr;
            // m34b: NO IsHostileToActor here (not proven safe on a VR job thread; rule: test hostility from Papyrus in a VR plugin) -
            // SBKO_Helper.NarrateWoken refuses an NPC hostile to the player (Papyrus Actor.IsHostileToActor); 'sbko sleepstate' still reads it
            const bool  asleep  = w.state == kSitSleeping || w.state == kSitWantToWake;
            const char* byText  = w.by == WokenBy::kTalk ? "the player talked to her (activated her)" : "the player picked Diary of Mine's Wake";
            std::string why, line;
            bool        pending = false, tagged = false, candidate = asleep || w.by == WokenBy::kDomWake || w.test;
            {
                std::scoped_lock lk(g_mtx);
                const auto it = g_map.find(w.target);
                if (it != g_map.end() && (Held(it->second.state) || it->second.state == State::kDown ||
                                          (it->second.state == State::kComingTo && it->second.awaitingWake))) {
                    Entry& e = it->second;
                    if (w.by == WokenBy::kDomWake && e.sleepMark > 0) {
                        e.wokeByPlayer = true;
                        tagged         = true;
                        candidate      = true;
                    } else
                        why = std::format("SBKO holds her ({}{}) - {}", StateName(e.state),
                                          e.cls == kClsChoke ? ", choked" : e.hitKO ? ", a hit knock-out" : "",
                                          w.by == WokenBy::kDomWake ? "DoM's Wake does not wake an injured / choked NPC (too injured) - nothing said"
                                                                    : "not a natural sleeper");
                } else if (dead)
                    why = "dead";
                else if (!npc)
                    why = "not an NPC (a creature)";
                else if (!has3D)
                    why = "not loaded";
                else if (w.by == WokenBy::kTalk && w.sneaking)
                    why = "you were sneaking (a pickpocket - she sleeps on)";
                else if (!asleep)
                    why = std::format("not asleep in a bed at the event (sit-sleep state {}: {})", w.state, SitSleepName(w.state));
                else if (!cfg.wokenNarration)
                    why = "bWokenNarration 0";
                else if (w.by == WokenBy::kTalk && !cfg.wokenOnTalk)
                    why = "bWokenOnTalk 0";
                else if (w.by == WokenBy::kDomWake && !cfg.wokenOnDomWake)
                    why = "bWokenOnDomWake 0";
                else {
                    auto [wit, isNew] = g_woken.try_emplace(w.target);
                    if (isNew || !(wit->second.by == WokenBy::kDomWake && w.by == WokenBy::kTalk))   // DoM's Wake overrides a talk, not the reverse
                        wit->second = Woken{ .by = w.by, .checks = 0, .state = w.state, .who = who };
                    pending = true;
                }
            }
            if ((cfg.logWoken && candidate) || w.test) {
                if (tagged)
                    logger::info("[woken] {}: {} - a Sleep-spell sleeper SBKO holds: her wake (another mod's Wake = the sleeper's wake) is narrated "
                                 "'<player> has just woken <NPC> from a deep sleep.'", who, byText);
                else if (pending)
                    logger::info("[woken] {} {} while asleep (sit-sleep state {}: {}){} - pending (confirmed at the next unpaused check)", who, byText,
                                 w.state, SitSleepName(w.state), w.test ? std::format(" [console test '{}']", w.test) : std::string());
                else
                    logger::info("[woken] {}: {} - no 'woken' line: {}", who, byText, why);
            }
            return { pending || tagged, tagged ? std::string("her wake will say you woke her") : pending ? std::string("pending") : "no: " + why };
        }

        // =============================================================================================================================
        // u6b (U5): THE CHANCE KNOCK-OUT OF A NORMAL NPC (any hit that leaves an NPC under 15 % HP has a knock-out
        // chance: an NPC whose HP drops to 5 % gets a KO roll first, then goes into bleedout; the armour formula makes mages and
        // archers easier to knock out than a fighter in full plate; if HP reaches 0, she is
        // dead). The damage hook (DamageHook, below) copies a qualifying hit; OnChanceKOTask rolls; PickBleedout decides a bleedout.
        // =============================================================================================================================
        enum class HitKind : std::uint8_t
        {
            kWeapon   = 0,   // a melee weapon (named by the hit event this thread sent just before the damage)
            kArrow    = 1,   // a bow / crossbow
            kBash     = 2,   // the hit event's bash bit (shield, pommel, bow, torch)
            kCreature = 3,   // a creature's own attack (the "unarmed" weapon of a non-NPC: a claw, a bite)
            kPhysical = 4,   // a physical hit with no hit event on this thread, a weapon in the attacker's hands
            kSpell    = 5,   // a spell's impact (an instant effect: firebolt, fireball, ice spike, lightning bolt ...)
            kBeam     = 6,   // a concentration spell held on her (Flames, Frostbite, Sparks ...) - one roll per beam
        };

        const char* HitKindName(HitKind a_k)
        {
            switch (a_k) {
                case HitKind::kWeapon:   return "a weapon";
                case HitKind::kArrow:    return "an arrow / bolt";
                case HitKind::kBash:     return "a bash";
                case HitKind::kCreature: return "a creature's attack";
                case HitKind::kPhysical: return "a weapon hit (no hit event on this thread; a weapon in the attacker's hands)";
                case HitKind::kSpell:    return "a spell's impact";
                case HitKind::kBeam:     return "a concentration beam";
            }
            return "?";
        }

        // what the damage hook copies for the roll task - plain values only
        struct RollCopy
        {
            RE::FormID    victim   = 0;
            RE::FormID    attacker = 0;
            RE::FormID    source   = 0;      // the weapon / spell form (the log only)
            std::uint32_t serial   = 0;
            std::uint32_t gen      = 0;
            HitKind       kind     = HitKind::kWeapon;
            float         damage   = 0.0f;   // this hit's Health loss
            float         cur      = 0.0f;   // her Health after it (at the hook)
            float         max      = 0.0f;   // her max (permanent + temporary) at the hook
            std::uint8_t  console  = 0;      // u6h: 0 = a real hit (the hook); 1 = 'sbko roll' (DRY: a won roll is NOT applied); 2 = 'sbko roll ko'
        };

        // thread_local: the roll's dice (each thread its own generator - no lock; seeded once per thread)
        float Roll100()
        {
            thread_local std::mt19937 rng{ std::random_device{}() };
            return std::uniform_real_distribution<float>(0.0f, 100.0f)(rng);
        }

        // a worn piece's class: 2 heavy, 1 light, 0 clothing / cloth (counts as "none" for boots and gloves, "clothes" for the body)
        int ArmorRank(const RE::TESObjectARMO* a_armo)
        {
            switch (a_armo->GetArmorType()) {
                case RE::BGSBipedObjectForm::ArmorType::kHeavyArmor: return 2;
                case RE::BGSBipedObjectForm::ArmorType::kLightArmor: return 1;
                default:                                              return 0;
            }
        }

        // The formula, for one victim and attacker. Reads only: one walk of her worn inventory (never initialised from here), both
        // levels (Actor::GetLevel, RELOCATION_ID 36344 / 37334, VR 0x5DE910). Devious Devices items (zad_Lockable / zad_InventoryDevice)
        // are not armour. Helmet = u6f rule A (HelmetRuleA, U6's test: slot 30 / 31 + Light / Heavy). Boots = the feet slot (37), gloves = the hands slot (33), body =
        // the body slot (32); an item covering several slots counts for each; two items on one slot = the heavier.
        struct Factors
        {
            float       chance = 0.0f;   // clamped 0..100
            std::string text;            // every term with its item and points (the log)
        };

        Factors ReadFactors(RE::Actor* a_victim, RE::Actor* a_attacker, float a_damage, float a_max)
        {
            const auto& cfg = Settings::Get();
            bool        helmet = false;
            int         feet = -1, hands = -1, body = -1;
            std::string helmetWhat, helmetNot, feetWhat, handsWhat, bodyWhat;   // u6f: helmetNot = head-ish pieces that did not count
            int         ddSkipped = 0;
            auto        name = [](const RE::TESObjectARMO* a) { return ArmoName(a); };
            if (auto* changes = a_victim->GetInventoryChanges(true); changes && changes->entryList) {
                for (const auto* entry : *changes->entryList) {
                    if (!entry || !entry->object || !entry->object->IsArmor() || !entry->IsWorn()) continue;
                    const auto* armo = entry->object->As<RE::TESObjectARMO>();
                    if (!armo) continue;
                    // u6f: helmet rule A (U6's own helper) - also the one place that recognises a Devious Devices item here
                    const HelmetWhy hw = HelmetRuleA(armo);
                    if (hw == HelmetWhy::kDevice) {
                        ++ddSkipped;   // a Devious Devices item is not armour
                        continue;
                    }
                    if (hw == HelmetWhy::kHelmet) {
                        if (!helmet) helmetWhat = HelmetText(armo, hw);
                        helmet = true;
                    } else if (std::string t = HelmetText(armo, hw); !t.empty()) {
                        helmetNot += (helmetNot.empty() ? "" : "; ") + t;
                    }
                    const int rank = ArmorRank(armo);
                    auto      take = [&](RE::BGSBipedObjectForm::BipedObjectSlot a_slot, int& a_best, std::string& a_what) {
                        if (armo->HasPartOf(a_slot) && rank > a_best) {
                            a_best = rank;
                            a_what = name(armo);
                        }
                    };
                    take(RE::BGSBipedObjectForm::BipedObjectSlot::kFeet, feet, feetWhat);
                    take(RE::BGSBipedObjectForm::BipedObjectSlot::kHands, hands, handsWhat);
                    take(RE::BGSBipedObjectForm::BipedObjectSlot::kBody, body, bodyWhat);
                }
            }
            const float pHelmet = helmet ? cfg.chanceKOHelmetOn : cfg.chanceKOHelmetOff;
            const float pFeet   = feet == 2 ? cfg.chanceKOFeetHeavy : feet == 1 ? cfg.chanceKOFeetLight : cfg.chanceKOFeetNone;
            const float pHands  = hands == 2 ? cfg.chanceKOHandsHeavy : hands == 1 ? cfg.chanceKOHandsLight : cfg.chanceKOHandsNone;
            const float pBody   = body == 2 ? cfg.chanceKOBodyHeavy : body == 1 ? cfg.chanceKOBodyLight : cfg.chanceKOBodyClothes;
            const int   vLevel  = a_victim->GetLevel();
            const int   aLevel  = a_attacker ? a_attacker->GetLevel() : -1;
            const float pLevel  = a_attacker ? std::clamp(static_cast<float>(aLevel - vLevel) * cfg.chanceKOPointsPerLevel,
                                                          -cfg.chanceKOLevelMax, cfg.chanceKOLevelMax)
                                             : 0.0f;
            const float dmgFrac = a_max > 0.0f ? std::clamp(a_damage / a_max, 0.0f, 1.0f) : 0.0f;
            const float pDamage = cfg.chanceKODamageMax * dmgFrac;
            Factors     f;
            f.chance = std::clamp(pHelmet + pFeet + pHands + pBody + pLevel + pDamage, 0.0f, 100.0f);
            auto piece = [](int a_rank, const char* a_none, const std::string& a_what) {
                return a_rank == 2   ? std::format("heavy ({})", a_what)
                       : a_rank == 1 ? std::format("light ({})", a_what)
                       : a_rank == 0 ? std::format("{} (cloth: {})", a_none, a_what)
                                     : std::string(a_none);
            };
            f.text = std::format("helmet {} {:+.0f} | boots {} {:+.0f} | gloves {} {:+.0f} | body {} {:+.0f} | level {} {:+.1f} | damage "
                                 "{:.1f}% of her max {:+.1f}{} | chance {:.1f}%",
                                 HelmetLogText(helmet, helmet ? std::format("({})", helmetWhat) : std::string(), helmetNot, "on", "off"),
                                 pHelmet,
                                 piece(feet, "none", feetWhat), pFeet, piece(hands, "none", handsWhat), pHands,
                                 piece(body, "clothes / naked", bodyWhat), pBody,
                                 a_attacker ? std::format("{} vs her {}", aLevel, vLevel) : std::string("(no attacker known)"), pLevel,
                                 dmgFrac * 100.0f, pDamage, ddSkipped ? std::format(" | {} Devious Devices item(s) not counted", ddSkipped) : "",
                                 f.chance);
            return f;
        }

        // the roll task's result -> her U5 record (the newest roll, the beam's, the bleedout entry that waits for it)
        void FinishRoll(RE::FormID a_id, std::uint32_t a_serial, RollState a_state, float a_chance, float a_value)
        {
            std::scoped_lock lk(g_u5Mtx);
            const auto       it = g_u5.find(a_id);
            if (it == g_u5.end()) return;
            U5Victim& v = it->second;
            if (v.rollSerial == a_serial) {
                v.rollState  = a_state;
                v.rollChance = a_chance;
                v.rollValue  = a_value;
            }
            for (auto& b : v.beams)   // m34b
                if (b.roll == a_serial) {
                    b.state  = a_state;
                    b.chance = a_chance;
                    b.value  = a_value;
                }
            if (v.entry == EntryKind::kRolled && v.entryRoll == a_serial) {
                v.entryState  = a_state;
                v.entryChance = a_chance;
                v.entryValue  = a_value;
            }
        }

        // U5: ONE qualifying hit, in an SKSE task (relayed by the damage hook; in VR the task may run on an engine job thread).
        // The same guards as U6 (reads with no lock of ours; SBKO's own lists under g_mtx, asked again for the knock-out), then the formula
        // and the roll. Won -> SBKO's own HIT knock-out (StartKnockOutU, hitKO: her Health at the knock-out is the hold, no 25 % cap, FDC
        // pre-mark); lost -> nothing (the game decides). One [chanceko] line per hit that reached here (bLogChanceKO).
        TaskResult OnChanceKOTask(const RollCopy& r)
        {
            const bool dry = r.console == 1;   // u6h: 'sbko roll' - everything but the knock-out
            if (r.gen != g_generation.load() || !g_gameReady.load()) {
                if (!dry) FinishRoll(r.victim, r.serial, RollState::kSkipped, -1.0f, -1.0f);
                return { false, "no roll: the game is loading" };
            }
            const auto& cfg = Settings::Get();
            auto*       a   = RE::TESForm::LookupByID<RE::Actor>(r.victim);
            auto*       af  = r.attacker ? RE::TESForm::LookupByID(r.attacker) : nullptr;
            auto*       att = af ? af->As<RE::Actor>() : nullptr;

            float       cur = 0.0f, mx = 0.0f;
            const float pct  = a ? HealthFractionMax(a, &cur, &mx) : 0.0f;
            const auto* st   = a ? a->AsActorState() : nullptr;
            const auto  life = st ? st->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
            TakenKind   takenKind = TakenKind::kNone;
            const char* taken = a ? TakenBy(a, takenKind) : nullptr;
            const char* fight = FistFightRunning();
            std::string why;
            if (!a)
                why = "she is gone";
            else if (sbko::IsDeadNow(a))
                why = "she died";
            else if (cur <= 0.0f)
                why = "her Health is at or below 0 now (death is the game's)";
            else if (!IsNPC(a) || IsGhost(a) || IsProtectedOrEssential(a))
                why = "not a normal NPC (now)";
            else if (!cfg.koFollowers && IsFollower(a))   // 2.1.0: a belt - the hook already drops them
                why = "a follower (bKnockOutFollowers 0)";
            else if (a->IsCommandedActor())   // u6d (F7): a belt - the hook already drops them
                why = "a summoned / commanded actor";
            else if (const char* cr = ChildOrRider(a))   // u6d fix 2: a belt - the hook already drops them
                why = cr;
            else if (st && st->IsUnconscious())
                why = "already unconscious";
            else if (taken)
                why = std::format("taken by another mod ({})", taken);
            else if (life == RE::ACTOR_LIFE_STATE::kReanimate || life == RE::ACTOR_LIFE_STATE::kRecycle)
                why = std::format("not a living NPC (life state {})", static_cast<int>(life));
            else if (g_brawlFaction && a->GetFactionRank(g_brawlFaction, false) >= 0)
                why = "in a brawl (DGIntimidateFaction)";
            else if (fight)
                why = fight;
            else if (!a->Get3D())
                why = "no 3D (not loaded)";
            else if (!Helper::Present())
                why = "SBKO_Helper.pex is missing";
            if (why.empty()) {
                std::scoped_lock lk(g_mtx);
                why = HeldWhy(r.victim);
            }

            // ---- the roll (only when nothing ruled her out)
            Factors f;
            float   value = -1.0f;
            bool    won   = false;
            if (why.empty()) {
                f     = ReadFactors(a, att, r.damage, r.max);
                value = Roll100();
                won   = value < f.chance;
            }

            // ---- the knock-out (SBKO's own hit knock-out path; asked again under g_mtx)
            const std::string who = a ? Who(a, r.victim) : std::format("0x{:08X}", r.victim);
            bool              knocked = false, fromBleed = false;
            std::string       koExtra, wonBut;
            std::vector<Call> calls;
            if (won && !dry) {
                const std::int32_t potions    = cfg.potionsOff ? PotionSpellsOn(a) : 0;
                const bool         guardSever = cfg.severActionsGuard && g_severLoaded && Helper::SeverPresent();   // asks the VM - not under g_mtx
                const std::string  ess        = EssFlags(a);
                const bool         foll       = IsFollower(a);
                fromBleed                     = IsBleedingOut(a);   // already kneeling: SetUnconscious first (the psc's bleedout order)
                std::scoped_lock lk(g_mtx);
                const auto       it = g_map.find(r.victim);
                if (std::string again = HeldWhy(r.victim); !again.empty())
                    wonBut = std::move(again);
                else {
                    Entry* const e = it != g_map.end() ? &it->second : &g_map[r.victim];
                    e->who   = who;
                    koExtra  = StartKnockOutU(*e, r.victim, potions, foll, ess, guardSever, fromBleed, true, calls);
                    e->hitKO  = true;   // after StartKnockOutU (its ResetEpisode clears both)
                    e->hitPct = pct;
                    SyncCount();
                    knocked = true;
                }
            }
            if (!dry)   // u6h: a dry console roll leaves her U5 record alone
                FinishRoll(r.victim, r.serial,
                           knocked ? RollState::kWon : (won || !why.empty()) ? RollState::kSkipped : RollState::kLost,
                           why.empty() ? f.chance : -1.0f, value);

            std::string result;
            if (cfg.logChanceKO || knocked || r.console) {
                const std::string attWho = att ? Who(att, r.attacker) : r.attacker ? std::format("0x{:08X} (gone)", r.attacker) : "(none)";
                const char*       srcName = "?";
                if (r.source)
                    if (const auto* sf = RE::TESForm::LookupByID(r.source); sf && sf->GetName() && *sf->GetName()) srcName = sf->GetName();
                result =
                    !why.empty()  ? "no roll: " + why
                    : knocked     ? std::format("rolled {:.1f} < {:.1f} -> KNOCKED OUT", value, f.chance)
                    : won && dry  ? std::format("rolled {:.1f} < {:.1f} -> WON (dry console test: NOT applied - 'sbko roll ko' applies a won "
                                                "roll)", value, f.chance)
                    : won         ? std::format("rolled {:.1f} < {:.1f} -> won, but not knocked out: {}", value, f.chance, wonBut)
                                  : std::format("rolled {:.1f} >= {:.1f} -> stays up (the game decides: she may kneel in bleedout or fight "
                                                "on)", value, f.chance);
                logger::info("[chanceko] {} <- {} ({}{}){} | HP {} after the hit ({:.1f} of {:.1f}; the hit took {:.1f}) | {}{}", who, attWho,
                             HitKindName(r.kind), r.source ? std::format(": {} 0x{:08X}", srcName, r.source) : std::string(),
                             r.console == 1   ? " [console test 'sbko roll': her CURRENT Health as the hit's result, the damage term 0, dry]"
                             : r.console == 2 ? " [console test 'sbko roll ko': her CURRENT Health as the hit's result, the damage term 0]"
                                              : "",
                             Pct1(r.max > 0.0f ? r.cur / r.max : 0.0f), r.cur, r.max, r.damage, why.empty() ? f.text + " | " : std::string(),
                             result);
            }
            if (knocked)
                logger::info("[KO] {} KNOCK-OUT BY A ROLL (U5: {:.1f}% chance won, {}) - KnockOutU dispatched ({}){}; her Health will be "
                             "held at her Health at the knock-out ({}; no {}% cap); Follower Death Chance roll: {}; no wake until it replies",
                             who, f.chance, HitKindName(r.kind),
                             fromBleed ? "she was already kneeling in bleedout: unconscious first, then Paralysis + shove"
                                       : "Paralysis + shove, then unconscious: not from bleedout",
                             koExtra, Pct1(pct), std::lround(cfg.unconsciousHealthFraction * 100.0f),
                             g_fdcRolledFaction ? "NONE (FDC_SBKO_RolledFaction rank 2 set before rank 10 - her Health is above 0)"
                                                : "- (FDC_SBKO_Patch.esp not loaded)");
            Run(calls);
            TaskResult out;
            out.acted  = knocked;
            out.what   = result.empty() ? (why.empty() ? std::string("rolled") : "no roll: " + why) : result;
            out.chance = why.empty() ? f.chance : -1.0f;
            out.roll   = value;
            out.won    = won;
            out.wonBut = wonBut;
            return out;
        }

        // U5: a NORMAL NPC in bleedout - what does her knock-out roll say? (Detect, before g_mtx; no lock of ours held.) The roll of the
        // hit that started this bleedout decides; a bleedout no rolled hit started rolls ONCE here; one a punch started never rolls.
        // The decision is kept for this bleedout (bleedSerial), so the check's scan, the grace and the event never roll it twice.
        // Writes its own log line (once per bleedout).
        BleedVerdict PickBleedout(RE::Actor* a_actor, RE::FormID a_id)
        {
            const auto&   cfg    = Settings::Get();
            EntryKind     kind   = EntryKind::kNone;
            RollState     state  = RollState::kNone;
            float         chance = -1.0f, value = -1.0f, dmg = 0.0f, mx = 0.0f;
            RE::FormID    attId  = 0;
            const char*   why    = "";
            std::uint32_t serial = 0;
            {
                std::scoped_lock lk(g_u5Mtx);
                U5Victim& v = g_u5[a_id];
                if (v.decided && v.decidedFor == v.bleedSerial)
                    return v.decidedKO ? BleedVerdict::kKnockOut : BleedVerdict::kActHurt;   // already decided (and logged) for this bleedout
                kind   = v.entry;
                state  = v.entryState;
                chance = v.entryChance;
                value  = v.entryValue;
                attId  = v.entryAttacker;
                dmg    = v.entryDamage;
                mx     = v.entryMax;
                why    = v.entryWhy;
                serial = v.bleedSerial;
            }
            auto decide = [&](bool a_ko) {
                std::scoped_lock lk(g_u5Mtx);
                U5Victim& v = g_u5[a_id];
                if (v.bleedSerial != serial) return;   // a newer bleedout was recorded meanwhile - that one is decided on its own
                v.decided    = true;
                v.decidedKO  = a_ko;
                v.decidedFor = serial;
            };
            const std::string who = Who(a_actor, a_id);
            if (const char* cr = ChildOrRider(a_actor); IsGhost(a_actor) || a_actor->IsCommandedActor() || cr) {   // u6d: F7 + fix 2
                decide(false);
                logger::info("[chanceko] {} in bleedout - acting hurt ({}: no knock-out roll); the game's bleedout", who,
                             IsGhost(a_actor) ? "a ghost" : a_actor->IsCommandedActor() ? "a summoned / commanded actor" : cr);
                return BleedVerdict::kActHurt;
            }
            switch (kind) {
                case EntryKind::kRolled:
                    switch (state) {
                        case RollState::kWon:
                            // u6d (F2): the roll task knocks her out BEFORE it marks the roll won, and PickBleedout is asked only
                            // for an NPC SBKO does not hold - so "won" here = that knock-out is no longer live (dropped / woken). Not a
                            // silent "nothing": this bleedout gets its one roll here.
                            why  = "the hit that put her there won its roll, but that knock-out is not live any more";
                            kind = EntryKind::kNotAHit;
                            break;
                        case RollState::kLost:
                            decide(false);
                            logger::info("[chanceko] {} in bleedout - ACTING HURT: the hit that put her there rolled {:.1f} >= {:.1f}% and lost "
                                         "(the game's bleedout: she kneels 5-15 s and gets up; SBKO does nothing)",
                                         who, value, chance);
                            return BleedVerdict::kActHurt;
                        case RollState::kSkipped:
                            decide(false);
                            logger::info("[chanceko] {} in bleedout - acting hurt: the hit that put her there was ruled out before its roll "
                                         "(its [chanceko] line says why); the game's bleedout",
                                         who);
                            return BleedVerdict::kActHurt;
                        default:
                            return BleedVerdict::kWait;   // the roll task has not run yet - the next look decides
                    }
                    break;   // u6d: only the won-but-not-live case gets here -> the roll below
                case EntryKind::kNoRoll:
                    decide(false);
                    logger::info("[chanceko] {} in bleedout - ACTING HURT, no roll: {}; the game's bleedout", who, why);
                    return BleedVerdict::kActHurt;
                case EntryKind::kNone:
                case EntryKind::kNotAHit:
                    break;
            }
            // ---- ONE roll at the bleedout (no rolled hit started it)
            auto*         af  = attId ? RE::TESForm::LookupByID(attId) : nullptr;
            auto*         att = af ? af->As<RE::Actor>() : nullptr;
            if (mx <= 0.0f) mx = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
            const Factors f   = ReadFactors(a_actor, att, dmg, mx);
            const float   r   = Roll100();
            const bool    won = r < f.chance;
            decide(won);
            if (cfg.logChanceKO || won)
                logger::info("[chanceko] {} ROLLED AT HER BLEEDOUT (no rolled hit started it: {}){} | {} | rolled {:.1f} {} {:.1f} -> {}", who,
                             kind == EntryKind::kNone ? "no damage of hers was seen starting it" : why,
                             att ? std::format(" | by {}", Who(att, attId)) : std::string(), f.text, r, won ? "<" : ">=", f.chance,
                             won ? "KNOCKED OUT (SBKO's bleedout knock-out)"
                                 : "ACTING HURT (the game's bleedout: she kneels 5-15 s and gets up; SBKO does nothing)");
            return won ? BleedVerdict::kKnockOut : BleedVerdict::kActHurt;
        }

        // ---- the damage hook --------------------------------------------------------------------------------------------------
        // What ONE damage on a Character was (decided before the game's own HandleHealthDamage body runs).
        struct HhdDecision
        {
            bool          seen     = false;               // a Health loss the hook looked at
            EntryKind     kind     = EntryKind::kNotAHit;
            const char*   why      = "";                  // a string literal (the bleedout's log)
            std::uint32_t roll     = 0;                   // kRolled: the roll it queued or carries
            RE::FormID    attacker = 0;
            float         damage   = 0.0f;
            float         max      = 0.0f;
        };

        // u6d (F1) + u6e (a torture whip does 0 damage: anything that does 0 damage is excluded from knocking anyone out, so
        // other mods' whips and toy weapons do not count): a TOY weapon never rolls U5 and never knocks out by
        // U6 - one the game flags NonHostile or NotUsedInNormalCombat (weaponData.flags2; Injury Engine's Torture Padded Whip
        // 000803:Injury_Engine.esp carries both - DoM's DOMSlap 404724:DiaryOfMine.esm too, and mods' whips /
        // crops / floggers), or whose base damage is 0. A 0-damage HIT never counts either. The hand-to-hand weapon is NOT judged here
        // (MAG_Unarmed 0001F4 has damage 0, flags 0): bare hands keep their own rule (a person's: never; a creature's: rolls).
        // (On VR, Injury Engine's ProcessHitData hook is skipped, so a lash's damage reaches HandleHealthDamage.)
        constexpr const char* kWhyNonHostile = "non-hostile weapon (torture / slap)";
        constexpr const char* kWhyNotCombat  = "weapon not used in normal combat (a toy / tool)";
        constexpr const char* kWhyNoDamage   = "weapon does no damage";
        constexpr const char* kWhyZeroHit    = "a 0-damage hit";

        // the reasons that get their ONE log line per hit (string literals - compared by address)
        bool IsToyWhy(const char* a_why)
        {
            return a_why == kWhyNonHostile || a_why == kWhyNotCombat || a_why == kWhyNoDamage || a_why == kWhyZeroHit;
        }

        // nullptr = an ordinary weapon (or none, or the hand-to-hand one)
        const char* ToyWeapon(const RE::TESObjectWEAP* a_w)
        {
            if (!a_w || a_w->IsHandToHandMelee()) return nullptr;
            if (a_w->weaponData.flags2.any(RE::TESObjectWEAP::Data::Flag2::kNonHostile)) return kWhyNonHostile;
            if (a_w->weaponData.flags2.any(RE::TESObjectWEAP::Data::Flag2::kNotUsedInNormalCombat)) return kWhyNotCombat;
            if (a_w->attackDamage == 0) return kWhyNoDamage;   // TESAttackDamageForm::attackDamage (unsigned: "<= 0" is 0)
            return nullptr;
        }

        // u6d: what a PHYSICAL damage was - from the hit event this thread sent just before it (a_hit; target 0 = none matched) or, without
        // one, the attacker's hands. nullptr = it may roll (a_kind / a_source set); else the reason it never rolls (a string literal).
        const char* PhysicalKind(const HitTls& a_hit, RE::Actor* a_attacker, HitKind& a_kind, RE::FormID& a_source)
        {
            const bool               matched = a_hit.target != 0;
            const RE::TESForm*       sf      = matched && a_hit.source ? RE::TESForm::LookupByID(a_hit.source) : nullptr;
            const RE::TESObjectWEAP* weap    = sf ? sf->As<RE::TESObjectWEAP>() : nullptr;
            if (matched) a_source = a_hit.source;
            if (const char* toy = ToyWeapon(weap)) return toy;   // F1 / u6e: the event's weapon (a whip lash, a slap - its bash too)
            if (matched && (a_hit.flags & kHitBash)) {
                a_kind = HitKind::kBash;
                return nullptr;
            }
            if (sf && !weap && sf->IsMagicItem()) {   // F3: a spell's physical blast (an explosion) - a spell, never a punch
                a_kind = HitKind::kSpell;
                return nullptr;
            }
            bool unarmed = false;
            if (weap)
                unarmed = weap->IsHandToHandMelee();
            else {
                // no hit event (or no weapon in it): her attacker's hands - an ordinary, non-hand-to-hand weapon = a weapon hit; F1 / u6e:
                // a TOY in a hand is not "armed", and hands with only toys = that toy's hit (no roll)
                bool        armed  = false;
                const char* toyWhy = nullptr;
                RE::FormID  toyId  = 0;
                for (const bool left : { false, true }) {
                    auto* f = a_attacker->GetEquippedObject(left);
                    auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
                    if (const char* t = ToyWeapon(w)) {
                        if (!toyWhy) {
                            toyWhy = t;
                            toyId  = w->GetFormID();
                        }
                    } else if (w && !w->IsHandToHandMelee())
                        armed = true;
                }
                if (!armed && toyWhy) {
                    a_source = toyId;
                    return toyWhy;
                }
                unarmed = !armed;
            }
            if (unarmed) {
                if (IsNPC(a_attacker)) return "a punch (bare hands) - punches never knock out";   // the U6 punch removal (the player too)
                if (!Settings::Get().chanceKOCreatureHits) return "a creature's attack (bChanceKOCreatureHits 0)";
                a_kind = HitKind::kCreature;
                return nullptr;
            }
            a_kind = !matched ? HitKind::kPhysical : (weap && (weap->IsBow() || weap->IsCrossbow())) ? HitKind::kArrow : HitKind::kWeapon;
            return nullptr;
        }

        // u6d (F1) / u6e: ONE log line for a toy-weapon hit or a 0-damage hit that left a normal NPC at or under the line (no roll)
        // - a task, never in the hook. Plain values only (a_why = one of the kWhy literals).
        void LogNoRollTask(RE::FormID a_victim, RE::FormID a_attacker, RE::FormID a_source, float a_cur, float a_max, float a_damage,
                           const char* a_why)
        {
            if (!Settings::Get().logChanceKO) return;
            auto*       a   = RE::TESForm::LookupByID<RE::Actor>(a_victim);
            auto*       af  = a_attacker ? RE::TESForm::LookupByID(a_attacker) : nullptr;
            auto*       att = af ? af->As<RE::Actor>() : nullptr;
            const char* src = "?";
            if (const auto* sf = a_source ? RE::TESForm::LookupByID(a_source) : nullptr; sf && sf->GetName() && *sf->GetName()) src = sf->GetName();
            logger::info("[chanceko] {} <- {} ({} 0x{:08X}) | HP {} after the hit ({:.1f} of {:.1f}; the hit took {:.1f}) | no roll: {}",
                         a ? Who(a, a_victim) : std::format("0x{:08X}", a_victim),
                         att ? Who(att, a_attacker) : std::format("0x{:08X}", a_attacker), src, a_source,
                         Pct1(a_max > 0.0f ? a_cur / a_max : 0.0f), a_cur, a_max, a_damage, a_why);
        }

        // u6h: the damage hook's WHO filter (it was inline in ClassifyDamage - same tests, same order, same reasons), shared with the console
        // test 'sbko roll' so the test asks exactly what the hook asks. nullptr = a normal NPC the hook rolls for; else why not (a literal).
        // Any game thread; takes only g_watchMtx (Watched), for a set lookup.
        const char* U5VictimOut(RE::Actor* a_this, RE::FormID a_id)
        {
            const auto* st = a_this->AsActorState();
            if (sbko::IsDeadNow(a_this) || !IsNPC(a_this) || IsProtectedOrEssential(a_this) || IsGhost(a_this)) return "not a normal NPC";
            if (!Settings::Get().koFollowers && IsFollower(a_this)) return "a follower (bKnockOutFollowers 0)";   // 2.1.0
            if (a_this->IsCommandedActor()) return "a summoned / commanded actor";   // u6d (F7)
            if (ChildOrRider(a_this)) return "a child, or riding a mount";           // u6d fix 2
            if (st && st->IsUnconscious()) return "unconscious";
            if (Watched(a_id)) return "SBKO holds her";
            return nullptr;
        }

        // Any game thread (main: weapons; AI workers: magic; Papyrus: DamageActorValue). Cheap first: attacker, then her Health against
        // the line, then the who-filters; only then the kind of hit and the relay. Takes only g_u5Mtx / g_watchMtx (each alone, for a map
        // / set operation); no log line, no Papyrus, no AddTask. a_hit = the hit event this thread sent for THIS damage (target 0 = none;
        // the thunk consumed it - u6d F4).
        void ClassifyDamage(RE::Actor* a_this, RE::Actor* a_attacker, float a_amount, const HitTls& a_hit, HhdDecision& d)
        {
            d.seen     = true;
            d.damage   = a_amount;
            d.attacker = a_attacker ? a_attacker->GetFormID() : 0;
            const RE::ValueModifierEffect* const ae  = (t_effect && t_effectTarget == a_this) ? t_effect : nullptr;
            const RE::FormID                     id  = a_this->GetFormID();
            const RE::FormID                     att = d.attacker;
            if (!ae) {
                // u6d (F2): a PHYSICAL damage is THIS thread's swing on her - an enchantment / poison right after it belongs to it
                const bool matched = a_attacker && a_hit.target == id && a_hit.cause == att;
                t_swing = (a_attacker && a_attacker != a_this) ? SwingTls{ .victim = id, .attacker = att, .hit = matched ? a_hit : HitTls{} }
                                                               : SwingTls{};
            }
            if (!a_attacker) {
                d.why = "no attacker (a fall, a trap, a script, a non-hostile effect)";
                return;
            }
            if (a_attacker == a_this) {
                d.why = "her own damage";
                return;
            }
            auto* avo = a_this->AsActorValueOwner();
            if (!avo) return;
            const float cur = avo->GetActorValue(RE::ActorValue::kHealth);
            const float mx  = a_this->GetActorValueMax(RE::ActorValue::kHealth);
            d.max           = mx;
            if (cur <= 0.0f || mx <= 0.0f) {
                d.why = "her Health went to 0 or below (death is the game's)";
                return;
            }
            const auto& cfg = Settings::Get();
            if (cur > cfg.chanceKOHealthBelow * mx) {
                d.why = "her Health after the hit was above fChanceKOHealthBelow";
                return;
            }
            // ---- from here: a hit that left her at or under the line (rare) - measured
            const auto t0 = std::chrono::steady_clock::now();
            g_perfDmgLow.fetch_add(1, std::memory_order_relaxed);
            struct Timer
            {
                std::chrono::steady_clock::time_point t;
                ~Timer() { g_perfDmgLowNs.fetch_add(NsSince(t), std::memory_order_relaxed); }
            } timer{ t0 };
            // u6h: the who-filter moved into U5VictimOut (unchanged tests and reasons) - the console's 'sbko roll' asks the same one
            if (const char* out = U5VictimOut(a_this, id)) {
                d.why = out;
                return;
            }

            // ---- the kind of hit
            HitKind    kind      = HitKind::kWeapon;
            RE::FormID source    = 0;
            bool       swingRoll = false;   // this roll is THIS thread's swing's (an enchantment / poison on it carries it)
            // a physical swing that can never roll: remember why (its enchantment / poison carries it); F1: one line for a non-hostile one
            auto noRoll = [&](const char* a_why) {
                t_swing.noRollWhy = a_why;
                d.kind            = EntryKind::kNoRoll;
                d.why             = a_why;
                if (IsToyWhy(a_why)) {   // F1 / u6e: a toy / a 0-damage swing - its one line
                    const RE::FormID src = source;
                    TaskRelay::Add([id, att, src, cur, mx, a_amount, a_why] { LogNoRollTask(id, att, src, cur, mx, a_amount, a_why); });
                }
            };
            if (ae) {
                const RE::MagicItem* spell = ae->spell;
                source                     = spell ? spell->GetFormID() : 0;
                if (spell && g_tickSpells.contains(source)) {
                    d.why = "a cloak's or a hazard's damage (a tick, not a hit)";
                    return;
                }
                const auto stype = spell ? spell->GetSpellType() : RE::MagicSystem::SpellType::kSpell;
                if (stype == RE::MagicSystem::SpellType::kEnchantment || stype == RE::MagicSystem::SpellType::kPoison) {
                    // u6d (F2): a weapon enchantment / poison is part of the swing it came with - THIS thread's last physical damage
                    // on her by the same attacker - never of an older roll. Over time (a poison's duration) = a tick, never a carry.
                    if (ae->ShouldModifyOnUpdate()) {
                        d.why = "a poison / enchantment over time (a tick, not a hit)";
                        return;
                    }
                    if (t_swing.victim != id || t_swing.attacker != att) {
                        d.why = "a weapon enchantment / poison with no weapon hit before it on this thread";
                        return;
                    }
                    if (t_swing.noRollWhy) {   // its swing never rolls (a punch, a non-hostile weapon) - neither does this
                        d.kind = EntryKind::kNoRoll;
                        d.why  = t_swing.noRollWhy;
                        return;
                    }
                    if (t_swing.roll) {   // its swing rolled - this carries THAT roll
                        d.kind = EntryKind::kRolled;
                        d.roll = t_swing.roll;
                        d.why  = "a weapon enchantment / poison on the hit that rolled";
                        return;
                    }
                    // its swing stayed above the line and this brought her under it: the SWING's roll, now (once - recorded in t_swing)
                    if (const char* why = PhysicalKind(t_swing.hit, a_attacker, kind, source)) {
                        noRoll(why);
                        return;
                    }
                    swingRoll = true;
                } else if (spell && spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    kind = HitKind::kBeam;
                    std::scoped_lock lk(g_u5Mtx);
                    if (const auto it = g_u5.find(id); it != g_u5.end()) {
                        for (const auto& b : it->second.beams)   // m34b: any beam already counted on her
                            if (b.roll && b.effect == ae && b.uid == ae->usUniqueID && b.spell == source) {
                                d.kind = EntryKind::kRolled;   // the same beam: its one roll
                                d.roll = b.roll;
                                d.why  = "the same concentration beam (one roll per beam)";
                                return;
                            }
                    }
                } else if (ae->ShouldModifyOnUpdate()) {
                    // the engine's own per-frame test (VME slot 0x1C): duration > 0 / disease / ability = a damage-over-time tick
                    d.why = "a damage-over-time tick (burning, poison, linger ...)";
                    return;
                } else
                    kind = HitKind::kSpell;
            } else {
                if (const char* why = PhysicalKind(t_swing.hit, a_attacker, kind, source)) {
                    noRoll(why);
                    return;
                }
                swingRoll = true;
            }

            // ---- queue the roll
            std::uint32_t serial = g_u5RollSerial.fetch_add(1, std::memory_order_relaxed) + 1;
            if (serial == 0) serial = g_u5RollSerial.fetch_add(1, std::memory_order_relaxed) + 1;   // 0 = "none" (after 4 G rolls)
            {
                std::scoped_lock lk(g_u5Mtx);
                U5Victim& v    = g_u5[id];
                v.rollSerial   = serial;
                v.rollAttacker = att;
                v.rollState    = RollState::kPending;
                v.rollChance   = -1.0f;
                v.rollValue    = -1.0f;
                if (kind == HitKind::kBeam)   // m34b: one more beam in her set
                    v.beams.push_back(U5Victim::Beam{ .effect = ae, .uid = ae->usUniqueID, .spell = source, .roll = serial,
                                                      .state = RollState::kPending });
            }
            if (swingRoll) t_swing.roll = serial;
            if (kind == HitKind::kPhysical) g_perfDmgPhysical.fetch_add(1, std::memory_order_relaxed);   // F3: a flood would show
            d.kind = EntryKind::kRolled;
            d.roll = serial;
            d.why  = "a hit that rolled";
            g_perfDmgRolls.fetch_add(1, std::memory_order_relaxed);
            const RollCopy rc{ .victim = id, .attacker = att, .source = source, .serial = serial, .gen = g_generation.load(),
                               .kind = kind, .damage = a_amount, .cur = cur, .max = mx };
            TaskRelay::Add([rc] { OnChanceKOTask(rc); });
        }

        // u6e (anything that does 0 damage is excluded): a PHYSICAL hit whose HandleHealthDamage delta is 0 (a hit that did no damage - e.g.
        // another mod zeroed it) never rolls, and neither does a weapon enchantment / poison riding on that swing (t_swing carries the
        // reason). One line when it would have mattered (a normal NPC at or under the line). Same thread rules as ClassifyDamage.
        void NoteZeroHit(RE::Actor* a_this, RE::Actor* a_attacker, const HitTls& a_hit)
        {
            if (!a_attacker || a_attacker == a_this || (t_effect && t_effectTarget == a_this)) return;   // magic: not a swing
            const RE::FormID id      = a_this->GetFormID();
            const RE::FormID att     = a_attacker->GetFormID();
            const bool       matched = a_hit.target == id && a_hit.cause == att;
            t_swing = SwingTls{ .victim = id, .attacker = att, .noRollWhy = kWhyZeroHit, .hit = matched ? a_hit : HitTls{} };
            auto* avo = a_this->AsActorValueOwner();
            if (!avo || !Settings::Get().logChanceKO) return;
            const float cur = avo->GetActorValue(RE::ActorValue::kHealth);
            const float mx  = a_this->GetActorValueMax(RE::ActorValue::kHealth);
            if (cur <= 0.0f || mx <= 0.0f || cur > Settings::Get().chanceKOHealthBelow * mx) return;
            if (sbko::IsDeadNow(a_this) || !IsNPC(a_this) || IsProtectedOrEssential(a_this) || IsGhost(a_this) || a_this->IsCommandedActor() ||
                ChildOrRider(a_this) || Watched(id))
                return;
            const RE::FormID src = matched ? a_hit.source : 0;
            TaskRelay::Add([id, att, src, cur, mx] { LogNoRollTask(id, att, src, cur, mx, 0.0f, kWhyZeroHit); });
        }

        // the damage that just put her into bleedout (lifeState went to bleedout inside the game's body) - recorded for PickBleedout.
        // Rare (one per bleedout); normal NPCs only.
        void RecordBleedEntry(RE::Actor* a_this, const HhdDecision& d)
        {
            if (!IsNPC(a_this) || IsProtectedOrEssential(a_this)) return;
            const float mx = d.max > 0.0f ? d.max : a_this->GetActorValueMax(RE::ActorValue::kHealth);
            g_perfDmgEntries.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lk(g_u5Mtx);
            U5Victim& v = g_u5[a_this->GetFormID()];
            if (++v.bleedSerial == 0) v.bleedSerial = 1;
            v.entry         = d.kind;
            v.entryRoll     = d.roll;
            v.entryState    = RollState::kNone;
            v.entryChance   = -1.0f;
            v.entryValue    = -1.0f;
            v.entryAttacker = d.attacker;
            v.entryDamage   = d.damage;
            v.entryMax      = mx;
            v.entryWhy      = d.why;
            v.decided       = false;
            v.decidedKO     = false;
            if (d.kind == EntryKind::kRolled) {
                // the roll's state as known NOW (the task may even have finished already); a carried roll no longer known = roll here
                if (d.roll == v.rollSerial) {
                    v.entryState  = v.rollState;
                    v.entryChance = v.rollChance;
                    v.entryValue  = v.rollValue;
                } else if (const auto b = std::find_if(v.beams.begin(), v.beams.end(), [&](const U5Victim::Beam& x) { return x.roll == d.roll; });
                           d.roll && b != v.beams.end()) {   // m34b: the beam set
                    v.entryState  = b->state;
                    v.entryChance = b->chance;
                    v.entryValue  = b->value;
                } else {
                    v.entry    = EntryKind::kNotAHit;
                    v.entryWhy = "the roll this damage carried is no longer known";
                }
            }
        }

        // =============================================================================================================================
        // u7a U4: FOLLOWER DEATH CHANCE RE-ROLLS ON EVERY HIT (each hit is a re-roll for FDC to kill that NPC; only OTHER
        // people's hits, a miss = nothing). The damage hook sees a Health loss on an NPC on the U4 watch (a BLEEDOUT knock-out SBKO holds
        // unconscious) -> ClassifyFdcHit applies the U5 "direct hit" rules (no lock but g_u5Mtx for the beam map; no log, no Papyrus) and
        // relays -> OnFdcHitTask (an SKSE task: engine reads with no lock of ours, g_mtx only for her entry) asks FDC's conditions (rank 10,
        // FDC_SBKO_RolledFaction rank 1, the player in combat) -> SBKO_Helper.FdcHit (Papyrus) sends SBKO_FdcHit; the FDC x SBKO patch's
        // running effect on her re-rolls with FDC's own settings. One hit = one roll (no busy guard, no cap - by design).
        // =============================================================================================================================
        struct FdcHitCopy
        {
            const char*   test     = nullptr;   // a console test's label (a string literal) - nullptr = a real hit from the hook
            RE::FormID    victim   = 0;
            RE::FormID    attacker = 0;
            RE::FormID    source   = 0;         // the weapon / spell form (the log + the mod event's text)
            HitKind       kind     = HitKind::kWeapon;
            float         damage   = 0.0f;      // this hit's Health loss
            std::uint32_t gen      = 0;
        };

        TaskResult OnFdcHitTask(const FdcHitCopy& c)
        {
            if (c.gen != g_generation.load() || !g_gameReady.load()) return { false, "no re-roll: the game is loading" };
            const auto& cfg = Settings::Get();
            auto*       a   = RE::TESForm::LookupByID<RE::Actor>(c.victim);
            if (!a) return { false, "no re-roll: no such NPC" };
            auto*             player  = RE::PlayerCharacter::GetSingleton();
            auto*             af      = c.attacker ? RE::TESForm::LookupByID(c.attacker) : nullptr;
            auto*             att     = af ? af->As<RE::Actor>() : nullptr;
            auto*             sf      = c.source ? RE::TESForm::LookupByID(c.source) : nullptr;
            const std::string attName = att ? (att->IsPlayerRef() ? std::string("the player") : NameOf(att)) : std::format("0x{:08X}", c.attacker);
            const std::string srcName = sf && sf->GetName() && *sf->GetName() ? std::string(sf->GetName())
                                        : c.source                           ? std::format("0x{:08X}", c.source)
                                                                             : std::string("no weapon / spell form");
            // ---- reads, no lock of ours
            const bool         dead   = sbko::IsDeadNow(a);
            const std::int32_t rank   = g_faction ? a->GetFactionRank(g_faction, false) : -1;
            const std::int32_t mark   = g_fdcRolledFaction ? a->GetFactionRank(g_fdcRolledFaction, false) : -1;
            const bool         combat = player && player->IsInCombat();   // VR slot 0xE5 verified; FollowerBumpGuard uses it
            // m34b: FDC's effect is NOT read here any more (MagicTarget::HasMagicEffect walks her active-effect list - in VR this
            // task may run on a job thread right while the main thread changes that list for this very hit); 'sbko fdc' reads it on the console
            const bool         foll   = IsFollower(a);
            const std::string  who    = Who(a, c.victim);
            std::string        why;   // empty = FDC's re-roll is asked
            {
                std::scoped_lock lk(g_mtx);
                const auto       it = g_map.find(c.victim);
                if (it == g_map.end())
                    why = "SBKO does not hold her any more";
                else {
                    const Entry& e = it->second;
                    if (e.releasing)
                        why = "SBKO is letting her go to another mod";
                    else if (e.state != State::kUnconscious)
                        why = std::format("she is {} (not held unconscious)", StateName(e.state));
                    else if (e.cls == kClsChoke)
                        why = "a choke knock-out - her Health is above 0, it never re-rolls (mark 2)";
                    else if (e.sleepMark > 0)
                        why = "a Sleep-spell knock-out - it never re-rolls (mark 2)";
                    else if (e.hitKO)
                        why = "a hit knock-out - her Health is above 0, it never re-rolls (mark 2)";
                }
            }
            if (why.empty()) {
                if (dead)
                    why = "she died of this hit";
                else if (rank != kRankDown)
                    why = std::format("SBKO_StateFaction rank {} - not knocked out by SBKO", rank);
                else if (mark < 1)
                    why = std::format("FDC has not rolled for this knock-out yet (mark {}) - its own first roll comes first", mark);
                else if (mark >= 2)
                    why = std::format("mark {} = a hit / sleep / choke knock-out - it never re-rolls", mark);
                else if (!combat)
                    why = "you are not in combat (FDC's own gate - its effect is not running on her)";
            }
            bool              asked = false;
            const std::string hit   = std::format("{}|{}|{}|{:.0f}", attName, srcName, HitKindName(c.kind), c.damage);
            if (why.empty()) {
                asked = Helper::FdcHit(c.victim, hit, cfg.fdcRerollDeathOnly);   // no lock held (Papyrus)
                if (!asked) why = "SBKO_Helper.FdcHit was NOT dispatched (an older SBKO_Helper.pex?)";
            }
            // m34b: the line for a follower (or an asked re-roll / a console test) only - a protected town NPC SBKO holds after a
            // bleedout gets no FDC effect anyway, and a dragon attack would otherwise write one "no re-roll" line per hit on each of them
            if ((cfg.logFdcReroll && (foll || asked)) || c.test)
                logger::info("[fdc] {} <- {} ({}, {}, {:.1f} dmg){} | {} | SBKO rank {}, FDC mark {}, you are {}in combat -> {}",
                             who, attName, srcName, HitKindName(c.kind), c.damage,
                             c.test ? std::format(" [console test '{}': a simulated qualifying hit - the hook's who / what filter is skipped]", c.test)
                                    : std::string(),
                             foll ? "a follower" : "not a follower", rank, mark, combat ? "" : "NOT ",
                             asked ? std::format("FDC re-roll asked (SBKO_FdcHit, {}; FDC's effect on her - only running in a fight - rolls)",
                                                 cfg.fdcRerollDeathOnly ? "death only" : "FDC's full roll")
                                   : "no re-roll: " + why);
            return { asked, asked ? std::format("FDC re-roll asked ({})", cfg.fdcRerollDeathOnly ? "death only" : "full roll") : "no re-roll: " + why };
        }

        // Any game thread (the damage hook, on an NPC on the U4 watch): the U5 "direct hit" rules, no log, no Papyrus, no AddTask (TaskRelay
        // only); only g_u5Mtx for the beam map. a_hit = the hit event this thread sent for THIS damage (target 0 = none; the thunk consumed it).
        void ClassifyFdcHit(RE::Actor* a_this, RE::Actor* a_attacker, float a_amount, const HitTls& a_hit)
        {
            if (!a_attacker || a_attacker == a_this) return;   // attacker-less damage (a fall, a trap, a script, IE's lash) / her own
            if (a_attacker->IsPlayerRef() && !Settings::Get().fdcRerollPlayerHits) return;   // design rule: the player's hits never
            const RE::FormID                     id  = a_this->GetFormID();
            const RE::FormID                     att = a_attacker->GetFormID();
            const RE::ValueModifierEffect* const ae  = (t_effect && t_effectTarget == a_this) ? t_effect : nullptr;
            HitKind                              kind   = HitKind::kWeapon;
            RE::FormID                           source = 0;
            if (ae) {
                const RE::MagicItem* spell = ae->spell;
                source                     = spell ? spell->GetFormID() : 0;
                if (spell && g_tickSpells.contains(source)) return;   // a cloak's / a hazard's damage - a tick
                const auto stype = spell ? spell->GetSpellType() : RE::MagicSystem::SpellType::kSpell;
                if (stype == RE::MagicSystem::SpellType::kEnchantment || stype == RE::MagicSystem::SpellType::kPoison)
                    return;   // a weapon enchantment / poison: part of the swing that already counted (or a tick)
                if (spell && spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    kind = HitKind::kBeam;
                    std::scoped_lock lk(g_u5Mtx);
                    auto& beams = g_fdcBeam[id];   // m34b: every beam counted on her, not only the last one
                    if (std::any_of(beams.begin(), beams.end(), [&](const FdcBeam& b) {
                            return b.effect == ae && b.uid == ae->usUniqueID && b.spell == source;
                        }))
                        return;   // a beam already counted: its one re-roll
                    beams.push_back(FdcBeam{ ae, ae->usUniqueID, source });
                } else if (ae->ShouldModifyOnUpdate())
                    return;   // a damage-over-time tick
                else
                    kind = HitKind::kSpell;
            } else {
                const bool matched = a_hit.target == id && a_hit.cause == att;
                if (PhysicalKind(matched ? a_hit : HitTls{}, a_attacker, kind, source)) return;   // a punch, a toy, a creature (switch off)
            }
            const FdcHitCopy c{ .victim = id, .attacker = att, .source = source, .kind = kind, .damage = a_amount, .gen = g_generation.load() };
            TaskRelay::Add([c] { OnFdcHitTask(c); });
        }

        // Actor::HandleHealthDamage(attacker, delta) on Character (NPCs only - the player has its own vtable). Health is ALREADY lowered
        // when it runs (the actor-value core calls it after the write, VR +0x62A09F); `delta` < 0 is a loss. PRE-call: decide (and maybe
        // relay a roll); then the previous slot owner (Injury Engine's hook when it is installed, else the game: bleedout check, death);
        // POST-call: did this damage put her into bleedout? The damage itself is never changed.
        struct DamageHook
        {
            static void thunk(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
            {
                // u7a: the hook serves U5 and / or U4 (each decided once at kDataLoaded)
                const bool  u5 = g_u5On.load(std::memory_order_relaxed);
                const bool  u4 = g_fdcOn.load(std::memory_order_relaxed);
                auto* const st = (a_this && (u5 || u4)) ? a_this->AsActorState() : nullptr;
                if (!st) return func(a_this, a_attacker, a_damage);
                g_perfDmgCalls.fetch_add(1, std::memory_order_relaxed);
                const bool  wasBleeding = st->IsBleedingOut();
                // u6d (F4): ONE hit event serves ONE physical damage on her - taken (and cleared) here, used or not, so an event
                // never names a LATER damage's weapon. A magic damage leaves it alone (the vfunc 0x20 scope says magic).
                HitTls hit{};
                if (!(t_effect && t_effectTarget == a_this) && t_hit.target != 0 && t_hit.target == a_this->GetFormID()) {
                    hit   = t_hit;
                    t_hit = HitTls{};
                }
                // u7a (U4): an NPC SBKO holds after a bleedout knock-out (rare: an atomic load first, then one set lookup) - FDC's re-roll
                // rules, then the game's own body. U5 would drop her anyway ("SBKO holds her").
                if (u4 && a_damage < 0.0f && g_fdcWatchCount.load(std::memory_order_relaxed) != 0 && g_gameReady.load(std::memory_order_relaxed) &&
                    FdcWatched(a_this->GetFormID())) {
                    ClassifyFdcHit(a_this, a_attacker, -a_damage, hit);
                    return func(a_this, a_attacker, a_damage);
                }
                if (!u5) return func(a_this, a_attacker, a_damage);
                HhdDecision d;
                if (a_damage < 0.0f && g_gameReady.load(std::memory_order_relaxed))
                    ClassifyDamage(a_this, a_attacker, -a_damage, hit, d);
                else if (a_damage == 0.0f && g_gameReady.load(std::memory_order_relaxed))
                    NoteZeroHit(a_this, a_attacker, hit);   // u6e: a hit that did no damage never rolls (nor its enchantment)
                func(a_this, a_attacker, a_damage);
                if (d.seen && !wasBleeding && st->IsBleedingOut()) RecordBleedEntry(a_this, d);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // ValueModifierEffect::ModifyActorValue (vfunc 0x20, the same slot on SE / AE / VR) - only a scope: "this effect is applying a
        // value to this actor on this thread", so the damage hook can tell a spell (and which kind) from a weapon. One template per vtable
        // (each keeps its own previous function - Dual overrides it; Injury Engine's hooks on VME / Dual are chained, not replaced).
        // u6d (F8): before SBKO writes a vtable slot - the vtable must lie inside the GAME's image, and the slot must hold a pointer
        // into code of a LOADED MODULE (the game, or a DLL that hooked it first, e.g. Injury Engine). Anything else (null, a wrong address
        // from an ID table, garbage) = the hook is skipped and logged. a_where = where the current pointer lives (the log).
        bool SlotLooksSane(std::uintptr_t a_vtbl, std::size_t a_idx, std::string& a_where)
        {
            // the Win32 values by number (windows.h's macros of the same names are visible in this build)
            constexpr std::uint32_t kMemImage  = 0x01000000u;   // MEM_IMAGE
            constexpr std::uint32_t kMemCommit = 0x00001000u;   // MEM_COMMIT
            constexpr std::uint32_t kExec      = 0x10u | 0x20u | 0x40u | 0x80u;   // PAGE_EXECUTE | _READ | _READWRITE | _WRITECOPY
            const std::uintptr_t exe = REL::Module::get().base();
            REX::W32::MEMORY_BASIC_INFORMATION mbi{};
            if (!a_vtbl || REX::W32::VirtualQuery(reinterpret_cast<const void*>(a_vtbl), &mbi, sizeof(mbi)) == 0 ||
                reinterpret_cast<std::uintptr_t>(mbi.allocationBase) != exe || mbi.state != kMemCommit) {
                a_where = std::format("the vtable 0x{:X} is NOT inside the game's image", a_vtbl);
                return false;
            }
            const std::uintptr_t fn = *reinterpret_cast<const std::uintptr_t*>(a_vtbl + a_idx * sizeof(void*));
            mbi = {};
            if (!fn || REX::W32::VirtualQuery(reinterpret_cast<const void*>(fn), &mbi, sizeof(mbi)) == 0 ||
                mbi.state != kMemCommit || mbi.type != kMemImage || (mbi.protect & kExec) == 0) {
                a_where = std::format("slot 0x{:X} holds 0x{:X} - NOT code of a loaded module", a_idx, fn);
                return false;
            }
            const auto mod = reinterpret_cast<std::uintptr_t>(mbi.allocationBase);
            if (mod == exe) {
                a_where = std::format("the game +0x{:X}", fn - exe);
                return true;
            }
            char path[260]{};
            REX::W32::GetModuleFileNameA(reinterpret_cast<REX::W32::HMODULE>(mod), path, static_cast<std::uint32_t>(sizeof(path)));
            const char* base = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/') base = p + 1;
            a_where = std::format("{} +0x{:X} (another mod hooked it first - chained)", *base ? base : "a loaded DLL", fn - mod);
            return true;
        }

        template <std::size_t I>
        struct EffectScope
        {
            static void thunk(RE::ValueModifierEffect* a_this, RE::Actor* a_actor, float a_value, RE::ActorValue a_av)
            {
                const RE::ValueModifierEffect* const prevE = t_effect;
                const RE::Actor* const               prevT = t_effectTarget;
                t_effect                                   = a_this;
                t_effectTarget                             = a_actor;
                func(a_this, a_actor, a_value, a_av);
                t_effect       = prevE;
                t_effectTarget = prevT;
            }
            static inline REL::Relocation<decltype(thunk)> func;
            // u6d (F8): written only when the slot looks sane; returns the log text ("" + a_ok false = skipped)
            static std::string Install(REL::VariantID a_vtbl, const char* a_name, bool& a_ok)
            {
                REL::Relocation<std::uintptr_t> vt{ a_vtbl };
                std::string                     where;
                a_ok = SlotLooksSane(vt.address(), 0x20, where);
                if (!a_ok) return std::format("{}: SKIPPED ({})", a_name, where);
                func = vt.write_vfunc(0x20, thunk);
                return std::format("{} ({})", a_name, where);
            }
        };

        // the check (under g_mtx): her bleedout decision is forgotten once she is seen UP (3D, not bleeding out) - her next bleedout is
        // decided anew even if no damage of hers was seen starting it. Engine reads with g_u5Mtx RELEASED.
        void ForgetStandingU5()
        {
            std::vector<std::pair<RE::FormID, std::uint32_t>> ids;
            {
                std::scoped_lock lk(g_u5Mtx);
                for (const auto& [id, v] : g_u5)
                    if (v.decided || v.entry != EntryKind::kNone) ids.emplace_back(id, v.bleedSerial);
            }
            if (ids.empty()) return;
            std::vector<std::pair<RE::FormID, std::uint32_t>> up;
            for (const auto& [id, serial] : ids) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a || (a->Get3D() && !IsBleedingOut(a))) up.emplace_back(id, serial);
            }
            std::scoped_lock lk(g_u5Mtx);
            for (const auto& [id, serial] : up) {
                const auto it = g_u5.find(id);
                if (it == g_u5.end() || it->second.bleedSerial != serial) continue;   // a new bleedout was recorded meanwhile - keep it
                it->second.entry    = EntryKind::kNone;
                it->second.entryWhy = "";
                it->second.decided  = false;
            }
        }

        void DrainCells()
        {
            g_drainQueued.store(false);   // first: an event pushed from now on queues another drain
            std::vector<CellEvent> events;
            {
                std::scoped_lock lk(g_inboxMtx);
                events.swap(g_inbox);
            }
            g_perfCellActors.fetch_add(events.size(), std::memory_order_relaxed);   // u2c P1
            const std::uint32_t gen = g_generation.load();
            std::vector<Call>   calls;
            // m34: the marks hear every actor attach / detach of this session (their own lock; g_mtx is not held here)
            for (const auto& ev : events) {
                if (ev.gen != gen) continue;
                if (ev.attached)
                    Marks::OnAttach(ev.id);
                else
                    Marks::OnDetach(ev.id);
            }
            {
                std::scoped_lock lk(g_mtx);
                for (const auto& ev : events) {
                    if (ev.gen != gen) continue;   // queued before a load: not this session's
                    if (ev.attached) {
                        if (auto pc = g_pendingConscious.find(ev.id); pc != g_pendingConscious.end()) {
                            // M1d: a pending NPC - made conscious BEFORE any adoption / knock-out logic looks at her. Her 3D
                            // usually comes a moment after the attach; then the next check does it. A u2a belt is left to the check
                            // (it needs "still unconscious, not taken").
                            auto* a = RE::TESForm::LookupByID<RE::Actor>(ev.id);
                            if (a && a->Get3D() && !pc->second.belt) {
                                const std::string who = Who(a, ev.id);
                                logger::info("[KO] LOAD SAFETY: {} was knocked out in an unloaded cell -> made conscious (seen loaded "
                                             "again: cell attached){}", who,
                                             pc->second.window ? " - FinishWake (+ NoBleedoutRecovery off)" : pc->second.unc ? " - FinishWake (unconscious off)" : "");
                                MakeConscious(ev.id, pc->second, who, calls);
                                g_pendingConscious.erase(pc);
                            }
                            continue;
                        }
                        if (auto it = g_map.find(ev.id); it != g_map.end()) {
                            // U2a: the external-clear rule's "engine reset" context - a cell attach restarts the count
                            it->second.checksSince = 0;
                            it->second.since       = "her cell attaching";
                            it->second.resetActive = true;
                        } else {
                            g_adoptCandidates.insert(ev.id);
                            // u6h (G4): attached during the load (the sink saw no game ready) or before the first check after it = in the
                            // loaded area at the load (an orphan there is re-held); any later attach = she ARRIVED (an orphan is woken)
                            if (ev.atLoad || g_adoptAll)
                                g_adoptAtLoad.insert(ev.id);
                            else
                                g_adoptAtLoad.erase(ev.id);
                        }
                        continue;
                    }
                    g_adoptCandidates.erase(ev.id);
                    g_adoptAtLoad.erase(ev.id);   // u6h
                    g_declined.erase(ev.id);   // m-3
                    // M1c: a detach NEVER wakes anyone (v10's OnCellDetach wake, psc:93-97, is gone). Her entry
                    // stays; once she is loaded again the checks go on, and a timer that ran out wakes her at the first one.
                    // U1: her graph (and our graph sink on it) goes with her 3D; the check re-attaches it when she is back.
                    if (auto it = g_map.find(ev.id);
                        it != g_map.end() && (it->second.state == State::kDown || it->second.state == State::kUnconscious)) {
                        // u2c fix 6: the event says a cell she was in detached - NOT that she left (her 3D often stays)
                        auto* a = RE::TESForm::LookupByID<RE::Actor>(ev.id);
                        logger::info("[KO] {} cell-detach event ({}) - nothing changes: she stays {}; only a heal or the timer wakes her",
                                     it->second.who, a && a->Get3D() ? "her 3D is still loaded" : "her 3D is gone - the checks wait for it",
                                     StateName(it->second.state));
                    }
                }
            }
            Run(calls);
        }

        // u2c P1: the PERF line - ONE line per minute of unpaused play, then every counter starts again. A report, not a limit.
        void EmitPerf(const std::string& a_held, const std::string& a_putBacks)
        {
            const float secs = g_perfSeconds;
            g_perfSeconds    = 0.0f;
            std::string calls;
            std::uint64_t papyrus = 0;
            for (std::size_t k = 0; k < static_cast<std::size_t>(Call::Kind::kCount); ++k) {
                const std::uint32_t n = g_perfCalls[k].exchange(0, std::memory_order_relaxed);
                if (!n) continue;
                const auto kind = static_cast<Call::Kind>(k);
                if (kind != Call::Kind::kAttachFoot && kind != Call::Kind::kDetachFoot) papyrus += n;
                calls += std::format("{}{} {}", calls.empty() ? "" : ", ", KindName(kind), n);
            }
            const std::uint64_t checks   = g_perfChecks;
            const std::uint64_t avgNs    = checks ? g_perfCheckNs / checks : 0;
            const std::uint64_t scanned  = checks ? g_perfScanned / checks : 0;
            const std::uint64_t eff      = g_perfEffectEvents.exchange(0, std::memory_order_relaxed);
            const std::uint64_t effRel   = g_perfEffectRelayed.exchange(0, std::memory_order_relaxed);
            const std::uint64_t effNs    = g_perfEffectNs.exchange(0, std::memory_order_relaxed);
            const std::uint64_t effAb    = g_perfEffectAbilities.exchange(0, std::memory_order_relaxed);
            const std::uint64_t gEv      = g_perfGraphEvents.exchange(0, std::memory_order_relaxed);
            const std::uint64_t gSteps   = g_perfGraphSteps.exchange(0, std::memory_order_relaxed);
            const std::uint64_t gNs      = g_perfGraphNs.exchange(0, std::memory_order_relaxed);
            const std::uint64_t bEv      = g_perfBleedEvents.exchange(0, std::memory_order_relaxed);
            const std::uint64_t bNs      = g_perfBleedNs.exchange(0, std::memory_order_relaxed);
            const std::uint64_t cEv      = g_perfCellEvents.exchange(0, std::memory_order_relaxed);
            const std::uint64_t cActors  = g_perfCellActors.exchange(0, std::memory_order_relaxed);
            const std::uint64_t cNs      = g_perfCellNs.exchange(0, std::memory_order_relaxed);
            const std::uint64_t putBacks = g_perfPutBacks.exchange(0, std::memory_order_relaxed);
            const std::uint64_t graceN   = g_perfGraceStarted.exchange(0, std::memory_order_relaxed);
            const std::uint64_t graceKO  = g_perfGraceKnockedOut.exchange(0, std::memory_order_relaxed);
            const std::uint64_t hitEv    = g_perfHitEvents.exchange(0, std::memory_order_relaxed);    // U6
            const std::uint64_t hitRel   = g_perfHitRelayed.exchange(0, std::memory_order_relaxed);
            const std::uint64_t hitNs    = g_perfHitNs.exchange(0, std::memory_order_relaxed);
            const std::uint64_t dmgCalls = g_perfDmgCalls.exchange(0, std::memory_order_relaxed);    // u6b (U5)
            const std::uint64_t dmgLow   = g_perfDmgLow.exchange(0, std::memory_order_relaxed);
            const std::uint64_t dmgLowNs = g_perfDmgLowNs.exchange(0, std::memory_order_relaxed);
            const std::uint64_t dmgRolls = g_perfDmgRolls.exchange(0, std::memory_order_relaxed);
            const std::uint64_t dmgEntry = g_perfDmgEntries.exchange(0, std::memory_order_relaxed);
            const std::uint64_t logNow   = g_logLines.load(std::memory_order_relaxed);
            const std::uint64_t logLines = logNow - g_perfLogBase;
            logger::info("[PERF] {:.0f} s of play: checks {}, {}, loaded scanned {}/check, check avg {:.1f} us / max {:.1f} us | calls "
                         "dispatched {} Papyrus{}{}{} | put-backs {}{}{}{} | log lines {} | effect events {} (relayed {}, {:.0f} ns avg "
                         "relayed; abilities of held NPCs dropped in the sink {}) | graph events {} (footsteps {}, {:.0f} ns avg) | bleedout events {} ({:.0f} ns avg) | cell events {} "
                         "(actors {}, {:.0f} ns avg per actor) | bleedout graces started {}, knocked out after one {} | hit events {} "
                         "(the player's on actors relayed {}, {:.0f} ns avg relayed){}",
                         secs, checks, a_held, scanned, avgNs / 1000.0, g_perfCheckMaxNs / 1000.0, papyrus, calls.empty() ? "" : " (",
                         calls, calls.empty() ? "" : ")", putBacks, a_putBacks.empty() ? "" : " (", a_putBacks,
                         a_putBacks.empty() ? "" : ")", logLines + 1, eff, effRel, effRel ? static_cast<double>(effNs) / effRel : 0.0, effAb, gEv,
                         gSteps, gSteps ? static_cast<double>(gNs) / gSteps : 0.0, bEv, bEv ? static_cast<double>(bNs) / bEv : 0.0, cEv,
                         cActors, cActors ? static_cast<double>(cNs) / cActors : 0.0, graceN, graceKO, hitEv, hitRel,
                         hitRel ? static_cast<double>(hitNs) / hitRel : 0.0,
                         g_u5On.load(std::memory_order_relaxed)
                             ? std::format(" | U5 damage hook: {} NPC damage calls, {} at or under the line ({:.0f} ns avg there), rolls queued {} "
                                           "({} of them a weapon hit with no hit event), normal-NPC bleedouts recorded {}",
                                           dmgCalls, dmgLow, dmgLow ? static_cast<double>(dmgLowNs) / dmgLow : 0.0, dmgRolls,
                                           g_perfDmgPhysical.exchange(0, std::memory_order_relaxed), dmgEntry)
                             : std::string());
            g_perfLogBase   = g_logLines.load(std::memory_order_relaxed);
            g_perfChecks    = 0;
            g_perfCheckNs   = 0;
            g_perfCheckMaxNs = 0;
            g_perfScanned   = 0;
        }

        // ---- THE CHECK (one task per fCheckSeconds, at most one queued) ---------------------------------------------------
        // v10 OnUpdate (psc:57-78) + OnUpdateGameTime (psc:80-84) + StepComingTo (psc:175-183) for every handled NPC, the
        // v10-leftover adoption, then the safety-net scan.
        void Tick()
        {
            g_tickQueued.store(false);
            if (!g_gameReady.load()) return;
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return;
            // v10's poll was game-time: it stood still in menus. A loading menu pauses the game as well (its flags hold
            // kPausesGame); it is tested by name too.
            if (ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
                std::scoped_lock lk(g_mtx);
                g_hasLastTick = false;   // a paused stretch never counts as coming-to time
                return;
            }
            if (!RE::PlayerCharacter::GetSingleton()) return;
            const auto  tickStart = std::chrono::steady_clock::now();   // u2c P1: the check's own cost (measurement only)
            const auto& cfg = Settings::Get();
            // u2a fix 1: a re-adoption asks the SeverActions guard - SeverPresent asks the VM, so never under g_mtx
            const bool severUsable = cfg.severActionsGuard && g_severLoaded && Helper::SeverPresent();
            (void)Helper::Present();   // u7a (U7): keeps SBKO_Helper's presence cached - the TakeOver native reads only the cache
            bool        perfDue = false;   // u2c P1: a minute of play is complete - the PERF line goes out after this check
            std::string perfHeld, perfPutBacks;

            // The loaded actors (high process), read with no lock of ours held.
            std::vector<RE::NiPointer<RE::Actor>> loaded;
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                pl->ForEachHighActor([&](RE::Actor* a) {
                    if (a && !a->IsPlayerRef()) loaded.emplace_back(a);
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }

            std::vector<Call> calls;
            {
                std::scoped_lock lk(g_mtx);
                const auto  now = std::chrono::steady_clock::now();
                const float dt  = g_hasLastTick ? std::chrono::duration<float>(now - g_lastTick).count() : 0.0f;
                g_lastTick      = now;
                g_hasLastTick   = true;
                g_perfSeconds  += dt;

                // 0. M1d LOAD SAFETY (first unpaused check after kPostLoadGame, the co-save map restored): every handled NPC
                //    who is NOT in the loaded area is made conscious now and dropped. M1e: "in the loaded area" = she has 3D OR
                //    her parent cell is attached - right after the load screen an in-area NPC may not have her 3D yet; she
                //    keeps the normal rules (step 2 simply skips her until the 3D is there). U1: an UNCONSCIOUS one gets the
                //    full FinishWake restore (unconscious off, Paralysis 0, HealRate, potions) + the SeverActions include.
                //    U2b: a u2a-save window leftover gets the same FinishWake + NoBleedoutRecovery off.
                if (g_loadSafety) {
                    g_loadSafety    = false;
                    std::size_t woke = 0, pend = 0, coming = 0, gone = 0;
                    // u2c fix round 1 (m-1): a CREATED (FF) NPC whose form is not found after the load was deleted by the
                    // game (a temporary witch / bandit) - her entry is dropped. A non-FF NPC not in memory stays (an unloaded
                    // persistent or non-persistent NPC comes back with her cell).
                    auto goneNow = [](RE::FormID a_id) { return (a_id >> 24) == 0xFF && !RE::TESForm::LookupByID(a_id); };
                    for (auto it = g_pendingConscious.begin(); it != g_pendingConscious.end();) {
                        if (goneNow(it->first)) {
                            logger::info("[KO] LOAD SAFETY: 0x{:08X} (on the load-safety list) is a created NPC whose form is gone after "
                                         "the load - the game deleted her; dropped", it->first);
                            it = g_pendingConscious.erase(it);
                            ++gone;
                        } else
                            ++it;
                    }
                    for (auto it = g_released.begin(); it != g_released.end();) {
                        if (goneNow(it->first)) {
                            logger::info("[KO] LOAD SAFETY: 0x{:08X} (released watch) is a created NPC whose form is gone after the load - "
                                         "the game deleted her; dropped", it->first);
                            it = g_released.erase(it);
                            ++gone;
                        } else
                            ++it;
                    }
                    for (auto it = g_map.begin(); it != g_map.end();) {
                        const RE::FormID id = it->first;
                        Entry&           e  = it->second;
                        auto*            a  = RE::TESForm::LookupByID<RE::Actor>(id);
                        if (!a && goneNow(id)) {
                            logger::info("[KO] LOAD SAFETY: {} ({}) is a created NPC whose form is gone after the load - the game deleted "
                                         "her; dropped", e.who, StateName(e.state));
                            it = g_map.erase(it);
                            ++gone;
                            continue;
                        }
                        if (a && (a->Get3D() || InAttachedCell(a))) {   // M1e: in the loaded area -> untouched
                            ++it;
                            continue;
                        }
                        if (e.state == State::kComingTo && !e.awaitingWake) {
                            // u2c fix 5: awake already - only her coming-to rank is left, and its clock runs while she is unloaded
                            e.rankDirty = true;
                            ++coming;
                            ++it;
                            continue;
                        }
                        const Pending p = PendingOf(e);
                        if (!a) {   // not in memory (a non-persistent NPC whose cell is unloaded): when she is first seen loaded
                            g_pendingConscious[id] = p;
                            ++pend;
                            logger::info("[KO] LOAD SAFETY: {} was knocked out ({}) and is not in memory -> made conscious the first "
                                         "time she is loaded", e.who, StateName(e.state));
                        } else {
                            const std::string who = Who(a, id);
                            logger::info("[KO] LOAD SAFETY: {} was knocked out in an unloaded cell -> made conscious ({}, {} HealRate "
                                         "{:.4f}{}, rank 0)",
                                         who, StateName(e.state), p.window ? "FinishWake (+ NoBleedoutRecovery off)" : p.unc ? "FinishWake" : "WakeUp", p.healRate,
                                         p.unc ? std::format(", potions mask {}, SeverActions include {}", (std::max)(0, p.potionMask),
                                                             p.severExcluded ? "yes" : "no")
                                               : std::string());
                            MakeConscious(id, p, who, calls);
                            if (p.unc) {
                                // u2a fix 1 (M-6): she has no 3D - SetUnconscious(false) may not take; the wake
                                // runs again when she is next seen loaded, still unconscious and not taken
                                Pending b = p;
                                b.belt    = true;
                                g_pendingConscious[id] = b;
                            }
                            ++woke;
                        }
                        it = g_map.erase(it);
                    }
                    logger::info("[KO] LOAD SAFETY done: {} made conscious, {} waiting until loaded, {} coming to (her clock runs on), "
                                 "{} in the loaded area keep their rules, {} created NPC(s) gone (dropped); {} on the load-safety list",
                                 woke, pend, coming, g_map.size() - coming, gone, g_pendingConscious.size());
                }
                // 0b. M1d: a pending NPC seen loaded - made conscious BEFORE the adoption / scan look at her
                for (auto it = g_pendingConscious.begin(); it != g_pendingConscious.end();) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(it->first);
                    if (!a || !a->Get3D()) {
                        ++it;
                        continue;
                    }
                    if (it->second.belt) {
                        // u2a fix 1: the second run - only while she is still unconscious and nobody has taken her
                        TakenKind tk = TakenKind::kNone;
                        if (TakenBy(a, tk)) {
                            ++it;
                            continue;
                        }
                        const auto* st = a->AsActorState();
                        if (sbko::IsDeadNow(a) || !(st && st->IsUnconscious())) {
                            logger::info("[KO] {} the second-run wake is not needed (she is {}) - dropped", Who(a, it->first),
                                         sbko::IsDeadNow(a) ? "dead" : "conscious");
                            it = g_pendingConscious.erase(it);
                            continue;
                        }
                        logger::info("[KO] {} STILL UNCONSCIOUS when seen loaded again - the wake runs once more ({})", Who(a, it->first),
                                     it->second.window ? "FinishWake rank 0 + NoBleedoutRecovery off" : "FinishWake rank 0");
                        MakeConscious(it->first, it->second, Who(a, it->first), calls);
                        it = g_pendingConscious.erase(it);
                        continue;
                    }
                    const std::string who = Who(a, it->first);
                    logger::info("[KO] LOAD SAFETY: {} was knocked out in an unloaded cell -> made conscious (seen loaded again){}", who,
                                 it->second.window ? " - FinishWake (+ NoBleedoutRecovery off)" : it->second.unc ? " - FinishWake (unconscious off)" : "");
                    MakeConscious(it->first, it->second, who, calls);
                    it = g_pendingConscious.erase(it);
                }

                // 1. v10 leftovers: every loaded actor after a load, then actors whose cell attached.
                if (g_adoptAll) {
                    g_adoptAll = false;
                    if (!g_faction)
                        logger::info("[KO] after load: SBKO_StateFaction missing - no v10 knockout can be adopted");
                    else {
                        for (auto& a : loaded) {
                            if (!a->Get3D()) {
                                // u6h (G4): in the loaded area at the load, 3D not built yet - when it is, she is judged as at the load
                                if (g_adoptCandidates.contains(a->GetFormID())) g_adoptAtLoad.insert(a->GetFormID());
                                continue;
                            }
                            AdoptCheck(a.get(), "after load", severUsable, calls);
                            g_adoptCandidates.erase(a->GetFormID());
                            g_adoptAtLoad.erase(a->GetFormID());
                        }
                    }
                }
                for (auto it = g_adoptCandidates.begin(); it != g_adoptCandidates.end();) {
                    auto* a = g_map.contains(*it) ? nullptr : RE::TESForm::LookupByID<RE::Actor>(*it);
                    if (a && !a->Get3D()) {   // attached, 3D not there yet: look again next check
                        ++it;
                        continue;
                    }
                    // u6h (G4): a cell attach during play = she ARRIVED from an unloaded cell (an orphan is woken, not held again)
                    const bool atLoad = g_adoptAtLoad.erase(*it) != 0;
                    if (a && !a->IsPlayerRef())
                        AdoptCheck(a, atLoad ? "cell attached at the load" : "cell attached during play", severUsable, calls, !atLoad);
                    it = g_adoptCandidates.erase(it);
                }

                // m-3: a declined NPC seen out of bleedout may be knocked out again the next time she drops
                for (auto it = g_declined.begin(); it != g_declined.end();) {
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(*it);
                    if (a && a->Get3D() && !IsBleedingOut(a))
                        it = g_declined.erase(it);
                    else
                        ++it;
                }
                // u6b (U5): a normal NPC seen up again - her bleedout decision is forgotten (the next bleedout is decided anew)
                if (g_u5On.load(std::memory_order_relaxed)) ForgetStandingU5();

                // 2. every handled NPC that is loaded; unloaded ones stay untouched
                const float hours = HoursNow();
                for (auto it = g_map.begin(); it != g_map.end();) {
                    const RE::FormID id = it->first;
                    Entry&           e  = it->second;
                    auto*            a  = RE::TESForm::LookupByID<RE::Actor>(id);
                    // U2a: ANOTHER MOD TOOK HER (Abduct / Tie-up / Bag / a disable) - in ANY held state, loaded or not (a bagged NPC is
                    // moved away at once): SBKO lets go. Waits while a call of SBKO's is still in flight (M-1: a late HoldDown would
                    // put Paralysis back on her).
                    if (a && e.unc && Held(e.state) && e.state != State::kEntering && !e.releasing && !sbko::IsDeadNow(a)) {
                        TakenKind         kind = TakenKind::kNone;
                        const char* const why  = TakenBy(a, kind);
                        if (why) {
                            if (e.holdInFlight || e.healthInFlight) {
                                logger::info("[KO] {} taken by another mod ({}) - SBKO lets go once its running call has returned", e.who,
                                             why);
                                ++it;
                                continue;
                            }
                            LetGoNow(e, id, why, kind, calls);   // the entry stays until LetGo's reply (OnLetGoDone)
                            ++it;
                            continue;
                        }
                    }
                    if (e.releasing) {   // u2a fix 1: a LetGo is in flight - nothing else is done to her meanwhile
                        ++it;
                        continue;
                    }
                    // u2c fix 5: the coming-to clock - unpaused check time whether or not she has 3D (menus / loading still
                    // pause it: no check runs then). In memory -> the rank is written; not in memory -> written when she is found.
                    // (fix 6: the clock also runs while a wake's stack is still out - as in u2b; the rank is written
                    // only after that stack finished, so FinishWake's own rank 3 can never land on top of a later step)
                    if (e.state == State::kComingTo && !(a && sbko::IsDeadNow(a))) {
                        if (e.comingToRank > 0) {
                            e.stageSeconds += dt;   // psc:176-177: STAGE_SECS of play per rank
                            if (e.stageSeconds >= cfg.stageSeconds) {
                                e.stageSeconds = 0.0f;
                                --e.comingToRank;
                                e.rankDirty = true;
                                logger::info("[KO] {} coming to: stage {}{}", e.who, e.comingToRank,   // psc:181
                                             !a ? " (not in memory - written when she is found)" : !a->Get3D() ? " (not loaded)" : "");
                            }
                        }
                        if (e.rankDirty && a && !e.awaitingWake) {
                            e.rankDirty = false;
                            calls.push_back(Call{ .kind = Call::Kind::kRank, .id = id, .rank = (std::max)(0, e.comingToRank) });
                            if (e.comingToRank <= 0) {
                                // u6c: the coming-to is over - the Sleep spell's marker goes back to 0 (hygiene: the prompts read it only
                                // inside SBKO's rank 1-3 / 10 gate; a non-member is left alone by the script)
                                if (g_sleepFaction) calls.push_back(Call{ .kind = Call::Kind::kSleepMark, .id = id, .rank = 0, .who = e.who });
                                it = g_map.erase(it);
                                continue;
                            }
                        }
                    }
                    if (!a || !a->Get3D()) {
                        if (e.had3D) ResetGraphWatch(id);   // fix 4: her graph goes with her 3D - re-attach when it is back
                        e.had3D = false;   // U2a: her 3D (re)loading restarts the external-clear "engine reset" count
                        ++it;
                        continue;
                    }
                    bool fresh3D = false;
                    if (!e.had3D) {
                        e.had3D       = true;
                        e.checksSince = 0;
                        e.since       = "her 3D loading";
                        e.resetActive = true;
                        fresh3D       = true;
                        ResetGraphWatch(id);   // fix 4: a rebuilt graph may sit at the old addresses - attach again
                    }
                    if (fresh3D || e.who.empty() || e.who[0] == '0') e.who = Who(a, id);   // u2c P3: not a new string every check
                    if (sbko::IsDeadNow(a)) {   // the death event was missed: same result as v10 OnDying
                        logger::info("[KO] {} is dead (seen by the check) while {} - ClearMark (rank 0, NoBleedoutRecovery off), dropped{}",
                                     e.who, StateName(e.state), (e.potionMask > 0 || e.severExcluded) ? "; potions / SeverActions back" : "");
                        calls.push_back(Call{ .kind = Call::Kind::kClearMark, .id = id, .rank = 0, .who = e.who });   // u2c fix 4
                        GiveBackOthers(e, id, calls);
                        it = g_map.erase(it);
                        continue;
                    }
                    ++e.checksSince;
                    // U2a log hygiene: the effects that landed since the last check (each kind was logged once, the rest counted)
                    const std::uint32_t healsNow = e.healsSinceCheck;
                    // u2c P3: built only when a verbose line will print it
                    const std::string   effLine  = cfg.verboseChecks
                                                       ? std::format("effects since last check: {} ({} new kinds) | heals: {} ({} this "
                                                                     "knock-out)", e.effectsSinceCheck, e.newKindsSinceCheck,
                                                                     e.healsSinceCheck, e.healsTotal)
                                                       : std::string();
                    if (e.abilityApplies > 0) {   // u2c P2: one line for a burst of ability (re-)applications (a 3D load)
                        if (cfg.logEffects)
                            logger::info("[KO] {}: {} constant-effect (ability) application(s) since the last check - a 3D load re-applies "
                                         "her abilities; counted, not logged one by one", e.who, e.abilityApplies);
                        e.abilityApplies = 0;
                    }
                    e.effectsSinceCheck  = 0;
                    e.newKindsSinceCheck = 0;
                    e.healsSinceCheck    = 0;
                    switch (e.state) {
                        case State::kEntering:
                            if (cfg.verboseChecks) logger::info("[KO] {} waiting for KnockOut's reply (no wake possible yet)", e.who);
                            break;
                        case State::kDown: {
                            ++e.downChecks;   // psc:70
                            const float pct = HealthFraction(a);
                            if (cfg.verboseChecks) {
                                // U1 (b): HealRate / HealRateMult / Health cur-max / unconscious / knock on every line
                                float       cur = 0.0f, perm = 0.0f;
                                HealthFraction(a, &cur, &perm);
                                auto*       avo  = a->AsActorValueOwner();
                                const auto* st   = a->AsActorState();
                                const float para = avo->GetActorValue(RE::ActorValue::kParalysis);
                                logger::info("[KO] {} DOWN check {}: HP {} ({:.1f} / {:.1f}) | bleedingout={} | paralysis={:.6f} | "
                                             "unconscious={} | HealRate {:.4f} x HealRateMult {:.2f} | knock={} | {}",
                                             e.who, e.downChecks, Pct(pct), cur, perm, IsBleedingOut(a) ? "TRUE" : "FALSE", para,
                                             st && st->IsUnconscious() ? "TRUE" : "FALSE", avo->GetActorValue(RE::ActorValue::kHealRate),
                                             avo->GetActorValue(RE::ActorValue::kHealRateMult),
                                             st ? static_cast<int>(st->GetKnockState()) : -1, effLine);   // psc:72
                            }
                            if (e.downChecks > cfg.graceChecks && pct >= cfg.wakeHealthFraction)   // psc:73-75
                                BeginWake(e, id, "healed to " + Pct(pct), pct, false, 0, calls);
                            else if (hours >= e.wakeAtHours) {   // psc:80-84
                                // M1c: N = game hours she was out, at least 1 (the narration's number)
                                const std::int32_t out = (std::max)(1, static_cast<std::int32_t>(std::lround(hours - e.koHours)));
                                BeginWake(e, id, std::format("auto-timer elapsed ({} game-hrs out)", out), pct, true, out, calls);
                            }
                            break;
                        }
                        case State::kUnconscious: {
                            // ---- U1: the guarded unconscious (T6, T6b, T7) + the heal wake + U2b: "too injured to wake up" -----
                            ++e.downChecks;
                            float       cur = 0.0f, mx = 0.0f;
                            const float pct   = HealthFractionMax(a, &cur, &mx);         // fix 1 (M-2): perm + temp maximum
                            auto*       avo   = a->AsActorValueOwner();
                            const auto* st    = a->AsActorState();
                            const float para  = avo->GetActorValue(RE::ActorValue::kParalysis);
                            const float hr    = avo->GetActorValue(RE::ActorValue::kHealRate);
                            const float hrm   = avo->GetActorValue(RE::ActorValue::kHealRateMult);
                            const bool  unc   = st && st->IsUnconscious();                  // ActorState.h:203
                            const bool  bleed = IsBleedingOut(a);
                            const bool  rag   = a->IsInRagdollState();                      // RELOCATION_ID 36492, VR 0x5EBA50
                            const int   knock = st ? static_cast<int>(st->GetKnockState()) : -1;
                            const std::uint32_t steps = TakeFootsteps(id);
                            const bool  settling = e.downChecks <= cfg.graceChecks;
                            const bool  inFlight = e.holdInFlight || e.healthInFlight;    // fix 1 (M-1)
                            if (!settling) e.settledOnce = true;   // u2a fix 1: the knock-out's settle is over (T7 never re-arms it)
                            if (cfg.verboseChecks)
                                logger::info("[KO] {} UNCONSCIOUS check {}: HP {} ({:.1f} / {:.1f}) | unconscious={} paralysis={:.2f} "
                                             "bleedingout={} ragdoll={} knock={} | HealRate {:.4f} x HealRateMult {:.2f} | {} | "
                                             "footsteps {} | potions mask {} | SeverActions {} | {}",
                                             e.who, e.downChecks, Pct1(pct), cur, mx, unc ? "TRUE" : "FALSE", para,
                                             bleed ? "TRUE" : "FALSE", rag ? 1 : 0, knock, hr, hrm,
                                             !settling ? (e.lockFrac >= 0.0f ? std::format("hold {}", Pct1(e.lockFrac)) : std::string("hold unset"))
                                                       : std::format("settling {}/{}{}", e.downChecks, cfg.graceChecks,
                                                                     e.lockFrac >= 0.0f ? std::format(" (hold {} kept)", Pct1(e.lockFrac))
                                                                                        : std::string()),
                                             cfg.logFootsteps ? steps : 0u, (std::max)(0, e.potionMask),
                                             e.severExcluded ? "excluded" : "-", effLine);
                            // (VRTE's choke KO, rank 5 = TakenBy() above: SBKO lets go - its part comes back when VRTE wakes her)
                            // T7 (fix 1, M-5: BEFORE the timer wake): in bleedout while unconscious (one life state - she left the
                            // unconscious one). Re-assert first; a due wake happens at a later check, once she is unconscious again
                            // (FinishWake only leaves the UNCONSCIOUS life state, never bleedout). U2a: never an "external wake".
                            if (bleed) {
                                if (!e.holdInFlight) {
                                    e.holdInFlight = true;
                                    e.downChecks   = 0;   // the settle starts again; the HOLD is kept (it never rises)
                                    logger::info("[KO] {} found in BLEEDOUT while unconscious (seen by the check) - re-asserted "
                                                 "(unconscious first, Paralysis 1, shove); the settle starts again, the hold stays {}{}",
                                                 e.who, e.lockFrac >= 0.0f ? Pct1(e.lockFrac) : std::string("unset"),
                                                 hours >= e.wakeAtHours ? "; her timer is due - she wakes once she is unconscious again"
                                                                        : "");
                                    calls.push_back(Call{ .kind = Call::Kind::kHoldDown, .id = id, .serial = e.episode, .who = e.who });
                                }
                                break;
                            }
                            // U2b: an external clear - ExternalClear() decides (always re-assert; "too injured" or silent; restrained
                            // = taken)
                            bool          reassert = false;
                            ClearDecision clear;
                            if ((!unc || para < 0.5f) && !e.holdInFlight) {
                                const ClearDecision d =
                                    ExternalClear(e, unc, para, st ? st->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive);
                                if (d.letGo) {
                                    if (e.healthInFlight) break;   // M-1: a put-back of SBKO's is still running
                                    LetGoNow(e, id, "restrained - taken by another mod", TakenKind::kCaptive, calls);
                                    break;
                                }
                                if (d.sleepWake) {   // u6f: a sleeper woken by another mod (DoM's Wake) - she wakes, no re-assert
                                    if (!e.sleepWakePending && !e.healPending) {
                                        e.sleepWakePending = true;
                                        e.sleepWakeTrigger = std::format("by another mod's Wake ({} cleared at check {} since {})", d.what,
                                                                         e.checksSince, e.since);
                                        logger::info("[sleep] {} woken {} - {}", e.who, e.sleepWakeTrigger, d.why);
                                    }
                                } else {
                                    clear    = d;
                                    reassert = true;   // logged where the shove is sent (a heal / timer wake below takes precedence)
                                }
                            }
                            // U2a: the heal wake (a heal seen while something of SBKO's was in flight - M-1)
                            if (e.healPending && cfg.healWake) {
                                if (inFlight) {
                                    logger::info("[KO] {} a heal is waiting - the wake waits for the running {} (next check)", e.who,
                                                 e.holdInFlight ? "HoldDown" : "HoldHealthTo");
                                    break;
                                }
                                BeginHealWake(e, id, "a heal seen earlier (the running call has returned)", pct, calls);
                                break;
                            }
                            // u6f: a sleeper's wake (a hit / the player's talk / another mod's Wake) that waited for a running call (M-1)
                            if (e.sleepWakePending) {
                                if (inFlight) {
                                    logger::info("[sleep] {} a wake is waiting - it waits for the running {} (next check)", e.who,
                                                 e.holdInFlight ? "HoldDown" : "HoldHealthTo");
                                    break;
                                }
                                BeginSleepWake(e, id, pct, calls);
                                break;
                            }
                            // T6b: the time wake. Fix 1 (M-1): never while a HoldDown / HoldHealthTo is still on the
                            // Papyrus stack - it waits for that call's reply, exactly as ENTERING waits for KnockOutU's.
                            if (hours >= e.wakeAtHours) {
                                if (inFlight) {
                                    logger::info("[KO] {} timer ran out - the wake waits for the running {} to return (next check)",
                                                 e.who, e.holdInFlight ? "HoldDown" : "HoldHealthTo");
                                    break;
                                }
                                const std::int32_t out = (std::max)(1, static_cast<std::int32_t>(std::lround(hours - e.koHours)));
                                BeginWakeU(e, id, std::format("auto-timer elapsed ({} game-hrs out)", out), pct, out, calls);
                                break;
                            }
                            bool shove = reassert;
                            if (reassert) {
                                logger::info("[KO] {} CLEARED externally: {} (unconscious={} paralysis={:.2f}) at check {} since {} - "
                                             "re-asserted, shoved ({}){}",
                                             e.who, clear.what, unc ? "TRUE" : "FALSE", para, e.checksSince, e.since, clear.why,
                                             clear.tooInjured ? " - TOO INJURED TO WAKE UP (HUD line)" : "");
                            }
                            if (cfg.logFootsteps && steps > 0) {
                                if (cfg.repairWalkingGraph && !shove && !e.holdInFlight) {
                                    logger::info("[KO] WALKING GRAPH on {}: {} footsteps since last check - re-shoved", e.who, steps);
                                    shove = true;
                                } else {
                                    logger::info("[KO] WALKING GRAPH on {}: {} footsteps since last check - {}", e.who, steps,
                                                 !cfg.repairWalkingGraph ? "not re-shoved (bRepairWalkingGraph 0)"
                                                 : shove                 ? "the re-assert above shoves her"
                                                                         : "a re-assert is still running");
                                }
                            }
                            if (e.shoveAfterLoad) {
                                e.shoveAfterLoad = false;
                                if (!rag && !shove && !e.holdInFlight) {
                                    logger::info("[KO] {} after the load she is not ragdolled (knock {}) - shoved once", e.who, knock);
                                    shove = true;
                                }
                            }
                            if (shove) {
                                e.holdInFlight = true;
                                calls.push_back(Call{ .kind = Call::Kind::kHoldDown, .id = id, .serial = e.episode, .who = e.who });
                            }
                            // U2b: someone tried to wake her - ONE HUD line per clear event (this re-assert; the next check sees her
                            // unconscious again), dispatched right after the re-assert
                            if (reassert && clear.tooInjured)
                                calls.push_back(Call{ .kind = Call::Kind::kTooInjured, .id = id, .who = e.who });
                            // T6: the HOLD, after the settle (iGraceChecks - no new timer). Fix 1 (rule: no
                            // health rise while she is out): hold = min(hold, her Health) every settled check - it follows damage
                            // DOWN and never rises; any rise above hold + 1 % (float tolerance) is put back to it.
                            if (!settling && mx > 0.0f) {
                                if (e.lockFrac < 0.0f) {
                                    // U6: a HIT knock-out is held at her Health at the knock-out (no cap) - min(that, her Health now),
                                    // so a rise during the settle is not kept and damage during it is; unknown (a save inside the
                                    // settle) = her Health now. A bleedout knock-out keeps the fUnconsciousHealthFraction cap.
                                    const float basis = e.hitKO ? (e.hitPct >= 0.0f ? (std::min)(e.hitPct, pct) : pct)
                                                                : (std::min)(cfg.unconsciousHealthFraction, pct);
                                    e.lockFrac = (std::max)(kLockFloor, basis);
                                    if (pct > e.lockFrac + kLockTolerance) {
                                        logger::info("[KO] {} SETTLED: HP drifted to {} - held at {}{}", e.who, Pct1(pct), Pct1(e.lockFrac),
                                                     e.hitKO ? " (a hit knock-out: her Health at the knock-out)" : "");
                                        if (!e.healthInFlight) {
                                            ++e.putBacksPerf;   // u2c: counted in the PERF line (logged by this SETTLED line)
                                            g_perfPutBacks.fetch_add(1, std::memory_order_relaxed);
                                            e.healthInFlight = true;
                                            calls.push_back(Call{ .kind = Call::Kind::kHoldHealth, .id = id, .serial = e.episode,
                                                                  .who = e.who, .target = e.lockFrac * mx });
                                        }
                                    } else if (e.hitKO) {
                                        logger::info("[KO] {} SETTLED: HP {} - held at {} (a hit knock-out: her Health at the knock-out {}, "
                                                     "no {}% cap, never under the 1% floor; the hold follows her down, never up); SBKO "
                                                     "never heals her",
                                                     e.who, Pct1(pct), Pct1(e.lockFrac),
                                                     e.hitPct >= 0.0f ? Pct1(e.hitPct) : std::string("not known (loaded inside the settle)"),
                                                     std::lround(cfg.unconsciousHealthFraction * 100.0f));
                                    } else {
                                        logger::info("[KO] {} SETTLED: HP {} - held at {} (at or under the {}% cap, never under the 1% "
                                                     "floor; the hold follows her down, never up); SBKO never heals her",
                                                     e.who, Pct1(pct), Pct1(e.lockFrac), std::lround(cfg.unconsciousHealthFraction * 100.0f));
                                    }
                                } else if (pct < e.lockFrac - kFallNoise) {
                                    // she lost Health (a hit, a drain): the hold follows her down - never below the safety floor
                                    const float was = e.lockFrac;
                                    e.lockFrac      = (std::max)(kLockFloor, pct);
                                    if (e.lockFrac < was)
                                        logger::info("[KO] {} HP fell to {} - the hold follows her DOWN: {} -> {} (it never rises)",
                                                     e.who, Pct1(pct), Pct1(was), Pct1(e.lockFrac));
                                } else if (pct > e.lockFrac + kLockTolerance && !e.healthInFlight) {
                                    ++e.putBacks;   // u2c fix 4 / P2: every put-back counted (PERF line); the FIRST one of a knock-out
                                    ++e.putBacksPerf;   // is logged in full (bVerboseChecks 1: every one)
                                    g_perfPutBacks.fetch_add(1, std::memory_order_relaxed);
                                    const bool        sayIt = cfg.verboseChecks || e.putBacks == 1;
                                    const std::string more  = cfg.verboseChecks ? std::string()
                                                                                : std::string(" | first put-back of this knock-out - the next "
                                                                                              "ones are counted in the PERF line");
                                    if (sayIt && healsNow > 0)
                                        logger::info("[KO] {} HP rose to {} after {} heal application(s) since the last check - put back "
                                                     "to {} ({}){}", e.who, Pct1(pct), healsNow, Pct1(e.lockFrac),
                                                     !cfg.healWake ? "bHealWake 0" : "no wake trigger from them", more);
                                    else if (sayIt)
                                        logger::info("[KO] {} HP rose to {} with NO heal - put back to {} (not a wake) | {}{}", e.who,
                                                     Pct1(pct), Pct1(e.lockFrac),
                                                     !cfg.logEffects       ? std::string("effects not logged (bLogEffects 0)")
                                                     : e.lastEffect.empty() ? std::string("no magic effect seen on her")
                                                                            : "last effect: " + e.lastEffect, more);
                                    e.healthInFlight = true;
                                    calls.push_back(Call{ .kind = Call::Kind::kHoldHealth, .id = id, .serial = e.episode, .who = e.who,
                                                          .target = e.lockFrac * mx });
                                }
                            }
                            PotionRecheck(e, a, id, calls);
                            // the graph listener: asked every check, but Run re-attaches only when her graph changed (u2c P3) - so a
                            // graph rebuilt by a cell reload is healed; footsteps only (U2b: no get-up signal any more)
                            if (cfg.logFootsteps)
                                calls.push_back(Call{ .kind = Call::Kind::kAttachFoot, .id = id, .who = e.who });
                            break;
                        }
                        case State::kWindow: {
                            // U2b: a heal-wake window from a u2a save (the window no longer exists) - finished at once, never
                            // restarted: FinishWake (+ NoBleedoutRecovery off, which that window had set), rank 3, coming to, quiet
                            logger::info("[KO] {} a u2a heal-wake window entry from the save - finished with FinishWake (+ "
                                         "NoBleedoutRecovery off), not restarted", e.who);
                            BeginStraightWake(e, id, "a u2a heal-wake window from the save - finished (U2b has no window)",
                                              HealthFractionMax(a), kNarrateNone, true, calls);
                            break;
                        }
                        case State::kComingTo:
                            // in bleedout again -> knocked out again (v10: !bIsDown && IsBleedingOut -> EnterDown) - left to
                            // the scan below, which applies the same filter and gap as the event path. u2c fix 5: her clock runs
                            // above, before the 3D test.
                            break;
                    }
                    ++it;
                }

                // 2b. u2a fix 1 (M-A): the RELEASED watch - an NPC SBKO let go with her unconscious flag on
                for (auto it = g_released.begin(); it != g_released.end();) {
                    const RE::FormID id = it->first;
                    Released&        r  = it->second;
                    auto*            a  = RE::TESForm::LookupByID<RE::Actor>(id);
                    if (!a || r.givingBack || g_map.contains(id)) {   // not in memory / a give-back in flight / held again: wait
                        ++it;
                        continue;
                    }
                    const std::string who = Who(a, id);
                    if (a->IsDeleted() || sbko::IsDeadNow(a)) {
                        if (r.deferred) {   // the death rule: another mod's state goes back (potions / SeverActions)
                            if (r.potionMask > 0)
                                calls.push_back(Call{ .kind = Call::Kind::kPotionsBack, .id = id, .who = who, .mask = r.potionMask });
                            if (r.severExcluded)
                                calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = id, .who = who, .packed = r.severPacked });
                        }
                        logger::info("[KO] {} released NPC is {} - no longer watched", who, a->IsDeleted() ? "deleted" : "dead");
                        it = g_released.erase(it);
                        continue;
                    }
                    if (!a->Get3D()) {   // not loaded: wait
                        ++it;
                        continue;
                    }
                    TakenKind tk = TakenKind::kNone;
                    if (TakenBy(a, tk) || (g_domSlaveFaction && a->GetFactionRank(g_domSlaveFaction, false) >= 0)) {
                        ++it;   // still taken (or now a DoM slave - DoM owns her): skip
                        continue;
                    }
                    const auto* st = a->AsActorState();
                    if (st && st->IsUnconscious()) {
                        // given back STILL UNCONSCIOUS and nobody holds her -> SBKO holds her again
                        const bool         shove   = !a->IsInRagdollState();
                        const std::int32_t potions = cfg.potionsOff ? PotionSpellsOn(a) : 0;
                        Entry e;
                        e.who     = who;
                        auto& ref = g_map[id] = std::move(e);
                        BeginReAdopt(ref, id, r, shove, potions, severUsable, calls);
                        g_declined.erase(id);
                        it = g_released.erase(it);
                        continue;
                    }
                    // conscious (awake, or in a bleedout of her own): no longer SBKO's business
                    if (r.deferred) {
                        // VRTE woke her: SBKO's part comes back now (VRTE restored the HealRate it found - SBKO's 0)
                        r.givingBack = true;
                        logger::info("[KO] {} VRTE released her awake - SBKO gives back its part (HealRate {}, potions mask {}, "
                                     "SeverActions {})",
                                     who, r.healRate >= 0.0f ? std::format("{:.4f}", r.healRate) : std::string("base"),
                                     (std::max)(0, r.potionMask), r.severExcluded ? "include" : "-");
                        calls.push_back(Call{ .kind = Call::Kind::kLetGo, .id = id, .serial = 0, .healRate = r.healRate, .who = who,
                                              .mask = (std::max)(0, r.potionMask), .flags = 8 });
                        if (r.severExcluded) {
                            calls.push_back(Call{ .kind = Call::Kind::kSeverUnguard, .id = id, .who = who, .packed = r.severPacked });
                            r.severExcluded = false;
                        }
                        ++it;   // erased by OnLetGoDone (serial 0); a failed dispatch clears givingBack and retries
                        continue;
                    }
                    logger::info("[KO] {} released NPC is conscious again - no longer watched", who);
                    it = g_released.erase(it);
                }
                PruneFdcBeams();   // m34b
                // 2c. u7a (M2): "woken to talk" - a pending natural sleeper is confirmed by her sit-sleep state at an UNPAUSED check
                for (auto it = g_woken.begin(); it != g_woken.end();) {
                    auto*       a    = RE::TESForm::LookupByID<RE::Actor>(it->first);
                    Woken&      w    = it->second;
                    const char* drop = nullptr;
                    TakenKind   tk   = TakenKind::kNone;
                    if (!a || sbko::IsDeadNow(a))
                        drop = "dead / gone";
                    else if (!a->Get3D())
                        drop = "unloaded";
                    else if (TakenBy(a, tk))
                        drop = "taken by another mod (Diary of Mine's Tie-up / Bag / Abduct, a disable ...)";
                    else if (const auto m = g_map.find(it->first); m != g_map.end() && m->second.state != State::kComingTo)
                        drop = "SBKO knocked her out meanwhile";
                    if (drop) {
                        if (cfg.logWoken) logger::info("[woken] {} not woken: {} - nothing said", w.who, drop);
                        it = g_woken.erase(it);
                        continue;
                    }
                    const auto*         st = a->AsActorState();
                    const std::uint32_t ss = st ? static_cast<std::uint32_t>(st->GetSitSleepState()) : 0;
                    if (ss != kSitSleeping) {
                        const std::int32_t mode = w.by == WokenBy::kTalk ? cfg.wokenTalkMode : 1;
                        if (cfg.logWoken)
                            logger::info("[woken] {} WOKEN BY THE PLAYER ({}): sit-sleep state {} -> {} ({}) - SkyrimNet: '<player> has just woken "
                                         "<NPC> up to talk to <NPC>.' ({}; NarrateWoken says nothing if she is hostile to you)",
                                         w.who, w.by == WokenBy::kTalk ? "talk" : "Diary of Mine's Wake", w.state, ss, SitSleepName(ss),
                                         mode == 1 ? "DirectNarration - she answers you" : "RegisterPersistentEvent - she knows it");
                        calls.push_back(Call{ .kind = Call::Kind::kWokenNarrate, .id = it->first, .rank = kWokenNatural, .who = w.who,
                                              .narrate = mode });
                        it = g_woken.erase(it);
                        continue;
                    }
                    if (++w.checks >= 2) {
                        if (cfg.logWoken)
                            logger::info("[woken] {} not woken (still asleep after 2 checks - Cancel / a pickpocket / the activation was blocked) - "
                                         "nothing said", w.who);
                        it = g_woken.erase(it);
                        continue;
                    }
                    ++it;
                }
                SyncCount();
                // u2c P1: a minute of play complete -> snapshot what only the lock can read; the line goes out after Run
                if (g_perfSeconds >= 60.0f) {
                    perfDue = true;
                    std::size_t held = 0, heldLoaded = 0, coming = 0;
                    for (auto& [hid, he] : g_map) {
                        if (Held(he.state) || he.state == State::kDown) {
                            ++held;
                            if (!he.who.empty() && he.had3D) ++heldLoaded;
                        } else
                            ++coming;
                        if (he.putBacksPerf > 0) {
                            perfPutBacks += std::format("{}{} {}", perfPutBacks.empty() ? "" : ", ", he.who, he.putBacksPerf);
                            he.putBacksPerf = 0;
                        }
                    }
                    perfHeld = std::format("held {} ({} with 3D), coming to {}, watched released {}, load-safety list {}", held,
                                           heldLoaded, coming, g_released.size(), g_pendingConscious.size());
                }
            }

            // 3. the safety net: loaded actors in bleedout that nobody handles yet. Detect locks itself.
            //    u2c P3: one life-state read per loaded actor; everything else only for the ones in bleedout.
            for (auto& a : loaded)
                if (IsBleedingOut(a.get())) Detect(a.get(), "the check's scan", calls);
            ProcessGrace(calls);   // u2d: a grace that came due while the game was paused (GraceTask waits for an unpaused check)

            Run(calls);

            // u2c P1: measurement only. The check's cost (lists, map, scan, Papyrus dispatches) - never a limit.
            const std::uint64_t ns = NsSince(tickStart);
            ++g_perfChecks;
            g_perfCheckNs += ns;
            g_perfCheckMaxNs = (std::max)(g_perfCheckMaxNs, ns);
            g_perfScanned += loaded.size();
            if (perfDue) EmitPerf(perfHeld, perfPutBacks);
            // m34 (build 3): the skin marks ride this check - no lock of ours held, the game not paused (no new timer)
            Marks::Tick(loaded);
        }

        // ---- event sinks: copy, hand off, return --------------------------------------------------------------------------

        class BleedoutSink final : public RE::BSTEventSink<RE::TESEnterBleedoutEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESEnterBleedoutEvent* a_event,
                                                  RE::BSTEventSource<RE::TESEnterBleedoutEvent>*) override
            {
                if (!a_event || !a_event->actor) return RE::BSEventNotifyControl::kContinue;
                const auto       t0 = std::chrono::steady_clock::now();   // u2c P4: copy + relay only (the log lines moved to the task)
                const RE::FormID id = a_event->actor->GetFormID();
                const auto       n  = ++g_bleedoutEvents;
                const std::uint32_t gen = g_generation.load();
                TaskRelay::Add([id, gen, n] { OnBleedoutTask(id, gen, n); });
                g_perfBleedEvents.fetch_add(1, std::memory_order_relaxed);
                g_perfBleedNs.fetch_add(NsSince(t0), std::memory_order_relaxed);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
            {
                // u7a: also while a loose choke record waits (dropped at her death)
                // m34: also while marked actors exist (their record is dropped at a death)
                if (!a_event || !a_event->actorDying || (g_handled.load() == 0 && g_chokeLooseCount.load() == 0 && !Marks::AnyRecords()))
                    return RE::BSEventNotifyControl::kContinue;
                const RE::FormID    id   = a_event->actorDying->GetFormID();
                const bool          dead = a_event->dead;
                const std::uint32_t gen  = g_generation.load();
                TaskRelay::Add([id, dead, gen] { OnDeathTask(id, dead, gen); });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // HOT PATH: every reference of every cell, thousands per cell change (VRBackpack lockpick_main.cpp:4473-4480).
        // One form-type compare for non-actors; actors go to the inbox (one short lock) and ONE drain task is relayed.
        class CellSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event,
                                                  RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
            {
                if (!a_event || !a_event->reference) return RE::BSEventNotifyControl::kContinue;
                g_perfCellEvents.fetch_add(1, std::memory_order_relaxed);   // u2c P1 (every reference - the cheapest possible count)
                const auto* ref = a_event->reference.get();
                if (!ref->Is(RE::FormType::ActorCharacter) || ref->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
                const auto t0 = std::chrono::steady_clock::now();
                // m1b (M-1): between kPreLoadGame and kPostLoadGame every detach is the old world's teardown, not
                // a detach. Since M1c a detach wakes nobody; it only clears this session's lists, so teardown detaches are
                // still dropped. (Attaches only add adoption candidates.)
                if (!a_event->attached && !g_gameReady.load()) return RE::BSEventNotifyControl::kContinue;
                {
                    std::scoped_lock lk(g_inboxMtx);
                    // u6h (G4): atLoad = no game is ready = this attach is part of a load (kPreLoadGame .. kPostLoadGame)
                    g_inbox.push_back(CellEvent{ ref->GetFormID(), a_event->attached, g_generation.load(), !g_gameReady.load() });
                }
                if (!g_drainQueued.exchange(true)) TaskRelay::Add([] { DrainCells(); });
                g_perfCellNs.fetch_add(NsSince(t0), std::memory_order_relaxed);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // U1 (bLogEffects): TESMagicEffectApplyEvent (target, caster, magicEffect - TESMagicEffectApplyEvent.h:13-16), DDAB's
        // route A (WornDevices.cpp HealApplySink, VR-proven in DDAB 1.2.6). EVERY effect on every actor passes here: an empty
        // held set returns at once; otherwise one set lookup under g_watchMtx, then copy + relay. U2a: it is also the heal
        // wake's trigger source - registered when bLogEffects OR bHealWake.
        class EffectSink final : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event,
                                                  RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*) override
            {
                g_perfEffectEvents.fetch_add(1, std::memory_order_relaxed);   // u2c P1 (every effect in the game)
                // u6c: the SLEEP SPELL first (before the "nobody held" fast path): the player's effect from g_sleepMgefs on an actor ->
                // copy + relay. g_sleepMgefs is immutable after kDataLoaded (built before this sink was registered) - no lock. Pure reads:
                // two pointers, a FormID compare, one binary search; no engine call. Never also relayed to OnEffectTask (not a heal).
                if (a_event && a_event->caster && a_event->target && g_sleepOn.load(std::memory_order_relaxed) &&
                    g_gameReady.load(std::memory_order_relaxed) && a_event->caster->IsPlayerRef() &&
                    std::binary_search(g_sleepMgefs.begin(), g_sleepMgefs.end(), a_event->magicEffect)) {
                    const RE::TESObjectREFR* tgt = a_event->target.get();
                    if (tgt->Is(RE::FormType::ActorCharacter) && !tgt->IsPlayerRef()) {
                        const RE::FormID    tid  = tgt->GetFormID();
                        const RE::FormID    mgef = a_event->magicEffect;
                        const std::uint32_t gen  = g_generation.load();
                        TaskRelay::Add([tid, mgef, gen] { OnSleepTask(tid, mgef, gen); });
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                // m34: an effect on a MARKED actor (a heal: IE's re-read + the choke mark's heal) - Marks filters on its own set first
                if (a_event && a_event->target && g_gameReady.load(std::memory_order_relaxed))
                    Marks::NoteEffect(a_event->target->GetFormID(), a_event->caster ? a_event->caster->GetFormID() : 0, a_event->magicEffect);
                if (!a_event || g_watchCount.load(std::memory_order_relaxed) == 0 || !a_event->target)
                    return RE::BSEventNotifyControl::kContinue;   // nobody held: the fast path
                const RE::FormID tid = a_event->target->GetFormID();
                if (!Watched(tid)) return RE::BSEventNotifyControl::kContinue;
                const auto       t0   = std::chrono::steady_clock::now();
                const RE::FormID mgef = a_event->magicEffect;
                // u2c fix round 1 (perf): a constant effect (an ability) is never a heal (IsHeal) - dropped HERE,
                // before the relay: a 3D reload re-applies ~46 of them per held NPC. One form lookup + one field read (the same
                // lookup DDAB's VR-proven HealApplySink makes in its sink, WornDevices.cpp:7009).
                if (const auto* m = RE::TESForm::LookupByID<RE::EffectSetting>(mgef);
                    m && m->data.castingType == RE::MagicSystem::CastingType::kConstantEffect) {
                    g_perfEffectAbilities.fetch_add(1, std::memory_order_relaxed);
                    return RE::BSEventNotifyControl::kContinue;
                }
                const RE::FormID    cid  = a_event->caster ? a_event->caster->GetFormID() : 0;
                const std::uint32_t gen  = g_generation.load();
                TaskRelay::Add([tid, cid, mgef, gen] { OnEffectTask(tid, cid, mgef, gen); });
                g_perfEffectRelayed.fetch_add(1, std::memory_order_relaxed);
                g_perfEffectNs.fetch_add(NsSince(t0), std::memory_order_relaxed);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // U6: TESHitEvent - EVERY hit of every fight passes here (any thread: PPB found it can be a Havok thread). Two FormID
        // compares drop all but the PLAYER's hits on actors; those are copied (plain values: target, source, the flags WORD, and
        // for PLANCK's extended event the node name, the head test - the node and up to kHeadParents parents by name, pure reads of
        // a node the event itself holds - the contact point, the hand) and relayed. No engine call, no lock of ours, no AddTask.
        class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
            {
                g_perfHitEvents.fetch_add(1, std::memory_order_relaxed);   // measurement only (every hit in the game)
                if (!a_event || !a_event->target || !g_gameReady.load(std::memory_order_relaxed))
                    return RE::BSEventNotifyControl::kContinue;
                const RE::TESObjectREFR* tgt  = a_event->target.get();
                const auto               base = reinterpret_cast<std::uintptr_t>(a_event);
                // m34: every actor hit is NAMED for Injury Engine's next ping (one set insert under Marks' inbox lock; off = one atomic load)
                if (tgt->Is(RE::FormType::ActorCharacter)) Marks::NoteHit(tgt->GetFormID());
                // u6f: a Sleep-spell SLEEPER wakes on ANY hit, by anyone (a hit with no attacker too - before the cause filter). Nobody
                // asleep = one atomic load; else one set lookup under g_watchMtx, then copy FormIDs + relay (the task filters the spell's
                // own hit and decides under g_mtx)
                if (g_sleepWatchCount.load(std::memory_order_relaxed) != 0 && Settings::Get().sleepWakeOnHit &&
                    tgt->Is(RE::FormType::ActorCharacter)) {
                    const RE::FormID tid = tgt->GetFormID();
                    if (Asleep(tid)) {
                        const RE::FormID    cid   = a_event->cause ? a_event->cause->GetFormID() : 0;
                        const RE::FormID    src   = a_event->source;
                        const RE::FormID    proj  = a_event->projectile;
                        const std::uint32_t flags = *reinterpret_cast<const std::uint32_t*>(base + 0x18) & 0xFFu;
                        const std::uint32_t gen   = g_generation.load();
                        TaskRelay::Add([tid, cid, src, proj, flags, gen] { OnSleepWakeTask(tid, cid, src, proj, flags, SleepWakeBy::kHit, gen); });
                    }
                }
                if (!a_event->cause) return RE::BSEventNotifyControl::kContinue;
                // u6b (U5): EVERY hit on an NPC / creature is remembered on THIS thread (plain values, no lock) - the engine sends a physical
                // hit's event just before its damage on the same thread, so the damage hook can tell a punch / a bash from a weapon
                if ((g_u5On.load(std::memory_order_relaxed) || g_fdcOn.load(std::memory_order_relaxed)) && tgt->Is(RE::FormType::ActorCharacter)) {
                    t_hit   = HitTls{ .target     = tgt->GetFormID(),
                                      .cause      = a_event->cause->GetFormID(),
                                      .source     = a_event->source,
                                      .projectile = a_event->projectile,
                                      .flags      = *reinterpret_cast<const std::uint32_t*>(base + 0x18) & 0xFFu };
                    t_swing = SwingTls{};   // u6d (F2): a new hit begins - the last swing's enchantment / poison window is over
                }
                if (!Settings::Get().headHit) return RE::BSEventNotifyControl::kContinue;   // u6b: registered for U5 only - nothing more
                if (!a_event->cause->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;   // R1: the player only (FormID 0x14)
                if (tgt->IsPlayerRef() || !tgt->Is(RE::FormType::ActorCharacter)) return RE::BSEventNotifyControl::kContinue;
                const auto     t0   = std::chrono::steady_clock::now();
                HitCopy        h;
                h.target     = tgt->GetFormID();
                h.source     = a_event->source;
                h.projectile = a_event->projectile;
                // the WORD at +0x18 (CommonLib declares a 1-byte flags field there - the magic would be invisible through it)
                h.flags  = *reinterpret_cast<const std::uint32_t*>(base + 0x18);
                h.planck = (h.flags & 0xFFFFFF00u) == kPlanckMagic;
                // u6 fix 3: no PLANCK in the process = no PLANCK hit, whatever the word says - the extended block is never read
                if (h.planck && !g_planckPresent.load(std::memory_order_relaxed)) {
                    h.planck        = false;
                    h.magicNoPlanck = true;
                }
                // u6 fix 1 (F5): with PLANCK's interface, a PLANCK hit must ALSO be PLANCK's current hit event - a vanilla
                // 0x20-byte event whose padding happened to hold the magic is never read past its end. PLANCK loaded but no
                // interface = the magic alone.
                if (h.planck) {
                    if (auto* api = g_planckApi.load(std::memory_order_acquire); api && api->GetCurrentHitEvent() != a_event) {
                        h.planck       = false;
                        h.magicNoMatch = true;
                    }
                }
                if (h.planck) {   // only then is the event longer than 0x20 bytes
                    // u6e: PLANCK's HitData* (+0x20) is the engine's own HitData its DispatchHitEvent hook was given (fork main.cpp:6086-
                    // 6095) - live during this sink call; totalDamage at +0x50 (PLANCK's VR layout include/RE/misc.h = CommonLib HitData.h)
                    if (const auto hd = *reinterpret_cast<const std::uintptr_t*>(base + 0x20); hd) {
                        h.hasDamage = true;
                        h.hitDamage = *reinterpret_cast<const float*>(hd + 0x50);
                    }
                    h.pos    = *reinterpret_cast<const RE::NiPoint3*>(base + 0x28);
                    h.isLeft = *reinterpret_cast<const bool*>(base + 0x50);
                    const auto* name = reinterpret_cast<const RE::BSFixedString*>(base + 0x48);
                    if (const char* n = name->c_str(); n && *n) h.node = n;
                    const RE::NiAVObject* obj = *reinterpret_cast<RE::NiAVObject* const*>(base + 0x40);
                    for (int up = 0; obj && up <= kHeadParents; ++up, obj = obj->parent) {
                        const char* on = obj->name.c_str();
                        if (on && _stricmp(on, kHeadNode) == 0) {
                            h.head   = true;
                            h.headUp = up;
                            break;
                        }
                    }
                    if (!h.head && _stricmp(h.node.c_str(), kHeadNode) == 0) {   // no node pointer, the name says it
                        h.head   = true;
                        h.headUp = 0;
                    }
                }
                h.gen = g_generation.load();
                TaskRelay::Add([h = std::move(h)] { OnHitTask(h); });
                g_perfHitRelayed.fetch_add(1, std::memory_order_relaxed);
                g_perfHitNs.fetch_add(NsSince(t0), std::memory_order_relaxed);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // 2.1.0 looting - the rules: only while the player SNEAKS (a standing click keeps the old
        // behaviour: Diary of Mine's Wake / Tie-up, talking), always clean (free looting, never a crime, seen or not), other mods'
        // interaction menus first. A living, paralysed, ragdolled NPC has no loot / pickpocket action of its own (in VR the engine
        // opens no menu), so SBKO opens her inventory: SBKO_Helper.LootInventory = OpenInventory(true); the menu sink logs the mode the
        // engine really used. Diary of Mine's own box choices (Cancel / Tie-up / ...) are perk fragments, not this activation.
        // The menus that mean "another interaction is on" - SBKO does not open over them. NOT MessageBoxMenu: Diary of Mine's own
        // "Cancel / Activate" box is the normal way here and may still be closing when this runs.
        constexpr std::array<std::string_view, 18> kLootYieldMenus{
            "ContainerMenu", "Dialogue Menu", "BarterMenu", "GiftMenu", "InventoryMenu", "MagicMenu", "Lockpicking Menu", "Book Menu",
            "Crafting Menu", "Training Menu", "Sleep/Wait Menu", "Console", "Journal Menu", "MapMenu", "StatsMenu", "FavoritesMenu",
            "TweenMenu", "CustomMenu"
        };
        const char* OtherMenuOpen()
        {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return nullptr;
            for (const auto m : kLootYieldMenus)
                if (ui->IsMenuOpen(m)) return m.data();
            return nullptr;
        }

        // Task context: what SBKO holds about her, for looting. Lootable = held, alive, STILL knocked out (unconscious / an old DOWN
        // knock-out) - not entering, coming to or being let go. Takes g_mtx.
        struct LootLook
        {
            bool        held      = false;
            bool        dead      = false;
            std::string who;
            State       state     = State::kDown;
            bool        releasing = false;
            [[nodiscard]] bool Lootable() const { return held && !dead && !releasing && (state == State::kUnconscious || state == State::kDown); }
        };
        LootLook LookForLoot(RE::Actor* a_a, RE::FormID a_id)
        {
            LootLook l;
            l.dead = !a_a || sbko::IsDeadNow(a_a);
            std::scoped_lock lk(g_mtx);
            const auto       it = g_map.find(a_id);
            if (it == g_map.end()) return l;
            l.held      = true;
            l.who       = it->second.who;
            l.state     = it->second.state;
            l.releasing = it->second.releasing;
            return l;
        }

        // Task context (TaskRelay -> SKSE task; in VR an engine job thread inside SKSE's task lock). Only an NPC SBKO holds STILL knocked
        // out is looted. No guard against a second click while her menu is on its way: that window is the two or three frames until
        // the menu opens (then OtherMenuOpen sees it), and a guard would block her for good if a menu ever failed to open.
        void OnLootTask(RE::FormID a_id, std::uint32_t a_gen)
        {
            if (a_gen != g_generation.load() || !g_gameReady.load()) return;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id);
            if (!a || sbko::IsDeadNow(a)) return;   // a corpse: the game's own loot menu
            const LootLook l = LookForLoot(a, a_id);
            if (!l.held) return;   // SBKO does not hold her: the game's own activation, nothing of ours
            const std::string& who       = l.who;
            const State        state     = l.state;
            const bool         releasing = l.releasing;
            if (!l.Lootable()) {
                logger::info("[loot] {} - not looted: she is {} (only a knocked-out NPC lying still can be looted)", who,
                             releasing ? "being let go" : StateName(state));
                return;
            }
            if (const char* m = OtherMenuOpen()) {
                logger::info("[loot] {} - not opened: {} is open (another interaction goes first)", who, m);
                return;
            }
            g_lootPending.store(a_id);
            const bool sent = Helper::LootInventory(a_id);
            if (!sent) g_lootPending.store(0);
            logger::info("[loot] {} - you activated her while sneaking and she is {}: {}", who, StateName(state),
                         sent ? "her inventory opens (free looting, bLootKnockedOut)"
                              : "her inventory did NOT open - SBKO_Helper.pex has no LootInventory (an older script next to this DLL?)");
        }

        // ContainerMenu opening / closing (UI thread; every menu passes here - one name compare first). Recognises SBKO's own loot menu
        // (the pending NPC = the menu's target) and logs the mode the engine used (kNPCMode = clean; kSteal would mean stolen items).
        // Its task then checks she is STILL knocked out - she may have come to while the menu was on its way (CloseOurLoot found no open
        // menu then) - and closes it if not. The close of SBKO's menu is logged too: a close right after the open = another mod took
        // the menu over (VR Backpack turns a sneaking NPC-mode menu on a helpless NPC into its pickpocket panel). No lock of ours, no
        // AddTask: atomics + TaskRelay pushes.
        class LootMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!a_event || a_event->menuName != RE::ContainerMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
                if (!a_event->opening) {
                    const RE::FormID was = g_lootOpen.exchange(0);
                    if (was != 0) TaskRelay::Add([was] { logger::info("[loot] 0x{:08X} - her inventory menu is closed", was); });
                    return RE::BSEventNotifyControl::kContinue;
                }
                const RE::FormID want = g_lootPending.exchange(0);
                if (want == 0) return RE::BSEventNotifyControl::kContinue;   // a container menu of someone else's making
                const auto          mode = static_cast<std::uint32_t>(RE::ContainerMenu::GetContainerMode());
                const auto          ref  = RE::TESObjectREFR::LookupByHandle(RE::ContainerMenu::GetTargetRefHandle());
                const RE::FormID    got  = ref ? ref->GetFormID() : 0;
                const std::uint32_t gen  = g_generation.load();
                if (got == want) g_lootOpen.store(want);
                TaskRelay::Add([want, got, mode, gen] {
                    const char* name = mode == 0 ? "loot" : mode == 1 ? "STEAL (items would be stolen)" : mode == 2 ? "pickpocket" : mode == 3 ? "NPC / trade (clean)" : "?";
                    if (got != want) {
                        logger::info("[loot] 0x{:08X} - a container menu opened, but for 0x{:08X} (mode {}): not SBKO's loot", want, got, mode);
                        return;
                    }
                    logger::info("[loot] 0x{:08X} - her inventory menu is open: container mode {} = {}", want, mode, name);
                    if (gen != g_generation.load()) return;
                    const LootLook l = LookForLoot(RE::TESForm::LookupByID<RE::Actor>(want), want);
                    if (l.Lootable()) return;
                    HideOurLootMenu(want, !l.held ? "is no longer held by SBKO" : l.dead ? "died" : l.releasing ? "is being let go" : "came to while her menu was opening");
                });
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        LootMenuSink g_lootMenuSink;

        // u6f: TESActivateEvent - EVERY activation in the game passes here (doors, containers, NPCs at furniture ...). Only the PLAYER
        // activating an actor goes on: (u6f) one SBKO holds asleep (one set lookup under g_watchMtx; bSleepWakeOnTalk) -> OnSleepWakeTask;
        // u7a (M2) else two field reads - her sit-sleep state, the player's sneaking bit - and a natural sleeper (7 / 8, not sneaking; or
        // anyone while bLogWokenProbe) -> OnWokenEventTask. No engine call, no lock of ours beyond the set, no AddTask.
        class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* a_event, RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                if (!a_event || !a_event->objectActivated || !a_event->actionRef || !g_gameReady.load(std::memory_order_relaxed))
                    return RE::BSEventNotifyControl::kContinue;
                if (!a_event->actionRef->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
                RE::TESObjectREFR* obj = a_event->objectActivated.get();
                if (!obj->Is(RE::FormType::ActorCharacter) || obj->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
                const RE::FormID    tid = obj->GetFormID();
                const std::uint32_t gen = g_generation.load();
                const auto&         cfg = Settings::Get();
                // 2.1.0 looting: the player SNEAKING (Actor::IsSneaking - the sneak bit, not swimming, not mounted) + SBKO holding anyone
                // (one atomic read) -> the loot task (it checks it is HER, still knocked out); this bypasses only OUR wake branch below -
                // a crouched loot never wakes her (the probe still logs: a sneaking player never wakes anyone there). kContinue: the
                // engine and other mods see the activation as always. A standing player goes on below, unchanged.
                bool looting = false;
                if (cfg.lootKnockedOut && g_handled.load(std::memory_order_relaxed) != 0) {
                    const auto* pa = a_event->actionRef.get()->As<RE::Actor>();
                    if (pa && pa->IsSneaking()) {
                        TaskRelay::Add([tid, gen] { OnLootTask(tid, gen); });
                        if (!cfg.logWokenProbe) return RE::BSEventNotifyControl::kContinue;
                        looting = true;
                    }
                }
                if (!looting && g_sleepWatchCount.load(std::memory_order_relaxed) != 0 && g_sleepOn.load(std::memory_order_relaxed) && cfg.sleepWakeOnTalk &&
                    Asleep(tid)) {
                    const RE::FormID cid = a_event->actionRef->GetFormID();
                    TaskRelay::Add([tid, cid, gen] { OnSleepWakeTask(tid, cid, 0, 0, 0, SleepWakeBy::kTalk, gen); });
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!g_wokenTalkOn.load(std::memory_order_relaxed)) return RE::BSEventNotifyControl::kContinue;
                auto*       act = obj->As<RE::Actor>();
                const auto* st  = act ? act->AsActorState() : nullptr;
                auto*       pc  = a_event->actionRef.get()->As<RE::Actor>();
                const auto* pst = pc ? pc->AsActorState() : nullptr;
                if (!st) return RE::BSEventNotifyControl::kContinue;
                WokenCopy w;
                w.target   = tid;
                w.by       = WokenBy::kTalk;
                w.state    = static_cast<std::uint32_t>(st->GetSitSleepState());
                w.sneaking = pst && pst->IsSneaking();
                w.gen      = gen;
                const bool asleep = w.state == kSitSleeping || w.state == kSitWantToWake;
                if ((asleep && !w.sneaking) || cfg.logWokenProbe) TaskRelay::Add([w] { OnWokenEventTask(w); });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // u7a (M2): TESPerkEntryRunEvent - a perk entry's fragment runs (rare: Activate-type entries with a script, picked from a menu). The
        // perk FormID (+0x10) is compared with Diary of Mine's DOMInteractPerk FIRST; only then are the two references read (the layout is
        // UNVERIFIED in VR - a wrong one can never match). With bLogWokenProbe every event is copied (perk, flag, the cause pointer compared
        // with the player - never dereferenced) for the probe line. No engine call, no lock of ours, no AddTask.
        class PerkRunSink final : public RE::BSTEventSink<RE::TESPerkEntryRunEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESPerkEntryRunEvent* a_event, RE::BSTEventSource<RE::TESPerkEntryRunEvent>*) override
            {
                if (!a_event || !g_gameReady.load(std::memory_order_relaxed)) return RE::BSEventNotifyControl::kContinue;
                const bool dom   = g_domPerkId != 0 && a_event->perkId == g_domPerkId;
                const bool probe = Settings::Get().logWokenProbe;
                if (!dom && !probe) return RE::BSEventNotifyControl::kContinue;
                WokenCopy w;
                w.by            = WokenBy::kDomWake;
                w.domPerk       = dom;
                w.perk          = a_event->perkId;
                w.flag          = a_event->flag;
                w.frag          = static_cast<std::int32_t>(a_event->flag);
                w.causeIsPlayer = a_event->cause.get() == static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton());
                w.gen           = g_generation.load();
                if (dom) {
                    RE::TESObjectREFR* t = a_event->target.get();
                    if (t && t->Is(RE::FormType::ActorCharacter) && !t->IsPlayerRef()) {
                        w.target = t->GetFormID();
                        auto*       act = t->As<RE::Actor>();
                        const auto* st  = act ? act->AsActorState() : nullptr;
                        w.state         = st ? static_cast<std::uint32_t>(st->GetSitSleepState()) : 0;
                    } else if (!probe)
                        return RE::BSEventNotifyControl::kContinue;
                }
                TaskRelay::Add([w] { OnWokenEventTask(w); });
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        BleedoutSink g_bleedoutSink;
        DeathSink    g_deathSink;
        CellSink     g_cellSink;
        EffectSink   g_effectSink;
        HitSink      g_hitSink;   // U6
        ActivateSink g_activateSink;   // u6f
        PerkRunSink  g_perkSink;       // u7a (M2)

        // The check thread (VRCD AddOn Options.cpp:556-584): sleeps, then queues AT MOST ONE check task. It holds no lock
        // when it calls AddTask, and the task never re-adds itself (FBG post-mortem).
        void StartCheckThread()
        {
            std::thread([] {
                std::uint64_t flushed = 0;
                auto period = [] {
                    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<float>(Settings::Get().checkSeconds));
                };
                auto nextCheck = std::chrono::steady_clock::now() + period();
                for (;;) {
                    // u2d: sleep until the next check OR the earliest bleedout grace's due moment, whichever comes first
                    {
                        std::unique_lock lk(g_threadMtx);
                        for (;;) {
                            const auto wakeAt = (std::min)(nextCheck, g_graceDue);
                            if (std::chrono::steady_clock::now() >= wakeAt) break;
                            g_threadCv.wait_until(lk, wakeAt);
                        }
                    }
                    const auto now      = std::chrono::steady_clock::now();
                    bool       graceDue = false;
                    {
                        std::scoped_lock lk(g_threadMtx);
                        if (now >= g_graceDue) {
                            g_graceDue = (std::chrono::steady_clock::time_point::max)();
                            graceDue   = true;
                        }
                    }
                    if (graceDue && g_gameReady.load() && !g_graceQueued.exchange(true)) {
                        if (auto* tasks = SKSE::GetTaskInterface())
                            tasks->AddTask([] { GraceTask(); });
                        else
                            g_graceQueued.store(false);
                    }
                    if (now < nextCheck) continue;   // woken only for a grace
                    nextCheck = now + period();
                    // u2c P2: the log goes to disk HERE - on this thread of ours, never the game's - when lines were written since the
                    // last wake (so at most one check interval of info lines is lost in a crash; warnings flush at once). No flush
                    // per line, and no spdlog flusher thread (a thread killed mid-flush at exit would leave its lock held).
                    if (const std::uint64_t lines = g_logLines.load(std::memory_order_relaxed); lines != flushed) {
                        flushed = lines;
                        if (auto log = spdlog::default_logger()) log->flush();
                    }
                    if (!g_gameReady.load()) continue;
                    if (g_tickQueued.exchange(true)) continue;
                    if (auto* tasks = SKSE::GetTaskInterface())
                        tasks->AddTask([] { Tick(); });
                    else
                        g_tickQueued.store(false);
                }
            }).detach();
        }
    }

    // u2d (regen 0.1 for all protected and essential NPCs): the engine's out-of-combat bonus for Essential NPCs (5.0 HP/s by
    // default, usually not overridden by any plugin) is a GAME SETTING, so it is set from SBKO's ini at data load and again after
    // every load (game settings are not saved). GameSettingCollection = RELOCATION_ID(514622, 400782), VR 0x2F8A780 in
    // version-1-4-15-0.csv:10559; GetSetting is a map lookup, Setting::SetFloat a field write (Setting.h:83). -1 = not touched.
    // u2f: one writer for each float game setting SBKO owns ([Regen]); a_want < 0 = not touched.
    void ApplyGameSetting(const char* a_when, const char* a_name, float a_want, const char* a_what)
    {
        auto* gsc = RE::GameSettingCollection::GetSingleton();
        auto* s   = gsc ? gsc->GetSetting(a_name) : nullptr;
        if (!s) {
            logger::warn("[regen] {}: the game setting {} was NOT found - {} is unchanged", a_when, a_name, a_what);
            return;
        }
        const float old = s->GetFloat();
        if (a_want < 0.0f) {
            logger::info("[regen] {}: {} left at {} (ini value {} < 0 = not touched)", a_when, a_name, old, a_want);
            return;
        }
        s->SetFloat(a_want);
        logger::info("[regen] {}: game setting {} {} -> {} (SBKO ini [Regen]; {}; console: getgs {})", a_when, a_name, old, s->GetFloat(),
                     a_what, a_name);
    }

    // u6 fix 1 (F5): PLANCK's interface, once, at kDataLoaded (after every plugin's kPostPostLoad - PLANCK registers its listener at
    // its own load). Only when bHeadHit is on and activeragdoll.dll is loaded. The request message 0x92F38745 is the one other PLANCK
    // clients already send (e.g. PPB's DismemberGuard: "DG: PLANCK interface=OK build=80100") - the known broadcast
    // CTD risk (InteractiveActivatorsVR) is therefore not a new exposure. Any failure = the magic word alone (one line).
    void AcquirePlanck()
    {
        if (!Settings::Get().headHit || g_planckApi.load()) return;
        // u6 fix 3: PLANCK present = its DLL is in the process (decided here, once; the interface below is optional on top of it)
        const bool present = REX::W32::GetModuleHandleA("activeragdoll.dll") != nullptr;
        g_planckPresent.store(present, std::memory_order_relaxed);
        if (!present) {
            logger::info("[KO] U6: PLANCK (activeragdoll.dll) is not loaded - PLANCK's extended hit block is NEVER read; u6b: a mace / "
                         "warhammer then counts ANYWHERE (the Knockout mod's rule - it has no hit-location test); bashes count anywhere");
            return;
        }
        auto* msg = SKSE::GetMessagingInterface();
        struct PlanckMessage
        {
            void* (*GetApiFunction)(unsigned int a_revision) = nullptr;
        } pm;
        if (!msg || !msg->Dispatch(0x92F38745, &pm, sizeof(void*), "PLANCK") || !pm.GetApiFunction) {
            logger::info("[KO] U6: PLANCK did not answer the interface request - a PLANCK hit is recognised by its magic word alone");
            return;
        }
        auto* api = static_cast<IPlanckInterface001*>(pm.GetApiFunction(1));
        if (!api) {
            logger::info("[KO] U6: PLANCK's GetApiFunction(1) returned null - a PLANCK hit is recognised by its magic word alone");
            return;
        }
        const std::uint32_t build = api->GetBuildNumber();   // slot 0: the same in every layout
        if (build < 80000) {
            logger::info("[KO] U6: PLANCK build {} is older than 0.8.0 (GetCurrentHitEvent sits elsewhere) - a PLANCK hit is recognised by its "
                         "magic word alone",
                         build);
            return;
        }
        g_planckApi.store(api, std::memory_order_release);
        logger::info("[KO] U6: PLANCK interface OK, build {} - a PLANCK hit = the magic word AND GetCurrentHitEvent() == the event", build);
    }

    void ApplyRegenSetting(const char* a_when)
    {
        ApplyGameSetting(a_when, "fEssentialNonCombatHealRateBonus", Settings::Get().essentialRegenBonus,
                         "the engine's out-of-combat health bonus for essential NPCs");
        // u2f (1 %): an UNCONSCIOUS (or restrained) essential / protected NPC hit by a lethal blow is set to this fraction
        // of her max instead of going down (kill path SkyrimVR.exe+0x60CA19, the only reader; engine 0.3) - seen in game: -28 -> 30 %
        ApplyGameSetting(a_when, "fEssentialHealthPercentReGain", Settings::Get().essentialLethalHitHealth,
                         "the Health an unconscious / restrained essential or protected NPC is set to when a lethal blow lands");
    }

    // u6b (U5): a keyword by EditorID - the engine's EditorID map first, else a walk of the loaded keywords (BGSKeyword keeps its EDID)
    RE::BGSKeyword* FindKeyword(const char* a_edid)
    {
        if (auto* k = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(a_edid)) return k;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return nullptr;
        for (auto* k : dh->GetFormArray<RE::BGSKeyword>()) {
            const char* e = k ? k->GetFormEditorID() : nullptr;
            if (e && _stricmp(e, a_edid) == 0) return k;
        }
        return nullptr;
    }

    // u6b (U5): the damage hook + the 6 effect-scope hooks, ONCE at kDataLoaded (main menu - no damage can run while a slot is written),
    // only with bChanceKO 1. HandleHealthDamage = Character vtable slot 0x104 on SE / AE, 0x106 on VR (CommonLib Actor.cpp:2183
    // RelocateVirtual(0x104, 0x106); VR Character vtable 0x16D6DE0 = VTABLE_Character[0]). write_vfunc returns what was in the slot
    // (Injury Engine's own hook - it installs at its plugin load, before kDataLoaded - or the game's), and the thunk always calls it.
    // u7a (U4): one line for the FDC re-roll's state at start
    void LogU4(const char* a_off)
    {
        const auto& cfg = Settings::Get();
        auto found = [](const RE::TESForm* f) { return f ? std::format("0x{:08X}", f->GetFormID()) : std::string("NOT found"); };
        if (a_off)
            logger::info("[KO] U4 FDC re-roll on hits: OFF ({})", a_off);
        else
            logger::info("[KO] U4 FDC re-roll on hits: ON - FDC_SBKO_RolledFaction {}, FDC's effect Inju3_MainEffect {} (log only); the damage hook "
                         "serves U4 (U5 {}) | a follower SBKO holds after a BLEEDOUT knock-out (FDC rolled once: mark 1) gets one more FDC roll per "
                         "direct hit while you are in combat | {} | a re-roll that misses {} | hit / sleep / choke knock-outs never (mark 2)",
                         found(g_fdcRolledFaction), found(g_fdcEffect), g_u5On.load() ? "ON" : "OFF",
                         cfg.fdcRerollPlayerHits ? "your own hits count too" : "only OTHER people's hits (yours never - bFdcRerollPlayerHits 0)",
                         cfg.fdcRerollDeathOnly ? "does nothing" : "= FDC's full roll (small injury)");
    }

    // u6b (U5) + u7a (U4): ONE damage hook for both. Installed when bChanceKO OR (bFdcRerollOnHit AND FDC_SBKO_Patch.esp loaded).
    void InstallChanceKO()
    {
        const auto& cfg    = Settings::Get();
        const bool  wantU4 = cfg.fdcRerollOnHit && g_fdcRolledFaction != nullptr;
        const char* u4Off  = !cfg.fdcRerollOnHit ? "bFdcRerollOnHit 0" : !g_fdcRolledFaction ? "FDC_SBKO_Patch.esp not loaded" : nullptr;
        if (!cfg.chanceKO) {
            logger::info("[KO] U5 chance knock-out: OFF ({}){}",
                         !cfg.koNormal ? "bKnockOutNormalNPCs 0 - an ordinary NPC is never knocked out"
                                       : "bChanceKO 0 - every NPC that bleeds out is knocked out, as before u6b",
                         wantU4 ? "; the damage hook is still installed for U4" : " - no damage hook");
            if (!wantU4) {
                LogU4(u4Off);
                return;
            }
        }
        std::size_t cloaks = 0, hazards = 0;
        if (auto* dh = RE::TESDataHandler::GetSingleton()) {
            for (auto* m : dh->GetFormArray<RE::EffectSetting>())
                if (m && m->GetArchetype() == RE::EffectArchetypes::ArchetypeID::kCloak && m->data.associatedForm &&
                    g_tickSpells.insert(m->data.associatedForm->GetFormID()).second)
                    ++cloaks;
            for (auto* hz : dh->GetFormArray<RE::BGSHazard>())
                if (hz && hz->data.spell && g_tickSpells.insert(hz->data.spell->GetFormID()).second) ++hazards;
        }
        // (u6f: the Devious Devices keywords are found in OnDataLoaded now - helmet rule A needs them with bChanceKO 0 too)

        const bool        vr   = REL::Module::IsVR();
        const std::size_t slot = vr ? 0x106 : 0x104;
        REL::Relocation<std::uintptr_t> character{ RE::VTABLE_Character[0] };
        // u6d (F8): every slot is checked before it is written - a refused HandleHealthDamage slot = U5 OFF (no half-installed state);
        // a refused effect slot = that effect class only is not marked (its damage then reads as physical - logged)
        std::string hhdWhere;
        if (!SlotLooksSane(character.address(), slot, hhdWhere)) {
            logger::error("[KO] U5 chance knock-out: NOT installed - the Character vtable's HandleHealthDamage slot 0x{:X} failed the check ({}); "
                          "U5 is OFF this session (an ordinary NPC that bleeds out is knocked out without a roll, as before u6b)",
                          slot, hhdWhere);
            LogU4(wantU4 ? "the damage hook could not be installed" : u4Off);
            return;
        }
        DamageHook::func = character.write_vfunc(slot, DamageHook::thunk);
        std::string scopes;
        int         scopesOk = 0, scopesSkipped = 0;
        auto add = [&](std::string a_text, bool a_ok) {
            (a_ok ? scopesOk : scopesSkipped) += 1;
            scopes += (scopes.empty() ? "" : "; ") + a_text;
        };
        bool ok = false;
        add(EffectScope<0>::Install(RE::VTABLE_ValueModifierEffect[0], "ValueModifier", ok), ok);
        add(EffectScope<1>::Install(RE::VTABLE_DualValueModifierEffect[0], "DualValueModifier", ok), ok);
        add(EffectScope<2>::Install(RE::VTABLE_AbsorbEffect[0], "Absorb", ok), ok);
        add(EffectScope<3>::Install(RE::VTABLE_AccumulatingValueModifierEffect[0], "AccumulatingValueModifier", ok), ok);
        add(EffectScope<4>::Install(RE::VTABLE_PeakValueModifierEffect[0], "PeakValueModifier", ok), ok);
        add(EffectScope<5>::Install(RE::VTABLE_TargetValueModifierEffect[0], "TargetValueModifier", ok), ok);
        add(EffectScope<6>::Install(RE::VTABLE_ValueAndConditionsEffect[0], "ValueAndConditions", ok), ok);   // u6d (F3)
        add(EffectScope<7>::Install(RE::VTABLE_GrabActorEffect[0], "GrabActor", ok), ok);                     // u6d (F3)
        g_u5On.store(cfg.chanceKO);
        g_fdcOn.store(wantU4);   // u7a
        auto found = [](const RE::TESForm* f) { return f ? std::format("0x{:08X}", f->GetFormID()) : std::string("not found"); };
        if (cfg.chanceKO)
        logger::info("[KO] U5 chance knock-out: HandleHealthDamage hook on Character vtable slot 0x{:X} ({}; it held {} - chained) + the "
                     "value-modifier effect scope at vfunc 0x20 on {} classes{} | a NORMAL NPC hit by anyone to 0 < Health <= {:.0f}% rolls; "
                     "punches and non-hostile weapons never roll; summoned / commanded actors never; creature attacks {} | ticks: {} cloak + "
                     "{} hazard spells known | Devious Devices keywords: zad_Lockable {}, zad_InventoryDevice {} (DD items are not armour) | a "
                     "normal NPC's bleedout knocks her out only when her roll says so",
                     slot, vr ? "VR" : "SE / AE", hhdWhere, scopesOk, scopesSkipped ? std::format(" ({} SKIPPED)", scopesSkipped) : "",
                     cfg.chanceKOHealthBelow * 100.0f, cfg.chanceKOCreatureHits ? "roll" : "never roll (bChanceKOCreatureHits 0)", cloaks,
                     hazards, found(g_kwDDLockable), found(g_kwDDInventory));
        else
            logger::info("[KO] damage hook (u7a: for U4 only, U5 is OFF): HandleHealthDamage on Character vtable slot 0x{:X} ({}; it held {} - "
                         "chained) + the value-modifier effect scope on {} classes{} | ticks: {} cloak + {} hazard spells known",
                         slot, vr ? "VR" : "SE / AE", hhdWhere, scopesOk, scopesSkipped ? std::format(" ({} SKIPPED)", scopesSkipped) : "", cloaks,
                         hazards);
        if (scopesSkipped)
            logger::warn("[KO] U5 effect scope slots: {}", scopes);
        else
            logger::info("[KO] U5 effect scope slots: {}", scopes);
        LogU4(wantU4 ? nullptr : u4Off);
    }

    // u7a: a record of ANY plugin by its local id - the toolbox lookup first, then the light-plugin id built by hand (LookupOwn's formula)
    template <class T>
    T* LookupIn(RE::TESDataHandler* a_dh, RE::FormID a_local, const char* a_plugin)
    {
        if (!a_dh) return nullptr;
        if (auto* f = a_dh->LookupForm<T>(a_local, a_plugin)) return f;
        if (const RE::TESFile* file = a_dh->LookupModByName(a_plugin); file && file->GetCompileIndex() != 0xFF) {
            const RE::FormID id = file->IsLight()
                                      ? (0xFE000000u | (static_cast<RE::FormID>(file->GetSmallFileCompileIndex()) << 12) | (a_local & 0xFFFu))
                                      : ((static_cast<RE::FormID>(file->GetCompileIndex()) << 24) | a_local);
            return RE::TESForm::LookupByID<T>(id);
        }
        return nullptr;
    }

    // u6c: one of SBKO's OWN records (SimpleBleedoutKO_001.esp, light) by its local id - the toolbox lookup first, then the light-plugin
    // id built by hand (the FDC fallback's formula). Null when the ESP does not have it (an older ESP) or it is another record type.
    template <class T>
    T* LookupOwn(RE::TESDataHandler* a_dh, RE::FormID a_local)
    {
        if (!a_dh) return nullptr;
        if (auto* f = a_dh->LookupForm<T>(a_local, kPlugin)) return f;
        if (const RE::TESFile* file = a_dh->LookupModByName(kPlugin); file && file->GetCompileIndex() != 0xFF) {
            const RE::FormID id = file->IsLight()
                                      ? (0xFE000000u | (static_cast<RE::FormID>(file->GetSmallFileCompileIndex()) << 12) | a_local)
                                      : ((static_cast<RE::FormID>(file->GetCompileIndex()) << 24) | a_local);
            return RE::TESForm::LookupByID<T>(id);
        }
        return nullptr;
    }

    // u6c: the Sleep spell, ONCE at kDataLoaded and BEFORE the effect sink is registered (the sink reads g_sleepMgefs with no lock).
    // No SBKO_KnockOutEffect / SBKO_SleepEffect in the ESP (an older ESP) = OFF with one line; the faction alone decides whether the
    // knock-outs write the marker (SBKO_SleepFaction rank, 0 for every other knock-out).
    void InstallSleep()
    {
        const auto& cfg = Settings::Get();
        auto*       dh  = RE::TESDataHandler::GetSingleton();
        g_sleepKeyword  = LookupOwn<RE::BGSKeyword>(dh, kSleepKeywordLocal);
        g_sleepEffect   = LookupOwn<RE::EffectSetting>(dh, kSleepEffectLocal);
        g_sleepFaction  = LookupOwn<RE::TESFaction>(dh, kSleepFactionLocal);
        g_sleepProjectile = LookupOwn<RE::BGSProjectile>(dh, kSleepProjectileLocal);   // u6f
        g_sleepMgefs.clear();
        std::string names;
        auto add = [&](const RE::EffectSetting* a_m) {
            if (!a_m || std::find(g_sleepMgefs.begin(), g_sleepMgefs.end(), a_m->GetFormID()) != g_sleepMgefs.end()) return;
            g_sleepMgefs.push_back(a_m->GetFormID());
            const char* n = a_m->GetName();
            names += std::format("{}{} 0x{:08X}", names.empty() ? "" : ", ", n && *n ? n : "(no name)", a_m->GetFormID());
        };
        add(g_sleepEffect);
        if (g_sleepKeyword && dh)
            for (auto* m : dh->GetFormArray<RE::EffectSetting>())
                if (m && m->HasKeyword(g_sleepKeyword)) add(m);
        std::sort(g_sleepMgefs.begin(), g_sleepMgefs.end());
        auto found = [](const RE::TESForm* f) { return f ? std::format("0x{:08X}", f->GetFormID()) : std::string("NOT found"); };
        if (g_sleepMgefs.empty()) {
            g_sleepMgefs.clear();
            logger::info("[KO] Sleep spell (u6c): OFF - {} has no SBKO_KnockOutEffect (local 0x{:03X}) / SBKO_SleepEffect (local 0x{:03X}) "
                         "(an older ESP){}",
                         kPlugin, kSleepKeywordLocal, kSleepEffectLocal,
                         g_sleepFaction ? std::format("; SBKO_SleepFaction {} found - knock-outs still reset its rank", found(g_sleepFaction))
                                        : std::string(""));
            return;
        }
        g_sleepOn.store(cfg.sleep);
        logger::info("[KO] Sleep spell (u6c): {} | {} effect(s): {} (SBKO_KnockOutEffect {}) | the PLAYER's cast on a valid NPC = SBKO's hit "
                     "knock-out, 100 % (no roll, no detection or Health rule, no helmet test); U6's targets; followers {}; per-cast log {} | "
                     "seen = her RequestDetectionLevel(player) > 0 at the knock-out -> SBKO_SleepFaction {} rank 2 (seen) / 1 (not seen), "
                     "0 for every other knock-out and at the coming-to's end{}",
                     cfg.sleep ? "ON" : "OFF (bSleep 0)", g_sleepMgefs.size(), names, found(g_sleepKeyword),
                     cfg.sleepFollowers ? "too" : "NOT (bSleepFollowers 0)", cfg.logSleep ? "on" : "off", found(g_sleepFaction),
                     g_sleepFaction ? "" : " (NO faction: the knock-out works, the wake prompt is the ordinary one)");
        logger::info("[KO] Sleep wake (u6f): a Sleep-spell sleeper wakes STRAIGHT UP (the heal wake's path; injury knock-outs still need a "
                     "heal) on any hit by anyone {} | when the player talks to (activates) her {} | another mod's Wake (Diary of Mine's "
                     "menu) always | the spell's own hit never counts (its spell / scroll, projectile {})",
                     cfg.sleepWakeOnHit ? "ON" : "OFF (bSleepWakeOnHit 0)", cfg.sleepWakeOnTalk ? "ON" : "OFF (bSleepWakeOnTalk 0)",
                     found(g_sleepProjectile));
    }

    void OnDataLoaded()
    {
        if (g_started.exchange(true)) return;
        ApplyRegenSetting("kDataLoaded");

        auto* dh  = RE::TESDataHandler::GetSingleton();
        g_faction = dh ? dh->LookupForm<RE::TESFaction>(kFactionLocalID, kPlugin) : nullptr;
        if (g_faction)
            logger::info("[KO] SBKO_StateFaction = 0x{:08X} ({} local 0x{:03X})", g_faction->GetFormID(), kPlugin, kFactionLocalID);
        else
            logger::warn("[KO] SBKO_StateFaction (local 0x{:03X} in {}) NOT FOUND - knockouts still work, but SkyrimNet is not "
                         "told (no rank) and no v10 knockout can be adopted from a save",
                         kFactionLocalID, kPlugin);

        g_kwNPC = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);
        if (g_kwNPC)
            logger::info("[KO] ActorTypeNPC keyword = 0x{:08X} (by ID, Skyrim.esm)", g_kwNPC->GetFormID());
        else
            logger::error("[KO] the ActorTypeNPC keyword (0x{:08X}, Skyrim.esm) was NOT found - no NPC passes the filter, the "
                          "knockout is OFF this session",
                          kActorTypeNPC);

        g_brawlFaction = RE::TESForm::LookupByID<RE::TESFaction>(kBrawlFaction);
        if (g_brawlFaction)
            logger::info("[KO] brawl guard: DGIntimidateFaction = 0x{:08X} (by ID, Skyrim.esm)", g_brawlFaction->GetFormID());
        else
            logger::warn("[KO] brawl guard OFF: DGIntimidateFaction (0x{:08X}, Skyrim.esm) was NOT found", kBrawlFaction);

        // ---- U1 forms (all soft: an absent mod = no rule) ----
        g_followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(kFollowerFaction);
        logger::info("[KO] follower log: IsPlayerTeammate OR CurrentFollowerFaction {} (the SeverActions guard itself is decided by "
                     "SeverActions' own lists)",
                     g_followerFaction ? std::format("0x{:08X} rank >= 0", g_followerFaction->GetFormID())
                                       : std::string("(NOT found - teammates only)"));
        g_vrteFaction = dh ? dh->LookupForm<RE::TESFaction>(kVrteFactionLocalID, kVrtePlugin) : nullptr;
        logger::info("[KO] VRTE guard: {}", g_vrteFaction ? std::format("VRTE_ChokeStateFaction = 0x{:08X} - rank 5 = VRTE holds her, "
                                                                        "SBKO never knocks out / re-asserts on her",
                                                                        g_vrteFaction->GetFormID())
                                                          : std::string("VRTouchEvents.esp not loaded - no rule"));
        int potions = 0;
        for (int i = 0; i < 3; ++i) {
            g_potion[i] = dh ? dh->LookupForm<RE::SpellItem>(kPotionLocal[i], kPotionPlugin) : nullptr;
            if (g_potion[i]) ++potions;
        }
        logger::info("[KO] potions off while unconscious: {} of 3 Smart NPC Potions abilities found{}", potions,
                     potions ? "" : " (Smart_NPC_Potions.esp not loaded - nothing to take off)");
        {
            const RE::TESFile* sev   = dh ? dh->LookupModByName(kSeverPlugin) : nullptr;   // TESDataHandler.cpp:176 (a walk of `files`)
            const bool         onLO  = sev && sev->compileIndex != 0xFF;
            auto*              quest = onLO ? dh->LookupForm<RE::TESQuest>(kSeverQuestLocal, kSeverPlugin) : nullptr;
            g_severLoaded            = onLO && quest;
            logger::info("[KO] SeverActions guard: {}",
                         g_severLoaded ? std::format("SeverActions.esp loaded, survival quest 0x{:08X} - every knocked-out NPC is "
                                                     "offered to SBKO_SeverCompat.Guard (SeverActions' own lists decide; "
                                                     "bSeverActionsGuard {})",
                                                     quest->GetFormID(), Settings::Get().severActionsGuard ? 1 : 0)
                         : onLO ? std::string("SeverActions.esp loaded but its quest 0xD62 was NOT found - guard OFF")
                                : std::string("SeverActions.esp not loaded - nothing to guard"));
        }
        // ---- U2a forms ----
        g_domFaction    = dh ? dh->LookupForm<RE::TESFaction>(kDomFactionLocal, kDomPlugin) : nullptr;
        g_domBagFaction = dh ? dh->LookupForm<RE::TESFaction>(kDomBagFactionLocal, kDomPlugin) : nullptr;
        g_domTiedFaction = dh ? dh->LookupForm<RE::TESFaction>(kDomTiedFactionLocal, kDomPlugin) : nullptr;
        g_domSlaveFaction = dh ? dh->LookupForm<RE::TESFaction>(kDomSlaveFactionLocal, kDomPlugin) : nullptr;
        logger::info("[KO] taken by another mod (SBKO lets go): DOMBeingCaptured {} | DOMActionTied {} | DOMActionInContainer (bag) {} | "
                     "a restrained life state | a disabled reference | VRTE rank 5; a released NPC who is given back still unconscious "
                     "and no longer taken is re-adopted (never while she is a DoM slave)",
                     g_domFaction ? std::format("0x{:08X}", g_domFaction->GetFormID()) : std::string("(DiaryOfMine.esm not loaded)"),
                     g_domTiedFaction ? std::format("0x{:08X}", g_domTiedFaction->GetFormID()) : std::string("(not found)"),
                     g_domBagFaction ? std::format("0x{:08X}", g_domBagFaction->GetFormID()) : std::string("(not found)"));
        logger::info("[KO] heal wake: bHealWake {} - the first real heal wakes her STRAIGHT UP (U2b: no bleedout window)",
                     Settings::Get().healWake ? 1 : 0);
        logger::info("[KO] external wake (U2b): another mod clearing an unconscious NPC's flag (Diary of Mine's menu Wake) does NOT wake "
                     "her - SBKO puts her back down and shows \"<name> is too injured to wake up.\" (bNotify {}) - u6f: EXCEPT a "
                     "Sleep-spell sleeper, who wakes up",
                     Settings::Get().notify ? 1 : 0);
        logger::info("[KO] u2c: NoBleedoutRecovery ON while SBKO holds an NPC, OFF on every exit; the orphan net (rank 10 + unconscious, no "
                     "record) runs on a cell attach and once after a load; light-plugin ids are saved by plugin name ('LREF'); one PERF "
                     "line per minute of play (measurement only); bVerboseChecks {}",
                     Settings::Get().verboseChecks ? 1 : 0);
        logger::info("[KO] u2d: {:.1f} s in the engine's bleedout before every knock-out (fBleedoutGraceSeconds - his sequencing: other "
                     "mods act on bleedout first); every wake raises Health to at least {:.0f}% of max (fWakeHealthFraction)",
                     Settings::Get().bleedoutGraceSeconds, Settings::Get().wakeHealthFraction * 100.0f);
        // ---- U6 forms (Skyrim.esm, by ID) ----
        g_kwGhost       = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeGhost);
        g_questBrawl    = RE::TESForm::LookupByID<RE::TESQuest>(kQuestBrawl);
        g_questC00      = RE::TESForm::LookupByID<RE::TESQuest>(kQuestC00);
        g_questC00Fight = RE::TESForm::LookupByID<RE::TESQuest>(kQuestC00Fight);
        // u6 fix 2: FDC_SBKO_RolledFaction - the toolbox lookup first, then the light-plugin id built by hand (FE | slot << 12 | local,
        // the same formula as the co-save's ResolveSaved): the patch is an ESL
        g_fdcRolledFaction = dh ? dh->LookupForm<RE::TESFaction>(kFdcRolledFactionLocal, kFdcPatchPlugin) : nullptr;
        if (!g_fdcRolledFaction && dh) {
            if (const RE::TESFile* f = dh->LookupModByName(kFdcPatchPlugin); f && f->GetCompileIndex() != 0xFF) {
                const RE::FormID id = f->IsLight()
                                          ? (0xFE000000u | (static_cast<RE::FormID>(f->GetSmallFileCompileIndex()) << 12) | kFdcRolledFactionLocal)
                                          : ((static_cast<RE::FormID>(f->GetCompileIndex()) << 24) | kFdcRolledFactionLocal);
                g_fdcRolledFaction = RE::TESForm::LookupByID<RE::TESFaction>(id);
            }
        }
        // u7a (U4): FDC's own effect (log only - the task says whether it is running on her)
        g_fdcEffect = LookupIn<RE::EffectSetting>(dh, kFdcEffectLocal, kFdcPlugin);
        // u7a (M2): Diary of Mine's interaction perk (its menu: Wake / Bag / Abduct / Untie / Tie-up)
        g_domInteractPerk = LookupIn<RE::BGSPerk>(dh, kDomInteractPerkLocal, kDomPlugin);
        g_domPerkId       = g_domInteractPerk ? g_domInteractPerk->GetFormID() : 0;
        // u6b / u6f: Devious Devices' item keywords (by EditorID; null = DD absent) - U5's armour read AND helmet rule A skip such items
        g_kwDDLockable  = FindKeyword("zad_Lockable");
        g_kwDDInventory = FindKeyword("zad_InventoryDevice");
        {
            const auto& c = Settings::Get();
            auto        found = [](const RE::TESForm* f) { return f ? std::format("0x{:08X}", f->GetFormID()) : std::string("NOT found"); };
            logger::info("[KO] helmet rule A (u6f; U6's immunity and U5's helmet factor): a WORN armour on slot 30 (head) or 31 (hair) that "
                         "is Light or Heavy armour = a helmet; Clothing (hoods, circlets, cloth wigs, capes) and items off slots 30 / 31 "
                         "never count, whatever keyword they carry (ArmorHelmet is no longer the test); Devious Devices items skipped "
                         "(zad_Lockable {}, zad_InventoryDevice {})",
                         found(g_kwDDLockable), found(g_kwDDInventory));
            // u6g: [HeadHit] sHelmetIgnorePlugins - the list as resolved now; each name that is not a loaded plugin is named once, here
            if (c.helmetIgnorePlugins.empty()) {
                logger::info("[KO] helmet rule A (u6g): sHelmetIgnorePlugins is empty - no plugin is ignored (a wig that is Light / Heavy "
                             "armour on slot 30 / 31 counts as a helmet)");
            } else {
                std::string loaded, missing;
                std::size_t nLoaded = 0;
                for (const auto& n : c.helmetIgnorePlugins) {
                    const RE::TESFile* f = dh ? dh->LookupModByName(n) : nullptr;   // TESDataHandler.cpp:176 (a walk of `files`)
                    if (f && f->GetCompileIndex() != 0xFF) {
                        loaded += std::format("{}{} ({})", loaded.empty() ? "" : ", ", f->GetFilename(),
                                              f->IsLight() ? std::format("light FE{:03X}", f->GetSmallFileCompileIndex())
                                                           : std::format("{:02X}", f->GetCompileIndex()));
                        ++nLoaded;
                    } else {
                        missing += std::format("{}{}", missing.empty() ? "" : ", ", n);
                    }
                }
                logger::info("[KO] helmet rule A (u6g): sHelmetIgnorePlugins - a piece DEFINED by one of these plugins is never a helmet "
                             "(U6's immunity and U5's helmet factor): {} of {} loaded: {}",
                             nLoaded, c.helmetIgnorePlugins.size(), loaded.empty() ? std::string("(none)") : loaded);
                if (!missing.empty())
                    logger::info("[KO] helmet rule A (u6g): sHelmetIgnorePlugins - NOT loaded (nothing to ignore from them): {}", missing);
            }
            if (!c.headHit)
                logger::info("[KO] U6 head-hit / sneak knock-out: OFF (bHeadHit 0) - the player's hits never knock out");
            else
                logger::info("[KO] U6 head-hit / sneak knock-out: the player's hit on an NPC - a bash {} anywhere, a mace or warhammer {} {} "
                             "(u6 fix 2: punches never knock out); 100 % when a sneak attack or undetected {} / "
                             "Health <= {:.0f}% {}; a worn helmet (rule A) = immune {}; followers {}; per-hit log {}; Follower Death Chance "
                             "roll for a hit knock-out: {} | forms: ActorTypeGhost {}, DGIntimidateQuest {}, C00 {}, C00JorrvaskrFight {} (a "
                             "missing form = that rule is off)",
                             c.headHitBash ? "on" : "OFF", c.headHitBlunt ? "on" : "OFF",
                             g_planckPresent.load() ? "- on PLANCK's head node only (PLANCK is loaded)"
                                                    : "- ANYWHERE (u6b: PLANCK is not loaded - the Knockout mod's rule)",
                             c.headHitSneak ? "on" : "OFF", c.headHitHealthThreshold * 100.0f, c.headHitLowHealth ? "on" : "OFF",
                             c.headHitHelmetProtects ? "on" : "OFF", c.headHitFollowers ? "too" : "NOT", c.logHeadHits ? "on" : "off",
                             g_fdcRolledFaction ? std::format("NONE (FDC_SBKO_RolledFaction {} rank 2 before rank 10)",
                                                              found(g_fdcRolledFaction))
                                                : std::string("- (FDC_SBKO_Patch.esp not loaded)"),
                             found(g_kwGhost), found(g_questBrawl), found(g_questC00), found(g_questC00Fight));
        }

        InstallChanceKO();   // u6b (U5): before the hit sink - the sink remembers hits for the damage hook only when it is in
        InstallSleep();      // u6c: before the effect sink - its effect list is read by the sink with no lock

        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESEnterBleedoutEvent>(&g_bleedoutSink);
            holder->AddEventSink<RE::TESDeathEvent>(&g_deathSink);
            holder->AddEventSink<RE::TESCellAttachDetachEvent>(&g_cellSink);
            const bool sleep   = g_sleepOn.load();   // u6c
            const bool marks   = Marks::On();   // m34: the marks read hits (IE's ping attribution) and heals on marked actors
            const bool effects = Settings::Get().logEffects || Settings::Get().healWake || sleep || marks;
            if (effects) holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_effectSink);
            const bool u6   = Settings::Get().headHit;   // U6
            const bool u5   = g_u5On.load();             // u6b: U5 needs the hit's weapon (a punch never rolls)
            const bool wakeHit  = sleep && Settings::Get().sleepWakeOnHit;    // u6f: a hit wakes a sleeper
            const bool wakeTalk = sleep && Settings::Get().sleepWakeOnTalk;   // u6f: the player's talk wakes a sleeper
            const bool u4       = g_fdcOn.load();                            // u7a: U4 needs the hit's weapon too
            const bool hits = u6 || u5 || wakeHit || u4 || marks;
            if (hits) holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
            // u7a (M2): the activate sink also serves "woken to talk"; the perk sink serves DoM's Wake
            const auto& cw = Settings::Get();
            g_wokenTalkOn.store(cw.wokenNarration && cw.wokenOnTalk);
            g_wokenPerkOn.store(cw.wokenNarration && cw.wokenOnDomWake && g_domPerkId != 0);
            if (wakeTalk || g_wokenTalkOn.load() || cw.lootKnockedOut) holder->AddEventSink<RE::TESActivateEvent>(&g_activateSink);   // 2.1.0: + looting
            if (cw.lootKnockedOut) {   // 2.1.0: SBKO's own loot menu (mode log, close on wake / death)
                if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_lootMenuSink);
            }
            if (g_wokenPerkOn.load()) holder->AddEventSink<RE::TESPerkEntryRunEvent>(&g_perkSink);
            logger::info("[KO] M2 woken to talk: {} | talk to an NPC asleep in a bed {} | Diary of Mine's Wake {} (DOMInteractPerk {}) | the talk path's "
                         "SkyrimNet mode {} | probe lines {}",
                         cw.wokenNarration ? "ON" : "OFF (bWokenNarration 0)", g_wokenTalkOn.load() ? "ON (TESActivateEvent)" : "off",
                         g_wokenPerkOn.load() ? "ON (TESPerkEntryRunEvent - its layout is UNVERIFIED in VR: the probe line settles it)"
                                              : (g_domPerkId == 0 ? "off (DiaryOfMine.esm not loaded)" : "off"),
                         g_domPerkId ? std::format("0x{:08X}", g_domPerkId) : std::string("not found"),
                         cw.wokenTalkMode == 1 ? "1 (she answers you)" : "0 (she only knows it)", cw.logWokenProbe ? "ON (bLogWokenProbe 1)" : "off");
            std::string hitUses;
            if (u6) hitUses += "U6: the player's head-hit / sneak knock-out";
            if (u5) hitUses += std::string(hitUses.empty() ? "" : "; ") + "U5: every hit's weapon for the damage hook";
            if (u4) hitUses += std::string(hitUses.empty() ? "" : "; ") + "u7a U4: the hit's weapon for the FDC re-roll";
            if (wakeHit) hitUses += std::string(hitUses.empty() ? "" : "; ") + "u6f: any hit wakes a Sleep-spell sleeper";
            if (marks) hitUses += std::string(hitUses.empty() ? "" : "; ") + "m34: every actor hit is named for Injury Engine's ping (skin marks)";
            logger::info("[KO] event sinks registered: TESEnterBleedoutEvent, TESDeathEvent, TESCellAttachDetachEvent{}{}{}",
                         !effects ? ""
                         : sleep  ? ", TESMagicEffectApplyEvent (effect log + the heal wake trigger + u6c: the Sleep spell)"
                                  : ", TESMagicEffectApplyEvent (effect log + the heal wake trigger)",
                         hits ? std::format(", TESHitEvent ({})", hitUses) : std::string(),
                         wakeTalk           ? ", TESActivateEvent (u6f: the player talking to a Sleep-spell sleeper wakes her; u7a M2 too)"
                         : g_wokenTalkOn.load() ? ", TESActivateEvent (u7a M2: woken to talk)"
                                                : "");
        } else {
            logger::error("[KO] no ScriptEventSourceHolder - no event sinks; only the check's scan can find a knockout");
        }

        StartCheckThread();
        const auto& cfg = Settings::Get();
        logger::info("[KO] check thread started: one check every {} s while a game runs and is not paused; ranks {} (down) and "
                     "{} -> 0 (coming to) are fixed (the SkyrimNet contract); U1: a protected / essential NPC in bleedout is made "
                     "UNCONSCIOUS, Health held at min({}%, her Health after {} settle checks); U2a: the first heal wakes her",
                     cfg.checkSeconds, kRankDown, kRankComingTo, std::lround(cfg.unconsciousHealthFraction * 100.0f), cfg.graceChecks);
    }

    void OnPreLoadGame()
    {
        g_gameReady.store(false);
        ++g_generation;
        g_lootPending.store(0);   // 2.1.0 looting: a menu asked for in the old world is not ours when it opens
        g_lootOpen.store(0);
        {
            std::scoped_lock lk(g_u5Mtx);   // u6b (U5): a roll still queued finds no record (its generation is old anyway)
            g_u5.clear();
            g_fdcBeam.clear();              // u7a (U4)
        }
        {
            std::scoped_lock lk(g_watchMtx);   // u7a (U7): a take-over still queued belongs to the old world (its task drops it)
            g_takeOverPending.clear();
        }
        std::scoped_lock lk(g_mtx);
        g_hasLastTick = false;
        g_woken.clear();   // u7a (M2)
    }

    void OnGameReady(bool a_postLoad)
    {
        ApplyRegenSetting(a_postLoad ? "kPostLoadGame" : "kNewGame");   // u2d: game settings are not saved - set again after every load
        std::size_t n = 0;
        {
            std::scoped_lock lk(g_mtx);
            g_hasLastTick = false;
            g_adoptAll    = a_postLoad;
            g_loadSafety  = a_postLoad;   // M1d: only at a game LOAD (a new game has nothing to do)
            n             = g_map.size();
        }
        g_gameReady.store(true);
        logger::info("[KO] {} - {} NPC(s) handled from the co-save{}", a_postLoad ? "kPostLoadGame" : "kNewGame", n,
                     a_postLoad ? "; the first unpaused check runs the load safety, then looks for v10 knockouts to adopt" : "");
    }

    // ---- U1 / U2a console tests ------------------------------------------------------------------------------------------

    std::string ConsoleU(RE::Actor* a_actor)
    {
        if (!a_actor) return "sbkou: nothing selected";
        const RE::FormID  id   = a_actor->GetFormID();
        const std::string name = NameOf(a_actor);
        enum class Act { kDrop, kMeddle, kNone } act = Act::kDrop;
        std::string why;
        {
            std::scoped_lock lk(g_mtx);
            if (auto it = g_map.find(id); it != g_map.end() && it->second.releasing) {
                act = Act::kNone;
                why = "is being let go to another mod";
            } else if (it != g_map.end()) {
                switch (it->second.state) {
                    case State::kUnconscious: act = Act::kMeddle; break;
                    case State::kEntering:    act = Act::kNone; why = "is being knocked out right now"; break;
                    case State::kDown:        act = Act::kNone; why = "is held by an old M1 knockout"; break;
                    case State::kWindow:      act = Act::kNone; why = "is a u2a leftover being finished"; break;
                    case State::kComingTo:
                        act = Act::kNone;
                        why = std::format("is coming to (stage {})", it->second.comingToRank);
                        break;
                }
            }
        }
        if (act == Act::kNone) {
            logger::info("[sbkou] {} (0x{:08X}) {} - nothing done", name, id, why);
            return std::format("sbkou: {} {} - nothing done", name, why);
        }
        if (act == Act::kMeddle) {
            if (!Helper::TestMeddle(id)) return "sbkou: SBKO_Helper script missing - nothing done";
            logger::info("[sbkou] {} (0x{:08X}) held UNCONSCIOUS -> the meddling test dispatched (+50% Health with no magic effect, "
                         "SetUnconscious false, Paralysis 0); the next check puts her back down, puts the Health back and - once "
                         "settled and 2+ checks after a load / attach - shows the 'too injured to wake up' line",
                         name, id);
            return std::format("sbkou: meddling on {} - she should stay down, too injured", name);
        }
        if (!IsProtectedOrEssential(a_actor)) {
            logger::info("[sbkou] {} (0x{:08X}) is neither protected nor essential - not dropped (she would die)", name, id);
            return std::format("sbkou: {} is not protected or essential - she would die, nothing done", name);
        }
        if (IsBleedingOut(a_actor)) return std::format("sbkou: {} is already in bleedout - nothing done", name);
        if (!Helper::TestDrop(id)) return "sbkou: SBKO_Helper script missing - nothing done";
        logger::info("[sbkou] {} (0x{:08X}) protected/essential, not held -> TestDrop dispatched (DamageActorValue Health = current + 1, "
                     "no attacker): the engine's bleedout -> SBKO's unconscious", name, id);
        return std::format("sbkou: {} dropped to 0 HP - watch her go unconscious", name);
    }

    std::string ConsoleT(RE::Actor* a_actor, const char* a_cmd)
    {
        if (!a_actor) return std::format("{}: nothing selected", a_cmd);
        const RE::FormID  id   = a_actor->GetFormID();
        const std::string name = NameOf(a_actor);
        bool              done = false;
        {
            std::scoped_lock lk(g_mtx);
            if (auto it = g_map.find(id); it != g_map.end() && it->second.state == State::kUnconscious) {
                it->second.wakeAtHours = 0.0f;   // her timer runs out now: the next unpaused check wakes her (T6b)
                done                   = true;
            }
        }
        if (!done) {
            logger::info("[{}] {} (0x{:08X}) is not held unconscious by SBKO - nothing done", a_cmd, name, id);
            return std::format("{}: {} is not held unconscious by SBKO - nothing done", a_cmd, name);
        }
        logger::info("[{}] {} (0x{:08X}) timer set to run out now - the next unpaused check wakes her (FinishWake)", a_cmd, name, id);
        return std::format("{}: {}'s timer ends now - she wakes after the console closes", a_cmd, name);
    }

    // =====================================================================================================================
    // u6h: THE CONSOLE TESTS - 'sbko <test>' (TestCmd.cpp: select an NPC in the console,
    // type a console command and the test runs). Called on the console's thread (the main thread, the game paused -
    // a console command's zero-param exec runs on the main thread) with a non-player NPC TestCmd has checked, and NO lock of ours held.
    // Each test runs the SAME code the game runs - the task function the sink relays to, fed with what the sink would have copied -
    // synchronously here (each task takes g_mtx once per scope and dispatches its Papyrus after unlocking, exactly as when it runs as a
    // task). The verdicts are the tasks' own: nothing is skipped to make a test pass. ONE console line each (under 15 words); every detail
    // goes to SimpleBleedoutKO.log.
    // =====================================================================================================================
    namespace
    {
        // the first clause of a verdict, at most a_words words - the console line stays short (the full text is in the log)
        std::string Short(std::string a_s, std::size_t a_words = 8)
        {
            for (const char* cut : { " (", " - ", "; ", " | " })
                if (const auto p = a_s.find(cut); p != std::string::npos) a_s.erase(p);
            std::string out;
            std::size_t words = 0, i = 0;
            while (i < a_s.size()) {
                const auto        sp = a_s.find(' ', i);
                const std::string w  = a_s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
                if (!w.empty()) {
                    if (words == a_words) {
                        out += " ...";
                        break;
                    }
                    out += (out.empty() ? "" : " ") + w;
                    ++words;
                }
                if (sp == std::string::npos) break;
                i = sp + 1;
            }
            return out;
        }

        const char* YesNo(bool a_b) { return a_b ? "YES" : "no"; }

        std::string FormText(const RE::TESForm* a_f)
        {
            if (!a_f) return "(none)";
            const char* n = a_f->GetName();
            return std::format("{} 0x{:08X}", n && *n ? n : "(no name)", a_f->GetFormID());
        }

        // the player's weapon in hand (the right hand first): any weapon but the hand-to-hand one (a toy counts - its rule decides)
        RE::TESObjectWEAP* PlayerWeapon(RE::PlayerCharacter* a_pc)
        {
            for (const bool left : { false, true }) {
                auto* f = a_pc ? a_pc->GetEquippedObject(left) : nullptr;
                auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
                if (w && !w->IsHandToHandMelee()) return w;
            }
            return nullptr;
        }

        // the player's mace / warhammer (U6's own keyword test), either hand
        RE::TESObjectWEAP* PlayerBlunt(RE::PlayerCharacter* a_pc, bool& a_left)
        {
            for (const bool left : { false, true }) {
                auto* f = a_pc ? a_pc->GetEquippedObject(left) : nullptr;
                auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
                if (w && !w->IsHandToHandMelee() && (w->HasKeywordID(kWeapTypeMace) || w->HasKeywordID(kWeapTypeHammer))) {
                    a_left = left;
                    return w;
                }
            }
            return nullptr;
        }

        // what the player bashes with: a shield or a torch in the left hand, else a weapon in hand (a bare hand never bashes - PLANCK)
        RE::TESForm* PlayerBashItem(RE::PlayerCharacter* a_pc)
        {
            if (auto* l = a_pc ? a_pc->GetEquippedObject(true) : nullptr; l && (l->IsArmor() || l->Is(RE::FormType::Light))) return l;
            return PlayerWeapon(a_pc);
        }

        const char* LifeName(RE::ACTOR_LIFE_STATE a_l)
        {
            switch (a_l) {
                case RE::ACTOR_LIFE_STATE::kAlive:         return "alive";
                case RE::ACTOR_LIFE_STATE::kDying:         return "dying";
                case RE::ACTOR_LIFE_STATE::kDead:          return "dead";
                case RE::ACTOR_LIFE_STATE::kUnconcious:    return "unconscious";
                case RE::ACTOR_LIFE_STATE::kReanimate:     return "reanimated";
                case RE::ACTOR_LIFE_STATE::kRecycle:       return "recycle";
                case RE::ACTOR_LIFE_STATE::kRestrained:    return "restrained";
                case RE::ACTOR_LIFE_STATE::kEssentialDown: return "essential down";
                case RE::ACTOR_LIFE_STATE::kBleedout:      return "bleedout";
            }
            return "?";
        }

        std::string RankText(RE::Actor* a_actor, RE::TESFaction* a_faction)
        {
            if (!a_faction) return "(faction not loaded)";
            const std::int32_t r = a_actor->GetFactionRank(a_faction, false);
            return r < 0 ? std::string("not a member") : std::format("rank {}", r);
        }

        const char* RollStateName(RollState a_s)
        {
            switch (a_s) {
                case RollState::kNone:    return "none";
                case RollState::kPending: return "pending";
                case RollState::kWon:     return "won (knocked out)";
                case RollState::kLost:    return "lost";
                case RollState::kSkipped: return "ruled out (or won but not knocked out)";
            }
            return "?";
        }

        const char* EntryKindName(EntryKind a_k)
        {
            switch (a_k) {
                case EntryKind::kNone:    return "none recorded";
                case EntryKind::kNotAHit: return "not a rolled hit (one roll at the bleedout)";
                case EntryKind::kNoRoll:  return "a hit that never rolls (she acts hurt)";
                case EntryKind::kRolled:  return "a hit that rolled (its roll decides)";
            }
            return "?";
        }

        // 'sbko status': every WORN armour piece and what helmet rule A says about it - one line each
        void LogWornArmour(RE::Actor* a_actor)
        {
            auto* changes = a_actor->GetInventoryChanges(true);
            if (!changes || !changes->entryList) {
                logger::info("[sbko status]   worn: (no inventory changes - nothing worn is known)");
                return;
            }
            int n = 0;
            for (const auto* entry : *changes->entryList) {
                if (!entry || !entry->object || !entry->object->IsArmor() || !entry->IsWorn()) continue;
                const auto* armo = entry->object->As<RE::TESObjectARMO>();
                if (!armo) continue;
                const std::uint32_t mask = armo->GetSlotMask().underlying();
                std::string         slots;
                for (int b = 0; b < 32; ++b)
                    if (mask & (1u << b)) slots += std::format("{}{}", slots.empty() ? "" : "+", 30 + b);
                const auto*     file    = OriginFile(armo);
                const HelmetWhy why     = HelmetRuleA(armo);
                const char*     verdict = why == HelmetWhy::kHelmet        ? "A HELMET"
                                          : why == HelmetWhy::kNotHeadSlot ? "not a helmet: not a head / hair slot"
                                          : why == HelmetWhy::kClothing    ? "not a helmet: clothing on slot 30 / 31"
                                          : why == HelmetWhy::kDevice      ? "skipped: a Devious Devices item"
                                                                           : "not a helmet: defined by a plugin on sHelmetIgnorePlugins";
                logger::info("[sbko status]   worn: {} | slots {} | {} | defined by {}{} | ArmorHelmet keyword {} | rule A: {}", ArmoName(armo),
                             slots.empty() ? std::string("none") : slots,
                             armo->IsHeavyArmor() ? "heavy armour" : armo->IsLightArmor() ? "light armour" : "clothing",
                             file ? std::string(file->GetFilename()) : std::string("?"), IsHelmetIgnoredFile(file) ? " (ON sHelmetIgnorePlugins)" : "",
                             armo->HasKeywordID(kArmorHelmet) ? "yes" : "no", verdict);
                ++n;
            }
            if (!n) logger::info("[sbko status]   worn: no armour piece");
        }
    }

    const char* ConsoleNotNpc(RE::Actor* a_actor)
    {
        return IsNPC(a_actor) ? nullptr : "is not an NPC (no ActorTypeNPC - a creature)";
    }

    std::string ConsoleHelp()
    {
        static constexpr const char* kLines[] = {
            "SimpleBleedoutKO 2.1.0 console tests. Click an NPC in the console, then type ONE of these (each prints one line in the "
            "console; the detail is in this log):",
            "  sbko status            - everything SBKO knows about her (reads only - changes nothing)",
            "  sbkos                  - the same as 'sbko status' with no word to type (the fallback if 'sbko <word>' is not understood)",
            "  sbko sleep             - PUTS HER TO SLEEP: the Sleep spell's own path, as if you cast it (seen = her detection of you now)",
            "  sbko headko            - KNOCKS HER OUT if the U6 rules allow it: your sneak BASH (shield / torch / weapon in hand) through the "
            "real head-hit path (helmet, follower, brawl ... all still apply)",
            "  sbko headko blunt      - the same with your mace / warhammer on her head (PLANCK's head node is filled in when PLANCK is loaded)",
            "  sbko headko open       - (also 'sbko headko blunt open') no sneak bit: her detection of you, or her Health, decides",
            "  sbko roll              - U5 now, as if your weapon's hit had left her at her CURRENT Health: the factors, the chance and a "
            "roll - NOT applied (dry)",
            "  sbko roll ko           - KNOCKS HER OUT when that roll is won - only when a real hit could roll (a normal NPC, her Health at "
            "or under fChanceKOHealthBelow)",
            "  sbko hitwake           - WAKES a Sleep-spell sleeper: your hit through the real sleep-wake path",
            "  sbko toy               - what the toy-weapon rule says about the weapons in YOUR hands (no NPC needed)",
            "  sbko timer             - her knock-out timer ends now: the next check wakes her (the timer-wake test; = sbkot)",
            "  sbko refill            - is the combat-end refill patch installed, and what it does for her",
            "  sbko fdc               - (U4) her Follower Death Chance state: SBKO's kind, the FDC mark, combat, FDC's effect (reads only)",
            "  sbko fdchit            - (U4) a simulated qualifying hit on a follower SBKO holds after a bleedout knock-out -> FDC's re-roll "
            "is ASKED (console 'set PlayerChosenDeathChance to 100' makes the roll certain - put 30 back after)",
            "  sbko takeover [secs]   - (U7) CHOKES HER OUT: VRTE's TakeOver path (source console.choke, by you, held secs - default 15)",
            "  sbko holds             - (U7) what SBKO_API.Holds answers for her",
            "  sbko release [secs]    - (U7) the choke ends after secs (default 27): SBKO_API.ChokeReleased's path (her total seconds)",
            "  sbko woken             - (M2) SkyrimNet is told you just woke her to talk (NarrateWoken kind 1) - never on an unconscious NPC",
            "  sbko sleepstate        - (M2) her sit-sleep state and what the 'woken to talk' rules say (reads only)",
            "  sbko help              - this list",
            "  sbko                   - (no word) the old neck / wrist / ankle band test (SBKO_Test.Run)",
            "  sbkot                  - = sbko timer",
            "  sbkou  !! DANGEROUS !! - on a protected / essential NPC SBKO does NOT hold it DROPS HER TO 0 HEALTH (a real bleedout, then "
            "SBKO's knock-out); on one SBKO holds it is the 'meddling' test (she should stay down). NEVER use it as a status check",
            "Never on you (the player); a creature is refused. Every test runs the code the game runs - its result lines are the usual "
            "[sleep] / [headhit] / [chanceko] / [KO] lines, marked [console test ...].",
        };
        for (const char* l : kLines) logger::info("[sbko help] {}", l);
        return "sbko: status sleep headko roll hitwake toy timer refill fdc fdchit takeover holds release woken sleepstate - see log";
    }

    std::string ConsoleStatus(RE::Actor* a)
    {
        const auto&       cfg    = Settings::Get();
        const RE::FormID  id     = a->GetFormID();
        const std::string name   = NameOf(a);
        const std::string who    = Who(a, id);
        auto*             player = RE::PlayerCharacter::GetSingleton();

        // ---- the engine's view of her (reads only, on the console's thread)
        float         cur = 0.0f, mx = 0.0f;
        const float   pct    = HealthFractionMax(a, &cur, &mx);
        auto*         avo    = a->AsActorValueOwner();
        const float   para   = avo ? avo->GetActorValue(RE::ActorValue::kParalysis) : 0.0f;
        const float   hr     = avo ? avo->GetActorValue(RE::ActorValue::kHealRate) : 0.0f;
        const float   hrm    = avo ? avo->GetActorValue(RE::ActorValue::kHealRateMult) : 0.0f;
        const auto*   st     = a->AsActorState();
        const auto    life   = st ? st->GetLifeState() : RE::ACTOR_LIFE_STATE::kAlive;
        const bool    dead   = sbko::IsDeadNow(a);
        const bool    has3D  = a->Get3D() != nullptr;
        TakenKind     tk     = TakenKind::kNone;
        const char*   taken  = TakenBy(a, tk);
        const char*   fight  = FistFightRunning();
        const char*   cr     = ChildOrRider(a);
        const bool    normal = IsNPC(a) && !IsProtectedOrEssential(a) && !IsGhost(a);
        std::string   helmetWhat, helmetNot;
        const bool    helmet = WornHelmet(a, helmetWhat, helmetNot);
        const Factors f      = player ? ReadFactors(a, player, 0.0f, mx) : Factors{};
        // her detection of you NOW (= Papyrus IsDetectedBy: > 0 detected). It gets-or-creates her detection record - fine on this thread.
        const bool         detRead = player && !dead && has3D;
        const std::int32_t det     = detRead ? a->RequestDetectionLevel(player, RE::DETECTION_PRIORITY::kNormal) : kNoDetectRecord;
        const float        hours   = HoursNow();

        // ---- SBKO's own lists (g_mtx), then her U5 record (g_u5Mtx) - each alone, copies only
        std::optional<Entry>    entry;
        std::optional<Pending>  pending;
        std::optional<Released> released;
        bool                    declined = false, candidate = false;
        std::string             heldWhy;
        {
            std::scoped_lock lk(g_mtx);
            if (auto it = g_map.find(id); it != g_map.end()) entry = it->second;
            if (auto it = g_pendingConscious.find(id); it != g_pendingConscious.end()) pending = it->second;
            if (auto it = g_released.find(id); it != g_released.end()) released = it->second;
            declined  = g_declined.contains(id);
            candidate = g_adoptCandidates.contains(id);
            heldWhy   = HeldWhy(id);
        }
        const bool              watched = Watched(id);
        const bool              asleep  = Asleep(id);
        std::optional<U5Victim> u5;
        {
            std::scoped_lock lk(g_u5Mtx);
            if (auto it = g_u5.find(id); it != g_u5.end()) u5 = it->second;
        }

        // ---- the block
        logger::info("[sbko status] ===== {} | base {} | race {} | level {} (you: {}) =====", who, FormText(a->GetActorBase()),
                     FormText(a->GetRace()), a->GetLevel(), player ? player->GetLevel() : 0);
        logger::info("[sbko status] body: HP {} ({:.1f} of {:.1f}; max = permanent + temporary) | life state {} ({}) | dead {} | bleeding out {} "
                     "| unconscious {} | Paralysis {:.2f} | HealRate {:.4f} x HealRateMult {:.2f} | ragdoll {} | knock state {} | 3D {} | cell "
                     "attached {} | disabled {}",
                     Pct1(pct), cur, mx, LifeName(life), static_cast<int>(life), YesNo(dead), YesNo(IsBleedingOut(a)),
                     YesNo(st && st->IsUnconscious()), para, hr, hrm, YesNo(a->IsInRagdollState()),
                     st ? static_cast<int>(st->GetKnockState()) : -1, YesNo(has3D), YesNo(InAttachedCell(a)), YesNo(a->IsDisabled()));
        logger::info("[sbko status] who: ActorTypeNPC {} | ghost {} | summoned / commanded {} | child / rider {} | follower {} (teammate {}, "
                     "CurrentFollowerFaction {}) | {} | U5 normal NPC {} | brawl (DGIntimidateFaction) {} | fist-fight {}",
                     YesNo(IsNPC(a)), YesNo(IsGhost(a)), YesNo(a->IsCommandedActor()), cr ? cr : "no", YesNo(IsFollower(a)),
                     YesNo(a->IsPlayerTeammate()), RankText(a, g_followerFaction), EssFlags(a),
                     !g_u5On.load() ? "- (U5 is OFF)" : normal ? "YES (a hit that leaves her at or under the line rolls)"
                                                               : "no (essential / protected / not an NPC / a ghost: never rolls)",
                     RankText(a, g_brawlFaction), fight ? fight : "none running");
        logger::info("[sbko status] marks: SBKO_StateFaction {} (10 knocked out, 3-1 coming to) | SBKO_SleepFaction {} (2 asleep + saw you, 1 "
                     "asleep + did not, 3 choked) | FDC_SBKO_RolledFaction {} (1 = FDC rolled once - a hit re-rolls; 2 = a hit / sleep / choke "
                     "knock-out, never) | VRTE_ChokeStateFaction {} (5 = VRTE holds her) | Smart NPC Potions abilities on her now: mask {}",
                     RankText(a, g_faction), RankText(a, g_sleepFaction), RankText(a, g_fdcRolledFaction), RankText(a, g_vrteFaction),
                     PotionSpellsOn(a));
        logger::info("[sbko status] taken by another mod: {} | Diary of Mine: DOMBeingCaptured {}, DOMActorFaction (slave) {}, DOMActionTied {}, "
                     "DOMActionInContainer (bag) {}",
                     taken ? taken : "no", RankText(a, g_domFaction), RankText(a, g_domSlaveFaction), RankText(a, g_domTiedFaction),
                     RankText(a, g_domBagFaction));
        if (entry) {
            const Entry& e       = *entry;
            const bool   timerOn = e.state == State::kUnconscious || e.state == State::kDown;
            logger::info("[sbko status] SBKO HOLDS HER: state {}{} | kind {} | sleep mark {} | knocked out at game hour {:.2f} ({:.1f} h ago) | "
                         "timer {} | saved HealRate {} | hold {} | settle checks {} ({}) | coming-to rank {} ({:.0f} s on it) | potions taken: "
                         "mask {} | SeverActions excluded {} | heal wake waiting {}{} | sleep wake waiting {}{} | wake running {} | HoldDown / "
                         "HoldHealthTo in flight {} / {} | re-adopted {} | orphan {} | quiet re-assert rule {} (check {} since {})",
                         StateName(e.state), e.releasing ? " (BEING LET GO to another mod)" : "",
                         !e.unc            ? "an old M1 knock-out"
                         : e.sleepMark > 0 ? "the Sleep spell"
                         : e.cls == kClsChoke ? "a CHOKE knock-out (VRTE's take-over: held at her Health; a real heal or the timer wakes her)"
                         : e.hitKO         ? "a HIT knock-out (held at her Health then)"
                                           : "a bleedout knock-out (the fUnconsciousHealthFraction cap)",
                         e.sleepMark == kSleepSeen ? "2 (she saw you)" : e.sleepMark == kSleepUnseen ? "1 (she did not see you)" : "0",
                         e.koHours, hours - e.koHours,
                         timerOn ? std::format("wakes at hour {:.2f} = in {:.1f} game h (rolled {:.1f} h)", e.wakeAtHours, e.wakeAtHours - hours,
                                               e.recoverHours)
                                 : std::string("- (not running in this state)"),
                         e.savedHealRate >= 0.0f ? std::format("{:.4f}", e.savedHealRate) : std::string("her base value (-2.0)"),
                         e.lockFrac >= 0.0f ? Pct1(e.lockFrac) : std::string("not set yet"), e.downChecks, e.settledOnce ? "settled" : "settling",
                         e.comingToRank, e.stageSeconds, e.potionMask, YesNo(e.severExcluded), YesNo(e.healPending),
                         e.healPending ? " (" + e.healTrigger + ")" : std::string(), YesNo(e.sleepWakePending),
                         e.sleepWakePending ? " (" + e.sleepWakeTrigger + ")" : std::string(), YesNo(e.awaitingWake), YesNo(e.holdInFlight),
                         YesNo(e.healthInFlight), YesNo(e.readopted), YesNo(e.orphan), e.resetActive ? "armed" : "off", e.checksSince, e.since);
            if (e.hitKO)
                logger::info("[sbko status] hit knock-out: her Health at the knock-out {}",
                             e.hitPct >= 0.0f ? Pct1(e.hitPct) : std::string("not known (loaded from a save)"));
        } else {
            logger::info("[sbko status] SBKO record: NONE - not held, not coming to");
        }
        logger::info("[sbko status] SBKO lists: load-safety list {}{} | released watch {}{} | declined {} | adoption candidate {} | held set {} | "
                     "sleeper set {} | a NEW hit / sleep knock-out now: {}",
                     YesNo(pending.has_value()),
                     pending ? (pending->belt ? " (a second-run belt)" : " (made conscious when next seen loaded)") : "",
                     YesNo(released.has_value()),
                     released ? std::format(" (hit KO {}, sleep mark {}, VRTE deferred {})", YesNo(released->hitKO), released->sleepMark,
                                            YesNo(released->deferred))
                              : std::string(),
                     YesNo(declined), YesNo(candidate), YesNo(watched), YesNo(asleep),
                     heldWhy.empty() ? std::string("allowed by SBKO's lists") : heldWhy);
        if (u5)
            logger::info("[sbko status] U5 record: newest roll {} (chance {:.1f}%, rolled {:.1f}) | bleedouts recorded {} | the last one: {} ({}) | "
                         "decided {}",
                         RollStateName(u5->rollState), u5->rollChance, u5->rollValue, u5->bleedSerial, EntryKindName(u5->entry),
                         u5->entryWhy ? u5->entryWhy : "", !u5->decided ? "no" : u5->decidedKO ? "KNOCK OUT" : "acting hurt");
        else
            logger::info("[sbko status] U5 record: none (no hit at or under the line and no bleedout recorded for her since the load)");
        {   // u7a: U4 / U7 / M2
            std::optional<ChokeLoose> loose;
            bool                      wokenPending = false;
            {
                std::scoped_lock lk(g_mtx);
                if (auto it = g_chokeLoose.find(id); it != g_chokeLoose.end()) loose = it->second;
                wokenPending = g_woken.contains(id);
            }
            const auto* stz = a->AsActorState();
            const auto  ss  = stz ? static_cast<std::uint32_t>(stz->GetSitSleepState()) : 0u;
            logger::info("[sbko status] u7a: U4 FDC re-roll {} (on its watch {}) | U7 choke {}{} | take-over pending {} | SBKO_API.Holds {} | M2 sit-sleep "
                         "state {} ({}), 'woken to talk' pending {}",
                         g_fdcOn.load() ? "ON" : "OFF", YesNo(FdcWatched(id)),
                         entry && entry->cls == kClsChoke ? std::format("knock-out held {:.1f} s (recorded at hour {:.2f})", entry->chokeSeconds, entry->chokeHours)
                         : released && released->choke  ? std::format("knock-out (released) held {:.1f} s", released->chokeSeconds)
                                                        : std::string("knock-out: none"),
                         loose ? std::format(" | a loose choke (no take-over) held {:.1f} s at hour {:.2f}", loose->seconds, loose->hours) : std::string(),
                         YesNo(TakeOverPending(id)), YesNo(ApiHolds(a)), ss, SitSleepName(ss), YesNo(wokenPending));
        }
        std::string ignore;
        for (const auto& n : cfg.helmetIgnorePlugins) ignore += (ignore.empty() ? "" : ", ") + n;
        logger::info("[sbko status] helmet (rule A: a worn Light / Heavy armour on slot 30 or 31): {} | sHelmetIgnorePlugins: {}",
                     HelmetLogText(helmet, helmetWhat, helmetNot, "YES:", "no"), ignore.empty() ? std::string("(empty)") : ignore);
        LogWornArmour(a);
        logger::info("[sbko status] U5 formula if YOU hit her (the damage term counts 0 here): {} | U5 {} | a hit rolls only when it leaves her at "
                     "or under {:.0f}% (now {})",
                     player ? f.text : std::string("(no player)"), g_u5On.load() ? "ON" : "OFF", cfg.chanceKOHealthBelow * 100.0f, Pct1(pct));
        logger::info("[sbko status] her detection of you now: {} - the U6 sneak trigger (not detected = 0 or less) and the Sleep spell's seen / "
                     "not seen",
                     !detRead                 ? std::string("not read (dead / no 3D)")
                     : det == kNoDetectRecord ? std::format("{} (no detection record = she does not see you)", det)
                                              : std::format("{} ({})", det, det > 0 ? "she DETECTS you" : "she does not detect you"));
        logger::info("[sbko status] ===== end of {} =====", who);

        const std::string state = !entry                             ? std::string(pending    ? "on the load-safety list"
                                                                                   : released ? "on the released watch"
                                                                                              : "not held")
                                  : entry->releasing                 ? std::string("being let go")
                                  : entry->state == State::kComingTo ? std::format("COMING TO stage {}", entry->comingToRank)
                                                                     : std::format("{}{}", StateName(entry->state),
                                                                                   entry->sleepMark == kSleepSeen     ? " (asleep, saw you)"
                                                                                   : entry->sleepMark == kSleepUnseen ? " (asleep, unseen)"
                                                                                   : entry->cls == kClsChoke          ? " (choked)"
                                                                                   : entry->hitKO                     ? " (hit KO)"
                                                                                                                      : "");
        return std::format("sbko status: {} - {}, HP {}%, helmet {} - see log", name, state, std::lround(pct * 100.0f), helmet ? "YES" : "no");
    }

    std::string ConsoleSleep(RE::Actor* a)
    {
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        if (!g_sleepOn.load() || g_sleepMgefs.empty()) {
            logger::info("[sbko sleep] {} - the Sleep spell is OFF ({}) - nothing done", Who(a, id),
                         g_sleepMgefs.empty() ? "SimpleBleedoutKO_001.esp has no Sleep records" : "bSleep 0");
            return "sbko sleep: the Sleep spell is OFF - nothing done";
        }
        const RE::FormID mgef = g_sleepEffect ? g_sleepEffect->GetFormID() : g_sleepMgefs.front();
        logger::info("[sbko sleep] {} - the Sleep spell's real path (OnSleepTask, effect 0x{:08X}) as if you cast it on her now", Who(a, id), mgef);
        const TaskResult r = OnSleepTask(id, mgef, g_generation.load(), "sbko sleep");
        return std::format("sbko sleep: {} {}", name, r.acted ? r.what : Short(r.what));
    }

    std::string ConsoleHeadKO(RE::Actor* a, bool a_blunt, bool a_open)
    {
        const auto&       cfg  = Settings::Get();
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        const char*       cmd  = a_blunt ? (a_open ? "sbko headko blunt open" : "sbko headko blunt") : (a_open ? "sbko headko open" : "sbko headko");
        if (!cfg.headHit) {
            logger::info("[sbko headko] {} - U6 is OFF (bHeadHit 0): no hit of yours knocks out - nothing done", Who(a, id));
            return std::format("{}: U6 is OFF (bHeadHit 0) - nothing done", cmd);
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return std::format("{}: no player", cmd);
        HitCopy h;
        h.test   = cmd;
        h.target = id;
        h.gen    = g_generation.load();
        std::string with;
        if (a_blunt) {
            bool  left = false;
            auto* w    = PlayerBlunt(pc, left);
            if (!w) {
                logger::info("[sbko headko] {} - no mace / warhammer in your hands - nothing done", Who(a, id));
                return std::format("{}: equip a mace or warhammer first - nothing done", cmd);
            }
            h.source = w->GetFormID();
            if (g_planckPresent.load()) {   // what PLANCK's extended hit event carries for a hit on her head
                h.planck = true;
                h.head   = true;
                h.headUp = 0;
                h.node   = kHeadNode;
                h.isLeft = left;
            }
            with = std::format("hit with your {} on her head ({})", FormText(w),
                               g_planckPresent.load() ? "PLANCK's head node filled in - PLANCK is loaded" : "no PLANCK: a blunt weapon counts anywhere");
        } else {
            auto* item = PlayerBashItem(pc);
            if (!item) {
                logger::info("[sbko headko] {} - nothing to bash with (a bare hand never bashes) - nothing done", Who(a, id));
                return std::format("{}: equip a shield, torch or weapon first - nothing done", cmd);
            }
            h.source = item->GetFormID();
            h.flags |= kHitBash;
            with = std::format("bash with your {}", FormText(item));
        }
        if (!a_open) h.flags |= kHitSneak;
        logger::info("[sbko headko] {} - a simulated {} {} -> OnHitTask (the real U6 rules: helmet, follower, brawl, held ... and the trigger)",
                     Who(a, id), a_open ? "OPEN (no sneak bit)" : "SNEAK", with);
        const TaskResult r = OnHitTask(h);
        return std::format("{}: {} {}", cmd, name, Short(r.what, 7));   // up to 4 words of command + a name + 7 = under 15
    }

    std::string ConsoleRoll(RE::Actor* a, bool a_apply)
    {
        const auto&       cfg  = Settings::Get();
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        const char*       cmd  = a_apply ? "sbko roll ko" : "sbko roll";
        if (!g_u5On.load()) {
            logger::info("[sbko roll] {} - U5 is OFF (bChanceKO 0, or its damage hook was not installed) - nothing to roll", Who(a, id));
            return std::format("{}: U5 is OFF - nothing to roll", cmd);
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return std::format("{}: no player", cmd);
        // the hit: your weapon in hand, as the hit event your swing sends (her, you, the weapon); PhysicalKind is the hook's own
        // classification (a punch / a toy never roll; a bow = an arrow)
        auto*       w   = PlayerWeapon(pc);
        HitTls      hit{ .target = id, .cause = pc->GetFormID(), .source = w ? w->GetFormID() : 0 };
        HitKind     kind   = HitKind::kWeapon;
        RE::FormID  source = 0;
        const char* noRoll = PhysicalKind(hit, pc, kind, source);
        float       cur = 0.0f, mx = 0.0f;
        const float pct = HealthFractionMax(a, &cur, &mx);
        // the hook's own tests on her: her Health after "the hit" (= now) against the line, then its who-filter (U5VictimOut)
        const char* hookOut = (cur <= 0.0f || mx <= 0.0f)          ? "her Health is at or below 0 (death is the game's)"
                              : cur > cfg.chanceKOHealthBelow * mx ? "her Health is above fChanceKOHealthBelow (no hit leaving her there rolls)"
                                                                   : U5VictimOut(a, id);
        logger::info("[sbko roll] {} - your {} ({}) as if its hit had left her at her CURRENT Health {} ({:.1f} of {:.1f}); the hook's own tests: "
                     "{}{} -> {}",
                     Who(a, id), noRoll ? "hit" : HitKindName(kind), w ? FormText(w) : std::string("bare hands"), Pct1(pct), cur, mx,
                     noRoll    ? std::format("NO ROLL - {}", noRoll)
                     : hookOut ? std::format("a real hit would NOT roll - {}", hookOut)
                               : std::string("passed"),
                     a_apply ? "" : " (dry: a won roll is NOT applied)",
                     noRoll || (a_apply && hookOut) ? "nothing more" : "OnChanceKOTask (the real roll task)");
        if (noRoll) return std::format("{}: {} - no roll: {}", cmd, name, Short(noRoll, 6));
        if (a_apply && hookOut) return std::format("{}: {} - no roll: {}", cmd, name, Short(hookOut, 6));
        std::uint32_t serial = g_u5RollSerial.fetch_add(1, std::memory_order_relaxed) + 1;
        if (serial == 0) serial = g_u5RollSerial.fetch_add(1, std::memory_order_relaxed) + 1;   // 0 = "none"
        if (a_apply) {   // what the hook writes before it relays a roll (the task's FinishRoll then records the result)
            std::scoped_lock lk(g_u5Mtx);
            U5Victim& v    = g_u5[id];
            v.rollSerial   = serial;
            v.rollAttacker = pc->GetFormID();
            v.rollState    = RollState::kPending;
            v.rollChance   = -1.0f;
            v.rollValue    = -1.0f;
        }
        const RollCopy   rc{ .victim = id, .attacker = pc->GetFormID(), .source = source, .serial = serial, .gen = g_generation.load(),
                             .kind = kind, .damage = 0.0f, .cur = cur, .max = mx, .console = static_cast<std::uint8_t>(a_apply ? 2 : 1) };
        const TaskResult r = OnChanceKOTask(rc);
        if (r.chance < 0.0f) return std::format("{}: {} - {}", cmd, name, Short(r.what, 7));
        const std::string dice = std::format("chance {:.0f}%, rolled {:.0f}", r.chance, r.roll);
        if (a_apply)   // u6h fix 1: a won roll that knocked nobody out says why (SBKO's lists)
            return std::format("{}: {} - {}: {}", cmd, name, dice,
                               r.acted ? std::string("KNOCKED OUT")
                               : r.won ? "won but " + (r.wonBut.empty() ? std::string("not knocked out") : Short(r.wonBut, 4))
                                       : std::string("lost, stays up"));
        return std::format("{}: {} - {}: {} (dry{})", cmd, name, dice, r.won ? "WON" : "lost", hookOut ? "; no real hit rolls" : ", not applied");
    }

    std::string ConsoleHitWake(RE::Actor* a)
    {
        const auto&       cfg  = Settings::Get();
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        if (!g_sleepOn.load() || !cfg.sleepWakeOnHit) {
            logger::info("[sbko hitwake] {} - a hit wakes no sleeper ({}) - nothing done", Who(a, id),
                         !g_sleepOn.load() ? "the Sleep spell is OFF" : "bSleepWakeOnHit 0");
            return "sbko hitwake: a hit wakes no sleeper (Sleep off / bSleepWakeOnHit 0)";
        }
        if (!Asleep(id)) {   // the hit sink's own filter
            logger::info("[sbko hitwake] {} - not one of SBKO's Sleep-spell sleepers - nothing done", Who(a, id));
            return std::format("sbko hitwake: {} is not a Sleep-spell sleeper - nothing done", name);
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return "sbko hitwake: no player";
        auto* w = PlayerWeapon(pc);
        logger::info("[sbko hitwake] {} - a simulated hit from you ({}) -> OnSleepWakeTask (the real sleep-wake path)", Who(a, id),
                     w ? FormText(w) : std::string("no weapon form"));
        const TaskResult r = OnSleepWakeTask(id, pc->GetFormID(), w ? w->GetFormID() : 0, 0, 0, SleepWakeBy::kHit, g_generation.load(),
                                             "sbko hitwake");
        return std::format("sbko hitwake: {} {}", name, Short(r.what));
    }

    std::string ConsoleToy()
    {
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return "sbko toy: no player";
        std::string hud[2];
        for (const bool left : { false, true }) {
            const char*  hand = left ? "left" : "right";
            auto*        f    = pc->GetEquippedObject(left);
            auto*        w    = f ? f->As<RE::TESObjectWEAP>() : nullptr;
            std::string& out  = hud[left ? 1 : 0];
            if (!f) {
                logger::info("[sbko toy] {} hand: empty", hand);
                out = "empty";
                continue;
            }
            if (!w) {
                const bool shield = f->IsArmor(), torch = f->Is(RE::FormType::Light), spell = f->IsMagicItem();
                logger::info("[sbko toy] {} hand: {} - {}", hand, FormText(f),
                             shield  ? "a shield: not a weapon; its BASH counts (U6: a bash anywhere; U5: a bash rolls)"
                             : torch ? "a torch: not a weapon; its BASH counts (U6: a bash anywhere; U5: a bash rolls)"
                             : spell ? "a spell: U5 - its impact rolls (a concentration beam once per cast); U6 - never"
                                     : "not a weapon");
                out = shield ? "shield" : torch ? "torch" : spell ? "spell" : "other";
                continue;
            }
            using F2          = RE::TESObjectWEAP::Data::Flag2;
            const bool  h2h   = w->IsHandToHandMelee();
            const char* toy   = ToyWeapon(w);
            const bool  blunt = !h2h && (w->HasKeywordID(kWeapTypeMace) || w->HasKeywordID(kWeapTypeHammer));
            // U5's classification of a hit event naming this weapon (PhysicalKind - the hook's own; target only has to be non-zero)
            const HitTls ev{ .target = 1, .cause = pc->GetFormID(), .source = w->GetFormID() };
            HitKind      kind = HitKind::kWeapon;
            RE::FormID   src  = 0;
            const char*  u5   = PhysicalKind(ev, pc, kind, src);
            logger::info("[sbko toy] {} hand: {} | NonHostile {} | NotUsedInNormalCombat {} | base damage {} | hand-to-hand {} | the toy rule: {} | "
                         "U5 (a hit with it): {} | U6: {}",
                         hand, FormText(w), YesNo(w->weaponData.flags2.any(F2::kNonHostile)),
                         YesNo(w->weaponData.flags2.any(F2::kNotUsedInNormalCombat)), w->attackDamage, YesNo(h2h),
                         toy   ? std::format("a TOY - {}", toy)
                         : h2h ? std::string("not judged (bare hands keep their own rule)")
                               : std::string("an ordinary weapon"),
                         u5 ? std::format("no roll - {}", u5) : std::format("rolls ({})", HitKindName(kind)),
                         toy     ? std::format("never knocks out ({}) - its bash neither", toy)
                         : h2h   ? std::string("a punch never knocks out")
                         : blunt ? std::string(g_planckPresent.load()
                                                   ? "a blunt weapon: a sneak / undetected / low-Health hit on the HEAD knocks out (PLANCK)"
                                                   : "a blunt weapon: a sneak / undetected / low-Health hit ANYWHERE knocks out (no PLANCK)")
                                 : std::string("not blunt: only a bash with it can knock out"));
            out = toy ? std::format("TOY ({})", toy == kWhyNonHostile ? "non-hostile" : toy == kWhyNotCombat ? "not combat" : "no damage")
                  : h2h ? std::string("bare hand")
                        : std::string("ok");
        }
        HitKind     kind  = HitKind::kWeapon;
        RE::FormID  src   = 0;
        const char* hands = PhysicalKind(HitTls{}, pc, kind, src);
        logger::info("[sbko toy] the hands check (a weapon hit with NO hit event on its thread - your hands decide): {}",
                     hands ? std::format("no roll - {}", hands) : std::format("rolls ({})", HitKindName(kind)));
        return std::format("sbko toy: right {}, left {} - see log", hud[0], hud[1]);
    }

    std::string ConsoleRefill(RE::Actor* a)
    {
        const auto&       cfg  = Settings::Get();
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        const std::string who  = Who(a, id);
        const auto        inst = RefillGuard::GetInstallState();
        const bool        ess  = IsProtectedOrEssential(a);
        const char*       skip = RefillGuard::SkipReason(a);   // the combat-end thunk's own decision
        const bool        held = Watched(id);
        const float       pct  = HealthFractionMax(a);
        logger::info("[sbko refill] {} | combat-end patch (Actor::StopCombat's full heal) {} | wake patch (NoBleedoutRecovery off's full heal) {}{} | "
                     "iCombatEndRefill {} | {} | an SBKO call running on her now {} | SBKO holds her {}",
                     who, inst.stopPatch ? "INSTALLED" : "NOT installed", inst.flagPatch ? "INSTALLED" : "NOT installed",
                     *inst.why ? std::format(" ({})", inst.why) : std::string(), cfg.combatEndRefill, EssFlags(a),
                     YesNo(RefillGuard::CallRunning(id)), YesNo(held));
        std::string verdict, hud;
        if (!inst.stopPatch) {
            verdict = std::format("the combat-end patch is NOT installed ({}) - the engine refills her as usual (if she is essential / protected)",
                                  *inst.why ? inst.why : "?");
            hud     = "patch NOT installed - the engine refills as usual";
        } else if (!ess) {
            verdict = "she is NOT essential / protected - the engine's combat-end full heal never applies to her (nothing to skip): she keeps "
                      "her Health";
            hud     = std::format("{} is not essential / protected - never refilled", name);
        } else if (skip) {
            verdict = std::format("SKIPPED for her ({}) - when she leaves combat she stays at {}", skip, Pct1(pct));
            hud     = std::format("patch ON - {} stays hurt at combat end", name);
        } else {
            verdict = std::format("NOT skipped (iCombatEndRefill {}: {}) - the engine refills her to FULL when she leaves combat", cfg.combatEndRefill,
                                  cfg.combatEndRefill == 1 ? "followers only - she is not one" : "the engine as usual");
            hud     = std::format("patch ON - {} refilled at combat end (mode {})", name, cfg.combatEndRefill);
        }
        if (held)
            verdict += " | while SBKO holds her NoBleedoutRecovery is ON, so the engine's combat-end refill does not fire anyway; at her wake the "
                       "wake patch skips the flag-off refill (she wakes at fWakeHealthFraction, never full)";
        logger::info("[sbko refill] {} -> {}", who, verdict);
        return "sbko refill: " + hud;
    }

    // ---- u7a: the SBKO_API natives' work (Api.cpp) -----------------------------------------------------------------------------
    bool ApiTakeOver(RE::Actor* a_target, RE::Actor* a_aggressor, std::string_view a_source, float a_seconds)
    {
        const TakeOverVerdict v = TakeOverDecide(a_target, a_aggressor, a_source, a_seconds, nullptr);
        if (v == TakeOverVerdict::kDeclined) return false;
        if (v == TakeOverVerdict::kAccepted || v == TakeOverVerdict::kAlready) {   // kAlready: the task updates a choke's seconds
            const TakeOverCopy t{ .target    = a_target->GetFormID(),
                                  .aggressor = a_aggressor ? a_aggressor->GetFormID() : 0,
                                  .source    = std::string(a_source),
                                  .seconds   = a_seconds,
                                  .gen       = g_generation.load() };
            TaskRelay::Add([t] { OnTakeOverTask(t); });
        }
        return true;
    }

    bool ApiHolds(RE::Actor* a_target)
    {
        if (!a_target) return false;
        const RE::FormID id = a_target->GetFormID();
        return Watched(id) || TakeOverPending(id);
    }

    void ApiChokeReleased(RE::Actor* a_victim, float a_seconds, const char* a_from)
    {
        if (!a_victim || a_victim->IsPlayerRef() || !g_gameReady.load()) {
            if (Settings::Get().logTakeOver)
                logger::info("[takeover] ChokeReleased({}, {:.1f} s) from {} ignored ({})", a_victim ? Who(a_victim, a_victim->GetFormID()) : std::string("None"),
                             a_seconds, a_from, !a_victim ? "no actor" : a_victim->IsPlayerRef() ? "the player" : "no game is ready");
            return;
        }
        const RE::FormID    id  = a_victim->GetFormID();
        const std::uint32_t gen = g_generation.load();
        TaskRelay::Add([id, a_seconds, a_from, gen] { OnChokeReleasedTask(id, a_seconds, a_from, gen); });   // a_from = a string literal
    }

    // ---- u7a console tests --------------------------------------------------------------------------------------------------------
    // m34: the one heal rule, for Marks.cpp (a pure read)
    bool IsHealEffect(const RE::EffectSetting* a_m) { return IsHeal(a_m); }

    std::string ConsoleFdc(RE::Actor* a)
    {
        const RE::FormID  id     = a->GetFormID();
        const std::string name   = NameOf(a);
        auto*             player = RE::PlayerCharacter::GetSingleton();
        const std::int32_t rank  = g_faction ? a->GetFactionRank(g_faction, false) : -1;
        const std::int32_t mark  = g_fdcRolledFaction ? a->GetFactionRank(g_fdcRolledFaction, false) : -1;
        const bool         combat = player && player->IsInCombat();
        auto*              mt     = a->AsMagicTarget();
        const int          effect = !g_fdcEffect ? -1 : (mt && mt->HasMagicEffect(g_fdcEffect)) ? 1 : 0;
        std::string        kind   = "not held by SBKO";
        {
            std::scoped_lock lk(g_mtx);
            if (const auto it = g_map.find(id); it != g_map.end()) {
                const Entry& e = it->second;
                kind = std::format("{} ({})", StateName(e.state),
                                   e.cls == kClsChoke ? "choke KO" : e.sleepMark > 0 ? "sleep KO" : e.hitKO ? "hit KO" : e.unc ? "bleedout KO" : "old M1 KO");
            }
        }
        logger::info("[sbko fdc] {}: SBKO {} | SBKO_StateFaction rank {} | FDC_SBKO_RolledFaction rank {} | you are {}in combat | FDC's effect on her: {} | "
                     "a follower {} | on the U4 watch {} | U4 {} (bFdcRerollPlayerHits {}, bFdcRerollDeathOnly {})",
                     Who(a, id), kind, rank, mark, combat ? "" : "NOT ", effect < 0 ? "unknown (FDC's effect not found)" : effect ? "active" : "off",
                     YesNo(IsFollower(a)), YesNo(FdcWatched(id)), g_fdcOn.load() ? "ON" : "OFF", Settings::Get().fdcRerollPlayerHits ? 1 : 0,
                     Settings::Get().fdcRerollDeathOnly ? 1 : 0);
        return std::format("sbko fdc: {} - {}, mark {}, combat {}, FDC effect {}", name, kind, mark, combat ? "yes" : "no",
                           effect < 0 ? "?" : effect ? "on" : "off");
    }

    std::string ConsoleFdcHit(RE::Actor* a)
    {
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        if (!g_fdcOn.load()) {
            logger::info("[sbko fdchit] {} - U4 is OFF (bFdcRerollOnHit 0, FDC_SBKO_Patch.esp not loaded, or no damage hook) - nothing done", Who(a, id));
            return "sbko fdchit: U4 is OFF - nothing done";
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        auto* w  = PlayerWeapon(pc);
        logger::info("[sbko fdchit] {} - a simulated qualifying hit (your {}) -> OnFdcHitTask (the real FDC re-roll checks)", Who(a, id),
                     w ? FormText(w) : std::string("no weapon form"));
        const FdcHitCopy c{ .test = "sbko fdchit", .victim = id, .attacker = pc ? pc->GetFormID() : 0, .source = w ? w->GetFormID() : 0,
                            .kind = HitKind::kWeapon, .damage = 0.0f, .gen = g_generation.load() };
        const TaskResult r = OnFdcHitTask(c);
        return std::format("sbko fdchit: {} {}", name, Short(r.what, 8));
    }

    std::string ConsoleTakeOver(RE::Actor* a, float a_seconds)
    {
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        auto*             pc   = RE::PlayerCharacter::GetSingleton();
        std::string       why;
        const TakeOverVerdict v = TakeOverDecide(a, pc, "console.choke", a_seconds, "sbko takeover", &why);
        if (v == TakeOverVerdict::kDeclined) return std::format("sbko takeover: {} declined - {}", name, Short(why, 7));
        if (v == TakeOverVerdict::kPending) return std::format("sbko takeover: {} - a take-over is already landing", name);
        const TakeOverCopy t{ .test = "sbko takeover", .target = id, .aggressor = pc ? pc->GetFormID() : 0, .source = "console.choke",
                              .seconds = a_seconds, .gen = g_generation.load() };
        const TaskResult r = OnTakeOverTask(t);
        return std::format("sbko takeover: {} {}", name, Short(r.what, 8));
    }

    std::string ConsoleHolds(RE::Actor* a)
    {
        const RE::FormID  id   = a->GetFormID();
        const bool        h    = ApiHolds(a);
        std::string       kind = "-";
        {
            std::scoped_lock lk(g_mtx);
            if (const auto it = g_map.find(id); it != g_map.end())
                kind = std::format("{}{}", StateName(it->second.state), it->second.cls == kClsChoke ? ", choked" : "");
        }
        logger::info("[sbko holds] {} - SBKO_API.Holds = {} (held set {}, take-over pending {}; her record: {})", Who(a, id), h ? "True" : "False",
                     YesNo(Watched(id)), YesNo(TakeOverPending(id)), kind);
        return std::format("sbko holds: {} - {}", NameOf(a), h ? std::format("SBKO holds her ({})", kind) : std::string("not held"));
    }

    std::string ConsoleRelease(RE::Actor* a, float a_seconds)
    {
        const RE::FormID id = a->GetFormID();
        logger::info("[sbko release] {} - a simulated choke release after {:.1f} s -> OnChokeReleasedTask (the ChokeReleased native's path)", Who(a, id),
                     a_seconds);
        const TaskResult r = OnChokeReleasedTask(id, a_seconds, "console", g_generation.load(), "sbko release");
        return std::format("sbko release: {} {}", NameOf(a), Short(r.what, 8));
    }

    std::string ConsoleWoken(RE::Actor* a)
    {
        const RE::FormID  id   = a->GetFormID();
        const std::string name = NameOf(a);
        const auto*       st   = a->AsActorState();
        if (st && st->IsUnconscious()) {
            logger::info("[sbko woken] {} is unconscious - SkyrimNet is never told an unconscious NPC was woken to talk - nothing done", Who(a, id));
            return std::format("sbko woken: {} is unconscious - nothing done", name);
        }
        const auto& cfg       = Settings::Get();
        const bool  skyrimNet = Helper::SkyrimNetPresent();
        const bool  sent      = Helper::NarrateWoken(id, kWokenNatural, cfg.wokenTalkMode, cfg.notify, skyrimNet);
        logger::info("[sbko woken] {} - NarrateWoken(kind {} = woken by you to talk, mode {}, SkyrimNet {}) {} [console test: whatever her sleep state]",
                     Who(a, id), kWokenNatural, cfg.wokenTalkMode, skyrimNet ? 1 : 0, sent ? "dispatched" : "NOT dispatched");
        return std::format("sbko woken: {} - {}", name, sent ? (skyrimNet ? "SkyrimNet told you woke her" : "sent (SkyrimNet not found)")
                                                             : std::string("NOT dispatched - see log"));
    }

    std::string ConsoleSleepState(RE::Actor* a)
    {
        const RE::FormID    id     = a->GetFormID();
        auto*               pc     = RE::PlayerCharacter::GetSingleton();
        const auto*         st     = a->AsActorState();
        const auto*         pst    = pc ? pc->AsActorState() : nullptr;
        const std::uint32_t ss     = st ? static_cast<std::uint32_t>(st->GetSitSleepState()) : 0u;
        const bool          talk   = st && st->actorState2.talkingToPlayer;
        const bool          hostile = pc && !sbko::IsDeadNow(a) && a->IsHostileToActor(pc);
        bool                held = false, pending = false;
        {
            std::scoped_lock lk(g_mtx);
            const auto it = g_map.find(id);
            held          = it != g_map.end() && it->second.state != State::kComingTo;
            pending       = g_woken.contains(id);
        }
        logger::info("[sbko sleepstate] {}: sit-sleep state {} ({}) | talking to you {} | hostile to you {} | you sneaking {} | held by SBKO {} | 'woken to "
                     "talk' pending {} | M2: talk {}, Diary of Mine's Wake {} (DOMInteractPerk {})",
                     Who(a, id), ss, SitSleepName(ss), YesNo(talk), YesNo(hostile), YesNo(pst && pst->IsSneaking()), YesNo(held), YesNo(pending),
                     g_wokenTalkOn.load() ? "ON" : "off", g_wokenPerkOn.load() ? "ON" : "off",
                     g_domPerkId ? std::format("0x{:08X}", g_domPerkId) : std::string("not found"));
        return std::format("sbko sleepstate: {} - state {} ({}){}", NameOf(a), ss, SitSleepName(ss), hostile ? ", hostile" : "");
    }

    // ---- u2c fix 2: light-plugin FormIDs ---------------------------------------------------------------------------------------
    // SKSE's ResolveFormID (in VR: skyrimvresl's replacement) maps an id by the plugin's index in the save's
    // plugin list. An FE id whose plugin stopped being light comes back as <new index> | <old light slot> << 12 | local - garbage
    // (an NPC's FE38E673 -> 9038E673: AnotherOakwood.esp lost its ESL flag). So every FE id is ALSO saved by plugin name.
    namespace
    {
        struct LightRef
        {
            std::string   name;
            std::uint16_t local = 0;
        };

        // FE id -> her plugin: the form's own first file (field reads, TESForm.h:275-287 / TESFile.cpp:33-42), else the light plugin
        // that holds that slot NOW (the id is this session's - also right for a pending NPC who is not in memory).
        const RE::TESFile* LightFileOf(RE::FormID a_id)
        {
            if (auto* form = RE::TESForm::LookupByID(a_id)) {
                if (const auto* f = form->GetFile(0); f && f->IsFormInMod(a_id)) return f;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((a_id >> 12) & 0xFFF)) : nullptr;
        }

        // Caller holds g_mtx. Record 'LREF' v1: count u32, then per FE id: raw id u32, local u16, name length u16, name bytes.
        void WriteLightRefs(SKSE::SerializationInterface* a_intf)
        {
            std::vector<RE::FormID> fe;
            auto add = [&fe](RE::FormID id) {
                if ((id >> 24) == 0xFE && std::find(fe.begin(), fe.end(), id) == fe.end()) fe.push_back(id);
            };
            for (const auto& [id, e] : g_map) add(id);
            for (const auto& [id, p] : g_pendingConscious) add(id);
            for (const auto& [id, r] : g_released) add(id);
            for (const auto& [id, c] : g_chokeLoose) add(id);   // u7a (U7)
            {   // m34: the marked actors (record 'MARK') - Marks' own lock inside g_mtx (the allowed order)
                std::vector<RE::FormID> marked;
                Marks::CollectIds(marked);
                for (const auto id : marked) add(id);
            }
            std::vector<std::pair<RE::FormID, const RE::TESFile*>> refs;
            for (const auto id : fe) {
                if (const auto* f = LightFileOf(id))
                    refs.emplace_back(id, f);
                else
                    logger::warn("[co-save] light-plugin id 0x{:08X}: no plugin found - saved without a name (the older-save rule "
                                 "applies on load)", id);
            }
            if (!a_intf->OpenRecord(kLightRefType, kLightRefVersion)) {
                logger::warn("[co-save] could not open the light-plugin record - FE ids are saved without their plugin name");
                return;
            }
            a_intf->WriteRecordData(static_cast<std::uint32_t>(refs.size()));
            for (const auto& [id, f] : refs) {
                const std::string_view name  = f->GetFilename();
                const std::uint16_t    local = static_cast<std::uint16_t>(id & 0xFFF);
                const std::uint16_t    len   = static_cast<std::uint16_t>((std::min)(name.size(), static_cast<std::size_t>(0xFFFF)));
                a_intf->WriteRecordData(id);
                a_intf->WriteRecordData(local);
                a_intf->WriteRecordData(len);
                a_intf->WriteRecordData(name.data(), len);
            }
            if (!refs.empty())
                logger::info("[co-save] saved {} light-plugin id(s) by plugin name (record 'LREF' v{})", refs.size(), kLightRefVersion);
        }

        // A saved id -> this session's id (0 = it does not resolve; `a_how` says why or how). FF: kept raw. FE with a saved name:
        // rebuilt from the plugin. FE from an older save: ResolveFormID, repaired when her plugin became a regular one. Others:
        // ResolveFormID as before.
        RE::FormID ResolveSaved(SKSE::SerializationInterface* a_intf, RE::FormID a_raw,
                                const std::unordered_map<RE::FormID, LightRef>& a_refs, std::string& a_how)
        {
            const std::uint32_t top = a_raw >> 24;
            if (top == 0xFF) {
                a_how = "a created reference - kept as saved";
                return a_raw;
            }
            if (top == 0xFE) {
                if (auto it = a_refs.find(a_raw); it != a_refs.end()) {
                    auto*              dh = RE::TESDataHandler::GetSingleton();
                    const RE::TESFile* f  = dh ? dh->LookupModByName(it->second.name) : nullptr;   // TESDataHandler.cpp:176-185
                    if (!f) {
                        a_how = std::format("her plugin {} is not in the data folder", it->second.name);
                        return 0;
                    }
                    if (f->GetCompileIndex() == 0xFF) {
                        a_how = std::format("her plugin {} is not active", it->second.name);
                        return 0;
                    }
                    const RE::FormID id = f->IsLight()
                                              ? (0xFE000000u | (static_cast<RE::FormID>(f->GetSmallFileCompileIndex()) << 12) |
                                                 (it->second.local & 0xFFFu))
                                              : ((static_cast<RE::FormID>(f->GetCompileIndex()) << 24) | (it->second.local & 0xFFFu));
                    a_how = std::format("rebuilt from {} local 0x{:03X}{}", it->second.name, it->second.local,
                                        f->IsLight() ? "" : " (the plugin is a regular one now)");
                    return id;
                }
                RE::FormID r = 0;
                if (!a_intf->ResolveFormID(a_raw, r)) {
                    a_how = "an older save's light-plugin id whose plugin is gone";
                    return 0;
                }
                if ((r >> 24) != 0xFE && (r >> 24) != 0xFF && (r & 0x00FFF000) != 0) {
                    const RE::FormID fixed = r & 0xFF000FFF;   // the plugin became regular: drop the old light slot (bits 12-23)
                    a_how = std::format("an older save's light-plugin id: ResolveFormID gave 0x{:08X}, repaired to 0x{:08X} (her plugin "
                                        "is a regular one now)", r, fixed);
                    return fixed;
                }
                a_how = r == a_raw ? std::string("an older save's light-plugin id - unchanged")
                                   : std::format("an older save's light-plugin id - resolved to 0x{:08X}", r);
                return r;
            }
            RE::FormID r = 0;
            if (!a_intf->ResolveFormID(a_raw, r)) {
                a_how = "her plugin left the load order";
                return 0;
            }
            // u2c fix round 1 (m-2): an id an OLDER build already saved mangled (u2b wrote 9038E673 for FE38E673 after
            // AnotherOakwood.esp lost its ESL flag) - repaired only when ALL hold: no form under it; bits 12-23 non-zero; its
            // plugin (the top byte) is loaded and NOT light now; and <top byte> | <low 12 bits> IS an actor (an ex-ESL plugin
            // keeps its compacted local ids <= 0xFFF). Otherwise it is left as it is.
            if ((r >> 24) != 0xFE && (r >> 24) != 0xFF && (r & 0x00FFF000) != 0 && !RE::TESForm::LookupByID(r)) {
                auto*              dh    = RE::TESDataHandler::GetSingleton();
                const RE::TESFile* f     = dh ? dh->LookupLoadedModByIndex(static_cast<std::uint8_t>(r >> 24)) : nullptr;
                const RE::FormID   fixed = r & 0xFF000FFF;
                auto*              cand  = f && !f->IsLight() ? RE::TESForm::LookupByID<RE::Actor>(fixed) : nullptr;
                // u2c fix round 2: only an actor that carries SBKO's OWN mark (SBKO_StateFaction rank >= 1:
                // knocked out or coming to) - a bystander who happens to sit at <plugin> | <low 12 bits> is never taken
                if (cand && g_faction && cand->GetFactionRank(g_faction, false) >= 1) {
                    logger::info("[co-save] REPAIRED mangled id 0x{:08X} -> 0x{:08X} ({} is a regular plugin now; that actor is found)",
                                 r, fixed, f->GetFilename());
                    a_how = std::format("REPAIRED from the mangled 0x{:08X}", r);
                    return fixed;
                }
            }
            a_how = r == a_raw ? std::string() : std::format("resolved from 0x{:08X}", a_raw);
            return r;
        }
    }

    // ---- co-save: record 'STAT' v4 (U1; U2a used state 5 and flags bit1; U2b keeps the layout - state 5 = a u2a leftover) ------
    // u2c: the layout is still v4; FE ids get their plugin name in the record 'LREF' written before it.
    // per actor: formID u32, state u8 (1 DOWN / 2 COMING_TO / 3 UNCONSCIOUS / 5 WINDOW), savedHealRate f32, wakeAtHours f32,
    // comingToRank i32, stageSeconds f32, downChecks i32, recoverHours f32, koHours f32 (v2+; a v1 record gets
    // wakeAtHours - recoverHours); v4 then: cls u8, flags u8 (bit0 temp-protected - never set so far, bit1 healPending (U2a),
    // bit2 SBKO excluded her from SeverActions survival, bit3 a U1 knock-out, bit5 0x20 a U6 HIT knock-out - its hold has no
    // fUnconsciousHealthFraction cap; a new bit in the same byte, so the layout and the version stay v4 and an older DLL ignores it;
    // u6f: bit6 0x40 a Sleep-spell sleeper - a hit / talk wakes her - and bit7 0x80 she saw the caster (sleepMark 2), the same way),
    // potionMask i32 (-1 none), lockFrac f32 (-1 = settle not over), windowSeconds f32, severPacked i32.
    // U2a mapping (old): WAKING / DROPPING / WINDOW and a heal / straight wake still running -> 5 WINDOW with 0 s.
    // U2b mapping: a heal wake (FinishWake) still running -> 3 UNCONSCIOUS + flags bit1 (the heal wake runs again after the load);
    // a u2a WINDOW leftover not finished yet (or its finishing FinishWake still running) -> 5 WINDOW again. On load a 5 WINDOW is
    // FINISHED at the first check (FinishWake + NoBleedoutRecovery off, rank 3, quiet) - never restarted. windowSeconds = 0.
    // v3 (M1d) then: pendingConscious count u32, per NPC formID u32 + savedHealRate f32 (v1 / v2: an empty list);
    // v4 adds per pending NPC: state u8 (3 = a U1 knock-out -> FinishWake, 5 = a u2a window leftover -> FinishWake +
    // NoBleedoutRecovery off), flags u8 (bit2),
    // potionMask i32, severPacked i32.
    // ⛔ Writer and reader change TOGETHER, with the version.
    void OnSave(SKSE::SerializationInterface* a_intf)
    {
        std::scoped_lock lk(g_mtx);
        WriteLightRefs(a_intf);   // u2c fix 2: 'LREF' FIRST - the reader needs it before STAT / RELS
        if (!a_intf->OpenRecord(kRecordType, kRecordVersion)) {
            logger::warn("[co-save] could not open the SBKO record - the knockout state is NOT saved with this game");
            return;
        }
        const std::uint32_t n = static_cast<std::uint32_t>(g_map.size());
        a_intf->WriteRecordData(n);
        for (const auto& [id, e] : g_map) {
            State        st     = e.state;
            float        saved  = e.savedHealRate;
            float        wakeAt = e.wakeAtHours;
            std::int32_t rank   = e.comingToRank;
            float        stage  = e.stageSeconds;
            std::int32_t checks = e.downChecks;
            float        lock   = e.lockFrac;
            float        window = 0.0f;   // U2b: no window
            // u6f: a sleeper's wake that was still waiting (a hit / talk) is saved like a pending heal - the first check after the load wakes her
            bool         healSav = e.healPending || e.sleepWakePending;
            if (st == State::kEntering) {   // no reply yet: saved as DOWN / UNCONSCIOUS, the HealRate comes back from her base value
                st     = e.unc ? State::kUnconscious : State::kDown;
                saved  = kHealRateUseBase;
                wakeAt = e.dispatchHours + e.recoverHours;
                checks = 0;
                lock   = -1.0f;
            } else if (st == State::kWindow) {
                window = 0.0f;   // a u2a leftover not finished yet: saved as it came, finished at the first check after the load
            } else if (st == State::kComingTo && e.awaitingWake && e.unc && e.wakeKind == WakeKind::kStraight) {
                if (e.leftoverWake) {
                    st = State::kWindow;   // finishing a u2a leftover, still running: saved as it came - finished again on load
                } else {
                    // U2b: a heal wake (FinishWake) still running - saved as UNCONSCIOUS with the heal pending: the first check
                    // after the load runs the heal wake again (idempotent); her timer is kept
                    st      = State::kUnconscious;
                    healSav = true;
                    rank    = 0;
                    stage   = 0.0f;
                }
            } else if (st == State::kComingTo && e.awaitingWake) {
                // m1b (m-1): WakeUp / FinishWake has not finished - the save may not carry its stack. Saved as DOWN /
                // UNCONSCIOUS with the SAME HealRate and an elapsed timer: the first check after the load wakes her again
                // (harmless if she is up - both wakes are idempotent).
                st     = e.unc ? State::kUnconscious : State::kDown;
                wakeAt = 0.0f;
                rank   = 0;
                stage  = 0.0f;
            }
            const std::uint8_t stByte = static_cast<std::uint8_t>(st);
            // U6: bit 0x20 = a HIT knock-out (the hold has no fUnconsciousHealthFraction cap) - an older DLL ignores the bit
            // u6f: 0x40 = a Sleep-spell sleeper, 0x80 = she saw the caster (sleepMark 2) - new bits in the same byte (an older DLL ignores them)
            const std::uint8_t flags  = static_cast<std::uint8_t>((healSav ? 0x02 : 0) | (e.severExcluded ? 0x04 : 0) |
                                                                  (e.unc ? 0x08 : 0) | (e.hitKO ? 0x20 : 0) |
                                                                  (e.sleepMark > 0 ? 0x40 : 0) | (e.sleepMark == kSleepSeen ? 0x80 : 0));
            a_intf->WriteRecordData(id);
            a_intf->WriteRecordData(stByte);
            a_intf->WriteRecordData(saved);
            a_intf->WriteRecordData(wakeAt);
            a_intf->WriteRecordData(rank);
            a_intf->WriteRecordData(stage);
            a_intf->WriteRecordData(checks);
            a_intf->WriteRecordData(e.recoverHours);
            a_intf->WriteRecordData(e.koHours);   // v2
            a_intf->WriteRecordData(e.cls);       // v4
            a_intf->WriteRecordData(flags);
            a_intf->WriteRecordData(e.potionMask);
            a_intf->WriteRecordData(lock);
            a_intf->WriteRecordData(window);
            a_intf->WriteRecordData(e.severPacked);
            logger::info("[co-save] saved {} {}{} (HealRate {:.4f}, wake at hour {:.2f}, out since hour {:.2f}, rank {}, stage {:.1f} s, "
                         "checks {}{}{})",
                         e.who, StateName(st), st != e.state ? std::format(" (was {})", StateName(e.state)) : std::string(),
                         saved, wakeAt, e.koHours, rank, stage, checks,
                         st == State::kUnconscious ? std::format(", hold {}, potions mask {}, SeverActions {}{}",
                                                                 lock >= 0.0f ? Pct1(lock) : std::string("settling"), e.potionMask,
                                                                 e.severExcluded ? "excluded (" + SeverNeeds(e.severPacked) + ")" : "-",
                                                                 e.sleepMark > 0 ? ", a Sleep-spell sleeper" : e.hitKO ? ", a hit knock-out" : "")
                         : st == State::kWindow ? std::format(", potions mask {}, SeverActions {} - a u2a leftover, finished on load",
                                                              e.potionMask, e.severExcluded ? "excluded" : "-")
                                                : std::string(),
                         healSav ? ", a heal is pending" : "");
        }
        // v3 (M1d): the NPCs still waiting for the load safety's wake
        const std::uint32_t np = static_cast<std::uint32_t>(g_pendingConscious.size());
        a_intf->WriteRecordData(np);
        for (const auto& [id, p] : g_pendingConscious) {
            const std::uint8_t stByte =
                static_cast<std::uint8_t>(p.window ? State::kWindow : p.unc ? State::kUnconscious : State::kDown);
            const std::uint8_t flags  = static_cast<std::uint8_t>((p.severExcluded ? 0x04 : 0) | (p.belt ? 0x10 : 0));
            a_intf->WriteRecordData(id);
            a_intf->WriteRecordData(p.healRate);
            a_intf->WriteRecordData(stByte);   // v4
            a_intf->WriteRecordData(flags);
            a_intf->WriteRecordData(p.potionMask);
            a_intf->WriteRecordData(p.severPacked);
        }
        logger::info("[co-save] saved {} NPC(s), {} waiting for the load safety (record 'STAT' v{})", n, np, kRecordVersion);
        // u2a fix 1: the released watch - its own record 'RELS' v1 (an older DLL skips an unknown record type)
        if (!a_intf->OpenRecord(kReleasedType, kReleasedVersion)) {
            logger::warn("[co-save] could not open the released-watch record - those NPCs are not saved");
            return;
        }
        const std::uint32_t nr = static_cast<std::uint32_t>(g_released.size());
        a_intf->WriteRecordData(nr);
        for (const auto& [id, r] : g_released) {
            // u6 fix 1 (F1): 0x04 = a hit knock-out - a new bit in the same byte (RELS stays v1; an older DLL reads only 0x01 / 0x02)
            // u6f: 0x08 = a Sleep-spell sleeper, 0x10 = she saw the caster
            // u7a: 0x20 = a choke knock-out (its seconds in 'CHOK')
            const std::uint8_t fl = static_cast<std::uint8_t>((r.deferred ? 0x01 : 0) | (r.severExcluded ? 0x02 : 0) |
                                                              (r.hitKO ? 0x04 : 0) | (r.sleepMark > 0 ? 0x08 : 0) |
                                                              (r.sleepMark == kSleepSeen ? 0x10 : 0) | (r.choke ? 0x20 : 0));
            a_intf->WriteRecordData(id);
            a_intf->WriteRecordData(r.healRate);
            a_intf->WriteRecordData(r.potionMask);
            a_intf->WriteRecordData(fl);
            a_intf->WriteRecordData(r.severPacked);
        }
        if (nr) logger::info("[co-save] saved {} released NPC(s) to watch (record 'RELS' v{})", nr, kReleasedVersion);
        // u7a (U7): the choke seconds (build 3's neck mark) - 'CHOK' v1, last
        if (!a_intf->OpenRecord(kChokeType, kChokeVersion)) {
            logger::warn("[co-save] could not open the choke record - the choke seconds are not saved");
        } else {
            struct ChokeItem
            {
                RE::FormID   id;
                std::uint8_t where;
                float        seconds, hours;
            };
            std::vector<ChokeItem> items;
            for (const auto& [id, e] : g_map)
                if (e.cls == kClsChoke) items.push_back({ id, 0, e.chokeSeconds, e.chokeHours });
            for (const auto& [id, r] : g_released)
                if (r.choke) items.push_back({ id, 1, r.chokeSeconds, r.chokeHours });
            for (const auto& [id, c] : g_chokeLoose) items.push_back({ id, 2, c.seconds, c.hours });
            a_intf->WriteRecordData(static_cast<std::uint32_t>(items.size()));
            for (const auto& it : items) {
                a_intf->WriteRecordData(it.id);
                a_intf->WriteRecordData(it.where);
                a_intf->WriteRecordData(it.seconds);
                a_intf->WriteRecordData(it.hours);
            }
            if (!items.empty()) logger::info("[co-save] saved {} choke record(s) (record 'CHOK' v{})", items.size(), kChokeVersion);
        }
        Marks::Save(a_intf);   // m34: record 'MARK' v1 (the skin marks), last
        spdlog::default_logger()->flush();   // u2c P2: a save is a natural point to have the log on disk (no flush per line)
    }

    void OnLoad(SKSE::SerializationInterface* a_intf)
    {
        std::uint32_t type = 0, version = 0, length = 0;
        std::unordered_map<RE::FormID, LightRef> lrefs;   // u2c fix 2: filled by 'LREF', which a u2c save writes first
        struct ChokeLoaded
        {
            RE::FormID   id;
            std::uint8_t where;
            float        seconds, hours;
        };
        std::vector<ChokeLoaded> chokes;   // u7a: 'CHOK' - applied after every record is read
        while (a_intf->GetNextRecordInfo(type, version, length)) {
            if (type == Marks::kRecordType) {   // m34: the skin marks ('LREF' came first - its names resolve FE ids)
                Marks::Load(a_intf, version, [&](RE::FormID a_raw, std::string& a_how) { return ResolveSaved(a_intf, a_raw, lrefs, a_how); });
                continue;
            }
            if (type == kChokeType) {   // u7a (U7)
                if (version != kChokeVersion) {
                    logger::warn("[co-save] choke record version {} is not {} - discarded", version, kChokeVersion);
                    continue;
                }
                std::uint32_t n = 0;
                if (a_intf->ReadRecordData(n) != sizeof n) continue;
                for (std::uint32_t i = 0; i < n; ++i) {
                    RE::FormID   raw = 0;
                    std::uint8_t where = 0;
                    float        secs = 0.0f, hrs = 0.0f;
                    if (a_intf->ReadRecordData(raw) != sizeof raw || a_intf->ReadRecordData(where) != sizeof where ||
                        a_intf->ReadRecordData(secs) != sizeof secs || a_intf->ReadRecordData(hrs) != sizeof hrs) {
                        logger::warn("[co-save] the choke record is cut after {} of {}", i, n);
                        break;
                    }
                    std::string      how;
                    const RE::FormID newId = ResolveSaved(a_intf, raw, lrefs, how);
                    if (!newId) {
                        logger::info("[co-save] choke 0x{:08X} dropped: {}", raw, how);
                        continue;
                    }
                    chokes.push_back({ newId, where, secs, hrs });
                }
                continue;
            }
            if (type == kLightRefType) {
                if (version != kLightRefVersion) {
                    logger::warn("[co-save] light-plugin record version {} is not {} - discarded (FE ids use the older-save rule)", version,
                                 kLightRefVersion);
                    continue;
                }
                std::uint32_t n = 0;
                if (a_intf->ReadRecordData(n) != sizeof n) continue;
                for (std::uint32_t i = 0; i < n; ++i) {
                    RE::FormID    raw   = 0;
                    std::uint16_t local = 0, len = 0;
                    if (a_intf->ReadRecordData(raw) != sizeof raw || a_intf->ReadRecordData(local) != sizeof local ||
                        a_intf->ReadRecordData(len) != sizeof len) {
                        logger::warn("[co-save] the light-plugin record is cut after {} of {}", i, n);
                        break;
                    }
                    std::string name(len, '\0');
                    if (len && a_intf->ReadRecordData(name.data(), len) != len) {
                        logger::warn("[co-save] the light-plugin record is cut after {} of {}", i, n);
                        break;
                    }
                    lrefs[raw] = LightRef{ std::move(name), local };
                }
                logger::info("[co-save] read {} light-plugin id(s) with their plugin names (record 'LREF' v{})", lrefs.size(), version);
                continue;
            }
            if (type == kReleasedType) {   // u2a fix 1: the released watch (its own record - STAT's layout is unchanged)
                if (version != kReleasedVersion) {
                    logger::warn("[co-save] released-watch record version {} is not {} - discarded", version, kReleasedVersion);
                    continue;
                }
                std::scoped_lock lk(g_mtx);
                g_released.clear();
                std::uint32_t n = 0;
                if (a_intf->ReadRecordData(n) != sizeof n) continue;
                for (std::uint32_t i = 0; i < n; ++i) {
                    RE::FormID   id = 0;
                    Released     r;
                    std::uint8_t fl = 0;
                    const bool whole = a_intf->ReadRecordData(id) == sizeof id && a_intf->ReadRecordData(r.healRate) == sizeof r.healRate &&
                                       a_intf->ReadRecordData(r.potionMask) == sizeof r.potionMask &&
                                       a_intf->ReadRecordData(fl) == sizeof fl && a_intf->ReadRecordData(r.severPacked) == sizeof r.severPacked;
                    if (!whole) {
                        logger::warn("[co-save] the released-watch record is cut after {} of {} NPC(s)", i, n);
                        break;
                    }
                    std::string      how;
                    const RE::FormID newId = ResolveSaved(a_intf, id, lrefs, how);   // u2c fix 2
                    if (!newId) {
                        logger::info("[co-save] released-watch 0x{:08X} dropped: {}", id, how);
                        continue;
                    }
                    if (!how.empty()) logger::info("[co-save] released-watch 0x{:08X} -> 0x{:08X} ({})", id, newId, how);
                    r.deferred      = (fl & 0x01) != 0;
                    r.severExcluded = (fl & 0x02) != 0;
                    r.hitKO         = (fl & 0x04) != 0;   // u6 fix 1 (F1): an older save never sets it = a bleedout knock-out
                    r.sleepMark     = (fl & 0x08) ? ((fl & 0x10) ? kSleepSeen : kSleepUnseen) : 0;   // u6f (an older save: 0)
                    r.choke         = (fl & 0x20) != 0;   // u7a (its seconds come with 'CHOK')
                    g_released[newId] = r;
                }
                logger::info("[co-save] restored {} released NPC(s) to watch", g_released.size());
                continue;
            }
            if (type != kRecordType) {
                logger::warn("[co-save] unknown record type 0x{:08X} - skipped", type);
                continue;
            }
            if (version < 1 || version > kRecordVersion) {   // v1 (M1 / M1b), v2 (M1c), v3 (M1d/e/f) are still read
                logger::warn("[co-save] record version {} is not 1..{} - discarded (no NPC restored)", version, kRecordVersion);
                continue;
            }
            std::scoped_lock lk(g_mtx);
            g_map.clear();
            g_pendingConscious.clear();
            bool cut = false;
            std::uint32_t n = 0;
            if (a_intf->ReadRecordData(n) != sizeof n) {
                logger::warn("[co-save] the SBKO record is empty or cut - nothing restored");
                SyncCount();
                continue;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::FormID   id = 0;
                std::uint8_t st = 0, cls = 0, flags = 0;
                float        saved = 0.0f, wakeAt = 0.0f, stage = 0.0f, recover = 0.0f, ko = 0.0f, lock = -1.0f, window = 0.0f;
                std::int32_t rank = 0, checks = 0, mask = -1, packed = 0;
                const bool   whole = a_intf->ReadRecordData(id) == sizeof id && a_intf->ReadRecordData(st) == sizeof st &&
                                   a_intf->ReadRecordData(saved) == sizeof saved && a_intf->ReadRecordData(wakeAt) == sizeof wakeAt &&
                                   a_intf->ReadRecordData(rank) == sizeof rank && a_intf->ReadRecordData(stage) == sizeof stage &&
                                   a_intf->ReadRecordData(checks) == sizeof checks &&
                                   a_intf->ReadRecordData(recover) == sizeof recover &&
                                   (version < 2 || a_intf->ReadRecordData(ko) == sizeof ko) &&
                                   (version < 4 || (a_intf->ReadRecordData(cls) == sizeof cls &&
                                                    a_intf->ReadRecordData(flags) == sizeof flags &&
                                                    a_intf->ReadRecordData(mask) == sizeof mask &&
                                                    a_intf->ReadRecordData(lock) == sizeof lock &&
                                                    a_intf->ReadRecordData(window) == sizeof window &&
                                                    a_intf->ReadRecordData(packed) == sizeof packed));
                if (!whole) {
                    logger::warn("[co-save] the SBKO record is cut after {} of {} NPC(s) - the rest is lost", i, n);
                    cut = true;
                    break;
                }
                std::string      how;
                const RE::FormID newId = ResolveSaved(a_intf, id, lrefs, how);   // u2c fix 2
                if (!newId) {
                    logger::info("[co-save] 0x{:08X} dropped: {}", id, how);
                    continue;
                }
                if (!how.empty()) logger::info("[co-save] 0x{:08X} -> 0x{:08X} ({})", id, newId, how);
                const auto state   = static_cast<State>(st);
                const bool okState = state == State::kDown || state == State::kComingTo ||
                                     (version >= 4 && (state == State::kUnconscious || state == State::kWindow));
                if (!okState) {
                    logger::warn("[co-save] 0x{:08X} has an unknown state {} - dropped", newId, st);
                    continue;
                }
                Entry e;
                e.state         = state;
                e.serial        = ++g_serial;
                e.episode       = e.serial;
                e.savedHealRate = saved;
                e.wakeAtHours   = wakeAt;
                e.comingToRank  = rank;
                e.stageSeconds  = stage;
                e.downChecks    = checks;
                e.recoverHours  = recover;
                if (version < 2) {
                    // a v1 record has no knock-out moment: the timer was rolled at it, so wakeAt - recover is that moment. An
                    // m1b "WakeUp still running" entry was written with wakeAt 0 - then the load time minus the roll stands in.
                    ko = wakeAt - recover;
                    if (ko < 0.0f) ko = (std::max)(0.0f, HoursNow() - recover);
                }
                e.koHours        = ko;
                e.cls            = cls;
                e.unc            = state == State::kUnconscious || state == State::kWindow || (flags & 0x08) != 0;
                e.potionMask     = mask;
                e.lockFrac       = lock;
                e.windowSeconds  = window;
                e.severExcluded  = (flags & 0x04) != 0;
                e.severPacked    = packed;
                e.healPending    = state == State::kUnconscious && (flags & 0x02) != 0;   // U2a: a heal seen before the save
                e.hitKO          = (flags & 0x20) != 0;   // U6: a hit knock-out (an older save never sets it = a bleedout knock-out)
                // u6f: a Sleep-spell sleeper (2 seen / 1 not) - an older save never sets it = an injury knock-out (a heal wakes her)
                e.sleepMark      = (flags & 0x40) ? ((flags & 0x80) ? kSleepSeen : kSleepUnseen) : 0;
                e.hitPct         = -1.0f;                 // not saved: a hold not settled yet takes her Health at the settle
                e.shoveAfterLoad = state == State::kUnconscious;   // she may load standing: the first check shoves her once
                e.checksSince = 0;                                 // U2a: the external-clear "engine reset" context
                e.since       = "the load";
                e.resetActive = true;
                e.settledOnce = lock >= 0.0f;                      // u2a fix 1: a settled hold = the settle was over
                e.who         = std::format("0x{:08X}", newId);   // the name is filled in by the first check that sees her
                e.rankDirty   = state == State::kComingTo;          // u2c fix 5: her rank is written again once she is found
                logger::info("[co-save] restored {} {} (HealRate {:.4f}, wake at hour {:.2f}, out since hour {:.2f}{}, rank {}, "
                             "stage {:.1f} s, checks {}{}{})",
                             e.who, StateName(state), saved, wakeAt, ko, version < 2 ? " (v1 record: derived)" : "", rank, stage, checks,
                             state == State::kUnconscious
                                 ? std::format(", hold {}, potions mask {}, SeverActions {}{}",
                                               lock >= 0.0f ? Pct1(lock) : std::string("settling"), mask,
                                               e.severExcluded ? "excluded (" + SeverNeeds(packed) + ")" : "-",
                                               e.sleepMark > 0 ? ", a Sleep-spell sleeper" : e.hitKO ? ", a hit knock-out" : "")
                             : state == State::kWindow ? std::string(" - a u2a heal-wake window: finished at the first check (FinishWake + "
                                                                     "NoBleedoutRecovery off)")
                                                       : std::string(),
                             e.healPending ? ", a heal is pending (the wake starts at the first check)" : "");
                g_map[newId] = std::move(e);
            }
            // v3 (M1d): the NPCs still waiting for the load safety's wake (v1 / v2: none)
            std::uint32_t np = 0;
            if (!cut && version >= 3 && a_intf->ReadRecordData(np) == sizeof np) {
                for (std::uint32_t i = 0; i < np; ++i) {
                    RE::FormID   id    = 0;
                    Pending      p;
                    std::uint8_t st = static_cast<std::uint8_t>(State::kDown), flags = 0;
                    const bool whole = a_intf->ReadRecordData(id) == sizeof id && a_intf->ReadRecordData(p.healRate) == sizeof p.healRate &&
                                       (version < 4 || (a_intf->ReadRecordData(st) == sizeof st &&
                                                        a_intf->ReadRecordData(flags) == sizeof flags &&
                                                        a_intf->ReadRecordData(p.potionMask) == sizeof p.potionMask &&
                                                        a_intf->ReadRecordData(p.severPacked) == sizeof p.severPacked));
                    if (!whole) {
                        logger::warn("[co-save] the load-safety list is cut after {} of {} NPC(s)", i, np);
                        break;
                    }
                    std::string      how;
                    const RE::FormID newId = ResolveSaved(a_intf, id, lrefs, how);   // u2c fix 2
                    if (!newId) {
                        logger::info("[co-save] load-safety 0x{:08X} dropped: {}", id, how);
                        continue;
                    }
                    if (!how.empty()) logger::info("[co-save] load-safety 0x{:08X} -> 0x{:08X} ({})", id, newId, how);
                    p.window        = st == static_cast<std::uint8_t>(State::kWindow);
                    p.unc           = p.window || st == static_cast<std::uint8_t>(State::kUnconscious);
                    p.severExcluded = (flags & 0x04) != 0;
                    p.belt          = (flags & 0x10) != 0;   // u2a fix 1
                    g_pendingConscious[newId] = p;
                    logger::info("[co-save] restored 0x{:08X} as waiting for the load safety ({} HealRate {:.4f}{})", newId,
                                 p.window ? "FinishWake + NoBleedoutRecovery off" : p.unc ? "FinishWake" : "WakeUp", p.healRate,
                                 p.unc ? std::format(", potions mask {}, SeverActions include {}", p.potionMask, p.severExcluded ? "yes" : "no")
                                       : std::string());
                }
            }
            SyncCount();
            logger::info("[co-save] restored {} NPC(s), {} waiting for the load safety (record v{})", g_map.size(),
                         g_pendingConscious.size(), version);
        }
        if (!chokes.empty()) {   // u7a (U7): the choke seconds onto their entries / the loose list
            std::scoped_lock lk(g_mtx);
            std::size_t      onEntry = 0, onReleased = 0, loose = 0, lost = 0;
            for (const auto& c : chokes) {
                if (c.where == 0) {
                    if (auto it = g_map.find(c.id); it != g_map.end() && it->second.cls == kClsChoke) {
                        it->second.chokeSeconds = c.seconds;
                        it->second.chokeHours   = c.hours;
                        ++onEntry;
                        continue;
                    }
                } else if (c.where == 1) {
                    if (auto it = g_released.find(c.id); it != g_released.end() && it->second.choke) {
                        it->second.chokeSeconds = c.seconds;
                        it->second.chokeHours   = c.hours;
                        ++onReleased;
                        continue;
                    }
                } else if (c.where == 2) {
                    g_chokeLoose[c.id] = ChokeLoose{ c.seconds, c.hours };
                    ++loose;
                    continue;
                }
                ++lost;
            }
            g_chokeLooseCount.store(g_chokeLoose.size());
            logger::info("[co-save] restored the choke seconds (record 'CHOK'): {} on a choke knock-out, {} on the released watch, {} loose (no "
                         "knock-out){}",
                         onEntry, onReleased, loose, lost ? std::format(", {} with no matching record (dropped)", lost) : std::string());
        }
    }

    void OnRevert(SKSE::SerializationInterface*)
    {
        ++g_generation;
        Marks::Revert();   // m34
        {
            std::scoped_lock lk(g_mtx);
            g_map.clear();
            g_adoptCandidates.clear();
            g_adoptAtLoad.clear();   // u6h
            g_declined.clear();
            g_pendingConscious.clear();
            g_released.clear();   // u2a fix 1
            g_silentLogged.clear();   // u2c
            g_grace.clear();          // u2d
            g_woken.clear();          // u7a (M2)
            g_chokeLoose.clear();     // u7a (U7) - refilled by 'CHOK' when the save has one
            g_chokeLooseCount.store(0);
            g_lootPending.store(0);   // 2.1.0 looting: a menu of the old world is not ours to close
            g_lootOpen.store(0);
            g_loadSafety  = false;
            g_adoptAll    = false;
            g_hasLastTick = false;
            SyncCount();
        }
        {
            std::scoped_lock lk(g_u5Mtx);   // u6b (U5): rolls and bleedout decisions belong to the old world
            g_u5.clear();
            g_fdcBeam.clear();              // u7a (U4)
        }
        {
            std::scoped_lock lk(g_watchMtx);   // u7a (U7)
            g_takeOverPending.clear();
        }
        // U1: the old world's graphs keep our static graph sink (harmless: an id not in g_foot is ignored)
        std::scoped_lock lk(g_footMtx);
        g_foot.clear();
        g_footCount.store(0);
    }
}
