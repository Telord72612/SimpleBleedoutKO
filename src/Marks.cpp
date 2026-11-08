#include "PCH.h"
#include "Marks.h"
#include "Knockout.h"   // IsHealEffect (the one heal rule)
#include "Settings.h"
#include "TaskRelay.h"
#include "VmCall.h"

#include <map>
#include <nlohmann/json.hpp>

// =====================================================================================================================================
// m34 (build 3) THE SKIN MARKS: the IE read, the table (marks.json),
// the slots and the paint recipe.
//
// WHAT IT DOES
//   * M4, the IE mirror (Injury Engine puts an injury on, SBKO puts a mark on it; IE removes the injury, SBKO removes it): every humanoid
//     Injury Engine (IE) wounds - the player too (bMarkPlayer), never a child, no FACE marks on a Khajiit / Argonian head (bBeastFaceMarks 0) -
//     gets the Collygon mark of her worst wound per body part (marks.json: source -> family -> part -> code; stage -> alpha on the DDAB ladder
//     0.50 / 0.75 / 1.00, floor 0.30). The mark goes on when IE lists the wound (GetStateJson) and comes off when IE stops listing it (or
//     says "healed"). Burns / frost / shock have no art: unmarked (a 'gap' line; bStandIns 1 = the stand-in bruises).
//   * M3, the painter: SBKO_Marks.Apply(Int, String ops) runs RaceMenu's NiOverride for her - AddOverlays only when she has none + Wait(0.5),
//     NO QueueNiNodeUpdate, the texture (key 9) once per mark life, then the look (keys 0 / 1 / 2 / 3 / 7 / 8: matte, tint,
//     alpha); a stage change writes key 7 / 8 only; a clear writes default.dds then removes our keys. Never another mod's slot; never
//     RemoveOverlays. SlaveTats paths get a leading backslash (SlaveTats NG then never claims / blanks the slot).
//   * THE CHOKE HAND MARK (a hand mark around the neck by how long the choke was held, healed with time or a
//     healing spell, like the skin mark of shackles in DDAB): Collygon "Bruise - Choked Two Hands", Face [Ovl16], look C. Seconds held
//     < 20 -> 0.50, 20-30 -> 0.75, >= 30 -> 1.00 (sChokeSteps); life 12 game h per step (fChokeHoursPerStep); fades by thirds (its strength
//     -> halfway to the 0.30 floor -> the floor with Collygon's "Faded" texture -> cleared); each healing spell / potion takes half of the life
//     that is LEFT (fChokeHealShare; DDAB's MarksHealLocked: the whole life moves earlier, so the third jumps forward); one cast = one heal
//     (DDAB's stream rule, 1.5 s concentration / 0.5 s otherwise - how one cast is recognised, not a cooldown). A re-choke keeps the stronger
//     alpha and the longer remaining life. A choke let go BEFORE the passout: bChokeMarkBeforePassout 1 = the floor
//     alpha (0.30) with a life of fChokeHoursPerStep x seconds / fChokePassoutSeconds (a 10 s choke -> 8 game h); 0 = no mark.
//
// WHEN IT READS (no new timer: the check's 1.5 s tail, IE's ping, the sinks)
//   IE's mod-callback ping Injury_Engine_Injury (names nobody) + the actors a hit / heal named since the last two checks -> read those; a ping
//   no hit / heal explains -> the sweep: IE's HasOpenWounds for every loaded humanoid with no record (a marked one is read in full), a full read
//   only for a yes (m34d: all of them asked in ONE Papyrus call, SBKO_Marks.IEWounded). A marked actor: her Health moved a whole percent
//   since her last read (real wounds fade with regen - IE sends no ping), a whole game hour passed (torture marks drain), her 3D loading, kPostLoadGame + 5 s (the reconcile: read, then op 1 for every mark).
//   One read and one write in flight per actor ("in flight", not a cooldown). A detach drops her live state (the record and the skin stay).
//
// THREADS: sinks copy + flag (g_inMtx / atomics) or TaskRelay::Add; every decision runs in an SKSE task under g_mtx (this file's own; held only
// for map work, never around a dispatch; Knockout's g_mtx -> this one is the only allowed nesting); replies run on a VM thread and only
// TaskRelay::Add. Co-save record 'MARK' v2 (m34c; v1 read) (Knockout::OnSave writes it after 'CHOK'; FE ids get their plugin name in 'LREF').
//
// m34c FIX ROUND 1 (a review): the worst wound per part is ranked only among wounds that paint
// there (Plan); a record is checked to still be HER (IdentityCheck: sex, a created id's base) and an FF record goes when its actor is gone;
// ownership = the ONE slot SBKO wrote (Mark::slot, carried in every op, co-saved) - never "a slot holding the same file"; beast races get no
// choke mark / bands; one choke mark per neck and replies booked by a mark uid; the painter's in-flight flag survives a detach; the sweep line
// only when it found someone; IE's DLL missing / silent = one line, mirror off; SBKO_Marks.pex missing = one line, nothing collected.
//
// m34d PERF: a live [PERF] line showed
// IE pinging ~40 times a second while wounds exist, so the sweep ran at almost every check and dispatched one Papyrus call PER loaded humanoid
// (~26 -> ~17 dispatches a second). Now ONE call per sweep: SBKO_Marks.IEWounded(Int[] ids) asks Injury Engine's HasOpenWounds for every
// candidate on one stack and hands the array back with 0 for each one without an open wound; the yes ones are read in full, as before. The
// same candidates, the same in-flight set, the same reads - no new timer / cap. An older SBKO_Marks.pex (no IEWounded) -> one line, and the
// per-actor calls (IEHasWounds) for the session. [PERF] counts "sweep calls" (the dispatches) next to the asks.
// =====================================================================================================================================
namespace SBKO::Marks
{
    namespace
    {
        using json  = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        constexpr const char*   kScript     = "SBKO_Marks";
        constexpr const char*   kIEScript   = "Injury_Engine_Engine";
        constexpr const char*   kWhat       = "no skin mark is painted (install SBKO_Marks.pex)";
        constexpr const char*   kIEWhat     = "the skin marks do not follow Injury Engine's wounds (Injury Engine is not installed) - the choke "
                                              "mark still works";
        constexpr std::uint32_t kRecVersion = 2;   // m34c: v2 = + her base NPC per actor; v1 is still read
        constexpr RE::FormID    kActorTypeNPC = 0x00013794;   // Skyrim.esm keyword (Knockout's IsNPC uses the same id)
        constexpr RE::FormID    kPlayerId     = 0x14;
        constexpr float         kEps          = 0.001f;
        constexpr double        kStreamConcS  = 1.5;   // DDAB kHealStreamS: one concentration cast = one heal while it keeps arriving
        constexpr double        kStreamFfS    = 0.5;   // DDAB kHealStreamFfS: a fire-and-forget spell / a potion's effects in one moment

        enum Area : int { kBody = 0, kFace = 1, kHands = 2, kFeet = 3 };
        enum PartCls : int { kPFace = 0, kPBack = 1, kPArm = 2, kPLeg = 3 };
        enum Kind : std::uint8_t { kIE = 0, kChoke = 1, kManual = 2 };

        const char* AreaName(int a_area)
        {
            switch (a_area) {
                case kBody:  return "Body";
                case kFace:  return "Face";
                case kHands: return "Hands";
                case kFeet:  return "Feet";
                default:     return "?";
            }
        }
        const char* KindName(Kind a_k) { return a_k == kIE ? "IE" : a_k == kChoke ? "choke" : "manual"; }

        // ---- the table (marks.json): written ONCE at Install (kDataLoaded), read-only afterwards ---------------------------------------
        struct FileDef
        {
            int         area  = -1;
            bool        sided = false;
            int         tint  = -1;       // the code's own tint (-1 = the family's)
            std::string path[2][2];       // [0 = L or unsided, 1 = R][0 = female, 1 = male], relative to Data/Textures
            bool        ok[2][2] = {};    // the file is there and its DDS header is sane (DDAB's check)
        };
        struct Spec
        {
            std::vector<std::string> codes;   // one code, or the list her stable pick chooses from
            int                      at    = 0;       // the lowest stage it shows at (0 light / 1 medium / 2 severe)
            bool                     other = false;   // the other side from her stable pick (the slap's "other cheek")
            int                      tint  = -1;      // -1 = the code's, else the family's
            bool                     blood = false;   // a blood layer: one step lighter (face blood layers 0.50 / 0.75)
        };
        struct Family
        {
            int               tint = 0xFFFFFF;
            std::vector<Spec> part[4];   // face, back, arm, leg
        };
        struct SourceDef
        {
            std::string fam[3];     // per stage (light / medium / severe); "" = no art
            std::string stand[3];   // bStandIns 1: the stand-in family per stage
            bool        real = true;
        };
        struct Table
        {
            std::unordered_map<std::string, FileDef>   files;
            std::unordered_map<std::string, Family>    families;
            std::unordered_map<std::string, SourceDef> sources;
            std::unordered_map<std::string, int>       tints;
            std::string chokeMark  = "N1";
            std::string chokeFaded = "N1F";
            int         chokeTint  = 0x5A6EC8;
            int         filesOk = 0, filesBad = 0;
        };
        Table g_tab;

        std::atomic<bool>          g_on{ false };
        std::atomic<std::uint32_t> g_gen{ 0 };       // bumped at kPreLoadGame / revert: a reply of the old game is dropped
        // m34c: SBKO_Marks.pex is missing (the check found no script) - the hit sink stops naming actors (cleared at a load)
        std::atomic<bool>          g_painterAbsent{ false };
        // m34c: Injury_Engine_Engine.pex is installed but its natives did not answer (an EMPTY GetStateJson for an actor in
        // memory, twice in a row - IE's DLL did not load) -> the IE mirror is off for this session (one warn line; cleared at a load)
        std::atomic<bool>          g_ieDead{ false };
        std::atomic<int>           g_ieEmpty{ 0 };
        bool                       g_ieDll = false;               // Injury_Engine.dll is loaded in this process (read once at kDataLoaded)
        std::atomic<bool>          g_ieDllSaid{ false }, g_painterSaid{ false };   // their one line each (cleared at a load)
        // m34d: the installed SBKO_Marks.pex has no IEWounded (an m34c or older .pex next to this DLL) -> the sweep asks one actor per call
        // (IEHasWounds, the m34c way) for this session; set (with its one line) when the VM refuses IEWounded, cleared at a load
        std::atomic<bool>          g_sweepOld{ false };
        RE::BGSKeyword*            g_kwNPC   = nullptr;
        RE::BGSKeyword*            g_kwBeast = nullptr;   // IsBeastRace (Khajiit / Argonian heads: no face marks unless bBeastFaceMarks)
        std::string                g_skee;                // the RaceMenu DLL found ("" = none)

        // ---- the records (g_mtx) ----------------------------------------------------------------------------------------------------------
        struct Mark
        {
            Kind          kind  = kIE;
            std::uint32_t uid   = 0;      // m34c: this mark's own number (an apply reply is booked by it; not saved)
            std::uint8_t  area  = kFace;
            std::int8_t   slot  = -1;     // m34c: the ONE slot SBKO wrote this mark into (-1 = none). Only this slot is ever
                                          // re-looked or cleared - a slot SBKO did not write is never touched (co-saved)
            std::uint8_t  first = 0, last = 0;   // the band it may use
            std::string   code;
            std::string   path;           // the WANTED key-9 path (guarded: a SlaveTats path starts with a backslash)
            std::string   shownPath;      // what of ours is on her skin now ("" = nothing)
            float         alpha = 0.0f, shownAlpha = -1.0f;
            std::int32_t  tint  = 0, shownTint = -1;
            // IE
            std::string   source, part, stage;
            bool          real = true;
            // the choke (game hours)
            float         bornH = 0.0f, endH = 0.0f, base = 0.0f;
            float         seconds = 0.0f;   // the choke: the seconds held (the largest seen) - kept HERE, not on the knock-out entry, so the
                                            // mark fades on its own clock after she wakes / is released
            std::uint32_t heals = 0;
            int           third = -1;     // the third last planned (the log)
            // the painter's bookkeeping (not saved)
            bool          remove   = false;   // wanted gone: cleared, then dropped
            bool          noSlot   = false;   // the band was full at its paint: tried again only when her wanted set changes
            bool          lostOnce = false;   // its slot was taken by another mod once: repainted once, then given up
            bool          permFail = false;   // the band does not exist on this RaceMenu / lost twice
        };
        struct Rec
        {
            std::vector<Mark> marks;
            bool              female = true;
            RE::FormID        base   = 0;          // m34c: her base NPC (BaseKey) when the record was made - a created (FF) actor
                                                   // whose id now carries another base is another actor (0 = not known yet: set at first sight)
            std::string       who;
            // live (not saved)
            std::uint32_t     live      = 0;       // bumped at a detach (m34c: the log only - an apply reply is always booked, it is what the
                                                   // painter did on her skin; the in-flight flag survives a detach)
            bool              wantApply = false;
            bool              reconcile = false;   // a load / her 3D loading: re-read IE, then op 1 for every mark
            float             hpAtRead   = -1.0f;  // her Health fraction at the last IE read
            float             hourAtRead = -1.0f;  // the game hour at the last IE read
            std::string       lastSig, lastSkip;   // the last read / skip set logged (one line per change)
            std::unordered_map<RE::FormID, Clock::time_point> healLast;   // the stream rule: caster -> last application
        };
        std::mutex                                     g_mtx;
        std::unordered_map<RE::FormID, Rec>            g_recs;
        std::unordered_set<RE::FormID>                 g_reading, g_applying, g_asking;   // in flight: IEState / Apply / IEWounded (m34d) or IEHasWounds
        std::unordered_map<RE::FormID, const char*>    g_wantRead;   // a read is wanted (why: a string literal)
        std::unordered_map<RE::FormID, std::string>    g_lastGaps;   // the last gap set logged per actor (a log de-dupe, any actor)
        struct Sweep
        {
            std::uint32_t serial = 0;
            int           asked = 0, replied = 0, yes = 0, marked = 0;
        } g_sweep;
        bool              g_reconcileArmed = false;
        Clock::time_point g_reconcileAt{};
        bool              g_hasLastTick = false;
        Clock::time_point g_lastTick{};
        float             g_perfSeconds = 0.0f;

        // the effect sink's filter: the actors with a record (g_idMtx, rebuilt by SyncIds under g_mtx: order g_mtx -> g_idMtx)
        std::mutex                     g_idMtx;
        std::unordered_set<RE::FormID> g_ids;
        std::atomic<std::size_t>       g_idCount{ 0 };

        // IE's ping + the actors a hit / heal named (g_inMtx: the sinks' only lock; order g_mtx -> g_inMtx)
        std::mutex                     g_inMtx;
        std::unordered_set<RE::FormID> g_named, g_namedPrev;
        std::atomic<bool>              g_ping{ false };

        // MEASUREMENT ONLY (the [PERF] marks line) - never a limit
        std::atomic<std::uint64_t> g_pPings{ 0 }, g_pNamed{ 0 }, g_pReads{ 0 }, g_pAsks{ 0 }, g_pApplies{ 0 }, g_pOps{ 0 }, g_pSetups{ 0 },
            g_pSweeps{ 0 }, g_pHeals{ 0 }, g_pWorstNs{ 0 };
        std::atomic<std::uint64_t> g_pSweepCalls{ 0 };   // m34d: the sweep's Papyrus dispatches (one per sweep; one per actor on an old .pex)

