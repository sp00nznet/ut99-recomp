# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
- Borderless fullscreen on Alt+Enter (in place of UT's display-mode change),
  remembered across starts in `System\RecompHost.ini` (docs/bringup.md 21).
- *Mod > Recomp Graphics*: our UnrealScript package (`mod/RecompMenu`, built
  by the game's own `UCC.exe make` in `tools/build_mod.py`) adds a window to
  UT's Mod menu for anti-aliasing, texture filtering, the frame cap, vsync,
  HUD fonts and menu size. The host reads the first four from the game's ini
  (`[RecompMenu.RecompGraphicsClient]`); flags still override.
- Scaled HUD fonts (`RecompMenu.RecompFontInfo`): sizes keep growing past
  1024 pixels wide, up to UT's largest (30 point); on by default in the
  launcher's ini.
- `tools/uscript.py`: the UnrealScript text a package carries.
  `tools/click_original.py` takes keys and positions (`T@esc`, `T@X,Y`).

### Fixed
- The launcher passes `-INI=`/`-USERINI=`: without the dash, a start with no
  map took the ini's name for the map URL and resolved it as a host name.
- `tools/hd_ini.py` added nothing for a section the ini lacked; a rerun now
  keeps an existing pair and adds only what is missing (`--reset` starts over).
- Frame pacing for the OpenGL renderer: `--vsync 0|1`, `--fps-cap N` and
  `--show-fps`, by wrapping `SwapBuffers`; the launcher caps at 200 fps
  (docs/bringup.md 19).
- `--msaa N` and `--aniso N`: anti-aliasing and trilinear, anisotropic
  texture filtering for UT's OpenGL renderer, by wrapping the GL functions it
  finds with `GetProcAddress` (`src/runtime/glfx.c`); the launcher passes
  `--msaa 8 --aniso 16` (docs/bringup.md 18).
- The launcher runs UT's OpenGL renderer, recompiled, on its own ini pair:
  `tools/hd_ini.py` writes `UTRecomp.ini` (OpenGL, a 16:9 window) and
  `UTRecompUser.ini` (Hor+ field of view), passed as `INI=`/`USERINI=`; the
  stock ini keeps the software renderer for headless runs (docs/bringup.md 17).
- Played by hand in a live window: mouse look, Capture the Flag on three
  maps, a match to its end, map changes and a clean quit, nothing changed in
  the host for it. `Setup.cmd`'s launcher runs it.
- `Setup.cmd` (`tools/setup.ps1`): the Quick start in one double-click. It
  checks the tools and asks before installing any, clones pcrecomp on its
  `ut99-integration` branch (or moves a clone there) until the toolkit PRs
  merge, finds UT in the Steam library and copies it into `game\` set to run
  windowed, then rebases, catalogs, lifts, builds, checks every module binds
  and writes `Unreal Tournament (recomp).cmd`.
- The conformance harness, `tools/conformance.py`: 11 boot milestones from
  three headless runs (the intro reaching the city, a Deathmatch with a
  translucent HUD and no crash, the dedicated server answering a query) and
  lift health (errors, bodies with no terminator, instructions lifted as
  nothing), against `conformance.json`; a regression fails. netlab runs it
  as the recipe's QA.
- Translucency in the software renderer: the HUD's speech area is the
  original's translucent panel, not a black box. SoftDrv's span loops blend
  with MMX `paddusb`, which lifted as nothing (pcrecomp#61). `run_lift.py`
  counts every instruction lifted as nothing, per module
  (docs/bringup.md 16).
- `tools/click_original.py`: scripted clicks into the original game, for
  reference recordings under offstage; `tools/disasm_at.py`: a module's code
  at a VA, past its incremental-link thunk.
- The pipeline for the Steam build (v436): `tools/rebase.py` unwraps the
  SteamStub 2.0 launcher (needs pcrecomp `feat/steamstub-2.0`) and moves the
  four DLLs whose linked bases collide; `tools/catalog.py` runs disasm32 on
  every module in parallel; `run_lift.py` lifts each module into its own
  directory and builds one dispatch table over all of them (docs/RECON.md).
- The host, `ut99.exe` on pcrecomp `native32`: every module mapped at its
  base and bound, guest-to-guest imports on lifted exports, each DLL's
  `DllMain` run when the game first loads it, after its own guest imports.
  `--ucc` enters `UCC.exe` instead of the launcher.
- The dedicated server runs: `--ucc -- server` loads DM-Deck16][, starts a
  Deathmatch and answers server-browser queries; `tools/query.py` asks it.
  Needs pcrecomp eh32 (guest C++ exceptions: `StaticLoadObject` catches its
  own throws) and the disasm32 padded-entry fix (docs/bringup.md).
- `run_lift.py`: catch resume points are labels in their parent's body,
  which gets a landing pad; the shared tables are rewritten only on change.
- The main menu and a Deathmatch against bots, driven by `--key VK@S`
  (`esc`, `enter`, arrows, `fire`, `altfire`). Fire.dll's self-patching asm
  in `.data` runs natively (pcrecomp#57), and native32's bridge no longer
  trusts a native callee with ebx/ebp (#59): firing a weapon used to end the
  process (docs/bringup.md 14-15). Room is reserved for the crash report
  when the stack runs out.
- Headless mode: `--headless --record out.mp4 --frames N`, nothing on any
  screen, the game's frames to ffmpeg at 30 fps (docs/bringup.md 10).
- The intro renders like the original's. Every float-to-int cast had returned
  0x80000000 (`_ftol` ran natively on the real FPU; pcrecomp#56), so game time
  stood still; and the renderer's clipper never saw an edge cross a plane
  (`xor; jge` mis-lifted; pcrecomp#58).
- `--native Module.dll` runs a module's original code among the lifted ones,
  and `--lift-in-native FILE` lifts chosen functions of it (pcrecomp#57);
  `tools/find_bad_function.py` bisects a module down to the function that
  breaks it, from headless recordings.
- Builds go through recomp-netlab (`netlab build ut99-recomp`, clang-cl).
- The game boots and renders its intro in the software renderer
  (docs/bringup.md 7-9). Code the guest builds at run time (Galaxy's patched
  mixer copies) runs natively: its heap region is made executable on the
  first execute fault. `--log` prints the engine's log as it is written.
  A watchdog stop removes `Running.ini`, so the next run is not Recovery Mode.
- The host hands UCC its own argv (`__getmainargs`) and reports the text of
  an uncaught guest throw.
