#include "PCH.h"
#include "Helper.h"
#include "VmCall.h"
#include "RefillGuard.h"

// Arguments are always built from PRVALUES (static_cast / literals): MakeFunctionArguments forwards its parameter types,
// so an lvalue would instantiate FunctionArguments<T&> (MouthBridge main.cpp:141-142). A FormID travels as a Papyrus Int
// with its bits unchanged (an 0xFF... reference arrives negative; Game.GetFormEx / GetForm still resolves it).
namespace SBKO::Helper
{
    namespace
    {
        constexpr const char* kWhat = "the knockout does nothing (no NPC is knocked out, woken or ranked by SBKO)";

        std::int32_t AsInt(RE::FormID a_id) { return static_cast<std::int32_t>(a_id); }

        VmCall::Callback::Fn FloatOf(FloatReply a_reply)
        {
            return [reply = std::move(a_reply)](const RE::BSScript::Variable& r) {
                if (!reply) return;
                if (r.IsFloat())
                    reply(true, r.GetFloat());
                else if (r.IsInt())
                    reply(true, static_cast<float>(r.GetSInt()));
                else
                    reply(false, 0.0f);
            };
        }

        VmCall::Callback::Fn IntOf(IntReply a_reply)
        {
            return [reply = std::move(a_reply)](const RE::BSScript::Variable& r) {
                if (!reply) return;
                if (r.IsInt())
                    reply(true, r.GetSInt());
                else
                    reply(false, 0);
            };
        }

        VmCall::Callback::Fn DoneOf(DoneReply a_done)
        {
            return [done = std::move(a_done)](const RE::BSScript::Variable&) {
                if (done) done();
            };
        }

        // u2e: SBKO's own call on her is running -> RefillGuard skips the engine's full heal inside it (SetUnconscious(true) runs
        // StopCombat; SetNoBleedoutRecovery(false) out of combat refills). Leave() when the Papyrus function has finished (its reply,
        // VM thread) or at once when nothing was dispatched (the reply then never runs).
        VmCall::Callback::Fn Guarded(RE::FormID a_id, VmCall::Callback::Fn a_fn)
        {
            return [a_id, fn = std::move(a_fn)](const RE::BSScript::Variable& r) {
                RefillGuard::Leave(a_id);
                if (fn) fn(r);
            };
        }
        bool GuardedCall(RE::FormID a_id, const char* a_fn, RE::BSScript::IFunctionArguments* a_args, VmCall::Callback::Fn a_reply)
        {
            RefillGuard::Enter(a_id);
            const bool sent = VmCall::StaticIfPresentThen(kScript, a_fn, a_args, kWhat, Guarded(a_id, std::move(a_reply)));
            if (!sent) RefillGuard::Leave(a_id);
            return sent;
        }

        constexpr const char* kSeverWhat =
            "the SeverActions guard does nothing (a knocked-out follower stays in SeverActions' survival tracking)";
    }

    bool Present()
    {
        return VmCall::ClassPresent(RE::BSScript::Internal::VirtualMachine::GetSingleton(), kScript, kWhat);
    }

    int PresentCached() { return VmCall::ClassPresentCached(kScript); }

