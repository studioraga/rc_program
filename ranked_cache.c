/* ranked_cache.c
 *
 * Incremental ranked-cache implementation.
 *
 * Completed stages:
 *   Stage 0 - basic CacheEntry data model
 *   Stage 1 - deterministic database abstraction
 *   Stage 2 - simplest fixed-size cache using a linear array
 *   Stage 3 - explicit linear minimum-rank eviction path
 *   Stage 4 - complete cache_get() hit/miss path
 *   Stage 5 - assertions and cache invariants
 *   Stage 6 - identify the first bottleneck with lookup instrumentation
 *   Stage 7 - standalone hash-table lookup validation
 *   Stage 8 - identify the second bottleneck with rank-scan instrumentation
 *   Stage 9 - standalone binary min-heap validation
 *   Stage 10 - integrated hash table + min-heap cache for fixed-rank Part 1
 *   Stage 11 - thorough fixed-rank Part 1 validation
 *   Stage 12 - introduce and validate the Part 2 rank-change contract
 *   Stage 13 - prove ordinary heap lacks direct arbitrary-entry location
 *   Stage 14 - add and maintain CacheEntry.heap_index reverse position
 *   Stage 15 - harden min_heap_swap() as the indexed-heap consistency primitive
 *   Stage 16 - add min_heap_update_rank() for arbitrary resident priority repair
 *   Stage 17 - test rank-decrease repair independently
 *   Stage 18 - test rank-increase repair independently
 *   Stage 19 - integrate dynamic rank repair into cache hits
 *   Stage 20 - add integrated cache runtime statistics
 *
 * Stage 2 intentionally uses linear scanning for:
 *   - lookup
 *   - duplicate detection before insert
 *   - minimum-rank search
 *
 * Stage 5 adds explicit structural invariant validation and development-time
 * assertions without changing the Stage 4 cache algorithm or complexity.
 *
 * Stage 6 does not optimize the cache. It instruments the existing linear
 * lookup path so the first bottleneck can be demonstrated with exact operation
 * counts rather than wall-clock timing.
 *
 * Stage 7 introduces a standalone hash table and validates its lookup, insert,
 * collision, deletion, and tombstone behavior independently. The hash table is
 * deliberately not connected to Cache or cache_get() yet.
 *
 * Stage 8 does not optimize eviction. It instruments the existing linear
 * minimum-rank scan so the second bottleneck can be demonstrated with exact
 * rank-comparison counts.
 *
 * Stage 9 introduces and tests a standalone array-backed binary min-heap.
 * The heap is deliberately not connected to Cache, cache_get(), or eviction.
 *
 * Stage 10 combines the validated Stage 7 hash table and Stage 9 min-heap in a
 * separate fixed-rank Part 1 cache. The earlier linear Cache remains present as
 * a regression/reference implementation. Dynamic rank updates are not supported.
 *
 * Stage 11 does not change the Part 1 algorithms. It expands validation of the
 * integrated fixed-rank cache across boundaries, ties, collisions, repeated hits,
 * repeated evictions, duplicate rejection, stable storage, and slot reuse.
 *
 * Stage 12 begins Part 2 by validating the new contract only: getEntryRank()
 * may return a lower, unchanged, or higher rank after a lookup. Stage 12
 * deliberately does not repair the ordinary heap after such a change; instead,
 * it proves why an arbitrary-priority update mechanism is required next.
 *
 * Stage 13 identifies the exact missing ordinary-heap capability. The hash table
 * can return CacheEntry * by key, but MinHeap stores only an array of pointers and
 * records no reverse mapping from CacheEntry * to heap index. Locating an arbitrary
 * resident in the heap therefore requires a linear scan at this checkpoint.
 *
 * Stage 14 adds that reverse position directly to CacheEntry as heap_index and
 * maintains it during heap push/swap/pop operations. Stage 14 does not yet use
 * the index to repair a changed priority; it only establishes trustworthy O(1)
 * resident-to-heap-position metadata for the next incremental step.
 *
 * Stage 17 independently stress-tests only the rank-decrease/sift-up branch of
 * min_heap_update_rank(). Stage 18 now mirrors that discipline for the
 * rank-increase/sift-down branch without changing production heap logic.
 */

#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdint.h>

#define MAX_CACHE_CAPACITY 100U
#define HASH_TABLE_CAPACITY 211U
#define HEAP_INDEX_NONE SIZE_MAX

typedef unsigned long long CacheKey;
typedef long long Rank;

typedef struct {
    CacheKey key;
    unsigned long long value;
    Rank rank;
    size_t heap_index;
} CacheEntry;

/*
 * Simplest cache representation for Stage 2.
 *
 * entries  - fixed-size array that stores resident cache entries
 * size     - number of valid entries currently stored
 * capacity - maximum number of entries allowed for this cache instance
 */
typedef struct {
    CacheEntry entries[MAX_CACHE_CAPACITY];
    size_t size;
    size_t capacity;
} Cache;


/* ---------- Stage 6: lookup instrumentation ---------- */

/*
 * Exact counters for the existing linear key-lookup path.
 *
 * These counters are diagnostic only. They deliberately do not change the
 * lookup algorithm or the cache data structure.
 */
typedef struct {
    uint64_t lookup_calls;
    uint64_t key_comparisons;
    uint64_t lookup_hits;
    uint64_t lookup_misses;
} LookupStats;

static LookupStats g_lookup_stats;

void lookup_stats_reset(void)
{
    g_lookup_stats.lookup_calls = 0U;
    g_lookup_stats.key_comparisons = 0U;
    g_lookup_stats.lookup_hits = 0U;
    g_lookup_stats.lookup_misses = 0U;
}

LookupStats lookup_stats_snapshot(void)
{
    return g_lookup_stats;
}


/* ---------- Stage 8: minimum-rank scan instrumentation ---------- */

typedef struct {
    uint64_t min_scan_calls;
    uint64_t rank_comparisons;
} MinRankStats;

static MinRankStats g_min_rank_stats;

void min_rank_stats_reset(void)
{
    g_min_rank_stats.min_scan_calls = 0U;
    g_min_rank_stats.rank_comparisons = 0U;
}

MinRankStats min_rank_stats_snapshot(void)
{
    return g_min_rank_stats;
}


/* ---------- Stage 7: standalone hash table ---------- */

/*
 * Stage 7 uses open addressing with linear probing.
 *
 * The table stores key -> CacheEntry * mappings. It is tested independently
 * from Cache and cache_get() so hash-table correctness can be established
 * before any cache integration takes place.
 */
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

/*
 * Pedagogical Stage 7 hash function.
 *
 * A modulo hash keeps collision tests deterministic and easy to explain.
 * Keys K and K + HASH_TABLE_CAPACITY intentionally collide.
 */
size_t hash_table_bucket(CacheKey key)
{
    return (size_t)(key % (CacheKey)HASH_TABLE_CAPACITY);
}

/* Initialize every slot as empty. Complexity: O(M), M = table capacity. */
void hash_table_init(HashTable *table)
{
    size_t i;

    if (table == NULL) {
        return;
    }

    table->size = 0U;

    for (i = 0U; i < HASH_TABLE_CAPACITY; ++i) {
        table->slots[i].key = 0ULL;
        table->slots[i].entry = NULL;
        table->slots[i].state = HASH_SLOT_EMPTY;
    }
}

/*
 * Look up a key with open addressing and linear probing.
 *
 * Stop at an EMPTY slot because the key cannot occur later in that probe
 * chain. A DELETED slot is a tombstone, so probing must continue through it.
 *
 * Expected/average complexity at controlled load: O(1)
 * Worst case: O(M)
 */
CacheEntry *hash_table_lookup(const HashTable *table, CacheKey key)
{
    size_t start;
    size_t probe;

    if (table == NULL) {
        return NULL;
    }

    start = hash_table_bucket(key);

    for (probe = 0U; probe < HASH_TABLE_CAPACITY; ++probe) {
        size_t index = (start + probe) % HASH_TABLE_CAPACITY;
        const HashSlot *slot = &table->slots[index];

        if (slot->state == HASH_SLOT_EMPTY) {
            return NULL;
        }

        if (slot->state == HASH_SLOT_OCCUPIED && slot->key == key) {
            return slot->entry;
        }
    }

    return NULL;
}

/*
 * Insert a key -> CacheEntry * mapping.
 *
 * Duplicate keys are rejected. The first tombstone encountered is remembered
 * and reused if the key is not already present.
 *
 * Expected/average complexity at controlled load: O(1)
 * Worst case: O(M)
 */
int hash_table_insert(HashTable *table, CacheEntry *entry)
{
    size_t start;
    size_t probe;
    size_t first_deleted = 0U;
    int have_deleted = 0;

    if (table == NULL || entry == NULL || table->size >= HASH_TABLE_CAPACITY) {
        return 0;
    }

    start = hash_table_bucket(entry->key);

    for (probe = 0U; probe < HASH_TABLE_CAPACITY; ++probe) {
        size_t index = (start + probe) % HASH_TABLE_CAPACITY;
        HashSlot *slot = &table->slots[index];

        if (slot->state == HASH_SLOT_OCCUPIED) {
            if (slot->key == entry->key) {
                return 0;
            }
            continue;
        }

        if (slot->state == HASH_SLOT_DELETED) {
            if (!have_deleted) {
                first_deleted = index;
                have_deleted = 1;
            }
            continue;
        }

        /* HASH_SLOT_EMPTY */
        if (have_deleted) {
            slot = &table->slots[first_deleted];
        }

        slot->key = entry->key;
        slot->entry = entry;
        slot->state = HASH_SLOT_OCCUPIED;
        ++table->size;
        return 1;
    }

    if (have_deleted) {
        HashSlot *slot = &table->slots[first_deleted];

        slot->key = entry->key;
        slot->entry = entry;
        slot->state = HASH_SLOT_OCCUPIED;
        ++table->size;
        return 1;
    }

    return 0;
}

/*
 * Remove a mapping while preserving the probe chain with a tombstone.
 *
 * Expected/average complexity at controlled load: O(1)
 * Worst case: O(M)
 */
int hash_table_remove(HashTable *table,
                      CacheKey key,
                      CacheEntry **removed_entry)
{
    size_t start;
    size_t probe;

    if (table == NULL) {
        return 0;
    }

    start = hash_table_bucket(key);

    for (probe = 0U; probe < HASH_TABLE_CAPACITY; ++probe) {
        size_t index = (start + probe) % HASH_TABLE_CAPACITY;
        HashSlot *slot = &table->slots[index];

        if (slot->state == HASH_SLOT_EMPTY) {
            return 0;
        }

        if (slot->state == HASH_SLOT_OCCUPIED && slot->key == key) {
            if (removed_entry != NULL) {
                *removed_entry = slot->entry;
            }

            slot->entry = NULL;
            slot->state = HASH_SLOT_DELETED;
            --table->size;
            return 1;
        }
    }

    return 0;
}

/*
 * Validate the standalone hash table without connecting it to Cache.
 *
 * The validator checks slot accounting and verifies that every occupied
 * mapping can be found through the table's own lookup function.
 */
int hash_table_validate(const HashTable *table)
{
    size_t i;
    size_t occupied = 0U;

    if (table == NULL || table->size > HASH_TABLE_CAPACITY) {
        return 0;
    }

    for (i = 0U; i < HASH_TABLE_CAPACITY; ++i) {
        const HashSlot *slot = &table->slots[i];

        if (slot->state == HASH_SLOT_OCCUPIED) {
            if (slot->entry == NULL || slot->entry->key != slot->key) {
                return 0;
            }

            if (hash_table_lookup(table, slot->key) != slot->entry) {
                return 0;
            }

            ++occupied;
        } else if (slot->state == HASH_SLOT_EMPTY) {
            if (slot->entry != NULL) {
                return 0;
            }
        } else if (slot->state == HASH_SLOT_DELETED) {
            if (slot->entry != NULL) {
                return 0;
            }
        } else {
            return 0;
        }
    }

    return occupied == table->size;
}


/* ---------- Stage 5: structural invariants ---------- */

/*
 * Validate the internal structural invariants of a cache.
 *
 * Invariants checked:
 *   - cache pointer is non-NULL
 *   - configured capacity is in [1, MAX_CACHE_CAPACITY]
 *   - resident size never exceeds configured capacity
 *   - no two resident entries have the same key
 *
 * This function returns a status instead of asserting so tests can verify
 * that deliberately corrupted cache states are detected.
 *
 * Complexity:
 *   O(N^2) because duplicate-key validation compares resident pairs.
 *
 * This validation cost is for correctness/debug checking only and is not
 * part of the intended production cache algorithm.
 */
int cache_validate(const Cache *cache)
{
    size_t i;
    size_t j;

    if (cache == NULL) {
        return 0;
    }

    if (cache->capacity == 0U || cache->capacity > MAX_CACHE_CAPACITY) {
        return 0;
    }

    if (cache->size > cache->capacity) {
        return 0;
    }

    for (i = 0U; i < cache->size; ++i) {
        for (j = i + 1U; j < cache->size; ++j) {
            if (cache->entries[i].key == cache->entries[j].key) {
                return 0;
            }
        }
    }

    return 1;
}

/*
 * Development-time assertion wrapper.
 * Standard C assert() is active unless NDEBUG is defined at build time.
 */
void cache_assert_invariants(const Cache *cache)
{
    assert(cache_validate(cache));
}

/* ---------- Stage 1: deterministic database abstraction ---------- */

CacheEntry db_read_entry(CacheKey key)
{
    CacheEntry e;

    e.key = key;
    e.value = key * 100ULL;
    e.rank = (Rank)(key * 10ULL);
    e.heap_index = HEAP_INDEX_NONE;

    return e;
}

/* ---------- Stage 2: simplest cache implementation ---------- */

/*
 * Initialize an empty cache.
 *
 * Returns:
 *   1 on success
 *   0 if capacity is invalid
 */
int cache_init(Cache *cache, size_t capacity)
{
    if (cache == NULL || capacity == 0U || capacity > MAX_CACHE_CAPACITY) {
        return 0;
    }

    cache->size = 0U;
    cache->capacity = capacity;
    cache_assert_invariants(cache);
    return 1;
}

/*
 * Check whether the cache has reached its configured capacity.
 *
 * Complexity:
 *   O(1)
 */
int cache_is_full(const Cache *cache)
{
    if (cache == NULL) {
        return 0;
    }

    cache_assert_invariants(cache);
    return cache->size >= cache->capacity;
}

/*
 * Find an entry by key using a linear scan.
 *
 * Returns:
 *   pointer to the matching entry on hit
 *   NULL on miss
 *
 * Complexity:
 *   O(N)
 */
CacheEntry *cache_lookup(Cache *cache, CacheKey key)
{
    size_t i;

    if (cache == NULL) {
        return NULL;
    }

    cache_assert_invariants(cache);
    ++g_lookup_stats.lookup_calls;

    for (i = 0U; i < cache->size; ++i) {
        ++g_lookup_stats.key_comparisons;

        if (cache->entries[i].key == key) {
            ++g_lookup_stats.lookup_hits;
            return &cache->entries[i];
        }
    }

    ++g_lookup_stats.lookup_misses;
    return NULL;
}

/*
 * Insert a new entry.
 *
 * Stage 2 policy:
 *   - reject duplicate keys
 *   - reject insertion if the cache is already full
 *
 * Returns:
 *   1 if inserted
 *   0 otherwise
 *
 * Complexity:
 *   O(N), because duplicate detection uses cache_lookup().
 */
int cache_insert(Cache *cache, CacheEntry entry)
{
    if (cache == NULL) {
        return 0;
    }

    cache_assert_invariants(cache);

    if (cache_is_full(cache)) {
        return 0;
    }

    if (cache_lookup(cache, entry.key) != NULL) {
        return 0;
    }

    cache->entries[cache->size] = entry;
    ++cache->size;

    cache_assert_invariants(cache);
    return 1;
}

/*
 * Find the array index of the entry having the minimum rank.
 *
 * Tie rule:
 *   if two entries have the same rank, the first one encountered
 *   in the array is selected.
 *
 * Returns:
 *   1 when an entry exists and writes its index to *min_index
 *   0 for an empty/invalid cache
 *
 * Complexity:
 *   O(N)
 */
int cache_find_min_rank_index(const Cache *cache, size_t *min_index)
{
    size_t i;
    size_t candidate;

    if (cache == NULL || min_index == NULL || cache->size == 0U) {
        return 0;
    }

    cache_assert_invariants(cache);
    ++g_min_rank_stats.min_scan_calls;
    candidate = 0U;

    for (i = 1U; i < cache->size; ++i) {
        ++g_min_rank_stats.rank_comparisons;
        if (cache->entries[i].rank < cache->entries[candidate].rank) {
            candidate = i;
        }
    }

    *min_index = candidate;
    return 1;
}

/* ---------- Stage 3: explicit linear minimum-rank eviction ---------- */

/*
 * Remove an entry when its array index is already known.
 *
 * The last valid resident entry is copied into the removed slot, so no
 * O(N) shift of the remaining array is required. Cache order has no
 * semantic meaning at this checkpoint.
 *
 * Returns:
 *   1 on successful removal
 *   0 for an invalid cache/index
 *
 * Complexity:
 *   O(1)
 */
int cache_remove_at(Cache *cache, size_t index, CacheEntry *removed_entry)
{
    if (cache == NULL || index >= cache->size) {
        return 0;
    }

    cache_assert_invariants(cache);

    if (removed_entry != NULL) {
        *removed_entry = cache->entries[index];
    }

    cache->entries[index] = cache->entries[cache->size - 1U];
    --cache->size;

    cache_assert_invariants(cache);
    return 1;
}

/*
 * Evict the minimum-ranked entry.
 *
 * Stage 3 composes two explicit operations:
 *   1. locate the minimum-ranked resident with a linear O(N) scan
 *   2. remove that known array slot in O(1)
 *
 * Overall complexity remains O(N), dominated by victim selection.
 */
int cache_evict_min(Cache *cache, CacheEntry *evicted_entry)
{
    size_t min_index;

    if (!cache_find_min_rank_index(cache, &min_index)) {
        return 0;
    }

    return cache_remove_at(cache, min_index, evicted_entry);
}


/* ---------- Stage 4: complete cache_get() path ---------- */

/*
 * Get an entry from the cache.
 *
 * Hit path:
 *   - linearly search the cache
 *   - return the resident entry
 *
 * Miss path:
 *   - read the entry through db_read_entry()
 *   - if the cache is full, evict the current minimum-rank resident
 *   - insert the fetched entry
 *   - return the newly resident entry
 *
 * Returns NULL only if cache state/operations prevent completion.
 *
 * Complexity with the current linear Stage 4 structures:
 *   hit:  O(N)
 *   miss: O(N) + DB-read cost
 *
 * The miss path can perform more than one linear pass. Optimization is
 * deliberately deferred to later stages.
 */
CacheEntry *cache_get(Cache *cache, CacheKey key)
{
    CacheEntry *found;
    CacheEntry fetched;

    if (cache == NULL) {
        return NULL;
    }

    cache_assert_invariants(cache);
    found = cache_lookup(cache, key);
    if (found != NULL) {
        return found;
    }

    fetched = db_read_entry(key);

    if (cache_is_full(cache)) {
        if (!cache_evict_min(cache, NULL)) {
            return NULL;
        }
    }

    if (!cache_insert(cache, fetched)) {
        return NULL;
    }

    return cache_lookup(cache, key);
}

/*
 * Print current cache contents as {key:rank, ...}.
 * Used only by the Stage 2 validation code.
 */
void cache_print(const Cache *cache)
{
    size_t i;

    printf("cache = {");

    if (cache != NULL) {
        for (i = 0U; i < cache->size; ++i) {
            printf("%llu:%lld",
                   cache->entries[i].key,
                   cache->entries[i].rank);

            if (i + 1U < cache->size) {
                printf(", ");
            }
        }
    }

    printf("}\n");
}

/*
 * Small PASS/FAIL helper for incremental validation.
 */
int check(int condition, const char *name)
{
    if (condition) {
        printf("[PASS] %s\n", name);
        return 1;
    }

    printf("[FAIL] %s\n", name);
    return 0;
}

/* ---------- Stage 6: deterministic bottleneck identification ---------- */

