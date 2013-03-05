/*
 * ntfs-capture.c
 *
 * Capture a WIM image from a NTFS volume.  We capture everything we can,
 * including security data and alternate data streams.
 */

/*
 * Copyright (C) 2012, 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wimlib; if not, see http://www.gnu.org/licenses/.
 */


#include "config.h"

#include <ntfs-3g/endians.h>
#include <ntfs-3g/types.h>

#include "wimlib_internal.h"


#include "dentry.h"
#include "lookup_table.h"
#include "buffer_io.h"
#include <ntfs-3g/layout.h>
#include <ntfs-3g/acls.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/misc.h>
#include <ntfs-3g/reparse.h>
#include <ntfs-3g/security.h> /* security.h before xattrs.h */
#include <ntfs-3g/xattrs.h>
#include <ntfs-3g/volume.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include "rbtree.h"

/* Red-black tree that maps SHA1 message digests of security descriptors to
 * security IDs, which are themselves indices into the table of security
 * descriptors in the 'struct wim_security_data'. */
struct sd_set {
	struct wim_security_data *sd;
	struct rb_root rb_root;
};

struct sd_node {
	int security_id;
	u8 hash[SHA1_HASH_SIZE];
	struct rb_node rb_node;
};

static void free_sd_tree(struct rb_node *node)
{
	if (node) {
		free_sd_tree(node->rb_left);
		free_sd_tree(node->rb_right);
		FREE(container_of(node, struct sd_node, rb_node));
	}
}
/* Frees a security descriptor index set. */
static void destroy_sd_set(struct sd_set *sd_set)
{
	free_sd_tree(sd_set->rb_root.rb_node);
}

/* Inserts a a new node into the security descriptor index tree. */
static void insert_sd_node(struct sd_set *set, struct sd_node *new)
{
	struct rb_root *root = &set->rb_root;
	struct rb_node **p = &(root->rb_node);
	struct rb_node *rb_parent = NULL;

	while (*p) {
		struct sd_node *this = container_of(*p, struct sd_node, rb_node);
		int cmp = hashes_cmp(new->hash, this->hash);

		rb_parent = *p;
		if (cmp < 0)
			p = &((*p)->rb_left);
		else if (cmp > 0)
			p = &((*p)->rb_right);
		else
			wimlib_assert(0); /* Duplicate SHA1 message digest */
	}
	rb_link_node(&new->rb_node, rb_parent, p);
	rb_insert_color(&new->rb_node, root);
}

/* Returns the index of the security descriptor having a SHA1 message digest of
 * @hash.  If not found, return -1. */
static int lookup_sd(struct sd_set *set, const u8 hash[SHA1_HASH_SIZE])
{
	struct rb_node *node = set->rb_root.rb_node;

	while (node) {
		struct sd_node *sd_node = container_of(node, struct sd_node, rb_node);
		int cmp = hashes_cmp(hash, sd_node->hash);
		if (cmp < 0)
			node = node->rb_left;
		else if (cmp > 0)
			node = node->rb_right;
		else
			return sd_node->security_id;
	}
	return -1;
}

/*
 * Adds a security descriptor to the indexed security descriptor set as well as
 * the corresponding `struct wim_security_data', and returns the new security
 * ID; or, if there is an existing security descriptor that is the same, return
 * the security ID for it.  If a new security descriptor cannot be allocated,
 * return -1.
 */
static int sd_set_add_sd(struct sd_set *sd_set, const char descriptor[],
		         size_t size)
{
	u8 hash[SHA1_HASH_SIZE];
	int security_id;
	struct sd_node *new;
	u8 **descriptors;
	u64 *sizes;
	u8 *descr_copy;
	struct wim_security_data *sd;

	sha1_buffer((const u8*)descriptor, size, hash);

	security_id = lookup_sd(sd_set, hash);
	if (security_id >= 0) /* Identical descriptor already exists */
		return security_id;

	/* Need to add a new security descriptor */
	new = MALLOC(sizeof(*new));
	if (!new)
		goto out;
	descr_copy = MALLOC(size);
	if (!descr_copy)
		goto out_free_node;

	sd = sd_set->sd;

	memcpy(descr_copy, descriptor, size);
	new->security_id = sd->num_entries;
	copy_hash(new->hash, hash);

	descriptors = REALLOC(sd->descriptors,
			      (sd->num_entries + 1) * sizeof(sd->descriptors[0]));
	if (!descriptors)
		goto out_free_descr;
	sd->descriptors = descriptors;
	sizes = REALLOC(sd->sizes,
			(sd->num_entries + 1) * sizeof(sd->sizes[0]));
	if (!sizes)
		goto out_free_descr;
	sd->sizes = sizes;
	sd->descriptors[sd->num_entries] = descr_copy;
	sd->sizes[sd->num_entries] = size;
	sd->num_entries++;
	DEBUG("There are now %d security descriptors", sd->num_entries);
	sd->total_length += size + sizeof(sd->sizes[0]);
	insert_sd_node(sd_set, new);
	return new->security_id;
out_free_descr:
	FREE(descr_copy);
out_free_node:
	FREE(new);
out:
	return -1;
}

