/*
 * resource.c
 *
 * Read uncompressed and compressed metadata and file resources from a WIM file.
 */

/*
 * Copyright (C) 2012, 2013 Eric Biggers
 *
 * This file is part of wimlib, a library for working with WIM files.
 *
 * wimlib is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * wimlib is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * wimlib; if not, see http://www.gnu.org/licenses/.
 */

#include "wimlib_internal.h"
#include "dentry.h"
#include "lookup_table.h"
#include "buffer_io.h"
#include "lzx.h"
#include "xpress.h"
#include "sha1.h"

#ifdef __WIN32__
#  include "win32.h"
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef WITH_NTFS_3G
#  include <time.h>
#  include <ntfs-3g/attrib.h>
#  include <ntfs-3g/inode.h>
#  include <ntfs-3g/dir.h>
#endif

#if defined(__WIN32__) && !defined(INVALID_HANDLE_VALUE)
#  define INVALID_HANDLE_VALUE ((HANDLE)(-1))
#endif

/*
 * Reads all or part of a compressed resource into an in-memory buffer.
 *
 * @fp:      		The FILE* for the WIM file.
 * @resource_compressed_size:  	 The compressed size of the resource.
 * @resource_uncompressed_size:  The uncompressed size of the resource.
 * @resource_offset:		 The offset of the start of the resource from
 * 					the start of the stream @fp.
 * @resource_ctype:	The compression type of the resource.
 * @len:		The number of bytes of uncompressed data to read from
 * 				the resource.
 * @offset:		The offset of the bytes to read within the uncompressed
 * 				resource.
 * @contents_len:	An array into which the uncompressed data is written.
 * 				It must be at least @len bytes long.
 *
 * Returns zero on success, nonzero on failure.
 */
