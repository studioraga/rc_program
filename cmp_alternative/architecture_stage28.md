# Stage 28 Architecture — Alternative Ranked-Cache Designs

## 1. Common cache contract

All five implementations deliberately share the same behavior so the data structures can be compared fairly.

```text
                  +-------------------+
GET(key, scenario)|                   |
----------------->| ranked cache      |
                  |                   |
                  +---------+---------+
                            |
                +-----------+-----------+
                |                       |
              HIT                     MISS
                |                       |
                v                       v
       apply rank scenario       db_read_entry(key)
       to resident only                 |
                |                       v
                |                cache full?
                |                 /       \
                |               yes       no
                |                |         |
                |                v         |
                |          evict minimum   |
                |                |         |
                +----------------+---------+
                                 |
                                 v
                          return resident
```

Ordering is identical across all versions:

```text
(rank_a, key_a) < (rank_b, key_b)
```

when:

```text
rank_a < rank_b
```

or, for equal ranks:

```text
key_a < key_b
```

That secondary key rule makes minimum selection deterministic.

---

# 2. Version A — Linear Baseline

## Structure

```text
VersionA
+------------------------------------------------------+
| entries[0] | entries[1] | ... | entries[N-1]        |
+------------------------------------------------------+
```

There is only one array.

## Lookup

```text
key
 |
 v
entries[0] ?
entries[1] ?
entries[2] ?
...
entries[N-1] ?
```

Worst case:

```text
O(N)
```

## Rank update

Once the resident pointer is known:

```c
resident->rank = new_rank;
```

is:

```text
O(1)
```

But a complete dynamic cache hit still paid O(N) to locate that resident.

## Minimum/eviction

```text
scan every resident
keep smallest (rank,key)
```

Cost:

```text
O(N)
```

Removal uses swap-with-last after the victim index is known.

## Strengths

- smallest code surface,
- contiguous memory,
- excellent locality at small N,
- easy correctness reasoning,
- useful oracle/baseline.

## Weaknesses

- lookup scales linearly,
- minimum selection scales linearly,
- no ordered index for dynamic priorities.

---

# 3. Version B — Hash + Linear Minimum

## Structure

```text
                       +----------------------+
key ------------------>| open-address hash    |
                       +----------+-----------+
                                  |
                                  v
                        +-------------------+
                        | stable resident    |
                        | entry pool         |
                        +-------------------+

Eviction still scans every active resident in the pool.
```

## Lookup

Expected:

```text
O(1)
```

## Rank update

After hash lookup:

```text
O(1)
```

because there is no ordered rank data structure to repair.

## Minimum/eviction

Still:

```text
O(N)
```

This version demonstrates an important optimization lesson:

> Fixing lookup does not automatically fix eviction.

## Strengths

- removes the first bottleneck,
- simple rank mutation,
- bounded memory,
- much simpler than tree/heap alternatives.

## Weaknesses

- minimum-rank scan remains linear,
- full-cache misses still pay O(N) victim selection.

---

# 4. Version C — Hash + AVL Tree

## Structure

```text
                         +------------------+
key -------------------->| hash table       |
                         +--------+---------+
                                  |
                                  v
                          StableEntry *

Every resident is also represented in an AVL tree:

                 (rank,key)
                     30,7
                   /      \
                20,4      50,2
               /   \       /  \
            ...    ...   ...  ...
```

The tree is ordered by:

```text
(rank,key)
```

## Lookup

Hash:

```text
expected O(1)
```

## Rank update

Changing a rank changes the tree key.

Therefore:

```text
remove old (rank,key) node    O(log N)
change resident rank          O(1)
insert new (rank,key) node    O(log N)
```

Overall:

```text
O(log N)
```

## Minimum

The minimum is the leftmost AVL node.

Current implementation:

```text
O(log N)
```

A separately maintained pointer to the minimum could reduce peek to O(1), but Stage 28 intentionally keeps the standard leftmost traversal.

## Eviction

```text
find leftmost minimum         O(log N)
remove from AVL               O(log N)
remove from hash              expected O(1)
```

Overall:

```text
O(log N)
```

## Strengths

- deterministic ordered structure,
- bounded memory,
- logarithmic rank changes,
- logarithmic eviction,
- supports ordered traversal/range operations naturally.

## Weaknesses

- more pointer-heavy than a heap,
- rotations increase implementation complexity,
- each tree node requires left/right pointers and height metadata,
- current implementation allocates/frees tree nodes during rank updates.

---

# 5. Version D — Hash + Lazy Heap

## Core idea

Do **not** modify an existing heap node when rank changes.

Instead:

```text
resident rank changes
       |
       v
increment resident generation
       |
       v
push NEW heap record
       |
       v
old heap record remains stale
```

## Structure

```text
key -> hash -> StableEntry
                    |
                    | current:
                    | key=2 rank=40 generation=7
                    v

Lazy heap may contain:

(rank=10,key=2,generation=1)  STALE
(rank=20,key=2,generation=2)  STALE
(rank=50,key=9,generation=3)  current
(rank=40,key=2,generation=7)  current
...
```

