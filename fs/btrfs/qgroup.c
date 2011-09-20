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

#include <linux/sched.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/rbtree.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "ctree.h"
#include "transaction.h"
#include "disk-io.h"
#include "locking.h"
#include "ulist.h"
#include "ioctl.h"

/* TODO XXX FIXME
 *  - subvol delete -> delete when ref goes to 0? delete limits also?
 *  - reorganize keys
 *  - compressed
 *  - sync
 *  - rescan
 *  - copy also limits on subvol creation
 *  - limit
 *  - caches fuer ulists
 *  - performance benchmarks
 *  - check all ioctl parameters
 */

/*
 * one struct for each qgroup, organized in fs_info->qgroup_tree.
 */
struct btrfs_qgroup {
	u64 qgroupid;

	/*
	 * state
	 */
	u64 rfer;	/* referenced */
	u64 rfer_cmpr;	/* referenced compressed */
	u64 excl;	/* exclusive */
	u64 excl_cmpr;	/* exclusive compressed */

	/*
	 * limits
	 */
	u64 lim_flags;	/* which limits are set */
	u64 max_rfer;
	u64 max_excl;
	u64 rsv_rfer;
	u64 rsv_excl;

	/*
	 * reservation tracking
	 */
	u64 reserved;

	/*
	 * lists
	 */
	struct list_head groups;  /* groups this group is member of */
	struct list_head members; /* groups that are members of this group */
	struct list_head dirty;   /* dirty groups */
	struct rb_node node;	  /* tree of qgroups */

	/*
	 * temp variables for accounting operations
	 */
	u64 tag;
	u64 refcnt;
};

/*
 * glue structure to represent the relations between qgroups.
 */
struct btrfs_qgroup_list {
	struct list_head next_group;
	struct list_head next_member;
	struct btrfs_qgroup *group;
	struct btrfs_qgroup *member;
};

/* must be called with qgroup_lock held */
static struct btrfs_qgroup *find_qgroup_rb(struct btrfs_fs_info *fs_info,
					   u64 qgroupid)
{
	struct rb_node *n = fs_info->qgroup_tree.rb_node;
	struct btrfs_qgroup *qgroup;

	while (n) {
		qgroup = rb_entry(n, struct btrfs_qgroup, node);
		if (qgroup->qgroupid < qgroupid)
			n = n->rb_left;
		else if (qgroup->qgroupid > qgroupid)
			n = n->rb_right;
		else
			return qgroup;
	}
	return NULL;
}

/* must be called with qgroup_lock held */
static struct btrfs_qgroup *add_qgroup_rb(struct btrfs_fs_info *fs_info,
					  u64 qgroupid)
{
	struct rb_node **p = &fs_info->qgroup_tree.rb_node;
	struct rb_node *parent = NULL;
	struct btrfs_qgroup *qgroup;

	while (*p) {
		parent = *p;
		qgroup = rb_entry(parent, struct btrfs_qgroup, node);

		if (qgroup->qgroupid < qgroupid)
			p = &(*p)->rb_left;
		else if (qgroup->qgroupid > qgroupid)
			p = &(*p)->rb_right;
		else
			return qgroup;
	}

	qgroup = kzalloc(sizeof(*qgroup), GFP_NOFS);
	if (!qgroup)
		return ERR_PTR(-ENOMEM);

	qgroup->qgroupid = qgroupid;
	INIT_LIST_HEAD(&qgroup->groups);
	INIT_LIST_HEAD(&qgroup->members);
	INIT_LIST_HEAD(&qgroup->dirty);

	rb_link_node(&qgroup->node, parent, p);
	rb_insert_color(&qgroup->node, &fs_info->qgroup_tree);

	return qgroup;
}

/* must be called with qgroup_lock held */
static int del_qgroup_rb(struct btrfs_fs_info *fs_info, u64 qgroupid)
{
	struct btrfs_qgroup *qgroup = find_qgroup_rb(fs_info, qgroupid);
	struct btrfs_qgroup_list *list;

	if (!qgroup)
		return -ENOENT;

	rb_erase(&qgroup->node, &fs_info->qgroup_tree);
	list_del(&qgroup->dirty);

	while (!list_empty(&qgroup->groups)) {
		list = list_first_entry(&qgroup->groups,
					struct btrfs_qgroup_list, next_group);
		list_del(&list->next_group);
		list_del(&list->next_member);
		kfree(list);
	}

	while (!list_empty(&qgroup->members)) {
		list = list_first_entry(&qgroup->members,
					struct btrfs_qgroup_list, next_member);
		list_del(&list->next_group);
		list_del(&list->next_member);
		kfree(list);
	}
	kfree(qgroup);

	return 0;
}

/* must be called with qgroup_lock held */
static int add_relation_rb(struct btrfs_fs_info *fs_info,
			   u64 memberid, u64 parentid)
{
	struct btrfs_qgroup *member;
	struct btrfs_qgroup *parent;
	struct btrfs_qgroup_list *list;

	member = find_qgroup_rb(fs_info, memberid);
	parent = find_qgroup_rb(fs_info, parentid);
	if (!member || !parent)
		return -ENOENT;

	list = kzalloc(sizeof(*list), GFP_NOFS);
	if (!list)
		return -ENOMEM;

	list->group = parent;
	list->member = member;
	list_add_tail(&list->next_group, &member->groups);
	list_add_tail(&list->next_member, &parent->members);

	return 0;
}

/* must be called with qgroup_lock held */
static int del_relation_rb(struct btrfs_fs_info *fs_info,
			   u64 memberid, u64 parentid)
{
	struct btrfs_qgroup *member;
	struct btrfs_qgroup *parent;
	struct btrfs_qgroup_list *list;

	member = find_qgroup_rb(fs_info, memberid);
	parent = find_qgroup_rb(fs_info, parentid);
	if (!member || !parent)
		return -ENOENT;

	list_for_each_entry(list, &member->groups, next_group) {
		if (list->group == parent) {
			list_del(&list->next_group);
			list_del(&list->next_member);
			kfree(list);
			return 0;
		}
	}
	return -ENOENT;
}

/*
 * The full config is read in one go, only called from open_ctree()
 * It doesn't use any locking, as at this point we're still single-threaded
 */
