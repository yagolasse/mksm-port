#!/usr/bin/env bash
# Run the recompiled exe against an extracted game folder and capture its output.
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
#      RECOMP_APU_DSP_ACK defaults to the GP DSP command doorbell DirectSound
#      waits on during init (DSP scratch + 0x810). The address comes from a
#      contiguous allocation; if audio init hangs again, re-check it with
#      RECOMP_WATCHDOG_SECS (ebx in the dump) and override it here.
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
export RECOMP_APU_DSP_ACK="${RECOMP_APU_DSP_ACK-0x83550810}"

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
