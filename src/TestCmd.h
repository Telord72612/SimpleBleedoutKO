#pragma once

// The `sbko` test console command (full name SBKOTest). SkyrimVR's console has no cgf / CallGlobalFunction, so the
// command takes over an unused vanilla console command at kDataLoaded and dispatches the Papyrus global
// SBKO_Test.Run(Int aiFormId) for the selected NPC.
namespace SBKO::TestCmd
{
    void Register();   // call once, at kDataLoaded
}
