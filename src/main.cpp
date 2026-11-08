#include "PCH.h"
#include <spdlog/sinks/callback_sink.h>   // u2c P1: the line-counting sink
#include "Api.h"       // u7a (U7): the SBKO_API natives
#include "Helper.h"
#include "Knockout.h"
#include "Marks.h"     // m34 (build 3): the skin marks
#include "RefillGuard.h"
#include "Settings.h"
#include "TestCmd.h"
#include "VmCall.h"

// SimpleBleedoutKO 2.0 - the C++ SKSE plugin. 2.0.0-test1: the first brick, a test console command only.
// 2.0.0-m0: the same brick moved onto MinLL/CommonLibVR 4.39.5 (MIT) - ONE DLL for SE 1.5.97 / AE 1.6.x
// incl. 1.7.99 / VR; behaviour unchanged.
// 2.0.0-m1: THE KNOCKOUT IN C++ (Knockout.cpp) - v10's SBKO_DownWatcherScript retired (the same deploy turns
// it into an empty stub and removes the SPID line); the engine writes go through SBKO_Helper.psc (Helper.cpp). Settings
// from SimpleBleedoutKO.ini (Settings.cpp), state in the SKSE co-save. The `sbko` console test is kept unchanged.
// 2.0.0-m1b: an independent review's fixes M-1, m-1..m-4 (Knockout.cpp).
// 2.0.0-m1c: two changes - no wake on a cell detach (heal or timer only); a timer wake is narrated
// (SBKO_Helper.NarrateWake); co-save record 'STAT' v2 (+ koHours).
// 2.0.0-m1d: the load safety - at a game load, knocked-out NPCs NOT loaded are made conscious
// (WakeUp(id, saved, 0, 0)); one not in memory is woken when first seen loaded. Co-save record 'STAT' v3.
// 2.0.0-m1e: the load safety keeps an NPC whose 3D is not built yet but whose cell is attached.
// 2.0.0-m1f: the brawl guard - a bleeding-out member of DGIntimidateFaction (Skyrim.esm 0x04CFA6; vanilla and
// SeverActions brawls) is never knocked out, and stays skipped until she is seen out of bleedout.
// 2.0.0-u1: THE GUARDED UNCONSCIOUS (U1):
// a protected / essential NPC in bleedout is made UNCONSCIOUS (SetUnconscious first,
// Paralysis 1, the 0.001 shove) instead of kept in bleedout; Health held at min(fUnconsciousHealthFraction, her Health after
// the settle); re-assert + shove when anything clears it; Smart NPC Potions abilities off; a follower excluded from
// SeverActions survival (SBKO_SeverCompat.psc); VRTE's KO (rank 5) left alone; time wake only (FinishWake). Log only: magic
// effects on held NPCs (refined heal rule) and footsteps of a ragdolled graph (re-shove). Co-save 'STAT' v4. Console sbkou /
// sbkot added (each on its own donor).
// 2.0.0-u1 fix round 1 (an independent review): the hold follows her
// Health DOWN and never rises; the time wake / VRTE let-go wait for a HoldDown / HoldHealthTo still in flight (M-1); Health is
// measured against the engine's maximum (permanent + temporary, Actor::GetActorValueMax) and the put-back target goes to Papyrus as
// absolute Health, SBKO_Helper.HoldHealthTo (M-2); bleedout-while-unconscious is tested before the timer wake (M-5).
// 2.0.0-u2a (an NPC stayed down through 77 Healing Hands applications): THE HEAL WAKE. The first
// HEAL (refined rule, any Actor caster incl. herself) on an unconscious NPC -> (a) WakeStand: unconscious off, Paralysis 0, the
// engine's get-up -> (b) at GetUpEnd/GetUpExit (graph sink) or a standing check (2nd-check fallback) DropToBleedout:
// NoBleedoutRecovery on + Health -1 = the engine's bleedout FROM STANDING -> (c) WINDOW fWindowSeconds (15) of real bleedout
// (DoM's capture -> LetGo) -> EndWindow (M1's WakeUp path + potions / SeverActions back) -> coming to + NarrateHealWake.
// bHealWakeWindow 0 = the heal wakes her straight up (FinishWake). SeverActions guard: membership now decided in Papyrus
// (teammate OR SeverActions roster OR its survival list; -3 = not tracked). Effect log: each kind once per knock-out, counted.
// 2.0.0-u2a fix round 1 (an independent and a scenario review): a released NPC who comes
// back still unconscious and no longer taken is RE-ADOPTED (watch list saved in co-save record 'RELS'); every load-safety / bag /
// unloaded "unconscious off" also gets a second run when she is next seen loaded; the window puts a no-heal rise back to the drop
// level and EndWindow always ends above 0; the external-wake exceptions are exactly these (settle, bleedout, 2 checks after a load /
// attach / 3D load, Paralysis-only); TakenBy (incl. VRTE rank 5) is asked before every heal-wake step; LetGo retries until replied;
// bLogEffects 0 silences the EFFECT lines; "could not start" only at the 2nd check after the drop.
// 2.0.0-u2a fix round 2 (a delta review): a ReAdopt that cannot be dispatched puts her back on the released watch
// (same record, carried on the call) instead of forgetting her unconscious.
// 2.0.0-u2b (no bleedout after unconscious - Diary of Mine's menu already works on unconscious NPCs):
// NO bleedout window (WAKING / DROPPING / WINDOW, DropToBleedout, EndWindow, bHealWakeWindow, fWindowSeconds removed). A heal wakes
// her straight up (FinishWake + NarrateHealWake). Another mod's "wake" (DoM's menu Wake) does NOT wake her: SBKO puts her back down
// and shows "<name> is too injured to wake up." (bExternalWakeCounts removed). Bag / Abduct / Tie-up / VRTE: SBKO lets go (unchanged).
// A u2a save's window entry is finished with FinishWake (+ NoBleedoutRecovery off) at the first check.
// 2.0.0-u2b fix (a scenario review): a Diary of Mine slave (DOMActorFaction) counts as taken (a fast Abduct is never
// read as a Wake; slaves are never knocked out by SBKO), and DoM's bag keeps her unconscious (watched, re-adopted when she comes out
// of the sack still unconscious - they need healing).
// 2.0.0-u2c (six fixes + a performance pass): no QueueNiNodeUpdate in any wake (the heal-wake
// vanish / pops); light-plugin (FE) ids saved by plugin name (record 'LREF'); the orphan net (rank 10 + unconscious, no record ->
// held again; a stale conscious mark -> rank 0); NoBleedoutRecovery ON while held, OFF on every exit; the coming-to clock runs
// while she is unloaded; log fixes; one PERF line per minute of play, bVerboseChecks 0, the log flushed on warnings / by the check
// thread (every fCheckSeconds when lines were written) / at a save instead of on every line.
// 2.0.0-u2c fix round 1 (two reviews): a 256 KB log buffer; constant-effect (ability) events dropped in the sink;
// the PERF line counts real graph attaches; a created (FF) NPC whose form is gone after a load is dropped; a mangled pending id
// (9038E673-style, written by u2b) is repaired at load when its ex-ESL actor is found.
// 2.0.0-u2c fix round 2: that repair only takes an actor carrying SBKO's own mark (SBKO_StateFaction
// rank >= 1) - never a bystander at the same plugin index + low 12 bits.
// 2.0.0-u2d: the essential regen bonus game setting from the ini ([Regen] fEssentialNonCombatHealRateBonus,
// 0.1; engine 5.0) at data load + after every load; every wake raises Health to at least fWakeHealthFraction (0.25) of max
// (the old "+50 under 30 %" is gone); 1.5 s in the engine's bleedout before every knock-out (fBleedoutGraceSeconds - other mods act
// on bleedout first; the check thread wakes at the due moment); SKSE::Init(a_skse, false) so SBKO's own logger stays (u2c's buffer /
// flush / line count were never live); essential / protected logged for the reference and the base apart.
// 2.0.0-u2e: RefillGuard - the engine's FULL heal of essential / protected NPCs at combat end
// (Actor::StopCombat +0x62E96D, found by static RE) is skipped for every essential /
// protected NPC ([Regen] iCombatEndRefill 2; 1 = followers, 0 = nobody), and the same heal inside SBKO's own knock-out (SetUnconscious
// runs StopCombat) and wake / let-go / clear (SetNoBleedoutRecovery(false) +0x62C9B9) is skipped - she is held at her real Health and
// wakes at fWakeHealthFraction, not full. VR 1.4.15 only, both sites byte-checked (fail closed). fBleedoutGraceSeconds default 0.
// 2.0.0-u2e fix round 1: LetGo raises a living NPC to fWakeHealthFraction (LetGo +2 args - needs SBKO_Helper.pex u2e);
// modes 1 / 2 cover NPCs only (creatures keep the refill); mode 1 = SBKO's follower test; the in-flight set is also reset at kNewGame /
// kPostLoadGame; log wording and the Health basis (permanent + temporary).
// 2.0.0-u2f (1 %; a quieter log): the game setting fEssentialHealthPercentReGain from the ini
// ([Regen] 0.01; engine 0.3 - an unconscious essential at <= 0 HP hit again was set to 30 %, e.g. -28 HP -> 30 %);
// a combat-end "[refill]" line only when she is hurt and her Health moved >= 0.5 since her last one (the skip is unchanged).
// 2.0.0-u6 (rules R1-R8): THE HEAD-HIT / SNEAK KNOCK-OUT inside SBKO
// (Knockout.cpp HitSink + OnHitTask).
// The player's punch or bash anywhere, or mace / warhammer on PLANCK's head node (NPC Head [Head]), knocks an NPC out at 100 % (no
// roll) when it is a sneak attack, she does not detect the player (Papyrus IsDetectedBy's own test) or her Health <= 10 %; a worn ArmorHelmet
// item = immune; not ghosts, not bleeding out / down / held / taken, not in a brawl or the Companions' fist-fight; followers too
// (bHeadHitFollowers). SBKO's own knock-out (KnockOutU aiFromBleedout 0) and wake rules; the hold of a hit knock-out = her Health at
// the knock-out (no 25 % cap; co-save STAT flags bit 0x20, layout still v4). ini [HeadHit] (10 keys); one [headhit] line per player
// punch / bash / blunt hit on an actor; the PERF line counts hit events.
// 2.0.0-u6 fix round 1: F1 a released hit knock-out keeps its no-cap hold when re-adopted (Released
// + RELS flag 0x04 + ReAdopt call bit 8); F2 the low-Health trigger has a 0.1 % float tolerance (Injury Engine's 10 % floor), logged when
// it decided; F3 detection is asked LAST (only when every other rule passed and neither the sneak bit nor low Health said yes); F5 a
// PLANCK hit is also confirmed by PLANCK's GetCurrentHitEvent() (interface at kDataLoaded, build >= 0.8.0; else the magic word alone);
// F7 the "replied -1" line worded by kind; F8 a player hit with source 0 and no bash bit is logged (not read as a punch).
// 2.0.0-u6 fix round 2 (no punch mechanic - only a blunt weapon or a bash; a follower knocked out by a hit
// gets no FDC roll): punches never knock out (bHeadHitFists removed; no line for an unarmed hit, F8's source-0 line gone);
// a hit knock-out and its re-adoption get FDC_SBKO_RolledFaction rank 1 before rank 10 = no Follower Death Chance roll (KnockOutU and
// ReAdopt + Int aiNoFdcRoll: needs SBKO_Helper.pex u6 fix 2 - deploy the DLL and the pex TOGETHER). Bleedout knock-outs still roll.
// 2.0.0-u6 fix round 3: PLANCK's extended hit block is never read unless activeragdoll.dll is loaded (decided once
// at kDataLoaded; SE / AE always skip it) - a blunt weapon then logs "no head data"; bashes are unaffected.
// 2.0.0-u6b:
//   (1) U6 without PLANCK (the Knockout mod does not require PLANCK; PLANCK is kept for VR): with activeragdoll.dll
//       loaded a mace / warhammer counts on PLANCK's head node only (unchanged); without it (SE / AE always, VR without PLANCK) it counts
//       ANYWHERE, like the Knockout mod. The [headhit] line says which rule applied.
//   (2) U5 THE CHANCE KNOCK-OUT (any hit that puts an NPC under 15 % HP has a knockout chance): a NORMAL NPC (not essential,
//       not protected, no creature / ghost) hit by ANYONE to 0 < Health <= fChanceKOHealthBelow (0.15) rolls the formula (helmet 0/20,
//       boots 0/5/10, gloves 0/5/10, body 0/10/20, level attacker - victim -20..+20, this hit's damage 0..10); won = SBKO's hit knock-out
//       (held at her Health, no 25 % pull-down, no FDC roll), lost = the game decides. Punches never roll; ticks / cloaks / hazards /
//       attacker-less damage are no hit; one roll per concentration beam. A normal NPC's BLEEDOUT no longer knocks her out by itself: the
//       roll of the hit that started it decides; a bleedout no rolled hit started rolls once. Essential / protected: unchanged. Damage
//       hook = Actor::HandleHealthDamage on Character (vtable 0x104 SE / AE, 0x106 VR), pre-call, chained (Injury Engine), damage
//       untouched; + vfunc 0x20 scope hooks on 6 value-modifier effect vtables. ini [ChanceKO] (18 keys); one [chanceko] line per roll.
// 2.0.0-u6c (the Sleep spell, phase B): the player's
//   Sleep spell / scroll (ESP 0x805, or any effect with SBKO_KnockOutEffect 0x804) = SBKO's hit knock-out, 100 % on a valid NPC; seen /
//   not seen -> SBKO_SleepFaction rank 2 / 1 for the SkyrimNet wake prompt (needs SBKO_Helper.pex u6c: SetSleepMark). ini [Sleep] (3 keys).
//   Works with the old ESP too (the feature is then off, one line).
// 2.0.0-u6d (the U6b review, fix round 1):
//   F1 a NonHostile weapon (Injury Engine's Torture Padded Whip, Diary of Mine's DOMSlap, any other) never rolls U5 (hit event and hand
//   check) and never knocks out by U6; F2 a weapon enchantment / poison belongs to THIS thread's last swing on her (never an older roll;
//   it brings the swing's roll when it is what takes her under the line); F3 + ValueAndConditionsEffect / GrabActorEffect scopes, a hit
//   event whose source is a spell = a spell; F4 one hit event serves one damage; F7 summoned / commanded actors are never knocked out
//   (U5 / U6 / Sleep); F8 every vtable slot checked before it is written; F10 an acting-hurt NPC makes no more lines / reads while she
//   kneels, and no "rolled at her bleedout" line when no knock-out can follow it then. No ini / Papyrus change.
//   fix round 2 (the Sleep review): children and riders on a mount are never knocked out
//   (U5 / U6 / Sleep); a Sleep knock-out is quiet only when SBKO_SleepFaction exists; a refused static Papyrus call is logged (VmCall);
//   "falls into a deep sleep." only after KnockOutU really knocked her out (sent from its reply); SBKO_Helper.pex u6d: a Sleep episode
//   "wakes up" (NarrateWake / NarrateHealWake read the marker). Deploy the DLL and the pex together (the wording needs the pex; no arity
//   change - either one alone still runs).
// 2.0.0-u6e (nothing that does 0 damage may knock anyone out, so another mod's whip or toy weapon
//   does not count): a TOY weapon - NonHostile, NotUsedInNormalCombat or base damage 0 (the hand-to-hand weapon excepted: bare hands keep
//   their rule) - and a 0-damage hit (HandleHealthDamage delta 0; for U6 PLANCK's HitData totalDamage) never roll U5 and never knock out
//   by U6: the hit event's weapon, its bash, its enchantment / poison, the hands check. One log line per such hit. No ini / Papyrus change.
// 2.0.0-u6f (a play test: a Sleep-spell sleeper could not be woken by a hit or by talking to her, though it is
//   just sleep; a sneak knock-out from behind failed on some NPCs):
//   (1) HELMET RULE A for U6's immunity and U5's helmet factor (one helper): a
//   worn armour on slot 30 / 31 that is Light or Heavy armour - Clothing and off-slot items (a cape carrying ArmorHelmet) never count, DD
//   items skipped; the lines say why a head-ish piece did not count. (2) A SLEEP-SPELL sleeper wakes straight up (the heal wake's path) on
//   any hit by anyone (TESHitEvent; not the spell's own) or the player activating / talking to her (new TESActivateEvent sink) - also on
//   another mod's Wake (DoM's menu); injury knock-outs unchanged. The sleeper mark is co-saved (STAT flags 0x40 / 0x80, RELS 0x08 / 0x10;
//   an older DLL ignores the bits). ini [Sleep] + bSleepWakeOnHit / bSleepWakeOnTalk. No Papyrus change.
// 2.0.0-u6g (wig / hair mods such as KS Hairdos are excluded):
//   ini [HeadHit] sHelmetIgnorePlugins = a comma list of plugin file names; a
//   worn armour DEFINED by one of them (its originating file, GetFile(0)) is never a helmet in rule A (U6's immunity + U5's helmet
//   factor). Default = known wig / hair plugins (KSWigsSMP.esp, Xing ... Dint999.esp, ...). Start line lists it (and any
//   name not loaded); the helmet text says "a listed hair / wig plugin". No other behaviour change. No Papyrus change.
// 2.0.0-u6h (console tests on a selected NPC, the stuck-KO audit fixes, a regeneration
//   rate of 0.01 per sec):
//   (1) [Regen] fEssentialNonCombatHealRateBonus default 0.01 (almost nothing: no
//   healing without a potion or magic). (2) Stuck-KO audit G3: a death event whose task finds her NOT dead any more (cancelled / undone)
//   keeps her held (no ClearMark; a cleared flag re-asserted quietly for 2 checks); G4 (the M1d rule): an orphan knock-out (rank 10 +
//   unconscious, no record) that ARRIVES with a cell attach during play is woken at once (FinishWake rank 0) instead of held for a new
//   6-12 h - after a load in the loaded area she is still re-held. (3) Console tests: `sbko <test>` (three optional words, the 'help'
//   command's kChar type) - help, status, sleep, headko [blunt] [open], roll [ko], hitwake, toy, timer, refill - each runs the game's own
//   task code (OnSleepTask / OnHitTask / OnChanceKOTask dry or applied / OnSleepWakeTask) on the selected NPC, one console line, the
//   detail in the log; `sbkos` = status with no word (4th donor, ToggleWaterJittering). sbkou / sbkot / plain sbko unchanged (creatures
//   now refused). RefillGuard reports its install state. No co-save, ini-key or Papyrus change.
// 2.0.0-u7a (build 2 = U4 + U7 + M2):
//   (U4) a follower SBKO holds after a BLEEDOUT knock-out gets one more Follower Death
//   Chance roll per qualifying direct hit by someone else while the player is in combat (the damage hook - now installed for U5 OR U4 -
//   classifies; SBKO_Helper.FdcHit sends the mod event SBKO_FdcHit from Papyrus; the FDC x SBKO patch re-rolls, death only). (U7) the
//   SBKO_API natives ApiVersion / TakeOver / Holds / ChokeReleased (Api.cpp): VRTouchEvents hands a choked-out NPC to SBKO - SBKO's own choke
//   knock-out (held at her Health, no FDC roll, heal / 6-12 h timer only, SBKO_SleepFaction rank 3 for the prompts); the seconds held are
//   co-saved (record 'CHOK' v1; RELS flag 0x20; STAT's cls byte = 1) for build 3's neck mark. (M2) "woken to talk": the player talking to an
//   NPC asleep in a bed, or Diary of Mine's Wake (TESPerkEntryRunEvent, defined here), -> SkyrimNet is told once (SBKO_Helper.NarrateWoken);
//   probe lines for the two UNVERIFIED VR layouts. ini [FDC] / [TakeOver] / [Woken]. Console: sbko fdc / fdchit / takeover / holds / release
//   / woken / sleepstate. Needs SBKO_Helper.pex u7a + SBKO_API.pex; the FDC re-roll needs the u7a FDC patch scripts.
// 2.0.0-m34 (build 3 = M3 / M4 skin marks + the choke neck mark;
//   on top of u7a): Marks.cpp - (M4) every humanoid Injury Engine wounds (the player
//   too, never a child; no face marks on a Khajiit / Argonian head) gets the Collygon mark of her worst wound per body part from marks.json, on
//   while IE lists the wound, off when IE drops it (GetStateJson, read at IE's ping + the hit / heal that named her, her Health moving a whole
//   percent, a game hour, her 3D loading, a load; HasOpenWounds first in a sweep); alphas on the DDAB ladder 0.50 / 0.75 / 1.00, floor 0.30;
//   burns / frost / shock unmarked. (M3) SBKO_Marks.psc paints with RaceMenu's NiOverride: AddOverlays only when she has none + Wait 0.5, NO
//   QueueNiNodeUpdate, never another mod's slot (Face 10-12, Body 3-5, Hands 2, Feet 2). (choke) Collygon "Choked Two Hands" in Face 16 by the
//   seconds held (< 20 s 0.50, 20-30 s 0.75, >= 30 s 1.00), 12 game h per step, fading by thirds ("Faded" in the last), each heal halves what
//   is left; a choke let go before the passout = the 0.30 floor with a shorter life (bChokeMarkBeforePassout). Co-save record 'MARK' v1 (the
//   mark keeps its own seconds / start / step - independent of the knock-out entry). ini [Marks]; console 'sbko marks ...'; a [PERF] marks line.
//   Needs SBKO_Marks.pex + SKSE/Plugins/SimpleBleedoutKO/marks.json; new vcpkg dependency nlohmann-json (header-only).
// 2.0.0-m34b (the u7a review's fix round; marks untouched): (1) U4 and U5 remember
//   EVERY concentration beam counted on a victim (a set, not one slot) - two beams at once no longer roll every frame; (3) no HasMagicEffect in
//   the U4 task (the console keeps it) and no IsHostileToActor in the M2 task (SBKO_Helper.NarrateWoken refuses a hostile NPC in Papyrus);
//   (4) the "[fdc] no re-roll" line only for followers; (8) bLogWokenProbe defaults OFF in code (the ini can turn it on for a first run).
//   The FDC patch gets bDeathDone (one death per effect) and unregisters SBKO_FdcHit in OnEffectFinish.
// 2.0.0-m34c (the marks review, fix round 1):
//   (1) the worst wound per part is ranked only among the wounds that PAINT something there (a hair pull no longer hides a slap); (2) a marks
//   record must still be HER - sex compared at every sight / read (a regenerated actor = the record forgotten), a created (FF) id's base NPC
//   kept and compared (a reused id = SBKO's marks cleared from its own slots, record dropped), an FF record dropped as soon as its actor is gone;
//   co-save 'MARK' v2 (+ base; v1 read); (3) SBKO re-looks / clears ONLY the slot it wrote (the slot index travels with every op; SBKO_Marks.pex
//   m34c) - a slot holding the same Collygon file but painted by SlaveTats / Combat Wound is never adopted or cleared; (4) no choke mark and no
//   wrist / ankle bands on Khajiit / Argonian (bBeastFaceMarks 0); (5) one choke mark per neck (a mark being cleared is adopted; replies booked
//   on the exact mark by a uid) - no Face 16 clear + repaint loop; (6) a detach keeps the painter's in-flight flag until its reply (always
//   booked) - never two Apply stacks on one actor; (8) one [marks] sweep line only when it found someone; Injury_Engine.dll not loaded / its
//   natives silent -> one line, the IE mirror off for the session; (9) SBKO_Marks.pex missing -> one line, the hit list emptied and no longer
//   filled; the ini slot bands checked together at load (in order, <= 127, the choke slot outside the face band; else all back to defaults).
// 2.0.0-m34d (a performance fix): the marks sweep (an IE ping
//   no hit / heal explains - ~40 pings a second while wounds exist) asks Injury Engine's HasOpenWounds for ALL its candidates in ONE Papyrus
//   call (SBKO_Marks.IEWounded, an Int[] in, the same array back with 0 for no open wound) instead of one call per loaded humanoid (~26x fewer
//   dispatches); the yes ones are read in full as before. Same behaviour, no new timer / cap. [PERF] marks counts "sweep calls" next to the
//   asks. Needs SBKO_Marks.pex m34d (an older .pex -> one warn line and the per-actor calls for the session). No co-save / ini change.
// 2.0.0 release candidate rc200:
//   the m34d code with the SKSE author "Telord" and a plain "SimpleBleedoutKO 2.0.0" banner (the build tag "rc200 (m34d)" on the runtime
//   line), the built-in defaults = the public ini (Settings.h), the source paths trimmed from the DLL (CMakeLists.txt). Log texts only
//   otherwise; no behaviour change.
// NOTE: the v10 mod's Papyrus user log is `Logs/Script/User/SimpleBleedoutKO.N.log` - a different folder from this
// DLL's `SKSE/SimpleBleedoutKO.log`, no clash.

