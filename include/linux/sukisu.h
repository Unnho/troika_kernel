/* SPDX-License-Identifier: GPL-2.0 */
/* Manual-hook interface for the pinned SukiSU built-in submodule. */
#ifndef _LINUX_SUKISU_H
#define _LINUX_SUKISU_H

#include <linux/fs.h>
#include <linux/types.h>

struct filename;

#ifdef CONFIG_KSU
int ksu_handle_execveat(int *fd, struct filename **filename, void *argv,
			void *envp, int *flags);
int ksu_handle_faccessat(int *dfd, const char __user **filename, int *mode,
			 int *flags);
int ksu_handle_stat(int *dfd, const char __user **filename, int *flags);
int ksu_handle_vfs_read(struct file **file, char __user **buf, size_t *count,
			loff_t *pos);
void ksu_handle_vfs_fstat(int fd, loff_t *kstat_size);
#endif

#endif /* _LINUX_SUKISU_H */