static inline ntfschar *attr_record_name(ATTR_RECORD *ar)
{
	return (ntfschar*)((u8*)ar + le16_to_cpu(ar->name_offset));
}

/* Calculates the SHA1 message digest of a NTFS attribute.
 *
 * @ni:  The NTFS inode containing the attribute.
 * @ar:	 The ATTR_RECORD describing the attribute.
 * @md:  If successful, the returned SHA1 message digest.
 * @reparse_tag_ret:	Optional pointer into which the first 4 bytes of the
 * 				attribute will be written (to get the reparse
 * 				point ID)
 *
 * Return 0 on success or nonzero on error.
 */
static int ntfs_attr_sha1sum(ntfs_inode *ni, ATTR_RECORD *ar,
			     u8 md[SHA1_HASH_SIZE],
			     bool is_reparse_point,
			     u32 *reparse_tag_ret)
{
	s64 pos = 0;
	s64 bytes_remaining;
	char buf[BUFFER_SIZE];
	ntfs_attr *na;
	SHA_CTX ctx;

	na = ntfs_attr_open(ni, ar->type, attr_record_name(ar),
			    ar->name_length);
	if (!na) {
		ERROR_WITH_ERRNO("Failed to open NTFS attribute");
		return WIMLIB_ERR_NTFS_3G;
	}

	bytes_remaining = na->data_size;

	if (is_reparse_point) {
		if (ntfs_attr_pread(na, 0, 8, buf) != 8)
			goto out_error;
		*reparse_tag_ret = le32_to_cpu(*(u32*)buf);
		DEBUG("ReparseTag = %#x", *reparse_tag_ret);
		pos = 8;
		bytes_remaining -= 8;
	}

	sha1_init(&ctx);
	while (bytes_remaining) {
		s64 to_read = min(bytes_remaining, sizeof(buf));
		if (ntfs_attr_pread(na, pos, to_read, buf) != to_read)
			goto out_error;
		sha1_update(&ctx, buf, to_read);
		pos += to_read;
		bytes_remaining -= to_read;
	}
	sha1_final(md, &ctx);
	ntfs_attr_close(na);
	return 0;
out_error:
	ERROR_WITH_ERRNO("Error reading NTFS attribute");
	return WIMLIB_ERR_NTFS_3G;
}

/* Load the streams from a file or reparse point in the NTFS volume into the WIM
 * lookup table */
