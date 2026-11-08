# SimpleBleedoutKO 2.0.0

**Knock-outs instead of endless bleedout.** When a follower, a quest NPC or any other protected / essential character drops, they are
**knocked out cold** - ragdolled, unconscious, not regenerating - and they stay down until **you heal them** or a few game hours pass.
Ordinary NPCs can be knocked out too: by a lucky hit when they are badly hurt, by your shield bash or mace, or by a new **Sleep** spell.
With SkyrimNet, everyone knows what happened: the unconscious NPC only breathes and groans, onlookers see them lying there, and they come
round dazed and confused.

One SKSE plugin for **Skyrim SE, AE and VR**. **Tested in Skyrim VR 1.4.15. Built for SE / AE but not tested there yet** - please report
how it goes and attach `SimpleBleedoutKO.log`.

---

## What it does

### Protected and essential NPCs are knocked out, not left kneeling
- The moment a protected or essential NPC (your followers, quest characters) drops into bleedout, they are **knocked out**: they fall
  ragdolled, unconscious, and their Health is **held low** - no natural regeneration, no drinking potions, no getting up on their own.
- Another mod that tries to "wake" them gets *"<name> is too injured to wake up."* - they need healing.
- You (the player) are never knocked out.

### They wake when you heal them - or after a few hours
- **Any real heal wakes them** straight away: a healing spell, a staff, a scroll, a potion they receive - from you or anyone else. They get
  up with at least 25 % of their Health.
- If nobody heals them, they wake by themselves after **6 to 12 game hours**.
- Waking is not instant clarity: they are **groggy for about three minutes** (three stages), and with SkyrimNet they behave like it.

### Ordinary NPCs: a knock-out roll when they are badly hurt
Bandits, soldiers, mages, townsfolk... a hit that leaves them alive at **15 % Health or less** rolls for a knock-out. Who landed the hit
does not matter (you, a follower, another NPC, a wolf). The chance is the sum of:

| What they wear / the hit | Points |
|---|---|
| No helmet | 20 (helmet: 0) |
| Boots: none / light / heavy | 10 / 5 / 0 |
| Gloves: none / light / heavy | 10 / 5 / 0 |
| Body: clothes or nothing / light / heavy | 20 / 10 / 0 |
| Level: each level the attacker is above them (below = minus) | +/-1, at most +/-20 |
| Hit size: a hit as big as their whole Health | up to 10 |

So a robed mage hit hard by a stronger foe can go down often (up to 90 %), a fighter in full plate almost never. Lose the roll and the game
decides as usual (they may kneel, flee or fight on). Punches, burning / poison ticks, cloaks, traps and falls never roll. Every number is in
the ini.

### Your bash or your mace can knock someone out
- A **shield or weapon bash** (anywhere), or a **mace / warhammer** hit - **to the head** in Skyrim VR with PLANCK, anywhere without it -
  knocks an NPC out **for certain** when it is a **sneak attack**, when they **do not see you**, or when they are at **10 % Health or less**.
- A real **helmet** (light or heavy armour on the head) protects them. Hoods, circlets, capes and wigs do not count (wig mods are listed in
  the ini - add your own).
- Bare-handed punches never knock anyone out. Brawls and the Companions' fist-fight are never affected. Your followers can be knocked out
  by you too (switch in the ini).

### The Sleep spell
- **Sleep** (Illusion, Apprentice): touch a person and cast it - they fall into a deep magical sleep. No roll, no crime.
- **Drevis Neloren** at the College of Winterhold sells the tome and two scrolls (needs SkyPatcher; otherwise use the console:
  `help sleep`, then `player.additem <id> 1`).
- A sleeper wakes on **any hit**, when you **talk to them**, with Diary of Mine's **Wake**, by a heal, or after 6-12 game hours.
- With SkyrimNet: if they **saw** your glowing hand, they half-remember it and wonder whether it was a dream; if not, they just wake confused.

### Skin marks from Injury Engine
With **Injury Engine**, **RaceMenu** and **Battle Wounds** installed, everyone Injury Engine wounds (you included) gets the matching mark
on the wounded part - a bruise, a cut, a black eye, blood at the lip, a grip mark around a wrist or ankle - at 50 / 75 / 100 % strength by
wound stage. When Injury Engine heals the wound, the mark goes. Burns, frost and shock have no art yet. Which mark goes with which wound is
in `SKSE/Plugins/SimpleBleedoutKO/marks.json` (editable).

### SkyrimNet awareness
A SkyrimNet prompt pack (`telord.simplebleedoutko`) tells the AI:
- the knocked-out NPC is **unconscious** (only breathing, groaning, muffled sounds - no speech while gagged);
- **onlookers** see them lying unconscious;
- on waking they come round **dazed and confused**, clearing over a few minutes (different memories after a wound, a Sleep spell seen or
  unseen, or a choke);
