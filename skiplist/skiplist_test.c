/*
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/skiplist.h>
#include <linux/kthread.h>
#include <linux/rbtree.h>
#include <linux/random.h>

static int threads = 1;
static int rounds = 100;
static int items = 100;
static int module_exiting;
static struct completion startup = COMPLETION_INITIALIZER(startup);
static DEFINE_MUTEX(fill_mutex);
static int filled;

static struct timespec *times;


#define FILL_TIME_INDEX 0
#define CHECK_TIME_INDEX 1
#define DEL_TIME_INDEX 2
#define RANDOM_INS_INDEX 3
#define RANDOM_LOOKUP_INDEX 4
#define FIRST_THREAD_INDEX 5

#define SKIPLIST_RCU_BENCH 1
#define SKIPLIST_BENCH 2
#define RBTREE_BENCH 3

static int benchmark = SKIPLIST_RCU_BENCH;

module_param(threads, int, 0);
module_param(rounds, int, 0);
module_param(items, int, 0);
module_param(benchmark, int, 0);

MODULE_PARM_DESC(threads, "number of threads to run");
MODULE_PARM_DESC(rounds, "how many random operations to run");
MODULE_PARM_DESC(items, "number of items to fill the list with");
MODULE_PARM_DESC(benchmark, "benchmark to run 1=skiplist-rcu 2=skiplist-locking 3=rbtree");
MODULE_LICENSE("GPL");

static atomic_t threads_running = ATOMIC_INIT(0);

/*
 * since the skiplist code is more concurrent, it is also more likely to
 * have races into the same slot during our delete/insert bashing run.
 * This makes counts the number of delete/insert pairs done so we can
 * make sure the results are roughly accurate
 */
static atomic_t pops_done = ATOMIC_INIT(0);

static struct kmem_cache *slot_cache;
struct sl_list skiplist;

spinlock_t rbtree_lock;
struct rb_root rb_root = RB_ROOT;

struct rbtree_item {
	struct rb_node rb_node;
	unsigned long key;
	unsigned long size;
};

static int __insert_one_rbtree(struct rb_root *root, struct rbtree_item *ins)
{
	struct rb_node **p = &root->rb_node;
	struct rb_node *parent = NULL;
	struct rbtree_item *item;
	unsigned long key = ins->key;
	int ret = -EEXIST;

	while (*p) {
		parent = *p;
		item = rb_entry(parent, struct rbtree_item, rb_node);

		if (key < item->key)
			p = &(*p)->rb_left;
		else if (key >= item->key + item->size)
			p = &(*p)->rb_right;
		else
			goto out;
	}

	rb_link_node(&ins->rb_node, parent, p);
	rb_insert_color(&ins->rb_node, root);
	ret = 0;
out:

	return ret;
}

static int insert_one_rbtree(struct rb_root *root, unsigned long key,
			     unsigned long size)
{
	int ret;
	struct rbtree_item *ins;

	ins = kmalloc(sizeof(*ins), GFP_KERNEL);
	ins->key = key;
	ins->size = size;

	spin_lock(&rbtree_lock);
	ret = __insert_one_rbtree(root, ins);
	spin_unlock(&rbtree_lock);

	if (ret) {
		kfree(ins);
	}
	return ret;
}

static struct rbtree_item *__lookup_one_rbtree(struct rb_root *root, unsigned long key)
{
	struct rb_node *p = root->rb_node;
	struct rbtree_item *item;
	struct rbtree_item *ret;

	while (p) {
		item = rb_entry(p, struct rbtree_item, rb_node);

		if (key < item->key)
			p = p->rb_left;
		else if (key >= item->key + item->size)
			p = p->rb_right;
		else {
			ret = item;
			goto out;
		}
	}

	ret = NULL;
out:
	return ret;
}

static struct rbtree_item *__lookup_first_rbtree(struct rb_root *root, unsigned long key)
{
	struct rb_node *p = root->rb_node;
	struct rbtree_item *item;
	struct rbtree_item *ret;

