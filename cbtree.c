#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include "cbtree.h"

#define assert(s) BUG_ON(!(s))

// Sequential operations
#define SHARED(typ, name) struct { typ val; } name
#define SET(sh, v) ((sh).val = (v))
#define GET(sh) ((const __typeof__((sh).val)) (sh).val)

/******************************************************************
 * Tree types
 */

enum { INPLACE = 1 };

typedef uintptr_t k_t;

typedef struct cb_kv kv_t;

struct TreeBB_Node
{
        SHARED(struct TreeBB_Node *, left);
        SHARED(struct TreeBB_Node *, right);
        SHARED(unsigned int, size);

        kv_t kv;

        struct rcu_head rcu;
};

typedef struct TreeBB_Node node_t;

static inline node_t*
nodeOf(kv_t *kv)
{
        return (node_t*)((char*)kv - offsetof(node_t, kv));
}

/******************************************************************
 * Kernel adaptors
 */

static struct kmem_cache *node_cache;

/*
 * jmal: Changes to adapt to loadable
 * module: remove __init macro and core_initcall,
 * add cb_init wrapper to cbtree.h in order to create
 * node cache on tree initialization. kmem_cache_destroy
 * also added to tree destroyer. Alternatively, for module
 * use the node cache could be replaced with kernel alloc
 * calls
 */

int 
TreeBBInit(void)
{
        node_cache = kmem_cache_create("struct TreeBB_Node",
                                       sizeof(node_t), 0, SLAB_PANIC, NULL);
        return 0;
}

static node_t *
TreeBBNewNode(void)
{
        return kmem_cache_alloc(node_cache, GFP_ATOMIC);
}

static void
__TreeBBFreeNode(struct rcu_head *rcu)
{
        node_t *n = container_of(rcu, node_t, rcu);
        kmem_cache_free(node_cache, n);
}

static void
TreeBBFreeNode(node_t *n)
{
        call_rcu(&n->rcu, __TreeBBFreeNode);
}

/******************************************************************
 * Tree algorithms
 */

enum { WEIGHT = 4 };

static inline int
nodeSize(node_t *node)
{
        return node ? GET(node->size) : 0;
}

static inline node_t *
mkNode(node_t *left, node_t *right, kv_t *kv)
{
        node_t *node = TreeBBNewNode();
        SET(node->left, left);
        SET(node->right, right);
        SET(node->size, 1 + nodeSize(left) + nodeSize(right));
        node->kv = *kv;
        return node;
}

static node_t *
singleL(node_t *left, node_t *right, kv_t *kv)
{
//        printf("%s\n", __func__);
        node_t *res = mkNode(mkNode(left, GET(right->left), kv),
                             GET(right->right), &right->kv);
        TreeBBFreeNode(right);
        return res;
}

static node_t *
doubleL(node_t *left, node_t *right, kv_t *kv)
{
//        printf("%s\n", __func__);
        node_t *res = mkNode(mkNode(left, GET(GET(right->left)->left), kv),
                             mkNode(GET(GET(right->left)->right),
                                    GET(right->right),
                                    &right->kv),
                             &GET(right->left)->kv);
        TreeBBFreeNode(GET(right->left));
        TreeBBFreeNode(right);
        return res;
}

static node_t *
singleR(node_t *left, node_t *right, kv_t *kv)
{
//        printf("%s\n", __func__);
        node_t *res = mkNode(GET(left->left),
                             mkNode(GET(left->right), right, kv),
                             &left->kv);
        TreeBBFreeNode(left);
        return res;
}

static node_t *
doubleR(node_t *left, node_t *right, kv_t *kv)
{
//        printf("%s\n", __func__);
        node_t *res = mkNode(mkNode(GET(left->left),
                                    GET(GET(left->right)->left),
                                    &left->kv),
                             mkNode(GET(GET(left->right)->right), right, kv),
                             &GET(left->right)->kv);
        TreeBBFreeNode(GET(left->right));
        TreeBBFreeNode(left);
        return res;
}