/*
 * Fill a cache directly with valid unique entries for lookup profiling.
 * This avoids cache_insert() because Stage 6 is measuring cache_lookup()
 * itself, not insertion's duplicate-check lookup.
 */
int stage6_fill_profile_cache(Cache *cache, size_t count)
{
    size_t i;

    if (!cache_init(cache, count)) {
        return 0;
    }

    for (i = 0U; i < count; ++i) {
        CacheKey key = (CacheKey)(i + 1U);

        cache->entries[i] = db_read_entry(key);
    }

    cache->size = count;
    cache_assert_invariants(cache);
    return 1;
}

/*
 * Profile one lookup and verify the exact number of key comparisons.
 */
int stage6_check_lookup_cost(Cache *cache,
                             CacheKey key,
                             uint64_t expected_comparisons,
                             int expect_hit,
                             const char *name)
{
    CacheEntry *entry;
    LookupStats stats;
    int passed;

    lookup_stats_reset();
    entry = cache_lookup(cache, key);
    stats = lookup_stats_snapshot();

    passed = (stats.lookup_calls == 1U &&
              stats.key_comparisons == expected_comparisons &&
              stats.lookup_hits == (expect_hit ? 1U : 0U) &&
              stats.lookup_misses == (expect_hit ? 0U : 1U) &&
              ((entry != NULL) == expect_hit));

    printf("[PROFILE] %-28s size=%zu comparisons=%llu result=%s\n",
           name,
           cache->size,
           (unsigned long long)stats.key_comparisons,
           entry != NULL ? "HIT" : "MISS");

    return check(passed, name);
}


/* ---------- Stage 7: independent hash-table validation ---------- */

