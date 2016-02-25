/*
 * (C) 2011  Liu Bo <liubo2009@cn.fujitsu.com>
 * (C) 2013 Fusion-io
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <linux/rcupdate.h>
#include <linux/random.h>
#include <linux/lockdep.h>
#include <linux/sched.h>

#include "include/skiplist.h"

static struct kmem_cache *slab_caches[SKIP_MAXLEVEL];

/*
 * before starting an insert, we preload based on the current
 * height of the list.  This holds the preload result
 * in a per-cpu array
 */
struct skip_preload {
	/*
	 * the preload is filled in based on the highest possible level
	 * of the list you're preloading.  So we basically end up with
	 * one preloaded node for each max size.
	 */
	struct sl_leaf *preload[SKIP_MAXLEVEL + 1];
};

static DEFINE_PER_CPU(struct skip_preload, skip_preloads) = { {NULL,}, };

static void sl_init_node(struct sl_node *node, int level)
{
	int i;
	spin_lock_init(&node->lock);

	/*
	 * from a locking point of view, the skiplists are a dumb linked
	 * list where we take up to three locks in order from left to
	 * right.  I haven't been able to teach lockdep how to do this
	 * yet
	 */
	lockdep_set_novalidate_class(&node->lock);

	for (i = 0; i <= level; i++) {
		node->ptrs[i].prev = NULL;
		node->ptrs[i].next = NULL;
	}
	node->level = level;
	node->pending = SKIPLIST_LIVE;
}

static void sl_init_leaf(struct sl_leaf *leaf, int level)
{
	atomic_set(&leaf->refs, 1);
	sl_init_node(&leaf->node, level);
}

/*
 * the rcu based searches need to block reuse until a given search round
 * is done.  So, we use call_rcu for freeing the leaf structure.
 */
static void sl_free_rcu(struct rcu_head *head)
{
	struct sl_leaf *leaf = container_of(head, struct sl_leaf, rcu_head);
	kmem_cache_free(slab_caches[leaf->node.level], leaf);
}

void skiplist_get_leaf(struct sl_leaf *leaf)
{
	atomic_inc(&leaf->refs);
}
EXPORT_SYMBOL(skiplist_get_leaf);

int skiplist_get_leaf_not_zero(struct sl_leaf *leaf)
{
	return atomic_inc_not_zero(&leaf->refs);
}
EXPORT_SYMBOL(skiplist_get_leaf_not_zero);

void skiplist_put_leaf(struct sl_leaf *leaf)
{
	BUG_ON(atomic_read(&leaf->refs) == 0);
	if (atomic_dec_and_test(&leaf->refs))
		call_rcu(&leaf->rcu_head, sl_free_rcu);
}
EXPORT_SYMBOL(skiplist_put_leaf);

/*
 * if a node is currently inserting, this spins until the
 * insertion is complete.  Calling this with another node
 * locked usually leads to deadlocks because the linking
 * needs to take neighboring locks.
 */
void skiplist_wait_pending_insert(struct sl_node *node)
{
	while (sl_node_inserting(node)) {
		cpu_relax();
		smp_rmb();
	}
}
EXPORT_SYMBOL(skiplist_wait_pending_insert);

/*
 * helper function to pull out the next live leaf at a given level.
 * no locks are required or taken
 *
 * Note that if this returns NULL, you may have to wait for a pending
 * insertion on 'node' before you can trust the answer
 */
static inline struct sl_leaf *sl_next_leaf(struct sl_list *list,
					   struct sl_node *node, int l)
{
	do {
		node = rcu_dereference(node->ptrs[l].next);
		if (!node)
			return NULL;
		if (!sl_node_dead(node))
			return sl_entry(node);
	} while (1);
}

/*
 * helper functions to wade through dead nodes pending deletion
 * and return live ones.  This does wait on pending insertions
 * as it walks backwards.
 */
static struct sl_node *find_live_prev(struct sl_list *list,
				      struct sl_node *node, int level)
{
	struct sl_node *prev = NULL;
	/* head->prev points to the max, this makes sure we don't loop */
	if (node == list->head)
		return NULL;

	while (node) {
		prev = rcu_dereference(node->ptrs[level].prev);
		if (!prev) {
			skiplist_wait_pending_insert(node);
			prev = rcu_dereference(node->ptrs[level].prev);
		}
		node = prev;
		/*
		 * the head is never dead, so we'll never walk past
		 * it down in this loop
		 */
		if (!sl_node_dead(prev))
			break;
	}

	return node;
}

/*
 * walks forward to find a live next pointer.  This does
 * not wait on pending insertions because it would deadlock
 * the callers
 */
static struct sl_node *find_live_next(struct sl_list *list,
				      struct sl_node *node, int level)
{
	while (1) {
		node = rcu_dereference(node->ptrs[level].next);
		if (!node)
			return NULL;
		if (!sl_node_dead(node))
			return node;
	}
}

/*
 * return the highest value for a given leaf.  This is cached
 * in leaf->max so that we don't have to wander into
 * the slot pointers.  The max is equal to the key + size of the
 * last slot.
 */
static unsigned long sl_max_key(struct sl_leaf *leaf)
{
	smp_rmb();
	return leaf->max;
}

/*
 * return the lowest key for a given leaf. This comes out
 * of the node key array and not the slots
 */
static unsigned long sl_min_key(struct sl_leaf *leaf)
{
	smp_rmb();
	return leaf->keys[0];
}

void sl_lock_node(struct sl_node *n)
{
	spin_lock(&n->lock);
}

void sl_unlock_node(struct sl_node *n)
{
	if (n)
		spin_unlock(&n->lock);
}

void skiplist_lock_leaf(struct sl_leaf *leaf)
{
	sl_lock_node(&leaf->node);
}
EXPORT_SYMBOL(skiplist_lock_leaf);

void skiplist_unlock_leaf(struct sl_leaf *leaf)
{
	if (leaf)
		sl_unlock_node(&leaf->node);
}
EXPORT_SYMBOL(skiplist_unlock_leaf);

void assert_locked_node(struct sl_node *n)
{
	assert_spin_locked(&n->lock);
}

/*
 * helper function to link a single level during an insert.
 * prev must be locked, and it is the node we are linking after.
 *
 * This will find a live next pointer, lock it, and link it
 * with our new node
 */
static void sl_link_one_level(struct sl_list *list,
			      struct sl_node *prev,
			      struct sl_node *node, int level)
{
	struct sl_node *next;
	struct sl_node *test;

	assert_locked_node(prev);
	BUG_ON(sl_node_dead(prev));

again:
	next = find_live_next(list, prev, level);
	if (next) {
		/*
		 * next may be pending insertion at this point
		 * but it was linked far enough up for prev to
		 * point to it.  So we can safely just use it
		 */
		sl_lock_node(next);
		test = find_live_next(list, prev, level);
		if (test != next || sl_node_dead(next)) {
			sl_unlock_node(next);
			goto again;
		}
		/*
		 * make sure the our next and prev really point to each
		 * other now that we have next locked.
		 */
		if (find_live_prev(list, next, level) != prev) {
			sl_unlock_node(next);
			goto again;
		}
	}