int btrfs_read_qgroup_config(struct btrfs_fs_info *fs_info)
{
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_path *path = NULL;
	struct extent_buffer *l;
	int slot;
	int ret = 0;
	u64 flags = 0;

	if (!fs_info->quota_enabled)
		return 0;

	path = btrfs_alloc_path();
	if (!path) {
		ret = -ENOMEM;
		goto out;
	}

	/* default this to quota off, in case no status key is found */
	fs_info->qgroup_flags = 0;

	/*
	 * pass 1: read status, all qgroup infos and limits
	 */
	key.objectid = 0;
	key.type = 0;
	key.offset = 0;
	ret = btrfs_search_slot_for_read(quota_root, &key, path, 1, 1);
	if (ret)
		goto out;

	while (1) {
		struct btrfs_qgroup *qgroup;

		slot = path->slots[0];
		l = path->nodes[0];
		btrfs_item_key_to_cpu(l, &found_key, slot);

		if (found_key.type == BTRFS_QGROUP_STATUS_KEY) {
			struct btrfs_qgroup_status_item *ptr;

			ptr = btrfs_item_ptr(l, slot,
					     struct btrfs_qgroup_status_item);

			if (btrfs_qgroup_status_version(l, ptr) !=
			    BTRFS_QGROUP_STATUS_VERSION) {
				printk(KERN_ERR
				 "btrfs: old qgroup version, quota disabled\n");
				goto out;
			}
			if (btrfs_qgroup_status_generation(l, ptr) !=
			    fs_info->generation) {
				flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
				printk(KERN_ERR
					"btrfs: qgroup generation mismatch, "
					"marked as inconsistent\n");
			}
			fs_info->qgroup_flags = btrfs_qgroup_status_flags(l,
									  ptr);
			/* FIXME read scan element */
			goto next1;
		}

		if (found_key.type != BTRFS_QGROUP_INFO_KEY &&
		    found_key.type != BTRFS_QGROUP_LIMIT_KEY)
			goto next1;

		qgroup = find_qgroup_rb(fs_info, found_key.offset);
		if ((qgroup && found_key.type == BTRFS_QGROUP_INFO_KEY) ||
		    (!qgroup && found_key.type == BTRFS_QGROUP_LIMIT_KEY)) {
			printk(KERN_ERR "btrfs: inconsitent qgroup config\n");
			flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
		}
		if (!qgroup) {
			qgroup = add_qgroup_rb(fs_info, found_key.offset);
			if (IS_ERR(qgroup)) {
				ret = PTR_ERR(qgroup);
				goto out;
			}
		}
		switch (found_key.type) {
		case BTRFS_QGROUP_INFO_KEY: {
			struct btrfs_qgroup_info_item *ptr;

			ptr = btrfs_item_ptr(l, slot,
					     struct btrfs_qgroup_info_item);
			qgroup->rfer = btrfs_qgroup_info_rfer(l, ptr);
			qgroup->rfer_cmpr = btrfs_qgroup_info_rfer_cmpr(l, ptr);
			qgroup->excl = btrfs_qgroup_info_excl(l, ptr);
			qgroup->excl_cmpr = btrfs_qgroup_info_excl_cmpr(l, ptr);
			/* generation currently unused */
			break;
		}
		case BTRFS_QGROUP_LIMIT_KEY: {
			struct btrfs_qgroup_limit_item *ptr;

			ptr = btrfs_item_ptr(l, slot,
					     struct btrfs_qgroup_limit_item);
			qgroup->lim_flags = btrfs_qgroup_limit_flags(l, ptr);
			qgroup->max_rfer = btrfs_qgroup_limit_max_rfer(l, ptr);
			qgroup->max_excl = btrfs_qgroup_limit_max_excl(l, ptr);
			qgroup->rsv_rfer = btrfs_qgroup_limit_rsv_rfer(l, ptr);
			qgroup->rsv_excl = btrfs_qgroup_limit_rsv_excl(l, ptr);
			break;
		}
		}
next1:
		ret = btrfs_next_item(quota_root, path);
		if (ret < 0)
			goto out;
		if (ret)
			break;
	}
	btrfs_release_path(path);

	/*
	 * pass 2: read all qgroup relations
	 */
	key.objectid = 0;
	key.type = BTRFS_QGROUP_RELATION_KEY;
	key.offset = 0;
	ret = btrfs_search_slot_for_read(quota_root, &key, path, 1, 0);
	if (ret)
		goto out;
	while (1) {
		slot = path->slots[0];
		l = path->nodes[0];
		btrfs_item_key_to_cpu(l, &found_key, slot);

		if (found_key.type != BTRFS_QGROUP_RELATION_KEY)
			goto next2;

		if (found_key.objectid > found_key.offset) {
			/* parent <- member, not needed to build config */
			/* FIXME should we omit the key completely? */
			goto next2;
		}

		ret = add_relation_rb(fs_info, found_key.objectid,
				      found_key.offset);
		if (ret)
			goto out;
next2:
		ret = btrfs_next_item(quota_root, path);
		if (ret < 0)
			goto out;
		if (ret)
			break;
	}
out:
	fs_info->qgroup_flags |= flags;
	if (!(fs_info->qgroup_flags & BTRFS_QGROUP_STATUS_FLAG_ON)) {
		fs_info->quota_enabled = 0;
		fs_info->pending_quota_state = 0;
	}
	btrfs_free_path(path);

	return ret < 0 ? ret : 0;
}

/*
 * This is only called from close_ctree() or open_ctree(), both in single-
 * treaded paths. Clean up the in-memory structures. No locking needed.
 */
void btrfs_free_qgroup_config(struct btrfs_fs_info *fs_info)
{
	struct rb_node *n;
	struct btrfs_qgroup *qgroup;
	struct btrfs_qgroup_list *list;

	while ((n = rb_first(&fs_info->qgroup_tree))) {
		qgroup = rb_entry(n, struct btrfs_qgroup, node);
		rb_erase(n, &fs_info->qgroup_tree);

		WARN_ON(!list_empty(&qgroup->dirty));

		while (!list_empty(&qgroup->groups)) {
			list = list_first_entry(&qgroup->groups,
						struct btrfs_qgroup_list,
						next_group);
			list_del(&list->next_group);
			list_del(&list->next_member);
			kfree(list);
		}

		while (!list_empty(&qgroup->members)) {
			list = list_first_entry(&qgroup->members,
						struct btrfs_qgroup_list,
						next_member);
			list_del(&list->next_group);
			list_del(&list->next_member);
			kfree(list);
		}
		kfree(qgroup);
	}
}

static int add_qgroup_relation_item(struct btrfs_trans_handle *trans,
				    struct btrfs_root *quota_root,
				    u64 src, u64 dst)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_key key;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = src;
	key.type = BTRFS_QGROUP_RELATION_KEY;
	key.offset = dst;

	ret = btrfs_insert_empty_item(trans, quota_root, path, &key, 0);

	btrfs_mark_buffer_dirty(path->nodes[0]);

	btrfs_free_path(path);
	return ret;
}

static int del_qgroup_relation_item(struct btrfs_trans_handle *trans,
				    struct btrfs_root *quota_root,
				    u64 src, u64 dst)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_key key;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = src;
	key.type = BTRFS_QGROUP_RELATION_KEY;
	key.offset = dst;

	ret = btrfs_search_slot(trans, quota_root, &key, path, -1, 1);
	if (ret < 0)
		goto out;

	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_del_item(trans, quota_root, path);
out:
	btrfs_free_path(path);
	return ret;
}

static int add_qgroup_item(struct btrfs_trans_handle *trans,
			   struct btrfs_root *quota_root, u64 qgroupid)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_qgroup_info_item *qgroup_info;
	struct btrfs_qgroup_limit_item *qgroup_limit;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_INFO_KEY;
	key.offset = qgroupid;

	ret = btrfs_insert_empty_item(trans, quota_root, path, &key,
				      sizeof(*qgroup_info));
	if (ret)
		goto out;

	leaf = path->nodes[0];
	qgroup_info = btrfs_item_ptr(leaf, path->slots[0],
				 struct btrfs_qgroup_info_item);
	btrfs_set_qgroup_info_generation(leaf, qgroup_info, trans->transid);
	btrfs_set_qgroup_info_rfer(leaf, qgroup_info, 0);
	btrfs_set_qgroup_info_rfer_cmpr(leaf, qgroup_info, 0);
	btrfs_set_qgroup_info_excl(leaf, qgroup_info, 0);
	btrfs_set_qgroup_info_excl_cmpr(leaf, qgroup_info, 0);

	btrfs_mark_buffer_dirty(leaf);

	btrfs_release_path(path);

	key.type = BTRFS_QGROUP_LIMIT_KEY;
	ret = btrfs_insert_empty_item(trans, quota_root, path, &key,
				      sizeof(*qgroup_limit));
	if (ret)
		goto out;

	leaf = path->nodes[0];
	qgroup_limit = btrfs_item_ptr(leaf, path->slots[0],
				  struct btrfs_qgroup_limit_item);
	btrfs_set_qgroup_limit_flags(leaf, qgroup_limit, 0);
	btrfs_set_qgroup_limit_max_rfer(leaf, qgroup_limit, 0);
	btrfs_set_qgroup_limit_max_excl(leaf, qgroup_limit, 0);
	btrfs_set_qgroup_limit_rsv_rfer(leaf, qgroup_limit, 0);
	btrfs_set_qgroup_limit_rsv_excl(leaf, qgroup_limit, 0);

	btrfs_mark_buffer_dirty(leaf);

	ret = 0;
