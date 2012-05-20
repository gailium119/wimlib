/*
 * modify.c
 *
 * Support for modifying WIM files with image-level operations (delete an image,
 * add an image, export an imagex from one WIM to another.)  There is nothing
 * here that lets you change individual files in the WIM; for that you will need
 * to look at the filesystem implementation in mount.c.
 *
 * Copyright (C) 2012 Eric Biggers
 *
 * wimlib - Library for working with WIM files 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option) any
 * later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with this library; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA 
 */

#include "wimlib_internal.h"
#include "util.h"
#include "dentry.h"
#include "xml.h"
#include "lookup_table.h"
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

/* 
 * Recursively builds a dentry tree from a directory tree on disk, outside the
 * WIM file.
 *
 * @root:  A dentry that has already been created for the root of the dentry
 * 	tree.
 * @source_path:  The path to the root of the tree on disk. 
 * @root_stat:   A pointer to a `struct stat' that contains the metadata for the
 * 	root of the tree on disk. 
 * @lookup_table: The lookup table for the WIM file.  For each file added to the
 * 		dentry tree being built, an entry is added to the lookup table, 
 * 		unless an identical file is already in the lookup table.  These
 * 		lookup table entries that are added point to the file on disk.
 *
 * @return:	0 on success, nonzero on failure.  It is a failure if any of
 *		the files cannot be `stat'ed, or if any of the needed
 *		directories cannot be opened or read.  Failure to add the files
 *		to the WIM may still occur later when trying to actually read 
 *		the regular files in the tree into the WIM as file resources.
 */
static int build_dentry_tree(struct dentry *root, const char *source_path, 
			struct stat *root_stat, struct lookup_table* lookup_table)
{
	int ret = 0;

	stbuf_to_dentry(root_stat, root);
	if (dentry_is_directory(root)) {
		/* Open the directory on disk */
		DIR *dir;
		struct dirent *p;
		struct stat child_stat;
		struct dentry *child;

		dir = opendir(source_path);
		if (!dir) {
			ERROR("Failed to open the directory `%s': %m\n",
					source_path);
			return WIMLIB_ERR_OPEN;
		}

		/* Buffer for names of files in directory. */
		size_t len = strlen(source_path);
		char name[len + 1 + FILENAME_MAX + 1];
		memcpy(name, source_path, len);
		name[len] = '/';
		errno = 0;

		/* Create a dentry for each entry in the directory on disk, and recurse
		 * to any subdirectories. */
		while ((p = readdir(dir)) != NULL) {
			if (p->d_name[0] == '.' && (p->d_name[1] == '\0'
				|| (p->d_name[1] == '.' && p->d_name[2] == '\0')))
					continue;
			strcpy(name + len + 1, p->d_name);
			if (stat(name, &child_stat) != 0) {
				ERROR("cannot stat `%s': %m\n", name);
				ret = WIMLIB_ERR_STAT;
				break;
			}
			child = new_dentry(p->d_name);
			ret = build_dentry_tree(child, name, &child_stat, 
						lookup_table);
			if (ret != 0)
				break;
			link_dentry(child, root);
		}
		closedir(dir);
	} else {
		struct lookup_table_entry *lte;

		/* For each non-directory, we must check to see if the file is
		 * in the lookup table already; if it is, we increment its
		 * refcnt; otherwise, we create a new lookup table entry and
		 * insert it. */
		ret = sha1sum(source_path, root->hash);
		if (ret != 0) {
			ERROR("Failed to calculate sha1sum for file `%s'\n", 
						source_path);
			return ret;
		}

		lte = lookup_resource(lookup_table, root->hash);
		if (lte) {
			lte->refcnt++;
		} else {
			char *file_on_disk = STRDUP(source_path);
			if (!file_on_disk) {
				ERROR("Failed to allocate memory for file "
						"path!\n");
				return WIMLIB_ERR_NOMEM;
			}
			lte = new_lookup_table_entry();
			if (!lte) {
				ERROR("Failed to allocate memory for new "
						"lookup table entry!\n");
				FREE(file_on_disk);
				return WIMLIB_ERR_NOMEM;
			}
			lte->file_on_disk = file_on_disk;
			lte->resource_entry.flags = 0;
			lte->refcnt       = 1;
			lte->part_number  = 1;
			lte->resource_entry.original_size = root_stat->st_size;
			lte->resource_entry.size = root_stat->st_size;
			memcpy(lte->hash, root->hash, WIM_HASH_SIZE);
			lookup_table_insert(lookup_table, lte);
		}
	}
	return ret;
}