	rcu_assign_pointer(node->ptrs[level].next, next);
	rcu_assign_pointer(node->ptrs[level].prev, prev);
	rcu_assign_pointer(prev->ptrs[level].next, node);

	/*
	 * if next is null, we're the last node on this level.
	 * The head->prev pointer is used to cache this fact
	 */
	if (next)
		rcu_assign_pointer(next->ptrs[level].prev, node);
	else
		rcu_assign_pointer(list->head->ptrs[level].prev, node);

	sl_unlock_node(next);
}

/*
 * for everything above level 0 in a new leaf, we link from the bottom
 * up.  This walks the prev pointers in the new leaf to find higher
 * leaves to link with.  The basic idea is to go down one level
 * and walk backwards until you find a leaf of the right level.
 *
 * It looks like much more work than walking down from the top, but
 * most of the leaves are at the lower levels.  Walking from the top
 * is sure to waste time going past leaves we never use.
 */
static noinline struct sl_node *find_higher_prev(struct sl_list *list,
					struct sl_leaf *ins, int level)
{
	struct sl_node *cur;
	struct sl_node *prev;
	struct sl_leaf *leaf;
	int search_level = level - 1;

	BUG_ON(level == 0);

	/*
	 * step one, walk backward on the lower level until
	 * we find a leaf of the proper height.
	 */
	cur = &ins->node;
	while (1) {
		prev = find_live_prev(list, cur, search_level);
		skiplist_wait_pending_insert(prev);
		if (prev->level >= level)
			break;
		cur = prev;
	}

	/*
	 * now we have a node at the right level, but while we
	 * walked someone might have inserted leaves between
	 * prev and the insertion point.  Walk forward to make
	 * sure we have best leaf for linking.
	 */
	while (1) {
		leaf = sl_next_leaf(list, prev, level);
		if (!leaf || sl_max_key(ins) <= sl_min_key(leaf))
			return prev;

		prev = &leaf->node;
		skiplist_wait_pending_insert(prev);
	}
}

/*
 * this does all of the hard work to finish linking a pending leaf.
 * 'level' tells us how high the caller has already linked, and the
 * caller must have at least linked level 0.
 *
 * leaf must be unlocked, nothing is locked when we return.
 */
static noinline void sl_link_pending_leaf(struct sl_list *list,
				 struct sl_leaf *leaf, int level)
{
	struct sl_node *prev;
	struct sl_node *search_prev;
	struct sl_node *next;
	struct sl_leaf *next_leaf;
	struct sl_node *node = &leaf->node;
	int last_level = node->level;

	/* did the caller do enough already? */
	if (level > node->level)
		return;

	while (level <= last_level) {
		if (node->ptrs[level].prev)
			prev = find_live_prev(list, node, level);
		else
			prev = find_higher_prev(list, leaf, level);

		skiplist_wait_pending_insert(prev);
		sl_lock_node(prev);

		/* don't link with the dead */
		if (sl_node_dead(prev)) {
			sl_unlock_node(prev);
			continue;
		}

		/* lock ourselves */
		sl_lock_node(node);

		/*
		 * if prev and next are already set for this level,
		 * we're done.
		 */
		if (node->ptrs[level].prev) {
			if (node->ptrs[level].next) {
				level++;
				goto unlock_node;
			}
			/*
			 * if our node already has a prev pointer
			 * make sure the prev we found does point to us
			 */
			if (find_live_next(list, prev, level) != node)
				goto unlock_node;
		}
again:
		/*
		 * if our node has a next pointer, use it.  Otherwise
		 * use the next from prev
		 */
		if (node->ptrs[level].next)
			next = find_live_next(list, node, level);
		else
			next = find_live_next(list, prev, level);

		/*
		 * if we followed prev, someone might have raced in
		 * and linked this level of node.  Make sure we
		 * don't deadlock by trying to node ourselves twice.
		 */
		if (next == node)
			goto again;

		if (next) {
			/*
			 * someone may have raced in and inserted a
			 * node between prev and node.  Since we
			 * have node locked, we have to make sure not
			 * to break locking rules if next is actually
			 * before node in the list.
			 */
			next_leaf = sl_entry(next);
			if (sl_min_key(next_leaf) < sl_max_key(leaf))
				goto unlock_node;

			/*
			 * we can't check next for pending here.
			 * we'd just end up deadlocked on him
			 * waiting for us to clear pending.
			 */
			sl_lock_node(next);

			/*
			 * make sure we've still got the right spot to
			 * link things in
			 */
			if (sl_node_dead(next))
				goto unlock_all;

			if (sl_min_key(next_leaf) < sl_max_key(leaf))
				goto unlock_all;

			/*
			 * finally make sure next really points back to
			 * either prev or our node
			 */
			search_prev = find_live_prev(list, next, level);
			if (search_prev != node && search_prev != prev)
				goto unlock_all;
		}
		rcu_assign_pointer(node->ptrs[level].next, next);
		rcu_assign_pointer(node->ptrs[level].prev, prev);
		rcu_assign_pointer(prev->ptrs[level].next, node);

		/*
		 * if next is null, we're the last node on this level.
		 * The head->prev pointer is used to cache this fact
		 */
		if (next)
			rcu_assign_pointer(next->ptrs[level].prev, node);
		else
			rcu_assign_pointer(list->head->ptrs[level].prev, node);

		level++;
unlock_all:
		sl_unlock_node(next);
unlock_node:
		sl_unlock_node(node);
		sl_unlock_node(prev);
	}
}

/*
 * when we split a leaf to do an insert, this links our new leaf with the
 * one we split.  We'll use any pointers we can from 'after'
 */
static void __sl_link_after_node(struct sl_list *list, struct sl_node *node,
				 struct sl_node *after, int level)
{
	int i;

	/* first use all the pointers from 'after' */
	for (i = 0; i <= after->level && i <= level; i++)
		sl_link_one_level(list, after, node, i);
}

/*
 * final stage of linking, this is where we actually clear pending
 */
static void __link_pending_insert(struct sl_list *list, struct sl_node *node, int start_level)
{
	sl_link_pending_leaf(list, sl_entry(node), start_level);
	smp_wmb();
	node->pending = 0;
}

static void sl_link_after_node(struct sl_list *list, struct sl_node *node,
			       struct sl_node *after, int level)
{
	__sl_link_after_node(list, node, after, level);

	sl_unlock_node(node);
	sl_unlock_node(after);

	__link_pending_insert(list, node, after->level + 1);
}

/*
 * returns the first leaf in the list, locked with an
 * extra reference added
 */
struct sl_leaf *skiplist_first_leaf(struct sl_list *list)
{
	struct sl_leaf *leaf = NULL;
	struct sl_node *p;

	rcu_read_lock();
	while (1) {
		p = find_live_next(list, list->head, 0);
		if (!p)
			break;

		sl_lock_node(p);
		if (!sl_node_dead(p) &&
		    find_live_next(list, list->head, 0) == p) {
			leaf = sl_entry(p);
			skiplist_get_leaf(leaf);
			goto out;
		}
		sl_unlock_node(p);
	}
out:
	rcu_read_unlock();
	return leaf;
}
EXPORT_SYMBOL(skiplist_first_leaf);

