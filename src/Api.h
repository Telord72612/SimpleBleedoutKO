#pragma once

// u7a (U7): SimpleBleedoutKO's public Papyrus API - the natives of the script SBKO_API (Scripts/Source/SBKO_API.psc, shipped
// by SBKO). VRTouchEvents calls them through its own gate script when SimpleBleedoutKO_001.esp is loaded:
//   Int  Function ApiVersion() Global Native                                                            -> 1
//   Bool Function TakeOver(Actor akTarget, Actor akAggressor, String asSource, Float afChokeSeconds) Global Native
//   Bool Function Holds(Actor akTarget) Global Native
//   Function ChokeReleased(Actor akVictim, Float afSecondsHeld) Global Native   (the choke ended: the TOTAL seconds held - build 3's neck mark)
// The work is in Knockout.cpp (ApiTakeOver / ApiHolds / ApiChokeReleased): a native never takes the knockout mutex; it decides on reads +
// SBKO's held set, answers at once, and hands the work to an SKSE task (TaskRelay).
namespace SBKO::Api
{
    inline constexpr std::int32_t kApiVersion = 1;
    inline constexpr const char*  kScript     = "SBKO_API";

    bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
}
