# PROBLEMS.md — rejected, uncertain and caveated items

Set: `MANIFEST.md` (16 ROMs, 1,630,464 B). This file records everything that was
rejected, every caveat attached to included ROMs, and open questions.
The `roms/` set is frozen: ROM bytes and names must not change after this report.

---

## A. REJECTED — not included in the final set

### A1. `celeste.nes` — Celeste NES (demo 1.00) — REJECTED after harness testing
- **Reason:** loads (iNES parses, mapper 4 selected) but the framebuffer stays a solid
  gray backdrop for 3600 frames — the PPU never enables. nofrendo core incompatibility,
  not a ROM corruption problem. Harness verdict: "passes load only".
- License status (for the record, not the reason for rejection): code MIT
  (`license.txt`); graphics/music under a custom *strictly non-commercial* license;
  the Celeste IP itself is owned by Maddy Makes Games, Inc. — a non-commercial fan demake.
- License copy kept for the record: `licenses/celeste.txt`.
- Source it came from: https://github.com/iProgramMC/CelesteNES/releases/download/demo-1.00/celeste.nes
  (sha256 `1efcb458ac4fd41e48cfbed1245b1a278a96d6adb94cb8f9ef876c1a6107b5e2`).

### A2. Lizard (`bbbradsmith/lizard_src_demo`) — REJECTED: assets not redistributable
- Repo `license.txt`: CC BY 4.0 applies to the **source code only**; "The assets/ folder
  is provided for reference only, and may not be redistributed", and the name/characters/
  art/music "may not be reused without permission". The GitHub distribution itself omits
  the asset folder because its license is incompatible.
- A ROM built from this source would embed the non-redistributable assets → rejected.
- Local clone: `/tmp/roms-src/bbbradsmith_lizard_src_demo`.

### A3. Cold & Starving (`jrs0ul/nes_survival`) — REJECTED: no license
- No LICENSE/COPYING file anywhere in the repository; only a README. Built ROMs exist
  locally (`Cold & Starving(Europe).nes`, `Cold & Starving(USA, Japan).nes`, mapper 2,
  131,088 B each) but no redistribution rights are granted anywhere → rejected.

### A4. Tactus (homebrew-db entry `entries/tactus`) — REJECTED: unsupported mapper
- ROM is **mapper 28** (Action 53), which is not implemented in
  `components/nofrendo/mappers` (supported list excludes 28/30/31/71/111/118/163/268).
  The homebrew-db entry also has no license field → doubly rejected.

### A5. Action-53 (mapper 28) builds in `mhughson/mbh-cart` — REJECTED: unsupported mapper
- The repo's compo/`A53` builds are mapper 28 and cannot run. Only the author's MMC1
  (mapper 1) build of MineKart Madness is included (see §B4).

### A6. Entries with no verifiable redistribution license (homebrew-db / itch.io only)
Rejected under the "free to download is not enough" rule — the homebrew-db entry or the
itch.io page gives no explicit license and no upstream repository/LICENSE could be
verified for the ROM:
- `irritating-ship` (fiskbit — itch.io page only, no license)
- all `johnybot_*` entries: chessnovice, color-match-combat, dig, enter-password,
  guild-quest, heist, kart-racer, nes-dvd-screensaver, quarti, sudoku-23, super-snake-off,
  tiny-golf, treasure-hunt (itch.io pages only, no license)
- `plourples/helmet-nes` (itch.io page only, no license)
- `wrymouth/my-dvd-logo-daycare` (itch.io page only, no license)
- `cybercoaster` (homebrew-db metadata says `license: CC-BY-4.0`, but the entry has **no
  repository URL** and no upstream LICENSE file could be verified → excluded as
  unverifiable; could be added later if the upstream repo/license is located)

### A7. Other candidates considered but not used
- **240p Test Suite** (`pinobatch/240p-test-mini`, GPL-2.0-or-later): legitimate and
  redistributable, but it is a calibration/test *tool*, not a game → not part of a game set.
