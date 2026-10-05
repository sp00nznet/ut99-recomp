# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions follow [SemVer](https://semver.org/).

## [Unreleased]

### Added
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
- The game boots and renders its intro in the software renderer
  (docs/bringup.md 7-9). Code the guest builds at run time (Galaxy's patched
  mixer copies) runs natively: its heap region is made executable on the
  first execute fault. `--log` prints the engine's log as it is written.
  A watchdog stop removes `Running.ini`, so the next run is not Recovery Mode.
- The host hands UCC its own argv (`__getmainargs`) and reports the text of
  an uncaught guest throw.
