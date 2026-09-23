#define _POSIX_C_SOURCE 200809L

/*
 * ranked_cached_cmp_alternative.c
 *
 * Stage 28 — compare alternative ranked-cache implementations.
 *
 * This file is intentionally separate from ranked_cache.c.  The main source
 * remains the incremental production/reference implementation through Stage 27.
 * V6 adds validation + instrumentation + benchmark on top of the five cache
 * alternatives.  The A-E algorithms remain unchanged; V6 observes them.
 *
 * This comparison program implements five complete caches that share exactly
 * the same external semantics so their data-structure trade-offs can be studied
 * side-by-side:
 *
 *   Version A — linear baseline
 *       array only
 *       lookup          O(N)
 *       minimum/evict   O(N)
 *       rank mutation   O(1) after the resident is known
 *
 *   Version B — hash + linear minimum
 *       hash lookup     expected O(1)
 *       minimum/evict   O(N)
 *       rank mutation   O(1) after hash lookup
 *
 *   Version C — hash + AVL tree
 *       hash lookup     expected O(1)
 *       rank update     O(log N)
 *       minimum         O(log N) traversal to leftmost node
 *       eviction        O(log N)
 *
 *   Version D — hash + lazy heap
 *       hash lookup     expected O(1)
 *       no in-place heap priority update
 *       rank change     append a new versioned heap record, O(log M)
 *       old heap records remain stale until lazily discarded
 *       minimum         amortized cleanup of stale records
 *       memory          grows with updates (M may be much larger than N)
 *
 *   Version E — hash + indexed heap
 *       hash lookup     expected O(1)
 *       rank update     O(log N)
 *       minimum         O(1)
 *       eviction        O(log N)
 *       bounded heap    exactly one heap position per resident
 *
 * All versions use the same deterministic policy:
 *
 *     smaller rank wins; on equal rank, smaller key wins.
 *
 * Dynamic rank changes occur on hits only.  Misses read the deterministic
 * backing database, evict the current minimum if full, then insert the fetched
 * entry.  This mirrors the Part 2 semantics validated in ranked_cache.c.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CMP_MAX_CAPACITY 100U
#define CMP_HASH_CAPACITY 257U
#define CMP_LAZY_HEAP_MAX 8192U
#define CMP_HEAP_INDEX_NONE SIZE_MAX

typedef unsigned long long CacheKey;
typedef long long Rank;

typedef struct {
    CacheKey key;
    unsigned long long value;
    Rank rank;
} CacheEntry;

typedef enum {
    RANK_DECREASE = 0,
    RANK_UNCHANGED = 1,
    RANK_INCREASE = 2
} RankScenario;

typedef struct {
    uint64_t accesses;
    uint64_t hits;
    uint64_t misses;
    uint64_t db_reads;
    uint64_t insertions;
    uint64_t evictions;
    uint64_t rank_updates;
    uint64_t rank_decreases;
    uint64_t rank_unchanged;
    uint64_t rank_increases;
} CacheStats;

typedef struct {
    uint64_t state;
    CacheKey key_space;
} WorkloadGenerator;

typedef struct {
    CacheKey key;
    RankScenario scenario;
} WorkloadOp;


/* ------------------------------------------------------------------------- */
/* V6 — validation + instrumentation + benchmark                             */
/* ------------------------------------------------------------------------- */

/*
 * Instrumentation is intentionally external to the A-E data structures.
 *
 * A single active pointer lets the benchmark disable instrumentation entirely
 * while the dedicated instrumentation pass counts primitive operations.
 */
typedef struct {
    uint64_t linear_lookup_comparisons;
    uint64_t linear_min_comparisons;

    uint64_t hash_slot_probes;

    uint64_t avl_key_comparisons;
    uint64_t avl_rotations;

    uint64_t lazy_heap_comparisons;
    uint64_t lazy_heap_swaps;
    uint64_t lazy_heap_pushes;
    uint64_t lazy_heap_pops;
    uint64_t lazy_stale_discards;

    uint64_t indexed_heap_comparisons;
    uint64_t indexed_heap_swaps;
} V6Instrumentation;

typedef struct {
    double seconds;
    double ns_per_op;
    uint64_t checksum;
} V6Timing;

static V6Instrumentation *g_v6_instrumentation = NULL;

static void v6_count(uint64_t *counter)
{
    if (g_v6_instrumentation != NULL && counter != NULL) {
        ++(*counter);
    }
}

static double v6_elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static CacheEntry db_read_entry(CacheKey key)
{
    CacheEntry e;
    e.key = key;
    e.value = key * 100ULL;
    e.rank = (Rank)(key * 10ULL);
    return e;
}

static Rank apply_rank_scenario(Rank current, RankScenario scenario)
{
    switch (scenario) {
    case RANK_DECREASE:
        return current - 30LL;
    case RANK_UNCHANGED:
        return current;
    case RANK_INCREASE:
        return current + 30LL;
    default:
        return current;
    }
}

static int entry_less(const CacheEntry *a, const CacheEntry *b)
{
    if (a->rank != b->rank) {
        return a->rank < b->rank;
    }
    return a->key < b->key;
}

static void stats_record_rank_change(CacheStats *stats, Rank old_rank, Rank new_rank)
{
    ++stats->rank_updates;
    if (new_rank < old_rank) {
        ++stats->rank_decreases;
    } else if (new_rank > old_rank) {
        ++stats->rank_increases;
    } else {
        ++stats->rank_unchanged;
    }
}

static int stats_equal(CacheStats a, CacheStats b)
{
    return a.accesses == b.accesses &&
           a.hits == b.hits &&
           a.misses == b.misses &&
           a.db_reads == b.db_reads &&
           a.insertions == b.insertions &&
           a.evictions == b.evictions &&
           a.rank_updates == b.rank_updates &&
           a.rank_decreases == b.rank_decreases &&
           a.rank_unchanged == b.rank_unchanged &&
           a.rank_increases == b.rank_increases;
}

static uint64_t workload_next_u64(WorkloadGenerator *g)
{
    g->state = g->state * UINT64_C(6364136223846793005) +
               UINT64_C(1442695040888963407);
    return g->state;
}

static int workload_init(WorkloadGenerator *g, uint64_t seed, CacheKey key_space)
{
    if (g == NULL || key_space == 0ULL) {
        return 0;
    }
    g->state = seed;
    g->key_space = key_space;
    return 1;
}

static int workload_next(WorkloadGenerator *g, WorkloadOp *op)
{
    uint64_t key_bits;
    uint64_t scenario_bits;

    if (g == NULL || op == NULL || g->key_space == 0ULL) {
        return 0;
    }

    key_bits = workload_next_u64(g);
    scenario_bits = workload_next_u64(g);
    op->key = (CacheKey)(key_bits % g->key_space) + 1ULL;
    op->scenario = (RankScenario)(scenario_bits % 3ULL);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Shared open-addressing hash table used by Versions B, C, D, and E.       */
/* ------------------------------------------------------------------------- */

typedef enum {
    HASH_EMPTY = 0,
    HASH_OCCUPIED = 1,
    HASH_DELETED = 2
} HashState;

typedef struct {
    CacheKey key;
    void *value;
    HashState state;
} HashSlot;

typedef struct {
    HashSlot slots[CMP_HASH_CAPACITY];
    size_t size;
} HashTable;

static size_t hash_bucket(CacheKey key)
{
    return (size_t)(key % CMP_HASH_CAPACITY);
}

static void hash_init(HashTable *table)
{
    size_t i;
    table->size = 0U;
    for (i = 0U; i < CMP_HASH_CAPACITY; ++i) {
        table->slots[i].key = 0ULL;
        table->slots[i].value = NULL;
        table->slots[i].state = HASH_EMPTY;
    }
}

static void *hash_lookup(const HashTable *table, CacheKey key)
{
    size_t start;
    size_t step;

    if (table == NULL) {
        return NULL;
    }

    start = hash_bucket(key);
    for (step = 0U; step < CMP_HASH_CAPACITY; ++step) {
        size_t index = (start + step) % CMP_HASH_CAPACITY;
        const HashSlot *slot = &table->slots[index];

        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->hash_slot_probes);
        }

        if (slot->state == HASH_EMPTY) {
            return NULL;
        }
        if (slot->state == HASH_OCCUPIED && slot->key == key) {
            return slot->value;
        }
    }
    return NULL;
}