static int
read_compressed_resource(FILE *fp, u64 resource_compressed_size,
			 u64 resource_uncompressed_size,
			 u64 resource_offset, int resource_ctype,
			 u64 len, u64 offset, void *contents_ret)
{

	DEBUG2("comp size = %"PRIu64", uncomp size = %"PRIu64", "
	       "res offset = %"PRIu64"",
	       resource_compressed_size,
	       resource_uncompressed_size,
	       resource_offset);
	DEBUG2("resource_ctype = %"TS", len = %"PRIu64", offset = %"PRIu64"",
	       wimlib_get_compression_type_string(resource_ctype), len, offset);
	/* Trivial case */
	if (len == 0)
		return 0;

	int (*decompress)(const void *, unsigned, void *, unsigned);
	/* Set the appropriate decompress function. */
	if (resource_ctype == WIMLIB_COMPRESSION_TYPE_LZX)
		decompress = lzx_decompress;
	else
		decompress = xpress_decompress;

	/* The structure of a compressed resource consists of a table of chunk
	 * offsets followed by the chunks themselves.  Each chunk consists of
	 * compressed data, and there is one chunk for each WIM_CHUNK_SIZE =
	 * 32768 bytes of the uncompressed file, with the last chunk having any
	 * remaining bytes.
	 *
	 * The chunk offsets are measured relative to the end of the chunk
	 * table.  The first chunk is omitted from the table in the WIM file
	 * because its offset is implicitly given by the fact that it directly
	 * follows the chunk table and therefore must have an offset of 0.
	 */

	/* Calculate how many chunks the resource conists of in its entirety. */
	u64 num_chunks = (resource_uncompressed_size + WIM_CHUNK_SIZE - 1) /
								WIM_CHUNK_SIZE;
	/* As mentioned, the first chunk has no entry in the chunk table. */
	u64 num_chunk_entries = num_chunks - 1;


	/* The index of the chunk that the read starts at. */
	u64 start_chunk = offset / WIM_CHUNK_SIZE;
	/* The byte offset at which the read starts, within the start chunk. */
	u64 start_chunk_offset = offset % WIM_CHUNK_SIZE;

	/* The index of the chunk that contains the last byte of the read. */
	u64 end_chunk   = (offset + len - 1) / WIM_CHUNK_SIZE;
	/* The byte offset of the last byte of the read, within the end chunk */
	u64 end_chunk_offset = (offset + len - 1) % WIM_CHUNK_SIZE;

	/* Number of chunks that are actually needed to read the requested part
	 * of the file. */
	u64 num_needed_chunks = end_chunk - start_chunk + 1;

	/* If the end chunk is not the last chunk, an extra chunk entry is
	 * needed because we need to know the offset of the chunk after the last
	 * chunk read to figure out the size of the last read chunk. */
	if (end_chunk != num_chunks - 1)
		num_needed_chunks++;

	/* Declare the chunk table.  It will only contain offsets for the chunks
	 * that are actually needed for this read. */
	u64 chunk_offsets[num_needed_chunks];

	/* Set the implicit offset of the first chunk if it is included in the
	 * needed chunks.
	 *
	 * Note: M$'s documentation includes a picture that shows the first
	 * chunk starting right after the chunk entry table, labeled as offset
	 * 0x10.  However, in the actual file format, the offset is measured
	 * from the end of the chunk entry table, so the first chunk has an
	 * offset of 0. */
	if (start_chunk == 0)
		chunk_offsets[0] = 0;

	/* According to M$'s documentation, if the uncompressed size of
	 * the file is greater than 4 GB, the chunk entries are 8-byte
	 * integers.  Otherwise, they are 4-byte integers. */
	u64 chunk_entry_size = (resource_uncompressed_size >= (u64)1 << 32) ?
									8 : 4;

	/* Size of the full chunk table in the WIM file. */
	u64 chunk_table_size = chunk_entry_size * num_chunk_entries;

	/* Read the needed chunk offsets from the table in the WIM file. */

	/* Index, in the WIM file, of the first needed entry in the
	 * chunk table. */
	u64 start_table_idx = (start_chunk == 0) ? 0 : start_chunk - 1;

	/* Number of entries we need to actually read from the chunk
	 * table (excludes the implicit first chunk). */
	u64 num_needed_chunk_entries = (start_chunk == 0) ?
				num_needed_chunks - 1 : num_needed_chunks;

	/* Skip over unneeded chunk table entries. */
	u64 file_offset_of_needed_chunk_entries = resource_offset +
				start_table_idx * chunk_entry_size;
	if (fseeko(fp, file_offset_of_needed_chunk_entries, SEEK_SET) != 0) {
		ERROR_WITH_ERRNO("Failed to seek to byte %"PRIu64" to read "
				 "chunk table of compressed resource",
				 file_offset_of_needed_chunk_entries);
		return WIMLIB_ERR_READ;
	}

	/* Number of bytes we need to read from the chunk table. */
	size_t size = num_needed_chunk_entries * chunk_entry_size;

	u8 chunk_tab_buf[size];

	if (fread(chunk_tab_buf, 1, size, fp) != size)
		goto err;

	/* Now fill in chunk_offsets from the entries we have read in
	 * chunk_tab_buf. */

	u64 *chunk_tab_p = chunk_offsets;
	if (start_chunk == 0)
		chunk_tab_p++;

	if (chunk_entry_size == 4) {
		u32 *entries = (u32*)chunk_tab_buf;
		while (num_needed_chunk_entries--)
			*chunk_tab_p++ = le32_to_cpu(*entries++);
	} else {
		u64 *entries = (u64*)chunk_tab_buf;
		while (num_needed_chunk_entries--)
			*chunk_tab_p++ = le64_to_cpu(*entries++);
	}

	/* Done with the chunk table now.  We must now seek to the first chunk
	 * that is needed for the read. */

	u64 file_offset_of_first_needed_chunk = resource_offset +
				chunk_table_size + chunk_offsets[0];
	if (fseeko(fp, file_offset_of_first_needed_chunk, SEEK_SET) != 0) {
		ERROR_WITH_ERRNO("Failed to seek to byte %"PRIu64" to read "
				 "first chunk of compressed resource",
				 file_offset_of_first_needed_chunk);
		return WIMLIB_ERR_READ;
	}

	/* Pointer to current position in the output buffer for uncompressed
	 * data. */
	u8 *out_p = contents_ret;

	/* Buffer for compressed data.  While most compressed chunks will have a
	 * size much less than WIM_CHUNK_SIZE, WIM_CHUNK_SIZE - 1 is the maximum
	 * size in the worst-case.  This assumption is valid only if chunks that
	 * happen to compress to more than the uncompressed size (i.e. a
	 * sequence of random bytes) are always stored uncompressed. But this seems
	 * to be the case in M$'s WIM files, even though it is undocumented. */
	u8 compressed_buf[WIM_CHUNK_SIZE - 1];


	/* Decompress all the chunks. */
	for (u64 i = start_chunk; i <= end_chunk; i++) {

		DEBUG2("Chunk %"PRIu64" (start %"PRIu64", end %"PRIu64").",
		       i, start_chunk, end_chunk);

		/* Calculate the sizes of the compressed chunk and of the
		 * uncompressed chunk. */
		unsigned compressed_chunk_size;
		unsigned uncompressed_chunk_size;
		if (i != num_chunks - 1) {
			/* All the chunks except the last one in the resource
			 * expand to WIM_CHUNK_SIZE uncompressed, and the amount
			 * of compressed data for the chunk is given by the
			 * difference of offsets in the chunk offset table. */
			compressed_chunk_size = chunk_offsets[i + 1 - start_chunk] -
						chunk_offsets[i - start_chunk];
			uncompressed_chunk_size = WIM_CHUNK_SIZE;
		} else {
			/* The last compressed chunk consists of the remaining
			 * bytes in the file resource, and the last uncompressed
			 * chunk has size equal to however many bytes are left-
			 * that is, the remainder of the uncompressed size when
			 * divided by WIM_CHUNK_SIZE.
			 *
			 * Note that the resource_compressed_size includes the
			 * chunk table, so the size of it must be subtracted. */
			compressed_chunk_size = resource_compressed_size -
						chunk_table_size -
						chunk_offsets[i - start_chunk];

			uncompressed_chunk_size = resource_uncompressed_size %
								WIM_CHUNK_SIZE;

			/* If the remainder is 0, the last chunk actually
			 * uncompresses to a full WIM_CHUNK_SIZE bytes. */
			if (uncompressed_chunk_size == 0)
				uncompressed_chunk_size = WIM_CHUNK_SIZE;
		}

		DEBUG2("compressed_chunk_size = %u, "
		       "uncompressed_chunk_size = %u",
		       compressed_chunk_size, uncompressed_chunk_size);


		/* Figure out how much of this chunk we actually need to read */
		u64 start_offset;
		if (i == start_chunk)
			start_offset = start_chunk_offset;
		else
			start_offset = 0;
		u64 end_offset;
		if (i == end_chunk)
			end_offset = end_chunk_offset;
		else
			end_offset = WIM_CHUNK_SIZE - 1;

		u64 partial_chunk_size = end_offset + 1 - start_offset;
		bool is_partial_chunk = (partial_chunk_size !=
						uncompressed_chunk_size);

		DEBUG2("start_offset = %"PRIu64", end_offset = %"PRIu64"",
		       start_offset, end_offset);
		DEBUG2("partial_chunk_size = %"PRIu64"", partial_chunk_size);

		/* This is undocumented, but chunks can be uncompressed.  This
		 * appears to always be the case when the compressed chunk size
		 * is equal to the uncompressed chunk size. */
		if (compressed_chunk_size == uncompressed_chunk_size) {
			/* Probably an uncompressed chunk */

			if (start_offset != 0) {
				if (fseeko(fp, start_offset, SEEK_CUR) != 0) {
					ERROR_WITH_ERRNO("Uncompressed partial "
							 "chunk fseek() error");
					return WIMLIB_ERR_READ;
				}
			}
			if (fread(out_p, 1, partial_chunk_size, fp) !=
					partial_chunk_size)
				goto err;
		} else {
			/* Compressed chunk */
			int ret;

			/* Read the compressed data into compressed_buf. */
			if (fread(compressed_buf, 1, compressed_chunk_size,
						fp) != compressed_chunk_size)
				goto err;

			/* For partial chunks we must buffer the uncompressed
			 * data because we don't need all of it. */
			if (is_partial_chunk) {
				u8 uncompressed_buf[uncompressed_chunk_size];

				ret = decompress(compressed_buf,
						compressed_chunk_size,
						uncompressed_buf,
						uncompressed_chunk_size);
				if (ret != 0)
					return WIMLIB_ERR_DECOMPRESSION;
				memcpy(out_p, uncompressed_buf + start_offset,
						partial_chunk_size);
			} else {
				ret = decompress(compressed_buf,
						compressed_chunk_size,
						out_p,
						uncompressed_chunk_size);
				if (ret != 0)
					return WIMLIB_ERR_DECOMPRESSION;
			}
		}

		/* Advance the pointer into the uncompressed output data by the
		 * number of uncompressed bytes that were written.  */
		out_p += partial_chunk_size;
	}

	return 0;

err:
	if (feof(fp))
		ERROR("Unexpected EOF in compressed file resource");
	else
		ERROR_WITH_ERRNO("Error reading compressed file resource");
	return WIMLIB_ERR_READ;
}

