/*
 * Copyright (C) 2011 STRATO.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */
#include "ctree.h"
#include "transaction.h"
#include "disk-io.h"
#include "locking.h"
#include "free-space-cache.h"
#include "print-tree.h"

/*
 * This implements snapshot deletions with the use of the readahead framework.
 * The fs-tree is not tranversed in sequential (key-) order, but by descending
 * into multiple paths at once. In addition, up to DROPTREE_MAX_ROOTS snapshots
 * will be deleted in parallel.
 * The basic principle is as follows:
 * When a tree node is found, first its refcnt and flags are fetched (via
 * readahead) from the extent allocation tree. Simplified, if the refcnt
 * is > 1, other trees also reference this node and we also have to drop our
 * ref to it and are done. If on the other hand the refcnt is 1, it's our
 * node and we have to free it (the same holds for data extents). So we fetch
 * the actual node (via readahead) and add all nodes/extents it points to to
 * the queue to again fetch refcnts for them.
 * While the case where refcnt > 1 looks like the easy one, there's one special
 * case to take into account: If the node still uses non-shared refs and we
 * are the owner of the node, we're not allowed to just drop our ref, as it
 * would leave the node with an unresolvable backref (it points to our tree).
 * So we first have to convert the refs to shared refs, and not only for this
 * node, but for its full subtree. We can stop descending if we encounter a
 * node of which we are not owner.
 * One big difference to the old snapshot deletion code that sequentially walks
 * the tree is that we can't represent the current deletion state with a single
 * key. As we delete in several paths simultaneously, we have to record all
 * loose ends in the state. To not get an exponentially growing state, we don't
 * delete the refs top-down, but bottom-up. In addition, we restrict the
 * currently processed nodes per-level globally. This way, we have a bounded
 * size for the state and can preallocate the needed space before any work
 * starts. During each transction commit, we write the state to a special
 * inode (DROPTREE_INO) recorded in the root tree.
 * For a commit, all outstanding readahead-requests get cancelled and moved
 * to a restart queue.
 *
 * The central data structure here is the droptree_node (dn). It represents a
 * file system extent, meaning a tree node, tree leaf or a data extent. The
 * dn's are organized in a tree corresponding to the disk structure. Each dn
 * keeps a bitmap that records which of its children are finished. When the
 * last bit gets set by a child, the freeing of the node is triggered. In
 * addition, struct droptree_root represents a fs tree to delete. It is mainly
 * used to keep all roots in a list. The reada_control for this tree is also
 * recorded here. We don't keep it in the dn's, as it is being freed and re-
 * created with each transaction commit.
 */

#define DROPTREE_MAX_ROOTS	50

/*
 * constants used in the on-disk state for deletion progress
 */
#define DROPTREE_STATE_GO_UP	0xffff
#define DROPTREE_STATE_GO_DOWN	0xfffe
#define DROPTREE_STATE_END	0xfffd

/*
 * used in droptree inode
 */
#define DROPTREE_VERSION	1

/*
 * helper struct used by main loop to keep all roots currently being deleted
 * in a list.
 */
struct droptree_root {
	struct droptree_node	*top_dn;
	struct btrfs_root	*root;
	struct reada_control	*rc;
	struct list_head	list;
};

/*
 * this structure contains all information needed to carry a
 * node/leaf/data extent through all phases. Each phases fills
 * in the information it got, so don't assume all information is
 * available at any time.
 */
struct droptree_node {
	/*
	 * pointer back to our root
	 */
	struct droptree_root	*droproot;

	/*
	 * where our place in the parent is. parent_slot is the bit number
	 * in parent->map
	 */
	struct droptree_node	*parent;
	int			parent_slot;

	/*
	 * tree of nodes. Both are protected by lock at bottom.
	 */
	struct list_head	next;
	struct list_head	children;

	/*
	 * information about myself
	 */
	u64			start;
	u64			len;
	int			level;
	u64			generation;
	u64			flags;
	u64			owner;		/* only for data extents */
	u64			offset;		/* only for data extents */

	/*
	 * bitmap to record the completion of each child node. Protected
	 * by lock at bottom.
	 */
	u32			*map;
	int			nritems;

	/*
	 * the readahead for prefetching the extent tree entry
	 */
	struct reada_control	*sub_rc;

	/*
	 * to get out of end_io worker context into readahead worker context.
	 * This is always needed if we want to do disk-IO, as otherwise we
	 * might end up holding all end_io worker, thus preventing the IO from
	 * completion and deadlocking. end_transaction might do disk-IO.
	 */
	struct btrfs_work	work;

	/*
	 * used to queue node either in restart queue or requeue queue.
	 * Protected by fs_info->droptree_lock.
	 */
	struct list_head	list;

	/*
	 * convert == 1 means we need to convert the refs in this tree to
	 * shared refs. The point where we started the conversion is marked
	 * with conversion_point == 1. When we finished conversion, the normal
	 * dropping of refs restarts at this point. When we encounter a node
	 * that doesn't need conversion, we mark it with convert_stop == 1.
	 * This also means no nodes below this point need conversion.
	 */
	unsigned int		convert:1;
	unsigned int		conversion_point:1;
	unsigned int		convert_stop:1;

	/*
	 * lock for bitmap and lists
	 */
	spinlock_t		lock;
};

static struct btrfs_key min_key = { 0 };
static struct btrfs_key max_key = {
	.objectid = (u64)-1,
	.type = (u8)-1,
	.offset = (u64)-1
};

static void droptree_fetch_ref(struct btrfs_work *work);
static void droptree_free_ref(struct btrfs_work *work);
static void droptree_kick(struct btrfs_fs_info *fs_info);
static void droptree_free_up(struct btrfs_trans_handle *trans,
			     struct droptree_node *dn, int last_ref);
static void droptree_reada_conv(struct btrfs_root *root,
				struct reada_control *rc,
				u64 wanted_generation,
				struct extent_buffer *eb,
				u64 start, int err,
				struct btrfs_key *top, void *ctx);
static int droptree_reada_dn(struct droptree_node *dn);
static struct droptree_node *droptree_alloc_node(struct droptree_root *dr);
static void droptree_reada_fstree(struct btrfs_root *root,
				  struct reada_control *rc,
				  u64 wanted_generation,
				  struct extent_buffer *eb,
				  u64 start, int err,
				  struct btrfs_key *top, void *ctx);

/*
 * Bit operations. These are mostly a copy of lib/bitmap.c, with 2 differences:
 * a) it always operates on u32 instead of unsigned long. This makes it easier
 *    to save them to disk in a portable format
 * b) the locking has to be provided externally
 */
#define DT_BITS_PER_BYTE	8
#define DT_BITS_PER_U32		(sizeof(u32) * DT_BITS_PER_BYTE)
#define DT_BIT_MASK(nr)		(1UL << ((nr) % DT_BITS_PER_U32))
#define DT_BIT_WORD(nr)		((nr) / DT_BITS_PER_U32)
#define DT_BITS_TO_U32(nr)	DIV_ROUND_UP(nr, DT_BITS_PER_BYTE * sizeof(u32))
#define DT_BITMAP_LAST_WORD_MASK(nbits)					\
(									\
	((nbits) % DT_BITS_PER_U32) ?					\
		(1UL << ((nbits) % DT_BITS_PER_U32))-1 : ~0UL		\
)

static void droptree_set_bit(int nr, u32 *addr)
{
	u32 mask = DT_BIT_MASK(nr);
	u32 *p = ((u32 *)addr) + DT_BIT_WORD(nr);

	*p |= mask;
}

