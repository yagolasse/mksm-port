#!/usr/bin/env bash
# Regenerate patches/xboxrecomp-NNNN-*.patch from the toolkit submodule's
# commits on top of upstream, then check that the series applies on a clean
# upstream checkout and reproduces the submodule's HEAD exactly.
#
# Usage (from anywhere; run it from bash, not PowerShell):
#     tools/regen_patches.sh [slug ...]
#
# Commit i (oldest first) keeps the file name of the i-th existing patch. Each
# commit past the existing files needs a slug argument, in order, naming its
# new file (xboxrecomp-NNNN-<slug>.patch). Files are written by git itself, so
# they are UTF-8 + LF; PowerShell's `>` writes UTF-16 + CRLF, which git am
# rejects ("Patch format detection failed").
#
# Exit 0 = patches written and verified. It does not stage or commit anything.

set -euo pipefail

PORT="$(cd "$(dirname "$0")/.." && pwd)"
SUB="$PORT/external/xboxrecomp"
PATCHES="$PORT/patches"
UPSTREAM="${UPSTREAM:-origin/main}"

base="$(git -C "$SUB" merge-base HEAD "$UPSTREAM")"
mapfile -t commits < <(git -C "$SUB" rev-list --reverse "$base..HEAD")
mapfile -t existing < <(ls "$PATCHES"/xboxrecomp-[0-9][0-9][0-9][0-9]-*.patch 2>/dev/null | sort)

if [ "${#commits[@]}" -lt "${#existing[@]}" ]; then
    echo "error: ${#commits[@]} commits on top of $UPSTREAM but ${#existing[@]} patch files;" >&2
    echo "       remove the stale files by hand first" >&2
    exit 1
fi
need=$(( ${#commits[@]} - ${#existing[@]} ))
if [ "$#" -ne "$need" ]; then
    echo "error: $need new commit(s) need $need slug argument(s), got $#" >&2
    for c in "${commits[@]:${#existing[@]}}"; do
        echo "       new: $(git -C "$SUB" log -1 --format='%h %s' "$c")" >&2
    done
    exit 1
fi

slugs=("$@")
for i in "${!commits[@]}"; do
    if [ "$i" -lt "${#existing[@]}" ]; then
        out="${existing[$i]}"
    else
        out="$PATCHES/$(printf 'xboxrecomp-%04d-%s.patch' $((i + 1)) "${slugs[$((i - ${#existing[@]}))]}")"
    fi
    git -C "$SUB" format-patch -1 "${commits[$i]}" --stdout > "$out"
    echo "wrote $(basename "$out")  <- $(git -C "$SUB" log -1 --format='%h %s' "${commits[$i]}")"
done

# Encoding check: no UTF-16 BOM, no NUL bytes, no CR.
bad=0
for f in "$PATCHES"/xboxrecomp-*.patch; do
    if head -c 2 "$f" | od -An -tx1 | grep -qE 'ff fe|fe ff'; then echo "BAD (UTF-16 BOM): $f"; bad=1; fi
    if ! tr -d '\000' < "$f" | cmp -s - "$f"; then echo "BAD (NUL bytes): $f"; bad=1; fi
    if grep -q $'\r' "$f"; then echo "BAD (CRLF): $f"; bad=1; fi
done
[ "$bad" -eq 0 ] || exit 1

# Apply the series on a clean upstream checkout and compare with HEAD.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git clone -q "$SUB" "$tmp/x"
git -C "$tmp/x" checkout -q "$base"
git -C "$tmp/x" -c user.name=regen -c user.email=regen@localhost am -q "$PATCHES"/xboxrecomp-*.patch
if git -C "$tmp/x" diff --quiet "$(git -C "$SUB" rev-parse HEAD)" HEAD; then
    echo "OK: ${#commits[@]} patches apply on $UPSTREAM ($(git -C "$SUB" log -1 --format=%h "$base")) and reproduce $(git -C "$SUB" log -1 --format=%h HEAD)"
else
    echo "FAIL: the applied series differs from the submodule HEAD:" >&2
    git -C "$tmp/x" diff --stat "$(git -C "$SUB" rev-parse HEAD)" HEAD >&2
    exit 1
fi