out:
	btrfs_free_path(path);
	return ret;
}

static int del_qgroup_item(struct btrfs_trans_handle *trans,
			   struct btrfs_root *quota_root, u64 qgroupid)
{
	int ret;
	struct btrfs_path *path;
	struct btrfs_key key;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_INFO_KEY;
	key.offset = qgroupid;
	ret = btrfs_search_slot(trans, quota_root, &key, path, -1, 1);
	if (ret < 0)
		goto out;

	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_del_item(trans, quota_root, path);
	if (ret)
		goto out;

	btrfs_release_path(path);

	key.type = BTRFS_QGROUP_LIMIT_KEY;
	ret = btrfs_search_slot(trans, quota_root, &key, path, -1, 1);
	if (ret < 0)
		goto out;

	if (ret > 0) {
		ret = -ENOENT;
		goto out;
	}

	ret = btrfs_del_item(trans, quota_root, path);

out:
	btrfs_free_path(path);
	return ret;
}

static int update_qgroup_limit_item(struct btrfs_trans_handle *trans,
				    struct btrfs_root *root, u64 qgroupid,
				    u64 flags, u64 max_rfer, u64 max_excl,
				    u64 rsv_rfer, u64 rsv_excl)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_qgroup_limit_item *qgroup_limit;
	int ret;
	int slot;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_LIMIT_KEY;
	key.offset = qgroupid;

	path = btrfs_alloc_path();
	BUG_ON(!path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;

	if (ret)
		goto out;

	l = path->nodes[0];
	slot = path->slots[0];
	qgroup_limit = btrfs_item_ptr(l, path->slots[0],
				      struct btrfs_qgroup_limit_item);
	btrfs_set_qgroup_limit_flags(l, qgroup_limit, flags);
	btrfs_set_qgroup_limit_max_rfer(l, qgroup_limit, max_rfer);
	btrfs_set_qgroup_limit_max_excl(l, qgroup_limit, max_excl);
	btrfs_set_qgroup_limit_rsv_rfer(l, qgroup_limit, rsv_rfer);
	btrfs_set_qgroup_limit_rsv_excl(l, qgroup_limit, rsv_excl);

	btrfs_mark_buffer_dirty(l);

out:
	btrfs_free_path(path);
	return ret;
}

static int update_qgroup_info_item(struct btrfs_trans_handle *trans,
				   struct btrfs_root *root,
				   struct btrfs_qgroup *qgroup)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_qgroup_info_item *qgroup_info;
	int ret;
	int slot;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_INFO_KEY;
	key.offset = qgroup->qgroupid;

	path = btrfs_alloc_path();
	BUG_ON(!path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;

	if (ret)
		goto out;

	l = path->nodes[0];
	slot = path->slots[0];
	qgroup_info = btrfs_item_ptr(l, path->slots[0],
				 struct btrfs_qgroup_info_item);
	btrfs_set_qgroup_info_generation(l, qgroup_info, trans->transid);
	btrfs_set_qgroup_info_rfer(l, qgroup_info, qgroup->rfer);
	btrfs_set_qgroup_info_rfer_cmpr(l, qgroup_info, qgroup->rfer_cmpr);
	btrfs_set_qgroup_info_excl(l, qgroup_info, qgroup->excl);
	btrfs_set_qgroup_info_excl_cmpr(l, qgroup_info, qgroup->excl_cmpr);

	btrfs_mark_buffer_dirty(l);

out:
	btrfs_free_path(path);
	return ret;
}

