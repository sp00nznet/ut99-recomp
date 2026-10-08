# One-click setup for the Unreal Tournament static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's "Step by
# step" is the same thing by hand, command for command.
#
# It copies your install into game\ (UT writes its ini and logs there, so your
# Steam copy is left alone), unwraps and rebases the modules into work\, builds
# the function catalogs, lifts them to C, builds the 32-bit host and leaves a
# launcher. Each step is skipped when its output already exists (-Force redoes
# them). Everything it does goes to setup.log. Nothing is downloaded except the
# tools it asks about; nothing from the game leaves this machine.
param([switch]$Force, [string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path (Split-Path -Parent $Root) 'tools' }
$T = Join-Path $Toolkit 'tools'
$Log = Join-Path $Root 'setup.log'
# Until pcrecomp #52-#59 and #61 merge, the toolkit is this branch: main with them merged.
$Branch = 'ut99-integration'
Set-Location $Root
# A log line that cannot be written (the log open in another program) is
# not a reason to stop the setup.
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/8] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Ask($q) { $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}

Clear-Host
Say "Unreal Tournament (1999) - static recompilation setup" 'White'
Say "You need your installed copy of Unreal Tournament (Steam) and about 3 GB free."
Say "The function catalogs, the lift and the build take 40 to 80 minutes, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the pipeline needs"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"

if ((Exec ($pyargs + @('-c', 'import pefile, capstone, cryptography'))) -ne 0) {
  Say "  The Python packages pefile, capstone and cryptography are missing (about 25 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile, capstone and cryptography are required." }
  Run "Installing pefile, capstone and cryptography" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone', 'cryptography'))
}

