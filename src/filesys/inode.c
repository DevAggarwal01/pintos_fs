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
struct inode_disk {
    uint32_t is_directory;            // 0 for file, nonzero for directory
    off_t length;                     // file size in bytes
    unsigned magic;                   // magic number
    uint32_t direct[DIRECT_BLOCKS];   // direct data block sectors
    block_sector_t indirect;          // single indirect block sector
    block_sector_t double_indirect;   // double indirect block sector
};

// in memory representation of an inode
struct inode {
    struct list_elem elem;                    // element in list of open inodes
    block_sector_t sector;                    // sector number of on disk inode
    int open_cnt;                             // number of openers
    bool removed;                             // true if deleted but still open
    int deny_write_cnt;                       // 0 allows writes, >0 denies writes
    struct inode_disk data;                   // cached on disk inode contents
    struct lock extending_file_lock;          // lock for synchronizing access to inode
};

// list of all currently open inodes
static struct list open_inodes;


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


/**
 * Converts a size in bytes to the number of sectors required to store that many bytes.
 */
static inline size_t bytes_to_sectors(off_t size) {
    return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}


/**
 * Initialize the inode subsystem.
 */
void inode_init(void) {
    list_init(&open_inodes);
}


/**
 * Returns whether the inode represents a directory.
 */
bool inode_is_directory(const struct inode *inode) {
    return inode != NULL && inode->data.is_directory != 0;
}


/**
 * Returns the number of times the inode has been opened.
 */
int inode_get_open_count(const struct inode *inode) {
    return inode != NULL ? inode->open_cnt : 0;
}


/**
 * Translates a logical data block index inside inode disk
 * structure to the corresponding physical disk sector.
 */
static bool block_index_to_sector(struct inode_disk *d, size_t index, block_sector_t *sector) {
    // index is outside representable range
    if (index >= MAX_ENTRIES) {
        return false;
    }
    // for direct blocks
    if (index < DIRECT_BLOCKS) {
        if (d->direct[index] == 0) {
            return false;
        }
        *sector = d->direct[index];
        return true;
    }
    // for first level indirect blocks
    if (index < DIRECT_BLOCKS + INDIRECT_BLOCK_ENTRIES) {
        // indirect block not allocated
        if (d->indirect == 0) {
            return false;
        }
        // read the indirect block
        block_sector_t *indirect_block = malloc(BLOCK_SECTOR_SIZE);
        if (indirect_block == NULL) {
            return false;
        }
        block_read(fs_device, d->indirect, indirect_block);
        // get the sector from the indirect block
        size_t ib_index = index - DIRECT_BLOCKS;
        ASSERT(ib_index < INDIRECT_BLOCK_ENTRIES);
        // indirect block entry not allocated
        if (indirect_block[ib_index] == 0) {
            free(indirect_block);
            return false;
        }
        // return the sector
        *sector = indirect_block[ib_index];
        free(indirect_block);
        return true;
    }
    // for second level indirect blocks
    if (d->double_indirect == 0) {
        return false;
    }
    // calculate indices
    size_t double_index = index - DIRECT_BLOCKS - INDIRECT_BLOCK_ENTRIES;
    size_t level_one_index = double_index / INDIRECT_BLOCK_ENTRIES;
    size_t level_two_index = double_index % INDIRECT_BLOCK_ENTRIES;
    // validate indices
    ASSERT(level_one_index < INDIRECT_BLOCK_ENTRIES);
    ASSERT(level_two_index < INDIRECT_BLOCK_ENTRIES);
    // read first level indirect block
    block_sector_t *first_level_indirect_block = malloc(BLOCK_SECTOR_SIZE);
    if (first_level_indirect_block == NULL) {
        return false;
    }
    block_read(fs_device, d->double_indirect, first_level_indirect_block);
    // first level indirect block entry not allocated
    if (first_level_indirect_block[level_one_index] == 0) {
        free(first_level_indirect_block);
        return false;
    }
    // read second level indirect block
    block_sector_t *second_level_indirect_block = malloc(BLOCK_SECTOR_SIZE);
    if (second_level_indirect_block == NULL) {
        free(first_level_indirect_block);
        return false;
    }
    block_read(fs_device, first_level_indirect_block[level_one_index], second_level_indirect_block);
    // second level indirect block entry not allocated
    if (second_level_indirect_block[level_two_index] == 0) {
        free(first_level_indirect_block);
        free(second_level_indirect_block);
        return false;
    }
    // return the sector
    *sector = second_level_indirect_block[level_two_index];
    free(first_level_indirect_block);
    free(second_level_indirect_block);
    return true;
}