/*
 * Reads uncompressed data from an open file stream.
 */
int
read_uncompressed_resource(FILE *fp, u64 offset, u64 len, void *contents_ret)
{
	if (fseeko(fp, offset, SEEK_SET) != 0) {
		ERROR("Failed to seek to byte %"PRIu64" of input file "
		      "to read uncompressed resource (len = %"PRIu64")",
		      offset, len);
		return WIMLIB_ERR_READ;
	}
	if (fread(contents_ret, 1, len, fp) != len) {
		if (feof(fp)) {
			ERROR("Unexpected EOF in uncompressed file resource");
		} else {
			ERROR("Failed to read %"PRIu64" bytes from "
			      "uncompressed resource at offset %"PRIu64,
			      len, offset);
		}
		return WIMLIB_ERR_READ;
	}
	return 0;
}

/* Reads the contents of a struct resource_entry, as represented in the on-disk
 * format, from the memory pointed to by @p, and fills in the fields of @entry.
 * A pointer to the byte after the memory read at @p is returned. */
const u8 *
get_resource_entry(const u8 *p, struct resource_entry *entry)
{
	u64 size;
	u8 flags;

	p = get_u56(p, &size);
	p = get_u8(p, &flags);
	entry->size = size;
	entry->flags = flags;

	/* offset and original_size are truncated to 62 bits to avoid possible
	 * overflows, when converting to a signed 64-bit integer (off_t) or when
	 * adding size or original_size.  This is okay since no one would ever
	 * actually have a WIM bigger than 4611686018427387903 bytes... */
	p = get_u64(p, &entry->offset);
	if (entry->offset & 0xc000000000000000ULL) {
		WARNING("Truncating offset in resource entry");
		entry->offset &= 0x3fffffffffffffffULL;
	}
	p = get_u64(p, &entry->original_size);
	if (entry->original_size & 0xc000000000000000ULL) {
		WARNING("Truncating original_size in resource entry");
		entry->original_size &= 0x3fffffffffffffffULL;
	}
	return p;
}

