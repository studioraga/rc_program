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

## Stage 8 — Identify the Second Bottleneck

**Status: COMPLETE AND VALIDATED**

Stage 8 does not change the cache algorithm. It instruments the existing
minimum-rank victim-selection path so its cost can be measured exactly.

After Stage 6 identified O(N) key lookup as the first bottleneck and Stage 7
validated a standalone hash table independently, the next remaining linear
operation in the current cache is:

```c
cache_find_min_rank_index()
```

This function selects the eviction victim by scanning every resident entry's
rank.

### Stage 8 instrumentation

Stage 8 adds diagnostic counters:

```c
typedef struct {
    uint64_t min_scan_calls;
    uint64_t rank_comparisons;
} MinRankStats;
```

with:

```c
void min_rank_stats_reset(void);
MinRankStats min_rank_stats_snapshot(void);
```

The counters do not alter rank ordering, eviction policy, or the cache data
structure.

### Why the expected comparison count is N - 1

For a non-empty cache containing `N` resident entries, index `0` becomes the
initial candidate. The function then compares entries `1` through `N - 1`
against the current candidate.

Therefore:

```text
rank comparisons = N - 1
```

regardless of whether the true minimum is located at the beginning, middle,
or end of the resident array.

### Victim-position experiment

For a 100-entry cache, Stage 8 explicitly moves the unique minimum rank to
three different positions.

Expected result:

| Minimum location | Resident entries | Rank comparisons |
|---|---:|---:|
| first | 100 | 99 |
| middle | 100 | 99 |
| last | 100 | 99 |

This demonstrates that finding the minimum cannot terminate early in the
current representation.

### Scaling experiment

Stage 8 measures minimum-rank selection with resident sizes:

```text
1
10
25
50
100
```

Expected exact counts:

| N | Rank comparisons |
|---:|---:|
| 1 | 0 |
| 10 | 9 |
| 25 | 24 |
| 50 | 49 |
| 100 | 99 |

The measured work grows directly with resident size.

### Stage 8 finding

The second algorithmic bottleneck is therefore:

```text
cache_find_min_rank_index() = O(N)
```

The eviction path remains:

```text
minimum-rank victim selection    O(N)
known-slot removal               O(1)
-------------------------------------
overall minimum-rank eviction    O(N)
```

This stage identifies the bottleneck only. It does not introduce a heap,
tree, ordered structure, hash-table/cache integration, or any replacement
victim-selection algorithm.

### Build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache8
```

### Sanitizer build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 -O0 -g3 \
    -fsanitize=address,undefined ranked_cache.c -o ranked_cache8_san
```

### Expected checkpoint result

```text
Second bottleneck identified: O(N) minimum-rank victim selection.
Known-slot removal remains O(1); eviction overall remains O(N).
Stage 8 validation: PASS
```

---

## Stage 9 — Standalone Binary Min-Heap

**Status: COMPLETE AND VALIDATED**

Stage 9 introduces an array-backed binary min-heap and tests it independently.
The heap is deliberately **not connected** to `Cache`, `cache_get()`,
`cache_evict_min()`, or the Stage 7 hash table at this checkpoint.

The purpose is to validate the candidate data structure for efficient
minimum-rank access before any cache integration is attempted.

### Heap representation

```c
typedef struct {
    CacheEntry *items[MAX_CACHE_CAPACITY];
    size_t size;
} MinHeap;
```

The heap stores pointers to existing `CacheEntry` objects. Index `0` is the
minimum-ranked entry whenever the heap invariant holds.

For array index `i`:

```text
parent = (i - 1) / 2
left   = 2*i + 1
right  = 2*i + 2
```

### Ordering rule

Entries are primarily ordered by ascending rank.

For deterministic equal-rank behavior, key is used as a secondary ordering
field:

```text
(rank_a < rank_b)
    or
(rank_a == rank_b and key_a < key_b)
```

This does not change the original cache policy; it only gives the standalone
heap test a deterministic tie result.

### Functions introduced

#### `min_heap_init()`

Initializes an empty heap.

```text
Time:  O(M)
```

where `M = MAX_CACHE_CAPACITY`, because the current educational
implementation clears every pointer slot.

#### `min_heap_entry_less()`

Compares two entries by rank and then key.

```text
Time: O(1)
```

#### `min_heap_swap()`

Swaps two heap pointers.

```text
Time: O(1)
```

#### `min_heap_sift_up()`

Repairs heap order after insertion by repeatedly comparing a node with its
parent and moving it upward when necessary.

```text
Worst case: O(log N)
```

#### `min_heap_sift_down()`

Repairs heap order after root replacement by repeatedly selecting the smaller
child and moving downward when necessary.

```text
Worst case: O(log N)
```

#### `min_heap_push()`

Appends an entry pointer at the end of the heap and restores ordering with
`sift_up`.

```text
Worst case: O(log N)
```

#### `min_heap_peek()`

Returns `items[0]` without removing it.

```text
Time: O(1)
```

This is the key property Stage 9 is intended to validate: the minimum-ranked
entry is directly accessible at the heap root.

#### `min_heap_pop_min()`

Removes the root, moves the final heap element to index `0`, and restores heap
order using `sift_down`.

```text
Worst case: O(log N)
```

#### `min_heap_validate()`

Checks the standalone heap invariant: no child may compare smaller than its
parent.

```text
Time: O(N)
```

This validator is test/debug logic and is not part of the heap operation's
production complexity.

---

## Stage 9 Independent Validation

The focused ordering test inserts entries with ranks:

```text
50, 20, 80, 10, 60, 20
```

The second rank `20` is intentional so equal-rank tie behavior is also tested.

After all insertions:

```text
min_heap_peek() -> key=4 rank=10
```

Repeated `min_heap_pop_min()` calls must produce:

```text
key=4 rank=10
key=2 rank=20
key=6 rank=20
key=1 rank=50
key=5 rank=60
key=3 rank=80
```

The heap invariant is checked after every push and every pop.

Stage 9 additionally validates:

- empty heap initialization,
- `peek` on an empty heap,
- `pop` on an empty heap,
- heap invariant after every insertion,
- heap invariant after every removal,
- deterministic equal-rank ordering,
- complete ascending pop order,
- empty state after all removals,
- accepting exactly `MAX_CACHE_CAPACITY` entries, and
- rejecting insertion beyond capacity.

### Stage 9 finding

The standalone heap demonstrates:

```text
minimum access     O(1)
insert             O(log N)
remove minimum     O(log N)
heap storage       O(K)
```

This directly addresses the data-structure capability that was missing when
Stage 8 proved the existing array-based victim selection was O(N).

However, **Stage 9 does not replace the existing eviction path**.
`cache_find_min_rank_index()` and `cache_evict_min()` continue to behave exactly
as they did in Stage 8.

### Build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache9
```

### Sanitizer build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 \
    -O0 -g3 -fsanitize=address,undefined \
    ranked_cache.c -o ranked_cache9_san
```

### Expected checkpoint result

```text
Stage 8 regression validation: PASS
Stage 9 min-heap validation: PASS
Stage 9 validation: PASS
```


---

## Stage 10 — Combine Hash Table + Min-Heap for Part 1

**Status: COMPLETE AND VALIDATED**

Stage 10 combines the independently validated Stage 7 hash table and Stage 9 binary min-heap into a new fixed-rank Part 1 cache path.

The earlier linear `Cache` implementation remains in the program as a regression/reference implementation. Stage 10 does not rewrite or remove the previous stages.

### Part 1 requirement

For Part 1, an entry's rank does not change while it remains cached.

That allows the integrated structure to use:

```text
HashTable
    key -> CacheEntry *

MinHeap
    minimum rank at heap[0]

Stable resident storage
    CacheEntry entries[MAX_CACHE_CAPACITY]
```

A cache hit therefore needs only a hash lookup. Because rank is fixed, no heap repair is required on a hit.

### Integrated representation

```c
typedef struct {
    CacheEntry entries[MAX_CACHE_CAPACITY];
    unsigned char active[MAX_CACHE_CAPACITY];
    size_t free_stack[MAX_CACHE_CAPACITY];
    size_t free_count;
    size_t size;
    size_t capacity;
    HashTable index;
    MinHeap min_heap;
} Part1Cache;
```

The roles are:

| Field | Purpose |
|---|---|
| `entries` | stable storage for resident `CacheEntry` objects |
| `active` | marks live resident slots |
| `free_stack` | reusable slot indices |
| `free_count` | number of currently available resident slots |
| `size` | number of resident entries |
| `capacity` | logical cache capacity |
| `index` | hash-based key lookup |
| `min_heap` | rank-ordered eviction structure |

Stable entry addresses are important because both the hash table and heap store `CacheEntry *` pointers.

### Free-slot stack

Stage 10 does not compact or move live entries after eviction.

Instead:

```text
insert
  -> pop one free slot
  -> construct resident entry there

 evict
  -> mark victim slot inactive
  -> push its slot index back onto free_stack
```

This preserves pointer stability and makes resident-slot allocation/recycling O(1).

### Functions introduced

#### `part1_cache_init()`

Initializes:

- resident storage,
- active flags,
- free-slot stack,
- hash table,
- min-heap.

#### `part1_cache_is_full()`

Checks whether `size >= capacity`.

```text
Complexity: O(1)
```

#### `part1_cache_lookup()`

Uses the integrated hash table instead of the earlier linear resident scan.

```text
Expected/average: O(1)
Worst case:       O(M)
```

where `M` is the fixed hash-table capacity.

#### `part1_cache_insert()`

Insertion performs:

```text
free-slot allocation      O(1)
hash-table insertion      expected O(1)
min-heap insertion        O(log N)
```

Therefore:

```text
overall expected insertion: O(log N)
```

The heap is the dominant operation.

#### `part1_cache_evict_min()`

Eviction now uses:

```text
min_heap_pop_min()        O(log N)
hash_table_remove()       expected O(1)
free-slot recycle         O(1)
```

The previous O(N) linear minimum-rank scan is not used by the integrated Part 1 path.

Therefore:

```text
overall expected eviction: O(log N)
```

#### `part1_cache_get()`

Part 1 hit path:

```text
hash lookup
    |
    +--> HIT -> return resident entry
```

Expected hit complexity:

```text
O(1)
```

No rank recalculation or heap update occurs because Part 1 ranks remain fixed.

Part 1 miss path:

```text
hash lookup
    |
    +--> MISS
           |
           +--> db_read_entry()
           |
           +--> if full: pop heap minimum
           |
           +--> insert into hash table + heap
           |
           +--> return resident entry
```

Ignoring backing-store latency, expected cache-maintenance complexity is:

```text
O(log N)
```

when eviction/insertion is required.

#### `part1_cache_validate()`

The Stage 10 validator verifies synchronization across all integrated structures:

- resident size is within capacity,
- `size + free_count == capacity`,
- hash-table size equals resident size,
- heap size equals resident size,
- each active resident is found through the hash table,
- each active resident appears exactly once in the heap,
- free slots are inactive and unique,
- every heap pointer refers to a live resident slot,
- hash-table and heap validators both pass.

This function is a correctness/debug aid and is intentionally more expensive than the cache operations themselves.

## Stage 10 Validation

The first integrated Part 1 validation uses capacity 3 and the existing deterministic database abstraction:

```text
GET 1 -> miss -> rank 10
GET 2 -> miss -> rank 20
GET 3 -> miss -> rank 30
GET 2 -> hit, rank remains 20
GET 4 -> miss while full
         heap minimum = key 1 / rank 10
         evict key 1
         insert key 4 / rank 40
```

After filling the cache:

```text
resident size = 3
hash size     = 3
heap size     = 3
```

Before `GET 4`:

```text
heap root = key 1, rank 10
```

After `GET 4`:

```text
key 1 absent
keys 2, 3, 4 resident
heap root = key 2, rank 20
```

The integrated validator passes after insertion, hit handling, and eviction.

### Build

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache10
```

### Sanitizer build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache10_san
```

### Expected checkpoint result

```text
Stage 10 Part 1 finding: hash lookup replaces the O(N) key scan.
Stage 10 Part 1 finding: heap root replaces the O(N) minimum-rank scan.
Stage 10 Part 1 validation: PASS

Stage 10 validation: PASS
```

### Stage 10 boundary

Stage 10 supports **fixed ranks only**.

It intentionally does **not** implement:

- rank recalculation on lookup,
- arbitrary heap priority update,
- indexed heap positions,
- Part 2 dynamic-rank behavior,
- concurrency or thread safety.

Those remain outside this checkpoint.

---

## Stage 11 — Thorough Part 1 Validation

**Status: COMPLETE AND VALIDATED**

Stage 11 changes no `Part1Cache` production algorithm.

Its purpose is to stress the Stage 10 fixed-rank integration across a broader set of functional and structural cases before any Part 2 work begins.

The Stage 10 architecture remains:

```text
stable CacheEntry storage
        |
        +--> HashTable: key -> CacheEntry *
        |
        +--> MinHeap: minimum-ranked CacheEntry *
```

The Stage 11 tests exercise this integrated path directly.

### Test group 1 — empty and partial capacity

Validate:

