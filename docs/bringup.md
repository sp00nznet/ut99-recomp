# Bring-up

Every wall so far, in the order they were hit, and what fixed it. The first
target is UCC's dedicated server (`ut99 --run --ucc --headless -- server
DM-Deck16][?game=Botpack.DeathMatchPlus`): no window and no renderer, but
Core, Engine, IpDrv, UWeb and the UnrealScript VM all run.

## 1. The launcher's code is encrypted

`UnrealTournament.exe` is wrapped in SteamStub 2.0; its first catalog found
0 call targets. Unwrapped statically by pcrecomp's `drm/steamstub.py`
(pcrecomp#52), called from `tools/rebase.py`. [RECON.md](RECON.md) has the
layout.

## 2. Map and bind

`build\ut99.exe` with no arguments maps every module at its base and binds
it. First run, all 13 bound with nothing missing:

```
[bind] 0x10300000: 14 native, 505 guest, 0 shimmed, 0 unresolved
...
[bind] 0x10900000: 109 native, 312 guest, 5 shimmed, 0 unresolved
```

"guest" is an import from another lifted module (Engine takes 505 from
Core), bound to that module's export so the call lands on lifted code.

## 3. "Can't resolve package name"

The first `--run --ucc` ran Core's `DllMain` and UCC's `main` into Core,
then died on a C++ throw:

```
=== C++ throw 0xE06D7363 at 0x76B41E54, thread 47092 ===
  thrown: Can't resolve package name
  in lifted sub_10170320 (Core.dll), last native call MSVCRT.dll!_CxxThrowException
```

That text is `Core.int`'s `PackageResolveFailed`, and the throw is not an
error: `UObject::StaticLoadObject` catches it itself and returns NULL. UCC
looks its commandlet up by trying class names, so the first miss throws. A
lifted guest throw went to the host's `_CxxThrowException`, whose SEH walk
knows nothing of the guest's frames.

Fixed in the toolkit: pcrecomp's eh32 (branch `feat/eh32`). The dispatch
reads only guest data (the `fs:[0]` chain, each frame's FuncInfo), runs the
unwind and catch funclets as guest code, and longjmps into a landing pad the
lifter gives every function with catch blocks. 56 functions across the
modules have one; most of this build's `guard`/`unguard` is compiled out.

One more piece was ours: a catch's resume address is reached only through
the funclet's return value, so the catalog often starts a function there and
cuts the parent short. `run_lift.py` takes resume points out of the function
walls and walks each one as a root of its parent's body.

## 4. UCC read the host's argv

With exceptions caught, UCC printed its banner and then:

```
Commandlet --run not found
```

UCC's `main(argc, argv)` gets argv from msvcrt's `__getmainargs`, which
parses the *process* command line: the host's. The host shims
`__getmainargs` with the guest's argv (everything after `--`).

## 5. A static constructor the catalog missed

The server then loaded every package and spawned its actors, and loading
`UWeb.dll` faulted:

```
Spawning: UWeb.WebServer
[load] UWeb.dll: DllMain
[callback] Windows called guest 0x10A03230, which is not lifted
```

`0x10A03230` is a static constructor named only by UWeb's `_initterm`
table. The function before it ends in a call that never returns, then 12
`nop`s of padding; disasm32 decoded on through the padding, so the data scan
saw the pointer land on an already-decoded instruction and skipped it as a
label. Fixed in the toolkit (pcrecomp branch
`feat/disasm32-nop-padded-entries`): a data pointer to a 16-aligned start
right after padding is an entry.

## 6. The dedicated server runs

With those, UCC's server comes all the way up and stays up:

```
Bringing Level DM-Deck16][.MyLevel up for play (20)...
Base Mutator is DM-Deck16][.DMMutator0
UdpServerQuery(crt): Port 7778 successfully bound.
Game engine initialized
```

and answers a server browser's query (`tools/query.py`) from its
UnrealScript query handler, through IpDrv:

```
hostname    Another UT Server
mapname     DM-Deck16][
gametype    DeathMatchPlus
numplayers  0
maxplayers  16
gamever     436
```

A 90-second run made 1.5 billion indirect calls (the VM's natives and
virtual dispatch) with no fault.

## 7. The launcher: Recovery Mode

`ut99 --run` (on an offstage virtual monitor) ran the launcher, attached every
DLL, and crashed in actor initialisation the first time. Every run after that
opened UT's own Recovery Mode dialog, drawn by the recompiled Window.dll:

![Recovery Mode](screenshots/recovery-dialog.png)

The launcher marks a session with `System\Running.ini` and deletes it on a
clean exit; a crash or a killed run leaves it. (`RunCount` in the ini is
only the splash rotation.) The host's watchdog now deletes the marker when it
stops a run on purpose; a real crash still leaves it.

`--log` prints the engine's log on stderr as it is written. Every module logs
through Core's exported `FOutputDevice::Logf`, so the host wraps that import;
UT's own log file is flushed only at exit and a killed run leaves it empty.

## 8. Galaxy's mixers are built at run time

The next crash was an execute fault at a heap address:

```
=== fault 0xC0000005 at 0x0CE0E81B, thread 59904 ===
  execute of 0x0CE0E81B
  region 0x0CDE0000+0xB0000 protect 0x4 type 0x20000
```

Those bytes are Galaxy's own mixer code (four templates in its `.text` start
with them): Galaxy copies the templates into `GlobalAlloc` memory, patches
them, and calls the copies. Heap memory was executable in 1999; under DEP it
is not. A static lift cannot contain code patched at run time, but the copy
is plain x86 working on guest memory in this address space, so the host
makes such a region executable on its first execute fault and the mixer runs
natively:

```
[log] Galaxy is using DirectSound
[log] Galaxy initialized
[gencode] guest-built code at 0x0D83023B: 0x0D830000+0x99000 made executable
[log] Game engine initialized
```

## 9. The intro renders

With that, the game loads the CityIntro map, starts the title music and the
software renderer draws the flyby in the game's window:

![The intro, software renderer](screenshots/intro-corridor.png)

The shipping ini starts in fullscreen through DirectDraw at 640x480x16, and
that mode set fails on Windows 11 (`DirectDraw Failed 0x0x16:
DDERR_UNSUPPORTED`). The recompiled build carries on in the window; the
**original binary does not start at all** with that ini, it faults in
`UWindowsViewport::ddSetMode` (docs/RECON.md). Both are run windowed without
DirectDraw for comparison (`StartupFullscreen=False`, `UseDirectDraw=False`):
the original reaches the same corridor, and is as dark there, since the
intro is lit that way in the software renderer. A frame-exact comparison is
the conformance harness's job (ROADMAP).
