#ifndef _WIMLIB_WIN32_H
#define _WIMLIB_WIN32_H

#ifdef __WIN32__

#include "wimlib/callback.h"
#include "wimlib/types.h"
#include <direct.h>
#include <windef.h>

struct wim_lookup_table_entry;
struct iovec;

extern int
read_win32_file_prefix(const struct wim_lookup_table_entry *lte,
		       u64 size,
		       consume_data_callback_t cb,
		       void *ctx_or_buf,
		       int _ignored_flags);

extern int
read_win32_encrypted_file_prefix(const struct wim_lookup_table_entry *lte,
				 u64 size,
				 consume_data_callback_t cb,
				 void *ctx_or_buf,
				 int _ignored_flags);


extern void
win32_global_init(void);

extern void
win32_global_cleanup(void);

#define FNM_PATHNAME 0x1
#define FNM_NOESCAPE 0x2
#define FNM_NOMATCH 1
extern int
fnmatch(const tchar *pattern, const tchar *string, int flags);

extern int
fsync(int fd);

extern unsigned
win32_get_number_of_processors(void);

extern tchar *
realpath(const tchar *path, tchar *resolved_path);

typedef enum {
	CODESET
} nl_item;

extern int
win32_rename_replacement(const tchar *oldpath, const tchar *newpath);

extern int
win32_truncate_replacement(const tchar *path, off_t size);

extern int
win32_strerror_r_replacement(int errnum, tchar *buf, size_t buflen);

extern int
win32_get_file_and_vol_ids(const wchar_t *path, u64 *ino_ret, u64 *dev_ret);


extern ssize_t
pread(int fd, void *buf, size_t count, off_t offset);

extern ssize_t
pwrite(int fd, const void *buf, size_t count, off_t offset);

extern ssize_t
writev(int fd, const struct iovec *iov, int iovcnt);

#endif /* __WIN32__ */

#endif /* _WIMLIB_WIN32_H */