int stage7_run_hash_table_tests(void)
{
    HashTable table;
    CacheEntry entries[7];
    CacheEntry *found;
    CacheEntry *removed = NULL;
    size_t bucket_1;
    size_t bucket_collision;
    int passed = 1;

    /*
     * 1 and 1 + HASH_TABLE_CAPACITY deliberately collide under the Stage 7
     * modulo hash. A third colliding key exercises tombstone reuse.
     */
    entries[0] = db_read_entry(10ULL);
    entries[1] = db_read_entry(20ULL);
    entries[2] = db_read_entry(30ULL);
    entries[3] = db_read_entry(1ULL);
    entries[4] = db_read_entry(1ULL + (CacheKey)HASH_TABLE_CAPACITY);
    entries[5] = db_read_entry(1ULL + (2ULL * (CacheKey)HASH_TABLE_CAPACITY));
    entries[6] = db_read_entry(20ULL); /* duplicate-key insertion attempt */

    printf("\n=== Stage 7: standalone hash-table lookup ===\n");

    hash_table_init(&table);
    passed &= check(table.size == 0U && hash_table_validate(&table),
                    "initialize empty hash table");

    passed &= check(hash_table_insert(&table, &entries[0]),
                    "hash insert key 10");
    passed &= check(hash_table_insert(&table, &entries[1]),
                    "hash insert key 20");
    passed &= check(hash_table_insert(&table, &entries[2]),
                    "hash insert key 30");
    passed &= check(table.size == 3U && hash_table_validate(&table),
                    "three hash mappings validate");

    found = hash_table_lookup(&table, 20ULL);
    passed &= check(found == &entries[1] && found->value == 2000ULL,
                    "hash lookup finds key 20");
    passed &= check(hash_table_lookup(&table, 99ULL) == NULL,
                    "hash lookup reports missing key 99");

    passed &= check(!hash_table_insert(&table, &entries[6]) &&
                    table.size == 3U,
                    "duplicate hash key is rejected");

    /* Collision path: both keys begin at the same bucket. */
    bucket_1 = hash_table_bucket(entries[3].key);
    bucket_collision = hash_table_bucket(entries[4].key);
    passed &= check(bucket_1 == bucket_collision,
                    "collision test keys share initial bucket");
    passed &= check(hash_table_insert(&table, &entries[3]),
                    "insert first collision key");
    passed &= check(hash_table_insert(&table, &entries[4]),
                    "linear probing inserts colliding key");
    passed &= check(hash_table_lookup(&table, entries[3].key) == &entries[3] &&
                    hash_table_lookup(&table, entries[4].key) == &entries[4],
                    "collision-chain lookups succeed");

    /* Delete the first colliding key; lookup must continue through tombstone. */
    passed &= check(hash_table_remove(&table, entries[3].key, &removed) &&
                    removed == &entries[3],
                    "remove first collision key");
    passed &= check(hash_table_lookup(&table, entries[3].key) == NULL,
                    "deleted hash key is absent");
    passed &= check(hash_table_lookup(&table, entries[4].key) == &entries[4],
                    "lookup crosses tombstone to colliding key");

    /* A new key in the same collision family should reuse the tombstone. */
    passed &= check(hash_table_insert(&table, &entries[5]),
                    "insert reuses deleted collision slot");
    passed &= check(hash_table_lookup(&table, entries[5].key) == &entries[5],
                    "lookup finds tombstone-reuse entry");
    passed &= check(hash_table_validate(&table),
                    "final standalone hash table validates");

    printf("Stage 7 hash-table validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}


/* ---------- Stage 9: standalone binary min-heap ---------- */

/*
 * Stage 9 validates a binary min-heap independently before any cache
 * integration. The heap stores pointers to CacheEntry objects and orders them
 * by rank. Equal ranks are broken deterministically by key.
 */
typedef struct {
    CacheEntry *items[MAX_CACHE_CAPACITY];
    size_t size;
} MinHeap;

/* Return non-zero when a should appear before b in the min-heap. */
int min_heap_entry_less(const CacheEntry *a, const CacheEntry *b)
{
    if (a->rank != b->rank) {
        return a->rank < b->rank;
    }

    return a->key < b->key;
}

void min_heap_init(MinHeap *heap)
{
    size_t i;

    if (heap == NULL) {
        return;
    }

    heap->size = 0U;
    for (i = 0U; i < MAX_CACHE_CAPACITY; ++i) {
        heap->items[i] = NULL;
    }
}

int min_heap_swap(MinHeap *heap, size_t a, size_t b)
{
    CacheEntry *entry_a;
    CacheEntry *entry_b;

    /*
     * Stage 15 treats swap as the core indexed-heap consistency primitive.
     * Refuse malformed requests before touching either the heap array or
     * reverse-position metadata.
     */
    if (heap == NULL ||
        a >= heap->size ||
        b >= heap->size ||
        heap->items[a] == NULL ||
        heap->items[b] == NULL) {
        return 0;
    }

    /*
     * A self-swap is a valid no-op. Reassert the reverse-position invariant
     * without unnecessarily exchanging pointers.
     */
    if (a == b) {
        heap->items[a]->heap_index = a;
        return 1;
    }

    entry_a = heap->items[a];
    entry_b = heap->items[b];

    /*
     * Commit both sides of the swap, then synchronize both reverse indices.
     * After this function returns success:
     *
     *     heap->items[a] == entry_b
     *     entry_b->heap_index == a
     *
     *     heap->items[b] == entry_a
     *     entry_a->heap_index == b
     */
    heap->items[a] = entry_b;
    heap->items[b] = entry_a;

    entry_b->heap_index = a;
    entry_a->heap_index = b;

    return 1;
}

void min_heap_sift_up(MinHeap *heap, size_t index)
{
    while (index > 0U) {
        size_t parent = (index - 1U) / 2U;

        if (!min_heap_entry_less(heap->items[index], heap->items[parent])) {
            break;
        }

        if (!min_heap_swap(heap, index, parent)) {
            return;
        }
        index = parent;
    }
}

void min_heap_sift_down(MinHeap *heap, size_t index)
{
    for (;;) {
        size_t left = 2U * index + 1U;
        size_t right = left + 1U;
        size_t smallest = index;

        if (left < heap->size &&
            min_heap_entry_less(heap->items[left], heap->items[smallest])) {
            smallest = left;
        }

        if (right < heap->size &&
            min_heap_entry_less(heap->items[right], heap->items[smallest])) {
            smallest = right;
        }

        if (smallest == index) {
            break;
        }

        if (!min_heap_swap(heap, index, smallest)) {
            return;
        }
        index = smallest;
    }
}


/*
 * Update the rank of one resident already stored in the indexed heap.
 *
 * Membership is validated through the Stage 14 reverse position:
 *
 *     entry->heap_index
 *     heap->items[entry->heap_index] == entry
 *
 * After changing the rank:
 *   lower rank  -> repair upward
 *   higher rank -> repair downward
 *   same rank   -> no movement
 *
 * Complexity: O(log N) worst case.
 */
int min_heap_update_rank(MinHeap *heap, CacheEntry *entry, Rank new_rank)
{
    size_t index;
    Rank old_rank;

    if (heap == NULL || entry == NULL) {
        return 0;
    }

    index = entry->heap_index;

    if (index == HEAP_INDEX_NONE ||
        index >= heap->size ||
        heap->items[index] != entry) {
        return 0;
    }

    old_rank = entry->rank;

    if (new_rank == old_rank) {
        return 1;
    }

    entry->rank = new_rank;

    if (new_rank < old_rank) {
        min_heap_sift_up(heap, index);
    } else {
        min_heap_sift_down(heap, index);
    }

    return 1;
}

/*
 * Insert one entry pointer and restore heap order by bubbling upward.
 * Complexity: O(log N) worst case.
 */
int min_heap_push(MinHeap *heap, CacheEntry *entry)
{
    size_t index;

    if (heap == NULL || entry == NULL || heap->size >= MAX_CACHE_CAPACITY) {
        return 0;
    }

    index = heap->size;
    heap->items[index] = entry;
    entry->heap_index = index;
    ++heap->size;
    min_heap_sift_up(heap, index);
    return 1;
}

/*
 * Return the minimum-ranked entry without removing it.
 * Complexity: O(1).
 */
CacheEntry *min_heap_peek(const MinHeap *heap)
{
    if (heap == NULL || heap->size == 0U) {
        return NULL;
    }

    return heap->items[0];
}

/*
 * Remove and return the root. Move the last item to the root and repair the
 * heap downward. Complexity: O(log N) worst case.
 */
CacheEntry *min_heap_pop_min(MinHeap *heap)
{
    CacheEntry *minimum;

    if (heap == NULL || heap->size == 0U) {
        return NULL;
    }

    minimum = heap->items[0];
    --heap->size;

    if (heap->size > 0U) {
        heap->items[0] = heap->items[heap->size];
        heap->items[heap->size] = NULL;
        heap->items[0]->heap_index = 0U;
        min_heap_sift_down(heap, 0U);
    } else {
        heap->items[0] = NULL;
    }

    minimum->heap_index = HEAP_INDEX_NONE;
    return minimum;
}

/*
 * Verify the binary min-heap invariant:
 * every parent is <= each existing child according to rank/key ordering.
 * Used only by Stage 9 tests.
 */
int min_heap_validate(const MinHeap *heap)
{
    size_t i;

    if (heap == NULL || heap->size > MAX_CACHE_CAPACITY) {
        return 0;
    }

    for (i = 0U; i < heap->size; ++i) {
        size_t left = 2U * i + 1U;
        size_t right = left + 1U;

        if (heap->items[i] == NULL ||
            heap->items[i]->heap_index != i) {
            return 0;
        }

        if (left < heap->size &&
            min_heap_entry_less(heap->items[left], heap->items[i])) {
            return 0;
        }

        if (right < heap->size &&
            min_heap_entry_less(heap->items[right], heap->items[i])) {
            return 0;
        }
    }

    return 1;
}

int stage9_run_min_heap_tests(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 50LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 80LL, HEAP_INDEX_NONE},
        {4ULL, 400ULL, 10LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 60LL, HEAP_INDEX_NONE},
        {6ULL, 600ULL, 20LL, HEAP_INDEX_NONE}
    };
    const Rank expected_ranks[] = {10LL, 20LL, 20LL, 50LL, 60LL, 80LL};
    const CacheKey expected_keys[] = {4ULL, 2ULL, 6ULL, 1ULL, 5ULL, 3ULL};
    size_t i;
    int passed = 1;

    printf("\n=== Stage 9: standalone binary min-heap ===\n");

    min_heap_init(&heap);
    passed &= check(heap.size == 0U && min_heap_peek(&heap) == NULL,
                    "initialize empty min-heap");
    passed &= check(min_heap_pop_min(&heap) == NULL,
                    "pop from empty min-heap returns NULL");
    passed &= check(min_heap_validate(&heap),
                    "empty min-heap validates");

    /* Insert ranks 50,20,80,10,60 plus an equal-rank tie at rank 20. */
    for (i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "push entry into min-heap");
        passed &= check(min_heap_validate(&heap),
                        "heap invariant holds after push");
    }

    passed &= check(min_heap_peek(&heap) == &entries[3] &&
                    min_heap_peek(&heap)->rank == 10LL,
                    "peek returns minimum rank 10");

    printf("Stage 9 pop order:\n");
    for (i = 0U; i < sizeof(expected_ranks) / sizeof(expected_ranks[0]); ++i) {
        CacheEntry *entry = min_heap_pop_min(&heap);

        if (entry != NULL) {
            printf("  pop %zu -> key=%llu rank=%lld\n",
                   i + 1U,
                   entry->key,
                   entry->rank);
        }

        passed &= check(entry != NULL &&
                        entry->rank == expected_ranks[i] &&
                        entry->key == expected_keys[i],
                        "pop order is ascending rank/key");
        passed &= check(min_heap_validate(&heap),
                        "heap invariant holds after pop");
    }

    passed &= check(heap.size == 0U && min_heap_peek(&heap) == NULL,
                    "heap is empty after all pops");

    /* Capacity guard: fill the heap, then reject one additional insertion. */
    min_heap_init(&heap);
    {
        CacheEntry full_entries[MAX_CACHE_CAPACITY];
        CacheEntry extra = {9999ULL, 999900ULL, -9999LL, HEAP_INDEX_NONE};

        for (i = 0U; i < MAX_CACHE_CAPACITY; ++i) {
            full_entries[i].key = (CacheKey)(1000U + i);
            full_entries[i].value = full_entries[i].key * 100ULL;
            full_entries[i].rank = (Rank)(1000LL - (long long)i);
            passed &= min_heap_push(&heap, &full_entries[i]);
        }

        passed &= check(heap.size == MAX_CACHE_CAPACITY &&
                        min_heap_validate(&heap),
                        "heap accepts exactly MAX_CACHE_CAPACITY entries");
        passed &= check(!min_heap_push(&heap, &extra),
                        "heap rejects insertion beyond capacity");
    }

    printf("Stage 9 heap finding: minimum is heap[0] in O(1); push/pop are O(log N).\n");
    printf("Stage 9 heap remains standalone and is not used by cache eviction.\n");
    printf("Stage 9 min-heap validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}


/* ---------- Stage 10: combine hash table + heap for Part 1 ---------- */

/*
 * Stage 20 runtime statistics for the integrated cache path.
 *
 * These counters deliberately live above the low-level hash/heap validators so
 * debug validation does not contaminate user-visible cache-operation metrics.
 */
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

void cache_stats_zero(CacheStats *stats)
{
    if (stats == NULL) {
        return;
    }

    stats->accesses = 0U;
    stats->hits = 0U;
    stats->misses = 0U;
    stats->db_reads = 0U;
    stats->insertions = 0U;
    stats->evictions = 0U;
    stats->rank_provider_calls = 0U;
    stats->rank_updates = 0U;
    stats->rank_decreases = 0U;
    stats->rank_unchanged = 0U;
    stats->rank_increases = 0U;
}

/*
 * Fixed-rank Part 1 cache.
 *
 * The resident entry array provides stable addresses for CacheEntry objects.
 * Unlike the earlier linear Cache, resident entries are never moved while
 * cached. A small free-slot stack recycles array slots after eviction.
 *
 * index     - key -> CacheEntry * lookup through the Stage 7 hash table
 * min_heap  - minimum-rank victim selection through the Stage 9 binary heap
 * active    - marks which resident slots currently contain live entries
 * free_stack/free_count - O(1) resident-slot allocation/recycling
 */
typedef struct {
    CacheEntry entries[MAX_CACHE_CAPACITY];
    unsigned char active[MAX_CACHE_CAPACITY];
    size_t free_stack[MAX_CACHE_CAPACITY];
    size_t free_count;
    size_t size;
    size_t capacity;
    HashTable index;
    MinHeap min_heap;
    CacheStats stats;
} Part1Cache;

int part1_cache_init(Part1Cache *cache, size_t capacity)
{
    size_t i;

    if (cache == NULL || capacity == 0U || capacity > MAX_CACHE_CAPACITY) {
        return 0;
    }

    cache->size = 0U;
    cache->capacity = capacity;
    cache->free_count = capacity;

    for (i = 0U; i < MAX_CACHE_CAPACITY; ++i) {
        cache->entries[i].key = 0ULL;
        cache->entries[i].value = 0ULL;
        cache->entries[i].rank = 0LL;
        cache->active[i] = 0U;
        cache->free_stack[i] = 0U;
    }

    /* Arrange the stack so the first allocation uses entries[0]. */
    for (i = 0U; i < capacity; ++i) {
        cache->free_stack[i] = capacity - 1U - i;
    }

    hash_table_init(&cache->index);
    min_heap_init(&cache->min_heap);
    cache_stats_zero(&cache->stats);
    return 1;
}

void part1_cache_stats_reset(Part1Cache *cache)
{
    if (cache == NULL) {
        return;
    }

    cache_stats_zero(&cache->stats);
}

CacheStats part1_cache_stats_snapshot(const Part1Cache *cache)
{
    CacheStats snapshot;

    cache_stats_zero(&snapshot);
    if (cache != NULL) {
        snapshot = cache->stats;
    }

    return snapshot;
}

int part1_cache_is_full(const Part1Cache *cache)
{
    return cache != NULL && cache->size >= cache->capacity;
}

/* Expected/average O(1) at the current controlled hash-table load. */
CacheEntry *part1_cache_lookup(const Part1Cache *cache, CacheKey key)
{
    if (cache == NULL) {
        return NULL;
    }

    return hash_table_lookup(&cache->index, key);
}

/*
 * Insert a fixed-rank entry into both indexing structures.
 *
 * Expected/average complexity:
 *   hash insert     O(1)
 *   heap push       O(log N)
 *   slot allocation O(1)
 * Overall: O(log N), dominated by heap maintenance.
 */
int part1_cache_insert(Part1Cache *cache,
                       CacheEntry entry,
                       CacheEntry **inserted_entry)
{
    size_t slot_index;
    CacheEntry *resident;

    if (cache == NULL || part1_cache_is_full(cache) || cache->free_count == 0U) {
        return 0;
    }

    if (hash_table_lookup(&cache->index, entry.key) != NULL) {
        return 0;
    }

    slot_index = cache->free_stack[--cache->free_count];
    resident = &cache->entries[slot_index];
    *resident = entry;
    resident->heap_index = HEAP_INDEX_NONE;
    cache->active[slot_index] = 1U;

    if (!hash_table_insert(&cache->index, resident)) {
        cache->active[slot_index] = 0U;
        cache->free_stack[cache->free_count++] = slot_index;
        return 0;
    }

    if (!min_heap_push(&cache->min_heap, resident)) {
        CacheEntry *removed = NULL;
        (void)hash_table_remove(&cache->index, resident->key, &removed);
        cache->active[slot_index] = 0U;
        cache->free_stack[cache->free_count++] = slot_index;
        return 0;
    }

    ++cache->size;
    ++cache->stats.insertions;

    if (inserted_entry != NULL) {
        *inserted_entry = resident;
    }

    return 1;
}

/*
 * Evict the current minimum-ranked resident.
 *
 * The heap root provides the victim directly, so no O(N) rank scan occurs.
 * Heap removal is O(log N); hash removal is expected O(1).
 */
int part1_cache_evict_min(Part1Cache *cache, CacheEntry *evicted_entry)
{
    CacheEntry *victim;
    CacheEntry *removed = NULL;
    ptrdiff_t slot_index;

    if (cache == NULL || cache->size == 0U) {
        return 0;
    }

    victim = min_heap_pop_min(&cache->min_heap);
    if (victim == NULL) {
        return 0;
    }

    if (!hash_table_remove(&cache->index, victim->key, &removed) ||
        removed != victim) {
        /* Valid integrated state should make this path unreachable. */
        (void)min_heap_push(&cache->min_heap, victim);
        return 0;
    }

    slot_index = victim - cache->entries;
    if (slot_index < 0 || (size_t)slot_index >= cache->capacity ||
        cache->active[(size_t)slot_index] == 0U) {
        return 0;
    }

    if (evicted_entry != NULL) {
        *evicted_entry = *victim;
    }

    cache->active[(size_t)slot_index] = 0U;
    cache->free_stack[cache->free_count++] = (size_t)slot_index;
    --cache->size;
    ++cache->stats.evictions;
    return 1;
}

/*
 * Complete fixed-rank Part 1 access path.
 *
 * Hit:
 *   expected O(1) hash lookup.
 *
 * Miss:
 *   DB read + optional O(log N) heap eviction + O(log N) insertion.
 * Rank does not change on lookup in Part 1.
 */
CacheEntry *part1_cache_get(Part1Cache *cache, CacheKey key)
{
    CacheEntry *resident;
    CacheEntry fetched;

    if (cache == NULL) {
        return NULL;
    }

    ++cache->stats.accesses;

    resident = part1_cache_lookup(cache, key);
    if (resident != NULL) {
        ++cache->stats.hits;
        return resident;
    }

    ++cache->stats.misses;
    ++cache->stats.db_reads;
    fetched = db_read_entry(key);

    if (part1_cache_is_full(cache)) {
        if (!part1_cache_evict_min(cache, NULL)) {
            return NULL;
        }
    }

    if (!part1_cache_insert(cache, fetched, &resident)) {
        return NULL;
    }

    return resident;
}

/*
 * Debug/test validator for the integrated Part 1 cache.
 * This is intentionally thorough rather than optimized.
 */
int part1_cache_validate(const Part1Cache *cache)
{
    size_t i;
    size_t j;
    size_t active_count = 0U;

    if (cache == NULL || cache->capacity == 0U ||
        cache->capacity > MAX_CACHE_CAPACITY ||
        cache->size > cache->capacity ||
        cache->free_count > cache->capacity ||
        cache->size + cache->free_count != cache->capacity) {
        return 0;
    }

    if (cache->index.size != cache->size ||
        cache->min_heap.size != cache->size ||
        !hash_table_validate(&cache->index) ||
        !min_heap_validate(&cache->min_heap)) {
        return 0;
    }

    for (i = 0U; i < cache->capacity; ++i) {
        if (cache->active[i] != 0U) {
            size_t heap_occurrences = 0U;
            CacheEntry *resident = (CacheEntry *)&cache->entries[i];

            ++active_count;

            if (hash_table_lookup(&cache->index, resident->key) != resident ||
                resident->heap_index >= cache->min_heap.size ||
                cache->min_heap.items[resident->heap_index] != resident) {
                return 0;
            }

            for (j = 0U; j < cache->min_heap.size; ++j) {
                if (cache->min_heap.items[j] == resident) {
                    ++heap_occurrences;
                }
            }

            if (heap_occurrences != 1U) {
                return 0;
            }
        }
    }

    if (active_count != cache->size) {
        return 0;
    }

    for (i = 0U; i < cache->free_count; ++i) {
        size_t slot = cache->free_stack[i];

        if (slot >= cache->capacity || cache->active[slot] != 0U) {
            return 0;
        }

        for (j = i + 1U; j < cache->free_count; ++j) {
            if (cache->free_stack[j] == slot) {
                return 0;
            }
        }
    }

    for (i = 0U; i < cache->min_heap.size; ++i) {
        CacheEntry *entry = cache->min_heap.items[i];
        ptrdiff_t slot;

        if (entry == NULL) {
            return 0;
        }

        slot = entry - cache->entries;
        if (slot < 0 || (size_t)slot >= cache->capacity ||
            cache->active[(size_t)slot] == 0U) {
            return 0;
        }
    }

    return 1;
}

int stage10_run_integrated_part1_tests(void)
{
    Part1Cache cache;
    CacheEntry *entry;
    CacheEntry *minimum;
    int passed = 1;

    printf("\n=== Stage 10: integrated hash table + min-heap for Part 1 ===\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize integrated Part 1 cache");
    passed &= check(part1_cache_validate(&cache),
                    "fresh integrated cache validates");

    entry = part1_cache_get(&cache, 1ULL);
    passed &= check(entry != NULL && entry->key == 1ULL && entry->rank == 10LL,
                    "Part 1 GET 1 miss inserts through hash+heap");

    entry = part1_cache_get(&cache, 2ULL);
    passed &= check(entry != NULL && entry->key == 2ULL && entry->rank == 20LL,
                    "Part 1 GET 2 miss inserts through hash+heap");

    entry = part1_cache_get(&cache, 3ULL);
    passed &= check(entry != NULL && entry->key == 3ULL && entry->rank == 30LL,
                    "Part 1 GET 3 miss fills integrated cache");

    passed &= check(cache.size == 3U &&
                    cache.index.size == 3U &&
                    cache.min_heap.size == 3U &&
                    part1_cache_validate(&cache),
                    "hash, heap, and resident counts stay synchronized");

    entry = part1_cache_get(&cache, 2ULL);
    passed &= check(entry != NULL && entry->key == 2ULL &&
                    cache.size == 3U && part1_cache_validate(&cache),
                    "Part 1 hit returns resident without rank change");

    minimum = min_heap_peek(&cache.min_heap);
    passed &= check(minimum != NULL && minimum->key == 1ULL &&
                    minimum->rank == 10LL,
                    "heap root identifies current minimum in O(1)");

    entry = part1_cache_get(&cache, 4ULL);
    passed &= check(entry != NULL && entry->key == 4ULL && entry->rank == 40LL,
                    "full-cache miss evicts heap minimum and inserts key 4");

    passed &= check(part1_cache_lookup(&cache, 1ULL) == NULL,
                    "evicted key 1 is absent from hash table");
    passed &= check(part1_cache_lookup(&cache, 2ULL) != NULL &&
                    part1_cache_lookup(&cache, 3ULL) != NULL &&
                    part1_cache_lookup(&cache, 4ULL) != NULL,
                    "keys 2, 3, and 4 remain resident");

    minimum = min_heap_peek(&cache.min_heap);
    passed &= check(minimum != NULL && minimum->key == 2ULL &&
                    minimum->rank == 20LL,
                    "heap minimum advances to key 2 after eviction");

    passed &= check(cache.size == 3U &&
                    cache.index.size == 3U &&
                    cache.min_heap.size == 3U &&
                    part1_cache_validate(&cache),
                    "integrated Part 1 cache validates after eviction");

    printf("Stage 10 Part 1 finding: hash lookup replaces the O(N) key scan.\n");
    printf("Stage 10 Part 1 finding: heap root replaces the O(N) minimum-rank scan.\n");
    printf("Stage 10 Part 1 validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}


/* ---------- Stage 8: identify second bottleneck ---------- */

/*
 * Prepare a valid cache whose minimum rank appears at a chosen array index.
 * Every other rank is intentionally larger. This lets Stage 8 prove that
 * minimum-rank search still scans the full resident set regardless of where
 * the minimum happens to reside.
 */
int stage8_prepare_rank_profile(Cache *cache,
                                size_t count,
                                size_t min_position)
{
    size_t i;

    if (cache == NULL || count == 0U || count > MAX_CACHE_CAPACITY ||
        min_position >= count) {
        return 0;
    }

    if (!cache_init(cache, count)) {
        return 0;
    }

    for (i = 0U; i < count; ++i) {
        CacheKey key = (CacheKey)(i + 1U);

        cache->entries[i].key = key;
        cache->entries[i].value = key * 100ULL;
        cache->entries[i].rank = (Rank)(1000LL + (long long)i);
    }

    cache->entries[min_position].rank = -1LL;
    cache->size = count;
    cache_assert_invariants(cache);
    return 1;
}

/*
 * Run one minimum-rank search and verify both the selected victim and the
 * exact number of rank comparisons. A linear scan over N residents compares
 * entries 1..N-1 against the current candidate, so it performs N-1 rank
 * comparisons for every non-empty cache regardless of victim position.
 */
int stage8_check_min_scan(Cache *cache,
                          size_t expected_min_position,
                          uint64_t expected_comparisons,
                          const char *name)
{
    size_t min_index = 0U;
    MinRankStats stats;
    int found;
    int passed;

    min_rank_stats_reset();
    found = cache_find_min_rank_index(cache, &min_index);
    stats = min_rank_stats_snapshot();

    passed = (found &&
              min_index == expected_min_position &&
              stats.min_scan_calls == 1U &&
              stats.rank_comparisons == expected_comparisons);

    printf("[PROFILE] %-30s size=%zu comparisons=%llu min-index=%zu\n",
           name,
           cache->size,
           (unsigned long long)stats.rank_comparisons,
           min_index);

    return check(passed, name);
}

int stage8_run_min_rank_bottleneck_tests(void)
{
    Cache profile;
    MinRankStats stats;
    size_t min_index;
    size_t sizes[] = {1U, 10U, 25U, 50U, 100U};
    size_t i;
    int passed = 1;

    printf("\n=== Stage 8: identify second bottleneck ===\n");

    /* Same N, different victim positions: scan cost must stay N-1. */
    passed &= check(stage8_prepare_rank_profile(&profile, 100U, 0U),
                    "prepare minimum-at-first profile");
    passed &= stage8_check_min_scan(&profile, 0U, 99U,
                                    "minimum at first entry");

    passed &= check(stage8_prepare_rank_profile(&profile, 100U, 49U),
                    "prepare minimum-at-middle profile");
    passed &= stage8_check_min_scan(&profile, 49U, 99U,
                                    "minimum at middle entry");

    passed &= check(stage8_prepare_rank_profile(&profile, 100U, 99U),
                    "prepare minimum-at-last profile");
    passed &= stage8_check_min_scan(&profile, 99U, 99U,
                                    "minimum at last entry");

    /* Scale N while keeping a valid unique cache. */
    printf("\nStage 8 minimum-rank scaling:\n");
    printf("  size    rank-comparisons\n");

    for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t n = sizes[i];

        passed &= check(stage8_prepare_rank_profile(&profile, n, n - 1U),
                        "prepare rank-scaling cache");

        min_rank_stats_reset();
        min_index = 0U;
        passed &= cache_find_min_rank_index(&profile, &min_index);
        stats = min_rank_stats_snapshot();

        printf("  %4zu    %llu\n",
               n,
               (unsigned long long)stats.rank_comparisons);

        passed &= check(min_index == n - 1U &&
                        stats.min_scan_calls == 1U &&
                        stats.rank_comparisons == (uint64_t)(n - 1U),
                        "minimum-rank comparisons equal N-1");
    }

    printf("\nStage 8 finding: cache_find_min_rank_index() scans all residents.\n");
    printf("Second bottleneck identified: O(N) minimum-rank victim selection.\n");
    printf("Known-slot removal remains O(1); eviction overall remains O(N).\n");
    printf("Stage 8 minimum-rank validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 11: test fixed-rank Part 1 thoroughly ---------- */

/*
 * Stage 11 intentionally changes no Part1Cache production algorithm.
 * These helpers exercise the Stage 10 integrated path more thoroughly.
 */

int stage11_test_empty_and_partial_capacity(void)
{
    Part1Cache cache;
    CacheEntry evicted;
    CacheEntry *entry;
    int passed = 1;

    printf("\n[Stage 11] empty and partial-capacity behavior\n");

    passed &= check(part1_cache_init(&cache, 4U),
                    "initialize capacity-4 Part 1 cache");
    passed &= check(part1_cache_validate(&cache),
                    "empty Part 1 cache validates");
    passed &= check(!part1_cache_is_full(&cache),
                    "empty cache is not full");
    passed &= check(part1_cache_lookup(&cache, 99ULL) == NULL,
                    "lookup miss on empty cache returns NULL");
    passed &= check(!part1_cache_evict_min(&cache, &evicted),
                    "eviction from empty cache is rejected");

    entry = part1_cache_get(&cache, 10ULL);
    passed &= check(entry != NULL &&
                    entry->key == 10ULL &&
                    entry->value == 1000ULL &&
                    entry->rank == 100LL,
                    "first miss inserts deterministic DB entry");

    entry = part1_cache_get(&cache, 20ULL);
    passed &= check(entry != NULL &&
                    entry->key == 20ULL &&
                    entry->rank == 200LL,
                    "second miss inserts without reaching capacity");

    passed &= check(cache.size == 2U &&
                    cache.index.size == 2U &&
                    cache.min_heap.size == 2U &&
                    cache.free_count == 2U &&
                    !part1_cache_is_full(&cache) &&
                    part1_cache_validate(&cache),
                    "partial cache keeps all structures synchronized");

    passed &= check(min_heap_peek(&cache.min_heap) != NULL &&
                    min_heap_peek(&cache.min_heap)->key == 10ULL,
                    "partial cache exposes correct minimum");

    return passed;
}

int stage11_test_repeated_hits_keep_fixed_rank(void)
{
    Part1Cache cache;
    CacheEntry *first;
    CacheEntry *again;
    CacheEntry *minimum_before;
    Rank original_rank;
    size_t i;
    int passed = 1;

    printf("\n[Stage 11] repeated hits preserve fixed-rank semantics\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize repeated-hit cache");

    first = part1_cache_get(&cache, 5ULL);
    passed &= check(first != NULL && first->rank == 50LL,
                    "initial GET 5 inserts rank 50");

    passed &= check(part1_cache_get(&cache, 8ULL) != NULL,
                    "GET 8 inserts second resident");
    passed &= check(part1_cache_get(&cache, 9ULL) != NULL,
                    "GET 9 fills repeated-hit cache");

    original_rank = first->rank;
    minimum_before = min_heap_peek(&cache.min_heap);

    for (i = 0U; i < 10U; ++i) {
        again = part1_cache_get(&cache, 5ULL);

        passed &= check(again == first &&
                        again->rank == original_rank &&
                        cache.size == 3U &&
                        cache.index.size == 3U &&
                        cache.min_heap.size == 3U &&
                        min_heap_peek(&cache.min_heap) == minimum_before &&
                        part1_cache_validate(&cache),
                        "repeated hit returns same resident and leaves rank/heap unchanged");
    }

    return passed;
}

int stage11_test_duplicate_rejection(void)
{
    Part1Cache cache;
    CacheEntry original = {7ULL, 700ULL, 70LL, HEAP_INDEX_NONE};
    CacheEntry duplicate = {7ULL, 7777ULL, 1LL, HEAP_INDEX_NONE};
    CacheEntry *resident = NULL;
    CacheEntry *before;
    size_t size_before;
    size_t hash_before;
    size_t heap_before;
    size_t free_before;
    int passed = 1;

    printf("\n[Stage 11] duplicate-key rejection\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize duplicate test cache");
    passed &= check(part1_cache_insert(&cache, original, &resident),
                    "insert original key 7");

    before = resident;
    size_before = cache.size;
    hash_before = cache.index.size;
    heap_before = cache.min_heap.size;
    free_before = cache.free_count;

    resident = NULL;
    passed &= check(!part1_cache_insert(&cache, duplicate, &resident),
                    "duplicate key insertion is rejected");

    resident = part1_cache_lookup(&cache, 7ULL);
    passed &= check(resident == before &&
                    resident != NULL &&
                    resident->value == 700ULL &&
                    resident->rank == 70LL &&
                    cache.size == size_before &&
                    cache.index.size == hash_before &&
                    cache.min_heap.size == heap_before &&
                    cache.free_count == free_before &&
                    part1_cache_validate(&cache),
                    "duplicate rejection leaves integrated state unchanged");

    return passed;
}

int stage11_test_equal_rank_tie(void)
{
    Part1Cache cache;
    CacheEntry a = {30ULL, 3000ULL, 50LL, HEAP_INDEX_NONE};
    CacheEntry b = {10ULL, 1000ULL, 50LL, HEAP_INDEX_NONE};
    CacheEntry c = {20ULL, 2000ULL, 50LL, HEAP_INDEX_NONE};
    CacheEntry evicted;
    CacheEntry *minimum;
    int passed = 1;

    printf("\n[Stage 11] equal-rank deterministic tie handling\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize equal-rank cache");
    passed &= check(part1_cache_insert(&cache, a, NULL),
                    "insert key 30 rank 50");
    passed &= check(part1_cache_insert(&cache, b, NULL),
                    "insert key 10 rank 50");
    passed &= check(part1_cache_insert(&cache, c, NULL),
                    "insert key 20 rank 50");

    minimum = min_heap_peek(&cache.min_heap);
    passed &= check(minimum != NULL &&
                    minimum->key == 10ULL &&
                    minimum->rank == 50LL,
                    "equal ranks choose lower key as heap minimum");

    passed &= check(part1_cache_evict_min(&cache, &evicted),
                    "evict equal-rank minimum");
    passed &= check(evicted.key == 10ULL &&
                    evicted.rank == 50LL &&
                    part1_cache_lookup(&cache, 10ULL) == NULL &&
                    part1_cache_lookup(&cache, 20ULL) != NULL &&
                    part1_cache_lookup(&cache, 30ULL) != NULL &&
                    part1_cache_validate(&cache),
                    "equal-rank eviction removes deterministic key 10");

    return passed;
}

int stage11_test_integrated_hash_collisions(void)
{
    Part1Cache cache;
    CacheEntry a = {1ULL, 100ULL, 30LL, HEAP_INDEX_NONE};
    CacheEntry b = {212ULL, 21200ULL, 10LL, HEAP_INDEX_NONE};
    CacheEntry c = {423ULL, 42300ULL, 20LL, HEAP_INDEX_NONE};
    CacheEntry evicted;
    int passed = 1;

    printf("\n[Stage 11] integrated hash collisions and tombstone traversal\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize collision cache");

    /* 1, 212, and 423 all hash to the same initial bucket modulo 211. */
    passed &= check(part1_cache_insert(&cache, a, NULL),
                    "insert collision key 1");
    passed &= check(part1_cache_insert(&cache, b, NULL),
                    "insert collision key 212");
    passed &= check(part1_cache_insert(&cache, c, NULL),
                    "insert collision key 423");

    passed &= check(part1_cache_lookup(&cache, 1ULL) != NULL &&
                    part1_cache_lookup(&cache, 212ULL) != NULL &&
                    part1_cache_lookup(&cache, 423ULL) != NULL &&
                    part1_cache_validate(&cache),
                    "all colliding keys are reachable through integrated hash index");

    passed &= check(part1_cache_evict_min(&cache, &evicted),
                    "evict minimum colliding key");
    passed &= check(evicted.key == 212ULL && evicted.rank == 10LL,
                    "heap selects key 212 as collision-set victim");

    passed &= check(part1_cache_lookup(&cache, 212ULL) == NULL &&
                    part1_cache_lookup(&cache, 423ULL) != NULL &&
                    part1_cache_lookup(&cache, 1ULL) != NULL &&
                    part1_cache_validate(&cache),
                    "remaining colliding keys survive hash tombstone");

    return passed;
}

int stage11_test_capacity_boundaries(void)
{
    Part1Cache cache;
    Part1Cache maximum;
    CacheEntry extra = {1001ULL, 100100ULL, 10010LL, HEAP_INDEX_NONE};
    CacheEntry *entry;
    size_t i;
    int passed = 1;

    printf("\n[Stage 11] capacity boundaries\n");

    passed &= check(!part1_cache_init(NULL, 1U),
                    "NULL Part 1 cache initialization is rejected");
    passed &= check(!part1_cache_init(&cache, 0U),
                    "zero capacity is rejected");
    passed &= check(!part1_cache_init(&cache, MAX_CACHE_CAPACITY + 1U),
                    "capacity above maximum is rejected");

    passed &= check(part1_cache_init(&cache, 1U),
                    "capacity-one cache initializes");
    entry = part1_cache_get(&cache, 9ULL);
    passed &= check(entry != NULL &&
                    entry->key == 9ULL &&
                    part1_cache_is_full(&cache) &&
                    part1_cache_validate(&cache),
                    "capacity-one cache becomes full after one insert");

    passed &= check(!part1_cache_insert(&cache, extra, NULL),
                    "direct insert beyond full capacity is rejected");
    passed &= check(part1_cache_lookup(&cache, 9ULL) != NULL &&
                    cache.size == 1U &&
                    part1_cache_validate(&cache),
                    "failed over-capacity insert leaves state unchanged");

    entry = part1_cache_get(&cache, 10ULL);
    passed &= check(entry != NULL &&
                    entry->key == 10ULL &&
                    part1_cache_lookup(&cache, 9ULL) == NULL &&
                    cache.size == 1U &&
                    part1_cache_validate(&cache),
                    "capacity-one miss evicts old minimum and reuses capacity");

    passed &= check(part1_cache_init(&maximum, MAX_CACHE_CAPACITY),
                    "maximum-capacity cache initializes");

    for (i = 0U; i < MAX_CACHE_CAPACITY; ++i) {
        CacheEntry e;
        e.key = (CacheKey)(10000U + i);
        e.value = (unsigned long long)e.key * 100ULL;
        e.rank = (Rank)(i + 1U);

        if (!part1_cache_insert(&maximum, e, NULL)) {
            passed &= check(0, "fill maximum-capacity cache");
            break;
        }
    }

    passed &= check(maximum.size == MAX_CACHE_CAPACITY &&
                    maximum.index.size == MAX_CACHE_CAPACITY &&
                    maximum.min_heap.size == MAX_CACHE_CAPACITY &&
                    maximum.free_count == 0U &&
                    part1_cache_is_full(&maximum) &&
                    part1_cache_validate(&maximum),
                    "maximum-capacity cache fills and validates");

    passed &= check(!part1_cache_insert(&maximum, extra, NULL),
                    "maximum-capacity direct overflow is rejected");

    return passed;
}

int stage11_test_slot_reuse_and_pointer_stability(void)
{
    Part1Cache cache;
    CacheEntry *p1;
    CacheEntry *p2;
    CacheEntry *p3;
    CacheEntry *p4;
    int passed = 1;

    printf("\n[Stage 11] stable resident addresses and free-slot reuse\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize slot-reuse cache");

    p1 = part1_cache_get(&cache, 1ULL);
    p2 = part1_cache_get(&cache, 2ULL);
    p3 = part1_cache_get(&cache, 3ULL);

    passed &= check(p1 != NULL && p2 != NULL && p3 != NULL &&
                    part1_cache_validate(&cache),
                    "capture initial stable resident pointers");

    p4 = part1_cache_get(&cache, 4ULL);

    passed &= check(p4 != NULL &&
                    part1_cache_lookup(&cache, 1ULL) == NULL &&
                    part1_cache_lookup(&cache, 2ULL) == p2 &&
                    part1_cache_lookup(&cache, 3ULL) == p3 &&
                    part1_cache_lookup(&cache, 4ULL) == p4 &&
                    part1_cache_validate(&cache),
                    "surviving resident pointers remain stable after eviction");

    passed &= check(p4 == p1,
                    "new resident reuses the freed victim slot");

    return passed;
}

int stage11_test_repeated_evictions_and_consistency(void)
{
    Part1Cache cache;
    CacheEntry *entry;
    CacheKey key;
    int passed = 1;

    printf("\n[Stage 11] repeated evictions and cross-structure consistency\n");

    passed &= check(part1_cache_init(&cache, 5U),
                    "initialize repeated-eviction cache");

    for (key = 1ULL; key <= 5ULL; ++key) {
        entry = part1_cache_get(&cache, key);
        passed &= check(entry != NULL &&
                        entry->key == key &&
                        part1_cache_validate(&cache),
                        "fill repeated-eviction cache");
    }

    /* Hits must not alter fixed ranks or resident count. */
    passed &= check(part1_cache_get(&cache, 3ULL) ==
                    part1_cache_lookup(&cache, 3ULL),
                    "hit key 3 returns existing resident");
    passed &= check(part1_cache_get(&cache, 5ULL) ==
                    part1_cache_lookup(&cache, 5ULL),
                    "hit key 5 returns existing resident");
    passed &= check(cache.size == 5U && part1_cache_validate(&cache),
                    "repeated hits keep full cache structurally unchanged");

    for (key = 6ULL; key <= 8ULL; ++key) {
        entry = part1_cache_get(&cache, key);
        passed &= check(entry != NULL &&
                        entry->key == key &&
                        cache.size == 5U &&
                        cache.index.size == 5U &&
                        cache.min_heap.size == 5U &&
                        cache.free_count == 0U &&
                        part1_cache_validate(&cache),
                        "full-cache miss preserves synchronized size after eviction");
    }

    passed &= check(part1_cache_lookup(&cache, 1ULL) == NULL &&
                    part1_cache_lookup(&cache, 2ULL) == NULL &&
                    part1_cache_lookup(&cache, 3ULL) == NULL &&
                    part1_cache_lookup(&cache, 4ULL) != NULL &&
                    part1_cache_lookup(&cache, 5ULL) != NULL &&
                    part1_cache_lookup(&cache, 6ULL) != NULL &&
                    part1_cache_lookup(&cache, 7ULL) != NULL &&
                    part1_cache_lookup(&cache, 8ULL) != NULL,
                    "repeated evictions retain the five highest fixed ranks");

    passed &= check(min_heap_peek(&cache.min_heap) != NULL &&
                    min_heap_peek(&cache.min_heap)->key == 4ULL &&
                    min_heap_peek(&cache.min_heap)->rank == 40LL &&
                    part1_cache_validate(&cache),
                    "heap minimum advances correctly after repeated evictions");

    return passed;
}

int stage11_run_part1_thorough_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 11: thorough fixed-rank Part 1 validation ===\n");

    passed &= stage11_test_empty_and_partial_capacity();
    passed &= stage11_test_repeated_hits_keep_fixed_rank();
    passed &= stage11_test_duplicate_rejection();
    passed &= stage11_test_equal_rank_tie();
    passed &= stage11_test_integrated_hash_collisions();
    passed &= stage11_test_capacity_boundaries();
    passed &= stage11_test_slot_reuse_and_pointer_stability();
    passed &= stage11_test_repeated_evictions_and_consistency();

    printf("\nStage 11 Part 1 coverage:\n");
    printf("  empty/partial capacity       : tested\n");
    printf("  repeated fixed-rank hits     : tested\n");
    printf("  duplicate rejection          : tested\n");
    printf("  equal-rank deterministic tie : tested\n");
    printf("  integrated hash collisions   : tested\n");
    printf("  capacity boundaries          : tested\n");
    printf("  stable addresses/slot reuse  : tested\n");
    printf("  repeated evictions           : tested\n");
    printf("Stage 11 Part 1 validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 12: begin Part 2 rank-change contract ---------- */

/*
 * Part 2 changes one fundamental assumption from Part 1:
 *
 *   after an entry is looked up, getEntryRank(entry) may return a new rank.
 *
 * The new rank is not constrained to move in one direction. It may be:
 *   - lower than the existing rank,
 *   - equal to the existing rank, or
 *   - higher than the existing rank.
 *
 * Stage 12 models that contract and demonstrates the limitation of the
 * existing ordinary binary heap. It intentionally does NOT implement heap
 * repair for an arbitrary resident entry yet.
 */

typedef enum {
    PART2_RANK_DECREASE = 0,
    PART2_RANK_UNCHANGED = 1,
    PART2_RANK_INCREASE = 2
} Part2RankScenario;

/*
 * Deterministic Stage 12 stand-in for the interview-supplied getEntryRank().
 *
 * The actual problem statement treats getEntryRank(entry) as an external
 * ranking function. For this incremental checkpoint we need deterministic
 * values so tests can prove all three directions of change.
 *
 * Test precondition: the ranks used here are comfortably away from signed
 * overflow/underflow.
 */
Rank stage12_get_entry_rank(const CacheEntry *entry,
                            Part2RankScenario scenario)
{
    if (entry == NULL) {
        return 0LL;
    }

    switch (scenario) {
        case PART2_RANK_DECREASE:
            return entry->rank - 30LL;

        case PART2_RANK_INCREASE:
            return entry->rank + 30LL;

        case PART2_RANK_UNCHANGED:
        default:
            return entry->rank;
    }
}

/*
 * Locate a resident through the integrated hash table and apply the new rank
 * returned by the Stage 12 getEntryRank() stand-in.
 *
 * IMPORTANT:
 * This helper intentionally does not repair the heap. That is the behavior
 * under examination in Stage 12, not the final Part 2 implementation.
 */
int stage12_apply_rank_change_without_heap_repair(
    Part1Cache *cache,
    CacheKey key,
    Part2RankScenario scenario,
    Rank *old_rank,
    Rank *new_rank,
    CacheEntry **resident_out)
{
    CacheEntry *resident;
    Rank updated;

    if (cache == NULL) {
        return 0;
    }

    resident = part1_cache_lookup(cache, key);
    if (resident == NULL) {
        return 0;
    }

    updated = stage12_get_entry_rank(resident, scenario);

    if (old_rank != NULL) {
        *old_rank = resident->rank;
    }

    resident->rank = updated;

    if (new_rank != NULL) {
        *new_rank = updated;
    }

    if (resident_out != NULL) {
        *resident_out = resident;
    }

    return 1;
}

/*
 * Restore one resident rank after a Stage 12 experiment.
 *
 * Stage 12 experiments change exactly one rank and then restore it before the
 * next case. Because the original Stage 10 heap was valid before the mutation,
 * restoring the original rank restores the original ordering relationship.
 */
int stage12_restore_rank(Part1Cache *cache,
                         CacheKey key,
                         Rank original_rank)
{
    CacheEntry *resident;

    if (cache == NULL) {
        return 0;
    }

    resident = part1_cache_lookup(cache, key);
    if (resident == NULL) {
        return 0;
    }

    resident->rank = original_rank;
    return 1;
}

int stage12_prepare_part2_probe_cache(Part1Cache *cache)
{
    CacheEntry e1 = {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE};
    CacheEntry e2 = {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE};
    CacheEntry e3 = {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE};

    if (!part1_cache_init(cache, 3U)) {
        return 0;
    }

    if (!part1_cache_insert(cache, e1, NULL) ||
        !part1_cache_insert(cache, e2, NULL) ||
        !part1_cache_insert(cache, e3, NULL)) {
        return 0;
    }

    return part1_cache_validate(cache);
}

int stage12_test_rank_direction_contract(void)
{
    CacheEntry sample = {7ULL, 700ULL, 70LL, HEAP_INDEX_NONE};
    Rank lower;
    Rank same;
    Rank higher;
    int passed = 1;

    printf("\n[Stage 12] Part 2 getEntryRank direction contract\n");

    lower = stage12_get_entry_rank(&sample, PART2_RANK_DECREASE);
    same = stage12_get_entry_rank(&sample, PART2_RANK_UNCHANGED);
    higher = stage12_get_entry_rank(&sample, PART2_RANK_INCREASE);

    passed &= check(lower < sample.rank,
                    "getEntryRank can return a lower rank");
    passed &= check(same == sample.rank,
                    "getEntryRank can return an unchanged rank");
    passed &= check(higher > sample.rank,
                    "getEntryRank can return a higher rank");

    return passed;
}

int stage12_test_decrease_can_stale_heap(void)
{
    Part1Cache cache;
    CacheEntry *resident;
    CacheEntry *heap_root;
    Rank old_rank;
    Rank new_rank;
    int passed = 1;

    printf("\n[Stage 12] decreasing a non-root rank can stale the ordinary heap\n");

    passed &= check(stage12_prepare_part2_probe_cache(&cache),
                    "prepare valid Part 2 probe cache");

    /*
     * Initial heap minimum is key 1 / rank 10.
     * Change key 3 from rank 30 to rank 0 without heap repair.
     * Key 3 should now be the true minimum, but it is still below the root in
     * the existing heap array.
     */
    passed &= check(stage12_apply_rank_change_without_heap_repair(
                        &cache,
                        3ULL,
                        PART2_RANK_DECREASE,
                        &old_rank,
                        &new_rank,
                        &resident),
                    "apply lower rank to key 3");

    heap_root = min_heap_peek(&cache.min_heap);

    passed &= check(old_rank == 30LL &&
                    new_rank == 0LL &&
                    resident != NULL &&
                    resident->key == 3ULL,
                    "key 3 rank changes from 30 to 0");

    passed &= check(part1_cache_lookup(&cache, 3ULL) == resident,
                    "hash lookup still finds the same resident after rank change");

    passed &= check(heap_root != NULL &&
                    heap_root->key == 1ULL &&
                    heap_root->rank == 10LL &&
                    resident->rank < heap_root->rank,
                    "heap root is stale after unrepaired rank decrease");

    passed &= check(!min_heap_validate(&cache.min_heap),
                    "heap validator detects rank-decrease ordering violation");
    passed &= check(!part1_cache_validate(&cache),
                    "integrated validator rejects stale heap after rank decrease");

    passed &= check(stage12_restore_rank(&cache, 3ULL, old_rank),
                    "restore key 3 original rank");
    passed &= check(part1_cache_validate(&cache),
                    "restoring original rank restores valid Part 1 state");

    return passed;
}

int stage12_test_increase_can_stale_heap(void)
{
    Part1Cache cache;
    CacheEntry *resident;
    CacheEntry *heap_root;
    Rank old_rank;
    Rank new_rank;
    int passed = 1;

    printf("\n[Stage 12] increasing the root rank can stale the ordinary heap\n");

    passed &= check(stage12_prepare_part2_probe_cache(&cache),
                    "prepare valid Part 2 probe cache");

    /*
     * Initial heap root is key 1 / rank 10.
     * Change it to rank 40 without heap repair.
     * Keys 2 and 3 now outrank the root, so the heap property is violated.
     */
    passed &= check(stage12_apply_rank_change_without_heap_repair(
                        &cache,
                        1ULL,
                        PART2_RANK_INCREASE,
                        &old_rank,
                        &new_rank,
                        &resident),
                    "apply higher rank to heap-root key 1");

    heap_root = min_heap_peek(&cache.min_heap);

    passed &= check(old_rank == 10LL &&
                    new_rank == 40LL &&
                    resident != NULL &&
                    resident->key == 1ULL,
                    "key 1 rank changes from 10 to 40");

    passed &= check(part1_cache_lookup(&cache, 1ULL) == resident,
                    "hash lookup remains valid after root rank change");

    passed &= check(heap_root == resident &&
                    heap_root->rank == 40LL &&
                    part1_cache_lookup(&cache, 2ULL)->rank == 20LL,
                    "ordinary heap still exposes stale root after rank increase");

    passed &= check(!min_heap_validate(&cache.min_heap),
                    "heap validator detects rank-increase ordering violation");
    passed &= check(!part1_cache_validate(&cache),
                    "integrated validator rejects stale heap after rank increase");

    passed &= check(stage12_restore_rank(&cache, 1ULL, old_rank),
                    "restore key 1 original rank");
    passed &= check(part1_cache_validate(&cache),
                    "restoring root rank restores valid Part 1 state");

    return passed;
}

int stage12_test_unchanged_rank_needs_no_repair(void)
{
    Part1Cache cache;
    CacheEntry *resident;
    CacheEntry *before_root;
    CacheEntry *after_root;
    Rank old_rank;
    Rank new_rank;
    int passed = 1;

    printf("\n[Stage 12] unchanged rank preserves ordinary heap validity\n");

    passed &= check(stage12_prepare_part2_probe_cache(&cache),
                    "prepare valid unchanged-rank probe cache");

    before_root = min_heap_peek(&cache.min_heap);

    passed &= check(stage12_apply_rank_change_without_heap_repair(
                        &cache,
                        2ULL,
                        PART2_RANK_UNCHANGED,
                        &old_rank,
                        &new_rank,
                        &resident),
                    "apply unchanged rank to key 2");

    after_root = min_heap_peek(&cache.min_heap);

    passed &= check(old_rank == 20LL &&
                    new_rank == 20LL &&
                    resident != NULL &&
                    resident->key == 2ULL,
                    "key 2 rank remains 20");

    passed &= check(before_root == after_root &&
                    min_heap_validate(&cache.min_heap) &&
                    part1_cache_validate(&cache),
                    "unchanged rank requires no heap repair");

    return passed;
}

int stage12_run_part2_contract_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 12: begin Part 2 rank-change contract ===\n");

    passed &= stage12_test_rank_direction_contract();
    passed &= stage12_test_decrease_can_stale_heap();
    passed &= stage12_test_increase_can_stale_heap();
    passed &= stage12_test_unchanged_rank_needs_no_repair();

    printf("\nStage 12 Part 2 findings:\n");
    printf("  getEntryRank may move rank lower, equal, or higher.\n");
    printf("  hash lookup still finds the resident in O(1) expected time.\n");
    printf("  an ordinary heap does not self-repair after arbitrary rank change.\n");
    printf("  decrease may require upward heap movement.\n");
    printf("  increase may require downward heap movement.\n");
    printf("  unchanged rank requires no heap movement.\n");
    printf("Stage 12 conclusion: arbitrary resident priority update must be solved next.\n");
    printf("Stage 12 Part 2 contract validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 13: recognize why the ordinary heap is insufficient ---------- */

/*
 * Stage 12 proved that an arbitrary resident rank change can invalidate the
 * ordinary heap. Stage 13 isolates the next problem:
 *
 *   the hash table can find CacheEntry * by key,
 *   but the ordinary heap has no O(1) way to answer:
 *
 *       "At which heap array index is this CacheEntry * stored?"
 *
 * The following instrumentation measures a diagnostic linear scan over the
 * heap. It is deliberately NOT a production priority-update implementation.
 */

typedef struct {
    uint64_t locate_calls;
    uint64_t pointer_comparisons;
    uint64_t locate_hits;
    uint64_t locate_misses;
} HeapLocateStats;

static HeapLocateStats g_heap_locate_stats;

void heap_locate_stats_reset(void)
{
    g_heap_locate_stats.locate_calls = 0U;
    g_heap_locate_stats.pointer_comparisons = 0U;
    g_heap_locate_stats.locate_hits = 0U;
    g_heap_locate_stats.locate_misses = 0U;
}

HeapLocateStats heap_locate_stats_snapshot(void)
{
    return g_heap_locate_stats;
}

/*
 * Diagnostic Stage 13 helper: find an arbitrary entry pointer in the ordinary
 * heap by scanning heap.items[] from index 0 upward.
 *
 * Complexity:
 *   best case:  O(1)
 *   worst case: O(N)
 *   miss:       O(N)
 *
 * No reverse index is maintained by MinHeap at this stage.
 */
int stage13_find_heap_index_linear(const MinHeap *heap,
                                   const CacheEntry *target,
                                   size_t *index_out)
{
    size_t i;

    ++g_heap_locate_stats.locate_calls;

    if (heap == NULL || target == NULL) {
        ++g_heap_locate_stats.locate_misses;
        return 0;
    }

    for (i = 0U; i < heap->size; ++i) {
        ++g_heap_locate_stats.pointer_comparisons;

        if (heap->items[i] == target) {
            if (index_out != NULL) {
                *index_out = i;
            }

            ++g_heap_locate_stats.locate_hits;
            return 1;
        }
    }

    ++g_heap_locate_stats.locate_misses;
    return 0;
}

/*
 * Build a deterministic valid heap whose array position is predictable.
 *
 * Entries have monotonically increasing rank and key:
 *   heap[0] rank 1
 *   heap[1] rank 2
 *   ...
 *
 * Because every parent index is smaller than each child index, the min-heap
 * invariant holds without calling push/sift functions. This is test scaffolding
 * only and avoids contaminating the location-cost experiment with heap build
 * operations.
 */
int stage13_prepare_positioned_heap(MinHeap *heap,
                                    CacheEntry entries[],
                                    size_t count)
{
    size_t i;

    if (heap == NULL ||
        entries == NULL ||
        count == 0U ||
        count > MAX_CACHE_CAPACITY) {
        return 0;
    }

    min_heap_init(heap);

    for (i = 0U; i < count; ++i) {
        entries[i].key = (CacheKey)(i + 1U);
        entries[i].value = (unsigned long long)(i + 1U) * 100ULL;
        entries[i].rank = (Rank)(i + 1U);
        entries[i].heap_index = i;

        heap->items[i] = &entries[i];
    }

    heap->size = count;
    return min_heap_validate(heap);
}

int stage13_check_location_cost(MinHeap *heap,
                                CacheEntry *target,
                                size_t expected_index,
                                uint64_t expected_comparisons,
                                int expected_found,
                                const char *label)
{
    HeapLocateStats stats;
    size_t actual_index = SIZE_MAX;
    int found;
    int passed;

    heap_locate_stats_reset();
    found = stage13_find_heap_index_linear(heap, target, &actual_index);
    stats = heap_locate_stats_snapshot();

    printf("[PROFILE] %-30s size=%zu comparisons=%llu result=%s",
           label,
           heap != NULL ? heap->size : 0U,
           (unsigned long long)stats.pointer_comparisons,
           found ? "FOUND" : "MISS");

    if (found) {
        printf(" index=%zu", actual_index);
    }

    printf("\n");

    passed = found == expected_found &&
             stats.locate_calls == 1U &&
             stats.pointer_comparisons == expected_comparisons;

    if (expected_found) {
        passed = passed &&
                 stats.locate_hits == 1U &&
                 stats.locate_misses == 0U &&
                 actual_index == expected_index;
    } else {
        passed = passed &&
                 stats.locate_hits == 0U &&
                 stats.locate_misses == 1U;
    }

    return check(passed, label);
}

int stage13_test_heap_position_profiles(void)
{
    MinHeap heap;
    CacheEntry entries[MAX_CACHE_CAPACITY];
    CacheEntry missing = {9999ULL, 999900ULL, 9999LL, HEAP_INDEX_NONE};
    int passed = 1;

    printf("\n[Stage 13] ordinary-heap arbitrary-entry location profiles\n");

    passed &= check(stage13_prepare_positioned_heap(
                        &heap, entries, MAX_CACHE_CAPACITY),
                    "prepare deterministic 100-entry ordinary heap");

    passed &= stage13_check_location_cost(
        &heap,
        &entries[0],
        0U,
        1U,
        1,
        "first heap entry");

    passed &= stage13_check_location_cost(
        &heap,
        &entries[49],
        49U,
        50U,
        1,
        "middle heap entry");

    passed &= stage13_check_location_cost(
        &heap,
        &entries[99],
        99U,
        100U,
        1,
        "last heap entry");

    passed &= stage13_check_location_cost(
        &heap,
        &missing,
        SIZE_MAX,
        100U,
        0,
        "missing heap entry");

    return passed;
}

int stage13_test_heap_location_scaling(void)
{
    size_t sizes[] = {1U, 10U, 25U, 50U, 100U};
    MinHeap heap;
    CacheEntry entries[MAX_CACHE_CAPACITY];
    CacheEntry missing = {8888ULL, 888800ULL, 8888LL, HEAP_INDEX_NONE};
    size_t i;
    int passed = 1;

    printf("\nStage 13 heap-location scaling (missing pointer):\n");
    printf("  size    pointer-comparisons\n");

    for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        HeapLocateStats stats;
        size_t n = sizes[i];
        size_t ignored_index = SIZE_MAX;
        int found;

        passed &= check(stage13_prepare_positioned_heap(
                            &heap, entries, n),
                        "prepare heap-location scaling case");

        heap_locate_stats_reset();
        found = stage13_find_heap_index_linear(
            &heap, &missing, &ignored_index);
        stats = heap_locate_stats_snapshot();

        printf("  %4zu    %llu\n",
               n,
               (unsigned long long)stats.pointer_comparisons);

        passed &= check(!found &&
                        stats.locate_calls == 1U &&
                        stats.locate_misses == 1U &&
                        stats.pointer_comparisons == (uint64_t)n,
                        "missing heap pointer comparisons equal heap size");
    }

    return passed;
}

int stage13_test_hash_to_heap_location_gap(void)
{
    Part1Cache cache;
    CacheEntry entry;
    CacheEntry *resident;
    HeapLocateStats stats;
    size_t heap_index = SIZE_MAX;
    CacheKey key;
    int found;
    int passed = 1;

    printf("\n[Stage 13] hash lookup versus ordinary-heap location gap\n");

    passed &= check(part1_cache_init(&cache, MAX_CACHE_CAPACITY),
                    "initialize integrated 100-entry Part 1 cache");

    /*
     * Increasing ranks preserve insertion order in the heap:
     * key 1 is at heap index 0 and key 100 is at heap index 99.
     */
    for (key = 1ULL; key <= (CacheKey)MAX_CACHE_CAPACITY; ++key) {
        entry.key = key;
        entry.value = key * 100ULL;
        entry.rank = (Rank)key;

        if (!part1_cache_insert(&cache, entry, NULL)) {
            passed &= check(0,
                            "fill integrated cache for heap-location test");
            return passed;
        }
    }

    passed &= check(part1_cache_validate(&cache),
                    "integrated 100-entry cache validates");

    /*
     * Stage 10 hash table gives the resident pointer directly.
     * Stage 13 then asks the ordinary heap where that pointer is located.
     */
    resident = part1_cache_lookup(&cache, 100ULL);

    passed &= check(resident != NULL &&
                    resident->key == 100ULL,
                    "hash table returns resident pointer for key 100");

    heap_locate_stats_reset();
    found = stage13_find_heap_index_linear(
        &cache.min_heap, resident, &heap_index);
    stats = heap_locate_stats_snapshot();

    printf("[PROFILE] hash found key 100 resident; ordinary heap location "
           "comparisons=%llu index=%zu\n",
           (unsigned long long)stats.pointer_comparisons,
           heap_index);

    passed &= check(found &&
                    heap_index == 99U &&
                    stats.locate_calls == 1U &&
                    stats.locate_hits == 1U &&
                    stats.pointer_comparisons == 100U,
                    "ordinary heap requires linear scan to locate hash-found resident");

    return passed;
}

int stage13_run_ordinary_heap_limitation_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 13: recognize why ordinary heap is insufficient ===\n");

    passed &= stage13_test_heap_position_profiles();
    passed &= stage13_test_heap_location_scaling();
    passed &= stage13_test_hash_to_heap_location_gap();

    printf("\nStage 13 findings:\n");
    printf("  hash table maps key -> CacheEntry * in expected O(1).\n");
    printf("  ordinary MinHeap stores CacheEntry * but no reverse heap position.\n");
    printf("  locating an arbitrary resident therefore requires scanning heap.items[].\n");
    printf("  first/middle/last lookup costs 1/50/100 comparisons in a 100-entry heap.\n");
    printf("  a missing resident requires N pointer comparisons for heap size N.\n");
    printf("  after hash lookup, worst-case heap location is still O(N).\n");
    printf("Stage 13 conclusion: Part 2 needs a direct resident -> heap-index mapping.\n");
    printf("Stage 13 ordinary-heap limitation validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 14: introduce and maintain heap_index ---------- */

/*
 * Stage 14 adds the direct reverse mapping identified as missing in Stage 13:
 *
 *     CacheEntry * -> entry->heap_index
 *
 * The field is metadata only at this checkpoint. Arbitrary rank repair is not
 * implemented yet.
 */

int stage14_check_all_heap_indices(const MinHeap *heap)
{
    size_t i;

    if (heap == NULL || heap->size > MAX_CACHE_CAPACITY) {
        return 0;
    }

    for (i = 0U; i < heap->size; ++i) {
        if (heap->items[i] == NULL ||
            heap->items[i]->heap_index != i) {
            return 0;
        }
    }

    return 1;
}

int stage14_test_push_and_swap_index_maintenance(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 50LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 80LL, HEAP_INDEX_NONE},
        {4ULL, 400ULL, 10LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 60LL, HEAP_INDEX_NONE}
    };
    size_t i;
    int passed = 1;

    printf("\n[Stage 14] heap_index maintenance across push/sift/swap\n");

    min_heap_init(&heap);

    for (i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(entries[i].heap_index == HEAP_INDEX_NONE,
                        "entry begins outside heap with invalid index");

        passed &= check(min_heap_push(&heap, &entries[i]),
                        "push entry while assigning heap_index");

        passed &= check(stage14_check_all_heap_indices(&heap) &&
                        min_heap_validate(&heap),
                        "all heap_index values match array positions after push");
    }

    passed &= check(min_heap_peek(&heap) == &entries[3] &&
                    entries[3].heap_index == 0U,
                    "minimum entry records heap index zero");

    return passed;
}

int stage14_test_pop_invalidates_and_repairs_indices(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 50LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 80LL, HEAP_INDEX_NONE},
        {4ULL, 400ULL, 10LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 60LL, HEAP_INDEX_NONE},
        {6ULL, 600ULL, 20LL, HEAP_INDEX_NONE}
    };
    size_t i;
    int passed = 1;

    printf("\n[Stage 14] pop invalidates removed index and repairs survivors\n");

    min_heap_init(&heap);
    for (i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare heap_index pop test");
    }

    while (heap.size > 0U) {
        CacheEntry *removed = min_heap_pop_min(&heap);

        passed &= check(removed != NULL &&
                        removed->heap_index == HEAP_INDEX_NONE,
                        "popped entry receives invalid heap_index");

        passed &= check(stage14_check_all_heap_indices(&heap) &&
                        min_heap_validate(&heap),
                        "survivor heap_index values remain synchronized after pop");
    }

    return passed;
}

int stage14_test_direct_index_lookup_after_hash_lookup(void)
{
    Part1Cache cache;
    CacheEntry *resident;
    size_t index;
    int passed = 1;

    printf("\n[Stage 14] direct resident-to-heap-index mapping\n");

    passed &= check(part1_cache_init(&cache, MAX_CACHE_CAPACITY),
                    "initialize Stage 14 integrated cache");

    for (CacheKey key = 1ULL;
         key <= (CacheKey)MAX_CACHE_CAPACITY;
         ++key) {
        CacheEntry entry;

        entry.key = key;
        entry.value = key * 100ULL;
        entry.rank = (Rank)key;
        entry.heap_index = HEAP_INDEX_NONE;

        if (!part1_cache_insert(&cache, entry, NULL)) {
            passed &= check(0, "fill Stage 14 integrated cache");
            return passed;
        }
    }

    passed &= check(part1_cache_validate(&cache),
                    "Stage 14 integrated cache validates");

    resident = part1_cache_lookup(&cache, 100ULL);
    passed &= check(resident != NULL && resident->key == 100ULL,
                    "hash lookup returns resident pointer for key 100");

    index = resident != NULL ? resident->heap_index : HEAP_INDEX_NONE;

    printf("[PROFILE] hash found key 100 resident; direct heap_index=%zu\n",
           index);

    passed &= check(index == 99U &&
                    index < cache.min_heap.size &&
                    cache.min_heap.items[index] == resident,
                    "heap_index directly identifies the resident heap slot");

    return passed;
}

int stage14_test_validator_detects_corrupted_heap_index(void)
{
    Part1Cache cache;
    CacheEntry *resident;
    size_t saved_index;
    int passed = 1;

    printf("\n[Stage 14] validator detects reverse-index corruption\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize reverse-index corruption test");

    passed &= check(part1_cache_get(&cache, 1ULL) != NULL &&
                    part1_cache_get(&cache, 2ULL) != NULL &&
                    part1_cache_get(&cache, 3ULL) != NULL &&
                    part1_cache_validate(&cache),
                    "prepare valid integrated cache with heap_index");

    resident = part1_cache_lookup(&cache, 2ULL);
    passed &= check(resident != NULL,
                    "locate resident for heap_index corruption test");

    if (resident == NULL) {
        return 0;
    }

    saved_index = resident->heap_index;
    resident->heap_index = HEAP_INDEX_NONE;

    passed &= check(!min_heap_validate(&cache.min_heap),
                    "heap validator rejects corrupted heap_index");
    passed &= check(!part1_cache_validate(&cache),
                    "integrated validator rejects corrupted heap_index");

    resident->heap_index = saved_index;

    passed &= check(min_heap_validate(&cache.min_heap) &&
                    part1_cache_validate(&cache),
                    "restoring heap_index restores valid integrated state");

    return passed;
}

int stage14_run_heap_index_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 14: introduce heap_index reverse position ===\n");

    passed &= stage14_test_push_and_swap_index_maintenance();
    passed &= stage14_test_pop_invalidates_and_repairs_indices();
    passed &= stage14_test_direct_index_lookup_after_hash_lookup();
    passed &= stage14_test_validator_detects_corrupted_heap_index();

    printf("\nStage 14 findings:\n");
    printf("  each heap-resident CacheEntry now stores its current heap_index.\n");
    printf("  min_heap_push assigns heap_index before sift-up.\n");
    printf("  min_heap_swap updates both moved entries.\n");
    printf("  min_heap_pop_min invalidates the removed entry index.\n");
    printf("  surviving entries keep indices synchronized after sift-down.\n");
    printf("  hash lookup can now reach heap position directly through resident->heap_index.\n");
    printf("Stage 14 boundary: heap_index is maintained but not yet used for priority repair.\n");
    printf("Stage 14 heap_index validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 15: harden min_heap_swap() carefully ---------- */

/*
 * Stage 14 introduced heap_index and made swaps maintain it.
 * Stage 15 isolates that operation and verifies it as a safe consistency
 * primitive before any later arbitrary-priority update starts depending on it.
 */

int stage15_test_non_adjacent_swap_updates_both_indices(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE},
        {4ULL, 400ULL, 40LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 50LL, HEAP_INDEX_NONE}
    };
    CacheEntry *left_before;
    CacheEntry *right_before;
    int passed = 1;

    printf("\n[Stage 15] non-adjacent swap keeps both reverse indices synchronized\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare Stage 15 swap heap");
    }

    passed &= check(min_heap_validate(&heap),
                    "pre-swap heap is valid");

    left_before = heap.items[1];
    right_before = heap.items[4];

    passed &= check(min_heap_swap(&heap, 1U, 4U),
                    "swap non-adjacent heap slots");

    passed &= check(heap.items[1] == right_before &&
                    right_before->heap_index == 1U &&
                    heap.items[4] == left_before &&
                    left_before->heap_index == 4U,
                    "swap updates both array slots and both heap_index fields");

    /*
     * The arbitrary test swap may violate rank ordering. Swap back before
     * asking the heap validator to check the complete heap invariant.
     */
    passed &= check(min_heap_swap(&heap, 1U, 4U),
                    "swap entries back to original positions");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "round-trip swap restores valid heap and reverse indices");

    return passed;
}