/*
 * returns the first leaf in the list.  No locks are held and
 * no references are added.  Must be called under rcu_read_lock()
 */
struct sl_leaf *skiplist_first_leaf_rcu(struct sl_list *list)
{
	struct sl_node *p;

	p = find_live_next(list, list->head, 0);
	if (p)
		return sl_entry(p);
	return NULL;

}
EXPORT_SYMBOL(skiplist_first_leaf_rcu);

/*
 * sequential search for lockless rcu.  The insert/deletion routines
 * order their operations to make this rcu safe.
 *
 * If we find the key, we return zero and set 'slot' to the location.
 *
 * If we don't find anything, we return 1 and set 'slot' to the location
 * where the insertion should take place
 *
 * case1:
 *       [ key ... size ]
 *  [found .. found size  ]
 *
 *  case2:
 *  [key ... size ]
 *      [found .. found size ]
 *
 *  case3:
 *  [key ...                 size ]
 *      [ found .. found size ]
 *
 *  case4:
 *  [key ...size ]
 *         [ found ... found size ]
 *
 *  case5:
 *                       [key ...size ]
 *         [ found ... found size ]
 */
int skiplist_search_leaf(struct sl_leaf *leaf, unsigned long key,
			 unsigned long size, int *slot)
{
	int i;
	int cur;
	int last;
	unsigned long this_key;
	struct sl_slot *found;

again:
	cur = 0;
	last = leaf->nr;

	/* find the first slot greater than our key */
	for (i = 0; i < last; i++) {
		smp_rmb();
		this_key = leaf->keys[i];
		if (this_key >= key + size)
			break;
		cur = i;
	}
	if (leaf->keys[cur] < key + size) {
		/*
		 * if we're in the middle of an insert, pointer may
		 * be null.  This little loop will wait for the insertion
		 * to finish.
		 */
		while (1) {
			found = rcu_dereference(leaf->ptrs[cur]);
			if (found)
				break;
			cpu_relax();
		}

		/* insert is juggling our slots, try again */
		if (found->key != leaf->keys[cur])
			goto again;

		/* case1, case2, case5 */
		if (found->key < key + size && found->key + found->size > key) {
			*slot = cur;
			return 0;
		}

		/* case3, case4 */
		if (found->key < key + size && found->key >= key) {
			*slot = cur;
			return 0;
		}

		*slot = cur + 1;
		return 1;
	}
	*slot = cur;
	return 1;
}
EXPORT_SYMBOL(skiplist_search_leaf);

/*
 * helper to put a cached leaf and zero out the
 * pointer
 */
static void invalidate_cache(struct sl_leaf **cache)
{
	if (cache) {
		skiplist_put_leaf(*cache);
		*cache = NULL;
	}
}

/*
 * helper to grab a reference on a cached leaf.
 */
static void cache_leaf(struct sl_leaf *leaf, struct sl_leaf **cache)
{
	assert_locked_node(&leaf->node);
	BUG_ON(sl_node_dead(&leaf->node));
	if (cache && *cache != leaf) {
		if (*cache)
			skiplist_put_leaf(*cache);
		skiplist_get_leaf(leaf);
		*cache = leaf;
	}
}

/*
 * this does the dirty work of splitting and/or shifting a leaf
 * to get a new slot inside.  The leaf must be locked.  slot
 * tells us where into we should insert in the leaf and the cursor
 * should have all the pointers we need to fully link any new nodes
 * we have to create.
 */
static noinline int add_key_to_leaf(struct sl_list *list, struct sl_leaf *leaf,
			   struct sl_slot *slot_ptr, unsigned long key,
			   int slot, int preload_token, struct sl_leaf **cache)
{
	struct sl_leaf *split;
	struct skip_preload *skp;
	int level;
	int finish_split_insert = 0;

	/* no splitting required, just shift our way in */
	if (leaf->nr < SKIP_KEYS_PER_NODE)
		goto insert;

	skp = this_cpu_ptr(&skip_preloads);
	split = skp->preload[preload_token];

	/*
	 * we need to insert a new leaf, but we try not to insert a new leaf at
	 * the same height as our previous one, it's just a waste of high level
	 * searching.  If the new node is the same level or lower than the
	 * existing one, try to use a level 0 leaf instead.
	 */
	if (leaf->node.level > 0 && split->node.level <= leaf->node.level) {
		if (skp->preload[0]) {
			preload_token = 0;
			split = skp->preload[0];
		}
	}
	skp->preload[preload_token] = NULL;

	level = split->node.level;

	/*
	 * bump our list->level to whatever we've found.  Nobody allocating
	 * a new node is going to set it higher than list->level + 1
	 */
	if (level > list->level)
		list->level = level;

	/*
	 * this locking is really only required for the small window where
	 * we are linking the node and someone might be deleting one of the
	 * nodes we are linking with.  The leaf passed in was already
	 * locked.
	 */
	split->node.pending = SKIPLIST_PENDING_INSERT;
	sl_lock_node(&split->node);

	if (slot == leaf->nr) {
		/*
		 * our new slot just goes at the front of the new leaf, don't
		 * bother shifting things in from the previous leaf.
		 */
		slot = 0;
		split->nr = 1;
		split->max = key + slot_ptr->size;
		split->keys[0] = key;
		split->ptrs[0] = slot_ptr;
		smp_wmb();
		cache_leaf(split, cache);
		sl_link_after_node(list, &split->node, &leaf->node, level);
		return 0;
	} else {
		int nr = SKIP_KEYS_PER_NODE / 2;
		int mid = SKIP_KEYS_PER_NODE - nr;
		int src_i = mid;
		int dst_i = 0;
		int orig_nr = leaf->nr;

		/* split the previous leaf in half and copy items over */
		split->nr = nr;
		split->max = leaf->max;

		while (src_i < slot) {
			split->keys[dst_i] = leaf->keys[src_i];
			split->ptrs[dst_i++] = leaf->ptrs[src_i++];
		}

		if (slot >= mid) {
			split->keys[dst_i] = key;
			split->ptrs[dst_i++] = slot_ptr;
			split->nr++;
		}

		while (src_i < orig_nr) {
			split->keys[dst_i] = leaf->keys[src_i];
			split->ptrs[dst_i++] = leaf->ptrs[src_i++];
		}

		/*
		 * this completes the initial link, but we still
		 * have the original node and split locked
		 */
		__sl_link_after_node(list, &split->node, &leaf->node, level);

		nr = SKIP_KEYS_PER_NODE - nr;

		/*
		 * now what we have all the items copied and our new
		 * leaf inserted, update the nr in this leaf.  Anyone
		 * searching in rculand will find the fully updated
		 */
		leaf->max = leaf->keys[nr - 1] + leaf->ptrs[nr - 1]->size;
		smp_wmb();
		leaf->nr = nr;

		/*
		 * if the slot was in split item, we're done,
		 * otherwise we need to move down into the
		 * code below that shifts the items and
		 * inserts the new key
		 */
		if (slot >= mid) {
			cache_leaf(split, cache);
			sl_unlock_node(&split->node);
			sl_unlock_node(&leaf->node);

			/* finish linking split all the way to the top */
			__link_pending_insert(list, &split->node,
					      leaf->node.level + 1);
			return 0;
		}

		/*
		 * unlock our split node and let the code below finish
		 * the key insertion.  We'll finish inserting split
		 * after we unlock leaf
		 */
		sl_unlock_node(&split->node);
		finish_split_insert = 1;
	}
insert:
	if (slot < leaf->nr) {
		int i;

		/*
		 * put something sane into the new last slot so rcu
		 * searchers won't get confused
		 */
		leaf->keys[leaf->nr] = 0;
		leaf->ptrs[leaf->nr] = NULL;
		smp_wmb();

		/* then bump the nr */
		leaf->nr++;

		/*
		 * now step through each pointer after our
		 * destination and bubble it forward.  memcpy
		 * would be faster but rcu searchers will be
		 * able to validate pointers as they go with
		 * this method.
		 */
		for (i = leaf->nr - 1; i > slot; i--) {
			leaf->keys[i] = leaf->keys[i - 1];
			leaf->ptrs[i] = leaf->ptrs[i - 1];
			/*
			 * make sure the key/pointer pair is
			 * fully visible in the new home before
			 * we move forward
			 */
			smp_wmb();
		}

		/* finally stuff in our key */
		leaf->keys[slot] = key;
		leaf->ptrs[slot] = slot_ptr;
		smp_wmb();
	} else {
		/*
		 * just extending the leaf, toss
		 * our key in and update things
		 */
		leaf->max = key + slot_ptr->size;
		leaf->keys[slot] = key;
		leaf->ptrs[slot] = slot_ptr;

		smp_wmb();
		leaf->nr++;
	}
	cache_leaf(leaf, cache);
	sl_unlock_node(&leaf->node);
	if (finish_split_insert)
		__link_pending_insert(list, &split->node, leaf->node.level + 1);
	return 0;
}