- empty-cache initialization,
- empty lookup miss,
- empty eviction rejection,
- insertion while capacity is still available,
- synchronized resident/hash/heap counts,
- correct heap minimum before capacity is reached.

Example:

```text
capacity = 4

GET 10 -> miss -> rank 100
GET 20 -> miss -> rank 200

size       = 2
hash size  = 2
heap size  = 2
free slots = 2
minimum    = key 10 / rank 100
```

### Test group 2 — repeated fixed-rank hits

Part 1 requires rank to remain unchanged on lookup.

Stage 11 repeatedly accesses the same resident entry and verifies:

- the same resident pointer is returned,
- the rank remains unchanged,
- cache size remains unchanged,
- hash size remains unchanged,
- heap size remains unchanged,
- the heap root remains unchanged,
- the integrated validator continues to pass.

This confirms that fixed-rank hits do not perform rank maintenance.

### Test group 3 — duplicate-key rejection

Insert:

```text
key   = 7
value = 700
rank  = 70
```

Then attempt to insert another entry with:

```text
key   = 7
value = 7777
rank  = 1
```

The duplicate must be rejected.

The original resident must remain unchanged and all structure counts must remain identical.

### Test group 4 — equal-rank deterministic tie behavior

Stage 11 inserts:

| Key | Rank |
|---:|---:|
| 30 | 50 |
| 10 | 50 |
| 20 | 50 |

The heap's deterministic secondary ordering by key requires:

```text
minimum = key 10 / rank 50
```

Eviction must therefore remove key `10`.

### Test group 5 — integrated hash collisions

Keys:

```text
1
212
423
```

all map to the same initial bucket with:

```text
HASH_TABLE_CAPACITY = 211
```

Stage 11 verifies these collisions inside `Part1Cache`, not merely in the standalone Stage 7 hash table.

The test then evicts the minimum-ranked colliding entry and verifies that lookups for later entries continue across the resulting hash-table tombstone.

### Test group 6 — capacity boundaries

Stage 11 validates:

```text
NULL cache initialization      -> reject
capacity = 0                   -> reject
capacity > MAX_CACHE_CAPACITY  -> reject
capacity = 1                   -> accept
capacity = MAX_CACHE_CAPACITY  -> accept
```

For a one-entry cache:

```text
GET 9
GET 10
```

the second miss must evict key `9`, insert key `10`, and preserve a resident size of exactly one.

The maximum-capacity test fills all `MAX_CACHE_CAPACITY` resident slots, validates the integrated state, and confirms that direct insertion beyond capacity is rejected.

### Test group 7 — stable addresses and free-slot reuse

Because the hash table and heap store `CacheEntry *`, resident addresses must remain stable.

Stage 11 fills a capacity-three cache with keys `1`, `2`, and `3`, captures their resident pointers, and then requests key `4`.

Key `1` is evicted.

The test verifies:

- pointers for keys `2` and `3` remain unchanged,
- key `4` occupies the recycled slot previously used by key `1`,
- the integrated validator passes.

### Test group 8 — repeated evictions

A capacity-five cache is filled with:

```text
keys 1, 2, 3, 4, 5
```

Then:

```text
GET 3 -> hit
GET 5 -> hit
GET 6 -> miss/full
GET 7 -> miss/full
GET 8 -> miss/full
```

Because the deterministic database rank is:

```text
rank = key * 10
```

the successive full-cache misses remove the lowest fixed ranks.

Final resident set:

```text
4, 5, 6, 7, 8
```

and:

```text
heap minimum = key 4 / rank 40
```

The validator is run throughout the sequence.

## Stage 11 Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache11
```

### Run

```bash
./ranked_cache11
```

The Stage 11 section must end with:

```text
Stage 11 Part 1 coverage:
  empty/partial capacity       : tested
  repeated fixed-rank hits     : tested
  duplicate rejection          : tested
  equal-rank deterministic tie : tested
  integrated hash collisions   : tested
  capacity boundaries          : tested
  stable addresses/slot reuse  : tested
  repeated evictions           : tested
Stage 11 Part 1 validation: PASS

Stage 11 validation: PASS
```

### Sanitizer build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache11_san

./ranked_cache11_san
```

The sanitizer build completes with exit status `0` and no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics.

### Stage 11 boundary

Stage 11 remains strictly within **fixed-rank Part 1**.

It does not add:

- dynamic rank recalculation,
- arbitrary heap-priority updates,
- indexed heap positions,
- Part 2 behavior,
- concurrency,
- performance benchmarking changes.


---

## Stage 12 — Begin Part 2: Rank May Change on Lookup

**Status: COMPLETE AND VALIDATED**

Stage 12 begins Part 2 by changing one assumption only:

> After a resident entry is looked up, `getEntryRank(entry)` may return a new rank.

The new rank is not constrained to move in one direction. It may be:

- lower than the existing rank,
- equal to the existing rank, or
- higher than the existing rank.

Stage 12 deliberately does **not** implement arbitrary heap-priority repair yet.

Its purpose is to model the Part 2 contract and prove why the existing ordinary binary heap from Part 1 is insufficient once a resident's rank can change in place.

### Deterministic Stage 12 rank provider

The original problem statement treats `getEntryRank(entry)` as an externally supplied ranking function.

For deterministic testing, Stage 12 introduces:

```c
typedef enum {
    PART2_RANK_DECREASE = 0,
    PART2_RANK_UNCHANGED = 1,
    PART2_RANK_INCREASE = 2
} Part2RankScenario;
```

and:

```c
Rank stage12_get_entry_rank(
    const CacheEntry *entry,
    Part2RankScenario scenario);
```

The Stage 12 test provider returns:

```text
decrease  -> current rank - 30
unchanged -> current rank
increase  -> current rank + 30
```

This is a controlled stand-in for the interview-supplied ranking callback. It exists only to validate all possible rank-change directions.

### Applying a Part 2 rank change

Stage 12 adds:

```c
stage12_apply_rank_change_without_heap_repair()
```

The helper:

1. finds the resident through the integrated hash table,
2. obtains a new rank from the Stage 12 `getEntryRank()` stand-in,
3. writes that rank into the existing resident,
4. deliberately does **not** repair the heap.

This is intentional.

Stage 12 is testing the question:

```text
What happens to the existing Part 1 heap
if a cached resident's priority changes arbitrarily?
```

### Probe cache

The Part 2 experiments begin with a valid three-entry integrated cache:

| Key | Rank |
|---:|---:|
| 1 | 10 |
| 2 | 20 |
| 3 | 30 |

Initially:

```text
heap minimum = key 1 / rank 10
```

and the complete `Part1Cache` validator passes.

---

### Test 1 — Rank decrease may require upward movement

Change:

```text
key 3
rank 30 -> 0
```

The hash table still finds exactly the same resident pointer.

However, the ordinary heap has not been repaired, so its root still reports:

```text
key 1 / rank 10
```

while key `3` now has the smaller rank:

```text
key 3 / rank 0
```

Therefore:

```text
heap root is stale
min_heap_validate() fails
part1_cache_validate() fails
```

This proves that a sufficiently lower arbitrary rank may require the resident to move **upward** in the heap.

Stage 12 then restores key `3` to rank `30`, after which the integrated validator passes again.

---

### Test 2 — Rank increase may require downward movement

Begin again from the valid probe cache:

```text
key 1 rank 10
key 2 rank 20
key 3 rank 30
```

Change the heap root:

```text
key 1
rank 10 -> 40
```

Without heap repair, key `1` remains at the root even though key `2` with rank `20` should now precede it.

Therefore:

```text
heap root is stale
min_heap_validate() fails
part1_cache_validate() fails
```

This proves that a sufficiently higher arbitrary rank may require the resident to move **downward** in the heap.

The original rank is then restored and the integrated cache validates again.

---

### Test 3 — Unchanged rank needs no repair

Change:

```text
key 2
rank 20 -> 20
```

The heap remains valid because the ordering relation did not change.

The tests verify:

```text
same rank
same heap root
min_heap_validate() passes
part1_cache_validate() passes
```

---

## Stage 12 Findings

Stage 12 establishes these Part 2 facts:

```text
hash lookup:
    still finds resident by key
    expected O(1)

rank update:
    may decrease
    may stay the same
    may increase

ordinary heap:
    does not automatically know which resident changed
    does not automatically repair itself

rank decreases:
    may require movement toward the root

rank increases:
    may require movement away from the root

unchanged rank:
    requires no heap movement
```

The key Stage 12 conclusion is:

> Part 2 requires an efficient way to locate the changed resident inside the heap and repair its position in either direction.

That problem is intentionally **not solved in Stage 12**.

---

## Stage 12 Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache12
```

### Run

```bash
./ranked_cache12
```

The Stage 12 section must end with:

```text
Stage 12 Part 2 findings:
  getEntryRank may move rank lower, equal, or higher.
  hash lookup still finds the resident in O(1) expected time.
  an ordinary heap does not self-repair after arbitrary rank change.
  decrease may require upward heap movement.
  increase may require downward heap movement.
  unchanged rank requires no heap movement.
Stage 12 conclusion: arbitrary resident priority update must be solved next.
Stage 12 Part 2 contract validation: PASS

Stage 12 validation: PASS
```

### Sanitizer build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache12_san

./ranked_cache12_san
```

The sanitizer build completes with exit status `0` and no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics.

### Stage 12 boundary

Stage 12 does **not** add:

- heap indices stored in cache entries,
- arbitrary heap-element lookup,
- heap priority update,
- indexed-heap repair,
- Part 2 production `cache_get()` behavior,
- concurrency,
- new performance optimization.

This checkpoint only establishes and validates the Part 2 problem that the next incremental stages must solve.

---

## Stage 13 — Recognize Why the Ordinary Heap Is Insufficient

**Status: COMPLETE AND VALIDATED**

Stage 12 proved that changing the rank of an arbitrary cached resident can make the ordinary Part 1 heap stale.

Stage 13 isolates the next missing capability:

> The hash table can locate a resident by key, but the ordinary heap does not record where that resident is stored in the heap array.

The current `MinHeap` contains:

```c
typedef struct {
    CacheEntry *items[MAX_CACHE_CAPACITY];
    size_t size;
} MinHeap;
```

It has no reverse mapping such as:

```text
CacheEntry * -> heap index
```

Therefore, after the hash table returns a `CacheEntry *`, the only general way to discover that resident's heap position with the current structure is to scan:

```text
heap.items[0]
heap.items[1]
...
heap.items[N-1]
```

This Stage 13 checkpoint measures that cost without adding any indexed-heap solution.

### Diagnostic heap-location instrumentation

Stage 13 introduces:

```c
typedef struct {
    uint64_t locate_calls;
    uint64_t pointer_comparisons;
    uint64_t locate_hits;
    uint64_t locate_misses;
} HeapLocateStats;
```

with:

```c
heap_locate_stats_reset()
heap_locate_stats_snapshot()
stage13_find_heap_index_linear()
```

`stage13_find_heap_index_linear()` is deliberately a **diagnostic helper**, not a production priority-update API.

Its behavior is:

```text
scan heap.items[] from index 0

if heap.items[i] == target:
    return i

otherwise:
    continue until heap.size
```

Current complexity:

```text
best case:  O(1)
worst hit:  O(N)
miss:       O(N)
```

### Deterministic positioned heap

To measure exact position cost, Stage 13 builds a valid heap with monotonically increasing key and rank:

```text
heap[0]  -> key 1   rank 1
heap[1]  -> key 2   rank 2
...
heap[99] -> key 100 rank 100
```

This layout is a valid min-heap because every parent has a smaller rank than its children.

The direct layout is test scaffolding only. It allows exact heap-position measurements without mixing heap-construction cost into the location experiment.

### Position experiment

For a 100-entry heap:

```text
target position    pointer comparisons

first              1
middle             50
last               100
missing            100
```

Validated output:

```text
[PROFILE] first heap entry   size=100 comparisons=1   result=FOUND index=0
[PROFILE] middle heap entry  size=100 comparisons=50  result=FOUND index=49
[PROFILE] last heap entry    size=100 comparisons=100 result=FOUND index=99
[PROFILE] missing heap entry size=100 comparisons=100 result=MISS
```

This demonstrates that the ordinary heap provides no direct arbitrary-entry access.

### Scaling experiment

Stage 13 also searches for a missing pointer with heap sizes:

```text
1
10
25
50
100
```

The comparison counts are exactly:

```text
heap size    pointer comparisons

1            1
10           10
25           25
50           50
100          100
```

Therefore:

```text
missing arbitrary-entry location cost = N pointer comparisons
```

which confirms:

```text
O(N)
```

heap-location complexity for the current ordinary heap.

### Hash lookup versus heap location

Stage 13 then combines the observation with the integrated Part 1 cache.

A valid 100-entry `Part1Cache` is constructed with monotonically increasing ranks.

The hash table can directly return:

```text
key 100 -> CacheEntry *
```

using the Stage 10 hash index.

But the ordinary heap still has no stored position for that resident.

The diagnostic scan reports:

```text
hash found key 100 resident
ordinary heap location comparisons = 100
heap index = 99
```