int stage15_test_parent_child_swap_indices(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE}
    };
    CacheEntry *root_before;
    CacheEntry *child_before;
    int passed = 1;

    printf("\n[Stage 15] parent-child swap updates exact positions\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare parent-child swap heap");
    }

    root_before = heap.items[0];
    child_before = heap.items[1];

    passed &= check(min_heap_swap(&heap, 0U, 1U),
                    "swap root and child");

    passed &= check(heap.items[0] == child_before &&
                    child_before->heap_index == 0U &&
                    heap.items[1] == root_before &&
                    root_before->heap_index == 1U,
                    "root-child swap synchronizes both reverse positions");

    passed &= check(min_heap_swap(&heap, 0U, 1U),
                    "restore parent-child ordering");

    passed &= check(min_heap_validate(&heap),
                    "restored parent-child heap validates");

    return passed;
}

int stage15_test_self_swap_is_safe_noop(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE}
    };
    CacheEntry *before;
    int passed = 1;

    printf("\n[Stage 15] self-swap is a safe no-op\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare self-swap heap");
    }

    before = heap.items[1];

    passed &= check(min_heap_swap(&heap, 1U, 1U),
                    "self-swap succeeds");

    passed &= check(heap.items[1] == before &&
                    before->heap_index == 1U &&
                    min_heap_validate(&heap),
                    "self-swap preserves pointer, index, and heap validity");

    return passed;
}