static int droptree_test_bit(int nr, const u32 *addr)
{
	return 1UL & (addr[DT_BIT_WORD(nr)] >> (nr & (DT_BITS_PER_U32 - 1)));
}

static int droptree_bitmap_full(const u32 *addr, int bits)
{
	int k;
	int lim = bits / DT_BITS_PER_U32;

	for (k = 0; k < lim; ++k)
		if (~addr[k])
			return 0;

	if (bits % DT_BITS_PER_U32)
		if (~addr[k] & DT_BITMAP_LAST_WORD_MASK(bits))
			return 0;

	return 1;
}

/*
 * This function is called from readahead.
 * Prefetch the extent tree entry for this node so we can later read the
 * refcnt without waiting for disk I/O. This readahead is implemented as
 * a sub readahead from the fstree readahead.
 */
static void droptree_reada_ref(struct btrfs_root *root,
			       struct reada_control *rc,
			       u64 wanted_generation, struct extent_buffer *eb,
			       u64 start, int err, struct btrfs_key *top,
			       void *ctx)
{
	int nritems;
	u64 generation = 0;
	int level = 0;
	int i;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct droptree_node *dn = ctx;
	int nfound = 0;
	int ret;

	mutex_lock(&fs_info->droptree_lock);
	if (err == -EAGAIN || fs_info->droptree_pause_req) {
		/*
		 * if cancelled, put to restart queue
		 */
		list_add_tail(&dn->list, &fs_info->droptree_restart);
		mutex_unlock(&fs_info->droptree_lock);
		return;
	}
	mutex_unlock(&fs_info->droptree_lock);

	if (eb) {
		level = btrfs_header_level(eb);
		generation = btrfs_header_generation(eb);
		if (wanted_generation != generation)
			err = 1;
	} else {
		BUG_ON(!err);	/* can't happen */
	}

	/*
	 * either when level 0 is reached and the prefetch is finished, or
	 * when an error occurs, we finish this readahead and kick the
	 * reading of the actual refcnt to a worker
	 */
	if (err || level == 0) {
error:
		dn->work.func = droptree_fetch_ref;
		dn->sub_rc = rc;

		/*
		 * we don't want our rc to go away right now, as this might
		 * signal the parent rc before we are done. In the next stage,
		 * we first add a new readahead to the parent and afterwards
		 * clear our sub-reada
		 */
		reada_control_elem_get(rc);

		/*
		 * we have to push the lookup_extent_info out to a worker,
		 * as we are currently running in the context of the end_io
		 * workers and lookup_extent_info might do a lookup, thus
		 * deadlocking
		 */
		btrfs_queue_worker(&fs_info->readahead_workers, &dn->work);

		return;
	}

	/*
	 * level 0 not reached yet, so continue down the tree with reada
	 */
	nritems = btrfs_header_nritems(eb);

	for (i = 0; i < nritems; i++) {
		u64 n_gen;
		struct btrfs_key key;
		struct btrfs_key next_key;
		u64 bytenr;

		btrfs_node_key_to_cpu(eb, &key, i);
		if (i + 1 < nritems)
			btrfs_node_key_to_cpu(eb, &next_key, i + 1);
		else
			next_key = *top;
		bytenr = btrfs_node_blockptr(eb, i);
		n_gen = btrfs_node_ptr_generation(eb, i);

		if (btrfs_comp_cpu_keys(&key, &rc->key_end) < 0 &&
		    btrfs_comp_cpu_keys(&next_key, &rc->key_start) > 0) {
			ret = reada_add_block(rc, bytenr, &next_key,
					      level - 1, n_gen, ctx);
			if (ret)
				goto error;
			++nfound;
		}
	}
	if (nfound > 1) {
		/*
		 * just a safeguard, we searched for a single key, so there
		 * must not be more than one entry in the node
		 */
		btrfs_print_leaf(rc->root, eb);
		BUG_ON(1);	/* inconsistent fs */
	}
	if (nfound == 0) {
		/*
		 * somewhere the tree changed while we were running down.
		 * So start over again from the top
		 */
		ret = droptree_reada_dn(dn);
		if (ret)
			goto error;
	}
}

/*
 * This is called from a worker, kicked off by droptree_reada_ref. We arrive
 * here after either the extent tree is prefetched or an error occured. In
 * any case, the refcnt is read synchronously now, hopefully without disk I/O.
 * If we encounter any hard errors here, we have no chance but to BUG.
 */
static void droptree_fetch_ref(struct btrfs_work *work)
{
	struct droptree_node *dn;
	int ret;
	u64 refs;
	u64 flags;
	struct btrfs_trans_handle *trans;
	struct extent_buffer *eb = NULL;
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;
	struct reada_control *sub_rc;
	int free_up = 0;
	int is_locked = 0;

	dn = container_of(work, struct droptree_node, work);

	root = dn->droproot->root;
	fs_info = root->fs_info;
	sub_rc = dn->sub_rc;

	trans = btrfs_join_transaction(fs_info->extent_root);
	BUG_ON(!trans);	/* can't back out */

	ret = btrfs_lookup_extent_info(trans, root, dn->start, dn->len,
				       &refs, &flags);
	BUG_ON(ret); /* can't back out */
	dn->flags = flags;

	if (dn->convert && dn->level >= 0) {
		eb = btrfs_find_create_tree_block(root, dn->start, dn->len);
		BUG_ON(!eb); /* can't back out */
		if (!btrfs_buffer_uptodate(eb, dn->generation)) {
			struct reada_control *conv_rc;

fetch_buf:
			/*
			 * we might need to convert the ref. To check this,
			 * we need to know the header_owner of the block, so
			 * we actually need the block's content. Just add
			 * a sub-reada for the content that points back here
			 */
			free_extent_buffer(eb);
			btrfs_end_transaction(trans, fs_info->extent_root);

			conv_rc = btrfs_reada_alloc(dn->droproot->rc,
						    root, NULL, NULL,
						    droptree_reada_conv);
			ret = reada_add_block(conv_rc, dn->start, NULL,
					      dn->level, dn->generation, dn);
			BUG_ON(ret < 0); /* can't back out */
			reada_start_machine(fs_info);

			return;
		}
		if (btrfs_header_owner(eb) != root->root_key.objectid) {
			/*
			 * we're not the owner of the block, so we can stop
			 * converting. No blocks below this will need conversion
			 */
			dn->convert_stop = 1;
			free_up = 1;
		} else {
			/* conversion needed, descend */
			btrfs_tree_read_lock(eb);
			btrfs_set_lock_blocking_rw(eb, BTRFS_READ_LOCK);
			is_locked = 1;
			droptree_reada_fstree(root, dn->droproot->rc,
					      dn->generation, eb, dn->start, 0,
					      NULL, dn);
		}
		goto out;
	}

	if (refs > 1) {
		/*
		 * we did the lookup without proper locking. If the refcnt is 1,
		 * no locking is needed, as we hold the only reference to
		 * the extent. When the refcnt is >1, it can change at any time.
		 * To prevent this, we lock either the extent (for a tree
		 * block), or the leaf referencing the extent (for a data
		 * extent). Afterwards we repeat the lookup.
		 */
		if (dn->level == -1)
			eb = btrfs_find_create_tree_block(root,
							  dn->parent->start,
							  dn->parent->len);
		else
			eb = btrfs_find_create_tree_block(root, dn->start,
							  dn->len);
		BUG_ON(!eb); /* can't back out */
		btrfs_tree_read_lock(eb);
		btrfs_set_lock_blocking_rw(eb, BTRFS_READ_LOCK);
		is_locked = 1;
		ret = btrfs_lookup_extent_info(trans, root, dn->start, dn->len,
					       &refs, &flags);
		BUG_ON(ret); /* can't back out */
		dn->flags = flags;

		if (refs == 1) {
			/*
			 * The additional ref(s) has gone away in the meantime.
			 * Now the extent is ours, no need for locking anymore
			 */
			btrfs_tree_read_unlock_blocking(eb);
			free_extent_buffer(eb);
			eb = NULL;
		} else if (!(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) &&
			   dn->level >= 0 &&
			   !btrfs_buffer_uptodate(eb, dn->generation)) {
			/*
			 * we might need to convert the ref. To check this,
			 * we need to know the header_owner of the block, so
			 * we actually need the block's contents. Just add
			 * a sub-reada for the content that points back here
			 */
			btrfs_tree_read_unlock_blocking(eb);

			goto fetch_buf;
		}
	}

	/*
	 * See if we have to convert the ref to a shared ref before we drop
	 * our ref. Above we have made sure the buffer is uptodate.
	 */
	if (refs > 1 && dn->level >= 0 &&
	    !(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) &&
	    btrfs_header_owner(eb) == root->root_key.objectid) {
		dn->convert = 1;
		/* when done with the conversion, switch to freeing again */
		dn->conversion_point = 1;

		droptree_reada_fstree(root, dn->droproot->rc,
				      dn->generation, eb, dn->start, 0,
				      NULL, dn);
	} else if (refs == 1 && dn->level >= 0) {
		/*
		 * refcnt is 1, descend into lower levels
		 */
		ret = reada_add_block(dn->droproot->rc, dn->start, NULL,
				      dn->level, dn->generation, dn);
		WARN_ON(ret < 0);
		reada_start_machine(fs_info);
	} else {
		/*
		 * either refcnt is >1, or we've reached the bottom of the
		 * tree. In any case, drop our reference
		 */
		free_up = 1;
	}
out:
	if (eb) {
		if (is_locked)
			btrfs_tree_read_unlock_blocking(eb);
		free_extent_buffer(eb);
	}

	if (free_up) {
		/*
		 * mark node as done in parent. This ends the lifecyle of dn
		 */
		droptree_free_up(trans, dn, refs == 1);
	}

	btrfs_end_transaction(trans, fs_info->extent_root);
	/*
	 * end the sub reada. This might complete the parent.
	 */
	reada_control_elem_put(sub_rc);
}

