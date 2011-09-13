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

#ifndef __BTRFS_ULIST__
#define __BTRFS_ULIST__

#include "ulist.h"

#define ULIST_SIZE 16

/*
 * ulist is a generic data structures to hold a collection of unique u64
 * values. The only operations it supports is adding to the list and
 * enumerating it.
 * It is possible to store an auxiliary value along with the key.
 * The implementation is preliminary and can probably be sped up significantly.
 */
struct ulist_node {
	u64 val;
	unsigned long aux;
	unsigned long next;
};

struct ulist {
	unsigned long nnodes;
	unsigned long gfp_mask;
	struct ulist_node *nodes;
	unsigned long nodes_alloced;
	struct ulist_node int_nodes[ULIST_SIZE];
};

void ulist_init(struct ulist *ulist, unsigned long gfp_mask);
void ulist_fini(struct ulist *ulist);
void ulist_reinit(struct ulist *ulist);
struct ulist *ulist_alloc(unsigned long gfp_mask);
void ulist_free(struct ulist *ulist);

/* returns < 0 on error, 0 on duplicate, 1 on insert */
int ulist_add(struct ulist *ulist, u64 val, unsigned long aux);

struct ulist_node *ulist_next(struct ulist *ulist, struct ulist_node *prev);
int ulist_merge(struct ulist *dst, struct ulist *src);

#endif