- **Numism** (`pinobatch/numism`, Apache-2.0): work-in-progress tech demo / test ROM
  ("Is it a platformer game? Is it a test ROM?" per its README) → not a finished game.
- **Super Tilt Bro other builds**: `Super_Tilt_Bro_(E).nes` on itch.io is the RAINBOW
  mapper build (mapper 682) and `tilt_no_network_unrom512_(E)` is mapper 30 (UNROM512) —
  both unsupported. The included file is the **mapper 2 (map002.c)** no-network UxROM
  variant, which *is* supported (see §B2).

### A8. `roborun.nes` — RoboRun-NES 1.1 — REJECTED after boot testing (PAL-only)
- **Reason:** the ROM is a PAL/Dendy-only build. On the NTSC-timed nofrendo core it draws
  "WRONG VIDEO REGION SELECTED / PLEASE SWITCH TO PAL OR DENDY" and is unplayable; the
  upstream README also says "Supports only PAL/Dendy". It was in the sourced 17-ROM
  candidate set and passed a naive frame-render test, but was removed once the region
  check was spotted.
- License: GPL-3.0-only (redistributable, so licensing is not the reason for exclusion).
- License copy kept for the record: `licenses/roborun.txt`.
- Source: https://github.com/heaven-hm/roborun-nes/releases/download/1.1/RoboRun-NES_1.1.nes

---

## B. INCLUDED with documented caveats