static int hash_insert(HashTable *table, CacheKey key, void *value)
{
    size_t start;
    size_t step;
    size_t first_deleted = SIZE_MAX;

    if (table == NULL || value == NULL || table->size >= CMP_HASH_CAPACITY) {
        return 0;
    }

    start = hash_bucket(key);
    for (step = 0U; step < CMP_HASH_CAPACITY; ++step) {
        size_t index = (start + step) % CMP_HASH_CAPACITY;
        HashSlot *slot = &table->slots[index];

        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->hash_slot_probes);
        }

        if (slot->state == HASH_OCCUPIED && slot->key == key) {
            return 0;
        }
        if (slot->state == HASH_DELETED && first_deleted == SIZE_MAX) {
            first_deleted = index;
        }
        if (slot->state == HASH_EMPTY) {
            size_t target = first_deleted != SIZE_MAX ? first_deleted : index;
            table->slots[target].key = key;
            table->slots[target].value = value;
            table->slots[target].state = HASH_OCCUPIED;
            ++table->size;
            return 1;
        }
    }

    if (first_deleted != SIZE_MAX) {
        table->slots[first_deleted].key = key;
        table->slots[first_deleted].value = value;
        table->slots[first_deleted].state = HASH_OCCUPIED;
        ++table->size;
        return 1;
    }
    return 0;
}

static int hash_remove(HashTable *table, CacheKey key)
{
    size_t start;
    size_t step;

    if (table == NULL) {
        return 0;
    }

    start = hash_bucket(key);
    for (step = 0U; step < CMP_HASH_CAPACITY; ++step) {
        size_t index = (start + step) % CMP_HASH_CAPACITY;
        HashSlot *slot = &table->slots[index];

        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->hash_slot_probes);
        }

        if (slot->state == HASH_EMPTY) {
            return 0;
        }
        if (slot->state == HASH_OCCUPIED && slot->key == key) {
            slot->state = HASH_DELETED;
            slot->value = NULL;
            --table->size;
            return 1;
        }
    }
    return 0;
}

static int hash_validate(const HashTable *table)
{
    size_t occupied = 0U;
    size_t i;

    if (table == NULL) {
        return 0;
    }

    for (i = 0U; i < CMP_HASH_CAPACITY; ++i) {
        if (table->slots[i].state == HASH_OCCUPIED) {
            ++occupied;
            if (table->slots[i].value == NULL) {
                return 0;
            }
        }
    }
    return occupied == table->size;
}

/* ------------------------------------------------------------------------- */
/* Version A — linear baseline                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    CacheEntry entries[CMP_MAX_CAPACITY];
    size_t size;
    size_t capacity;
    CacheStats stats;
} VersionA;

static int a_init(VersionA *cache, size_t capacity)
{
    if (cache == NULL || capacity == 0U || capacity > CMP_MAX_CAPACITY) {
        return 0;
    }
    memset(cache, 0, sizeof(*cache));
    cache->capacity = capacity;
    return 1;
}

static CacheEntry *a_lookup(VersionA *cache, CacheKey key)
{
    size_t i;
    for (i = 0U; i < cache->size; ++i) {
        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->linear_lookup_comparisons);
        }
        if (cache->entries[i].key == key) {
            return &cache->entries[i];
        }
    }
    return NULL;
}

static size_t a_min_index(const VersionA *cache)
{
    size_t best = 0U;
    size_t i;
    for (i = 1U; i < cache->size; ++i) {
        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->linear_min_comparisons);
        }
        if (entry_less(&cache->entries[i], &cache->entries[best])) {
            best = i;
        }
    }
    return best;
}

static CacheEntry *a_get(VersionA *cache, CacheKey key, RankScenario scenario)
{
    CacheEntry *resident;
    CacheEntry fetched;

    if (cache == NULL) {
        return NULL;
    }

    ++cache->stats.accesses;
    resident = a_lookup(cache, key);
    if (resident != NULL) {
        Rank old_rank = resident->rank;
        Rank new_rank = apply_rank_scenario(old_rank, scenario);
        ++cache->stats.hits;
        resident->rank = new_rank; /* O(1) once resident is known. */
        stats_record_rank_change(&cache->stats, old_rank, new_rank);
        return resident;
    }

    ++cache->stats.misses;
    ++cache->stats.db_reads;
    fetched = db_read_entry(key);

    if (cache->size == cache->capacity) {
        size_t victim = a_min_index(cache); /* O(N). */
        cache->entries[victim] = cache->entries[cache->size - 1U];
        --cache->size;
        ++cache->stats.evictions;
    }

    cache->entries[cache->size] = fetched;
    ++cache->size;
    ++cache->stats.insertions;
    return &cache->entries[cache->size - 1U];
}

static int a_validate(const VersionA *cache)
{
    size_t i;
    size_t j;
    if (cache == NULL || cache->size > cache->capacity ||
        cache->capacity == 0U || cache->capacity > CMP_MAX_CAPACITY) {
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

/* ------------------------------------------------------------------------- */
/* Stable-slot helper used by Versions B, C, D, and E.                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    CacheEntry data;
    unsigned char active;
    size_t heap_index;   /* Version E. */
    uint64_t generation; /* Version D. */
} StableEntry;

typedef struct {
    StableEntry entries[CMP_MAX_CAPACITY];
    size_t free_stack[CMP_MAX_CAPACITY];
    size_t free_count;
    size_t size;
    size_t capacity;
} StablePool;

static int pool_init(StablePool *pool, size_t capacity)
{
    size_t i;
    if (pool == NULL || capacity == 0U || capacity > CMP_MAX_CAPACITY) {
        return 0;
    }
    memset(pool, 0, sizeof(*pool));
    pool->capacity = capacity;
    pool->free_count = capacity;
    for (i = 0U; i < capacity; ++i) {
        pool->free_stack[i] = capacity - 1U - i;
        pool->entries[i].heap_index = CMP_HEAP_INDEX_NONE;
    }
    return 1;
}

static StableEntry *pool_alloc(StablePool *pool, CacheEntry data)
{
    size_t index;
    StableEntry *entry;
    if (pool == NULL || pool->free_count == 0U) {
        return NULL;
    }
    index = pool->free_stack[--pool->free_count];
    entry = &pool->entries[index];
    entry->data = data;
    entry->active = 1U;
    entry->heap_index = CMP_HEAP_INDEX_NONE;
    entry->generation = 1U;
    ++pool->size;
    return entry;
}

static int pool_release(StablePool *pool, StableEntry *entry)
{
    size_t index;
    if (pool == NULL || entry == NULL || !entry->active) {
        return 0;
    }
    index = (size_t)(entry - pool->entries);
    if (index >= pool->capacity || pool->free_count >= pool->capacity) {
        return 0;
    }
    entry->active = 0U;
    entry->heap_index = CMP_HEAP_INDEX_NONE;
    pool->free_stack[pool->free_count++] = index;
    --pool->size;
    return 1;
}

static int pool_validate(const StablePool *pool)
{
    size_t active = 0U;
    size_t i;
    if (pool == NULL || pool->capacity == 0U ||
        pool->capacity > CMP_MAX_CAPACITY || pool->size > pool->capacity ||
        pool->free_count + pool->size != pool->capacity) {
        return 0;
    }
    for (i = 0U; i < pool->capacity; ++i) {
        if (pool->entries[i].active) {
            ++active;
        }
    }
    return active == pool->size;
}

/* ------------------------------------------------------------------------- */
/* Version B — hash + linear minimum                                         */
/* ------------------------------------------------------------------------- */

typedef struct {
    StablePool pool;
    HashTable hash;
    CacheStats stats;
} VersionB;

static int b_init(VersionB *cache, size_t capacity)
{
    if (cache == NULL || !pool_init(&cache->pool, capacity)) {
        return 0;
    }
    hash_init(&cache->hash);
    memset(&cache->stats, 0, sizeof(cache->stats));
    return 1;
}

static StableEntry *b_find_min(VersionB *cache)
{
    StableEntry *best = NULL;
    size_t i;
    for (i = 0U; i < cache->pool.capacity; ++i) {
        StableEntry *candidate = &cache->pool.entries[i];
        if (candidate->active) {
            if (best != NULL && g_v6_instrumentation != NULL) {
                v6_count(&g_v6_instrumentation->linear_min_comparisons);
            }
            if (best == NULL || entry_less(&candidate->data, &best->data)) {
                best = candidate;
            }
        }
    }
    return best;
}

static CacheEntry *b_get(VersionB *cache, CacheKey key, RankScenario scenario)
{
    StableEntry *resident;
    CacheEntry fetched;

    if (cache == NULL) {
        return NULL;
    }

    ++cache->stats.accesses;
    resident = (StableEntry *)hash_lookup(&cache->hash, key); /* expected O(1). */
    if (resident != NULL) {
        Rank old_rank = resident->data.rank;
        Rank new_rank = apply_rank_scenario(old_rank, scenario);
        ++cache->stats.hits;
        resident->data.rank = new_rank; /* O(1) after hash lookup. */
        stats_record_rank_change(&cache->stats, old_rank, new_rank);
        return &resident->data;
    }

    ++cache->stats.misses;
    ++cache->stats.db_reads;
    fetched = db_read_entry(key);

    if (cache->pool.size == cache->pool.capacity) {
        StableEntry *victim = b_find_min(cache); /* O(N). */
        if (victim == NULL || !hash_remove(&cache->hash, victim->data.key) ||
            !pool_release(&cache->pool, victim)) {
            return NULL;
        }
        ++cache->stats.evictions;
    }

    resident = pool_alloc(&cache->pool, fetched);
    if (resident == NULL || !hash_insert(&cache->hash, key, resident)) {
        if (resident != NULL) {
            (void)pool_release(&cache->pool, resident);
        }
        return NULL;
    }
    ++cache->stats.insertions;
    return &resident->data;
}

static int b_validate(const VersionB *cache)
{
    size_t i;
    if (cache == NULL || !pool_validate(&cache->pool) ||
        !hash_validate(&cache->hash) || cache->hash.size != cache->pool.size) {
        return 0;
    }
    for (i = 0U; i < cache->pool.capacity; ++i) {
        const StableEntry *entry = &cache->pool.entries[i];
        if (entry->active && hash_lookup(&cache->hash, entry->data.key) != entry) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Version C — hash + AVL tree keyed by (rank,key)                           */
/* ------------------------------------------------------------------------- */

typedef struct AvlNode {
    StableEntry *entry;
    struct AvlNode *left;
    struct AvlNode *right;
    int height;
} AvlNode;

typedef struct {
    StablePool pool;
    HashTable hash;
    AvlNode *root;
    CacheStats stats;
} VersionC;

static int avl_height(const AvlNode *node)
{
    return node == NULL ? 0 : node->height;
}

static int avl_max_int(int a, int b)
{
    return a > b ? a : b;
}

static void avl_refresh(AvlNode *node)
{
    node->height = 1 + avl_max_int(avl_height(node->left), avl_height(node->right));
}

static int avl_entry_cmp(const CacheEntry *a, const CacheEntry *b)
{
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->avl_key_comparisons);
    }
    if (a->rank < b->rank) return -1;
    if (a->rank > b->rank) return 1;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return 0;
}

static AvlNode *avl_rotate_right(AvlNode *y)
{
    AvlNode *x = y->left;
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->avl_rotations);
    }
    AvlNode *t2 = x->right;
    x->right = y;
    y->left = t2;
    avl_refresh(y);
    avl_refresh(x);
    return x;
}

