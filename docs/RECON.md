# Recon: the binaries

What the Steam build of Unreal Tournament is made of, and what that meant for
the pipeline. The tools that act on it are `tools/rebase.py`,
`tools/catalog.py` and `run_lift.py`.

## The build

The Steam release (app 13240) is **v436**, the last Epic patch for Windows
before the community took over: `System\Manifest.ini` says `Version=436`, and
every engine DLL is linked 2000-10-25 by MSVC 6.0 (linker 6.0). It is the
plain edition, not GOTY: no S3TC texture CD, no bonus packs beyond what
`System\` holds.

## The modules

UT is a launcher and a dozen DLLs. Everything the game runs is lifted; the
rest of `System\` is left out:

| Module | Role | Linked base | Mapped at | Lifted |
|---|---|---|---|---|
| `UnrealTournament.exe` | launcher, main loop, splash, first-run setup | `0x10900000` | same | yes |
| `UCC.exe` | console launcher: commandlets, dedicated server | `0x00400000` | same | yes |
| `Core.dll` | objects, names, packages, UnrealScript VM, config | `0x10100000` | same | yes |
| `Engine.dll` | actors, levels, physics, net, canvas | `0x10300000` | same | yes |
| `Window.dll` | Win32 UI framework (WWindow) | `0x11000000` | same | yes |
| `WinDrv.dll` | viewport manager: window, input | `0x11100000` | same | yes |
| `Render.dll` | the scene renderer (BSP, meshes, lighting) | `0x10B00000` | same | yes |
| `SoftDrv.dll` | software rasteriser, the ini's default | `0x10D00000` | same | yes |
| `OpenGlDrv.dll` | OpenGL device | `0x10000000` | `0x11200000` | yes |
| `D3DDrv.dll` | Direct3D 7 device | `0x10000000` | `0x11300000` | yes |
| `Galaxy.dll` | audio | `0x10600000` | same | yes |
| `Fire.dll` | procedural textures (fire, water) | `0x10500000` | `0x10800000` | yes |
| `IpDrv.dll` | TCP/IP, master server | `0x10700000` | same | yes |
| `UWeb.dll` | the web admin server | `0x10700000` | `0x10A00000` | yes |
| `Editor.dll`, `UnrealEd.exe` | the editor | | | no |
| `GlideDrv`, `MeTaLDrv`, `SGLDrv` | 3dfx, S3, PowerVR devices | | | no |
| `Setup.exe`, `GotoHeat.exe`, `mplaynow.exe`, `testapp.exe` | installer, old online services, a copy of the launcher | | | no |

A static recompilation bakes every absolute address into the C, so modules
that collide have to be moved before anything else reads them. Epic's own
bases do not fit together: Engine (2.8 MB from `0x10300000`) runs over
Fire's `0x10500000`, both render devices sit at `0x10000000`, and IpDrv and
UWeb share `0x10700000`. `tools/rebase.py` applies each loser's `.reloc`
once, up front; every module has a relocation table, so nothing is guessed.
The host maps the copies at exactly those bases (`src/runtime/host.c`).

`MSVCRT.dll` in `System\` is the 1997 runtime the game shipped; the host
uses Windows' own `msvcrt.dll`, which every module imports by the same name.

## SteamStub

`UnrealTournament.exe` is wrapped: its `.text` is encrypted, the entry point
moved into a `.bind` section, and a disassembler finds nothing in it (the
first catalog decoded 0 call targets and covered 18% by accident). It is an
older SteamStub than the one on KotOR, with no `steamdrm.dll` inside:


The header's first dword seeds a running XOR over the rest (each dword XORed
with the previous *encoded* one). Decoded, it holds the OEP (`0x1092238C`),
the code range (`0x10901000`, `0x26000` bytes), a key, and the app id
(13240). With flag 4 set the stub runs the same running XOR over `.text`,
keyed by that field, and jumps to the OEP: an MSVC `WinMainCRTStartup`.

pcrecomp's `drm/steamstub.py` now unwraps this layout as well as KotOR's
(pcrecomp branch `feat/steamstub-2.0`), and `tools/rebase.py` calls it on
any image whose last section is `.bind`. `UCC.exe` and the DLLs are not
wrapped.

## No RTTI

The engine is built without `/GR`: `cpp/rtti.py` finds one class in each
module (`type_info` itself). Unreal has its own class system (`UClass`,
`IsA`), so nothing needed C++ RTTI. Vtable methods therefore come from
disasm32's data-pointer scan, not from RTTI seeds.

## Exports

The DLLs export their whole C++ surface by decorated name: Core 2,003
exports, Engine 2,355, Window 1,204. disasm32 seeds every export into code
as a function, and the names are there for the reading
(`?StaticConstructor@UObject@@QAEXXZ`). Some exports are data, and the
engine finds them by name at run time: `int<Class>exec<Func>` holds a native
function's pointer, `autoclass<Class>` registers a class. The host's
`GetProcAddress` answers both from the guest's export table.

## The function catalog

`tools/catalog.py`, disasm32 from exports and the entry point:

| Module | Functions | Coverage of `.text` |
|---|---:|---:|
| `UnrealTournament.exe` (unwrapped) | 2,062 | 74.1% |
| `UCC.exe` | 872 | 65.0% |
| `Core.dll` | 7,495 | 75.6% |
| `Engine.dll` | 14,547 | 76.8% |
| `Window.dll` | 3,650 | 73.3% |
| `WinDrv.dll` | 618 | 90.5% |
| `Render.dll` | 1,164 | 74.3% |
| `OpenGlDrv.dll` | 404 | 65.1% |
| `Galaxy.dll` | 1,116 | 73.1% |
| `Fire.dll` | 565 | 95.2% |
| `IpDrv.dll` | 2,159 | 71.4% |
| `UWeb.dll` | 348 | 91.3% |