    bool KnockOut(RE::FormID a_id, bool a_notify, FloatReply a_reply)
    {
        return VmCall::StaticIfPresentThen(kScript, "KnockOut",
                                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_notify ? 1 : 0)),
                                           kWhat, FloatOf(std::move(a_reply)));
    }

    bool WakeUp(RE::FormID a_id, float a_savedHealRate, std::int32_t a_comingTo, bool a_notify, float a_wakeHealth, float a_wakeFrac,
                DoneReply a_done)
    {
        return GuardedCall(a_id, "WakeUp",
                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<float>(a_savedHealRate), static_cast<std::int32_t>(a_comingTo),
                                                     static_cast<std::int32_t>(a_notify ? 1 : 0), static_cast<float>(a_wakeHealth),
                                                     static_cast<float>(a_wakeFrac)),
                           DoneOf(std::move(a_done)));
    }

    bool SetRank(RE::FormID a_id, std::int32_t a_rank)
    {
        return VmCall::StaticIfPresent(kScript, "SetRank",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_rank)), kWhat);
    }

    // u6c: the Sleep spell's marker (SBKO_SleepFaction rank) - one global, no reply
    bool SetSleepMark(RE::FormID a_id, std::int32_t a_mark, bool a_notify)
    {
        return VmCall::StaticIfPresent(kScript, "SetSleepMark",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_mark),
                                                                 static_cast<std::int32_t>(a_notify ? 1 : 0)),
                                       kWhat);
    }

    bool HealthPct(RE::FormID a_id, FloatReply a_reply)
    {
        return VmCall::StaticIfPresentThen(kScript, "HealthPct", RE::MakeFunctionArguments(AsInt(a_id)), kWhat,
                                           FloatOf(std::move(a_reply)));
    }

    bool NarrateWake(RE::FormID a_id, std::int32_t a_hours, bool a_notify, bool a_narrate)
    {
        return VmCall::StaticIfPresent(kScript, "NarrateWake",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_hours),
                                                                 static_cast<std::int32_t>(a_notify ? 1 : 0),
                                                                 static_cast<std::int32_t>(a_narrate ? 1 : 0)),
                                       kWhat);
    }

    namespace
    {
        std::atomic<int> g_skyrimNet{ -1 };   // -1 = not asked since the last load, 0 = absent, 1 = present
    }

    bool SkyrimNetPresent()
    {
        const int known = g_skyrimNet.load();
        if (known >= 0) return known == 1;
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return false;
        // TypeIsValid asks the linker to LOAD the type (an installed-but-cold script answers true) - VmCall.h's reasoning
        const bool ok       = vm->TypeIsValid(RE::BSFixedString("SkyrimNetApi"));
        int        expected = -1;
        if (g_skyrimNet.compare_exchange_strong(expected, ok ? 1 : 0))
            logger::info("[KO] Papyrus script 'SkyrimNetApi' {} - timer wakes are {}", ok ? "found" : "NOT found",
                         ok ? "narrated through SkyrimNet" : "told by the HUD line only (aiNarrate 0)");
        return ok;
    }

    void ClearCache() { g_skyrimNet.store(-1); }

    // ---- U1 ----------------------------------------------------------------------------------------------------------------

    bool KnockOutU(RE::FormID a_id, bool a_notify, bool a_fromBleedout, bool a_noFdcRoll, FloatReply a_reply)
    {
        // u6 fix 2: 4 arguments - needs SBKO_Helper.pex u6 fix 2 (an older pex refuses the call: "not dispatched", logged)
        return GuardedCall(a_id, "KnockOutU",
                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_notify ? 1 : 0),
                                                     static_cast<std::int32_t>(a_fromBleedout ? 1 : 0),
                                                     static_cast<std::int32_t>(a_noFdcRoll ? 1 : 0)),
                           FloatOf(std::move(a_reply)));
    }

    bool PotionsOff(RE::FormID a_id, IntReply a_reply)
    {
        return VmCall::StaticIfPresentThen(kScript, "PotionsOff", RE::MakeFunctionArguments(AsInt(a_id)), kWhat,
                                           IntOf(std::move(a_reply)));
    }

    bool PotionsBack(RE::FormID a_id, std::int32_t a_mask)
    {
        return VmCall::StaticIfPresent(kScript, "PotionsBack",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_mask)), kWhat);
    }

    bool HoldHealthTo(RE::FormID a_id, float a_targetHealth, FloatReply a_reply)
    {
        return VmCall::StaticIfPresentThen(kScript, "HoldHealthTo",
                                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<float>(a_targetHealth)), kWhat,
                                           FloatOf(std::move(a_reply)));
    }

    bool HoldDown(RE::FormID a_id, DoneReply a_done)
    {
        return GuardedCall(a_id, "HoldDown", RE::MakeFunctionArguments(AsInt(a_id)), DoneOf(std::move(a_done)));
    }

    bool FinishWake(RE::FormID a_id, float a_savedHealRate, std::int32_t a_rank, std::int32_t a_potionMask, bool a_notify,
                    float a_wakeHealth, float a_wakeFrac, DoneReply a_done)
    {
        return GuardedCall(a_id, "FinishWake",
                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<float>(a_savedHealRate), static_cast<std::int32_t>(a_rank),
                                                     static_cast<std::int32_t>(a_potionMask), static_cast<std::int32_t>(a_notify ? 1 : 0),
                                                     static_cast<float>(a_wakeHealth), static_cast<float>(a_wakeFrac)),
                           DoneOf(std::move(a_done)));
    }

    bool TestDrop(RE::FormID a_id)
    {
        return VmCall::StaticIfPresent(kScript, "TestDrop", RE::MakeFunctionArguments(AsInt(a_id)), kWhat);
    }

    bool TestMeddle(RE::FormID a_id)
    {
        return VmCall::StaticIfPresent(kScript, "TestMeddle", RE::MakeFunctionArguments(AsInt(a_id)), kWhat);
    }

    // ---- U2a ---------------------------------------------------------------------------------------------------------------

    bool LetGo(RE::FormID a_id, float a_savedHealRate, std::int32_t a_potionMask, std::int32_t a_flags, float a_wakeHealth,
               float a_wakeFrac, DoneReply a_done)
    {
        return GuardedCall(a_id, "LetGo",
                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<float>(a_savedHealRate),
                                                     static_cast<std::int32_t>(a_potionMask), static_cast<std::int32_t>(a_flags),
                                                     static_cast<float>(a_wakeHealth), static_cast<float>(a_wakeFrac)),
                           DoneOf(std::move(a_done)));
    }

    bool ReAdopt(RE::FormID a_id, bool a_shove, float a_carriedHealRate, bool a_noFdcRoll, FloatReply a_reply)
    {
        // u6 fix 2: 4 arguments - needs SBKO_Helper.pex u6 fix 2 (an older pex refuses the call: "not dispatched", logged)
        return VmCall::StaticIfPresentThen(kScript, "ReAdopt",
                                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_shove ? 1 : 0),
                                                                     static_cast<float>(a_carriedHealRate),
                                                                     static_cast<std::int32_t>(a_noFdcRoll ? 1 : 0)),
                                           kWhat, FloatOf(std::move(a_reply)));
    }

    bool NarrateHealWake(RE::FormID a_id, bool a_notify, bool a_narrate)
    {
        return VmCall::StaticIfPresent(kScript, "NarrateHealWake",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_notify ? 1 : 0),
                                                                 static_cast<std::int32_t>(a_narrate ? 1 : 0)),
                                       kWhat);
    }

    bool SayTooInjured(RE::FormID a_id, bool a_notify)
    {
        return VmCall::StaticIfPresent(kScript, "SayTooInjured",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_notify ? 1 : 0)), kWhat);
    }

    // ---- u7a ------------------------------------------------------------------------------------------------------------------

    // U4: no reply - the roll is FDC's (its own Papyrus thread, its own Trace lines)
    bool FdcHit(RE::FormID a_id, const std::string& a_hit, bool a_deathOnly)
    {
        return VmCall::StaticIfPresent(kScript, "FdcHit",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<RE::BSFixedString>(a_hit.c_str()),
                                                                 static_cast<std::int32_t>(a_deathOnly ? 1 : 0)),
                                       kWhat);
    }

    // M2: one global, no reply
    bool NarrateWoken(RE::FormID a_id, std::int32_t a_kind, std::int32_t a_mode, bool a_notify, bool a_narrate)
    {
        return VmCall::StaticIfPresent(kScript, "NarrateWoken",
                                       RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_kind),
                                                                 static_cast<std::int32_t>(a_mode), static_cast<std::int32_t>(a_notify ? 1 : 0),
                                                                 static_cast<std::int32_t>(a_narrate ? 1 : 0)),
                                       kWhat);
    }

    bool ClearMark(RE::FormID a_id, std::int32_t a_rank)
    {
        // u2e: StaticIfPresentThen now (was StaticIfPresent) - the reply only ends the RefillGuard cover
        return GuardedCall(a_id, "ClearMark", RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_rank)), {});
    }

    bool SeverPresent()
    {
        return VmCall::ClassPresent(RE::BSScript::Internal::VirtualMachine::GetSingleton(), kSeverScript, kSeverWhat);
    }

    bool SeverGuard(RE::FormID a_id, IntReply a_reply)
    {
        return VmCall::StaticIfPresentThen(kSeverScript, "Guard", RE::MakeFunctionArguments(AsInt(a_id)), kSeverWhat,
                                           IntOf(std::move(a_reply)));
    }

    bool SeverUnguard(RE::FormID a_id, std::int32_t a_packed, IntReply a_reply)
    {
        return VmCall::StaticIfPresentThen(kSeverScript, "Unguard",
                                           RE::MakeFunctionArguments(AsInt(a_id), static_cast<std::int32_t>(a_packed)),
                                           kSeverWhat, IntOf(std::move(a_reply)));
    }
}