	while (p) {
		item = rb_entry(p, struct rbtree_item, rb_node);

		if (key < item->key)
			p = p->rb_left;
		else if (key >= item->key + item->size)
			p = p->rb_right;
		else {
			ret = item;
			goto out;
		}
	}

	if (p)
		ret = rb_entry(p, struct rbtree_item, rb_node);
	else
		ret = NULL;
out:
	return ret;
}

static int lookup_one_rbtree(struct rb_root *root, unsigned long key)
{
	struct rbtree_item *item;
	int ret;

	spin_lock(&rbtree_lock);
	item = __lookup_one_rbtree(root, key);
	if (item)
		ret = 0;
	else
		ret = -ENOENT;
	spin_unlock(&rbtree_lock);

	return ret;
}

static int delete_many_rbtree(struct rb_root *root, unsigned long key)
{
	int ret = 0;
	struct rbtree_item *item;
	struct rbtree_item **victims;
	struct rb_node *next;
	int nr_victims = 128;
	int found = 0;
	int i;

	nr_victims = min(nr_victims, items / 2);

	victims = kzalloc(nr_victims * sizeof(victims[0]), GFP_KERNEL);
	spin_lock(&rbtree_lock);
	item = __lookup_first_rbtree(root, key);
	if (!item) {
		spin_unlock(&rbtree_lock);
		goto out;
	}

	while (found < nr_victims) {
		victims[found] = item;
		next = rb_next(&item->rb_node);
		rb_erase(&item->rb_node, root);
		found++;
		if (!next)
			break;
		item = rb_entry(next, struct rbtree_item, rb_node);
	}
	spin_unlock(&rbtree_lock);

	for (i = 0; i < found; i++) {
		item = victims[i];
		spin_lock(&rbtree_lock);
		ret = __insert_one_rbtree(root, item);
		if (ret) {
			printk(KERN_CRIT "delete_many unable to insert %lu\n", key);
			kfree(item);
		}
		spin_unlock(&rbtree_lock);
	}
out:
	atomic_add(found, &pops_done);
	kfree(victims);
	return ret;

}

static int delete_one_rbtree(struct rb_root *root, unsigned long key)
{
	int ret = -ENOENT;
	struct rbtree_item *item;

	spin_lock(&rbtree_lock);
	item = __lookup_first_rbtree(root, key);
	if (!item)
		goto out;

	rb_erase(&item->rb_node, root);

	ret = __insert_one_rbtree(root, item);
	if (ret) {
		printk(KERN_CRIT "delete_one unable to insert %lu\n", key);
		goto out;
	}
	ret = 0;
out:
	spin_unlock(&rbtree_lock);
	atomic_inc(&pops_done);
	return ret;

}

static int run_initial_fill_rbtree(void)
{
	unsigned long i;
	unsigned long key;
	int ret;
	int inserted = 0;

	for (i = 0; i < items; i++) {
		key = i * 4096;
		ret = insert_one_rbtree(&rb_root, key, 4096);
		if (ret)
			return ret;
		inserted++;
	}
	printk("rbtree inserted %d items\n", inserted);
	return 0;
}

static unsigned long tester_random(void)
{
	return (prandom_u32() % items) * 4096;
}

static void random_insert_rbtree(void)
{
	unsigned long i;
	unsigned long key;
	int ret;
	int inserted = 0;

	for (i = 0; i < items; i++) {
		key = tester_random();
		ret = insert_one_rbtree(&rb_root, key, 4096);
		if (!ret)
			inserted++;
	}
	printk("rbtree radomly inserted %d items\n", inserted);
}

static void random_lookup_rbtree(void)
{
	int i;
	unsigned long key;
	int ret;
	int found = 0;

	for (i = 0; i < items; i++) {
		key = tester_random();
		ret = lookup_one_rbtree(&rb_root, key);
		if (!ret)
			found++;
	}
	printk("rbtree randomly searched %d items\n", found);
}