So the complete Part 2 problem at this checkpoint is:

```text
key
 |
 v
HashTable
 |
 | expected O(1)
 v
CacheEntry *
 |
 | ordinary heap has no reverse position
 v
linear heap scan
 |
 | O(N)
 v
heap index
```

This defeats the intended efficient arbitrary-priority update path.

## Stage 13 Conclusion

The current structures solve different parts of the problem:

```text
HashTable:
    key -> CacheEntry *
    expected O(1)

MinHeap:
    minimum -> heap[0]
    O(1)

MinHeap push/pop:
    O(log N)

Missing capability:
    CacheEntry * -> heap index
```

Without that missing mapping, an arbitrary resident rank update would require:

```text
hash lookup              expected O(1)
linear heap location     O(N)
heap repair              O(log N)
```

and therefore remains dominated by:

```text
O(N)
```

Stage 13 establishes the requirement:

> Part 2 needs a direct resident-to-heap-index mapping before arbitrary rank repair can be efficient.

That mapping is intentionally **not implemented in Stage 13**.

---

## Stage 13 Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache13
```

### Run

```bash
./ranked_cache13
```

The Stage 13 section must end with:

```text
Stage 13 findings:
  hash table maps key -> CacheEntry * in expected O(1).
  ordinary MinHeap stores CacheEntry * but no reverse heap position.
  locating an arbitrary resident therefore requires scanning heap.items[].
  first/middle/last lookup costs 1/50/100 comparisons in a 100-entry heap.
  a missing resident requires N pointer comparisons for heap size N.
  after hash lookup, worst-case heap location is still O(N).
Stage 13 conclusion: Part 2 needs a direct resident -> heap-index mapping.
Stage 13 ordinary-heap limitation validation: PASS

Stage 13 validation: PASS
```

### Sanitizer build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache13_san

./ranked_cache13_san
```

The sanitizer build completes with exit status `0` and no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics.

### Stage 13 boundary

Stage 13 does **not** introduce:

- a `heap_index` field in `CacheEntry`,
- a reverse heap-position table,
- indexed-heap swap maintenance,
- arbitrary priority repair,
- production Part 2 `cache_get()` behavior,
- any sift-up/sift-down update decision for an arbitrary resident.

It only proves why those capabilities are necessary.

---

## Stage 14 — Introduce `heap_index`

**Status: COMPLETE AND VALIDATED**

Stage 13 proved that the current ordinary heap has no direct way to answer:

```text
CacheEntry * -> heap array index
```

and therefore requires an `O(N)` pointer scan to locate an arbitrary resident.

Stage 14 introduces the missing reverse-position metadata directly into `CacheEntry`.

### Updated cache-entry representation

```c
#define HEAP_INDEX_NONE SIZE_MAX

typedef struct {
    CacheKey key;
    unsigned long long value;
    Rank rank;
    size_t heap_index;
} CacheEntry;
```

`heap_index` has two meanings:

```text
0 .. heap->size - 1
    entry is resident in the heap at that exact array index

HEAP_INDEX_NONE
    entry is not currently resident in a heap
```

At this checkpoint, `heap_index` is **metadata only**.

Stage 14 does not yet use it to repair an arbitrary priority/rank change.

---

### Heap operations now maintain the reverse index

#### `min_heap_push()`

Before sift-up:

```c
index = heap->size;
heap->items[index] = entry;
entry->heap_index = index;
++heap->size;

min_heap_sift_up(heap, index);
```

Any swaps caused by sift-up update the metadata.

Complexity remains:

```text
O(log N)
```

#### `min_heap_swap()`

A swap now updates both moved entries:

```c
heap->items[a]->heap_index = a;
heap->items[b]->heap_index = b;
```

This establishes the Stage 14 invariant:

```text
heap->items[i]->heap_index == i
```

for every resident heap entry.

The swap itself remains:

```text
O(1)
```

#### `min_heap_pop_min()`

When the root is removed:

1. move the final heap item to index `0`,
2. set the moved entry's `heap_index` to `0`,
3. sift downward,
4. update indices during every swap,
5. mark the removed entry as:

```c
HEAP_INDEX_NONE
```

Therefore a popped entry no longer claims to belong to the heap.

Complexity remains:

```text
O(log N)
```

---

### Heap validator enhancement

`min_heap_validate()` now checks both:

1. ordinary min-heap ordering, and
2. reverse-index consistency.

For every resident position:

```c
heap->items[i] != NULL
heap->items[i]->heap_index == i
```

A corrupted `heap_index` therefore causes validation failure even when rank ordering itself is still correct.

---

### Integrated Part 1 cache compatibility

`Part1Cache` already keeps `CacheEntry` objects at stable addresses.

Stage 14 initializes an inserted resident with:

```c
resident->heap_index = HEAP_INDEX_NONE;
```

before it enters the heap.

`min_heap_push()` then assigns the actual position.

`part1_cache_validate()` is extended to verify:

```text
resident->heap_index < min_heap.size
min_heap.items[resident->heap_index] == resident
```

in addition to the earlier hash-table/heap/resident consistency checks.

This keeps the Stage 10/11 integrated cache valid while adding the new Part 2 metadata.

---

## Stage 14 Validation

### Test 1 — push/sift/swap index maintenance

Entries with ranks:

```text
50
20
80
10
60
```

are pushed into the heap.

After every push, Stage 14 verifies:

```text
heap.items[i]->heap_index == i
```

for every resident.

Because lower-rank entries bubble upward, this exercises the updated `min_heap_swap()` path rather than merely checking append positions.

The final minimum entry:

```text
key 4
rank 10
```

must record:

```text
heap_index = 0
```

---

### Test 2 — pop invalidation and survivor repair

A six-entry heap is repeatedly popped until empty.

For every removal Stage 14 verifies:

```text
removed->heap_index == HEAP_INDEX_NONE
```

and for every survivor:

```text
heap.items[i]->heap_index == i
```

after sift-down.

This validates both sides of the reverse-position lifecycle:

```text
outside heap
    HEAP_INDEX_NONE

push
    valid array index

swaps
    updated array index

pop
    HEAP_INDEX_NONE
```

---

### Test 3 — direct hash-to-heap location

A valid 100-entry integrated cache is built.

The hash table returns:

```text
key 100 -> CacheEntry *
```

Stage 13 then required a linear heap scan to recover:

```text
heap index = 99
```

Stage 14 obtains it directly:

```c
index = resident->heap_index;
```

Validated result:

```text
key 100 -> resident -> heap_index 99
```

and:

```c
cache.min_heap.items[index] == resident
```

This changes the location step from Stage 13's:

```text
resident -> heap index = O(N)
```

to:

```text
resident -> heap index = O(1)
```

metadata access.

No priority repair is performed yet.

---

### Test 4 — deliberate reverse-index corruption

Stage 14 deliberately changes one valid resident's metadata to:

```c
HEAP_INDEX_NONE
```

while leaving it physically inside the heap.

The tests verify:

```text
min_heap_validate()    -> fail
part1_cache_validate() -> fail
```

After restoring the saved index:

```text
min_heap_validate()    -> pass
part1_cache_validate() -> pass
```

This proves the new invariant is actively checked rather than merely stored.

---

## Stage 14 Complexity

The production heap complexities do not change:

```text
min_heap_peek()      O(1)
min_heap_swap()      O(1)
min_heap_push()      O(log N)
min_heap_pop_min()   O(log N)
```

The new reverse position gives:

```text
CacheEntry * -> heap index    O(1)
```

instead of the Stage 13 diagnostic:

```text
CacheEntry * -> heap index    O(N)
```

However, Stage 14 intentionally stops before implementing:

```text
new rank
   |
   v
choose sift-up or sift-down
   |
   v
repair arbitrary resident
```

That remains a later incremental step.

---

## Stage 14 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache14
```

The build should complete with no warnings.

Run:

```bash
./ranked_cache14
```

The Stage 14 section must end with:

```text
Stage 14 findings:
  each heap-resident CacheEntry now stores its current heap_index.
  min_heap_push assigns heap_index before sift-up.
  min_heap_swap updates both moved entries.
  min_heap_pop_min invalidates the removed entry index.
  surviving entries keep indices synchronized after sift-down.
  hash lookup can now reach heap position directly through resident->heap_index.
Stage 14 boundary: heap_index is maintained but not yet used for priority repair.
Stage 14 heap_index validation: PASS

Stage 14 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache14_ubsan

./ranked_cache14_ubsan
```

This checkpoint was validated successfully with UBSan.

### AddressSanitizer + UndefinedBehaviorSanitizer on Node1

Run on the Node1 Ubuntu environment:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache14_san

./ranked_cache14_san
```

The current execution sandbox cannot reserve the AddressSanitizer shadow-memory region, so ASan could not be truthfully validated here. The normal build and UBSan build both pass. Run the combined ASan/UBSan command above on Node1 before committing if you want to preserve the same sanitizer gate used in prior stages.

---

### Stage 14 boundary

Stage 14 does **not** implement:

- arbitrary rank update,
- indexed priority repair,
- `min_heap_update_rank()`,
- sift-up/sift-down selection after a rank change,
- production Part 2 `cache_get()`,
- dynamic-rank eviction behavior.

It only introduces and validates the `heap_index` reverse mapping.

---

## Stage 15 — Modify `min_heap_swap()` Carefully

**Status: COMPLETE AND VALIDATED**

Stage 14 introduced `CacheEntry.heap_index` and made the heap maintain the reverse-position metadata.

Stage 15 focuses on one operation only:

```c
min_heap_swap()
```

This operation is the consistency primitive that every future indexed-heap repair will depend on.

The Stage 14 implementation already updated both `heap_index` values after a swap, but it assumed:

- `heap` was non-NULL,
- both indices were inside `heap->size`,
- both heap slots contained valid `CacheEntry *`,
- self-swap behavior did not need to be stated explicitly.

Stage 15 hardens those assumptions before any arbitrary-rank repair is allowed to depend on this function.

### Updated swap interface

Stage 15 changes:

```c
void min_heap_swap(...)
```

to:

```c
int min_heap_swap(
    MinHeap *heap,
    size_t a,
    size_t b);
```

Return value:

```text
1 -> swap request is valid and completed
0 -> request is invalid and no swap was performed
```

### Validation before mutation

The function now rejects the request before touching heap state when:

```text
heap == NULL
a >= heap->size
b >= heap->size
heap->items[a] == NULL
heap->items[b] == NULL
```

This is important because a partial indexed-heap update would be worse than a clean failure.

For an invalid request:

```text
heap.items[]      unchanged
entry heap_index  unchanged
```

### Explicit self-swap behavior

When:

```text
a == b
```

the operation is a valid no-op.

Stage 15 reasserts:

```c
heap->items[a]->heap_index = a;
```

and returns success.

No pointer movement occurs.

### Successful two-entry swap

For two distinct valid positions:

```text
before:

heap[a] -> entry_a
entry_a.heap_index = a

heap[b] -> entry_b
entry_b.heap_index = b
```

Stage 15 commits:

```text
after:

heap[a] -> entry_b
entry_b.heap_index = a

heap[b] -> entry_a
entry_a.heap_index = b
```

The array ownership and reverse metadata therefore move together.

### Sift callers

`min_heap_sift_up()` and `min_heap_sift_down()` continue to use the same swap primitive.

They now acknowledge the swap return value:

```c
if (!min_heap_swap(...)) {
    return;
}
```

A valid heap should never trigger this failure path, but the code no longer assumes a malformed swap can safely continue.

---

## Stage 15 Validation

### Test 1 — non-adjacent swap

A valid five-entry heap is created.

Positions `1` and `4` are exchanged.

Stage 15 verifies:

```text
heap[1] == previous heap[4]
previous heap[4].heap_index == 1

heap[4] == previous heap[1]
previous heap[1].heap_index == 4
```

The entries are then swapped back and the full heap validator must pass.

### Test 2 — parent/child swap

Positions `0` and `1` are exchanged.

The test verifies that both pointers and both reverse positions are updated exactly.

The swap is reversed before validating min-heap ordering.

### Test 3 — self-swap

For:

```c
min_heap_swap(&heap, 1, 1)
```

Stage 15 verifies:

```text
same pointer
same heap_index
heap remains valid
```

### Test 4 — invalid indices

Stage 15 verifies rejection of:

```text
NULL heap
left index == heap->size
right index == heap->size
```

After every rejected request:

```text
heap array is unchanged
heap_index metadata is unchanged
heap remains valid
```

### Test 5 — malformed NULL slot

A controlled test temporarily places `NULL` into one active heap slot.

`min_heap_swap()` must reject the operation without updating the other entry or the saved entry's `heap_index`.

After restoring the test slot, the heap validator must pass.

---

## Stage 15 Complexity

The swap remains:

```text
O(1)
```

The added validation consists only of constant-time pointer, bound, and slot checks.

Heap operation complexity therefore remains:

```text
min_heap_swap()      O(1)
min_heap_sift_up()   O(log N)
min_heap_sift_down() O(log N)
min_heap_push()      O(log N)
min_heap_pop_min()   O(log N)
```

Stage 15 does not implement arbitrary priority updates.