namespace
{
    constexpr REL::Version kVersion{ 2, 0, 0 };

    // "SE 1.5.97.0" / "AE 1.6.1170.0" / "VR 1.4.15.0" - the runtime this DLL was loaded into (REL::Module).
    std::string RuntimeName()
    {
        const char* rt = REL::Module::IsVR() ? "VR" : (REL::Module::IsAE() ? "AE" : "SE");
        return std::format("{} {}", rt, REL::Module::get().version().string("."));
    }

    std::string Banner()
    {
        return std::format("SimpleBleedoutKO 2.0.0 ({}, CommonLib MinLL 4.39.5 MIT) - knock-outs instead of bleedout: guarded unconscious, "
                           "heal / timer wake, combat-end heal guard (VR), bash / mace knock-out, knock-out roll for ordinary NPCs, the Sleep "
                           "spell, woken-to-talk narration, Injury Engine skin marks, SBKO_API; console 'sbko help' / 'sbkos' / 'sbkou' / 'sbkot'",
                           RuntimeName());
    }

    // <Documents>/My Games/Skyrim VR/SKSE/SimpleBleedoutKO.log - the sink truncates, so the previous session's log is
    // first renamed to SimpleBleedoutKO.log0 (a copy if the rename fails; the game goes on if both fail). Returns the
    // line that says which happened (PPB main.cpp InitLogging pattern).
    std::string InitLogging()
    {
        auto path = SKSE::log::log_directory();
        if (!path) return {};
        *path /= "SimpleBleedoutKO.log";

        std::string kept;
        {
            std::error_code             ec;
            const std::filesystem::path log0 = path->parent_path() / "SimpleBleedoutKO.log0";
            if (!std::filesystem::exists(*path, ec))
                kept = ec ? std::format("previous SimpleBleedoutKO.log NOT kept as .log0: it could not be looked at ({})", ec.message())
                          : std::string("previous SimpleBleedoutKO.log NOT kept as .log0: there was none");
            else {
                const auto        bytes = std::filesystem::file_size(*path, ec);
                const std::string size  = ec ? std::string("size not read") : std::format("{} bytes", bytes);
                std::error_code   ecRename;
                std::filesystem::rename(*path, log0, ecRename);
                if (!ecRename)
                    kept = std::format("previous SimpleBleedoutKO.log kept as SimpleBleedoutKO.log0 ({})", size);
                else {
                    std::error_code ecCopy;
                    std::filesystem::copy_file(*path, log0, std::filesystem::copy_options::overwrite_existing, ecCopy);
                    kept = !ecCopy ? std::format("previous SimpleBleedoutKO.log kept as SimpleBleedoutKO.log0 ({}) - by a copy: the "
                                                 "rename failed ({})", size, ecRename.message())
                                   : std::format("previous SimpleBleedoutKO.log NOT kept as .log0: the rename failed ({}) and the "
                                                 "copy failed ({})", ecRename.message(), ecCopy.message());
                }
            }
        }

        // u2c fix round 1: a 256 KB stdio buffer instead of the CRT's ~4 KB, set right after fopen and before
        // any write (spdlog 1.16 file_event_handlers.after_open, details/file_helper-inl.h:50-52) - a burst of lines no longer runs
        // a synchronous WriteFile on the writer's (game-side) thread; the disk write happens at the flushes (warn+, SBKO's check
        // thread, a save).
        spdlog::file_event_handlers handlers;
        handlers.after_open = [](const spdlog::filename_t&, std::FILE* a_file) {
            if (a_file) std::setvbuf(a_file, nullptr, _IOFBF, 256 * 1024);
        };
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true, handlers);
        // u2c P1: a second sink that only counts the lines (the PERF line's "log lines")
        auto counter = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [](const spdlog::details::log_msg&) { SBKO::Knockout::g_logLines.fetch_add(1, std::memory_order_relaxed); });
        auto log = std::make_shared<spdlog::logger>("SimpleBleedoutKO", spdlog::sinks_init_list{ sink, counter });
        log->set_level(spdlog::level::info);
        // u2c P2: NOT a flush per line (the log folder may be cloud-synced; u2b flushed every info line). Warnings and errors flush
        // at once; everything else is flushed by SBKO's check thread (its own thread, never the game's) at its next wake - every
        // fCheckSeconds - when lines were written, and at each save (Knockout.cpp StartCheckThread / OnSave).
        log->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
        return kept;
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) return;
        switch (a_msg->type) {
            case SKSE::MessagingInterface::kDataLoaded:
                logger::info("kDataLoaded - registering the console command, reading the ini, starting the knockout");
                SBKO::TestCmd::Register();
                SBKO::Settings::Load();
                SBKO::RefillGuard::Install();   // u2e: after the ini (iCombatEndRefill); once - kDataLoaded comes once
                SBKO::Knockout::AcquirePlanck();   // u6 fix 1 (F5): after the ini (bHeadHit), before the hit sink is registered
                SBKO::Marks::Install();            // m34: after the ini, BEFORE Knockout registers its sinks (they ask Marks::On())
                SBKO::Knockout::OnDataLoaded();
                break;
            case SKSE::MessagingInterface::kPreLoadGame:
                SBKO::RefillGuard::Reset();     // u2e: calls still running die with the old game
                SBKO::Knockout::OnPreLoadGame();
                SBKO::Marks::OnPreLoadGame();   // m34
                logger::info("kPreLoadGame - knockout checks stopped until the game is loaded");
                break;
            case SKSE::MessagingInterface::kNewGame:
            case SKSE::MessagingInterface::kPostLoadGame:
                // A "script absent" answer cached during a load storm must not outlive the load; this is also what
                // lets SBKO_Test.pex / SBKO_Helper.pex installed between loads be found without a restart.
                VmCall::ClearCache();
                SBKO::Helper::ClearCache();   // M1c: the SkyrimNetApi answer is asked again after every load
                SBKO::RefillGuard::Reset();   // u2e: also here - a call dispatched during the load died with it, and a
                                              // new game from the main menu has no kPreLoadGame
                logger::info("{} - Papyrus presence cache cleared",
                             a_msg->type == SKSE::MessagingInterface::kNewGame ? "kNewGame" : "kPostLoadGame");
                SBKO::Knockout::OnGameReady(a_msg->type == SKSE::MessagingInterface::kPostLoadGame);
                SBKO::Marks::OnGameReady(a_msg->type == SKSE::MessagingInterface::kPostLoadGame);   // m34: the reconcile 5 s after a load
                break;
            default:
                break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    const std::string kept = InitLogging();
    // u2d (found in a u2c play audit): SKSE::Init's default a_log = true runs CommonLib's log::init() (API.cpp:97-99), which made
    // ITS logger the default on the same file - SBKO's 256 KB buffer, flush_on(warn) and the line counter were never live. false =
    // CommonLib leaves SBKO's logger alone.
    SKSE::Init(a_skse, false);

    logger::info("==================================================");
    logger::info("{}", Banner());
    logger::info("runtime {} {} - build rc200 (m34d)",REL::Module::IsVR() ? "VR" : (REL::Module::IsAE() ? "AE" : "SE"),
                 REL::Module::get().version().string());
    if (!kept.empty()) logger::info("{}", kept);
    logger::info("==================================================");
    // u2d: the proof that SBKO's own logger writes this file - its counting sink saw the lines above (0 would mean another logger
    // replaced it, as CommonLib's did in u2c)
    logger::info("log: SBKO's own logger '{}' writes this file - {} line(s) counted so far by its counting sink (256 KB buffer, flush on "
                 "warnings, at saves and by the check thread)",
                 spdlog::default_logger() ? spdlog::default_logger()->name() : std::string("(none)"),
                 SBKO::Knockout::g_logLines.load(std::memory_order_relaxed));

    // u7a (U7): the SBKO_API natives (ApiVersion / TakeOver / Holds / ChokeReleased) - VRTouchEvents' gate script calls them
    if (auto* papyrus = SKSE::GetPapyrusInterface(); !papyrus || !papyrus->Register(SBKO::Api::RegisterPapyrus))
        logger::error("SKSE Papyrus interface: SBKO_API natives NOT registered - VRTouchEvents keeps its own choke knock-out");

    if (auto* msg = SKSE::GetMessagingInterface(); msg && msg->RegisterListener(OnMessage))
        logger::info("SKSE messaging listener registered (kDataLoaded / kPreLoadGame / kNewGame / kPostLoadGame)");
    else
        logger::error("SKSE messaging listener NOT registered - no knockout and no 'sbko' command this session");

    // The knockout state (who is down / coming to, her saved HealRate, the timer) lives in the SKSE co-save, record
    // 'STAT' v1 under the unique ID 'SBKO' (DDAB Base main.cpp:696-701 pattern).
    if (auto* ser = SKSE::GetSerializationInterface()) {
        ser->SetUniqueID(SBKO::Knockout::kSerializationID);
        ser->SetSaveCallback(SBKO::Knockout::OnSave);
        ser->SetLoadCallback(SBKO::Knockout::OnLoad);
        ser->SetRevertCallback(SBKO::Knockout::OnRevert);
        logger::info("SKSE co-save registered (unique ID 'SBKO', record 'STAT' v4 - u6: flags bit 0x20 = a hit knock-out; u7a: cls 1 = a choke, "
                     "+ record 'CHOK' v1 = the choke seconds; m34c: + record 'MARK' v2 = the skin marks; v1 / v2 / v3 are still read)");
    } else {
        logger::error("no SKSE serialization interface - knockouts are NOT saved with the game");
    }
    return true;
}

// The toolbox's own declaration macro (the same one its add_commonlibsse_plugin() helper generates - Injury Engine's
// route). It exports SKSEPlugin_Version AND SKSEPlugin_Query (the SE/VR loaders). AddressLibrary sets
// versionIndependence AddressLibraryPostAE, and on this fork PluginDeclaration also raises versionIndependenceEx
// AddressLibraryV5 (SKSE 2.3.0 on AE 1.7.99); StructCompatibility::Independent = versionIndependenceEx NoStructUse.
SKSEPluginInfo(
    .Version              = kVersion,
    .Name                 = "SimpleBleedoutKO",
    .Author               = "Telord",
    .StructCompatibility  = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)
