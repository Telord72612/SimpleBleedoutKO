#include "PCH.h"
#include "TestCmd.h"
#include "Knockout.h"
#include "Marks.h"    // m34: 'sbko marks ...'
#include "VmCall.h"

#include <cstring>   // u6h: strnlen

// The `sbko` console test (SBKO 2.0 first brick). Pattern copied from the VRCD AddOn
// (its RegisterConsole + Launch) and AIHands' HeelFix.
//
// Select an NPC in the console, type `sbko`: the Papyrus global SBKO_Test.Run(Int aiFormId) is dispatched with her
// FormID. The script does the timed part - this DLL only starts it. (Unchanged since test 3.)
//
// U1: two more zero-parameter commands, each on ITS OWN donor:
//   sbkou - selected NPC: not held + protected/essential -> SBKO_Helper.TestDrop (DamageActorValue to 0, no attacker = the
//           real bleedout path); held UNCONSCIOUS -> SBKO_Helper.TestMeddle ("foreign meddling": +50 % Health with no magic
//           effect, SetUnconscious false, Paralysis 0). The decision lives in Knockout::ConsoleU. U2b: that is another mod's
//           wake - SBKO puts her back down and the HUD says "<name> is too injured to wake up." (once settled).
//   sbkot - selected NPC held UNCONSCIOUS -> her timer runs out now (Knockout::ConsoleT).
// Every reply is ONE HUD line under 15 words.
//
// u6h (console tests: select an NPC, type a console command, and the test
// runs on her): `sbko <test> [option] [option]` - `sbko` gets three OPTIONAL string (kChar) parameters, the same parameter type and the
// same console compiler as the vanilla 'help' command (VR command table: every entry, 'help' and this donor included, compiles through
// SkyrimVR.exe +0x2FC8C0; 'help' = kChar optional + kInt optional - read from the exe's
// command table). `sbko` with no word is the old band test, unchanged. The tests (Knockout.cpp Console*): help, status, sleep,
// headko [blunt] [open], roll [ko], hitwake, toy, timer, refill. `sbkos` (the 4th donor, ToggleWaterJittering - free: 305 DLLs + every
// plugin source scanned, only SimpleBleedoutKO names it) = `sbko status` with NO word, the proven zero-parameter
// path - the fallback if the console does not hand the word over. Every target command refuses the player and a creature.
//
// u7a (build 2): + fdc, fdchit (U4), takeover [secs], holds, release [secs] (U7), woken, sleepstate (M2) - the same rules (one
// console line, the detail in the log, the game's own task code). The seconds word must be a number (anything else is refused).
//
// m34 (build 3): + 'sbko marks <word> [options]' (Marks::Console) - status | ie | slots | sync | paint <code> [stage] | alpha <code>
// <0-1> | clear [<code>|all] | wound <source> <part> [points] | heal | choke <seconds> | age <hours> | perf - on the selected actor or, with
// nothing selected, the PLAYER ("everyone" includes the player). 'sbko' now takes FIVE optional words (the same kChar type; 'sbko marks wound
// slapping face 30' is five) - the compiled-line reader and the typed-text reader both allow five.
namespace SBKO::TestCmd
{
    namespace
    {
        constexpr const char* kScript   = "SBKO_Test";
        constexpr const char* kFunction = "Run";   // Function Run(Int aiFormId) Global