static AvlNode *avl_rotate_left(AvlNode *x)
{
    AvlNode *y = x->right;
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->avl_rotations);
    }
    AvlNode *t2 = y->left;
    y->left = x;
    x->right = t2;
    avl_refresh(x);
    avl_refresh(y);
    return y;
}

static AvlNode *avl_balance(AvlNode *node)
{
    int balance;
    if (node == NULL) return NULL;
    avl_refresh(node);
    balance = avl_height(node->left) - avl_height(node->right);
    if (balance > 1) {
        if (avl_height(node->left->left) < avl_height(node->left->right)) {
            node->left = avl_rotate_left(node->left);
        }
        return avl_rotate_right(node);
    }
    if (balance < -1) {
        if (avl_height(node->right->right) < avl_height(node->right->left)) {
            node->right = avl_rotate_right(node->right);
        }
        return avl_rotate_left(node);
    }
    return node;
}

static AvlNode *avl_insert_node(AvlNode *root, AvlNode *node)
{
    int cmp;
    if (root == NULL) return node;
    cmp = avl_entry_cmp(&node->entry->data, &root->entry->data);
    if (cmp < 0) {
        root->left = avl_insert_node(root->left, node);
    } else if (cmp > 0) {
        root->right = avl_insert_node(root->right, node);
    } else {
        return root;
    }
    return avl_balance(root);
}

static AvlNode *avl_min_node(AvlNode *root)
{
    AvlNode *current = root;
    while (current != NULL && current->left != NULL) {
        current = current->left;
    }
    return current;
}

static AvlNode *avl_delete_key(AvlNode *root,
                               const CacheEntry *key,
                               StableEntry **removed_entry)
{
    int cmp;
    if (root == NULL) return NULL;

    cmp = avl_entry_cmp(key, &root->entry->data);
    if (cmp < 0) {
        root->left = avl_delete_key(root->left, key, removed_entry);
    } else if (cmp > 0) {
        root->right = avl_delete_key(root->right, key, removed_entry);
    } else {
        if (root->left == NULL || root->right == NULL) {
            AvlNode *child = root->left != NULL ? root->left : root->right;
            if (removed_entry != NULL) {
                *removed_entry = root->entry;
            }
            free(root);
            return child;
        } else {
            AvlNode *successor = avl_min_node(root->right);
            StableEntry *old_owner = root->entry;
            CacheEntry old_key = old_owner->data;
            root->entry = successor->entry;
            successor->entry = old_owner;
            root->right = avl_delete_key(root->right, &old_key, removed_entry);
        }
    }
    return avl_balance(root);
}

static int avl_insert_entry(VersionC *cache, StableEntry *entry)
{
    AvlNode *node = (AvlNode *)calloc(1U, sizeof(*node));
    if (node == NULL) return 0;
    node->entry = entry;
    node->height = 1;
    cache->root = avl_insert_node(cache->root, node);
    return 1;
}

static int avl_remove_entry(VersionC *cache, StableEntry *entry)
{
    CacheEntry key;
    StableEntry *removed = NULL;
    if (cache == NULL || entry == NULL) return 0;
    key = entry->data;
    cache->root = avl_delete_key(cache->root, &key, &removed);
    return removed == entry;
}

static int avl_validate_rec(const AvlNode *node,
                            const CacheEntry *lower,
                            const CacheEntry *upper,
                            size_t *count)
{
    int expected_height;
    int balance;
    if (node == NULL) return 1;
    if (node->entry == NULL || !node->entry->active) return 0;
    if (lower != NULL && avl_entry_cmp(&node->entry->data, lower) <= 0) return 0;
    if (upper != NULL && avl_entry_cmp(&node->entry->data, upper) >= 0) return 0;
    if (!avl_validate_rec(node->left, lower, &node->entry->data, count)) return 0;
    ++*count;
    if (!avl_validate_rec(node->right, &node->entry->data, upper, count)) return 0;
    expected_height = 1 + avl_max_int(avl_height(node->left), avl_height(node->right));
    balance = avl_height(node->left) - avl_height(node->right);
    return node->height == expected_height && balance >= -1 && balance <= 1;
}

static void avl_free_all(AvlNode *node)
{
    if (node == NULL) return;
    avl_free_all(node->left);
    avl_free_all(node->right);
    free(node);
}

static int c_init(VersionC *cache, size_t capacity)
{
    if (cache == NULL || !pool_init(&cache->pool, capacity)) return 0;
    hash_init(&cache->hash);
    cache->root = NULL;
    memset(&cache->stats, 0, sizeof(cache->stats));
    return 1;
}

static CacheEntry *c_get(VersionC *cache, CacheKey key, RankScenario scenario)
{
    StableEntry *resident;
    CacheEntry fetched;
    if (cache == NULL) return NULL;

    ++cache->stats.accesses;
    resident = (StableEntry *)hash_lookup(&cache->hash, key);
    if (resident != NULL) {
        Rank old_rank = resident->data.rank;
        Rank new_rank = apply_rank_scenario(old_rank, scenario);
        ++cache->stats.hits;
        if (!avl_remove_entry(cache, resident)) return NULL;
        resident->data.rank = new_rank;
        if (!avl_insert_entry(cache, resident)) return NULL;
        stats_record_rank_change(&cache->stats, old_rank, new_rank);
        return &resident->data;
    }

    ++cache->stats.misses;
    ++cache->stats.db_reads;
    fetched = db_read_entry(key);

    if (cache->pool.size == cache->pool.capacity) {
        AvlNode *min_node = avl_min_node(cache->root);
        StableEntry *victim;
        if (min_node == NULL) return NULL;
        victim = min_node->entry;
        if (!avl_remove_entry(cache, victim) ||
            !hash_remove(&cache->hash, victim->data.key) ||
            !pool_release(&cache->pool, victim)) {
            return NULL;
        }
        ++cache->stats.evictions;
    }

    resident = pool_alloc(&cache->pool, fetched);
    if (resident == NULL || !hash_insert(&cache->hash, key, resident) ||
        !avl_insert_entry(cache, resident)) {
        return NULL;
    }
    ++cache->stats.insertions;
    return &resident->data;
}

