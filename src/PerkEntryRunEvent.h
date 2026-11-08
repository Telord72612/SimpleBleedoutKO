#pragma once

// u7a (M2): TESPerkEntryRunEvent - the engine's "a perk entry's fragment runs" event (the Papyrus VM sinks it: that is how a
// perk fragment such as Diary of Mine's menu Wake / DOMWakeUp runs - CommonLib SkyrimVM.h:119). The toolbox (CommonLibVR MinLL 4.39.5)
// DECLARES it (ScriptEventSourceHolder.h:39, its source at holder +0x9A0) but never defines it; this completes the declaration.
// Layout = po3's CommonLibSSE (TESPerkEntryRunEvent.h): cause +0x00, target +0x08, perkId +0x10, flag +0x14 (the entry's fragment
// index), size 0x18. ⚠ UNVERIFIED in VR: the sink compares perkId with Diary of Mine's perk FormID BEFORE
// it touches either reference, so a wrong layout can never match and never send the sink after a bad pointer; bLogWokenProbe prints the
// raw fields once per event (the first VR run settles it).
#include "RE/N/NiSmartPointer.h"

namespace RE
{
    class TESObjectREFR;

    struct TESPerkEntryRunEvent
    {
    public:
        // members
        NiPointer<TESObjectREFR> cause;    // 00 - who ran the entry (the player, for an Activate entry the player picked)
        NiPointer<TESObjectREFR> target;   // 08 - the reference it ran on (the NPC he activated)
        FormID                   perkId;   // 10 - the perk
        std::uint16_t            flag;     // 14 - the fragment index of the entry (Diary of Mine's DOMInteractPerk: 0 Wake, 1 Bag, 2 Abduct,
                                           //      3 Untie, 4 Tie-up - read from its VMAD)
        std::uint16_t            pad16;    // 16
    };
    static_assert(sizeof(TESPerkEntryRunEvent) == 0x18);
}
