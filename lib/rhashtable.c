/*
 * Resizable, Scalable, Concurrent Hash Table
 *
 * Copyright (c) 2015 Herbert Xu <herbert@gondor.apana.org.au>
 * Copyright (c) 2014-2015 Thomas Graf <tgraf@suug.ch>
 * Copyright (c) 2008-2014 Patrick McHardy <kaber@trash.net>
 *
 * Code partially derived from nft_hash
 * Rewritten with rehash code from br_multicast plus single list
 * pointer as suggested by Josh Triplett
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/log2.h>
#include <linux/sched.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/rhashtable.h>
#include <linux/err.h>
#include <linux/export.h>

#define HASH_DEFAULT_SIZE	64UL
#define HASH_MIN_SIZE		4U
#define BUCKET_LOCKS_PER_CPU	32UL

union nested_table {
	union nested_table __rcu *table;
	struct rhash_head __rcu *bucket;
};

static u32 head_hashfn(struct rhashtable *ht,
		       const struct bucket_table *tbl,
		       const struct rhash_head *he)
{
	return rht_head_hashfn(ht, tbl, he, ht->p);
}

#ifdef CONFIG_PROVE_LOCKING
#define ASSERT_RHT_MUTEX(HT) BUG_ON(!lockdep_rht_mutex_is_held(HT))

int lockdep_rht_mutex_is_held(struct rhashtable *ht)
{
	return (debug_locks) ? lockdep_is_held(&ht->mutex) : 1;
}
EXPORT_SYMBOL_GPL(lockdep_rht_mutex_is_held);

int lockdep_rht_bucket_is_held(const struct bucket_table *tbl, u32 hash)
{
	spinlock_t *lock = rht_bucket_lock(tbl, hash);

	return (debug_locks) ? lockdep_is_held(lock) : 1;
}
EXPORT_SYMBOL_GPL(lockdep_rht_bucket_is_held);
#else
#define ASSERT_RHT_MUTEX(HT)
#endif


static int alloc_bucket_locks(struct rhashtable *ht, struct bucket_table *tbl,
			      gfp_t gfp)
{
	unsigned int i, size;
#if defined(CONFIG_PROVE_LOCKING)
	unsigned int nr_pcpus = 2;
#else
	unsigned int nr_pcpus = num_possible_cpus();
#endif

	nr_pcpus = min_t(unsigned int, nr_pcpus, 64UL);
	size = roundup_pow_of_two(nr_pcpus * ht->p.locks_mul);

	/* Never allocate more than 0.5 locks per bucket */
	size = min_t(unsigned int, size, tbl->size >> 1);

	if (tbl->nest)
		size = min(size, 1U << tbl->nest);

	if (sizeof(spinlock_t) != 0) {
		if (gfpflags_allow_blocking(gfp))
			tbl->locks = kvmalloc(size * sizeof(spinlock_t), gfp);
		else
			tbl->locks = kmalloc_array(size, sizeof(spinlock_t),
						   gfp);
		if (!tbl->locks)
			return -ENOMEM;
		for (i = 0; i < size; i++)
			spin_lock_init(&tbl->locks[i]);
	}
	tbl->locks_mask = size - 1;

	return 0;
}

static void nested_table_free(union nested_table *ntbl, unsigned int size)
{
	const unsigned int shift = PAGE_SHIFT - ilog2(sizeof(void *));
	const unsigned int len = 1 << shift;
	unsigned int i;

	ntbl = rcu_dereference_raw(ntbl->table);
	if (!ntbl)
		return;

	if (size > len) {
		size >>= shift;
		for (i = 0; i < len; i++)
			nested_table_free(ntbl + i, size);
	}

	kfree(ntbl);
}

static void nested_bucket_table_free(const struct bucket_table *tbl)
{
	unsigned int size = tbl->size >> tbl->nest;
	unsigned int len = 1 << tbl->nest;
	union nested_table *ntbl;
	unsigned int i;

	ntbl = (union nested_table *)rcu_dereference_raw(tbl->buckets[0]);

	for (i = 0; i < len; i++)
		nested_table_free(ntbl + i, size);

	kfree(ntbl);
}

static void bucket_table_free(const struct bucket_table *tbl)
{
	if (tbl->nest)
		nested_bucket_table_free(tbl);

	kvfree(tbl->locks);
	kvfree(tbl);
}

static void bucket_table_free_rcu(struct rcu_head *head)
{
	bucket_table_free(container_of(head, struct bucket_table, rcu));
}

