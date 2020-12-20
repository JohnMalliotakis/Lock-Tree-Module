#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#include <linux/printk.h>
#include <linux/string.h>
#include <asm/bug.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include "aux_structs.h"

static struct rb_data *rb_data_lookup(struct rb_root *root, uint32_t offset)
{
	struct rb_node *index;

	index = root->rb_node;

	while(index){
		struct rb_data *data = container_of(index, struct rb_data, node);
		if(offset < data->offset)
			index = index->rb_left;
		else if(offset > data->offset)
			index = index->rb_right;
		else
			return data;
	}
	return NULL;
}

/* 
 * Auxiliary red black tree search and insert
 * functions, based on rb_data offset field
 */
static char *rb_data_search(struct rb_root *root, uint32_t offset)
{
	struct rb_data *found_node = rb_data_lookup(root, offset);
	return found_node ? found_node->str : NULL;
}

static int rb_data_insert(struct rb_root *root, char *str, uint32_t offset)
{
	struct rb_node **index, *parent = NULL;
	struct rb_data *newnode; 

	BUG_ON(str == NULL);

	index = &root->rb_node;

	/*
	 * GFP_ATOMIC flag is used because we cannot allow
	 * kmalloc to block while holding a lock
	 */
	newnode = kmalloc(sizeof(struct rb_data), GFP_ATOMIC);

	if(!newnode){
		pr_err("Memory allocation failed for RB tree node\n");
		return -1;
	}

	newnode->str = kmalloc((strlen(str) + 1) * sizeof(char), GFP_ATOMIC);
	if(!newnode->str){
		pr_err("Memory allocation failed for RB tree node string\n");
		kfree(newnode);
		return -1;
	}

	newnode->str = strcpy(newnode->str, str);
	newnode->offset = offset;

	while(*index){
		struct rb_data *data = container_of(*index, struct rb_data, node);
		parent = *index;

		if(offset < data->offset)
			index = &((*index)->rb_left);
		else if(offset > data->offset)
			index = &((*index)->rb_right);
		else
			/* Node already in rbtree */
			return -1;
	}

	/* Link new node and rebalance */
	rb_link_node(&newnode->node, parent, index);
	rb_insert_color(&newnode->node, root);
	return 0;
}

static int rb_data_erase(struct rb_root *root, uint32_t offset)
{
	struct rb_data *node_to_remove = rb_data_lookup(root, offset);

	if(node_to_remove){
		//pr_info("Deleted string %s from RB tree\n", node_to_remove->str);
		rb_erase(&node_to_remove->node, root);
		kfree(node_to_remove->str);
		kfree(node_to_remove);
		return 0;
	}
	return -1;
}

static void rb_data_destroy(struct rb_root *root)
{
	struct rb_data *del, *temp;
	/*
	 * Use kernel provided postorder
	 * traversal macro to destroy red black
	 * tree
	 */
	rbtree_postorder_for_each_entry_safe(del, temp, root, node){
		kfree(del->str);
		kfree(del);
	}
}


/* 
 * RCU tree call wrappers
 * Offset is stored in both
 * key and value of cb_kv
 * */

static char *rcu_tree_search(struct cb_root *root, uint32_t offset)
{
	struct cb_kv *found = cb_find(root, offset);
	return found ? (char *)(found->value) : NULL;
}

static int rcu_tree_insert(struct cb_root *root, char *str, uint32_t offset)
{
	char *data;

	BUG_ON(str == NULL);
	
	data = kmalloc((strlen(str) + 1) * sizeof(char), GFP_ATOMIC);

	if(!data){
		pr_err("Could not allocate memory for RCU tree node string\n");
		return -1;
	}
	data = strcpy(data, str);
	/*
	 * RCU tree is configured to cause a kernel
	 * panic upon error, so if we return from this
	 * call, all is good
	 */
	cb_insert(root, offset, (void *)data);
	return 0;
}

static int rcu_tree_erase(struct cb_root *root, uint32_t offset)
{
	char *deleted = (char *)cb_erase(root, offset);

	if(!deleted)
		return -1;

	//pr_info("Deleted string %s from RCU tree\n", deleted);
	kfree(deleted);
	return 0;
}

static void kv_destroy(struct cb_kv *kv)
{
	kfree(kv->value);
}

static void rcu_tree_destroy(struct cb_root *root)
{
	cb_destroy(root, kv_destroy);
}