static int capture_ntfs_streams(struct wim_dentry *dentry, ntfs_inode *ni,
				char path[], size_t path_len,
				struct wim_lookup_table *lookup_table,
				ntfs_volume **ntfs_vol_p,
				ATTR_TYPES type)
{
	ntfs_attr_search_ctx *actx;
	u8 attr_hash[SHA1_HASH_SIZE];
	struct ntfs_location *ntfs_loc = NULL;
	int ret = 0;
	struct wim_lookup_table_entry *lte;

	DEBUG2("Capturing NTFS data streams from `%s'", path);

	/* Get context to search the streams of the NTFS file. */
	actx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!actx) {
		ERROR_WITH_ERRNO("Cannot get NTFS attribute search "
				 "context");
		return WIMLIB_ERR_NTFS_3G;
	}

	/* Capture each data stream or reparse data stream. */
	while (!ntfs_attr_lookup(type, NULL, 0,
				 CASE_SENSITIVE, 0, NULL, 0, actx))
	{
		char *stream_name_utf8;
		u32 reparse_tag;
		u64 data_size = ntfs_get_attribute_value_length(actx->attr);
		u64 name_length = actx->attr->name_length;

		if (data_size == 0) {
			if (errno != 0) {
				ERROR_WITH_ERRNO("Failed to get size of attribute of "
						 "`%s'", path);
				ret = WIMLIB_ERR_NTFS_3G;
				goto out_put_actx;
			}
			/* Empty stream.  No lookup table entry is needed. */
			lte = NULL;
		} else {
			if (type == AT_REPARSE_POINT && data_size < 8) {
				ERROR("`%s': reparse point buffer too small",
				      path);
				ret = WIMLIB_ERR_NTFS_3G;
				goto out_put_actx;
			}
			/* Checksum the stream. */
			ret = ntfs_attr_sha1sum(ni, actx->attr, attr_hash,
						type == AT_REPARSE_POINT, &reparse_tag);
			if (ret != 0)
				goto out_put_actx;

			if (type == AT_REPARSE_POINT)
				dentry->d_inode->i_reparse_tag = reparse_tag;

			/* Make a lookup table entry for the stream, or use an existing
			 * one if there's already an identical stream. */
			lte = __lookup_resource(lookup_table, attr_hash);
			ret = WIMLIB_ERR_NOMEM;
			if (lte) {
				lte->refcnt++;
			} else {
				ntfs_loc = CALLOC(1, sizeof(*ntfs_loc));
				if (!ntfs_loc)
					goto out_put_actx;
				ntfs_loc->ntfs_vol_p = ntfs_vol_p;
				ntfs_loc->path_utf8 = MALLOC(path_len + 1);
				if (!ntfs_loc->path_utf8)
					goto out_free_ntfs_loc;
				memcpy(ntfs_loc->path_utf8, path, path_len + 1);
				if (name_length) {
					ntfs_loc->stream_name_utf16 = MALLOC(name_length * 2);
					if (!ntfs_loc->stream_name_utf16)
						goto out_free_ntfs_loc;
					memcpy(ntfs_loc->stream_name_utf16,
					       attr_record_name(actx->attr),
					       actx->attr->name_length * 2);
					ntfs_loc->stream_name_utf16_num_chars = name_length;
				}

				lte = new_lookup_table_entry();
				if (!lte)
					goto out_free_ntfs_loc;
				lte->ntfs_loc = ntfs_loc;
				lte->resource_location = RESOURCE_IN_NTFS_VOLUME;
				if (type == AT_REPARSE_POINT) {
					ntfs_loc->is_reparse_point = true;
					lte->resource_entry.original_size = data_size - 8;
					lte->resource_entry.size = data_size - 8;
				} else {
					ntfs_loc->is_reparse_point = false;
					lte->resource_entry.original_size = data_size;
					lte->resource_entry.size = data_size;
				}
				ntfs_loc = NULL;
				DEBUG("Add resource for `%s' (size = %"PRIu64")",
				      dentry->file_name_utf8,
				      lte->resource_entry.original_size);
				copy_hash(lte->hash, attr_hash);
				lookup_table_insert(lookup_table, lte);
			}
		}
		if (name_length == 0) {
			/* Unnamed data stream.  Put the reference to it in the
			 * dentry's inode. */
		#if 0
			if (dentry->d_inode->i_lte) {
				ERROR("Found two un-named data streams for "
				      "`%s'", path);
				ret = WIMLIB_ERR_NTFS_3G;
				goto out_free_lte;
			}
			dentry->d_inode->i_lte = lte;
		#else
			if (dentry->d_inode->i_lte) {
				WARNING("Found two un-named data streams for "
					"`%s'", path);
				free_lookup_table_entry(lte);
			} else {
				dentry->d_inode->i_lte = lte;
			}
		#endif
		} else {
			/* Named data stream.  Put the reference to it in the
			 * alternate data stream entries */
			struct wim_ads_entry *new_ads_entry;
			size_t stream_name_utf8_len;

			ret = utf16_to_utf8((const char*)attr_record_name(actx->attr),
					    name_length * 2,
					    &stream_name_utf8,
					    &stream_name_utf8_len);
			if (ret != 0)
				goto out_free_lte;
			new_ads_entry = inode_add_ads(dentry->d_inode, stream_name_utf8);
			FREE(stream_name_utf8);
			if (!new_ads_entry)
				goto out_free_lte;

			wimlib_assert(new_ads_entry->stream_name_len == name_length * 2);

			new_ads_entry->lte = lte;
		}
	}
	ret = 0;
	goto out_put_actx;
