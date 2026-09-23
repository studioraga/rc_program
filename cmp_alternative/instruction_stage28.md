# Stage 28 Instructions — Compare Alternative Ranked-Cache Implementations

## Purpose

Stage 28 is intentionally implemented in a new source file:

```text
ranked_cached_cmp_alternative.c
```

Do **not** merge the five comparison implementations into `ranked_cache.c`.
The main `ranked_cache.c` remains the incremental Stage 0–27 implementation.

The comparison program exists to answer a different engineering question:

> If the cache semantics are kept identical, how do five alternative data-structure designs differ in lookup cost, rank-update cost, minimum/eviction cost, memory behavior, implementation complexity, and correctness risk?

All five versions use the same logical contract:

- fixed maximum capacity,
- deterministic backing database,
- rank changes on hits only,
- smaller rank has higher eviction priority,
- equal ranks break ties by smaller key,
- a miss reads the database,
- a full cache evicts the current `(rank,key)` minimum before insertion.

## Files introduced or changed

Stage 28 adds:

```text
ranked_cached_cmp_alternative.c
instruction.md
architecture.md
```

Stage 28 updates:

```text
README.md
.gitignore
```

`ranked_cache.c` is deliberately left unchanged.

## Version matrix

| Version | Design | Lookup | Rank update | Minimum | Eviction | Main trade-off |
|---|---|---:|---:|---:|---:|---|
| A | Linear array | O(N) | O(1)* | O(N) | O(N) | Simplest baseline |
| B | Hash + linear minimum scan | expected O(1) | O(1)* | O(N) | O(N) | Fixes lookup only |
| C | Hash + AVL tree | expected O(1) | O(log N) | O(log N) | O(log N) | Balanced ordered index |
| D | Hash + lazy heap | expected O(1) | O(log M) append | amortized stale cleanup | amortized + O(log M) | Simple updates, unbounded stale memory |
| E | Hash + indexed heap | expected O(1) | O(log N) | O(1) | O(log N) | Bounded final optimized design |

`*` Rank mutation itself is O(1) after the resident has already been located. Version A still pays O(N) to find the resident; Version B uses expected O(1) hash lookup.

`M` is the number of lazy-heap records. In Version D, `M` can grow with the number of rank updates and can be much larger than the number of resident entries `N`.

## Build 1 — warning-clean functional build

From the repository root:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    ranked_cached_cmp_alternative.c \
    -o ranked_cached_cmp_alternative
```

Run:

```bash
./ranked_cached_cmp_alternative
```

The final line must be:

```text
Stage 28 alternative-implementation validation: PASS
```

## Build 2 — ASan/UBSan correctness build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cached_cmp_alternative.c \
    -o ranked_cached_cmp_alternative_san
```

Run:

```bash
ASAN_OPTIONS=detect_leaks=1 \
UBSAN_OPTIONS=halt_on_error=1 \
./ranked_cached_cmp_alternative_san
```

Required result:

```text
no AddressSanitizer diagnostics
no UndefinedBehaviorSanitizer diagnostics
Stage 28 alternative-implementation validation: PASS
```

## Build 3 — optimized semantic-equivalence build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    -O3 \
    -DNDEBUG \
    ranked_cached_cmp_alternative.c \
    -o ranked_cached_cmp_alternative_optimized
```

Run:

```bash
./ranked_cached_cmp_alternative_optimized
```

The optimized build must produce the same deterministic correctness result as the normal and sanitizer builds.

## What the test program validates

### 1. Complexity table

The program prints the intended complexity model for Versions A–E before executing tests.

### 2. Targeted minimum/eviction equivalence

A capacity-three cache inserts keys `1, 2, 3`, then inserts key `4`.

All five versions must evict key `1`, because:

```text
key 1 -> rank 10
key 2 -> rank 20
key 3 -> rank 30
```

and key `1` is the deterministic minimum.

### 3. Lazy-heap memory demonstration

Version D and Version E both begin with four residents.

The test repeatedly changes key `2`'s rank 100 times.

Expected qualitative result:

```text
Version D:
    resident entries = 4
    heap records      > 4

Version E:
    resident entries = 4
    heap records      = 4
```

In the current deterministic validation:

```text
Version D residents=4 heap records=104
Version E residents=4 heap records=4
```

This is the central memory trade-off of lazy deletion.

### 4. 2,000-operation deterministic equivalence workload

Configuration:

```text
capacity   = 8
key_space  = 17
operations = 2000
seed       = 0x6a09e667f3bcc909
```

`key_space=17` is intentionally larger than capacity and exercises real hit/miss/eviction churn.

All five implementations must produce the same:

- returned-entry checksum,
- access count,
- hit count,
- miss count,
- database-read count,
- insertion count,
- eviction count,
- rank-update count,
- rank-decrease count,
- rank-unchanged count,
- rank-increase count,
- final resident/absent state for all keys,
- final resident values and ranks.

The current deterministic oracle is:

```text
accesses     = 2000
hits         = 937
misses       = 1063
evictions    = 1055
rank_updates = 937
checksum     = 5843817599207590962
```

Version D also reports its accumulated lazy-heap record count and number of stale records discarded.

## How to study each version in the source

Search for these section headers:

```text
Version A — linear baseline
Version B — hash + linear minimum
Version C — hash + AVL tree
Version D — hash + lazy heap
Version E — hash + indexed heap
```

Read them in that order. The file intentionally evolves the design one bottleneck at a time.

## Acceptance checklist

Stage 28 is complete only when all of the following are true:

```text
[ ] ranked_cache.c is unchanged
[ ] ranked_cached_cmp_alternative.c builds with -Werror
[ ] normal comparison run passes
[ ] ASan/UBSan comparison run passes
[ ] optimized semantic-equivalence run passes
[ ] targeted eviction equivalence passes
[ ] lazy-heap memory-growth demonstration passes
[ ] 2,000-operation cross-version equivalence passes
[ ] all five CacheStats objects match
[ ] all five final logical cache states match
[ ] every version-specific validator passes
[ ] README.md documents Stage 28
[ ] instruction.md exists
[ ] architecture.md exists
[ ] generated binaries are ignored in .gitignore
```

## Git workflow

Review only the intended Stage 28 changes:

```bash
git status
git diff -- \
    ranked_cached_cmp_alternative.c \
    README.md \
    instruction.md \
    architecture.md \
    .gitignore
```

Stage them:

```bash
git add \
    ranked_cached_cmp_alternative.c \
    README.md \
    instruction.md \
    architecture.md \
    .gitignore
```

Review the staged patch:

```bash
git diff --cached
```

Then commit and push.