/**
 * Allocates a data sector for the given logical index inside inode.
 */
static bool allocate_sector(struct inode_disk *d, size_t index, block_sector_t *write_to_sector) {
    // index is outside representable range
    if (index >= MAX_ENTRIES) {
        return false;
    }
    // for direct blocks
    if (index < DIRECT_BLOCKS) {
        if (d->direct[index] == 0) {
            if (!free_map_allocate(1, &d->direct[index])) {
                return false;
            }
        }
        *write_to_sector = d->direct[index];
        return true;
    }
    // for first level indirect blocks
    if (index < DIRECT_BLOCKS + INDIRECT_BLOCK_ENTRIES) {
        // allocate the indirect block if needed
        if (d->indirect == 0) {
            if (!free_map_allocate(1, &d->indirect)) {
                return false;
            }
            memset(empty_block, 0, BLOCK_SECTOR_SIZE);
            block_write(fs_device, d->indirect, empty_block);
        }
        // read the indirect block
        block_sector_t *indirect_block = malloc(BLOCK_SECTOR_SIZE);
        if (indirect_block == NULL) {
            return false;
        }
        block_read(fs_device, d->indirect, indirect_block);
        // assess the entry
        size_t ib_index = index - DIRECT_BLOCKS;
        ASSERT(ib_index < INDIRECT_BLOCK_ENTRIES);
        // allocate data block if needed
        if (indirect_block[ib_index] == 0) {
            if (!free_map_allocate(1, &indirect_block[ib_index])) {
                free(indirect_block);
                return false;
            }
            block_write(fs_device, d->indirect, indirect_block);
        }
        // return the sector
        *write_to_sector = indirect_block[ib_index];
        free(indirect_block);
        return true;
    }
    // for second level indirect blocks
    size_t double_index = index - DIRECT_BLOCKS - INDIRECT_BLOCK_ENTRIES;
    size_t level_one_index = double_index / INDIRECT_BLOCK_ENTRIES;
    size_t level_two_index = double_index % INDIRECT_BLOCK_ENTRIES;
    // validate indices
    ASSERT(level_one_index < INDIRECT_BLOCK_ENTRIES);
    ASSERT(level_two_index < INDIRECT_BLOCK_ENTRIES);
    // allocate double indirect block if needed
    if (d->double_indirect == 0) {
        if (!free_map_allocate(1, &d->double_indirect)) {
            return false;
        }
        memset(empty_block, 0, BLOCK_SECTOR_SIZE);
        block_write(fs_device, d->double_indirect, empty_block);
    }
    // read first level indirect block
    block_sector_t *first_level_indirect_block = malloc(BLOCK_SECTOR_SIZE);
    if (first_level_indirect_block == NULL) {
        return false;
    }
    block_read(fs_device, d->double_indirect, first_level_indirect_block);
    // allocate first level indirect block entry if needed
    if (first_level_indirect_block[level_one_index] == 0) {
        if (!free_map_allocate(1, &first_level_indirect_block[level_one_index])) {
            free(first_level_indirect_block);
            return false;
        }
        memset(empty_block, 0, BLOCK_SECTOR_SIZE);
        block_write(fs_device, first_level_indirect_block[level_one_index], empty_block);
        block_write(fs_device, d->double_indirect, first_level_indirect_block);
    }
    // read second level indirect block
    block_sector_t *second_level_indirect_block = malloc(BLOCK_SECTOR_SIZE);
    if (second_level_indirect_block == NULL) {
        free(first_level_indirect_block);
        return false;
    }
    block_read(fs_device, first_level_indirect_block[level_one_index], second_level_indirect_block);
    // allocate second level indirect block entry if needed
    if (second_level_indirect_block[level_two_index] == 0) {
        if (!free_map_allocate(1, &second_level_indirect_block[level_two_index])) {
            free(first_level_indirect_block);
            free(second_level_indirect_block);
            return false;
        }
        block_write(fs_device, first_level_indirect_block[level_one_index], second_level_indirect_block);
    }
    // return the sector
    *write_to_sector = second_level_indirect_block[level_two_index];
    free(first_level_indirect_block);
    free(second_level_indirect_block);
    return true;
}


