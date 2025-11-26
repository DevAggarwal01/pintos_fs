#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include <string.h>

// the function that handles all system calls
static void syscall_handler (struct intr_frame *);
// lock for file system operations
struct lock file_lock;

/**
 * Initializes the system call system by setting the system call interrupt gate
 * The method be called exactly once during kernel startup, before any user process.
 */
void syscall_init (void)
{
    // initialize the syscall handler.
    intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
    // initialize the file lock
    lock_init(&file_lock);
}

/**
 * Exits the current process with the given status code.
 */
void system_exit (int status){
    // get the current thread and set its exit code
    struct thread *t = thread_current();
    t->exit_code = status;
    // record exit status in the corresponding child record, so parent can get it
    if (t->child_record != NULL) {
        t->child_record->exit_code = status;
        t->child_record->exited = true;
        sema_up(&t->child_record->exit_sema);
    }
    // print exit message and terminate the thread
    printf("%s: exit(%d)\n", t->name, status);
    process_exit();
    thread_exit();
}

/**
 * Translates a user virtual address to a kernel page.
 * Returns NULL if the address is invalid.
 */
static uint8_t *addr_to_page(const void *useraddr) {
    // check for null and that the address is in user address range
    if (useraddr == NULL || !is_user_vaddr(useraddr)) {
        return NULL;
    }
    // look up the page in the current thread's page directory
    return pagedir_get_page(thread_current()->pagedir, useraddr);
}

/**
 * Copies data from user space to kernel space.
 * Returns true on success, false if any byte could not be accessed.
 */
static bool copy_data(void *kernel_dst, const void *user_src_, size_t size) {
    // create pointers for source and destination
    const uint8_t *user_src = user_src_;
    uint8_t *kernel_ptr = kernel_dst;
    // copy byte by byte, checking each address
    for (size_t i = 0; i < size; i++) {
        uint8_t *page = addr_to_page(user_src + i);
        if (page == NULL) {
            // invalid address, so failed to copy
            return false;
        }
        kernel_ptr[i] = *(user_src + i);
    }
    // successfully copied all bytes
    return true;
}

/**
 * Copies a null-terminated string from user space to kernel space.
 * Returns pointer to kernel string on success, NULL on failure.
 */
static char *copy_string(const char *user_str) {
    // check for null and that the address is in user address range
    if (user_str == NULL || !is_user_vaddr(user_str)) {
        return NULL;
    }
    // allocate a page for the string buffer
    char *buffer = palloc_get_page(0);
    if (buffer == NULL) {
        return NULL;
    }
    // copy byte by byte until null terminator or page size limit
    for (size_t i = 0; i < PGSIZE; i++) {
        uint8_t *page = addr_to_page(user_str + i);
        if (page == NULL) {
            palloc_free_page(buffer);
            return NULL;
        }
        buffer[i] = *(user_str + i);
        if (buffer[i] == '\0') {
            return buffer;
        }
    }
    // string too long (no null terminator within page)
    palloc_free_page(buffer);
    return NULL;
}


/**
 * Creates a new file descriptor entry for the given file in the current thread.
 * Returns pointer to the new file descriptor entry on success, NULL on failure.
 */
static struct fd_entry *create_fd(struct file *file) {
    // get the current thread
    struct thread *t = thread_current();
    // allocate a page for a new file descriptor entry
    struct fd_entry *fd_entry = palloc_get_page(0);
    if (fd_entry == NULL) {
        return NULL;
    }
    // initialize the entry and add it to the thread's file descriptor list
    fd_entry->fd = t->next_fd++;
    fd_entry->f = file;
    list_push_back(&t->fds, &fd_entry->elem);
    // return the new file descriptor entry
    return fd_entry;
}

/**
 * Finds the file descriptor entry for the given fd in the current thread.
 * Returns pointer to the file descriptor entry on success, NULL if not found.
 */
