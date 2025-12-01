#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done (void) { free_map_close (); }

static struct dir *get_parent_directory(const char *path, char leaf[NAME_MAX + 1]) {
  if(path == NULL || path[0] == '\0') {
      return NULL;
  }
  struct dir *current_dir;
  // if first char is '/', start from root because it is absolute path
  if (path[0] == '/') {
      current_dir = dir_open_root();
  } else { // relative path
    struct thread *t = thread_current();

    if (t->current_dir != NULL) {
        current_dir = dir_reopen(t->current_dir);
    } else {
      current_dir = dir_open_root();
    }
  }

  if(current_dir == NULL) {
      return NULL;
  }

  // strtok_r modifies the input string, so we make a copy
  size_t len = strlen (path);
  char *path_copy = malloc(len + 1);
  if (path_copy == NULL) {
      dir_close(current_dir);
      return NULL;
  }
  strlcpy(path_copy, path, len + 1);

  char *save_ptr;
  char *token = strtok_r(path_copy, "/", &save_ptr); 
  
  // path is just "/"
  if(token == NULL) {
      free(path_copy);
      leaf[0] = '\0';
      return current_dir;
  }

  while (token != NULL) {
    if (strlen(token) > NAME_MAX) {
        dir_close(current_dir);
        free(path_copy);
        return NULL;
    }

    char *next_token = strtok_r(NULL, "/", &save_ptr);

    // this is the last file in the path
    if (next_token == NULL) {
        strlcpy(leaf, token, NAME_MAX + 1);
        free(path_copy);
        return current_dir;
    } else { // go to next directory
        struct inode *inode;
        if (!dir_lookup(current_dir, token, &inode)) {
            dir_close(current_dir);
            free(path_copy);
            return NULL;
        }
        struct dir *next_dir = dir_open(inode);
        dir_close(current_dir);
        if (next_dir == NULL) {
          inode_close(inode);
          free(path_copy);
          return NULL;
        }
        
        current_dir = next_dir;
    }
    token = next_token;
  }

  // shouldn't reach here if path is valid
  free(path_copy);
  dir_close(current_dir);
  return NULL;
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create (const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  char filename[NAME_MAX + 1];
  
  struct dir *dir = get_parent_directory(name, filename);
  
  if (dir == NULL) {
      return false;
  }
  // don't allow the "." and ".." filenames to be created
  if(filename[0] == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
      dir_close(dir);
      return false;
  }

  bool success = (dir != NULL && free_map_allocate (1, &inode_sector) &&
                  inode_create (inode_sector, initial_size, false) &&
                  dir_add (dir, filename, inode_sector));

  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  dir_close (dir);

  return success;
}
// creates a folder for the mkdir system call
bool filesys_mkdir (const char *name)
{
  block_sector_t inode_sector = 0;
  char filename[NAME_MAX + 1];
  
  struct dir *parent_dir = get_parent_directory(name, filename);
  
  if (parent_dir == NULL) {
      return false;
  }
  // don't allow the "." and ".." filenames to be created
  if(filename[0] == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
      dir_close(parent_dir);
      return false;
  }
  // allocate space for the new directory inode
  if(!free_map_allocate (1, &inode_sector)) {
      dir_close(parent_dir);
      return false;
  }
  
  block_sector_t parent_sector = inode_get_inumber(dir_get_inode(parent_dir));

  // create the directory
  bool success = (parent_dir != NULL && dir_create (inode_sector, 16, parent_sector) &&
                  dir_add (parent_dir, filename, inode_sector));

  // fall back if unsuccessful
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  dir_close (parent_dir);

  return success;
}
// changes the current working directory for the chdir system call
bool filesys_chdir (const char *name) {
  char filename[NAME_MAX + 1];
  
  struct dir *parent_dir = get_parent_directory(name, filename);
  
  if (parent_dir == NULL) {
      return false;
  }

  struct inode *inode;
  if (!dir_lookup (parent_dir, filename, &inode)) {
    dir_close (parent_dir);
    return false;
  }

  // open the intended directory
  struct dir *new_dir = dir_open(inode);
  if (new_dir == NULL) {
      dir_close(parent_dir);
      return false;
  }

  dir_close(parent_dir);

  // close current directory if we have one before reassigning to new directory
  struct thread *t = thread_current();
  if (t->current_dir != NULL) {
      dir_close(t->current_dir);
  }
  t->current_dir = new_dir;

  return true;
}
/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *filesys_open (const char *name)
{
  if (name == NULL || name[0] == '\0') {
      return NULL;
  }

  if(strcmp(name, "/") == 0) {
      struct inode *root_inode = inode_open (ROOT_DIR_SECTOR);
      return file_open(root_inode);
  }

  char filename[NAME_MAX + 1];
  struct dir *parent_dir = get_parent_directory(name, filename);
  if (parent_dir == NULL) {
      return NULL;
  }

  struct inode *inode = NULL;

  if(!dir_lookup (parent_dir, filename, &inode)) {
    // file_open can handle a null inode
    inode = NULL;
  }

  dir_close (parent_dir);
  return file_open (inode);
}

struct inode *get_inode_from_path(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }

    if(strcmp(path, "/") == 0) {
        return inode_open (ROOT_DIR_SECTOR);
    }

    char filename[NAME_MAX + 1];
    struct dir *parent_dir = get_parent_directory(path, filename);
    if (parent_dir == NULL) {
        return NULL;
    }

    struct inode *inode = NULL;

    if(!dir_lookup (parent_dir, filename, &inode)) {
      // file does not exist
      inode = NULL;
    }

    dir_close (parent_dir);
    return inode;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove (const char *name)
{
  if(name == NULL || name[0] == '\0') {
      return false;
  }

  if(strcmp(name, "/") == 0) {
      return false;
  }

  char filename[NAME_MAX + 1];
  struct dir *parent_dir = get_parent_directory(name, filename);
  if (parent_dir == NULL) {
      return false;
  }

  // don't allow the "." and ".." filenames to be removed
  if(filename[0] == '\0' || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
      dir_close(parent_dir);
      return false;
  }

  bool success = parent_dir != NULL && dir_remove (parent_dir, filename);
  dir_close (parent_dir);

  return success;
}

/* Formats the file system. */
static void do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
