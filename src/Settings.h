#pragma once

// SimpleBleedoutKO.ini - the player-tunable numbers of the knockout (M1). Every default = v10's constant
// (SBKO_DownWatcherScript.psc:23-32). The two SkyrimNet ranks (10 = knocked out, 3 = first coming-to rank) are the
// prompt contract: constants in Knockout.cpp, never ini keys.
//
// Files: Data/SKSE/Plugins/SimpleBleedoutKO.ini (the advanced options), then Data/SKSE/Plugins/SBKO_Setting.ini (2.1.0: the
// everyday options under friendlier names - see Settings.cpp kUserKeys; a value there wins). Read ONCE at kDataLoaded, before the
// check thread starts; read-only afterwards, so no lock is needed to read it.
namespace SBKO::Settings
{
    struct Values
    {
        // [WhoGetsKnockedOut] (2.1.0) - the AUTOMATIC knock-outs (dropping in a fight): who SBKO knocks out at bleedout. Switched
        // off = that group keeps the game's own bleedout. An NPC SBKO already holds is never affected. The player's own deliberate
        // knock-outs have their own switches (bHeadHitFollowers, bSleepFollowers, ...).
        bool  koFollowers        = true;    // bKnockOutFollowers  - followers (player teammates / CurrentFollowerFaction)
        bool  koProtected        = true;    // bKnockOutProtected  - other protected / essential NPCs
        bool  koNormal           = true;    // bKnockOutNormalNPCs - ordinary NPCs (the knock-out roll); 0 = also bChanceKO 0
        bool  lootKnockedOut     = true;    // bLootKnockedOut     - 2.1.0: the player activating, WHILE SNEAKING, an NPC SBKO holds knocked
                                            //   out opens her inventory - free looting, never a crime (OpenInventory(true)); other mods'
                                            //   interaction menus first; the menu closes if she wakes / dies. A standing click is unchanged
        // [Knockout]
        float wakeHealthFraction = 0.25f;   // fWakeHealthFraction - u2d: her Health at EVERY wake is raised to at least this
                                            //   fraction of max (perm + temp), never lowered (wake up at 25 % HP); an old M1
                                            //   DOWN knock-out also wakes by heal once she reaches it (v10 WAKE_THRESHOLD)
        int   graceChecks        = 2;       // iGraceChecks        - first N down-checks never heal-wake (v10 GRACE_TICKS)
        float recoverMinHours    = 6.0f;    // fRecoverMinHours    - timer-fallback low, game hours (v10 RECOVER_MIN_H)
        float recoverMaxHours    = 12.0f;   // fRecoverMaxHours    - timer-fallback high, game hours (v10 RECOVER_MAX_H)
        float checkSeconds       = 1.5f;    // fCheckSeconds       - check period, real seconds (v10 POLL_SECS)
        float stageSeconds       = 60.0f;   // fStageSeconds       - seconds of play per coming-to rank (v10 STAGE_SECS)
        float bleedoutGraceSeconds = 0.0f;  // fBleedoutGraceSeconds - seconds in the engine's own bleedout before SBKO knocks her out.
                                            //   u2d had a 1.5 s delay; u2e default 0 = at once (no more
                                            //   1.5 s bleedout first). A sequencing choice, not a limit
        bool  notify             = true;    // bNotify             - the two HUD lines (v10 NOTIFY)
        // [Unconscious] (U1 - the guarded unconscious)
        float unconsciousHealthFraction = 0.25f;   // fUnconsciousHealthFraction - after the settle, Health is held at
                                                   // min(this x max, her Health at the settle); a rise is put back
        bool  severActionsGuard  = true;    // bSeverActionsGuard  - a knocked-out FOLLOWER is excluded from SeverActions survival
        bool  potionsOff         = true;    // bPotionsOff         - Smart NPC Potions' three abilities off while she is out
        bool  repairWalkingGraph = true;    // bRepairWalkingGraph - footsteps from a ragdolled NPC -> one re-shove per check
        // [HealWake] (U2a; U2b: no bleedout window - a heal wakes her straight up)
        bool  healWake           = true;    // bHealWake           - the first HEAL on an unconscious NPC wakes her (FinishWake)
        // [Regen] (u2d: a regen of 0.1 for all protected and essential NPCs; u6h: 0.01 per sec is supported - at low level NPCs
        // otherwise heal faster, which makes no sense; 0.01 is almost nothing: no healing without a potion or magic)
        float essentialRegenBonus = 0.1f;   // fEssentialNonCombatHealRateBonus - written into the game setting of that name at data
                                            //   load and after every load (engine default 5.0 HP/s out of combat); 0.01 = almost
                                            //   nothing: no healing without a potion or magic; -1 = not touched.
                                            //   rc200: the built-in default = the public ini's 0.1
        float essentialLethalHitHealth = 0.01f;   // fEssentialHealthPercentReGain - u2f (1 %): written into the game setting
                                                  //   of that name at data load and after every load (engine 0.3): the Health fraction an
                                                  //   unconscious / restrained essential or protected NPC is set to when a lethal blow
                                                  //   lands (the engine's only use of it); -1 = not touched
        int   combatEndRefill    = 2;       // iCombatEndRefill - u2e: the engine's FULL heal when an essential /
                                            //   protected NPC leaves combat (Actor::StopCombat) is skipped for 2 = every such NPC,
                                            //   1 = followers (player teammates) only, 0 = nobody (the engine as usual). In every mode it
                                            //   is skipped inside SBKO's own knock-out / wake / let-go calls (RefillGuard). VR 1.4.15,
                                            //   SE / AE since 2.1.0. SBKO_Setting.ini name: iBlockCombatEndHeal
        // [HeadHit] (U6 - rules R1-R8 =
        // the Knockout mod's rules + a blunt weapon to the head). The player's hit knocks an NPC out with SBKO's own knock-out. No roll,
        // no timer, no cap, no cooldown: every switch below only turns a rule on or off.
        bool  headHit            = true;    // bHeadHit            - the master switch (0 = the TESHitEvent sink is not even registered)
        // (u6 fix round 2, the punch mechanic removed: bHeadHitFists is GONE - a punch never knocks out; an old ini's key is
        //  logged as unknown and does nothing)
        bool  headHitBash        = true;    // bHeadHitBash        - a bash (shield, weapon pommel, bow, torch: the event's bash bit)
                                            //                       counts, anywhere (R2)
        bool  headHitBlunt       = true;    // bHeadHitBlunt       - a mace / warhammer: with PLANCK loaded ONLY on the head (PLANCK's
                                            //                       hit node NPC Head [Head], R3); u6b (the way [Knockout] does
                                            //                       it): WITHOUT PLANCK (SE / AE always, VR without it) ANYWHERE
        bool  headHitSneak       = true;    // bHeadHitSneak       - trigger: the game's sneak-attack bit OR she does not detect the
                                            //                       player (Papyrus IsDetectedBy's own test) (R4)
        bool  headHitLowHealth   = true;    // bHeadHitLowHealth   - trigger: her Health <= fHeadHitHealthThreshold (R4)
        float headHitHealthThreshold = 0.10f;   // fHeadHitHealthThreshold - Health fraction (current / max) for that trigger
        bool  headHitHelmetProtects  = true;    // bHeadHitHelmetProtects  - a worn HELMET = immune (R5). u6f rule A: a worn armour on
                                                //   biped slot 30 (head) or 31 (hair) that is Light or Heavy armour; Clothing (hoods,
                                                //   circlets, cloth wigs, capes) and items off slots 30 / 31 never count, whatever keyword
                                                //   they carry; Devious Devices items are skipped
        // sHelmetIgnorePlugins (u6g: wig / hair mods such as KS Hairdos) - comma-separated plugin file
        //   names (case-insensitive, spaces around commas trimmed): a worn armour DEFINED by one of them (its originating file, not an
        //   override) is never a helmet - U6's immunity AND U5's helmet factor. Empty = no plugin ignored. Default = the wig / hair
        //   plugins of a large test load order whose wigs rule A counted as helmets.
        //   rc200: the built-in list = the public ini
        std::vector<std::string> helmetIgnorePlugins = {
            "KSWigsSMP.esp",            "Xing SMP Hairs and Wigs - Dint999.esp", "[ELLE] Roca.esp",
            "[ELLE] Warrior.esp",       "[ELLE] Knight Elaine.esp",              "[ELLE] SK Shinobi.esp", "DreadGirl.esp",
            "Skyrim On Skooma.esp",
        };
        bool  headHitFollowers   = true;    // bHeadHitFollowers   - followers can be knocked out by the player's hit too
        bool  logHeadHits        = true;    // bLogHeadHits        - one line per player bash / blunt hit on an actor (values +
                                            //                       verdict); the knock-out itself is always logged
        // [ChanceKO] (U5 - any hit that puts an NPC under 15 % HP has a knockout chance, by an armour /
        // level / damage formula). A NORMAL NPC (not essential, not protected) hit by anyone to 0 < Health <= fChanceKOHealthBelow
        // rolls; won = SBKO's knock-out, lost = the game decides (she may kneel in bleedout or fight on). Her bleedout no longer
        // knocks her out by itself: the roll decides (a bleedout no rolled hit started rolls once at the bleedout).
        bool  chanceKO           = true;    // bChanceKO           - the master switch (0 = no damage hook; every NPC that bleeds out
                                            //                       is knocked out, as before)
        float chanceKOHealthBelow = 0.15f;  // fChanceKOHealthBelow - Health fraction (current / max) at or under which a hit rolls
        bool  chanceKOCreatureHits = true;  // bChanceKOCreatureHits - a creature's claw / bite (its "unarmed" attack) rolls like a
                                            //                       weapon; an NPC's (or your) bare-hand punch never rolls
        bool  logChanceKO        = true;    // bLogChanceKO        - one line per roll (each factor, the chance, the roll, the result)
                                            //                       and per hit that reached the roll but was ruled out
        // the formula: helmet + boots + gloves + body + level + damage, clamped to 0..100 %
        float chanceKOHelmetOn    = 0.0f;   // fChanceKOHelmetOn    - a worn helmet (u6f rule A, the same test as [HeadHit])
        float chanceKOHelmetOff   = 20.0f;  // fChanceKOHelmetOff   - no helmet
        float chanceKOFeetHeavy   = 0.0f;   // fChanceKOFeetHeavy
        float chanceKOFeetLight   = 5.0f;   // fChanceKOFeetLight
        float chanceKOFeetNone    = 10.0f;  // fChanceKOFeetNone    - no boots, or cloth shoes
        float chanceKOHandsHeavy  = 0.0f;   // fChanceKOHandsHeavy
        float chanceKOHandsLight  = 5.0f;   // fChanceKOHandsLight
        float chanceKOHandsNone   = 10.0f;  // fChanceKOHandsNone   - no gloves, or cloth gloves
        float chanceKOBodyHeavy   = 0.0f;   // fChanceKOBodyHeavy
        float chanceKOBodyLight   = 10.0f;  // fChanceKOBodyLight
        float chanceKOBodyClothes = 20.0f;  // fChanceKOBodyClothes - clothes, a robe, or nothing
        float chanceKOPointsPerLevel = 1.0f;   // fChanceKOPointsPerLevel - per level the attacker is ABOVE her (below = minus)
        float chanceKOLevelMax       = 20.0f;  // fChanceKOLevelMax       - the level term stays within -this .. +this
        float chanceKODamageMax      = 10.0f;  // fChanceKODamageMax      - the damage term for a hit of 100 % of her max (scaled)
        // [Sleep] (u6c - a spell: it works 100 % if it hits): the Sleep spell (SimpleBleedoutKO_001.esp MGEF 0x805, or
        // any effect carrying SBKO_KnockOutEffect 0x804) cast by the PLAYER knocks a valid NPC out with SBKO's own hit knock-out - no
        // roll, no detection rule, no Health rule, no helmet test. Whether she saw the caster only picks the wake prompt (SBKO_SleepFaction
        // rank 2 / 1). No timer, cap or cooldown: each switch only turns a rule on or off. Without the ESP records the feature is off.
        bool  sleep              = true;    // bSleep              - the master switch (0 = the spell does nothing)
        bool  sleepFollowers     = true;    // bSleepFollowers     - followers can be put to sleep too
        bool  logSleep           = true;    // bLogSleep           - one [sleep] line per Sleep effect from the player on an actor (the
                                            //                       knock-out itself is always logged)
        // u6f (a sleeper must wake from a hit or from being talked to - it is just sleep):
        // ONLY a Sleep-spell knock-out (SBKO_SleepFaction rank >= 1) - injury knock-outs still need a heal
        bool  sleepWakeOnHit     = true;    // bSleepWakeOnHit     - any hit on her by anyone (weapon, bash, arrow, spell, a punch, a
                                            //                       0-damage hit) wakes her straight up (the heal wake's path)
        bool  sleepWakeOnTalk    = true;    // bSleepWakeOnTalk    - the player activating (talking to) her wakes her. (Another mod's
                                            //                       "Wake" - Diary of Mine's menu clears her flag - wakes a sleeper
                                            //                       whatever this says)
        // [FDC] (u7a U4 - each hit is a re-roll for FDC to kill that NPC: only OTHER people's
        // hits, a miss = nothing, a protected / essential non-follower never dies while knocked out). A follower SBKO holds after a
        // BLEEDOUT knock-out (FDC already rolled once - FDC_SBKO_RolledFaction rank 1) gets one more Follower Death Chance roll per
        // qualifying direct hit while she is out and the player is in combat (FDC's own gate). Hit / Sleep / choke knock-outs never roll.
        // Needs FDC_SBKO_Patch.esp + the u7a FDC patch scripts (INJU_Version3 listens for the mod event SBKO_FdcHit).
        bool  fdcRerollOnHit     = true;    // bFdcRerollOnHit     - the master switch (0 = no re-roll on a hit)
        bool  fdcRerollPlayerHits = false;  // bFdcRerollPlayerHits - the PLAYER's own hits never re-roll (1 = they do)
        bool  fdcRerollDeathOnly = true;    // bFdcRerollDeathOnly - a re-roll that misses does nothing (0 = FDC's full roll again:
                                            //                       the small injury each time)
        bool  logFdcReroll       = true;    // bLogFdcReroll       - one [fdc] line per hit on a watched follower (asked / why not)
        // [TakeOver] (u7a U7 - the choke knock-out passes to SBKO when SBKO is present and stays in VRTE when SBKO
        // is not there; SBKO's rules apply once SBKO takes over): the SBKO_API natives (ApiVersion /
        // TakeOver / Holds). A CHOKED-out NPC handed over by VRTouchEvents is SBKO's own choke knock-out: held at her Health (no FDC
        // roll), wakes only on a real heal or SBKO's 6-12 game-hour timer, SBKO_SleepFaction rank 3 = "choked" for SkyrimNet's prompts.
        bool  takeOver           = true;    // bTakeOver           - 0 = TakeOver always answers False (VRTE keeps its own knock-out)
        bool  logTakeOver        = true;    // bLogTakeOver        - one [takeover] line per call (accepted / declined / already SBKO's)
        // [Woken] (u7a M2 - letting SkyrimNet know "(Player's name) just woke you to talk to you" gives
        // better roleplay): the player wakes a sleeper (talks to an NPC asleep in a bed, or picks Diary of Mine's Wake) -> SkyrimNet is
        // told once, she answers the player. A Sleep-spell sleeper woken by DoM's Wake gets "woken from a deep sleep" by the player.
        bool  wokenNarration     = true;    // bWokenNarration     - the master switch
        bool  wokenOnTalk        = true;    // bWokenOnTalk        - the player activating (talking to) an NPC asleep in a bed
        bool  wokenOnDomWake     = true;    // bWokenOnDomWake     - Diary of Mine's menu Wake (its perk entry; needs DiaryOfMine.esm)
        int   wokenTalkMode      = 1;       // iWokenTalkMode      - the talk path: 1 = she answers you (DirectNarration), 0 = she only
                                            //                       knows it (RegisterPersistentEvent - if the vanilla greeting doubles up)
        bool  logWoken           = true;    // bLogWoken           - one [woken] line per trigger / outcome
        bool  logWokenProbe      = false;   // bLogWokenProbe      - m34b: OFF unless the ini says 1 (a debug aid, for the
                                            //                       first VR run): one [probe] line per player activation of an NPC and per
                                            //                       perk-entry event (the two UNVERIFIED VR event layouts); then 0
        // [Marks] (m34 build 3 - everyone: Injury Engine puts an injury, SBKO puts a mark on it; Injury Engine removes an injury, SBKO
        // removes the mark. + the choke hand mark, by how long the choke was held, healed with time or a healing spell, like the skin
        // mark of shackles in DDAB). Collygon marks painted with
        // RaceMenu's NiOverride (SBKO_Marks.psc) on every humanoid Injury Engine wounds (the player too; not children). No cap, no timer:
        // the reads ride the existing check (fCheckSeconds) and IE's own ping; the slot bands are where SBKO may paint (never another mod's slot).
        bool  marks              = true;    // bMarks              - the master switch (0 = no mark is read, painted or kept)
        bool  markPlayer         = true;    // bMarkPlayer         - the player gets IE's marks too (everyone)
        bool  beastFaceMarks     = false;   // bBeastFaceMarks     - Khajiit / Argonian (IsBeastRace) get the human-UV marks too: IE face marks,
                                            //                       the choke mark, the wrist / ankle bands (0: body marks only - Collygon's
                                            //                       face art and SBKO's bands are drawn for human UVs; m34c)
        bool  wristAnkleBands    = true;    // bWristAnkleBands    - SBKO's own wrist / ankle bands (Hands / Feet) for pulling / falling wounds
        bool  faceLayers         = true;    // bFaceLayers         - the face's 2nd / 3rd layers (an eye, blood) at medium / severe
        bool  standIns           = false;   // bStandIns           - burns / frost painted as dark / violet bruises (default: unmarked)
        int   faceFirst          = 10;      // iFaceFirst / iFaceLast   - Face [Ovl10] - [Ovl12]: IE face marks
        int   faceLast           = 12;
        int   bodyFirst          = 3;       // iBodyFirst / iBodyLast   - Body [Ovl3] - [Ovl5]: one slot per wounded body part, the worst first
        int   bodyLast           = 5;
        int   handsSlot          = 2;       // iHandsSlot          - Hands [Ovl2]: the wrist band
        int   feetSlot           = 2;       // iFeetSlot           - Feet [Ovl2]: the ankle band
        int   chokeSlot          = 16;      // iChokeSlot          - Face [Ovl16]: the choke hand mark (its own slot)
        float stageAlphas[3]     = { 0.50f, 0.75f, 1.00f };   // sStageAlphas - light, medium, severe (the DDAB ladder)
        float alphaFloor         = 0.30f;   // fAlphaFloor         - the lowest alpha ever painted (the "barely used" step)
        float alphaScale         = 1.0f;    // fAlphaScale         - one "stronger / softer" knob over every alpha (the result stays 0.30..1.0)
        bool  chokeMark          = false;   // bChokeMark          - the choke hand mark (Face 16). rc200: off by default (= the
                                            //                       public ini; VRTouchEvents paints its own choke neck marks)
        float chokeSteps[2]      = { 20.0f, 30.0f };   // sChokeSteps - seconds held: < 20 -> step 1 (0.50), 20-30 -> step 2 (0.75), >= 30 -> 3 (1.00)
        float chokeHoursPerStep  = 12.0f;   // fChokeHoursPerStep  - its life: 12 game hours per step (DDAB's 12 h per phase)
        float chokeHealShare     = 0.5f;    // fChokeHealShare     - each healing spell / potion takes this share of the life that is LEFT
        bool  chokeMarkBeforePassout = false;  // bChokeMarkBeforePassout - a choke let go BEFORE she passed out: a lighter mark (the 0.30 floor,
                                               //   life = fChokeHoursPerStep x seconds / fChokePassoutSeconds); 0 = no mark. rc200: 0 = the public ini
        float chokePassoutSeconds = 15.0f;  // fChokePassoutSeconds - VRTE's passout (15 s until they pass out): a loose release
                                            //   at or above it counts as a passout choke (the ladder)
        std::string marksFile    = "SKSE/Plugins/SimpleBleedoutKO/marks.json";   // sMarksFile - the mark table (relative to Data/)
        bool  logMarks           = true;    // bLogMarks           - one [marks] line per change (paint / fade / clear / gap / skip / choke)
        bool  perfMarks          = false;   // bPerfMarks          - one "[PERF] marks:" line per minute of play (a report, never a limit).
                                            //                       rc200: off by default (= the public ini)
        // [Log]
        bool  verboseChecks      = false;   // bVerboseChecks      - one log line per check of every held NPC (u2c: OFF by default;
                                            //   the log then carries state changes, anomalies and the PERF line)
        bool  logEffects         = true;    // bLogEffects         - every magic effect that lands on a held NPC (log only)
        bool  logFootsteps       = true;    // bLogFootsteps       - FootLeft/FootRight of held NPCs counted per check
    };

    [[nodiscard]] const Values& Get() noexcept;

    // Parse SimpleBleedoutKO.ini, then SBKO_Setting.ini (missing file / key -> the default), log every value read and the
    // effective set with the file each value came from.
    void Load();
}
