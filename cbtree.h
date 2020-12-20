/*
 *  Concurrent balanced trees
 */

#ifndef	_LINUX_CBTREE_H
#define	_LINUX_CBTREE_H

#include <linux/kernel.h>
#include <linux/stddef.h>

struct cb_root
{
	// XXX Should be SHARED(...)
        struct TreeBB_Node *root;
};

struct cb_kv
{
	uintptr_t key;
	void *value;
};

#define CB_ROOT	(struct cb_root) { NULL, }

#define CB_EMPTY_ROOT(cbroot)	(GET((cbroot)->root) == NULL)

static inline void
cb_insert(struct cb_root *tree, uintptr_t key, void *value)
{
	void TreeBB_Insert(struct cb_root *tree, uintptr_t key, void *value);
	TreeBB_Insert(tree, key, value);
}

static inline void *
cb_erase(struct cb_root *tree, uintptr_t key)
{
	void *TreeBB_Delete(struct cb_root *tree, uintptr_t key);
	return TreeBB_Delete(tree, key);
}

static inline struct cb_kv *
cb_find(struct cb_root *tree, uintptr_t needle)
{
	struct cb_kv *TreeBB_Find(struct cb_root *tree, uintptr_t needle);
	return TreeBB_Find(tree, needle);
}

static inline struct cb_kv *
cb_find_gt(struct cb_root *tree, uintptr_t needle)
{
	struct cb_kv *TreeBB_FindGT(struct cb_root *tree, uintptr_t needle);
	return TreeBB_FindGT(tree, needle);
}

static inline struct cb_kv *
cb_find_le(struct cb_root *tree, uintptr_t needle)
{
	struct cb_kv *TreeBB_FindLE(struct cb_root *tree, uintptr_t needle);
	return TreeBB_FindLE(tree, needle);
}

/*
 * jmal: Add for each wrapper, add tree
 * destroyer, add initialization function
 * to create tree node cache, in order to adapt
 * to loadable module
 */
static inline void
cb_for_each(struct cb_root *tree, void (*cb)(struct cb_kv *))
{
	void TreeBB_ForEach(struct cb_root *tree, void (*func)(struct cb_kv *));
	TreeBB_ForEach(tree, cb);
}

static inline void
cb_destroy(struct cb_root *tree, void (*kv_destroyer)(struct cb_kv *))
{
	void TreeBB_Destroy(struct cb_root *tree, void (*func)(struct cb_kv *));
	TreeBB_Destroy(tree, kv_destroyer);
}

/* 
 * XXX: Only call this once no matter how many trees you use!
 * This function creates a common kernel cache for all tree nodes
 */
static inline void
cb_init(void)
{
	int TreeBBInit(void);
	TreeBBInit();
}

#endif	/* _LINUX_CBTREE_H */
