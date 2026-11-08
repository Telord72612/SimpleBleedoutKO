#include "PCH.h"
#include "RefillGuard.h"
#include "Settings.h"

#include <array>
#include <cstring>

// See RefillGuard.h for the engine facts. Offsets and bytes: SkyrimVR.exe 1.4.15, read from the decrypted image
// (the signatures below are those bytes).
namespace SBKO::RefillGuard
{
    namespace
    {
        // Actor::StopCombat: `je +8 ; mov rcx,rsi ; call 0x5DECE0` - the call is the refill
        constexpr std::uintptr_t                kStopSigAt = 0x62E968;
        constexpr std::uintptr_t                kStopCall  = 0x62E96D;
        constexpr std::array<std::uint8_t, 10>  kStopSig{ 0x74, 0x08, 0x48, 0x8B, 0xCE, 0xE8, 0x6E, 0x03, 0xFB, 0xFF };
        // Actor::SetNoBleedoutRecovery: `je +0xD ; mov rcx,rbx ; add rsp,0x20 ; pop rbx ; jmp 0x5DECE0` - the tail jump is the refill
        constexpr std::uintptr_t                kFlagSigAt = 0x62C9AF;
        constexpr std::uintptr_t                kFlagJmp   = 0x62C9B9;
        constexpr std::array<std::uint8_t, 15>  kFlagSig{ 0x74, 0x0D, 0x48, 0x8B, 0xCB, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9, 0x22, 0x23, 0xFB, 0xFF };
        constexpr std::uintptr_t                kRefillFn  = 0x5DECE0;   // "Health back to full" (ResetHealthAndLimbs' Health half)

        // Skyrim.esm, by ID (the same forms Knockout.cpp uses): found once at Install, read-only afterwards
        constexpr RE::FormID kActorTypeNPC    = 0x00013794;   // SBKO's own scope: the base OR the race carries it
        constexpr RE::FormID kFollowerFaction = 0x0005C84E;   // CurrentFollowerFaction
        RE::BGSKeyword*      g_kwNPC          = nullptr;
        RE::TESFaction*      g_followerFaction = nullptr;

        std::mutex                            g_mtx;          // guards the two maps below: a lookup or a change, never a call
        std::unordered_map<RE::FormID, int>   g_inFlight;     // NPC -> SBKO calls on her still running
        std::unordered_map<RE::FormID, float> g_lastLogged;   // u2f: NPC -> her Health in her last combat-end "[refill]" line

        // u6h: what Install() did (written once at kDataLoaded, read by the console test 'sbko refill')
        std::atomic<bool> g_stopOn{ false }, g_flagOn{ false };
        const char*       g_installWhy = "not installed yet (kDataLoaded has not run)";

        // u2f (quieter; in one play session 50 of 78 lines were at full Health, 64 came from one combat flicker): a combat-end skip is
        // logged only when she is HURT and her Health moved >= 0.5 since her last such line. The skip itself happens every time.
        bool ShouldLogStop(RE::FormID a_id, float a_cur, float a_max)
        {
            if (a_max <= 0.0f || a_cur >= a_max - 0.05f) return false;   // full: the engine had nothing to restore
            std::scoped_lock lk(g_mtx);
            const auto it = g_lastLogged.find(a_id);
            if (it != g_lastLogged.end() && std::fabs(it->second - a_cur) < 0.5f) return false;
            g_lastLogged[a_id] = a_cur;
            return true;
        }

        bool InFlight(RE::FormID a_id)
        {
            std::scoped_lock lk(g_mtx);
            return g_inFlight.contains(a_id);
        }

        std::string Who(RE::Actor* a_actor)
        {
            const char* name = a_actor->GetName();
            return std::format("{} (0x{:08X})", name && *name ? name : "?", a_actor->GetFormID());
        }

        // the max = permanent + temporary (GetActorValueMax), the hold's own basis (Knockout.cpp HealthFractionMax)
        std::string HealthText(RE::Actor* a_actor)
        {
            auto* av = a_actor->AsActorValueOwner();
            if (!av) return "Health not read";
            const float cur = av->GetActorValue(RE::ActorValue::kHealth);
            const float mx  = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
            return mx > 0.0f ? std::format("Health stays {:.1f} of {:.1f} = {:.1f}%", cur, mx, 100.0f * cur / mx)
                             : std::format("Health stays {:.1f}", cur);
        }

