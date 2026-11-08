Scriptname SBKO_DownWatcherScript extends ActiveMagicEffect
{SimpleBleedoutKO - RETIRED in 2.0: an EMPTY stub on purpose.
 The knock-out now lives in SimpleBleedoutKO.dll (C++) + SBKO_Helper.psc. Saves made with the old Papyrus-only version may still carry
 its ability on NPCs; this stub lets those saves load without errors and makes the old effect do nothing.
 The variables below are kept ONLY so the data the old version wrote into saves still maps onto this script (no "variable not
 found" lines at load). No events: old pending updates (OnUpdate / OnUpdateGameTime) arrive, find no handler, and stop.}

Actor TargetActor
Faction kStateFaction
Int   iComingTo   = 0
Int   iStageTicks = 0
Bool  bIsDown = false
Bool  bBusy   = false
Int   iDownTicks = 0
Float fRecoverHours  = 6.0
Float fSavedHealRate = -1.0
