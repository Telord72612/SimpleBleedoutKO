Scriptname SBKO_Marks Hidden
; =============================================================================
;  SimpleBleedoutKO 2.0.0 - the skin marks' "hands".
;
;  Everyone Injury Engine wounds gets the matching mark; when Injury Engine heals the wound, the mark goes. The DLL
;  (SimpleBleedoutKO.dll) decides EVERYTHING - which mark, which slot band, which alpha, when. This script only runs RaceMenu's
;  NiOverride natives for it (RaceMenu VR has no C++ overlay interface) and reads Injury Engine for it.
;    * No QueueNiNodeUpdate: AddOverlays itself queues the face slots. So: AddOverlays only when she has none, Wait(0.5), then
;      write. Never QueueNiNodeUpdate, never ApplyNodeOverrides, never RemoveOverlays. Every write persist = true (RaceMenu's
;      co-save keeps it).
;    * the texture (key 9) is written once per mark life - the expensive write; afterwards the look only (key 7 tint, key 8 alpha).
;    * a slot is OURS only when the DLL says SBKO wrote it (the slot index travels with every op, co-saved by the DLL) AND it still
;      holds our file. A slot SBKO did not write is never adopted, re-tinted or cleared - even when it holds the same Battle Wounds
;      file (SlaveTats / other overlay mods paint those too). A new mark takes a FREE slot of its band: no key 9, or RaceMenu's
;      default.dds, or SlaveTats' blank.dds. No free slot = nothing painted, the DLL is told.
;    * clear: RaceMenu's default.dds first (a Remove alone does not revert the shader), then every key we set.
;  Look (matte): key 0 = 0, key 1 = 1.0, key 2 = 0.0, key 3 = 0.0, key 7 = the tint, key 8 = the alpha.
;  Ints across the VM boundary (the FormID as an Int, resolved here with Game.GetFormEx).
;  Injury Engine is a SOFT dependency: compiled against a stub of Injury_Engine_Engine (never shipped); the DLL calls IEState /
;  IEHasWounds / IEWounded / Wound only when the VM knows Injury_Engine_Engine.
;  Caprica note: a computed Bool passed as a call argument goes into a NAMED local first.
; =============================================================================

; -----------------------------------------------------------------------------
;  THE DLL'S CALLS
; -----------------------------------------------------------------------------