static node_t *
mkBalancedL(node_t *left, node_t *right, kv_t *kv)
{
        int rln = nodeSize(GET(right->left)),
                rrn = nodeSize(GET(right->right));
        if (rln < rrn)
                return singleL(left, right, kv);
        return doubleL(left, right, kv);
}

static node_t *
mkBalancedR(node_t *left, node_t *right, kv_t *kv)
{
        int lln = nodeSize(GET(left->left)),
                lrn = nodeSize(GET(left->right));
        if (lrn < lln)
                return singleR(left, right, kv);
        return doubleR(left, right, kv);
}

/**
 * Create a balanced node from the given children to replace cur.  In
 * non-destructive mode, this always returns a new node.  In
 * in-place-mode, it may update cur directly if no rotations are
 * necessary; otherwise it returns a new node, which the caller must
 * arrange to swap in to the tree in place of cur.  replace should be
 * 0 if the left subtree of cur is being replaced, or 1 if the right
 * subtree of cur is being replaced.  inPlace specifies whether cur
 * can be modified in place.  This will always free any nodes that are
 * discarded (including cur if it gets replaced).
 *
 * This is written to be lightweight enough to get inlined if
 * 'replace' and 'inPlace' are compile-time constants.  Once inlined,
 * constant folding will eliminate most of the code.
 */
static inline node_t *
mkBalanced(node_t *cur, node_t *left, node_t *right, int replace, bool inPlace)
{
        int ln = nodeSize(left), rn = nodeSize(right);
        kv_t *kv = &cur->kv;
        node_t *res;

        if (ln+rn < 2)
                goto balanced;
        if (rn > WEIGHT * ln)
                res = mkBalancedL(left, right, kv);
        else if (ln > WEIGHT * rn)
                res = mkBalancedR(left, right, kv);
        else
                goto balanced;

        TreeBBFreeNode(cur);
        return res;

balanced:
        if (inPlace) {
                // When updating in-place, we only ever modify one of
                // the two pointers as our visible write.  We also
                // modify the size, but this is okay because size is
                // never in a reader's read set.
                if (replace == 0) {
                        assert(GET(cur->right) == right);
                        // XXX rcu_assign_pointer
			smp_wmb();
                        SET(cur->left, left);
                } else {
                        assert(GET(cur->left) == left);
                        // XXX rcu_assign_pointer
			smp_wmb();
                        SET(cur->right, right);
                }
                SET(cur->size, 1 + nodeSize(left) + nodeSize(right));
                return cur;
        } else {
                res = mkNode(left, right, kv);
                TreeBBFreeNode(cur);
                return res;
        }
}

static node_t *
insert(node_t *node, kv_t *kv)
{
        if (!node)
                return mkNode(NULL, NULL, kv);

        // Note that, even in in-place mode, we rebalance the tree
        // from the bottom up.  This has some nifty properties beyond
        // code simplicity.  In particular, the size fields get
        // updated from the bottom up, after we've performed the
        // insert or discovered it was unnecessary, which means we
        // don't need a second pass to decrement them if the element
        // turns out to be in the tree already.  However, this may be
        // incompatible with lazy write locking.  At the least, the
        // delayed updates to the size fields combined with competing
        // writers might result in tree imbalance.

        if (kv->key < node->kv.key)
                return mkBalanced(node, insert(GET(node->left), kv),
                                  GET(node->right), 0, INPLACE);
        if (kv->key > node->kv.key)
                return mkBalanced(node, GET(node->left),
                                  insert(GET(node->right), kv),
                                  1, INPLACE);
        return node;
}

void
TreeBB_Insert(struct cb_root *tree, uintptr_t key, void *value)
{
        kv_t kv = {key, value};
        node_t *nroot = insert(tree->root, &kv);
        rcu_assign_pointer(tree->root, nroot);
}