static int c_validate(const VersionC *cache)
{
    size_t tree_count = 0U;
    size_t i;
    if (cache == NULL || !pool_validate(&cache->pool) ||
        !hash_validate(&cache->hash) || cache->hash.size != cache->pool.size ||
        !avl_validate_rec(cache->root, NULL, NULL, &tree_count) ||
        tree_count != cache->pool.size) {
        return 0;
    }
    for (i = 0U; i < cache->pool.capacity; ++i) {
        const StableEntry *entry = &cache->pool.entries[i];
        if (entry->active && hash_lookup(&cache->hash, entry->data.key) != entry) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Version D — hash + lazy heap                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    StableEntry *entry;
    CacheKey key_snapshot;
    Rank rank_snapshot;
    uint64_t generation_snapshot;
} LazyHeapNode;

typedef struct {
    LazyHeapNode nodes[CMP_LAZY_HEAP_MAX];
    size_t size;
} LazyHeap;

typedef struct {
    StablePool pool;
    HashTable hash;
    LazyHeap heap;
    CacheStats stats;
    uint64_t stale_discards;
} VersionD;

static int lazy_node_less(const LazyHeapNode *a, const LazyHeapNode *b)
{
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->lazy_heap_comparisons);
    }
    if (a->rank_snapshot != b->rank_snapshot) {
        return a->rank_snapshot < b->rank_snapshot;
    }
    return a->key_snapshot < b->key_snapshot;
}

static void lazy_swap(LazyHeapNode *a, LazyHeapNode *b)
{
    LazyHeapNode tmp = *a;
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->lazy_heap_swaps);
    }
    *a = *b;
    *b = tmp;
}

static int lazy_push(LazyHeap *heap, LazyHeapNode node)
{
    size_t i;
    if (heap == NULL || heap->size >= CMP_LAZY_HEAP_MAX) return 0;
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->lazy_heap_pushes);
    }
    i = heap->size++;
    heap->nodes[i] = node;
    while (i > 0U) {
        size_t parent = (i - 1U) / 2U;
        if (!lazy_node_less(&heap->nodes[i], &heap->nodes[parent])) break;
        lazy_swap(&heap->nodes[i], &heap->nodes[parent]);
        i = parent;
    }
    return 1;
}

static int lazy_pop_raw(LazyHeap *heap, LazyHeapNode *out)
{
    size_t i = 0U;
    if (heap == NULL || heap->size == 0U) return 0;
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->lazy_heap_pops);
    }
    if (out != NULL) *out = heap->nodes[0];
    --heap->size;
    if (heap->size == 0U) return 1;
    heap->nodes[0] = heap->nodes[heap->size];
    while (1) {
        size_t left = 2U * i + 1U;
        size_t right = left + 1U;
        size_t best = i;
        if (left < heap->size && lazy_node_less(&heap->nodes[left], &heap->nodes[best])) best = left;
        if (right < heap->size && lazy_node_less(&heap->nodes[right], &heap->nodes[best])) best = right;
        if (best == i) break;
        lazy_swap(&heap->nodes[i], &heap->nodes[best]);
        i = best;
    }
    return 1;
}

static int lazy_is_current(const LazyHeapNode *node)
{
    const StableEntry *entry;
    if (node == NULL || node->entry == NULL) return 0;
    entry = node->entry;
    return entry->active &&
           entry->data.key == node->key_snapshot &&
           entry->data.rank == node->rank_snapshot &&
           entry->generation == node->generation_snapshot;
}

static StableEntry *d_peek_current_min(VersionD *cache)
{
    while (cache->heap.size > 0U) {
        LazyHeapNode *root = &cache->heap.nodes[0];
        if (lazy_is_current(root)) {
            return root->entry;
        }
        (void)lazy_pop_raw(&cache->heap, NULL);
        ++cache->stale_discards;
        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->lazy_stale_discards);
        }
    }
    return NULL;
}

static int d_push_current_version(VersionD *cache, StableEntry *entry)
{
    LazyHeapNode node;
    node.entry = entry;
    node.key_snapshot = entry->data.key;
    node.rank_snapshot = entry->data.rank;
    node.generation_snapshot = entry->generation;
    return lazy_push(&cache->heap, node);
}

static int d_init(VersionD *cache, size_t capacity)
{
    if (cache == NULL || !pool_init(&cache->pool, capacity)) return 0;
    hash_init(&cache->hash);
    memset(&cache->heap, 0, sizeof(cache->heap));
    memset(&cache->stats, 0, sizeof(cache->stats));
    cache->stale_discards = 0U;
    return 1;
}

static CacheEntry *d_get(VersionD *cache, CacheKey key, RankScenario scenario)
{
    StableEntry *resident;
    CacheEntry fetched;
    if (cache == NULL) return NULL;

    ++cache->stats.accesses;
    resident = (StableEntry *)hash_lookup(&cache->hash, key);
    if (resident != NULL) {
        Rank old_rank = resident->data.rank;
        Rank new_rank = apply_rank_scenario(old_rank, scenario);
        ++cache->stats.hits;
        resident->data.rank = new_rank;
        ++resident->generation;
        if (!d_push_current_version(cache, resident)) return NULL;
        stats_record_rank_change(&cache->stats, old_rank, new_rank);
        return &resident->data;
    }

    ++cache->stats.misses;
    ++cache->stats.db_reads;
    fetched = db_read_entry(key);

    if (cache->pool.size == cache->pool.capacity) {
        StableEntry *victim = d_peek_current_min(cache);
        LazyHeapNode popped;
        if (victim == NULL || !lazy_pop_raw(&cache->heap, &popped) ||
            popped.entry != victim || !hash_remove(&cache->hash, victim->data.key) ||
            !pool_release(&cache->pool, victim)) {
            return NULL;
        }
        ++cache->stats.evictions;
    }

    resident = pool_alloc(&cache->pool, fetched);
    if (resident == NULL || !hash_insert(&cache->hash, key, resident) ||
        !d_push_current_version(cache, resident)) {
        return NULL;
    }
    ++cache->stats.insertions;
    return &resident->data;
}

static int lazy_heap_order_valid(const LazyHeap *heap)
{
    size_t i;
    for (i = 0U; i < heap->size; ++i) {
        size_t left = 2U * i + 1U;
        size_t right = left + 1U;
        if (left < heap->size && lazy_node_less(&heap->nodes[left], &heap->nodes[i])) return 0;
        if (right < heap->size && lazy_node_less(&heap->nodes[right], &heap->nodes[i])) return 0;
    }
    return 1;
}