---

## Stage 15 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache15

./ranked_cache15
```

Expected final lines:

```text
Stage 15 findings:
  swap validates heap, bounds, and resident slots before mutation.
  successful swaps update both heap array positions and both heap_index values.
  self-swap is an explicit safe no-op.
  invalid swaps fail without partially changing heap metadata.
  sift-up/down continue to use the same consistency-preserving swap primitive.
Stage 15 boundary: swap is hardened; arbitrary rank repair is still not implemented.
Stage 15 heap-swap validation: PASS

Stage 15 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache15_ubsan

./ranked_cache15_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache15_san

./ranked_cache15_san
```

Both sanitizer builds pass for this checkpoint.

---

### Stage 15 boundary

Stage 15 deliberately does **not** implement:

- arbitrary rank change repair,
- `min_heap_update_rank()`,
- choosing sift-up versus sift-down after a rank change,
- production Part 2 `cache_get()`,
- dynamic-rank eviction behavior.

It only makes `min_heap_swap()` a safer, explicitly validated indexed-heap consistency primitive.

---

## Stage 16 — Write `min_heap_update_rank()`

**Status: COMPLETE AND VALIDATED**

Stage 14 introduced `CacheEntry.heap_index`.

Stage 15 hardened `min_heap_swap()` so every heap movement keeps pointer ownership and reverse indices synchronized.

Stage 16 now uses those two prerequisites to implement the first efficient arbitrary-resident rank-repair primitive:

```c
int min_heap_update_rank(
    MinHeap *heap,
    CacheEntry *entry,
    Rank new_rank);
```

This stage does **not** yet wire dynamic ranking into production `cache_get()`.

It implements and validates the heap operation only.

### Membership validation

Before changing the rank, `min_heap_update_rank()` verifies:

```text
heap != NULL
entry != NULL
entry->heap_index != HEAP_INDEX_NONE
entry->heap_index < heap->size
heap->items[entry->heap_index] == entry
```

Therefore the operation does not need to scan `heap.items[]`.

The resident position is obtained directly from:

```c
entry->heap_index
```

which is:

```text
O(1)
```

### Update direction

The function captures the old rank before mutation.

Then:

```text
new_rank < old_rank
    |
    +--> assign new rank
    +--> min_heap_sift_up()

new_rank > old_rank
    |
    +--> assign new rank
    +--> min_heap_sift_down()

new_rank == old_rank
    |
    +--> no heap movement
```

Because only the rank changes and the key remains fixed, this directional choice is sufficient for the existing rank/key ordering rule.

Worst-case repair complexity:

```text
O(log N)
```

### Why `heap_index` matters

Before Stage 14/16, an arbitrary priority update would require:

```text
key -> resident          expected O(1)
resident -> heap index   O(N)
heap repair              O(log N)
```

Stage 16 changes the middle step to:

```text
resident -> heap index   O(1)
```

so the heap update primitive becomes:

```text
O(log N)
```

worst case.

---

## Stage 16 Validation

### Test 1 — lower rank sifts upward

A seven-entry heap starts with:

```text
key 7
rank 70
heap_index 6
```

Stage 16 updates:

```text
70 -> 5
```

The entry must move upward until:

```text
heap_index = 0
heap root = key 7 / rank 5
```

The tests then verify:

```text
min_heap_validate() passes
all heap_index values match physical positions
```

### Test 2 — higher rank sifts downward

The root starts as:

```text
key 1
rank 10
heap_index 0
```

Stage 16 updates:

```text
10 -> 100
```

The former root must move downward.

The new root becomes:

```text
rank 20
```

and the full heap plus reverse-index invariants must remain valid.

### Test 3 — unchanged rank

For a resident with rank `40`:

```text
40 -> 40
```

Stage 16 verifies:

```text
same heap_index
same heap root
heap remains valid
```

No sift operation is required.

### Test 4 — equal-rank tie ordering

The heap contains:

```text
key 10 rank 10
key 20 rank 20
key 5  rank 30
```

Update:

```text
key 5 rank 30 -> 10
```

Now key `5` ties key `10` on rank.

The existing secondary ordering rule uses the lower key, so:

```text
key 5 / rank 10
```

must become the heap root.

This validates that `min_heap_update_rank()` preserves the existing deterministic rank/key ordering rather than only comparing numeric ranks at the final position.

### Test 5 — invalid updates are non-destructive

Stage 16 rejects:

```text
NULL heap
NULL entry
entry not resident in the heap
entry with HEAP_INDEX_NONE
entry whose heap_index points to a different heap slot
```

For rejected requests, the test verifies:

```text
entry rank unchanged
heap state unchanged
```

After restoring controlled test metadata, the heap validator must still pass.

### Test 6 — integrated hash lookup plus indexed rank update

A five-entry integrated `Part1Cache` is filled.

The hash table locates:

```text
key 5 -> CacheEntry *
```

with original rank:

```text
50
```

Stage 16 then calls:

```c
min_heap_update_rank(
    &cache.min_heap,
    resident,
    5);
```

The resident reaches:

```text
heap_index = 0
heap root = key 5 / rank 5
```

without any Stage 13 linear heap-location scan.

Finally:

```c
part1_cache_validate(&cache)
```

must pass.

---

## Stage 16 Complexity

```text
membership check through heap_index   O(1)

unchanged-rank update                 O(1)

rank decrease + sift-up               O(log N)

rank increase + sift-down             O(log N)

overall min_heap_update_rank()        O(log N) worst case
```

The existing operations remain:

```text
min_heap_swap()      O(1)
min_heap_peek()      O(1)
min_heap_push()      O(log N)
min_heap_pop_min()   O(log N)
```

---

## Stage 16 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache16

./ranked_cache16
```

Expected final Stage 16 lines:

```text
Stage 16 findings:
  heap_index locates the resident directly in O(1).
  lower rank repairs with sift-up.
  higher rank repairs with sift-down.
  unchanged rank requires no heap movement.
  hardened swaps keep heap_index synchronized during repair.
  arbitrary resident rank repair is O(log N) worst case.
Stage 16 boundary: heap_update_rank exists but production Part 2 cache_get is not wired yet.
Stage 16 heap-update-rank validation: PASS

Stage 16 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache16_ubsan

./ranked_cache16_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache16_san

./ranked_cache16_san
```

Both sanitizer builds pass for this checkpoint.

---

### Stage 16 boundary

Stage 16 deliberately does **not** implement:

- production Part 2 `cache_get()`,
- calling `getEntryRank()` automatically on every cache hit,
- dynamic-rank eviction behavior through the public cache API,
- rollback around an external rank provider,
- concurrency.

The stage implements only the indexed heap rank-update primitive.

---

## Stage 17 — Test Rank Decrease Independently

**Status: COMPLETE AND VALIDATED**

Stage 16 implemented:

```c
min_heap_update_rank()
```

with three branches:

```text
new rank < old rank  -> sift up
new rank = old rank  -> no movement
new rank > old rank  -> sift down
```

Stage 17 changes **no production heap algorithm**.

Its purpose is to isolate the rank-decrease branch and validate it more thoroughly before moving to the separate rank-increase test stage.

The production function under test remains unchanged.

---

### Test 1 — decrease with no movement

A seven-entry heap contains:

```text
key 7
rank 70
heap_index 6
parent rank 30
```

Update:

```text
70 -> 65
```

Although the rank became smaller, it is still larger than the parent rank.

Therefore:

```text
heap_index remains 6
```

This validates an important rule:

> A rank decrease does not automatically imply a heap swap.

The heap and every reverse index must remain valid.

---

### Test 2 — decrease exactly one level

Start again with:

```text
key 7
rank 70
heap_index 6
parent at index 2 with rank 30
root rank 10
```

Update:

```text
70 -> 25
```

The new rank is smaller than the parent rank `30`, so the resident crosses one parent:

```text
heap_index 6 -> 2
```

But rank `25` is still greater than root rank `10`, so repair stops there.

Expected result:

```text
key 7
rank 25
heap_index 2
```

This validates a bounded one-level sift-up path.

---

### Test 3 — decrease through multiple levels to root

Update the same leaf-style resident:

```text
70 -> 5
```

The new rank is lower than every ancestor.

Expected result:

```text
heap_index 6 -> 2 -> 0
heap root = key 7 / rank 5
```

The heap-order and `heap_index` invariants must both remain valid after the multi-level repair.

---

### Test 4 — equal-rank key tie during decrease

The test heap contains:

```text
key 10 rank 10
key 20 rank 20
key 30 rank 30
key 5  rank 40
```

Key `5` begins below key `20`.

Update:

```text
key 5
rank 40 -> 20
```

The new rank ties the parent:

```text
key 5  rank 20
key 20 rank 20
```

The existing deterministic secondary ordering uses the lower key, so key `5` must cross key `20`.

Expected result:

```text
key 5 -> heap_index 1
```

It then stops below the root because:

```text
root key 10 rank 10
```

still has a lower rank.

This validates that the rank-decrease path preserves the complete rank/key ordering rule.

---

### Test 5 — repeated decreases use the updated reverse index

A resident begins at:

```text
heap_index 6
rank 70
```

First update:

```text
70 -> 25
heap_index 6 -> 2
```

Second update on the **same resident**:

```text
25 -> 5
heap_index 2 -> 0
```

The second operation must use the newly maintained:

```c
entry->heap_index
```

rather than relying on the original position.

This verifies the Stage 14/15 reverse-index maintenance is sufficient for repeated indexed updates.

---

### Test 6 — integrated hash-found decrease

A five-entry integrated `Part1Cache` contains:

```text
key 1 rank 10
key 2 rank 20
key 3 rank 30
key 4 rank 40
key 5 rank 50
```

The hash table locates:

```text
key 5 -> CacheEntry *
```

Then Stage 17 calls:

```c
min_heap_update_rank(
    &cache.min_heap,
    resident,
    5);
```

Expected result:

```text
key 5
rank 5
heap_index 0
heap root = key 5
```

The test also verifies:

```text
hash lookup still returns the same resident
part1_cache_validate() passes
```

No linear heap scan is required.

---

## Stage 17 Findings

The independent decrease tests establish:

```text
smaller rank may require:
    no movement
    one-level movement
    multi-level movement

repair direction:
    upward only

reverse position:
    heap_index remains synchronized after every swap

tie behavior:
    equal ranks still use key ordering

repeated updates:
    use the newly updated heap_index

integrated lookup:
    hash-found resident can be decreased directly
```

The asymptotic complexity remains:

```text
membership / heap position   O(1)
rank decrease repair         O(log N) worst case
```

---

## Stage 17 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache17

./ranked_cache17
```

Expected Stage 17 ending:

```text
Stage 17 findings:
  a smaller rank does not always require movement.
  when needed, decrease repair moves only upward.
  one-level and multi-level sift-up paths both preserve heap_index.
  equal-rank ordering still uses the deterministic key tie-break.
  repeated decreases use the resident's newly maintained heap_index.
  a hash-found resident can be decreased and repaired without a heap scan.
Stage 17 boundary: only rank-decrease behavior is expanded here.
Stage 17 rank-decrease validation: PASS

Stage 17 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache17_ubsan

./ranked_cache17_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache17_san

./ranked_cache17_san
```

Both sanitizer builds pass.

---

### Stage 17 boundary

Stage 17 deliberately does **not** add:

- new production rank-update logic,
- new sift-down behavior,
- a production Part 2 `cache_get()`,
- automatic `getEntryRank()` invocation,
- dynamic-rank eviction through the public cache API.

It only expands validation of the rank-decrease branch already introduced in Stage 16.

---

## Stage 18 — Test Rank Increase Independently

**Status: COMPLETE AND VALIDATED**

Stage 16 implemented:

```c
min_heap_update_rank()
```

and Stage 17 independently stress-tested only the rank-decrease/sift-up branch.

Stage 18 changes **no production heap algorithm**.

Its purpose is to isolate the opposite branch:

```text
new rank > old rank
```

and validate the sift-down behavior independently before any production Part 2 cache integration.

---

### Test 1 — increase with no movement

A seven-entry heap contains:

```text
key 2
rank 20
heap_index 1
children ranks 40 and 50
```

Update:

```text
20 -> 35
```

Although the rank became larger, `35` is still smaller than both child ranks.

Therefore:

```text
heap_index remains 1
```

This validates an important rule:

> A rank increase does not automatically imply a heap swap.

The heap and all reverse indices remain valid.

---

### Test 2 — increase exactly one level

Start with the heap root:

```text
key 1
rank 10
heap_index 0
children ranks 20 and 30
```

Update:

```text
10 -> 25
```

The new rank is larger than the smaller child rank `20`, so the resident crosses exactly one child:

```text
heap_index 0 -> 1
```

At index `1`, its new children have ranks `40` and `50`, so rank `25` is already in a valid position.

Expected result:

```text
key 1
rank 25
heap_index 1
```

---

### Test 3 — increase through multiple levels

Update the root:

```text
10 -> 100
```

The entry must repeatedly swap with the smaller child:

```text
heap_index 0 -> 1 -> 3
```

Expected result:

```text
key 1
rank 100
heap_index 3
```

The new heap root becomes:

```text
rank 20
```

The heap-order and `heap_index` invariants remain valid after the multi-level repair.

---

### Test 4 — equal-rank child tie during increase

The test heap begins with:

```text
key 30 rank 10
key 20 rank 20
key 10 rank 20
```

Then update the root:

```text
key 30
rank 10 -> 20
```

Now the target and both children all have rank `20`.

The existing comparator uses key as the deterministic secondary ordering.

Among the two children:

```text
key 10 < key 20
```

and both are also smaller than target key `30`.

Therefore sift-down must choose:

```text
key 10
```

as the new root.

Expected result:

```text
heap root = key 10 / rank 20
target key 30 -> heap_index 2
```

This validates that the increase path preserves the full `(rank, key)` ordering rule when choosing the downward child.

---

### Test 5 — repeated increases use the updated reverse index

The root resident begins at:

```text
heap_index 0
rank 10
```

First update:

```text
10 -> 25
heap_index 0 -> 1
```

Second update on the **same resident**:

```text
25 -> 100
heap_index 1 -> 3
```

The second operation must use the newly maintained:

```c
entry->heap_index
```

rather than assuming the old root position.

This verifies that repeated sift-down repairs compose correctly.

---

### Test 6 — integrated hash-found increase

A five-entry integrated `Part1Cache` contains:

```text
key 1 rank 10
key 2 rank 20
key 3 rank 30
key 4 rank 40
key 5 rank 50
```

The hash table locates:

```text
key 1 -> CacheEntry *
```

The resident begins at the heap root.

Stage 18 then calls:

```c
min_heap_update_rank(
    &cache.min_heap,
    resident,
    100);
