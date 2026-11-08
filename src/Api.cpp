#include "PCH.h"
#include "Api.h"
#include "Knockout.h"

// u7a (U7: the choke knock-out passes to SBKO when SBKO is present and stays in VRTE when SBKO is not there): the
// SBKO_API natives. Registered once at plugin load through SKSE's Papyrus interface (DDAB's VR-proven pattern,
// its RegisterPapyrus). Each native only forwards to Knockout.cpp, which answers from reads and SBKO's
// held set (never the knockout mutex, never TypeIsValid - Helper::PresentCached) and hands the work to an SKSE task.
// An older SBKO DLL leaves these unbound: one Papyrus error line on the caller's side and 0 / False back -> VRTE runs its own code.
namespace SBKO::Api
{
    namespace
    {
        std::int32_t ApiVersion(RE::StaticFunctionTag*) { return kApiVersion; }

        bool TakeOver(RE::StaticFunctionTag*, RE::Actor* a_target, RE::Actor* a_aggressor, RE::BSFixedString a_source, float a_seconds)
        {
            const char* src = a_source.c_str();
            return Knockout::ApiTakeOver(a_target, a_aggressor, src ? src : "", a_seconds);
        }

        bool Holds(RE::StaticFunctionTag*, RE::Actor* a_target) { return Knockout::ApiHolds(a_target); }

        void ChokeReleased(RE::StaticFunctionTag*, RE::Actor* a_victim, float a_seconds)
        {
            Knockout::ApiChokeReleased(a_victim, a_seconds, "VRTouchEvents");
        }
    }

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            logger::error("[KO] U7 take-over API: no Papyrus VM - SBKO_API natives NOT registered (VRTouchEvents keeps its own choke knock-out)");
            return false;
        }
        a_vm->RegisterFunction("ApiVersion", kScript, ApiVersion);
        a_vm->RegisterFunction("TakeOver", kScript, TakeOver);
        a_vm->RegisterFunction("Holds", kScript, Holds);
        a_vm->RegisterFunction("ChokeReleased", kScript, ChokeReleased);
        logger::info("[KO] U7 take-over API: SBKO_API natives registered (ApiVersion {} | TakeOver(Actor, Actor, String, Float) | Holds(Actor) | "
                     "ChokeReleased(Actor, Float)); sources: VRTouchEvents.choke (+ console.choke for 'sbko takeover'); a choke = SBKO's own "
                     "knock-out: held at her Health, no FDC roll, wakes on a real heal or SBKO's timer, SBKO_SleepFaction rank 3 (choked)",
                     kApiVersion);
        return true;
    }
}
