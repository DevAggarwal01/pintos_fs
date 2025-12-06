#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

// identifies an inode
#define INODE_MAGIC 0x494e4f44

// number of pointers to data sectors inside a single indirect block
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

// number of direct data block pointers stored in the inode itself
#define DIRECT_BLOCKS 123

// maximum number of data sectors a single inode can reference
#define MAX_ENTRIES (DIRECT_BLOCKS + INDIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES * INDIRECT_BLOCK_ENTRIES)

// zero filled sector sized buffer used when initializing blocks
static uint8_t empty_block[BLOCK_SECTOR_SIZE];

// on disk representation of an inode
// this struct must be exactly one sector in size
struct inode_disk {
    uint32_t is_directory;    // 0 for file, nonzero for directory
    off_t length;             // file size in bytes
    unsigned magic;           // magic number

    // the following fields together must consume the remaining bytes
    uint32_t direct[DIRECT_BLOCKS];   // direct data block sectors
    block_sector_t indirect;          // single indirect block sector
    block_sector_t double_indirect;   // double indirect block sector
};

// in memory representation of an inode
struct inode {
    struct list_elem elem;       // element in list of open inodes
    block_sector_t sector;       // sector number of on disk inode
    int open_cnt;                // number of openers
    bool removed;                // true if deleted but still open
    int deny_write_cnt;          // 0 allows writes, >0 denies writes
    struct inode_disk data;      // cached on disk inode contents
    struct lock extend_file_write_lock;          // lock for synchronization
};

// list of all currently open inodes
static struct list open_inodes;