int stage15_test_invalid_swap_rejected_without_mutation(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE}
    };
    CacheEntry *snapshot[3];
    size_t index_snapshot[3];
    int passed = 1;

    printf("\n[Stage 15] invalid swap requests fail without mutation\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare invalid-swap heap");
    }

    for (size_t i = 0U; i < 3U; ++i) {
        snapshot[i] = heap.items[i];
        index_snapshot[i] = heap.items[i]->heap_index;
    }

    passed &= check(!min_heap_swap(NULL, 0U, 1U),
                    "NULL heap swap is rejected");
    passed &= check(!min_heap_swap(&heap, heap.size, 0U),
                    "out-of-range left index is rejected");
    passed &= check(!min_heap_swap(&heap, 0U, heap.size),
                    "out-of-range right index is rejected");

    for (size_t i = 0U; i < 3U; ++i) {
        passed &= check(heap.items[i] == snapshot[i] &&
                        heap.items[i]->heap_index == index_snapshot[i],
                        "invalid swap leaves heap array and metadata unchanged");
    }

    passed &= check(min_heap_validate(&heap),
                    "heap remains valid after rejected swaps");

    return passed;
}

int stage15_test_null_slot_swap_rejected(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE}
    };
    CacheEntry *saved;
    int passed = 1;

    printf("\n[Stage 15] malformed NULL-slot swap is rejected safely\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare NULL-slot swap heap");
    }

    saved = heap.items[2];
    heap.items[2] = NULL;

    passed &= check(!min_heap_swap(&heap, 0U, 2U),
                    "swap refuses NULL resident slot");

    passed &= check(heap.items[0] == &entries[0] &&
                    heap.items[2] == NULL &&
                    entries[0].heap_index == 0U &&
                    saved->heap_index == 2U,
                    "rejected NULL-slot swap performs no partial metadata update");

    heap.items[2] = saved;

    passed &= check(min_heap_validate(&heap),
                    "restoring malformed slot restores valid heap");

    return passed;
}

int stage15_run_heap_swap_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 15: modify min_heap_swap() carefully ===\n");

    passed &= stage15_test_non_adjacent_swap_updates_both_indices();
    passed &= stage15_test_parent_child_swap_indices();
    passed &= stage15_test_self_swap_is_safe_noop();
    passed &= stage15_test_invalid_swap_rejected_without_mutation();
    passed &= stage15_test_null_slot_swap_rejected();

    printf("\nStage 15 findings:\n");
    printf("  swap validates heap, bounds, and resident slots before mutation.\n");
    printf("  successful swaps update both heap array positions and both heap_index values.\n");
    printf("  self-swap is an explicit safe no-op.\n");
    printf("  invalid swaps fail without partially changing heap metadata.\n");
    printf("  sift-up/down continue to use the same consistency-preserving swap primitive.\n");
    printf("Stage 15 boundary: swap is hardened; arbitrary rank repair is still not implemented.\n");
    printf("Stage 15 heap-swap validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 16: write min_heap_update_rank() ---------- */

int stage16_prepare_update_heap(MinHeap *heap, CacheEntry entries[], size_t count)
{
    size_t i;

    if (heap == NULL || entries == NULL || count == 0U || count > 7U) {
        return 0;
    }

    min_heap_init(heap);

    for (i = 0U; i < count; ++i) {
        entries[i].key = (CacheKey)(i + 1U);
        entries[i].value = (unsigned long long)(i + 1U) * 100ULL;
        entries[i].rank = (Rank)((i + 1U) * 10U);
        entries[i].heap_index = HEAP_INDEX_NONE;

        if (!min_heap_push(heap, &entries[i])) {
            return 0;
        }
    }

    return min_heap_validate(heap);
}

int stage16_test_rank_decrease_sifts_up(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 16] lower rank repairs upward\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare rank-decrease heap");

    target = &entries[6]; /* key 7, rank 70; initially a leaf */

    passed &= check(target->heap_index == 6U,
                    "decrease target starts at leaf index 6");

    passed &= check(min_heap_update_rank(&heap, target, 5LL),
                    "update key 7 rank from 70 to 5");

    passed &= check(target->rank == 5LL &&
                    target->heap_index == 0U &&
                    min_heap_peek(&heap) == target,
                    "lower rank moves target to heap root");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "heap and reverse indices validate after sift-up repair");

    return passed;
}

int stage16_test_rank_increase_sifts_down(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 16] higher rank repairs downward\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare rank-increase heap");

    target = &entries[0]; /* key 1, rank 10; root */

    passed &= check(target->heap_index == 0U &&
                    min_heap_peek(&heap) == target,
                    "increase target starts at root");

    passed &= check(min_heap_update_rank(&heap, target, 100LL),
                    "update key 1 rank from 10 to 100");

    passed &= check(target->rank == 100LL &&
                    target->heap_index != 0U &&
                    min_heap_peek(&heap) != target &&
                    min_heap_peek(&heap)->rank == 20LL,
                    "higher rank moves former root downward");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "heap and reverse indices validate after sift-down repair");

    return passed;
}

