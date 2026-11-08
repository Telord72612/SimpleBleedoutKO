#include "PCH.h"
#include "Settings.h"

// The hand ini parser - shape copied from the SkyrimNetMouthBridge plugin:
// strip ;# comments, `key = value`, case-insensitive keys, unknown keys warned, a bad value keeps the default. Sections
// are tracked only so each logged line says where the key sat; a key is accepted in any section (the names are unique).
namespace SBKO::Settings
{
    namespace
    {
        Values g_values;

        std::string Trim(const std::string& s)
        {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return {};
            const auto e = s.find_last_not_of(" \t\r\n");
            return s.substr(b, e - b + 1);
        }

        std::string Lower(std::string s)
        {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool ParseBool(const std::string& v)
        {
            if (_stricmp(v.c_str(), "true") == 0) return true;
            if (_stricmp(v.c_str(), "false") == 0) return false;
            return std::stoi(v) != 0;
        }

        // Input sanity only (not gameplay limits): a value that cannot work is raised to the smallest that can, and the
        // log says so. Nothing is capped from above.
        float AtLeast(const char* a_key, float a_value, float a_min)
        {
            if (a_value >= a_min) return a_value;
            logger::warn("[ini] {} = {} cannot work - raised to {}", a_key, a_value, a_min);
            return a_min;
        }
        int AtLeast(const char* a_key, int a_value, int a_min)
        {
            if (a_value >= a_min) return a_value;
            logger::warn("[ini] {} = {} cannot work - raised to {}", a_key, a_value, a_min);
            return a_min;
        }

        // u6g: "a, b , c" -> {"a","b","c"} (each trimmed; empty pieces dropped; a repeated name - case-insensitive - kept once)
        std::vector<std::string> SplitList(const std::string& a_val)
        {
            std::vector<std::string> out;
            std::size_t              start = 0;
            while (start <= a_val.size()) {
                const auto        comma = a_val.find(',', start);
                const std::string piece = Trim(a_val.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!piece.empty() &&
                    std::none_of(out.begin(), out.end(), [&](const std::string& s) { return _stricmp(s.c_str(), piece.c_str()) == 0; }))
                    out.push_back(piece);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            return out;
        }

        std::string JoinList(const std::vector<std::string>& a_list)
        {
            std::string s;
            for (const auto& n : a_list) s += (s.empty() ? "" : ", ") + n;
            return s;
        }
    }

    const Values& Get() noexcept { return g_values; }

    void Load()
    {
        Values                          v;   // defaults = v10
        std::unordered_set<std::string> readKeys;   // lower-case names of the keys the file set
        // Relative to the game's working directory; MO2's VFS maps Data/ as usual.
        const std::filesystem::path path{ "Data/SKSE/Plugins/SimpleBleedoutKO.ini" };
        std::ifstream               in(path);
        if (!in) {
            logger::info("[ini] no {} - every value is the built-in default (the same values as the shipped ini)", path.generic_string());
        } else {
            logger::info("[ini] reading {}", path.generic_string());
            std::string line;
            std::string section = "(none)";
            int         n       = 0;
            while (std::getline(in, line)) {
                if (const auto c = line.find_first_of(";#"); c != std::string::npos) line.erase(c);
                const std::string t = Trim(line);
                if (t.empty()) continue;
                if (t.front() == '[') {
                    const auto close = t.find(']');
                    section          = close == std::string::npos ? t.substr(1) : t.substr(1, close - 1);
                    continue;
                }
                const auto eq = t.find('=');
                if (eq == std::string::npos) {
                    logger::warn("[ini] [{}] line '{}' has no '=' - ignored", section, t);
                    continue;
                }
                const std::string key = Trim(t.substr(0, eq));
                const std::string val = Trim(t.substr(eq + 1));
                if (key.empty()) continue;
                if (val.empty() && _stricmp(key.c_str(), "sHelmetIgnorePlugins") == 0) {   // u6g: empty = a real value (no plugin)
                    v.helmetIgnorePlugins.clear();
                    ++n;
                    readKeys.insert(Lower(key));
                    logger::info("[ini] [{}] {} = (empty) (read) - no plugin is ignored", section, key);
                    continue;
                }
                if (val.empty()) {
                    logger::warn("[ini] [{}] {} has no value - kept the default", section, key);
                    continue;
                }
                const auto is = [&](const char* k) { return _stricmp(key.c_str(), k) == 0; };
                try {
                    if (is("fWakeHealthFraction"))   v.wakeHealthFraction = AtLeast("fWakeHealthFraction", std::stof(val), 0.0f);
                    else if (is("iGraceChecks"))     v.graceChecks        = AtLeast("iGraceChecks", std::stoi(val), 0);
                    else if (is("fRecoverMinHours")) v.recoverMinHours    = AtLeast("fRecoverMinHours", std::stof(val), 0.0f);
                    else if (is("fRecoverMaxHours")) v.recoverMaxHours    = AtLeast("fRecoverMaxHours", std::stof(val), 0.0f);
                    else if (is("fCheckSeconds"))    v.checkSeconds       = AtLeast("fCheckSeconds", std::stof(val), 0.1f);
                    else if (is("fStageSeconds"))    v.stageSeconds       = AtLeast("fStageSeconds", std::stof(val), 0.0f);
                    else if (is("fBleedoutGraceSeconds")) v.bleedoutGraceSeconds = AtLeast("fBleedoutGraceSeconds", std::stof(val), 0.0f);
                    else if (is("fEssentialNonCombatHealRateBonus")) v.essentialRegenBonus = std::stof(val);   // < 0 = not touched
                    else if (is("fEssentialHealthPercentReGain")) v.essentialLethalHitHealth = std::stof(val);   // u2f; < 0 = not touched
                    else if (is("iCombatEndRefill")) {
                        const int m = std::stoi(val);
                        if (m < 0 || m > 2) {
                            logger::warn("[ini] [{}] iCombatEndRefill = {} is not 0, 1 or 2 - kept the default {}", section, val,
                                         v.combatEndRefill);
                            continue;
                        }
                        v.combatEndRefill = m;
                    }
                    else if (is("bNotify"))          v.notify             = ParseBool(val);
                    else if (is("bVerboseChecks"))   v.verboseChecks      = ParseBool(val);
                    else if (is("fUnconsciousHealthFraction"))
                        v.unconsciousHealthFraction = AtLeast("fUnconsciousHealthFraction", std::stof(val), 0.01f);
                    else if (is("bSeverActionsGuard"))  v.severActionsGuard  = ParseBool(val);
                    else if (is("bPotionsOff"))         v.potionsOff         = ParseBool(val);
                    else if (is("bLogEffects"))         v.logEffects         = ParseBool(val);
                    else if (is("bLogFootsteps"))       v.logFootsteps       = ParseBool(val);
                    else if (is("bRepairWalkingGraph")) v.repairWalkingGraph = ParseBool(val);
                    else if (is("bHealWake"))           v.healWake           = ParseBool(val);
                    // U6 [HeadHit]
                    else if (is("bHeadHit"))              v.headHit               = ParseBool(val);
                    else if (is("bHeadHitBash"))          v.headHitBash           = ParseBool(val);
                    else if (is("bHeadHitBlunt"))         v.headHitBlunt          = ParseBool(val);
                    else if (is("bHeadHitSneak"))         v.headHitSneak          = ParseBool(val);
                    else if (is("bHeadHitLowHealth"))     v.headHitLowHealth      = ParseBool(val);
                    else if (is("fHeadHitHealthThreshold"))
                        v.headHitHealthThreshold = AtLeast("fHeadHitHealthThreshold", std::stof(val), 0.0f);
                    else if (is("bHeadHitHelmetProtects")) v.headHitHelmetProtects = ParseBool(val);
                    else if (is("sHelmetIgnorePlugins"))  v.helmetIgnorePlugins   = SplitList(val);   // u6g
                    else if (is("bHeadHitFollowers"))     v.headHitFollowers      = ParseBool(val);
                    else if (is("bLogHeadHits"))          v.logHeadHits           = ParseBool(val);
                    // u6b (U5) [ChanceKO]
                    else if (is("bChanceKO"))             v.chanceKO              = ParseBool(val);
                    else if (is("fChanceKOHealthBelow"))
                        v.chanceKOHealthBelow = AtLeast("fChanceKOHealthBelow", std::stof(val), 0.0f);
                    else if (is("bChanceKOCreatureHits")) v.chanceKOCreatureHits  = ParseBool(val);
                    else if (is("bLogChanceKO"))          v.logChanceKO           = ParseBool(val);
                    else if (is("fChanceKOHelmetOn"))     v.chanceKOHelmetOn      = std::stof(val);
                    else if (is("fChanceKOHelmetOff"))    v.chanceKOHelmetOff     = std::stof(val);
                    else if (is("fChanceKOFeetHeavy"))    v.chanceKOFeetHeavy     = std::stof(val);
                    else if (is("fChanceKOFeetLight"))    v.chanceKOFeetLight     = std::stof(val);
                    else if (is("fChanceKOFeetNone"))     v.chanceKOFeetNone      = std::stof(val);
                    else if (is("fChanceKOHandsHeavy"))   v.chanceKOHandsHeavy    = std::stof(val);
                    else if (is("fChanceKOHandsLight"))   v.chanceKOHandsLight    = std::stof(val);
                    else if (is("fChanceKOHandsNone"))    v.chanceKOHandsNone     = std::stof(val);
                    else if (is("fChanceKOBodyHeavy"))    v.chanceKOBodyHeavy     = std::stof(val);
                    else if (is("fChanceKOBodyLight"))    v.chanceKOBodyLight     = std::stof(val);
                    else if (is("fChanceKOBodyClothes"))  v.chanceKOBodyClothes   = std::stof(val);
                    else if (is("fChanceKOPointsPerLevel")) v.chanceKOPointsPerLevel = std::stof(val);
                    else if (is("fChanceKOLevelMax"))
                        v.chanceKOLevelMax = AtLeast("fChanceKOLevelMax", std::stof(val), 0.0f);
                    else if (is("fChanceKODamageMax"))    v.chanceKODamageMax     = std::stof(val);
                    // u6c [Sleep]
                    else if (is("bSleep"))                v.sleep                 = ParseBool(val);
                    else if (is("bSleepFollowers"))       v.sleepFollowers        = ParseBool(val);
                    else if (is("bLogSleep"))             v.logSleep              = ParseBool(val);
                    // u6f [Sleep]
                    else if (is("bSleepWakeOnHit"))       v.sleepWakeOnHit        = ParseBool(val);
                    else if (is("bSleepWakeOnTalk"))      v.sleepWakeOnTalk       = ParseBool(val);
                    // u7a [FDC] (U4)
                    else if (is("bFdcRerollOnHit"))       v.fdcRerollOnHit        = ParseBool(val);
                    else if (is("bFdcRerollPlayerHits"))  v.fdcRerollPlayerHits   = ParseBool(val);
                    else if (is("bFdcRerollDeathOnly"))   v.fdcRerollDeathOnly    = ParseBool(val);
                    else if (is("bLogFdcReroll"))         v.logFdcReroll          = ParseBool(val);
                    // u7a [TakeOver] (U7)
                    else if (is("bTakeOver"))             v.takeOver              = ParseBool(val);
                    else if (is("bLogTakeOver"))          v.logTakeOver           = ParseBool(val);
                    // u7a [Woken] (M2)
                    else if (is("bWokenNarration"))       v.wokenNarration        = ParseBool(val);
                    else if (is("bWokenOnTalk"))          v.wokenOnTalk           = ParseBool(val);
                    else if (is("bWokenOnDomWake"))       v.wokenOnDomWake        = ParseBool(val);
                    else if (is("iWokenTalkMode")) {
                        const int m = std::stoi(val);
                        if (m < 0 || m > 1) {
                            logger::warn("[ini] [{}] iWokenTalkMode = {} is not 0 or 1 - kept the default {}", section, val, v.wokenTalkMode);
                            continue;
                        }
                        v.wokenTalkMode = m;
                    }
                    else if (is("bLogWoken"))             v.logWoken              = ParseBool(val);
                    else if (is("bLogWokenProbe"))        v.logWokenProbe         = ParseBool(val);
                    // m34 [Marks] (build 3)
                    else if (is("bMarks"))                v.marks                 = ParseBool(val);
                    else if (is("bMarkPlayer"))           v.markPlayer            = ParseBool(val);
                    else if (is("bBeastFaceMarks"))       v.beastFaceMarks        = ParseBool(val);
                    else if (is("bWristAnkleBands"))      v.wristAnkleBands       = ParseBool(val);
                    else if (is("bFaceLayers"))           v.faceLayers            = ParseBool(val);
                    else if (is("bStandIns"))             v.standIns              = ParseBool(val);
                    else if (is("iFaceFirst"))            v.faceFirst             = AtLeast("iFaceFirst", std::stoi(val), 0);
                    else if (is("iFaceLast"))             v.faceLast              = AtLeast("iFaceLast", std::stoi(val), 0);
                    else if (is("iBodyFirst"))            v.bodyFirst             = AtLeast("iBodyFirst", std::stoi(val), 0);
                    else if (is("iBodyLast"))             v.bodyLast              = AtLeast("iBodyLast", std::stoi(val), 0);
                    else if (is("iHandsSlot"))            v.handsSlot             = AtLeast("iHandsSlot", std::stoi(val), 0);
                    else if (is("iFeetSlot"))             v.feetSlot              = AtLeast("iFeetSlot", std::stoi(val), 0);
                    else if (is("iChokeSlot"))            v.chokeSlot             = AtLeast("iChokeSlot", std::stoi(val), 0);
                    else if (is("sStageAlphas")) {
                        const auto parts = SplitList(val);
                        if (parts.size() != 3) {
                            logger::warn("[ini] [{}] sStageAlphas = {} is not three numbers (light, medium, severe) - kept the default", section, val);
                            continue;
                        }
                        float a[3] = {};
                        for (int i = 0; i < 3; ++i) a[i] = AtLeast("sStageAlphas", std::stof(parts[i]), 0.0f);
                        for (int i = 0; i < 3; ++i) v.stageAlphas[i] = a[i];
                    }
                    else if (is("fAlphaFloor"))           v.alphaFloor            = AtLeast("fAlphaFloor", std::stof(val), 0.0f);
                    else if (is("fAlphaScale"))           v.alphaScale            = AtLeast("fAlphaScale", std::stof(val), 0.0f);
                    else if (is("bChokeMark"))            v.chokeMark             = ParseBool(val);
                    else if (is("sChokeSteps")) {
                        const auto parts = SplitList(val);
                        if (parts.size() != 2) {
                            logger::warn("[ini] [{}] sChokeSteps = {} is not two numbers of seconds - kept the default", section, val);
                            continue;
                        }
                        const float s0 = AtLeast("sChokeSteps", std::stof(parts[0]), 0.0f);
                        const float s1 = AtLeast("sChokeSteps", std::stof(parts[1]), s0);
                        v.chokeSteps[0] = s0;
                        v.chokeSteps[1] = s1;
                    }
                    else if (is("fChokeHoursPerStep"))    v.chokeHoursPerStep     = AtLeast("fChokeHoursPerStep", std::stof(val), 0.0f);
                    else if (is("fChokeHealShare"))       v.chokeHealShare        = AtLeast("fChokeHealShare", std::stof(val), 0.0f);
                    else if (is("bChokeMarkBeforePassout")) v.chokeMarkBeforePassout = ParseBool(val);
                    else if (is("fChokePassoutSeconds"))  v.chokePassoutSeconds   = AtLeast("fChokePassoutSeconds", std::stof(val), 0.1f);
                    else if (is("sMarksFile"))            v.marksFile             = val;
                    else if (is("bLogMarks"))             v.logMarks              = ParseBool(val);
                    else if (is("bPerfMarks"))            v.perfMarks             = ParseBool(val);
                    else {
                        logger::warn("[ini] [{}] unknown key '{}' ignored", section, key);
                        continue;
                    }
                    ++n;
                    readKeys.insert(Lower(key));
                    logger::info("[ini] [{}] {} = {} (read)", section, key, val);
                } catch (...) {
                    logger::warn("[ini] [{}] bad value for '{}': '{}' - kept the default", section, key, val);
                }
            }
            logger::info("[ini] {} value(s) read; every key not read keeps its default", n);
        }
        if (v.recoverMaxHours < v.recoverMinHours)
            logger::warn("[ini] fRecoverMaxHours {} is below fRecoverMinHours {} - the timer will always be {} game hours "
                         "(v10's rule: max <= min -> min)",
                         v.recoverMaxHours, v.recoverMinHours, v.recoverMinHours);

        // m34c: the [Marks] slot bands must make sense TOGETHER - each in order (first <= last), every slot
        // 0..127 (the co-save keeps a slot in one signed byte), and the choke slot outside the IE face band (both are Face slots: an
        // overlap lets the face marks take the choke's slot). Any violation = a warning and ALL the slot keys back to their defaults.
        {
            const Values d;
            const bool   inRange = v.faceFirst <= 127 && v.faceLast <= 127 && v.bodyFirst <= 127 && v.bodyLast <= 127 && v.handsSlot <= 127 &&
                                 v.feetSlot <= 127 && v.chokeSlot <= 127;
            const bool   ordered = v.faceFirst <= v.faceLast && v.bodyFirst <= v.bodyLast;
            const bool   apart   = v.chokeSlot < v.faceFirst || v.chokeSlot > v.faceLast;
            if (!inRange || !ordered || !apart) {
                logger::warn("[ini] [Marks] slot bands do not fit together (Face {}-{}, Body {}-{}, Hands {}, Feet {}, choke Face {}: {}) - every slot key "
                             "goes back to its default (Face {}-{}, Body {}-{}, Hands {}, Feet {}, choke Face {})",
                             v.faceFirst, v.faceLast, v.bodyFirst, v.bodyLast, v.handsSlot, v.feetSlot, v.chokeSlot,
                             !inRange ? "a slot above 127" : !ordered ? "a band's first slot is after its last" : "the choke slot is inside the face band",
                             d.faceFirst, d.faceLast, d.bodyFirst, d.bodyLast, d.handsSlot, d.feetSlot, d.chokeSlot);
                v.faceFirst = d.faceFirst;
                v.faceLast  = d.faceLast;
                v.bodyFirst = d.bodyFirst;
                v.bodyLast  = d.bodyLast;
                v.handsSlot = d.handsSlot;
                v.feetSlot  = d.feetSlot;
                v.chokeSlot = d.chokeSlot;
            }
        }

        g_values         = v;
        const auto from  = [&](const char* k) { return readKeys.contains(Lower(k)) ? "from the ini" : "default"; };
        logger::info("[ini] in effect: [Knockout] fWakeHealthFraction = {} ({})", v.wakeHealthFraction, from("fWakeHealthFraction"));
        logger::info("[ini] in effect: [Knockout] iGraceChecks = {} ({})", v.graceChecks, from("iGraceChecks"));
        logger::info("[ini] in effect: [Knockout] fRecoverMinHours = {} ({})", v.recoverMinHours, from("fRecoverMinHours"));
        logger::info("[ini] in effect: [Knockout] fRecoverMaxHours = {} ({})", v.recoverMaxHours, from("fRecoverMaxHours"));
        logger::info("[ini] in effect: [Knockout] fCheckSeconds = {} ({})", v.checkSeconds, from("fCheckSeconds"));
        logger::info("[ini] in effect: [Knockout] fStageSeconds = {} ({})", v.stageSeconds, from("fStageSeconds"));
        logger::info("[ini] in effect: [Knockout] bNotify = {} ({})", v.notify ? 1 : 0, from("bNotify"));
        logger::info("[ini] in effect: [Knockout] fBleedoutGraceSeconds = {} ({})", v.bleedoutGraceSeconds, from("fBleedoutGraceSeconds"));
        logger::info("[ini] in effect: [Regen] fEssentialNonCombatHealRateBonus = {} ({}){}", v.essentialRegenBonus,
                     from("fEssentialNonCombatHealRateBonus"), v.essentialRegenBonus < 0.0f ? " - the game setting is not touched" : "");
        logger::info("[ini] in effect: [Regen] fEssentialHealthPercentReGain = {} ({}){}", v.essentialLethalHitHealth,
                     from("fEssentialHealthPercentReGain"), v.essentialLethalHitHealth < 0.0f ? " - the game setting is not touched" : "");
        logger::info("[ini] in effect: [Regen] iCombatEndRefill = {} ({}) - the engine's full heal at combat end is skipped for {}",
                     v.combatEndRefill, from("iCombatEndRefill"),
                     v.combatEndRefill >= 2 ? "every essential / protected NPC"
                     : v.combatEndRefill == 1 ? "followers (player teammates)"
                                              : "nobody but the NPCs SBKO itself knocks out / wakes");
        logger::info("[ini] in effect: [Unconscious] fUnconsciousHealthFraction = {} ({})", v.unconsciousHealthFraction,
                     from("fUnconsciousHealthFraction"));
        logger::info("[ini] in effect: [Unconscious] bSeverActionsGuard = {} ({})", v.severActionsGuard ? 1 : 0, from("bSeverActionsGuard"));
        logger::info("[ini] in effect: [Unconscious] bPotionsOff = {} ({})", v.potionsOff ? 1 : 0, from("bPotionsOff"));
        logger::info("[ini] in effect: [Unconscious] bRepairWalkingGraph = {} ({})", v.repairWalkingGraph ? 1 : 0,
                     from("bRepairWalkingGraph"));
        logger::info("[ini] in effect: [HealWake] bHealWake = {} ({})", v.healWake ? 1 : 0, from("bHealWake"));
        // U6
        logger::info("[ini] in effect: [HeadHit] bHeadHit = {} ({}){}", v.headHit ? 1 : 0, from("bHeadHit"),
                     v.headHit ? "" : " - the player's hits never knock out; the hit sink is not registered");
        logger::info("[ini] in effect: [HeadHit] bHeadHitBash = {} ({})", v.headHitBash ? 1 : 0, from("bHeadHitBash"));
        logger::info("[ini] in effect: [HeadHit] bHeadHitBlunt = {} ({})", v.headHitBlunt ? 1 : 0, from("bHeadHitBlunt"));
        logger::info("[ini] in effect: [HeadHit] bHeadHitSneak = {} ({})", v.headHitSneak ? 1 : 0, from("bHeadHitSneak"));
        logger::info("[ini] in effect: [HeadHit] bHeadHitLowHealth = {} ({})", v.headHitLowHealth ? 1 : 0, from("bHeadHitLowHealth"));
        logger::info("[ini] in effect: [HeadHit] fHeadHitHealthThreshold = {} ({})", v.headHitHealthThreshold,
                     from("fHeadHitHealthThreshold"));
        logger::info("[ini] in effect: [HeadHit] bHeadHitHelmetProtects = {} ({})", v.headHitHelmetProtects ? 1 : 0,
                     from("bHeadHitHelmetProtects"));
        logger::info("[ini] in effect: [HeadHit] sHelmetIgnorePlugins = {} ({}) - {}",   // u6g
                     v.helmetIgnorePlugins.empty() ? std::string("(empty)") : JoinList(v.helmetIgnorePlugins), from("sHelmetIgnorePlugins"),
                     v.helmetIgnorePlugins.empty() ? std::string("no plugin is ignored")
                                                   : std::format("a piece defined by one of these {} plugin(s) is never a helmet (U6 + U5)",
                                                                 v.helmetIgnorePlugins.size()));
        logger::info("[ini] in effect: [HeadHit] bHeadHitFollowers = {} ({})", v.headHitFollowers ? 1 : 0, from("bHeadHitFollowers"));
        logger::info("[ini] in effect: [HeadHit] bLogHeadHits = {} ({})", v.logHeadHits ? 1 : 0, from("bLogHeadHits"));
        // u6b (U5)
        logger::info("[ini] in effect: [ChanceKO] bChanceKO = {} ({}){}", v.chanceKO ? 1 : 0, from("bChanceKO"),
                     v.chanceKO ? "" : " - no damage hook; every NPC that bleeds out is knocked out (as before u6b)");
        logger::info("[ini] in effect: [ChanceKO] fChanceKOHealthBelow = {} ({})", v.chanceKOHealthBelow, from("fChanceKOHealthBelow"));
        logger::info("[ini] in effect: [ChanceKO] bChanceKOCreatureHits = {} ({})", v.chanceKOCreatureHits ? 1 : 0,
                     from("bChanceKOCreatureHits"));
        logger::info("[ini] in effect: [ChanceKO] bLogChanceKO = {} ({})", v.logChanceKO ? 1 : 0, from("bLogChanceKO"));
        logger::info("[ini] in effect: [ChanceKO] formula: helmet on {} ({}) / off {} ({}) | boots heavy {} ({}) / light {} ({}) / none {} "
                     "({}) | gloves heavy {} ({}) / light {} ({}) / none {} ({}) | body heavy {} ({}) / light {} ({}) / clothes {} ({}) | "
                     "{} per level ({}) within +-{} ({}) | damage up to {} ({})",
                     v.chanceKOHelmetOn, from("fChanceKOHelmetOn"), v.chanceKOHelmetOff, from("fChanceKOHelmetOff"),
                     v.chanceKOFeetHeavy, from("fChanceKOFeetHeavy"), v.chanceKOFeetLight, from("fChanceKOFeetLight"),
                     v.chanceKOFeetNone, from("fChanceKOFeetNone"), v.chanceKOHandsHeavy, from("fChanceKOHandsHeavy"),
                     v.chanceKOHandsLight, from("fChanceKOHandsLight"), v.chanceKOHandsNone, from("fChanceKOHandsNone"),
                     v.chanceKOBodyHeavy, from("fChanceKOBodyHeavy"), v.chanceKOBodyLight, from("fChanceKOBodyLight"),
                     v.chanceKOBodyClothes, from("fChanceKOBodyClothes"), v.chanceKOPointsPerLevel, from("fChanceKOPointsPerLevel"),
                     v.chanceKOLevelMax, from("fChanceKOLevelMax"), v.chanceKODamageMax, from("fChanceKODamageMax"));
        // u6c
        logger::info("[ini] in effect: [Sleep] bSleep = {} ({}){}", v.sleep ? 1 : 0, from("bSleep"),
                     v.sleep ? "" : " - the Sleep spell does nothing");
        logger::info("[ini] in effect: [Sleep] bSleepFollowers = {} ({})", v.sleepFollowers ? 1 : 0, from("bSleepFollowers"));
        logger::info("[ini] in effect: [Sleep] bLogSleep = {} ({})", v.logSleep ? 1 : 0, from("bLogSleep"));
        // u6f
        logger::info("[ini] in effect: [Sleep] bSleepWakeOnHit = {} ({}){}", v.sleepWakeOnHit ? 1 : 0, from("bSleepWakeOnHit"),
                     v.sleepWakeOnHit ? " - any hit wakes a Sleep-spell sleeper" : " - a hit never wakes a sleeper");
        logger::info("[ini] in effect: [Sleep] bSleepWakeOnTalk = {} ({}){}", v.sleepWakeOnTalk ? 1 : 0, from("bSleepWakeOnTalk"),
                     v.sleepWakeOnTalk ? " - activating (talking to) a sleeper wakes her"
                                       : " - talking to a sleeper never wakes her (another mod's 'Wake' still does)");
        // u7a (U4 / U7 / M2)
        logger::info("[ini] in effect: [FDC] bFdcRerollOnHit = {} ({}){}", v.fdcRerollOnHit ? 1 : 0, from("bFdcRerollOnHit"),
                     v.fdcRerollOnHit ? " - a qualifying hit on a follower SBKO holds after a bleedout knock-out = one more FDC roll"
                                      : " - no FDC re-roll on a hit");
        logger::info("[ini] in effect: [FDC] bFdcRerollPlayerHits = {} ({}){}", v.fdcRerollPlayerHits ? 1 : 0, from("bFdcRerollPlayerHits"),
                     v.fdcRerollPlayerHits ? " - your own hits re-roll too" : " - only OTHER people's hits re-roll (yours never)");
        logger::info("[ini] in effect: [FDC] bFdcRerollDeathOnly = {} ({}){}", v.fdcRerollDeathOnly ? 1 : 0, from("bFdcRerollDeathOnly"),
                     v.fdcRerollDeathOnly ? " - a re-roll that misses does nothing" : " - a re-roll that misses = FDC's full roll (small injury)");
        logger::info("[ini] in effect: [FDC] bLogFdcReroll = {} ({})", v.logFdcReroll ? 1 : 0, from("bLogFdcReroll"));
        logger::info("[ini] in effect: [TakeOver] bTakeOver = {} ({}){}", v.takeOver ? 1 : 0, from("bTakeOver"),
                     v.takeOver ? " - SBKO_API.TakeOver accepts a choked-out NPC from VRTouchEvents" : " - TakeOver always answers False");
        logger::info("[ini] in effect: [TakeOver] bLogTakeOver = {} ({})", v.logTakeOver ? 1 : 0, from("bLogTakeOver"));
        logger::info("[ini] in effect: [Woken] bWokenNarration = {} ({})", v.wokenNarration ? 1 : 0, from("bWokenNarration"));
        logger::info("[ini] in effect: [Woken] bWokenOnTalk = {} ({})", v.wokenOnTalk ? 1 : 0, from("bWokenOnTalk"));
        logger::info("[ini] in effect: [Woken] bWokenOnDomWake = {} ({})", v.wokenOnDomWake ? 1 : 0, from("bWokenOnDomWake"));
        logger::info("[ini] in effect: [Woken] iWokenTalkMode = {} ({}) - {}", v.wokenTalkMode, from("iWokenTalkMode"),
                     v.wokenTalkMode == 1 ? "she answers you (DirectNarration)" : "she only knows it (RegisterPersistentEvent)");
        logger::info("[ini] in effect: [Woken] bLogWoken = {} ({})", v.logWoken ? 1 : 0, from("bLogWoken"));
        logger::info("[ini] in effect: [Woken] bLogWokenProbe = {} ({})", v.logWokenProbe ? 1 : 0, from("bLogWokenProbe"));
        // m34 (build 3)
        logger::info("[ini] in effect: [Marks] bMarks = {} ({}){} | bMarkPlayer = {} ({}) | bBeastFaceMarks = {} ({}) | bWristAnkleBands = {} ({}) | "
                     "bFaceLayers = {} ({}) | bStandIns = {} ({})",
                     v.marks ? 1 : 0, from("bMarks"), v.marks ? "" : " - no skin mark is read, painted or kept", v.markPlayer ? 1 : 0,
                     from("bMarkPlayer"), v.beastFaceMarks ? 1 : 0, from("bBeastFaceMarks"), v.wristAnkleBands ? 1 : 0, from("bWristAnkleBands"),
                     v.faceLayers ? 1 : 0, from("bFaceLayers"), v.standIns ? 1 : 0, from("bStandIns"));
        logger::info("[ini] in effect: [Marks] slots Face {}-{} ({}/{}) | Body {}-{} ({}/{}) | Hands {} ({}) | Feet {} ({}) | choke Face {} ({})",
                     v.faceFirst, v.faceLast, from("iFaceFirst"), from("iFaceLast"), v.bodyFirst, v.bodyLast, from("iBodyFirst"), from("iBodyLast"),
                     v.handsSlot, from("iHandsSlot"), v.feetSlot, from("iFeetSlot"), v.chokeSlot, from("iChokeSlot"));
        logger::info("[ini] in effect: [Marks] sStageAlphas = {:.2f},{:.2f},{:.2f} ({}) | fAlphaFloor = {:.2f} ({}) | fAlphaScale = {:.2f} ({})",
                     v.stageAlphas[0], v.stageAlphas[1], v.stageAlphas[2], from("sStageAlphas"), v.alphaFloor, from("fAlphaFloor"), v.alphaScale,
                     from("fAlphaScale"));
        logger::info("[ini] in effect: [Marks] bChokeMark = {} ({}) | sChokeSteps = {:.0f},{:.0f} ({}) | fChokeHoursPerStep = {} ({}) | "
                     "fChokeHealShare = {} ({}) | bChokeMarkBeforePassout = {} ({}){} | fChokePassoutSeconds = {} ({})",
                     v.chokeMark ? 1 : 0, from("bChokeMark"), v.chokeSteps[0], v.chokeSteps[1], from("sChokeSteps"), v.chokeHoursPerStep,
                     from("fChokeHoursPerStep"), v.chokeHealShare, from("fChokeHealShare"), v.chokeMarkBeforePassout ? 1 : 0,
                     from("bChokeMarkBeforePassout"),
                     v.chokeMarkBeforePassout ? " - a choke let go before the passout leaves a light mark (0.30 floor, a shorter life)"
                                              : " - only a choke that knocked her out leaves a mark",
                     v.chokePassoutSeconds, from("fChokePassoutSeconds"));
        logger::info("[ini] in effect: [Marks] sMarksFile = {} ({}) | bLogMarks = {} ({}) | bPerfMarks = {} ({})", v.marksFile, from("sMarksFile"),
                     v.logMarks ? 1 : 0, from("bLogMarks"), v.perfMarks ? 1 : 0, from("bPerfMarks"));
        logger::info("[ini] in effect: [Log] bVerboseChecks = {} ({})", v.verboseChecks ? 1 : 0, from("bVerboseChecks"));
        logger::info("[ini] in effect: [Log] bLogEffects = {} ({})", v.logEffects ? 1 : 0, from("bLogEffects"));
        logger::info("[ini] in effect: [Log] bLogFootsteps = {} ({})", v.logFootsteps ? 1 : 0, from("bLogFootsteps"));
    }
}