static int update_qgroup_status_item(struct btrfs_trans_handle *trans,
				     struct btrfs_fs_info *fs_info,
				    struct btrfs_root *root)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	struct extent_buffer *l;
	struct btrfs_qgroup_status_item *ptr;
	int ret;
	int slot;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_STATUS_KEY;
	key.offset = 0;

	path = btrfs_alloc_path();
	BUG_ON(!path);
	ret = btrfs_search_slot(trans, root, &key, path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;

	if (ret)
		goto out;

	l = path->nodes[0];
	slot = path->slots[0];
	ptr = btrfs_item_ptr(l, slot, struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_flags(l, ptr, fs_info->qgroup_flags);
	btrfs_set_qgroup_status_generation(l, ptr, trans->transid);
	/* XXX scan */

	btrfs_mark_buffer_dirty(l);

out:
	btrfs_free_path(path);
	return ret;
}

/*
 * called with qgroup_lock held
 */
static int btrfs_clean_quota_tree(struct btrfs_trans_handle *trans,
				  struct btrfs_root *root)
{
	struct btrfs_path *path;
	struct btrfs_key key;
	int ret;

	if (!root)
		return -EINVAL;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while (1) {
		key.objectid = 0;
		key.offset = 0;
		key.type = 0;

		path->leave_spinning = 1;
		ret = btrfs_search_slot(trans, root, &key, path, -1, 1);
		if (ret > 0) {
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
		} else if (ret < 0) {
			break;
		}

		ret = btrfs_del_item(trans, root, path);
		if (ret)
			goto out;
		btrfs_release_path(path);
	}
	ret = 0;
out:
	root->fs_info->pending_quota_state = 0;
	btrfs_free_path(path);
	return ret;
}

int btrfs_quota_enable(struct btrfs_trans_handle *trans,
		       struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *quota_root;
	struct btrfs_path *path = NULL;
	struct btrfs_qgroup_status_item *ptr;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int ret = 0;

	spin_lock(&fs_info->qgroup_lock);
	if (fs_info->quota_root) {
		fs_info->pending_quota_state = 1;
		spin_unlock(&fs_info->qgroup_lock);
		goto out;
	}
	spin_unlock(&fs_info->qgroup_lock);

	/*
	 * initially create the quota tree
	 */
	quota_root = btrfs_create_tree(trans, fs_info,
				       BTRFS_QUOTA_TREE_OBJECTID);
	if (IS_ERR(quota_root)) {
		ret =  PTR_ERR(quota_root);
		goto out;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_STATUS_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, quota_root, path, &key,
				      sizeof(*ptr));
	if (ret)
		goto out;

	leaf = path->nodes[0];
	ptr = btrfs_item_ptr(leaf, path->slots[0],
				 struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_generation(leaf, ptr, trans->transid);
	btrfs_set_qgroup_status_version(leaf, ptr, BTRFS_QGROUP_STATUS_VERSION);
	fs_info->qgroup_flags = BTRFS_QGROUP_STATUS_FLAG_ON |
				BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
	btrfs_set_qgroup_status_flags(leaf, ptr, fs_info->qgroup_flags);
	btrfs_set_qgroup_status_scan(leaf, ptr, 0);

	btrfs_mark_buffer_dirty(leaf);

	spin_lock(&fs_info->qgroup_lock);
	fs_info->quota_root = quota_root;
	fs_info->pending_quota_state = 1;
	spin_unlock(&fs_info->qgroup_lock);
out:
	btrfs_free_path(path);
	return ret;
}

int btrfs_quota_disable(struct btrfs_trans_handle *trans,
			struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_root *quota_root;
	int ret = 0;

	spin_lock(&fs_info->qgroup_lock);
	fs_info->pending_quota_state = 0;
	quota_root = fs_info->quota_root;
	fs_info->quota_root = NULL;
	btrfs_free_qgroup_config(fs_info);
	spin_unlock(&fs_info->qgroup_lock);

	if (!quota_root)
		return -EINVAL;

	ret = btrfs_clean_quota_tree(trans, quota_root);
	if (ret)
		goto out;

	ret = btrfs_del_root(trans, tree_root, &quota_root->root_key);
	if (ret)
		goto out;

	list_del(&quota_root->dirty_list);

	btrfs_tree_lock(quota_root->node);
	clean_tree_block(trans, tree_root, quota_root->node);
	btrfs_tree_unlock(quota_root->node);
	btrfs_free_tree_block(trans, quota_root, quota_root->node, 0, 1);

	free_extent_buffer(quota_root->node);
	free_extent_buffer(quota_root->commit_root);
	kfree(quota_root);
out:
	return ret;
}

int btrfs_quota_rescan(struct btrfs_fs_info *fs_info)
{
	/* FIXME */
	return 0;
}

int btrfs_add_qgroup_relation(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *fs_info, u64 src, u64 dst)
{
	struct btrfs_root *quota_root;
	int ret = 0;

	quota_root = fs_info->quota_root;
	if (!quota_root)
		return -EINVAL;

	ret = add_qgroup_relation_item(trans, quota_root, src, dst);
	if (ret)
		return ret;

	ret = add_qgroup_relation_item(trans, quota_root, dst, src);
	if (ret) {
		del_qgroup_relation_item(trans, quota_root, src, dst);
		return ret;
	}

	spin_lock(&fs_info->qgroup_lock);
	ret = add_relation_rb(quota_root->fs_info, src, dst);
	spin_unlock(&fs_info->qgroup_lock);

	return ret;
}

int btrfs_del_qgroup_relation(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *fs_info, u64 src, u64 dst)
{
	struct btrfs_root *quota_root;
	int ret = 0;
	int err;

	quota_root = fs_info->quota_root;
	if (!quota_root)
		return -EINVAL;

	ret = del_qgroup_relation_item(trans, quota_root, src, dst);
	err = del_qgroup_relation_item(trans, quota_root, dst, src);
	if (err && !ret)
		ret = err;

	spin_lock(&fs_info->qgroup_lock);
	del_relation_rb(fs_info, src, dst);

	spin_unlock(&fs_info->qgroup_lock);

	return ret;
}

int btrfs_create_qgroup(struct btrfs_trans_handle *trans,
			struct btrfs_fs_info *fs_info, u64 qgroupid, char *name)
{
	struct btrfs_root *quota_root;
	struct btrfs_qgroup *qgroup;
	int ret = 0;

	quota_root = fs_info->quota_root;
	if (!quota_root)
		return -EINVAL;

	ret = add_qgroup_item(trans, quota_root, qgroupid);

	spin_lock(&fs_info->qgroup_lock);
	qgroup = add_qgroup_rb(fs_info, qgroupid);
	if (IS_ERR(qgroup))
		ret = PTR_ERR(qgroup);

	spin_unlock(&fs_info->qgroup_lock);

	return ret;
}

int btrfs_remove_qgroup(struct btrfs_trans_handle *trans,
			struct btrfs_fs_info *fs_info, u64 qgroupid)
{
	struct btrfs_root *quota_root;
	int ret = 0;

	quota_root = fs_info->quota_root;
	if (!quota_root)
		return -EINVAL;

	ret = del_qgroup_item(trans, quota_root, qgroupid);

	spin_lock(&fs_info->qgroup_lock);
	del_qgroup_rb(quota_root->fs_info, qgroupid);

	spin_unlock(&fs_info->qgroup_lock);

	return ret;
}

int btrfs_limit_qgroup(struct btrfs_trans_handle *trans,
		       struct btrfs_fs_info *fs_info, u64 qgroupid,
		       struct btrfs_qgroup_limit *limit)
{
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_qgroup *qgroup;
	int ret = 0;

	if (!quota_root)
		return -EINVAL;

	ret = update_qgroup_limit_item(trans, quota_root, qgroupid,
				       limit->flags, limit->max_rfer,
				       limit->max_excl, limit->rsv_rfer,
				       limit->rsv_excl);
	if (ret) {
		fs_info->qgroup_flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
		printk(KERN_INFO "unable to update quota limit for %llu\n",
		       (unsigned long long)qgroupid);
	}

	spin_lock(&fs_info->qgroup_lock);

	qgroup = find_qgroup_rb(fs_info, qgroupid);
	if (!qgroup) {
		ret = -ENOENT;
		goto unlock;
	}
	qgroup->lim_flags = limit->flags;
	qgroup->max_rfer = limit->max_rfer;
	qgroup->max_excl = limit->max_excl;
	qgroup->rsv_rfer = limit->rsv_rfer;
	qgroup->rsv_excl = limit->rsv_excl;

unlock:
	spin_unlock(&fs_info->qgroup_lock);

	return ret;
}

/*
 * this structure records all encountered refs on the way up to the root
 */
struct __prelim_ref {
	struct list_head list;
	u64 root_id;
	struct btrfs_key key;
	int level;
	int count;
	u64 parent;
};

static int __add_prelim_ref(struct list_head *head, u64 root_id,
			    struct btrfs_key *key, int level, u64 parent,
			    int count)
{
	struct __prelim_ref *ref;

	ref = kmalloc(sizeof(*ref), GFP_NOFS);
	if (!ref)
		return -ENOMEM;

	ref->root_id = root_id;
	if (key)
		ref->key = *key;
	else
		memset(&ref->key, 0, sizeof(ref->key));
	ref->level = level;
	ref->count = count;
	ref->parent = parent;
	list_add_tail(&ref->list, head);

	return 0;
}

/*
 * resolve an indirect backref in the form (root_id, key, level)
 * to a logical address
 */
static int __resolve_indirect_ref(struct btrfs_fs_info *fs_info, u64 root_id,
				 struct btrfs_key *key, int level,
				 u64 *parent)
{
	struct btrfs_path *path;
	struct btrfs_root *root;
	struct btrfs_key root_key;
	int ret = 0;
	int root_level;

	*parent = 0;
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	root_key.objectid = root_id;
	root_key.type = BTRFS_ROOT_ITEM_KEY;
	root_key.offset = (u64)-1;
	root = btrfs_read_fs_root_no_name(fs_info, &root_key);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto out;
	}

	rcu_read_lock();
	root_level = btrfs_header_level(root->node);
	rcu_read_unlock();

	if (root_level + 1 == level)
		goto out;

	path->lowest_level = level;
	path->nested = 1;
	ret = btrfs_search_slot(NULL, root, key, path, 0, 0);

	/* FIXME can we just add the full path to root? */
	if (ret >= 0) {
		if (path->nodes[level])
			*parent = path->nodes[level]->start;
		else
			WARN_ON(1);
	}

out:
	btrfs_free_path(path);

	return 0;
}

/*
 * resolve all indirect backrefs from the list
 */
static int __resolve_indirect_refs(struct btrfs_fs_info *fs_info,
				   struct list_head *head)
{
	int err;
	int ret = 0;
	struct __prelim_ref *ref;
	u64 parent = 0;

	list_for_each_entry(ref, head, list) {
		if (ref->parent)	/* already direct */
			continue;
		if (ref->count == 0)
			continue;
		err = __resolve_indirect_ref(fs_info, ref->root_id,
					     &ref->key, ref->level, &parent);
		if (err == 0)
			ref->parent = parent;
		if (err && ret == 0)
			ret = err;
	}

	return ret;
}

/*
 * merge two lists of backrefs and adjust counts accordingly
 *
 * mode = 1: merge identical keys, if key is set
 * mode = 2: merge identical parents
 */
static int __merge_refs(struct list_head *head, int mode)
{
	struct list_head *pos1;

	list_for_each(pos1, head) {
		struct list_head *n2;
		struct list_head *pos2;
		struct __prelim_ref *ref1;

		ref1 = list_entry(pos1, struct __prelim_ref, list);

		if (mode == 1 && ref1->key.type == 0)
			continue;
		for (pos2 = pos1->next, n2 = pos2->next; pos2 != head;
		     pos2 = n2, n2 = pos2->next) {
			struct __prelim_ref *ref2;

			ref2 = list_entry(pos2, struct __prelim_ref, list);

			if (mode == 1) {
				if (memcmp(&ref1->key, &ref2->key,
					   sizeof(ref1->key)) ||
				    ref1->level != ref2->level ||
				    ref1->root_id != ref2->root_id)
					continue;
				ref1->count += ref2->count;
			} else {
				if (ref1->parent != ref2->parent)
					continue;
				ref1->count += ref2->count;
			}
			list_del(&ref2->list);
			kfree(ref2);
		}

	}
	return 0;
}

/*
 * add all currently queued delayed refs from this head whose seq nr is
 * smaller or equal that seq to the list
 */
static int __add_delayed_refs(struct btrfs_delayed_ref_head *head, u64 seq,
			      struct btrfs_key *info_key,
			      struct list_head *prefs)
{
	struct btrfs_delayed_extent_op *extent_op = head->extent_op;
	struct rb_node *n = &head->node.rb_node;
	int sgn;
	int ret;

	if (extent_op && extent_op->update_key)
		btrfs_disk_key_to_cpu(info_key, &extent_op->key);

	while ((n = rb_prev(n))) {
		struct btrfs_delayed_ref_node *node;
		node = rb_entry(n, struct btrfs_delayed_ref_node,
				rb_node);
		if (node->bytenr != head->node.bytenr)
			break;
		WARN_ON(node->is_head);

		if (node->seq > seq)
			continue;

		switch (node->action) {
		case BTRFS_ADD_DELAYED_EXTENT:
		case BTRFS_UPDATE_DELAYED_HEAD:
			WARN_ON(1);
			continue;
		case BTRFS_ADD_DELAYED_REF:
			sgn = 1;
			break;
		case BTRFS_DROP_DELAYED_REF:
			sgn = -1;
			break;
		default:
			BUG_ON(1);
		}
		switch (node->type) {
		case BTRFS_TREE_BLOCK_REF_KEY: {
			struct btrfs_delayed_tree_ref *ref;

			ref = btrfs_delayed_node_to_tree_ref(node);
			ret = __add_prelim_ref(prefs, ref->root, info_key,
					      ref->level + 1, 0,
					      node->ref_mod * sgn);
			break;
		}
		case BTRFS_SHARED_BLOCK_REF_KEY: {
			struct btrfs_delayed_tree_ref *ref;

			ref = btrfs_delayed_node_to_tree_ref(node);
			ret = __add_prelim_ref(prefs, ref->root, info_key,
					      ref->level + 1, ref->parent,
					      node->ref_mod * sgn);
			break;
		}
		case BTRFS_EXTENT_DATA_REF_KEY: {
			struct btrfs_delayed_data_ref *ref;
			struct btrfs_key key;

			ref = btrfs_delayed_node_to_data_ref(node);

			key.objectid = ref->objectid;
			key.type = BTRFS_INODE_ITEM_KEY;
			key.offset = ref->offset;
			ret = __add_prelim_ref(prefs, ref->root, &key, 0, 0,
					       node->ref_mod * sgn);
			break;
		}
		case BTRFS_SHARED_DATA_REF_KEY: {
			struct btrfs_delayed_data_ref *ref;
			struct btrfs_key key;

			ref = btrfs_delayed_node_to_data_ref(node);

			key.objectid = ref->objectid;
			key.type = BTRFS_INODE_ITEM_KEY;
			key.offset = ref->offset;
			ret = __add_prelim_ref(prefs, ref->root, &key, 0,
					       ref->parent,
					       node->ref_mod * sgn);
			break;
		}
		default:
			WARN_ON(1);
		}
		BUG_ON(ret);
	}

	return 0;
}

/*
 * add all inline backrefs for bytenr to the list
 */
static int __add_inline_refs(struct btrfs_fs_info *fs_info,
			     struct btrfs_path *path, u64 bytenr,
			     struct btrfs_key *info_key, int *info_level,
			     struct list_head *prefs)
{
	int ret;
	int slot;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	unsigned long ptr;
	unsigned long end;
	struct btrfs_extent_item *ei;
	u64 flags;
	u64 item_size;

	/*
	 * enumerate all inline refs
	 */
	leaf = path->nodes[0];
	slot = path->slots[0] - 1;

	item_size = btrfs_item_size_nr(leaf, slot);
	BUG_ON(item_size < sizeof(*ei));

	ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
	flags = btrfs_extent_flags(leaf, ei);

	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + item_size;

	if (flags & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		struct btrfs_tree_block_info *info;
		struct btrfs_disk_key disk_key;

		info = (struct btrfs_tree_block_info *)ptr;
		*info_level = btrfs_tree_block_level(leaf, info);
		btrfs_tree_block_key(leaf, info, &disk_key);
		btrfs_disk_key_to_cpu(info_key, &disk_key);
		ptr += sizeof(struct btrfs_tree_block_info);
		BUG_ON(ptr > end);
	} else {
		BUG_ON(!(flags & BTRFS_EXTENT_FLAG_DATA));
	}

	while (ptr < end) {
		struct btrfs_extent_inline_ref *iref;
		u64 offset;
		int type;

		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		offset = btrfs_extent_inline_ref_offset(leaf, iref);

		switch (type) {
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefs, 0, NULL, 0, offset, 1);
			break;
		case BTRFS_SHARED_DATA_REF_KEY: {
			struct btrfs_shared_data_ref *sdref;

			sdref = (struct btrfs_shared_data_ref *)(iref + 1);
			ret = __add_prelim_ref(prefs, 0, NULL, 0, offset,
				     btrfs_shared_data_ref_count(leaf, sdref));
			break;
		}
		case BTRFS_TREE_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefs, offset, info_key,
					       *info_level + 1, 0, 1);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY: {
			struct btrfs_extent_data_ref *dref;
			u64 root;

			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			key.objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			key.type = BTRFS_INODE_ITEM_KEY;
			key.offset = btrfs_extent_data_ref_offset(leaf, dref);
			root = btrfs_extent_data_ref_root(leaf, dref);
			ret = __add_prelim_ref(prefs, root, &key, 0, 0,
				btrfs_extent_data_ref_count(leaf, dref));
			break;
		}
		default:
			WARN_ON(1);
		}
		BUG_ON(ret);
		ptr += btrfs_extent_inline_ref_size(type);
	}

	return 0;
}