int stage16_test_unchanged_rank_no_movement(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    size_t before_index;
    CacheEntry *before_root;
    int passed = 1;

    printf("\n[Stage 16] unchanged rank requires no movement\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare unchanged-rank heap");

    target = &entries[3];
    before_index = target->heap_index;
    before_root = min_heap_peek(&heap);

    passed &= check(min_heap_update_rank(&heap, target, target->rank),
                    "update resident with identical rank");

    passed &= check(target->heap_index == before_index &&
                    min_heap_peek(&heap) == before_root &&
                    min_heap_validate(&heap),
                    "unchanged rank preserves heap position and ordering");

    return passed;
}

int stage16_test_rank_update_tie_ordering(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {10ULL, 1000ULL, 10LL, HEAP_INDEX_NONE},
        {20ULL, 2000ULL, 20LL, HEAP_INDEX_NONE},
        {30ULL, 3000ULL, 30LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 40LL, HEAP_INDEX_NONE}
    };
    CacheEntry *target = &entries[3];
    int passed = 1;

    printf("\n[Stage 16] rank update preserves key tie ordering\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare tie-order update heap");
    }

    /*
     * Lower key 5 from rank 30 to rank 10. It now ties key 10 on rank and
     * should precede it because min_heap_entry_less() uses key second.
     */
    passed &= check(min_heap_update_rank(&heap, target, 10LL),
                    "decrease key 5 into equal-rank tie");

    passed &= check(min_heap_peek(&heap) == target &&
                    target->heap_index == 0U &&
                    target->rank == 10LL,
                    "equal rank uses lower key as deterministic minimum");

    passed &= check(min_heap_validate(&heap),
                    "heap validates after tie-aware rank update");

    return passed;
}

int stage16_test_invalid_update_rejected_without_mutation(void)
{
    MinHeap heap;
    CacheEntry entries[3];
    CacheEntry outsider = {999ULL, 99900ULL, 1LL, HEAP_INDEX_NONE};
    CacheEntry *target;
    Rank saved_rank;
    size_t saved_index;
    int passed = 1;

    printf("\n[Stage 16] invalid rank updates are non-destructive\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 3U),
                    "prepare invalid-update heap");

    target = &entries[1];
    saved_rank = target->rank;
    saved_index = target->heap_index;

    passed &= check(!min_heap_update_rank(NULL, target, 1LL),
                    "NULL heap update is rejected");
    passed &= check(!min_heap_update_rank(&heap, NULL, 1LL),
                    "NULL entry update is rejected");
    passed &= check(!min_heap_update_rank(&heap, &outsider, 0LL),
                    "non-resident entry update is rejected");

    target->heap_index = HEAP_INDEX_NONE;
    passed &= check(!min_heap_update_rank(&heap, target, 1LL),
                    "resident with invalid heap_index is rejected");
    passed &= check(target->rank == saved_rank,
                    "failed invalid-index update does not change rank");

    target->heap_index = saved_index;
    passed &= check(min_heap_validate(&heap),
                    "restored reverse index returns heap to valid state");

    target->heap_index = (saved_index + 1U) % heap.size;
    passed &= check(!min_heap_update_rank(&heap, target, 1LL),
                    "mismatched heap slot backlink is rejected");
    passed &= check(target->rank == saved_rank,
                    "failed backlink update does not change rank");

    target->heap_index = saved_index;
    passed &= check(min_heap_validate(&heap),
                    "heap remains valid after rejected rank updates");

    return passed;
}

int stage16_test_update_after_hash_lookup(void)
{
    Part1Cache cache;
    CacheEntry *resident;
    int passed = 1;

    printf("\n[Stage 16] hash lookup plus indexed rank update\n");

    passed &= check(part1_cache_init(&cache, 5U),
                    "initialize integrated rank-update cache");

    for (CacheKey key = 1ULL; key <= 5ULL; ++key) {
        passed &= check(part1_cache_get(&cache, key) != NULL,
                        "fill integrated cache for rank update");
    }

    resident = part1_cache_lookup(&cache, 5ULL);
    passed &= check(resident != NULL &&
                    resident->rank == 50LL,
                    "hash lookup finds key 5 resident");

    passed &= check(min_heap_update_rank(&cache.min_heap, resident, 5LL),
                    "indexed heap updates hash-found resident rank");

    passed &= check(resident->rank == 5LL &&
                    resident->heap_index == 0U &&
                    min_heap_peek(&cache.min_heap) == resident,
                    "hash-found resident reaches heap root without linear scan");

    passed &= check(part1_cache_validate(&cache),
                    "integrated cache validates after direct heap rank update");

    return passed;
}

int stage16_run_heap_update_rank_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 16: write min_heap_update_rank() ===\n");

    passed &= stage16_test_rank_decrease_sifts_up();
    passed &= stage16_test_rank_increase_sifts_down();
    passed &= stage16_test_unchanged_rank_no_movement();
    passed &= stage16_test_rank_update_tie_ordering();
    passed &= stage16_test_invalid_update_rejected_without_mutation();
    passed &= stage16_test_update_after_hash_lookup();

    printf("\nStage 16 findings:\n");
    printf("  heap_index locates the resident directly in O(1).\n");
    printf("  lower rank repairs with sift-up.\n");
    printf("  higher rank repairs with sift-down.\n");
    printf("  unchanged rank requires no heap movement.\n");
    printf("  hardened swaps keep heap_index synchronized during repair.\n");
    printf("  arbitrary resident rank repair is O(log N) worst case.\n");
    printf("Stage 16 boundary: heap_update_rank exists but production Part 2 cache_get is not wired yet.\n");
    printf("Stage 16 heap-update-rank validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 17: test rank decrease independently ---------- */

/*
 * Stage 16 implemented min_heap_update_rank(). Stage 17 makes no production
 * changes; it exercises only the new-rank < old-rank branch in greater depth.
 */

int stage17_test_decrease_without_movement(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    size_t original_index;
    int passed = 1;

    printf("\n[Stage 17] rank decrease that does not require movement\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare no-move decrease heap");

    target = &entries[6]; /* key 7, rank 70, index 6, parent rank 30 */
    original_index = target->heap_index;

    passed &= check(min_heap_update_rank(&heap, target, 65LL),
                    "decrease key 7 rank from 70 to 65");

    passed &= check(target->rank == 65LL &&
                    target->heap_index == original_index,
                    "smaller rank still above parent threshold stays in place");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "no-move decrease preserves heap and reverse indices");

    return passed;
}

int stage17_test_decrease_one_level(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 17] rank decrease moves exactly one level upward\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare one-level decrease heap");

    target = &entries[6]; /* index 6, parent index 2 rank 30 */

    passed &= check(target->heap_index == 6U,
                    "one-level target starts at index 6");

    passed &= check(min_heap_update_rank(&heap, target, 25LL),
                    "decrease key 7 rank from 70 to 25");

    passed &= check(target->rank == 25LL &&
                    target->heap_index == 2U &&
                    heap.items[2] == target,
                    "decrease crosses parent rank and moves to index 2");

    passed &= check(heap.items[0]->rank == 10LL &&
                    target->rank > heap.items[0]->rank,
                    "target stops below root when root still has lower rank");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "one-level decrease preserves all invariants");

    return passed;
}

int stage17_test_decrease_multiple_levels_to_root(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 17] rank decrease moves through multiple levels to root\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare multi-level decrease heap");

    target = &entries[6];

    passed &= check(min_heap_update_rank(&heap, target, 5LL),
                    "decrease key 7 rank from 70 to 5");

    passed &= check(target->heap_index == 0U &&
                    min_heap_peek(&heap) == target &&
                    target->rank == 5LL,
                    "multi-level decrease reaches heap root");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "multi-level decrease preserves heap/index consistency");

    return passed;
}

int stage17_test_decrease_equal_rank_key_tie(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {10ULL, 1000ULL, 10LL, HEAP_INDEX_NONE},
        {20ULL, 2000ULL, 20LL, HEAP_INDEX_NONE},
        {30ULL, 3000ULL, 30LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 40LL, HEAP_INDEX_NONE}
    };
    CacheEntry *target = &entries[3];
    int passed = 1;

    printf("\n[Stage 17] rank decrease respects equal-rank key tie ordering\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare tie-order decrease heap");
    }

    passed &= check(target->heap_index == 3U,
                    "tie-order target starts at index 3");

    passed &= check(min_heap_update_rank(&heap, target, 20LL),
                    "decrease key 5 rank from 40 to 20");

    passed &= check(target->rank == 20LL &&
                    target->heap_index == 1U &&
                    heap.items[1] == target &&
                    min_heap_peek(&heap)->key == 10ULL,
                    "equal rank uses lower key to cross parent, then stops below lower-rank root");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "tie-order decrease leaves heap valid");

    return passed;
}

int stage17_test_repeated_decreases_use_updated_heap_index(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 17] repeated decreases use the resident's updated heap_index\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare repeated-decrease heap");

    target = &entries[6];

    passed &= check(min_heap_update_rank(&heap, target, 25LL),
                    "first decrease moves key 7 from index 6 to index 2");
    passed &= check(target->heap_index == 2U,
                    "first decrease updates heap_index to 2");

    passed &= check(min_heap_update_rank(&heap, target, 5LL),
                    "second decrease uses new index and moves key 7 to root");
    passed &= check(target->heap_index == 0U &&
                    min_heap_peek(&heap) == target,
                    "second decrease reaches root through updated reverse index");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "repeated decreases preserve heap/index consistency");

    return passed;
}

int stage17_test_integrated_hash_found_rank_decrease(void)
{
    Part1Cache cache;
    CacheEntry entry;
    CacheEntry *resident;
    CacheKey key;
    int passed = 1;

    printf("\n[Stage 17] integrated hash-found resident rank decrease\n");

    passed &= check(part1_cache_init(&cache, 5U),
                    "initialize integrated decrease cache");

    for (key = 1ULL; key <= 5ULL; ++key) {
        entry.key = key;
        entry.value = key * 100ULL;
        entry.rank = (Rank)(key * 10ULL);
        entry.heap_index = HEAP_INDEX_NONE;

        passed &= check(part1_cache_insert(&cache, entry, NULL),
                        "fill integrated decrease cache");
    }

    resident = part1_cache_lookup(&cache, 5ULL);
    passed &= check(resident != NULL &&
                    resident->rank == 50LL,
                    "hash lookup finds key 5 before decrease");

    passed &= check(min_heap_update_rank(&cache.min_heap, resident, 5LL),
                    "decrease hash-found key 5 rank from 50 to 5");

    passed &= check(resident->heap_index == 0U &&
                    min_heap_peek(&cache.min_heap) == resident &&
                    resident->rank == 5LL,
                    "integrated decrease moves hash-found resident to root");

    passed &= check(part1_cache_lookup(&cache, 5ULL) == resident,
                    "hash index still points to same resident after decrease");

    passed &= check(part1_cache_validate(&cache),
                    "integrated cache validates after independent decrease repair");

    return passed;
}

int stage17_run_rank_decrease_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 17: test rank decrease independently ===\n");

    passed &= stage17_test_decrease_without_movement();
    passed &= stage17_test_decrease_one_level();
    passed &= stage17_test_decrease_multiple_levels_to_root();
    passed &= stage17_test_decrease_equal_rank_key_tie();
    passed &= stage17_test_repeated_decreases_use_updated_heap_index();
    passed &= stage17_test_integrated_hash_found_rank_decrease();

    printf("\nStage 17 findings:\n");
    printf("  a smaller rank does not always require movement.\n");
    printf("  when needed, decrease repair moves only upward.\n");
    printf("  one-level and multi-level sift-up paths both preserve heap_index.\n");
    printf("  equal-rank ordering still uses the deterministic key tie-break.\n");
    printf("  repeated decreases use the resident's newly maintained heap_index.\n");
    printf("  a hash-found resident can be decreased and repaired without a heap scan.\n");
    printf("Stage 17 boundary: only rank-decrease behavior is expanded here.\n");
    printf("Stage 17 rank-decrease validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 18: test rank increase independently ---------- */

/*
 * Stage 16 implemented min_heap_update_rank(). Stage 18 makes no production
 * changes; it exercises only the new-rank > old-rank branch in greater depth.
 */

int stage18_test_increase_without_movement(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    size_t original_index;
    int passed = 1;

    printf("\n[Stage 18] rank increase that does not require movement\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare no-move increase heap");

    target = &entries[1]; /* key 2, rank 20, index 1, children 40 and 50 */
    original_index = target->heap_index;

    passed &= check(original_index == 1U,
                    "no-move increase target starts at internal index 1");

    passed &= check(min_heap_update_rank(&heap, target, 35LL),
                    "increase key 2 rank from 20 to 35");

    passed &= check(target->rank == 35LL &&
                    target->heap_index == original_index,
                    "higher rank still below both children stays in place");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "no-move increase preserves heap and reverse indices");

    return passed;
}

int stage18_test_increase_one_level(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 18] rank increase moves exactly one level downward\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare one-level increase heap");

    target = &entries[0]; /* root rank 10; children ranks 20 and 30 */

    passed &= check(target->heap_index == 0U,
                    "one-level increase target starts at root");

    passed &= check(min_heap_update_rank(&heap, target, 25LL),
                    "increase key 1 rank from 10 to 25");

    passed &= check(target->rank == 25LL &&
                    target->heap_index == 1U &&
                    heap.items[1] == target,
                    "increase crosses smaller child and moves to index 1");

    passed &= check(heap.items[0]->rank == 20LL &&
                    target->rank < heap.items[3]->rank &&
                    target->rank < heap.items[4]->rank,
                    "target stops after one level when still below its new children");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "one-level increase preserves all invariants");

    return passed;
}

int stage18_test_increase_multiple_levels(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 18] rank increase moves through multiple levels downward\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare multi-level increase heap");

    target = &entries[0];

    passed &= check(min_heap_update_rank(&heap, target, 100LL),
                    "increase key 1 rank from 10 to 100");

    passed &= check(target->rank == 100LL &&
                    target->heap_index == 3U &&
                    heap.items[3] == target &&
                    min_heap_peek(&heap)->rank == 20LL,
                    "multi-level increase descends from root to leaf index 3");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "multi-level increase preserves heap/index consistency");

    return passed;
}

int stage18_test_increase_equal_rank_key_tie(void)
{
    MinHeap heap;
    CacheEntry entries[] = {
        {30ULL, 3000ULL, 10LL, HEAP_INDEX_NONE},
        {20ULL, 2000ULL, 20LL, HEAP_INDEX_NONE},
        {10ULL, 1000ULL, 20LL, HEAP_INDEX_NONE}
    };
    CacheEntry *target = &entries[0];
    int passed = 1;

    printf("\n[Stage 18] rank increase respects equal-rank child tie ordering\n");

    min_heap_init(&heap);
    for (size_t i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        passed &= check(min_heap_push(&heap, &entries[i]),
                        "prepare tie-order increase heap");
    }

    passed &= check(target->heap_index == 0U,
                    "tie-order increase target starts at root");

    passed &= check(min_heap_update_rank(&heap, target, 20LL),
                    "increase key 30 rank from 10 to 20");

    /*
     * Both children now tie target on rank 20. Among the two children,
     * key 10 is smaller than key 20 and also smaller than target key 30,
     * so sift-down must choose key 10 as the new root.
     */
    passed &= check(min_heap_peek(&heap)->key == 10ULL &&
                    min_heap_peek(&heap)->rank == 20LL &&
                    target->heap_index == 2U &&
                    heap.items[2] == target,
                    "equal-rank children use lower key when choosing downward swap");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "tie-order increase leaves heap valid");

    return passed;
}

int stage18_test_repeated_increases_use_updated_heap_index(void)
{
    MinHeap heap;
    CacheEntry entries[7];
    CacheEntry *target;
    int passed = 1;

    printf("\n[Stage 18] repeated increases use the resident's updated heap_index\n");

    passed &= check(stage16_prepare_update_heap(&heap, entries, 7U),
                    "prepare repeated-increase heap");

    target = &entries[0];

    passed &= check(min_heap_update_rank(&heap, target, 25LL),
                    "first increase moves key 1 from index 0 to index 1");
    passed &= check(target->heap_index == 1U,
                    "first increase updates heap_index to 1");

    passed &= check(min_heap_update_rank(&heap, target, 100LL),
                    "second increase uses new index and moves key 1 to leaf");
    passed &= check(target->heap_index == 3U &&
                    heap.items[3] == target,
                    "second increase descends through updated reverse index");

    passed &= check(min_heap_validate(&heap) &&
                    stage14_check_all_heap_indices(&heap),
                    "repeated increases preserve heap/index consistency");

    return passed;
}

int stage18_test_integrated_hash_found_rank_increase(void)
{
    Part1Cache cache;
    CacheEntry entry;
    CacheEntry *resident;
    CacheKey key;
    int passed = 1;

    printf("\n[Stage 18] integrated hash-found resident rank increase\n");

    passed &= check(part1_cache_init(&cache, 5U),
                    "initialize integrated increase cache");

    for (key = 1ULL; key <= 5ULL; ++key) {
        entry.key = key;
        entry.value = key * 100ULL;
        entry.rank = (Rank)(key * 10ULL);
        entry.heap_index = HEAP_INDEX_NONE;

        passed &= check(part1_cache_insert(&cache, entry, NULL),
                        "fill integrated increase cache");
    }

    resident = part1_cache_lookup(&cache, 1ULL);
    passed &= check(resident != NULL &&
                    resident->rank == 10LL &&
                    resident->heap_index == 0U,
                    "hash lookup finds root key 1 before increase");

    passed &= check(min_heap_update_rank(&cache.min_heap, resident, 100LL),
                    "increase hash-found key 1 rank from 10 to 100");

    passed &= check(resident->rank == 100LL &&
                    resident->heap_index == 3U &&
                    min_heap_peek(&cache.min_heap) != resident &&
                    min_heap_peek(&cache.min_heap)->key == 2ULL,
                    "integrated increase moves hash-found former root downward");

    passed &= check(part1_cache_lookup(&cache, 1ULL) == resident,
                    "hash index still points to same resident after increase");

    passed &= check(part1_cache_validate(&cache),
                    "integrated cache validates after independent increase repair");

    return passed;
}

