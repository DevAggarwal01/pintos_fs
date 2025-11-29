#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// number of pointers to direct blocks in 1 indirect block
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

// number of sectors maximum a file can have
#define MAX_ENTRIES (124 + INDIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES * INDIRECT_BLOCK_ENTRIES)

// used as a buffer to initialize indirect blocks with zeros
static block_sector_t empty_block[INDIRECT_BLOCK_ENTRIES];

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  // block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */

  // THE FOLLOWING BLOCKS MUST TOTAL 508 BYTES = 126 blocks OF 4 BYTES EACH
  uint32_t direct[124];       /* Direct blocks */
  block_sector_t indirect;    /* Indirect block */
  block_sector_t double_indirect; /* Double indirect block */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

/**
 * converts a data block index (0-based) into a block sector_t
 */
static bool block_index_to_sector (struct inode_disk *d, size_t index, block_sector_t *sector) {
  
  // out of bounds
  if (index >= MAX_ENTRIES || index < 0) {
    return false;
  }

  // direct blocks
  if(index < 124) {
    *sector = d->direct[index];
    return true;
  }

  // 1st level indirect block
  if(index < 124 + INDIRECT_BLOCK_ENTRIES) {
    if (d->indirect == 0) {
      return false;
    }
    // reading the indirect block
    block_sector_t indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read (fs_device, d->indirect, &indirect_block);
    // check if sector is allocated
    if(indirect_block[index - 124] == 0) {
      return false;
    }
    *sector = indirect_block[index - 124];
    return true;
  }

  // 2nd level indirect block is an array of indirect blocks
  if(d->double_indirect == 0) {
    return false;
  }
  // calculating indices
  size_t double_index = index - 124 - INDIRECT_BLOCK_ENTRIES;
  size_t level_one_index = double_index / INDIRECT_BLOCK_ENTRIES;
  size_t level_two_index = double_index % INDIRECT_BLOCK_ENTRIES;

  // reading the double indirect block

  block_sector_t first_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
  block_read(fs_device, d->double_indirect, &first_level_indirect_block);
  
  // checking if level one indirect block is allocated
  if (first_level_indirect_block[level_one_index] == 0) {
    return false;
  }

  block_sector_t second_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
  block_read(fs_device, first_level_indirect_block[level_one_index], &second_level_indirect_block);

  // checking if level two indirect block is allocated
  if (second_level_indirect_block[level_two_index] == 0) {
    return false;
  }

  *sector = second_level_indirect_block[level_two_index];
  return true;
}
// allocate the sector if needed at index for inode_disk d
static bool allocate_sector(struct inode_disk *d, size_t index, block_sector_t *write_to_sector) {
  if(index >= MAX_ENTRIES || index < 0) {
    return false; // out of bounds
  }

  // direct blocks
  if(index < 124) {
    // direct block not allocated
    if (d->direct[index] == 0) {
      // allocate the block
      if (!free_map_allocate(1, &d->direct[index])) {
        // allocation failed
        return false;
      }
      
    }
    *write_to_sector = d->direct[index];
    return true;
  }

  // 1st level indirect block
  if(index < 124 + INDIRECT_BLOCK_ENTRIES) {
    // indirect block not allocated
    if (d->indirect == 0) {
      if (!free_map_allocate(1, &d->indirect)) {
        return false;
      }
      // zero out the entire block
      block_write(fs_device, d->indirect, &empty_block);
    }

    // reading the indirect block
    block_sector_t indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read (fs_device, d->indirect, &indirect_block);

    // sector in indirect block not allocated
    if (indirect_block[index - 124] == 0) {
      // allocate data block if sector not allocated
      if (!free_map_allocate(1, &indirect_block[index - 124])) {
        return false;
      }

      // update indirect block
      block_write(fs_device, d->indirect, &indirect_block);
    }
    *write_to_sector = indirect_block[index - 124];
    return true;
  }

  // 2nd level indirect block

  // ensure double indirect block exists
  if(d->double_indirect == 0) {
    if (!free_map_allocate(1, &d->double_indirect)) {
      return false;
    }
    // initialize double indirect block with zeros
    block_write(fs_device, d->double_indirect, &empty_block);
  }

  // calculating indices
  size_t double_index = index - 124 - INDIRECT_BLOCK_ENTRIES;
  size_t level_one_index = double_index / INDIRECT_BLOCK_ENTRIES;
  size_t level_two_index = double_index % INDIRECT_BLOCK_ENTRIES;

  // read the double indirect block
  block_sector_t first_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
  // d->double_indirect is guaranteed to have been allocated
  block_read(fs_device, d->double_indirect, &first_level_indirect_block);

  // ensure 1st level indirect block exists
  if (first_level_indirect_block[level_one_index] == 0) {
    if (!free_map_allocate(1, &first_level_indirect_block[level_one_index])) {
      return false;
    }
    // initialize new block with zeros
    block_write(fs_device, first_level_indirect_block[level_one_index], &empty_block);
    // update the d->double_indirect block
    block_write(fs_device, d->double_indirect, &first_level_indirect_block);
  }

  // read the 2nd level indirect block
  block_sector_t second_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
  block_read(fs_device, first_level_indirect_block[level_one_index], &second_level_indirect_block);

  // allocate the data block if the 2nd level indirect block entry is not allocated
  if (second_level_indirect_block[level_two_index] == 0) {
    if (!free_map_allocate(1, &second_level_indirect_block[level_two_index])) {
      return false;
    }
    // update the 2nd level indirect block
    block_write(fs_device, first_level_indirect_block[level_one_index], &second_level_indirect_block);
  }
  *write_to_sector = second_level_indirect_block[level_two_index];
  return true;
}

