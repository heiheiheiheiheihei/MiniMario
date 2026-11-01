# MiniMario free-homebrew NES ROM set — MANIFEST

Generated: **2026-11-01 11:27 UTC+08:00** (Asia/Shanghai) — final set, frozen after this point.

**16 ROMs, 1,630,464 bytes total** (flat filenames; every file is a byte-identical copy of the upstream artifact named below — only the filename was changed).

- Container (MMROM1) budget: partition `0x1F0000` = 2,031,616 B; overhead = header 12 B + 16×40 B entry table = 652 B, first blob 16-B aligned (656 B); required = 1,630,464 + 656 = **1,631,120 B** → fits with **400,496 B spare**.
- All files are standard 16-byte iNES headers and use mappers implemented in `components/nofrendo/mappers` (0, 1, 2).
- All filenames are lowercase ASCII, ≤ 31 chars (menu-name limit). Every file size is a multiple of 16, so 16-byte alignment adds no padding.
- License text copies: `licenses/<rom>.txt` (verbatim upstream license file(s) + provenance header).
- Boot status: **all 16 ROMs load and render non-blank frames** in a headless x86-64 build of the same patched nofrendo core (600-frame canonical run + 3600-frame stability run, no crashes/timeouts). Two sourced candidates were rejected after boot testing: See PROBLEMS.md.
- **Not included:** `celeste.nes` (Celeste NES demo-1.00) — nofrendo never enables its PPU (solid black; jsnes renders the title), and `roborun.nes` (RoboRun-NES 1.1) — PAL/Dendy-only build that shows "WRONG VIDEO REGION SELECTED" on the NTSC core. See PROBLEMS.md; their license copies are retained at `licenses/celeste.txt` and `licenses/roborun.txt`.
- **Minekart Madness license coverage CONFIRMED:** the shipped MMC1 ROM is `promotion/ninjapad-itch/ninjapad-main/roms/minekart_madness_mmc1.nes` from the MIT-licensed `mhughson/mbh-cart` repository ((c) 2022 Matthew Hughson, the game's author; GitHub reports the repo license as MIT), committed by the author in commit 123d774. It is the game's own source repository, so the MIT LICENSE covers the ROM. See §6 below and `licenses/minekart_madness.txt`.

## ROM table

| # | File | Title | Author | Bytes | Mapper | License (SPDX) | sha256 |
|---|------|-------|--------|------:|:------:|----------------|--------|
| 1 | `croom.nes` | Concentration Room v0.03 | Damian Yerrick | 24,592 | 0 | GPL-3.0-or-later (upstream README adds an exception allowing distribution of exact iNES copies without source) | `9b0b1e9b14b07a2e69db287e04f711e16af42b6cc85523b66bdfa640df80507a` |
| 2 | `driar.nes` | Driar | Stefan Adolfsson & David Eriksson (Rickard Nilsson, early dev) | 131,088 | 1 | CC-BY-NC-3.0 | `b45b0e075ac21eb11cff7c0607a23eaf586a318e4d944aaf7af3e1b0d7cbfea1` |
| 3 | `falling.nes` | Falling | Jesse Williams (xram64 / tragicmuffin) | 40,976 | 0 | MIT | `e22b947542c2d7e595bf84725b333be7af8189c5965b9c53e356a249c7d79943` |
| 4 | `floatmancer.nes` | Floatmancer | NotExactlySiev | 40,976 | 0 | CC0-1.0 | `080cb3a284b886264c271177ad1bc1ab5387098ce1725f73976ebdb99c4a41c4` |
| 5 | `mega_mountain.nes` | Mega Mountain: NES Edition | adrian09_01 (NES port), Doug Fraker/nesdoug (tutorial base), Damian Yerrick (Pently music) | 40,976 | 0 | GPL-3.0 (repo LICENSE) and MIT (License.txt); Pently music engine zlib | `b3a29bc7d6c272ec9af42a21eea44872f17feadcb866a5098ba28a7fbbc78d2d` |
| 6 | `minekart_madness.nes` | MineKart Madness (NESDev Compo 2022, MMC1 build) | Matthew Hughson (mhughson) | 262,160 | 1 | MIT (repository root LICENSE, (c) 2022 Matthew Hughson); nested NinjaPad showcase subtree additionally CC-BY-4.0 | `45d18d8b3662e6c08043759e550e658bcd23e48394a00f24de5e5fbe57e4900a` |
| 7 | `nes_runner.nes` | runner.nes ("NES Runner") | Zorchenhimer | 81,936 | 1 | MIT | `1a67bfabdde2d96eacb04eb7f9d17e8da6c082d4976c318315fe31e2d19b4384` |
| 8 | `nova.nes` | Nova the Squirrel v1.0.6a | Nova Squirrel | 262,160 | 1 | GPL-3.0-or-later (code) + CC-BY-NC-SA-4.0 (assets) | `e4780e90b9d1587489bfb797d2ca395be21371ea9262fa9f87f99324ec6960ab` |
| 9 | `rainfall.nes` | Rainfall | bbossa | 40,976 | 0 | CC0-1.0 | `8f1b56d939d1d551ac8e639a18833053f4b1865af40420d30dc73dfa5110d4e7` |
| 10 | `rhde.nes` | RHDE: Furniture Fight v0.07 | Damian Yerrick | 32,784 | 0 | FSFAP | `b2c4748a5b3651e393572046daf126213653b58b55472cdfdf4b863834dd0241` |
| 11 | `robotfindskitten.nes` | robotfindskitten v0.10 | Damian Yerrick | 32,784 | 0 | Zlib | `13abbea91f553780c88c2a85a40b7e86fd5916026c01bfc4f88a8b9b9a9abfe1` |
| 12 | `ruder.nes` | Zap Ruder v0.03 | Damian Yerrick | 24,592 | 0 | FSFAP (LICENSE.txt; homebrew-db metadata labels it "Zlib" - the actual license text is FSFAP, both permissive) | `dc007ccbeeb8aebe19e8ec08091c0a1140a22254804d7443dc82f3b955934d8b` |
| 13 | `sliding_blaster.nes` | Big City Sliding Blaster | Nova Squirrel | 24,592 | 0 | Zlib | `8d9e48253238e2c994d65a5c13818c8f1aae7f37fdfcb3e48a916d9298ad620f` |
| 14 | `super_tilt_bro.nes` | Super Tilt Bro. (no-network UxROM build) | Sylvain Gadrat (sgadrat) + contributors (see in-ROM credits) | 524,304 | 2 | WTFPL | `6f077796b539b2b75783452fa515d453f72bd7416fe25aeec6fe01c45ec95d40` |
| 15 | `thwaite.nes` | Thwaite v0.04a (128K build) | Damian Yerrick | 24,592 | 0 | GPL-3.0-or-later | `80e0918ea17577477c2f1b16beefadf457f207931ffbeae7612d8d230f999843` |
| 16 | `twin_dragons.nes` | Twin Dragons | Garydos (code), surt (art), Shiru (FamiTone music/SFX) | 40,976 | 0 | Zlib (code) + CC-BY-3.0 (some art) + CC-BY-4.0 (Garydos music); Shiru music/SFX marked all-rights-reserved upstream (documented caveat) | `dc03a714636b0fd0cf2246475073baba1fcc82ba9d4ac7e55cac5ea8804f0598` |

**Totals:** 16 files, **1,630,464 B** (1.555 MiB); container **1,631,120 B** of 2,031,616 B.

## Source URLs, versions and per-ROM notes

### 1. `croom.nes` — Concentration Room v0.03

- **Author:** Damian Yerrick
- **size / mapper:** 24,592 B, mapper 0
- **sha256:** `9b0b1e9b14b07a2e69db287e04f711e16af42b6cc85523b66bdfa640df80507a`
- **License:** GPL-3.0-or-later (upstream README adds an exception allowing distribution of exact iNES copies without source)
- **License text:** `licenses/croom.txt`
- **Source URL:** https://github.com/nesdev-org/homebrew-db/blob/master/entries/croom/croom.nes
- **Version / commit / build:** homebrew-db commit d64e758 (upstream repo: github.com/pinobatch/croom-nes; db metadata: GPL-3.0-or-later)
- **Note:** Memory/puzzle game, 1-2 players. db bytes are a newer build (v0.03) than the GitHub release v0.02a.

### 2. `driar.nes` — Driar

- **Author:** Stefan Adolfsson & David Eriksson (Rickard Nilsson, early dev)
- **size / mapper:** 131,088 B, mapper 1
- **sha256:** `b45b0e075ac21eb11cff7c0607a23eaf586a318e4d944aaf7af3e1b0d7cbfea1`
- **License:** CC-BY-NC-3.0
- **License text:** `licenses/driar.txt`
- **Source URL:** http://driar.se/driar/driar.zip (official site http://driar.se/)
- **Version / commit / build:** official zip dated 2012-02-26; zip sha256 757c9c58feb5849c2bb507e844fcb898eef783bb69cf4fa77502cd76e70841b0; Driar.nes extracted verbatim
- **Note:** Mapper 1 (MMC1, CHR-RAM), 128 KB. PAL-designed (50 Hz) but works on NTSC (60 Hz) and plays slightly faster. 38 single-player levels.

### 3. `falling.nes` — Falling

- **Author:** Jesse Williams (xram64 / tragicmuffin)
- **size / mapper:** 40,976 B, mapper 0
- **sha256:** `e22b947542c2d7e595bf84725b333be7af8189c5965b9c53e356a249c7d79943`
- **License:** MIT
- **License text:** `licenses/falling.txt`
- **Source URL:** https://github.com/xram64/falling-nes/blob/52dcb8a951200562e696dfc2aba5d4d14edd0078/falling.nes
- **Version / commit / build:** commit 52dcb8a
- **Note:** Repo-tracked released ROM (byte-identical upstream).

### 4. `floatmancer.nes` — Floatmancer

- **Author:** NotExactlySiev
- **size / mapper:** 40,976 B, mapper 0
- **sha256:** `080cb3a284b886264c271177ad1bc1ab5387098ce1725f73976ebdb99c4a41c4`
- **License:** CC0-1.0
- **License text:** `licenses/floatmancer.txt`
- **Source URL:** https://github.com/NotExactlySiev/floatmancer/blob/83dc018ec15c2e3b07a961dd3b0fd2544b206a3d/bin/fmancer.asm.rom
- **Version / commit / build:** commit 83dc018 (renamed from fmancer.asm.rom; bytes unchanged)
- **Note:** Renamed only.

### 5. `mega_mountain.nes` — Mega Mountain: NES Edition

- **Author:** adrian09_01 (NES port), Doug Fraker/nesdoug (tutorial base), Damian Yerrick (Pently music)
- **size / mapper:** 40,976 B, mapper 0
- **sha256:** `b3a29bc7d6c272ec9af42a21eea44872f17feadcb866a5098ba28a7fbbc78d2d`
- **License:** GPL-3.0 (repo LICENSE) and MIT (License.txt); Pently music engine zlib
- **License text:** `licenses/mega_mountain.txt`
- **Source URL:** https://github.com/pinobatch/MegaMountainNES/blob/9a6d728b8619199512b957e4899c2ab6138dc89b/mega_mountain.nes
- **Version / commit / build:** commit 9a6d728
- **Note:** Repo-tracked released ROM. Dual upstream LICENSE/License.txt, both permissive.

### 6. `minekart_madness.nes` — MineKart Madness (NESDev Compo 2022, MMC1 build)

- **Author:** Matthew Hughson (mhughson)
- **size / mapper:** 262,160 B, mapper 1
- **sha256:** `45d18d8b3662e6c08043759e550e658bcd23e48394a00f24de5e5fbe57e4900a`
- **License:** MIT (repository root LICENSE, (c) 2022 Matthew Hughson); nested NinjaPad showcase subtree additionally CC-BY-4.0
- **License text:** `licenses/minekart_madness.txt`
- **Source URL:** https://github.com/mhughson/mbh-cart/blob/123d7742fbdce9120ad283737f172172670740f1/promotion/ninjapad-itch/ninjapad-main/roms/minekart_madness_mmc1.nes
- **Version / commit / build:** commit 123d774 ("Updated ninja pad with MMC1 rom")
- **Note:** LICENSE COVERAGE CONFIRMED (Lead request): the exact file is `promotion/ninjapad-itch/ninjapad-main/roms/minekart_madness_mmc1.nes` in the MIT-licensed mhughson/mbh-cart repository ((c) 2022 Matthew Hughson, the game author; GitHub reports repo license = MIT). The ROM was committed by the author himself in commit 123d774; the repo description is "Minekart Madness. NESDev Compo Entry 2022", i.e. this is the game's own source repository, not a third-party bundle. The nested NinjaPad v1.2.5 showcase directory carries its own CC-BY-4.0 LICENSE ((c) 2021 Ninja Dynamics) which also permits redistribution. Full license texts: licenses/minekart_madness.txt. Mapper 1 (MMC1), 256 KB; >256 KB (flag). The repo's mapper-28 Action-53 builds are unsupported and not included.

### 7. `nes_runner.nes` — runner.nes ("NES Runner")

- **Author:** Zorchenhimer
- **size / mapper:** 81,936 B, mapper 1
- **sha256:** `1a67bfabdde2d96eacb04eb7f9d17e8da6c082d4976c318315fe31e2d19b4384`
- **License:** MIT
- **License text:** `licenses/nes_runner.txt`
- **Source URL:** https://github.com/zorchenhimer/nes-runner/releases/download/v1/runner.nes
- **Version / commit / build:** release v1 (2019-06-27)
- **Note:** Mapper 1 (MMC1); 64K PRG + 16K CHR. Renamed from runner.nes.

### 8. `nova.nes` — Nova the Squirrel v1.0.6a

- **Author:** Nova Squirrel
- **size / mapper:** 262,160 B, mapper 1
- **sha256:** `e4780e90b9d1587489bfb797d2ca395be21371ea9262fa9f87f99324ec6960ab`
- **License:** GPL-3.0-or-later (code) + CC-BY-NC-SA-4.0 (assets)
- **License text:** `licenses/nova.txt`
- **Source URL:** https://github.com/NovaSquirrel/NovaTheSquirrel/releases/download/v1.0.6a/nova.nes
- **Version / commit / build:** release v1.0.6a (2019-04-30)
- **Note:** Mapper 1 (MMC1, CHR-RAM); 256 KB PRG. >256 KB (flag). Release bytes match homebrew-db entry.

### 9. `rainfall.nes` — Rainfall

- **Author:** bbossa
- **size / mapper:** 40,976 B, mapper 0
- **sha256:** `8f1b56d939d1d551ac8e639a18833053f4b1865af40420d30dc73dfa5110d4e7`
- **License:** CC0-1.0
- **License text:** `licenses/rainfall.txt`
- **Source URL:** https://github.com/bbossa/rainfall_nes/blob/84350fd9ff4fe4e454eb5f44b4e1ace428346301/bin/rainfall.nes
- **Version / commit / build:** commit 84350fd
- **Note:** Repo-tracked released ROM.

### 10. `rhde.nes` — RHDE: Furniture Fight v0.07

- **Author:** Damian Yerrick
- **size / mapper:** 32,784 B, mapper 0
- **sha256:** `b2c4748a5b3651e393572046daf126213653b58b55472cdfdf4b863834dd0241`
- **License:** FSFAP
- **License text:** `licenses/rhde.txt`
- **Source URL:** https://github.com/pinobatch/rhde-nes/releases/download/v0.07/rhde.nes
- **Version / commit / build:** release v0.07 (2019-04-15); byte-identical to homebrew-db copy
- **Note:** Turn-based strategy, 1-2 players. Mapper 0, 32K PRG, CHR-RAM.

### 11. `robotfindskitten.nes` — robotfindskitten v0.10

- **Author:** Damian Yerrick
- **size / mapper:** 32,784 B, mapper 0
- **sha256:** `13abbea91f553780c88c2a85a40b7e86fd5916026c01bfc4f88a8b9b9a9abfe1`
- **License:** Zlib
- **License text:** `licenses/robotfindskitten.txt`
- **Source URL:** https://github.com/pinobatch/rfk-nes/releases/download/v0.10/robotfindskitten.nes
- **Version / commit / build:** release v0.10 (2018-10-20)
- **Note:** Zen game. Mapper 0, 32K PRG, CHR-RAM. Repo HEAD rebuild differs only by an embedded day number, so the official release binary is shipped.

### 12. `ruder.nes` — Zap Ruder v0.03

- **Author:** Damian Yerrick
- **size / mapper:** 24,592 B, mapper 0
- **sha256:** `dc007ccbeeb8aebe19e8ec08091c0a1140a22254804d7443dc82f3b955934d8b`
- **License:** FSFAP (LICENSE.txt; homebrew-db metadata labels it "Zlib" - the actual license text is FSFAP, both permissive)
- **License text:** `licenses/ruder.txt`
- **Source URL:** https://github.com/nesdev-org/homebrew-db/blob/master/entries/zap-ruder/ruder.nes
- **Version / commit / build:** homebrew-db commit d64e758 (upstream repo: github.com/pinobatch/zap-ruder)
- **Note:** Arcade shooter; controller build (Zapper mapped to A in the upstream build config). db build (0.03) is newer than the GitHub release v0.03a.

### 13. `sliding_blaster.nes` — Big City Sliding Blaster

- **Author:** Nova Squirrel
- **size / mapper:** 24,592 B, mapper 0
- **sha256:** `8d9e48253238e2c994d65a5c13818c8f1aae7f37fdfcb3e48a916d9298ad620f`
- **License:** Zlib
- **License text:** `licenses/sliding_blaster.txt`
- **Source URL:** built from https://github.com/NovaSquirrel/SlidingBlaster at commit fc60d79e2fd1684cd4bc6a926458e9a284212b15
- **Version / commit / build:** commit fc60d79 (no official release asset exists); build: ca65 src/blaster.s -> ld65 -C src/nrom128.x
- **Note:** Built from upstream source with ca65 2.19/ld65 (Ubuntu 2.19-2build1); no source changes.

### 14. `super_tilt_bro.nes` — Super Tilt Bro. (no-network UxROM build)

- **Author:** Sylvain Gadrat (sgadrat) + contributors (see in-ROM credits)
- **size / mapper:** 524,304 B, mapper 2
- **sha256:** `6f077796b539b2b75783452fa515d453f72bd7416fe25aeec6fe01c45ec95d40`
- **License:** WTFPL
- **License text:** `licenses/super_tilt_bro.txt`
- **Source URL:** official download https://sgadrat.itch.io/super-tilt-bro, file "Super_Tilt_Bro_(E).nes" (512 kB, page version 2.6 at download time); build variant per https://github.com/sgadrat/super-tilt-bro (master b132fd2) -DMAPPER_UNROM -> tilt_no_network_unrom_(E)
- **Version / commit / build:** official itch.io download v2.6; ROM iNES header matches repo MAPPER_UNROM config exactly (mapper 2, submapper 1, battery, 512 KiB PRG)
- **Note:** 2-player versus fighter (needs 2 controllers); has single-player Adventure mode. Mapper 2, 512 KB PRG, CHR-RAM. >256 KB (flag). Host harness 600-frame PASS.

### 15. `thwaite.nes` — Thwaite v0.04a (128K build)

- **Author:** Damian Yerrick
- **size / mapper:** 24,592 B, mapper 0
- **sha256:** `80e0918ea17577477c2f1b16beefadf457f207931ffbeae7612d8d230f999843`
- **License:** GPL-3.0-or-later
- **License text:** `licenses/thwaite.txt`
- **Source URL:** https://github.com/nesdev-org/homebrew-db/blob/master/entries/thwaite/thwaite128.nes
- **Version / commit / build:** homebrew-db commit d64e758; thwaite128.nes = Thwaite 0.04a 128K build (upstream repo: github.com/pinobatch/thwaite-nes)
- **Note:** Mapper 0, 16K PRG + 8K CHR. 1-2 player shooter. GitHub release v0.04 (different build, sha a2df24d9...) and repo HEAD (0.04b) exist; the newer 0.04a db build is shipped.

### 16. `twin_dragons.nes` — Twin Dragons

- **Author:** Garydos (code), surt (art), Shiru (FamiTone music/SFX)
- **size / mapper:** 40,976 B, mapper 0
- **sha256:** `dc03a714636b0fd0cf2246475073baba1fcc82ba9d4ac7e55cac5ea8804f0598`
- **License:** Zlib (code) + CC-BY-3.0 (some art) + CC-BY-4.0 (Garydos music); Shiru music/SFX marked all-rights-reserved upstream (documented caveat)
- **License text:** `licenses/twin_dragons.txt`
- **Source URL:** https://github.com/Garydos/twin-dragons-nes/blob/bcf27d0c5117a2aac19e6c33bdb3c776ada069fb/bin/twin_dragons.nes
- **Version / commit / build:** commit bcf27d0 (2019-11-16); byte-identical to the ROM committed by the author
- **Note:** Single-player platformer. Included by Lead decision (also present in the shipped firmware/roms.bin). Shiru music/SFX caveat documented in PROBLEMS.md and licenses/twin_dragons.txt.

## Provenance policy

1. Prefer the author's own published binary: GitHub release asset > ROM committed in the upstream repo > official project website > `nesdev-org/homebrew-db` published copy. Nothing was patched, rebuilt or edited; only renamed.
2. Every ROM's license was checked against the upstream repository LICENSE file (and README asset clauses where the LICENSE covers only code).
3. Built-from-source is used only where no upstream binary exists (Sliding Blaster), from a pinned upstream commit with no source changes.
4. ROMs over 256 KB are flagged in PROBLEMS.md; all were boot-tested before inclusion.
5. Exact provenance of the special-case ROMs: `super_tilt_bro.nes` = official itch.io `Super_Tilt_Bro_(E).nes` (512 kB, page v2.6) which is the repo's `-DMAPPER_UNROM` no-network UxROM build; `thwaite.nes` = homebrew-db `thwaite128.nes` (Thwaite 0.04a 128K build); `driar.nes` = `Driar.nes` from the official `http://driar.se/driar/driar.zip`; `minekart_madness.nes` = MIT `mhughson/mbh-cart` author-committed MMC1 ROM.

