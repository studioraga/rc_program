# Ranked Cache — Incremental C Implementation

## Project Purpose

This repository incrementally develops a fixed-size, fully associative ranked cache in C.

The implementation is intentionally built in small, testable stages. At every completed stage, the code should make it possible to explain:

- what requirement is being solved,
- what data structure is currently being used,
- what operation is being tested,
- what the current time/space complexity is, and
- what has been functionally validated.

The repository is intended both as an interview-preparation exercise and as a systems-programming benchmark that can later be used to study correctness, latency, throughput, memory behavior, and optimization trade-offs.

---

## Problem Statement

Implement a cache with a maximum capacity `K`.

Each cache entry contains:

- a key,
- a value, and
- a rank.

The cache exposes a lookup operation.

On a cache hit, the cached entry is returned.

On a cache miss, the entry is obtained from a backing database, its rank is determined, and the entry is inserted into the cache.

If the cache is already full, the entry with the minimum rank is the eviction candidate.

The complete problem has two parts:

- **Part 1:** an entry's rank does not change while it remains cached.
- **Part 2:** an entry's rank may change after lookup.

This README documents only the implementation progress that has actually been completed.

---

## Important Operations Identified

The complete problem requires support for the following logical operations:

1. Find an entry by key.
2. Insert an entry.
3. Find the lowest-ranked entry.
4. Remove the lowest-ranked entry.
5. Change an arbitrary entry's rank.

No optimized data structure for these operations has been introduced at the current checkpoint.

---

## Manual Oracle

Before implementing the full cache, the following small example is used as the first manual correctness oracle.

### Cache capacity

```text
capacity = 3
```

### Example database

| Key | Value | Rank |
|---:|---:|---:|
| 1 | 100 | 50 |
| 2 | 200 | 20 |
| 3 | 300 | 80 |
| 4 | 400 | 70 |

### Example access sequence

```text
GET 1
GET 2
GET 3
GET 1
GET 4
```

### Expected reasoning

```text
GET 1
MISS
cache = {1:50}

GET 2
MISS
cache = {1:50, 2:20}

GET 3
MISS
cache = {1:50, 2:20, 3:80}

GET 1
HIT

GET 4
MISS
cache is full

minimum rank = key 2, rank 20
evict key 2

cache = {1:50, 3:80, 4:70}
```

This example is retained as a reference oracle for later implementation stages.

---

# Current Implementation Status

## Stage 0 — Basic Data Model

**Status: COMPLETE AND VALIDATED**

Stage 0 introduced the representation of one cache entry.

That data model remains the foundation for the Stage 1 database abstraction.

### Type definitions

```c
typedef unsigned long long CacheKey;
typedef long long Rank;
```

`CacheKey` represents the identifier used to address an entry.

`Rank` represents the numeric priority/rank associated with an entry.

### Cache entry structure

```c
typedef struct {
    CacheKey key;
    unsigned long long value;
    Rank rank;
} CacheEntry;
```

The members are:

| Member | Type | Purpose |
|---|---|---|
| `key` | `CacheKey` | Identifies the entry |
| `value` | `unsigned long long` | Represents the entry payload for the Stage 0 test |
| `rank` | `Rank` | Stores the entry's rank |

### Stage 0 validation

Stage 0 validated direct construction and printing of one `CacheEntry`. That data model remains unchanged and is now reused by the later completed stages.

---

## Stage 1 — Database Abstraction

**Status: COMPLETE AND VALIDATED**

Stage 1 introduces a deterministic backing-database abstraction through:

```c
CacheEntry db_read_entry(CacheKey key);
```

The function accepts a cache key and returns a complete `CacheEntry`.

### Deterministic database mapping

The current Stage 1 implementation constructs the returned entry as:

```c
e.key = key;
e.value = key * 100;
e.rank = key * 10;
```

This is intentionally synthetic and deterministic. It provides a predictable stand-in for the later external/backing-store read while allowing the cache logic to be developed and tested independently.

For example:

```text
input key = 5
```

produces:

```text
key   = 5
value = 500
rank  = 50
```

### Stage 1 validation path

Stage 1 validated obtaining an entry through the database abstraction:

```c
CacheEntry e = db_read_entry(5);
```

and printing the returned fields. The `db_read_entry()` implementation remains unchanged in Stage 2.

### Stage 1 validation result

