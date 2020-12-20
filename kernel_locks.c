#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#include <linux/random.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/timekeeping.h>
#include <linux/random.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <asm/atomic.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include "aux_structs.h"

/*
 * XXX: Be careful!
 * The order of strings in this array must match the
 * enums defined in aux_struct.h for the parameter string
 * to be translated correctly. NULL is the last element of each
 * array so that we do not require a size parameter to know when done
 */
static char *possible_lock_types[] = {"MUTEX", "RWLOCK", "SPINLOCK", "RWSEM", NULL};
static char *possible_tree_types[] = {"RB_TREE", "RCU_TREE", NULL};

static unsigned int num_threads = 8;
static unsigned int num_ops = 1000000;
static char *lock_type = "SPINLOCK";
static char *tree_type = "RB_TREE";
static unsigned int del_ratio = 20;

/* 
 * Our module parameters are not visible to sysfs
 * since they are not meant to be configured at
 * runtime, one and done
 */
module_param(num_threads, uint, 0);
MODULE_PARM_DESC(num_threads, "Number of threads to run operations, default: 8");

module_param(num_ops, uint, 0);
MODULE_PARM_DESC(num_ops, "Number of operations to perform on each stage, default: 1000000");

module_param(lock_type, charp, 0);
MODULE_PARM_DESC(lock_type, "Locking mechanism to be used for operations, \
possible values: MUTEX, RWLOCK, SPINLOCK, RWSEM, default: SPINLOCK");

module_param(tree_type, charp, 0);
MODULE_PARM_DESC(tree_type, "Tree structure to be used for operations, \
possible values: RB_TREE, RCU_TREE, default: RB_TREE");

module_param(del_ratio, uint, 0);
MODULE_PARM_DESC(del_ratio, "Percentage of deletes in overall operations for \
lookup/delete stage, possible values: 0-100, default: 20");


/* Our lock-tree structure to be used for the experiment */
static struct lock_tree global_lt;

/*
 * We need some sort of barrier to 
 * synchronize our workers so that the
 * operations are correctly timed. A
 * simple barrier implementation is in the
 * aux_structs files. Its operation is explained
 * there.
 */
static struct simple_barrier stage_one;
static struct simple_barrier stage_two;
static struct simple_barrier finish;

/* Translation functions to go from string to enum */
static LOCKTYPE_T translate_lock_string(void)
{
	int i = 0;
	char *type;
	while(possible_lock_types[i]){
		type = possible_lock_types[i];
		if(!strncmp(lock_type, type, strlen(type)))
			break;
		i++;
	}
	/* Was the type found? */
	if(!possible_lock_types[i]){
		pr_err("Invalid lock type string, falling back to default SPINLOCK\n");
		return SPINLOCK;
	}
	return (LOCKTYPE_T)i;
}

static TREETYPE_T translate_tree_string(void)
{
	int i = 0;
	char *type;
	while(possible_tree_types[i]){
		type = possible_tree_types[i];
		if(!strncmp(tree_type, type, strlen(type)))
			break;
		i++;
	}
	/* Was the type found? */
	if(!possible_tree_types[i]){
		pr_err("Invalid tree type string, falling back to default RB_TREE\n");
		return RB_TREE;
	}
	return (TREETYPE_T)i;
}

/* 
 * First stage: Each thread inserts
 * num_ops/num_threads entries on the tree
 * The key used is the linear offset based 
 * on the thread id, and the data is the 
 * string "dummy_data"
 *
 * Second stage: Each thread performs lookups/deletes
 * randomly, while adhering to the global delete ratio,
 * and chooses a random offset for the operation
 */