struct wim_pair {
	WIMStruct *src_wim;
	WIMStruct *dest_wim;
};

/* 
 * This function takes in a dentry that was previously located only in image(s)
 * in @src_wim, but now is being added to @dest_wim. If there is in fact already a
 * lookup table entry for this file in the lookup table of the destination WIM
 * file, we simply increment its reference count.  Otherwise, a new lookup table
 * entry is created that references the location of the file resource in the
 * source WIM file through the other_wim_fp field of the lookup table entry.
 */
static int add_lookup_table_entry_to_dest_wim(struct dentry *dentry, void *arg)
{
	WIMStruct *src_wim, *dest_wim;
	struct lookup_table_entry *src_table_entry;
	struct lookup_table_entry *dest_table_entry;

	src_wim = ((struct wim_pair*)arg)->src_wim;
	dest_wim = ((struct wim_pair*)arg)->dest_wim;

	if (dentry_is_directory(dentry))
		return 0;

	src_table_entry = wim_lookup_resource(src_wim, dentry);
	if (!src_table_entry)
		return 0;

	dest_table_entry = wim_lookup_resource(dest_wim, dentry);
	if (dest_table_entry) {
		dest_table_entry->refcnt++;
	} else {
		dest_table_entry = new_lookup_table_entry();
		if (!dest_table_entry) {
			ERROR("Could not allocate lookup table entry!\n");
			return WIMLIB_ERR_NOMEM;
		}
		dest_table_entry->other_wim_fp = src_wim->fp;
		dest_table_entry->other_wim_ctype = 
				wimlib_get_compression_type(src_wim);
		dest_table_entry->refcnt = 1;
		memcpy(&dest_table_entry->resource_entry, 
		       &src_table_entry->resource_entry, 
		       sizeof(struct resource_entry));
		memcpy(dest_table_entry->hash, dentry->hash, WIM_HASH_SIZE);
		lookup_table_insert(dest_wim->lookup_table, dest_table_entry);
	}
	return 0;
}

/*
 * Adds an image (given by its dentry tree) to the image metadata array of a WIM
 * file, adds an entry to the lookup table for the image metadata, updates the
 * image count in the header, and selects the new image. 
 *
 * Does not update the XML data.
 *
 * @w:		  The WIMStruct for the WIM file.
 * @root_dentry:  The root of the directory tree for the image.
 */
static int add_new_dentry_tree(WIMStruct *w, struct dentry *root_dentry)
{
	struct lookup_table_entry *imd_lookup_entry;
	struct image_metadata *imd;
	struct image_metadata *new_imd;

	DEBUG("Reallocing image metadata array for image_count = %u\n",
			w->hdr.image_count + 1);
	imd = CALLOC((w->hdr.image_count + 1), sizeof(struct image_metadata));

	if (!imd) {
		ERROR("Failed to allocate memory for new image metadata "
				"array!\n");
		return WIMLIB_ERR_NOMEM;
	}

	memcpy(imd, w->image_metadata, 
	       w->hdr.image_count * sizeof(struct image_metadata));
	
	imd_lookup_entry = new_lookup_table_entry();
	if (!imd_lookup_entry) {
		ERROR("Failed to allocate new lookup table entry!\n");
		FREE(imd);
		return WIMLIB_ERR_NOMEM;
	}

	imd_lookup_entry->resource_entry.flags = WIM_RESHDR_FLAG_METADATA;
	randomize_byte_array(imd_lookup_entry->hash, WIM_HASH_SIZE);
	lookup_table_insert(w->lookup_table, imd_lookup_entry);

	w->hdr.image_count++;

	new_imd = &imd[w->hdr.image_count - 1];
	new_imd->lookup_table_entry = imd_lookup_entry;
	new_imd->modified           = true;
	new_imd->root_dentry        = root_dentry;
	w->image_metadata = imd;

	/* Change the current image to the new one. */
	return wimlib_select_image(w, w->hdr.image_count);
}

/*
 * Copies an image, or all the images, from a WIM file, into another WIM file.
 */