static union nested_table *nested_table_alloc(struct rhashtable *ht,
					      union nested_table __rcu **prev,
					      unsigned int shifted,
					      unsigned int nhash)
{
	union nested_table *ntbl;
	int i;

	ntbl = rcu_dereference(*prev);
	if (ntbl)
		return ntbl;

	ntbl = kzalloc(PAGE_SIZE, GFP_ATOMIC);

	if (ntbl && shifted) {
		for (i = 0; i < PAGE_SIZE / sizeof(ntbl[0].bucket); i++)
			INIT_RHT_NULLS_HEAD(ntbl[i].bucket, ht,
					    (i << shifted) | nhash);
	}

	rcu_assign_pointer(*prev, ntbl);

	return ntbl;
}

static struct bucket_table *nested_bucket_table_alloc(struct rhashtable *ht,
						      size_t nbuckets,
						      gfp_t gfp)
{
	const unsigned int shift = PAGE_SHIFT - ilog2(sizeof(void *));
	struct bucket_table *tbl;
	size_t size;

	if (nbuckets < (1 << (shift + 1)))
		return NULL;

	size = sizeof(*tbl) + sizeof(tbl->buckets[0]);

	tbl = kzalloc(size, gfp);
	if (!tbl)
		return NULL;

	if (!nested_table_alloc(ht, (union nested_table __rcu **)tbl->buckets,
				0, 0)) {
		kfree(tbl);
		return NULL;
	}

	tbl->nest = (ilog2(nbuckets) - 1) % shift + 1;

	return tbl;
}

static struct bucket_table *bucket_table_alloc(struct rhashtable *ht,
					       size_t nbuckets,
					       gfp_t gfp)
{
	struct bucket_table *tbl = NULL;
	size_t size;
	int i;

	size = sizeof(*tbl) + nbuckets * sizeof(tbl->buckets[0]);
	if (gfp != GFP_KERNEL)
		tbl = kzalloc(size, gfp | __GFP_NOWARN | __GFP_NORETRY);
	else
		tbl = kvzalloc(size, gfp);

	size = nbuckets;

	if (tbl == NULL && gfp != GFP_KERNEL) {
		tbl = nested_bucket_table_alloc(ht, nbuckets, gfp);
		nbuckets = 0;
	}
	if (tbl == NULL)
		return NULL;

	tbl->size = size;

	if (alloc_bucket_locks(ht, tbl, gfp) < 0) {
		bucket_table_free(tbl);
		return NULL;
	}

	INIT_LIST_HEAD(&tbl->walkers);

	tbl->hash_rnd = get_random_u32();

	for (i = 0; i < nbuckets; i++)
		INIT_RHT_NULLS_HEAD(tbl->buckets[i], ht, i);

	return tbl;
}

static struct bucket_table *rhashtable_last_table(struct rhashtable *ht,
						  struct bucket_table *tbl)
{
	struct bucket_table *new_tbl;

	do {
		new_tbl = tbl;
		tbl = rht_dereference_rcu(tbl->future_tbl, ht);
	} while (tbl);

	return new_tbl;
}

static int rhashtable_rehash_one(struct rhashtable *ht, unsigned int old_hash)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	struct bucket_table *new_tbl = rhashtable_last_table(ht,
		rht_dereference_rcu(old_tbl->future_tbl, ht));
	struct rhash_head __rcu **pprev = rht_bucket_var(old_tbl, old_hash);
	int err = -EAGAIN;
	struct rhash_head *head, *next, *entry;
	spinlock_t *new_bucket_lock;
	unsigned int new_hash;

	if (new_tbl->nest)
		goto out;

	err = -ENOENT;

	rht_for_each(entry, old_tbl, old_hash) {
		err = 0;
		next = rht_dereference_bucket(entry->next, old_tbl, old_hash);

		if (rht_is_a_nulls(next))
			break;

		pprev = &entry->next;
	}

	if (err)
		goto out;

	new_hash = head_hashfn(ht, new_tbl, entry);

	new_bucket_lock = rht_bucket_lock(new_tbl, new_hash);

	spin_lock_nested(new_bucket_lock, SINGLE_DEPTH_NESTING);
	head = rht_dereference_bucket(new_tbl->buckets[new_hash],
				      new_tbl, new_hash);

	RCU_INIT_POINTER(entry->next, head);

	rcu_assign_pointer(new_tbl->buckets[new_hash], entry);
	spin_unlock(new_bucket_lock);

	rcu_assign_pointer(*pprev, next);

