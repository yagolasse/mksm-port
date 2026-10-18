#!/usr/bin/env bash
# Run the recompiled exe against an extracted game folder and capture its output.
# tools/run.bat is the Windows (cmd) twin: keep the defaults of the two scripts in sync.
# tools/smoke_test.py (RUN_ENV) uses the same defaults too, except RECOMP_FB_WINDOW=0
# (headless).
#
# The exe is a GUI-subsystem program, so its printf/stderr output only goes
# somewhere when redirected; this script pipes both into a log file (started
# without a redirect it writes %LOCALAPPDATA%\mksm_recomp\last_run.log).
# The exe has the defaults below built in (plan B10a, applied where a variable
# is unset); the script still sets them so a log shows what the run used.
#
# Usage: tools/run.sh [extra exe args...]
#
# Env: MKSM_GAME_DIR (default ../game_files): extracted disc root with default.xbe
#      MKSM_EXE      (default build/Release/mksm_recomp.exe; on Linux the build dir's
#                    mksm_recomp)
#      MKSM_LOG      (default ../logs/run_<timestamp>.log)
#      MKSM_SAVE_DIR (default %LOCALAPPDATA%\mksm_recomp\saves): the saves
#                    (UDATA/TDATA); older saves in <game>\UDATA or directly in
#                    mksm_recomp\ move there once. The emulated hard disk and
#                    settings.ini stay in %LOCALAPPDATA%\mksm_recomp.
#      RECOMP_SETTINGS defaults to 0: dev runs ignore the player's settings.ini
#                    (no file = the Xbox look); 1 = use it, <path> = that file.
#      Toolkit vars such as RECOMP_WATCHDOG_SECS / RECOMP_AC97_READY pass through.
#      RECOMP_PB_EXEC defaults to 1 (0 turns it off): boot needs the pushbuffer executor, since
#      D3D init waits for the GPU to report progress.
#      RECOMP_AC97_READY defaults to 1 (0 or empty turns it off): reports the
#      AC'97 codec and brings up the emulated APU. Without it DirectSoundCreate
#      fails and the game later spins on a NULL IDirectSound.
#      RECOMP_USB defaults to 1 (0 or empty turns it off): the emulated USB
#      host controllers the game's XAPI finds its gamepads on; a host XInput
#      pad drives the first one.
#      RECOMP_KEYBOARD defaults to 1: keyboard on top of pad 1, read from the
#      framebuffer window (RECOMP_FB_WINDOW=1) while it has the focus.
#      RECOMP_APU_DSP_ACK defaults to auto: acks the GP DSP command doorbell
#      DirectSound init waits on, found from the APU's GP scratch table
#      (scratch page 0 + 0x810, logged as "DSP doorbell auto"). A fixed
#      address also works but moves with the allocation order. If audio init
#      hangs, compare ebx in a RECOMP_WATCHDOG_SECS dump with the logged one.
#      RECOMP_APU_EP_ALIVE defaults to 0xFE85A018: that EP DSP word reads 0xCCCCCC
#      ("EP program running"), else DirectSound resets the DSPs and busy-waits
#      10 ms about 20 times a second (plan 2.7a6). Empty turns it off.
#      RECOMP_HEAP_RECLAIM defaults to 1 (0 turns it off): freed contiguous
#      and heap blocks are reused. Without it the contiguous arena runs out
#      by the second OPENING.SFD and the movie after New Game stays black.
#      RECOMP_TITLE_KEVENTS defaults to 1 (0 turns it off): events the game
#      builds by hand get a host event, so waits on them block instead of
#      failing at once (the XDK timer thread needs it). RECOMP_TITLE_KEVENTS_SKIP
#      (empty by default) lists events that keep failing at once; D3D's VBlank
#      event 0x003F29CC was one until main.c's vblank thread set it (plan
#      2.7a6): RECOMP_TITLE_KEVENTS_SKIP=0x003F29CC is the old behaviour.
#      RECOMP_PB_GPU defaults to 1 (0 = the software executor): xemu's PGRAPH on
#      OpenGL draws the frames (movies included).
#      RECOMP_FB_WINDOW defaults to 1 (0 or empty turns it off): the game window.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$HERE/.." && pwd)"