WIMLIBAPI int wimlib_export_image(WIMStruct *src_wim, 
				  int src_image, 
				  WIMStruct *dest_wim, 
				  const char *dest_name, 
				  const char *dest_description, 
				  int flags)
{
	int boot_idx;
	int i;
	int ret;
	struct dentry *root;
	struct wim_pair wims;

	if (src_image == WIM_ALL_IMAGES) {
		if (src_wim->hdr.image_count > 1) {

			/* multi-image export. */

			if (flags & WIMLIB_EXPORT_FLAG_BOOT) {

				/* Specifying the boot flag on a multi-image
				 * source WIM makes the boot index default to
				 * the bootable image in the source WIM.  It is
				 * an error if there is no such bootable image.
				 * */

				if (src_wim->hdr.boot_idx == 0) {
					ERROR("Cannot specify `boot' flag "
							"when exporting multiple "
							"images from a WIM with no "
							"bootable images!\n");
					return WIMLIB_ERR_INVALID_PARAM;
				} else {
					boot_idx = src_wim->hdr.boot_idx;
				}
			}
			if (dest_name || dest_description) {
				ERROR("Image name or image description "
						"was specified, but we are exporting "
						"multiple images!\n");
				return WIMLIB_ERR_INVALID_PARAM;
			}
			for (i = 1; i <= src_wim->hdr.image_count; i++) {
				int export_flags = flags;

				if (i != boot_idx)
					export_flags &= ~WIMLIB_EXPORT_FLAG_BOOT;

				ret = wimlib_export_image(src_wim, i, dest_wim, 
							NULL, dest_description,
							export_flags);
				if (ret != 0)
					return ret;
			}
			return 0;
		} else {
			src_image = 1; 
		}
	}

	ret = wimlib_select_image(src_wim, src_image);
	if (ret != 0) {
		ERROR("Could not select image %d from the WIM `%s' "
			"to export it!\n", src_image, src_wim->filename);
		return ret;
	}

	if (!dest_name) {
		dest_name = wimlib_get_image_name(src_wim, src_image);
		DEBUG("Using name `%s' for source image %d\n", 
				dest_name, src_image);
	}

	DEBUG("Exporting image %d from `%s'\n", src_image, src_wim->filename);

	if (wimlib_image_name_in_use(dest_wim, dest_name)) {
		ERROR("There is already an image named `%s' "
			"in the destination WIM!\n", dest_name);
		return WIMLIB_ERR_IMAGE_NAME_COLLISION;
	}


	root = wim_root_dentry(src_wim);
	for_dentry_in_tree(root, increment_dentry_refcnt, NULL);
	wims.src_wim = src_wim;
	wims.dest_wim = dest_wim;
	for_dentry_in_tree(root, add_lookup_table_entry_to_dest_wim, &wims);
	ret = add_new_dentry_tree(dest_wim, root);
#ifdef ENABLE_SECURITY_DATA
	struct wim_security_data *sd = wim_security_data(src_wim);
	struct image_metadata *new_imd = wim_get_current_image_metadata(dest_wim);
	new_imd->security_data = sd;
	if (sd)
		sd->refcnt++;
#endif
	if (ret != 0)
		return ret;

	if (flags & WIMLIB_EXPORT_FLAG_BOOT) {
		DEBUG("Setting boot_idx to %d\n", dest_wim->hdr.image_count);
		dest_wim->hdr.boot_idx = dest_wim->hdr.image_count;
	}

	return xml_export_image(src_wim->wim_info, src_image, &dest_wim->wim_info,
			dest_name, dest_description);
}

/* 
 * Deletes an image from the WIM. 
 */