out:
	return err;
}

static int rhashtable_rehash_chain(struct rhashtable *ht,
				    unsigned int old_hash)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	spinlock_t *old_bucket_lock;
	int err;

	old_bucket_lock = rht_bucket_lock(old_tbl, old_hash);

	spin_lock_bh(old_bucket_lock);
	while (!(err = rhashtable_rehash_one(ht, old_hash)))
		;

	if (err == -ENOENT) {
		old_tbl->rehash++;
		err = 0;
	}
	spin_unlock_bh(old_bucket_lock);

	return err;
}

static int rhashtable_rehash_attach(struct rhashtable *ht,
				    struct bucket_table *old_tbl,
				    struct bucket_table *new_tbl)
{
	/* Protect future_tbl using the first bucket lock. */
	spin_lock_bh(old_tbl->locks);

	/* Did somebody beat us to it? */
	if (rcu_access_pointer(old_tbl->future_tbl)) {
		spin_unlock_bh(old_tbl->locks);
		return -EEXIST;
	}

	/* Make insertions go into the new, empty table right away. Deletions
	 * and lookups will be attempted in both tables until we synchronize.
	 */
	rcu_assign_pointer(old_tbl->future_tbl, new_tbl);

	spin_unlock_bh(old_tbl->locks);

	return 0;
}

static int rhashtable_rehash_table(struct rhashtable *ht)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	struct bucket_table *new_tbl;
	struct rhashtable_walker *walker;
	unsigned int old_hash;
	int err;

	new_tbl = rht_dereference(old_tbl->future_tbl, ht);
	if (!new_tbl)
		return 0;

	for (old_hash = 0; old_hash < old_tbl->size; old_hash++) {
		err = rhashtable_rehash_chain(ht, old_hash);
		if (err)
			return err;
		cond_resched();
	}

	/* Publish the new table pointer. */
	rcu_assign_pointer(ht->tbl, new_tbl);

	spin_lock(&ht->lock);
	list_for_each_entry(walker, &old_tbl->walkers, list)
		walker->tbl = NULL;
	spin_unlock(&ht->lock);

	/* Wait for readers. All new readers will see the new
	 * table, and thus no references to the old table will
	 * remain.
	 */
	call_rcu(&old_tbl->rcu, bucket_table_free_rcu);

	return rht_dereference(new_tbl->future_tbl, ht) ? -EAGAIN : 0;
}

static int rhashtable_rehash_alloc(struct rhashtable *ht,
				   struct bucket_table *old_tbl,
				   unsigned int size)
{
	struct bucket_table *new_tbl;
	int err;

	ASSERT_RHT_MUTEX(ht);

	new_tbl = bucket_table_alloc(ht, size, GFP_KERNEL);
	if (new_tbl == NULL)
		return -ENOMEM;

	err = rhashtable_rehash_attach(ht, old_tbl, new_tbl);
	if (err)
		bucket_table_free(new_tbl);

	return err;
}

/**
 * rhashtable_shrink - Shrink hash table while allowing concurrent lookups
 * @ht:		the hash table to shrink
 *
 * This function shrinks the hash table to fit, i.e., the smallest
 * size would not cause it to expand right away automatically.
 *
 * The caller must ensure that no concurrent resizing occurs by holding
 * ht->mutex.
 *
 * The caller must ensure that no concurrent table mutations take place.
 * It is however valid to have concurrent lookups if they are RCU protected.
 *
 * It is valid to have concurrent insertions and deletions protected by per
 * bucket locks or concurrent RCU protected lookups and traversals.
 */
static int rhashtable_shrink(struct rhashtable *ht)
{
	struct bucket_table *old_tbl = rht_dereference(ht->tbl, ht);
	unsigned int nelems = atomic_read(&ht->nelems);
	unsigned int size = 0;

	if (nelems)
		size = roundup_pow_of_two(nelems * 3 / 2);
	if (size < ht->p.min_size)
		size = ht->p.min_size;

	if (old_tbl->size <= size)
		return 0;

	if (rht_dereference(old_tbl->future_tbl, ht))
		return -EEXIST;

	return rhashtable_rehash_alloc(ht, old_tbl, size);
}