### B1. `twin_dragons.nes` — Shiru music/SFX are "all rights reserved" upstream
- Included **by explicit Lead decision** (the ROM is also in the currently shipped
  `firmware/roms.bin`, sha256 `dc03a714…`, so it is byte-identical to the repo's copy).
- Upstream README "Licensing": code = zlib; some artwork = CC-BY 3.0; other music
  (Garydos) = CC-BY 4.0; **but** "Music in the `famitone5` directory and the SFX
  `twin_dragons_sfx.ftm` file in the `music` directory is made by Shiru, all rights
  reserved." The compiled ROM therefore embeds all-rights-reserved data.
- Mitigation: the author distributes the compiled binary himself in
  https://github.com/Garydos/twin-dragons-nes (`bin/twin_dragons.nes`, commit bcf27d0);
  MiniMario ships it verbatim, non-commercially, with this notice preserved and the
  music/SFX explicitly **not** relicensed. Do not extract/reuse the music/SFX.
- Full text + caveat: `licenses/twin_dragons.txt`.

### B2. `super_tilt_bro.nes` — >256 KB, mapper-2 UxROM, 2-player-oriented
- 524,304 B (512 KB PRG, mapper 2, NES-2.0-style header byte7=0x08, submapper 1, battery
  flag). nofrendo's `map002.c` + `mmc_bankrom(16,…)` uses `bank % rom_banks` (32), so the
  512 KB UxROM bank window works; harness reports **600-frame PASS** (host + device).
- This build is the repo's `-DMAPPER_UNROM` "no network" UxROM variant
  (`tilt_no_network_unrom_(E)`), obtained from the official itch.io download
  (`Super_Tilt_Bro_(E).nes`, 512 kB, page version 2.6 at download time).
- Game is a 2-player versus fighter (2 controllers needed for versus); it also has a
  single-player Adventure mode. Keeper by Lead request (compat test ROM).
- Flagged `>256 KB (compat-unverified)` in MANIFEST per instruction, but boot-tested OK.

### B3. `nova.nes` — >256 KB (256 KB PRG)
- 262,160 B, mapper 1 (MMC1, CHR-RAM). Harness/host boot OK (300 frames; pre-tested
  earlier by the harness as well). Flag: >256 KB.
- License: GPL-3.0-or-later code + CC-BY-NC-SA-4.0 assets → redistribution allowed for
  non-commercial use; the "Nova" character may not be reused elsewhere (not relevant to
  shipping the unmodified ROM).

### B4. `minekart_madness.nes` — >256 KB (256 KB) and an "untested" build upstream
- 262,160 B, mapper 1 (MMC1). Author's commit message for this MMC1 build says
  "Untested (can't upload yet)", but it **loads and renders fine** in the host nofrendo
  harness (300 frames, non-blank, changing frames) and will be re-checked by the
  nofrendo-harness teammate.
- **License coverage confirmed (Lead request):** the shipped bytes are
  `promotion/ninjapad-itch/ninjapad-main/roms/minekart_madness_mmc1.nes` from
  https://github.com/mhughson/mbh-cart (commit `123d7742fbdce9120ad283737f172172670740f1`).
  That repository is the game's own source repository ("Minekart Madness. NESDev Compo
  Entry 2022") and its root `LICENSE` is **MIT © 2022 Matthew Hughson**, the game's
  author; GitHub reports the repo license as MIT. The nested NinjaPad v1.2.5 showcase
  directory carries its own **CC-BY-4.0** LICENSE (© 2021 Ninja Dynamics) which also
  permits redistribution. Full texts: `licenses/minekart_madness.txt`.
- A smaller official 64 KB build exists on itch.io (`mhughson.itch.io/minekart-madness`,
  `minekart_madness.nes`); the MMC1 build was chosen because its bytes are directly
  verifiable in the MIT repo.
- The repo's mapper-28 Action-53 builds are unsupported and were not used (§A5).

### B5. `driar.nes` — PAL-designed game
- Official `Driar.txt`: designed for PAL 50 Hz, "works well on NTSC (60 Hz) as well",
  but the game and music play "a bit faster" on NTSC. Boot-tested OK.

### B6. Version-choice discrepancies for `nesdev-org/homebrew-db` sourced ROMs
- `thwaite.nes`: shipped = db `thwaite128.nes` (24,592 B) which is **Thwaite 0.04a**;
  the GitHub release tag `v0.04` asset is a *different* build (sha256 `a2df24d9…`) and
  repo HEAD builds 0.04b. Older/newer builds all GPL-3.0-or-later. Shipped version is
  the newest published build we could verify as an artifact.
- `croom.nes`: shipped = db v0.03; GitHub release is v0.02a.
- `ruder.nes`: shipped = db v0.03; GitHub release is v0.03a.
- `rhde.nes`: db copy and GitHub release v0.07 are byte-identical.
- `robotfindskitten.nes`: GitHub release v0.10 is shipped; rebuilding repo HEAD produces
  different bytes only because an embedded build-day number changes.
- None of these differences are license or compatibility problems.

### B7. Header notes (low risk)
- `nes_runner.nes`, `nova.nes`, `super_tilt_bro.nes`, `minekart_madness.nes` carry a
  NES-2.0-style byte 7 (`0x08`). nofrendo ignores NES 2.0 extended fields: it reads
  mapper from byte 6 high nibble + byte 7 high nibble and ROM sizes from bytes 4/5, which
  are correct for all four (mappers 1/1/2/1). All boot-tested fine.
- `super_tilt_bro.nes` has the battery flag set; nofrendo always allocates SRAM anyway
  (no `.sav` persistence in this port, harmless).

---

## C. Boot-test results (headless host build of the same nofrendo core, 600 frames)

All 16 shipped ROMs returned `RESULT ok` with non-blank frames that changed during the
run (600 frames each; a 3600-frame stability run also passed). Two sourced candidates
were rejected after this testing: Celeste NES (nofrendo never enables the PPU; the same
ROM renders its title screen in jsnes, so it is a core-only incompatibility) and
RoboRun-NES (PAL-only region check, see A8).

## D. Freeze note

The `roms/` set is final: 16 `*.nes`, `MANIFEST.md`, `PROBLEMS.md`,
`licenses/*.txt` (18 files — includes the retained `celeste.txt` and `roborun.txt`
for the two rejected candidates). No shipped ROM will be renamed or rewritten.