/*
 * helper function for insert.  This will either return an existing
 * key or insert a new slot into the list.  leaf must be locked.
 */
static noinline int find_or_add_key(struct sl_list *list, unsigned long key,
				    unsigned long size, struct sl_leaf *leaf,
				    struct sl_slot *slot_ptr,
				    int preload_token, struct sl_leaf **cache)
{
	int ret;
	int slot;

	if (key < leaf->max) {
		ret = skiplist_search_leaf(leaf, key, size, &slot);
		if (ret == 0) {
			ret = -EEXIST;
			cache_leaf(leaf, cache);
			sl_unlock_node(&leaf->node);
			goto out;
		}
	} else {
		slot = leaf->nr;
	}

	atomic_inc(&slot_ptr->refs);

	/* add_key_to_leaf unlocks the node */
	add_key_to_leaf(list, leaf, slot_ptr, key, slot, preload_token, cache);
	ret = 0;

out:
	return ret;
}

/*
 * pull a new leaf out of the prealloc area, and insert the slot/key into it
 */
static struct sl_leaf *alloc_leaf(struct sl_slot *slot_ptr, unsigned long key,
				  int preload_token)
{
	struct sl_leaf *leaf;
	struct skip_preload *skp;
	int level;

	skp = this_cpu_ptr(&skip_preloads);
	leaf = skp->preload[preload_token];
	skp->preload[preload_token] = NULL;
	level = leaf->node.level;

	leaf->keys[0] = key;
	leaf->ptrs[0] = slot_ptr;
	leaf->nr = 1;
	leaf->max = key + slot_ptr->size;
	return leaf;
}

/*
 * this returns with preempt disabled and the preallocation
 * area setup for a new insert.  To get there, it may or
 * may not allocate a new leaf for the next insert.
 *
 * If allocations are done, this will also try to preallocate a level 0
 * leaf, which allows us to optimize insertion by not placing two
 * adjacent nodes together with the same level.
 *
 * This returns < 0 on errors.  If everything works, it returns a preload
 * token which you should use when fetching your preallocated items.
 *
 * The token allows us to preallocate based on the current
 * highest level of the list.  For a list of level N, we won't allocate
 * higher than N + 1.
 */
int skiplist_preload(struct sl_list *list, gfp_t gfp_mask)
{
	struct skip_preload *skp;
	struct sl_leaf *leaf;
	struct sl_leaf *leaf0 = NULL;
	int alloc_leaf0 = 1;
	int level;
	int max_level = min_t(int, list->level + 1, SKIP_MAXLEVEL - 1);
	int token = max_level;

	preempt_disable();
	skp = this_cpu_ptr(&skip_preloads);
	if (max_level && !skp->preload[0])
		alloc_leaf0 = 1;

	if (skp->preload[max_level])
		return token;

	preempt_enable();
	level = skiplist_get_new_level(list, max_level);
	leaf = kmem_cache_alloc(slab_caches[level], gfp_mask);
	if (leaf == NULL)
		return -ENOMEM;

	if (alloc_leaf0)
		leaf0 = kmem_cache_alloc(slab_caches[0], gfp_mask);

	preempt_disable();
	skp = this_cpu_ptr(&skip_preloads);

	if (leaf0) {
		sl_init_leaf(leaf0, 0);
		if (skp->preload[0] == NULL) {
			skp->preload[0] = leaf0;
		} else {
			skiplist_put_leaf(leaf0);
		}
	}

	sl_init_leaf(leaf, level);
	if (skp->preload[max_level]) {
		skiplist_put_leaf(leaf);
		return token;
	}
	skp->preload[max_level] = leaf;

	return token;
}
EXPORT_SYMBOL(skiplist_preload);

/*
 * use the kernel prandom call to pick a new random level.  This
 * uses P = .50.  If you bump the SKIP_MAXLEVEL past 32 levels,
 * this function needs updating.
 */
int skiplist_get_new_level(struct sl_list *list, int max_level)
{
	int level = 0;
	unsigned long randseed;

	randseed = prandom_u32();

	while (randseed && (randseed & 1)) {
		randseed >>= 1;
		level++;
		if (level == max_level)
			break;
	}
	return (level >= SKIP_MAXLEVEL ? SKIP_MAXLEVEL - 1: level);
}
EXPORT_SYMBOL(skiplist_get_new_level);

/*
 * just return the level of the leaf we're going to use
 * for the next insert
 */
static int pending_insert_level(int preload_token)
{
	struct skip_preload *skp;
	skp = this_cpu_ptr(&skip_preloads);
	return skp->preload[preload_token]->node.level;
}

/*
 * after a lockless search, this makes sure a given key is still
 * inside the min/max of a leaf.  If not, you have to repeat the
 * search and try again.
 */
static int verify_key_in_leaf(struct sl_leaf *leaf, unsigned long key,
			      unsigned long size)
{
	if (sl_node_dead(&leaf->node))
		return 0;

	if (key + size < sl_min_key(leaf) ||
	    key >= sl_max_key(leaf))
		return 0;
	return 1;
}

