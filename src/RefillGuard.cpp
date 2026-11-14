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

        // ---- 2.1.0: Skyrim SE / AE. Every site is FOUND at load inside its own function (Address Library ids) and accepted only
        // when its call / jump goes to the heal routine itself; anything else = that patch is not installed (fail closed). Read from
        // SkyrimSE.exe 1.5.97, 1.6.1170 and 1.7.104: the same code on all three,
        // except that AE moved the wake refill into a small helper that SetNoBleedoutRecovery tail-jumps to.
        constexpr std::int16_t kAny = -1;   // any byte
        constexpr std::int16_t kRex = -2;   // a REX.W prefix: 48 or 49
        constexpr std::int16_t kRcx = -3;   // the ModRM of `mov rcx, r64`: C8..CF (SE uses rsi, AE rdi)
        // `and eax,0x1E00000 ; cmp eax,0xE00000 ; je +8 ; mov rcx,r ; call heal` - StopCombat's refill (not EssentialDown, then heal)
        constexpr std::array<std::int16_t, 20> kStopPat{ 0x25, 0x00, 0x00, 0xE0, 0x01, 0x3D, 0x00, 0x00, 0xE0, 0x00,
                                                         0x74, 0x08, kRex, 0x8B, kRcx, 0xE8, kAny, kAny, kAny, kAny };
        constexpr std::size_t                  kStopOp = 15;   // the E8
        // the same test, then `je +0xD ; mov rcx,r ; add rsp,0x20 ; pop rbx ; jmp heal` - the wake refill (SE: in SetNoBleedoutRecovery,
        // AE: in its helper)
        constexpr std::array<std::int16_t, 25> kFlagPat{ 0x25, 0x00, 0x00, 0xE0, 0x01, 0x3D, 0x00, 0x00, 0xE0, 0x00, 0x74, 0x0D, kRex,
                                                         0x8B, kRcx, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xE9, kAny, kAny, kAny, kAny };
        constexpr std::size_t                  kFlagOp = 20;   // the E9
        // AE: SetNoBleedoutRecovery's `test al,al ; jne +0xD ; mov rcx,r ; add rsp,0x20 ; pop rbx ; jmp helper`
        constexpr std::array<std::int16_t, 17> kStepPat{ 0x84, 0xC0, 0x75, 0x0D, kRex, 0x8B, kAny, 0x48, 0x83,
                                                         0xC4, 0x20, 0x5B, 0xE9, kAny, kAny, kAny, kAny };
        constexpr std::size_t                  kStepOp = 12;   // the E9
        constexpr std::array<std::uint8_t, 6>  kHealHead{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };   // `push rbx ; sub rsp,0x20` (SE + AE)

        template <std::size_t N>
        std::vector<std::uintptr_t> FindAll(std::uintptr_t a_from, std::size_t a_len, const std::array<std::int16_t, N>& a_pat)
        {
            std::vector<std::uintptr_t> hits;
            const auto*                 p = reinterpret_cast<const std::uint8_t*>(a_from);
            for (std::size_t i = 0; i + N <= a_len; ++i) {
                bool ok = true;
                for (std::size_t k = 0; k < N && ok; ++k) {
                    const std::int16_t w = a_pat[k];
                    const std::uint8_t b = p[i + k];
                    ok = w == kAny ? true
                       : w == kRex ? (b == 0x48 || b == 0x49)
                       : w == kRcx ? (b >= 0xC8 && b <= 0xCF)
                                   : b == static_cast<std::uint8_t>(w);
                }
                if (ok) hits.push_back(a_from + i);
            }
            return hits;
        }

        // the destination of the E8 / E9 rel32 instruction at a_op
        std::uintptr_t Rel32Target(std::uintptr_t a_op)
        {
            std::int32_t rel = 0;
            std::memcpy(&rel, reinterpret_cast<const void*>(a_op + 1), sizeof(rel));
            return a_op + 5 + static_cast<std::intptr_t>(rel);
        }

        // The ONE match of a_pat in [a_from, a_from + a_len) whose call / jump goes to a_target -> the address of that E8 / E9;
        // 0 = not exactly one match, or it goes elsewhere (a_why says which)
        template <std::size_t N>
        std::uintptr_t FindSite(std::uintptr_t a_from, std::size_t a_len, const std::array<std::int16_t, N>& a_pat, std::size_t a_op,
                                std::uintptr_t a_target, std::uintptr_t a_base, std::string& a_why)
        {
            const auto hits = FindAll(a_from, a_len, a_pat);
            if (hits.size() != 1) {
                a_why = std::format("{} matches of its byte pattern in the function (exactly 1 expected)", hits.size());
                return 0;
            }
            const std::uintptr_t op = hits[0] + a_op;
            const std::uintptr_t to = Rel32Target(op);
            if (to != a_target) {
                a_why = std::format("its jump goes to +0x{:X}, not to +0x{:X} (another mod patched it?)", to - a_base, a_target - a_base);
                return 0;
            }
            return op;
        }

        void InstallFlat(int a_mode)
        {
            const bool           ae   = REL::Module::IsAE();
            const std::uintptr_t base = REL::Module::get().base();
            const std::uintptr_t heal = REL::RelocationID(36350, 37340).address();   // "Health back to full" (the routine both sites call)
            const std::uintptr_t stop = REL::RelocationID(37613, 38566).address();   // Actor::StopCombat
            const std::uintptr_t snbr = REL::RelocationID(37583, 38533).address();   // Actor::SetNoBleedoutRecovery
            if (std::memcmp(reinterpret_cast<const void*>(heal), kHealHead.data(), kHealHead.size()) != 0) {
                g_installWhy = "the heal routine has other bytes than expected (an unknown game build, or another mod hooked it?)";
                logger::warn("[refill] NOT installed: the heal routine (SkyrimSE.exe+0x{:X}) does not start with the expected bytes - an "
                             "unknown game build, or another mod hooked it? The engine refills essential / protected NPCs as usual",
                             heal - base);
                return;
            }
            // log only: the Character vtable's StopCombat (slot 0xE5). Another mod may have replaced the slot; SBKO patches the engine's
            // own body, which a slot replacement leaves alone.
            const auto*          vt   = reinterpret_cast<const std::uintptr_t*>(RE::VTABLE_Character[0].address());
            const std::uintptr_t slot = vt ? vt[0xE5] : 0;
            if (slot != stop)
                logger::info("[refill] note: the Character vtable's StopCombat slot points to +0x{:X}, not the engine's +0x{:X} (another mod hooks "
                             "it) - SBKO patches the engine's body, which still runs behind that hook", slot - base, stop - base);

            std::string          whyStop, whyFlag;
            const std::uintptr_t stopSite = FindSite(stop, 0x400, kStopPat, kStopOp, heal, base, whyStop);
            std::uintptr_t       flagSite = 0;
            if (!ae) {
                flagSite = FindSite(snbr, 0x80, kFlagPat, kFlagOp, heal, base, whyFlag);
            } else {
                const std::uintptr_t helper = REL::ID(38680).address();   // AE: the wake refill's own little function
                std::string          whyStep;
                if (FindSite(snbr, 0x80, kStepPat, kStepOp, helper, base, whyStep) == 0)
                    whyFlag = std::format("SetNoBleedoutRecovery's jump to its helper: {}", whyStep);
                else
                    flagSite = FindSite(helper, 0x40, kFlagPat, kFlagOp, heal, base, whyFlag);
            }
            if (!stopSite && !flagSite) {
                g_installWhy = "neither site was found as expected (another mod patched them, or an unknown game build)";
                logger::warn("[refill] NOT installed: StopCombat (SkyrimSE.exe+0x{:X}): {} | SetNoBleedoutRecovery (+0x{:X}): {} - the engine "
                             "refills essential / protected NPCs as usual", stop - base, whyStop, snbr - base, whyFlag);
                return;
            }
            g_installWhy = stopSite && flagSite ? "" : !stopSite ? "the StopCombat site was not found as expected"
                                                                 : "the SetNoBleedoutRecovery site was not found as expected";
            SKSE::AllocTrampoline(64);   // ONE allocation for both patches (a second one could free the first under a written call)
            auto& tramp = SKSE::GetTrampoline();
            if (stopSite) {
                StopCombatRefill::func = tramp.write_call<5>(stopSite, StopCombatRefill::thunk);
                g_stopOn.store(true);
                logger::info("[refill] combat-end patch installed: Actor::StopCombat's full heal (SkyrimSE.exe+0x{:X}, a call to the heal routine "
                             "+0x{:X}) is skipped for {}, and for any NPC SBKO is knocking out",
                             stopSite - base, heal - base,
                             a_mode >= 2 ? "every essential / protected NPC (iCombatEndRefill 2; creatures keep it)"
                             : a_mode == 1 ? "followers (teammates or CurrentFollowerFaction, iCombatEndRefill 1)"
                                           : "nobody else (iCombatEndRefill 0: the engine refills as usual)");
            } else {
                logger::warn("[refill] combat-end patch NOT installed: Actor::StopCombat (SkyrimSE.exe+0x{:X}): {} - the engine refills at "
                             "combat end as usual", stop - base, whyStop);
            }
            if (flagSite) {
                FlagOffRefill::func = tramp.write_branch<5>(flagSite, FlagOffRefill::thunk);
                g_flagOn.store(true);
                logger::info("[refill] wake patch installed: SetNoBleedoutRecovery(false)'s full heal (SkyrimSE.exe+0x{:X}{}, a jump to the heal "
                             "routine +0x{:X}) is skipped while SBKO's own wake / let-go / clear runs on her",
                             flagSite - base, ae ? " in its helper" : "", heal - base);
            } else {
                logger::warn("[refill] wake patch NOT installed: SetNoBleedoutRecovery (SkyrimSE.exe+0x{:X}): {} - a wake out of combat refills "
                             "her to full", snbr - base, whyFlag);
            }
        }
    }

    void Install()
    {
        const int mode    = Settings::Get().combatEndRefill;
        g_kwNPC           = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);
        g_followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(kFollowerFaction);
        if (!g_kwNPC)
            logger::warn("[refill] keyword ActorTypeNPC (0x{:08X}) not found - iCombatEndRefill 1 / 2 cover nobody (only SBKO's own calls)",
                         kActorTypeNPC);
        if (!REL::Module::IsVR()) {   // 2.1.0: Skyrim SE / AE
            InstallFlat(mode);
            return;
        }
        if (const auto ver = REL::Module::get().version(); ver != REL::Version(1, 4, 15, 0)) {
            g_installWhy = "this Skyrim VR is not 1.4.15 (the addresses are for 1.4.15)";
            logger::warn("[refill] not installed: Skyrim VR {} is not 1.4.15 (the addresses are for 1.4.15) - the engine refills as usual",
                         ver.string("."));
            return;
        }
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
