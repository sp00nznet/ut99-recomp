# Roadmap

## Bring-up

In order; each is done when its check is in the harness or the docs.

1. **UCC runs.** Done: `ut99 --run --ucc -- server` runs a Deathmatch
   server headless and answers queries (docs/bringup.md). Next: a client
   joining it, and bots (`?numbots=`) in a match that ends.
2. **The launcher boots.** Done: the intro as the original renders it, and
   the main menu (docs/bringup.md 9-14). Next: the first-run wizard, instead
   of skipping it in the ini.
3. **Headless and `--record`**: done (docs/bringup.md 10), and the README's
   screenshots come from it; `--key` scripts input.
4. **A match played**: Deathmatch against bots, scripted: done (docs/bringup.md
   15), the HUD as the original draws it (16). Next: a match to its frag
   limit, a map change, CTF; Galaxy's SSE and 3DNow! mixer paths, still
   lifted as nothing (sound is not checked yet).
5. **Interactive play**: a live window with mouse look (WinDrv's captured
   mouse), which the presenter will own.
6. **Conformance harness** (`tools/conformance.py`): boot milestones and lift
   health against `conformance.json`, failing on regression; netlab recipe.
7. **Setup.cmd**: the Quick start, finding the Steam install, run end to end
   from a clean folder.

## The makeover

What Gunman and Red Alert 2 got, adapted to an engine that already renders
in 3D:

- **Our own presenter** (Direct3D 11): a resizable window, borderless
  fullscreen, settings remembered, the game's resolution decoupled from the
  window's.
- **A modern render device.** UT's renderers are plug-ins
  (`URenderDevice`), so the makeover is a new device, not a patch to an old
  one: Direct3D 11, any resolution, anti-aliasing, anisotropic filtering,
  no 16-bit banding, a proper gamma, and the HUD and fonts scaled for 1080p
  and 4K instead of shrinking.
- **Widescreen**: Hor+ field of view, and the menus and HUD placed for wide
  screens.
- **Frame pacing**: UT's tick is tied to the frame; uncapped, high frame
  rates need the known speed and physics fixes.

## Deferred

- The editor (`UnrealEd.exe`, `Editor.dll`): not needed to play.
- Glide, MeTaL and SGL devices: their hardware is gone.
- Online play against the public master server: LAN first.

## Out of scope

- Shipping any of the game's files or anything generated from them.
- Reimplementing the engine: the point is Epic's own code, recompiled.