        // modes 1 and 2 cover NPCs only (SBKO's own scope, ActorTypeNPC on the base or the race) - a protected
        // creature keeps the engine's refill (it may be what stands a kBleedout creature back up)
        bool IsNPC(RE::Actor* a_actor)
        {
            if (!g_kwNPC) return false;
            const auto* npc  = a_actor->GetActorBase();
            const auto* race = a_actor->GetRace();
            return (npc && npc->HasKeyword(g_kwNPC)) || (race && race->HasKeyword(g_kwNPC));
        }

        // SBKO's own follower test (Knockout.cpp IsFollower) - teammate OR CurrentFollowerFaction rank >= 0
        bool IsFollower(RE::Actor* a_actor)
        {
            if (a_actor->IsPlayerTeammate()) return true;
            return g_followerFaction && a_actor->GetFactionRank(g_followerFaction, false) >= 0;
        }

        // nullptr = let the engine refill her
        const char* StopReason(RE::Actor* a_actor)
        {
            if (!a_actor || a_actor->IsPlayerRef()) return nullptr;   // SBKO never acts on the player
            if (InFlight(a_actor->GetFormID())) return "an SBKO call is running on her (knock-out / hold): she is held at her real Health";
            const int mode = Settings::Get().combatEndRefill;
            if (mode <= 0 || !IsNPC(a_actor)) return nullptr;
            if (mode >= 2) return "iCombatEndRefill 2: every essential / protected NPC stays hurt";
            if (IsFollower(a_actor)) return "iCombatEndRefill 1: a follower stays hurt";
            return nullptr;
        }