/*
 * add all non-inline backrefs for bytenr to the list
 */
static int __add_keyed_refs(struct btrfs_fs_info *fs_info,
			    struct btrfs_path *path, u64 bytenr,
			    struct btrfs_key *info_key, int info_level,
			    struct list_head *prefs)
{
	struct btrfs_root *extent_root = fs_info->extent_root;
	int ret;
	int slot;
	struct extent_buffer *leaf;
	struct btrfs_key key;

	while (1) {
		ret = btrfs_next_item(extent_root, path);
		if (ret < 0)
			break;
		if (ret) {
			ret = 0;
			break;
		}

		slot = path->slots[0];
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);

		if (key.objectid != bytenr)
			break;
		if (key.type < BTRFS_TREE_BLOCK_REF_KEY)
			continue;
		if (key.type > BTRFS_SHARED_DATA_REF_KEY)
			break;

		switch (key.type) {
		case BTRFS_SHARED_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefs, 0, NULL, 0,
					       key.offset, 1);
			break;
		case BTRFS_SHARED_DATA_REF_KEY: {
			struct btrfs_shared_data_ref *sdref;

			sdref = btrfs_item_ptr(leaf, slot,
					      struct btrfs_shared_data_ref);
			ret = __add_prelim_ref(prefs, 0, NULL, 0, key.offset,
				     btrfs_shared_data_ref_count(leaf, sdref));
			break;
		}
		case BTRFS_TREE_BLOCK_REF_KEY:
			ret = __add_prelim_ref(prefs, key.offset,
					info_key, info_level + 1, 0, 1);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY: {
			struct btrfs_extent_data_ref *dref;
			u64 root;

			dref = btrfs_item_ptr(leaf, slot,
					      struct btrfs_extent_data_ref);
			key.objectid = btrfs_extent_data_ref_objectid(leaf,
								      dref);
			key.type = BTRFS_INODE_ITEM_KEY;
			key.offset = btrfs_extent_data_ref_offset(leaf, dref);
			root = btrfs_extent_data_ref_root(leaf, dref);
			ret = __add_prelim_ref(prefs, root, &key, 0, 0,
				btrfs_extent_data_ref_count(leaf, dref));
			break;
		}
		default:
			WARN_ON(1);
		}
		BUG_ON(ret);
	}

	return ret;
}