        void NoteWorst(Clock::time_point a_t0)
        {
            const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - a_t0).count());
            std::uint64_t cur = g_pWorstNs.load(std::memory_order_relaxed);
            while (ns > cur && !g_pWorstNs.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {}
        }

        // ---- small readers --------------------------------------------------------------------------------------------------------------
        float HoursNow()
        {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? cal->GetHoursPassed() : 0.0f;
        }

        std::string NameOf(RE::Actor* a_actor)
        {
            if (!a_actor) return "(none)";
            const char* n = a_actor->GetDisplayFullName();
            if (!n || !*n) n = a_actor->GetName();
            return (n && *n) ? std::string(n) : std::string("(no name)");
        }
        std::string Who(RE::Actor* a_actor, RE::FormID a_id) { return std::format("{} (0x{:08X})", NameOf(a_actor), a_id); }

        float HealthFrac(RE::Actor* a_actor)
        {
            auto* avo = a_actor ? a_actor->AsActorValueOwner() : nullptr;
            if (!avo) return 0.0f;
            const float mx = a_actor->GetActorValueMax(RE::ActorValue::kHealth);
            return mx > 0.0f ? avo->GetActorValue(RE::ActorValue::kHealth) / mx : 0.0f;
        }

        bool IsFemale(RE::Actor* a_actor)
        {
            const auto* base = a_actor ? a_actor->GetActorBase() : nullptr;
            return base && base->IsFemale();
        }

        bool IsHumanoid(RE::Actor* a_actor)
        {
            if (!a_actor || !g_kwNPC) return false;
            const auto* npc  = a_actor->GetActorBase();
            const auto* race = a_actor->GetRace();
            return (npc && npc->HasKeyword(g_kwNPC)) || (race && race->HasKeyword(g_kwNPC));
        }

        bool IsChildActor(RE::Actor* a_actor)
        {
            if (!a_actor) return false;
            const auto* race = a_actor->GetRace();
            return (race && race->data.flags.all(RE::RACE_DATA::Flag::kChild)) || a_actor->IsChild();
        }

        bool IsBeast(RE::Actor* a_actor)
        {
            const auto* race = a_actor ? a_actor->GetRace() : nullptr;
            return race && g_kwBeast && race->HasKeyword(g_kwBeast);
        }

        // m34c: who she IS - her base NPC; a leveled actor's runtime base is a created (FF) form, so its template NPC is used
        // when there is one. Compared only for created (FF) actors, whose id the engine recycles.
        RE::FormID BaseKey(RE::Actor* a_actor)
        {
            if (!a_actor) return 0;
            RE::TESNPC* b = a_actor->GetActorBase();
            if (b && (b->GetFormID() >> 24) == 0xFF)
                if (auto* t = a_actor->GetTemplateBase()) b = t;
            return b ? b->GetFormID() : 0;
        }

        // Who may carry IE's marks: a humanoid adult (the player only with bMarkPlayer). nullptr = yes, else why not.
        const char* NotEligible(RE::Actor* a_actor, bool a_need3D = true)
        {
            if (!a_actor) return "not in memory";
            if (sbko::IsDeadNow(a_actor)) return "dead";
            if (a_need3D && !a_actor->Is3DLoaded()) return "no 3D";
            if (!IsHumanoid(a_actor)) return "not a humanoid (no ActorTypeNPC)";
            if (IsChildActor(a_actor)) return "a child";
            if (a_actor->IsPlayerRef() && !Settings::Get().markPlayer) return "the player (bMarkPlayer 0)";
            return nullptr;
        }

        // FNV-1a over her FormID + two words: the "stable pick" (a site / a side that never jumps between reads)
        std::uint32_t Seed(RE::FormID a_id, std::string_view a_a, std::string_view a_b)
        {
            std::uint32_t h   = 2166136261u;
            auto          mix = [&h](std::uint8_t c) {
                h ^= c;
                h *= 16777619u;
            };
            for (int i = 0; i < 4; ++i) mix(static_cast<std::uint8_t>(a_id >> (8 * i)));
            for (const char c : a_a) mix(static_cast<std::uint8_t>(c));
            mix('|');
            for (const char c : a_b) mix(static_cast<std::uint8_t>(c));
            return h;
        }

        std::string FileName(const std::string& a_path)
        {
            const auto p = a_path.find_last_of("\\/");
            return p == std::string::npos ? a_path : a_path.substr(p + 1);
        }

        // SlaveTats owns every overlay path that starts with Actors\Character\slavetats\: ours get a leading backslash
        std::string Guard(const std::string& a_rel)
        {
            static constexpr std::string_view kPrefix = "actors\\character\\slavetats\\";
            if (a_rel.size() >= kPrefix.size() && _strnicmp(a_rel.c_str(), kPrefix.data(), kPrefix.size()) == 0) return "\\" + a_rel;
            return a_rel;
        }