// allocates data block sectors for inode_disk d
static bool allocate_sectors(struct inode_disk *d, size_t num_sectors) {
  for(int i = 0; i < num_sectors; i++) {
    block_sector_t new_sector;
    if (!allocate_sector(d, i, &new_sector)) {
      return false;
    }
  }
  return true;
}

static bool deallocate_all_blocks(struct inode_disk *d) {
  size_t total_sectors = bytes_to_sectors(d->length);

  // deallocate direct blocks
  for (int i = 0; i < 124 && total_sectors > 0; i++) {
    if (d->direct[i] != 0) {
      free_map_release(d->direct[i], 1);
      d->direct[i] = 0;
    }
    total_sectors--;
  }

  // deallocate indirect block
  if (d->indirect != 0 && total_sectors > 0) {
    block_sector_t indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read(fs_device, d->indirect, &indirect_block);

    for (int i = 0; i < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; i++) {
      // each element is a data block sector
      if (indirect_block[i] != 0) {
        free_map_release(indirect_block[i], 1);
        indirect_block[i] = 0;
      }
      total_sectors--;
    }
    // deallocate the indirect block containing the pointers
    free_map_release(d->indirect, 1);
    d->indirect = 0;
  }

  // deallocate double indirect blocks
  if (d->double_indirect != 0 && total_sectors > 0) {
    // read the double indirect block
    block_sector_t first_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
    block_read(fs_device, d->double_indirect, &first_level_indirect_block);

    // deallocate all the nested indirect blocks (2nd level indirect blocks)
    for (int i = 0; i < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; i++) {
      if (first_level_indirect_block[i] != 0) {
        block_sector_t second_level_indirect_block[INDIRECT_BLOCK_ENTRIES];
        block_read(fs_device, first_level_indirect_block[i], &second_level_indirect_block);

        // deallocate the nested data blocks inside the 2nd level indirect block
        for (int j = 0; j < INDIRECT_BLOCK_ENTRIES && total_sectors > 0; j++) {
          if (second_level_indirect_block[j] != 0) {
            // deallocate data blocks
            free_map_release(second_level_indirect_block[j], 1);
            second_level_indirect_block[j] = 0;
          }
          total_sectors--;
        }
        // deallocate the 2nd level indirect block
        free_map_release(first_level_indirect_block[i], 1);
        first_level_indirect_block[i] = 0;
      }
    }
    // deallocate the double indirect block
    free_map_release(d->double_indirect, 1);
    d->double_indirect = 0;

  }
  d->length = 0;
  block_write(fs_device, d->sector, d);
  return true;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length && pos >= 0) {
    size_t index = pos / BLOCK_SECTOR_SIZE;
    block_sector_t sector;
    if(!block_index_to_sector(&inode->data, index, &sector)) {
      return -1;
    }
    return sector;
  }
  return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init (void) { list_init (&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if( allocate_sectors(disk_inode, sectors)) {
        // zero all the data blocks to remove garbage values
        for (int i = 0; i < sectors; i++) {
          block_sector_t sector;
          if (!block_index_to_sector(disk_inode, i, &sector)) {
            free(disk_inode);
            return false;
          }
          // update block to zeros
          block_write (fs_device, sector, empty_block);
        }

        block_write (fs_device, sector, disk_inode);
        success = true;
      }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk. (Does it?  Check code.)
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          deallocate_all_blocks(&inode->data);
          free_map_release (inode->sector, 1);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at (struct inode *inode, void *buffer_, off_t size,
                     off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

static bool extend_capacity(struct inode *inode, off_t length) {
  if(length < inode->data.length) {
    // no need to extend
    return true;
  }

  size_t old_sectors = bytes_to_sectors(inode->data.length);
  size_t new_sectors = bytes_to_sectors(length);

  struct inode_disk *d = &inode->data;

  // allocate new sectors
  for (int i = old_sectors; i < new_sectors; i++) {
    block_sector_t sector;
    if (!allocate_sector(d, i, &sector)) {
      return false;
    }
    block_write(fs_device, sector, &empty_block);
  }
  d->length = length;
  // inode in memory updated, now write back to disk
  block_write (fs_device, inode->sector, d);
  return true;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                      off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;
  
  // if write goes beyond the EOF then extend the inode capacity first
  off_t end_offset = offset + size;
  if (end_offset > inode->data.length) {
    if (!extend_capacity(inode, end_offset)) {
      return 0;
    }
  }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length (const struct inode *inode) { return inode->data.length; }