static void rht_deferred_worker(struct work_struct *work)
{
	struct rhashtable *ht;
	struct bucket_table *tbl;
	int err = 0;

	ht = container_of(work, struct rhashtable, run_work);
	mutex_lock(&ht->mutex);

	tbl = rht_dereference(ht->tbl, ht);
	tbl = rhashtable_last_table(ht, tbl);

	if (rht_grow_above_75(ht, tbl))
		err = rhashtable_rehash_alloc(ht, tbl, tbl->size * 2);
	else if (ht->p.automatic_shrinking && rht_shrink_below_30(ht, tbl))
		err = rhashtable_shrink(ht);
	else if (tbl->nest)
		err = rhashtable_rehash_alloc(ht, tbl, tbl->size);

	if (!err)
		err = rhashtable_rehash_table(ht);

	mutex_unlock(&ht->mutex);

	if (err)
		schedule_work(&ht->run_work);
}

static int rhashtable_insert_rehash(struct rhashtable *ht,
				    struct bucket_table *tbl)
{
	struct bucket_table *old_tbl;
	struct bucket_table *new_tbl;
	unsigned int size;
	int err;

	old_tbl = rht_dereference_rcu(ht->tbl, ht);

	size = tbl->size;

	err = -EBUSY;

	if (rht_grow_above_75(ht, tbl))
		size *= 2;
	/* Do not schedule more than one rehash */
	else if (old_tbl != tbl)
		goto fail;

	err = -ENOMEM;

	new_tbl = bucket_table_alloc(ht, size, GFP_ATOMIC);
	if (new_tbl == NULL)
		goto fail;

	err = rhashtable_rehash_attach(ht, tbl, new_tbl);
	if (err) {
		bucket_table_free(new_tbl);
		if (err == -EEXIST)
			err = 0;
	} else
		schedule_work(&ht->run_work);

	return err;

fail:
	/* Do not fail the insert if someone else did a rehash. */
	if (likely(rcu_dereference_raw(tbl->future_tbl)))
		return 0;

	/* Schedule async rehash to retry allocation in process context. */
	if (err == -ENOMEM)
		schedule_work(&ht->run_work);

	return err;
}

static void *rhashtable_lookup_one(struct rhashtable *ht,
				   struct bucket_table *tbl, unsigned int hash,
				   const void *key, struct rhash_head *obj)
{
	struct rhashtable_compare_arg arg = {
		.ht = ht,
		.key = key,
	};
	struct rhash_head __rcu **pprev;
	struct rhash_head *head;
	int elasticity;

	elasticity = RHT_ELASTICITY;
	pprev = rht_bucket_var(tbl, hash);
	rht_for_each_continue(head, *pprev, tbl, hash) {
		struct rhlist_head *list;
		struct rhlist_head *plist;

		elasticity--;
		if (!key ||
		    (ht->p.obj_cmpfn ?
		     ht->p.obj_cmpfn(&arg, rht_obj(ht, head)) :
		     rhashtable_compare(&arg, rht_obj(ht, head)))) {
			pprev = &head->next;
			continue;
		}

		if (!ht->rhlist)
			return rht_obj(ht, head);

		list = container_of(obj, struct rhlist_head, rhead);
		plist = container_of(head, struct rhlist_head, rhead);

		RCU_INIT_POINTER(list->next, plist);
		head = rht_dereference_bucket(head->next, tbl, hash);
		RCU_INIT_POINTER(list->rhead.next, head);
		rcu_assign_pointer(*pprev, obj);

		return NULL;
	}

	if (elasticity <= 0)
		return ERR_PTR(-EAGAIN);

	return ERR_PTR(-ENOENT);
}

static struct bucket_table *rhashtable_insert_one(struct rhashtable *ht,
						  struct bucket_table *tbl,
						  unsigned int hash,
						  struct rhash_head *obj,
						  void *data)
{
	struct rhash_head __rcu **pprev;
	struct bucket_table *new_tbl;
	struct rhash_head *head;

	if (!IS_ERR_OR_NULL(data))
		return ERR_PTR(-EEXIST);

	if (PTR_ERR(data) != -EAGAIN && PTR_ERR(data) != -ENOENT)
		return ERR_CAST(data);

	new_tbl = rcu_dereference(tbl->future_tbl);
	if (new_tbl)
		return new_tbl;

	if (PTR_ERR(data) != -ENOENT)
		return ERR_CAST(data);

	if (unlikely(rht_grow_above_max(ht, tbl)))
		return ERR_PTR(-E2BIG);

	if (unlikely(rht_grow_above_100(ht, tbl)))
		return ERR_PTR(-EAGAIN);

	pprev = rht_bucket_insert(ht, tbl, hash);
	if (!pprev)
		return ERR_PTR(-ENOMEM);

	head = rht_dereference_bucket(*pprev, tbl, hash);

	RCU_INIT_POINTER(obj->next, head);
	if (ht->rhlist) {
		struct rhlist_head *list;

		list = container_of(obj, struct rhlist_head, rhead);
		RCU_INIT_POINTER(list->next, NULL);
	}

	rcu_assign_pointer(*pprev, obj);

	atomic_inc(&ht->nelems);
	if (rht_grow_above_75(ht, tbl))
		schedule_work(&ht->run_work);

	return NULL;
}