int stage18_run_rank_increase_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 18: test rank increase independently ===\n");

    passed &= stage18_test_increase_without_movement();
    passed &= stage18_test_increase_one_level();
    passed &= stage18_test_increase_multiple_levels();
    passed &= stage18_test_increase_equal_rank_key_tie();
    passed &= stage18_test_repeated_increases_use_updated_heap_index();
    passed &= stage18_test_integrated_hash_found_rank_increase();

    printf("\nStage 18 findings:\n");
    printf("  a higher rank does not always require movement.\n");
    printf("  when needed, increase repair moves only downward.\n");
    printf("  one-level and multi-level sift-down paths both preserve heap_index.\n");
    printf("  equal-rank child selection still uses the deterministic key tie-break.\n");
    printf("  repeated increases use the resident's newly maintained heap_index.\n");
    printf("  a hash-found resident can be increased and repaired without a heap scan.\n");
    printf("Stage 18 boundary: only rank-increase behavior is expanded here.\n");
    printf("Stage 18 rank-increase validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 19: integrate dynamic rank into cache hits ---------- */

/*
 * Stage 19 introduces the Part 2 cache-hit path without changing the existing
 * fixed-rank part1_cache_get() API.
 *
 * On a hit:
 *   1. hash lookup returns the existing resident,
 *   2. the rank provider computes the post-hit rank,
 *   3. min_heap_update_rank() repairs the indexed heap,
 *   4. the same resident pointer is returned.
 *
 * On a miss:
 *   Stage 19 intentionally preserves the Stage 10 Part 1 miss path by
 *   delegating to part1_cache_get(). No dynamic rank callback is invoked.
 */

typedef Rank (*Part2RankProvider)(const CacheEntry *entry, void *context);

CacheEntry *part2_cache_get(Part1Cache *cache,
                            CacheKey key,
                            Part2RankProvider rank_provider,
                            void *rank_context)
{
    CacheEntry *resident;
    Rank new_rank;

    if (cache == NULL || rank_provider == NULL) {
        return NULL;
    }

    resident = part1_cache_lookup(cache, key);

    if (resident == NULL) {
        /*
         * Stage 19 changes hits only. Miss fetch/eviction/insertion semantics
         * remain exactly those of the validated Part 1 implementation.
         * part1_cache_get() owns the miss/access statistics for this path.
         */
        return part1_cache_get(cache, key);
    }

    ++cache->stats.accesses;
    ++cache->stats.hits;
    ++cache->stats.rank_provider_calls;

    {
        Rank old_rank = resident->rank;

        new_rank = rank_provider(resident, rank_context);

        if (!min_heap_update_rank(&cache->min_heap, resident, new_rank)) {
            return NULL;
        }

        ++cache->stats.rank_updates;
        if (new_rank < old_rank) {
            ++cache->stats.rank_decreases;
        } else if (new_rank > old_rank) {
            ++cache->stats.rank_increases;
        } else {
            ++cache->stats.rank_unchanged;
        }
    }

    return resident;
}

typedef struct {
    Part2RankScenario scenario;
    uint64_t calls;
} Stage19RankContext;

Rank stage19_rank_provider(const CacheEntry *entry, void *context)
{
    Stage19RankContext *rank_context = (Stage19RankContext *)context;

    if (rank_context == NULL) {
        return entry != NULL ? entry->rank : 0LL;
    }

    ++rank_context->calls;
    return stage12_get_entry_rank(entry, rank_context->scenario);
}

int stage19_prepare_dynamic_hit_cache(Part1Cache *cache)
{
    CacheEntry entries[] = {
        {1ULL, 100ULL, 10LL, HEAP_INDEX_NONE},
        {2ULL, 200ULL, 20LL, HEAP_INDEX_NONE},
        {3ULL, 300ULL, 30LL, HEAP_INDEX_NONE},
        {4ULL, 400ULL, 40LL, HEAP_INDEX_NONE},
        {5ULL, 500ULL, 50LL, HEAP_INDEX_NONE}
    };
    size_t i;

    if (!part1_cache_init(cache, 5U)) {
        return 0;
    }

    for (i = 0U; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (!part1_cache_insert(cache, entries[i], NULL)) {
            return 0;
        }
    }

    return part1_cache_validate(cache);
}

int stage19_test_hit_rank_decrease(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_DECREASE, 0U};
    CacheEntry *before;
    CacheEntry *after;
    int passed = 1;

    printf("\n[Stage 19] cache hit integrates a lower dynamic rank\n");

    passed &= check(stage19_prepare_dynamic_hit_cache(&cache),
                    "prepare dynamic-hit cache for decrease");

    before = part1_cache_lookup(&cache, 3ULL);

    passed &= check(before != NULL &&
                    before->rank == 30LL &&
                    before->heap_index == 2U,
                    "key 3 starts at rank 30 and heap index 2");

    after = part2_cache_get(&cache, 3ULL, stage19_rank_provider, &context);

    passed &= check(after == before &&
                    context.calls == 1U,
                    "dynamic hit returns same resident and invokes provider once");

    passed &= check(after != NULL &&
                    after->rank == 0LL &&
                    after->heap_index == 0U &&
                    min_heap_peek(&cache.min_heap) == after,
                    "lower dynamic rank repairs resident upward to heap root");

    passed &= check(part1_cache_validate(&cache),
                    "integrated cache validates after dynamic rank decrease hit");

    return passed;
}

int stage19_test_hit_rank_unchanged(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_UNCHANGED, 0U};
    CacheEntry *before;
    CacheEntry *after;
    size_t original_index;
    CacheEntry *original_root;
    int passed = 1;

    printf("\n[Stage 19] cache hit integrates an unchanged dynamic rank\n");

    passed &= check(stage19_prepare_dynamic_hit_cache(&cache),
                    "prepare dynamic-hit cache for unchanged rank");

    before = part1_cache_lookup(&cache, 3ULL);
    original_index = before != NULL ? before->heap_index : HEAP_INDEX_NONE;
    original_root = min_heap_peek(&cache.min_heap);

    after = part2_cache_get(&cache, 3ULL, stage19_rank_provider, &context);

    passed &= check(after == before &&
                    context.calls == 1U &&
                    after != NULL &&
                    after->rank == 30LL,
                    "unchanged-rank hit returns same resident and invokes provider once");

    passed &= check(after->heap_index == original_index &&
                    min_heap_peek(&cache.min_heap) == original_root &&
                    part1_cache_validate(&cache),
                    "unchanged dynamic rank leaves heap position and root unchanged");

    return passed;
}

int stage19_test_hit_rank_increase(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_INCREASE, 0U};
    CacheEntry *before;
    CacheEntry *after;
    int passed = 1;

    printf("\n[Stage 19] cache hit integrates a higher dynamic rank\n");

    passed &= check(stage19_prepare_dynamic_hit_cache(&cache),
                    "prepare dynamic-hit cache for increase");

    before = part1_cache_lookup(&cache, 1ULL);

    passed &= check(before != NULL &&
                    before->rank == 10LL &&
                    before->heap_index == 0U,
                    "key 1 starts as heap root at rank 10");

    after = part2_cache_get(&cache, 1ULL, stage19_rank_provider, &context);

    passed &= check(after == before &&
                    context.calls == 1U,
                    "higher-rank hit returns same resident and invokes provider once");

    passed &= check(after != NULL &&
                    after->rank == 40LL &&
                    after->heap_index != 0U &&
                    min_heap_peek(&cache.min_heap) != after &&
                    min_heap_peek(&cache.min_heap)->key == 2ULL,
                    "higher dynamic rank repairs former root downward");

    passed &= check(part1_cache_validate(&cache),
                    "integrated cache validates after dynamic rank increase hit");

    return passed;
}

int stage19_test_repeated_dynamic_hits(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_DECREASE, 0U};
    CacheEntry *resident;
    int passed = 1;

    printf("\n[Stage 19] repeated dynamic hits reuse updated heap_index\n");

    passed &= check(stage19_prepare_dynamic_hit_cache(&cache),
                    "prepare cache for repeated dynamic hits");

    resident = part1_cache_lookup(&cache, 5ULL);

    passed &= check(resident != NULL &&
                    resident->rank == 50LL,
                    "key 5 starts at rank 50");

    resident = part2_cache_get(&cache, 5ULL, stage19_rank_provider, &context);
    passed &= check(resident != NULL &&
                    resident->rank == 20LL &&
                    context.calls == 1U &&
                    part1_cache_validate(&cache),
                    "first dynamic hit decreases rank and preserves cache validity");

    resident = part2_cache_get(&cache, 5ULL, stage19_rank_provider, &context);
    passed &= check(resident != NULL &&
                    resident->rank == -10LL &&
                    resident->heap_index == 0U &&
                    context.calls == 2U &&
                    min_heap_peek(&cache.min_heap) == resident &&
                    part1_cache_validate(&cache),
                    "second dynamic hit uses updated heap_index and reaches root");

    return passed;
}

int stage19_test_miss_preserves_part1_semantics(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_DECREASE, 0U};
    CacheEntry *resident;
    int passed = 1;

    printf("\n[Stage 19] cache miss preserves existing Part 1 semantics\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize Stage 19 miss-path cache");

    resident = part2_cache_get(&cache, 7ULL, stage19_rank_provider, &context);

    passed &= check(resident != NULL &&
                    resident->key == 7ULL &&
                    resident->value == 700ULL &&
                    resident->rank == 70LL,
                    "Part 2 miss inserts deterministic DB entry unchanged");

    passed &= check(context.calls == 0U,
                    "rank provider is not invoked on cache miss");

    passed &= check(cache.size == 1U &&
                    cache.index.size == 1U &&
                    cache.min_heap.size == 1U &&
                    part1_cache_validate(&cache),
                    "miss path preserves integrated Part 1 structure");

    return passed;
}

int stage19_test_dynamic_hit_changes_next_eviction_victim(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_INCREASE, 0U};
    CacheEntry *resident;
    CacheEntry evicted;
    int passed = 1;

    printf("\n[Stage 19] repaired dynamic hit is visible to later eviction\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize eviction-effect cache");
    passed &= check(part1_cache_get(&cache, 1ULL) != NULL &&
                    part1_cache_get(&cache, 2ULL) != NULL &&
                    part1_cache_get(&cache, 3ULL) != NULL,
                    "fill eviction-effect cache with ranks 10, 20, 30");

    resident = part2_cache_get(&cache, 1ULL, stage19_rank_provider, &context);

    passed &= check(resident != NULL &&
                    resident->rank == 40LL &&
                    min_heap_peek(&cache.min_heap) != NULL &&
                    min_heap_peek(&cache.min_heap)->key == 2ULL,
                    "increased hit rank changes the current heap minimum");

    passed &= check(part1_cache_evict_min(&cache, &evicted) &&
                    evicted.key == 2ULL &&
                    evicted.rank == 20LL,
                    "subsequent eviction uses repaired dynamic rank ordering");

    passed &= check(part1_cache_lookup(&cache, 1ULL) == resident &&
                    part1_cache_lookup(&cache, 2ULL) == NULL &&
                    part1_cache_validate(&cache),
                    "dynamic-hit resident survives and integrated cache remains valid");

    return passed;
}

int stage19_test_invalid_dynamic_hit_arguments(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_UNCHANGED, 0U};
    CacheEntry *resident;
    int passed = 1;

    printf("\n[Stage 19] invalid Part 2 hit API arguments are rejected\n");

    passed &= check(part1_cache_init(&cache, 2U),
                    "initialize invalid-argument cache");
    passed &= check(part1_cache_get(&cache, 1ULL) != NULL,
                    "insert resident for invalid-argument checks");

    resident = part1_cache_lookup(&cache, 1ULL);

    passed &= check(part2_cache_get(NULL,
                                    1ULL,
                                    stage19_rank_provider,
                                    &context) == NULL,
                    "NULL cache is rejected");

    passed &= check(part2_cache_get(&cache,
                                    1ULL,
                                    NULL,
                                    &context) == NULL,
                    "NULL rank provider is rejected");

    passed &= check(resident != NULL &&
                    resident->rank == 10LL &&
                    context.calls == 0U &&
                    part1_cache_validate(&cache),
                    "rejected Part 2 accesses do not mutate resident state");

    return passed;
}

int stage19_run_dynamic_hit_integration_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 19: integrate dynamic rank into cache hits ===\n");

    passed &= stage19_test_hit_rank_decrease();
    passed &= stage19_test_hit_rank_unchanged();
    passed &= stage19_test_hit_rank_increase();
    passed &= stage19_test_repeated_dynamic_hits();
    passed &= stage19_test_miss_preserves_part1_semantics();
    passed &= stage19_test_dynamic_hit_changes_next_eviction_victim();
    passed &= stage19_test_invalid_dynamic_hit_arguments();

    printf("\nStage 19 findings:\n");
    printf("  Part 2 cache hits invoke the rank provider exactly once.\n");
    printf("  lower/same/higher hit ranks reuse min_heap_update_rank().\n");
    printf("  the same resident pointer is returned after dynamic hit repair.\n");
    printf("  repeated hits reuse the resident's maintained heap_index.\n");
    printf("  misses preserve the existing Part 1 fetch/insert behavior.\n");
    printf("  repaired hit ranks immediately affect later eviction ordering.\n");
    printf("Stage 19 boundary: dynamic rank is integrated on hits only.\n");
    printf("Stage 19 dynamic-hit integration validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 20: add integrated cache statistics ---------- */

int stage20_stats_are_zero(const CacheStats *stats)
{
    return stats != NULL &&
           stats->accesses == 0U &&
           stats->hits == 0U &&
           stats->misses == 0U &&
           stats->db_reads == 0U &&
           stats->insertions == 0U &&
           stats->evictions == 0U &&
           stats->rank_provider_calls == 0U &&
           stats->rank_updates == 0U &&
           stats->rank_decreases == 0U &&
           stats->rank_unchanged == 0U &&
           stats->rank_increases == 0U;
}

int stage20_test_initial_and_reset_statistics(void)
{
    Part1Cache cache;
    CacheStats stats;
    int passed = 1;

    printf("\n[Stage 20] statistics initialize and reset deterministically\n");

    passed &= check(part1_cache_init(&cache, 3U),
                    "initialize Stage 20 statistics cache");

    stats = part1_cache_stats_snapshot(&cache);
    passed &= check(stage20_stats_are_zero(&stats),
                    "fresh cache statistics are all zero");

    passed &= check(part1_cache_get(&cache, 1ULL) != NULL,
                    "perform one miss before reset");
    passed &= check(part1_cache_get(&cache, 1ULL) != NULL,
                    "perform one hit before reset");

    stats = part1_cache_stats_snapshot(&cache);
    passed &= check(stats.accesses == 2U &&
                    stats.hits == 1U &&
                    stats.misses == 1U &&
                    stats.db_reads == 1U &&
                    stats.insertions == 1U,
                    "statistics record pre-reset activity");

    part1_cache_stats_reset(&cache);
    stats = part1_cache_stats_snapshot(&cache);

    passed &= check(stage20_stats_are_zero(&stats),
                    "statistics reset clears every counter");
    passed &= check(cache.size == 1U &&
                    part1_cache_lookup(&cache, 1ULL) != NULL &&
                    part1_cache_validate(&cache),
                    "statistics reset does not change cache contents");

    return passed;
}

int stage20_test_part1_access_statistics(void)
{
    Part1Cache cache;
    CacheStats stats;
    int passed = 1;

    printf("\n[Stage 20] Part 1 hit/miss/DB/insert/eviction statistics\n");

    passed &= check(part1_cache_init(&cache, 2U),
                    "initialize capacity-two statistics cache");

    passed &= check(part1_cache_get(&cache, 1ULL) != NULL,
                    "GET 1 records first miss");
    passed &= check(part1_cache_get(&cache, 1ULL) != NULL,
                    "GET 1 records hit");
    passed &= check(part1_cache_get(&cache, 2ULL) != NULL,
                    "GET 2 records second miss");
    passed &= check(part1_cache_get(&cache, 3ULL) != NULL,
                    "GET 3 records miss, eviction, and insertion");

    stats = part1_cache_stats_snapshot(&cache);

    passed &= check(stats.accesses == 4U &&
                    stats.hits == 1U &&
                    stats.misses == 3U &&
                    stats.db_reads == 3U &&
                    stats.insertions == 3U &&
                    stats.evictions == 1U,
                    "Part 1 operation counters match exact access sequence");

    passed &= check(stats.rank_provider_calls == 0U &&
                    stats.rank_updates == 0U &&
                    stats.rank_decreases == 0U &&
                    stats.rank_unchanged == 0U &&
                    stats.rank_increases == 0U,
                    "Part 1 accesses do not increment dynamic-rank counters");

    passed &= check(part1_cache_lookup(&cache, 1ULL) == NULL &&
                    part1_cache_lookup(&cache, 2ULL) != NULL &&
                    part1_cache_lookup(&cache, 3ULL) != NULL &&
                    part1_cache_validate(&cache),
                    "Part 1 statistics do not alter eviction semantics");

    return passed;
}

int stage20_test_dynamic_rank_statistics(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_DECREASE, 0U};
    CacheStats stats;
    CacheEntry *resident;
    int passed = 1;

    printf("\n[Stage 20] Part 2 dynamic-rank direction statistics\n");

    passed &= check(stage19_prepare_dynamic_hit_cache(&cache),
                    "prepare dynamic statistics cache");
    part1_cache_stats_reset(&cache);

    resident = part2_cache_get(&cache, 3ULL, stage19_rank_provider, &context);
    passed &= check(resident != NULL && resident->rank == 0LL,
                    "dynamic decrease hit succeeds");

    context.scenario = PART2_RANK_UNCHANGED;
    resident = part2_cache_get(&cache, 3ULL, stage19_rank_provider, &context);
    passed &= check(resident != NULL && resident->rank == 0LL,
                    "dynamic unchanged hit succeeds");

    context.scenario = PART2_RANK_INCREASE;
    resident = part2_cache_get(&cache, 3ULL, stage19_rank_provider, &context);
    passed &= check(resident != NULL && resident->rank == 30LL,
                    "dynamic increase hit succeeds");

    stats = part1_cache_stats_snapshot(&cache);

    passed &= check(stats.accesses == 3U &&
                    stats.hits == 3U &&
                    stats.misses == 0U &&
                    stats.db_reads == 0U &&
                    stats.insertions == 0U &&
                    stats.evictions == 0U,
                    "dynamic-hit accesses do not contaminate miss counters");

    passed &= check(stats.rank_provider_calls == 3U &&
                    stats.rank_updates == 3U &&
                    stats.rank_decreases == 1U &&
                    stats.rank_unchanged == 1U &&
                    stats.rank_increases == 1U &&
                    context.calls == 3U,
                    "dynamic-rank counters classify decrease/equal/increase exactly");

    passed &= check(part1_cache_validate(&cache),
                    "dynamic statistics preserve integrated cache validity");

    return passed;
}

int stage20_test_part2_miss_statistics(void)
{
    Part1Cache cache;
    Stage19RankContext context = {PART2_RANK_DECREASE, 0U};
    CacheStats stats;
    CacheEntry *resident;
    int passed = 1;

    printf("\n[Stage 20] Part 2 miss is counted once by the existing miss path\n");

    passed &= check(part1_cache_init(&cache, 2U),
                    "initialize Part 2 miss statistics cache");
    passed &= check(part1_cache_get(&cache, 1ULL) != NULL &&
                    part1_cache_get(&cache, 2ULL) != NULL,
                    "fill Part 2 miss statistics cache");

    part1_cache_stats_reset(&cache);

    resident = part2_cache_get(&cache, 3ULL, stage19_rank_provider, &context);
    passed &= check(resident != NULL && resident->key == 3ULL,
                    "Part 2 miss inserts requested key");

    stats = part1_cache_stats_snapshot(&cache);

    passed &= check(stats.accesses == 1U &&
                    stats.hits == 0U &&
                    stats.misses == 1U &&
                    stats.db_reads == 1U &&
                    stats.insertions == 1U &&
                    stats.evictions == 1U,
                    "Part 2 miss is counted exactly once");

    passed &= check(stats.rank_provider_calls == 0U &&
                    stats.rank_updates == 0U &&
                    context.calls == 0U,
                    "Part 2 miss does not invoke or count rank provider");

    passed &= check(part1_cache_validate(&cache),
                    "Part 2 miss statistics preserve integrated validity");

    return passed;
}

int stage20_test_statistics_snapshot_is_non_mutating(void)
{
    Part1Cache cache;
    CacheStats first;
    CacheStats second;
    int passed = 1;

    printf("\n[Stage 20] statistics snapshot is read-only\n");

    passed &= check(part1_cache_init(&cache, 2U),
                    "initialize snapshot statistics cache");
    passed &= check(part1_cache_get(&cache, 1ULL) != NULL,
                    "create statistics before snapshot");

    first = part1_cache_stats_snapshot(&cache);
    second = part1_cache_stats_snapshot(&cache);

    passed &= check(first.accesses == second.accesses &&
                    first.hits == second.hits &&
                    first.misses == second.misses &&
                    first.db_reads == second.db_reads &&
                    first.insertions == second.insertions &&
                    first.evictions == second.evictions &&
                    first.rank_provider_calls == second.rank_provider_calls &&
                    first.rank_updates == second.rank_updates &&
                    first.rank_decreases == second.rank_decreases &&
                    first.rank_unchanged == second.rank_unchanged &&
                    first.rank_increases == second.rank_increases,
                    "taking a statistics snapshot does not change counters");

    return passed;
}