Build:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache6
```

Run:

```bash
./ranked_cache6
```

Observed and expected output:

```text
key=5 value=500 rank=50
```

Stage 1 therefore validates that:

- a key can be passed into the backing-store abstraction,
- the abstraction constructs and returns a complete `CacheEntry`,
- the returned key is preserved,
- the value is derived deterministically,
- the rank is derived deterministically, and
- the caller receives the expected entry.

At the Stage 1 checkpoint, no cache storage or cache-management logic had yet been implemented.

### Stage 1 complexity

For the current synthetic implementation, `db_read_entry()` performs only a fixed number of assignments and arithmetic operations.

```text
Time:  O(1)
Space: O(1)
```

This describes only the deterministic Stage 1 abstraction. It does not model the latency of a real database, file, network, or persistent-storage operation.


---

## Stage 2 — Simplest Linear Cache

**Status: COMPLETE AND VALIDATED**

Stage 2 introduces the first functioning cache representation using a fixed-size array.

The objective at this checkpoint is correctness and observability, not optimization.

### Cache representation

```c
#define MAX_CACHE_CAPACITY 100U

typedef struct {
    CacheEntry entries[MAX_CACHE_CAPACITY];
    size_t size;
    size_t capacity;
} Cache;
```

The fields are:

| Member | Purpose |
|---|---|
| `entries` | Fixed-size array containing resident cache entries |
| `size` | Number of currently valid resident entries |
| `capacity` | Configured maximum number of entries for this cache instance |

The physical storage limit is `MAX_CACHE_CAPACITY`, while each initialized cache can use a smaller logical capacity.

The Stage 2 validation uses:

```text
capacity = 3
```

### Functions introduced

#### `cache_init()`

```c
int cache_init(Cache *cache, size_t capacity);
```

Initializes an empty cache and validates that the requested capacity is non-zero and does not exceed `MAX_CACHE_CAPACITY`.

Current complexity:

```text
Time:  O(1)
Space: O(1)
```

#### `cache_is_full()`

```c
int cache_is_full(const Cache *cache);
```

Checks whether:

```text
size >= capacity
```

Current complexity:

```text
Time: O(1)
```

#### `cache_lookup()`

```c
CacheEntry *cache_lookup(Cache *cache, CacheKey key);
```

Searches the resident entries from index `0` through `size - 1` and compares each key.

Returns:

- a pointer to the matching entry on a hit,
- `NULL` on a miss.

Current complexity:

```text
Best case:  O(1)
Worst case: O(N)
Average:    O(N) for this linear representation
```

#### `cache_insert()`

```c
int cache_insert(Cache *cache, CacheEntry entry);
```

The Stage 2 insertion policy:

1. reject an invalid cache,
2. reject insertion when capacity has already been reached,
3. linearly check for a duplicate key,
4. append the entry at `entries[size]`,
5. increment `size`.

Because duplicate detection uses the linear `cache_lookup()`:

```text
Time:  O(N)
Space: O(1) additional space
```

#### `cache_find_min_rank_index()`

```c
int cache_find_min_rank_index(const Cache *cache,
                              size_t *min_index);
```

Linearly scans the resident entries and records the index of the smallest rank.

The current tie rule is deterministic:

> If multiple entries have the same minimum rank, the first one encountered in the array is selected.

Current complexity:

```text
Time:  O(N)
Space: O(1)
```

#### `cache_evict_min()`

```c
int cache_evict_min(Cache *cache,
                    CacheEntry *evicted_entry);
```

Eviction first calls the linear minimum-rank search.

After finding the victim, the implementation copies the last valid array entry into the victim's slot and decrements `size`.

This avoids shifting every later entry.

Current complexity:

```text
minimum-rank search: O(N)
victim replacement:  O(1)
overall eviction:    O(N)
```

#### `cache_print()`

Prints the current resident cache in compact `{key:rank}` form.

This function exists for validation and visibility at the current checkpoint.

#### `check()`

A small test helper used by `main()` to print `[PASS]` or `[FAIL]` for each Stage 2 invariant.

---

## Stage 2 Validation

The Stage 2 test intentionally uses the original manual-oracle entries:

| Key | Value | Rank |
|---:|---:|---:|
| 1 | 100 | 50 |
| 2 | 200 | 20 |
| 3 | 300 | 80 |
| 4 | 400 | 70 |

The Stage 1 `db_read_entry()` abstraction remains in the source unchanged, but it is not yet connected to cache hit/miss processing in this checkpoint.

### Operations tested

The active `main()` validates:

```text
cache initialization
insert
capacity check
lookup hit
lookup miss
minimum-rank search
minimum-rank eviction
post-eviction lookup
insert after eviction
final resident-set validation
```

### Expected cache progression

After inserting keys `1`, `2`, and `3`:

```text
cache = {1:50, 2:20, 3:80}
```

The cache is full.

A direct attempt to insert key `4` is rejected until eviction occurs.

The minimum-rank search must identify:

```text
key  = 2
rank = 20
```

After evicting key `2`, key `4` is inserted.

Final expected resident set:

```text
cache = {1:50, 3:80, 4:70}
```

### Build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache6
```