/*
 * mark the slot in the parent as done.  This might also complete the parent,
 * so walk the tree up as long as nodes complete
 *
 * can't be called from end_io worker context, as it needs a transaction
 */
static noinline void droptree_free_up(struct btrfs_trans_handle *trans,
			     struct droptree_node *dn, int last_ref)
{
	struct btrfs_root *root = dn->droproot->root;
	struct btrfs_fs_info *fs_info = root->fs_info;

	while (dn) {
		struct droptree_node *parent = dn->parent;
		int slot = dn->parent_slot;
		u64 parent_start = 0;
		int ret;

		if (parent && parent->flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			parent_start = parent->start;

		if (dn->level >= 0) {
			mutex_lock(&fs_info->droptree_lock);
			--fs_info->droptree_req[dn->level];
			mutex_unlock(&fs_info->droptree_lock);
		}

		if (dn->convert) {
			if (dn->level >= 0 &&
			    !(dn->flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) &&
			    !dn->convert_stop) {
				struct extent_buffer *eb;
				eb = read_tree_block(root, dn->start, dn->len,
						     dn->generation);
				BUG_ON(!eb); /* can't back out */
				btrfs_tree_lock(eb);
				btrfs_set_lock_blocking(eb);
				ret = btrfs_inc_ref(trans, root, eb, 1, 1);
				BUG_ON(ret); /* can't back out */
				ret = btrfs_dec_ref(trans, root, eb, 0, 1);
				BUG_ON(ret); /* can't back out */
				ret = btrfs_set_disk_extent_flags(trans,
						root, eb->start, eb->len,
						BTRFS_BLOCK_FLAG_FULL_BACKREF,
						0);
				BUG_ON(ret); /* can't back out */
				btrfs_tree_unlock(eb);
				free_extent_buffer(eb);
				dn->flags |= BTRFS_BLOCK_FLAG_FULL_BACKREF;
			}
			dn->convert = 0;
			if (dn->conversion_point) {
				/*
				 * start over again for this node, clean it
				 * and enqueue it again
				 */
				dn->conversion_point = 0;

				kfree(dn->map);
				dn->map = NULL;
				dn->nritems = 0;

				/*
				 * just add to the list and let droptree_kick
				 * do the actual work of enqueueing
				 */
				mutex_lock(&fs_info->droptree_lock);
				list_add_tail(&dn->list,
					   &fs_info->droptree_queue[dn->level]);
				reada_control_elem_get(dn->droproot->rc);
				mutex_unlock(&fs_info->droptree_lock);

				goto out;
			}
		} else if (last_ref && dn->level >= 0) {
			struct extent_buffer *eb;

			/*
			 * btrfs_free_tree_block needs to read the block to
			 * act on the owner recorded in the header. We have
			 * read the block some time ago, so hopefully it is
			 * still in the cache
			 */
			eb = read_tree_block(root, dn->start, dn->len,
					     dn->generation);
			BUG_ON(!eb); /* can't back out */
			btrfs_free_tree_block(trans, root, eb,
					      parent_start, 1, 0);
			free_extent_buffer(eb);
		} else {
			btrfs_free_extent(trans, root, dn->start, dn->len,
					  parent_start,
					  root->root_key.objectid,
					  dn->owner, dn->offset, 0);
		}

		if (!parent)
			break;

		/*
		 * this free is after the parent check, as we don't want to
		 * free up the top level node. The main loop uses dn->map
		 * as an indication if the tree is done.
		 */
		spin_lock(&parent->lock);
		list_del(&dn->next);
		spin_unlock(&parent->lock);
		kfree(dn->map);
		kfree(dn);

		/*
		 * notify up
		 */
		spin_lock(&parent->lock);
		droptree_set_bit(slot, parent->map);
		if (!droptree_bitmap_full(parent->map, parent->nritems)) {
			spin_unlock(&parent->lock);
			break;
		}
		spin_unlock(&parent->lock);

		dn = parent;
		last_ref = 1;
	}

out:
	droptree_kick(fs_info);
}

/*
 * this callback is used when we need the actual eb to decide whether to
 * convert the refs for this node or not. It just loops back to
 * droptree_reada_fetch_ref
 */
static void droptree_reada_conv(struct btrfs_root *root,
				struct reada_control *rc,
				u64 wanted_generation,
				struct extent_buffer *eb,
				u64 start, int err,
				struct btrfs_key *top, void *ctx)
{
	struct droptree_node *dn = ctx;
	struct btrfs_fs_info *fs_info = root->fs_info;

	if (err == -EAGAIN) {
		/*
		 * we're still in the process of fetching the refs.
		 * As we want to start over cleanly after the commit,
		 * we also have to give up the sub_rc
		 */
		reada_control_elem_put(dn->sub_rc);

		mutex_lock(&fs_info->droptree_lock);
		list_add_tail(&dn->list, &fs_info->droptree_restart);
		mutex_unlock(&fs_info->droptree_lock);
		return;
	}

	if (err || eb == NULL)
		BUG(); /* can't back out */

	/* not yet done with the conversion stage, go back to step 2 */
	btrfs_queue_worker(&fs_info->readahead_workers, &dn->work);

