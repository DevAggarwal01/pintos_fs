#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"

struct bitmap;
struct inode;

// monitor for synchronization
struct monitor {
    struct lock lock;
    struct condition cond;
    int readers;
    bool writer;      // this bool is only for file extending writes
};

// initializes the inode subsystem
void inode_init(void);

// creates an inode on disk at sector with given length and directory flag
bool inode_create(block_sector_t sector, off_t length, bool is_directory);

// opens the inode stored in the given sector
struct inode *inode_open(block_sector_t sector);

// reopens an already open inode
struct inode *inode_reopen(struct inode *inode);

// returns the on disk sector number of the inode
block_sector_t inode_get_inumber(const struct inode *inode);

// closes inode and frees it when the last opener closes it
void inode_close(struct inode *inode);

// marks inode as removed so its blocks are freed when last opener closes it
void inode_remove(struct inode *inode);

// reads up to size bytes starting at offset from inode into buffer
off_t inode_read_at(struct inode *inode, void *buffer, off_t size, off_t offset);

// writes up to size bytes from buffer into inode starting at offset
off_t inode_write_at(struct inode *inode, const void *buffer, off_t size, off_t offset);

// disables writes to inode for this opener
void inode_deny_write(struct inode *inode);

// re enables writes to inode for this opener
void inode_allow_write(struct inode *inode);

// returns current length of inode in bytes
off_t inode_length(const struct inode *inode);

// returns true if inode represents a directory
bool inode_is_directory(const struct inode *inode);

// returns the current open count of inode or 0 if inode is null
int get_open_cnt(const struct inode *inode);

// monitor initialization
void monitor_init(struct monitor *mon);

// acquires shared read access on the monitor
void monitor_read_enter(struct monitor *mon);

// releases shared read access on the monitor
void monitor_read_exit(struct monitor *mon);

// acquires exclusive write access on the monitor
void monitor_write_enter(struct monitor *mon);

// releases exclusive write access on the monitor
void monitor_write_exit(struct monitor *mon);

#endif // filesys/inode_h