/*
 * this adds all existing backrefs (inline backrefs, backrefs and delayed
 * refs) for the given bytenr to the refs list, merges duplicates and resolves
 * indirect refs to their parent bytenr.
 * When roots are found, they're added to the roots list
 *
 * FIXME some caching might speed things up
 */
static int __find_all_roots(struct btrfs_trans_handle *trans,
			    struct btrfs_fs_info *fs_info, u64 bytenr,
			    struct ulist *roots, struct ulist *refs, u64 seq)
{
	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_key info_key = { 0 };
	struct btrfs_delayed_ref_root *delayed_refs = NULL;
	struct btrfs_delayed_ref_head *head = NULL;
	int info_level = 0;
	int ret;
	struct list_head prefs_delayed;
	struct list_head prefs;
	struct __prelim_ref *ref;

	INIT_LIST_HEAD(&prefs);
	INIT_LIST_HEAD(&prefs_delayed);

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = (u64)-1;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * grab both a lock on the path and a lock on the delayed ref head.
	 * We need both to get a consistent picture of how the refs look
	 * at a specified point in time
	 */
again:
	ret = btrfs_search_slot(trans, fs_info->extent_root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	BUG_ON(ret == 0);

	/*
	 * look if there are updates for this ref queued and lock the head
	 */
	delayed_refs = &trans->transaction->delayed_refs;
	spin_lock(&delayed_refs->lock);
	head = btrfs_find_delayed_ref_head(trans, bytenr);
	if (head) {
		if (!mutex_trylock(&head->mutex)) {
			atomic_inc(&head->node.refs);
			spin_unlock(&delayed_refs->lock);

			btrfs_release_path(path);

			/*
			 * Mutex was contended, block until it's
			 * released and try again
			 */
			mutex_lock(&head->mutex);
			mutex_unlock(&head->mutex);
			btrfs_put_delayed_ref(&head->node);
			goto again;
		}
		ret = __add_delayed_refs(head, seq, &info_key, &prefs_delayed);
		if (ret)
			goto out;
	}
	spin_unlock(&delayed_refs->lock);

	if (path->slots[0]) {
		struct extent_buffer *leaf;
		int slot;

		leaf = path->nodes[0];
		slot = path->slots[0] - 1;
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.objectid == bytenr &&
		    key.type == BTRFS_EXTENT_ITEM_KEY) {
			ret = __add_inline_refs(fs_info, path, bytenr,
						&info_key, &info_level, &prefs);
			if (ret)
				goto out;
			ret = __add_keyed_refs(fs_info, path, bytenr, &info_key,
					       info_level, &prefs);
			if (ret)
				goto out;
		}
	}
	btrfs_release_path(path);

	/*
	 * when adding the delayed refs above, the info_key might not have
	 * been known yet. Go over the list and replace the missing keys
	 */
	list_for_each_entry(ref, &prefs_delayed, list) {
		if ((ref->key.offset | ref->key.type | ref->key.objectid) == 0)
			memcpy(&ref->key, &info_key, sizeof(ref->key));
	}
	list_splice_init(&prefs_delayed, &prefs);

	ret = __merge_refs(&prefs, 1);
	if (ret)
		goto out;

	ret = __resolve_indirect_refs(fs_info, &prefs);
	if (ret)
		goto out;

	ret = __merge_refs(&prefs, 2);
	if (ret)
		goto out;

	while (!list_empty(&prefs)) {
		ref = list_first_entry(&prefs, struct __prelim_ref, list);
		list_del(&ref->list);
		if (ref->count < 0)
			WARN_ON(1);
		if (ref->count && ref->root_id && ref->parent == 0) {
			/* no parent == root of tree */
			ret = ulist_add(roots, ref->root_id, 0);
			BUG_ON(ret < 0);
		}
		if (ref->count && ref->parent) {
			ret = ulist_add(refs, ref->parent, 0);
			BUG_ON(ret < 0);
		}
		kfree(ref);
	}

out:
	if (head)
		mutex_unlock(&head->mutex);
	btrfs_free_path(path);
	while (!list_empty(&prefs)) {
		ref = list_first_entry(&prefs, struct __prelim_ref, list);
		list_del(&ref->list);
		kfree(ref);
	}
	while (!list_empty(&prefs_delayed)) {
		ref = list_first_entry(&prefs_delayed, struct __prelim_ref,
				       list);
		list_del(&ref->list);
		kfree(ref);
	}

	return ret;
}

/*
 * walk all backrefs for a given extent to find all roots that reference this
 * extent. Walking a backref means finding all extents that reference this
 * extent and in turn walk the backrefs of those, too. Naturally this is a
 * recursive process, but here it is implemented in an iterative fashion. We
 * add the extent initially to a list, find all referencing extents and add
 * those to the list, too. This is repeated until the list is empty. All found
 * roots are added to the roots list which is returned from this function.
 */
static struct ulist *find_all_roots(struct btrfs_trans_handle *trans,
				    struct btrfs_fs_info *fs_info, u64 bytenr,
				    u64 num_bytes, u64 seq)
{
	struct ulist *roots;
	struct ulist *refs;
	struct ulist_node *node = NULL;
	int ret;

	roots = ulist_alloc(GFP_NOFS);
	if (!roots)
		return ERR_PTR(-ENOMEM);
	refs = ulist_alloc(GFP_NOFS);
	if (!refs) {
		ulist_free(roots);
		return ERR_PTR(-ENOMEM);
	}

	ulist_add(refs, bytenr, 0);

	while ((node = ulist_next(refs, node))) {
		ret = __find_all_roots(trans, fs_info, node->val, roots, refs,
				       seq);
		if (ret < 0 && ret != -ENOENT) {
			ulist_free(refs);
			ulist_free(roots);
			return ERR_PTR(ret);
		}
	}

	ulist_free(refs);

	return roots;
}

static void qgroup_dirty(struct btrfs_fs_info *fs_info,
			 struct btrfs_qgroup *qgroup)
{
	if (list_empty(&qgroup->dirty))
		list_add(&qgroup->dirty, &fs_info->dirty_qgroups);
}

/*
 * btrfs_qgroup_record_ref is called for every ref that is added to or deleted
 * from the fs. First, all roots referencing the extent are searched, and
 * then the space is accounted accordingly to the different roots. The
 * accounting algorithm works in 3 steps documented inline.
 */
int btrfs_qgroup_record_ref(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *fs_info,
			     struct btrfs_delayed_ref_node *node,
			     struct btrfs_delayed_extent_op *extent_op)
{
	struct btrfs_key ins;
	struct btrfs_root *quota_root;
	u64 ref_root;
	struct btrfs_qgroup *qgroup;
	struct ulist_node *unode;
	struct ulist *roots = NULL;
	struct ulist *tmp = NULL;
	u64 seq;
	int ret = 0;
	int sgn;

