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
