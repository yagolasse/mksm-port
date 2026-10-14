@echo off
rem Run the recompiled exe against an extracted game folder and capture its output.
rem Windows (cmd) twin of tools/run.sh: keep the defaults of the two scripts in sync.
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
rem      RECOMP_APU_DSP_ACK defaults to the GP DSP command doorbell DirectSound
rem      waits on during init (DSP scratch + 0x810). The address comes from a
rem      contiguous allocation; if audio init hangs again, re-check it with
rem      RECOMP_WATCHDOG_SECS (ebx in the dump) and override it here.
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
if not defined RECOMP_APU_DSP_ACK set "RECOMP_APU_DSP_ACK=0x83550810"

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