out_free_lte:
	free_lookup_table_entry(lte);
out_free_ntfs_loc:
	if (ntfs_loc) {
		FREE(ntfs_loc->path_utf8);
		FREE(ntfs_loc->stream_name_utf16);
		FREE(ntfs_loc);
	}
out_put_actx:
	ntfs_attr_put_search_ctx(actx);
	if (ret == 0)
		DEBUG2("Successfully captured NTFS streams from `%s'", path);
	else
		ERROR("Failed to capture NTFS streams from `%s", path);
	return ret;
}

/* Red-black tree that maps NTFS inode numbers to DOS names */
struct dos_name_map {
	struct rb_root rb_root;
};

struct dos_name_node {
	struct rb_node rb_node;
	char dos_name[24];
	int name_len_bytes;
	u64 ntfs_ino;
};

/* Inserts a new DOS name into the map */
static int insert_dos_name(struct dos_name_map *map,
			   const ntfschar *dos_name, int name_len,
			   u64 ntfs_ino)
{
	struct dos_name_node *new_node;
	struct rb_node **p;
	struct rb_root *root;
	struct rb_node *rb_parent;

	DEBUG("DOS name_len = %d", name_len);
	new_node = MALLOC(sizeof(struct dos_name_node));
	if (!new_node)
		return -1;

	/* DOS names are supposed to be 12 characters max (that's 24 bytes,
	 * assuming 2-byte ntfs characters) */
	wimlib_assert(name_len * sizeof(ntfschar) <= sizeof(new_node->dos_name));

	/* Initialize the DOS name, DOS name length, and NTFS inode number of
	 * the red-black tree node */
	memcpy(new_node->dos_name, dos_name, name_len * sizeof(ntfschar));
	new_node->name_len_bytes = name_len * sizeof(ntfschar);
	new_node->ntfs_ino = ntfs_ino;

	/* Insert the red-black tree node */
	root = &map->rb_root;
	p = &root->rb_node;
	rb_parent = NULL;
	while (*p) {
		struct dos_name_node *this;

		this = container_of(*p, struct dos_name_node, rb_node);
		rb_parent = *p;
		if (new_node->ntfs_ino < this->ntfs_ino)
			p = &((*p)->rb_left);
		else if (new_node->ntfs_ino > this->ntfs_ino)
			p = &((*p)->rb_right);
		else {
			/* This should be impossible since a NTFS inode cannot
			 * have multiple DOS names, and we only should get each
			 * DOS name entry once from the ntfs_readdir() calls. */
			ERROR("NTFS inode %"PRIu64" has multiple DOS names",
			      ntfs_ino);
			return -1;
		}
	}
	rb_link_node(&new_node->rb_node, rb_parent, p);
	rb_insert_color(&new_node->rb_node, root);
	DEBUG("Inserted DOS name for inode %"PRIu64, ntfs_ino);
	return 0;
}

/* Returns a structure that contains the DOS name and its length for a NTFS
 * inode, or NULL if the inode has no DOS name. */
static struct dos_name_node *
lookup_dos_name(const struct dos_name_map *map, u64 ntfs_ino)
{
	struct rb_node *node = map->rb_root.rb_node;
	while (node) {
		struct dos_name_node *this;
		this = container_of(node, struct dos_name_node, rb_node);
		if (ntfs_ino < this->ntfs_ino)
			node = node->rb_left;
		else if (ntfs_ino > this->ntfs_ino)
			node = node->rb_right;
		else
			return this;
	}
	return NULL;
}

static int set_dentry_dos_name(struct wim_dentry *dentry, void *arg)
{
	const struct dos_name_map *map = arg;
	const struct dos_name_node *node;

	if (dentry->is_win32_name) {
		node = lookup_dos_name(map, dentry->d_inode->i_ino);
		if (node) {
			dentry->short_name = MALLOC(node->name_len_bytes);
			if (!dentry->short_name)
				return WIMLIB_ERR_NOMEM;
			memcpy(dentry->short_name, node->dos_name,
			       node->name_len_bytes);
			dentry->short_name_len = node->name_len_bytes;
			DEBUG("Assigned DOS name to ino %"PRIu64,
			      dentry->d_inode->i_ino);
		} else {
			WARNING("NTFS inode %"PRIu64" has Win32 name with no "
				"corresponding DOS name",
				dentry->d_inode->i_ino);
		}
	}
	return 0;
}

