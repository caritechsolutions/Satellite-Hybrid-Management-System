# The librist sync check (R3)

librist is a modified fork, and there are two copies of it: this repo's
`librist/` (headend) and riststb's `librist/` (box). They must agree on
everything that decides how bytes are cut, numbered and filtered, because the
box and the headend are two ends of one protocol. They have already drifted once
without anyone noticing, which is why this exists.

**riststb is canonical.** Changes are made there and mirrored here; the manifest
records which riststb commit this copy claims to match.

## What runs where

| piece | where | when |
|---|---|---|
| `librist/RISTSTB-SYNC` | this repo | checked in |
| `./librist-sync-update.sh <riststb>` | a machine with both trees | after mirroring a change |
| the check block in `install.sh` | the headend | every install, before the librist build |

The headend has no riststb checkout and no credentials for it, so the check
cannot diff against the real tree. It verifies this copy against the recorded
values instead. That is the whole reason the manifest is a checked-in file.

## What it hashes

Seven files, by sha256, byte-for-byte:

```
src/pcr_cut.c              src/program-selection.c    include/librist/headers.h
src/pcr_cut.h              src/program-selection.h    include/librist/urlparam.h
                                                      tools/ristsender.c
```

These are the files the Part 8 work touched, and they are **currently identical
in both trees** — that is what makes a whole-file hash usable on them. The check
therefore changes nothing about a normal install: it passes silently and the
build proceeds exactly as before.

## What it cannot hash, and what it does instead

`src/rist-common.c`, `src/udp.c` and `src/rist.c` are shared code that still
carries per-tree work — 162, 112 and 12 differing lines respectively. A
whole-file hash over them would fail on every install for reasons that have
nothing to do with Part 8.

**This matters more than it sounds, because the drift that prompted this check
lived in exactly those two files.** The three fixes reconciled in R2 were in
`rist-common.c` and `udp.c`; a hash-only check would not have caught the failure
it was built to prevent.

So those files are covered by content assertion instead — `require` and `forbid`
lines naming the specific shared fixes:

| file | assertion | what it pins |
|---|---|---|
| `rist-common.c` | require `check->last_rtt != 0 && ...` | NACK recovery-agent selection: `last_rtt == 0` means *not yet measured*, not zero latency |
| `rist-common.c` | require `program_selection_selection_unchanged(...)` | Part 6 selection acts on change, not on receipt |
| `rist-common.c` | require `RIST_URL_PARAM_PCR_CUT` | the `pcr_cut` flag is actually wired to the parser |
| `udp.c` | require `5000ULL * RIST_CLOCK`, `10000ULL * RIST_CLOCK` | NTP is 2^32 units/s |
| `udp.c` | forbid `> 5000000000ULL`, `> 10000000000ULL` | the raw literals those replaced |

Writing these assertions is what found a third NTP-units site that R2 had
missed (riststb `4501700`) — R2's message said "both converted literals" and
there were three.

## What it still does not cover

Stated plainly so it is not assumed away:

1. **It is one-directional.** It catches divergence introduced *here*. A change
   made in riststb and never mirrored here leaves the manifest stale, and the
   check passes — the recorded hashes still describe this copy. Only
   `librist-sync-update.sh`, run where both trees exist, sees both sides; it
   refuses to write a manifest while any mirrored file differs.
2. **The drifted files are covered by named assertions, not by equivalence.** A
   new fix landing in one tree only, in `rist-common.c` or `udp.c`, is invisible
   until someone adds an assertion for it. Adding the assertion is part of
   mirroring the fix.
3. **The remaining drift is real and unreconciled in both directions.** Beyond
   R2, riststb carries FSR agent-selection and log-quiescing work in
   `rist-common.c`/`udp.c` that this tree does not; `rist.c` differs only in
   brace style. Converging the three files fully would let them be hashed too
   and would retire (2). That is the right end state and it is not done.

## Escape hatches

`SKIP_LIBRIST_SYNC_CHECK=1` builds anyway. A checkout with no
`librist/RISTSTB-SYNC` warns and continues, so older branches still install.
Both are warnings, never silent.
