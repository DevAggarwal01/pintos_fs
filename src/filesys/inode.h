#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h"

struct bitmap;
struct inode;

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
int inode_get_open_count(const struct inode *inode);

#endif // filesys/inode_h