static struct fd_entry *find_fd(int fd) {
    // get the current thread
    struct thread *t = thread_current();
    struct list_elem *e;
    // search the thread's file descriptor list for the given fd
    for (e = list_begin(&t->fds); e != list_end(&t->fds); e = list_next(e)) {
        struct fd_entry *fd_entry = list_entry(e, struct fd_entry, elem);
        if (fd_entry->fd == fd) {
            // found the file descriptor entry, so return it
            return fd_entry;
        }
    }
    // not found, return NULL
    return NULL;
}

/**
 * Removes the file descriptor entry for the given fd in the current thread.
 * Does nothing if the fd is not found.
 */
void remove_fd(int fd) {
    // get the current thread
    struct thread *t = thread_current();
    struct list_elem *e;
    // search the thread's file descriptor list for the given fd
    for (e = list_begin(&t->fds); e != list_end(&t->fds); e = list_next(e)) {
        struct fd_entry *fd_entry = list_entry(e, struct fd_entry, elem);
        if (fd_entry->fd == fd) {
            // found the file descriptor entry, so remove and free it
            list_remove(e);
            palloc_free_page(fd_entry);
            return;
        }
    }
}


/**
 * The main system call handler function.
 * Uses the appropriate system call based on the syscall number.
 */
static void syscall_handler (struct intr_frame *f UNUSED) {
    // get the syscall number from the stack
    uint8_t *sp = f->esp;
    // check for null and that the stack pointer is in user address range
    if (sp == NULL || !is_user_vaddr((const void *)sp)) {
        system_exit(-1);
    }
    // read the syscall number by copying from user space to kernel space
    int syscall_num;
    if (!copy_data(&syscall_num, sp, sizeof(int))) {
        system_exit(-1);
    }

    // ALL SYSTEM CALLS HANDLED HERE
    switch (syscall_num) {
        // Case 1: for the HALT system call
        case SYS_HALT: {
            // directly shut down the system
            shutdown_power_off();
            break;
        }
        // Case 2: for the EXIT system call
        case SYS_EXIT: {
            // get the exit status argument
            int status;
            if (!copy_data(&status, sp + 4, sizeof(int))) {
                system_exit(-1);
            }
            // use the helper method to exit the process
            system_exit(status);
            break;
        }
        // Case 3: for the EXEC system call
        case SYS_EXEC: {
            // get the command line argument; if invalid, exit with -1
            const char *cmd_linePtr;
            if (!copy_data(&cmd_linePtr, sp + 4, sizeof(const char *))) {
                system_exit(-1);
            }
            // check for null
            if (cmd_linePtr == NULL) {
                system_exit(-1);
            }
            // get a kernel copy of the command line string
            char *cmd_line = copy_string((char *)cmd_linePtr);
            if (cmd_line == NULL) {
                system_exit(-1);
            }
            // execute the command line and return the new process's TID
            f->eax = process_execute(cmd_line);
            palloc_free_page(cmd_line);
            break;
        }
        // Case 4: for the WAIT system call
        case SYS_WAIT: {
            // get the TID argument
            tid_t tid;
            if (!copy_data(&tid, sp + 4, sizeof(tid_t))) {
                f->eax = -1;
                break;
            }
            // call process_wait method and return its result
            f->eax = process_wait(tid);
            break;
        }
        // Case 5: for the CREATE system call
        case SYS_CREATE: {
            // get the file name and initial size arguments
            const char *filePtr;
            unsigned initial_size;
            if (!copy_data(&filePtr, sp + 4, sizeof(const char *))) {
                system_exit(-1);
            }
            if (filePtr == NULL) {
                system_exit(-1);
            }
            if (!copy_data(&initial_size, sp + 8, sizeof(unsigned))) {
                system_exit(-1);
            }
            // copy the file name string from user to kernel space
            char *file = copy_string((char *)filePtr);
            if (file == NULL) {
                system_exit(-1);
            }
            // check for empty file name
            if (file[0] == '\0') {
                palloc_free_page(file);
                f->eax = false;
                return;
            }
            // create the file and return the result (lock around file system call)
            lock_acquire(&file_lock);
            f->eax = filesys_create(file, initial_size);
            lock_release(&file_lock);
            palloc_free_page(file);
            break;
        }
        // Case 6: for the REMOVE system call
        case SYS_REMOVE: {
            // get the file name argument
            const char *filePtr;
            if (!copy_data(&filePtr, sp + 4, sizeof(const char *))) {
                system_exit(-1);
            }
            if (filePtr == NULL) {
                system_exit(-1);
            }
            // copy the file name string from user to kernel space
            char *file = copy_string((char *)filePtr);
            if (file == NULL) {
                system_exit(-1);
            }
            // check for empty file name
            if (file[0] == '\0') {
                palloc_free_page(file);
                f->eax = false;
                return;
            }
            // remove the file and return the result (lock around file system call)
            lock_acquire(&file_lock);
            f->eax = filesys_remove(file);
            lock_release(&file_lock);
            palloc_free_page(file);
            break;
        }
        // Case 7: for the OPEN system call
        case SYS_OPEN: {
            // get the file name argument
            const char *fileNamePtr;
            if (!copy_data(&fileNamePtr, sp + 4, sizeof(const char *))) {
                system_exit(-1);
            }
            if (fileNamePtr == NULL) {
                system_exit(-1);
            }
            // copy the file name string from user to kernel space
            char *fileName = copy_string((char *)fileNamePtr);
            if (fileName == NULL) {
                system_exit(-1);
            }
            // check for empty file name
            if (fileName[0] == '\0') {
                palloc_free_page(fileName);
                f->eax = -1;
                return;
            }
            // open the file (lock around file system call)
            lock_acquire(&file_lock);
            struct file *file = filesys_open(fileName);
            lock_release(&file_lock);
            if (file == NULL) {
                palloc_free_page(fileName);
                f->eax = -1;
                break;
            }
            // create a new file descriptor entry for the opened file
            struct fd_entry *fd = create_fd(file);
            if (fd == NULL) {
                // failed to create fd entry; close file and return -1
                f->eax = -1;
                file_close(file);
                palloc_free_page(fileName);
                break;
            }
            // return the new file descriptor number
            f->eax = fd->fd;
            palloc_free_page(fileName);
            break;
        }
        // Case 8: for the CLOSE system call
        case SYS_FILESIZE: {
            // get the file descriptor argument
            int fd;
            if (!copy_data(&fd, sp + 4, sizeof(int))) {
                system_exit(-1);
            }
            // find the file descriptor entry
            struct fd_entry *fd_entry = find_fd(fd);
            if (fd_entry == NULL) {
                f->eax = -1;
                break;
            }
            // get the file size (lock around file system call)
            lock_acquire(&file_lock);
            f->eax = file_length(fd_entry->f);
            lock_release(&file_lock);
            break;
        }
        // Case 9: for the READ system call
        case SYS_READ: {
            // get the arguments: fd, buffer, size
            int fd;
            void *buffer;
            unsigned size;
            if (!copy_data(&fd, sp + 4, sizeof(int)) ||
                !copy_data(&buffer, sp + 8, sizeof(void *)) ||
                !copy_data(&size, sp + 12, sizeof(unsigned))) {
                system_exit(-1);
            }
            if (size == 0) {
                f->eax = 0;
                break;
            }
            if (buffer == NULL) {
                system_exit(-1);
            }
            // validate each page of buffer to be read into
            char *buf = (char *)buffer;
            unsigned remaining = size;
            while (remaining > 0) {
                // check that the page is valid
                if (addr_to_page(buf) == NULL) {
                    system_exit(-1);
                }
                // move to next page
                size_t offset = (uintptr_t)buf & (PGSIZE - 1);
                // calculate the size of the chunk to read
                size_t chunk = PGSIZE - offset;
                // limit chunk to remaining size
                if (chunk > remaining) {
                    chunk = remaining;
                }
                buf += chunk;
                remaining -= chunk;
            }
            // handle reading from keyboard or file
            if (fd == 0) {
                for (unsigned i = 0; i < size; i++) {
                    ((char *)buffer)[i] = input_getc();
                }
                f->eax = size;
                break;
            }
            // find the file descriptor entry
            struct fd_entry *fd_entry = find_fd(fd);
            if (fd_entry == NULL) {
                f->eax = -1;
                break;
            }
            // read from the file (lock around file system call)
            lock_acquire(&file_lock);
            f->eax = file_read(fd_entry->f, buffer, size);
            lock_release(&file_lock);
            break;
        }
        // Case 10: for the WRITE system call
        case SYS_WRITE: {
            // get the arguments: fd, buffer, size
            int fd;
            const void *buffer;
            unsigned size;
            if (!copy_data(&fd, sp + 4, sizeof(int)) ||
                !copy_data(&buffer, sp + 8, sizeof(const void *)) ||
                !copy_data(&size, sp + 12, sizeof(unsigned))) {
                system_exit(-1);
            }
            if (size == 0) {
                f->eax = 0;
                break;
            }
            if (buffer == NULL) {
                system_exit(-1);
            }
            // validate each page of buffer to be written from
            const char *bufw = (const char *)buffer;
            unsigned remainingw = size;
            while (remainingw > 0) {
                // check that the page is valid
                if (addr_to_page(bufw) == NULL) {
                    system_exit(-1);
                }
                // move to next page
                size_t offset = (uintptr_t)bufw & (PGSIZE - 1);
                // calculate the size of the chunk to write
                size_t chunk = PGSIZE - offset;
                // limit chunk to remaining size
                if (chunk > remainingw) {
                    chunk = remainingw;
                }
                bufw += chunk;
                remainingw -= chunk;
            }
            if (fd == 1) {
                // handle writing to console or file
                putbuf(buffer, size);
                f->eax = size;
                break;
            } else if (fd == 0) {
                // handle writing to keyboard (not allowed)
                f->eax = -1;
                break;
            } else {
                // find the file descriptor entry
                struct fd_entry *fd_entry = find_fd(fd);
                if (fd_entry == NULL) {
                    f->eax = -1;
                    break;
                }
                // write to the file (lock around file system call)
                lock_acquire(&file_lock);
                int written = 0;
                // write in a loop until all bytes are written
                while (written < (int)size) {
                    int needWrite = size - written;
                    int wrote = file_write(fd_entry->f, (const uint8_t *)buffer + written, needWrite);
                    if (wrote <= 0) {
                        break;
                    }
                    written += wrote;
                }
                lock_release(&file_lock);
                f->eax = written;
                break;
            }
        }
        // Case 11: for the SEEK system call
        case SYS_SEEK: {
            // get the arguments: fd, position
            int fd;
            unsigned position;
            if (!copy_data(&fd, sp + 4, sizeof(int))) {
                system_exit(-1);
            }
            if (!copy_data(&position, sp + 8, sizeof(unsigned))) {
                system_exit(-1);
            }
            // find the file descriptor entry
            struct fd_entry *fd_entry = find_fd(fd);
            if (fd_entry == NULL) {
                system_exit(-1);
            }
            // seek to the position in the file (lock around file system call)
            lock_acquire(&file_lock);
            file_seek(fd_entry->f, position);
            lock_release(&file_lock);
            break;
        }
        // Case 12: for the TELL system call
        case SYS_TELL: {
            // get the fd argument
            int fd;
            if (!copy_data(&fd, sp + 4, sizeof(int))) {
                system_exit(-1);
            }
            // find the file descriptor entry
            struct fd_entry *fd_entry = find_fd(fd);
            if (fd_entry == NULL) {
                f->eax = -1;
                break;
            }
            // tell the current position in the file (lock around file system call)
            lock_acquire(&file_lock);
            f->eax = file_tell(fd_entry->f);
            lock_release(&file_lock);
            break;
        }
        // Case 13: for the CLOSE system call
        case SYS_CLOSE: {
            // get the fd argument
            int fd;
            if (!copy_data(&fd, sp + 4, sizeof(int))) {
                system_exit(-1);
            }
            // find the file descriptor entry
            struct fd_entry *fd_entry = find_fd(fd);
            if (fd_entry == NULL) {
                f->eax = -1;
                break;
            }
            // close the file and remove the fd entry (lock around file system call)
            lock_acquire(&file_lock);
            file_close(fd_entry->f);
            lock_release(&file_lock);
            remove_fd(fd);
            break;
        }
        default:
            // unknown syscall number
            system_exit(-1);
    }
}