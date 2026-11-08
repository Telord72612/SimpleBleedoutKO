# SimpleBleedoutKO - changelog

## 2.0.0 - first public release

Compared with the earlier private, Papyrus-only version (v10):

**Rebuilt as an SKSE plugin**
- One DLL for Skyrim SE, AE and VR. The old SPID line and the ability on every NPC are gone (an empty stub of the old script stays, so old
  saves load cleanly). All settings live in `SKSE/Plugins/SimpleBleedoutKO.ini`, each one explained.

**A real unconscious state**
- A downed protected / essential NPC is truly unconscious: ragdolled, Health held, no natural regeneration, no potion drinking.
- Another mod's "wake" is refused with "<name> is too injured to wake up." - Diary of Mine's Bag, Abduct and Tie-up still take her.

**Waking**
- Only a real heal (any healing spell, staff, scroll or potion, from anyone) or a 6-12 game-hour timer wakes her; she then comes round in
  three stages. A timer wake is narrated through SkyrimNet.
- She wakes with at least 25 % of her Health.

**Safer saves and loads**
- Knocked-out NPCs left in unloaded cells are released when a game loads; lost or stuck knock-outs are found and fixed.

**Followers stay hurt**
- The game's out-of-combat regeneration for essential / protected NPCs is set low (game-wide; the ini value), the killing-blow regain is
  lowered, and in Skyrim VR the engine's full heal at the end of every fight is skipped.

**Ordinary NPCs**
- Instead of every bleedout, a knock-out ROLL when a hit leaves them at 15 % Health or less - from their armour, the level gap and the
  size of the hit.

**Your own knock-outs**
- Your bash, or your mace / warhammer (to the head in VR with PLANCK, anywhere without it), knocks an NPC out for certain on a sneak attack,
  when they do not see you, or at 10 % Health or less. A real helmet protects; hoods, circlets and wigs do not (wig mods listed in the ini).

**The Sleep spell**
- Illusion, Apprentice; sold by Drevis Neloren (with SkyPatcher). A sleeper wakes on any hit, when you talk to her, with Diary of Mine's
  Wake, a heal or the timer.

**Skin marks**
- With Injury Engine, RaceMenu and Battle Wounds: the matching mark on every wounded part, on when Injury Engine lists the wound, off when it
  heals. Wrist / ankle bands for grip and fall wounds.

**SkyrimNet**
- Prompt pack 1.2.1: unconscious, coming-to, onlooker, Sleep-spell and choke lines; "woken to talk" narration when you wake a sleeper in a
  bed.

**For other mods**
- `SBKO_API`: VRTouchEvents can hand a choked-out NPC to this mod (waits for a VRTouchEvents update).

**Performance**
- The skin-mark check asks Injury Engine about everyone in one script call instead of one per NPC.