void lt_init_lock(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	switch(lt->lock_type){
		case MUTEX:
			mutex_init(&(lt->lock.mlock));
			break;
		case RWLOCK:
			rwlock_init(&(lt->lock.rwlock));
			break;
		case SPINLOCK:
			spin_lock_init(&(lt->lock.slock));
			break;
		case RWSEM:
			init_rwsem(&(lt->lock.rwsem));
			break;
		default:
			BUG();
			break;
	}
}

void lt_init_tree(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	switch(lt->tree_type){
		case RB_TREE:
			lt->tree.rb_tree = RB_ROOT;
			break;
		case RCU_TREE:
			cb_init();
			lt->tree.rcu_tree = CB_ROOT;
			break;
		default:
			BUG();
			break;
	}
}

void lt_read_lock(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	/* Reader lock only necessary on red black tree */
	if(lt->tree_type != RCU_TREE){
		switch(lt->lock_type){
			/* On mutexes and spinlocks read and write locks are the same */
			case MUTEX:
				mutex_lock(&(lt->lock.mlock));
				break;
			case RWLOCK:
				read_lock(&(lt->lock.rwlock));
				break;
			case SPINLOCK:
				spin_lock(&(lt->lock.slock));
				break;
			case RWSEM:
				down_read(&(lt->lock.rwsem));
				break;
		}
	}
}

void lt_read_unlock(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	if(lt->tree_type != RCU_TREE){
		switch(lt->lock_type){
			case MUTEX:
				mutex_unlock(&(lt->lock.mlock));
				break;
			case RWLOCK:
				read_unlock(&(lt->lock.rwlock));
				break;
			case SPINLOCK:
				spin_unlock(&(lt->lock.slock));
				break;
			case RWSEM:
				up_read(&(lt->lock.rwsem));
				break;
		}
	}
}

void lt_write_lock(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	switch(lt->lock_type){
		case MUTEX:
			mutex_lock(&(lt->lock.mlock));
			break;
		case RWLOCK:
			write_lock(&(lt->lock.rwlock));
			break;
		case SPINLOCK:
			spin_lock(&(lt->lock.slock));
			break;
		case RWSEM:
			down_write(&(lt->lock.rwsem));
			break;
	}
}

void lt_write_unlock(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	switch(lt->lock_type){
		case MUTEX:
			mutex_unlock(&(lt->lock.mlock));
			break;
		case RWLOCK:
			write_unlock(&(lt->lock.rwlock));
			break;
		case SPINLOCK:
			spin_unlock(&(lt->lock.slock));
			break;
		case RWSEM:
			up_write(&(lt->lock.rwsem));
			break;
	}
}

char *lt_search(struct lock_tree *lt, uint32_t offset)
{
	BUG_ON(lt == NULL);

	if(lt->tree_type == RB_TREE)
		return rb_data_search(&(lt->tree.rb_tree), offset);
	return rcu_tree_search(&(lt->tree.rcu_tree), offset);
}

int lt_insert(struct lock_tree *lt, char *str, uint32_t offset)
{
	BUG_ON(lt == NULL);

	if(lt->tree_type == RB_TREE)
		return rb_data_insert(&(lt->tree.rb_tree), str, offset);
	return rcu_tree_insert(&(lt->tree.rcu_tree), str, offset);
}

int lt_erase(struct lock_tree *lt, uint32_t offset)
{
	BUG_ON(lt == NULL);

	if(lt->tree_type == RB_TREE)
		return rb_data_erase(&(lt->tree.rb_tree), offset);
	return rcu_tree_erase(&(lt->tree.rcu_tree), offset);
}

void lt_destroy_tree(struct lock_tree *lt)
{
	BUG_ON(lt == NULL);

	switch(lt->tree_type){
		case RB_TREE:
			rb_data_destroy(&(lt->tree.rb_tree));
			break;
		case RCU_TREE:
			rcu_tree_destroy(&(lt->tree.rcu_tree));
			break;
	}
}

void simple_barrier_init(struct simple_barrier *b, int num_threads)
{
	if(!b){
		pr_err("NULL barrier argument passed\n");
		return;
	}
	/* Initialize wait queue and set atomic counter */
	init_waitqueue_head(&(b->wq));
	atomic_set(&(b->counter), num_threads);
}

void simple_barrier_wait(struct simple_barrier *b)
{
	if(!b){
		pr_err("NULL barrier argument passed\n");
		return;
	}
	/* 
	 * Decrement and test barrier atomic
	 * If the result is false, then join the wait_queue,
	 * else wake up those sleeping in the wait queue
	 */
	if(atomic_dec_and_test(&(b->counter)) == false)
		wait_event_interruptible(b->wq, atomic_read(&(b->counter)) == 0);
	else
		wake_up_interruptible(&(b->wq));
}