	droptree_kick(fs_info);
}

/*
 * After having fetched the refcnt for a node and decided we have to descend
 * into it, we arrive here. Called from reada for the actual extent.
 * The main idea is to find all pointers to lower nodes and add them to reada.
 */
static void droptree_reada_fstree(struct btrfs_root *root,
				  struct reada_control *rc,
				  u64 wanted_generation,
				  struct extent_buffer *eb,
				  u64 start, int err,
				  struct btrfs_key *top, void *ctx)
{
	int nritems;
	u64 generation;
	int level;
	int i;
	struct droptree_node *dn = ctx;
	struct droptree_node *child;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct droptree_node **child_map = NULL;
	u32 *finished_map = NULL;
	int nrsent = 0;
	int ret;

	if (err == -EAGAIN) {
		mutex_lock(&fs_info->droptree_lock);
		list_add_tail(&dn->list, &fs_info->droptree_restart);
		mutex_unlock(&fs_info->droptree_lock);
		return;
	}

	if (err || eb == NULL) {
		/*
		 * FIXME we can't deal with I/O errors here. One possibility
		 * would to abandon the subtree and just leave it allocated,
		 * wasting the space. Another way would be to turn the fs
		 * readonly.
		 */
		BUG(); /* can't back out */
	}

	level = btrfs_header_level(eb);
	nritems = btrfs_header_nritems(eb);
	generation = btrfs_header_generation(eb);

	if (wanted_generation != generation) {
		/*
		 * the fstree is supposed to be static, as it is inaccessible
		 * from user space. So if there's a generation mismatch here,
		 * something has gone wrong.
		 */
		BUG(); /* can't back out */
	}

	/*
	 * allocate a bitmap if we don't already have one. In case we restart
	 * a snapshot deletion after a mount, the map already contains completed
	 * slots. If we have the map, we put it aside for the moment and replace
	 * it with a zero-filled map. During the loop, we repopulate it. If we
	 * wouldn't do that, we might end up with a dn already being freed
	 * by completed children that got enqueued during the loop. This way
	 * we make sure the dn might only be freed during the last round.
	 */
	if (dn->map) {
		struct droptree_node *it;
		/*
		 * we are in a restore. build a map of all child nodes that
		 * are already present
		 */
		child_map = kzalloc(nritems * sizeof(struct droptree_node),
				    GFP_NOFS);
		BUG_ON(!child_map); /* can't back out */
		BUG_ON(nritems != dn->nritems); /* inconsistent fs */
		list_for_each_entry(it, &dn->children, next) {
			BUG_ON(it->parent_slot < 0 ||
			       it->parent_slot >= nritems); /* incons. fs */
			child_map[it->parent_slot] = it;
		}
		finished_map = dn->map;
		dn->map = NULL;
	}
	dn->map = kzalloc(DT_BITS_TO_U32(nritems) * sizeof(u32), GFP_NOFS);
	dn->nritems = nritems;