        // DDAB's MarkFileOk (WornDevices.cpp:4231-4270): the file through MO2's virtual Data, then its 128-byte DDS header - square and a power of
        // two (refuses the 2094x2074 crash class of Collygon's own "Major Lip Wound (F)").
        bool DdsOk(const std::string& a_rel, std::string& a_why)
        {
            std::string p = "Data/Textures/" + a_rel;
            for (auto& c : p)
                if (c == '\\') c = '/';
            FILE* f = nullptr;
            if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) {
                a_why = "not found";
                return false;
            }
            unsigned char     h[128] = {};
            const std::size_t n      = std::fread(h, 1, sizeof h, f);
            std::fclose(f);
            if (n < sizeof h || std::memcmp(h, "DDS ", 4) != 0) {
                a_why = "is not a DDS file";
                return false;
            }
            auto rd = [&h](int o) {
                return static_cast<std::uint32_t>(h[o]) | (static_cast<std::uint32_t>(h[o + 1]) << 8) |
                       (static_cast<std::uint32_t>(h[o + 2]) << 16) | (static_cast<std::uint32_t>(h[o + 3]) << 24);
            };
            const std::uint32_t hgt = rd(12), wid = rd(16);
            const bool          pow2 = wid && hgt && (wid & (wid - 1)) == 0 && (hgt & (hgt - 1)) == 0;
            if (wid != hgt || !pow2) {
                a_why = std::format("is {}x{} - not square / not a power of two, refused", wid, hgt);
                return false;
            }
            return true;
        }

        // the code's path for her (side: false = left / unsided, true = right). false = no file / not usable.
        bool PathFor(const std::string& a_code, bool a_right, bool a_female, std::string& a_out)
        {
            const auto it = g_tab.files.find(a_code);
            if (it == g_tab.files.end()) return false;
            const FileDef& fd   = it->second;
            const int      side = fd.sided && a_right ? 1 : 0;
            const int      sex  = a_female ? 0 : 1;
            if (!fd.ok[side][sex]) return false;
            a_out = Guard(fd.path[side][sex]);
            return true;
        }

        void BandOf(int a_area, int& a_first, int& a_last)
        {
            const auto& c = Settings::Get();
            switch (a_area) {
                case kFace:  a_first = c.faceFirst; a_last = c.faceLast; break;
                case kBody:  a_first = c.bodyFirst; a_last = c.bodyLast; break;
                case kHands: a_first = a_last = c.handsSlot; break;
                case kFeet:  a_first = a_last = c.feetSlot; break;
                default:     a_first = 0; a_last = -1; break;
            }
        }

        // every alpha through the one knob: x fAlphaScale, at most 1.0, at least the floor (under 30 % was not visible)
        float Shape(float a_alpha)
        {
            const auto& c  = Settings::Get();
            const float fl = (std::min)(c.alphaFloor, 1.0f);
            return (std::max)(fl, (std::min)(1.0f, a_alpha * c.alphaScale));
        }

        // ---- IE's stage words -> the layer stage and the alpha ----
        struct StageInfo
        {
            int   eff   = -1;      // 0 light, 1 medium, 2 severe; -1 = an unknown word
            float alpha = 0.0f;    // the mark (before Shape)
            float blood = 0.0f;    // a blood layer: one step lighter
            bool  heal  = false;   // a torture part during an IE heal session (high / mid / low)
        };
        StageInfo StageOf(const std::string& a_word)
        {
            const auto& c  = Settings::Get();
            const float l0 = c.stageAlphas[0], l1 = c.stageAlphas[1], l2 = c.stageAlphas[2], fl = c.alphaFloor;
            auto        is = [&](const char* w) { return _stricmp(a_word.c_str(), w) == 0; };
            if (is("light"))  return { 0, l0, fl, false };
            if (is("medium")) return { 1, l1, l0, false };
            if (is("severe")) return { 2, l2, l1, false };
            if (is("high"))   return { 0, fl, fl, true };   // light during a heal session: almost gone
            if (is("mid"))    return { 0, l0, fl, true };   // medium during a heal session
            if (is("low"))    return { 1, l1, l0, true };   // severe during a heal session
            return {};
        }

        // IE's part name -> where a mark goes; a_order = IE's own order for the Body budget (back, right arm, left arm, right leg, left leg)
        bool PartOf(const std::string& a_part, int& a_cls, int& a_side, int& a_order)
        {
            auto is = [&](const char* w) { return _stricmp(a_part.c_str(), w) == 0; };
            if (is("face"))      { a_cls = kPFace; a_side = 0; a_order = 0; return true; }
            if (is("back"))      { a_cls = kPBack; a_side = 0; a_order = 1; return true; }
            if (is("right arm")) { a_cls = kPArm;  a_side = 1; a_order = 2; return true; }
            if (is("left arm"))  { a_cls = kPArm;  a_side = 0; a_order = 3; return true; }
            if (is("right leg")) { a_cls = kPLeg;  a_side = 1; a_order = 4; return true; }
            if (is("left leg"))  { a_cls = kPLeg;  a_side = 0; a_order = 5; return true; }
            return false;
        }

        // ---- marks.json ----------------------------------------------------------------------------------------------------------------------
        std::string JStr(const json& a_o, const char* a_k)
        {
            const auto it = a_o.find(a_k);
            return it != a_o.end() && it->is_string() ? it->get<std::string>() : std::string();
        }

        int TintOf(const json& a_v)
        {
            if (!a_v.is_string()) return -1;
            const std::string s = a_v.get<std::string>();
            if (const auto it = g_tab.tints.find(s); it != g_tab.tints.end()) return it->second;
            if (s.size() == 6 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
                return static_cast<int>(std::stoul(s, nullptr, 16));
            return -1;
        }

        int StageWord(const std::string& a_s)
        {
            if (_stricmp(a_s.c_str(), "medium") == 0) return 1;
            if (_stricmp(a_s.c_str(), "severe") == 0) return 2;
            return 0;
        }

        bool LoadTable(const std::string& a_rel, std::string& a_err)
        {
            const std::filesystem::path path{ "Data/" + a_rel };
            std::ifstream               in(path, std::ios::binary);
            if (!in) {
                a_err = std::format("{} not found", path.generic_string());
                return false;
            }
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const json        j = json::parse(text, nullptr, false, true);   // comments allowed
            if (j.is_discarded() || !j.is_object()) {
                a_err = std::format("{} is not valid JSON", path.generic_string());
                return false;
            }
            try {
                if (const auto t = j.find("tints"); t != j.end() && t->is_object())
                    for (const auto& [k, v] : t->items()) {
                        const std::string s = v.is_string() ? v.get<std::string>() : std::string();
                        if (s.size() == 6) g_tab.tints[k] = static_cast<int>(std::stoul(s, nullptr, 16));
                    }
                const auto files = j.find("files");
                if (files == j.end() || !files->is_object()) {
                    a_err = "no \"files\" object";
                    return false;
                }
                for (const auto& [code, v] : files->items()) {
                    if (!v.is_object()) continue;
                    FileDef           fd;
                    const std::string area = JStr(v, "area");
                    fd.area = _stricmp(area.c_str(), "Face") == 0 ? kFace : _stricmp(area.c_str(), "Body") == 0 ? kBody
                            : _stricmp(area.c_str(), "Hands") == 0 ? kHands : _stricmp(area.c_str(), "Feet") == 0 ? kFeet : -1;
                    if (fd.area < 0) {
                        logger::warn("[marks] marks.json: file code '{}' has no area Face / Body / Hands / Feet - skipped", code);
                        continue;
                    }
                    if (const auto t = v.find("tint"); t != v.end()) fd.tint = TintOf(*t);
                    auto pair = [&](const char* a_key, int a_side) {
                        const auto it = v.find(a_key);
                        if (it == v.end() || !it->is_array() || it->size() != 2) return false;
                        for (int s = 0; s < 2; ++s) fd.path[a_side][s] = (*it)[static_cast<std::size_t>(s)].is_string()
                                                                             ? (*it)[static_cast<std::size_t>(s)].get<std::string>() : std::string();
                        return true;
                    };
                    if (pair("U", 0))
                        fd.sided = false;
                    else if (pair("L", 0) && pair("R", 1))
                        fd.sided = true;
                    else {
                        logger::warn("[marks] marks.json: file code '{}' needs \"U\": [female, male] or \"L\" + \"R\" - skipped", code);
                        continue;
                    }
                    for (int s = 0; s < (fd.sided ? 2 : 1); ++s)
                        for (int x = 0; x < 2; ++x) {
                            std::string why;
                            fd.ok[s][x] = !fd.path[s][x].empty() && DdsOk(fd.path[s][x], why);
                            if (fd.ok[s][x])
                                ++g_tab.filesOk;
                            else {
                                ++g_tab.filesBad;
                                logger::warn("[marks] art for {} ({} {}): '{}' {} - that mark is skipped", code, s == 0 ? (fd.sided ? "left" : "") : "right",
                                             x == 0 ? "female" : "male", fd.path[s][x], fd.path[s][x].empty() ? "(no path)" : why);
                            }
                        }
                    g_tab.files[code] = std::move(fd);
                }
                if (const auto fams = j.find("families"); fams != j.end() && fams->is_object())
                    for (const auto& [name, v] : fams->items()) {
                        if (!v.is_object()) continue;
                        Family fam;
                        if (const auto t = v.find("tint"); t != v.end())
                            if (const int ti = TintOf(*t); ti >= 0) fam.tint = ti;
                        static constexpr const char* kParts[4] = { "face", "back", "arm", "leg" };
                        for (int p = 0; p < 4; ++p) {
                            const auto arr = v.find(kParts[p]);
                            if (arr == v.end() || !arr->is_array()) continue;
                            for (const auto& s : *arr) {
                                if (!s.is_object()) continue;
                                Spec sp;
                                if (const std::string c = JStr(s, "code"); !c.empty()) sp.codes.push_back(c);
                                if (const auto pk = s.find("pick"); pk != s.end() && pk->is_array())
                                    for (const auto& c : *pk)
                                        if (c.is_string()) sp.codes.push_back(c.get<std::string>());
                                std::erase_if(sp.codes, [&](const std::string& c) {
                                    if (g_tab.files.contains(c)) return false;
                                    logger::warn("[marks] marks.json: family '{}' {} names code '{}' that has no file - dropped", name, kParts[p], c);
                                    return true;
                                });
                                if (sp.codes.empty()) continue;
                                sp.at    = StageWord(JStr(s, "at"));
                                sp.other = _stricmp(JStr(s, "side").c_str(), "other") == 0;
                                if (const auto t = s.find("tint"); t != s.end()) sp.tint = TintOf(*t);
                                if (const auto b = s.find("blood"); b != s.end() && b->is_boolean()) sp.blood = b->get<bool>();
                                fam.part[p].push_back(std::move(sp));
                            }
                        }
                        g_tab.families[name] = std::move(fam);
                    }
                if (const auto srcs = j.find("sources"); srcs != j.end() && srcs->is_object())
                    for (const auto& [id, v] : srcs->items()) {
                        if (!v.is_object()) continue;
                        SourceDef sd;
                        auto three = [&](const char* a_key, std::string (&a_out)[3]) {
                            const auto it = v.find(a_key);
                            if (it == v.end()) return;
                            if (it->is_string())
                                for (auto& o : a_out) o = it->get<std::string>();
                            else if (it->is_array() && it->size() == 3)
                                for (std::size_t i = 0; i < 3; ++i) a_out[i] = (*it)[i].is_string() ? (*it)[i].get<std::string>() : std::string();
                        };
                        three("family", sd.fam);
                        three("standin", sd.stand);
                        if (const auto r = v.find("real"); r != v.end() && r->is_boolean()) sd.real = r->get<bool>();
                        for (int s = 0; s < 3; ++s)
                            for (const std::string* f : { &sd.fam[s], &sd.stand[s] })
                                if (!f->empty() && !g_tab.families.contains(*f))
                                    logger::warn("[marks] marks.json: source '{}' names family '{}' that is not defined - that stage stays unmarked",
                                                 id, *f);
                        g_tab.sources[id] = std::move(sd);
                    }
                if (const auto ch = j.find("choke"); ch != j.end() && ch->is_object()) {
                    if (const std::string m = JStr(*ch, "mark"); !m.empty()) g_tab.chokeMark = m;
                    if (const std::string f = JStr(*ch, "faded"); !f.empty()) g_tab.chokeFaded = f;
                    if (const auto t = ch->find("tint"); t != ch->end())
                        if (const int ti = TintOf(*t); ti >= 0) g_tab.chokeTint = ti;
                }
            } catch (const std::exception& e) {
                a_err = std::format("{}: {}", path.generic_string(), e.what());
                return false;
            }
            return true;
        }

        // ---- IE's GetStateJson ---------------------------------------------------------------------------------------------------------------
        struct Wound
        {
            std::string source, part, stage;
        };
        bool ParseIE(const std::string& a_js, std::vector<Wound>& a_out, bool& a_healed, std::string& a_err)
        {
            a_healed = false;
            const json j = json::parse(a_js, nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                a_err = "not JSON";
                return false;
            }
            try {
                a_healed = j.contains("healed");
                if (const auto s = j.find("sources"); s != j.end() && s->is_array())
                    for (const auto& src : *s) {
                        if (!src.is_object()) continue;
                        const std::string id = JStr(src, "id");
                        const auto        ps = src.find("parts");
                        if (id.empty() || ps == src.end() || !ps->is_array()) continue;
                        for (const auto& p : *ps)
                            if (p.is_object()) a_out.push_back({ id, JStr(p, "part"), JStr(p, "stage") });
                    }
            } catch (const std::exception& e) {
                a_err = e.what();
                return false;
            }
            return true;
        }

        // ---- THE PLAN: IE's wounds -> the wanted IE marks (pure: the table, the ini, her id / sex / race) --------------------------------------
        struct Want
        {
            std::string code, path, source, part, stage;
            int         area = kFace, first = 0, last = 0, tint = 0, prio = 0;
            float       alpha = 0.0f;
            bool        real  = true;
        };
        struct PlanOut
        {
            std::vector<Want>        want;
            std::vector<std::string> gaps, skips;
            std::string              sig;   // the wound list (the 'read' line is logged when it changes)
        };

        PlanOut Plan(RE::FormID a_id, bool a_female, bool a_beast, const std::vector<Wound>& a_wounds)
        {
            const auto& cfg = Settings::Get();
            PlanOut     out;
            struct Win
            {
                const Wound*      w;
                StageInfo         st;
                bool              real;
                int               order;
                std::vector<Want> wants;   // what THIS wound paints on THIS part (m34c: built before the ranking)
            };
            // m34c: a Khajiit / Argonian gets no human-UV art - no face marks and no wrist / ankle bands (bBeastFaceMarks 0)
            const bool beastBlock = a_beast && !cfg.beastFaceMarks;
            bool       beastFaceSaid = false, beastBandSaid = false;
            // the wants of one wound on its part (empty = it paints nothing there: no family spec at this stage, its art missing, a band
            // switched off, a beast head) - specs below the stage, the face-layer switch and the band switch applied here
            auto wantsOf = [&](const Wound& w, const Family& fam, const StageInfo& st, bool real, int cls, int side, int order) {
                std::vector<Want> v;
                const auto&       specs = fam.part[cls];
                if (cls == kPFace && beastBlock) {
                    if (!beastFaceSaid && !specs.empty())
                        out.skips.push_back(std::format("face ({} {}): a Khajiit / Argonian head - no face mark (bBeastFaceMarks 0)", w.source,
                                                        w.stage));
                    beastFaceSaid = beastFaceSaid || !specs.empty();
                    return v;
                }
                const std::uint32_t seed      = Seed(a_id, w.source, w.part);
                const bool          seedRight = ((seed >> 8) & 1u) != 0;
                int                 layer     = 0;
                for (const auto& sp : specs) {
                    const int thisLayer = layer++;
                    if (sp.at > st.eff) continue;
                    if (cls == kPFace && thisLayer > 0 && !cfg.faceLayers) continue;
                    const std::string& code = sp.codes.size() == 1 ? sp.codes[0] : sp.codes[(seed >> 12) % sp.codes.size()];
                    const auto         fit  = g_tab.files.find(code);
                    if (fit == g_tab.files.end()) continue;
                    const FileDef& fd = fit->second;
                    if (fd.area == kHands || fd.area == kFeet) {
                        if (!cfg.wristAnkleBands) continue;
                        if (beastBlock) {
                            if (!beastBandSaid)
                                out.skips.push_back(std::format("{} band ({} {}): a Khajiit / Argonian - no wrist / ankle band (bBeastFaceMarks 0)",
                                                                fd.area == kHands ? "wrist" : "ankle", w.source, w.part));
                            beastBandSaid = true;
                            continue;
                        }
                    }
                    bool right = (cls == kPArm || cls == kPLeg) ? side == 1 : seedRight;
                    if (sp.other) right = !right;
                    Want nw;
                    if (!PathFor(code, right, a_female, nw.path)) {
                        out.gaps.push_back(std::format("{} {} {}: {}'s art is missing", w.source, w.part, w.stage, code));
                        continue;
                    }
                    nw.code   = code;
                    nw.area   = fd.area;
                    BandOf(fd.area, nw.first, nw.last);
                    nw.alpha  = Shape(sp.blood ? st.blood : st.alpha);
                    nw.tint   = sp.tint >= 0 ? sp.tint : fd.tint >= 0 ? fd.tint : fam.tint;
                    nw.source = w.source;
                    nw.part   = w.part;
                    nw.stage  = w.stage;
                    nw.real   = real;
                    nw.prio   = st.eff * 100 + (real ? 10 : 0) + (9 - order);
                    v.push_back(std::move(nw));
                }
                return v;
            };
            // m34c: the worst wound per part is chosen ONLY among the wounds that paint something on that part - a hair pull
            // (no face art) no longer hides the slap on the same face; a light whip on the back (its art starts at medium) no longer hides a bruise
            std::map<std::string, Win> win;   // IE part -> its worst wound that paints there
            for (const auto& w : a_wounds) {
                out.sig += std::format("{}{} {} {}", out.sig.empty() ? "" : "; ", w.source, w.part, w.stage);
                int cls = 0, side = 0, order = 0;
                if (!PartOf(w.part, cls, side, order)) {
                    out.gaps.push_back(std::format("{} {} {}: no skin site (unmarked)", w.source, w.part, w.stage));
                    continue;
                }
                const StageInfo st = StageOf(w.stage);
                if (st.eff < 0) {
                    out.gaps.push_back(std::format("{} {} '{}': an unknown stage word (unmarked)", w.source, w.part, w.stage));
                    continue;
                }
                const auto sit = g_tab.sources.find(w.source);
                if (sit == g_tab.sources.end()) {
                    out.gaps.push_back(std::format("{} {} {}: a source marks.json does not know (unmarked)", w.source, w.part, w.stage));
                    continue;
                }
                const SourceDef& sd = sit->second;
                std::string      fn = sd.fam[st.eff];
                if (fn.empty() && cfg.standIns) fn = sd.stand[st.eff];
                const auto fit = fn.empty() ? g_tab.families.end() : g_tab.families.find(fn);
                if (fit == g_tab.families.end()) {
                    out.gaps.push_back(std::format("{} {} {}: no art (unmarked)", w.source, w.part, w.stage));
                    continue;
                }
                Win cand{ &w, st, sd.real, order, wantsOf(w, fit->second, st, sd.real, cls, side, order) };
                if (cand.wants.empty()) {
                    if (!(cls == kPFace && beastBlock))
                        out.gaps.push_back(std::format("{} {} {}: family '{}' paints nothing on this part at this stage (unmarked)", w.source,
                                                       w.part, w.stage, fn));
                    continue;
                }
                auto [it, fresh] = win.try_emplace(w.part, std::move(cand));
                if (!fresh) {   // the worst wins: the higher stage, then a real wound over torture, then the higher alpha, then IE's order
                    const Win& cur = it->second;
                    const Win& nw  = cand;
                    const bool better = nw.st.eff > cur.st.eff || (nw.st.eff == cur.st.eff && nw.real && !cur.real) ||
                                        (nw.st.eff == cur.st.eff && nw.real == cur.real && nw.st.alpha > cur.st.alpha + kEps);
                    if (better) it->second = std::move(cand);
                }
            }
            std::vector<Want> bodyWant;   // the Body-area candidates: the budget applies (the worst parts first)
            std::vector<Want> faceWant;
            auto              add = [&](std::vector<Want>& v, Want&& w) {
                for (auto& o : v)
                    if (_stricmp(o.path.c_str(), w.path.c_str()) == 0) {   // one file once (both wrists = one band): the stronger alpha
                        if (w.alpha > o.alpha) o = std::move(w);
                        return;
                    }
                v.push_back(std::move(w));
            };
            for (auto& [part, wn] : win)
                for (auto& w : wn.wants) {
                    if (w.area == kBody)
                        add(bodyWant, std::move(w));
                    else if (w.area == kFace)
                        add(faceWant, std::move(w));
                    else
                        add(out.want, std::move(w));
                }
            auto budget = [&](std::vector<Want>& a_v, int a_area) {
                int first = 0, last = -1;
                BandOf(a_area, first, last);
                const std::size_t room = last >= first ? static_cast<std::size_t>(last - first + 1) : 0;
                std::stable_sort(a_v.begin(), a_v.end(), [](const Want& a, const Want& b) { return a.prio > b.prio; });
                for (std::size_t i = 0; i < a_v.size(); ++i) {
                    if (i < room)
                        out.want.push_back(a_v[i]);
                    else
                        out.skips.push_back(std::format("{} {} {} ({}): {} {}-{} full ({} worse mark(s) shown)", a_v[i].source, a_v[i].part,
                                                        a_v[i].stage, a_v[i].code, AreaName(a_area), first, last, room));
                }
            };
            budget(faceWant, kFace);
            budget(bodyWant, kBody);
            return out;
        }

        // ---- the painter's ops ---------------------------------------------------------------------------------------------------------------
        struct OpRef
        {
            int           op = 0;     // 1 paint, 2 look, 3 clear
            Kind          kind = kIE;
            std::string   path, code;
            float         alpha = 0.0f;
            std::int32_t  tint = 0;
            std::uint32_t uid  = 0;   // m34c: the exact mark the reply is booked on (never "the first one with that file")
            int           slot = -1;  // m34c: the slot SBKO wrote it into (-1 = none: op 1 takes a free one)
        };
        struct Job
        {
            int                kind = 0;   // 0 read, 1 apply, 2 ask one (HasOpenWounds - an old .pex), 3 ask the whole sweep at once (m34d)
            RE::FormID         id   = 0;
            const char*        why  = "";
            std::uint32_t      live = 0, sweep = 0;
            std::string        ops;
            std::vector<OpRef> refs;
            bool               dump = false;   // console 'ie': log the wounds and the plan
            std::vector<RE::FormID> ids;       // m34d, kind 3: every actor this sweep asks about (one Papyrus call)
            std::uint32_t      marked = 0;     // m34d, kind 3: the marked ones this sweep reads in full instead (its log line)
        };

        // caller holds g_mtx. Builds the ops that bring her skin to the wanted state; drops removed marks that are not on her skin.
        // m34c: every op names the ONE slot SBKO wrote (Mark::slot); the painter re-looks / clears only that slot, and only
        // while it still holds our file - a slot SBKO did not write (SlaveTats / Combat Wound painting the same Collygon file) is never
        // adopted, re-tinted or cleared. Op 1 with no slot of ours takes a FREE slot of the band.
        void BuildOps(Rec& r, bool a_reconcile, std::string& a_ops, std::vector<OpRef>& a_refs)
        {
            auto push = [&](int a_op, const Mark& m, const std::string& a_path, int a_slot) {
                a_ops += std::format("{}{}|{}|{}|{}|{}|{}|{:.3f}|{}|{}", a_ops.empty() ? "" : ";", a_op, m.code, static_cast<int>(m.area),
                                     static_cast<int>(m.first), static_cast<int>(m.last), a_path, m.alpha, m.tint, a_slot);
                a_refs.push_back({ a_op, m.kind, a_path, m.code, m.alpha, m.tint, m.uid, a_slot });
            };
            for (auto it = r.marks.begin(); it != r.marks.end();) {
                Mark&      m     = *it;
                const bool shown = !m.shownPath.empty() && m.slot >= 0;
                if (m.remove) {
                    if (!shown) {   // nothing of ours on her skin: forget it now
                        it = r.marks.erase(it);
                        continue;
                    }
                    push(3, m, m.shownPath, m.slot);
                    ++it;
                    continue;
                }
                if (m.permFail) {
                    ++it;
                    continue;
                }
                if (shown && _stricmp(m.shownPath.c_str(), m.path.c_str()) != 0) {   // the texture changes (the Faded third): ours out, the new in
                    push(3, m, m.shownPath, m.slot);
                    push(1, m, m.path, -1);
                } else if (!shown) {
                    if (!m.noSlot || a_reconcile) push(1, m, m.path, -1);
                } else if (a_reconcile)
                    push(1, m, m.path, m.slot);   // our own slot: re-looked if it still holds our file, else a free one
                else if (std::fabs(m.shownAlpha - m.alpha) > kEps || m.shownTint != m.tint)
                    push(2, m, m.path, m.slot);
                ++it;
            }
        }

        void SyncIds()   // caller holds g_mtx
        {
            std::scoped_lock lk(g_idMtx);
            g_ids.clear();
            for (const auto& [id, r] : g_recs) g_ids.insert(id);
            g_idCount.store(g_ids.size());
        }

        // m34c: a mark of this kind and file - the one NOT being removed first (a re-paint adopts it), else a removed one
        Mark* FindMark(Rec& r, Kind a_kind, const std::string& a_path)
        {
            Mark* removed = nullptr;
            for (auto& m : r.marks)
                if (m.kind == a_kind && _stricmp(m.path.c_str(), a_path.c_str()) == 0) {
                    if (!m.remove) return &m;
                    if (!removed) removed = &m;
                }
            return removed;
        }
        Mark* FindUid(Rec& r, std::uint32_t a_uid)
        {
            for (auto& m : r.marks)
                if (m.uid == a_uid) return &m;
            return nullptr;
        }

        // m34c: is the actor under this record still the one it was made for? Caller holds g_mtx; she is in memory with 3D.
        // 0 = yes (her base / sex filled in when unknown). 1 = her SEX changed (a regenerated leveled actor): our overrides sit in the other
        // sex's override set, which RaceMenu no longer shows on her -> the record is forgotten (IE re-reads her). 2 = a created (FF) id now
        // carries another base NPC (the engine recycled the id): every mark of the record is cleared from OUR slots, then the record goes.
        int IdentityCheck(Rec& r, RE::Actor* a, RE::FormID a_id, std::vector<std::string>& a_logs)
        {
            const bool       fem = IsFemale(a);
            const RE::FormID bk  = BaseKey(a);
            if (r.base == 0) r.base = bk;
            if (fem != r.female) {
                a_logs.push_back(std::format("[marks] {}: her sex is not the record's any more (a regenerated actor) - record forgotten ({} mark(s); "
                                             "RaceMenu no longer shows the other sex's overrides on her)", Who(a, a_id), r.marks.size()));
                return 1;
            }
            if ((a_id >> 24) == 0xFF && bk != 0 && bk != r.base) {
                a_logs.push_back(std::format("[marks] {}: a created actor whose id now carries another base (0x{:08X}, was 0x{:08X}) - the id was "
                                             "reused: SBKO's {} mark(s) cleared from its own slots, record dropped",
                                             Who(a, a_id), bk, r.base, r.marks.size()));
                for (auto& m : r.marks) m.remove = true;
                r.base      = bk;
                r.wantApply = true;
                return 2;
            }
            return 0;
        }
        std::uint32_t g_uid = 0;   // g_mtx: the next mark uid (not saved - given again at a load)

        // ---- the choke mark: its third, its alpha, its texture (DDAB's MarkStage / MarkStageAlpha) ----
        int ChokeThird(const Mark& m, float a_hours)
        {
            if (!(a_hours < m.endH)) return 0;
            const float span  = m.endH - m.bornH;
            const float share = span > 0.0f ? (a_hours - m.bornH) / span : 1.0f;
            return share < 1.0f / 3.0f ? 3 : share < 2.0f / 3.0f ? 2 : 1;
        }
        float ChokeAlpha(const Mark& m, int a_third)
        {
            const float fl = (std::min)(Settings::Get().alphaFloor, m.base);
            return a_third == 3 ? m.base : a_third == 2 ? fl + (m.base - fl) * 0.5f : fl;
        }

        // caller holds g_mtx: brings her choke marks to now; true = something must be painted. a_logs collects the lines.
        bool ChokeStep(Rec& r, bool a_female, float a_hours, std::vector<std::string>& a_logs)
        {
            bool need = false;
            for (auto& m : r.marks) {
                if (m.kind != kChoke || m.remove) continue;
                const int t = ChokeThird(m, a_hours);
                if (t == 0) {
                    m.remove = true;
                    need     = true;
                    a_logs.push_back(std::format("[marks] choke {}: ended -> cleared (its {:.1f} game h are over)", r.who, m.endH - m.bornH));
                    continue;
                }
                std::string path;
                if (!PathFor(t == 1 ? g_tab.chokeFaded : g_tab.chokeMark, false, a_female, path)) PathFor(g_tab.chokeMark, false, a_female, path);
                const float alpha = Shape(ChokeAlpha(m, t));
                if (t != m.third) {
                    if (m.third > 0)
                        a_logs.push_back(std::format("[marks] choke {}: third {} -> {:.2f}{} ({:.1f} game h left)", r.who, 4 - t, alpha,
                                                     t == 1 ? std::format(" with '{}'", FileName(path)) : std::string(" (halfway to the floor)"),
                                                     m.endH - a_hours));
                    m.third = t;
                }
                m.path  = path;
                m.alpha = alpha;
                m.tint  = g_tab.chokeTint;
                if (_stricmp(m.shownPath.c_str(), m.path.c_str()) != 0 || std::fabs(m.shownAlpha - m.alpha) > kEps || m.shownTint != m.tint) need = true;
            }
            return need;
        }

        // ---- dispatch (NO lock held) -------------------------------------------------------------------------------------------------------
        void OnIEState(RE::FormID a_id, std::uint32_t a_gen, std::string a_js, const char* a_why, bool a_dump);
        void OnApplyReply(RE::FormID a_id, std::uint32_t a_live, std::uint32_t a_gen, std::vector<OpRef> a_refs, std::string a_res);
        void OnAskReply(RE::FormID a_id, std::uint32_t a_gen, std::uint32_t a_sweep, bool a_yes);
        void OnSweepReply(std::uint32_t a_gen, std::uint32_t a_sweep, std::uint32_t a_marked, std::vector<RE::FormID> a_ids,
                          std::vector<RE::FormID> a_yes);

        void Dispatch(Job& j)
        {
            const std::uint32_t gen = g_gen.load();
            const RE::FormID    id  = j.id;
            bool                sent = false;
            if (j.kind == 0) {
                g_pReads.fetch_add(1, std::memory_order_relaxed);
                const char* why  = j.why;
                const bool  dump = j.dump;
                sent = VmCall::StaticIfPresentThen(kScript, "IEState", RE::MakeFunctionArguments(static_cast<std::int32_t>(id)), kWhat,
                                                   [id, gen, why, dump](const RE::BSScript::Variable& v) {
                                                       std::string js = v.IsString() ? std::string(v.GetString()) : std::string();
                                                       TaskRelay::Add([id, gen, js = std::move(js), why, dump]() { OnIEState(id, gen, js, why, dump); });
                                                   });
                if (!sent) {
                    std::scoped_lock lk(g_mtx);
                    g_reading.erase(id);
                }
            } else if (j.kind == 1) {
                g_pApplies.fetch_add(1, std::memory_order_relaxed);
                g_pOps.fetch_add(j.refs.size(), std::memory_order_relaxed);
                const std::uint32_t live = j.live;
                sent = VmCall::StaticIfPresentThen(
                    kScript, "Apply", RE::MakeFunctionArguments(static_cast<std::int32_t>(id), static_cast<RE::BSFixedString>(j.ops.c_str())), kWhat,
                    [id, live, gen, refs = j.refs](const RE::BSScript::Variable& v) {
                        std::string res = v.IsString() ? std::string(v.GetString()) : std::string("-9");
                        TaskRelay::Add([id, live, gen, refs, res = std::move(res)]() { OnApplyReply(id, live, gen, refs, res); });
                    });
                if (!sent) {
                    std::scoped_lock lk(g_mtx);
                    g_applying.erase(id);
                    if (auto it = g_recs.find(id); it != g_recs.end()) it->second.wantApply = true;
                }
            } else if (j.kind == 3) {
                // m34d: the whole sweep in ONE Papyrus call - the ids go in as one Int[]; SBKO_Marks.IEWounded hands the same array back with
                // 0 for every actor without an open wound (an element is a yes only when it still equals the id asked at that position)
                g_pSweepCalls.fetch_add(1, std::memory_order_relaxed);
                g_pAsks.fetch_add(j.ids.size(), std::memory_order_relaxed);
                std::vector<std::int32_t> arg;
                arg.reserve(j.ids.size());
                for (const auto a : j.ids) arg.push_back(static_cast<std::int32_t>(a));
                const std::uint32_t sweep  = j.sweep;
                const std::uint32_t marked = j.marked;
                sent = VmCall::StaticIfPresentThen(kScript, "IEWounded", RE::MakeFunctionArguments(std::move(arg)), kWhat,
                                                   [ids = j.ids, gen, sweep, marked](const RE::BSScript::Variable& v) {
                                                       std::vector<RE::FormID> yes;
                                                       if (v.IsArray()) {
                                                           if (const auto arr = v.GetArray(); arr) {
                                                               const std::size_t n = (std::min)(static_cast<std::size_t>(arr->size()), ids.size());
                                                               for (std::size_t i = 0; i < n; ++i) {
                                                                   const auto& e = (*arr)[static_cast<std::uint32_t>(i)];
                                                                   if (e.IsInt() && static_cast<RE::FormID>(e.GetSInt()) == ids[i]) yes.push_back(ids[i]);
                                                               }
                                                           }
                                                       }
                                                       TaskRelay::Add([ids, gen, sweep, marked, yes = std::move(yes)]() {
                                                           OnSweepReply(gen, sweep, marked, ids, yes);
                                                       });
                                                   });
                if (!sent) {
                    // the class is there (Tick checked it) but the VM refused IEWounded: an older SBKO_Marks.pex -> per-actor asks this session
                    if (VmCall::ClassPresentCached(kScript) == 1 && !g_sweepOld.exchange(true))
                        logger::warn("[marks] the installed SBKO_Marks.pex has no IEWounded (an older .pex next to this DLL) - the sweep "
                                     "asks Injury Engine one actor per call this session (slower); install this version's SBKO_Marks.pex");
                    OnSweepReply(gen, sweep, marked, j.ids, {});   // nobody answered: the in-flight set is freed, the next sweep asks again
                }
            } else {
                g_pAsks.fetch_add(1, std::memory_order_relaxed);
                g_pSweepCalls.fetch_add(1, std::memory_order_relaxed);   // m34d: one dispatch per actor (an old .pex)
                const std::uint32_t sweep = j.sweep;
                sent = VmCall::StaticIfPresentThen(kScript, "IEHasWounds", RE::MakeFunctionArguments(static_cast<std::int32_t>(id)), kWhat,
                                                   [id, gen, sweep](const RE::BSScript::Variable& v) {
                                                       const bool yes = v.IsBool() ? v.GetBool() : (v.IsInt() ? v.GetSInt() != 0 : false);
                                                       TaskRelay::Add([id, gen, sweep, yes]() { OnAskReply(id, gen, sweep, yes); });
                                                   });
                if (!sent) OnAskReply(id, gen, j.sweep, false);
            }
        }

        // ---- the replies (SKSE tasks) ----------------------------------------------------------------------------------------------------------
        void OnIEState(RE::FormID a_id, std::uint32_t a_gen, std::string a_js, const char* a_why, bool a_dump)
        {
            const auto t0 = Clock::now();
            if (a_gen != g_gen.load()) return;   // the old game's (its in-flight sets were cleared at kPreLoadGame)
            const auto& cfg   = Settings::Get();
            auto*       a     = RE::TESForm::LookupByID<RE::Actor>(a_id);
            const char* notOk = NotEligible(a);
            const std::string who = a ? Who(a, a_id) : std::format("0x{:08X}", a_id);
            std::vector<Wound> wounds;
            bool               healed = false;
            std::string        err;
            const bool         parsed = !a_js.empty() && ParseIE(a_js, wounds, healed, err);
            const bool         female = IsFemale(a);
            const RE::FormID   baseK  = BaseKey(a);
            // m34c: a bound GetStateJson never answers "" (it always writes {"sources":[...]}); "" for an actor in memory =
            // Injury Engine's natives are not bound (its script is there, its DLL is not) - twice in a row -> the IE mirror is off (one line)
            if (a_js.empty() && a) {
                if (g_ieEmpty.fetch_add(1) + 1 >= 2 && !g_ieDead.exchange(true))
                    logger::warn("[marks] Injury Engine's script answers but its DLL does not (GetStateJson returned nothing twice - Injury_Engine.dll "
                                 "not loaded / natives not bound) - the IE mirror is OFF this session; the choke mark still works");
            } else if (!a_js.empty())
                g_ieEmpty.store(0);
            PlanOut            plan;
            if (parsed && !notOk) plan = Plan(a_id, female, IsBeast(a), wounds);
            const float hp    = a ? HealthFrac(a) : 0.0f;
            const float hours = HoursNow();
            std::vector<std::string> logs;
            Job                      job;
            bool                     dispatch = false;
            {
                std::scoped_lock lk(g_mtx);
                g_reading.erase(a_id);
                if (notOk) {
                    if (a_dump) logs.push_back(std::format("[marks] ie {}: not read - {}", who, notOk));
                    // a marked actor that lost her 3D keeps her record; her next 3D load reconciles
                    if (auto it = g_recs.find(a_id); it != g_recs.end()) it->second.reconcile = true;
                } else if (!parsed) {
                    if (!a_js.empty() || a_dump)   // an empty answer is counted above, not logged per read
                        logs.push_back(std::format("[marks] ie {}: Injury Engine's state could not be read ({}) - nothing changed",
                                                   who, a_js.empty() ? "an empty answer" : err));
                } else {
                    auto it = g_recs.find(a_id);
                    // m34c: the record must still be HER (sex; a created id's base) - refreshed at every read
                    if (it != g_recs.end() && IdentityCheck(it->second, a, a_id, logs) == 1) {
                        g_recs.erase(it);
                        it = g_recs.end();
                    }
                    if (it == g_recs.end() && !plan.want.empty()) {
                        it                = g_recs.emplace(a_id, Rec{}).first;
                        it->second.female = female;
                        it->second.base   = baseK;
                        it->second.who    = who;
                    }
                    // the gap lines: once per change, for any actor
                    std::string gapKey;
                    for (const auto& g : plan.gaps) gapKey += g + " | ";
                    if (cfg.logMarks && !gapKey.empty() && g_lastGaps[a_id] != gapKey)
                        for (const auto& g : plan.gaps) logs.push_back(std::format("[marks] gap {}: {}", who, g));
                    if (gapKey.empty())
                        g_lastGaps.erase(a_id);
                    else
                        g_lastGaps[a_id] = gapKey;
                    if (it != g_recs.end()) {
                        Rec& r        = it->second;
                        r.who         = who;
                        r.hpAtRead    = hp;
                        r.hourAtRead  = hours;
                        if (cfg.logMarks && plan.sig != r.lastSig)
                            logs.push_back(std::format("[marks] read {} ({}): IE {}{}", who, a_why,
                                                       plan.sig.empty() ? std::string("no wound") : plan.sig, healed ? " - healed" : ""));
                        r.lastSig = plan.sig;
                        std::string skipKey;
                        for (const auto& s : plan.skips) skipKey += s + " | ";
                        if (cfg.logMarks && skipKey != r.lastSkip)
                            for (const auto& s : plan.skips) logs.push_back(std::format("[marks] skip {}: {}", who, s));
                        r.lastSkip = skipKey;
                        // the diff: IE marks not wanted any more -> clear; wanted -> paint / look
                        bool changed = false;
                        std::size_t cleared = 0;
                        for (auto& m : r.marks) {
                            if (m.kind != kIE || m.remove) continue;
                            const bool wanted = std::any_of(plan.want.begin(), plan.want.end(),
                                                            [&](const Want& w) { return _stricmp(w.path.c_str(), m.path.c_str()) == 0; });
                            if (wanted) continue;
                            m.remove = true;
                            changed  = true;
                            ++cleared;
                            if (cfg.logMarks && !healed)
                                logs.push_back(std::format("[marks] clear {} {} {}[{}] (IE: {} {} gone)", who, m.code, AreaName(m.area),
                                                           m.slot >= 0 ? std::to_string(m.slot) : std::string("?"), m.source, m.part));
                        }
                        if (healed && cleared && cfg.logMarks)
                            logs.push_back(std::format("[marks] healed {}: all {} IE mark(s) cleared", who, cleared));
                        for (const auto& w : plan.want) {
                            Mark* m = FindMark(r, kIE, w.path);   // m34c: the live one first, else a removed one (adopted)
                            if (!m) {
                                Mark nm;
                                nm.uid   = ++g_uid;
                                nm.kind  = kIE;
                                nm.area  = static_cast<std::uint8_t>(w.area);
                                nm.first = static_cast<std::uint8_t>(w.first);
                                nm.last  = static_cast<std::uint8_t>(w.last);
                                nm.code  = w.code;
                                nm.path  = w.path;
                                nm.alpha = w.alpha;
                                nm.tint  = w.tint;
                                nm.source = w.source;
                                nm.part   = w.part;
                                nm.stage  = w.stage;
                                nm.real   = w.real;
                                if (cfg.logMarks)
                                    logs.push_back(std::format("[marks] paint {} {} '{}' {} {}-{} alpha {:.2f} tint {:06X} ({} {} {})", who, w.code,
                                                               FileName(w.path), AreaName(w.area), w.first, w.last, w.alpha, w.tint, w.source,
                                                               w.part, w.stage));
                                r.marks.push_back(std::move(nm));
                                changed = true;
                                continue;
                            }
                            if (m->remove) {   // wanted again before its clear landed
                                m->remove = false;
                                changed   = true;
                            }
                            if (std::fabs(m->alpha - w.alpha) > kEps || m->tint != w.tint) {
                                if (cfg.logMarks)
                                    logs.push_back(std::format("[marks] {} {} {} {}[{}] {:.2f} -> {:.2f} ({} -> {} {} {})",
                                                               w.alpha < m->alpha ? "fade" : "raise", who, w.code, AreaName(m->area),
                                                               m->slot >= 0 ? std::to_string(m->slot) : std::string("?"), m->alpha, w.alpha,
                                                               m->stage, w.source, w.part, w.stage));
                                changed = true;
                            }
                            m->alpha  = w.alpha;
                            m->tint   = w.tint;
                            m->source = w.source;
                            m->part   = w.part;
                            m->stage  = w.stage;
                            m->real   = w.real;
                            m->first  = static_cast<std::uint8_t>(w.first);
                            m->last   = static_cast<std::uint8_t>(w.last);
                        }
                        if (changed)
                            for (auto& m : r.marks) m.noSlot = false;   // her wanted set changed: a full band may have room now
                        (void)ChokeStep(r, r.female, hours, logs);
                        if ((changed || r.reconcile || r.wantApply) && !g_applying.contains(a_id)) {
                            BuildOps(r, r.reconcile, job.ops, job.refs);
                            r.reconcile = false;
                            r.wantApply = false;
                            if (!job.refs.empty()) {
                                g_applying.insert(a_id);
                                job.kind = 1;
                                job.id   = a_id;
                                job.live = r.live;
                                dispatch = true;
                            }
                        } else if (changed)
                            r.wantApply = true;   // an apply is in flight: served by the next check
                        if (r.marks.empty()) {
                            g_recs.erase(it);
                            SyncIds();
                        } else if (it->second.marks.size())
                            SyncIds();
                    }
                }
            }
            if (a_dump) {
                logs.push_back(std::format("[marks] ie {}: {} wound(s){}{}", who, wounds.size(), healed ? " - IE says healed" : "",
                                           wounds.empty() ? std::string() : " -> " + plan.sig));
                for (const auto& w : plan.want)
                    logs.push_back(std::format("[marks] ie {}:   wanted {} '{}' {} {}-{} alpha {:.2f} tint {:06X} ({} {} {})", who, w.code,
                                               FileName(w.path), AreaName(w.area), w.first, w.last, w.alpha, w.tint, w.source, w.part, w.stage));
                for (const auto& g : plan.gaps) logs.push_back(std::format("[marks] ie {}:   gap {}", who, g));
                for (const auto& s : plan.skips) logs.push_back(std::format("[marks] ie {}:   skip {}", who, s));
            }
            for (const auto& l : logs) logger::info("{}", l);
            if (dispatch) Dispatch(job);
            NoteWorst(t0);
        }

        void OnApplyReply(RE::FormID a_id, std::uint32_t a_live, std::uint32_t a_gen, std::vector<OpRef> a_refs, std::string a_res)
        {
            const auto t0 = Clock::now();
            if (a_gen != g_gen.load()) return;
            const auto& cfg = Settings::Get();
            // "setup;r|slot;r|slot..." (setup 1 = AddOverlays now, 0 = she had overlays, -1 = no actor / no 3D: nothing was done)
            std::vector<std::string> tok;
            for (std::size_t s = 0;;) {
                const auto e = a_res.find(';', s);
                tok.push_back(a_res.substr(s, e == std::string::npos ? std::string::npos : e - s));
                if (e == std::string::npos) break;
                s = e + 1;
            }
            int setup = -9;
            try {
                setup = tok.empty() ? -9 : std::stoi(tok[0]);
            } catch (...) {
                setup = -9;
            }
            std::vector<std::string> logs;
            {
                std::scoped_lock lk(g_mtx);
                g_applying.erase(a_id);
                auto it = g_recs.find(a_id);
                if (it == g_recs.end()) return;
                Rec& r = it->second;
                // m34c: the reply is ALWAYS booked, even after a detach - it is what the painter did on her skin; the in-flight
                // flag is cleared only here (or when nothing was dispatched), so two Apply stacks never run on one actor at once
                (void)a_live;
                if (setup == 1) {
                    g_pSetups.fetch_add(1, std::memory_order_relaxed);
                    if (cfg.logMarks) logs.push_back(std::format("[marks] setup {}: AddOverlays (no node update)", r.who));
                }
                if (setup < 0) {
                    r.reconcile = true;   // no actor / no 3D when the script ran: everything again when she has 3D
                    if (setup == -9) logs.push_back(std::format("[marks] apply {}: the script answered '{}' - nothing known; repainted at her next "
                                                                "reconcile", r.who, a_res));
                } else {
                    for (std::size_t i = 0; i < a_refs.size(); ++i) {
                        const OpRef& ref  = a_refs[i];
                        int          res  = -9, slot = -1;
                        if (i + 1 < tok.size()) {
                            const auto bar = tok[i + 1].find('|');
                            try {
                                res  = std::stoi(tok[i + 1].substr(0, bar));
                                slot = bar == std::string::npos ? -1 : std::stoi(tok[i + 1].substr(bar + 1));
                            } catch (...) {
                                res = -9;
                            }
                        }
                        Mark* m = FindUid(r, ref.uid);   // m34c: the exact mark this op was built for
                        if (!m) continue;
                        const bool wasShown = _stricmp(m->shownPath.c_str(), ref.path.c_str()) == 0;
                        if (res == -1) {   // she lost her 3D during the call: unchanged, everything again at her next 3D load
                            r.reconcile = true;
                            continue;
                        }
                        if (ref.op == 1) {
                            if (res == 1) {
                                m->shownPath  = ref.path;
                                m->shownAlpha = ref.alpha;
                                m->shownTint  = ref.tint;
                                m->slot       = static_cast<std::int8_t>(slot);
                                m->noSlot     = false;
                                continue;
                            }
                            if (ref.slot >= 0 || wasShown) {   // our slot does not hold it any more and it was not painted elsewhere
                                m->shownPath.clear();
                                m->slot = -1;
                            }
                            if (res == 0) {
                                m->noSlot = true;
                                if (cfg.logMarks)
                                    logs.push_back(std::format("[marks] skip {} {}: no free {} slot in {}-{} (other mods hold them) - not painted; tried "
                                                               "again when her wounds change", r.who, ref.code, AreaName(m->area), m->first, m->last));
                            } else if (res == -4) {
                                m->permFail = true;
                                logs.push_back(std::format("[marks] {} {}: the band {} {}-{} does not exist on this RaceMenu (its slot count is lower) - "
                                                           "never painted", r.who, ref.code, AreaName(m->area), m->first, m->last));
                            }
                        } else if (ref.op == 2) {
                            if (res == 1) {
                                m->shownAlpha = ref.alpha;
                                m->shownTint  = ref.tint;
                            } else if (res == -3) {   // OUR slot holds another file now (another mod painted over it): never touched again
                                logs.push_back(std::format("[marks] lost {} {}: {}[{}] holds another file now (another mod) - {}", r.who, ref.code,
                                                           AreaName(m->area), ref.slot, m->lostOnce ? "given up" : "painted once more in a free slot"));
                                m->shownPath.clear();
                                m->slot = -1;
                                if (!m->lostOnce) {
                                    m->lostOnce = true;
                                    r.wantApply = true;
                                } else
                                    m->permFail = true;
                            }
                        } else if (ref.op == 3) {
                            if ((res == 2 || res == 3) && wasShown) {   // 2 cleared; 3 = our slot no longer held our file (never touched)
                                m->shownPath.clear();
                                m->slot = -1;
                            }
                        }
                    }
                }
                std::erase_if(r.marks, [](const Mark& m) { return m.remove && m.shownPath.empty(); });
                if (r.marks.empty()) {
                    if (cfg.logMarks) logs.push_back(std::format("[marks] {}: no mark left - record dropped", r.who));
                    g_recs.erase(it);
                    SyncIds();
                }
            }
            for (const auto& l : logs) logger::info("{}", l);
            NoteWorst(t0);
        }

        void OnAskReply(RE::FormID a_id, std::uint32_t a_gen, std::uint32_t a_sweep, bool a_yes)
        {
            if (a_gen != g_gen.load()) return;
            Job         job;
            bool        dispatch = false;
            std::string line;
            {
                std::scoped_lock lk(g_mtx);
                g_asking.erase(a_id);
                if (a_sweep == g_sweep.serial) {
                    ++g_sweep.replied;
                    if (a_yes) ++g_sweep.yes;
                    // m34c: ONE line per sweep that FOUND someone (an unexplained ping that finds nobody stays in [PERF] only)
                    if (g_sweep.replied == g_sweep.asked && g_sweep.yes > 0 && Settings::Get().logMarks)
                        line = std::format("[marks] sweep: IE ping with no named actor - {} loaded humanoid(s) asked, {} have wounds{}", g_sweep.asked,
                                           g_sweep.yes, g_sweep.marked ? std::format(" (+ {} marked one(s) read in full)", g_sweep.marked) : "");
                }
                if (a_yes && !g_reading.contains(a_id)) {   // a full read now (no wait for the next check)
                    g_reading.insert(a_id);
                    job.kind = 0;
                    job.id   = a_id;
                    job.why  = "IE sweep: she has open wounds";
                    dispatch = true;
                }
            }
            if (!line.empty()) logger::info("{}", line);
            if (dispatch) Dispatch(job);
        }

        // m34d: the whole sweep's answer (an SKSE task; or straight from Dispatch when nothing was sent - a_yes empty). Frees every asked
        // actor's in-flight flag, logs the one line when it found someone, and starts a full read for every yes - what OnAskReply does for
        // one actor, for all of them at once.
        void OnSweepReply(std::uint32_t a_gen, std::uint32_t a_sweep, std::uint32_t a_marked, std::vector<RE::FormID> a_ids,
                          std::vector<RE::FormID> a_yes)
        {
            if (a_gen != g_gen.load()) return;
            std::vector<Job> jobs;
            std::string      line;
            {
                std::scoped_lock lk(g_mtx);
                for (const auto id : a_ids) g_asking.erase(id);
                if (a_sweep == g_sweep.serial) {
                    g_sweep.replied += static_cast<int>(a_ids.size());
                    g_sweep.yes += static_cast<int>(a_yes.size());
                }
                // m34c: ONE line per sweep that FOUND someone (an unexplained ping that finds nobody stays in [PERF] only)
                if (!a_yes.empty() && Settings::Get().logMarks)
                    line = std::format("[marks] sweep: IE ping with no named actor - {} loaded humanoid(s) asked, {} have wounds{}", a_ids.size(),
                                       a_yes.size(), a_marked ? std::format(" (+ {} marked one(s) read in full)", a_marked) : "");
                for (const auto id : a_yes) {
                    if (g_reading.contains(id)) continue;   // a full read now (no wait for the next check)
                    g_reading.insert(id);
                    Job job;
                    job.kind = 0;
                    job.id   = id;
                    job.why  = "IE sweep: she has open wounds";
                    jobs.push_back(std::move(job));
                }
            }
            if (!line.empty()) logger::info("{}", line);
            for (auto& j : jobs) Dispatch(j);
        }

        // ---- IE's ping: a mod-callback sink (any thread - IE's damage hook, a worker, a VM thread). One compare, one atomic. ----
        class PingSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
            {
                if (a_event && g_on.load(std::memory_order_relaxed)) {
                    const char* n = a_event->eventName.c_str();
                    if (n && _stricmp(n, "Injury_Engine_Injury") == 0) {
                        g_ping.store(true, std::memory_order_relaxed);
                        g_pPings.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        PingSink g_pingSink;

        void OnEffectTask(RE::FormID a_tid, RE::FormID a_cid, RE::FormID a_mgef, std::uint32_t a_gen);

        void EmitPerf(bool a_now)
        {
            float       secs = 0.0f;
            std::size_t recs = 0, loaded = 0, marks = 0;
            {
                std::scoped_lock lk(g_mtx);
                secs = g_perfSeconds;
                if (!a_now) g_perfSeconds = 0.0f;
                recs = g_recs.size();
                for (const auto& [id, r] : g_recs) {
                    marks += r.marks.size();
                    auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                    if (a && a->Is3DLoaded()) ++loaded;
                }
            }
            auto take = [a_now](std::atomic<std::uint64_t>& c) { return a_now ? c.load() : c.exchange(0); };
            const std::uint64_t pings = take(g_pPings), named = take(g_pNamed), reads = take(g_pReads), asks = take(g_pAsks),
                                applies = take(g_pApplies), ops = take(g_pOps), setups = take(g_pSetups), sweeps = take(g_pSweeps),
                                heals = take(g_pHeals), worst = take(g_pWorstNs), sweepCalls = take(g_pSweepCalls);
            logger::info("[PERF] marks: {:.0f} s{} - {} reads, {} writes ({} ops), {} setups, {} sweeps ({} sweep calls, {} HasOpenWounds asks), {} IE "
                         "pings, {} hit / heal names, {} choke heals | records {} ({} with 3D), {} marks | worst {:.2f} ms",
                         secs, a_now ? " so far (console)" : "", reads, applies, ops, setups, sweeps, sweepCalls, asks, pings, named, heals, recs, loaded,
                         marks, static_cast<double>(worst) / 1.0e6);
        }
    }

    // =====================================================================================================================================
    bool On() noexcept { return g_on.load(std::memory_order_relaxed); }

    void Install()
    {
        const auto& cfg = Settings::Get();
        if (!cfg.marks) {
            logger::info("[marks] start: OFF (bMarks 0) - no skin mark is read, painted or kept");
            return;
        }
        if (REX::W32::GetModuleHandleA("skeevr.dll"))
            g_skee = "skeevr.dll";
        else if (REX::W32::GetModuleHandleA("skee64.dll"))
            g_skee = "skee64.dll";
        g_kwNPC   = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);
        // m34c: IsBeastRace by its Skyrim.esm id first (0x000D61D1, taken only if that keyword really is IsBeastRace), else by editor id
        const auto* kwById = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000D61D1);
        const char* kwEd   = kwById ? kwById->GetFormEditorID() : nullptr;
        if (auto* kw = const_cast<RE::BGSKeyword*>(kwById); kw && kwEd && _stricmp(kwEd, "IsBeastRace") == 0)
            g_kwBeast = kw;
        else
            g_kwBeast = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("IsBeastRace");
        g_ieDll = REX::W32::GetModuleHandleA("Injury_Engine.dll") != nullptr;   // m34c
        std::string err;
        if (!LoadTable(cfg.marksFile, err)) {
            logger::warn("[marks] start: OFF - the mark table could not be loaded ({}) - no skin mark this session", err);
            return;
        }
        if (g_skee.empty()) {
            logger::warn("[marks] start: OFF - RaceMenu's skee DLL (skeevr.dll / skee64.dll) is not loaded: NiOverride cannot paint - no skin mark "
                         "this session");
            return;
        }
        if (!g_kwNPC) {
            logger::warn("[marks] start: OFF - the ActorTypeNPC keyword was not found - no skin mark this session");
            return;
        }
        if (auto* src = SKSE::GetModCallbackEventSource())
            src->AddEventSink(&g_pingSink);
        else
            logger::warn("[marks] no SKSE mod-callback event source - Injury Engine's ping is not heard (hits / heals / Health / hours still read)");
        auto*       dh   = RE::TESDataHandler::GetSingleton();
        const bool  iePl = dh && dh->LookupModByName("Injury_Engine.esp") != nullptr;
        std::size_t specs = 0;
        for (const auto& [n, f] : g_tab.families)
            for (const auto& p : f.part) specs += p.size();
        g_on.store(true);
        logger::info("[marks] start: ON - RaceMenu {} found (NiOverride); marks.json: {} file codes ({} files ok, {} bad), {} families ({} marks), {} IE "
                     "sources, choke '{}' / faded '{}'; bands Face {}-{} Body {}-{} Hands {} Feet {} choke Face {}; Injury_Engine.esp {}{}; IsBeastRace "
                     "keyword {}; the IE ping sink (Injury_Engine_Injury) registered; no QueueNiNodeUpdate (AddOverlays + Wait 0.5 for a new actor); "
                     "player {}; everyone else IE wounds (humanoid adults)",
                     g_skee, g_tab.files.size(), g_tab.filesOk, g_tab.filesBad, g_tab.families.size(), specs, g_tab.sources.size(), g_tab.chokeMark,
                     g_tab.chokeFaded, cfg.faceFirst, cfg.faceLast, cfg.bodyFirst, cfg.bodyLast, cfg.handsSlot, cfg.feetSlot, cfg.chokeSlot,
                     iePl ? "loaded" : "NOT loaded (the IE mirror stays idle; the choke mark works)",
                     g_ieDll ? ", Injury_Engine.dll loaded" : iePl ? ", but Injury_Engine.dll is NOT loaded (the IE mirror is off)" : "",
                     g_kwBeast ? std::format("0x{:08X}", g_kwBeast->GetFormID()) : std::string("not found (beast heads get face marks)"),
                     cfg.markPlayer ? "too (bMarkPlayer 1)" : "NOT (bMarkPlayer 0)");
    }

    void OnPreLoadGame()
    {
        ++g_gen;
        std::scoped_lock lk(g_mtx);
        g_reading.clear();
        g_applying.clear();
        g_asking.clear();
        g_wantRead.clear();
        g_lastGaps.clear();
        g_sweep          = Sweep{ g_sweep.serial + 1 };
        g_reconcileArmed = false;
        g_hasLastTick    = false;
        for (auto& [id, r] : g_recs) {
            ++r.live;
            r.healLast.clear();
        }
    }

    void OnGameReady(bool a_postLoad)
    {
        g_ping.store(false);
        g_painterAbsent.store(false);   // m34c: asked again after every load (VmCall's cache is cleared too)
        g_painterSaid.store(false);
        g_ieDead.store(false);
        g_ieEmpty.store(0);
        g_ieDllSaid.store(false);
        g_sweepOld.store(false);   // m34d: the batched sweep is tried again after every load (VmCall's cache is cleared too)
        {
            std::scoped_lock lk(g_inMtx);
            g_named.clear();
            g_namedPrev.clear();
        }
        std::scoped_lock lk(g_mtx);
        g_hasLastTick    = false;
        g_reconcileArmed = a_postLoad && !g_recs.empty();
        g_reconcileAt    = Clock::now() + std::chrono::seconds(5);
        if (g_on.load() && a_postLoad && !g_recs.empty())
            logger::info("[marks] load: {} marked actor(s) restored - reconciled 5 s after the load, each when she has 3D", g_recs.size());
    }

    void NoteHit(RE::FormID a_target)
    {
        // m34c: with SBKO_Marks.pex missing nothing is collected (the list is emptied by the check, one line)
        if (!g_on.load(std::memory_order_relaxed) || g_painterAbsent.load(std::memory_order_relaxed)) return;
        std::scoped_lock lk(g_inMtx);
        g_named.insert(a_target);
        g_pNamed.fetch_add(1, std::memory_order_relaxed);
    }

    void NoteEffect(RE::FormID a_target, RE::FormID a_caster, RE::FormID a_mgef)
    {
        if (!g_on.load(std::memory_order_relaxed) || g_idCount.load(std::memory_order_relaxed) == 0) return;
        {
            std::scoped_lock lk(g_idMtx);
            if (!g_ids.contains(a_target)) return;
        }
        // an ability (a 3D load re-applies dozens) is never a heal - dropped here, as Knockout's sink does (one lookup + one field)
        if (const auto* m = RE::TESForm::LookupByID<RE::EffectSetting>(a_mgef);
            !m || m->data.castingType == RE::MagicSystem::CastingType::kConstantEffect)
            return;
        const std::uint32_t gen = g_gen.load();
        TaskRelay::Add([a_target, a_caster, a_mgef, gen] { OnEffectTask(a_target, a_caster, a_mgef, gen); });
    }

    namespace
    {
        // a magic effect landed on a marked actor: a HEAL from an actor (herself included - a fed potion) names her for IE's next ping, and
        // takes fChokeHealShare of what is left of her choke mark's life (one cast = one heal: DDAB's stream rule)
        void OnEffectTask(RE::FormID a_tid, RE::FormID a_cid, RE::FormID a_mgef, std::uint32_t a_gen)
        {
            if (a_gen != g_gen.load()) return;
            auto* m = RE::TESForm::LookupByID<RE::EffectSetting>(a_mgef);
            if (!Knockout::IsHealEffect(m)) return;
            RE::Actor*  ca          = nullptr;
            std::string caster;
            if (a_cid == a_tid) {
                caster = "her own potion / spell";
            } else if (a_cid) {
                auto* cf = RE::TESForm::LookupByID(a_cid);
                ca       = cf ? cf->As<RE::Actor>() : nullptr;
                if (!ca) return;   // a non-actor caster is never a heal (Knockout's rule)
                caster = ca->IsPlayerRef() ? std::string("the player") : Who(ca, a_cid);
            } else
                return;
            {
                std::scoped_lock lk(g_inMtx);
                g_named.insert(a_tid);   // IE re-reads her at its ping
            }
            const auto& cfg   = Settings::Get();
            const float hours = HoursNow();
            const bool  conc  = m->data.castingType == RE::MagicSystem::CastingType::kConcentration;
            const char* mn    = m->GetName();
            std::string line;
            {
                std::scoped_lock lk(g_mtx);
                auto             it = g_recs.find(a_tid);
                if (it == g_recs.end()) return;
                Rec& r = it->second;
                for (auto& mk : r.marks) {
                    if (mk.kind != kChoke || mk.remove || !(cfg.chokeHealShare > 0.0f)) continue;
                    const auto now  = Clock::now();
                    auto&      last = r.healLast[a_cid];
                    const bool same = last != Clock::time_point{} &&
                                      std::chrono::duration<double>(now - last).count() < (conc ? kStreamConcS : kStreamFfS);
                    last = now;
                    if (same) break;   // the same cast still arriving - one heal
                    const float left = mk.endH - hours;
                    if (!(left > 0.0f)) break;
                    const float cut = left * (std::min)(cfg.chokeHealShare, 1.0f);
                    mk.endH  -= cut;
                    mk.bornH -= cut;
                    ++mk.heals;
                    r.wantApply = true;
                    g_pHeals.fetch_add(1, std::memory_order_relaxed);
                    line = std::format("[marks] choke {}: healed by {} ({}) from {} - life left {:.1f} -> {:.1f} game h (heal {})", r.who,
                                       mn && *mn ? mn : "a healing effect", conc ? "concentration" : "one cast / potion", caster, left, left - cut,
                                       mk.heals);
                    break;
                }
            }
            if (!line.empty() && cfg.logMarks) logger::info("{}", line);
        }
    }

    bool AnyRecords() noexcept { return g_idCount.load(std::memory_order_relaxed) != 0; }

    void OnAttach(RE::FormID a_id)
    {
        if (!g_on.load()) return;
        std::scoped_lock lk(g_mtx);
        if (auto it = g_recs.find(a_id); it != g_recs.end()) it->second.reconcile = true;
    }

    void OnDetach(RE::FormID a_id)
    {
        if (!g_on.load()) return;
        std::string line;
        {
            std::scoped_lock lk(g_mtx);
            g_reading.erase(a_id);
            // m34c: g_applying is NOT cleared here - a running Apply keeps its in-flight flag until its reply is booked, so a
            // quick re-attach can never start a second painter call on her at the same time
            g_asking.erase(a_id);
            g_wantRead.erase(a_id);
            g_lastGaps.erase(a_id);
            auto it = g_recs.find(a_id);
            if (it == g_recs.end()) return;
            Rec& r = it->second;
            ++r.live;
            r.reconcile = true;
            r.wantApply = false;
            r.healLast.clear();
            if (Settings::Get().logMarks)
                line = std::format("[marks] unload {}: live state dropped, record kept ({} mark(s))", r.who, r.marks.size());
        }
        if (!line.empty()) logger::info("{}", line);
    }

    void OnDeath(RE::FormID a_id)
    {
        if (!g_on.load()) return;
        std::string line;
        {
            std::scoped_lock lk(g_mtx);
            g_reading.erase(a_id);
            g_applying.erase(a_id);
            g_asking.erase(a_id);
            g_wantRead.erase(a_id);
            g_lastGaps.erase(a_id);
            auto it = g_recs.find(a_id);
            if (it == g_recs.end()) return;
            line = std::format("[marks] {} died - her {} mark(s) stay on the body; record dropped (no more reads)", it->second.who,
                               it->second.marks.size());
            g_recs.erase(it);
            SyncIds();
        }
        if (Settings::Get().logMarks) logger::info("{}", line);
    }

    void OnChoke(RE::FormID a_id, float a_seconds, bool a_passout, const char* a_why)
    {
        if (!g_on.load()) return;
        const auto& cfg = Settings::Get();
        auto*       a   = RE::TESForm::LookupByID<RE::Actor>(a_id);
        const std::string who = a ? Who(a, a_id) : std::format("0x{:08X}", a_id);
        auto say = [&](const std::string& s) {
            if (cfg.logMarks) logger::info("[marks] choke {}: {}", who, s);
        };
        if (!cfg.chokeMark) return say(std::format("held {:.1f} s ({}) - no mark (bChokeMark 0)", a_seconds, a_why));
        if (!a || sbko::IsDeadNow(a)) return say("dead / not in memory - no mark");
        if (a->IsPlayerRef()) return say("that is the player - no choke mark on the player");
        if (!IsHumanoid(a) || IsChildActor(a)) return say("not a humanoid adult - no mark");
        // m34c: the choke art is Collygon FACE art (the human head UV) - a Khajiit / Argonian gets none (bBeastFaceMarks 0)
        if (IsBeast(a) && !cfg.beastFaceMarks)
            return say("a Khajiit / Argonian head - no choke mark (bBeastFaceMarks 0: the art is drawn for human heads)");
        const bool pass = a_passout || a_seconds >= cfg.chokePassoutSeconds;
        float      base = 0.0f, life = 0.0f;
        int        step = 0;
        if (pass) {
            step = a_seconds < cfg.chokeSteps[0] ? 1 : a_seconds < cfg.chokeSteps[1] ? 2 : 3;
            base = cfg.stageAlphas[step - 1];
            life = cfg.chokeHoursPerStep * static_cast<float>(step);
        } else {
            if (!cfg.chokeMarkBeforePassout)
                return say(std::format("let go after {:.1f} s, before the passout ({}) - no mark (bChokeMarkBeforePassout 0)", a_seconds, a_why));
            base = cfg.alphaFloor;
            life = cfg.chokeHoursPerStep * std::clamp(a_seconds / cfg.chokePassoutSeconds, 0.0f, 1.0f);
        }
        if (!(life > 0.0f)) return say(std::format("held {:.1f} s ({}) - a life of 0 game h: no mark", a_seconds, a_why));
        const bool  female = IsFemale(a);
        std::string path;
        if (!PathFor(g_tab.chokeMark, false, female, path)) return say(std::format("its art ('{}') is missing - no mark", g_tab.chokeMark));
        const float hours = HoursNow();
        std::string what;
        std::vector<std::string> idLogs;
        {
            std::scoped_lock lk(g_mtx);
            // m34c: an existing record must still be HER
            if (auto old = g_recs.find(a_id); old != g_recs.end() && IdentityCheck(old->second, a, a_id, idLogs) == 1) g_recs.erase(old);
            auto [it, fresh] = g_recs.try_emplace(a_id);
            Rec& r = it->second;
            if (fresh) {
                r.female = female;
                r.base   = BaseKey(a);
            }
            r.who   = who;
            // m34c: ONE choke mark per neck - a choke mark being cleared (the console's 'clear', its end) is ADOPTED as the
            // new one (its slot stays SBKO's; no second mark, no clear + repaint of Face 16 every check)
            Mark* m       = nullptr;
            Mark* removed = nullptr;
            for (auto& o : r.marks)
                if (o.kind == kChoke) (o.remove ? removed : m) = &o;
            if (!m && removed) {
                removed->remove   = false;
                removed->permFail = false;
                removed->noSlot   = false;
                removed->lostOnce = false;
                removed->base     = 0.0f;   // a fresh life below
                removed->bornH    = hours;
                removed->endH     = hours;
                removed->heals    = 0;
                removed->seconds  = 0.0f;
                removed->third    = 3;
                m                 = removed;
            }
            if (!m) {
                Mark nm;
                nm.uid   = ++g_uid;
                nm.kind  = kChoke;
                nm.area  = kFace;
                nm.first = nm.last = static_cast<std::uint8_t>(cfg.chokeSlot);
                nm.code  = g_tab.chokeMark;
                nm.path  = path;
                nm.base  = base;
                nm.seconds = a_seconds;
                nm.bornH = hours;
                nm.endH  = hours + life;
                nm.tint  = g_tab.chokeTint;
                nm.alpha = Shape(base);
                nm.third = 3;
                r.marks.push_back(std::move(nm));
                what = std::format("{} -> step {:.2f}, life {:.1f} game h, Face[{}] '{}'",
                                   pass ? std::format("held {:.1f} s", a_seconds)
                                        : std::format("let go after {:.1f} s, before the passout (a light mark)", a_seconds),
                                   base, life, cfg.chokeSlot, FileName(path));
            } else {
                const float left = m->endH - hours;
                bool        ch   = false;
                if (life > left) {
                    m->bornH = hours;
                    m->endH  = hours + life;
                    m->heals = 0;
                    ch       = true;
                }
                if (base > m->base + kEps) {
                    m->base = base;
                    ch      = true;
                }
                m->noSlot  = false;
                m->seconds = (std::max)(m->seconds, a_seconds);
                what = std::format("held {:.1f} s again -> {} (step {:.2f}, {:.1f} game h left)", a_seconds,
                                   ch ? "renewed (the stronger step / the longer life wins)" : "kept (the mark there is stronger and longer)",
                                   m->base, m->endH - hours);
            }
            r.wantApply = true;
            SyncIds();
        }
        for (const auto& l : idLogs) logger::info("{}", l);
        say(std::format("{} ({})", what, a_why));
    }

    void Tick(const std::vector<RE::NiPointer<RE::Actor>>& a_loaded)
    {
        if (!g_on.load()) return;
        const auto  t0  = Clock::now();
        const auto& cfg = Settings::Get();
        auto*       vm  = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!VmCall::ClassPresent(vm, kScript, kWhat)) {
            // m34c: no painter = the feature is off - the hit list is emptied and no longer filled (asked again after a load)
            g_painterAbsent.store(true);
            g_ping.store(false);
            {
                std::scoped_lock lk(g_inMtx);
                g_named.clear();
                g_namedPrev.clear();
            }
            if (!g_painterSaid.exchange(true))
                logger::warn("[marks] SBKO_Marks.pex is missing - the skin marks are OFF this session (no hit is collected, nothing is read or painted)");
            return;
        }
        g_painterAbsent.store(false);
        // m34c: IE counts only when its script AND its DLL answer (Injury_Engine.dll loaded, natives not found dead)
        const bool ieScript = VmCall::ClassPresent(vm, kIEScript, kIEWhat);
        if (ieScript && !g_ieDll && !g_ieDllSaid.exchange(true))
            logger::warn("[marks] Injury Engine's script is installed but Injury_Engine.dll is NOT loaded - the IE mirror is OFF this session (the choke "
                         "mark still works)");
        const bool  ie    = ieScript && g_ieDll && !g_ieDead.load();
        const float hours = HoursNow();
        bool        perfDue = false;

        // IE's ping + the actors a hit / heal named in this check and the one before (the hit event comes just before IE's damage hook)
        const bool              ping = g_ping.exchange(false);
        std::vector<RE::FormID> named;
        {
            std::scoped_lock lk(g_inMtx);
            if (ping) {
                named.assign(g_named.begin(), g_named.end());
                for (const auto id : g_namedPrev)
                    if (!g_named.contains(id)) named.push_back(id);
                g_named.clear();
                g_namedPrev.clear();
            } else {
                g_namedPrev.swap(g_named);
                g_named.clear();
            }
        }
        // the sweep's candidates (reads only, before our lock): every loaded humanoid adult + the player
        std::vector<RE::FormID> sweepIds;
        if (ping && ie && named.empty()) {
            for (const auto& p : a_loaded)
                if (!NotEligible(p.get())) sweepIds.push_back(p->GetFormID());
            if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && !NotEligible(pc)) sweepIds.push_back(kPlayerId);
        }
        std::vector<Job>         jobs;
        std::vector<std::string> logs;
        {
            std::scoped_lock lk(g_mtx);
            const auto       now = Clock::now();
            const float      dt  = g_hasLastTick ? std::chrono::duration<float>(now - g_lastTick).count() : 0.0f;
            g_lastTick           = now;
            g_hasLastTick        = true;
            g_perfSeconds += dt;
            if (g_perfSeconds >= 60.0f && cfg.perfMarks) perfDue = true;

            bool reconcileAll = false;
            if (g_reconcileArmed && now >= g_reconcileAt) {
                g_reconcileArmed = false;
                reconcileAll     = true;
                // a created (FF) actor whose form is gone after the load was deleted by the game: her record goes
                for (auto it = g_recs.begin(); it != g_recs.end();) {
                    if ((it->first >> 24) == 0xFF && !RE::TESForm::LookupByID(it->first)) {
                        logs.push_back(std::format("[marks] load: 0x{:08X} is a created actor whose form is gone - record dropped", it->first));
                        it = g_recs.erase(it);
                    } else
                        ++it;
                }
            }
            if (ping && ie) {
                if (!named.empty()) {
                    for (const auto id : named)
                        if (!g_wantRead.contains(id)) g_wantRead[id] = "IE ping + a hit / heal on her";
                } else {
                    g_pSweeps.fetch_add(1, std::memory_order_relaxed);
                    g_sweep = Sweep{ g_sweep.serial + 1 };
                    // m34d: the unmarked candidates go into ONE call (kind 3) instead of one call each - the same candidates, the same
                    // in-flight set; an older SBKO_Marks.pex (g_sweepOld) -> one call per actor (kind 2), as m34c did
                    const bool perActor = g_sweepOld.load();
                    Job        batch;
                    batch.kind  = 3;
                    batch.sweep = g_sweep.serial;
                    for (const auto id : sweepIds) {
                        if (g_recs.contains(id)) {
                            if (!g_wantRead.contains(id)) g_wantRead[id] = "IE ping (no named actor) - a marked actor";
                            ++g_sweep.marked;
                        } else if (!g_asking.contains(id) && !g_reading.contains(id)) {
                            g_asking.insert(id);
                            if (perActor) {
                                Job j;
                                j.kind  = 2;
                                j.id    = id;
                                j.sweep = g_sweep.serial;
                                jobs.push_back(std::move(j));
                            } else
                                batch.ids.push_back(id);
                            ++g_sweep.asked;
                        }
                    }
                    if (!batch.ids.empty()) {
                        batch.marked = static_cast<std::uint32_t>(g_sweep.marked);
                        jobs.push_back(std::move(batch));
                    }
                    // m34c: no line for a sweep that asked nobody / found nobody - only OnAskReply's line when it found someone
                }
            }
            std::vector<RE::FormID> dead, gone, other;
            for (auto& [id, r] : g_recs) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!a) {
                    // m34c: a created (FF) actor not in memory was deleted - her record goes NOW (the engine reuses FF ids)
                    if ((id >> 24) == 0xFF) gone.push_back(id);
                    continue;
                }
                if (sbko::IsDeadNow(a)) {   // a death the sink missed
                    dead.push_back(id);
                    continue;
                }
                if (reconcileAll) r.reconcile = true;
                if (!a->Is3DLoaded()) continue;
                // m34c: still HER? (sex; a created id's base) - at every check she has 3D, so also at a load's reconcile
                if (IdentityCheck(r, a, id, logs) == 1) {
                    other.push_back(id);
                    continue;
                }
                bool ieMarks = false, realMarks = false, tortMarks = false;
                for (const auto& m : r.marks)
                    if (m.kind == kIE && !m.remove) {
                        ieMarks = true;
                        (m.real ? realMarks : tortMarks) = true;
                    }
                if (ieMarks && !ie && reconcileAll) {   // IE was uninstalled: its marks go (one line)
                    for (auto& m : r.marks)
                        if (m.kind == kIE) m.remove = true;
                    r.wantApply = true;
                    logs.push_back(std::format("[marks] {}: Injury Engine is not installed any more - her IE marks are cleared", r.who));
                    ieMarks = false;
                }
                if (ie && ieMarks && !g_wantRead.contains(id)) {
                    if (realMarks && r.hpAtRead > -0.5f && std::fabs(HealthFrac(a) - r.hpAtRead) >= 0.01f)
                        g_wantRead[id] = "her Health moved a whole percent";
                    else if (tortMarks && r.hourAtRead >= 0.0f && std::floor(hours) != std::floor(r.hourAtRead))
                        g_wantRead[id] = "a game hour passed (torture marks drain)";
                }
                if (r.reconcile) {
                    if (ie && ieMarks) {
                        if (!g_wantRead.contains(id)) g_wantRead[id] = "reconcile (a load / her 3D loading)";
                    } else
                        r.wantApply = true;
                }
                if (ChokeStep(r, r.female, hours, logs)) r.wantApply = true;
                if (r.wantApply && !g_applying.contains(id) && !g_reading.contains(id) && !g_wantRead.contains(id)) {
                    Job j;
                    BuildOps(r, r.reconcile, j.ops, j.refs);
                    r.reconcile = false;
                    r.wantApply = false;
                    if (!j.refs.empty()) {
                        g_applying.insert(id);
                        j.kind = 1;
                        j.id   = id;
                        j.live = r.live;
                        jobs.push_back(std::move(j));
                    }
                }
            }
            for (const auto id : dead) {
                if (auto it = g_recs.find(id); it != g_recs.end()) {
                    logs.push_back(std::format("[marks] {} is dead - her marks stay on the body; record dropped", it->second.who));
                    g_recs.erase(it);
                }
            }
            for (const auto id : gone) {
                if (auto it = g_recs.find(id); it != g_recs.end()) {
                    logs.push_back(std::format("[marks] {}: a created actor no longer in memory (deleted) - record dropped ({} mark(s))", it->second.who,
                                               it->second.marks.size()));
                    g_recs.erase(it);
                }
                g_wantRead.erase(id);
            }
            for (const auto id : other) {   // a regenerated actor (IdentityCheck 1): forgotten, and IE reads her as a new one
                g_recs.erase(id);
                if (ie) g_wantRead[id] = "her record belonged to another actor - read anew";
            }
            std::erase_if(g_recs, [](const auto& kv) { return kv.second.marks.empty(); });
            // the reads (one in flight per actor; an actor not eligible / not loaded now is dropped from the list)
            for (auto it = g_wantRead.begin(); it != g_wantRead.end();) {
                const RE::FormID id = it->first;
                if (g_reading.contains(id)) {   // served after the running read
                    ++it;
                    continue;
                }
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!ie || NotEligible(a)) {
                    it = g_wantRead.erase(it);
                    continue;
                }
                g_reading.insert(id);
                Job j;
                j.kind = 0;
                j.id   = id;
                j.why  = it->second;
                jobs.push_back(std::move(j));
                it = g_wantRead.erase(it);
            }
            SyncIds();
        }
        for (const auto& l : logs) logger::info("{}", l);
        for (auto& j : jobs) Dispatch(j);
        NoteWorst(t0);
        if (perfDue) EmitPerf(false);
    }

    // ---- co-save 'MARK' v2 ------------------------------------------------------------------------------------------------------------------
    // count u32; per actor: id u32, female u8, base u32 (v2: BaseKey - 0 = not known), marks u16; per mark: kind u8, area u8, slot i8, first u8, last u8, code, path, shownPath (str =
    // u16 length + bytes), alpha f32, shownAlpha f32, tint i32, shownTint i32, source, part, stage (str), real u8, bornH f32, endH f32, base f32,
    // heals u32, seconds f32 (the choke's seconds held). ⛔ Writer and reader change TOGETHER, with the version.
    namespace
    {
        void WriteStr(SKSE::SerializationInterface* a_intf, const std::string& a_s)
        {
            const auto n = static_cast<std::uint16_t>((std::min)(a_s.size(), static_cast<std::size_t>(0xFFFF)));
            a_intf->WriteRecordData(n);
            if (n) a_intf->WriteRecordData(a_s.data(), n);
        }
        bool ReadStr(SKSE::SerializationInterface* a_intf, std::string& a_s)
        {
            std::uint16_t n = 0;
            if (a_intf->ReadRecordData(n) != sizeof n) return false;
            a_s.assign(n, '\0');
            return n == 0 || a_intf->ReadRecordData(a_s.data(), n) == n;
        }
    }

    void Save(SKSE::SerializationInterface* a_intf)
    {
        std::scoped_lock lk(g_mtx);
        if (!a_intf->OpenRecord(kRecordType, kRecVersion)) {
            logger::warn("[co-save] could not open the marks record - the skin marks' state is not saved (the skin keeps them; RaceMenu saves those)");
            return;
        }
        a_intf->WriteRecordData(static_cast<std::uint32_t>(g_recs.size()));
        std::size_t n = 0;
        for (const auto& [id, r] : g_recs) {
            a_intf->WriteRecordData(id);
            a_intf->WriteRecordData(static_cast<std::uint8_t>(r.female ? 1 : 0));
            a_intf->WriteRecordData(r.base);   // v2
            a_intf->WriteRecordData(static_cast<std::uint16_t>(r.marks.size()));
            for (const auto& m : r.marks) {
                a_intf->WriteRecordData(static_cast<std::uint8_t>(m.kind));
                a_intf->WriteRecordData(m.area);
                a_intf->WriteRecordData(m.slot);
                a_intf->WriteRecordData(m.first);
                a_intf->WriteRecordData(m.last);
                WriteStr(a_intf, m.code);
                WriteStr(a_intf, m.path);
                WriteStr(a_intf, m.shownPath);
                a_intf->WriteRecordData(m.alpha);
                a_intf->WriteRecordData(m.shownAlpha);
                a_intf->WriteRecordData(m.tint);
                a_intf->WriteRecordData(m.shownTint);
                WriteStr(a_intf, m.source);
                WriteStr(a_intf, m.part);
                WriteStr(a_intf, m.stage);
                a_intf->WriteRecordData(static_cast<std::uint8_t>(m.real ? 1 : 0));
                a_intf->WriteRecordData(m.bornH);
                a_intf->WriteRecordData(m.endH);
                a_intf->WriteRecordData(m.base);
                a_intf->WriteRecordData(m.heals);
                a_intf->WriteRecordData(m.seconds);
                ++n;
            }
        }
        if (!g_recs.empty()) logger::info("[co-save] saved {} marked actor(s), {} mark(s) (record 'MARK' v{})", g_recs.size(), n, kRecVersion);
    }

    void Load(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, const std::function<RE::FormID(RE::FormID, std::string&)>& a_resolve)
    {
        if (a_version < 1 || a_version > kRecVersion) {
            logger::warn("[co-save] marks record version {} is not {} - discarded (RaceMenu keeps what is on the skin; SBKO forgets it)", a_version,
                         kRecVersion);
            return;
        }
        std::scoped_lock lk(g_mtx);
        g_recs.clear();
        std::uint32_t count = 0;
        if (a_intf->ReadRecordData(count) != sizeof count) return;
        std::size_t marks = 0, dropped = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID    raw = 0, base = 0;
            std::uint8_t  fem = 1;
            std::uint16_t nm  = 0;
            if (a_intf->ReadRecordData(raw) != sizeof raw || a_intf->ReadRecordData(fem) != sizeof fem ||
                (a_version >= 2 && a_intf->ReadRecordData(base) != sizeof base) || a_intf->ReadRecordData(nm) != sizeof nm) {
                logger::warn("[co-save] the marks record is cut after {} of {} actor(s)", i, count);
                break;
            }
            Rec  r;
            r.female = fem != 0;
            r.base   = base;   // v1: 0 = filled in at her first sight
            bool cut = false;
            for (std::uint16_t k = 0; k < nm; ++k) {
                Mark         m;
                std::uint8_t kind = 0, real = 1;
                const bool   whole =
                    a_intf->ReadRecordData(kind) == sizeof kind && a_intf->ReadRecordData(m.area) == sizeof m.area &&
                    a_intf->ReadRecordData(m.slot) == sizeof m.slot && a_intf->ReadRecordData(m.first) == sizeof m.first &&
                    a_intf->ReadRecordData(m.last) == sizeof m.last && ReadStr(a_intf, m.code) && ReadStr(a_intf, m.path) &&
                    ReadStr(a_intf, m.shownPath) && a_intf->ReadRecordData(m.alpha) == sizeof m.alpha &&
                    a_intf->ReadRecordData(m.shownAlpha) == sizeof m.shownAlpha && a_intf->ReadRecordData(m.tint) == sizeof m.tint &&
                    a_intf->ReadRecordData(m.shownTint) == sizeof m.shownTint && ReadStr(a_intf, m.source) && ReadStr(a_intf, m.part) &&
                    ReadStr(a_intf, m.stage) && a_intf->ReadRecordData(real) == sizeof real && a_intf->ReadRecordData(m.bornH) == sizeof m.bornH &&
                    a_intf->ReadRecordData(m.endH) == sizeof m.endH && a_intf->ReadRecordData(m.base) == sizeof m.base &&
                    a_intf->ReadRecordData(m.heals) == sizeof m.heals && a_intf->ReadRecordData(m.seconds) == sizeof m.seconds;
                if (!whole) {
                    cut = true;
                    break;
                }
                m.kind = static_cast<Kind>(kind <= 2 ? kind : 2);
                m.real = real != 0;
                m.uid  = ++g_uid;
                if (m.slot < 0) m.shownPath.clear();   // m34c: on her skin only with the slot SBKO wrote
                r.marks.push_back(std::move(m));
            }
            if (cut) {
                logger::warn("[co-save] the marks record is cut inside actor 0x{:08X} - the rest is lost", raw);
                break;
            }
            std::string      how;
            const RE::FormID id = a_resolve ? a_resolve(raw, how) : raw;
            if (!id) {
                ++dropped;
                logger::info("[co-save] marks of 0x{:08X} dropped: {}", raw, how);
                continue;
            }
            r.who     = std::format("0x{:08X}", id);
            r.reconcile = true;
            marks += r.marks.size();
            if (!r.marks.empty()) g_recs[id] = std::move(r);
        }
        SyncIds();
        logger::info("[co-save] restored {} marked actor(s), {} mark(s){} (record 'MARK' v{}) - reconciled once each has 3D", g_recs.size(), marks,
                     dropped ? std::format(", {} dropped", dropped) : std::string(), a_version);
    }

    void Revert()
    {
        ++g_gen;
        std::scoped_lock lk(g_mtx);
        g_recs.clear();
        g_reading.clear();
        g_applying.clear();
        g_asking.clear();
        g_wantRead.clear();
        g_lastGaps.clear();
        g_reconcileArmed = false;
        SyncIds();
    }

    void CollectIds(std::vector<RE::FormID>& a_out)
    {
        std::scoped_lock lk(g_mtx);
        for (const auto& [id, r] : g_recs) a_out.push_back(id);
    }

    // ---- console ----------------------------------------------------------------------------------------------------------------------------
    const char* ConsoleHelpLine() noexcept
    {
        return "sbko marks [status | ie | slots | sync | paint <code> [light|medium|severe] | alpha <code> <0-1> | clear [<code>|all] | wound <source> "
               "<part> [points] | heal | choke <seconds> | age <game hours> | perf] - on the selected actor (none = you)";
    }

    std::string Console(RE::Actor* a, const std::vector<std::string>& w)
    {
        const auto&       cfg = Settings::Get();
        const std::string sub = w.empty() ? std::string("status") : w[0];
        if (!a) return "sbko marks: no actor";
        const RE::FormID  id   = a->GetFormID();
        const std::string who  = Who(a, id);
        const std::string name = NameOf(a);
        if (!g_on.load()) return "sbko marks: OFF (bMarks 0, marks.json or RaceMenu missing) - see log";
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!VmCall::ClassPresent(vm, kScript, kWhat)) return "sbko marks: SBKO_Marks.pex is not installed";
        auto num = [](const std::string& s, float& out) {
            try {
                std::size_t used = 0;
                out              = std::stof(s, &used);
                return used == s.size();
            } catch (...) {
                return false;
            }
        };
        auto upper = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        };
        logger::info("[marks] console 'sbko marks {}' on {}", sub, who);
        if (sub == "status" || sub == "show") {
            std::size_t ieN = 0, chN = 0, mnN = 0;
            {
                std::scoped_lock lk(g_mtx);
                const auto       it = g_recs.find(id);
                if (it == g_recs.end()) {
                    logger::info("[marks] status {}: no record (no mark of SBKO's on her){}", who,
                                 g_reading.contains(id) ? " - an IE read is in flight" : "");
                } else {
                    const Rec& r     = it->second;
                    const float hours = HoursNow();
                    logger::info("[marks] status {}: {} mark(s), {} | read in flight {}, apply in flight {}, reconcile {}, last IE read: Health {:.1f}% "
                                 "at hour {:.2f} | IE said: {}",
                                 who, r.marks.size(), r.female ? "female files" : "male files", g_reading.contains(id) ? "yes" : "no",
                                 g_applying.contains(id) ? "yes" : "no", r.reconcile ? "pending" : "no", r.hpAtRead * 100.0f, r.hourAtRead,
                                 r.lastSig.empty() ? std::string("(nothing)") : r.lastSig);
                    for (const auto& m : r.marks) {
                        (m.kind == kIE ? ieN : m.kind == kChoke ? chN : mnN) += 1;
                        logger::info("[marks] status {}:   {} {} '{}' {}[{}] band {}-{} alpha {:.2f} (shown {}) tint {:06X}{}{}{}", who, KindName(m.kind),
                                     m.code, FileName(m.path), AreaName(m.area), m.slot >= 0 ? std::to_string(m.slot) : std::string("?"),
                                     static_cast<int>(m.first), static_cast<int>(m.last), m.alpha,
                                     m.shownPath.empty() ? std::string("not on her skin") : std::format("{:.2f}", m.shownAlpha), m.tint,
                                     m.kind == kIE ? std::format(" | {} {} {} ({})", m.source, m.part, m.stage, m.real ? "real" : "torture") : std::string(),
                                     m.kind == kChoke ? std::format(" | held {:.1f} s, step {:.2f}, third {}, {:.1f} of {:.1f} game h left, heals {}",
                                                                    m.seconds, m.base, ChokeThird(m, hours), m.endH - hours, m.endH - m.bornH, m.heals)
                                                      : std::string(),
                                     m.remove ? " | being cleared" : m.noSlot ? " | no free slot" : m.permFail ? " | given up" : "");
                    }
                }
            }
            return std::format("{}: {} SBKO mark(s) (IE {}, choke {}, by hand {}) - see log", name, ieN + chN + mnN, ieN, chN, mnN);
        }
        if (sub == "perf") {
            EmitPerf(true);
            return "marks PERF counters written to the log";
        }
        if (sub == "choke") {
            float s = 27.0f;
            if (w.size() > 1 && (!num(w[1], s) || !(s > 0.0f) || s > 600.0f)) return std::format("sbko marks choke: '{}' is not seconds (1-600)", w[1]);
            OnChoke(id, s, s >= cfg.chokePassoutSeconds, "console 'sbko marks choke'");
            return std::format("{}: choke mark for {:.0f} s - painted at the next check", name, s);
        }
        if (sub == "age") {
            float h = 0.0f;
            if (w.size() < 2 || !num(w[1], h) || !(h > 0.0f)) return "sbko marks age <game hours> - e.g. age 9";
            std::size_t n = 0;
            {
                std::scoped_lock lk(g_mtx);
                if (auto it = g_recs.find(id); it != g_recs.end())
                    for (auto& m : it->second.marks)
                        if (m.kind == kChoke && !m.remove) {
                            m.bornH -= h;
                            m.endH -= h;
                            it->second.wantApply = true;
                            ++n;
                        }
            }
            logger::info("[marks] age {}: choke mark clock moved {:.1f} game h forward ({} mark(s))", who, h, n);
            return n ? std::format("{}: choke mark {:.0f} game h older - next check", name, h) : std::format("{}: no choke mark", name);
        }
        if (sub == "sync" || sub == "apply") {
            const bool ie = VmCall::ClassPresent(vm, kIEScript, kIEWhat) && g_ieDll && !g_ieDead.load();
            {
                std::scoped_lock lk(g_mtx);
                if (auto it = g_recs.find(id); it != g_recs.end()) it->second.reconcile = true;
                if (ie) g_wantRead[id] = "console 'sbko marks sync'";
            }
            return std::format("{}: IE read + repaint at the next check{}", name, ie ? "" : " (no Injury Engine: repaint only)");
        }
        if (sub == "ie") {
            if (!VmCall::ClassPresent(vm, kIEScript, kIEWhat) || !g_ieDll || g_ieDead.load())
                return "sbko marks ie: Injury Engine is not installed / its DLL is not loaded";
            if (const char* why = NotEligible(a)) return std::format("{}: no marks - {}", name, why);
            {
                std::scoped_lock lk(g_mtx);
                if (g_reading.contains(id)) return std::format("{}: a read is already in flight - try again", name);
                g_reading.insert(id);
            }
            Job j;
            j.kind = 0;
            j.id   = id;
            j.why  = "console 'sbko marks ie'";
            j.dump = true;
            Dispatch(j);
            return std::format("{}: asked Injury Engine - wounds and plan go to the log", name);
        }
        if (sub == "slots") {
            const bool sent = VmCall::StaticIfPresentThen(kScript, "Slots", RE::MakeFunctionArguments(static_cast<std::int32_t>(id)), kWhat,
                                                          [who](const RE::BSScript::Variable& v) {
                                                              const std::string s = v.IsString() ? std::string(v.GetString()) : std::string("?");
                                                              logger::info("[marks] slots {}: {}", who, s.empty() ? std::string("(none written)") : s);
                                                          });
            return sent ? std::format("{}: every overlay slot goes to the log", name) : std::string("sbko marks slots: not dispatched");
        }
        if (sub == "heal") {
            const bool sent = VmCall::StaticIfPresent(kScript, "HealFull", RE::MakeFunctionArguments(static_cast<std::int32_t>(id)), kWhat);
            return sent ? std::format("{}: Health to full - IE trims, the marks follow", name) : std::string("sbko marks heal: not dispatched");
        }
        if (sub == "wound") {
            if (!VmCall::ClassPresent(vm, kIEScript, kIEWhat) || !g_ieDll || g_ieDead.load())
                return "sbko marks wound: Injury Engine is not installed / its DLL is not loaded";
            if (w.size() < 3) return "sbko marks wound <source> <part> [points] - parts face back larm rarm lleg rleg";
            static const std::pair<const char*, int> kParts[] = { { "face", 0 }, { "larm", 1 }, { "leftarm", 1 }, { "rarm", 2 }, { "rightarm", 2 },
                                                                  { "back", 3 }, { "lleg", 4 }, { "leftleg", 4 }, { "rleg", 5 }, { "rightleg", 5 } };
            int part = -1;
            for (const auto& [n, p] : kParts)
                if (w[2] == n) part = p;
            if (part < 0) return std::format("sbko marks wound: part '{}' unknown - face back larm rarm lleg rleg", w[2]);
            float pts = 30.0f;
            if (w.size() > 3 && (!num(w[3], pts) || !(pts > 0.0f) || pts > 100.0f)) return "sbko marks wound: points 1-100";
            const std::string src = w[1];
            const bool sent = VmCall::StaticIfPresentThen(
                kScript, "Wound",
                RE::MakeFunctionArguments(static_cast<std::int32_t>(id), static_cast<RE::BSFixedString>(src.c_str()), static_cast<std::int32_t>(part),
                                          static_cast<std::int32_t>(pts)),
                kWhat, [who, src](const RE::BSScript::Variable& v) {
                    logger::info("[marks] wound {}: Injury Engine answered {} for '{}' (a hit code / source index; -1 = refused - an unknown source or "
                                 "a dead actor)", who, v.IsInt() ? v.GetSInt() : -99, src);
                });
            return sent ? std::format("{}: {} wound on part {} asked of IE - marks at its ping", name, src, part) : std::string("not dispatched");
        }
        if (sub == "paint" || sub == "alpha" || sub == "clear") {
            const std::string code = w.size() > 1 ? upper(w[1]) : std::string();
            if (sub == "clear") {
                const bool all = code.empty() || code == "ALL";
                std::size_t n  = 0;
                {
                    std::scoped_lock lk(g_mtx);
                    if (auto it = g_recs.find(id); it != g_recs.end()) {
                        for (auto& m : it->second.marks)
                            if (all || _stricmp(m.code.c_str(), code.c_str()) == 0) {
                                m.remove = true;
                                ++n;
                            }
                        it->second.wantApply = true;
                    }
                }
                logger::info("[marks] clear {}: {} mark(s) of '{}' set to clear (IE marks come back at IE's next read while IE still lists the wound)",
                             who, n, all ? std::string("all") : code);
                return std::format("{}: {} mark(s) cleared at the next check", name, n);
            }
            const auto fit = g_tab.files.find(code);
            if (fit == g_tab.files.end()) return std::format("sbko marks {}: code '{}' is not in marks.json", sub, code);
            if (sub == "alpha") {
                float al = 0.0f;
                if (w.size() < 3 || !num(w[2], al) || al < 0.0f || al > 1.0f) return "sbko marks alpha <code> <0-1>";
                std::size_t n = 0;
                {
                    std::scoped_lock lk(g_mtx);
                    if (auto it = g_recs.find(id); it != g_recs.end()) {
                        for (auto& m : it->second.marks)
                            if (!m.remove && _stricmp(m.code.c_str(), code.c_str()) == 0) {
                                m.alpha = al;
                                if (m.kind == kChoke) m.base = al;
                                ++n;
                            }
                        it->second.wantApply = true;
                    }
                }
                return std::format("{}: {} mark(s) {} -> alpha {:.2f} (IE's next read may set it back)", name, n, code, al);
            }
            // paint: a mark BY HAND (no IE) - kept until 'sbko marks clear'
            if (const char* why = NotEligible(a)) return std::format("{}: no marks - {}", name, why);
            const std::string st = w.size() > 2 ? w[2] : std::string("medium");
            const StageInfo   si = StageOf(st);
            if (si.eff < 0 || si.heal) return "sbko marks paint <code> [light|medium|severe]";
            const FileDef& fd     = fit->second;
            const bool     female = IsFemale(a);
            std::string    path;
            const bool     right = fd.sided && ((Seed(id, "manual", code) >> 8) & 1u) != 0;
            if (!PathFor(code, right, female, path)) return std::format("sbko marks paint: the art of {} is missing", code);
            int first = 0, last = 0;
            BandOf(fd.area, first, last);
            std::vector<std::string> idLogs;
            {
                std::scoped_lock lk(g_mtx);
                // m34c: an existing record must still be HER
                if (auto old = g_recs.find(id); old != g_recs.end() && IdentityCheck(old->second, a, id, idLogs) == 1) g_recs.erase(old);
                auto [it, fresh] = g_recs.try_emplace(id);
                Rec& r           = it->second;
                if (fresh) {
                    r.female = female;
                    r.base   = BaseKey(a);
                }
                r.who   = who;
                Mark* m = FindMark(r, kManual, path);
                if (!m) {
                    Mark nm;
                    nm.uid   = ++g_uid;
                    nm.kind  = kManual;
                    nm.area  = static_cast<std::uint8_t>(fd.area);
                    nm.first = static_cast<std::uint8_t>(first);
                    nm.last  = static_cast<std::uint8_t>(last);
                    nm.code  = code;
                    nm.path  = path;
                    r.marks.push_back(std::move(nm));
                    m = &r.marks.back();
                }
                m->remove = false;
                m->noSlot = false;
                m->alpha  = Shape(si.alpha);
                m->tint   = fd.tint >= 0 ? fd.tint : 0x5A6EC8;
                m->stage  = st;
                r.wantApply = true;
                SyncIds();
                logger::info("[marks] paint {} {} '{}' {} {}-{} alpha {:.2f} tint {:06X} (by hand, {}; kept until 'sbko marks clear')", who, code,
                             FileName(path), AreaName(fd.area), first, last, m->alpha, m->tint, st);
            }
            for (const auto& l : idLogs) logger::info("{}", l);
            return std::format("{}: {} {} painted at the next check", name, code, st);
        }
        return std::format("sbko marks: unknown word '{}' - type sbko help", sub);
    }
}