static node_t *
deleteMin(node_t *node, node_t **minOut)
{
        node_t *left = GET(node->left), *right = GET(node->right);
        if (!left) {
                *minOut = node;
                return right;
        }
        // This must always happen non-destructively because the goal
        // is to move the min element up in the tree, so the in-place
        // write must be performed up there to make this operation
        // atomic.  (Alternatively, a concurrent reader may already be
        // walking between the element being replaced and the min
        // element being removed, which means we must keep this min
        // element visible.)
        return mkBalanced(node, deleteMin(left, minOut), right, 0, false);
}

static node_t *
delete(node_t *node, k_t key, void **deleted)
{
        node_t *min, *left, *right;

        if (!node) {
                *deleted = NULL;
                return NULL;
        }

        left = GET(node->left);
        right = GET(node->right);
        if (key < node->kv.key)
                return mkBalanced(node, delete(left, key, deleted), right, 0, INPLACE);
        if (key > node->kv.key)
                return mkBalanced(node, left, delete(right, key, deleted), 1, INPLACE);

        // We found our node to delete
        *deleted = node->kv.value;
        TreeBBFreeNode(node);
        if (!left)
                return right;
        if (!right)
                return left;
        right = deleteMin(right, &min);
        // This needs to be performed non-destructively because the
        // min element is still linked in to the tree below us.  Thus,
        // we need to create a new min element here, which will be
        // atomically swapped in to the tree by our parent (along with
        // the new subtree where the min element has been removed).
        return mkBalanced(min, left, right, 1, false);
}

void *
TreeBB_Delete(struct cb_root *tree, uintptr_t key)
{
        void *deleted;
        node_t *nroot = delete(tree->root, key, &deleted);
        rcu_assign_pointer(tree->root, nroot);
        return deleted;
}

struct cb_kv *
TreeBB_Find(struct cb_root *tree, uintptr_t needle)
{
        node_t *node;

        node = tree->root;
        while (node) {
                if (node->kv.key == needle)
                        break;
                else if (node->kv.key > needle)
                        node = GET(node->left);
                else
                        node = GET(node->right);
        }
        return node ? &node->kv : NULL;
}

struct cb_kv *
TreeBB_FindGT(struct cb_root *tree, uintptr_t needle)
{
        node_t *node = tree->root;
        node_t *res = NULL;

        while (node) {
                if (node->kv.key > needle) {
                        res = node;
                        node = GET(node->left);
                } else {
                        node = GET(node->right);
                }
        }
        return res ? &res->kv : NULL;
}

struct cb_kv *
TreeBB_FindLE(struct cb_root *tree, uintptr_t needle)
{
        node_t *node = tree->root;
        node_t *res = NULL;

        while (node) {
                if (node->kv.key == needle)
                        return &node->kv;

                if (node->kv.key > needle) {
                        node = GET(node->left);
                } else {
                        res = node;
                        node = GET(node->right);
                }
        }
        return res ? &res->kv : NULL;
}

static void
foreach(node_t *node, void (*cb)(struct cb_kv *))
{
        if (!node)
                return;
        foreach(GET(node->left), cb);
        cb(&node->kv);
        foreach(GET(node->right), cb);
}

void
TreeBB_ForEach(struct cb_root *tree, void (*cb)(struct cb_kv *))
{
        foreach(tree->root, cb);
}

/* jmal: Add postorder tree destroyer */

static void
destroy_helper(node_t *node)
{
	if(!node)
		return;
	destroy_helper(GET(node->left));
	destroy_helper(GET(node->right));
	kmem_cache_free(node_cache, node);
}

void
TreeBB_Destroy(struct cb_root *tree, void (*kv_destroyer)(struct cb_kv *))
{
	node_t *node = tree->root;
	/* Call key-value pair destroyer on each tree node */
	if(kv_destroyer != NULL)
		foreach(node, kv_destroyer);
	/* Post order node freeing */
	destroy_helper(node);
	/* Destroy kmem cache created on tree init */
	kmem_cache_destroy(node_cache);
}
