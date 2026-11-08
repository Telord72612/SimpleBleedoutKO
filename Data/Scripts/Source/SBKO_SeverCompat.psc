Scriptname SBKO_SeverCompat Hidden
{SimpleBleedoutKO 2.0.0 - the SeverActions guard. EVERY SeverActions call SBKO makes lives in this one script, so SBKO_Helper never
 references a SeverActions type. The DLL calls these globals ONLY when SeverActions.esp is loaded AND the VM knows this script.
 Why: SeverActions' survival tick feeds a follower and its eating animation can force an idle on a knocked-out follower (her
 animation then walks while her body lies ragdolled). An EXCLUDED follower is skipped by the whole survival update, so SBKO
 excludes a follower it knocks out and includes her back at the wake.
 Note: SeverActions zeroes her hunger / fatigue / cold on exclude and seeds RANDOM needs on include when all three are zero - so
 Guard reads the three first and Unguard writes them back after the include.
 The survival script sits on quest SeverActions (0x000D62 in SeverActions.esp). WHO is guarded is decided HERE, by SeverActions'
 own lists (its follower roster and its survival list); an NPC SeverActions does not track gets -3 and nothing is done.}

SeverActions_Survival Function Survival() Global
	Return Game.GetFormFromFile(0x000D62, "SeverActions.esp") as SeverActions_Survival
EndFunction

; 0..100 -> tenths, 0..1000
Int Function Tenths(Float afValue) Global
	If afValue < 0.0
		afValue = 0.0
	ElseIf afValue > 100.0
		afValue = 100.0
	EndIf
	Return (afValue * 10.0 + 0.5) as Int
EndFunction

; Does SeverActions track her? A player teammate (what its survival list starts from), OR its native follower roster
; (SeverActionsNativeExt.Native_GetIsFollower), OR its survival list itself (SeverActionsNative.Survival_GetCurrentFollowers:
; loaded teammates plus dismissed followers with a home, when its dismissed-follower survival is on).
Bool Function Tracked(Actor akNpc) Global
	If akNpc.IsPlayerTeammate()
		Return true
	EndIf
	If SeverActionsNativeExt.Native_GetIsFollower(akNpc)
		Return true
	EndIf
	Actor[] aList = SeverActionsNative.Survival_GetCurrentFollowers()
	If aList
		Return aList.Find(akNpc) >= 0
	EndIf
	Return false
EndFunction

; Exclude her from SeverActions' survival tracking for the knock-out. Returns:
;   -1 = no actor / no SeverActions survival quest - nothing done
;   -2 = SeverActions survival is switched OFF - nothing done (no tick runs, and an exclude would zero her frozen needs)
;   -3 = SeverActions does not track her (not a teammate, not on its roster, not on its survival list) - nothing done
;    0 = the user had ALREADY excluded her - nothing done, and nothing is done at the wake either
;   >0 = SBKO excluded her; the value is her needs snapshot: 2^30 + hunger*2^20 + fatigue*2^10 + cold, each in tenths (0..1000)
Int Function Guard(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None
		Return -1
	EndIf
	SeverActions_Survival kSurv = Survival()
	If kSurv == None
		Return -1
	EndIf
	If !kSurv.IsFollowerSurvivalEnabled()
		Return -2
	EndIf
	If !Tracked(akNpc)
		Return -3
	EndIf
	If kSurv.IsFollowerExcluded(akNpc)
		Return 0
	EndIf
	Int iHun = Tenths(SeverActionsNative.Native_Survival_GetHunger(akNpc))
	Int iFat = Tenths(SeverActionsNative.Native_Survival_GetFatigue(akNpc))
	Int iCol = Tenths(SeverActionsNative.Native_Survival_GetCold(akNpc))
	kSurv.ExcludeFollower(akNpc)
	Debug.Trace("[SBKO] SeverActions guard: " + akNpc.GetDisplayName() + " excluded from survival while knocked out (needs H=" \
		+ (iHun as Float / 10.0) + " F=" + (iFat as Float / 10.0) + " C=" + (iCol as Float / 10.0) + " kept for the wake)")
	Return 1073741824 + iHun * 1048576 + iFat * 1024 + iCol
EndFunction

; At the wake: include her back, then write her snapshot back. Returns:
;   -1 = no actor / no quest / not a snapshot - nothing done
;    2 = she is no longer excluded (someone included her meanwhile) - her needs are left as they are
;    1 = included and restored
Int Function Unguard(Int aiFormId, Int aiPacked) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	If akNpc == None || aiPacked < 1073741824
		Return -1
	EndIf
	SeverActions_Survival kSurv = Survival()
	If kSurv == None
		Return -1
	EndIf
	If !kSurv.IsFollowerExcluded(akNpc)
		Return 2
	EndIf
	Int iRest = aiPacked - 1073741824
	Int iHun = iRest / 1048576
	iRest = iRest - iHun * 1048576
	Int iFat = iRest / 1024
	Int iCol = iRest - iFat * 1024
	Float fH = iHun as Float / 10.0
	Float fF = iFat as Float / 10.0
	Float fC = iCol as Float / 10.0
	kSurv.IncludeFollower(akNpc)                              ; may seed random needs (all were zeroed) - overwritten just below
	kSurv.SetFollowerHunger(akNpc, (fH + 0.5) as Int)
	kSurv.SetFollowerFatigue(akNpc, (fF + 0.5) as Int)
	kSurv.SetFollowerCold(akNpc, (fC + 0.5) as Int)
	SeverActionsNative.Native_Survival_SetNeeds(akNpc, fH, fF, fC)   ; the exact snapshot (tenths)
	Debug.Trace("[SBKO] SeverActions guard: " + akNpc.GetDisplayName() + " included in survival again, needs restored H=" + fH \
		+ " F=" + fF + " C=" + fC)
	Return 1
EndFunction