static void *rhashtable_try_insert(struct rhashtable *ht, const void *key,
				   struct rhash_head *obj)
{
	struct bucket_table *new_tbl;
	struct bucket_table *tbl;
	unsigned int hash;
	spinlock_t *lock;
	void *data;

	tbl = rcu_dereference(ht->tbl);

	/* All insertions must grab the oldest table containing
	 * the hashed bucket that is yet to be rehashed.
	 */
	for (;;) {
		hash = rht_head_hashfn(ht, tbl, obj, ht->p);
		lock = rht_bucket_lock(tbl, hash);
		spin_lock_bh(lock);

		if (tbl->rehash <= hash)
			break;

		spin_unlock_bh(lock);
		tbl = rcu_dereference(tbl->future_tbl);
	}

	data = rhashtable_lookup_one(ht, tbl, hash, key, obj);
	new_tbl = rhashtable_insert_one(ht, tbl, hash, obj, data);
	if (PTR_ERR(new_tbl) != -EEXIST)
		data = ERR_CAST(new_tbl);

	while (!IS_ERR_OR_NULL(new_tbl)) {
		tbl = new_tbl;
		hash = rht_head_hashfn(ht, tbl, obj, ht->p);
		spin_lock_nested(rht_bucket_lock(tbl, hash),
				 SINGLE_DEPTH_NESTING);

		data = rhashtable_lookup_one(ht, tbl, hash, key, obj);
		new_tbl = rhashtable_insert_one(ht, tbl, hash, obj, data);
		if (PTR_ERR(new_tbl) != -EEXIST)
			data = ERR_CAST(new_tbl);

		spin_unlock(rht_bucket_lock(tbl, hash));
	}

	spin_unlock_bh(lock);

	if (PTR_ERR(data) == -EAGAIN)
		data = ERR_PTR(rhashtable_insert_rehash(ht, tbl) ?:
			       -EAGAIN);

	return data;
}

void *rhashtable_insert_slow(struct rhashtable *ht, const void *key,
			     struct rhash_head *obj)
{
	void *data;

	do {
		rcu_read_lock();
		data = rhashtable_try_insert(ht, key, obj);
		rcu_read_unlock();
	} while (PTR_ERR(data) == -EAGAIN);

	return data;
}
EXPORT_SYMBOL_GPL(rhashtable_insert_slow);

/**
 * rhashtable_walk_enter - Initialise an iterator
 * @ht:		Table to walk over
 * @iter:	Hash table Iterator
 *
 * This function prepares a hash table walk.
 *
 * Note that if you restart a walk after rhashtable_walk_stop you
 * may see the same object twice.  Also, you may miss objects if
 * there are removals in between rhashtable_walk_stop and the next
 * call to rhashtable_walk_start.
 *
 * For a completely stable walk you should construct your own data
 * structure outside the hash table.
 *
 * This function may sleep so you must not call it from interrupt
 * context or with spin locks held.
 *
 * You must call rhashtable_walk_exit after this function returns.
 */
void rhashtable_walk_enter(struct rhashtable *ht, struct rhashtable_iter *iter)
{
	iter->ht = ht;
	iter->p = NULL;
	iter->slot = 0;
	iter->skip = 0;

	spin_lock(&ht->lock);
	iter->walker.tbl =
		rcu_dereference_protected(ht->tbl, lockdep_is_held(&ht->lock));
	list_add(&iter->walker.list, &iter->walker.tbl->walkers);
	spin_unlock(&ht->lock);
}
EXPORT_SYMBOL_GPL(rhashtable_walk_enter);

/**
 * rhashtable_walk_exit - Free an iterator
 * @iter:	Hash table Iterator
 *
 * This function frees resources allocated by rhashtable_walk_init.
 */