- when **you wake someone sleeping in a bed** (or with Diary of Mine's Wake), they react to being woken;
- a timer wake is narrated: *"<Name> regains consciousness after being passed out for N hours, confused and unwell."*

### Plays well with
- **Diary of Mine** - Bag, Abduct and Tie-up take a knocked-out NPC from this mod; its "Wake" on an injured NPC says they are too injured;
  its "Wake" on a sleeper works. Diary of Mine's slaves are never knocked out.
- **SeverActions** - a knocked-out follower is taken out of its hunger / fatigue / cold tracking and put back, needs restored, on waking.
- **Smart NPC Potions** - no potion drinking while unconscious.
- **VRTouchEvents** - never knocks out someone VRTouchEvents is choking. A choke hand-off (a choked-out NPC kept by this mod's rules) is
  built in and waits for a VRTouchEvents update.
- **PLANCK** (VR) - the head-only mace rule.
- **Devious Devices** - its items never count as armour or a helmet.

---

## Requirements
**Required**
- **SKSE64** (SE / AE) or **SKSEVR** (VR)
- **Address Library for SKSE Plugins** (SE / AE) or **VR Address Library for SKSEVR** (VR)

**Recommended** (each one is optional; without it its feature switches itself off, with one line in the log)
- **SkyrimNet** (Beta 25 or later) - the AI awareness
- **RaceMenu** (SE / AE) or **RaceMenu VR** - the skin marks. Face marks need RaceMenu's **face overlay count raised to 13 or more**
  (see Installation); in VR, face overlays need RaceMenu VR 0.5 or later.
- **Battle Wounds** by AltNameByeee (LoversLab) - the skin-mark art; install it as **loose files** (not packed in a BSA)
- **Injury Engine** by GoodProvider - the wounds the marks follow.
  *VR note:* in Skyrim VR use Injury Engine **0.2.0 or later** - older versions (0.1.6 and before) do not record weapon hits in VR, so
  you would see few marks. Tested in VR with Injury Engine 0.2.0.
- **SkyPatcher** - Drevis sells the Sleep spell
- **PLANCK** (VR) - head-only mace knock-outs

**Optional:** Diary of Mine, SeverActions, Smart NPC Potions, VRTouchEvents, Devious Devices.

## Installation
1. Install with your mod manager (MO2 / Vortex) - a plain zip, no installer.
2. **For face marks:** open RaceMenu's `SKSE/Plugins/skee64.ini` (SE / AE) or `skeevr.ini` (VR) and set `[Overlays/Face]
   iNumOverlays=13` or more (the stock value is 3; body, hand and feet marks work with the stock counts).
3. Optional: tune `SKSE/Plugins/SimpleBleedoutKO.ini` (every setting is explained inside; restart the game after an edit).
4. It can be added to an existing save.

**Uninstall:** heal (or `sbkot`) every knocked-out NPC first so they are up, save, then remove the mod. Never rename
`SimpleBleedoutKO_001.esp`.

## Settings worth knowing
- **Game-wide regeneration:** the mod lowers the game's own out-of-combat regeneration for *every* essential / protected NPC
  (`fEssentialNonCombatHealRateBonus`, game default 5.0, this mod 0.1) so followers stay hurt after a fight. 0.01 = no healing without
  potions or magic; -1 = leave the game's value alone.
- **Skyrim VR only:** the game's full heal of essential NPCs at the end of every fight is skipped (`iCombatEndRefill`: 2 everyone
  protected, 1 followers only, 0 off). On SE / AE this setting does nothing.
- **Diary of Mine users:** set `bSleepWakeOnTalk=0`, or talking to a sleeper wakes them before you can choose Tie-up or Bag.
- The log: `Documents/My Games/<game>/SKSE/SimpleBleedoutKO.log` - attach it to any bug report.

## Compatibility
- **Follower Death and Injury Chance:** this mod knocks a downed follower out at once (`fBleedoutGraceSeconds=0`), which may come before
  Follower Death and Injury Chance makes its death roll. If you use it, try `fBleedoutGraceSeconds=1.5` so its roll happens first
  (**not verified** - please report). The `[FDC]` section of the ini needs a separate patch that is **not** part of this mod; without it
  that section does nothing.
- **Other bleedout / knock-out managers** (bleedout revamps, a follower framework's bleedout recovery, NPC-revive mods): only one mod should
  own the knock-out. Turn their bleedout handling off.
- **Overlay mods** (SlaveTats, other RaceMenu overlay users): the marks only use the slots listed in the ini (face 10-12, body 3-5, hands 2,
  feet 2) and skip any slot another mod holds.
- One light plugin (ESL-flagged), no vanilla record edited.
- **Skyrim SE / AE:** built for them (one DLL), not tested yet. The VR-only parts (the combat-end heal guard, PLANCK's head-only rule) switch
  themselves off there and say so in the log.

## FAQ
- **My follower won't get up.** Heal them - any healing spell, staff, scroll or potion - or wait 6-12 game hours.
- **No skin marks at all.** Check the log's `[marks] start:` line: RaceMenu missing, Battle Wounds not installed as loose files, or Injury
  Engine missing. In VR, see the Injury Engine note above.
- **Body marks but no face marks.** Raise RaceMenu's face overlay count (Installation, step 2).
- **My punches never knock anyone out.** By design: a bash, a mace / warhammer, the chance roll or the Sleep spell.
- **I hit a sleeping NPC and they woke.** That is the Sleep spell: it is only sleep.

## Console (select the NPC first)
`sbko help` lists everything. Useful ones: `sbko status` (what the mod knows about this NPC; `sbkos` does the same), `sbkot` (end this
NPC's knock-out timer now), `sbko marks status`. These are debugging tools.
**`sbkou` is DANGEROUS**: on a protected / essential NPC it drops them to 0 Health - never use it as a status check, and not in a save you
care about.

## For modders
`SBKO_API` (Papyrus, `Native Hidden`, source in `Scripts/Source/SBKO_API.psc`): `Int ApiVersion()` (= 1),
`Bool Holds(Actor akTarget)`, `Bool TakeOver(Actor akTarget, Actor akAggressor, String asSource, Float afChokeSeconds)` (source
`"VRTouchEvents.choke"`), `ChokeReleased(Actor akVictim, Float afSecondsHeld)`. Call them through your own gate script so your scripts
still load without this mod. The knock-out state is also readable as the rank of `SBKO_StateFaction` (`SimpleBleedoutKO_001.esp` 0x803;
10 = knocked out, 3 -> 1 = coming round) and `SBKO_SleepFaction` (0x80A: 1 / 2 = Sleep spell, 3 = choked).

## Building from source
This repository holds the full source: the C++ SKSE plugin (`src/`, `CMakeLists.txt`) and the mod's own files laid out as they
install into the game's `Data/` folder (`Data/`: the plugin `SimpleBleedoutKO_001.esp`, the Papyrus sources, the ini, `marks.json`,
the SkyPatcher and SkyrimNet files). The compiled files (`SimpleBleedoutKO.dll`, the `.pex` scripts) and the four band textures are
in the release zip - to play, install the zip from the Releases page.

**The DLL** (one DLL for SE 1.5.97, AE 1.6.x incl. 1.7.99 and VR 1.4.15):
- Visual Studio 2026 with the C++ workload (MSVC, C++23; its bundled vcpkg provides spdlog, nlohmann-json and the other ports in
  `vcpkg.json`), CMake 3.21 or later.
- `git clone --recurse-submodules https://github.com/Telord72612/SimpleBleedoutKO.git` - CommonLib comes as the submodule
  `extern/CommonLibVR` (MinLL/CommonLibVR v4.39.5, MIT; it brings OpenVR as its own submodule). After a plain clone:
  `git submodule update --init --recursive`.
- Run `build.bat` (or `cmake --preset all` then `cmake --build build/all --config Release`). The DLL and its PDB land in
  `_stage/SKSE/Plugins/`; nothing is copied into a game folder.

**The scripts** (`Data/Scripts/Source/*.psc`): compile with the Creation Kit's compiler or Caprica (game: Skyrim) against the vanilla
and SKSE script sources plus the script sources of RaceMenu (`NiOverride`), SkyrimNet (`SkyrimNetApi`), Injury Engine
(`Injury_Engine_Engine`) and - for `SBKO_SeverCompat` only - SeverActions.

## Credits
- **Battle Wounds** by AltNameByeee (LoversLab) - the skin-mark art (a requirement, not included). The wrist / ankle bands in this mod are
  reshaped from its "Strangled" neck mark and included with the author's permission on credit; they are not covered by this mod's licence.
- **Knockout - Health and Stealth-Based KOs** - the ragdoll knock-out technique and the bash / sneak rules that inspired ours.
- **Injury Engine** by GoodProvider - the sister mod whose wounds drive the marks.
- **SkyrimNet**, **PLANCK** by FlyingParticle, **RaceMenu** by expired6978 / RaceMenu VR.
- Built on **CommonLibVR (MinLL 4.39.5, MIT)**, **nlohmann/json**, spdlog, {fmt} - notices in `THIRD_PARTY_NOTICES.txt` beside this file.
  Full credits: `CREDITS.txt`.

## Licence
MIT (see `LICENSE` beside this file) for the code, scripts, prompts and data of SimpleBleedoutKO. **Exception:** the four wrist / ankle
band textures are derived from Battle Wounds and are not under the MIT licence (see Credits).