static void free_dos_name_tree(struct rb_node *node) {
	if (node) {
		free_dos_name_tree(node->rb_left);
		free_dos_name_tree(node->rb_right);
		FREE(container_of(node, struct dos_name_node, rb_node));
	}
}

static void destroy_dos_name_map(struct dos_name_map *map)
{
	free_dos_name_tree(map->rb_root.rb_node);
}

struct readdir_ctx {
	struct wim_dentry *parent;
	ntfs_inode *dir_ni;
	char *path;
	size_t path_len;
	struct wim_lookup_table	*lookup_table;
	struct sd_set *sd_set;
	struct dos_name_map *dos_name_map;
	const struct capture_config *config;
	ntfs_volume **ntfs_vol_p;
	int add_image_flags;
	wimlib_progress_func_t progress_func;
};

static int
build_dentry_tree_ntfs_recursive(struct wim_dentry **root_p, ntfs_inode *dir_ni,
				 ntfs_inode *ni, char path[], size_t path_len,
				 int name_type,
				 struct wim_lookup_table *lookup_table,
				 struct sd_set *sd_set,
				 const struct capture_config *config,
				 ntfs_volume **ntfs_vol_p,
				 int add_image_flags,
				 wimlib_progress_func_t progress_func);

static int wim_ntfs_capture_filldir(void *dirent, const ntfschar *name,
				    const int name_len, const int name_type,
				    const s64 pos, const MFT_REF mref,
				    const unsigned dt_type)
{
	struct readdir_ctx *ctx;
	size_t utf8_name_len;
	char *utf8_name;
	struct wim_dentry *child;
	int ret;
	size_t path_len;

	ctx = dirent;
	if (name_type & FILE_NAME_DOS) {
		/* If this is the entry for a DOS name, store it for later. */
		ret = insert_dos_name(ctx->dos_name_map, name,
				      name_len, mref & MFT_REF_MASK_CPU);

		/* Return now if an error occurred or if this is just a DOS name
		 * and not a Win32+DOS name. */
		if (ret != 0 || name_type == FILE_NAME_DOS)
			return ret;
	}
	ret = utf16_to_utf8((const char*)name, name_len * 2,
			    &utf8_name, &utf8_name_len);
	if (ret != 0)
		return -1;

	if (utf8_name[0] == '.' &&
	     (utf8_name[1] == '\0' ||
	      (utf8_name[1] == '.' && utf8_name[2] == '\0'))) {
		/* . or .. entries
		 *
		 * note: name_type is POSIX for these, so DOS names will not
		 * have been inserted for them.  */
		ret = 0;
		goto out_free_utf8_name;
	}

	/* Open the inode for this directory entry and recursively capture the
	 * directory tree rooted at it */
	ntfs_inode *ni = ntfs_inode_open(ctx->dir_ni->vol, mref);
	if (!ni) {
		ERROR_WITH_ERRNO("Failed to open NTFS inode");
		goto out_free_utf8_name;
	}
	path_len = ctx->path_len;
	if (path_len != 1)
		ctx->path[path_len++] = '/';
	memcpy(ctx->path + path_len, utf8_name, utf8_name_len + 1);
	path_len += utf8_name_len;
	child = NULL;
	ret = build_dentry_tree_ntfs_recursive(&child, ctx->dir_ni,
					       ni, ctx->path, path_len, name_type,
					       ctx->lookup_table, ctx->sd_set,
					       ctx->config, ctx->ntfs_vol_p,
					       ctx->add_image_flags,
					       ctx->progress_func);
	if (child)
		dentry_add_child(ctx->parent, child);
	ntfs_inode_close(ni);
out_free_utf8_name:
	FREE(utf8_name);
	return ret;
}

/* Recursively build a WIM dentry tree corresponding to a NTFS volume.
 * At the same time, update the WIM lookup table with lookup table entries for
 * the NTFS streams, and build an array of security descriptors.
 */