// returns number of sectors needed to store a file of size bytes
static inline size_t bytes_to_sectors(off_t size) {
    return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

// converts a data block index to the corresponding sector
static bool block_index_to_sector(struct inode_disk *d, size_t index, block_sector_t *sector);

// allocates a data sector for the given logical index inside inode d
static bool allocate_sector(struct inode_disk *d, size_t index, block_sector_t *write_to_sector);

// allocates the first num_sectors logical sectors for inode d
static bool allocate_sectors(struct inode_disk *d, size_t num_sectors);

// releases all data sectors owned by inode d back to the free map
static bool deallocate_all_blocks(struct inode_disk *d);

// grows inode to be able to represent new_length bytes
static bool extend_capacity(struct inode *inode, off_t new_length);

// returns true if inode represents a directory
bool inode_is_directory(const struct inode *inode) {
    return inode != NULL && inode->data.is_directory != 0;
}

// returns current open count of inode or 0 if inode is null
int get_open_cnt(const struct inode *inode) {
    return inode != NULL ? inode->open_cnt : 0;
}

// maps a logical data block index within the inode to a data sector
static bool block_index_to_sector(struct inode_disk *d, size_t index, block_sector_t *sector) {
    // index is outside representable range
    if (index >= MAX_ENTRIES) {
        return false;
    }

    // direct blocks
    if (index < DIRECT_BLOCKS) {
        if (d->direct[index] == 0) {
            return false;
        }
        *sector = d->direct[index];
        return true;
    }

    // first level indirect block
    if (index < DIRECT_BLOCKS + INDIRECT_BLOCK_ENTRIES) {
        if (d->indirect == 0) {
            return false;
        }

        block_sector_t indirect_block[INDIRECT_BLOCK_ENTRIES];
        block_read(fs_device, d->indirect, indirect_block);

        size_t ib_index = index - DIRECT_BLOCKS;
        ASSERT(ib_index < INDIRECT_BLOCK_ENTRIES);

        if (indirect_block[ib_index] == 0) {
            return false;
        }

        *sector = indirect_block[ib_index];
        return true;
    }

    // second level double indirect
    if (d->double_indirect == 0) {
        return false;
    }

    size_t double_index = index - DIRECT_BLOCKS - INDIRECT_BLOCK_ENTRIES;
    size_t level_one_index = double_index / INDIRECT_BLOCK_ENTRIES;
    size_t level_two_index = double_index % INDIRECT_BLOCK_ENTRIES;

    ASSERT(level_one_index < INDIRECT_BLOCK_ENTRIES);
    ASSERT(level_two_index < INDIRECT_BLOCK_ENTRIES);

    block_sector_t first_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read(fs_device, d->double_indirect, first_level_indirect_block);

    if (first_level_indirect_block[level_one_index] == 0) {
        return false;
    }

    block_sector_t second_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read(fs_device, first_level_indirect_block[level_one_index], second_level_indirect_block);

    if (second_level_indirect_block[level_two_index] == 0) {
        return false;
    }

    *sector = second_level_indirect_block[level_two_index];
    return true;
}

// allocates a physical sector for the logical data block index inside inode d
static bool allocate_sector(struct inode_disk *d, size_t index, block_sector_t *write_to_sector) {
    if (index >= MAX_ENTRIES) {
        return false;
    }

    // direct blocks
    if (index < DIRECT_BLOCKS) {
        if (d->direct[index] == 0) {
            if (!free_map_allocate(1, &d->direct[index])) {
                return false;
            }
        }
        *write_to_sector = d->direct[index];
        return true;
    }

    // first level indirect
    if (index < DIRECT_BLOCKS + INDIRECT_BLOCK_ENTRIES) {
        // allocate the indirect block if needed
        if (d->indirect == 0) {
            if (!free_map_allocate(1, &d->indirect)) {
                return false;
            }
            memset(empty_block, 0, BLOCK_SECTOR_SIZE);
            block_write(fs_device, d->indirect, empty_block);
        }

        block_sector_t indirect_block[INDIRECT_BLOCK_ENTRIES];
        block_read(fs_device, d->indirect, indirect_block);

        size_t ib_index = index - DIRECT_BLOCKS;
        ASSERT(ib_index < INDIRECT_BLOCK_ENTRIES);

        if (indirect_block[ib_index] == 0) {
            if (!free_map_allocate(1, &indirect_block[ib_index])) {
                return false;
            }
            block_write(fs_device, d->indirect, indirect_block);
        }

        *write_to_sector = indirect_block[ib_index];
        return true;
    }

    // second level double indirect
    size_t double_index = index - DIRECT_BLOCKS - INDIRECT_BLOCK_ENTRIES;
    size_t level_one_index = double_index / INDIRECT_BLOCK_ENTRIES;
    size_t level_two_index = double_index % INDIRECT_BLOCK_ENTRIES;

    ASSERT(level_one_index < INDIRECT_BLOCK_ENTRIES);
    ASSERT(level_two_index < INDIRECT_BLOCK_ENTRIES);

    // allocate double indirect block itself if needed
    if (d->double_indirect == 0) {
        if (!free_map_allocate(1, &d->double_indirect)) {
            return false;
        }
        memset(empty_block, 0, BLOCK_SECTOR_SIZE);
        block_write(fs_device, d->double_indirect, empty_block);
    }

    // first level table under double indirect
    block_sector_t first_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read(fs_device, d->double_indirect, first_level_indirect_block);

    if (first_level_indirect_block[level_one_index] == 0) {
        if (!free_map_allocate(1, &first_level_indirect_block[level_one_index])) {
            return false;
        }
        memset(empty_block, 0, BLOCK_SECTOR_SIZE);
        block_write(fs_device, first_level_indirect_block[level_one_index], empty_block);
        block_write(fs_device, d->double_indirect, first_level_indirect_block);
    }

    // second level data pointer table
    block_sector_t second_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read(fs_device, first_level_indirect_block[level_one_index], second_level_indirect_block);

    if (second_level_indirect_block[level_two_index] == 0) {
        if (!free_map_allocate(1, &second_level_indirect_block[level_two_index])) {
            return false;
        }
        block_write(fs_device, first_level_indirect_block[level_one_index], second_level_indirect_block);
    }

    *write_to_sector = second_level_indirect_block[level_two_index];
    return true;
}

// allocates the first num_sectors logical data sectors of inode d
static bool allocate_sectors(struct inode_disk *d, size_t num_sectors) {
    ASSERT(num_sectors <= MAX_ENTRIES);
    for (size_t i = 0; i < num_sectors; i++) {
        block_sector_t new_sector;
        if (!allocate_sector(d, i, &new_sector)) {
            return false;
        }
    }
    return true;
}

// releases all data blocks referenced by inode d
static bool deallocate_all_blocks(struct inode_disk *d) {
    size_t total_sectors = bytes_to_sectors(d->length);

    // direct blocks
    for (size_t i = 0; i < DIRECT_BLOCKS && total_sectors > 0; i++) {
        if (d->direct[i] != 0) {
            free_map_release(d->direct[i], 1);
            d->direct[i] = 0;
        }
        total_sectors--;
    }

    // first level indirect
    if (d->indirect != 0 && total_sectors > 0) {
        block_sector_t indirect_block[INDIRECT_BLOCK_ENTRIES];
        block_read(fs_device, d->indirect, indirect_block);

        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; i++) {
            if (indirect_block[i] != 0) {
                free_map_release(indirect_block[i], 1);
                indirect_block[i] = 0;
            }
            total_sectors--;
        }

        free_map_release(d->indirect, 1);
        d->indirect = 0;
    }

    // second level double indirect
    if (d->double_indirect != 0 && total_sectors > 0) {
        block_sector_t first_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
        block_read(fs_device, d->double_indirect, first_level_indirect_block);

        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; i++) {
            if (first_level_indirect_block[i] != 0) {
                block_sector_t second_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
                block_read(fs_device, first_level_indirect_block[i], second_level_indirect_block);

                for (size_t j = 0; j < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; j++) {
                    if (second_level_indirect_block[j] != 0) {
                        free_map_release(second_level_indirect_block[j], 1);
                        second_level_indirect_block[j] = 0;
                    }
                    total_sectors--;
                }

                free_map_release(first_level_indirect_block[i], 1);
                first_level_indirect_block[i] = 0;
            }
        }

        free_map_release(d->double_indirect, 1);
        d->double_indirect = 0;
    }

    d->length = 0;
    return true;
}

