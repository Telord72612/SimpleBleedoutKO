# SimpleBleedoutKO - changelog

## 2.1.0

**An easy settings file**
- New `SKSE/Plugins/SBKO_Setting.ini`: the everyday options under plain names, each explained (who gets knocked out, healing, waking,
  your hits, the Sleep spell, extras). It is read after `SimpleBleedoutKO.ini` and its values win; the moved options are commented out in
  `SimpleBleedoutKO.ini`, which keeps the advanced ones. The log names the file each value came from and warns when the two disagree.

**Choose who gets knocked out**
- `bKnockOutFollowers`, `bKnockOutProtected`, `bKnockOutNormalNPCs`: a group switched off gets the game's own bleedout again (someone
  already knocked out is never affected; your own bash / mace / Sleep knock-outs keep their own switches).
- `fNormalNPCKnockOutHealthPercent`: how low an ordinary NPC's Health must be for the knock-out roll (default 15).

**Loot a knocked-out NPC while sneaking**
- Crouch and activate a knocked-out NPC: her inventory opens - free looting, never a crime. Standing up, the click works as before.
  The inventory closes if she wakes, dies or is let go, and never opens over another menu. With MageVR Reborn's VR Backpack pickpocket
  on, its pickpocket panel shows instead (items marked stolen). Switch: `bLootKnockedOut`.

**Followers stay hurt after a fight on SE / AE too**
- The skip of the game's full heal of essential NPCs at the end of a fight, and of the full heal when this mod wakes someone, now works on
  Skyrim SE 1.5.97 and AE 1.6.x / 1.7.x as well as VR. Each patch finds its exact place in the game at load and installs only there; on a
  game build it does not recognise it stays off and says so in the log.

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
