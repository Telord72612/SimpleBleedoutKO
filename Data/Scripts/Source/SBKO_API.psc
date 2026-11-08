Scriptname SBKO_API Native Hidden
{SimpleBleedoutKO's public API (ApiVersion 1): another mod (VRTouchEvents' choke) can hand a choked-out NPC to SimpleBleedoutKO.
 The natives live in SimpleBleedoutKO.dll (plugin name SimpleBleedoutKO). Call them only with SimpleBleedoutKO_001.esp loaded, through
 your own gate script (so your scripts still load without SBKO). An older SBKO DLL leaves them unbound: one Papyrus error line and
 0 / False back - run your own code then.
 TakeOver   -> True  = from now on SBKO owns her completely (its knock-out lands on the next frame: Paralysis, shove, then
                       unconscious); she is held at her Health, wakes only on a real heal or SBKO's 6-12 game-hour timer,
                       SkyrimNet is told she was choked unconscious. Already held by SBKO = True too (nothing more to do).
               False = declined (SBKO off or not ready, she is dead / not loaded / not an NPC / a child or a rider, Diary of Mine or
                       another mod has her, or an unknown source) -> run your own knock-out.
 Holds      -> SBKO holds her knocked out right now (or is about to: a TakeOver that has not landed yet).
 ChokeReleased -> the player let go of the choke after afSecondsHeld seconds IN ALL (after the passout or before it): SBKO keeps the
                  total for its neck mark (the longer the hold, the stronger the mark).}

Int Function ApiVersion() Global Native
	; 1 = TakeOver / Holds / ChokeReleased below

Bool Function TakeOver(Actor akTarget, Actor akAggressor, String asSource, Float afChokeSeconds) Global Native
	; asSource: "VRTouchEvents.choke". afChokeSeconds: how long the choke was held when she passed out.

Bool Function Holds(Actor akTarget) Global Native

Function ChokeReleased(Actor akVictim, Float afSecondsHeld) Global Native
	; once, when the player lets go of the choke (with or without a passout): the total seconds the choke was held