// translates a byte offset in inode into the corresponding disk sector
// returns -1 if pos is outside the current file length
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
    ASSERT(inode != NULL);

    if (pos < 0 || pos >= inode->data.length) {
        return (block_sector_t) -1;
    }

    size_t index = pos / BLOCK_SECTOR_SIZE;
    block_sector_t sector;
    bool ok = block_index_to_sector((struct inode_disk *) &inode->data, index, &sector);

    // for any byte inside the file there must be a mapping
    ASSERT(ok);
    ASSERT(sector != 0);

    return sector;
}

// grows inode to at least new_length bytes
static bool extend_capacity(struct inode *inode, off_t new_length) {
    if (new_length <= inode->data.length) {
        return true;
    }

    size_t old_sectors = bytes_to_sectors(inode->data.length);
    size_t new_sectors = bytes_to_sectors(new_length);

    struct inode_disk *d = &inode->data;

    for (size_t i = old_sectors; i < new_sectors; i++) {
        block_sector_t sector;
        if (!allocate_sector(d, i, &sector)) {
            return false;
        }
        memset(empty_block, 0, BLOCK_SECTOR_SIZE);
        block_write(fs_device, sector, empty_block);
    }

    d->length = new_length;
    block_write(fs_device, inode->sector, d);
    return true;
}

// list of open inodes is initialized here
void inode_init(void) {
    list_init(&open_inodes);
}

// creates a new inode on disk at sector with given length and directory flag
bool inode_create(block_sector_t sector, off_t length, bool is_directory) {
    struct inode_disk *disk_inode = NULL;
    bool success = false;

    ASSERT(length >= 0);
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode != NULL) {
        size_t sectors = bytes_to_sectors(length);
        disk_inode->length = length;
        disk_inode->magic = INODE_MAGIC;
        disk_inode->is_directory = is_directory ? 1u : 0u;

        // allocate all data sectors up front
        if (allocate_sectors(disk_inode, sectors)) {
            // zero out each allocated data block
            for (size_t i = 0; i < sectors; i++) {
                block_sector_t data_sector;
                bool ok = block_index_to_sector(disk_inode, i, &data_sector);
                ASSERT(ok);
                memset(empty_block, 0, BLOCK_SECTOR_SIZE);
                block_write(fs_device, data_sector, empty_block);
            }

            // write the inode metadata to disk
            block_write(fs_device, sector, disk_inode);
            success = true;
        }

        free(disk_inode);
    }
    return success;
}

// opens the inode stored in the given sector
struct inode *inode_open(block_sector_t sector) {
    struct list_elem *e;
    struct inode *inode;

    // return existing in memory inode if already open
    for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
        inode = list_entry(e, struct inode, elem);
        if (inode->sector == sector) {
            inode->open_cnt++;
            return inode;
        }
    }

    // allocate and initialize new inode
    inode = malloc(sizeof *inode);
    if (inode == NULL) {
        return NULL;
    }

    inode->sector = sector;
    inode->open_cnt = 1;
    inode->removed = false;
    inode->deny_write_cnt = 0;
    lock_init(&inode->extend_file_write_lock);

    list_push_front(&open_inodes, &inode->elem);
    block_read(fs_device, inode->sector, &inode->data);

    ASSERT(inode->data.magic == INODE_MAGIC);
    return inode;
}