static void check_post_work_rbtree(void)
{
	unsigned long key = 0;
	int errors = 0;
	struct rb_node *p = rb_first(&rb_root);
	struct rbtree_item *item;

	while (p) {
		item = rb_entry(p, struct rbtree_item, rb_node);
		if (item->key != key) {
			if (!errors)
				printk("rbtree failed to find key %lu\n", key);
			errors++;
		}
		key += 4096;
		p = rb_next(p);
	}
	printk(KERN_CRIT "rbtree check found %d errors\n", errors);
}

static void delete_all_items_rbtree(int check)
{
	unsigned long key = 0;
	int errors = 0;
	struct rb_node *p = rb_first(&rb_root);
	struct rb_node *next;
	struct rbtree_item *item;

	while (p) {
		item = rb_entry(p, struct rbtree_item, rb_node);
		if (check && item->key != key) {
			if (!errors)
				printk("rbtree failed to find key %lu\n", key);
			errors++;
		}
		key += 4096;
		next = rb_next(p);
		rb_erase(p, &rb_root);
		kfree(item);
		p = next;
	}
	printk(KERN_CRIT "rbtree deletion found %d errors\n", errors);
}

static void put_slot(struct sl_slot *slot)
{
	if (atomic_dec_and_test(&slot->refs))
		kmem_cache_free(slot_cache, slot);

}
static int insert_one_skiplist(struct sl_list *skiplist, unsigned long key,
			       unsigned long size, struct sl_leaf **cache)
{
	int ret;
	int preload_token;
	struct sl_slot *slot;

	slot = kmem_cache_alloc(slot_cache, GFP_KERNEL);
	if (!slot)
		return -ENOMEM;

	slot->key = key;
	slot->size = size;
	atomic_set(&slot->refs, 0);

	preload_token = skiplist_preload(skiplist, GFP_KERNEL);
	if (preload_token < 0) {
		ret = preload_token;
		goto out;
	}

	ret = skiplist_insert(skiplist, slot, preload_token, cache);
	preempt_enable();

out:
	if (ret)
		kmem_cache_free(slot_cache, slot);

	return ret;
}

static int run_initial_fill_skiplist(void)
{
	unsigned long i;
	unsigned long key;
	int ret = 0;
	int inserted = 0;
	struct sl_leaf *cache = NULL;

	sl_init_list(&skiplist, GFP_KERNEL);

	for (i = 0; i < items; i++) {
		key = i * 4096;
		ret = insert_one_skiplist(&skiplist, key, 4096, &cache);
		if (ret)
			break;
		inserted++;
	}
	if (cache)
		skiplist_put_leaf(cache);

	printk("skiplist inserted %d items\n", inserted);
	return ret;
}

static void check_post_work_skiplist(void)
{
	unsigned long i;
	unsigned long key = 0;
	struct sl_slot *slot;
	struct sl_leaf *leaf;
	struct sl_leaf *next;
	int errors = 0;
	unsigned long found = 0;

	leaf = skiplist_first_leaf(&skiplist);
	while (leaf) {
		for (i = 0; i < leaf->nr; i++) {
			slot = leaf->ptrs[i];
			if (slot->key != key) {
				if (errors == 0)
					printk("key mismatch wanted %lu found %lu\n", key, slot->key);
				errors++;
			}
			key += 4096;
		}
		found += leaf->nr;
		next = skiplist_next_leaf(&skiplist, leaf);
		skiplist_unlock_leaf(leaf);
		skiplist_put_leaf(leaf);
		leaf = next;
	}
	if (found != items)
		printk("skiplist check only found %lu items\n", found);
	printk(KERN_CRIT "skiplist check found %lu items with %d errors\n", found, errors);
}