        // Unused vanilla console commands, handed out IN ORDER: sbko takes the first one still carrying its vanilla name (so it
        // stays on ToggleHDRMultipleLuminance, as since test 1), sbkou the next, sbkot the next, sbkos (u6h) the last. Chosen
        // against: (1) every quoted name in other plugins' source (PPB, CollViz, HavokWig / HavokPatcher, PhysProbe, Standpoint, VRCD AddOn - none
        // uses these), (2) the SkyrimVR 1.4.15 command table dump
        // (all four present, 0 params, not a ref function; each name occurs exactly once in
        // SkyrimVR.exe), (3) a byte scan of all installed SKSE DLLs (every mod's SKSE/Plugins + Data/SKSE/Plugins): no DLL names any
        // of them. RE-CHECKED for U1: 300 DLLs byte-scanned, only SimpleBleedoutKO.dll names them; in plugin source only
        // this file. RE-CHECKED for u6h: 305 DLLs, the same answer. All four are renderer debug toggles (HDR luminance /
        // TAA blur / jitter) nobody types. A missing one is skipped (LocateConsoleCommand -> null; a renamed slot no longer answers to
        // its vanilla name).
        constexpr const char* kDonors[] = {
            "ToggleHDRMultipleLuminance",   // THML
            "TogglePostponeBlurryEffect",   // tpbe
            "ToggleAlphaEffectJittering",   // tej
            "ToggleWaterJittering",         // twj
        };

        // u6h: `sbko`'s three optional words (the engine keeps this pointer - a static array, never on a stack)
        RE::SCRIPT_PARAMETER kSbkoParams[] = {
            { "Test (help, status, sleep, headko, roll, hitwake, toy, timer, refill, fdc, fdchit, takeover, holds, release, woken, sleepstate, marks)",
              RE::SCRIPT_PARAM_TYPE::kChar, true },
            { "Option", RE::SCRIPT_PARAM_TYPE::kChar, true },
            { "Option", RE::SCRIPT_PARAM_TYPE::kChar, true },
            { "Option", RE::SCRIPT_PARAM_TYPE::kChar, true },   // m34: 'sbko marks wound <source> <part> <points>' needs five words
            { "Option", RE::SCRIPT_PARAM_TYPE::kChar, true },
        };
        constexpr std::size_t kMaxWords   = std::extent_v<decltype(kSbkoParams)>;
        constexpr std::size_t kMaxWordLen = 64;   // input sanity: a test name is one short word
        std::uint16_t         g_sbkoOpcode = 0;   // 0x100 + her index in the console table (set at Register) - the bytes' own check

        void Say(const char* a_cmd, const std::string& a_line)
        {
            if (auto* con = RE::ConsoleLog::GetSingleton())
                con->Print("%s", a_line.c_str());
            logger::info("[{}] console reply: {}", a_cmd, a_line);
        }

        std::string NameOf(RE::Actor* a_actor)
        {
            if (!a_actor) return "(none)";
            const char* n = a_actor->GetDisplayFullName();
            if (!n || !*n) n = a_actor->GetName();
            return (n && *n) ? std::string(n) : std::string("(no name)");
        }