/*
 * The insertion code tries to delay taking locks for as long as possible.
 * Once we've found a good place to insert, we need to make sure the leaf
 * we have picked is still a valid location.
 *
 * This checks the previous and next pointers to make sure everything is
 * still correct for the insert.  You should send an unlocked leaf, and
 * it will return 1 with the leaf locked if everything worked.
 *
 */
static int verify_key_in_path(struct sl_list *list,
			      struct sl_node *node, unsigned long key,
			      unsigned long size)
{
	struct sl_leaf *prev = NULL;
	struct sl_leaf *next;
	struct sl_node *p;
	struct sl_leaf *leaf = NULL;
	struct sl_node *lock1;
	struct sl_node *lock2;
	struct sl_node *lock3;
	int level = 0;
	int ret = -EAGAIN;

again:
	lock1 = NULL;
	lock2 = NULL;
	lock3 = NULL;
	if (node != list->head) {
		p = rcu_dereference(node->ptrs[level].prev);
		skiplist_wait_pending_insert(p);
		sl_lock_node(p);
		lock1 = p;

		sl_lock_node(node);
		lock2 = node;

		if (sl_node_dead(node))
			goto out;

		if (p->ptrs[level].next != node) {
			sl_unlock_node(lock1);
			sl_unlock_node(lock2);
			goto again;
		}

		if (sl_node_dead(p))
			goto out;


		if (p != list->head)
			prev = sl_entry(p);

		/*
		 * once we have the locks, make sure everyone
		 * still points to each other
		 */
		if (node->ptrs[level].prev != p) {
			sl_unlock_node(lock1);
			sl_unlock_node(lock2);
			goto again;
		}

		leaf = sl_entry(node);
	} else {
		sl_lock_node(node);
		lock2 = node;
	}

	/*
	 * rule #1, the key must be greater than the max of the previous
	 * leaf
	 */
	if (prev && key < sl_max_key(prev))
		goto out;

	/* we're done with prev, unlock it */
	sl_unlock_node(lock1);
	lock1 = NULL;

	/* rule #2 is the key must be smaller than the min key
	 * in the next node.  If the key is already smaller than
	 * the max key of this node, we know we're safe for rule #2
	 */
	if (leaf && key < sl_max_key(leaf))
		return 0;
again_next:
	p = node->ptrs[level].next;
	if (p)
		next = sl_entry(p);
	else
		next = NULL;

	if (next) {
		if (key >= sl_min_key(next))
			goto out;

		if (sl_node_inserting(&next->node)) {
			sl_unlock_node(lock2);
			skiplist_wait_pending_insert(&next->node);
			goto again;
		}
		sl_lock_node(&next->node);
		lock3 = &next->node;

		if (sl_node_dead(&next->node)) {
			sl_unlock_node(lock3);
			sl_unlock_node(lock2);
			goto again;
		}

		if (next->node.ptrs[level].prev != node) {
			sl_unlock_node(lock3);
			goto again_next;
		}

		/*
		 * rule #2 the key must be smaller than the min key
		 * in the next node
		 */
		if (key >= sl_min_key(next))
			goto out;

		if (key + size > sl_min_key(next)) {
			ret = -EEXIST;
			goto out;
		}
	}
	/*
	 * return with our leaf locked and sure that our leaf is the
	 * best place for this key
	 */
	sl_unlock_node(lock1);
	sl_unlock_node(lock3);
	return 0;

out:
	sl_unlock_node(lock1);
	sl_unlock_node(lock2);
	sl_unlock_node(lock3);
	return ret;
}


/*
 * Before calling this you must have stocked the preload area by
 * calling skiplist_preload, and you must have kept preemption
 * off.  preload_token comes from skiplist_preload, pass in
 * exactly what preload gave you.
 *
 * 'slot' will be inserted into the skiplist, returning 0
 * on success or < 0 on failure
 *
 * If 'cache' isn't null, we try to insert into cache first.
 * When we return a reference to the leaf where the insertion
 * was done is added.
 *
 * More details in the comments below.
 */