/**
 * Allocates the first num_sectors logical sectors for inode.
 */
static bool allocate_sectors(struct inode_disk *d, size_t num_sectors) {
    // make sure num_sectors does not exceed maximum representable
    ASSERT(num_sectors <= MAX_ENTRIES);
    // allocate each sector
    for (size_t i = 0; i < num_sectors; i++) {
        block_sector_t new_sector;
        if (!allocate_sector(d, i, &new_sector)) {
            // allocation failed
            return false;
        }
    }
    // all sectors allocated successfully
    return true;
}


/**
 * Releases all data sectors owned by inode back to the free map.
 */
static bool deallocate_all_blocks(struct inode_disk *d) {
    // calculate total number of sectors to release
    size_t total_sectors = bytes_to_sectors(d->length);
    // release direct blocks
    for (size_t i = 0; i < DIRECT_BLOCKS && total_sectors > 0; i++) {
        if (d->direct[i] != 0) {
            free_map_release(d->direct[i], 1);
            d->direct[i] = 0;
        }
        total_sectors--;
    }
    // release single indirect blocks
    if (d->indirect != 0 && total_sectors > 0) {
        // read the indirect block
        block_sector_t *indirect_block = malloc(BLOCK_SECTOR_SIZE);
        if (indirect_block == NULL) {
            return false;
        }
        block_read(fs_device, d->indirect, indirect_block);
        // release each data block referenced by the indirect block
        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; i++) {
            if (indirect_block[i] != 0) {
                free_map_release(indirect_block[i], 1);
                indirect_block[i] = 0;
            }
            total_sectors--;
        }
        // release the indirect block itself
        free_map_release(d->indirect, 1);
        d->indirect = 0;
        free(indirect_block);
    }
    // release second level indirect blocks
    if (d->double_indirect != 0 && total_sectors > 0) {
        // read first level indirect block
        block_sector_t *first_level_indirect_block = malloc(BLOCK_SECTOR_SIZE);
        if (first_level_indirect_block == NULL) {
            return false;
        }
        block_read(fs_device, d->double_indirect, first_level_indirect_block);
        // for each first level entry
        for (size_t i = 0; i < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; i++) {
            // if first level entry is allocated
            if (first_level_indirect_block[i] != 0) {
                // read second level indirect block
                block_sector_t *second_level_indirect_block = malloc(BLOCK_SECTOR_SIZE);
                if (second_level_indirect_block == NULL) {
                    free(first_level_indirect_block);
                    return false;
                }
                block_read(fs_device, first_level_indirect_block[i], second_level_indirect_block);
                // release each data block referenced by the second level indirect block
                for (size_t j = 0; j < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; j++) {
                    if (second_level_indirect_block[j] != 0) {
                        free_map_release(second_level_indirect_block[j], 1);
                        second_level_indirect_block[j] = 0;
                    }
                    total_sectors--;
                }
                // release the second level indirect block itself
                free_map_release(first_level_indirect_block[i], 1);
                first_level_indirect_block[i] = 0;
                free(second_level_indirect_block);
            }
        }
        // release the double indirect block itself
        free_map_release(d->double_indirect, 1);
        d->double_indirect = 0;
        free(first_level_indirect_block);
    }
    // all blocks released
    d->length = 0;
    return true;
}


/**
 * Converts a byte offset into the corresponding disk sector.
 */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
    ASSERT(inode != NULL);
    // check if pos is within file length
    if (pos < 0 || pos >= inode->data.length) {
        return (block_sector_t) -1;
    }
    // calculate the data block index
    size_t index = pos / BLOCK_SECTOR_SIZE;
    block_sector_t sector;
    bool ok = block_index_to_sector((struct inode_disk *) &inode->data, index, &sector);
    // assert that sector retrieval was successful
    ASSERT(ok);
    ASSERT(sector != 0);
    // return the sector number
    return sector;
}