// reopens an already open inode
struct inode *inode_reopen(struct inode *inode) {
    if (inode == NULL) {
        return NULL;
    }
    inode->open_cnt++;
    return inode;
}

// closes inode and frees memory resources if this is the last opener
void inode_close(struct inode *inode) {
    if (inode == NULL) {
        return;
    }

    ASSERT(inode->open_cnt > 0);
    inode->open_cnt--;
    if (inode->open_cnt > 0) {
        return;
    }

    // remove from global open list
    list_remove(&inode->elem);

    // if removed flag is set then free all data blocks and inode sector
    if (inode->removed) {
        deallocate_all_blocks(&inode->data);
        free_map_release(inode->sector, 1);
    }

    free(inode);
}

// marks inode as removed so that blocks are freed when last opener closes it
void inode_remove(struct inode *inode) {
    ASSERT(inode != NULL);
    inode->removed = true;
}

// reads up to size bytes starting at offset from inode into buffer
// returns number of bytes actually read
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset) {
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;
    uint8_t *bounce = NULL;

    // allow concurrent readers and non extending writers
    off_t end_offset = offset + size;
    bool inode_len = inode_length(inode);
    bool touches_end = size > 0 && offset <= inode_len && end_offset > inode_len;

    if(touches_end) {
      lock_acquire(&inode->extend_file_write_lock);
    }

    while (size > 0) {
        // stop if offset is already past end of file
        if (offset >= inode_length(inode)) {
            break;
        }

        block_sector_t sector_idx = byte_to_sector(inode, offset);
        if (sector_idx == (block_sector_t) -1) {
            break;
        }

        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0) {
            break;
        }

        if (bounce == NULL) {
            bounce = malloc(BLOCK_SECTOR_SIZE);
            if (bounce == NULL) {
                break;
            }
        }

        // always read into bounce buffer then copy
        block_read(fs_device, sector_idx, bounce);
        memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);

        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
    }

    free(bounce);
    
    if(touches_end) {
      lock_release(&inode->extend_file_write_lock);
    }

    return bytes_read;
}


// writes up to size bytes from buffer into inode starting at offset
// returns number of bytes actually written
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset) {
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;
    uint8_t *bounce = NULL;

    if (inode->deny_write_cnt) {
        return 0;
    }

    off_t end_offset = offset + size;
    bool is_extending_write = end_offset > inode->data.length;

    bool writes_to_eof = !is_extending_write && end_offset == inode->data.length && size > 0 && offset < inode->data.length;

    bool need_lock = is_extending_write || writes_to_eof;

    if(need_lock) {
        lock_acquire(&inode->extend_file_write_lock);
    }

    // writers that extend the file take exclusive monitor access
    if (is_extending_write) {
        if (!extend_capacity(inode, end_offset)) {
            if(need_lock) {
                lock_release(&inode->extend_file_write_lock);
            }
            return 0;
        }
    }

    while (size > 0) {
        // stop if offset has somehow moved beyond length
        if (offset >= inode_length(inode)) {
            break;
        }

        block_sector_t sector_idx = byte_to_sector(inode, offset);
        if (sector_idx == (block_sector_t) -1) {
            break;
        }

        int sector_ofs = offset % BLOCK_SECTOR_SIZE;

        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;

        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0) {
            break;
        }

        if (bounce == NULL) {
            bounce = malloc(BLOCK_SECTOR_SIZE);
            if (bounce == NULL) {
                break;
            }
        }

        // read existing sector into bounce
        block_read(fs_device, sector_idx, bounce);
        // overlay the new bytes
        memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
        // write the sector back to disk
        block_write(fs_device, sector_idx, bounce);

        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }

    free(bounce);
    if (need_lock) {
        lock_release(&inode->extend_file_write_lock);
    }

    return bytes_written;
}


// disables writes to inode
// may be called at most once per inode opener
void inode_deny_write(struct inode *inode) {
    inode->deny_write_cnt++;
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

// re enables writes to inode
// must be called once by each opener that called inode_deny_write
void inode_allow_write(struct inode *inode) {
    ASSERT(inode->deny_write_cnt > 0);
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    inode->deny_write_cnt--;
}

// returns the current length of inode in bytes
off_t inode_length(const struct inode *inode) {
    return inode->data.length;
}

// returns the on disk sector number of the inode
block_sector_t inode_get_inumber(const struct inode *inode) {
    return inode->sector;
}