	/*
	 * fetch refs for all lower nodes
	 */
	for (i = 0; i < nritems; i++) {
		u64 n_gen;
		struct btrfs_key key;
		u64 bytenr;
		u64 num_bytes;
		u64 owner = level - 1;
		u64 offset = 0;

		/*
		 * in case of recovery, we could have already finished this
		 * slot
		 */
		if (finished_map && droptree_test_bit(i, finished_map))
			goto next_slot;

		if (level == 0) {
			struct btrfs_file_extent_item *fi;

			btrfs_item_key_to_cpu(eb, &key, i);
			if (btrfs_key_type(&key) != BTRFS_EXTENT_DATA_KEY)
				goto next_slot;
			fi = btrfs_item_ptr(eb, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(eb, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				goto next_slot;
			bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
			if (bytenr == 0) {
next_slot:
				spin_lock(&dn->lock);
				droptree_set_bit(i, dn->map);
				if (droptree_bitmap_full(dn->map, nritems)) {
					spin_unlock(&dn->lock);
					goto free;
				}
				spin_unlock(&dn->lock);
				continue;
			}
			num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
			key.offset -= btrfs_file_extent_offset(eb, fi);
			owner = key.objectid;
			offset = key.offset;
			n_gen = 0;
		} else {
			btrfs_node_key_to_cpu(eb, &key, i);
			bytenr = btrfs_node_blockptr(eb, i);
			num_bytes = btrfs_level_size(root, 0);
			n_gen = btrfs_node_ptr_generation(eb, i);
		}

		if (child_map && child_map[i]) {
			child = child_map[i];
			child->generation = n_gen;
		} else {
			child = droptree_alloc_node(dn->droproot);
			BUG_ON(!child);
			child->parent = dn;
			child->parent_slot = i;
			child->level = level - 1;
			child->start = bytenr;
			child->len = num_bytes;
			child->owner = owner;
			child->offset = offset;
			child->generation = n_gen;
			child->convert = dn->convert;
		}
		++nrsent;

		/*
		 * limit the number of outstanding requests for a given level.
		 * The limit is global to all outstanding snapshot deletions.
		 * Only requests for levels >= 0 are limited. The level of this
		 * request is level-1.
		 */
		if (level > 0) {
			mutex_lock(&fs_info->droptree_lock);
			if ((fs_info->droptree_pause_req ||
			    (fs_info->droptree_req[level - 1] >=
			     fs_info->droptree_limit[level - 1]))) {
				/*
				 * limit reached or pause requested, enqueue
				 * request and get an rc->elem for it
				 */
				list_add_tail(&child->list,
					   &fs_info->droptree_queue[level - 1]);
				reada_control_elem_get(rc);
				mutex_unlock(&fs_info->droptree_lock);
				continue;
			}
			++fs_info->droptree_req[level - 1];
			mutex_unlock(&fs_info->droptree_lock);
		}
		if (list_empty(&child->next)) {
			spin_lock(&dn->lock);
			list_add(&child->next, &dn->children);
			spin_unlock(&dn->lock);
		}
		/*
		 * this might immediately call back into completion,
		 * so dn can become invalid in the last round
		 */
		ret = droptree_reada_dn(child);
		BUG_ON(ret); /* can't back out */
	}

	if (nrsent == 0) {
free:
		/*
		 * this leaf didn't contain any EXTENT_DATA items. It can't be
		 * a node, as nodes are never empty. This point might also be
		 * reached via the label <free> when we set the last bit our-
		 * selves. This is possible when all enqueued readas already
		 * finish while we loop.
		 * We need to drop our ref and notify our parents, but can't
		 * do this in the context of the end_io workers, as it might
		 * cause disk-I/O causing a deadlock. So kick off to a worker.
		 */
		dn->work.func = droptree_free_ref;

		/*
		 * we don't want our rc to go away right now, as this might
		 * signal the parent rc before we are done.
		 */
		reada_control_elem_get(rc);
		btrfs_queue_worker(&fs_info->readahead_workers, &dn->work);
	}

	kfree(child_map);
	kfree(finished_map);
	droptree_kick(fs_info);
}

/*
 * worker deferred from droptree_reada_fstree in case the extent didn't contain
 * anything to descend into. Just free this node and notify the parent
 */
static void droptree_free_ref(struct btrfs_work *work)
{
	struct droptree_node *dn;
	struct btrfs_trans_handle *trans;
	struct reada_control *fs_rc;
	struct btrfs_root *root;
	struct btrfs_fs_info *fs_info;

	dn = container_of(work, struct droptree_node, work);
	fs_rc = dn->droproot->rc;
	root = dn->droproot->root;
	fs_info = root->fs_info;

	trans = btrfs_join_transaction(fs_info->extent_root);
	BUG_ON(!trans); /* can't back out */

	droptree_free_up(trans, dn, 1);

	btrfs_end_transaction(trans, fs_info->extent_root);

	/*
	 * end the sub reada. This might complete the parent.
	 */
	reada_control_elem_put(fs_rc);
}

/*
 * add a node to readahead. For the top-level node, just add the block to the
 * fs-rc, for all other nodes add a sub-reada.
 */
static int droptree_reada_dn(struct droptree_node *dn)
{
	struct btrfs_key ex_start;
	struct btrfs_key ex_end;
	int ret;
	struct droptree_root *dr = dn->droproot;

	ex_start.objectid = dn->start;
	ex_start.type = BTRFS_EXTENT_ITEM_KEY;
	ex_start.offset = dn->len;
	ex_end = ex_start;
	++ex_end.offset;

	if (!dn->parent)
		ret = reada_add_block(dr->rc, dn->start, &max_key,
				      dn->level, dn->generation, dn);
	else
		ret = btrfs_reada_add(dr->rc, dr->root->fs_info->extent_root,
				      &ex_start, &ex_end,
				      droptree_reada_ref, dn, NULL);

	return ret;
}

/*
 * after a restart from a commit, all previously canceled requests need to be
 * restarted. Also we moved the queued dns to the requeue queue, so move them
 * back here
 */
static void droptree_restart(struct btrfs_fs_info *fs_info)
{
	int ret;
	struct droptree_node *dn;

	/*
	 * keep the lock over the whole operation, otherwise the enqueued
	 * blocks could immediately be handled and the elem count drop to
	 * zero before we're done enqueuing
	 */
	mutex_lock(&fs_info->droptree_lock);
	if (fs_info->droptree_pause_req) {
		mutex_unlock(&fs_info->droptree_lock);
		return;
	}

	while (!list_empty(&fs_info->droptree_restart)) {
		dn = list_first_entry(&fs_info->droptree_restart,
				      struct droptree_node, list);

		list_del_init(&dn->list);

		ret = droptree_reada_dn(dn);
		BUG_ON(ret); /* can't back out */
	}

	while (!list_empty(&fs_info->droptree_requeue)) {
		dn = list_first_entry(&fs_info->droptree_requeue,
				      struct droptree_node, list);
		list_del_init(&dn->list);
		list_add_tail(&dn->list, &fs_info->droptree_queue[dn->level]);
		reada_control_elem_get(dn->droproot->rc);
	}

	mutex_unlock(&fs_info->droptree_lock);
}

/*
 * for a commit, everything that's queued in droptree_queue[level] is put
 * aside into requeue queue. Also the elem on the parent is given up, allowing
 * the count to drop to zero
 */
static void droptree_move_to_requeue(struct btrfs_fs_info *fs_info)
{
	int i;
	struct droptree_node *dn;
	struct reada_control *rc;

	mutex_lock(&fs_info->droptree_lock);

	for (i = 0; i < BTRFS_MAX_LEVEL; ++i) {
		while (!list_empty(fs_info->droptree_queue + i)) {
			dn = list_first_entry(fs_info->droptree_queue + i,
					      struct droptree_node, list);

			list_del_init(&dn->list);
			rc = dn->droproot->rc;
			list_add_tail(&dn->list, &fs_info->droptree_requeue);
			reada_control_elem_put(rc);
		}
	}
	mutex_unlock(&fs_info->droptree_lock);
}

/*
 * check if we have room in readahead at any level and send respective nodes
 * to readahead
 */
static void droptree_kick(struct btrfs_fs_info *fs_info)
{
	int i;
	int ret;
	struct droptree_node *dn;
	struct reada_control *rc;

	mutex_lock(&fs_info->droptree_lock);

	for (i = 0; i < BTRFS_MAX_LEVEL; ++i) {
		while (!list_empty(fs_info->droptree_queue + i)) {
			if (fs_info->droptree_pause_req) {
				mutex_unlock(&fs_info->droptree_lock);
				droptree_move_to_requeue(fs_info);
				return;
			}

			if (fs_info->droptree_req[i] >=
			    fs_info->droptree_limit[i])
				break;

			dn = list_first_entry(fs_info->droptree_queue + i,
					      struct droptree_node, list);

			list_del_init(&dn->list);
			rc = dn->droproot->rc;

			++fs_info->droptree_req[i];
			mutex_unlock(&fs_info->droptree_lock);

			spin_lock(&dn->parent->lock);
			if (list_empty(&dn->next))
				list_add(&dn->next, &dn->parent->children);
			spin_unlock(&dn->parent->lock);

			ret = droptree_reada_dn(dn);
			BUG_ON(ret); /* can't back out */

			/*
			 * we got an elem on the rc when the dn got enqueued,
			 * drop it here so elem can go down to zero
			 */
			reada_control_elem_put(rc);
			mutex_lock(&fs_info->droptree_lock);
		}
	}
	mutex_unlock(&fs_info->droptree_lock);
}

/*
 * mark the running droptree as paused and cancel add requests. When this
 * returns, droptree is completly paused.
 */
int btrfs_droptree_pause(struct btrfs_fs_info *fs_info)
{
	struct reada_control *top_rc;

	mutex_lock(&fs_info->droptree_lock);

	++fs_info->droptree_pause_req;
	top_rc = fs_info->droptree_rc;
	if (top_rc)
		kref_get(&top_rc->refcnt);
	mutex_unlock(&fs_info->droptree_lock);

	if (top_rc) {
		btrfs_reada_abort(fs_info, top_rc);
		btrfs_reada_detach(top_rc); /* free our ref */
	}
	/* move all queued requests to requeue */
	droptree_move_to_requeue(fs_info);

	mutex_lock(&fs_info->droptree_lock);
	while (fs_info->droptrees_running) {
		mutex_unlock(&fs_info->droptree_lock);
		wait_event(fs_info->droptree_wait,
			   fs_info->droptrees_running == 0);
		mutex_lock(&fs_info->droptree_lock);
	}
	mutex_unlock(&fs_info->droptree_lock);

	return 0;
}

void btrfs_droptree_continue(struct btrfs_fs_info *fs_info)
{
	mutex_lock(&fs_info->droptree_lock);
	--fs_info->droptree_pause_req;
	mutex_unlock(&fs_info->droptree_lock);

	wake_up(&fs_info->droptree_wait);
}

/*
 * find the special inode used to save droptree state. If it doesn't exist,
 * create it. Similar to the free_space_cache inodes this is generated in the
 * root tree.
 */
static noinline struct inode *droptree_get_inode(struct btrfs_fs_info *fs_info)
{
	struct btrfs_key location;
	struct inode *inode;
	struct btrfs_trans_handle *trans;
	struct btrfs_root *root = fs_info->tree_root;
	struct btrfs_disk_key disk_key;
	struct btrfs_inode_item *inode_item;
	struct extent_buffer *leaf;
	struct btrfs_path *path;
	u64 flags = BTRFS_INODE_NOCOMPRESS | BTRFS_INODE_PREALLOC;
	int ret = 0;

	location.objectid = BTRFS_DROPTREE_INO_OBJECTID;
	location.type = BTRFS_INODE_ITEM_KEY;
	location.offset = 0;

	inode = btrfs_iget(fs_info->sb, &location, root, NULL);
	if (inode && !IS_ERR(inode))
		return inode;

	path = btrfs_alloc_path();
	if (!path)
		return ERR_PTR(-ENOMEM);
	inode = NULL;

	/*
	 * inode does not exist, create it
	 */
	trans = btrfs_start_transaction(root, 1);
	if (!trans) {
		btrfs_free_path(path);
		return ERR_PTR(-ENOMEM);
	}

	ret = btrfs_insert_empty_inode(trans, root, path,
				       BTRFS_DROPTREE_INO_OBJECTID);
	if (ret)
		goto out;

	leaf = path->nodes[0];
	inode_item = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_inode_item);
	btrfs_item_key(leaf, &disk_key, path->slots[0]);
	memset_extent_buffer(leaf, 0, (unsigned long)inode_item,
			     sizeof(*inode_item));
	btrfs_set_inode_generation(leaf, inode_item, trans->transid);
	btrfs_set_inode_size(leaf, inode_item, 0);
	btrfs_set_inode_nbytes(leaf, inode_item, 0);
	btrfs_set_inode_uid(leaf, inode_item, 0);
	btrfs_set_inode_gid(leaf, inode_item, 0);
	btrfs_set_inode_mode(leaf, inode_item, S_IFREG | 0600);
	btrfs_set_inode_flags(leaf, inode_item, flags);
	btrfs_set_inode_nlink(leaf, inode_item, 1);
	btrfs_set_inode_transid(leaf, inode_item, trans->transid);
	btrfs_set_inode_block_group(leaf, inode_item, 0);
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(path);

	inode = btrfs_iget(fs_info->sb, &location, root, NULL);

out:
	btrfs_free_path(path);
	btrfs_end_transaction(trans, root);

	if (IS_ERR(inode))
		return inode;

	return inode;
}

/*
 * basic allocation and initialization of a droptree node
 */
static struct droptree_node *droptree_alloc_node(struct droptree_root *dr)
{
	struct droptree_node *dn;