	if (!fs_info->quota_enabled)
		return 0;

	BUG_ON(!fs_info->quota_root);

	ins.objectid = node->bytenr;
	ins.offset = node->num_bytes;
	ins.type = BTRFS_EXTENT_ITEM_KEY;

	if (node->type == BTRFS_TREE_BLOCK_REF_KEY ||
	    node->type == BTRFS_SHARED_BLOCK_REF_KEY) {
		struct btrfs_delayed_tree_ref *ref;
		ref = btrfs_delayed_node_to_tree_ref(node);
		ref_root = ref->root;
	} else if (node->type == BTRFS_EXTENT_DATA_REF_KEY ||
		   node->type == BTRFS_SHARED_DATA_REF_KEY) {
		struct btrfs_delayed_data_ref *ref;
		ref = btrfs_delayed_node_to_data_ref(node);
		ref_root = ref->root;
	} else {
		BUG();
	}

	if (!is_fstree(ref_root)) {
		/*
		 * non-fs-trees are not being accounted
		 */
		return 0;
	}

	switch (node->action) {
	case BTRFS_ADD_DELAYED_REF:
	case BTRFS_ADD_DELAYED_EXTENT:
		sgn = 1;
		break;
	case BTRFS_DROP_DELAYED_REF:
		sgn = -1;
		break;
	case BTRFS_UPDATE_DELAYED_HEAD:
		return 0;
	default:
		BUG();
	}

	roots = find_all_roots(trans, fs_info, node->bytenr, node->num_bytes,
			       sgn > 0 ? node->seq  - 1 : node->seq);
	if (IS_ERR(roots)) {
		ret = PTR_ERR(roots);
		goto out;
	}

	spin_lock(&fs_info->qgroup_lock);
	quota_root = fs_info->quota_root;
	if (!quota_root)
		goto out;

	qgroup = find_qgroup_rb(fs_info, ref_root);
	if (!qgroup)
		goto out;

	/*
	 * step 1: for each old ref, visit all nodes once and inc refcnt
	 */
	unode = NULL;
	tmp = ulist_alloc(GFP_NOFS);
	if (!tmp) {
		ret = -ENOMEM;
		goto out;
	}
	seq = fs_info->qgroup_seq;
	fs_info->qgroup_seq += roots->nnodes + 1; /* max refcnt */

	while ((unode = ulist_next(roots, unode))) {
		struct ulist_node *tmp_unode;
		struct btrfs_qgroup *qg;

		qg = find_qgroup_rb(fs_info, unode->val);
		if (!qg)
			continue;

		ulist_reinit(tmp);
						/* XXX id not needed */
		ulist_add(tmp, qg->qgroupid, (unsigned long)qg);
		tmp_unode = NULL;
		while ((tmp_unode = ulist_next(tmp, tmp_unode))) {
			struct btrfs_qgroup_list *glist;

			qg = (struct btrfs_qgroup *)tmp_unode->aux;
			if (qg->refcnt < seq)
				qg->refcnt = seq + 1;
			else
				++qg->refcnt;

			list_for_each_entry(glist, &qg->groups, next_group) {
				ulist_add(tmp, glist->group->qgroupid,
					  (unsigned long)glist->group);
			}
		}
	}

	/*
	 * step 2: walk from the new root
	 */
	ulist_reinit(tmp);
	ulist_add(tmp, qgroup->qgroupid, (unsigned long)qgroup);
	unode = NULL;
	while ((unode = ulist_next(tmp, unode))) {
		struct btrfs_qgroup *qg;
		struct btrfs_qgroup_list *glist;

		qg = (struct btrfs_qgroup *)unode->aux;
		if (qg->refcnt < seq) {
			/* not visited by step 1 */
			qg->rfer += sgn * node->num_bytes;
			qg->rfer_cmpr += sgn * node->num_bytes;
			if (roots->nnodes == 0) {
				qg->excl += sgn * node->num_bytes;
				qg->excl_cmpr += sgn * node->num_bytes;
			}
			qgroup_dirty(fs_info, qg);
		}
		WARN_ON(qg->tag >= seq);
		qg->tag = seq;

		list_for_each_entry(glist, &qg->groups, next_group) {
			ulist_add(tmp, glist->group->qgroupid,
				  (unsigned long)glist->group);
		}
	}

	/*
	 * step 3: walk again from old refs
	 */
	while ((unode = ulist_next(roots, unode))) {
		struct btrfs_qgroup *qg;
		struct ulist_node *tmp_unode;

		qg = find_qgroup_rb(fs_info, unode->val);
		if (!qg)
			continue;

		ulist_reinit(tmp);
		ulist_add(tmp, qg->qgroupid, (unsigned long)qg);
		tmp_unode = NULL;
		while ((tmp_unode = ulist_next(tmp, tmp_unode))) {
			struct btrfs_qgroup_list *glist;

			qg = (struct btrfs_qgroup *)tmp_unode->aux;
			if (qg->tag == seq)
				continue;

			if (qg->refcnt - seq == roots->nnodes) {
				qg->excl -= sgn * node->num_bytes;
				qg->excl_cmpr -= sgn * node->num_bytes;
				qgroup_dirty(fs_info, qg);
			}

			list_for_each_entry(glist, &qg->groups, next_group) {
				ulist_add(tmp, glist->group->qgroupid,
					  (unsigned long)glist->group);
			}
		}
	}
	ret = 0;
out:
	spin_unlock(&fs_info->qgroup_lock);
	ulist_free(roots);
	ulist_free(tmp);

	return ret;
}

/*
 * called from commit_transaction. Writes all changed qgroups to disk.
 */
int btrfs_run_qgroups(struct btrfs_trans_handle *trans,
		      struct btrfs_fs_info *fs_info)
{
	struct btrfs_root *quota_root = fs_info->quota_root;
	int ret = 0;

	if (!quota_root)
		goto out;

	fs_info->quota_enabled = fs_info->pending_quota_state;

	spin_lock(&fs_info->qgroup_lock);
	while (!list_empty(&fs_info->dirty_qgroups)) {
		struct btrfs_qgroup *qgroup;
		qgroup = list_first_entry(&fs_info->dirty_qgroups,
					  struct btrfs_qgroup, dirty);
		list_del_init(&qgroup->dirty);
		spin_unlock(&fs_info->qgroup_lock);
		ret = update_qgroup_info_item(trans, quota_root, qgroup);
		if (ret)
			fs_info->qgroup_flags |=
					BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
		spin_lock(&fs_info->qgroup_lock);
	}
	if (fs_info->quota_enabled)
		fs_info->qgroup_flags |= BTRFS_QGROUP_STATUS_FLAG_ON;
	else
		fs_info->qgroup_flags &= ~BTRFS_QGROUP_STATUS_FLAG_ON;
	spin_unlock(&fs_info->qgroup_lock);

	ret = update_qgroup_status_item(trans, fs_info, quota_root);
	if (ret)
		fs_info->qgroup_flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;

out:

	return ret;
}

/*
 * copy the acounting information between qgroups. This is necessary when a
 * snapshot or a subvolume is created
 */
int btrfs_qgroup_inherit(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *fs_info, u64 srcid, u64 objectid,
			 struct btrfs_qgroup_inherit *inherit)
{
	int ret = 0;
	int i;
	u64 *i_qgroups;
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_qgroup *srcgroup;
	struct btrfs_qgroup *dstgroup;
	u32 level_size = 0;

	if (!fs_info->quota_enabled)
		return 0;

	if (!quota_root)
		ret = -EINVAL;

	/*
	 * create a tracking group for the subvol itself
	 */
	ret = add_qgroup_item(trans, quota_root, objectid);
	if (ret)
		goto out;