/**
 * Grows inode to be able to represent new_length bytes.
 */
static bool extend_capacity(struct inode *inode, off_t new_length) {
    // no need to extend
    if (new_length <= inode->data.length) {
        return true;
    }
    // allocate new sectors
    size_t old_sectors = bytes_to_sectors(inode->data.length);
    size_t new_sectors = bytes_to_sectors(new_length);
    struct inode_disk *d = &inode->data;
    // allocate each new sector
    for (size_t i = old_sectors; i < new_sectors; i++) {
        block_sector_t sector;
        if (!allocate_sector(d, i, &sector)) {
            return false;
        }
        memset(empty_block, 0, BLOCK_SECTOR_SIZE);
        block_write(fs_device, sector, empty_block);
    }
    // update inode length and write back to disk
    d->length = new_length;
    block_write(fs_device, inode->sector, d);
    return true;
}


/**
 * Creates an inode on disk at sector with given length and directory flag.
 */
bool inode_create(block_sector_t sector, off_t length, bool is_directory) {
    struct inode_disk *disk_inode = NULL;
    bool success = false;
    ASSERT(length >= 0);
    ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);
    // allocate inode structure
    disk_inode = calloc(1, sizeof *disk_inode);
    if (disk_inode != NULL) {
        // initialize inode metadata
        size_t sectors = bytes_to_sectors(length);
        disk_inode->length = length;
        disk_inode->magic = INODE_MAGIC;
        disk_inode->is_directory = is_directory ? 1u : 0u;
        // allocate all required data sectors
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
        // free inode structure
        free(disk_inode);
    }
    // return whether inode creation succeeded
    return success;
}


/**
 * Opens the inode stored in the given sector.
 */
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
    // allocate and initialize new inode if not already open
    inode = malloc(sizeof *inode);
    if (inode == NULL) {
        return NULL;
    }
    // initialize inode fields
    inode->sector = sector;
    inode->open_cnt = 1;
    inode->removed = false;
    inode->deny_write_cnt = 0;
    lock_init(&inode->extending_file_lock);
    // add to list of open inodes and read from disk
    list_push_front(&open_inodes, &inode->elem);
    block_read(fs_device, inode->sector, &inode->data);
    // validate inode magic number
    ASSERT(inode->data.magic == INODE_MAGIC);
    return inode;
}


/**
 * Reopens an already open inode.
 */
struct inode *inode_reopen(struct inode *inode) {
    if (inode == NULL) {
        return NULL;
    }
    inode->open_cnt++;
    return inode;
}


/**
 * Closes inode and frees it when the last opener closes it.
 */
void inode_close(struct inode *inode) {
    // ignore null inode
    if (inode == NULL) {
        return;
    }
    // decrement open count; return if still open
    ASSERT(inode->open_cnt > 0);
    inode->open_cnt--;
    if (inode->open_cnt > 0) {
        return;
    }
    // final closer; remove inode from memory
    // remove from list of open inodes
    list_remove(&inode->elem);
    // if removed flag is set then free all data blocks and inode sector
    if (inode->removed) {
        deallocate_all_blocks(&inode->data);
        free_map_release(inode->sector, 1);
    }
    // free inode structure
    free(inode);
}


/**
 * Marks inode as removed so its blocks are freed when last opener closes it.
 */
void inode_remove(struct inode *inode) {
    // mark inode as removed
    ASSERT(inode != NULL);
    inode->removed = true;
}