```

Expected result:

```text
key 1
rank 100
heap_index 3

new heap root = key 2 / rank 20
```

The test also verifies:

```text
hash lookup still returns the same resident
part1_cache_validate() passes
```

No linear heap scan is required.

---

## Stage 18 Findings

The independent increase tests establish:

```text
larger rank may require:
    no movement
    one-level movement
    multi-level movement

repair direction:
    downward only

child selection:
    uses complete rank/key ordering

reverse position:
    heap_index remains synchronized after every swap

repeated updates:
    use the newly updated heap_index

integrated lookup:
    hash-found resident can be increased directly
```

The asymptotic complexity remains:

```text
membership / heap position   O(1)
rank increase repair         O(log N) worst case
```

---

## Stage 18 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache18

./ranked_cache18
```

Expected Stage 18 ending:

```text
Stage 18 findings:
  a higher rank does not always require movement.
  when needed, increase repair moves only downward.
  one-level and multi-level sift-down paths both preserve heap_index.
  equal-rank child selection still uses the deterministic key tie-break.
  repeated increases use the resident's newly maintained heap_index.
  a hash-found resident can be increased and repaired without a heap scan.
Stage 18 boundary: only rank-increase behavior is expanded here.
Stage 18 rank-increase validation: PASS

Stage 18 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache18_ubsan

./ranked_cache18_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache18_san

./ranked_cache18_san
```

Both sanitizer builds pass.

---

### Stage 18 boundary

Stage 18 deliberately does **not** add:

- new production rank-update logic,
- new sift-up behavior,
- production Part 2 `cache_get()`,
- automatic `getEntryRank()` invocation,
- dynamic-rank eviction through the public cache API.

It only expands validation of the rank-increase branch already introduced in Stage 16.

---

## Stage 19 — Integrate Dynamic Rank Into Cache Hits

**Status: COMPLETE AND VALIDATED**

Stage 16 introduced `min_heap_update_rank()`.

Stage 17 independently validated rank decreases.

Stage 18 independently validated rank increases.

Stage 19 now integrates that already-tested heap-update primitive into a **separate Part 2 cache-hit path**.

The existing fixed-rank API:

```c
part1_cache_get()
```

remains unchanged.

Stage 19 introduces:

```c
typedef Rank (*Part2RankProvider)(
    const CacheEntry *entry,
    void *context);

CacheEntry *part2_cache_get(
    Part1Cache *cache,
    CacheKey key,
    Part2RankProvider rank_provider,
    void *rank_context);
```

This keeps Part 1 and Part 2 behavior explicit and independently testable.

### Part 2 hit path

For a cache hit:

```text
key
 |
 v
hash lookup
 |
 v
resident CacheEntry *
 |
 v
rank_provider(resident)
 |
 v
new rank
 |
 v
min_heap_update_rank()
 |
 v
same resident pointer returned
```

The hit path therefore uses:

```text
hash lookup                  expected O(1)
resident heap position       O(1) through heap_index
heap rank repair             O(log N) worst case
```

So a dynamic-rank cache hit is:

```text
O(log N) worst case
```

after the expected-O(1) hash lookup.

### Part 2 miss path

Stage 19 changes **hits only**.

If the key is absent:

```c
return part1_cache_get(cache, key);
```

Therefore the validated Part 1 miss behavior remains:

```text
database read
optional minimum eviction
insert into stable resident storage
insert into hash table
insert into min-heap
```

The rank provider is not called on a miss.

---

## Stage 19 Deterministic Rank Provider

For validation, Stage 19 adapts the deterministic Stage 12 rank scenarios through a callback context:

```c
typedef struct {
    Part2RankScenario scenario;
    uint64_t calls;
} Stage19RankContext;
```

The provider uses:

```text
PART2_RANK_DECREASE
PART2_RANK_UNCHANGED
PART2_RANK_INCREASE
```

and counts callback invocations.

This lets Stage 19 verify that a cache hit invokes the dynamic rank provider exactly once.

---

## Stage 19 Validation

### Test 1 — lower dynamic rank on hit

Initial integrated cache:

```text
key 1 rank 10
key 2 rank 20
key 3 rank 30
key 4 rank 40
key 5 rank 50
```

Lookup key `3`.

The provider returns:

```text
30 -> 0
```

Stage 19 verifies:

```text
same resident pointer returned
provider called exactly once
rank becomes 0
heap_index becomes 0
resident becomes heap minimum
part1_cache_validate() passes
```

### Test 2 — unchanged dynamic rank on hit

Lookup key `3` with:

```text
30 -> 30
```

Verify:

```text
same resident pointer
provider called once
same heap_index
same heap root
integrated validator passes
```

No heap movement occurs.

### Test 3 — higher dynamic rank on hit

Lookup heap-root key `1`.

The provider returns:

```text
10 -> 40
```

The former root moves downward.

Expected new minimum:

```text
key 2 / rank 20
```

Stage 19 verifies that the returned pointer is still the original key `1` resident and that the integrated cache remains valid.

### Test 4 — repeated dynamic hits

Key `5` begins at:

```text
rank 50
```

First hit:

```text
50 -> 20
```

Second hit on the same resident:

```text
20 -> -10
```

The second update uses the `heap_index` maintained by the first update.

Expected final state:

```text
key 5
rank -10
heap_index 0
heap root = key 5
provider calls = 2
```

### Test 5 — miss preserves Part 1 semantics

Request a missing key through `part2_cache_get()`.

Stage 19 verifies:

```text
database entry inserted normally
rank provider call count remains zero
resident/hash/heap sizes remain synchronized
integrated validator passes
```

This confirms Stage 19 changes the hit path only.

### Test 6 — repaired hit rank affects later eviction

Start with:

```text
key 1 rank 10
key 2 rank 20
key 3 rank 30
```

Hit key `1` with an increased rank:

```text
10 -> 40
```

The new heap minimum becomes:

```text
key 2 / rank 20
```

A subsequent minimum eviction must therefore remove key `2`, not key `1`.

This confirms that dynamic hit repair is visible to later cache eviction behavior.

### Test 7 — invalid Part 2 API arguments

Stage 19 rejects:

```text
NULL cache
NULL rank provider
```

and verifies that rejected requests do not mutate the existing resident rank or integrated cache state.

---

## Stage 19 Complexity

Dynamic hit:

```text
hash lookup                         expected O(1)
rank-provider invocation            external/user-defined
resident heap position              O(1)
min_heap_update_rank                O(log N) worst case
```

Therefore the data-structure portion of a dynamic hit is:

```text
O(log N) worst case
```

The miss path remains the existing Part 1 path.

---

## Stage 19 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache19

./ranked_cache19
```

Expected final Stage 19 output:

```text
Stage 19 findings:
  Part 2 cache hits invoke the rank provider exactly once.
  lower/same/higher hit ranks reuse min_heap_update_rank().
  the same resident pointer is returned after dynamic hit repair.
  repeated hits reuse the resident's maintained heap_index.
  misses preserve the existing Part 1 fetch/insert behavior.
  repaired hit ranks immediately affect later eviction ordering.
Stage 19 boundary: dynamic rank is integrated on hits only.
Stage 19 dynamic-hit integration validation: PASS

Stage 19 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache19_ubsan

./ranked_cache19_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache19_san

./ranked_cache19_san
```

Both sanitizer builds pass.

---

### Stage 19 boundary

Stage 19 deliberately does **not** add:

- dynamic rank computation on cache misses,
- a new database-rank policy,
- public API replacement of `part1_cache_get()`,
- concurrency,
- additional eviction policy changes,
- failure rollback for an external rank provider with side effects.

This checkpoint integrates dynamic rank changes into **cache hits only**.

---

## Stage 20 — Add Runtime Statistics

**Status: COMPLETE AND VALIDATED**

Stage 20 adds observability only.

It does not change:

- cache lookup policy,
- hash-table behavior,
- heap ordering,
- eviction policy,
- Part 1 fixed-rank semantics,
- Part 2 dynamic-rank semantics.

The statistics are stored directly in the integrated `Part1Cache` so each cache instance owns its own counters.

### Statistics structure

Stage 20 adds:

```c
typedef struct {
    uint64_t accesses;
    uint64_t hits;
    uint64_t misses;
    uint64_t db_reads;
    uint64_t insertions;
    uint64_t evictions;
    uint64_t rank_provider_calls;
    uint64_t rank_updates;
    uint64_t rank_decreases;
    uint64_t rank_unchanged;
    uint64_t rank_increases;
} CacheStats;
```

and extends:

```c
Part1Cache
```

with:

```c
CacheStats stats;
```

### Counter meanings

```text
accesses
    completed cache access attempts through part1_cache_get() or
    successful-hit processing through part2_cache_get()

hits
    requested key was already resident

misses
    requested key was not resident and entered the existing miss path

db_reads
    backing-store reads issued for cache misses

insertions
    successful integrated cache insertions

evictions
    successful minimum-rank evictions

rank_provider_calls
    Part 2 hit calls made to the external dynamic-rank provider

rank_updates
    successful Part 2 heap-rank updates after provider execution

rank_decreases
    successful Part 2 hit updates where new rank < old rank

rank_unchanged
    successful Part 2 hit updates where new rank == old rank

rank_increases
    successful Part 2 hit updates where new rank > old rank
```

### Why counters are not inside low-level lookup helpers

Stage 20 intentionally does **not** increment runtime access counters inside:

```c
part1_cache_lookup()
hash_table_lookup()
part1_cache_validate()
min_heap_validate()
```

Those functions are also used by validators and internal consistency checks.

Counting there would make debug/test validation artificially increase operational metrics.

Instead, counters are updated only at meaningful cache-operation boundaries.

---

## Statistics APIs

Stage 20 adds:

```c
void part1_cache_stats_reset(Part1Cache *cache);
```

and:

```c
CacheStats part1_cache_stats_snapshot(
    const Part1Cache *cache);
```

### Reset

`part1_cache_stats_reset()` clears only counters.

It does not modify:

- resident entries,
- cache size,
- hash table,
- min-heap,
- `heap_index`,
- free-slot state.

### Snapshot

`part1_cache_stats_snapshot()` returns a copy.

Reading statistics therefore does not mutate the counters.

For a `NULL` cache, the snapshot is an all-zero `CacheStats`.

---

## Part 1 statistics

`part1_cache_get()` now records:

### Hit

```text
accesses += 1
hits     += 1
```

### Miss

```text
accesses += 1
misses   += 1
db_reads += 1
```

A successful miss insertion also increments:

```text
insertions += 1
```

If the cache is full and minimum eviction succeeds:

```text
evictions += 1
```

The existing cache behavior is unchanged.

---

## Part 2 statistics

For a Part 2 hit:

```text
accesses            += 1
hits                += 1
rank_provider_calls += 1
```

After a successful indexed heap update:

```text
rank_updates += 1
```

Exactly one direction counter also increments:

```text
new < old -> rank_decreases += 1
new = old -> rank_unchanged += 1
new > old -> rank_increases += 1
```

### Part 2 miss accounting

A Part 2 miss still delegates to:

```c
part1_cache_get()
```

Therefore the miss is counted **once**, by the existing miss path.

The dynamic rank provider is not invoked on a miss.

This avoids double-counting:

```text
part2_cache_get()
    miss
      |
      v
part1_cache_get()
      |
      +--> accesses +1
      +--> misses +1
      +--> db_reads +1
