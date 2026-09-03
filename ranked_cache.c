/* rancked_cache.c
 *
 * 1. Approach notes:
 *
 * While writing the program, at every stage we should be able to explain: 
 * - what requirement I am solving, 
 * - what data structure I need, 
 * - what operation I am testing, 
 * - what complexity I currently have, and 
 * - what I will optimize next.
 *
 * 1. REQUIRMENTS:(Refers to Problem statement)
 * We need a cache with maximum capacity K.
 *
 * Each cached entry has:
 *    key
 *    value
 *    rank
 *
 * Operations:
 *    lookup(key)
 *
 * On hit:
 *    return cached entry
 *
 * On miss:
 *    fetch entry from DB
 *    calculate its rank
 *    insert it
 *
 * If cache is full:
 *    evict the entry with the minimum rank
 *
 * Part 1:
 *    rank never changes while cached
 *
 * Part 2:
 *    rank may change after every lookup
 *
 * 3. IDENTIFY THE IMPORTANT OPERATIONS:(This is already most of the algo-design work)
 *    - Find entry by key
 *    - Insert entry
 *    - Find the lowest-ranked entry
 *    - Remove the lowest-ranked entry
 *    - Change an arbitary entry's rank
 *
 * 4. Possible INPUT/OUTPUT's:
 *    
 *    Before defining the data structure, we need a simple layout possible of inputs/
 *    outputs 
 *
 *    Suppose:
 *      capacity = 3
 *
 *    Database:
 *      Key             Value               Rank
 *      1               100                 50
 *      2               200                 20
 *      3               300                 80
 *      4               400                 70
 *
 *    Operations:(on key)
 *      GET 1
 *      GET 2 
 *      GET 3
 *      GET 1
 *      GET 4
 *
 *    Expected reasoning
 *      GET 1
 *      MISS
 *      cache = {1:50}  
 *
 *      GET 2 
 *      MISS
 *      cache = {1:50, 2:20}}  
 *
 *      GET 3
 *      MISS
 *      cache = {1:50, 2:20, 3:80}  
 *
 *      GET 1
 *      HIT
 *
 *      GET 4
 *      Miss
 *      cache full ( because we define capacity = 3 )
 *
 *      minimum rank = key 2, rank 20
 *      evict key 2  
 *
 *      cache = {1:50, 3:80, 4:70}
 *
 * Let's call this 1st manual oracle. we need to test this works before we start with
 * optimization.
 *
 * Build command: gcc -Wall -Wextra -Wpedantic -std=c11 ranked_cache.c -o ranked_cache0
 * 
 *
 * STAGE 0 - write only the data model and prints expected Key, Value, and Rank r
 *           representation of one cache entry
 *
 *	     Note: In stage 0, cache logic is not introduced.
 *	     $ ./ranked_cache0
 *	         key=1 value=100 rank=50
 *
 *	    In the next stage 1, we shall introduce and implement the database abstraction     
 */

/* stage 0 - writing the basic data model */
#include<stdio.h>

typedef unsigned long long CacheKey;
typedef long long Rank;

typedef struct {
	CacheKey key;
	unsigned long long value;
	Rank rank;
} CacheEntry;

int main(void)
{
    CacheEntry e;

    e.key = 1;
    e.value = 100;
    e.rank = 50;

    printf("key=%llu value=%llu rank=%lld\n", e.key, e.value, e.rank);
    
    return 0;    

}	







