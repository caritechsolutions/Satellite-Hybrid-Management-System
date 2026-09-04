#!/bin/bash
#
# Regenerate librist/RISTSTB-SYNC — the record of which riststb commit this
# repo's librist/ copy is supposed to match.
#
#   ./librist-sync-update.sh /path/to/riststb-checkout
#
# Run this ON A MACHINE THAT HAS BOTH TREES, after mirroring a change between
# them. install.sh then verifies this repo's copy against the manifest at
# install time, where riststb is not available.
#
# What it does, in order:
#
#   1. Diffs every MIRRORED file (the set that must stay byte-identical between
#      the two trees) and refuses to write a manifest if any of them differ.
#      A manifest is a claim that the trees agree; generating one while they do
#      not would launder the divergence into a checked-in hash.
#   2. Reports — but does not block on — the files that are shared-but-drifted
#      (rist-common.c, udp.c, rist.c). Those carry per-tree work and cannot be
#      hashed whole; the manifest covers them with content assertions instead.
#   3. Writes the manifest: riststb commit, sha256 per mirrored file, and the
#      assertions.
#
set -e

RISTSTB="${1:-}"
[ -n "$RISTSTB" ] || { echo "usage: $0 <path-to-riststb-checkout>" >&2; exit 1; }
[ -d "$RISTSTB/librist/src" ] || { echo "no librist/src under $RISTSTB" >&2; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/librist/RISTSTB-SYNC"

# Byte-identical in both trees. Every file the Part 8 work touched lives here.
MIRRORED="
src/pcr_cut.c
src/pcr_cut.h
src/program-selection.c
src/program-selection.h
include/librist/headers.h
include/librist/urlparam.h
tools/ristsender.c
"

# Shared code that still carries per-tree work, so it cannot be hashed whole.
DRIFTED="src/rist-common.c src/udp.c src/rist.c"

fail=0
for f in $MIRRORED; do
    if ! diff -q "$RISTSTB/librist/$f" "$HERE/librist/$f" >/dev/null 2>&1; then
        echo "DIVERGED (must be mirrored): $f" >&2
        fail=1
    fi
done
if [ "$fail" -ne 0 ]; then
    echo >&2
    echo "Refusing to write a manifest while mirrored files differ." >&2
    echo "Mirror the change in both directions first, then re-run." >&2
    exit 1
fi

echo "mirrored files: all identical"
for f in $DRIFTED; do
    n=$(diff "$RISTSTB/librist/$f" "$HERE/librist/$f" 2>/dev/null | grep -c '^[<>]' || true)
    echo "drifted (assertions only): $f — ${n} differing lines"
done

SHA=$(git -C "$RISTSTB" rev-parse HEAD)
BR=$(git -C "$RISTSTB" rev-parse --abbrev-ref HEAD)

{
    echo "# librist sync manifest — this repo's librist/ vs the riststb tree."
    echo "#"
    echo "# riststb commit : ${SHA}"
    echo "# riststb branch : ${BR}"
    echo "# generated      : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "#"
    echo "# Regenerate with ./librist-sync-update.sh <riststb-checkout>."
    echo "# install.sh checks this and refuses to build on a mismatch."
    echo "#"
    echo "# sha256  <hex>  <path>            file must be byte-identical to riststb"
    echo "# require <path> <literal>         literal must appear at least once"
    echo "# forbid  <path> <literal>         literal must not appear"
    echo "#"
    echo "# require/forbid cover src/rist-common.c and src/udp.c, which carry"
    echo "# per-tree work and so cannot be hashed whole. They pin the fixes that"
    echo "# are shared between the trees — the ones that have already gone"
    echo "# missing on one side once."
    echo
    echo "riststb ${SHA}"
    echo
    for f in $MIRRORED; do
        h=$(sha256sum "$HERE/librist/$f" | cut -d' ' -f1)
        printf 'sha256\t%s\t%s\n' "$h" "$f"
    done
    echo
    # --- shared fixes in the drifted files -------------------------------
    # NACK recovery-agent selection: last_rtt == 0 means NOT YET MEASURED.
    printf 'require\tsrc/rist-common.c\tif (check->last_rtt != 0 && check->last_rtt < recovery_agent_rtt)\n'
    # Part 6 selection: act on change, not on receipt.
    printf 'require\tsrc/rist-common.c\tprogram_selection_selection_unchanged(p->adv_peer_id, content_str)\n'
    # NTP is 2^32 units/s, so a raw nanosecond literal is ~4.3x too short.
    printf 'require\tsrc/udp.c\t5000ULL * RIST_CLOCK\n'
    printf 'require\tsrc/udp.c\t10000ULL * RIST_CLOCK\n'
    printf 'forbid\tsrc/udp.c\t> 5000000000ULL\n'
    printf 'forbid\tsrc/udp.c\t> 10000000000ULL\n'
    # pcr_cut must be wired in, or the flag silently does nothing.
    printf 'require\tsrc/rist-common.c\tRIST_URL_PARAM_PCR_CUT\n'
} > "$OUT"

echo "wrote $OUT (riststb ${SHA:0:7})"