void rhashtable_walk_exit(struct rhashtable_iter *iter)
{
	spin_lock(&iter->ht->lock);
	if (iter->walker.tbl)
		list_del(&iter->walker.list);
	spin_unlock(&iter->ht->lock);
}
EXPORT_SYMBOL_GPL(rhashtable_walk_exit);

/**
 * rhashtable_walk_start - Start a hash table walk
 * @iter:	Hash table iterator
 *
 * Start a hash table walk at the current iterator position.  Note that we take
 * the RCU lock in all cases including when we return an error.  So you must
 * always call rhashtable_walk_stop to clean up.
 *
 * Returns zero if successful.
 *
 * Returns -EAGAIN if resize event occured.  Note that the iterator
 * will rewind back to the beginning and you may use it immediately
 * by calling rhashtable_walk_next.
 */
int rhashtable_walk_start(struct rhashtable_iter *iter)
	__acquires(RCU)
{
	struct rhashtable *ht = iter->ht;

	rcu_read_lock();

	spin_lock(&ht->lock);
	if (iter->walker.tbl)
		list_del(&iter->walker.list);
	spin_unlock(&ht->lock);

	if (!iter->walker.tbl) {
		iter->walker.tbl = rht_dereference_rcu(ht->tbl, ht);
		return -EAGAIN;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_walk_start);

/**
 * rhashtable_walk_next - Return the next object and advance the iterator
 * @iter:	Hash table iterator
 *
 * Note that you must call rhashtable_walk_stop when you are finished
 * with the walk.
 *
 * Returns the next object or NULL when the end of the table is reached.
 *
 * Returns -EAGAIN if resize event occured.  Note that the iterator
 * will rewind back to the beginning and you may continue to use it.
 */
void *rhashtable_walk_next(struct rhashtable_iter *iter)
{
	struct bucket_table *tbl = iter->walker.tbl;
	struct rhlist_head *list = iter->list;
	struct rhashtable *ht = iter->ht;
	struct rhash_head *p = iter->p;
	bool rhlist = ht->rhlist;

	if (p) {
		if (!rhlist || !(list = rcu_dereference(list->next))) {
			p = rcu_dereference(p->next);
			list = container_of(p, struct rhlist_head, rhead);
		}
		goto next;
	}

	for (; iter->slot < tbl->size; iter->slot++) {
		int skip = iter->skip;

		rht_for_each_rcu(p, tbl, iter->slot) {
			if (rhlist) {
				list = container_of(p, struct rhlist_head,
						    rhead);
				do {
					if (!skip)
						goto next;
					skip--;
					list = rcu_dereference(list->next);
				} while (list);

				continue;
			}
			if (!skip)
				break;
			skip--;
		}

next:
		if (!rht_is_a_nulls(p)) {
			iter->skip++;
			iter->p = p;
			iter->list = list;
			return rht_obj(ht, rhlist ? &list->rhead : p);
		}

		iter->skip = 0;
	}

	iter->p = NULL;

	/* Ensure we see any new tables. */
	smp_rmb();

	iter->walker.tbl = rht_dereference_rcu(tbl->future_tbl, ht);
	if (iter->walker.tbl) {
		iter->slot = 0;
		iter->skip = 0;
		return ERR_PTR(-EAGAIN);
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(rhashtable_walk_next);

/**
 * rhashtable_walk_stop - Finish a hash table walk
 * @iter:	Hash table iterator
 *
 * Finish a hash table walk.  Does not reset the iterator to the start of the
 * hash table.
 */
void rhashtable_walk_stop(struct rhashtable_iter *iter)
	__releases(RCU)
{
	struct rhashtable *ht;
	struct bucket_table *tbl = iter->walker.tbl;

	if (!tbl)
		goto out;

	ht = iter->ht;

	spin_lock(&ht->lock);
	if (tbl->rehash < tbl->size)
		list_add(&iter->walker.list, &tbl->walkers);
	else
		iter->walker.tbl = NULL;
	spin_unlock(&ht->lock);

	iter->p = NULL;

out:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(rhashtable_walk_stop);

static size_t rounded_hashtable_size(const struct rhashtable_params *params)
{
	size_t retsize;

	if (params->nelem_hint)
		retsize = max(roundup_pow_of_two(params->nelem_hint * 4 / 3),
			      (unsigned long)params->min_size);
	else
		retsize = max(HASH_DEFAULT_SIZE,
			      (unsigned long)params->min_size);

	return retsize;
}

static u32 rhashtable_jhash2(const void *key, u32 length, u32 seed)
{
	return jhash2(key, length, seed);
}

/**
 * rhashtable_init - initialize a new hash table
 * @ht:		hash table to be initialized
 * @params:	configuration parameters
 *
 * Initializes a new hash table based on the provided configuration
 * parameters. A table can be configured either with a variable or
 * fixed length key:
 *
 * Configuration Example 1: Fixed length keys
 * struct test_obj {
 *	int			key;
 *	void *			my_member;
 *	struct rhash_head	node;
 * };
 *
 * struct rhashtable_params params = {
 *	.head_offset = offsetof(struct test_obj, node),
 *	.key_offset = offsetof(struct test_obj, key),
 *	.key_len = sizeof(int),
 *	.hashfn = jhash,
 *	.nulls_base = (1U << RHT_BASE_SHIFT),
 * };
 *
 * Configuration Example 2: Variable length keys
 * struct test_obj {
 *	[...]
 *	struct rhash_head	node;
 * };
 *
 * u32 my_hash_fn(const void *data, u32 len, u32 seed)
 * {
 *	struct test_obj *obj = data;
 *
 *	return [... hash ...];
 * }
 *
 * struct rhashtable_params params = {
 *	.head_offset = offsetof(struct test_obj, node),
 *	.hashfn = jhash,
 *	.obj_hashfn = my_hash_fn,
 * };
 */
int rhashtable_init(struct rhashtable *ht,
		    const struct rhashtable_params *params)
{
	struct bucket_table *tbl;
	size_t size;

	if ((!params->key_len && !params->obj_hashfn) ||
	    (params->obj_hashfn && !params->obj_cmpfn))
		return -EINVAL;

	if (params->nulls_base && params->nulls_base < (1U << RHT_BASE_SHIFT))
		return -EINVAL;

	memset(ht, 0, sizeof(*ht));
	mutex_init(&ht->mutex);
	spin_lock_init(&ht->lock);
	memcpy(&ht->p, params, sizeof(*params));

	if (params->min_size)
		ht->p.min_size = roundup_pow_of_two(params->min_size);

	/* Cap total entries at 2^31 to avoid nelems overflow. */
	ht->max_elems = 1u << 31;

	if (params->max_size) {
		ht->p.max_size = rounddown_pow_of_two(params->max_size);
		if (ht->p.max_size < ht->max_elems / 2)
			ht->max_elems = ht->p.max_size * 2;
	}

	ht->p.min_size = max_t(u16, ht->p.min_size, HASH_MIN_SIZE);

	size = rounded_hashtable_size(&ht->p);

	if (params->locks_mul)
		ht->p.locks_mul = roundup_pow_of_two(params->locks_mul);
	else
		ht->p.locks_mul = BUCKET_LOCKS_PER_CPU;

	ht->key_len = ht->p.key_len;
	if (!params->hashfn) {
		ht->p.hashfn = jhash;

		if (!(ht->key_len & (sizeof(u32) - 1))) {
			ht->key_len /= sizeof(u32);
			ht->p.hashfn = rhashtable_jhash2;
		}
	}

	tbl = bucket_table_alloc(ht, size, GFP_KERNEL);
	if (tbl == NULL)
		return -ENOMEM;

	atomic_set(&ht->nelems, 0);

	RCU_INIT_POINTER(ht->tbl, tbl);

	INIT_WORK(&ht->run_work, rht_deferred_worker);

	return 0;
}
EXPORT_SYMBOL_GPL(rhashtable_init);

/**
 * rhltable_init - initialize a new hash list table
 * @hlt:	hash list table to be initialized
 * @params:	configuration parameters
 *
 * Initializes a new hash list table.
 *
 * See documentation for rhashtable_init.
 */
int rhltable_init(struct rhltable *hlt, const struct rhashtable_params *params)
{
	int err;

	/* No rhlist NULLs marking for now. */
	if (params->nulls_base)
		return -EINVAL;

	err = rhashtable_init(&hlt->ht, params);
	hlt->ht.rhlist = true;
	return err;
}
EXPORT_SYMBOL_GPL(rhltable_init);

static void rhashtable_free_one(struct rhashtable *ht, struct rhash_head *obj,
				void (*free_fn)(void *ptr, void *arg),
				void *arg)
{
	struct rhlist_head *list;

	if (!ht->rhlist) {
		free_fn(rht_obj(ht, obj), arg);
		return;
	}

	list = container_of(obj, struct rhlist_head, rhead);
	do {
		obj = &list->rhead;
		list = rht_dereference(list->next, ht);
		free_fn(rht_obj(ht, obj), arg);
	} while (list);
}

/**
 * rhashtable_free_and_destroy - free elements and destroy hash table
 * @ht:		the hash table to destroy
 * @free_fn:	callback to release resources of element
 * @arg:	pointer passed to free_fn
 *
 * Stops an eventual async resize. If defined, invokes free_fn for each
 * element to releasal resources. Please note that RCU protected
 * readers may still be accessing the elements. Releasing of resources
 * must occur in a compatible manner. Then frees the bucket array.
 *
 * This function will eventually sleep to wait for an async resize
 * to complete. The caller is responsible that no further write operations
 * occurs in parallel.
 */
void rhashtable_free_and_destroy(struct rhashtable *ht,
				 void (*free_fn)(void *ptr, void *arg),
				 void *arg)
{
	struct bucket_table *tbl;
	unsigned int i;

	cancel_work_sync(&ht->run_work);

	mutex_lock(&ht->mutex);
	tbl = rht_dereference(ht->tbl, ht);
	if (free_fn) {
		for (i = 0; i < tbl->size; i++) {
			struct rhash_head *pos, *next;

			cond_resched();
			for (pos = rht_dereference(*rht_bucket(tbl, i), ht),
			     next = !rht_is_a_nulls(pos) ?
					rht_dereference(pos->next, ht) : NULL;
			     !rht_is_a_nulls(pos);
			     pos = next,
			     next = !rht_is_a_nulls(pos) ?
					rht_dereference(pos->next, ht) : NULL)
				rhashtable_free_one(ht, pos, free_fn, arg);
		}
	}

	bucket_table_free(tbl);
	mutex_unlock(&ht->mutex);
}
EXPORT_SYMBOL_GPL(rhashtable_free_and_destroy);

void rhashtable_destroy(struct rhashtable *ht)
{
	return rhashtable_free_and_destroy(ht, NULL, NULL);
}
EXPORT_SYMBOL_GPL(rhashtable_destroy);

struct rhash_head __rcu **rht_bucket_nested(const struct bucket_table *tbl,
					    unsigned int hash)
{
	const unsigned int shift = PAGE_SHIFT - ilog2(sizeof(void *));
	static struct rhash_head __rcu *rhnull =
		(struct rhash_head __rcu *)NULLS_MARKER(0);
	unsigned int index = hash & ((1 << tbl->nest) - 1);
	unsigned int size = tbl->size >> tbl->nest;
	unsigned int subhash = hash;
	union nested_table *ntbl;

	ntbl = (union nested_table *)rcu_dereference_raw(tbl->buckets[0]);
	ntbl = rht_dereference_bucket_rcu(ntbl[index].table, tbl, hash);
	subhash >>= tbl->nest;

	while (ntbl && size > (1 << shift)) {
		index = subhash & ((1 << shift) - 1);
		ntbl = rht_dereference_bucket_rcu(ntbl[index].table,
						  tbl, hash);
		size >>= shift;
		subhash >>= shift;
	}

	if (!ntbl)
		return &rhnull;

	return &ntbl[subhash].bucket;

}
EXPORT_SYMBOL_GPL(rht_bucket_nested);

struct rhash_head __rcu **rht_bucket_nested_insert(struct rhashtable *ht,
						   struct bucket_table *tbl,
						   unsigned int hash)
{
	const unsigned int shift = PAGE_SHIFT - ilog2(sizeof(void *));
	unsigned int index = hash & ((1 << tbl->nest) - 1);
	unsigned int size = tbl->size >> tbl->nest;
	union nested_table *ntbl;
	unsigned int shifted;
	unsigned int nhash;

	ntbl = (union nested_table *)rcu_dereference_raw(tbl->buckets[0]);
	hash >>= tbl->nest;
	nhash = index;
	shifted = tbl->nest;
	ntbl = nested_table_alloc(ht, &ntbl[index].table,
				  size <= (1 << shift) ? shifted : 0, nhash);

	while (ntbl && size > (1 << shift)) {
		index = hash & ((1 << shift) - 1);
		size >>= shift;
		hash >>= shift;
		nhash |= index << shifted;
		shifted += shift;
		ntbl = nested_table_alloc(ht, &ntbl[index].table,
					  size <= (1 << shift) ? shifted : 0,
					  nhash);
	}

	if (!ntbl)
		return NULL;

	return &ntbl[hash].bucket;

}
EXPORT_SYMBOL_GPL(rht_bucket_nested_insert);
