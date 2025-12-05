#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"

struct bitmap;

// monitor for synchronization
struct monitor {
  struct lock lock;
  struct condition cond;
  int readers;
  bool writer;      // this bool is only for file extending writes
};

void inode_init (void);
bool inode_create (block_sector_t, off_t, bool is_directory);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_is_directory (const struct inode *);
bool get_open_cnt(const struct inode *inode);

void monitor_init(struct monitor *mon);
void monitor_read_enter(struct monitor *mon);
void monitor_read_exit(struct monitor *mon);
void monitor_write_enter(struct monitor *mon);
void monitor_write_exit(struct monitor *mon);
#endif /* filesys/inode.h */