static void delete_all_items_skiplist(int check)
{
	unsigned long i;
	unsigned long key = 0;
	struct sl_slot *slot;
	struct sl_leaf *leaf;
	int errors = 0;
	unsigned long slots_empty = 0;
	unsigned long total_leaves = 0;
	unsigned long found = 0;

	/*
	 * the benchmark is done at this point, so we can safely
	 * just free all the items.  In the real world you should
	 * not free anything until the leaf is fully removed from
	 * the tree.
	 */
	while (1) {
		leaf = skiplist_first_leaf(&skiplist);
		if (!leaf)
			break;
		for (i = 0; i < leaf->nr; i++) {
			slot = leaf->ptrs[i];
			if (check && slot->key != key) {
				if (errors == 0)
					printk("delete key mismatch wanted %lu found %lu\n", key, slot->key);
				errors++;
			}
			key += 4096;
			/* delete the one ref from the skiplist */
			put_slot(slot);
		}
		found += leaf->nr;
		slots_empty += SKIP_KEYS_PER_NODE - leaf->nr;
		total_leaves++;
		skiplist_delete_leaf(&skiplist, leaf, NULL);
	}
	printk(KERN_CRIT "skiplist delete found %lu items with %d errors %lu empty slots %lu leaves\n", found, errors, slots_empty, total_leaves);
}

static int lookup_one_skiplist(struct sl_list *skiplist, unsigned long key)
{
	int ret = 0;
	struct sl_slot *slot = NULL;

	if (benchmark == SKIPLIST_RCU_BENCH) {
		slot = skiplist_lookup_rcu(skiplist, key, 4096);
	} else if (benchmark == SKIPLIST_BENCH) {
		slot = skiplist_lookup(skiplist, key, 4096);
		if (slot)
			put_slot(slot);
	}
	if (!slot)
		ret = -ENOENT;
	return ret;
}

static int delete_one_skiplist(struct sl_list *skiplist, unsigned long key)
{
	int ret = 0;
	struct sl_slot *slot = NULL;
	int preload_token;

	slot = skiplist_delete(skiplist, key, 1);
	if (!slot)
		return -ENOENT;

	preload_token = skiplist_preload(skiplist, GFP_KERNEL);
	if (preload_token < 0) {
		ret = preload_token;
		goto out_no_preempt;
	}

	ret = skiplist_insert(skiplist, slot, preload_token, NULL);
	if (ret) {
		printk(KERN_CRIT "failed to insert key %lu ret %d\n", key, ret);
		goto out;
	}
	put_slot(slot);
	ret = 0;
	atomic_inc(&pops_done);
out:
	preempt_enable();
out_no_preempt:
	return ret;
}

static int delete_many_skiplist(struct sl_list *skiplist, unsigned long key)
{
	int ret = 0;
	int preload_token;
	struct sl_slot **victims;
	struct sl_leaf *leaf;
	struct sl_leaf *next = NULL;
	int nr_victims = 128;
	int found = 0;
	int i;
	struct sl_leaf *cache = NULL;

	nr_victims = min(nr_victims, items / 2);

	victims = kzalloc(nr_victims * sizeof(victims[0]), GFP_KERNEL);
	/*
	 * this is intentionally deleting adjacent items to empty
	 * skiplist leaves.  The goal is to find races between
	 * leaf deletion and the rest of the code
	 */
	leaf = skiplist_lookup_first_leaf(skiplist, key, 1);
	while (leaf) {
		memcpy(victims + found, leaf->ptrs, sizeof(victims[0]) * leaf->nr);
		found += leaf->nr;

		skiplist_delete_leaf(skiplist, leaf, &next);
		leaf = next;
		if (!leaf)
			break;

		if (leaf->nr + found > nr_victims) {
			skiplist_put_leaf(leaf);
			break;
		}

		skiplist_wait_pending_insert(&leaf->node);

		skiplist_lock_leaf(leaf);
		if (sl_node_dead(&leaf->node) ||
		    leaf->nr + found > nr_victims) {
			skiplist_put_leaf(leaf);
			skiplist_unlock_leaf(leaf);
			break;
		}
	}

	for (i = 0; i < found; i++) {
		preload_token = skiplist_preload(skiplist, GFP_KERNEL);
		if (preload_token < 0) {
			ret = preload_token;
			goto out;
		}

		ret = skiplist_insert(skiplist, victims[i], preload_token, &cache);
		if (ret) {
			printk(KERN_CRIT "failed to insert key %lu ret %d\n", key, ret);
			preempt_enable();
			goto out;
		}
		/* insert added an extra ref, take it away here */
		put_slot(victims[i]);
		ret = 0;
		preempt_enable();
	}
out:
	if (cache)
		skiplist_put_leaf(cache);

	atomic_add(found, &pops_done);
	kfree(victims);
	return ret;
}