```

---

## Stage 20 Validation

### Test 1 — initialization and reset

A newly initialized integrated cache must report all counters as zero.

After:

```text
GET 1 -> miss
GET 1 -> hit
```

the expected subset is:

```text
accesses   = 2
hits       = 1
misses     = 1
db_reads   = 1
insertions = 1
```

After:

```c
part1_cache_stats_reset(&cache);
```

every counter returns to zero while key `1` remains resident and the cache validator still passes.

### Test 2 — exact Part 1 sequence

Capacity:

```text
2
```

Sequence:

```text
GET 1 -> miss
GET 1 -> hit
GET 2 -> miss
GET 3 -> miss/full -> evict minimum -> insert
```

Expected counters:

```text
accesses   = 4
hits       = 1
misses     = 3
db_reads   = 3
insertions = 3
evictions  = 1
```

Dynamic-rank counters remain zero.

### Test 3 — dynamic rank directions

After preparing a five-entry dynamic cache, statistics are reset.

Three hits are performed on the same resident:

```text
decrease
unchanged
increase
```

Expected:

```text
accesses            = 3
hits                = 3
misses              = 0
rank_provider_calls = 3
rank_updates        = 3
rank_decreases      = 1
rank_unchanged      = 1
rank_increases      = 1
```

No DB read, insertion, or eviction is counted.

### Test 4 — Part 2 miss counted once

A full capacity-two cache receives a Part 2 request for a missing key.

Expected:

```text
accesses            = 1
hits                = 0
misses              = 1
db_reads            = 1
insertions          = 1
evictions           = 1
rank_provider_calls = 0
rank_updates        = 0
```

This confirms the Part 2 wrapper does not double-count the delegated miss.

### Test 5 — snapshot is non-mutating

Two consecutive statistics snapshots must contain identical values when no cache operation occurs between them.

---

## Stage 20 Complexity

Counter updates are constant-time integer increments.

Therefore Stage 20 does not change asymptotic cache complexity.

```text
statistics reset              O(1)
statistics snapshot           O(1)

Part 1 hit                    expected O(1)
Part 1 miss                   unchanged

Part 2 dynamic hit            O(log N) worst case
statistics overhead           O(1)
```

Memory overhead is one fixed-size `CacheStats` object per integrated cache instance.

---

## Stage 20 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache20

./ranked_cache20
```

Expected Stage 20 ending:

```text
Stage 20 findings:
  integrated cache accesses now expose deterministic runtime counters.
  hits/misses/DB reads/insertions/evictions are counted at operation boundaries.
  Part 2 hits classify rank decrease/equal/increase independently.
  Part 2 misses are counted once and do not invoke the rank provider.
  reset clears counters without changing cache contents.
  snapshot reads statistics without mutating them.
Stage 20 boundary: statistics add observability only; cache policy is unchanged.
Stage 20 statistics validation: PASS

Stage 20 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache20_ubsan

./ranked_cache20_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache20_san

./ranked_cache20_san
```

Both sanitizer builds pass.

---

### Stage 20 boundary

Stage 20 deliberately does **not** add:

- new eviction behavior,
- new ranking behavior,
- timing/profiling measurements,
- hit-rate calculations,
- percentages or derived metrics,
- persistence/export of statistics,
- concurrency/atomic counters.

It adds deterministic counters and snapshot/reset APIs only.

---

## Stage 21 — Add Deterministic Workload Generation

**Status: COMPLETE AND VALIDATED**

Stage 21 adds a reproducible workload generator for future correctness and performance experiments.

It deliberately does **not** add timing or benchmarking.

The generator produces two fields per operation:

```c
typedef struct {
    CacheKey key;
    Part2RankScenario scenario;
} WorkloadOp;
```

The key is bounded to a configured key space:

```text
1 .. key_space
```

and the scenario is one of:

```text
PART2_RANK_DECREASE
PART2_RANK_UNCHANGED
PART2_RANK_INCREASE
```

### Generator state

```c
typedef struct {
    uint64_t initial_seed;
    uint64_t state;
    CacheKey key_space;
} WorkloadGenerator;
```

Stage 21 uses a fixed 64-bit linear congruential generator rather than `rand()` or another platform library PRNG.

The state transition is:

```c
state =
    state * UINT64_C(6364136223846793005) +
    UINT64_C(1442695040888963407);
```

Unsigned 64-bit wraparound is defined by C, so the same initial state produces the same state sequence on conforming implementations.

Each `WorkloadOp` consumes two generator outputs:

```text
first output  -> key
second output -> Part 2 rank scenario
```

The key mapping is:

```c
key = (random_value % key_space) + 1;
```

The scenario mapping is:

```c
scenario = random_value % 3;
```

### Generator APIs

Stage 21 adds:

```c
workload_generator_init()
workload_generator_reset()
workload_generator_next_u64()
workload_generator_next()
workload_generate()
```

`workload_generator_reset()` restores the original seed so the same generator can replay the exact original stream.

---

## Golden deterministic sequence

For:

```text
seed      = 0x123456789abcdef0
key_space = 16
count     = 8
```

the required golden sequence is:

```text
operation  key  scenario

1          16   UNCHANGED
2          10   INCREASE
3           4   UNCHANGED
4          14   INCREASE
5           8   UNCHANGED
6           2   INCREASE
7          12   DECREASE
8           6   INCREASE
```

This exact sequence is checked in the Stage 21 tests.

The golden sequence protects the generator contract from accidental future changes to:

- PRNG constants,
- number of PRNG draws per operation,
- key mapping,
- scenario mapping.

---

## Stage 21 Validation

### Test 1 — fixed-seed golden sequence

Generate the first eight operations with the golden seed and compare every operation against the fixed expected vector.

### Test 2 — same seed produces the same stream

Two independent generators are initialized with:

```text
seed      = 0xfeedface12345678
key_space = 32
```

Both generate 64 operations.

Every corresponding `WorkloadOp` must be identical.

### Test 3 — different seeds diverge

Generators initialized with seed `1` and seed `2` produce 32 operations.

At least one operation must differ.

### Test 4 — bounded keys and valid scenarios

Generate 1000 operations with:

```text
key_space = 17
```

Every key must satisfy:

```text
1 <= key <= 17
```

Every scenario must be one of the three valid Part 2 scenarios.

The test also verifies that the generated sample contains all three scenario classes.

### Test 5 — reset and replay

Generate 32 operations, reset the generator, and generate 32 more.

The two streams must match operation-for-operation.

### Test 6 — invalid arguments

Stage 21 rejects:

```text
NULL generator initialization
zero key space
NULL generator for next operation
NULL operation output
NULL output with nonzero bulk-generation count
```

A zero-count bulk generation with a `NULL` output pointer is accepted because no output storage is required.

### Test 7 — deterministic execution and statistics

Two independent integrated caches are initialized identically.

Each executes:

```text
seed            = 0x3141592653589793
key_space       = 16
operation_count = 200
cache capacity  = 8
```

using the generated Part 2 operations.

The two runs must produce:

```text
identical CacheStats
identical access count = 200
identical resident key/rank state
identical heap minimum
valid integrated cache invariants
```

This validates that deterministic workload generation reproduces not only the operation stream but also the observable cache outcome.

No wall-clock time is captured.

---

## Stage 21 Complexity

Per generated operation:

```text
two fixed-width integer state transitions
two modulo operations
one WorkloadOp write
```

Therefore:

```text
workload_generator_next()    O(1)
generate N operations        O(N)
generator reset              O(1)
```

Memory use is:

```text
O(1)
```

for streaming generation, or:

```text
O(N)
```

only when the caller chooses to store N generated operations.

---

## Stage 21 Build and Validation

### Normal build

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache21

./ranked_cache21
```

Expected Stage 21 ending:

```text
Stage 21 findings:
  workload generation uses a fixed 64-bit LCG, not platform rand().
  the same seed/key-space/count reproduces the same operation stream.
  a fixed seed is protected by an explicit golden operation sequence.
  generated keys stay inside the configured key space.
  generated operations cover decrease/equal/increase rank scenarios.
  reset replays the stream from its original seed.
  replayed workloads reproduce cache statistics and logical cache state.
Stage 21 boundary: generation is deterministic; no timing benchmark is added.
Stage 21 deterministic-workload validation: PASS

Stage 21 validation: PASS
```

### UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=undefined \
    ranked_cache.c \
    -o ranked_cache21_ubsan

./ranked_cache21_ubsan
```

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -std=c11 \
    -O0 \
    -g3 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache21_san

./ranked_cache21_san
```

Both sanitizer builds pass.

---

### Stage 21 boundary

Stage 21 deliberately does **not** add:

- wall-clock timing,
- throughput calculations,
- latency calculations,
- benchmark warmup,
- randomized nondeterministic seeds,
- workload persistence,
- command-line workload configuration,
- concurrency.

It adds deterministic workload generation and replay only.

---

## Stage 22 — Correctness Build First

**Status: COMPLETE AND VALIDATED**

Stage 22 establishes a correctness gate before any later performance-oriented build.

It deliberately does **not** add:

- wall-clock timing,
- throughput measurement,
- latency measurement,
- `-O2` / `-O3`,
- benchmark warmup,
- performance tuning.

The Stage 22 correctness build instead prioritizes:

```text
warnings as errors
assertions enabled
-O0
debug symbols
frame pointers
ASan
UBSan
deterministic workload replay
invariant validation after every operation
```

### Correctness-build marker

Stage 22 supports the compile-time marker:

```c
RANKED_CACHE_CORRECTNESS_BUILD
```

through:

```c
int stage22_correctness_build_marker_enabled(void);
```

The documented correctness command defines this marker explicitly.

The source can still compile without the marker so earlier development commands remain usable.

### Assertions

Stage 22 verifies that:

```text
NDEBUG is not defined
```

for the correctness build.

That keeps the existing `assert()` checks active.

The Stage 22 checked workload combines:

```c
if (!part1_cache_validate(cache)) {
    return 0;
}

assert(part1_cache_validate(cache));
```

after **every generated cache operation**.

The explicit validator provides a normal test failure path.

The assertion adds fail-fast debug behavior.

---

## Stage 22 Checked Workload Runner

Stage 22 adds:

```c
int stage22_execute_checked_workload(
    Part1Cache *cache,
    uint64_t seed,
    CacheKey key_space,
    size_t operation_count);
```

For every deterministic Stage 21 operation:

```text
generate WorkloadOp
        |
        v
part2_cache_get()
        |
        v
part1_cache_validate()
        |
        v
assert(part1_cache_validate())
```

No timing information is collected.

---

## Stage 22 Validation

### Test 1 — correctness-build contract

The Stage 22 correctness configuration verifies:

```text
assertions enabled
RANKED_CACHE_CORRECTNESS_BUILD enabled
```

The dedicated build also uses:

```text
-Wall
-Wextra
-Wpedantic
-Werror
-O0
-g3
-fno-omit-frame-pointer
-fsanitize=address,undefined
```

### Test 2 — checked deterministic replay

Two independent caches execute:

```text
seed            = 0x6a09e667f3bcc909
key_space       = 32
cache capacity  = 16
operation_count = 1000
```

Every operation is immediately followed by a complete integrated invariant check.

The two runs must produce:

```text
1000 accesses each
identical CacheStats
identical logical cache state
valid integrated invariants
```

### Test 3 — multiple deterministic seeds

Stage 22 executes four additional fixed seeds:

```text
0x243f6a8885a308d3
0x13198a2e03707344
0xa4093822299f31d0
0x082efa98ec4e6c89
```

For each seed:

```text
key_space       = 48
cache capacity  = 24
operation_count = 750
```

After every operation:

```c
part1_cache_validate()
```

must pass.

The final statistics must also satisfy:

```text
accesses = 750

hits + misses = accesses

db_reads = misses
```

### Test 4 — controlled corruption detection

Stage 22 deliberately corrupts one resident's:

```c
heap_index
```

to:

```c
HEAP_INDEX_NONE
```

while leaving the resident physically present in the heap.

The tests verify:

```text
min_heap_validate()    -> fail
part1_cache_validate() -> fail
```

After restoring the original index:

```text
min_heap_validate()    -> pass
part1_cache_validate() -> pass
```

This verifies the correctness gate actively detects a known structural violation.

---

## Stage 22 Correctness Build

Use this build **before** any later optimized/performance build:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    -O0 \
    -g3 \
    -fno-omit-frame-pointer \
    -DRANKED_CACHE_CORRECTNESS_BUILD=1 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache22_correctness
```

Run:

```bash
ASAN_OPTIONS=detect_leaks=1 \
UBSAN_OPTIONS=halt_on_error=1 \
./ranked_cache22_correctness
```

Expected Stage 22 ending:

```text
Stage 22 findings:
  correctness validation runs with assertions enabled.
  deterministic workloads validate invariants after every operation.
  same-seed checked replay reproduces statistics and logical state.
  multiple fixed seeds exercise the integrated Part 2 path reproducibly.
  controlled metadata corruption is detected before performance work begins.
  no timing or optimization is introduced in the correctness gate.
Stage 22 boundary: correctness build only; performance build comes later.
Stage 22 correctness-build validation: PASS

Stage 22 validation: PASS
```

The correctness build completes with:

```text
no compiler warnings
no compiler errors
no AddressSanitizer diagnostics
no UndefinedBehaviorSanitizer diagnostics
exit status 0
```

### Compatibility warning build

The Stage 22 source also remains valid with the ordinary warning build:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache22
```

This is useful for source compatibility, but it is **not** a substitute for the correctness build above.

---

## Stage 22 Complexity

Stage 22 does not change production cache complexity.

The additional invariant checking belongs to the correctness/test configuration.

`part1_cache_validate()` includes debug-oriented cross-structure validation, so the checked workload intentionally prioritizes correctness rather than speed.

That cost must **not** be included in a later performance comparison.

---

### Stage 22 boundary

Stage 22 deliberately does **not** add:

- optimized compiler flags,
- `NDEBUG`,
- wall-clock clocks,
- timing harnesses,
- operations-per-second calculations,
- latency percentiles,
- performance comparison,
- benchmark result files,
- optimization changes.

The correctness build is established first. Performance measurement remains a later stage.

---

## Stage 23 — Optimized Build Second

**Status: COMPLETE AND VALIDATED**

Stage 22 established the correctness build first:

```text
-O0
assertions enabled
-Werror
debug symbols
frame pointers
ASan
UBSan
per-operation invariant validation
```

Stage 23 now adds the second build configuration:

```text
optimized build
```

without adding any timing or benchmarking.

The Stage 23 target uses:

```bash
-O3
-DNDEBUG
-DRANKED_CACHE_OPTIMIZED_BUILD=1
```

while retaining:

```bash
-Wall
-Wextra
-Wpedantic
-Werror
-std=c11
```

The purpose of Stage 23 is not to claim a speedup.

It is to prove that the source can be compiled in an optimized production-style configuration **after** the correctness gate has passed, while preserving deterministic functional behavior.

### Build-mode separation

Stage 23 adds a compile-time guard:

```c
#if defined(RANKED_CACHE_CORRECTNESS_BUILD) && \
    defined(RANKED_CACHE_OPTIMIZED_BUILD)
#error "correctness and optimized build markers are mutually exclusive"
#endif
```

This prevents accidentally mixing the two configurations.

The intended sequence is:

```text
Stage 22
correctness build
        |
        +--> assertions
        +--> ASan/UBSan
        +--> per-op validation
        |
        v
PASS
        |
        v
Stage 23
optimized build
        |
        +--> -O3
        +--> -DNDEBUG
        +--> no sanitizer instrumentation
        +--> no per-op correctness assertions
```

### Optimized-build contract

Stage 23 adds helpers that verify:

```text
RANKED_CACHE_OPTIMIZED_BUILD is defined
NDEBUG is defined
compiler optimization is enabled
```

For GCC/Clang-style builds, the optimized test also checks:

```c
__OPTIMIZE__
```

so accidentally compiling the Stage 23 target at `-O0` does not satisfy the full optimized-build contract.

### NDEBUG build hygiene

The first Stage 23 optimized compile exposed warnings that do not appear in the Stage 22 correctness build.

Stage 23 fixes those configuration-specific hazards without changing cache behavior:

- `cache_assert_invariants()` explicitly references its parameter even when `assert()` is compiled away by `NDEBUG`;
- Stage 12 probe variables are explicitly initialized so optimized data-flow analysis cannot report possible uninitialized use;
- Stage 22's build-contract test becomes build-mode aware so it still requires assertions in the correctness build but does not incorrectly reject the later optimized configuration.

These are build-hygiene changes only.

No cache algorithm is changed.

---

## Stage 23 Optimized Workload Path

Stage 22 intentionally validates the cache after every generated operation.

That is correct for a debug gate, but that validation overhead must not become part of a later performance workload.

Stage 23 therefore adds:

```c
stage23_execute_optimized_workload()
```

Its operation flow is:

```text
generate WorkloadOp
        |
        v
part2_cache_get()
        |
        v
next operation
```

There is no per-operation:

```text
part1_cache_validate()
assert()
```

inside this Stage 23 workload loop.

After the complete workload finishes, Stage 23 performs a final integrated validation.

This establishes the future optimized execution path without measuring it yet.

---

## Golden Correctness-Baseline Outcome

Stage 23 protects semantic equivalence using a deterministic workload whose expected outcome was established from the correctness baseline.

Configuration:

```text
seed            = 0x3141592653589793
key_space       = 16
cache capacity  = 8
operation_count = 200
```

The optimized build must produce exactly:

```text
accesses            = 200
hits                = 192
misses              = 8
db_reads            = 8
insertions          = 8
evictions           = 0

rank_provider_calls = 192
rank_updates        = 192
rank_decreases      = 65
rank_unchanged      = 65
rank_increases      = 62
```

The final resident key/rank state must be exactly:

```text
key 1   rank  -20
key 3   rank    0
key 5   rank   50
key 7   rank -110
key 9   rank  120
key 11  rank  350
key 13  rank   10
key 15  rank  150
```

Keys:

```text
2, 4, 6, 8, 10, 12, 14, 16
```

must be absent.

The final heap minimum must be:

```text
key 7 / rank -110
```

This makes the Stage 23 optimized build fail if compiler/build changes alter observable cache semantics.

---

## Optimized Replay Validation

Stage 23 also performs a same-seed replay using:

```text
seed            = 0x6a09e667f3bcc909
key_space       = 32
cache capacity  = 16
operation_count = 1000
```

Two independent optimized workload executions must produce:

```text
identical CacheStats
exactly 1000 accesses each
identical logical resident state
valid final integrated caches
```

This is still a correctness/equivalence test.

No clock is read.

---

## Stage 23 Builds

### 1. Re-run the correctness build

Before accepting the optimized target, verify the Stage 22 gate still passes with the Stage 23 source:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    -O0 \
    -g3 \
    -fno-omit-frame-pointer \
    -DRANKED_CACHE_CORRECTNESS_BUILD=1 \
    -fsanitize=address,undefined \
    ranked_cache.c \
    -o ranked_cache23_correctness

ASAN_OPTIONS=detect_leaks=1 \
UBSAN_OPTIONS=halt_on_error=1 \
./ranked_cache23_correctness
```

This build must retain:

```text
assertions enabled
ASan enabled
UBSan enabled
Stage 22 correctness gate passing
Stage 23 deterministic equivalence checks passing
```

### 2. Build the optimized target

Only after the correctness build passes:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    -O3 \
    -DNDEBUG \
    -DRANKED_CACHE_OPTIMIZED_BUILD=1 \
    ranked_cache.c \
    -o ranked_cache23_optimized
```

Run:

```bash
./ranked_cache23_optimized
```

Expected Stage 23 ending:

```text
Stage 23 findings:
  optimized build is explicitly separated from the correctness build.
  the optimized configuration uses -O3 and NDEBUG after correctness validation.
  deterministic golden statistics and resident state match the correctness baseline.
  same-seed optimized replay reproduces statistics and logical state.
  final integrated invariants remain valid without per-operation correctness checks.
  no clocks, throughput, or latency measurements are introduced yet.
Stage 23 boundary: optimized build established; performance measurement comes later.
Stage 23 optimized-build validation: PASS

Stage 23 validation: PASS
```

### 3. Optional ordinary compatibility build

The source also remains warning-clean without either build marker:

```bash
gcc \
    -Wall \
    -Wextra \
    -Wpedantic \
    -Werror \
    -std=c11 \
    ranked_cache.c \
    -o ranked_cache23

./ranked_cache23
```

This is a compatibility check, not the Stage 23 target configuration.

---

## Stage 23 Complexity

Stage 23 changes no cache data-structure complexity.

The optimized build changes compiler configuration only.

The Stage 23 workload removes Stage 22's per-operation debug validation from the optimized execution path, but no performance claim is made in this stage.

No throughput, latency, speedup, or operations-per-second result is reported.

---

### Stage 23 boundary

Stage 23 deliberately does **not** add:

- `clock_gettime()`,
- CPU-cycle counters,
- throughput calculations,
- latency measurements,
- warmup loops,
- benchmark repetitions,
- statistical timing summaries,
- `-march=native`,
- LTO,
- PGO,
- algorithm changes,
- cache-policy changes.

It establishes the optimized binary only after the correctness binary is already validated.

---

## Build Environment

Current target environment:

- Ubuntu 24.04 LTS
- GCC
- C11

### Build command

```bash
gcc -Wall -Wextra -Wpedantic -Werror -std=c11 -O3 -DNDEBUG -DRANKED_CACHE_OPTIMIZED_BUILD=1 ranked_cache.c -o ranked_cache23_optimized
```

The warning flags are intentionally enabled from the first stage:

- `-Wall`
- `-Wextra`
- `-Wpedantic`

This helps catch implementation mistakes early as the program becomes more complex.

---

## Run

```bash
./ranked_cache23_optimized
```

### Expected result

The active Stage 23 optimized test suite must end with:

```text
Stage 23 validation: PASS
```

The Stage 10 Part 1 path combines the hash table and min-heap while preserving the earlier linear cache as a regression/reference implementation.

### Validation result

Stages 0 through 23 have been validated successfully. The Stage 22 correctness build still passes with assertions and ASan/UBSan, and the Stage 23 optimized build passes with -O3, NDEBUG, deterministic golden equivalence, and final invariant validation.

---

## Current Functional Scope

At this commit, the program can:

- preserve and regress the original linear array cache,
- preserve Stage 6 lookup bottleneck instrumentation,
- preserve Stage 7 standalone hash-table validation,
- preserve Stage 8 minimum-rank bottleneck instrumentation,
- preserve Stage 9 standalone min-heap validation,
- initialize an integrated fixed-rank Part 1 cache,
- allocate resident slots without moving live entries,
- look up Part 1 residents through the hash table,
- insert Part 1 residents into both hash table and min-heap,
- return Part 1 cache hits without changing rank,
- identify the Part 1 eviction victim through the heap root,
- evict the minimum-ranked Part 1 resident without a linear rank scan,
- recycle evicted resident slots,
- keep hash-table, heap, resident count, and free-slot state synchronized, and
- validate the complete integrated Part 1 state,
- validate empty and partial-capacity Part 1 behavior,
- verify repeated hits preserve fixed rank and resident identity,
- verify duplicate insertion leaves integrated state unchanged,
- verify deterministic equal-rank eviction,
- verify integrated hash collision/tombstone behavior,
- validate capacity-one and maximum-capacity boundaries,
- verify stable resident pointers and free-slot reuse, and
- validate repeated full-cache evictions while maintaining cross-structure consistency,
- model the Part 2 `getEntryRank()` contract with lower/equal/higher outcomes,
- apply an arbitrary resident rank change through the integrated hash lookup,
- prove an unrepaired rank decrease can stale the ordinary heap,
- prove an unrepaired rank increase can stale the ordinary heap, and
- verify an unchanged rank leaves the existing heap valid,
- measure ordinary-heap arbitrary-entry location by pointer,
- prove first/middle/last heap-position costs of 1/50/100 comparisons,
- prove missing heap-pointer lookup scales linearly with heap size, and
- demonstrate that hash lookup still requires an O(N) heap scan to recover the resident's heap index at the Stage 13 checkpoint,
- store a `heap_index` reverse position in every heap-resident `CacheEntry`,
- maintain `heap_index` during heap push, swap, sift, and pop operations,
- invalidate `heap_index` when an entry leaves the heap,
- validate direct O(1) resident-to-heap-position metadata, and
- detect deliberate reverse-index corruption through the heap and integrated validators,
- reject invalid heap-swap arguments before mutation,
- treat self-swap as an explicit safe no-op,
- preserve both pointer ownership and both `heap_index` values across valid swaps, and
- verify invalid swaps do not partially change reverse-position metadata,
- update an arbitrary heap-resident rank through `heap_index`,
- repair lower ranks with sift-up,
- repair higher ranks with sift-down,
- leave unchanged ranks in place,
- preserve deterministic rank/key tie ordering during updates, and
- combine hash lookup with direct indexed heap rank repair without a linear heap scan,
- validate a rank decrease that requires no movement,
- validate one-level and multi-level sift-up repairs,
- validate equal-rank key ordering during a decrease,
- validate repeated decreases through the updated `heap_index`, and
- validate an integrated hash-found resident rank decrease,
- validate a rank increase that requires no movement,
- validate one-level and multi-level sift-down repairs,
- validate equal-rank child selection during an increase,
- validate repeated increases through the updated `heap_index`, and
- validate an integrated hash-found resident rank increase,
- invoke a dynamic rank provider on Part 2 cache hits,
- repair lower/equal/higher hit ranks through `min_heap_update_rank()`,
- preserve resident pointer identity across dynamic hit repair,
- reuse maintained `heap_index` across repeated dynamic hits,
- preserve the existing Part 1 miss path, and
- verify repaired hit ranks immediately affect later eviction ordering,
- expose per-cache access/hit/miss/DB/insert/eviction counters,
- expose Part 2 rank-provider and rank-direction counters,
- reset statistics without changing cache state,
- snapshot statistics without mutating counters, and
- verify Part 2 delegated misses are counted exactly once,
- generate reproducible key/scenario workload operations from a fixed seed,
- replay an identical workload by resetting or reusing the same seed,
- validate a fixed golden operation sequence,
- verify generated key/scenario bounds, and
- reproduce identical cache statistics and logical state from identical workloads,
- run deterministic Part 2 workloads with invariant validation after every operation,
- enforce a correctness build with assertions, `-Werror`, `-O0`, debug symbols, and frame pointers,
- validate deterministic checked replay across multiple fixed seeds, and
- verify controlled structural corruption is detected before performance work begins,
- compile a separate optimized target only after the correctness gate,
- enforce mutually exclusive correctness/optimized build markers,
- keep the optimized build warning-clean under `-O3 -DNDEBUG -Werror`,
- validate optimized output against a correctness-baseline golden outcome, and
- replay deterministic optimized workloads without per-operation debug validation.

At this commit, the program intentionally does **not** implement:

- production Part 2 rank changes inside `part1_cache_get()`,
- indexed heap positions for Part 2,
- concurrent/thread-safe access, or
- later-stage Part 2 optimization.

Those capabilities must only be documented after their corresponding implementation stages have actually been completed and validated.

---

## Complexity at the Current Checkpoint

The earlier linear `Cache` remains available as a regression/reference implementation:

```text
cache_lookup()                O(N)
cache_find_min_rank_index()   O(N)
cache_evict_min()             O(N)
cache_get() hit               O(N)
```

The new Stage 10 integrated fixed-rank Part 1 path is:

```text
part1_cache_is_full()         O(1)
part1_cache_lookup()          O(1) expected, O(M) worst case
part1_cache_insert()          O(log N) expected overall
part1_cache_evict_min()       O(log N) expected overall
part1_cache_get() hit         O(1) expected
part1_cache_get() miss        O(log N) cache maintenance + DB-read cost
min-heap minimum access       O(1)
resident-slot allocate/free   O(1)
Part 1 resident storage       O(K)
hash-table storage            O(M)
heap storage                  O(K)
```

The Stage 10 validator is intentionally thorough and may perform O(N²) work. It is a development correctness aid and is not part of the intended production operation complexity.

Stage 10 therefore addresses the two bottlenecks previously identified:

```text
Stage 6 bottleneck:
linear key lookup O(N)
        -> hash lookup expected O(1)