int stage20_run_statistics_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 20: add integrated cache statistics ===\n");

    passed &= stage20_test_initial_and_reset_statistics();
    passed &= stage20_test_part1_access_statistics();
    passed &= stage20_test_dynamic_rank_statistics();
    passed &= stage20_test_part2_miss_statistics();
    passed &= stage20_test_statistics_snapshot_is_non_mutating();

    printf("\nStage 20 findings:\n");
    printf("  integrated cache accesses now expose deterministic runtime counters.\n");
    printf("  hits/misses/DB reads/insertions/evictions are counted at operation boundaries.\n");
    printf("  Part 2 hits classify rank decrease/equal/increase independently.\n");
    printf("  Part 2 misses are counted once and do not invoke the rank provider.\n");
    printf("  reset clears counters without changing cache contents.\n");
    printf("  snapshot reads statistics without mutating them.\n");
    printf("Stage 20 boundary: statistics add observability only; cache policy is unchanged.\n");
    printf("Stage 20 statistics validation: %s\n", passed ? "PASS" : "FAIL");

    return passed;
}



/* ---------- Stage 21: add deterministic workload generation ---------- */

/*
 * Stage 21 adds a reproducible workload stream for future correctness and
 * performance experiments. It deliberately does not add timing or benchmarking.
 *
 * The generator uses a fixed 64-bit linear congruential generator (LCG) with
 * unsigned wraparound semantics defined by C. Given the same seed, key-space,
 * and operation count, it produces the same sequence on every conforming
 * implementation with uint64_t.
 */

typedef struct {
    uint64_t initial_seed;
    uint64_t state;
    CacheKey key_space;
} WorkloadGenerator;

typedef struct {
    CacheKey key;
    Part2RankScenario scenario;
} WorkloadOp;

int workload_generator_init(WorkloadGenerator *generator,
                            uint64_t seed,
                            CacheKey key_space)
{
    if (generator == NULL || key_space == 0ULL) {
        return 0;
    }

    generator->initial_seed = seed;
    generator->state = seed;
    generator->key_space = key_space;
    return 1;
}

void workload_generator_reset(WorkloadGenerator *generator)
{
    if (generator == NULL) {
        return;
    }

    generator->state = generator->initial_seed;
}

uint64_t workload_generator_next_u64(WorkloadGenerator *generator)
{
    if (generator == NULL) {
        return 0U;
    }

    /*
     * PCG-family LCG constants. Stage 21 uses only the deterministic LCG
     * state transition; no platform-dependent rand()/random() API is used.
     */
    generator->state =
        generator->state * UINT64_C(6364136223846793005) +
        UINT64_C(1442695040888963407);

    return generator->state;
}

int workload_generator_next(WorkloadGenerator *generator,
                            WorkloadOp *operation)
{
    uint64_t key_bits;
    uint64_t scenario_bits;

    if (generator == NULL ||
        operation == NULL ||
        generator->key_space == 0ULL) {
        return 0;
    }

    key_bits = workload_generator_next_u64(generator);
    scenario_bits = workload_generator_next_u64(generator);

    operation->key =
        (CacheKey)(key_bits % (uint64_t)generator->key_space) + 1ULL;
    operation->scenario =
        (Part2RankScenario)(scenario_bits % 3U);

    return 1;
}

int workload_generate(WorkloadGenerator *generator,
                      WorkloadOp operations[],
                      size_t count)
{
    size_t i;

    if (generator == NULL ||
        (operations == NULL && count != 0U)) {
        return 0;
    }

    for (i = 0U; i < count; ++i) {
        if (!workload_generator_next(generator, &operations[i])) {
            return 0;
        }
    }

    return 1;
}

int workload_operation_equal(const WorkloadOp *a,
                             const WorkloadOp *b)
{
    return a != NULL &&
           b != NULL &&
           a->key == b->key &&
           a->scenario == b->scenario;
}

/*
 * Test-only executor used to prove that deterministic generation produces
 * deterministic cache behavior and Stage 20 statistics.
 *
 * This is not a benchmark runner: it records no wall-clock time.
 */
int stage21_execute_generated_workload(Part1Cache *cache,
                                       uint64_t seed,
                                       CacheKey key_space,
                                       size_t operation_count)
{
    WorkloadGenerator generator;
    Stage19RankContext rank_context;
    WorkloadOp operation;
    size_t i;

    if (cache == NULL ||
        !workload_generator_init(&generator, seed, key_space)) {
        return 0;
    }

    for (i = 0U; i < operation_count; ++i) {
        if (!workload_generator_next(&generator, &operation)) {
            return 0;
        }

        rank_context.scenario = operation.scenario;
        rank_context.calls = 0U;

        if (part2_cache_get(cache,
                            operation.key,
                            stage19_rank_provider,
                            &rank_context) == NULL) {
            return 0;
        }
    }

    return part1_cache_validate(cache);
}

int stage21_stats_equal(CacheStats a, CacheStats b)
{
    return a.accesses == b.accesses &&
           a.hits == b.hits &&
           a.misses == b.misses &&
           a.db_reads == b.db_reads &&
           a.insertions == b.insertions &&
           a.evictions == b.evictions &&
           a.rank_provider_calls == b.rank_provider_calls &&
           a.rank_updates == b.rank_updates &&
           a.rank_decreases == b.rank_decreases &&
           a.rank_unchanged == b.rank_unchanged &&
           a.rank_increases == b.rank_increases;
}

int stage21_cache_logical_state_equal(const Part1Cache *a,
                                      const Part1Cache *b,
                                      CacheKey key_space)
{
    CacheKey key;

    if (a == NULL ||
        b == NULL ||
        a->size != b->size ||
        a->capacity != b->capacity ||
        a->min_heap.size != b->min_heap.size) {
        return 0;
    }

    for (key = 1ULL; key <= key_space; ++key) {
        CacheEntry *entry_a = part1_cache_lookup(a, key);
        CacheEntry *entry_b = part1_cache_lookup(b, key);

        if ((entry_a == NULL) != (entry_b == NULL)) {
            return 0;
        }

        if (entry_a != NULL &&
            (entry_a->key != entry_b->key ||
             entry_a->value != entry_b->value ||
             entry_a->rank != entry_b->rank)) {
            return 0;
        }
    }

    if (a->min_heap.size != 0U) {
        CacheEntry *min_a = min_heap_peek(&a->min_heap);
        CacheEntry *min_b = min_heap_peek(&b->min_heap);

        if (min_a == NULL ||
            min_b == NULL ||
            min_a->key != min_b->key ||
            min_a->rank != min_b->rank) {
            return 0;
        }
    }

    return 1;
}

int stage21_test_golden_sequence(void)
{
    static const WorkloadOp expected[] = {
        {16ULL, PART2_RANK_UNCHANGED},
        {10ULL, PART2_RANK_INCREASE},
        {4ULL,  PART2_RANK_UNCHANGED},
        {14ULL, PART2_RANK_INCREASE},
        {8ULL,  PART2_RANK_UNCHANGED},
        {2ULL,  PART2_RANK_INCREASE},
        {12ULL, PART2_RANK_DECREASE},
        {6ULL,  PART2_RANK_INCREASE}
    };
    WorkloadGenerator generator;
    WorkloadOp actual[sizeof(expected) / sizeof(expected[0])];
    size_t i;
    int passed = 1;

    printf("\n[Stage 21] fixed-seed golden workload sequence\n");

    passed &= check(workload_generator_init(
                        &generator,
                        UINT64_C(0x123456789abcdef0),
                        16ULL),
                    "initialize golden workload generator");

    passed &= check(workload_generate(
                        &generator,
                        actual,
                        sizeof(actual) / sizeof(actual[0])),
                    "generate golden workload operations");

    for (i = 0U; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        passed &= check(workload_operation_equal(&actual[i], &expected[i]),
                        "generated operation matches golden sequence");
    }

    return passed;
}

int stage21_test_same_seed_replay(void)
{
    WorkloadGenerator a;
    WorkloadGenerator b;
    WorkloadOp ops_a[64];
    WorkloadOp ops_b[64];
    size_t i;
    int passed = 1;

    printf("\n[Stage 21] same seed reproduces the same workload\n");

    passed &= check(workload_generator_init(
                        &a, UINT64_C(0xfeedface12345678), 32ULL),
                    "initialize first same-seed generator");
    passed &= check(workload_generator_init(
                        &b, UINT64_C(0xfeedface12345678), 32ULL),
                    "initialize second same-seed generator");

    passed &= check(workload_generate(&a, ops_a, 64U),
                    "generate first same-seed stream");
    passed &= check(workload_generate(&b, ops_b, 64U),
                    "generate second same-seed stream");

    for (i = 0U; i < 64U; ++i) {
        passed &= check(workload_operation_equal(&ops_a[i], &ops_b[i]),
                        "same seed produces identical operation");
    }

    return passed;
}

int stage21_test_different_seeds_diverge(void)
{
    WorkloadGenerator a;
    WorkloadGenerator b;
    WorkloadOp op_a;
    WorkloadOp op_b;
    size_t i;
    int difference_seen = 0;
    int passed = 1;

    printf("\n[Stage 21] different seeds diverge\n");

    passed &= check(workload_generator_init(&a, UINT64_C(1), 64ULL),
                    "initialize seed-1 generator");
    passed &= check(workload_generator_init(&b, UINT64_C(2), 64ULL),
                    "initialize seed-2 generator");

    for (i = 0U; i < 32U; ++i) {
        passed &= check(workload_generator_next(&a, &op_a) &&
                        workload_generator_next(&b, &op_b),
                        "generate different-seed operations");

        if (!workload_operation_equal(&op_a, &op_b)) {
            difference_seen = 1;
        }
    }

    passed &= check(difference_seen,
                    "different seeds produce a different workload stream");

    return passed;
}

int stage21_test_bounds_and_scenarios(void)
{
    WorkloadGenerator generator;
    WorkloadOp operation;
    size_t i;
    uint64_t scenario_counts[3] = {0U, 0U, 0U};
    int passed = 1;

    printf("\n[Stage 21] generated keys and scenarios stay in bounds\n");

    passed &= check(workload_generator_init(
                        &generator,
                        UINT64_C(0x1020304050607080),
                        17ULL),
                    "initialize bounded generator");

    for (i = 0U; i < 1000U; ++i) {
        passed &= check(workload_generator_next(&generator, &operation),
                        "generate bounded workload operation");

        passed &= check(operation.key >= 1ULL &&
                        operation.key <= 17ULL,
                        "generated key is within configured key space");

        passed &= check(operation.scenario >= PART2_RANK_DECREASE &&
                        operation.scenario <= PART2_RANK_INCREASE,
                        "generated rank scenario is valid");

        if (operation.scenario >= PART2_RANK_DECREASE &&
            operation.scenario <= PART2_RANK_INCREASE) {
            ++scenario_counts[(size_t)operation.scenario];
        }
    }

    passed &= check(scenario_counts[PART2_RANK_DECREASE] > 0U &&
                    scenario_counts[PART2_RANK_UNCHANGED] > 0U &&
                    scenario_counts[PART2_RANK_INCREASE] > 0U,
                    "bounded stream exercises all three rank scenarios");

    return passed;
}

int stage21_test_reset_replays_stream(void)
{
    WorkloadGenerator generator;
    WorkloadOp first[32];
    WorkloadOp replay[32];
    size_t i;
    int passed = 1;

    printf("\n[Stage 21] reset replays the original stream\n");

    passed &= check(workload_generator_init(
                        &generator,
                        UINT64_C(0x0ddc0ffeebadf00d),
                        25ULL),
                    "initialize reset/replay generator");

    passed &= check(workload_generate(&generator, first, 32U),
                    "generate pre-reset stream");

    workload_generator_reset(&generator);

    passed &= check(workload_generate(&generator, replay, 32U),
                    "generate post-reset stream");

    for (i = 0U; i < 32U; ++i) {
        passed &= check(workload_operation_equal(&first[i], &replay[i]),
                        "reset reproduces original operation");
    }

    return passed;
}

int stage21_test_invalid_generator_arguments(void)
{
    WorkloadGenerator generator;
    WorkloadOp operation;
    int passed = 1;

    printf("\n[Stage 21] invalid generator arguments are rejected\n");

    passed &= check(!workload_generator_init(NULL, 1U, 8ULL),
                    "NULL generator initialization is rejected");
    passed &= check(!workload_generator_init(&generator, 1U, 0ULL),
                    "zero key space is rejected");

    passed &= check(workload_generator_init(&generator, 1U, 8ULL),
                    "initialize valid argument-test generator");

    passed &= check(!workload_generator_next(NULL, &operation),
                    "NULL generator next operation is rejected");
    passed &= check(!workload_generator_next(&generator, NULL),
                    "NULL operation output is rejected");

    passed &= check(workload_generate(&generator, NULL, 0U),
                    "zero-count generation accepts NULL output");
    passed &= check(!workload_generate(&generator, NULL, 1U),
                    "nonzero generation rejects NULL output");

    return passed;
}

int stage21_test_deterministic_execution_and_statistics(void)
{
    Part1Cache first;
    Part1Cache second;
    CacheStats first_stats;
    CacheStats second_stats;
    const uint64_t seed = UINT64_C(0x3141592653589793);
    const CacheKey key_space = 16ULL;
    const size_t operation_count = 200U;
    int passed = 1;

    printf("\n[Stage 21] deterministic workload reproduces cache state and statistics\n");

    passed &= check(part1_cache_init(&first, 8U),
                    "initialize first workload-execution cache");
    passed &= check(part1_cache_init(&second, 8U),
                    "initialize second workload-execution cache");

    passed &= check(stage21_execute_generated_workload(
                        &first, seed, key_space, operation_count),
                    "execute first deterministic workload");
    passed &= check(stage21_execute_generated_workload(
                        &second, seed, key_space, operation_count),
                    "execute replayed deterministic workload");

    first_stats = part1_cache_stats_snapshot(&first);
    second_stats = part1_cache_stats_snapshot(&second);

    passed &= check(stage21_stats_equal(first_stats, second_stats),
                    "replayed workload produces identical statistics");

    passed &= check(first_stats.accesses == operation_count &&
                    second_stats.accesses == operation_count,
                    "workload statistics record exact operation count");

    passed &= check(stage21_cache_logical_state_equal(
                        &first, &second, key_space),
                    "replayed workload produces identical logical cache state");

    passed &= check(part1_cache_validate(&first) &&
                    part1_cache_validate(&second),
                    "both replayed caches satisfy integrated invariants");

    return passed;
}

int stage21_run_deterministic_workload_tests(void)
{
    int passed = 1;

    printf("\n=== Stage 21: add deterministic workload generation ===\n");

    passed &= stage21_test_golden_sequence();
    passed &= stage21_test_same_seed_replay();
    passed &= stage21_test_different_seeds_diverge();
    passed &= stage21_test_bounds_and_scenarios();
    passed &= stage21_test_reset_replays_stream();
    passed &= stage21_test_invalid_generator_arguments();
    passed &= stage21_test_deterministic_execution_and_statistics();

    printf("\nStage 21 findings:\n");
    printf("  workload generation uses a fixed 64-bit LCG, not platform rand().\n");
    printf("  the same seed/key-space/count reproduces the same operation stream.\n");
    printf("  a fixed seed is protected by an explicit golden operation sequence.\n");
    printf("  generated keys stay inside the configured key space.\n");
    printf("  generated operations cover decrease/equal/increase rank scenarios.\n");
    printf("  reset replays the stream from its original seed.\n");
    printf("  replayed workloads reproduce cache statistics and logical cache state.\n");
    printf("Stage 21 boundary: generation is deterministic; no timing benchmark is added.\n");
    printf("Stage 21 deterministic-workload validation: %s\n",
           passed ? "PASS" : "FAIL");

    return passed;
}

int main(void)
{
    Cache cache;
    Cache invalid;
    Cache duplicate;
    Cache profile;
    CacheEntry *entry;
    LookupStats stats;
    size_t sizes[] = {1U, 10U, 25U, 50U, 100U};
    size_t i;
    int all_passed = 1;

    printf("=== Stage 6 regression: identified linear lookup bottleneck ===\n");

    /* Stage 5 regression: normal cache behavior remains correct. */
    all_passed &= check(cache_init(&cache, 3U),
                        "initialize valid cache");
    all_passed &= check(cache_validate(&cache),
                        "fresh cache satisfies invariants");

    all_passed &= check(cache_get(&cache, 1ULL) != NULL,
                        "GET 1 succeeds");
    all_passed &= check(cache_get(&cache, 2ULL) != NULL,
                        "GET 2 succeeds");
    all_passed &= check(cache_get(&cache, 3ULL) != NULL,
                        "GET 3 succeeds");

    entry = cache_get(&cache, 2ULL);
    all_passed &= check(entry != NULL && entry->key == 2ULL,
                        "cache hit remains correct");

    entry = cache_get(&cache, 4ULL);
    all_passed &= check(entry != NULL && entry->key == 4ULL,
                        "full-cache miss remains correct");
    all_passed &= check(cache_validate(&cache),
                        "post-eviction cache satisfies invariants");

    /* Preserve Stage 5 invalid-state checks. */
    invalid = cache;
    invalid.size = invalid.capacity + 1U;
    all_passed &= check(!cache_validate(&invalid),
                        "validator detects size greater than capacity");

    invalid = cache;
    invalid.capacity = 0U;
    all_passed &= check(!cache_validate(&invalid),
                        "validator detects zero capacity");

    duplicate = cache;
    duplicate.entries[1].key = duplicate.entries[0].key;
    all_passed &= check(!cache_validate(&duplicate),
                        "validator detects duplicate resident keys");

    all_passed &= check(!cache_validate(NULL),
                        "validator rejects NULL cache");

    /*
     * Stage 6 experiment 1: within one 100-entry cache, compare the first,
     * middle, last, and missing-key cases. A linear scan should require
     * 1, 50, 100, and 100 key comparisons respectively.
     */
    all_passed &= check(stage6_fill_profile_cache(&profile, 100U),
                        "prepare 100-entry profiling cache");

    all_passed &= stage6_check_lookup_cost(&profile, 1ULL, 1U, 1,
                                           "first-entry lookup");
    all_passed &= stage6_check_lookup_cost(&profile, 50ULL, 50U, 1,
                                           "middle-entry lookup");
    all_passed &= stage6_check_lookup_cost(&profile, 100ULL, 100U, 1,
                                           "last-entry lookup");
    all_passed &= stage6_check_lookup_cost(&profile, 1000ULL, 100U, 0,
                                           "missing-entry lookup");

    /*
     * Stage 6 experiment 2: scale resident size while always requesting a
     * missing key. Exact comparison count must grow one-for-one with N.
     */
    printf("\nStage 6 lookup scaling (missing key):\n");
    printf("  size    key-comparisons\n");

    for (i = 0U; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t n = sizes[i];

        all_passed &= check(stage6_fill_profile_cache(&profile, n),
                            "prepare scaling cache");

        lookup_stats_reset();
        entry = cache_lookup(&profile, 1000ULL);
        stats = lookup_stats_snapshot();

        printf("  %4zu    %llu\n",
               n,
               (unsigned long long)stats.key_comparisons);

        all_passed &= check(entry == NULL &&
                            stats.lookup_calls == 1U &&
                            stats.lookup_misses == 1U &&
                            stats.key_comparisons == (uint64_t)n,
                            "missing lookup comparisons equal cache size");
    }

    printf("\nStage 6 finding: cache_lookup() grows linearly with resident size.\n");
    printf("First bottleneck identified: O(N) key lookup.\n");
    printf("Stage 6 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage7_run_hash_table_tests();

    printf("\nStage 7 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage8_run_min_rank_bottleneck_tests();

    printf("\nStage 8 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage9_run_min_heap_tests();

    printf("\nStage 9 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage10_run_integrated_part1_tests();

    printf("\nStage 10 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage11_run_part1_thorough_tests();

    printf("\nStage 11 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage12_run_part2_contract_tests();

    printf("\nStage 12 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage13_run_ordinary_heap_limitation_tests();

    printf("\nStage 13 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage14_run_heap_index_tests();

    printf("\nStage 14 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage15_run_heap_swap_tests();

    printf("\nStage 15 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage16_run_heap_update_rank_tests();

    printf("\nStage 16 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage17_run_rank_decrease_tests();

    printf("\nStage 17 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage18_run_rank_increase_tests();

    printf("\nStage 18 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage19_run_dynamic_hit_integration_tests();

    printf("\nStage 19 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage20_run_statistics_tests();

    printf("\nStage 20 regression validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    all_passed &= stage21_run_deterministic_workload_tests();

    printf("\nStage 21 validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    return all_passed ? 0 : 1;
}