/* Copies the struct resource_entry @entry to the memory pointed to by @p in the
 * on-disk format.  A pointer to the byte after the memory written at @p is
 * returned. */
u8 *
put_resource_entry(u8 *p, const struct resource_entry *entry)
{
	p = put_u56(p, entry->size);
	p = put_u8(p, entry->flags);
	p = put_u64(p, entry->offset);
	p = put_u64(p, entry->original_size);
	return p;
}

#ifdef WITH_FUSE
static FILE *
wim_get_fp(WIMStruct *w)
{
	pthread_mutex_lock(&w->fp_tab_mutex);
	FILE *fp;

	wimlib_assert(w->filename != NULL);

	for (size_t i = 0; i < w->num_allocated_fps; i++) {
		if (w->fp_tab[i]) {
			fp = w->fp_tab[i];
			w->fp_tab[i] = NULL;
			goto out;
		}
	}
	DEBUG("Opening extra file descriptor to `%"TS"'", w->filename);
	fp = tfopen(w->filename, T("rb"));
	if (!fp)
		ERROR_WITH_ERRNO("Failed to open `%"TS"'", w->filename);
out:
	pthread_mutex_unlock(&w->fp_tab_mutex);
	return fp;
}

static int
wim_release_fp(WIMStruct *w, FILE *fp)
{
	int ret = 0;
	FILE **fp_tab;

	pthread_mutex_lock(&w->fp_tab_mutex);

	for (size_t i = 0; i < w->num_allocated_fps; i++) {
		if (w->fp_tab[i] == NULL) {
			w->fp_tab[i] = fp;
			goto out;
		}
	}

	fp_tab = REALLOC(w->fp_tab, sizeof(FILE*) * (w->num_allocated_fps + 4));
	if (!fp_tab) {
		ret = WIMLIB_ERR_NOMEM;
		goto out;
	}
	w->fp_tab = fp_tab;
	memset(&w->fp_tab[w->num_allocated_fps], 0, 4 * sizeof(FILE*));
	w->fp_tab[w->num_allocated_fps] = fp;
	w->num_allocated_fps += 4;
out:
	pthread_mutex_unlock(&w->fp_tab_mutex);
	return ret;
}
#endif /* !WITH_FUSE */