	dn = kzalloc(sizeof(*dn), GFP_NOFS);
	if (!dn)
		return NULL;

	dn->droproot = dr;
	dn->parent = NULL;
	dn->parent_slot = 0;
	dn->map = NULL;
	dn->nritems = 0;
	INIT_LIST_HEAD(&dn->children);
	INIT_LIST_HEAD(&dn->next);
	INIT_LIST_HEAD(&dn->list);
	spin_lock_init(&dn->lock);

	return dn;
}

/*
 * add a new top-level node to the list of running droptrees. Allocate the
 * necessary droptree_root for it
 */
static struct droptree_root *droptree_add_droproot(struct list_head *list,
						   struct droptree_node *dn,
						   struct btrfs_root *root)
{
	struct droptree_root *dr;

	dr = kzalloc(sizeof(*dr), GFP_NOFS);
	if (!dr)
		return NULL;

	dn->droproot = dr;
	dr->root = root;
	dr->top_dn = dn;
	list_add_tail(&dr->list, list);

	return dr;
}

/*
 * Free a complete droptree
 *
 * again, recursion would be the easy way, but we have to iterate
 * through the tree. While freeing the nodes, also remove them from
 * restart/requeue-queue. If they're not empty after all trees have
 * been freed, something is wrong.
 */
static noinline void droptree_free_tree(struct droptree_node *dn)
{
	while (dn) {
		if (!list_empty(&dn->children)) {
			dn = list_first_entry(&dn->children,
					       struct droptree_node, next);
		} else {
			struct droptree_node *parent = dn->parent;
			list_del(&dn->next);
			/*
			 * if dn is not enqueued, list is empty and del_init
			 * changes nothing
			 */
			list_del_init(&dn->list);
			kfree(dn->map);
			kfree(dn);
			dn = parent;
		}
	}
}

/*
 * set up the reada_control for a droptree_root and enqueue it so it gets
 * started by droptree_restart
 */
static int droptree_reada_root(struct reada_control *top_rc,
			       struct droptree_root *dr)
{
	struct reada_control *rc;
	u64 start;
	u64 generation;
	int level;
	struct extent_buffer *node;
	struct btrfs_fs_info *fs_info = dr->root->fs_info;

	rc = btrfs_reada_alloc(top_rc, dr->root, &min_key, &max_key,
			       droptree_reada_fstree);
	if (!rc)
		return -ENOMEM;

	dr->rc = rc;
	kref_get(&rc->refcnt);

	node = btrfs_root_node(dr->root);
	start = node->start;
	level = btrfs_header_level(node);
	generation = btrfs_header_generation(node);
	free_extent_buffer(node);

	dr->top_dn->start = start;
	dr->top_dn->level = level;
	dr->top_dn->len = btrfs_level_size(dr->root, level);
	dr->top_dn->generation = generation;

	mutex_lock(&fs_info->droptree_lock);
	++fs_info->droptree_req[level];
	mutex_unlock(&fs_info->droptree_lock);

	/*
	 * add this root to the restart queue. It can't be started immediately,
	 * as the caller is not yet synchronized with the transaction commit
	 */
	mutex_lock(&fs_info->droptree_lock);
	list_add_tail(&dr->top_dn->list, &fs_info->droptree_restart);
	mutex_unlock(&fs_info->droptree_lock);

	return 0;
}

/*
 * write out the state of a tree to the droptree inode. To avoid recursion,
 * we do this iteratively using a dynamically allocated stack structure.
 */
static int droptree_save_tree(struct btrfs_fs_info *fs_info,
			      struct io_ctl *io,
			      struct droptree_node *dn)
{
	struct {
		struct list_head	*head;
		struct droptree_node	*cur;
	} *stack, *sp;
	struct droptree_node *cur;
	int down = 1;

	stack = kmalloc(sizeof(*stack) * BTRFS_MAX_LEVEL, GFP_NOFS);
	BUG_ON(!stack); /* can't back out */
	sp = stack;
	sp->head = &dn->next;
	sp->cur = dn;
	cur = dn;

	while (1) {
		if (down && cur->nritems && !cur->convert) {
			int i;
			int l;

			/*
			 * write out this node before descending down
			 */
			BUG_ON(cur->nritems && !cur->map); /* can't happen */
			io_ctl_set_u16(io, cur->parent_slot);
			io_ctl_set_u64(io, cur->start);
			io_ctl_set_u64(io, cur->len);
			io_ctl_set_u16(io, cur->nritems);
			l = DT_BITS_TO_U32(cur->nritems);

			for (i = 0; i < l; ++i)
				io_ctl_set_u32(io, cur->map[i]);
		}
		if (down && !list_empty(&cur->children)) {
			/*
			 * walk down one step
			 */
			if (cur->level > 0)
				io_ctl_set_u16(io, DROPTREE_STATE_GO_DOWN);
			++sp;
			sp->head = &cur->children;
			cur = list_first_entry(&cur->children,
					       struct droptree_node, next);
			sp->cur = cur;
		} else if (cur->next.next != sp->head) {
			/*
			 * step to the side
			 */
			cur = list_first_entry(&cur->next,
					       struct droptree_node, next);
			sp->cur = cur;
			down = 1;
		} else if (sp != stack) {
			/*
			 * walk up
			 */
			if (cur->level >= 0)
				io_ctl_set_u16(io, DROPTREE_STATE_GO_UP);
			--sp;
			cur = sp->cur;
			down = 0;
		} else {
			/*
			 * done
			 */
			io_ctl_set_u16(io, DROPTREE_STATE_END);
			break;
		}
	}
	kfree(stack);

	return 0;
}

/*
 * write out the full droptree state to disk
 */
static void droptree_save_state(struct btrfs_fs_info *fs_info,
				struct inode *inode,
				struct btrfs_trans_handle *trans,
				struct list_head *droplist)
{
	struct io_ctl io_ctl;
	struct droptree_root *dr;
	struct btrfs_root *root = fs_info->tree_root;
	struct extent_state *cached_state = NULL;
	int ret;

	io_ctl_init(&io_ctl, inode, root);
	io_ctl.check_crcs = 0;
	io_ctl_prepare_pages(&io_ctl, inode, 0);
	lock_extent_bits(&BTRFS_I(inode)->io_tree, 0,
			 i_size_read(inode) - 1,
			 0, &cached_state, GFP_NOFS);