; ONE call per actor with all her due marks, run here in order on one stack (two marks of hers never race for one free slot).
; asOps = records joined by ";", each "op|code|area|first|last|path|alpha|tint|slot" (slot = the one SBKO wrote, -1 = none):
;   op 1 paint (our slot still holding our file -> the look again; else a FREE slot of the band: the texture + the look),
;   op 2 look (alpha + tint on OUR slot only), op 3 clear (OUR slot only).
;   area 0 Body, 1 Face, 2 Hands, 3 Feet; first..last = the band; tint = 0xRRGGBB as a decimal Int.
; Returns "<setup>;<r>|<slot>;<r>|<slot>..." - setup 1 = AddOverlays was called now, 0 = she had overlays, -1 = no actor / no 3D
; (nothing done). r: 1 on her skin, 2 cleared, 3 nothing there (or not our slot), 0 no free slot, -1 no 3D (wait), -3 our slot no
; longer holds our file (nothing touched), -4 no such band on this RaceMenu, -9 a bad record.
String Function Apply(Int aiFormId, String asOps) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	if akNpc == None
		return "-1"
	EndIf
	if !akNpc.Is3DLoaded()
		return "-1"
	EndIf
	Int setup = 0
	if !NiOverride.HasOverlays(akNpc)
		NiOverride.AddOverlays(akNpc)              ; once per actor - RaceMenu builds her overlay shapes AND queues the face slots
		Utility.Wait(0.5)                          ; let the queued installs run (no node update)
		setup = 1
		if !akNpc.Is3DLoaded()
			return "-1"                            ; she unloaded during the wait - the DLL repaints at her next 3D load
		EndIf
	EndIf
	Bool bFemale = IsFemale(akNpc)
	String out = setup as String
	String[] ops = StringUtil.Split(asOps, ";")
	String[] f
	Int i = 0
	Int op = 0
	Int area = 0
	Int first = 0
	Int last = 0
	String path = ""
	Float alpha = 0.0
	Int tint = 0
	Int r = 0
	Int slot = -1
	Int mine = -1
	Bool ours = False
	Bool ready = False
	while i < ops.Length
		f = StringUtil.Split(ops[i], "|")
		r = -9
		slot = -1
		if f.Length >= 9
			op = f[0] as Int
			area = f[2] as Int
			first = f[3] as Int
			last = f[4] as Int
			path = f[5]
			alpha = f[6] as Float
			tint = f[7] as Int
			mine = f[8] as Int
			ready = akNpc.Is3DLoaded()
			if !ready
				r = -1
				slot = mine
			elseif op == 1
				slot = PaintIdx(akNpc, bFemale, area, first, last, mine, path, alpha, tint)
				if slot >= 0
					r = 1
				elseif slot == -1
					r = 0
				elseif slot == -2
					r = -4
				else
					r = -1
				EndIf
			elseif op == 2
				ours = Ours(akNpc, bFemale, area, mine, path)
				if ours
					NiOverride.AddNodeOverrideInt(akNpc, bFemale, AreaName(area) + " [Ovl" + mine + "]", 7, -1, tint, true)
					NiOverride.AddNodeOverrideFloat(akNpc, bFemale, AreaName(area) + " [Ovl" + mine + "]", 8, -1, alpha, true)
					r = 1
					slot = mine
				else
					r = -3
				EndIf
			elseif op == 3
				ours = Ours(akNpc, bFemale, area, mine, path)
				if ours
					ClearNode(akNpc, bFemale, AreaName(area) + " [Ovl" + mine + "]")
					r = 2
				else
					r = 3
				EndIf
			EndIf
		EndIf
		out = out + ";" + r + "|" + slot
		i += 1
	EndWhile
	return out
EndFunction

