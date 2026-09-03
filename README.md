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

### Stage 0 test object

The current `main()` creates one `CacheEntry`:

```c
e.key = 1;
e.value = 100;
e.rank = 50;
```

and prints its three fields.

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

### Stage 1 test path

`main()` now obtains the entry through the database abstraction:

```c
CacheEntry e = db_read_entry(5);
```

and prints the returned fields.

The Stage 0 direct field assignments are no longer used by the active test path.

### Stage 1 validation result

Build:

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache1
```

Run:

```bash
./ranked_cache1
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

No cache lookup, cache storage, hit/miss handling, capacity management, eviction, rank ordering, dynamic rank update, or performance optimization is implemented at this checkpoint.

### Stage 1 complexity

For the current synthetic implementation, `db_read_entry()` performs only a fixed number of assignments and arithmetic operations.

```text
Time:  O(1)
Space: O(1)
```

This describes only the deterministic Stage 1 abstraction. It does not model the latency of a real database, file, network, or persistent-storage operation.


---

## Build Environment

Current target environment:

- Ubuntu 24.04 LTS
- GCC
- C11

### Build command

```bash
gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache1
```

The warning flags are intentionally enabled from the first stage:

- `-Wall`
- `-Wextra`
- `-Wpedantic`

This helps catch implementation mistakes early as the program becomes more complex.

---

## Run

```bash
./ranked_cache1
```

### Expected output

```text
key=5 value=500 rank=50
```

### Validation result

Stages 0 and 1 have been compiled and executed successfully, with the active Stage 1 test producing the expected output.

---

## Current Functional Scope

At this commit, the program can:

- define a cache-key type,
- define a rank type,
- represent one cache entry,
- pass a key into `db_read_entry()`,
- construct a deterministic `CacheEntry` inside the database abstraction,
- return that entry by value to the caller, and
- print and validate the returned key, value, and rank.

At this commit, the program intentionally does **not** implement:

- cache storage,
- cache lookup,
- cache hit/miss handling,
- capacity management,
- eviction,
- rank ordering,
- dynamic rank updates,
- performance optimization, or
- benchmarking.

Those capabilities must only be documented here after their corresponding implementation stage has actually been completed and validated.

---

## Complexity at the Current Checkpoint

The active Stage 1 program performs a deterministic database-abstraction call followed by printing one returned entry.

For the current synthetic implementation:

```text
db_read_entry() time:  O(1)
Stage 1 extra space:   O(1)
```

No cache data structure or cache-operation complexity is claimed yet.

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
```