### Run

```bash
./ranked_cache6
```

### Validated output

```text
=== Stage 2: simplest linear cache ===
[PASS] initialize cache with capacity 3
[PASS] cache starts empty
[PASS] insert key 1
[PASS] insert key 2
[PASS] insert key 3
cache = {1:50, 2:20, 3:80}
[PASS] capacity check reports full
[PASS] insert is rejected while cache is full
[PASS] linear lookup finds key 1
[PASS] linear lookup reports missing key
[PASS] minimum-rank search succeeds
[PASS] minimum rank is key 2 with rank 20
[PASS] evict minimum-ranked entry
[PASS] evicted entry is key 2 rank 20
[PASS] evicted key 2 is no longer present
[PASS] cache is no longer full after eviction
[PASS] insert key 4 after eviction
[PASS] cache size returns to capacity
[PASS] final cache contains keys 1, 3, and 4
cache = {1:50, 3:80, 4:70}
Stage 2 validation: PASS
```

### Stage 2 operation complexity

| Operation | Current Stage 2 complexity | Reason |
|---|---:|---|
| initialize cache | O(1) | fixed assignments |
| capacity check | O(1) | compare `size` and `capacity` |
| lookup | O(N) | linear key scan |
| duplicate-aware insert | O(N) | calls linear lookup |
| minimum-rank search | O(N) | linear rank scan |
| eviction | O(N) | dominated by minimum-rank search |
| remove known array slot | O(1) | replace with last resident entry |
| cache storage | O(K) | fixed array capacity |

This is intentionally the simple reference implementation for the current checkpoint.

No hash table, heap, tree, dynamic-rank maintenance, performance benchmark, or other optimized cache structure is introduced here.

---

## Stage 3 — Linear Minimum-Rank Eviction

**Status: COMPLETE AND VALIDATED**

Stage 2 already proved that the minimum-ranked entry could be found and evicted with linear scanning. Stage 3 makes the eviction path explicit by separating victim selection from removal of a known array slot.

### `cache_remove_at()`

```c
int cache_remove_at(Cache *cache,
                    size_t index,
                    CacheEntry *removed_entry);
```

When the victim index is already known, the function copies the last resident entry into the removed slot and decrements `size`.

```text
known-slot removal: O(1)
```

The function rejects null caches and out-of-range indexes.

### `cache_evict_min()` composition

Stage 3 now expresses eviction as two operations:

```text
cache_find_min_rank_index()   O(N)
            ↓
cache_remove_at()             O(1)
```

Therefore:

```text
minimum-rank eviction overall: O(N)
```

The linear minimum search remains the dominant cost.

### Tie rule

If multiple resident entries have the same minimum rank, the first one encountered by the array scan is selected.

### Stage 3 validation

The original Stage 2 oracle remains passing, and Stage 3 adds dedicated checks for:

- eviction from an empty cache,
- rejection of an out-of-range removal index,
- equal-rank entries, and
- deterministic first-encountered tie eviction.

Build:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache6
```

The test must end with:

```text
Stage 3 validation: PASS
```

No automatic database-backed `cache_get()` path, assertion framework, optimized lookup structure, heap, tree, or dynamic-rank maintenance is introduced in Stage 3.

---

## Stage 4 — Complete `cache_get()`

**Status: COMPLETE AND VALIDATED**

Stage 4 composes the existing Stage 1–3 primitives into the first complete cache access operation:

```c
CacheEntry *cache_get(Cache *cache, CacheKey key);
```

### Hit path

```text
cache_get(key)
    ↓
linear cache_lookup()
    ↓