Each heap record snapshots:

```text
entry pointer
key
rank
generation
```

A heap record is current only if all snapshot fields still match the resident.

## Rank update

There is no arbitrary in-place heap priority update.

Instead:

```text
resident.rank = new_rank
resident.generation++
push new heap record
```

Cost:

```text
O(log M)
```

where `M` is heap-record count, not resident count.

## Minimum

Before using the root:

```text
while root is stale:
    pop root
```

Therefore minimum access has amortized cleanup cost.

## Memory behavior

This is Version D's defining trade-off.

After 100 updates to one resident in the four-entry demonstration:

```text
Version D:
    residents    = 4
    heap records = 104

Version E:
    residents    = 4
    heap records = 4
```

In the 2,000-operation churn workload Version D ends with only eight residents but hundreds of heap records.

## Strengths

- avoids indexed-heap bookkeeping,
- rank-update code is simple,
- common pattern for priority queues where stale records are acceptable,
- can be attractive when updates are rare and memory is plentiful.

## Weaknesses

- stale records consume memory,
- heap size depends on update history,
- stale cleanup makes minimum/eviction latency less predictable,
- long-running systems require compaction/rebuild or a memory limit,
- `M` may become much greater than `N`.

---

# 6. Version E — Hash + Indexed Heap

## Structure

```text
key
 |
 v
+-------------------+
| hash table        |
+---------+---------+
          |
          v
    StableEntry
    +------------------+
    | key              |
    | value            |
    | rank             |
    | heap_index ------+----------------+
    +------------------+                |
                                        v
                            +-------------------------+
Indexed min-heap            | [0] [1] [2] ... [N-1] |
                            +-------------------------+
```

The critical reverse mapping is:

```text
resident -> heap_index
```

## Lookup

Expected:

```text
O(1)
```

## Minimum

Heap root:

```text
heap[0]
```

Cost:

```text
O(1)
```

## Rank update

```text
hash lookup                  expected O(1)
read resident.heap_index     O(1)
change rank                  O(1)
sift up OR sift down         O(log N)
```

Overall heap repair:

```text
O(log N)
```

Every heap swap updates both residents' `heap_index` values.

## Eviction

```text
remove heap root             O(log N)
remove key from hash         expected O(1)
release stable slot          O(1)
```

Overall:

```text
O(log N)
```

## Memory behavior

Exactly one heap pointer per resident:

```text
heap records = resident count = N
```

No stale versions accumulate.

## Strengths

- expected O(1) key lookup,
- O(1) minimum peek,
- O(log N) arbitrary rank repair,
- O(log N) eviction,
- bounded memory,
- predictable steady-state behavior,
- matches the final optimized design developed incrementally in `ranked_cache.c`.

## Weaknesses

- swaps must update reverse indices correctly,
- stable resident addresses are required,
- invariant maintenance is more delicate than lazy deletion,
- implementation complexity is higher than Versions A, B, and D.

---

# 7. Side-by-side architecture summary

```text
Version A
=========
array
  |
  +--> lookup scan
  +--> minimum scan


Version B
=========
hash ---> stable pool
             |
             +--> minimum scan


Version C
=========
hash ---> stable pool <--- AVL tree ordered by (rank,key)
                              |
                              +--> leftmost = minimum


Version D
=========
hash ---> stable pool
             |
             +--> generation
                    |
                    v
               lazy heap
               current + stale versions


Version E
=========
hash ---> stable pool
             |
             +--> heap_index ----------------+
                                           v
                                      indexed heap
                                      one node/resident
```

---

# 8. Complexity summary

| Version | Lookup | Rank update | Minimum | Eviction | Memory characteristic |
|---|---:|---:|---:|---:|---|
| A | O(N) | O(1)* | O(N) | O(N) | O(N) compact array |
| B | expected O(1) | O(1)* | O(N) | O(N) | O(N) + hash |
| C | expected O(1) | O(log N) | O(log N) | O(log N) | O(N) + hash + AVL nodes |
| D | expected O(1) | O(log M) append | amortized stale cleanup | amortized + O(log M) | O(M), grows with updates |
| E | expected O(1) | O(log N) | O(1) | O(log N) | O(N), bounded |

`*` after the resident is known.

---

# 9. Why Version E is the final optimized implementation

Version E is not selected because it has the shortest code. It does not.

It is selected because it provides the most useful combined contract for this problem:

```text
key lookup          expected O(1)
minimum peek        O(1)
rank update         O(log N)
eviction            O(log N)
heap memory         O(N)
```

while avoiding Version D's unbounded stale-history growth.

Version C remains a strong alternative when ordered traversal, ranges, predecessor/successor operations, or more general sorted queries are part of the product requirement.

Version D remains a useful engineering alternative when updates are comparatively rare and implementation simplicity is more valuable than strict memory bounds or predictable cleanup latency.

Versions A and B remain important because they make the optimization progression visible and serve as correctness/reference implementations.