static int build_dentry_tree_ntfs_recursive(struct wim_dentry **root_p,
					    ntfs_inode *dir_ni,
					    ntfs_inode *ni,
				    	    char path[],
					    size_t path_len,
					    int name_type,
				    	    struct wim_lookup_table *lookup_table,
				    	    struct sd_set *sd_set,
				    	    const struct capture_config *config,
				    	    ntfs_volume **ntfs_vol_p,
					    int add_image_flags,
					    wimlib_progress_func_t progress_func)
{
	u32 attributes;
	int ret;
	struct wim_dentry *root;

	if (exclude_path(path, config, false)) {
		/* Exclude a file or directory tree based on the capture
		 * configuration file */
		if ((add_image_flags & WIMLIB_ADD_IMAGE_FLAG_VERBOSE)
		    && progress_func)
		{
			union wimlib_progress_info info;
			info.scan.cur_path = path;
			info.scan.excluded = true;
			progress_func(WIMLIB_PROGRESS_MSG_SCAN_DENTRY, &info);
		}
		*root_p = NULL;
		return 0;
	}

	/* Get file attributes */
	struct SECURITY_CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.vol = ni->vol;
	ret = ntfs_xattr_system_getxattr(&ctx, XATTR_NTFS_ATTRIB,
					 ni, dir_ni, (char *)&attributes,
					 sizeof(u32));
	if (ret != 4) {
		ERROR_WITH_ERRNO("Failed to get NTFS attributes from `%s'",
				 path);
		return WIMLIB_ERR_NTFS_3G;
	}

	if ((add_image_flags & WIMLIB_ADD_IMAGE_FLAG_VERBOSE)
	    && progress_func)
	{
		union wimlib_progress_info info;
		info.scan.cur_path = path;
		info.scan.excluded = false;
		progress_func(WIMLIB_PROGRESS_MSG_SCAN_DENTRY, &info);
	}

	/* Create the new WIM dentry */
	root = new_dentry_with_timeless_inode(path_basename(path));
	if (!root) {
		if (errno == EILSEQ)
			return WIMLIB_ERR_INVALID_UTF8_STRING;
		else if (errno == ENOMEM)
			return WIMLIB_ERR_NOMEM;
		else
			return WIMLIB_ERR_ICONV_NOT_AVAILABLE;
	}
	*root_p = root;

	if (name_type & FILE_NAME_WIN32) /* Win32 or Win32+DOS name */
		root->is_win32_name = 1;
	root->d_inode->i_creation_time    = le64_to_cpu(ni->creation_time);
	root->d_inode->i_last_write_time  = le64_to_cpu(ni->last_data_change_time);
	root->d_inode->i_last_access_time = le64_to_cpu(ni->last_access_time);
	root->d_inode->i_attributes       = le32_to_cpu(attributes);
	root->d_inode->i_ino              = ni->mft_no;
	root->d_inode->i_resolved         = 1;

	if (attributes & FILE_ATTR_REPARSE_POINT) {
		/* Junction point, symbolic link, or other reparse point */
		ret = capture_ntfs_streams(root, ni, path, path_len,
					   lookup_table, ntfs_vol_p,
					   AT_REPARSE_POINT);
	} else if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {

		/* Normal directory */
		s64 pos = 0;
		struct dos_name_map dos_name_map = { .rb_root = {.rb_node = NULL} };
		struct readdir_ctx ctx = {
			.parent          = root,
			.dir_ni          = ni,
			.path            = path,
			.path_len        = path_len,
			.lookup_table    = lookup_table,
			.sd_set          = sd_set,
			.dos_name_map    = &dos_name_map,
			.config          = config,
			.ntfs_vol_p      = ntfs_vol_p,
			.add_image_flags = add_image_flags,
			.progress_func   = progress_func,
		};
		ret = ntfs_readdir(ni, &pos, &ctx, wim_ntfs_capture_filldir);
		if (ret) {
			ERROR_WITH_ERRNO("ntfs_readdir()");
			ret = WIMLIB_ERR_NTFS_3G;
		} else {
			ret = for_dentry_child(root, set_dentry_dos_name,
					       &dos_name_map);
		}
		destroy_dos_name_map(&dos_name_map);
	} else {
		/* Normal file */
		ret = capture_ntfs_streams(root, ni, path, path_len,
					   lookup_table, ntfs_vol_p,
					   AT_DATA);
	}
	if (ret != 0)
		return ret;

	/* Get security descriptor */
	char _sd[1];
	char *sd = _sd;
	errno = 0;
	ret = ntfs_xattr_system_getxattr(&ctx, XATTR_NTFS_ACL,
					 ni, dir_ni, sd,
					 sizeof(sd));
	if (ret > sizeof(sd)) {
		sd = alloca(ret);
		ret = ntfs_xattr_system_getxattr(&ctx, XATTR_NTFS_ACL,
						 ni, dir_ni, sd, ret);
	}
	if (ret > 0) {
		root->d_inode->i_security_id = sd_set_add_sd(sd_set, sd, ret);
		if (root->d_inode->i_security_id == -1) {
			ERROR("Out of memory");
			return WIMLIB_ERR_NOMEM;
		}
		DEBUG("Added security ID = %u for `%s'",
		      root->d_inode->i_security_id, path);
		ret = 0;
	} else if (ret < 0) {
		ERROR_WITH_ERRNO("Failed to get security information from "
				 "`%s'", path);
		ret = WIMLIB_ERR_NTFS_3G;
	} else {
		root->d_inode->i_security_id = -1;
		DEBUG("No security ID for `%s'", path);
	}
	return ret;
}

