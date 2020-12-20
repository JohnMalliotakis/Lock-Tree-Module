#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <asm/atomic.h>
#include "cbtree.h"

/* 
 * Enums for lock and tree type.
 * Theoretically this allows for a
 * more modular design, as you could
 * add your own locks and trees to these
 * enums and then modify the lock_tree
 * functions to add their cases
 */
typedef enum {
	MUTEX,
	RWLOCK,
	SPINLOCK,
	RWSEM
}LOCKTYPE_T;

typedef enum {
	RB_TREE,
	RCU_TREE
}TREETYPE_T;

/* 
 * Wrapper data structure for
 * rbtree configuration
 */
struct rb_data {
	char *str;
	uint32_t offset;
	struct rb_node node;
};

/* 
 * The RCU tree internally utilizes
 * a key value struct with the entry
 * offset and arbitrary (void *) data,
 * so we don't need a struct for it
 */

/* 
 * Controlling tree structure
 * Unions for different lock
 * types and underlying trees.
 * Wrapper functions check current
 * config and call appropriate functions
 */
struct lock_tree {
	union {
		struct mutex mlock;
		rwlock_t rwlock;
		spinlock_t slock;
		struct rw_semaphore rwsem;
	}lock;
	union {
		struct rb_root rb_tree;
		struct cb_root rcu_tree;
	}tree;
	LOCKTYPE_T lock_type;
	TREETYPE_T tree_type;
};

/* 
 * Lock-tree operations, transparency
 * achieved via union, one call for
 * all possible configurations, for
 * instance, on the RCU tree configurations
 * the reader lock/unlock functions do nothing
 */

/* Initialization */
void lt_init_lock(struct lock_tree *lt);
void lt_init_tree(struct lock_tree *lt);
/* Locks */
void lt_read_lock(struct lock_tree *lt);
void lt_read_unlock(struct lock_tree *lt);
void lt_write_lock(struct lock_tree *lt);
void lt_write_unlock(struct lock_tree *lt);
/* Trees */
char *lt_search(struct lock_tree *lt, uint32_t offset);
int lt_insert(struct lock_tree *lt, char *str, uint32_t offset);
int lt_erase(struct lock_tree *lt, uint32_t offset);
void lt_destroy_tree(struct lock_tree *lt);

/*
 * Simple barrier implementation
 * using an atomic along with a
 * wait queue. Not the most sophisticated
 * approach since the single atomic will
 * cause cache invalidations among contending
 * CPUs but good enough for now.
 */
struct simple_barrier {
	wait_queue_head_t wq;
	atomic_t counter;
};

/* Barrier Functions */
void simple_barrier_init(struct simple_barrier *b, int num_threads);
void simple_barrier_wait(struct simple_barrier *b);