WIMLIBAPI int wimlib_delete_image(WIMStruct *w, int image)
{
	int num_images;
	int i;
	int ret;
	struct image_metadata *imd;

	if (image == WIM_ALL_IMAGES) {
		num_images = w->hdr.image_count;
		for (i = 1; i <= num_images; i++) {
			/* Always delete the first image, since by the end
			 * there won't be any more than that!  */
			ret = wimlib_delete_image(w, 1);
			if (ret != 0)
				return ret;
		}
		return 0;
	}

	DEBUG("Deleting image %d\n", image);

	/* Even if the dentry tree is not allocated, we must select it (and
	 * therefore allocate it) so that we can decrement the reference counts
	 * in the lookup table.  */
	ret = wimlib_select_image(w, image);
	if (ret != 0)
		return ret;

	/* Free the dentry tree, any lookup table entries that have their
	 * refcnt decremented to 0, and the security data. */
	imd = wim_get_current_image_metadata(w);
	free_dentry_tree(imd->root_dentry, w->lookup_table, true);
#ifdef ENABLE_SECURITY_DATA
	free_security_data(imd->security_data);
#endif

	/* Get rid of the lookup table entry for this image's metadata resource
	 * */
	lookup_table_remove(w->lookup_table, imd->lookup_table_entry);

	/* Get rid of the empty slot in the image metadata array. */
	for (i = image - 1; i < w->hdr.image_count - 1; i++)
		memcpy(&w->image_metadata[i], &w->image_metadata[i + 1],
				sizeof(struct image_metadata));

	/* Decrement the image count. */
	w->hdr.image_count--;
	if (w->hdr.image_count == 0) {
		FREE(w->image_metadata);
		w->image_metadata = NULL;
	}

	/* Fix the boot index. */
	if (w->hdr.boot_idx == image)
		w->hdr.boot_idx = 0;
	else if (w->hdr.boot_idx > image)
		w->hdr.boot_idx--;

	w->current_image = WIM_NO_IMAGE;

	/* Remove the image from the XML information. */
	xml_delete_image(&w->wim_info, image);
	return 0;
}

/*
 * Adds an image to a WIM file from a directory tree on disk.
 */
WIMLIBAPI int wimlib_add_image(WIMStruct *w, const char *dir, 
			       const char *name, const char *description, 
			       const char *flags_element, int flags)
{
	struct dentry *root_dentry;
	struct stat root_stat;
	int ret;

	DEBUG("Adding dentry tree from dir `%s'\n", dir);

	if (!name || !*name) {
		ERROR("Must specify a non-empty string for the image name!\n");
		return WIMLIB_ERR_INVALID_PARAM;
	}
	if (!dir) {
		ERROR("Must specify the name of a directory!\n");
		return WIMLIB_ERR_INVALID_PARAM;
	}

	if (wimlib_image_name_in_use(w, name)) {
		ERROR("There is already an image named `%s' in %s!\n",
				name, w->filename);
		return WIMLIB_ERR_IMAGE_NAME_COLLISION;
	}

	DEBUG("Creating root dentry.\n");

	root_dentry = new_dentry("");
	ret = calculate_dentry_full_path(root_dentry, NULL);
	if (ret != 0)
		return ret;
	root_dentry->attributes |= WIM_FILE_ATTRIBUTE_DIRECTORY;

	/* Construct the dentry tree from the outside filesystem. */
	if (stat(dir, &root_stat) != 0) {
		ERROR("Failed to stat `%s': %m\n", dir);
		return WIMLIB_ERR_STAT;
	}
	if (!S_ISDIR(root_stat.st_mode)) {
		ERROR("`%s' is not a directory!\n", dir);
		return WIMLIB_ERR_NOTDIR;
	}
	DEBUG("Building dentry tree.\n");
	ret = build_dentry_tree(root_dentry, dir, &root_stat, 
				w->lookup_table);

	if (ret != 0) {
		ERROR("Failed to build dentry tree for `%s'!\n", dir);
		goto err1;
	}

	DEBUG("Recalculating full paths of dentries.\n");
	ret = for_dentry_in_tree(root_dentry, 
				 calculate_dentry_full_path, NULL);
	if (ret != 0) {
		ERROR("Failed to calculate full paths of dentry tree.\n");
		goto err1;
	}

	ret = add_new_dentry_tree(w, root_dentry);
	if (ret != 0)
		goto err1;

	if (flags & WIMLIB_ADD_IMAGE_FLAG_BOOT) {
		/* Call wimlib_set_boot_idx rather than set boot_idx directly so
		 * that the boot metadata resource entry in the header gets
		 * updated. */
		wimlib_set_boot_idx(w, w->hdr.image_count);
	}

	ret = xml_add_image(w, root_dentry, name, description, flags_element);
	if (ret != 0)
		goto err1;

	return 0;
err1:
	free_dentry_tree(root_dentry, w->lookup_table, true);
	return ret;
}