        std::string Lower(std::string a_s)
        {
            for (auto& c : a_s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a_s;
        }

        // The shared front half: target = the command's own reference if the console gave one, else the console selection
        // (the NiPointer holds the selection alive for the whole call). Returns a non-player NPC - living and loaded unless
        // a_anyState ('sbko status' / 'sbkos' also read a dead or unloaded one) - or nullptr after saying why. u6h: a creature
        // (no ActorTypeNPC) is refused for every command.
        RE::Actor* Target(const char* a_cmd, RE::TESObjectREFR* a_thisObj, RE::NiPointer<RE::TESObjectREFR>& a_hold, bool a_anyState = false)
        {
            RE::TESObjectREFR* ref  = a_thisObj;
            const char*        from = "the command's reference";
            if (!ref) {
                a_hold = RE::Console::GetSelectedRef();
                ref    = a_hold.get();
                from   = "the console selection";
            }
            logger::info("[{}] typed: target from {} = {}", a_cmd, from,
                         ref ? std::format("0x{:08X}", ref->GetFormID()) : std::string("(nothing selected)"));
            if (!ref) {
                Say(a_cmd, std::format("{}: nothing selected - click an NPC in the console first", a_cmd));
                return nullptr;
            }
            auto* actor = ref->As<RE::Actor>();
            if (!actor) {
                Say(a_cmd, std::format("{}: the selected 0x{:08X} is not an actor - click an NPC", a_cmd, ref->GetFormID()));
                return nullptr;
            }
            if (actor->IsPlayerRef()) {
                Say(a_cmd, std::format("{}: that is you - click an NPC", a_cmd));
                return nullptr;
            }
            if (const char* why = Knockout::ConsoleNotNpc(actor)) {   // u6h: never a creature
                Say(a_cmd, std::format("{}: {} {} - nothing done", a_cmd, NameOf(actor), why));
                return nullptr;
            }
            if (a_anyState) return actor;
            if (!actor->Get3D()) {
                Say(a_cmd, std::format("{}: {} has no 3D loaded - pick an NPC near you", a_cmd, NameOf(actor)));
                return nullptr;
            }
            if (sbko::IsDeadNow(actor)) {
                Say(a_cmd, std::format("{}: {} is dead - pick a living NPC", a_cmd, NameOf(actor)));
                return nullptr;
            }
            return actor;
        }

        // ---- u6h: the words typed after `sbko` ----------------------------------------------------------------------------------
        // Two readers, NO engine call, every read bounded:
        //  (1) the compiled line - the console's compiler writes  opcode u16 | chunk size u16 | word count u16 | per word: length u16 +
        //      its characters  (CommonLib's SCRIPT_FUNCTION::ScriptData / StringChunk; SKSE-family ExtractArgs reads kChar the same way).
        //      Taken only when the bytes are self-consistent: printable words AND (our own opcode OR a size that adds up exactly).
        //  (2) the typed text - Script::text of the console's one-line script (the words after the first one).
        // (1) when it is consistent, else (2); neither, or both readable but DISAGREEING (u6h fix 1) = nothing is run (one console line +
        // a hex dump in the log).
        struct Words
        {
            bool                     ok = false;
            std::vector<std::string> words;
            std::string              how;
        };

        std::uint16_t U16(const std::uint8_t* a_p) { return static_cast<std::uint16_t>(a_p[0] | (a_p[1] << 8)); }

        bool Printable(std::string_view a_s)
        {
            return !a_s.empty() && std::all_of(a_s.begin(), a_s.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
        }

        bool FromBytes(const std::uint8_t* a_p, std::size_t a_avail, std::vector<std::string>& a_out, std::string& a_how)
        {
            if (!a_p || a_avail < 6) return false;
            const std::uint16_t op    = U16(a_p);
            const std::uint16_t chunk = U16(a_p + 2);
            const std::uint16_t n     = U16(a_p + 4);
            if (n > kMaxWords) return false;
            std::size_t              off = 6;
            std::vector<std::string> words;
            for (std::uint16_t i = 0; i < n; ++i) {
                if (off + 2 > a_avail) return false;
                const std::uint16_t len = U16(a_p + off);
                off += 2;
                if (len == 0 || len > kMaxWordLen || off + len > a_avail) return false;
                std::string w(reinterpret_cast<const char*>(a_p + off), len);
                if (!Printable(w)) return false;
                words.push_back(std::move(w));
                off += len;
            }
            const bool sizeOk = off == 4u + chunk || off == 6u + chunk;
            const bool opOk   = g_sbkoOpcode != 0 && op == g_sbkoOpcode;
            if (!sizeOk && !opOk) return false;
            a_out = std::move(words);
            a_how = std::format("the compiled line (opcode 0x{:X}{}, chunk size {}{}, {} word(s))", op, opOk ? "" : " - not the expected one",
                                chunk, sizeOk ? "" : " - does not add up", n);
            return true;
        }

        bool FromText(const RE::Script* a_script, std::vector<std::string>& a_out, std::string& a_how)
        {
            if (!a_script || !a_script->text) return false;
            const std::size_t len = strnlen(a_script->text, 512);
            if (len == 0 || len >= 512) return false;
            const std::string_view   line(a_script->text, len);
            std::vector<std::string> words;
            std::size_t              i = 0;
            bool                     first = true;
            while (i < line.size()) {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
                if (i >= line.size()) break;
                std::size_t e = i;
                while (e < line.size() && line[e] != ' ' && line[e] != '\t') ++e;
                std::string w(line.substr(i, e - i));
                i = e;
                if (first) {   // the command itself ("sbko", "SBKOTest", or "<ref>.sbko")
                    first = false;
                    continue;
                }
                if (w.size() >= 2 && w.front() == '"' && w.back() == '"') w = w.substr(1, w.size() - 2);
                if (w.empty()) continue;
                if (!Printable(w) || w.size() > kMaxWordLen) return false;
                words.push_back(std::move(w));
            }
            if (words.size() > kMaxWords) return false;
            a_out = std::move(words);
            a_how = std::format("the typed text '{}'", line);
            return true;
        }

        Words ReadWords(const RE::SCRIPT_FUNCTION::ScriptData* a_data, const RE::Script* a_script, std::uint32_t a_offset)
        {
            Words                r;
            const auto*          p = reinterpret_cast<const std::uint8_t*>(a_data);
            // the bound: the console script's own compiled data when it holds a_data; else 6 bytes + a short line's worth
            std::size_t          avail = 0;
            const char*          boundBy = "no bound known";
            if (p && a_script && a_script->data && a_script->header.dataSize > 0) {
                const auto* d0 = reinterpret_cast<const std::uint8_t*>(a_script->data);
                const auto* d1 = d0 + a_script->header.dataSize;
                if (p >= d0 && p < d1) {
                    avail   = static_cast<std::size_t>(d1 - p);
                    boundBy = "the script's data size";
                }
            }
            if (p && !avail) {
                avail   = 6 + kMaxWords * (2 + kMaxWordLen);   // a header + kMaxWords words at most (the console line's own size)
                boundBy = "a fixed cap";
            }
            std::string dump;
            for (std::size_t i = 0; p && i < (std::min)(avail, std::size_t{ 48 }); ++i) dump += std::format("{:02X} ", p[i]);
            std::vector<std::string> wb, wt;
            std::string              hb, ht;
            const bool okB = FromBytes(p, avail, wb, hb);
            const bool okT = FromText(a_script, wt, ht);
            logger::info("[sbko] typed: bytes [{}] ({} readable, {}; opcode offset {}) | from the bytes: {} | from the text: {}", dump, avail, boundBy,
                         a_offset, okB ? std::format("{} word(s)", wb.size()) : std::string("not readable"),
                         okT ? std::format("{} word(s) - {}", wt.size(), ht) : std::string("not readable"));
            if (okB) {
                if (okT) {
                    // u6h fix 1: two readers that DISAGREE (e.g. the bytes say 0 words, the text says 'status') = not
                    // understood - nothing runs (a disagreement must never run another test, the band test included)
                    bool same = wb.size() == wt.size();
                    for (std::size_t i = 0; same && i < wb.size(); ++i) same = _stricmp(wb[i].c_str(), wt[i].c_str()) == 0;
                    if (!same) {
                        logger::warn("[sbko] the compiled line ({} word(s)) and the typed text ({} word(s)) DISAGREE - nothing is run; send "
                                     "this line and the 'typed: bytes' line above",
                                     wb.size(), wt.size());
                        return r;   // ok false -> "could not be read"
                    }
                }
                r.ok    = true;
                r.words = std::move(wb);
                r.how   = hb;
            } else if (okT) {
                r.ok    = true;
                r.words = std::move(wt);
                r.how   = ht;
            }
            return r;
        }

        // the old `sbko` (no word): SBKO_Test.Run - unchanged since test 1
        void BandTest(RE::TESObjectREFR* a_thisObj)
        {
            RE::NiPointer<RE::TESObjectREFR> hold;
            auto*                            actor = Target("sbko", a_thisObj, hold);
            if (!actor) return;
            const std::string   name = NameOf(actor);
            const std::uint32_t id   = actor->GetFormID();
            // Papyrus Int = 32-bit signed; the FormID's bits are passed unchanged (an 0xFF... ref arrives negative,
            // Game.GetForm(aiFormId) still resolves it).
            logger::info("[sbko] {} (0x{:08X}) accepted - dispatching {}.{}(Int {})", name, id, kScript, kFunction,
                         static_cast<std::int32_t>(id));
            const bool sent = VmCall::StaticIfPresent(kScript, kFunction,
                                                      RE::MakeFunctionArguments(static_cast<std::int32_t>(id)),
                                                      "the sbko console test does nothing");
            if (!sent) {
                Say("sbko", "SBKO_Test script not installed");
                logger::info("[sbko] {}.{} NOT dispatched (script missing or no VM)", kScript, kFunction);
                return;
            }
            Say("sbko", std::format("SBKO test on {} (0x{:08X}): neck 10 s, then wrist 10 s, then ankle 10 s", name, id));   // unchanged since test 1
            logger::info("[sbko] {}.{} dispatched for 0x{:08X} - it runs once the console closes", kScript, kFunction, id);
        }

        bool ExecSbko(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData, RE::TESObjectREFR* a_thisObj,
                      RE::TESObjectREFR*, RE::Script* a_scriptObj, RE::ScriptLocals*, double&, std::uint32_t& a_opcodeOffsetPtr)
        {
            const Words in = ReadWords(a_scriptData, a_scriptObj, a_opcodeOffsetPtr);
            if (!in.ok) {
                Say("sbko", "sbko: the word after sbko could not be read - see log; sbkos = status");
                return true;
            }
            const std::string sub  = in.words.empty() ? std::string() : Lower(in.words[0]);
            std::vector<std::string> opts;
            for (std::size_t i = 1; i < in.words.size(); ++i) opts.push_back(Lower(in.words[i]));
            logger::info("[sbko] typed: test '{}'{} (read from {})", sub.empty() ? "(none - the band test)" : sub,
                         opts.empty() ? std::string() : std::format(", options '{}'{}", opts[0], opts.size() > 1 ? std::format(" '{}'", opts[1]) : ""),
                         in.how);
            if (sub.empty()) {
                BandTest(a_thisObj);
                return true;
            }
            // the options each test knows (anything else is refused - a typo must never run a different test)
            auto onlyOpts = [&](std::initializer_list<const char*> a_known) -> bool {
                for (const auto& o : opts)
                    if (std::none_of(a_known.begin(), a_known.end(), [&](const char* k) { return o == k; })) {
                        Say("sbko", std::format("sbko {}: unknown option '{}' - type sbko help", sub, o));
                        return false;
                    }
                return true;
            };
            auto has = [&](const char* a_o) { return std::find(opts.begin(), opts.end(), a_o) != opts.end(); };
            if (sub == "help" || sub == "?") {
                if (!onlyOpts({})) return true;   // u6h fix 1: extra words are refused like any unknown option
                logger::info("[sbko help] m34: {}", Marks::ConsoleHelpLine());
                Say("sbko help", Knockout::ConsoleHelp());
                return true;
            }
            if (sub == "marks") {   // m34: the selected actor (an NPC or anyone), or the player when nothing is selected
                RE::TESObjectREFR*               ref = a_thisObj;
                RE::NiPointer<RE::TESObjectREFR> hold;
                if (!ref) {
                    hold = RE::Console::GetSelectedRef();
                    ref  = hold.get();
                }
                RE::Actor* actor = ref ? ref->As<RE::Actor>() : RE::PlayerCharacter::GetSingleton();
                if (!actor) {
                    Say("sbko marks", std::format("sbko marks: the selected 0x{:08X} is not an actor", ref ? ref->GetFormID() : 0u));
                    return true;
                }
                logger::info("[sbko marks] typed: target {} (0x{:08X}){}", NameOf(actor), actor->GetFormID(),
                             ref ? "" : " - nothing selected: the player");
                Say("sbko marks", Marks::Console(actor, opts));
                return true;
            }
            if (sub == "toy") {   // the player's own hands - no NPC needed
                if (!onlyOpts({})) return true;   // u6h fix 1
                Say("sbko toy", Knockout::ConsoleToy());
                return true;
            }
            static constexpr const char* kTargetTests[] = { "status", "sleep",    "headko", "roll",    "hitwake", "timer", "refill",
                                                            "fdc",    "fdchit",   "takeover", "holds", "release", "woken", "sleepstate" };   // u7a: + 7
            if (std::none_of(std::begin(kTargetTests), std::end(kTargetTests), [&](const char* t) { return sub == t; })) {
                Say("sbko", std::format("sbko: unknown test '{}' - type sbko help", sub));
                return true;
            }
            const std::string cmd = "sbko " + sub;
            // u7a: takeover / release take ONE optional number of seconds (takeover 15 = VRTE's passout, release 27 = a 27 s hold)
            float secs = sub == "release" ? 27.0f : 15.0f;
            if (sub == "takeover" || sub == "release") {
                if (opts.size() > 1) {
                    Say(cmd.c_str(), std::format("{}: one number of seconds at most - type sbko help", cmd));
                    return true;
                }
                if (opts.size() == 1) {
                    try {
                        std::size_t used = 0;
                        const float v    = std::stof(opts[0], &used);
                        if (used != opts[0].size() || !(v > 0.0f) || v > 600.0f) throw std::invalid_argument("range");
                        secs = v;
                    } catch (...) {
                        Say(cmd.c_str(), std::format("{}: '{}' is not a number of seconds (1-600) - nothing done", cmd, opts[0]));
                        return true;
                    }
                }
            } else if (sub == "headko" ? !onlyOpts({ "blunt", "open" }) : sub == "roll" ? !onlyOpts({ "ko" }) : !onlyOpts({}))
                return true;
            RE::NiPointer<RE::TESObjectREFR> hold;
            auto* actor = Target(cmd.c_str(), a_thisObj, hold, sub == "status");
            if (!actor) return true;
            if (sub == "status")
                Say(cmd.c_str(), Knockout::ConsoleStatus(actor));
            else if (sub == "sleep")
                Say(cmd.c_str(), Knockout::ConsoleSleep(actor));
            else if (sub == "headko")
                Say(cmd.c_str(), Knockout::ConsoleHeadKO(actor, has("blunt"), has("open")));
            else if (sub == "roll")
                Say(cmd.c_str(), Knockout::ConsoleRoll(actor, has("ko")));
            else if (sub == "hitwake")
                Say(cmd.c_str(), Knockout::ConsoleHitWake(actor));
            else if (sub == "timer")
                Say(cmd.c_str(), Knockout::ConsoleT(actor, "sbko timer"));
            else if (sub == "refill")
                Say(cmd.c_str(), Knockout::ConsoleRefill(actor));
            else if (sub == "fdc")   // u7a
                Say(cmd.c_str(), Knockout::ConsoleFdc(actor));
            else if (sub == "fdchit")
                Say(cmd.c_str(), Knockout::ConsoleFdcHit(actor));
            else if (sub == "takeover")
                Say(cmd.c_str(), Knockout::ConsoleTakeOver(actor, secs));
            else if (sub == "holds")
                Say(cmd.c_str(), Knockout::ConsoleHolds(actor));
            else if (sub == "release")
                Say(cmd.c_str(), Knockout::ConsoleRelease(actor, secs));
            else if (sub == "woken")
                Say(cmd.c_str(), Knockout::ConsoleWoken(actor));
            else if (sub == "sleepstate")
                Say(cmd.c_str(), Knockout::ConsoleSleepState(actor));
            return true;
        }

        bool ExecSbkou(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*, RE::TESObjectREFR* a_thisObj,
                       RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, double&, std::uint32_t&)
        {
            RE::NiPointer<RE::TESObjectREFR> hold;
            if (auto* actor = Target("sbkou", a_thisObj, hold)) Say("sbkou", Knockout::ConsoleU(actor));
            return true;
        }

        bool ExecSbkot(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*, RE::TESObjectREFR* a_thisObj,
                       RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, double&, std::uint32_t&)
        {
            RE::NiPointer<RE::TESObjectREFR> hold;
            if (auto* actor = Target("sbkot", a_thisObj, hold)) Say("sbkot", Knockout::ConsoleT(actor, "sbkot"));
            return true;
        }

        // u6h: `sbko status` with no word to type (the proven zero-parameter path)
        bool ExecSbkos(const RE::SCRIPT_PARAMETER*, RE::SCRIPT_FUNCTION::ScriptData*, RE::TESObjectREFR* a_thisObj,
                       RE::TESObjectREFR*, RE::Script*, RE::ScriptLocals*, double&, std::uint32_t&)
        {
            RE::NiPointer<RE::TESObjectREFR> hold;
            if (auto* actor = Target("sbkos", a_thisObj, hold, true)) Say("sbkos", Knockout::ConsoleStatus(actor));
            return true;
        }

        struct Command
        {
            const char*                     longName;
            const char*                     shortName;
            const char*                     help;
            RE::SCRIPT_FUNCTION::Execute_t* exec;
            RE::SCRIPT_PARAMETER*           params    = nullptr;   // u6h: nullptr = zero parameters
            std::uint16_t                   numParams = 0;
        };
        const Command kCommands[] = {
            { "SBKOTest", "sbko", "SimpleBleedoutKO tests: click an NPC, then sbko <test> - 'sbko help' lists them", &ExecSbko, kSbkoParams,
              static_cast<std::uint16_t>(std::size(kSbkoParams)) },
            { "SBKOUnconscious", "sbkou", "SBKO U1 test (DANGEROUS): drops a protected NPC SBKO does not hold to 0 HP, or meddles with a held one",
              &ExecSbkou },
            { "SBKOTimer", "sbkot", "SBKO U1 test: select an NPC held unconscious - her timer runs out now", &ExecSbkot },
            { "SBKOStatus", "sbkos", "SBKO: select an NPC - everything SBKO knows about her, in SimpleBleedoutKO.log (= sbko status)", &ExecSbkos },
        };
    }

    void Register()
    {
        std::size_t next = 0;   // the next donor to try; every command takes its OWN slot
        for (const auto& cmd : kCommands) {
            RE::SCRIPT_FUNCTION* slot  = nullptr;
            const char*          donor = nullptr;
            std::string          missing;
            for (; next < std::size(kDonors); ++next) {
                slot = RE::SCRIPT_FUNCTION::LocateConsoleCommand(kDonors[next]);
                if (slot) {
                    donor = kDonors[next++];
                    break;
                }
                missing += std::string(missing.empty() ? "" : ", ") + kDonors[next];
            }
            if (!missing.empty())
                logger::warn("[{}] donor(s) not found (absent in this build, or already taken by another plugin): {}", cmd.shortName,
                             missing);
            if (!slot) {
                logger::error("[{}] no free console command left - '{}' is NOT available this session", cmd.shortName, cmd.shortName);
                continue;
            }
            slot->functionName        = cmd.longName;
            slot->shortName           = cmd.shortName;
            slot->helpString          = cmd.help;
            slot->referenceFunction   = false;   // the ref + zero-param combo dies in the console compiler (CollViz v2)
            if (cmd.params) {                    // u6h: sbko's optional words (kChar, as 'help')
                slot->numParams = cmd.numParams;
                slot->params    = cmd.params;
            } else {
                slot->SetParameters();           // zero parameters
            }
            slot->executeFunction     = cmd.exec;
            slot->conditionFunction   = nullptr;
            slot->editorFilter        = false;
            slot->invalidatesCellList = false;
            std::uint16_t opcode = 0;
            if (auto* first = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand(); first && slot >= first)
                opcode = static_cast<std::uint16_t>(RE::SCRIPT_FUNCTION::Commands::kConsoleOpBase + (slot - first));
            if (cmd.exec == &ExecSbko) g_sbkoOpcode = opcode;
            logger::info("[{}] console command '{}' ({}) registered on the unused vanilla '{}' (opcode 0x{:X}){}", cmd.shortName, cmd.shortName,
                         cmd.longName, donor, opcode,
                         cmd.params ? std::format(" with {} optional word(s) - 'sbko help' lists the tests", cmd.numParams) : std::string());
        }
    }
}