entry found
    ↓
return existing resident entry
```

The cache size and resident set remain unchanged on a hit.

Current hit complexity:

```text
O(N)
```

because key lookup is still a linear array scan.

### Miss path

```text
cache_get(key)
    ↓
linear cache_lookup() -> MISS
    ↓
db_read_entry(key)
    ↓
capacity check
    ↓
if full: cache_evict_min()
    ↓
cache_insert()
    ↓
return newly resident entry
```

With the current simple structures, the miss path may perform multiple linear scans. Its asymptotic cache-maintenance cost remains O(N), in addition to the backing-store read cost.

```text
miss = O(N) + database-read cost
```

No attempt is made yet to optimize repeated scans.

### Stage 4 validation workload

The unchanged Stage 1 deterministic database abstraction returns:

```text
key 1 -> value 100, rank 10
key 2 -> value 200, rank 20
key 3 -> value 300, rank 30
key 4 -> value 400, rank 40
```

The active test performs:

```text
GET 1   MISS -> insert 1
GET 2   MISS -> insert 2
GET 3   MISS -> insert 3
GET 2   HIT  -> return existing resident
GET 4   MISS while full
```

Before `GET 4`:

```text
cache = {1:10, 2:20, 3:30}
```

The minimum resident is key `1`, rank `10`, so the full-cache miss evicts key `1` and inserts key `4`.

Final resident keys:

```text
2, 3, 4
```

The physical array order is not part of cache semantics because known-slot eviction can replace the victim with the final array element.

Build:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache6
```

The active test must end with:

```text
Stage 4 validation: PASS
```

Stage 4 does not introduce assertions/invariant validation, optimized key lookup, rank-indexing structures, concurrency, or performance benchmarking.

---

## Stage 5 — Assertions and Invariants

**Status: COMPLETE AND VALIDATED**

Stage 5 adds explicit structural validation and development-time assertions without changing the Stage 4 cache algorithm.

### Structural invariants

A valid `Cache` must satisfy all of the following:

1. the cache pointer is non-NULL,
2. `capacity` is at least 1,
3. `capacity` does not exceed `MAX_CACHE_CAPACITY`,
4. `size <= capacity`, and
5. no two resident entries have the same key.

These rules are checked by:

```c
int cache_validate(const Cache *cache);
```

The function returns `1` for a structurally valid cache and `0` for an invalid cache.

### Duplicate-key validation complexity

The current validator compares resident entry pairs directly:

```text
for each resident i
    compare against residents i+1 ... N-1
```

Therefore the full validator is:

```text
O(N^2)
```

This is intentional at Stage 5. `cache_validate()` is a correctness/debugging oracle, not the optimized cache lookup mechanism.

### Development-time assertions

The helper:

```c
void cache_assert_invariants(const Cache *cache);
```

uses standard C:

```c
assert(cache_validate(cache));
```

The cache operations call this helper around valid-state transitions so structural corruption is detected close to where it occurs during development.

Standard `assert()` checks are disabled when the program is compiled with `NDEBUG` defined. At this checkpoint they remain enabled for correctness testing.

Because `cache_validate()` is currently O(N²), assertion-enabled builds can have substantially higher runtime cost than the underlying Stage 4 algorithm. Those checks must not be confused with the cache algorithm's eventual benchmark cost.

### Invalid states tested

Stage 5 deliberately constructs copies of the cache with invalid state and verifies that `cache_validate()` rejects them:

```text
size > capacity
capacity == 0
duplicate resident keys
NULL cache
```

The deliberately corrupted objects are not passed into mutating cache operations because those operations assert that structural state is valid.

### Regression validation

The complete Stage 4 `cache_get()` flow is re-run with assertions enabled:

```text
GET 1
GET 2
GET 3
GET 2   (hit)
GET 4   (full-cache miss and minimum-rank eviction)
```

The final resident set remains keys `2`, `3`, and `4`.

### Standard build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache6
./ranked_cache6
```

Expected final status:

```text
Stage 5 validation: PASS
```

### Sanitizer validation

Stage 5 was also validated with:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 \
    -O0 -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache5_san

./ranked_cache5_san
```

The sanitizer run completes with:

```text
Stage 5 validation: PASS
```

and no AddressSanitizer or UndefinedBehaviorSanitizer diagnostic was reported.

