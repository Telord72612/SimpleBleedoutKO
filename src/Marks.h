#pragma once

// m34 (build 3): THE SKIN MARKS - M3 (the painter) + M4 (the Injury Engine mirror) + the CHOKE hand mark.
// The rule: everyone - Injury Engine puts an injury on, SBKO puts a mark on it; Injury Engine removes an injury, SBKO removes it. Plus a
// hand mark around the neck by how long the choke was held, healed with time or a healing spell, like the skin mark of shackles in
// DDAB.
//
// C++ decides everything (which mark, where, how strong, when); SBKO_Marks.psc only runs RaceMenu's NiOverride natives for it (RaceMenu VR
// has no C++ overlay interface). The house rules (Knockout.cpp's thread model):
//   * sinks (hit / effect / IE's mod-event ping / cell / death) only copy + flag or TaskRelay::Add;
//   * every decision runs in an SKSE task (the check, a relayed task, a reply task) under this file's OWN mutex, held only for the map work -
//     never around a Papyrus dispatch; Knockout's g_mtx may be held when it calls in here (order g_mtx -> marks), never the reverse;
//   * Papyrus replies run on a VM thread: they only TaskRelay::Add;
//   * NO QueueNiNodeUpdate (RaceMenu face overlays need none): AddOverlays when she has none + Wait(0.5), then write.
// Cost controls only, no gameplay cap: 3D-loaded actors, reads only when something happened (IE's ping, a hit / heal on her, her Health moving
// a whole percent, a game hour for torture marks, her 3D loading, a load), HasOpenWounds before a full read in a sweep, the live state
// dropped on unload, one read + one write in flight per actor, a [PERF] marks line once a minute.
namespace SBKO::Marks
{
    void Install();                          // kDataLoaded, after Settings::Load and BEFORE Knockout registers its sinks
    [[nodiscard]] bool On() noexcept;        // bMarks 1 and the table loaded (the sinks feed this file only then)

    void OnPreLoadGame();                    // a load begins: in-flight work belongs to the old game
    void OnGameReady(bool a_postLoad);       // kPostLoadGame: every marked actor is reconciled 5 s later (when she has 3D)

    // The 1.5 s check's tail (Knockout::Tick, an SKSE task, no lock of Knockout's held, the game not paused). a_loaded = the high-process
    // actors Knockout already read (the player is not in it).
    void Tick(const std::vector<RE::NiPointer<RE::Actor>>& a_loaded);

    // ---- sinks (any thread): copy + flag only ----
    void NoteHit(RE::FormID a_target);                                         // TESHitEvent on an actor (attribution of IE's ping)
    void NoteEffect(RE::FormID a_target, RE::FormID a_caster, RE::FormID a_mgef);   // TESMagicEffectApplyEvent (marked actors only)
    [[nodiscard]] bool AnyRecords() noexcept;                                  // the death sink's early-out

    // ---- tasks (Knockout.cpp; no lock of Knockout's needs to be held) ----
    void OnAttach(RE::FormID a_id);          // her cell attached: reconcile when she has 3D
    void OnDetach(RE::FormID a_id);          // her cell detached: the live state is dropped, the record kept
    void OnDeath(RE::FormID a_id);           // a real death: the marks stay on the body, the record is dropped
    // The choke ended or was handed over: a_seconds held in all. a_passout = she passed out (a take-over, or a release of a choke knock-out);
    // a loose release with a_seconds >= fChokePassoutSeconds counts as a passout too.
    void OnChoke(RE::FormID a_id, float a_seconds, bool a_passout, const char* a_why);

    // ---- co-save: record 'MARK' (m34c: v2, v1 still read) under SBKO's unique ID (Knockout::OnSave / OnLoad / OnRevert call these) ----
    void Save(SKSE::SerializationInterface* a_intf);
    void Load(SKSE::SerializationInterface* a_intf, std::uint32_t a_version,
              const std::function<RE::FormID(RE::FormID, std::string&)>& a_resolve);
    void Revert();
    void CollectIds(std::vector<RE::FormID>& a_out);   // every actor id in the record (for Knockout's 'LREF' light-plugin names)
    inline constexpr std::uint32_t kRecordType = 'MARK';

    // ---- console: 'sbko marks <word> [options]' on the selected actor (or the player). One HUD line back; the detail in the log ----
    std::string Console(RE::Actor* a_actor, const std::vector<std::string>& a_words);
    [[nodiscard]] const char* ConsoleHelpLine() noexcept;
}