int skiplist_insert(struct sl_list *list, struct sl_slot *slot,
		    int preload_token, struct sl_leaf **cache)
{
	struct sl_node *p;
	struct sl_node *ins_locked = NULL;
	struct sl_leaf *leaf;
	unsigned long key = slot->key;
	unsigned long size = slot->size;
	unsigned long min_key;
	unsigned long max_key;
	int level;
	int ret;
	int err;

	rcu_read_lock();
	ret = -EEXIST;

	/* try our cache first */
	if (cache && *cache) {
		leaf = *cache;
		skiplist_wait_pending_insert(&leaf->node);
		err = verify_key_in_path(list, &leaf->node, key, size);
		if (err == -EEXIST) {
			ret = -EEXIST;
			goto fail;
		} else if (err == 0) {
			ins_locked = &leaf->node;
			level = 0;
			goto find_or_add;
		} else {
			invalidate_cache(cache);
		}
	}

again:
	p = list->head;
	level = list->level;

	do {
		while (1) {
			leaf = sl_next_leaf(list, p, level);
			if (!leaf) {
				skiplist_wait_pending_insert(p);
				/*
				 * if we're at level 0 and p points to
				 * the head, the list is just empty.  If
				 * we're not at level 0 yet, keep walking
				 * down.
				 */
				if (p == list->head || level != 0)
					break;

				err = verify_key_in_path(list, p, key, size);
				if (err == -EEXIST) {
					ret = -EEXIST;
					goto fail;
				} else if (err) {
					goto again;
				}

				leaf = sl_next_leaf(list, p, level);
				if (leaf) {
					sl_unlock_node(p);
					goto again;
				}

				/*
				 * p was the last leaf on the bottom level,
				 * We're here because 'key' was bigger than the
				 * max key in p.  find_or_add will append into
				 * the last leaf.
				 */
				ins_locked = p;
				goto find_or_add;
			}

			max_key = sl_max_key(leaf);

			/*
			 * strictly speaking this test is covered again below.
			 * But usually we have to walk forward through the
			 * pointers, so this is the most common condition.  Try
			 * it first.
			 */
			if (key >= max_key)
				goto next;

			min_key = sl_min_key(leaf);

			if (key < min_key) {
				/*
				 * our key is smaller than the smallest key in
				 * leaf.  If we're not in level 0 yet, we don't
				 * want to cross over into the leaf
				 */
				if (level != 0)
					break;

				p = &leaf->node;
				skiplist_wait_pending_insert(p);

				err = verify_key_in_path(list, p, key, size);
				if (err == -EEXIST) {
					ret = -EEXIST;
					goto fail;
				} else if (err) {
					goto again;
				}

				if (key >= sl_min_key(leaf)) {
					sl_unlock_node(p);
					goto again;
				}

				/*
				 * we are in level 0, prepend our key
				 * into this leaf
				 */
				ins_locked = p;
				goto find_or_add;
			}

			if (key < sl_max_key(leaf)) {
				/*
				 * our key is smaller than the max
				 * and bigger than the min, this is
				 * the one true leaf for our key no
				 * matter what level we're in
				 */
				p = &leaf->node;
				skiplist_wait_pending_insert(p);
				sl_lock_node(p);

				if (sl_node_dead(p) ||
				    key >= sl_max_key(leaf) ||
				    key < sl_min_key(leaf)) {
					sl_unlock_node(p);
					goto again;
				}
				ins_locked = p;
				goto find_or_add;
			}
next:
			p = &leaf->node;
		}

		level--;
	} while (level >= 0);

	/*
	 * we only get here if the list is completely empty.  FIXME
	 * this can be folded into the find_or_add code below
	 */

	sl_lock_node(list->head);
	if (list->head->ptrs[0].next != NULL) {
		sl_unlock_node(list->head);
		goto again;
	}
	atomic_inc(&slot->refs);
	leaf = alloc_leaf(slot, key, preload_token);
	level = leaf->node.level;
	leaf->node.pending = SKIPLIST_PENDING_INSERT;
	sl_lock_node(&leaf->node);

	if (level > list->level)
		list->level++;

	cache_leaf(leaf, cache);

	/* unlocks the leaf and the list head */
	sl_link_after_node(list, &leaf->node, list->head, level);
	ret = 0;
	rcu_read_unlock();

	return ret;

find_or_add:

	leaf = sl_entry(ins_locked);

	/* ins_locked is unlocked */
	ret = find_or_add_key(list, key, size, leaf, slot,
			      preload_token, cache);
fail:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL(skiplist_insert);

/*
 * lookup has two stages.  First we find the leaf that should have
 * our key, and then we go through all the slots in that leaf and
 * look for the key.  This helper function is just the first stage
 * and it must be called under rcu_read_lock().  You may be using the
 * non-rcu final lookup variant, but this part must be rcu.
 *
 * We'll return NULL if we find nothing or the candidate leaf
 * for you to search.
 *
 * last is set to one leaf before the leaf we checked.  skiplist_insert_hole
 * uses this to search for ranges.
 */
struct sl_leaf *__skiplist_lookup_leaf(struct sl_list *list,
				       struct sl_node **last,
				       unsigned long key,
				       unsigned long size)
{
	struct sl_node *p;
	struct sl_leaf *leaf;
	int level;
	struct sl_leaf *leaf_ret = NULL;
	unsigned long max_key = 0;
	unsigned long min_key = 0;

	level = list->level;
	p = list->head;
	do {
		while (1) {
			leaf = sl_next_leaf(list, p, level);
			if (!leaf) {
				if (sl_node_inserting(p)) {
					skiplist_wait_pending_insert(p);
					continue;
				}
				break;
			}
			max_key = sl_max_key(leaf);

			if (key >= max_key)
				goto next;

			min_key = sl_min_key(leaf);

			if (key < min_key)
				break;

			if (key < max_key) {
				leaf_ret = leaf;
				goto done;
			}
next:
			p = &leaf->node;
		}
		level--;
	} while (level >= 0);

done:
	if (last)
		*last = p;
	return leaf_ret;
}

/*
 * return the first leaf containing this key/size.  NULL
 * is returned if we find nothing.  Must be called under
 * rcu_read_lock.
 */
struct sl_leaf *skiplist_lookup_leaf_rcu(struct sl_list *list,
				     unsigned long key,
				     unsigned long size)
{
	return __skiplist_lookup_leaf(list, NULL, key, size);
}
EXPORT_SYMBOL(skiplist_lookup_leaf_rcu);

/*
 * returns a locked leaf that contains key/size, or NULL
 */
struct sl_leaf *skiplist_lookup_leaf(struct sl_list *list,
				     unsigned long key,
				     unsigned long size)

{
	struct sl_leaf *leaf;
	rcu_read_lock();
again:
	leaf = __skiplist_lookup_leaf(list, NULL, key, size);
	if (leaf) {
		sl_lock_node(&leaf->node);
		if (!verify_key_in_leaf(leaf, key, size)) {
			sl_unlock_node(&leaf->node);
			goto again;
		}
	}
	rcu_read_unlock();
	return leaf;
}
EXPORT_SYMBOL(skiplist_lookup_leaf);

/*
 * returns a locked leaf that might contain key/size.  An
 * extra reference is taken on the leaf we return
 */
struct sl_leaf *skiplist_lookup_first_leaf(struct sl_list *list,
					   unsigned long key,
					   unsigned long size)

{
	struct sl_leaf *leaf;
	struct sl_node *last = NULL;
	rcu_read_lock();
again:
	leaf = __skiplist_lookup_leaf(list, &last, key, size);
	if (leaf) {
		smp_rmb();
		skiplist_wait_pending_insert(&leaf->node);
		sl_lock_node(&leaf->node);
		if (!verify_key_in_leaf(leaf, key, size)) {
			sl_unlock_node(&leaf->node);
			goto again;
		}
		skiplist_get_leaf(leaf);
	} else if (last && last != list->head) {
		smp_rmb();
		if (sl_node_dead(last))
			goto again;

		skiplist_wait_pending_insert(last);
		sl_lock_node(last);

		if (sl_node_dead(last)) {
			sl_unlock_node(last);
			goto again;
		}
		leaf = sl_entry(last);
		skiplist_get_leaf(leaf);
	}
	rcu_read_unlock();
	return leaf;
}
EXPORT_SYMBOL(skiplist_lookup_first_leaf);

/*
 * rcu_read_lock must be held.  This returns a leaf that may
 * contain key/size
 */
struct sl_leaf *skiplist_lookup_first_leaf_rcu(struct sl_list *list,
					       unsigned long key,
					       unsigned long size)

{
	struct sl_leaf *leaf;
	struct sl_node *last;

again:
	last = NULL;
	leaf = __skiplist_lookup_leaf(list, &last, key, size);
	if (leaf) {
		return leaf;
	} else if (last && last != list->head) {
		if (sl_node_dead(last))
			goto again;
		return sl_entry(last);
	}
	return NULL;
}
EXPORT_SYMBOL(skiplist_lookup_first_leaf_rcu);

/*
 * given a locked leaf, this returns the next leaf in the
 * skiplist (locked)
 */
struct sl_leaf *skiplist_next_leaf(struct sl_list *list,
				   struct sl_leaf *leaf)
{
	struct sl_leaf *next_leaf = NULL;
	struct sl_node *node;

	rcu_read_lock();
	while (1) {
		node = find_live_next(list, &leaf->node, 0);
		if (!node)
			break;

		next_leaf = sl_entry(node);

		sl_lock_node(node);
		if (!sl_node_dead(node) &&
		    find_live_prev(list, node, 0) == &leaf->node) {
			skiplist_get_leaf(next_leaf);
			break;
		}

		sl_unlock_node(node);
	}
	rcu_read_unlock();

	return next_leaf;
}
EXPORT_SYMBOL(skiplist_next_leaf);

struct sl_leaf *skiplist_next_leaf_rcu(struct sl_list *list,
				   struct sl_leaf *leaf)
{
	struct sl_node *next;

	next = find_live_next(list, &leaf->node, 0);
	if (next)
		return sl_entry(next);
	return NULL;
}
EXPORT_SYMBOL(skiplist_next_leaf_rcu);

/*
 * this lookup function expects RCU to protect the slots in the leaves
 * as well as the skiplist indexing structures
 *
 * Note, you must call this with rcu_read_lock held, and you must verify
 * the result yourself.  If the key field of the returned slot doesn't
 * match your key, repeat the lookup.  Reference counting etc is also
 * all your responsibility.
 */
struct sl_slot *skiplist_lookup_rcu(struct sl_list *list, unsigned long key,
				    unsigned long size)
{
	struct sl_leaf *leaf;
	struct sl_slot *slot_ret = NULL;
	int slot;
	int ret;

again:
	leaf = __skiplist_lookup_leaf(list, NULL, key, size);
	if (leaf) {
		ret = skiplist_search_leaf(leaf, key, size, &slot);
		if (ret == 0)
			slot_ret = rcu_dereference(leaf->ptrs[slot]);
		else if (!verify_key_in_leaf(leaf, key, size))
			goto again;
	}
	return slot_ret;

}
EXPORT_SYMBOL(skiplist_lookup_rcu);

/*
 * this lookup function only uses RCU to protect the skiplist indexing
 * structs.  The actual slots are protected by full locks.
 */
struct sl_slot *skiplist_lookup(struct sl_list *list, unsigned long key,
				unsigned long size)
{
	struct sl_leaf *leaf;
	struct sl_slot *slot_ret = NULL;
	struct sl_node *p;
	int slot;
	int ret;

again:
	rcu_read_lock();
	leaf = __skiplist_lookup_leaf(list, &p, key, size);
	if (leaf) {
		sl_lock_node(&leaf->node);
		if (!verify_key_in_leaf(leaf, key, size)) {
			sl_unlock_node(&leaf->node);
			rcu_read_unlock();
			goto again;
		}
		ret = skiplist_search_leaf(leaf, key, size, &slot);
		if (ret == 0) {
			slot_ret = leaf->ptrs[slot];
			if (atomic_inc_not_zero(&slot_ret->refs) == 0)
				slot_ret = NULL;
		}
		sl_unlock_node(&leaf->node);
	}
	rcu_read_unlock();
	return slot_ret;

}
EXPORT_SYMBOL(skiplist_lookup);

/* helper for skiplist_insert_hole.  the iommu requires alignment */
static unsigned long align_start(unsigned long val, unsigned long align)
{
	return (val + align - 1) & ~(align - 1);
}

/*
 * this is pretty ugly, but it is used to find a free spot in the
 * tree for a new iommu allocation.  We start from a given
 * hint and try to find an aligned range of a given size.
 *
 * Send the slot pointer, and we'll update it with the location
 * we found.
 *
 * This will return -EAGAIN if we found a good spot but someone
 * raced in and allocated it before we could.  This gives the
 * caller the chance to update their hint.
 *
 * This will return -EEXIST if we couldn't find anything at all
 *
 * returns 0 if all went well, or some other negative error
 * if things went badly.
 */
int skiplist_insert_hole(struct sl_list *list, unsigned long hint,
			 unsigned long limit,
			 unsigned long size, unsigned long align,
			 struct sl_slot *slot,
			 gfp_t gfp_mask)
{
	unsigned long last_end = 0;
	struct sl_node *p;
	struct sl_leaf *leaf;
	int i;
	int ret = -EEXIST;
	int preload_token;
	int pending_level;

	preload_token = skiplist_preload(list, gfp_mask);
	if (preload_token < 0) {
		return preload_token;
	}
	pending_level = pending_insert_level(preload_token);

	/* step one, lets find our hint */
	rcu_read_lock();
again:

	last_end = max(last_end, hint);
	last_end = align_start(last_end, align);
	slot->key = align_start(hint, align);
	slot->size = size;
	leaf = __skiplist_lookup_leaf(list, &p, hint, 1);
	if (!p)
		p = list->head;

	if (leaf && !verify_key_in_leaf(leaf, hint, size)) {
		goto again;
	}

again_lock:
	sl_lock_node(p);
	if (sl_node_dead(p)) {
		sl_unlock_node(p);
		goto again;
	}

	if (p != list->head) {
		leaf = sl_entry(p);
		/*
		 * the leaf we found was past the hint,
		 * go back one
		 */
		if (sl_max_key(leaf) > hint) {
			struct sl_node *locked = p;
			p = p->ptrs[0].prev;
			sl_unlock_node(locked);
			goto again_lock;
		}
		last_end = align_start(sl_max_key(sl_entry(p)), align);
	}

	/*
	 * now walk at level 0 and find a hole.  We could use lockless walks
	 * if we wanted to bang more on the insertion code, but this
	 * instead holds the lock on each node as we inspect it
	 *
	 * This is a little sloppy, insert will return -eexist if we get it
	 * wrong.
	 */
	while(1) {
		leaf = sl_next_leaf(list, p, 0);
		if (!leaf)
			break;

		/* p and leaf are locked */
		sl_lock_node(&leaf->node);
		if (last_end > sl_max_key(leaf))
			goto next;

		for (i = 0; i < leaf->nr; i++) {
			if (last_end > leaf->keys[i])
				continue;
			if (leaf->keys[i] - last_end >= size) {

				if (last_end + size > limit) {
					sl_unlock_node(&leaf->node);
					goto out_rcu;
				}

				sl_unlock_node(p);
				slot->key = last_end;
				slot->size = size;
				goto try_insert;
			}
			last_end = leaf->keys[i] + leaf->ptrs[i]->size;
			last_end = align_start(last_end, align);
			if (last_end + size > limit) {
				sl_unlock_node(&leaf->node);
				goto out_rcu;
			}
		}
next:
		sl_unlock_node(p);
		p = &leaf->node;
	}

	if (last_end + size <= limit) {
		sl_unlock_node(p);
		slot->key = last_end;
		slot->size = size;
		goto try_insert;
	}

out_rcu:
	/* we've failed */
	sl_unlock_node(p);
	rcu_read_unlock();
	preempt_enable();

	return ret;

try_insert:
	/*
	 * if the pending_level is zero or there is room in the
	 * leaf, we're ready to insert.  This is true most of the
	 * time, and we won't have to drop our lock and give others
	 * the chance to race in and steal our spot.
	 */
	if (leaf && (pending_level == 0 || leaf->nr < SKIP_KEYS_PER_NODE) &&
	    !sl_node_dead(&leaf->node) && (slot->key >= sl_min_key(leaf) &&
	    slot->key + slot->size <= sl_max_key(leaf))) {
		ret = find_or_add_key(list, slot->key, size, leaf, slot,
				      preload_token, NULL);
		rcu_read_unlock();
		goto out;
	}
	/*
	 * no such luck, drop our lock and try the insert the
	 * old fashioned way
	 */
	if (leaf)
		sl_unlock_node(&leaf->node);

	rcu_read_unlock();
	ret = skiplist_insert(list, slot, preload_token, NULL);

out:
	/*
	 * if we get an EEXIST here, it just means we lost the race.
	 * return eagain to the caller so they can update the hint
	 */
	if (ret == -EEXIST)
		ret = -EAGAIN;

	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(skiplist_insert_hole);

/*
 * we erase one level at a time, from top to bottom.
 * The basic idea is to find a live prev and next pair,
 * and make them point to each other.
 *
 * For a given level, this takes locks on prev, node, next
 * makes sure they all point to each other and then
 * removes node from the middle.
 *
 * The node must already be marked dead and it must already
 * be empty.
 */
static void erase_one_level(struct sl_list *list,
			    struct sl_node *node, int level)
{
	struct sl_node *prev;
	struct sl_node *next;
	struct sl_node *test_prev;
	struct sl_node *test_next;

again:
	prev = find_live_prev(list, node, level);
	skiplist_wait_pending_insert(prev);
	sl_lock_node(prev);

	test_prev = find_live_prev(list, node, level);
	if (test_prev != prev || sl_node_dead(prev)) {
		sl_unlock_node(prev);
		goto again;
	}

again_next:
	next = find_live_next(list, prev, level);
	if (next) {
		if (sl_node_inserting(next)) {
			sl_unlock_node(prev);
			skiplist_wait_pending_insert(next);
			goto again;
		}
		sl_lock_node(next);
		test_next = find_live_next(list, prev, level);
		if (test_next != next || sl_node_dead(next)) {
			sl_unlock_node(next);
			goto again_next;
		}
		test_prev = find_live_prev(list, next, level);
		test_next = find_live_next(list, prev, level);
		if (test_prev != prev || test_next != next) {
			sl_unlock_node(prev);
			sl_unlock_node(next);
			goto again;
		}
	}
	rcu_assign_pointer(prev->ptrs[level].next, next);
	if (next)
		rcu_assign_pointer(next->ptrs[level].prev, prev);
	else if (prev != list->head)
		rcu_assign_pointer(list->head->ptrs[level].prev, prev);
	else
		rcu_assign_pointer(list->head->ptrs[level].prev, NULL);

	sl_unlock_node(prev);
	sl_unlock_node(next);
}

/*
 * for a marked dead, unlink all the levels.  The leaf must
 * not be locked
 */
void sl_erase(struct sl_list *list, struct sl_leaf *leaf)
{
	int i;
	int level = leaf->node.level;

	for (i = level; i >= 0; i--)
		erase_one_level(list, &leaf->node, i);
}

/*
 * helper for skiplist_delete, this pushes pointers
 * around to remove a single slot
 */
static void delete_slot(struct sl_leaf *leaf, int slot)
{
	if (slot != leaf->nr - 1) {
		int i;
		for (i = slot; i <= leaf->nr - 1; i++) {
			leaf->keys[i] = leaf->keys[i + 1];
			leaf->ptrs[i] = leaf->ptrs[i + 1];
			smp_wmb();
		}
	} else if (leaf->nr > 1) {
		leaf->max = leaf->keys[leaf->nr - 2] +
			leaf->ptrs[leaf->nr - 2]->size;
		smp_wmb();
	}
	leaf->nr--;
}

/*
 * find a given [key, size] in the skiplist and remove it.
 * If we find anything, we return the slot pointer that
 * was stored in the tree.
 *
 * deletion involves a mostly lockless lookup to
 * find the right leaf.  Then we take the lock and find the
 * correct slot.
 *
 * The slot is removed from the leaf, and if the leaf
 * is now empty, it is removed from the skiplist.
 */
struct sl_slot *skiplist_delete(struct sl_list *list,
				       unsigned long key,
				       unsigned long size)
{
	struct sl_slot *slot_ret = NULL;
	struct sl_leaf *leaf;
	int slot;
	int ret;

	rcu_read_lock();
again:
	leaf = __skiplist_lookup_leaf(list, NULL, key, size);
	if (!leaf)
		goto out;

	skiplist_wait_pending_insert(&leaf->node);

	sl_lock_node(&leaf->node);
	if (!verify_key_in_leaf(leaf, key, size)) {
		sl_unlock_node(&leaf->node);
		goto again;
	}

	ret = skiplist_search_leaf(leaf, key, size, &slot);
	if (ret == 0) {
		slot_ret = leaf->ptrs[slot];
	} else {
		sl_unlock_node(&leaf->node);
		goto out;
	}

	delete_slot(leaf, slot);
	if (leaf->nr == 0) {
		/*
		 * sl_erase has to mess wit the prev pointers, so
		 * we need to unlock it here
		 */
		leaf->node.pending = SKIPLIST_PENDING_DEAD;
		sl_unlock_node(&leaf->node);
		sl_erase(list, leaf);
		skiplist_put_leaf(leaf);
	} else {
		sl_unlock_node(&leaf->node);
	}
out:
	rcu_read_unlock();
	return slot_ret;
}
EXPORT_SYMBOL(skiplist_delete);

/*
 * this deletes an entire leaf, the caller must have some
 * other way to free all the slots that are linked in.  The
 * leaf must be locked.
 */
void skiplist_delete_leaf(struct sl_list *list, struct sl_leaf *leaf,
			  struct sl_leaf **cache_next)
{
	struct sl_node *next = NULL;

	rcu_read_lock();
	assert_locked_node(&leaf->node);
	leaf->nr = 0;

	BUG_ON(sl_node_inserting(&leaf->node));

	leaf->node.pending = SKIPLIST_PENDING_DEAD;

	if (cache_next) {
		*cache_next = NULL;
		next = find_live_next(list, &leaf->node, 0);
	}

	sl_unlock_node(&leaf->node);
	sl_erase(list, leaf);
	/* once for the skiplist */
	skiplist_put_leaf(leaf);
	/* once for the caller */
	skiplist_put_leaf(leaf);

	if (cache_next && next && !sl_node_dead(next)) {
		int ret;

		leaf = sl_entry(next);
		ret = skiplist_get_leaf_not_zero(leaf);
		if (ret)
			*cache_next = leaf;
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(skiplist_delete_leaf);

int sl_init_list(struct sl_list *list, gfp_t mask)
{
	list->head = kmalloc(sl_node_size(SKIP_MAXLEVEL), mask);
	if (!list->head)
		return -ENOMEM;
	sl_init_node(list->head, SKIP_MAXLEVEL);
	list->level = 0;
	return 0;
}
EXPORT_SYMBOL(sl_init_list);


static int skiplist_callback(struct notifier_block *nfb,
			     unsigned long action,
			     void *hcpu)
{
	int cpu = (long)hcpu;
	struct skip_preload *skp;
	struct sl_leaf *l;
	int level;
	int i;

	/* Free per-cpu pool of preloaded nodes */
	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		skp = &per_cpu(skip_preloads, cpu);
		for (i = 0; i < SKIP_MAXLEVEL + 1; i++) {
			l = skp->preload[i];
			if (!l)
				continue;
			level = l->node.level;
			kmem_cache_free(slab_caches[level], l);
			skp->preload[i] = NULL;
		}
	}
	return NOTIFY_OK;
}

void __init skiplist_init(void)
{
	char buffer[16];
	int i;

	hotcpu_notifier(skiplist_callback, 0);
	for (i = 0; i < SKIP_MAXLEVEL; i++) {
		snprintf(buffer, 16, "skiplist-%d", i);
		slab_caches[i] = kmem_cache_create(buffer,
				   sl_leaf_size(i), 0,
				   SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD |
				   SLAB_DESTROY_BY_RCU,
				   NULL);
	}
}