Stage 5 does not introduce optimized lookup, a hash table, a heap, a tree, dynamic-rank maintenance, concurrency, or performance benchmarking.


---

## Stage 6 — Identify the First Bottleneck

**Status: COMPLETE AND VALIDATED**

Stage 6 does **not** optimize the cache. Its purpose is to identify and prove the first algorithmic bottleneck in the current Stage 5 implementation before any data-structure replacement is attempted.

The current cache still stores resident entries in a fixed array, and `cache_lookup()` still scans that array from index `0` until it either finds the requested key or reaches the end.

### Why Stage 6 uses operation counts instead of timing

Stage 5 intentionally enables `cache_validate()`, whose duplicate-key invariant check is O(N²). Wall-clock timing of the assertion-enabled program would therefore mix together:

- linear lookup work,
- O(N²) debug invariant checking,
- printing and test-harness overhead, and
- machine/OS timing noise.

Stage 6 therefore identifies the bottleneck using deterministic **key-comparison counts** inside the existing `cache_lookup()` implementation. This gives a platform-independent measurement of how lookup work grows with resident cache size without changing the lookup algorithm.

### Lookup instrumentation

Stage 6 adds a diagnostic structure:

```c
typedef struct {
    uint64_t lookup_calls;
    uint64_t key_comparisons;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
} LookupStats;
```

The counters are reset and sampled using:

```c
void lookup_stats_reset(void);
LookupStats lookup_stats_snapshot(void);
```

`cache_lookup()` now increments these counters while executing the same linear scan used in Stage 5.

The instrumentation does not change:

- cache storage,
- lookup order,
- eviction policy,
- insertion policy,
- rank handling, or
- asymptotic complexity.

### Experiment 1 — position within a 100-entry cache

A valid 100-entry cache is populated with unique keys `1..100`, then four lookups are profiled.

| Probe | Expected result | Key comparisons |
|---|---|---:|
| key `1` | hit at first element | 1 |
| key `50` | hit in the middle | 50 |
| key `100` | hit at last element | 100 |
| key `1000` | miss | 100 |

Validated output:

```text
[PROFILE] first-entry lookup           size=100 comparisons=1 result=HIT
[PROFILE] middle-entry lookup          size=100 comparisons=50 result=HIT
[PROFILE] last-entry lookup            size=100 comparisons=100 result=HIT
[PROFILE] missing-entry lookup         size=100 comparisons=100 result=MISS
```

This demonstrates that lookup cost depends directly on where the key is found. A miss and a last-element hit both require scanning the complete resident set.

### Experiment 2 — scaling resident size

Stage 6 then performs the same missing-key lookup while increasing resident cache size.

Validated results:

```text
size    key-comparisons
   1    1
  10    10
  25    25
  50    50
 100    100
```

For a missing key:

```text
key comparisons = resident cache size = N
```

Therefore the measured lookup work grows linearly with `N`.

### First bottleneck identified

The first bottleneck is:

```text
cache_lookup() = O(N)
```

This affects not only direct cache hits. The current `cache_get()` miss path can invoke linear lookup more than once through its composed operations, including duplicate detection during insertion and the final lookup used to return the newly inserted entry.

At this checkpoint the important conclusion is only:

> Key lookup is the first operation to target because the existing implementation requires a linear scan of resident entries.

Stage 6 deliberately stops after identifying and measuring this bottleneck. It does not introduce a replacement lookup structure.

### Stage 6 helper

For controlled lookup profiling, Stage 6 adds:

```c
int stage6_fill_profile_cache(Cache *cache, size_t count);
```

This helper directly fills a structurally valid cache with deterministic unique entries. It intentionally avoids `cache_insert()` so the experiment measures `cache_lookup()` itself instead of insertion's additional duplicate-check lookup.

### Stage 6 validation

Normal build:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 \
    ranked_cache.c \
    -o ranked_cache6

./ranked_cache6
```

The run must end with:

```text
Stage 6 finding: cache_lookup() grows linearly with resident size.
First bottleneck identified: O(N) key lookup.
Stage 6 validation: PASS
```

Sanitizer validation:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 \
    -O0 -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache6_san

./ranked_cache6_san
```

The sanitizer build also completes with:

```text
Stage 6 validation: PASS
```

with no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics.

### Stage 6 complexity

No production algorithm has changed.