        struct StopCombatRefill
        {
            static void thunk(RE::Actor* a_actor)
            {
                if (const char* why = StopReason(a_actor)) {
                    auto*       av  = a_actor->AsActorValueOwner();
                    const float cur = av ? av->GetActorValue(RE::ActorValue::kHealth) : 0.0f;
                    if (av && ShouldLogStop(a_actor->GetFormID(), cur, a_actor->GetActorValueMax(RE::ActorValue::kHealth)))
                        logger::info("[refill] {} left combat - the engine's full heal SKIPPED ({}); {}", Who(a_actor), why,
                                     HealthText(a_actor));
                    return;
                }
                func(a_actor);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        struct FlagOffRefill
        {
            // reached by a TAIL jump (SetNoBleedoutRecovery's frame is already gone): returning goes straight to its caller
            static void thunk(RE::Actor* a_actor)
            {
                if (a_actor && !a_actor->IsPlayerRef() && InFlight(a_actor->GetFormID())) {
                    logger::info("[refill] {} NoBleedoutRecovery cleared by SBKO (wake / let-go / clear) - the engine's full heal "
                                 "SKIPPED; {} at that moment (a wake or a let-go then raises a living NPC to fWakeHealthFraction)",
                                 Who(a_actor), HealthText(a_actor));
                    return;
                }
                func(a_actor);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        template <std::size_t N>
        bool BytesAre(std::uintptr_t a_base, std::uintptr_t a_rva, const std::array<std::uint8_t, N>& a_sig, std::string& a_seen)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(a_base + a_rva);
            if (std::memcmp(p, a_sig.data(), N) == 0) return true;
            for (std::size_t i = 0; i < N; ++i) a_seen += std::format("{}{:02X}", i ? " " : "", p[i]);
            return false;
        }
    }

    void Install()
    {
        const int mode = Settings::Get().combatEndRefill;
        if (!REL::Module::IsVR()) {
            g_installWhy = "SE / AE: the patch is for Skyrim VR 1.4.15 only";
            logger::info("[refill] not installed: the engine-heal patch is for Skyrim VR 1.4.15 only - the engine refills essential / "
                         "protected NPCs as usual here");
            return;
        }
        if (const auto ver = REL::Module::get().version(); ver != REL::Version(1, 4, 15, 0)) {
            g_installWhy = "this Skyrim VR is not 1.4.15 (the addresses are for 1.4.15)";
            logger::warn("[refill] not installed: Skyrim VR {} is not 1.4.15 (the addresses are for 1.4.15) - the engine refills as usual",
                         ver.string("."));
            return;
        }
        g_kwNPC           = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);
        g_followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(kFollowerFaction);
        if (!g_kwNPC)
            logger::warn("[refill] keyword ActorTypeNPC (0x{:08X}) not found - iCombatEndRefill 1 / 2 cover nobody (only SBKO's own calls)",
                         kActorTypeNPC);
        const std::uintptr_t base = REL::Module::get().base();
        std::string          seenStop, seenFlag;
        const bool           okStop = BytesAre(base, kStopSigAt, kStopSig, seenStop);
        const bool           okFlag = BytesAre(base, kFlagSigAt, kFlagSig, seenFlag);
        if (!okStop && !okFlag) {
            g_installWhy = "neither site has the expected bytes (another mod patched them?)";
            logger::warn("[refill] NOT installed: neither site has the expected bytes (another mod patched them?) - StopCombat +0x{:X}: {} "
                         "| SetNoBleedoutRecovery +0x{:X}: {}", kStopSigAt, seenStop, kFlagSigAt, seenFlag);
            return;
        }
        g_installWhy = okStop && okFlag ? "" : !okStop ? "the StopCombat site has other bytes (another mod patched it?)"
                                                       : "the SetNoBleedoutRecovery site has other bytes (another mod patched it?)";
        SKSE::AllocTrampoline(64);   // ONE allocation for both patches (a second one could free the first under a written call)
        auto& tramp = SKSE::GetTrampoline();

        if (okStop) {
            StopCombatRefill::func   = tramp.write_call<5>(base + kStopCall, StopCombatRefill::thunk);
            g_stopOn.store(true);   // u6h
            const std::uintptr_t was = StopCombatRefill::func.address();
            logger::info("[refill] combat-end patch installed: Actor::StopCombat's full heal (SkyrimVR.exe+0x{:X}, was a call to +0x{:X}{}) is "
                         "skipped for {}, and for any NPC SBKO is knocking out",
                         kStopCall, was - base, was == base + kRefillFn ? "" : " - NOT the expected +0x5DECE0",
                         mode >= 2 ? "every essential / protected NPC (iCombatEndRefill 2; creatures keep it)"
                         : mode == 1 ? "followers (teammates or CurrentFollowerFaction, iCombatEndRefill 1)"
                                     : "nobody else (iCombatEndRefill 0: the engine refills as usual)");
        } else {
            logger::warn("[refill] combat-end patch NOT installed: Actor::StopCombat +0x{:X} has {} instead of the expected bytes (another "
                         "mod patched it?) - the engine refills at combat end as usual", kStopSigAt, seenStop);
        }

        if (okFlag) {
            FlagOffRefill::func      = tramp.write_branch<5>(base + kFlagJmp, FlagOffRefill::thunk);
            g_flagOn.store(true);   // u6h
            const std::uintptr_t was = FlagOffRefill::func.address();
            logger::info("[refill] wake patch installed: SetNoBleedoutRecovery(false)'s full heal (SkyrimVR.exe+0x{:X}, was a jump to +0x{:X}{}) "
                         "is skipped while SBKO's own wake / let-go / clear runs on her",
                         kFlagJmp, was - base, was == base + kRefillFn ? "" : " - NOT the expected +0x5DECE0");
        } else {
            logger::warn("[refill] wake patch NOT installed: SetNoBleedoutRecovery +0x{:X} has {} instead of the expected bytes - a wake "
                         "out of combat refills her to full", kFlagSigAt, seenFlag);
        }
    }

    void Enter(RE::FormID a_id)
    {
        std::scoped_lock lk(g_mtx);
        ++g_inFlight[a_id];
    }

    void Leave(RE::FormID a_id)
    {
        std::scoped_lock lk(g_mtx);
        if (auto it = g_inFlight.find(a_id); it != g_inFlight.end() && --it->second <= 0) g_inFlight.erase(it);
    }

    void Reset()
    {
        std::size_t n = 0;
        {
            std::scoped_lock lk(g_mtx);
            n = g_inFlight.size();
            g_inFlight.clear();
            g_lastLogged.clear();   // u2f: the first hurt skip after a load is logged again
        }
        if (n) logger::info("[refill] {} NPC(s) with an SBKO call still running at the load - forgotten (those calls die with the old game)", n);
    }

    // u6h: the console test 'sbko refill' (reads only)
    InstallState GetInstallState()
    {
        return InstallState{ .stopPatch = g_stopOn.load(), .flagPatch = g_flagOn.load(), .why = g_installWhy };
    }

    const char* SkipReason(RE::Actor* a_actor) { return StopReason(a_actor); }   // the thunk's own decision

    bool CallRunning(RE::FormID a_id) { return InFlight(a_id); }
}