Stage 8 bottleneck:
linear minimum-rank scan O(N)
        -> heap minimum O(1), pop O(log N)
```

Because Part 1 rank values do not change on lookup, a cache hit does not require heap maintenance.

Stage 11 does not change these production complexities. Its additional test helpers and
validator calls are correctness instrumentation rather than cache-operation optimizations.

Stage 12 also does not change production complexity. It demonstrates that after an
arbitrary rank change, the existing ordinary heap may become invalid. Hash lookup still
locates the resident in expected O(1), but Stage 12 intentionally provides no efficient
way to locate that resident's heap position or repair the heap yet.

Stage 13 measures that missing heap-position operation directly. With the current
`MinHeap`, locating an arbitrary `CacheEntry *` requires O(N) pointer scanning in the
worst case. Therefore a hypothetical Part 2 update using the current structures would
still be O(N) + O(log N), dominated by the O(N) heap-location step.

Stage 14 removes only that location bottleneck by maintaining `CacheEntry.heap_index`.
Once the hash table returns a resident pointer, reading its current heap position is
O(1). The priority-update/repair operation itself is intentionally not implemented yet.

Stage 15 preserves those complexities. `min_heap_swap()` remains O(1); the new
argument validation is constant-time and establishes a stronger consistency primitive
for later indexed-heap operations.

Stage 16 uses `heap_index` plus the hardened swap primitive to implement arbitrary
resident rank repair in O(log N) worst case. The operation performs no linear heap scan.

Stage 17 does not change that complexity. It independently stress-tests only the
rank-decrease/sift-up branch, including the valid case where a smaller rank does not
cross its parent and therefore requires no movement.

Stage 18 mirrors that validation for the rank-increase/sift-down branch. It confirms
that a larger rank may remain in place, move one level, or move multiple levels while
preserving the same O(log N) worst-case repair complexity.

Stage 19 integrates the already validated rank-update primitive into cache hits. The
hash lookup remains expected O(1), heap position lookup is O(1) through `heap_index`,
and dynamic hit repair is O(log N) worst case, excluding the external rank-provider cost.

Stage 20 adds only O(1) counter updates around those existing operation boundaries.
It does not change any lookup, heap, insertion, or eviction asymptotic complexity.

Stage 21 adds O(1) deterministic generation work per operation. It does not add
timing or alter any cache data-structure complexity.

Stage 22 changes no production asymptotic complexity. Its per-operation integrated
validator is intentionally correctness-oriented debug overhead and must be excluded
from later performance measurements.

Stage 23 changes compiler configuration only. The optimized workload path omits the
Stage 22 per-operation debug validator and verifies final state instead, but this stage
still reports no timing or performance result.

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


Stage 8
Second bottleneck identification
COMPLETE
BUILD PASS
RUN PASS
MINIMUM-POSITION PROFILE PASS
MINIMUM-RANK SCALING PASS
O(N) VICTIM-SELECTION BOTTLENECK CONFIRMED
KNOWN-SLOT REMOVAL REMAINS O(1)
ASAN/UBSAN PASS

Stage 9
Standalone binary min-heap validation
COMPLETE
BUILD PASS
RUN PASS
EMPTY-HEAP GUARDS PASS
HEAP PUSH PASS
HEAP PEEK-MIN PASS
HEAP POP-MIN PASS
HEAP INVARIANT PASS
EQUAL-RANK TIE PASS
ASCENDING POP ORDER PASS
CAPACITY GUARD PASS
ASAN/UBSAN PASS

Stage 10
Integrated hash table + min-heap for fixed-rank Part 1
COMPLETE
BUILD PASS
RUN PASS
HASH LOOKUP INTEGRATION PASS
HEAP EVICTION INTEGRATION PASS
FIXED-RANK HIT PASS
FULL-CACHE MISS/EVICTION PASS
HASH/HEAP/RESIDENT SYNCHRONIZATION PASS
INTEGRATED VALIDATOR PASS
ASAN/UBSAN PASS


Stage 11
Thorough fixed-rank Part 1 validation
COMPLETE
BUILD PASS
RUN PASS
EMPTY/PARTIAL CAPACITY PASS
REPEATED FIXED-RANK HIT PASS
DUPLICATE-KEY REJECTION PASS
EQUAL-RANK TIE PASS
INTEGRATED HASH COLLISION PASS
CAPACITY BOUNDARY PASS
STABLE ADDRESS/SLOT REUSE PASS
REPEATED EVICTION PASS
CROSS-STRUCTURE CONSISTENCY PASS
ASAN/UBSAN PASS


Stage 12
Begin Part 2 rank-change contract
COMPLETE
BUILD PASS
RUN PASS
LOWER-RANK CONTRACT PASS
UNCHANGED-RANK CONTRACT PASS
HIGHER-RANK CONTRACT PASS
HASH-LOOKUP RESIDENT IDENTITY PASS
STALE-HEAP DECREASE DETECTION PASS
STALE-HEAP INCREASE DETECTION PASS
UNCHANGED-RANK HEAP VALIDITY PASS
PART 1 STATE RESTORE PASS
PART 2 UPDATE PROBLEM CONFIRMED
ASAN/UBSAN PASS


Stage 13
Recognize ordinary-heap arbitrary-entry limitation
COMPLETE
BUILD PASS
RUN PASS
HEAP POSITION INSTRUMENTATION PASS
FIRST/MIDDLE/LAST LOCATION PROFILE PASS
MISSING-POINTER SCALING PASS
HASH-TO-HEAP LOCATION GAP PASS
O(N) ARBITRARY HEAP LOCATION CONFIRMED
DIRECT RESIDENT-TO-HEAP-INDEX REQUIREMENT CONFIRMED
ASAN/UBSAN PASS


Stage 14
Introduce heap_index reverse position
COMPLETE
BUILD PASS
RUN PASS
HEAP_INDEX FIELD PASS
PUSH INDEX ASSIGNMENT PASS
SWAP INDEX MAINTENANCE PASS
POP INDEX INVALIDATION PASS
SURVIVOR INDEX REPAIR PASS
DIRECT HASH-TO-HEAP INDEX PASS
REVERSE-INDEX CORRUPTION DETECTION PASS
UBSAN PASS
ASAN/UBSAN NODE1 VALIDATION COMMAND DOCUMENTED


Stage 15
Harden min_heap_swap consistency primitive
COMPLETE
BUILD PASS
RUN PASS
NON-ADJACENT SWAP PASS
PARENT-CHILD SWAP PASS
SELF-SWAP PASS
INVALID-INDEX REJECTION PASS
NULL-HEAP REJECTION PASS
NULL-SLOT REJECTION PASS
NO-PARTIAL-METADATA-MUTATION PASS
SIFT CALLER REGRESSION PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 16
Implement min_heap_update_rank
COMPLETE
BUILD PASS
RUN PASS
RANK-DECREASE SIFT-UP PASS
RANK-INCREASE SIFT-DOWN PASS
UNCHANGED-RANK NO-OP PASS
TIE-ORDER UPDATE PASS
INVALID-MEMBERSHIP REJECTION PASS
NON-DESTRUCTIVE FAILURE PASS
HASH-TO-INDEXED-UPDATE PASS
INTEGRATED VALIDATOR PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 17
Test rank decrease independently
COMPLETE
BUILD PASS
RUN PASS
DECREASE-NO-MOVEMENT PASS
DECREASE-ONE-LEVEL PASS
DECREASE-MULTI-LEVEL PASS
DECREASE-TIE-ORDER PASS
REPEATED-DECREASE PASS
HASH-FOUND-DECREASE PASS
HEAP_INDEX CONSISTENCY PASS
INTEGRATED VALIDATOR PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 18
Test rank increase independently
COMPLETE
BUILD PASS
RUN PASS
INCREASE-NO-MOVEMENT PASS
INCREASE-ONE-LEVEL PASS
INCREASE-MULTI-LEVEL PASS
INCREASE-TIE-ORDER PASS
REPEATED-INCREASE PASS
HASH-FOUND-INCREASE PASS
HEAP_INDEX CONSISTENCY PASS
INTEGRATED VALIDATOR PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 19
Integrate dynamic rank into cache hits
COMPLETE
BUILD PASS
RUN PASS
DYNAMIC-HIT DECREASE PASS
DYNAMIC-HIT UNCHANGED PASS
DYNAMIC-HIT INCREASE PASS
REPEATED-DYNAMIC-HIT PASS
MISS-PATH COMPATIBILITY PASS
DYNAMIC-RANK EVICTION EFFECT PASS
INVALID-API REJECTION PASS
INTEGRATED VALIDATOR PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 20
Add integrated cache statistics
COMPLETE
BUILD PASS
RUN PASS
INITIAL-ZERO STATS PASS
STATS RESET PASS
PART1 HIT/MISS COUNTERS PASS
DB-READ COUNTER PASS
INSERTION COUNTER PASS
EVICTION COUNTER PASS
RANK-PROVIDER COUNTER PASS
RANK-DIRECTION COUNTERS PASS
PART2 MISS SINGLE-COUNT PASS
SNAPSHOT NON-MUTATING PASS
INTEGRATED VALIDATOR PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 21
Add deterministic workload generation
COMPLETE
BUILD PASS
RUN PASS
GOLDEN-SEQUENCE PASS
SAME-SEED REPLAY PASS
DIFFERENT-SEED DIVERGENCE PASS
KEY-BOUND PASS
SCENARIO-BOUND PASS
RESET-REPLAY PASS
INVALID-ARGUMENT PASS
DETERMINISTIC-STATS PASS
DETERMINISTIC-CACHE-STATE PASS
INTEGRATED VALIDATOR PASS
UBSAN PASS
ASAN/UBSAN PASS


Stage 22
Correctness build first
COMPLETE
-WERROR BUILD PASS
ASSERTIONS ENABLED PASS
CORRECTNESS-BUILD MARKER PASS
-O0 DEBUG BUILD PASS
FRAME-POINTER BUILD PASS
CHECKED 1000-OP REPLAY PASS
MULTI-SEED CHECKED WORKLOAD PASS
PER-OP INVARIANT VALIDATION PASS
STATISTICS ACCOUNTING PASS
CONTROLLED-CORRUPTION DETECTION PASS
ASAN PASS
UBSAN PASS
NO TIMING/OPTIMIZATION INTRODUCED


Stage 23
Optimized build second
COMPLETE
CORRECTNESS BUILD REGRESSION PASS
OPTIMIZED -O3 BUILD PASS
NDEBUG BUILD PASS
-WERROR OPTIMIZED BUILD PASS
BUILD-MARKER EXCLUSION PASS
OPTIMIZATION-MARKER PASS
GOLDEN-STATS EQUIVALENCE PASS
GOLDEN-RESIDENT-STATE PASS
GOLDEN-HEAP-MINIMUM PASS
OPTIMIZED REPLAY PASS
FINAL INVARIANT VALIDATION PASS
NO TIMING/BENCHMARK INTRODUCED
```
