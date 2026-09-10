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
 */

#include <stdio.h>
#include <stddef.h>
#include <assert.h>
#include <stdint.h>

#define MAX_CACHE_CAPACITY 100U
#define HASH_TABLE_CAPACITY 211U

typedef unsigned long long CacheKey;
typedef long long Rank;

typedef struct {
    CacheKey key;
    unsigned long long value;
    Rank rank;
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

void min_heap_swap(MinHeap *heap, size_t a, size_t b)
{
    CacheEntry *tmp = heap->items[a];
    heap->items[a] = heap->items[b];
    heap->items[b] = tmp;
}

void min_heap_sift_up(MinHeap *heap, size_t index)
{
    while (index > 0U) {
        size_t parent = (index - 1U) / 2U;

        if (!min_heap_entry_less(heap->items[index], heap->items[parent])) {
            break;
        }

        min_heap_swap(heap, index, parent);
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

        min_heap_swap(heap, index, smallest);
        index = smallest;
    }
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
        min_heap_sift_down(heap, 0U);
    } else {
        heap->items[0] = NULL;
    }

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

        if (heap->items[i] == NULL) {
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
        {1ULL, 100ULL, 50LL},
        {2ULL, 200ULL, 20LL},
        {3ULL, 300ULL, 80LL},
        {4ULL, 400ULL, 10LL},
        {5ULL, 500ULL, 60LL},
        {6ULL, 600ULL, 20LL}
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
        CacheEntry extra = {9999ULL, 999900ULL, -9999LL};

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

    printf("\nStage 9 validation: %s\n",
           all_passed ? "PASS" : "FAIL");

    return all_passed ? 0 : 1;
}
