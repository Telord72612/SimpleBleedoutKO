#pragma once
// PRESENCE-PROVEN STATIC CALLS - copied from the DDAB Base (its VmCall.h),
// log wording changed to SBKO's.
//
// THE RULE THIS FILE EXISTS FOR:
//     DispatchStaticCall into a class that is NOT INSTALLED is a null-deref CTD, not a log line.
// (A Base v1.0.0 user CTD'd inside the engine's own VirtualMachine::DispatchStaticCall when the target .pex was
// missing.) Calling an EXISTING class with the WRONG ARITY logs cleanly and carries on; a MISSING CLASS kills the process.
//
// `TypeIsValid` (IVirtualMachine vtable 0x10) asks the linker to LOAD the type, so a script that is installed but has
// never run still answers true. `GetScriptObjectTypeNoLoad*` would answer false for an installed-but-cold script - do
// not swap it in.
//
// `logger` is a per-TU alias, so this header spells SKSE::log out.
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace VmCall
{
    namespace detail
    {
        inline std::mutex                            mtx;
        inline std::unordered_map<std::string, bool> present;   // class name -> answer
        inline std::unordered_map<std::string, bool> logged;    // class name -> already said it
    }

    // Forget every cached answer. Called at kNewGame / kPostLoadGame (main.cpp): a cached FALSE taken during a load
    // storm must not outlive it, and a player can install the missing script and load a save without restarting.
    inline void ClearCache()
    {
        std::scoped_lock lk(detail::mtx);
        detail::present.clear();
        detail::logged.clear();
    }

    // Is `cls` a script the VM can load? Cached per session; the absence is logged ONCE.
    // `what` is the consequence, e.g. "the sbko console test does nothing".
    inline bool ClassPresent(RE::BSScript::Internal::VirtualMachine* vm, const char* cls, const char* what)
    {
        if (!vm || !cls || !*cls) return false;

        {
            std::scoped_lock lk(detail::mtx);
            const auto it = detail::present.find(cls);
            if (it != detail::present.end()) return it->second;
        }

        const bool ok = vm->TypeIsValid(RE::BSFixedString(cls));

        bool sayIt = false;
        {
            std::scoped_lock lk(detail::mtx);
            detail::present[cls] = ok;
            if (!ok && !detail::logged[cls]) {
                detail::logged[cls] = true;
                sayIt               = true;
            }
        }
        if (sayIt) {
            SKSE::log::info("[SBKO] Papyrus script '{}' is not installed - {}. Nothing was dispatched (a dispatch into a "
                            "missing script is a crash, not a log line). Install its .pex under Data/Scripts.",
                            cls, what ? what : "that call is skipped");
        }
        return ok;
    }

    // u7a (SBKO U7): the CACHED answer only - no VM call (safe inside a Papyrus native). -1 = not asked since ClearCache, 0 / 1.
    inline int ClassPresentCached(const char* cls)
    {
        if (!cls || !*cls) return 0;
        std::scoped_lock lk(detail::mtx);
        const auto it = detail::present.find(cls);
        return it == detail::present.end() ? -1 : (it->second ? 1 : 0);
    }

    // The guarded dispatch. Returns false (and says why, once) when the class is absent.
    // `args` is deleted on BOTH paths.
    inline bool StaticIfPresent(const char* cls, const char* fn,
                                RE::BSScript::IFunctionArguments* args, const char* what)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            SKSE::log::warn("[SBKO] no Papyrus VM - '{}.{}' not dispatched", cls ? cls : "?", fn ? fn : "?");
            delete args;
            return false;
        }
        if (!ClassPresent(vm, cls, what)) {
            delete args;
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb;
        const bool queued = vm->DispatchStaticCall(cls, fn, args, cb);
        delete args;
        // u6d fix round 2: a refusal is no longer silent (an old .pex without the function, a wrong argument
        // count) - one warn line per refused call, and the caller sees false ("not dispatched"), as with StaticIfPresentThen
        if (!queued)
            SKSE::log::warn("[SBKO] the Papyrus VM refused '{}.{}' (no such function, or a wrong argument count? - an older .pex) - "
                            "nothing ran", cls ? cls : "?", fn ? fn : "?");
        return queued;
    }

    // ---- M1: the return-value twin --------------------------------------------------------------------------------
    // Shape copied from the SkyrimNetMouthBridge plugin. The functor
    // runs ON A PAPYRUS VM THREAD when the dispatched stack finishes (a function with no return value still finishes,
    // with a None variable). ⛔ In SBKO a reply function never takes the knockout mutex and never calls AddTask: it
    // logs, or hands its work to TaskRelay::Add (safe from any thread).
    class Callback final : public RE::BSScript::IStackCallbackFunctor
    {
    public:
        using Fn = std::function<void(const RE::BSScript::Variable&)>;
        explicit Callback(Fn a_fn) : _fn(std::move(a_fn)) {}
        void operator()(RE::BSScript::Variable a_result) override
        {
            if (_fn) _fn(a_result);
        }
        void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

    private:
        Fn _fn;
    };

    // The guarded dispatch WITH a reply. Same presence guard as StaticIfPresent; `args` deleted on every path.
    // Returns false when nothing was dispatched (class absent, no VM, or the VM refused the call - e.g. no function of
    // that name): the reply will then NEVER run, and the caller must undo whatever it prepared.
    inline bool StaticIfPresentThen(const char* cls, const char* fn, RE::BSScript::IFunctionArguments* args,
                                    const char* what, Callback::Fn reply)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            SKSE::log::warn("[SBKO] no Papyrus VM - '{}.{}' not dispatched", cls ? cls : "?", fn ? fn : "?");
            delete args;
            return false;
        }
        if (!ClassPresent(vm, cls, what)) {
            delete args;
            return false;
        }
        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(new Callback(std::move(reply)));
        const bool queued = vm->DispatchStaticCall(cls, fn, args, cb);
        delete args;   // the VM copied the values into its stack
        if (!queued)
            SKSE::log::warn("[SBKO] the Papyrus VM refused '{}.{}' (no such function, or a wrong argument count?) - "
                            "nothing ran", cls ? cls : "?", fn ? fn : "?");
        return queued;
    }
}