	io_ctl_set_u32(&io_ctl, DROPTREE_VERSION);	/* version */
	io_ctl_set_u64(&io_ctl, fs_info->generation);	/* generation */

	list_for_each_entry(dr, droplist, list) {
		io_ctl_set_u64(&io_ctl, dr->root->root_key.objectid);
		io_ctl_set_u64(&io_ctl, dr->root->root_key.offset);
		io_ctl_set_u8(&io_ctl, dr->top_dn->level);
		ret = droptree_save_tree(fs_info, &io_ctl, dr->top_dn);
		BUG_ON(ret); /* can't back out */
	}
	io_ctl_set_u64(&io_ctl, 0);	/* terminator */

	ret = btrfs_dirty_pages(root, inode, io_ctl.pages,
				io_ctl.num_pages,
				0, i_size_read(inode), &cached_state);
	BUG_ON(ret); /* can't back out */
	io_ctl_drop_pages(&io_ctl);
	unlock_extent_cached(&BTRFS_I(inode)->io_tree, 0,
			     i_size_read(inode) - 1, &cached_state,
			     GFP_NOFS);
	io_ctl_free(&io_ctl);

	ret = filemap_write_and_wait(inode->i_mapping);
	BUG_ON(ret); /* can't back out */

	ret = btrfs_update_inode(trans, root, inode);
	BUG_ON(ret); /* can't back out */
}

/*
 * read the saved state from the droptree inode and prepare everything so
 * it gets started by droptree_restart
 */
static int droptree_read_state(struct btrfs_fs_info *fs_info,
			       struct inode *inode,
			       struct reada_control *top_rc,
			       struct list_head *droplist)
{
	struct io_ctl io_ctl;
	u32 version;
	u64 generation;
	struct droptree_node **stack;
	int ret = 0;

	stack = kmalloc(sizeof(*stack) * BTRFS_MAX_LEVEL, GFP_NOFS);
	if (!stack)
		return -ENOMEM;

	io_ctl_init(&io_ctl, inode, fs_info->tree_root);
	io_ctl.check_crcs = 0;
	io_ctl_prepare_pages(&io_ctl, inode, 1);

	version = io_ctl_get_u32(&io_ctl);
	if (version != DROPTREE_VERSION) {
		printk(KERN_ERR "btrfs: snapshot deletion state has been saved "
				"with a different version, ignored\n");
		ret = -EINVAL;
		goto out;
	}
	/* FIXME generation is currently not needed */
	generation = io_ctl_get_u64(&io_ctl);

	while (1) {
		struct btrfs_key key;
		int ret;
		struct btrfs_root *del_root;
		struct droptree_root *dr;
		int level;
		int max_level;
		struct droptree_node *root_dn;

		key.objectid = io_ctl_get_u64(&io_ctl);
		if (key.objectid == 0)
			break;

		key.type = BTRFS_ROOT_ITEM_KEY;
		key.offset = io_ctl_get_u64(&io_ctl);
		max_level = level = io_ctl_get_u8(&io_ctl);

		BUG_ON(level < 0 || level >= BTRFS_MAX_LEVEL); /* incons. fs */
		del_root = btrfs_read_fs_root_no_radix(fs_info->tree_root,
							&key);
		if (IS_ERR(del_root)) {
			ret = PTR_ERR(del_root);
			BUG(); /* inconsistent fs */
		}

		root_dn = droptree_alloc_node(NULL);
		/*
		 * FIXME in this phase is should still be possible to undo
		 * everything and return a failure. Same goes for the allocation
		 * failures below
		 */
		BUG_ON(!root_dn); /* can't back out */
		dr = droptree_add_droproot(droplist, root_dn, del_root);
		BUG_ON(!dr); /* can't back out */

		stack[level] = root_dn;

		while (1) {
			u64 start;
			u64 len;
			u64 nritems;
			u32 *map;
			int n;
			int i;
			int parent_slot;
			struct droptree_node *dn;

			parent_slot = io_ctl_get_u16(&io_ctl);
			if (parent_slot == DROPTREE_STATE_GO_UP) {
				++level;
				BUG_ON(level > max_level); /* incons. fs */
				continue;
			}
			if (parent_slot == DROPTREE_STATE_GO_DOWN) {
				--level;
				BUG_ON(level < 0); /* incons. fs */
				continue;
			}
			if (parent_slot == DROPTREE_STATE_END)
				break;
			start = io_ctl_get_u64(&io_ctl);
			if (start == 0)
				break;

			len = io_ctl_get_u64(&io_ctl);
			nritems = io_ctl_get_u16(&io_ctl);
			n = DT_BITS_TO_U32(nritems);
			BUG_ON(n > 999999); /* incons. fs */
			BUG_ON(n == 0); /* incons. fs */

			map = kmalloc(n * sizeof(u32), GFP_NOFS);
			BUG_ON(!map); /* can't back out */

			for (i = 0; i < n; ++i)
				map[i] = io_ctl_get_u32(&io_ctl);

			if (level == max_level) {
				/* only for root node */
				dn = stack[level];
			} else {
				dn = droptree_alloc_node(dr);
				BUG_ON(!dn); /* can't back out */
				dn->parent = stack[level + 1];
				dn->parent_slot = parent_slot;
				list_add_tail(&dn->next,
					      &stack[level+1]->children);
				stack[level] = dn;
			}
			dn->level = level;
			dn->start = start;
			dn->len = len;
			dn->map = map;
			dn->nritems = nritems;
			dn->generation = 0;
		}
		ret = droptree_reada_root(top_rc, dr);
		BUG_ON(ret); /* can't back out */
	}
out:
	io_ctl_drop_pages(&io_ctl);
	io_ctl_free(&io_ctl);
	kfree(stack);