; Injury Engine's state for her: {"sources":[{"id",...,"parts":[{"part","region","stage","look","feel"}]}], "healed"?}; "" = no actor.
String Function IEState(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	if akNpc == None
		return ""
	EndIf
	return Injury_Engine_Engine.GetStateJson(akNpc)
EndFunction

; Injury Engine's cheap test: any wound left on her? (one actor per call - used only with an older DLL)
Bool Function IEHasWounds(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	if akNpc == None
		return False
	EndIf
	return Injury_Engine_Engine.HasOpenWounds(akNpc)
EndFunction

; The whole sweep in one call: the DLL passes every candidate's FormID (as an Int) in one array; the same array comes back with 0
; in place of each one that has no open wound (or is not found). One stack instead of one per actor.
; No "== None" test on the array: compiled, it becomes a None -> Int[] cast the game refuses (an error line on every call). The Length of
; a None array is 0, so the loop below simply does not run and the None goes back as it came.
Int[] Function IEWounded(Int[] aiFormIds) Global
	Int i = 0
	Actor akNpc
	Bool bOpen = False
	while i < aiFormIds.Length
		akNpc = Game.GetFormEx(aiFormIds[i]) as Actor
		bOpen = False
		if akNpc != None
			bOpen = Injury_Engine_Engine.HasOpenWounds(akNpc)
		EndIf
		if !bOpen
			aiFormIds[i] = 0
		EndIf
		i += 1
	EndWhile
	return aiFormIds
EndFunction

; -----------------------------------------------------------------------------
;  CONSOLE TESTS ('sbko marks ...')
; -----------------------------------------------------------------------------

; Every overlay slot of the four areas that holds a texture: "Face[10]=<file>@<alpha>; ..." ("" = none).
String Function Slots(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	if akNpc == None
		return "(no actor)"
	EndIf
	Bool bFemale = IsFemale(akNpc)
	String out = ""
	Int area = 0
	Int n = 0
	Int i = 0
	String an = ""
	String node = ""
	String path = ""
	Float alpha = -1.0
	while area <= 3
		an = AreaName(area)
		n = AreaCount(area)
		i = 0
		while i < n
			node = an + " [Ovl" + i + "]"
			if NiOverride.HasNodeOverride(akNpc, bFemale, node, 9, 0)
				path = NiOverride.GetNodeOverrideString(akNpc, bFemale, node, 9, 0)
				alpha = -1.0
				if NiOverride.HasNodeOverride(akNpc, bFemale, node, 8, -1)
					alpha = NiOverride.GetNodeOverrideFloat(akNpc, bFemale, node, 8, -1)
				EndIf
				if out != ""
					out = out + "; "
				EndIf
				out = out + an + "[" + i + "]=" + path + "@" + alpha
			EndIf
			i += 1
		EndWhile
		area += 1
	EndWhile
	return out
EndFunction

; A wound through Injury Engine's own public natives (both narrate, as IE's own actions do), by the player. A torture source ->
; HitPart on aiPart (0 face, 1 left arm, 2 right arm, 3 back, 4 left leg, 5 right leg); a real one -> RealHit with aiPoints (IE
; picks the part). Returns IE's answer (-1 = refused: an unknown source / a dead actor).
Int Function Wound(Int aiFormId, String asSource, Int aiPart, Int aiPoints) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	if akNpc == None
		return -1
	EndIf
	Actor akPlayer = Game.GetPlayer()
	Bool bTorture = Injury_Engine_Engine.IsTorture(asSource)
	if bTorture
		return Injury_Engine_Engine.HitPart(akNpc, akPlayer, asSource, aiPart)
	EndIf
	return Injury_Engine_Engine.RealHit(akNpc, akPlayer, asSource, aiPoints)
EndFunction

; Her Health to full (IE trims her real wounds as Health comes back; the marks follow at the next read).
Function HealFull(Int aiFormId) Global
	Actor akNpc = Game.GetFormEx(aiFormId) as Actor
	if akNpc == None
		return
	EndIf
	akNpc.RestoreActorValue("Health", 100000.0)
EndFunction

; -----------------------------------------------------------------------------
;  THE WORK
; -----------------------------------------------------------------------------

; Paint: >= 0 = the slot index, -1 = no free slot in the band, -2 = no such band on this RaceMenu. aiMine = the slot SBKO wrote this
; mark into before (-1 = none): while it still holds our file it is only re-looked; otherwise a FREE slot of the band takes the mark.
; A slot that merely holds the same file is NEVER adopted - it may belong to SlaveTats or another overlay mod.
Int Function PaintIdx(Actor akNpc, Bool abFemale, Int aiArea, Int aiFirst, Int aiLast, Int aiMine, String asPath, Float afAlpha, Int aiTint) Global
	String an = AreaName(aiArea)
	if an == ""
		return -2
	EndIf
	Bool ours = Ours(akNpc, abFemale, aiArea, aiMine, asPath)
	if ours
		Look(akNpc, abFemale, an + " [Ovl" + aiMine + "]", afAlpha, aiTint)
		return aiMine
	EndIf
	Int n = AreaCount(aiArea)
	if aiFirst >= n
		return -2
	EndIf
	Int slot = FreeSlot(akNpc, abFemale, aiArea, aiFirst, aiLast)
	if slot < 0
		return -1
	EndIf
	String node = an + " [Ovl" + slot + "]"
	NiOverride.AddNodeOverrideString(akNpc, abFemale, node, 9, 0, asPath, true)     ; the texture: the ONE expensive write
	Look(akNpc, abFemale, node, afAlpha, aiTint)
	return slot
EndFunction

; Is aiSlot SBKO's mark of asPath? = the DLL wrote it there (aiSlot >= 0, inside this RaceMenu's slot count) AND it still holds that file.
Bool Function Ours(Actor akNpc, Bool abFemale, Int aiArea, Int aiSlot, String asPath) Global
	if aiSlot < 0
		return False
	EndIf
	if aiSlot >= AreaCount(aiArea)
		return False
	EndIf
	String node = AreaName(aiArea) + " [Ovl" + aiSlot + "]"
	if !NiOverride.HasNodeOverride(akNpc, abFemale, node, 9, 0)
		return False
	EndIf
	String path = NiOverride.GetNodeOverrideString(akNpc, abFemale, node, 9, 0)
	String want = FileOf(asPath)
	if want == ""
		return False
	EndIf
	return FileOf(path) == want
EndFunction

; The look keys on one slot (matte, tint, alpha).
Function Look(Actor akNpc, Bool abFemale, String asNode, Float afAlpha, Int aiTint) Global
	NiOverride.AddNodeOverrideInt(akNpc, abFemale, asNode, 0, -1, 0, true)               ; emissive colour off
	NiOverride.AddNodeOverrideFloat(akNpc, abFemale, asNode, 1, -1, 1.0, true)           ; emissive multiple
	NiOverride.AddNodeOverrideFloat(akNpc, abFemale, asNode, 2, -1, 0.0, true)           ; glossiness 0 = matte
	NiOverride.AddNodeOverrideFloat(akNpc, abFemale, asNode, 3, -1, 0.0, true)           ; specular 0 = matte
	NiOverride.AddNodeOverrideInt(akNpc, abFemale, asNode, 7, -1, aiTint, true)          ; tint (0x5A6EC8 for bruises)
	NiOverride.AddNodeOverrideFloat(akNpc, abFemale, asNode, 8, -1, afAlpha, true)       ; alpha (0.30 / 0.50 / 0.75 / 1.00)
EndFunction

; Clear ONE slot: RaceMenu's default.dds first, then every key we set.
Function ClearNode(Actor akNpc, Bool abFemale, String asNode) Global
	NiOverride.AddNodeOverrideString(akNpc, abFemale, asNode, 9, 0, "actors\\character\\overlays\\default.dds", true)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 9, 0)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 0, -1)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 1, -1)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 2, -1)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 3, -1)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 7, -1)
	NiOverride.RemoveNodeOverride(akNpc, abFemale, asNode, 8, -1)
