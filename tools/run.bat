@echo off
rem Run the recompiled exe against an extracted game folder and capture its output.
rem Windows (cmd) twin of tools/run.sh: keep the defaults of the two scripts in sync.
rem tools/smoke_test.py (RUN_ENV) uses the same defaults too, except the two display ones
rem (RECOMP_PB_GPU, RECOMP_FB_WINDOW): it runs headless on the software executor unless --gpu.
rem
rem The exe is a GUI-subsystem program, so its printf/stderr output only goes
rem somewhere when redirected; this script writes both into a log file and
rem prints the tail of it when the game exits. For a live view, run in another
rem window:  powershell Get-Content -Wait <log>
rem
rem Usage: tools\run.bat [extra exe args...]
rem
rem Env: MKSM_GAME_DIR (default ..\game_files): extracted disc root with default.xbe
rem      MKSM_EXE      (default build\Release\mksm_recomp.exe)
rem      MKSM_LOG      (default ..\logs\run_<timestamp>.log)
rem      Toolkit vars such as RECOMP_WATCHDOG_SECS pass through.
rem      RECOMP_PB_EXEC defaults to 1: boot needs the pushbuffer executor, since
rem      D3D init waits for the GPU to report progress.
rem      RECOMP_AC97_READY defaults to 1 (0 turns it off; cmd can't tell empty
rem      from unset): reports the AC'97 codec and brings up the emulated APU.
rem      Without it DirectSoundCreate fails and the game later spins on a NULL
rem      IDirectSound.
rem      RECOMP_USB defaults to 1 (0 turns it off): the emulated USB host
rem      controllers the game's XAPI finds its gamepads on; a host XInput pad
rem      drives the first one.
rem      RECOMP_KEYBOARD defaults to 1: keyboard on top of pad 1, read from the
rem      framebuffer window (RECOMP_FB_WINDOW=1) while it has the focus.
rem      RECOMP_APU_DSP_ACK defaults to auto: acks the GP DSP command doorbell
rem      DirectSound init waits on, found from the APU's GP scratch table
rem      (scratch page 0 + 0x810, logged as "DSP doorbell auto"). A fixed
rem      address also works but moves with the allocation order. If audio init
rem      hangs, compare ebx in a RECOMP_WATCHDOG_SECS dump with the logged one.
rem      RECOMP_HEAP_RECLAIM defaults to 1 (0 turns it off): freed contiguous
rem      and heap blocks are reused. Without it the contiguous arena runs out
rem      by the second OPENING.SFD and the movie after New Game stays black.
rem      RECOMP_TITLE_KEVENTS defaults to 1 (0 turns it off): events the game
rem      builds by hand get a host event, so waits on them block instead of
rem      failing at once (the XDK timer thread needs it). RECOMP_TITLE_KEVENTS_SKIP
rem      defaults to D3D's event 0x003F29CC, which a GPU interrupt sets that the
rem      runtime doesn't model: waiting on it for real hangs the boot.
rem      RECOMP_PB_GPU defaults to 1 (0 = the software executor): xemu's PGRAPH on
rem      OpenGL draws the frames (movies included).
rem      RECOMP_FB_WINDOW defaults to 1 (0 turns it off): the game window.
setlocal

set "PORT=%~dp0.."
for %%I in ("%PORT%") do set "PORT=%%~fI"

if defined MKSM_GAME_DIR (set "GAME_DIR=%MKSM_GAME_DIR%") else (set "GAME_DIR=%PORT%\..\game_files")
if defined MKSM_EXE (set "EXE=%MKSM_EXE%") else (set "EXE=%PORT%\build\Release\mksm_recomp.exe")
if defined MKSM_LOG (
    set "LOG=%MKSM_LOG%"
) else (
    rem %DATE%/%TIME% are locale-dependent, so ask PowerShell for the timestamp.
    for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "LOG=%PORT%\..\logs\run_%%T.log"
)

for %%I in ("%GAME_DIR%") do set "GAME_DIR=%%~fI"
for %%I in ("%EXE%") do set "EXE=%%~fI"
for %%I in ("%LOG%") do set "LOG=%%~fI"

if not exist "%GAME_DIR%\default.xbe" (
    echo no default.xbe in %GAME_DIR% ^(set MKSM_GAME_DIR^) 1>&2
    exit /b 1
)
if not exist "%EXE%" (
    echo exe not found: %EXE% ^(build first, or set MKSM_EXE^) 1>&2
    exit /b 1
)

if not defined RECOMP_PB_EXEC set "RECOMP_PB_EXEC=1"
if not defined RECOMP_AC97_READY set "RECOMP_AC97_READY=1"
rem The runtime only checks that the variable exists, so "off" means unset.
if "%RECOMP_AC97_READY%"=="0" set "RECOMP_AC97_READY="
if not defined RECOMP_USB set "RECOMP_USB=1"
if "%RECOMP_USB%"=="0" set "RECOMP_USB="
if not defined RECOMP_KEYBOARD set "RECOMP_KEYBOARD=1"
if not defined RECOMP_APU_DSP_ACK set "RECOMP_APU_DSP_ACK=auto"
if not defined RECOMP_HEAP_RECLAIM set "RECOMP_HEAP_RECLAIM=1"
if "%RECOMP_HEAP_RECLAIM%"=="0" set "RECOMP_HEAP_RECLAIM="
if not defined RECOMP_TITLE_KEVENTS set "RECOMP_TITLE_KEVENTS=1"
if not defined RECOMP_TITLE_KEVENTS_SKIP set "RECOMP_TITLE_KEVENTS_SKIP=0x003F29CC"
if not defined RECOMP_PB_GPU set "RECOMP_PB_GPU=1"
if not defined RECOMP_FB_WINDOW set "RECOMP_FB_WINDOW=1"
if "%RECOMP_FB_WINDOW%"=="0" set "RECOMP_FB_WINDOW="

for %%I in ("%LOG%") do if not exist "%%~dpI" mkdir "%%~dpI"
echo exe:  %EXE%
echo game: %GAME_DIR%
echo log:  %LOG%

rem Run from the exe's folder so relative paths the runtime may create stay there.
for %%I in ("%EXE%") do pushd "%%~dpI"
"%EXE%" "%GAME_DIR%" %* > "%LOG%" 2>&1
set "STATUS=%ERRORLEVEL%"
popd

powershell -NoProfile -Command "Get-Content -Tail 40 -LiteralPath $env:LOG"
rem %=ExitCode% is the last exit code as 8 hex digits.
cmd /c exit %STATUS%
echo exit code: %STATUS% (0x%=ExitCode%)
>> "%LOG%" echo exit code: %STATUS% (0x%=ExitCode%)
exit /b %STATUS%
