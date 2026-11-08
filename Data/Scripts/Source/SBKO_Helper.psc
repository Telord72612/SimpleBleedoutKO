Scriptname SBKO_Helper Hidden
{SimpleBleedoutKO 2.0.0 - the Papyrus "hands" of the SKSE plugin (SimpleBleedoutKO.dll = the brain).
 The DLL watches bleedout and hits, keeps the timers and the state, and calls these globals with an Int FormID.
 No Utility.Wait anywhere: a wait inside the knock-out would let other scripts interleave with it.
 State = the RANK of SBKO_StateFaction (0x803): 10 knocked out, then 3 -> 2 -> 1 -> 0 while she comes to. Always a rank change,
 never RemoveFromFaction (SkyrimNet keeps reading the faction).
 SBKO_SleepFaction (0x80A) rank: 2 = put to sleep by the Sleep spell and she saw the caster, 1 = the Sleep spell, not seen,
 3 = choked unconscious (handed over through SBKO_API), 0 = any other knock-out.}

; The original knock-out (kept for knock-outs loaded from an older save; new knock-outs use KnockOutU).
; Returns the HealRate it saved before zeroing it (the DLL keeps it in the co-save), or -1.0 when the actor is gone or dead.
; If her HealRate already reads 0 while her base value is above 0, returns -2.0 = "restore her BASE HealRate on wake", never 0.
Float Function KnockOut(Int aiFormId, Int aiNotify) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return -1.0
	EndIf
	SetRank(aiFormId, 10)                                     ; SkyrimNet: she is unconscious from here on
	If !akNpc.GetNoBleedoutRecovery()
		akNpc.SetNoBleedoutRecovery(true)                     ; pin her IN bleedout; the engine won't auto-stand her or restore HP
	EndIf
	akNpc.SetNotShowOnStealthMeter(true)
	akNpc.ForceActorValue("Paralysis", 1)                     ; ragdoll the bleedout crawl into a limp ragdoll
	akNpc.PushActorAway(akNpc, 0.001)                         ; the ragdoll trigger (a tiny self-shove = a flop in place)
	UnequipHands(akNpc)                                       ; ragdoll empty-handed; the AI re-equips on wake
	akNpc.StopCombat()
	Float fSaved = akNpc.GetActorValue("HealRate")
	If fSaved <= 0.0 && akNpc.GetBaseActorValue("HealRate") > 0.0
		fSaved = -2.0                                         ; already zeroed: wake must restore the base value, never 0
	EndIf
	akNpc.ForceActorValue("HealRate", 0.0)                    ; zero natural regen so HealRate can't climb her HP and self-wake her
	If aiNotify != 0
		Debug.Notification(akNpc.GetBaseObject().GetName() + " is knocked out.")
	EndIf
	Return fSaved
EndFunction