static void random_lookup_skiplist(void)
{
	int i;
	unsigned long key;
	int ret;
	int found = 0;

	for (i = 0; i < items; i++) {
		key = tester_random();
		ret = lookup_one_skiplist(&skiplist, key);
		if (!ret)
			found++;
	}
	printk("skiplist randomly searched %d items\n", found);
}

static void random_insert_skiplist(void)
{
	int i;
	unsigned long key;
	int ret;
	int inserted = 0;

	for (i = 0; i < items; i++) {
		key = tester_random();
		ret = insert_one_skiplist(&skiplist, key, 4096, NULL);
		if (!ret)
			inserted++;
	}
	printk("skiplist randomly inserted %d items\n", inserted);
}

void tvsub(struct timespec * tdiff, struct timespec * t1, struct timespec * t0)
{
	tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
	tdiff->tv_nsec = t1->tv_nsec - t0->tv_nsec;
	if (tdiff->tv_nsec < 0 && tdiff->tv_sec > 0) {
		tdiff->tv_sec--;
		tdiff->tv_nsec += 1000000000ULL;
	}

	/* time shouldn't go backwards!!! */
	if (tdiff->tv_nsec < 0 || t1->tv_sec < t0->tv_sec) {
		tdiff->tv_sec = 0;
		tdiff->tv_nsec = 0;
	}
}

static void pretty_time(struct timespec *ts, unsigned long long *seconds, unsigned long long *ms)
{
	unsigned long long m;

	*seconds = ts->tv_sec;

	m = ts->tv_nsec / 1000000ULL;
	*ms = m;
}