static int tree_operation_thread(void *arg)
{
	int id = *(int *)arg;
	unsigned int i, per_thread_ops = num_ops / num_threads;
	unsigned int rand_op, rand_offset, deletes_remaining;
	char *found_str;
	/* ns accuracy kernel timers */	
	ktime_t time_start, time_done, time_diff;

	/*
	 * Add remainder to last thread if necessary
	 */
	if(id == num_threads - 1)
		per_thread_ops += num_ops % num_threads;
	deletes_remaining = per_thread_ops * del_ratio / 100;

	/* Begin first stage in a coordinated manner */
	simple_barrier_wait(&stage_one);

	if(!id)
		time_start = ktime_get();

	/* Start first stage */
	for(i=0;i<per_thread_ops;i++){
		lt_write_lock(&global_lt);
		lt_insert(&global_lt, "dummy_data", (id * i) + 1);
		lt_write_unlock(&global_lt);
	}

	/* 
	 * Synchronize to start second stage,
	 * coordinator must also time things
	 */
	simple_barrier_wait(&stage_two);
	if(!id){
		time_done = ktime_get();
		time_diff = ktime_sub(time_done, time_start);
		pr_info("Insert stage took %lld ms\n", ktime_to_ms(time_diff));
		time_start = ktime_get();
	}

	/* Start second stage */
	for(i=0;i<per_thread_ops;i++){
		rand_op = get_random_int() % 2;
		rand_offset = (get_random_int() % num_ops) + 1;
		/* 
		 * 0 for lookup, 1 for delete 
		 * Only delete as long as it is possible
		 * while conforming to the ratio parameter
		 */
		if(rand_op && deletes_remaining){
			deletes_remaining--;
			lt_write_lock(&global_lt);
			lt_erase(&global_lt, rand_offset);
			lt_write_unlock(&global_lt);
		}else{
			lt_read_lock(&global_lt);
			found_str = lt_search(&global_lt, rand_offset);
			lt_read_unlock(&global_lt);
		}
	}

	/* Synchronize to complete together */
	simple_barrier_wait(&finish);

	if(!id){
		time_done = ktime_get();
		time_diff = ktime_sub(time_done, time_start);
		pr_info("Search/Erase stage took %lld ms\n", ktime_to_ms(time_diff));
	}
	return 0;
}

static int __init kernel_locks_init(void)
{
	int i, *thread_ids;
	struct task_struct **workers;

	/* Setup our lock-tree structure */
	global_lt.lock_type = translate_lock_string();
	global_lt.tree_type = translate_tree_string();

	if(del_ratio > 100){
		pr_err("Invalid delete ratio argument, defaulting to 20%%\n");
		del_ratio = 20;
	}

	lt_init_lock(&global_lt);
	lt_init_tree(&global_lt);

	/* Initialize barriers */
	simple_barrier_init(&stage_one, num_threads);
	simple_barrier_init(&stage_two, num_threads);
	simple_barrier_init(&finish, num_threads);

	/* Allocate memory for worker ids and task pointers */
	thread_ids = kmalloc(num_threads * sizeof(*thread_ids), GFP_KERNEL);
	if(!thread_ids){
		pr_err("Could not kmalloc thread id array\n");
		return -1;
	}

	workers = kmalloc((num_threads - 1) * sizeof(*workers), GFP_KERNEL);
	if(!workers){
		pr_err("Could not kmalloc task_struct pointer array\n");
		kfree(thread_ids);
		return -1;
	}

	for(i = 1;i < num_threads;i++){
		thread_ids[i] = i;
		workers[i - 1] = kthread_create(tree_operation_thread, &thread_ids[i],
				"lock_tree_worker-%d", i);

		if(IS_ERR(workers[i - 1])){
			int j;
			pr_err("kthread_create failed for worker %d, aborting\n", i);
			/* 
			 * All workers should be stuck waiting for the coordinator
			 * on the first barrier so this should be good
			 */
			for(j=1;j<i;j++)
				kthread_stop(workers[j-1]);
			kfree(thread_ids);
			kfree(workers);
			return -1;
		}
		/* Round-robin CPU bind, then wakeup */
		kthread_bind(workers[i - 1], i % num_online_cpus());
		wake_up_process(workers[i - 1]);
	}
	/* Actual module process becomes coordinator */
	thread_ids[0] = 0;
	tree_operation_thread((void *)&thread_ids[0]);
	kfree(thread_ids);
	kfree(workers);
	return 0;
}

static void __exit kernel_locks_exit(void)
{
	lt_destroy_tree(&global_lt);
}

module_init(kernel_locks_init);
module_exit(kernel_locks_exit);

MODULE_DESCRIPTION("Lock and tree testing module");
MODULE_AUTHOR("John Malliotakis");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");