/*
 * Reads some data from the resource corresponding to a WIM lookup table entry.
 *
 * @lte:	The WIM lookup table entry for the resource.
 * @buf:	Buffer into which to write the data.
 * @size:	Number of bytes to read.
 * @offset:	Offset at which to start reading the resource.
 *
 * Returns zero on success, nonzero on failure.
 */
int
read_wim_resource(const struct wim_lookup_table_entry *lte, void *buf,
		  size_t size, u64 offset, int flags)
{
	int ctype;
	int ret = 0;
	FILE *fp;

	/* We shouldn't be allowing read over-runs in any part of the library.
	 * */
	if (flags & WIMLIB_RESOURCE_FLAG_RAW)
		wimlib_assert(offset + size <= lte->resource_entry.size);
	else
		wimlib_assert(offset + size <= lte->resource_entry.original_size);

	switch (lte->resource_location) {
	case RESOURCE_IN_WIM:
		/* The resource is in a WIM file, and its WIMStruct is given by
		 * the lte->wim member.  The resource may be either compressed
		 * or uncompressed. */
		wimlib_assert(lte->wim != NULL);

		#ifdef WITH_FUSE
		if (flags & WIMLIB_RESOURCE_FLAG_MULTITHREADED) {
			fp = wim_get_fp(lte->wim);
			if (!fp)
				return WIMLIB_ERR_OPEN;
		} else
		#endif
		{
			wimlib_assert(!(flags & WIMLIB_RESOURCE_FLAG_MULTITHREADED));
			wimlib_assert(lte->wim->fp != NULL);
			fp = lte->wim->fp;
		}

		ctype = wim_resource_compression_type(lte);

		wimlib_assert(ctype != WIMLIB_COMPRESSION_TYPE_NONE ||
			      (lte->resource_entry.original_size ==
			       lte->resource_entry.size));

		if ((flags & WIMLIB_RESOURCE_FLAG_RAW)
		    || ctype == WIMLIB_COMPRESSION_TYPE_NONE)
			ret = read_uncompressed_resource(fp,
							 lte->resource_entry.offset + offset,
							 size, buf);
		else
			ret = read_compressed_resource(fp,
						       lte->resource_entry.size,
						       lte->resource_entry.original_size,
						       lte->resource_entry.offset,
						       ctype, size, offset, buf);
	#ifdef WITH_FUSE
		if (flags & WIMLIB_RESOURCE_FLAG_MULTITHREADED) {
			int ret2 = wim_release_fp(lte->wim, fp);
			if (ret == 0)
				ret = ret2;
		}
	#endif
		break;
	case RESOURCE_IN_STAGING_FILE:
	case RESOURCE_IN_FILE_ON_DISK:
		/* The resource is in some file on the external filesystem and
		 * needs to be read uncompressed */
		wimlib_assert(lte->file_on_disk != NULL);
		BUILD_BUG_ON(&lte->file_on_disk != &lte->staging_file_name);
		/* Use existing file pointer if available; otherwise open one
		 * temporarily */
		if (lte->file_on_disk_fp) {
			fp = lte->file_on_disk_fp;
		} else {
			fp = tfopen(lte->file_on_disk, T("rb"));
			if (!fp) {
				ERROR_WITH_ERRNO("Failed to open the file "
						 "`%"TS"'", lte->file_on_disk);
				ret = WIMLIB_ERR_OPEN;
				break;
			}
		}
		ret = read_uncompressed_resource(fp, offset, size, buf);
		if (fp != lte->file_on_disk_fp)
			fclose(fp);
		break;
#ifdef __WIN32__
	case RESOURCE_WIN32:
		wimlib_assert(lte->win32_file_on_disk_fp != INVALID_HANDLE_VALUE);
		ret = win32_read_file(lte->file_on_disk,
				      lte->win32_file_on_disk_fp, offset,
				      size, buf);
		break;
#endif
	case RESOURCE_IN_ATTACHED_BUFFER:
		/* The resource is directly attached uncompressed in an
		 * in-memory buffer. */
		wimlib_assert(lte->attached_buffer != NULL);
		memcpy(buf, lte->attached_buffer + offset, size);
		break;
#ifdef WITH_NTFS_3G
	case RESOURCE_IN_NTFS_VOLUME:
		wimlib_assert(lte->ntfs_loc != NULL);
		wimlib_assert(lte->attr != NULL);
		if (lte->ntfs_loc->is_reparse_point)
			offset += 8;
		if (ntfs_attr_pread(lte->attr, offset, size, buf) != size) {
			ERROR_WITH_ERRNO("Error reading NTFS attribute "
					 "at `%"TS"'",
					 lte->ntfs_loc->path);
			ret = WIMLIB_ERR_NTFS_3G;
		}
		break;
#endif
	default:
		wimlib_assert(0);
		ret = -1;
		break;
	}
	return ret;
}