/**
 * Reads up to size bytes starting at offset from inode into buffer.
 */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset) {
    // cast buffer to uint8_t pointer for byte-wise operations
    uint8_t *buffer = buffer_;
    off_t bytes_read = 0;
    uint8_t *bounce = NULL;
    // allow concurrent readers and non extending writers
    off_t length = inode_length(inode);
    off_t end_offset = offset + size;
    // check if starts at or before EOF, and ends beyond EOF
    bool touches_eof = size > 0 && offset <= length && end_offset > length;
    if (touches_eof) {
        lock_acquire(&inode->extending_file_lock);
    }
    // read bytes until size is 0 or EOF is reached
    while (size > 0) {
        // stop if offset is already past end of file
        if (offset >= inode_length(inode)) {
            break;
        }
        // get the sector corresponding to the current offset
        block_sector_t sector_idx = byte_to_sector(inode, offset);
        if (sector_idx == (block_sector_t) -1) {
            break;
        }
        // calculate sector offset within the block
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;
        // calculate remaining bytes in inode and sector
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;
        // determine chunk size to read
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0) {
            break;
        }
        // allocate bounce buffer if needed
        if (bounce == NULL) {
            bounce = malloc(BLOCK_SECTOR_SIZE);
            if (bounce == NULL) {
                break;
            }
        }
        // always read into bounce buffer then copy
        block_read(fs_device, sector_idx, bounce);
        memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
        // update counters and offsets
        size -= chunk_size;
        offset += chunk_size;
        bytes_read += chunk_size;
    }
    // free bounce buffer
    free(bounce);
    if (touches_eof) {
        lock_release(&inode->extending_file_lock);
    }
    return bytes_read;
}


/**
 * Writes up to size bytes from buffer into inode starting at offset.
 */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset) {
    // cast buffer to uint8_t pointer for byte-wise operations
    const uint8_t *buffer = buffer_;
    off_t bytes_written = 0;
    uint8_t *bounce = NULL;
    // deny write if inode is write denied
    if (inode->deny_write_cnt) {
        return 0;
    }
    // check if write extends the file and acquire lock if needed
    off_t length = inode_length(inode);
    off_t end_offset = offset + size;
    bool is_extending_write = end_offset > length;
    bool write_ends_at_or_beyond_eof = is_extending_write && offset <= length;
    // writers that extend the file need exclusive access to inode
    if (write_ends_at_or_beyond_eof) {
        lock_acquire(&inode->extending_file_lock);
    }
    if (is_extending_write) {
        // extend the inode capacity
        if (!extend_capacity(inode, end_offset)) {
            if (write_ends_at_or_beyond_eof) {
                lock_release(&inode->extending_file_lock);
            }
            return 0;
        }
    }
    // write bytes until size is 0 or EOF is reached
    while (size > 0) {
        // stop if offset has somehow moved beyond length
        if (offset >= inode_length(inode)) {
            break;
        }
        // get the sector corresponding to the current offset
        block_sector_t sector_idx = byte_to_sector(inode, offset);
        if (sector_idx == (block_sector_t) -1) {
            break;
        }
        // calculate sector offset within the block
        int sector_ofs = offset % BLOCK_SECTOR_SIZE;
        // calculate remaining bytes in inode and sector
        off_t inode_left = inode_length(inode) - offset;
        int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
        int min_left = inode_left < sector_left ? inode_left : sector_left;
        // determine chunk size to write
        int chunk_size = size < min_left ? size : min_left;
        if (chunk_size <= 0) {
            break;
        }
        // allocate bounce buffer if needed
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
        // update counters and offsets
        size -= chunk_size;
        offset += chunk_size;
        bytes_written += chunk_size;
    }
    // free bounce buffer and release lock if acquired
    free(bounce);
    if (write_ends_at_or_beyond_eof) {
        lock_release(&inode->extending_file_lock);
    }
    return bytes_written;
}


/**
 * Disables writes to inode for this opener.
 * Must be called once by each opener to deny writes.
 */
void inode_deny_write(struct inode *inode) {
    inode->deny_write_cnt++;
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}


/**
 * Re-enables writes to inode for this opener.
 * Must be called once by each opener to re-enable writes.
 */
void inode_allow_write(struct inode *inode) {
    ASSERT(inode->deny_write_cnt > 0);
    ASSERT(inode->deny_write_cnt <= inode->open_cnt);
    inode->deny_write_cnt--;
}


/**
 * Returns current length of inode in bytes.
 */
off_t inode_length(const struct inode *inode) {
    return inode->data.length;
}


/**
 * Returns the on disk sector number of the inode.
 */
block_sector_t inode_get_inumber(const struct inode *inode) {
    return inode->sector;
}