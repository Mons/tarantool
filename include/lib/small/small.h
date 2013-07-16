#ifndef INCLUDES_TARANTOOL_SMALL_SMALL_H
#define INCLUDES_TARANTOOL_SMALL_SMALL_H
/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <stdint.h>
#include "lib/small/mempool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Small object allocator.
 *
 * The allocator consists of a collection of mempools.
 *
 * There are two containers of pools:
 *
 * pools for objects of size 8-500 bytes are stored in an array,
 * where pool->objsize of each array member is a multiple of 8-16
 * (value defined in STEP_SIZE constant). These are
 * "stepped" pools, since pool->objsize of each next pool in the
 * array differs from the  previous size by a fixed step size.
 *
 * For example, there is a pool for size range 16-32,
 * another one for 32-48, 48-64, etc. This makes the look up
 * procedure for small allocations just a matter of getting an
 * array index via a bit shift. All stepped pools are initialized
 * when an instance of small_alloc is created.
 *
 * Objects of size beyond the stepped pools range (the upper limit
 * is usually around 300 bytes), are stored in pools with a size
 * which is a multiple of alloc_factor. alloc_factor is itself
 * a configuration constant in the range (1.0, 2.0]. I.e. imagine
 * alloc_factor is 1.1, then there are pools for objects of size
 * 300-330, 330-363, and so on. These pools are created upon first
 * allocation within given range, and stored in a red-black tree.
 *
 * Initially this red-black tree contains only a pool for
 * alloc->object_max.
 * When a request for a new allocation of sz bytes arrives
 * and it can not be satisfied from a stepped pool,
 * a search for a nearest factored pool is made in the tree.
 *
 * If, for the nearest found factored pool:
 *
 * pool->objsize > sz * alloc_factor,
 *
 * (i.e. pool object size is too big) a new factored pool is
 * created and inserted into the tree.
 *
 * This way the tree only contains factored pools for sizes
 * which are actually used by the server, and can be kept
 * small.
 */

/** Basic constants of small object allocator. */
enum {
	/** How many stepped pools there is. */
	STEP_POOL_MAX = 32,
	/** How many factored pools there can be. */
	FACTOR_POOL_MAX = 256,
};

/**
 * A mempool to store objects sized within one multiple of
 * alloc_factor. Is a member of the red-black tree which
 * contains all such pools.
 *
 * Example: let's assume alloc_factor is 1.1. There will be an
 * instance of factor_pool for objects of size from 300 to 330,
 * from 330 to 363, and so on.
 */
struct factor_pool
{
	/** rb_tree entry */
	rb_node(struct factor_pool) node;
	/** the pool itself. */
	struct mempool pool;
	/**
	 * Objects starting from this size and up to
	 * pool->objsize are stored in this factored
	 * pool.
	 */
	size_t objsize_min;
	/** next free factor pool in the cache. */
	struct factor_pool *next;
};

typedef rb_tree(struct factor_pool) factor_tree_t;

/** A slab allocator for a wide range of object sizes. */
struct small_alloc {
	uint32_t step_pool_objsize_max;
	/**
	 * All slabs in all pools must be of the same order,
	 * otherwise small_free() has no way to derive from
	 * pointer its slab and then the pool.
	 */
	/**
	 * An array of "stepped" pools, pool->objsize of adjacent
	 * pools differ by a fixed size (step).
	 */
	struct mempool step_pools[STEP_POOL_MAX];
	/** A cache for nodes in the factor_pools tree. */
	struct factor_pool factor_pool_cache[FACTOR_POOL_MAX];
	/** First free element in factor_pool_cache. */
	struct factor_pool *factor_pool_next;
	/**
	 * A red-black tree with "factored" pools, i.e.
	 * each pool differs from its neighbor by a factor.
	 */
	factor_tree_t factor_pools;
	struct slab_cache *cache;
	/**
	 * The factor used for factored pools. Must be > 1.
	 * Is provided during initialization.
	 */
	float factor;
	/** All slabs in all mempools have the same order. */
	uint8_t slab_order;
};

/** Initialize a small memory allocator. */
void
small_alloc_create(struct small_alloc *alloc, struct slab_cache *cache,
		   uint32_t objsize_min, uint32_t objsize_max,
		   float alloc_factor);

/** Destroy the allocator and all allocated memory. */
void
small_alloc_destroy(struct small_alloc *alloc);

/** Allocate a piece of memory in the small allocator.
 *
 * @retval NULL   the requested size is beyond objsize_max
 *                or out of memory
 */
void *
smalloc_nothrow(struct small_alloc *alloc, size_t size);

/** Free memory chunk allocated by the small allocator. */
void
smfree(struct small_alloc *alloc, void *ptr);

/**
 * @brief Return an unique index associated with a chunk allocated
 * by the allocator.
 *
 * This index space is more dense than the pointers space,
 * especially in the least significant bits.  This number is
 * needed because some types of box's indexes (e.g. BITSET) have
 * better performance then they operate on sequential offsets
 * (i.e. dense space) instead of memory pointers (sparse space).
 *
 * The calculation is based on SLAB number and the position of an
 * item within it. Current implementation only guarantees that
 * adjacent chunks from one SLAB will have consecutive indexes.
 * That is, if two chunks were sequentially allocated from one
 * chunk they will have sequential ids. If a second chunk was
 * allocated from another SLAB thеn the difference between indexes
 * may be more than one.
 *
 * @param ptr pointer to memory allocated in small_alloc
 * @return unique index
 */
size_t
small_ptr_compress(struct small_alloc *alloc, void *ptr);

void *
small_ptr_decompress(struct small_alloc *alloc, size_t val);

typedef void (*mempool_stats_cb)(void *cb_ctx,
				 struct mempool_stats *stats);

void
small_stats(struct small_alloc *alloc,
	    struct small_stats *totals,
	    mempool_stats_cb cb, void *cb_ctx);

#ifdef __cplusplus
#include "exception.h"

static inline void *
smalloc(struct small_alloc *alloc, size_t size)
{
	void *ptr = smalloc_nothrow(alloc, size);
	if (ptr == NULL)
		tnt_raise(LoggedError, ER_MEMORY_ISSUE,
			  size, "small object allocator", "new slab");
	return ptr;
}

static inline void *
smalloc0(struct small_alloc *alloc, size_t size)
{
	return memset(smalloc(alloc, size), 0, size);
}

} /* extern "C" */
#endif

#endif /* INCLUDES_TARANTOOL_SMALL_SMALL_H */