/*
 * Reads all the data from the resource corresponding to a WIM lookup table
 * entry.
 *
 * @lte:	The WIM lookup table entry for the resource.
 * @buf:	Buffer into which to write the data.  It must be at least
 * 		wim_resource_size(lte) bytes long.
 *
 * Returns 0 on success; nonzero on failure.
 */
int
read_full_wim_resource(const struct wim_lookup_table_entry *lte,
		       void *buf, int flags)
{
	return read_wim_resource(lte, buf, wim_resource_size(lte), 0, flags);
}

/* Extracts the first @size bytes of a WIM resource to somewhere.  In the
 * process, the SHA1 message digest of the resource is checked if the full
 * resource is being extracted.
 *
 * @extract_chunk is a function that is called to extract each chunk of the
 * resource. */
int
extract_wim_resource(const struct wim_lookup_table_entry *lte,
		     u64 size,
		     extract_chunk_func_t extract_chunk,
		     void *extract_chunk_arg)
{
	u64 bytes_remaining = size;
	u8 buf[min(WIM_CHUNK_SIZE, bytes_remaining)];
	u64 offset = 0;
	int ret = 0;
	u8 hash[SHA1_HASH_SIZE];
	bool check_hash = (size == wim_resource_size(lte));
	SHA_CTX ctx;

	if (check_hash)
		sha1_init(&ctx);

	while (bytes_remaining) {
		u64 to_read = min(bytes_remaining, sizeof(buf));
		ret = read_wim_resource(lte, buf, to_read, offset, 0);
		if (ret != 0)
			return ret;
		if (check_hash)
			sha1_update(&ctx, buf, to_read);
		ret = extract_chunk(buf, to_read, offset, extract_chunk_arg);
		if (ret != 0) {
			ERROR_WITH_ERRNO("Error extracting WIM resource");
			return ret;
		}
		bytes_remaining -= to_read;
		offset += to_read;
	}
	if (check_hash) {
		sha1_final(hash, &ctx);
		if (!hashes_equal(hash, lte->hash)) {
		#ifdef ENABLE_ERROR_MESSAGES
			ERROR("Invalid checksum on the following WIM resource:");
			print_lookup_table_entry(lte, stderr);
		#endif
			return WIMLIB_ERR_INVALID_RESOURCE_HASH;
		}
	}
	return 0;
}

/* Write @n bytes from @buf to the file descriptor @fd, retrying on internupt
 * and on short writes.
 *
 * Returns short count and set errno on failure. */
static ssize_t
full_write(int fd, const void *buf, size_t n)
{
	const void *p = buf;
	ssize_t ret;
	ssize_t total = 0;

	while (total != n) {
		ret = write(fd, p, n);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			else
				break;
		}
		total += ret;
		p += ret;
	}
	return total;
}

int
extract_wim_chunk_to_fd(const void *buf, size_t len, u64 offset, void *arg)
{
	int fd = *(int*)arg;
	ssize_t ret = full_write(fd, buf, len);
	if (ret < len) {
		ERROR_WITH_ERRNO("Error writing to file descriptor");
		return WIMLIB_ERR_WRITE;
	} else {
		return 0;
	}
}

/*
 * Copies the file resource specified by the lookup table entry @lte from the
 * input WIM to the output WIM that has its FILE * given by
 * ((WIMStruct*)wim)->out_fp.
 *
 * The output_resource_entry, out_refcnt, and part_number fields of @lte are
 * updated.
 *
 * (This function is confusing and should be refactored somehow.)
 */
int
copy_resource(struct wim_lookup_table_entry *lte, void *wim)
{
	WIMStruct *w = wim;
	int ret;

	if ((lte->resource_entry.flags & WIM_RESHDR_FLAG_METADATA) &&
	    !w->write_metadata)
		return 0;

	ret = write_wim_resource(lte, w->out_fp,
				 wim_resource_compression_type(lte),
				 &lte->output_resource_entry, 0);
	if (ret != 0)
		return ret;
	lte->out_refcnt = lte->refcnt;
	lte->part_number = w->hdr.part_number;
	return 0;
}
