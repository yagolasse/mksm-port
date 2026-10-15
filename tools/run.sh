#!/usr/bin/env bash
# Run the recompiled exe against an extracted game folder and capture its output.
# tools/run.bat is the Windows (cmd) twin: keep the defaults of the two scripts in sync.
# tools/smoke_test.py (RUN_ENV) uses the same defaults too, except the two display ones
# (RECOMP_PB_GPU, RECOMP_FB_WINDOW): it runs headless on the software executor unless --gpu.
#
# The exe is a GUI-subsystem program, so its printf/stderr output only goes
# somewhere when redirected; this script pipes both into a log file.
#
# Usage: tools/run.sh [extra exe args...]
#
# Env: MKSM_GAME_DIR (default ../game_files): extracted disc root with default.xbe
#      MKSM_EXE      (default build/Release/mksm_recomp.exe)
#      MKSM_LOG      (default ../logs/run_<timestamp>.log)
#      Toolkit vars such as RECOMP_WATCHDOG_SECS / RECOMP_AC97_READY pass through.
#      RECOMP_PB_EXEC defaults to 1: boot needs the pushbuffer executor, since
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
#      RECOMP_HEAP_RECLAIM defaults to 1 (0 turns it off): freed contiguous
#      and heap blocks are reused. Without it the contiguous arena runs out
#      by the second OPENING.SFD and the movie after New Game stays black.
#      RECOMP_TITLE_KEVENTS defaults to 1 (0 turns it off): events the game
#      builds by hand get a host event, so waits on them block instead of
#      failing at once (the XDK timer thread needs it). RECOMP_TITLE_KEVENTS_SKIP
#      defaults to D3D's event 0x003F29CC, which a GPU interrupt sets that the
#      runtime doesn't model: waiting on it for real hangs the boot.
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
RECOMP_AC97_READY="${RECOMP_AC97_READY-1}"
# The runtime only checks that the variable exists, so "off" means unset.
if [ -z "$RECOMP_AC97_READY" ] || [ "$RECOMP_AC97_READY" = 0 ]; then
    unset RECOMP_AC97_READY
else
    export RECOMP_AC97_READY
fi
RECOMP_USB="${RECOMP_USB-1}"
if [ -z "$RECOMP_USB" ] || [ "$RECOMP_USB" = 0 ]; then
    unset RECOMP_USB
else
    export RECOMP_USB
fi
export RECOMP_KEYBOARD="${RECOMP_KEYBOARD-1}"
export RECOMP_APU_DSP_ACK="${RECOMP_APU_DSP_ACK-auto}"
RECOMP_HEAP_RECLAIM="${RECOMP_HEAP_RECLAIM-1}"
if [ -z "$RECOMP_HEAP_RECLAIM" ] || [ "$RECOMP_HEAP_RECLAIM" = 0 ]; then
    unset RECOMP_HEAP_RECLAIM
else
    export RECOMP_HEAP_RECLAIM
fi
export RECOMP_TITLE_KEVENTS="${RECOMP_TITLE_KEVENTS-1}"
export RECOMP_TITLE_KEVENTS_SKIP="${RECOMP_TITLE_KEVENTS_SKIP-0x003F29CC}"
export RECOMP_PB_GPU="${RECOMP_PB_GPU-1}"
RECOMP_FB_WINDOW="${RECOMP_FB_WINDOW-1}"
if [ -z "$RECOMP_FB_WINDOW" ] || [ "$RECOMP_FB_WINDOW" = 0 ]; then
    unset RECOMP_FB_WINDOW
else
    export RECOMP_FB_WINDOW
fi

mkdir -p "$(dirname "$LOG")"
LOG="$(cd "$(dirname "$LOG")" && pwd)/$(basename "$LOG")"
GAME_DIR_W="$(cygpath -w "$(cd "$GAME_DIR" && pwd)")"
echo "exe:  $EXE"
echo "game: $GAME_DIR_W"
echo "log:  $LOG"

# Run from the exe's folder so relative paths the runtime may create stay there.
cd "$(dirname "$EXE")"
"./$(basename "$EXE")" "$GAME_DIR_W" "$@" 2>&1 | tee "$LOG"
status=${PIPESTATUS[0]}
echo "exit code: $status (0x$(printf '%08X' "$status"))" | tee -a "$LOG"
exit "$status"