int build_dentry_tree_ntfs(struct wim_dentry **root_p,
			   const char *device,
			   struct wim_lookup_table *lookup_table,
			   struct wim_security_data *sd,
			   const struct capture_config *config,
			   int add_image_flags,
			   wimlib_progress_func_t progress_func,
			   void *extra_arg)
{
	ntfs_volume *vol;
	ntfs_inode *root_ni;
	int ret;
	struct sd_set sd_set = {
		.sd = sd,
		.rb_root = {NULL},
	};
	ntfs_volume **ntfs_vol_p = extra_arg;

	DEBUG("Mounting NTFS volume `%s' read-only", device);

#ifdef HAVE_NTFS_MNT_RDONLY
	/* NTFS-3g 2013 */
	vol = ntfs_mount(device, NTFS_MNT_RDONLY);
#else
	/* NTFS-3g 2011, 2012 */
	vol = ntfs_mount(device, MS_RDONLY);
#endif
	if (!vol) {
		ERROR_WITH_ERRNO("Failed to mount NTFS volume `%s' read-only",
				 device);
		return WIMLIB_ERR_NTFS_3G;
	}
	ntfs_open_secure(vol);

	/* We don't want to capture the special NTFS files such as $Bitmap.  Not
	 * to be confused with "hidden" or "system" files which are real files
	 * that we do need to capture.  */
	NVolClearShowSysFiles(vol);

	DEBUG("Opening root NTFS dentry");
	root_ni = ntfs_inode_open(vol, FILE_root);
	if (!root_ni) {
		ERROR_WITH_ERRNO("Failed to open root inode of NTFS volume "
				 "`%s'", device);
		ret = WIMLIB_ERR_NTFS_3G;
		goto out;
	}

	/* Currently we assume that all the UTF-8 paths fit into this length and
	 * there is no check for overflow. */
	char *path = MALLOC(32768);
	if (!path) {
		ERROR("Could not allocate memory for NTFS pathname");
		ret = WIMLIB_ERR_NOMEM;
		goto out_cleanup;
	}

	path[0] = '/';
	path[1] = '\0';
	ret = build_dentry_tree_ntfs_recursive(root_p, NULL, root_ni, path, 1,
					       FILE_NAME_POSIX, lookup_table,
					       &sd_set,
					       config, ntfs_vol_p,
					       add_image_flags,
					       progress_func);
out_cleanup:
	FREE(path);
	ntfs_inode_close(root_ni);
	destroy_sd_set(&sd_set);
out:
	ntfs_index_ctx_put(vol->secure_xsii);
	ntfs_index_ctx_put(vol->secure_xsdh);
	ntfs_inode_close(vol->secure_ni);

	if (ret) {
		if (ntfs_umount(vol, FALSE) != 0) {
			ERROR_WITH_ERRNO("Failed to unmount NTFS volume `%s'",
					 device);
			if (ret == 0)
				ret = WIMLIB_ERR_NTFS_3G;
		}
	} else {
		/* We need to leave the NTFS volume mounted so that we can read
		 * the NTFS files again when we are actually writing the WIM */
		*ntfs_vol_p = vol;
	}
	return ret;
}
