/* linux/fs.h — the file and inode types a character device's file_operations uses. Helix has
 * no VFS node for these; applespi's debugfs file is a stub (see debugfs.h). */
#ifndef HELIX_COMPAT_LINUX_FS_H
#define HELIX_COMPAT_LINUX_FS_H
#include <linux/types.h>

/* Transfer direction for the SPI message helpers (Linux's fs.h defines these as well). */
#define READ 0
#define WRITE 1

struct inode { void *i_private; };
struct file { void *private_data; };
struct module;
#define THIS_MODULE ((struct module *)0)
struct file_operations {
    struct module *owner;
    int (*open)(struct inode *, struct file *);
    ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
    loff_t (*llseek)(struct file *, loff_t, int);
    int (*release)(struct inode *, struct file *);
};
static inline loff_t default_llseek(struct file *f, loff_t o, int w) { (void)f; (void)w; return o; }
int nonseekable_open(struct inode *inode, struct file *filp);
#endif
