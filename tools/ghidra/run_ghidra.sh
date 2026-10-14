#!/usr/bin/env bash
# Headless Ghidra analysis + name export for default.xbe.
#
# Same as xboxrecomp's tools/ghidra_naming/run_ghidra.sh, but runs our Java
# ExportXbeNames (Ghidra 12 has no Jython) and keeps the Ghidra project outside
# the repo so it can also be opened in the Ghidra GUI afterwards.
#
# Usage: tools/ghidra/run_ghidra.sh            # import + analyze + export
#        IMPORT=0 tools/ghidra/run_ghidra.sh   # re-export from the saved project
#
# Env: GHIDRA_HOME (default /c/ghidra_12.1.4_PUBLIC)
#      XBE         (default ../game_files/default.xbe)
#      GHIDRA_WORK (default ../ghidra, holds the project + flat image)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$HERE/../.." && pwd)"
TOOLKIT="$PORT/external/xboxrecomp/tools/ghidra_naming"

GHIDRA_HOME="${GHIDRA_HOME:-/c/ghidra_12.1.4_PUBLIC}"
XBE="${XBE:-$PORT/../game_files/default.xbe}"
WORK="${GHIDRA_WORK:-$PORT/../ghidra}"
EXPORT_DIR="$TOOLKIT/export"    # where merge_names.py looks by default
IMPORT="${IMPORT:-1}"

mkdir -p "$WORK/project" "$WORK/scripts" "$EXPORT_DIR"
w() { cygpath -w "$1"; }
# One script dir: a ';'-joined -scriptPath gets split apart by analyzeHeadless.bat.
cp "$HERE/ExportXbeNames.java" "$TOOLKIT/ghidra_scripts/SetAnalysisOptions.java" "$WORK/scripts/"
SCRIPTS="$(w "$WORK/scripts")"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless.bat"

if [ "$IMPORT" = "1" ]; then
    python "$TOOLKIT/extract_for_ghidra.py" "$XBE" --out-dir "$WORK"
    "$HEADLESS" "$(w "$WORK/project")" mksm \
        -import "$(w "$WORK/xbe_flat.bin")" \
        -loader BinaryLoader -loader-baseAddr 0x10000 \
        -processor "x86:LE:32:default" -cspec windows \
        -overwrite \
        -scriptPath "$SCRIPTS" \
        -preScript SetAnalysisOptions.java \
        -postScript ExportXbeNames.java "$(w "$EXPORT_DIR")" < /dev/null
else
    "$HEADLESS" "$(w "$WORK/project")" mksm \
        -process xbe_flat.bin -noanalysis \
        -scriptPath "$SCRIPTS" \
        -postScript ExportXbeNames.java "$(w "$EXPORT_DIR")" < /dev/null
fi
ls -la "$EXPORT_DIR"