static void runbench(int thread_index)
{
	int ret = 0;
	unsigned long i;
	unsigned long key;
	struct timespec start;
	struct timespec cur;
	unsigned long long sec;
	unsigned long long ms;
	char *tag = "skiplist-rcu";

	if (benchmark == SKIPLIST_BENCH)
		tag = "skiplist-locking";
	else if (benchmark == RBTREE_BENCH)
		tag = "rbtree";

	mutex_lock(&fill_mutex);

	if (filled == 0) {
		start = current_kernel_time();

		printk(KERN_CRIT "Running %s benchmark\n", tag);

		if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
			ret = run_initial_fill_skiplist();
		else if (benchmark == RBTREE_BENCH)
			ret = run_initial_fill_rbtree();

		if (ret < 0) {
			printk(KERN_CRIT "failed to setup initial tree ret %d\n", ret);
			filled = ret;
		} else {
			filled = 1;
		}
		cur = current_kernel_time();
		tvsub(times + FILL_TIME_INDEX, &cur, &start);
		printk("initial fill done\n");
	}

	mutex_unlock(&fill_mutex);
	if (filled < 0)
		return;

	start = current_kernel_time();

	for (i = 0; i < rounds; i++) {
		key = tester_random();

		if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
			ret = lookup_one_skiplist(&skiplist, key);
		else if (benchmark == RBTREE_BENCH)
			ret = lookup_one_rbtree(&rb_root, key);

		cond_resched();

		key = tester_random();

		if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
			ret = delete_many_skiplist(&skiplist, key);
		else if (benchmark == RBTREE_BENCH)
			ret = delete_many_rbtree(&rb_root, key);

		cond_resched();

		key = tester_random();

		if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
			ret = delete_one_skiplist(&skiplist, key);
		else if (benchmark == RBTREE_BENCH)
			ret = delete_one_rbtree(&rb_root, key);

		cond_resched();
	}

	cur = current_kernel_time();
	tvsub(times + FIRST_THREAD_INDEX + thread_index, &cur, &start);

	if (!atomic_dec_and_test(&threads_running)) {
		return;
	}

	start = current_kernel_time();
	if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
		check_post_work_skiplist();
	else if (benchmark == RBTREE_BENCH)
		check_post_work_rbtree();

	cur = current_kernel_time();

	tvsub(times + CHECK_TIME_INDEX, &cur, &start);

	start = current_kernel_time();
	if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
		delete_all_items_skiplist(1);
	else if (benchmark == RBTREE_BENCH)
		delete_all_items_rbtree(1);
	cur = current_kernel_time();

	tvsub(times + DEL_TIME_INDEX, &cur, &start);

	start = current_kernel_time();
	if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH) {
		random_insert_skiplist();
	} else if (benchmark == RBTREE_BENCH) {
		random_insert_rbtree();
	}
	cur = current_kernel_time();

	tvsub(times + RANDOM_INS_INDEX, &cur, &start);

	start = current_kernel_time();
	if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH) {
		random_lookup_skiplist();
	} else if (benchmark == RBTREE_BENCH) {
		random_lookup_rbtree();
	}
	cur = current_kernel_time();

	tvsub(times + RANDOM_LOOKUP_INDEX, &cur, &start);

	if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH) {
		delete_all_items_skiplist(0);
	} else if (benchmark == RBTREE_BENCH) {
		delete_all_items_rbtree(0);
	}

	pretty_time(&times[FILL_TIME_INDEX], &sec, &ms);
	printk("%s fill time %llu s %llu ms\n", tag, sec, ms);
	pretty_time(&times[CHECK_TIME_INDEX], &sec, &ms);
	printk("%s check time %llu s %llu ms\n", tag, sec, ms);
	pretty_time(&times[DEL_TIME_INDEX], &sec, &ms);
	printk("%s del time %llu s %llu ms \n", tag, sec, ms);
	pretty_time(&times[RANDOM_INS_INDEX], &sec, &ms);
	printk("%s random insert time %llu s %llu ms \n", tag, sec, ms);
	pretty_time(&times[RANDOM_LOOKUP_INDEX], &sec, &ms);
	printk("%s random lookup time %llu s %llu ms \n", tag, sec, ms);
	for (i = 0; i < threads; i++) {
		pretty_time(&times[FIRST_THREAD_INDEX + i], &sec, &ms);
		printk("%s thread %lu time %llu s %llu ms\n", tag, i, sec, ms);
	}

	printk("worker thread pops done %d\n", atomic_read(&pops_done));

	kfree(times);
}

static int skiptest_thread(void *index)
{
	unsigned long thread_index = (unsigned long)index;
	complete(&startup);
	runbench(thread_index);
	complete(&startup);
	return 0;
}

static int __init skiptest_init(void)
{
	unsigned long i;
	init_completion(&startup);
	slot_cache = kmem_cache_create("skiplist_slot", sizeof(struct sl_slot), 0,
					   SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD |
					   SLAB_DESTROY_BY_RCU, NULL);

	if (!slot_cache)
		return -ENOMEM;

	spin_lock_init(&rbtree_lock);

	printk("skiptest benchmark module (%d threads) (%d items) (%d rounds)\n", threads, items, rounds);

	times = kmalloc(sizeof(times[0]) * (threads + FIRST_THREAD_INDEX),
			GFP_KERNEL);

	atomic_set(&threads_running, threads);
	for (i = 0; i < threads; i++) {
		kthread_run(skiptest_thread, (void *)i, "skiptest_thread");
	}
	for (i = 0; i < threads; i++)
		wait_for_completion(&startup);
	return 0;
}

static void __exit skiptest_exit(void)
{
	int i;
	module_exiting = 1;

	for (i = 0; i < threads; i++) {
		wait_for_completion(&startup);
	}

	synchronize_rcu();
	kmem_cache_destroy(slot_cache);
	printk("all skiptest threads done\n");
	return;
}

module_init(skiptest_init);
module_exit(skiptest_exit);
