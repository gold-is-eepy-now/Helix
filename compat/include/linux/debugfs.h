/* linux/debugfs.h — Helix has no debugfs: directories and files are accepted and dropped. */
#ifndef HELIX_COMPAT_LINUX_DEBUGFS_H
#define HELIX_COMPAT_LINUX_DEBUGFS_H
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
struct dentry { int unused; };
ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos, const void *from, size_t available);
struct dentry *debugfs_create_dir(const char *name, struct dentry *parent);
void debugfs_create_bool(const char *name, unsigned short mode, struct dentry *parent, bool *value);
struct dentry *debugfs_create_file(const char *name, unsigned short mode, struct dentry *parent,
                                   void *data, const struct file_operations *fops);
void debugfs_remove_recursive(struct dentry *dentry);
#endif