GAME_DIR="${MKSM_GAME_DIR:-$PORT/../game_files}"
EXE="${MKSM_EXE:-$PORT/build/Release/mksm_recomp.exe}"
LOG="${MKSM_LOG:-$PORT/../logs/run_$(date +%Y%m%d_%H%M%S).log}"

if [ ! -f "$GAME_DIR/default.xbe" ]; then
    echo "no default.xbe in $GAME_DIR (set MKSM_GAME_DIR)" >&2
    exit 1
fi
if [ ! -f "$EXE" ]; then
    echo "exe not found: $EXE (build first, or set MKSM_EXE)" >&2
    exit 1
fi

export RECOMP_PB_EXEC="${RECOMP_PB_EXEC-1}"
# RECOMP_AC97_READY, _USB, _HEAP_RECLAIM, _FB_WINDOW, _PB_EXEC: the toolkit only checks that
# they exist; the exe turns 0 into "not there" and fills an unset one with its default (on,
# plan B10a), so "off" is passed as 0 (empty becomes 0 here: an empty variable may not reach it).
RECOMP_AC97_READY="${RECOMP_AC97_READY-1}"
[ -n "$RECOMP_AC97_READY" ] || RECOMP_AC97_READY=0
export RECOMP_AC97_READY
RECOMP_USB="${RECOMP_USB-1}"
[ -n "$RECOMP_USB" ] || RECOMP_USB=0
export RECOMP_USB
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD-1}"
export RECOMP_APU_DSP_ACK="${RECOMP_APU_DSP_ACK-auto}"
export RECOMP_APU_EP_ALIVE="${RECOMP_APU_EP_ALIVE-0xFE85A018}"
# The DMA-engine ring walk (toolkit v0.14+); the default segment scan loses
# the tail of each ring lap (plan H.5a).
export RECOMP_PB_WALK="${RECOMP_PB_WALK-dma}"
# 8 x 512-sample device buffers (85 ms); the toolkit's 4 (43 ms) ran dry
# several times a second in movies and the level (plan H.5a).
export RECOMP_AUDIO_BUF_COUNT="${RECOMP_AUDIO_BUF_COUNT-8}"
RECOMP_HEAP_RECLAIM="${RECOMP_HEAP_RECLAIM-1}"
[ -n "$RECOMP_HEAP_RECLAIM" ] || RECOMP_HEAP_RECLAIM=0
export RECOMP_HEAP_RECLAIM
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS-1}"
export RECOMP_TITLE_KEVENTS_SKIP="${RECOMP_TITLE_KEVENTS_SKIP-}"
export RECOMP_PB_GPU="${RECOMP_PB_GPU-1}"
export RECOMP_SETTINGS="${RECOMP_SETTINGS-0}"
RECOMP_FB_WINDOW="${RECOMP_FB_WINDOW-1}"
[ -n "$RECOMP_FB_WINDOW" ] || RECOMP_FB_WINDOW=0
export RECOMP_FB_WINDOW

mkdir -p "$(dirname "$LOG")"
LOG="$(cd "$(dirname "$LOG")" && pwd)/$(basename "$LOG")"
GAME_DIR_W="$(cd "$GAME_DIR" && pwd)"
# Git Bash on Windows: the exe takes a Windows path. Linux: the path as is.
if command -v cygpath >/dev/null 2>&1; then GAME_DIR_W="$(cygpath -w "$GAME_DIR_W")"; fi
echo "exe:  $EXE"
echo "game: $GAME_DIR_W"
echo "log:  $LOG"

# Run from the exe's folder so relative paths the runtime may create stay there.
cd "$(dirname "$EXE")"
"./$(basename "$EXE")" "$GAME_DIR_W" "$@" 2>&1 | tee "$LOG"
status=${PIPESTATUS[0]}
echo "exit code: $status (0x$(printf '%08X' "$status"))" | tee -a "$LOG"
exit "$status"