; Her Health at a wake - raised to AT LEAST afWakeHealth (absolute: the DLL's fWakeHealthFraction x her maximum), never lowered
; (a big heal that already put her higher stays). afWakeHealth <= 0 = the DLL could not read her maximum: the same fraction of her
; base Health is used. Below 0 she is lifted first; she always ends above 0.
Function WakeHealth(Actor akNpc, Float afWakeHealth, Float afWakeFrac) Global
	Float fCur = akNpc.GetActorValue("Health")
	Float fTarget = afWakeHealth
	If fTarget <= 0.0
		fTarget = akNpc.GetBaseActorValue("Health") * afWakeFrac
	EndIf
	If fCur < fTarget
		akNpc.RestoreActorValue("Health", fTarget - fCur)
	EndIf
	Float fNow = akNpc.GetActorValue("Health")
	If fNow <= 0.0                                            ; she must end ABOVE 0 - else 10 % of her (base) maximum
		akNpc.RestoreActorValue("Health", (0.0 - fNow) + akNpc.GetBaseActorValue("Health") * 0.10)
	EndIf
EndFunction

; The original wake (pairs with KnockOut). afSavedHealRate >= 0 = put that back; -2.0 = the actor's base HealRate; anything else =
; leave HealRate alone. Also used to fully restore an NPC the DLL finds marked but has no record of - so an unconscious one is made
; conscious first.
Function WakeUp(Int aiFormId, Float afSavedHealRate, Int aiComingTo, Int aiNotify, Float afWakeHealth, Float afWakeFrac) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	akNpc.SetNoBleedoutRecovery(false)                        ; first - also when she is dead (nothing of SBKO's stays on)
	If akNpc.IsDead()
		SetRank(aiFormId, 0)
		Return
	EndIf
	WakeHealth(akNpc, afWakeHealth, afWakeFrac)               ; at least fWakeHealthFraction of max, never lowered
	If akNpc.IsUnconscious()
		akNpc.SetUnconscious(false)                           ; unconscious off BEFORE Paralysis
	EndIf
	akNpc.ForceActorValue("Paralysis", 0)                     ; un-ragdoll -> stand-up
	If afSavedHealRate >= 0.0
		akNpc.ForceActorValue("HealRate", afSavedHealRate)    ; restore natural regen
	ElseIf afSavedHealRate == -2.0
		akNpc.ForceActorValue("HealRate", akNpc.GetBaseActorValue("HealRate"))
	EndIf
	akNpc.SetNotShowOnStealthMeter(false)
	; no QueueNiNodeUpdate in any wake: rebuilding the 3D of a ragdolled actor can make her vanish or pop
	akNpc.EvaluatePackage()
	SetRank(aiFormId, aiComingTo)                             ; SkyrimNet: awake, but confused at first
	If aiNotify != 0
		Debug.Notification(akNpc.GetBaseObject().GetName() + " has recovered.")
	EndIf
EndFunction

; ============================================================================================================================
; THE GUARDED UNCONSCIOUS
; ============================================================================================================================

; The knock-out. Same return as KnockOut: the HealRate it saved (-2.0 = "her base value on wake"), or -1.0 when gone / dead.
; aiFromBleedout != 0: she is in bleedout now -> SetUnconscious FIRST, then Paralysis, then the shove (on a target already in
; bleedout SetUnconscious must come before the shove, or she snaps upright). aiFromBleedout == 0 (a conscious NPC): Paralysis,
; shove, then SetUnconscious.
; NoBleedoutRecovery is ON while SBKO holds her (it stops the essential regen at the source). Every exit turns it off again:
; FinishWake, LetGo, WakeUp, ClearMark.
; aiNoFdcRoll 1 = a knock-out that must not get a Follower Death Chance roll (a hit / Sleep / choke knock-out, only with
; FDC_SBKO_Patch.esp loaded): that mark is written BEFORE rank 10 (MarkNoFdcRollThenRank10). 0 = rank 10 only.
Float Function KnockOutU(Int aiFormId, Int aiNotify, Int aiFromBleedout, Int aiNoFdcRoll) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return -1.0
	EndIf
	If aiNoFdcRoll != 0
		MarkNoFdcRollThenRank10(akNpc)                        ; no Follower Death Chance roll for this knock-out
	Else
		SetRank(aiFormId, 10)                                 ; SkyrimNet: she is unconscious from here on
	EndIf
	If aiFromBleedout != 0
		akNpc.SetUnconscious(true)
		akNpc.ForceActorValue("Paralysis", 1)
		akNpc.PushActorAway(akNpc, 0.001)                     ; the ragdoll trigger (a tiny self-shove = a flop in place)
	Else
		akNpc.ForceActorValue("Paralysis", 1)
		akNpc.PushActorAway(akNpc, 0.001)
		akNpc.SetUnconscious(true)
	EndIf
	akNpc.SetNoBleedoutRecovery(true)                         ; the regen is off while held - after the unconscious / shove order
	akNpc.SetNotShowOnStealthMeter(true)
	UnequipHands(akNpc)                                       ; ragdoll empty-handed; the AI re-equips on wake
	akNpc.StopCombat()
	Float fSaved = akNpc.GetActorValue("HealRate")
	If fSaved <= 0.0 && akNpc.GetBaseActorValue("HealRate") > 0.0
		fSaved = -2.0                                         ; already zeroed: wake must restore the base value, never 0
	EndIf
	akNpc.ForceActorValue("HealRate", 0.0)                    ; zero natural regen
	If aiNotify != 0
		Debug.Notification(akNpc.GetBaseObject().GetName() + " is knocked out.")
	EndIf
	Return fSaved
EndFunction

; Smart NPC Potions' three drink-potion abilities (0xD62 NPCpotions_Spell, 0x80A NPCpotions_SpellMage, 0x80B NPCpotions_SpellAssassin,
; all Ability). Removes the ones she has and returns the mask (1 base, 2 Mage, 4 Assassin) so the wake can give back exactly those.
; The DLL calls this ONLY when it has seen at least one of the three spells on her (so GetFormFromFile never runs without
; Smart_NPC_Potions.esp loaded).
Int Function PotionsOff(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return 0
	EndIf
	Spell sBase     = Game.GetFormFromFile(0x000D62, "Smart_NPC_Potions.esp") as Spell
	Spell sMage     = Game.GetFormFromFile(0x00080A, "Smart_NPC_Potions.esp") as Spell
	Spell sAssassin = Game.GetFormFromFile(0x00080B, "Smart_NPC_Potions.esp") as Spell
	Int iMask = 0
	If sBase && akNpc.HasSpell(sBase)
		akNpc.RemoveSpell(sBase)
		iMask += 1
	EndIf
	If sMage && akNpc.HasSpell(sMage)
		akNpc.RemoveSpell(sMage)
		iMask += 2
	EndIf
	If sAssassin && akNpc.HasSpell(sAssassin)
		akNpc.RemoveSpell(sAssassin)
		iMask += 4
	EndIf
	Return iMask
EndFunction

; Gives back exactly the abilities in the mask. A mask <= 0 = nothing was taken.
Function PotionsBack(Int aiFormId, Int aiMask) Global
	If aiMask <= 0
		Return
	EndIf
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	GivePotionsBack(akNpc, aiMask)
EndFunction

Function GivePotionsBack(Actor akNpc, Int aiMask) Global
	If akNpc == None || aiMask <= 0
		Return
	EndIf
	Spell sBase     = Game.GetFormFromFile(0x000D62, "Smart_NPC_Potions.esp") as Spell
	Spell sMage     = Game.GetFormFromFile(0x00080A, "Smart_NPC_Potions.esp") as Spell
	Spell sAssassin = Game.GetFormFromFile(0x00080B, "Smart_NPC_Potions.esp") as Spell
	Bool bVerbose = false                                     ; no "learned" line; a named local, never a computed Bool argument
	If sBase && (aiMask % 2) == 1 && !akNpc.HasSpell(sBase)
		akNpc.AddSpell(sBase, bVerbose)
	EndIf
	If sMage && ((aiMask / 2) % 2) == 1 && !akNpc.HasSpell(sMage)
		akNpc.AddSpell(sMage, bVerbose)
	EndIf
	If sAssassin && ((aiMask / 4) % 2) == 1 && !akNpc.HasSpell(sAssassin)
		akNpc.AddSpell(sAssassin, bVerbose)
	EndIf
EndFunction

; The health hold's put-back. afTargetHealth = the ABSOLUTE Health the DLL holds her at (its hold fraction x her maximum, the
; maximum read in C++ as permanent + temporary modifier), so the DLL's test and this damage use one basis. More than the target =
; damaged down to exactly it; less = left alone (SBKO never heals). Computed HERE from the Health she has when it runs, so a hit
; that lands between the DLL's look and this call can never push her below the target.
; Returns her Health afterwards (absolute, for the DLL's log), or -1.0.
Float Function HoldHealthTo(Int aiFormId, Float afTargetHealth) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return -1.0
	EndIf
	Float fCur = akNpc.GetActorValue("Health")
	If afTargetHealth > 0.0 && fCur > afTargetHealth
		akNpc.DamageActorValue("Health", fCur - afTargetHealth)
	EndIf
	Return akNpc.GetActorValue("Health")
EndFunction

; The re-assert: unconscious FIRST (she may be in bleedout), Paralysis 1, then the same tiny self-shove as the knock-out. Also the
; "walking graph" repair: the shove puts her animation graph back into the ragdoll.
Function HoldDown(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return
	EndIf
	akNpc.SetUnconscious(true)
	akNpc.ForceActorValue("Paralysis", 1)
	akNpc.PushActorAway(akNpc, 0.001)
	akNpc.SetNoBleedoutRecovery(true)                         ; kept on while held (after the unconscious / shove order)
EndFunction

; The wake of a knock-out - the timer wake / the heal wake (aiRank 3) and the load safety's restore (aiRank 0). Idempotent: every
; step is safe to run twice. Unconscious off FIRST, then Paralysis 0 (the engine plays the get-up). Her Health is raised to at
; least fWakeHealthFraction of max (WakeHealth, never lowered; she always ends above 0), HealRate back (-2.0 = her base value),
; potions back (the mask), stealth meter, EvaluatePackage, rank. NoBleedoutRecovery is ALWAYS turned off here. No QueueNiNodeUpdate.
Function FinishWake(Int aiFormId, Float afSavedHealRate, Int aiRank, Int aiPotionMask, Int aiNotify, Float afWakeHealth, Float afWakeFrac) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	akNpc.SetNoBleedoutRecovery(false)                        ; first - also when she is dead (nothing of SBKO's stays on)
	If akNpc.IsDead()
		SetRank(aiFormId, 0)
		Return
	EndIf
	WakeHealth(akNpc, afWakeHealth, afWakeFrac)               ; at least fWakeHealthFraction of max, never lowered
	If akNpc.IsUnconscious()
		akNpc.SetUnconscious(false)
	EndIf
	akNpc.ForceActorValue("Paralysis", 0)
	If afSavedHealRate >= 0.0
		akNpc.ForceActorValue("HealRate", afSavedHealRate)
	ElseIf afSavedHealRate == -2.0
		akNpc.ForceActorValue("HealRate", akNpc.GetBaseActorValue("HealRate"))
	EndIf
	GivePotionsBack(akNpc, aiPotionMask)
	akNpc.SetNotShowOnStealthMeter(false)
	akNpc.EvaluatePackage()                                   ; no QueueNiNodeUpdate before it
	SetRank(aiFormId, aiRank)
	If aiNotify != 0
		Debug.Notification(akNpc.GetBaseObject().GetName() + " has recovered.")
	EndIf
EndFunction

; ============================================================================================================================
; Another mod takes her, the re-adopt, the heal-wake line, the "too injured" line
; ============================================================================================================================

; ANOTHER MOD TOOK HER (Diary of Mine's Abduct / Tie-up / Bag, VRTouchEvents' choke knock-out, or she was disabled): SBKO stops
; holding her - no HUD line - and gives back what IT set. NoBleedoutRecovery is ALWAYS turned off (flag bit 1 is still sent but no
; longer needed). aiFlags (bits): 2 = unconscious off (only a bag / a disable - never a captive or a restrained NPC:
; SetUnconscious(false) could untie her), 4 = Paralysis 0 (Diary of Mine's tie animation needs it; never for VRTouchEvents, which
; owns her Paralysis), 8 = HealRate + potions + stealth meter back (deferred for VRTouchEvents until it wakes her). Always rank 0.
; Also used, with aiFlags 8, to give back the deferred part once VRTouchEvents has woken her.
; A living NPC leaves SBKO with at least afWakeHealth (WakeHealth: fWakeHealthFraction of her max, never lowered, always above 0) -
; the DLL skips the engine's full refill, so without this she would be handed over at her held Health.
Function LetGo(Int aiFormId, Float afSavedHealRate, Int aiPotionMask, Int aiFlags, Float afWakeHealth, Float afWakeFrac) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	akNpc.SetNoBleedoutRecovery(false)                        ; always - nothing of SBKO's stays on after it lets go
	If !akNpc.IsDead()
		WakeHealth(akNpc, afWakeHealth, afWakeFrac)           ; never handed over at 1 % / below 0
	EndIf
	If ((aiFlags / 2) % 2) == 1 && akNpc.IsUnconscious()
		akNpc.SetUnconscious(false)
	EndIf
	If ((aiFlags / 4) % 2) == 1
		akNpc.ForceActorValue("Paralysis", 0)
	EndIf
	If ((aiFlags / 8) % 2) == 1
		If afSavedHealRate >= 0.0
			akNpc.ForceActorValue("HealRate", afSavedHealRate)
		ElseIf afSavedHealRate == -2.0
			akNpc.ForceActorValue("HealRate", akNpc.GetBaseActorValue("HealRate"))
		EndIf
		GivePotionsBack(akNpc, aiPotionMask)
		akNpc.SetNotShowOnStealthMeter(false)
	EndIf
	SetRank(aiFormId, 0)
EndFunction

; RE-ADOPT an NPC SBKO had released to another mod who is given back STILL UNCONSCIOUS (a capture that failed and left her
; unconscious, or an untie that left her unconscious). No SetUnconscious (she already is): rank 10, Paralysis 1, the self-shove only
; when the DLL saw her not ragdolled, stealth meter, HealRate 0. afCarried = the HealRate SBKO saved at her first knock-out (>= 0 or
; -2.0 = base) and is returned as the value to give back at the wake; anything else = save her current HealRate (KnockOut's -2.0
; rule). Returns -1.0 when gone / dead.
; Also the ORPHAN adopt (rank 10 + unconscious, no SBKO record - a lost knock-out): the DLL passes afCarried -1.0, so her current
; HealRate is saved (0 = SBKO's own zero -> -2.0 = her base value at the wake). NoBleedoutRecovery ON.
; aiNoFdcRoll: as in KnockOutU - a re-adopted hit knock-out is marked again.
Float Function ReAdopt(Int aiFormId, Int aiShove, Float afCarried, Int aiNoFdcRoll) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return -1.0
	EndIf
	If aiNoFdcRoll != 0
		MarkNoFdcRollThenRank10(akNpc)                        ; no Follower Death Chance roll for this knock-out
	Else
		SetRank(aiFormId, 10)
	EndIf
	akNpc.SetNoBleedoutRecovery(true)                         ; the regen stays off while held
	akNpc.ForceActorValue("Paralysis", 1)
	If aiShove != 0
		akNpc.PushActorAway(akNpc, 0.001)
	EndIf
	akNpc.SetNotShowOnStealthMeter(true)
	Float fSaved = afCarried
	If fSaved < 0.0 && fSaved != -2.0
		fSaved = akNpc.GetActorValue("HealRate")
		If fSaved <= 0.0 && akNpc.GetBaseActorValue("HealRate") > 0.0
			fSaved = -2.0
		EndIf
	EndIf
	akNpc.ForceActorValue("HealRate", 0.0)
	Return fSaved
EndFunction

; The line for a HEAL wake (the timer wake uses NarrateWake). One HUD line; SkyrimNet's DirectNarration with HER as the one who
; reacts, only when the DLL saw SkyrimNetApi. No pronouns in the strings.
; A Sleep-spell episode (SBKO_SleepFaction rank 1 / 2, still set at the wake) "wakes up"; a choke "comes round"; every other
; knock-out "regains consciousness".
Function NarrateHealWake(Int aiFormId, Int aiNotify, Int aiNarrate) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	String sName = akNpc.GetDisplayName()
	If sName == ""
		sName = akNpc.GetBaseObject().GetName()
	EndIf
	Bool bSleep = WasPutToSleep(akNpc)
	Bool bChoked = WasChoked(akNpc)
	If aiNotify != 0
		If bSleep
			Debug.Notification(sName + " wakes up.")
		ElseIf bChoked
			Debug.Notification(sName + " comes round.")
		Else
			Debug.Notification(sName + " regains consciousness.")
		EndIf
	EndIf
	If aiNarrate != 0
		Actor akNobody = None                                 ; explicit "addresses everyone nearby" (never a default argument)
		If bSleep
			SkyrimNetApi.DirectNarration(sName + " wakes up from a deep sleep.", akNpc, akNobody)
		ElseIf bChoked
			SkyrimNetApi.DirectNarration(sName + " comes round, coughing and confused, after being choked unconscious.", akNpc, akNobody)
		Else
			SkyrimNetApi.DirectNarration(sName + " regains consciousness after being healed, confused and unwell.", akNpc, akNobody)
		EndIf
	EndIf
EndFunction

; Is this episode the Sleep spell's? SBKO_SleepFaction (0x80A) rank 1 or 2 (1 not seen / 2 seen); an older ESP without the faction,
; or a non-member = False (an ordinary knock-out).
Bool Function WasPutToSleep(Actor akNpc) Global
	Int iMark = SleepMarkOf(akNpc)
	If iMark == 1 || iMark == 2                               ; rank 3 = choked, not a sleep
		Return True
	EndIf
	Return False
EndFunction

; Was she CHOKED unconscious (handed over through SBKO_API)? SBKO_SleepFaction rank 3.
Bool Function WasChoked(Actor akNpc) Global
	If SleepMarkOf(akNpc) == 3
		Return True
	EndIf
	Return False
EndFunction

; SBKO_SleepFaction's rank on her (-1 = no actor / no faction / not a member)
Int Function SleepMarkOf(Actor akNpc) Global
	If akNpc == None
		Return -1
	EndIf
	Faction kSleep = Game.GetFormFromFile(0x80A, "SimpleBleedoutKO_001.esp") as Faction
	If kSleep == None
		Return -1
	EndIf
	Return akNpc.GetFactionRank(kSleep)
EndFunction


; Another mod cleared her unconscious flag (for example Diary of Mine's "Wake" = SetUnconscious(false) + PushActorAway, Paralysis
; left at 1); the DLL has just put her back down - she needs healing. ONE HUD line per such clear. Her display name; no pronouns.
Function SayTooInjured(Int aiFormId, Int aiNotify) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || aiNotify == 0
		Return
	EndIf
	String sName = akNpc.GetDisplayName()
	If sName == ""
		sName = akNpc.GetBaseObject().GetName()
	EndIf
	Debug.Notification(sName + " is too injured to wake up.")
EndFunction

; SBKO's mark ends without a wake - at a DEATH while held (rank 0), or a STALE mark (rank 10 on a conscious NPC SBKO has no record
; of: a lost knock-out something else woke). Turns NoBleedoutRecovery off, and on a living NPC gives back what a lost knock-out may
; have left: HealRate (only if it reads 0 while her base value is above 0) and the stealth meter. Paralysis / the unconscious flag
; are not touched (she is conscious; another mod may own them). Then the rank.
Function ClearMark(Int aiFormId, Int aiRank) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	akNpc.SetNoBleedoutRecovery(false)
	If !akNpc.IsDead()
		If akNpc.GetActorValue("HealRate") <= 0.0 && akNpc.GetBaseActorValue("HealRate") > 0.0
			akNpc.ForceActorValue("HealRate", akNpc.GetBaseActorValue("HealRate"))
		EndIf
		akNpc.SetNotShowOnStealthMeter(false)
	EndIf
	SetRank(aiFormId, aiRank)
EndFunction

; ---- console test helpers (the 'sbkou' command) - never called by the knock-out itself ------------------------------------

; sbkou on a protected / essential NPC SBKO does not hold: drop her to 0 Health with NO attacker - the real bleedout path
; (the engine puts a protected / essential NPC into bleedout; the DLL refuses anyone else, who would die).
Function TestDrop(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return
	EndIf
	akNpc.DamageActorValue("Health", akNpc.GetActorValue("Health") + 1.0)
EndFunction

; sbkou on an NPC SBKO holds UNCONSCIOUS: the "another mod meddles" test - +50 % Health with no magic effect, unconscious off,
; Paralysis 0. The next check puts her back down (unconscious, Paralysis, shove), puts the Health back, and - once she is settled -
; shows "<name> is too injured to wake up.".
Function TestMeddle(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return
	EndIf
	Float fPct = akNpc.GetActorValuePercentage("Health")
	Float fMax = akNpc.GetBaseActorValue("Health")
	If fPct > 0.0
		fMax = akNpc.GetActorValue("Health") / fPct
	EndIf
	akNpc.RestoreActorValue("Health", fMax * 0.5)
	akNpc.SetUnconscious(false)
	akNpc.ForceActorValue("Paralysis", 0)
EndFunction

; ============================================================================================================================

; Optional Follower Death and Injury Chance patch (FDC_SBKO_Patch.esp, not part of this mod): its faction 0x800 at rank >= 1 means
; "this knock-out already had its FDC roll"; rank 2 = "never roll, not even on a hit". It is written FIRST, then rank 10, with both
; factions joined before either rank is written. The DLL calls this ONLY when FDC_SBKO_Patch.esp is loaded, so GetFormFromFile
; never asks for a missing file.
Function MarkNoFdcRollThenRank10(Actor akNpc) Global
	If akNpc == None
		Return
	EndIf
	Faction kState  = Game.GetFormFromFile(0x803, "SimpleBleedoutKO_001.esp") as Faction
	Faction kRolled = Game.GetFormFromFile(0x000800, "FDC_SBKO_Patch.esp") as Faction
	If kRolled && !akNpc.IsInFaction(kRolled)
		akNpc.AddToFaction(kRolled)
	EndIf
	If kState && !akNpc.IsInFaction(kState)
		akNpc.AddToFaction(kState)
	EndIf
	If kRolled
		akNpc.SetFactionRank(kRolled, 2)                      ; FIRST: "this knock-out never rolls" (2 - not even on a hit)
	EndIf
	If kState
		akNpc.SetFactionRank(kState, 10)                      ; then rank 10 (SkyrimNet: unconscious)
	EndIf
EndFunction

; The state rank: she joins SBKO_StateFaction on her first knock-out and is never removed; rank 0 = nothing to report.
; An NPC who was never knocked out is left out of the faction entirely.
Function SetRank(Int aiFormId, Int aiRank) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	Faction kState = Game.GetFormFromFile(0x803, "SimpleBleedoutKO_001.esp") as Faction
	If akNpc == None || kState == None
		Return                                                ; no actor, or an old ESP without the faction
	EndIf
	If aiRank <= 0 && !akNpc.IsInFaction(kState)
		Return
	EndIf
	akNpc.AddToFaction(kState)
	akNpc.SetFactionRank(kState, aiRank)
EndFunction

; SBKO_SleepFaction (0x80A) rank = what the SkyrimNet coming-to prompt reads at the wake. 2 = put to sleep by the Sleep spell and
; she saw the caster, 1 = by the spell, not seen, 3 = choked unconscious (the prompts' marker only, never a HUD line), 0 = any other
; knock-out / the coming-to ended. The DLL sends it at EVERY knock-out (right after KnockOutU) and at the coming-to's end. A rank
; change, never RemoveFromFaction; a non-member is left alone with 0 (no faction added to NPCs the spell never touched).
; aiNotify 1 with aiMark 1 / 2 = the one HUD line of a spell knock-out (KnockOutU's own "is knocked out." is not sent then).
; An older ESP without the faction: nothing happens (the DLL does not call this then).
Function SetSleepMark(Int aiFormId, Int aiMark, Int aiNotify) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	Faction kSleep = Game.GetFormFromFile(0x80A, "SimpleBleedoutKO_001.esp") as Faction
	If akNpc == None || kSleep == None
		Return
	EndIf
	If aiMark <= 0 && !akNpc.IsInFaction(kSleep)
		Return
	EndIf
	akNpc.AddToFaction(kSleep)
	akNpc.SetFactionRank(kSleep, aiMark)
	If (aiMark == 1 || aiMark == 2) && aiNotify != 0 && !akNpc.IsDead()   ; only after a real knock-out; never for a choke
		String sName = akNpc.GetDisplayName()
		If sName == ""
			sName = akNpc.GetBaseObject().GetName()
		EndIf
		Debug.Notification(sName + " falls into a deep sleep.")
	EndIf
EndFunction

; The line for a TIMER wake (a heal wake uses NarrateHealWake).
; aiNotify = the one-line HUD; aiNarrate = SkyrimNet's DirectNarration with HER as the one who reacts - the DLL passes 1 only when
; the VM knows the script SkyrimNetApi, so this line never runs without SkyrimNet. No pronouns in the strings.
Function NarrateWake(Int aiFormId, Int aiHours, Int aiNotify, Int aiNarrate) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return
	EndIf
	String sName = akNpc.GetDisplayName()
	If sName == ""
		sName = akNpc.GetBaseObject().GetName()
	EndIf
	String sHours = aiHours + " hours"
	If aiHours == 1
		sHours = "1 hour"
	EndIf
	Bool bSleep = WasPutToSleep(akNpc)                        ; a Sleep-spell episode just wakes up
	Bool bChoked = WasChoked(akNpc)
	If aiNotify != 0
		If bSleep
			Debug.Notification(sName + " wakes up after " + sHours + ".")
		ElseIf bChoked
			Debug.Notification(sName + " comes round after " + sHours + ".")
		Else
			Debug.Notification(sName + " regains consciousness after " + sHours + ".")
		EndIf
	EndIf
	If aiNarrate != 0
		Actor akNobody = None                                 ; explicit "addresses everyone nearby" (never a default argument)
		If bSleep
			SkyrimNetApi.DirectNarration(sName + " wakes up after sleeping for " + sHours + ".", akNpc, akNobody)
		ElseIf bChoked
			SkyrimNetApi.DirectNarration(sName + " comes round after being choked unconscious for " + sHours + ", coughing and confused.", akNpc, akNobody)
		Else
			SkyrimNetApi.DirectNarration(sName + " regains consciousness after being passed out for " + sHours + ", confused and unwell.", akNpc, akNobody)
		EndIf
	EndIf
EndFunction

;============================================================================================================================
; Follower Death Chance re-roll, woken-to-talk narration
;============================================================================================================================

; Optional Follower Death and Injury Chance patch (not part of this mod): one hit SBKO counted on a follower it holds after a
; BLEEDOUT knock-out -> the patch's running effect on her (RegisterForModEvent "SBKO_FdcHit") re-rolls with its own settings.
; Sent from Papyrus on purpose, never from C++ inside an SKSE task. Sender = HER (every running copy of the effect hears it; only
; hers rolls). asHit = "attacker|weapon|kind|damage"; aiDeathOnly 1 = a re-roll that misses does nothing (ini bFdcRerollDeathOnly).
; The DLL calls this only with FDC_SBKO_Patch.esp loaded.
Function FdcHit(Int aiFormId, String asHit, Int aiDeathOnly) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead()
		Return
	EndIf
	Float fDeathOnly = aiDeathOnly as Float                   ; a named local (Caprica temp-reuse rule)
	akNpc.SendModEvent("SBKO_FdcHit", asHit, fDeathOnly)
EndFunction

; The player has just woken her (SkyrimNet is told, so she can react). aiKind 1 = a natural sleeper woken to talk (a talk, or Diary
; of Mine's Wake) - no HUD line (vanilla has none); 2 = SBKO's Sleep-spell sleeper woken by the player (Diary of Mine's Wake) - HUD
; "<name> wakes up.". aiMode 1 = DirectNarration (she answers the player now, the fact is kept), 0 = RegisterPersistentEvent (she
; only knows it). aiNarrate = the DLL saw SkyrimNetApi. Names, never pronouns. Never for an unconscious / dead NPC.
; Never for an NPC hostile to the player either (a bandit asleep in a camp) - tested here, in Papyrus, because the DLL's task may
; run on a VR job thread.
Function NarrateWoken(Int aiFormId, Int aiKind, Int aiMode, Int aiNotify, Int aiNarrate) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || akNpc.IsDead() || akNpc.IsUnconscious()
		Return
	EndIf
	Actor akPlayer = Game.GetPlayer()
	If akPlayer == None || akNpc.IsHostileToActor(akPlayer)          ; never a None argument (IsHostileToActor(None) = a CTD)
		Debug.Trace("[SBKO] NarrateWoken: " + akNpc.GetDisplayName() + " is hostile to the player - no 'woken' line")
		Return
	EndIf
	String sName = akNpc.GetDisplayName()
	If sName == ""
		sName = akNpc.GetBaseObject().GetName()
	EndIf
	String sPlayer = akPlayer.GetDisplayName()
	String sLine = sPlayer + " has just woken " + sName + " up to talk to " + sName + "."
	If aiKind == 2
		sLine = sPlayer + " has just woken " + sName + " from a deep sleep."
		If aiNotify != 0
			Debug.Notification(sName + " wakes up.")
		EndIf
	EndIf
	If aiNarrate != 0
		If aiMode == 1
			SkyrimNetApi.DirectNarration(sLine, akNpc, akPlayer)
		Else
			SkyrimNetApi.RegisterPersistentEvent(sLine, akNpc, akPlayer)
		EndIf
	EndIf
EndFunction

; For the DLL's one-line parity log (C++ Health reading vs Papyrus) at each knock-out and wake.
Float Function HealthPct(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return -1.0
	EndIf
	Return akNpc.GetActorValuePercentage("Health")
EndFunction

; Empty the hands so they ragdoll clean (the technique of "Knockout - Health and Stealth-Based KOs").
Function UnequipHands(Actor akNpc) Global
	If akNpc.GetEquippedWeapon(0)
		akNpc.UnequipItem(akNpc.GetEquippedWeapon(0))
	ElseIf akNpc.GetEquippedSpell(1)
		akNpc.UnequipSpell(akNpc.GetEquippedSpell(1), 1)
	EndIf
	If akNpc.GetEquippedWeapon(1)
		akNpc.UnequipItem(akNpc.GetEquippedWeapon(1))
	ElseIf akNpc.GetEquippedSpell(0)
		akNpc.UnequipSpell(akNpc.GetEquippedSpell(0), 0)
	ElseIf akNpc.GetEquippedShield()
		akNpc.UnequipItem(akNpc.GetEquippedShield())
	EndIf
EndFunction