	if (inherit && inherit->flags & BTRFS_QGROUP_INHERIT_SET_LIMITS) {
		ret = update_qgroup_limit_item(trans, quota_root, objectid,
					       inherit->lim.flags,
					       inherit->lim.max_rfer,
					       inherit->lim.max_excl,
					       inherit->lim.rsv_rfer,
					       inherit->lim.rsv_excl);
		if (ret)
			goto out;
	}

	if (srcid) {
		struct btrfs_root *srcroot;
		struct btrfs_key srckey;
		int srcroot_level;

		srckey.objectid = srcid;
		srckey.type = BTRFS_ROOT_ITEM_KEY;
		srckey.offset = (u64)-1;
		srcroot = btrfs_read_fs_root_no_name(fs_info, &srckey);
		if (IS_ERR(srcroot)) {
			ret = PTR_ERR(srcroot);
			goto out;
		}

		rcu_read_lock();
		srcroot_level = btrfs_header_level(srcroot->node);
		level_size = btrfs_level_size(srcroot, srcroot_level);
		rcu_read_unlock();
	}

	/*
	 * add qgroup to all inherited groups
	 */
	if (inherit) {
		i_qgroups = (u64 *)(inherit + 1);
		for (i = 0; i < inherit->num_qgroups; ++i) {
			ret = add_qgroup_relation_item(trans, quota_root,
						       objectid, *i_qgroups);
			if (ret)
				goto out;
			ret = add_qgroup_relation_item(trans, quota_root,
						       *i_qgroups, objectid);
			if (ret)
				goto out;
			++i_qgroups;
		}
	}


	spin_lock(&fs_info->qgroup_lock);

	dstgroup = add_qgroup_rb(fs_info, objectid);
	if (!dstgroup)
		goto unlock;

	if (srcid) {
		srcgroup = find_qgroup_rb(fs_info, srcid);
		if (!srcgroup)
			goto unlock;
		dstgroup->rfer = srcgroup->rfer - level_size;
		dstgroup->rfer_cmpr = srcgroup->rfer_cmpr - level_size;
		srcgroup->excl = level_size;
		srcgroup->excl_cmpr = level_size;
		qgroup_dirty(fs_info, dstgroup);
		qgroup_dirty(fs_info, srcgroup);
	}

	if (!inherit)
		goto unlock;

	i_qgroups = (u64 *)(inherit + 1);
	for (i = 0; i < inherit->num_qgroups; ++i) {
		ret = add_relation_rb(quota_root->fs_info, objectid,
				      *i_qgroups);
		if (ret)
			goto unlock;
		++i_qgroups;
	}

	for (i = 0; i <  inherit->num_ref_copies; ++i) {
		struct btrfs_qgroup *src;
		struct btrfs_qgroup *dst;

		src = find_qgroup_rb(fs_info, i_qgroups[0]);
		dst = find_qgroup_rb(fs_info, i_qgroups[1]);

		if (!src || !dst) {
			ret = -EINVAL;
			goto unlock;
		}

		dst->rfer = src->rfer - level_size;
		dst->rfer_cmpr = src->rfer_cmpr - level_size;
		i_qgroups += 2;
	}
	for (i = 0; i <  inherit->num_excl_copies; ++i) {
		struct btrfs_qgroup *src;
		struct btrfs_qgroup *dst;

		src = find_qgroup_rb(fs_info, i_qgroups[0]);
		dst = find_qgroup_rb(fs_info, i_qgroups[1]);

		if (!src || !dst) {
			ret = -EINVAL;
			goto unlock;
		}

		dst->excl = src->excl + level_size;
		dst->excl_cmpr = src->excl_cmpr + level_size;
		i_qgroups += 2;
	}

unlock:
	spin_unlock(&fs_info->qgroup_lock);
out:
	return 0;
}

/*
 * reserve some space for a qgroup and all its parents. The reservation takes
 * place with start_transaction or dealloc_reserve, similar to ENOSPC
 * accounting. If not enough space is available, EDQUOT is returned.
 * We assume that the requested space is new for all qgroups.
 */
int btrfs_qgroup_reserve(struct btrfs_root *root, u64 num_bytes)
{
	struct btrfs_root *quota_root;
	struct btrfs_qgroup *qgroup;
	struct btrfs_fs_info *fs_info = root->fs_info;
	u64 ref_root = root->root_key.objectid;
	int ret = 0;
	struct ulist *ulist = NULL;
	struct ulist_node *unode;

	if (!is_fstree(ref_root))
		return 0;

	if (num_bytes == 0)
		return 0;

	spin_lock(&fs_info->qgroup_lock);
	quota_root = fs_info->quota_root;
	if (!quota_root)
		goto out;

	qgroup = find_qgroup_rb(fs_info, ref_root);
	if (!qgroup)
		goto out;

	/*
	 * in a first step, we check all affected qgroups if any limits would
	 * be exceeded
	 */
	ulist = ulist_alloc(GFP_NOFS);
	ulist_add(ulist, qgroup->qgroupid, (unsigned long)qgroup);
	unode = NULL;
	while ((unode = ulist_next(ulist, unode))) {
		struct btrfs_qgroup *qg;
		struct btrfs_qgroup_list *glist;

		qg = (struct btrfs_qgroup *)unode->aux;

		if ((qg->lim_flags & BTRFS_QGROUP_LIMIT_MAX_RFER) &&
		    qg->reserved + qg->rfer + num_bytes >
		    qg->max_rfer)
			ret = -EDQUOT;

		if ((qg->lim_flags & BTRFS_QGROUP_LIMIT_MAX_EXCL) &&
		    qg->reserved + qg->excl + num_bytes >
		    qg->max_excl)
			ret = -EDQUOT;

		list_for_each_entry(glist, &qg->groups, next_group) {
			ulist_add(ulist, glist->group->qgroupid,
				  (unsigned long)glist->group);
		}
	}
	if (ret)
		goto out;

	/*
	 * no limits exceeded, now record the reservation into all qgroups
	 */
	unode = NULL;
	while ((unode = ulist_next(ulist, unode))) {
		struct btrfs_qgroup *qg;

		qg = (struct btrfs_qgroup *)unode->aux;

		qg->reserved += num_bytes;
#if 0
		qgroup_dirty(fs_info, qg);/* XXX not necesarry */
#endif
	}

out:
	spin_unlock(&fs_info->qgroup_lock);
	ulist_free(ulist);

	return ret;
}

void btrfs_qgroup_free(struct btrfs_root *root, u64 num_bytes)
{
	struct btrfs_root *quota_root;
	struct btrfs_qgroup *qgroup;
	struct btrfs_fs_info *fs_info = root->fs_info;
	struct ulist *ulist = NULL;
	struct ulist_node *unode;
	u64 ref_root = root->root_key.objectid;

	if (!is_fstree(ref_root))
		return;

	if (num_bytes == 0)
		return;

	spin_lock(&fs_info->qgroup_lock);

	quota_root = fs_info->quota_root;
	if (!quota_root)
		goto out;

	qgroup = find_qgroup_rb(fs_info, ref_root);
	if (!qgroup)
		goto out;

	ulist = ulist_alloc(GFP_NOFS);
	ulist_add(ulist, qgroup->qgroupid, (unsigned long)qgroup);
	unode = NULL;
	while ((unode = ulist_next(ulist, unode))) {
		struct btrfs_qgroup *qg;
		struct btrfs_qgroup_list *glist;

		qg = (struct btrfs_qgroup *)unode->aux;

		qg->reserved -= num_bytes;
#if 0
qgroup_dirty(fs_info, qg);
#endif

		list_for_each_entry(glist, &qg->groups, next_group) {
			ulist_add(ulist, glist->group->qgroupid,
				  (unsigned long)glist->group);
		}
	}

out:
	spin_unlock(&fs_info->qgroup_lock);
	ulist_free(ulist);
}