	return ret;
}

/*
 * called from transaction.c with a list of roots to delete
 */
void droptree_drop_list(struct btrfs_fs_info *fs_info, struct list_head *list)
{
	struct btrfs_root *root = fs_info->tree_root;
	struct inode *inode = NULL;
	struct btrfs_path *path = NULL;
	int ret;
	struct btrfs_trans_handle *trans;
	u64 alloc_hint = 0;
	u64 prealloc;
	loff_t oldsize;
	long max_nodes;
	int i;
	struct list_head droplist;
	struct droptree_root *dr;
	struct droptree_root *dtmp;
	int running_roots = 0;
	struct reada_control *top_rc = NULL;

	if (btrfs_fs_closing(fs_info))
		return;

	inode = droptree_get_inode(fs_info);
	if (IS_ERR(inode))
		goto out;

	path = btrfs_alloc_path();
	if (!path)
		goto out;

	/*
	 * create a dummy reada_control to use as a parent for all trees
	 */
	top_rc = btrfs_reada_alloc(NULL, root, NULL, NULL, NULL);
	if (!top_rc)
		goto out;
	reada_control_elem_get(top_rc);
	INIT_LIST_HEAD(&droplist);

	if (i_size_read(inode) > 0) {
		/* read */
		ret = droptree_read_state(fs_info, inode, top_rc, &droplist);
		if (ret)
			goto out;
		list_for_each_entry(dr, &droplist, list)
			++running_roots;
	}
	mutex_lock(&fs_info->droptree_lock);
	BUG_ON(fs_info->droptree_rc); /* can't happen */
	fs_info->droptree_rc = top_rc;
	mutex_unlock(&fs_info->droptree_lock);

	while (1) {
		mutex_lock(&fs_info->droptree_lock);
		while (fs_info->droptree_pause_req) {
			mutex_unlock(&fs_info->droptree_lock);
			wait_event(fs_info->droptree_wait,
				   fs_info->droptree_pause_req == 0 ||
				   btrfs_fs_closing(fs_info));
			if (btrfs_fs_closing(fs_info))
				goto end;
			mutex_lock(&fs_info->droptree_lock);
		}
		++fs_info->droptrees_running;
		mutex_unlock(&fs_info->droptree_lock);

		/*
		 * 3 for truncation, including inode update
		 * for each root, we need to delete the root_item afterwards
		 */
		trans = btrfs_start_transaction(root, 3 + DROPTREE_MAX_ROOTS);
		if (!trans) {
			btrfs_free_path(path);
			iput(inode);
			return;
		}

		max_nodes = 0;
		for (i = 0; i < BTRFS_MAX_LEVEL; ++i)
			max_nodes += fs_info->droptree_limit[i];

		/*
		 * global header (version(4), generation(8)) + terminator (8)
		 */
		prealloc = 20;

		/*
		 * per root overhead: objectid(8), offset(8), level(1) +
		 * end marker (2)
		 */
		prealloc += 19 * DROPTREE_MAX_ROOTS;

		/*
		 * per node: parent slot(2), start(8), len(8), nritems(2) + map
		 * we add also room for one UP/DOWN per node
		 */
		prealloc += (22 +
			   DT_BITS_TO_U32(BTRFS_NODEPTRS_PER_BLOCK(root)) * 4) *
			   max_nodes;
		prealloc = ALIGN(prealloc, PAGE_CACHE_SIZE);

		/*
		 * preallocate the space, so writing it later on can't fail
		 *
		 * FIXME allocate block reserve instead, to reserve space
		 * for the truncation? */
		ret = btrfs_delalloc_reserve_space(inode, prealloc);
		if (ret)
			goto out;

		/*
		 * from here on, every error is fatal and must prevent the
		 * current transaction from comitting, as that would leave an
		 * inconsistent state on disk
		 */
		oldsize = i_size_read(inode);
		if (oldsize > 0) {
			BTRFS_I(inode)->generation = 0;
			btrfs_i_size_write(inode, 0);
			truncate_pagecache(inode, oldsize, 0);

			ret = btrfs_truncate_inode_items(trans, root, inode, 0,
							 BTRFS_EXTENT_DATA_KEY);
			BUG_ON(ret); /* can't back out */

			ret = btrfs_update_inode(trans, root, inode);
			BUG_ON(ret); /* can't back out */
		}
		/*
		 * add more roots until we reach the limit
		 */
		while (running_roots < DROPTREE_MAX_ROOTS &&
		       !list_empty(list)) {
			struct btrfs_root *del_root;
			struct droptree_node *dn;

			del_root = list_first_entry(list, struct btrfs_root,
						    root_list);
			list_del(&del_root->root_list);

			ret = btrfs_del_orphan_item(trans, root,
					      del_root->root_key.objectid);
			BUG_ON(ret); /* can't back out */
			dn = droptree_alloc_node(NULL);
			BUG_ON(!dn); /* can't back out */
			dr = droptree_add_droproot(&droplist, dn, del_root);
			BUG_ON(!dr); /* can't back out */
			ret = droptree_reada_root(top_rc, dr);
			BUG_ON(ret); /* can't back out */

			++running_roots;
		}

		/*
		 * kick off the already queued jobs from the last pause,
		 * and all freshly added roots
		 */
		droptree_restart(fs_info);
		droptree_kick(fs_info);

		/*
		 * wait for all readaheads to finish. a pause will also cause
		 * the wait to end
		 */
		list_for_each_entry(dr, &droplist, list)
			btrfs_reada_wait(dr->rc);

		/*
		 * signal droptree_pause that it can continue. We still have
		 * the trans handle, so the current transaction won't commit
		 * until we've written the state to disk
		 */
		mutex_lock(&fs_info->droptree_lock);
		--fs_info->droptrees_running;
		mutex_unlock(&fs_info->droptree_lock);
		wake_up(&fs_info->droptree_wait);

		/*
		 * collect all finished droptrees
		 */
		list_for_each_entry_safe(dr, dtmp, &droplist, list) {
			struct droptree_node *dn;
			int full;
			dn = dr->top_dn;
			spin_lock(&dn->lock);
			full = dn->map &&
			       droptree_bitmap_full(dn->map, dn->nritems);
			spin_unlock(&dn->lock);
			if (full) {
				struct btrfs_root *del_root = dr->root;

				list_del(&dr->list);
				ret = btrfs_del_root(trans, fs_info->tree_root,
						     &del_root->root_key);
				BUG_ON(ret); /* can't back out */
				if (del_root->in_radix) {
					btrfs_free_fs_root(fs_info, del_root);
				} else {
					free_extent_buffer(del_root->node);
					free_extent_buffer(del_root->
								   commit_root);
					kfree(del_root);
				}
				kfree(dr->top_dn->map);
				kfree(dr->top_dn);
				kfree(dr);
				--running_roots;
			}
		}

		if (list_empty(&droplist)) {
			/*
			 * nothing in progress. Just leave the droptree inode
			 * at length zero and drop out of the loop
			 */
			btrfs_delalloc_release_space(inode, prealloc);
			btrfs_end_transaction(trans, root);
			break;
		}

		/* we reserved the space for this above */
		ret = btrfs_prealloc_file_range_trans(inode, trans, 0, 0,
						      prealloc, prealloc,
						      prealloc, &alloc_hint);
		BUG_ON(ret); /* can't back out */

		droptree_save_state(fs_info, inode, trans, &droplist);

		btrfs_end_transaction(trans, root);

		if (btrfs_fs_closing(fs_info)) {
			printk("fs is closing, abort loop\n");
			break;
		}

		/* restart loop: create new reada_controls for the roots */
		list_for_each_entry(dr, &droplist, list) {
			dr->rc = btrfs_reada_alloc(top_rc, dr->root,
						   &min_key, &max_key,
						   droptree_reada_fstree);
			/*
			 * FIXME we could handle the allocation failure in
			 * principle, as we're currently in a consistent state
			 */
			BUG_ON(!dr->rc); /* can't back out */
			kref_get(&dr->rc->refcnt);
		}
	}
end:

	/*
	 * on unmount, we come here although we're in the middle of a deletion.
	 * This means there are still allocated dropnodes we have to free. We
	 * free them by going down all the droptree_roots.
	 */
	while (!list_empty(&droplist)) {
		dr = list_first_entry(&droplist, struct droptree_root, list);
		list_del(&dr->list);
		droptree_free_tree(dr->top_dn);
		if (dr->root->in_radix) {
			btrfs_free_fs_root(fs_info, dr->root);
		} else {
			free_extent_buffer(dr->root->node);
			free_extent_buffer(dr->root->commit_root);
			kfree(dr->root);
		}
		kfree(dr);
	}
	/*
	 * also delete everything from requeue
	 */
	while (!list_empty(&fs_info->droptree_requeue)) {
		struct droptree_node *dn;

		dn = list_first_entry(&fs_info->droptree_requeue,
				      struct droptree_node, list);
		list_del(&dn->list);
		kfree(dn->map);
		kfree(dn);
	}
	/*
	 * restart queue must be empty by now
	 */
	BUG_ON(!list_empty(&fs_info->droptree_restart)); /* can't happen */
out:
	if (path)
		btrfs_free_path(path);
	if (inode)
		iput(inode);
	if (top_rc) {
		mutex_lock(&fs_info->droptree_lock);
		fs_info->droptree_rc = NULL;
		mutex_unlock(&fs_info->droptree_lock);
		reada_control_elem_put(top_rc);
	}
}