EndFunction

; The first FREE slot of the band, -1 = none (free = no key 9, or RaceMenu's default.dds, or SlaveTats' blank.dds).
Int Function FreeSlot(Actor akNpc, Bool abFemale, Int aiArea, Int aiFirst, Int aiLast) Global
	String an = AreaName(aiArea)
	Int last = aiLast
	Int n = AreaCount(aiArea)
	if last > n - 1
		last = n - 1
	EndIf
	Int i = aiFirst
	String node = ""
	String path = ""
	String file = ""
	while i <= last
		node = an + " [Ovl" + i + "]"
		if !NiOverride.HasNodeOverride(akNpc, abFemale, node, 9, 0)
			return i
		EndIf
		path = NiOverride.GetNodeOverrideString(akNpc, abFemale, node, 9, 0)
		file = FileOf(path)
		if path == "" || file == "default.dds" || file == "blank.dds"
			return i
		EndIf
		i += 1
	EndWhile
	return -1
EndFunction

; The file name at the end of a path ("\" or "/" both count).
String Function FileOf(String asPath) Global
	Int i = StringUtil.GetLength(asPath) - 1
	String c = ""
	while i >= 0
		c = StringUtil.GetNthChar(asPath, i)
		if c == "\\" || c == "/"
			return StringUtil.Substring(asPath, i + 1)
		EndIf
		i -= 1
	EndWhile
	return asPath
EndFunction

Bool Function IsFemale(Actor akNpc) Global
	ActorBase base = akNpc.GetLeveledActorBase()
	Bool bFemale = False
	if base
		bFemale = base.GetSex() == 1
	EndIf
	return bFemale
EndFunction

String Function AreaName(Int aiArea) Global
	if aiArea == 0
		return "Body"
	elseif aiArea == 1
		return "Face"
	elseif aiArea == 2
		return "Hands"
	elseif aiArea == 3
		return "Feet"
	EndIf
	return ""
EndFunction

; How many overlay slots this RaceMenu has in that area (skee64.ini / skeevr.ini [Overlays/...] iNumOverlays).
Int Function AreaCount(Int aiArea) Global
	if aiArea == 0
		return NiOverride.GetNumBodyOverlays()
	elseif aiArea == 1
		return NiOverride.GetNumFaceOverlays()
	elseif aiArea == 2
		return NiOverride.GetNumHandOverlays()
	elseif aiArea == 3
		return NiOverride.GetNumFeetOverlays()
	EndIf
	return 0
EndFunction