static int d_validate(VersionD *cache)
{
    size_t i;
    if (cache == NULL || !pool_validate(&cache->pool) || !hash_validate(&cache->hash) ||
        cache->hash.size != cache->pool.size || cache->heap.size > CMP_LAZY_HEAP_MAX ||
        !lazy_heap_order_valid(&cache->heap)) {
        return 0;
    }
    for (i = 0U; i < cache->pool.capacity; ++i) {
        StableEntry *entry = &cache->pool.entries[i];
        size_t j;
        int current_record_found = 0;
        if (!entry->active) continue;
        if (hash_lookup(&cache->hash, entry->data.key) != entry) return 0;
        for (j = 0U; j < cache->heap.size; ++j) {
            if (cache->heap.nodes[j].entry == entry && lazy_is_current(&cache->heap.nodes[j])) {
                current_record_found = 1;
                break;
            }
        }
        if (!current_record_found) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Version E — hash + indexed heap                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
    StableEntry *items[CMP_MAX_CAPACITY];
    size_t size;
} IndexedHeap;

typedef struct {
    StablePool pool;
    HashTable hash;
    IndexedHeap heap;
    CacheStats stats;
} VersionE;

static void indexed_swap(IndexedHeap *heap, size_t a, size_t b)
{
    StableEntry *tmp = heap->items[a];
    if (g_v6_instrumentation != NULL) {
        v6_count(&g_v6_instrumentation->indexed_heap_swaps);
    }
    heap->items[a] = heap->items[b];
    heap->items[b] = tmp;
    heap->items[a]->heap_index = a;
    heap->items[b]->heap_index = b;
}

static void indexed_sift_up(IndexedHeap *heap, size_t i)
{
    while (i > 0U) {
        size_t parent = (i - 1U) / 2U;
        if (g_v6_instrumentation != NULL) {
            v6_count(&g_v6_instrumentation->indexed_heap_comparisons);
        }
        if (!entry_less(&heap->items[i]->data, &heap->items[parent]->data)) break;
        indexed_swap(heap, i, parent);
        i = parent;
    }
}

static void indexed_sift_down(IndexedHeap *heap, size_t i)
{
    while (1) {
        size_t left = 2U * i + 1U;
        size_t right = left + 1U;
        size_t best = i;
        if (left < heap->size) {
            if (g_v6_instrumentation != NULL) {
                v6_count(&g_v6_instrumentation->indexed_heap_comparisons);
            }
            if (entry_less(&heap->items[left]->data, &heap->items[best]->data)) best = left;
        }
        if (right < heap->size) {
            if (g_v6_instrumentation != NULL) {
                v6_count(&g_v6_instrumentation->indexed_heap_comparisons);
            }
            if (entry_less(&heap->items[right]->data, &heap->items[best]->data)) best = right;
        }
        if (best == i) break;
        indexed_swap(heap, i, best);
        i = best;
    }
}

static int indexed_push(IndexedHeap *heap, StableEntry *entry)
{
    size_t i;
    if (heap == NULL || entry == NULL || heap->size >= CMP_MAX_CAPACITY) return 0;
    i = heap->size++;
    heap->items[i] = entry;
    entry->heap_index = i;
    indexed_sift_up(heap, i);
    return 1;
}

static StableEntry *indexed_pop_min(IndexedHeap *heap)
{
    StableEntry *removed;
    if (heap == NULL || heap->size == 0U) return NULL;
    removed = heap->items[0];
    --heap->size;
    if (heap->size > 0U) {
        heap->items[0] = heap->items[heap->size];
        heap->items[0]->heap_index = 0U;
        indexed_sift_down(heap, 0U);
    }
    removed->heap_index = CMP_HEAP_INDEX_NONE;
    return removed;
}

static int indexed_update_rank(IndexedHeap *heap, StableEntry *entry, Rank new_rank)
{
    Rank old_rank;
    size_t index;
    if (heap == NULL || entry == NULL || entry->heap_index >= heap->size ||
        heap->items[entry->heap_index] != entry) {
        return 0;
    }
    old_rank = entry->data.rank;
    index = entry->heap_index;
    entry->data.rank = new_rank;
    if (new_rank < old_rank) {
        indexed_sift_up(heap, index);
    } else if (new_rank > old_rank) {
        indexed_sift_down(heap, index);
    }
    return 1;
}

static int e_init(VersionE *cache, size_t capacity)
{
    if (cache == NULL || !pool_init(&cache->pool, capacity)) return 0;
    hash_init(&cache->hash);
    memset(&cache->heap, 0, sizeof(cache->heap));
    memset(&cache->stats, 0, sizeof(cache->stats));
    return 1;
}

static CacheEntry *e_get(VersionE *cache, CacheKey key, RankScenario scenario)
{
    StableEntry *resident;
    CacheEntry fetched;
    if (cache == NULL) return NULL;

    ++cache->stats.accesses;
    resident = (StableEntry *)hash_lookup(&cache->hash, key);
    if (resident != NULL) {
        Rank old_rank = resident->data.rank;
        Rank new_rank = apply_rank_scenario(old_rank, scenario);
        ++cache->stats.hits;
        if (!indexed_update_rank(&cache->heap, resident, new_rank)) return NULL;
        stats_record_rank_change(&cache->stats, old_rank, new_rank);
        return &resident->data;
    }

    ++cache->stats.misses;
    ++cache->stats.db_reads;
    fetched = db_read_entry(key);

    if (cache->pool.size == cache->pool.capacity) {
        StableEntry *victim = indexed_pop_min(&cache->heap);
        if (victim == NULL || !hash_remove(&cache->hash, victim->data.key) ||
            !pool_release(&cache->pool, victim)) {
            return NULL;
        }
        ++cache->stats.evictions;
    }

    resident = pool_alloc(&cache->pool, fetched);
    if (resident == NULL || !hash_insert(&cache->hash, key, resident) ||
        !indexed_push(&cache->heap, resident)) {
        return NULL;
    }
    ++cache->stats.insertions;
    return &resident->data;
}

static int indexed_heap_validate(const IndexedHeap *heap)
{
    size_t i;
    if (heap == NULL || heap->size > CMP_MAX_CAPACITY) return 0;
    for (i = 0U; i < heap->size; ++i) {
        size_t left = 2U * i + 1U;
        size_t right = left + 1U;
        if (heap->items[i] == NULL || heap->items[i]->heap_index != i) return 0;
        if (left < heap->size && entry_less(&heap->items[left]->data, &heap->items[i]->data)) return 0;
        if (right < heap->size && entry_less(&heap->items[right]->data, &heap->items[i]->data)) return 0;
    }
    return 1;
}

static int e_validate(const VersionE *cache)
{
    size_t i;
    if (cache == NULL || !pool_validate(&cache->pool) || !hash_validate(&cache->hash) ||
        !indexed_heap_validate(&cache->heap) || cache->hash.size != cache->pool.size ||
        cache->heap.size != cache->pool.size) {
        return 0;
    }
    for (i = 0U; i < cache->pool.capacity; ++i) {
        const StableEntry *entry = &cache->pool.entries[i];
        if (entry->active) {
            if (hash_lookup(&cache->hash, entry->data.key) != entry ||
                entry->heap_index >= cache->heap.size ||
                cache->heap.items[entry->heap_index] != entry) {
                return 0;
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Common comparison/validation harness                                      */
/* ------------------------------------------------------------------------- */

static int logical_entry_equal(const CacheEntry *a, const CacheEntry *b)
{
    if (a == NULL || b == NULL) return a == b;
    return a->key == b->key && a->value == b->value && a->rank == b->rank;
}

static CacheEntry *b_lookup_public(VersionB *cache, CacheKey key)
{
    StableEntry *e = (StableEntry *)hash_lookup(&cache->hash, key);
    return e == NULL ? NULL : &e->data;
}

static CacheEntry *c_lookup_public(VersionC *cache, CacheKey key)
{
    StableEntry *e = (StableEntry *)hash_lookup(&cache->hash, key);
    return e == NULL ? NULL : &e->data;
}

static CacheEntry *d_lookup_public(VersionD *cache, CacheKey key)
{
    StableEntry *e = (StableEntry *)hash_lookup(&cache->hash, key);
    return e == NULL ? NULL : &e->data;
}

static CacheEntry *e_lookup_public(VersionE *cache, CacheKey key)
{
    StableEntry *e = (StableEntry *)hash_lookup(&cache->hash, key);
    return e == NULL ? NULL : &e->data;
}

static uint64_t checksum_mix(uint64_t checksum, const CacheEntry *entry)
{
    checksum ^= (uint64_t)entry->key * UINT64_C(0x9e3779b97f4a7c15);
    checksum += (uint64_t)entry->value;
    checksum ^= (uint64_t)entry->rank + UINT64_C(0x517cc1b727220a95);
    return checksum;
}

static int check_all_logical_states(VersionA *a,
                                    VersionB *b,
                                    VersionC *c,
                                    VersionD *d,
                                    VersionE *e,
                                    CacheKey key_space)
{
    CacheKey key;
    for (key = 1ULL; key <= key_space; ++key) {
        CacheEntry *ea = a_lookup(a, key);
        CacheEntry *eb = b_lookup_public(b, key);
        CacheEntry *ec = c_lookup_public(c, key);
        CacheEntry *ed = d_lookup_public(d, key);
        CacheEntry *ee = e_lookup_public(e, key);
        if (!logical_entry_equal(ea, eb) || !logical_entry_equal(ea, ec) ||
            !logical_entry_equal(ea, ed) || !logical_entry_equal(ea, ee)) {
            return 0;
        }
    }
    return 1;
}

static void print_stats(const char *name, CacheStats stats)
{
    printf("%-10s accesses=%" PRIu64 " hits=%" PRIu64 " misses=%" PRIu64
           " evictions=%" PRIu64 " rank_updates=%" PRIu64 "\n",
           name,
           stats.accesses,
           stats.hits,
           stats.misses,
           stats.evictions,
           stats.rank_updates);
}

static int run_deterministic_equivalence_test(void)
{
    const size_t capacity = 8U;
    const CacheKey key_space = 17ULL;
    const size_t operations = 2000U;
    const uint64_t seed = UINT64_C(0x6a09e667f3bcc909);
    WorkloadGenerator generator;
    VersionA a;
    VersionB b;
    VersionC c;
    VersionD d;
    VersionE e;
    uint64_t checksum_a = 0U;
    uint64_t checksum_b = 0U;
    uint64_t checksum_c = 0U;
    uint64_t checksum_d = 0U;
    uint64_t checksum_e = 0U;
    size_t i;

    if (!workload_init(&generator, seed, key_space) ||
        !a_init(&a, capacity) || !b_init(&b, capacity) ||
        !c_init(&c, capacity) || !d_init(&d, capacity) || !e_init(&e, capacity)) {
        return 0;
    }

    for (i = 0U; i < operations; ++i) {
        WorkloadOp op;
        CacheEntry *ea;
        CacheEntry *eb;
        CacheEntry *ec;
        CacheEntry *ed;
        CacheEntry *ee;

        if (!workload_next(&generator, &op)) return 0;
        ea = a_get(&a, op.key, op.scenario);
        eb = b_get(&b, op.key, op.scenario);
        ec = c_get(&c, op.key, op.scenario);
        ed = d_get(&d, op.key, op.scenario);
        ee = e_get(&e, op.key, op.scenario);
        if (ea == NULL || eb == NULL || ec == NULL || ed == NULL || ee == NULL) return 0;
        checksum_a = checksum_mix(checksum_a, ea);
        checksum_b = checksum_mix(checksum_b, eb);
        checksum_c = checksum_mix(checksum_c, ec);
        checksum_d = checksum_mix(checksum_d, ed);
        checksum_e = checksum_mix(checksum_e, ee);
    }

    if (!(checksum_a == checksum_b && checksum_a == checksum_c &&
          checksum_a == checksum_d && checksum_a == checksum_e)) {
        return 0;
    }

    if (!stats_equal(a.stats, b.stats) || !stats_equal(a.stats, c.stats) ||
        !stats_equal(a.stats, d.stats) || !stats_equal(a.stats, e.stats)) {
        return 0;
    }

    if (!check_all_logical_states(&a, &b, &c, &d, &e, key_space)) {
        return 0;
    }

    if (!a_validate(&a) || !b_validate(&b) || !c_validate(&c) ||
        !d_validate(&d) || !e_validate(&e)) {
        return 0;
    }

    printf("\nDeterministic equivalence workload: PASS\n");
    printf("  capacity=%zu key_space=%llu operations=%zu checksum=%" PRIu64 "\n",
           capacity, key_space, operations, checksum_a);
    print_stats("Version A", a.stats);
    print_stats("Version B", b.stats);
    print_stats("Version C", c.stats);
    print_stats("Version D", d.stats);
    print_stats("Version E", e.stats);
    printf("  Version D resident entries=%zu lazy heap records=%zu stale_discards=%" PRIu64 "\n",
           d.pool.size, d.heap.size, d.stale_discards);
    printf("  Version E resident entries=%zu indexed heap records=%zu\n",
           e.pool.size, e.heap.size);

    avl_free_all(c.root);
    return 1;
}

static int run_targeted_minimum_test(void)
{
    VersionA a;
    VersionB b;
    VersionC c;
    VersionD d;
    VersionE e;
    const CacheKey keys[] = {1ULL, 2ULL, 3ULL, 4ULL};
    size_t i;

    if (!a_init(&a, 3U) || !b_init(&b, 3U) || !c_init(&c, 3U) ||
        !d_init(&d, 3U) || !e_init(&e, 3U)) {
        return 0;
    }

    for (i = 0U; i < 4U; ++i) {
        if (a_get(&a, keys[i], RANK_UNCHANGED) == NULL ||
            b_get(&b, keys[i], RANK_UNCHANGED) == NULL ||
            c_get(&c, keys[i], RANK_UNCHANGED) == NULL ||
            d_get(&d, keys[i], RANK_UNCHANGED) == NULL ||
            e_get(&e, keys[i], RANK_UNCHANGED) == NULL) {
            return 0;
        }
    }

    /* capacity 3, inserting key 4 evicts key 1 (rank 10). */
    if (a_lookup(&a, 1ULL) != NULL || b_lookup_public(&b, 1ULL) != NULL ||
        c_lookup_public(&c, 1ULL) != NULL || d_lookup_public(&d, 1ULL) != NULL ||
        e_lookup_public(&e, 1ULL) != NULL) {
        return 0;
    }

    if (!check_all_logical_states(&a, &b, &c, &d, &e, 4ULL) ||
        !a_validate(&a) || !b_validate(&b) || !c_validate(&c) ||
        !d_validate(&d) || !e_validate(&e)) {
        return 0;
    }

    avl_free_all(c.root);
    return 1;
}

static int run_lazy_heap_memory_demo(void)
{
    VersionD d;
    VersionE e;
    size_t i;

    if (!d_init(&d, 4U) || !e_init(&e, 4U)) return 0;
    for (i = 1U; i <= 4U; ++i) {
        if (d_get(&d, (CacheKey)i, RANK_UNCHANGED) == NULL ||
            e_get(&e, (CacheKey)i, RANK_UNCHANGED) == NULL) return 0;
    }

    for (i = 0U; i < 100U; ++i) {
        RankScenario s = (RankScenario)(i % 3U);
        if (d_get(&d, 2ULL, s) == NULL || e_get(&e, 2ULL, s) == NULL) return 0;
    }

    if (!(d.heap.size > d.pool.size && e.heap.size == e.pool.size)) return 0;
    if (!d_validate(&d) || !e_validate(&e)) return 0;

    printf("\nLazy-heap memory demonstration:\n");
    printf("  Version D residents=%zu heap records=%zu (stale versions retained)\n",
           d.pool.size, d.heap.size);
    printf("  Version E residents=%zu heap records=%zu (one record per resident)\n",
           e.pool.size, e.heap.size);
    return 1;
}

static void print_complexity_table(void)
{
    printf("\nVersion complexity summary\n");
    printf("+---------+----------------------+----------------------+----------------------+----------------------+\n");
    printf("| Version | Lookup               | Rank update          | Minimum              | Eviction             |\n");
    printf("+---------+----------------------+----------------------+----------------------+----------------------+\n");
    printf("| A       | O(N)                 | O(1)*                | O(N)                 | O(N)                 |\n");
    printf("| B       | expected O(1)        | O(1)*                | O(N)                 | O(N)                 |\n");
    printf("| C       | expected O(1)        | O(log N)             | O(log N)**           | O(log N)             |\n");
    printf("| D       | expected O(1)        | O(log M) append      | amortized cleanup    | amortized + O(log M) |\n");
    printf("| E       | expected O(1)        | O(log N)             | O(1)                 | O(log N)             |\n");
    printf("+---------+----------------------+----------------------+----------------------+----------------------+\n");
    printf("* after the resident is already located.\n");
    printf("** AVL minimum is leftmost traversal; storing a min pointer could make peek O(1).\n");
    printf("M = number of lazy-heap records, which can grow with rank updates.\n");
}


/* ------------------------------------------------------------------------- */
/* V6 validation + instrumentation + benchmark harness                       */
/* ------------------------------------------------------------------------- */

#define V6_CAPACITY 32U
#define V6_KEY_SPACE 65ULL
#define V6_VALIDATION_OPS 4000U
#define V6_INSTRUMENT_OPS 4000U
#define V6_WARMUP_OPS 1000U
#define V6_BENCH_OPS 10000U
#define V6_BENCH_REPETITIONS 5U
#define V6_SEED UINT64_C(0x510e527fade682d1)

typedef enum {
    V6_VERSION_A = 0,
    V6_VERSION_B = 1,
    V6_VERSION_C = 2,
    V6_VERSION_D = 3,
    V6_VERSION_E = 4,
    V6_VERSION_COUNT = 5
} V6VersionId;

static const char *v6_version_name(V6VersionId id)
{
    static const char *names[V6_VERSION_COUNT] = {
        "Version A", "Version B", "Version C", "Version D", "Version E"
    };
    return (id >= V6_VERSION_A && id < V6_VERSION_COUNT) ? names[id] : "Unknown";
}

static int v6_generate_workload(WorkloadOp *ops,
                                size_t count,
                                uint64_t seed,
                                CacheKey key_space)
{
    WorkloadGenerator generator;
    size_t i;

    if ((ops == NULL && count != 0U) ||
        !workload_init(&generator, seed, key_space)) {
        return 0;
    }

    for (i = 0U; i < count; ++i) {
        if (!workload_next(&generator, &ops[i])) {
            return 0;
        }
    }
    return 1;
}

static uint64_t v6_run_version(V6VersionId id,
                               const WorkloadOp *ops,
                               size_t count,
                               size_t capacity,
                               CacheStats *stats_out,
                               int validate_final,
                               V6Instrumentation *instrumentation,
                               size_t *aux_size_out,
                               uint64_t *stale_discards_out)
{
    uint64_t checksum = 0U;
    size_t i;

    if (ops == NULL || stats_out == NULL) {
        return 0U;
    }

    memset(stats_out, 0, sizeof(*stats_out));
    if (aux_size_out != NULL) *aux_size_out = 0U;
    if (stale_discards_out != NULL) *stale_discards_out = 0U;
    if (instrumentation != NULL) {
        memset(instrumentation, 0, sizeof(*instrumentation));
    }

    g_v6_instrumentation = instrumentation;

    switch (id) {
    case V6_VERSION_A: {
        VersionA cache;
        if (!a_init(&cache, capacity)) {
            g_v6_instrumentation = NULL;
            return 0U;
        }
        for (i = 0U; i < count; ++i) {
            CacheEntry *entry = a_get(&cache, ops[i].key, ops[i].scenario);
            if (entry == NULL) {
                g_v6_instrumentation = NULL;
                return 0U;
            }
            checksum = checksum_mix(checksum, entry);
        }
        g_v6_instrumentation = NULL;
        if (validate_final && !a_validate(&cache)) return 0U;
        *stats_out = cache.stats;
        if (aux_size_out != NULL) *aux_size_out = cache.size;
        return checksum;
    }

    case V6_VERSION_B: {
        VersionB cache;
        if (!b_init(&cache, capacity)) {
            g_v6_instrumentation = NULL;
            return 0U;
        }
        for (i = 0U; i < count; ++i) {
            CacheEntry *entry = b_get(&cache, ops[i].key, ops[i].scenario);
            if (entry == NULL) {
                g_v6_instrumentation = NULL;
                return 0U;
            }
            checksum = checksum_mix(checksum, entry);
        }
        g_v6_instrumentation = NULL;
        if (validate_final && !b_validate(&cache)) return 0U;
        *stats_out = cache.stats;
        if (aux_size_out != NULL) *aux_size_out = cache.pool.size;
        return checksum;
    }

    case V6_VERSION_C: {
        VersionC cache;
        int valid;
        if (!c_init(&cache, capacity)) {
            g_v6_instrumentation = NULL;
            return 0U;
        }
        for (i = 0U; i < count; ++i) {
            CacheEntry *entry = c_get(&cache, ops[i].key, ops[i].scenario);
            if (entry == NULL) {
                g_v6_instrumentation = NULL;
                avl_free_all(cache.root);
                return 0U;
            }
            checksum = checksum_mix(checksum, entry);
        }
        g_v6_instrumentation = NULL;
        valid = !validate_final || c_validate(&cache);
        *stats_out = cache.stats;
        if (aux_size_out != NULL) *aux_size_out = cache.pool.size;
        avl_free_all(cache.root);
        return valid ? checksum : 0U;
    }

    case V6_VERSION_D: {
        VersionD cache;
        if (!d_init(&cache, capacity)) {
            g_v6_instrumentation = NULL;
            return 0U;
        }
        for (i = 0U; i < count; ++i) {
            CacheEntry *entry = d_get(&cache, ops[i].key, ops[i].scenario);
            if (entry == NULL) {
                g_v6_instrumentation = NULL;
                return 0U;
            }
            checksum = checksum_mix(checksum, entry);
        }
        g_v6_instrumentation = NULL;
        if (validate_final && !d_validate(&cache)) return 0U;
        *stats_out = cache.stats;
        if (aux_size_out != NULL) *aux_size_out = cache.heap.size;
        if (stale_discards_out != NULL) *stale_discards_out = cache.stale_discards;
        return checksum;
    }

    case V6_VERSION_E: {
        VersionE cache;
        if (!e_init(&cache, capacity)) {
            g_v6_instrumentation = NULL;
            return 0U;
        }
        for (i = 0U; i < count; ++i) {
            CacheEntry *entry = e_get(&cache, ops[i].key, ops[i].scenario);
            if (entry == NULL) {
                g_v6_instrumentation = NULL;
                return 0U;
            }
            checksum = checksum_mix(checksum, entry);
        }
        g_v6_instrumentation = NULL;
        if (validate_final && !e_validate(&cache)) return 0U;
        *stats_out = cache.stats;
        if (aux_size_out != NULL) *aux_size_out = cache.heap.size;
        return checksum;
    }

    default:
        g_v6_instrumentation = NULL;
        return 0U;
    }
}

static int v6_validate_semantic_equivalence(void)
{
    WorkloadOp *ops;
    CacheStats stats[V6_VERSION_COUNT];
    uint64_t checksum[V6_VERSION_COUNT];
    size_t aux_size[V6_VERSION_COUNT];
    uint64_t stale[V6_VERSION_COUNT];
    size_t i;
    int passed = 1;

    ops = (WorkloadOp *)malloc(V6_VALIDATION_OPS * sizeof(*ops));
    if (ops == NULL) return 0;

    passed &= v6_generate_workload(
        ops, V6_VALIDATION_OPS, V6_SEED, V6_KEY_SPACE);

    for (i = 0U; passed && i < V6_VERSION_COUNT; ++i) {
        checksum[i] = v6_run_version(
            (V6VersionId)i,
            ops,
            V6_VALIDATION_OPS,
            V6_CAPACITY,
            &stats[i],
            1,
            NULL,
            &aux_size[i],
            &stale[i]);

        if (checksum[i] == 0U) {
            passed = 0;
        }
    }

    for (i = 1U; passed && i < V6_VERSION_COUNT; ++i) {
        if (checksum[i] != checksum[0] || !stats_equal(stats[i], stats[0])) {
            passed = 0;
        }
    }

    if (passed) {
        printf("\nV6 semantic validation: PASS\n");
        printf("  capacity=%u key_space=%llu operations=%u checksum=%" PRIu64 "\n",
               V6_CAPACITY,
               (unsigned long long)V6_KEY_SPACE,
               V6_VALIDATION_OPS,
               checksum[0]);
        print_stats("V6 oracle", stats[0]);
        printf("  Version D lazy heap records=%zu stale_discards=%" PRIu64 "\n",
               aux_size[V6_VERSION_D], stale[V6_VERSION_D]);
        printf("  Version E indexed heap records=%zu\n",
               aux_size[V6_VERSION_E]);
    }

    free(ops);
    return passed;
}

static void v6_print_instrumentation_row(const char *name,
                                         const V6Instrumentation *m)
{
    printf("%-9s lookup_cmp=%" PRIu64
           " min_cmp=%" PRIu64
           " hash_probes=%" PRIu64
           " avl_cmp=%" PRIu64
           " avl_rot=%" PRIu64
           " lazy_cmp=%" PRIu64
           " lazy_swap=%" PRIu64
           " lazy_push=%" PRIu64
           " lazy_pop=%" PRIu64
           " stale=%" PRIu64
           " idx_cmp=%" PRIu64
           " idx_swap=%" PRIu64 "\n",
           name,
           m->linear_lookup_comparisons,
           m->linear_min_comparisons,
           m->hash_slot_probes,
           m->avl_key_comparisons,
           m->avl_rotations,
           m->lazy_heap_comparisons,
           m->lazy_heap_swaps,
           m->lazy_heap_pushes,
           m->lazy_heap_pops,
           m->lazy_stale_discards,
           m->indexed_heap_comparisons,
           m->indexed_heap_swaps);
}

static int v6_run_instrumentation_pass(void)
{
    WorkloadOp *ops;
    V6Instrumentation metrics[V6_VERSION_COUNT];
    CacheStats stats[V6_VERSION_COUNT];
    uint64_t checksum[V6_VERSION_COUNT];
    size_t aux_size[V6_VERSION_COUNT];
    uint64_t stale[V6_VERSION_COUNT];
    size_t i;
    int passed = 1;

    ops = (WorkloadOp *)malloc(V6_INSTRUMENT_OPS * sizeof(*ops));
    if (ops == NULL) return 0;

    passed &= v6_generate_workload(
        ops, V6_INSTRUMENT_OPS, V6_SEED, V6_KEY_SPACE);

    for (i = 0U; passed && i < V6_VERSION_COUNT; ++i) {
        checksum[i] = v6_run_version(
            (V6VersionId)i,
            ops,
            V6_INSTRUMENT_OPS,
            V6_CAPACITY,
            &stats[i],
            1,
            &metrics[i],
            &aux_size[i],
            &stale[i]);
        if (checksum[i] == 0U) passed = 0;
    }

    for (i = 1U; passed && i < V6_VERSION_COUNT; ++i) {
        if (checksum[i] != checksum[0] || !stats_equal(stats[i], stats[0])) {
            passed = 0;
        }
    }

    if (passed) {
        passed &= metrics[V6_VERSION_A].linear_lookup_comparisons > 0U;
        passed &= metrics[V6_VERSION_A].linear_min_comparisons > 0U;
        passed &= metrics[V6_VERSION_A].hash_slot_probes == 0U;

        passed &= metrics[V6_VERSION_B].hash_slot_probes > 0U;
        passed &= metrics[V6_VERSION_B].linear_min_comparisons > 0U;

        passed &= metrics[V6_VERSION_C].hash_slot_probes > 0U;
        passed &= metrics[V6_VERSION_C].avl_key_comparisons > 0U;
        passed &= metrics[V6_VERSION_C].avl_rotations > 0U;

        passed &= metrics[V6_VERSION_D].hash_slot_probes > 0U;
        passed &= metrics[V6_VERSION_D].lazy_heap_comparisons > 0U;
        passed &= metrics[V6_VERSION_D].lazy_heap_pushes > 0U;
        passed &= metrics[V6_VERSION_D].lazy_stale_discards > 0U;

        passed &= metrics[V6_VERSION_E].hash_slot_probes > 0U;
        passed &= metrics[V6_VERSION_E].indexed_heap_comparisons > 0U;
        passed &= metrics[V6_VERSION_E].indexed_heap_swaps > 0U;
    }

    if (passed) {
        printf("\nV6 instrumentation counters\n");
        for (i = 0U; i < V6_VERSION_COUNT; ++i) {
            v6_print_instrumentation_row(v6_version_name((V6VersionId)i),
                                         &metrics[i]);
        }
        printf("  Version D final lazy heap records=%zu\n",
               aux_size[V6_VERSION_D]);
        printf("  Version E final indexed heap records=%zu\n",
               aux_size[V6_VERSION_E]);
    }

    free(ops);
    return passed;
}

static int v6_benchmark_one(V6VersionId id,
                            const WorkloadOp *ops,
                            size_t count,
                            V6Timing *timing)
{
    CacheStats stats;
    struct timespec start;
    struct timespec end;
    uint64_t checksum;
    double seconds;

    if (ops == NULL || timing == NULL || count == 0U) {
        return 0;
    }

    g_v6_instrumentation = NULL;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return 0;

    checksum = v6_run_version(
        id, ops, count, V6_CAPACITY, &stats, 0, NULL, NULL, NULL);

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0 || checksum == 0U) return 0;

    seconds = v6_elapsed_seconds(start, end);
    if (seconds <= 0.0) return 0;

    timing->seconds = seconds;
    timing->ns_per_op = seconds * 1000000000.0 / (double)count;
    timing->checksum = checksum;
    return 1;
}

static void v6_sort_doubles(double *values, size_t count)
{
    size_t i;
    size_t j;
    for (i = 0U; i < count; ++i) {
        for (j = i + 1U; j < count; ++j) {
            if (values[j] < values[i]) {
                double tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }
}

static double v6_median(double *values, size_t count)
{
    v6_sort_doubles(values, count);
    return values[count / 2U];
}

static int v6_run_benchmark(void)
{
    WorkloadOp *warmup;
    WorkloadOp *measured;
    double samples[V6_VERSION_COUNT][V6_BENCH_REPETITIONS];
    uint64_t reference_checksum[V6_BENCH_REPETITIONS] = {0U};
    size_t rep;
    size_t version;
    int passed = 1;

    warmup = (WorkloadOp *)malloc(V6_WARMUP_OPS * sizeof(*warmup));
    measured = (WorkloadOp *)malloc(V6_BENCH_OPS * sizeof(*measured));
    if (warmup == NULL || measured == NULL) {
        free(warmup);
        free(measured);
        return 0;
    }

    passed &= v6_generate_workload(
        warmup, V6_WARMUP_OPS, V6_SEED ^ UINT64_C(0x1111111111111111),
        V6_KEY_SPACE);
    passed &= v6_generate_workload(
        measured, V6_BENCH_OPS, V6_SEED ^ UINT64_C(0x2222222222222222),
        V6_KEY_SPACE);

    /*
     * Warm each implementation independently outside the measured interval.
     * The warmup cache is intentionally discarded.
     */
    for (version = 0U; passed && version < V6_VERSION_COUNT; ++version) {
        CacheStats warm_stats;
        if (v6_run_version((V6VersionId)version,
                           warmup,
                           V6_WARMUP_OPS,
                           V6_CAPACITY,
                           &warm_stats,
                           0,
                           NULL,
                           NULL,
                           NULL) == 0U) {
            passed = 0;
        }
    }

    for (rep = 0U; passed && rep < V6_BENCH_REPETITIONS; ++rep) {
        /*
         * Rotate the first implementation each repetition to avoid always
         * giving Version A the same scheduling/cache position.
         */
        for (version = 0U; version < V6_VERSION_COUNT; ++version) {
            size_t rotated = (version + rep) % V6_VERSION_COUNT;
            V6Timing timing;

            if (!v6_benchmark_one((V6VersionId)rotated,
                                  measured,
                                  V6_BENCH_OPS,
                                  &timing)) {
                passed = 0;
                break;
            }

            samples[rotated][rep] = timing.ns_per_op;

            if (rotated == V6_VERSION_A) {
                reference_checksum[rep] = timing.checksum;
            } else if (reference_checksum[rep] != 0U &&
                       timing.checksum != reference_checksum[rep]) {
                passed = 0;
                break;
            }
        }

        /*
         * Version A may execute later in the rotation. Verify the whole row
         * once all five samples have completed.
         */
        if (passed) {
            uint64_t oracle = 0U;
            for (version = 0U; version < V6_VERSION_COUNT; ++version) {
                CacheStats stats;
                uint64_t checksum = v6_run_version(
                    (V6VersionId)version,
                    measured,
                    V6_BENCH_OPS,
                    V6_CAPACITY,
                    &stats,
                    1,
                    NULL,
                    NULL,
                    NULL);
                if (checksum == 0U) {
                    passed = 0;
                    break;
                }
                if (version == 0U) oracle = checksum;
                if (checksum != oracle) {
                    passed = 0;
                    break;
                }
            }
            reference_checksum[rep] = oracle;
        }
    }

    if (passed) {
        printf("\nV6 optimized-style benchmark (instrumentation disabled)\n");
        printf("  capacity=%u key_space=%llu warmup=%u measured=%u repetitions=%u\n",
               V6_CAPACITY,
               (unsigned long long)V6_KEY_SPACE,
               V6_WARMUP_OPS,
               V6_BENCH_OPS,
               V6_BENCH_REPETITIONS);
        for (version = 0U; version < V6_VERSION_COUNT; ++version) {
            double copy[V6_BENCH_REPETITIONS];
            double median;
            for (rep = 0U; rep < V6_BENCH_REPETITIONS; ++rep) {
                copy[rep] = samples[version][rep];
            }
            median = v6_median(copy, V6_BENCH_REPETITIONS);
            printf("  %-9s median=%9.2f ns/op samples=",
                   v6_version_name((V6VersionId)version), median);
            for (rep = 0U; rep < V6_BENCH_REPETITIONS; ++rep) {
                printf("%s%.2f", rep == 0U ? "" : ",", samples[version][rep]);
            }
            printf("\n");
        }
    }

    free(warmup);
    free(measured);
    return passed;
}

static int run_v6_validation_instrumentation_benchmark(void)
{
    int passed = 1;

    printf("\n=== V6 — Validation + instrumentation + benchmark ===\n");

    passed &= v6_validate_semantic_equivalence();
    printf("V6 cross-version validation: %s\n", passed ? "PASS" : "FAIL");

    if (passed) {
        passed &= v6_run_instrumentation_pass();
        printf("V6 instrumentation validation: %s\n", passed ? "PASS" : "FAIL");
    }

    if (passed) {
        passed &= v6_run_benchmark();
        printf("V6 benchmark validation: %s\n", passed ? "PASS" : "FAIL");
    }

    printf("V6 validation + instrumentation + benchmark: %s\n",
           passed ? "PASS" : "FAIL");
    return passed;
}

int main(void)
{
    int passed = 1;

    printf("Stage 28 — Alternative ranked-cache implementations\n");
    print_complexity_table();

    passed &= run_targeted_minimum_test();
    printf("\nTargeted minimum/eviction equivalence: %s\n", passed ? "PASS" : "FAIL");

    if (passed) {
        passed &= run_lazy_heap_memory_demo();
        printf("Lazy-heap memory trade-off validation: %s\n", passed ? "PASS" : "FAIL");
    }

    if (passed) {
        passed &= run_deterministic_equivalence_test();
    }

    if (passed) {
        passed &= run_v6_validation_instrumentation_benchmark();
    }

    printf("\nStage 28 alternative-implementation validation: %s\n",
           passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