# The toolkit, with what this game needs from it: SteamStub 2.0, guest C++
# exceptions, the x87 CRT helpers, MMX byte ops (pcrecomp #52-#61).
function Toolkit-Ready {
  $need = @(@('tools\drm\steamstub.py', 'unwrap_v20'), @('runtime\native32\eh32.c', ''),
            @('runtime\native32\x87crt.c', ''), @('runtime\recomp32\recomp_types.h', 'mmx_paddusb'),
            @('runtime\native32\native32.c', 'native32_lift_in_native'))
  foreach ($n in $need) {
    $f = Join-Path $Toolkit $n[0]
    if (-not (Test-Path $f)) { return $false }
    if ($n[1] -and -not (Select-String -Path $f -SimpleMatch $n[1] -Quiet)) { return $false }
  }
  return $true
}
if (-not (Test-Path (Join-Path $T 'lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp ($Branch)" @('git', 'clone', '--depth', '1', '--branch', $Branch, 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
if (-not (Toolkit-Ready)) {
  Say "  Your pcrecomp at $Toolkit lacks changes this game needs (pcrecomp #52-#61, not merged yet)."
  if (-not (Ask "  Switch that checkout to its '$Branch' branch, which has them?")) {
    Fail "pcrecomp needs the '$Branch' branch: git -C `"$Toolkit`" fetch origin $Branch, then check it out."
  }
  Run "Fetching $Branch" @('git', '-C', $Toolkit, 'fetch', 'origin', "${Branch}:${Branch}")
  Run "Checking out $Branch" @('git', '-C', $Toolkit, 'checkout', $Branch)
  if (-not (Toolkit-Ready)) { Fail "pcrecomp's $Branch branch still lacks what this game needs; see the README, Building from source." }
}
Say "  pcrecomp: $Toolkit"

# The compiler. build.cmd finds Visual Studio itself; this only checks that it
# is there, with CMake and Ninja. Visual Studio is several GB with an installer
# of its own, so this says what to install rather than installing it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) { Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again." }
Say "  Visual Studio: $vs"
# $tool, not $t: PowerShell names are case-insensitive, and $T is the toolkit path.
foreach ($tool in @(@('cmake', 'Kitware.CMake', '30 MB'), @('ninja', 'Ninja-build.Ninja', '1 MB'))) {
  if (Get-Command $tool[0] -ErrorAction SilentlyContinue) { continue }
  Say "  $($tool[0]) is not installed."
  if (-not (Ask "  Install $($tool[0]) now (winget, about $($tool[2]))?")) { Fail "$($tool[0]) is required." }
  Exec @('winget', 'install', '-e', '--id', $tool[1], '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  if (-not (Get-Command $tool[0] -ErrorAction SilentlyContinue)) { Fail "$($tool[0]) installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}

# ---------------------------------------------------------------- the game
Step 2 "Copying your Unreal Tournament into game\ (about 600 MB)"
function Is-Install([string]$d) {
  return ($d -and (Test-Path (Join-Path $d 'System\UnrealTournament.exe')) -and (Test-Path (Join-Path $d 'System\Engine.dll')))
}
function Find-Steam {
  $roots = @()
  foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
    try { $roots += (Get-ItemProperty $k -ErrorAction Stop).SteamPath } catch {}
    try { $roots += (Get-ItemProperty $k -ErrorAction Stop).InstallPath } catch {}
  }
  foreach ($r in ($roots | Where-Object { $_ } | Select-Object -Unique)) {
    $libs = @($r)
    $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
      $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' })
    }
    foreach ($l in $libs) {
      $d = Join-Path $l 'steamapps\common\Unreal Tournament'
      if (Is-Install $d) { return $d }
    }
  }
  return ""
}

if ((Is-Install 'game') -and -not $Force) {
  Say "  game\ is already there (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not (Is-Install $Game)) {
    $Game = Find-Steam
    if ($Game) { Say "  Found it in your Steam library: $Game" }
  }
  while (-not (Is-Install $Game)) {
    $Game = (Read-Host "  Paste the folder Unreal Tournament is installed in (the one with System\UnrealTournament.exe)").Trim('"', ' ')
    if (-not (Is-Install $Game)) { Say "  No System\UnrealTournament.exe and System\Engine.dll in that folder." 'Yellow' }
  }
  # A copy, not a link: the game writes its ini, logs and saves under System\,
  # and the ini is changed below. robocopy's exit codes below 8 are success.
  Say "  Copying from $Game..."
  $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/XD', 'directx9c', '/NFL', '/NDL', '/NJH', '/NP')
  if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }
}

# The shipped ini starts fullscreen through DirectDraw, which fails on Windows
# 11 (the original faults on it), and opens the first-run wizard, which the
# recompiled game skips for now (ROADMAP).
$ini = 'game\System\UnrealTournament.ini'
if (Test-Path $ini) {
  $text = (Get-Content $ini) -replace '^FirstRun=.*', 'FirstRun=436' -replace '^StartupFullscreen=.*', 'StartupFullscreen=False' -replace '^UseDirectDraw=.*', 'UseDirectDraw=False'
  Set-Content -Path $ini -Value $text -Encoding ASCII
  Say "  game\System\UnrealTournament.ini: windowed, no DirectDraw, first-run wizard skipped."
}

# ---------------------------------------------------------------- unwrap and rebase
Step 3 "Unwrapping the launcher and placing every module (seconds)"
if ((Test-Path 'work\rebased\UnrealTournament.exe') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:PCRECOMP = $Toolkit
  Run "Unwrapping and rebasing (tools\rebase.py)" ($pyargs + @('tools\rebase.py'))
}

# ---------------------------------------------------------------- catalog
Step 4 "Finding every function in 14 modules (about 20 minutes, once)"
if ((Test-Path 'work\UnrealTournament.exe.functions.json') -and (Test-Path 'work\Engine.dll.functions.json') -and -not $Force) {
  Say "  Already done (skipping)."
} else {
  $env:PCRECOMP = $Toolkit
  Run "Disassembling (tools\catalog.py)" ($pyargs + @('tools\catalog.py'))
}

# ---------------------------------------------------------------- lift
Step 5 "Lifting the game to C (about 3 minutes)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:PCRECOMP = $Toolkit
  Run "Lifting (run_lift.py)" ($pyargs + @('run_lift.py'))
}

# ---------------------------------------------------------------- build
Step 6 "Building build\ut99.exe (15 to 40 minutes)"
if ((Test-Path 'build\ut99.exe') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:CMAKE_ARGS = "-DPCRECOMP=$Toolkit"     # build.cmd passes it to the configure step
  Run "Compiling" @('cmd', '/c', (Join-Path $Root 'build.cmd'))
}

# ---------------------------------------------------------------- check
Step 7 "Checking that every module maps and binds (seconds)"
$old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$out = (& (Join-Path $Root 'build\ut99.exe') 2>&1 | Out-String)
$ErrorActionPreference = $old
Log $out
$binds = ([regex]::Matches($out, '\[bind\][^\r\n]*')).Value
if (-not $binds) { Fail "build\ut99.exe printed no [bind] lines." }
if ($binds | Where-Object { $_ -notmatch ' 0 unresolved' }) { Fail "a module has unresolved imports: $($binds -join '; ')" }
Say "  $($binds.Count) modules bound, 0 unresolved."

# ---------------------------------------------------------------- launcher
Step 8 "Making the launcher"
# Its own ini pair: UT's OpenGL renderer, a widescreen window, Hor+ FOV. The
# game saves its settings back into them, so they are made once (-Force remakes).
if ((Test-Path 'game\System\UTRecomp.ini') -and -not $Force) { Say "  game\System\UTRecomp.ini kept (your settings)." }
else { Run "Writing UTRecomp.ini and UTRecompUser.ini" ($pyargs + @('tools\hd_ini.py')) }
$lnk = Join-Path $Root 'Unreal Tournament (recomp).cmd'
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  'rem Runs the recompiled game, windowed, on its own ini pair (tools\hd_ini.py).',
  'rem A crashed run leaves System\Running.ini and the next start offers Recovery',
  'rem Mode; that file is removed first.',
  'cd /d "%~dp0"',
  'del /q game\System\Running.ini 2>nul',
  'start "" build\ut99.exe --run --msaa 8 --aniso 16 -- INI=UTRecomp.ini USERINI=UTRecompUser.ini')
Say "  $lnk"

Write-Host ""
Say "Done. Double-click 'Unreal Tournament (recomp).cmd' to run it." 'Green'
Read-Host "Press Enter to close" | Out-Null
