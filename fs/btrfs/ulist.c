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
#include <linux/rbtree.h>
#include <linux/slab.h>

#include "ulist.h"

void ulist_init(struct ulist *ulist, unsigned long gfp_mask)
{
	ulist->nnodes = 0;
	ulist->gfp_mask = gfp_mask;
	ulist->nodes = ulist->int_nodes;
	ulist->nodes_alloced = ULIST_SIZE;
}

void ulist_fini(struct ulist *ulist)
{
	if (ulist->nodes_alloced > ULIST_SIZE)
		kfree(ulist->nodes);
}

void ulist_reinit(struct ulist *ulist)
{
	ulist_fini(ulist);
	ulist_init(ulist, ulist->gfp_mask);
}

struct ulist *ulist_alloc(unsigned long gfp_mask)
{
	struct ulist *ulist = kmalloc(sizeof(*ulist), gfp_mask);

	if (!ulist)
		return NULL;

	ulist_init(ulist, gfp_mask);

	return ulist;
}

void ulist_free(struct ulist *ulist)
{
	if (!ulist)
		return;
	ulist_fini(ulist);
	kfree(ulist);
}

int ulist_add(struct ulist *ulist, u64 val, unsigned long aux)
{
	u64 i;

	for (i = 0; i < ulist->nnodes; ++i) {
		if (ulist->nodes[i].val == val)
			return 0;
	}

	if (ulist->nnodes > ulist->nodes_alloced) {
		u64 new_alloced = ulist->nodes_alloced + 128;
		struct ulist_node *new_nodes = kmalloc(sizeof(*new_nodes) *
					       new_alloced, ulist->gfp_mask);

		if (!new_nodes)
			return -ENOMEM;
		memcpy(new_nodes, ulist->nodes,
		       sizeof(*new_nodes) * ulist->nnodes);
		if (ulist->nodes_alloced > ULIST_SIZE)
			kfree(ulist->nodes);
		ulist->nodes = new_nodes;
		ulist->nodes_alloced = new_alloced;
	}
	ulist->nodes[ulist->nnodes].val = val;
	ulist->nodes[ulist->nnodes].aux = aux;
	ulist->nodes[ulist->nnodes].next = ulist->nnodes + 1;
	++ulist->nnodes;

	return 1;
}

struct ulist_node *ulist_next(struct ulist *ulist, struct ulist_node *prev)
{
	if (ulist->nnodes == 0)
		return NULL;

	if (!prev)
		return &ulist->nodes[0];

	if (prev->next < 0 || prev->next >= ulist->nnodes)
		return NULL;

	return &ulist->nodes[prev->next];
}

int ulist_merge(struct ulist *dst, struct ulist *src)
{
	struct ulist_node *node = NULL;
	int ret;

	while ((node = ulist_next(src, node))) {
		ret = ulist_add(dst, node->val, node->aux);
		if (ret)
			return ret;
	}

	return 0;
}