```text
cache_lookup() best-case hit       O(1)
cache_lookup() average/worst       O(N)
cache_lookup() miss                O(N)
lookup instrumentation update      O(1) per comparison
cache_validate()                   O(N²) debug/correctness check
```

The Stage 6 measurement confirms the existing linear lookup complexity rather than replacing it.

No hash table, heap, tree, dynamic-rank update mechanism, concurrency support, or optimized cache lookup is introduced in Stage 6.

---

## Stage 7 — Test Hash-Table Lookup Independently

**Status: COMPLETE AND VALIDATED**

Stage 7 introduces a standalone hash table solely to validate key-based lookup behavior before changing the existing cache implementation.

The important rule for this checkpoint is:

> The hash table is **not connected to `Cache`, `cache_lookup()`, `cache_insert()`, or `cache_get()` yet**.

Stage 6 proved that the current array-backed cache lookup is O(N). Stage 7 therefore tests the candidate O(1)-average lookup structure independently so hash-table correctness can be established before cache integration.

### Standalone hash-table representation

```c
#define HASH_TABLE_CAPACITY 211U

typedef enum {
    HASH_SLOT_EMPTY = 0,
    HASH_SLOT_OCCUPIED,
    HASH_SLOT_DELETED
} HashSlotState;

typedef struct {
    CacheKey key;
    CacheEntry *entry;
    HashSlotState state;
} HashSlot;

typedef struct {
    HashSlot slots[HASH_TABLE_CAPACITY];
    size_t size;
} HashTable;
```

The table stores:

```text
key -> CacheEntry *
```

It uses **open addressing with linear probing**.

The three slot states are required because deletion cannot simply turn a slot back into `EMPTY`. Doing so could break a collision probe chain and make later colliding keys unreachable.

`HASH_SLOT_DELETED` therefore acts as a tombstone.

### Hash function used at this checkpoint

```c
size_t hash_table_bucket(CacheKey key)
{
    return (size_t)(key % (CacheKey)HASH_TABLE_CAPACITY);
}
```

This modulo hash is intentionally simple and deterministic for Stage 7.

It makes collision tests easy to construct because:

```text
K
K + HASH_TABLE_CAPACITY
K + 2 * HASH_TABLE_CAPACITY
```

all begin at the same initial bucket.

This is a correctness-oriented test hash, not a claim that this is the final production-quality hash function.

### Functions introduced

#### `hash_table_init()`

```c
void hash_table_init(HashTable *table);
```

Initializes every hash slot as `HASH_SLOT_EMPTY` and resets the table size to zero.

Complexity:

```text
Time:  O(M)
Space: O(1) additional
```

where `M` is the fixed hash-table capacity.

#### `hash_table_bucket()`

```c
size_t hash_table_bucket(CacheKey key);
```

Calculates the starting bucket for a key.

Complexity:

```text
O(1)
```

#### `hash_table_lookup()`

```c
CacheEntry *hash_table_lookup(const HashTable *table,
                              CacheKey key);
```

Lookup behavior:

1. compute the initial bucket,
2. inspect the current slot,
3. return the entry when the key matches,
4. continue through `HASH_SLOT_DELETED` tombstones,
5. continue probing on an occupied non-matching slot,
6. stop on `HASH_SLOT_EMPTY`, because the key cannot occur later in that probe chain.

Complexity at a controlled load factor:

```text
Expected/average: O(1)
Worst case:       O(M)
```

The worst case occurs when clustering or high occupancy forces probing across much of the table.

#### `hash_table_insert()`

```c
int hash_table_insert(HashTable *table,
                      CacheEntry *entry);
```

Insertion:

- rejects invalid arguments,
- rejects duplicate keys,
- uses linear probing on collisions,
- remembers the first tombstone encountered,
- reuses that tombstone when the key is not already present.

Complexity:

```text
Expected/average: O(1)
Worst case:       O(M)
```

#### `hash_table_remove()`

```c
int hash_table_remove(HashTable *table,
                      CacheKey key,
                      CacheEntry **removed_entry);
```

Deletion marks the slot as:

```text
HASH_SLOT_DELETED
```

rather than `HASH_SLOT_EMPTY` so later members of the same collision chain remain reachable.

Complexity:

```text
Expected/average: O(1)
Worst case:       O(M)
```

#### `hash_table_validate()`

```c
int hash_table_validate(const HashTable *table);
```

The standalone validator checks:

- table size does not exceed capacity,
- every occupied slot contains a non-NULL entry,
- slot key matches the referenced entry key,
- every occupied mapping can be found through `hash_table_lookup()`,
- empty and deleted slots do not retain entry pointers,
- counted occupied slots equal `table->size`.

This validator exists for correctness testing only.

---

## Stage 7 Independent Validation

The test intentionally begins with ordinary, non-colliding entries:

```text
key 10
key 20
key 30
```

It validates:

```text
insert 10
insert 20
insert 30
lookup 20 -> HIT
lookup 99 -> MISS
duplicate insert 20 -> rejected
```

### Collision test

With:

```text
HASH_TABLE_CAPACITY = 211
```

the keys:

```text
1
212
423
```

share the same initial bucket because:

```text
1   % 211 = 1
212 % 211 = 1
423 % 211 = 1
```

Stage 7 verifies:

```text
insert key 1
insert key 212 through linear probing
lookup key 1 succeeds
lookup key 212 succeeds
```

### Tombstone test

After deleting key `1`, the first collision slot becomes a tombstone.

The important correctness test is:

```text
lookup key 212 still succeeds
```

If deletion incorrectly changed the slot to `EMPTY`, that lookup could terminate too early.

Stage 7 then inserts key `423` and validates tombstone reuse.

### Expected Stage 7 result

```text
=== Stage 7: standalone hash-table lookup ===
[PASS] initialize empty hash table
[PASS] hash insert key 10
[PASS] hash insert key 20
[PASS] hash insert key 30
[PASS] three hash mappings validate
[PASS] hash lookup finds key 20
[PASS] hash lookup reports missing key 99
[PASS] duplicate hash key is rejected
[PASS] collision test keys share initial bucket
[PASS] insert first collision key
[PASS] linear probing inserts colliding key
[PASS] collision-chain lookups succeed
[PASS] remove first collision key
[PASS] deleted hash key is absent
[PASS] lookup crosses tombstone to colliding key
[PASS] insert reuses deleted collision slot
[PASS] lookup finds tombstone-reuse entry
[PASS] final standalone hash table validates
Stage 7 hash-table validation: PASS

Stage 7 validation: PASS
```

### Stage 7 boundary

At this checkpoint:

```text
existing Cache implementation
        |
        +--> still uses linear array lookup

standalone HashTable
        |
        +--> independently tested
        +--> NOT integrated into Cache
```

Therefore Stage 7 proves the candidate data structure but does not yet claim that cache lookup itself is O(1).

No hash-table lookup is used by `cache_get()` in this stage.



---

## Build Environment

Current target environment:

- Ubuntu 24.04 LTS
- GCC
- C11

### Build command

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache7
```

The warning flags are intentionally enabled from the first stage:

- `-Wall`
- `-Wextra`
- `-Wpedantic`

This helps catch implementation mistakes early as the program becomes more complex.

---

## Run

```bash
./ranked_cache7
```

### Expected result

The active Stage 7 test suite must end with:

```text
Stage 7 validation: PASS
```

The Stage 7 hash table is standalone, so this checkpoint does not define a new cache-resident-set result.

### Validation result

Stages 0 through 7 have been validated successfully. The active Stage 7 test returns exit status `0`, including the sanitizer validation build.

---

## Current Functional Scope

At this commit, the program can:

- define the cache entry data model,
- provide the deterministic Stage 1 database abstraction,
- initialize a fixed-size array cache,
- track current size and configured capacity,
- detect when the cache is full,
- find an entry by key using linear scanning,
- insert a non-duplicate entry while capacity is available,
- find the minimum-ranked resident entry using linear scanning,
- remove a known array slot in O(1),
- evict the minimum-ranked entry by composing linear selection with known-slot removal,
- verify that an evicted key is absent,
- insert a new entry after capacity is freed, and
- validate the complete Stage 2 manual oracle,
- perform a complete cache hit through `cache_get()`, and
- perform a complete cache miss including database read, full-cache eviction, insertion, and return,
- validate cache structural invariants, and
- assert valid structural state during development operations,
- count linear lookup calls, hits, misses, and key comparisons, and
- demonstrate lookup work growing linearly with resident cache size,
- initialize and validate a standalone open-addressed hash table,
- insert standalone key-to-entry mappings,
- perform standalone hash-table hit/miss lookup,
- resolve collisions with linear probing,
- delete mappings using tombstones, and
- preserve collision-chain lookup across deleted slots.

At this commit, the program intentionally does **not** implement:

- hash-table integration with the cache lookup path,
- optimized rank ordering,
- dynamic rank updates,
- optimized lookup implementation, or
- concurrent/thread-safe access.

Those capabilities must only be documented after their corresponding implementation stages have actually been completed and validated.

---

## Complexity at the Current Checkpoint

Stage 2 is intentionally array-based and linear.

```text
cache_init()                  O(1)
cache_is_full()               O(1)
cache_lookup()                O(N)
cache_insert()                O(N)
cache_find_min_rank_index()   O(N)
cache_remove_at()             O(1)
cache_evict_min()             O(N)
cache_get() hit               O(N)
cache_get() miss              O(N) + DB-read cost
cache_validate()              O(N^2) debug/correctness check
lookup-stat counter update    O(1) per comparison
hash_table_bucket()           O(1)
hash_table_lookup()           O(1) expected, O(M) worst case
hash_table_insert()           O(1) expected, O(M) worst case
hash_table_remove()           O(1) expected, O(M) worst case
hash table storage            O(M)
additional working space      O(1)
resident cache storage        O(K)
```

Stage 6 directly confirms that a missing cache lookup performs exactly N key comparisons for N resident entries. The first algorithmic bottleneck is therefore the O(N) linear cache lookup.

Stage 7 independently validates a hash table whose expected lookup complexity is O(1) at a controlled load factor, but the cache itself still uses the Stage 6 linear lookup. Therefore the cache complexity has not changed yet. In assertion-enabled builds, the O(N²) invariant validator can still dominate wall-clock runtime; it remains a correctness aid rather than a production lookup mechanism.

---

## Development Rule

The repository follows a strict incremental workflow:

```text
implement one small stage
        ↓
build
        ↓
run
        ↓
validate expected behavior
        ↓
document the completed stage
        ↓
git commit
        ↓
git push
```

The README should be updated only with stages that have actually been completed and validated.

---

## Current Checkpoint

```text
Stage 0
Basic data model
COMPLETE
BUILD PASS
RUN PASS
EXPECTED OUTPUT PASS

Stage 1
Deterministic database abstraction
COMPLETE
BUILD PASS
RUN PASS
EXPECTED OUTPUT PASS

Stage 2
Simplest fixed-size linear cache
COMPLETE
BUILD PASS
RUN PASS
LOOKUP PASS
INSERT PASS
CAPACITY CHECK PASS
MINIMUM-RANK SEARCH PASS
EVICTION PASS
MANUAL ORACLE PASS

Stage 3
Linear minimum-rank eviction
COMPLETE
BUILD PASS
RUN PASS
KNOWN-SLOT REMOVAL PASS
EMPTY EVICTION GUARD PASS
EQUAL-RANK TIE PASS
EVICTION PASS

Stage 4
Complete cache_get()
COMPLETE
BUILD PASS
RUN PASS
CACHE HIT PASS
CACHE MISS PASS
FULL-CACHE EVICTION PASS
DATABASE FETCH PATH PASS
FINAL RESIDENT SET PASS

Stage 5
Assertions and invariants
COMPLETE
BUILD PASS
RUN PASS
STRUCTURAL VALIDATOR PASS
SIZE/CAPACITY INVARIANT PASS
DUPLICATE-KEY INVARIANT PASS
ASSERTION-ENABLED REGRESSION PASS
ASAN/UBSAN PASS

Stage 6
First bottleneck identification
COMPLETE
BUILD PASS
RUN PASS
LOOKUP INSTRUMENTATION PASS
FIRST/MIDDLE/LAST LOOKUP PROFILE PASS
MISSING-LOOKUP SCALING PASS
O(N) LOOKUP BOTTLENECK CONFIRMED
ASAN/UBSAN PASS

Stage 7
Standalone hash-table lookup validation
COMPLETE
BUILD PASS
RUN PASS
HASH INSERT PASS
HASH HIT/MISS LOOKUP PASS
DUPLICATE-KEY REJECTION PASS
COLLISION/LINEAR-PROBING PASS
TOMBSTONE DELETE PASS
TOMBSTONE-CHAIN LOOKUP PASS
TOMBSTONE REUSE PASS
HASH-TABLE VALIDATOR PASS
ASAN/UBSAN PASS
```
