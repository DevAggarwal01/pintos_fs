#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h" 
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h" // for semaphores and synchronization 
#include "syscall.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

// struct to pass information to the process_start function
struct start_info {
    char *fn_copy;              // copy of the file name (command line)
    struct child_record *rec;   // child record for this process
    struct thread *parent;      // parent thread pointer
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) {
     tid_t tid;
    // make a kernel copy of FILE_NAME for the child to use
    char *fn_copy = palloc_get_page (0);
    if (fn_copy == NULL) {
        return TID_ERROR;
    }
    strlcpy (fn_copy, file_name, PGSIZE);
    // allocate child record
    struct child_record *rec = palloc_get_page(0);
    if (rec == NULL) {
        palloc_free_page(fn_copy);
        return TID_ERROR;
    }
    // allocate page for start_info
    struct start_info *info = palloc_get_page(0);
    if (info == NULL) {
        palloc_free_page(fn_copy);
        palloc_free_page(rec);
        return TID_ERROR;
    }
    // initialize child record
    rec->parent_tid = thread_tid();
    rec->child_tid = -1;
    rec->exit_code = -1;
    rec->exited = false;
    rec->waited = false;
    rec->loaded = false;
    sema_init(&rec->start_sema, 0);
    sema_init(&rec->exit_sema, 0);
    sema_init(&rec->load_sema, 0);
    // register the record in global list and parent's child list
    enum intr_level old_level = intr_disable();
    struct thread *parent = thread_current();
    list_push_back(&parent->children, &rec->elem_child);
    intr_set_level(old_level);
    // fill start_info for the child thread
    info->fn_copy = fn_copy;
    info->rec = rec;
    info->parent = parent;
    // use a temporary kernel page to extract program name without affecting fn_copy
    char *prog_copy = palloc_get_page(0);
    if (prog_copy == NULL) {
        enum intr_level old_level = intr_disable();
        list_remove(&rec->elem_child);
        intr_set_level(old_level);
        palloc_free_page(info);
        palloc_free_page(rec);
        palloc_free_page(fn_copy);
        return TID_ERROR;
    }
    strlcpy(prog_copy, fn_copy, PGSIZE);
    // tokenize the temporary buffer to get the program name
    char *save_ptr;
    char *program = strtok_r(prog_copy, " ", &save_ptr);
    if (program == NULL) {
        enum intr_level old_level = intr_disable();
        list_remove(&rec->elem_child);
        intr_set_level(old_level);
        palloc_free_page(info);
        palloc_free_page(rec);
        palloc_free_page(fn_copy);
        palloc_free_page(prog_copy);
        return TID_ERROR;
    }
    // check that the program file exists before creating thread
    lock_acquire(&file_lock);
    struct file *file = filesys_open(program);
    lock_release(&file_lock);
    if (file == NULL) {
        enum intr_level old_level = intr_disable();
        list_remove(&rec->elem_child);
        intr_set_level(old_level);
        palloc_free_page(rec);
        palloc_free_page(fn_copy);
        palloc_free_page(prog_copy);
        return TID_ERROR;
    }
    file_close(file);
    // create the child thread
    tid = thread_create(program, PRI_DEFAULT, start_process, info);
    if (tid == TID_ERROR) {
        enum intr_level old_level = intr_disable();
        list_remove(&rec->elem_child);
        intr_set_level(old_level);
        palloc_free_page(info);
        palloc_free_page(rec);
        palloc_free_page(fn_copy);
        palloc_free_page(prog_copy);
        return TID_ERROR;
    }
    // store child's tid in the record (before letting child proceed)
    rec->child_tid = tid;
    // free temporary program copy
    palloc_free_page(prog_copy);
    // allow the child to run (start_process does sema_down on this)
    sema_up(&rec->start_sema);
    // wait for child to finish loading so that exec() can return -1 on failure
    sema_down(&rec->load_sema);

    // if the child failed to load, clean up and return error (-1)
    if (!rec->loaded) {
        enum intr_level old_level = intr_disable();
        list_remove(&rec->elem_child);
        intr_set_level(old_level);
        palloc_free_page(rec);
        return TID_ERROR;
    }
    // parent returns the child's tid
    return tid;
}


/* A thread function that loads a user process and starts it
   running. */
static void start_process (void *info){
    // initialize thread, start_info, and file_name structures
    struct thread *t = thread_current();
    struct start_info *start_info = info;
    char *file_name = start_info->fn_copy;
    // set up parent and child record pointers
    t->parent = start_info->parent;
    t->child_record = start_info->rec;
    palloc_free_page(start_info);
    // the file name (full command line) is passed in via file_name_
    struct intr_frame if_;
    bool success;
    // find this thread's own child record in the list of records
    struct child_record *rec = t->child_record;
    // wait until parent signals that setup is complete
    if (rec != NULL) {
        sema_down(&rec->start_sema);
    }
    // initialize interrupt frame, load executable, free file name
    memset (&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp);
    // tell parent whether load() succeeded so exec() can return -1 on failure
    if (rec != NULL) {
        enum intr_level old = intr_disable();
        rec->loaded = success;
        sema_up(&rec->load_sema);
        intr_set_level(old);
    }
    palloc_free_page (file_name);
    // if load failed, quit
    if (!success) {
        system_exit(-1);
        NOT_REACHED();
    }
    /* Start the user process by simulating a return from an
        interrupt, implemented by intr_exit (in
        threads/intr-stubs.S).  Because intr_exit takes all of its
        arguments on the stack in the form of a `struct intr_frame',
        we just point the stack pointer (%esp) to our stack frame
        and jump to it. */
    asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
    NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int process_wait (tid_t child_tid) { 
    // get this current thread and TID
    struct thread *parent = thread_current();
    tid_t my_tid = thread_tid();
    // find the child record for the given child TID
    struct child_record *rec = NULL;
    enum intr_level old = intr_disable();
    for (struct list_elem *e = list_begin(&parent->children); e != list_end(&parent->children); e = list_next(e)) {
        struct child_record *c = list_entry(e, struct child_record, elem_child);
        if (c->child_tid == child_tid) {
            rec = c;
            break;
        }
    }
    // if no such child or already waited, return -1
    if (rec == NULL || rec->waited) {
        intr_set_level(old);
        return -1;
    }
    // mark as waited (only one successful wait allowed)
    rec->waited = true;
    intr_set_level(old);
    // wait until child exits
    if(!rec->exited) {
        sema_down(&rec->exit_sema);
    }
    // capture exit status and remove/free the record
    int status = rec->exit_code;
    enum intr_level old_level = intr_disable();
    list_remove(&rec->elem_child);
    intr_set_level(old_level);
    palloc_free_page(rec);
    // return the child's exit status 
    return status;
}

/* Free the current process's resources. */
void process_exit (void) {
    // get current thread and its page directory
    struct thread *cur = thread_current();
    uint32_t *pd;
    // record exit status in the corresponding child record, so parent can get it
    tid_t my_tid = thread_tid();
    // if child record exists, update it and wake up parent
    if (cur->child_record && !cur->child_record->exited) {
        enum intr_level old = intr_disable();
        cur->child_record->exit_code = cur->exit_code;
        cur->child_record->exited = true;
        sema_up(&cur->child_record->exit_sema);
        intr_set_level(old);
    }
    // close executable file, allow writes
    if (cur->exec_file != NULL) {
        lock_acquire(&file_lock);
        file_allow_write(cur->exec_file);
        file_close(cur->exec_file);
        lock_release(&file_lock);
        cur->exec_file = NULL;
    }
    // close all open files and free file descriptors
    while(!list_empty(&cur->fds)) {
        enum intr_level old = intr_disable();
        if (list_empty(&cur->fds)) {
            intr_set_level(old);
            break;
        }
        struct list_elem *e = list_begin(&cur->fds);
        struct fd_entry *fd_entry = list_entry(e, struct fd_entry, elem);
        intr_set_level(old);
        lock_acquire(&file_lock);
        file_close(fd_entry->f);
        lock_release(&file_lock);
        remove_fd(fd_entry->fd);
    }
    /* Destroy the current process's page directory and switch back
        to the kernel-only page directory. */
    pd = cur->pagedir;
    if (pd != NULL) {
        /* Correct ordering here is crucial.  We must set
            cur->pagedir to NULL before switching page directories,
            so that a timer interrupt can't switch back to the
            process page directory.  We must activate the base page
            directory before destroying the process's page
            directory, or our active page directory will be one
            that's been freed (and cleared). */
        cur->pagedir = NULL;
        pagedir_activate (NULL);
        pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
{
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
{
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack (const char* cmdline, void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load (const char *file_name, void (**eip) (void), void **esp) {
    struct thread *t = thread_current ();
    struct Elf32_Ehdr ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;
    /* Allocate and activate page directory. */
    t->pagedir = pagedir_create ();
    if (t->pagedir == NULL)
        goto done;
    process_activate ();
    /* Open executable file. */

    // [THIS IS A MODIFICATION MADE IN THIS METHOD FROM THE ORIGINAL CODE]
    // -----START MODIFICATION-----
    // make a copy of file name to tokenize
    char *file_copy = palloc_get_page(0);
    if (file_copy == NULL) {
        // Could not allocate memory: fail gracefully.
        return false;
    }
    strlcpy(file_copy, file_name, PGSIZE);
    // extract the first word from filename
    char *save_ptr;
    char *program = strtok_r(file_copy, " ", &save_ptr);
    // open the file using the first word (which is the program name)
    lock_acquire(&file_lock);
    file = filesys_open(program);
    if (file != NULL) {
        /* Keep the file and prevent other writers while we execute it. */
        t->exec_file = file;
        file_deny_write(file);
    }
    lock_release(&file_lock);
    if (file == NULL) {
        printf ("load: %s: open failed\n", file_name);
        goto done;
    }
    // -----END MODIFICATION-----

    
    /* Read and verify executable header. */
    if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr ||
        memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
        ehdr.e_machine != 3 || ehdr.e_version != 1 ||
        ehdr.e_phentsize != sizeof (struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
        printf ("load: %s: error loading executable\n", file_name);
        goto done;
    }
    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) {
        struct Elf32_Phdr phdr;
        if (file_ofs < 0 || file_ofs > file_length (file)) {
            goto done;
        }
        file_seek (file, file_ofs);
        if (file_read (file, &phdr, sizeof phdr) != sizeof phdr) {
            goto done;
        }
        file_ofs += sizeof phdr;
        switch (phdr.p_type) {
            case PT_NULL:
            case PT_NOTE:
            case PT_PHDR:
            case PT_STACK:
            default:
                /* Ignore this segment. */
                break;
            case PT_DYNAMIC:
            case PT_INTERP:
            case PT_SHLIB:
                goto done;
            case PT_LOAD:
                if (validate_segment (&phdr, file)) {
                    bool writable = (phdr.p_flags & PF_W) != 0;
                    uint32_t file_page = phdr.p_offset & ~PGMASK;
                    uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                    uint32_t page_offset = phdr.p_vaddr & PGMASK;
                    uint32_t read_bytes, zero_bytes;
                    if (phdr.p_filesz > 0) {
                        /* Normal segment.
                        Read initial part from disk and zero the rest. */
                        read_bytes = page_offset + phdr.p_filesz;
                        zero_bytes =
                            (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) -
                            read_bytes);
                    } else {
                        /* Entirely zero.
                        Don't read anything from disk. */
                        read_bytes = 0;
                        zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                    }
                    if (!load_segment (file, file_page, (void *) mem_page, read_bytes, zero_bytes, writable)) {
                        goto done;
                    }
                } else {
                    goto done;
                }
                break;
        }
    }
    /* Set up stack. Pass the command-line so we can build argc/argv.*/

    // [THIS IS A MODIFICATION MADE IN THIS METHOD FROM THE ORIGINAL CODE]
    // -----START MODIFICATION-----
    if (!setup_stack (file_name, esp)) {
        goto done;
    }
    // -----END MODIFICATION-----

    /* Start address. */
    *eip = (void (*) (void)) ehdr.e_entry;
    success = true;
    done:
        /* We arrive here whether the load is successful or not. */
        // file_close (file);
        // return success;

        // [THIS IS A MODIFICATION MADE IN THIS METHOD FROM THE ORIGINAL CODE]
        // -----START MODIFICATION-----
        if (!success && file != NULL) {
            file_close(file);
            t->exec_file = NULL;
        }
        if (file_copy) {
            palloc_free_page(file_copy);
        }
        return success;
        // -----END MODIFICATION-----
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/**
 * Set up the user stack for the new process.
 * cmdline points to the palloc page that process_execute allocated (writable).
 * We parse cmdline in-place (using strtok_r), copy strings onto the user stack,
 * word-align, push argv pointers, push argv, push argc, and push fake return addr.
 */
static bool setup_stack (const char *cmdline, void **esp) {
    // allocate a page for the stack
    uint8_t *kpage;
    kpage = palloc_get_page(PAL_USER | PAL_ZERO); // flags
    if (kpage == NULL) {
        return false;
    }
    // map the page at the top of user virtual memory
    bool success = install_page(((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
    if (!success) {
        palloc_free_page(kpage);
        return false;
    }
    // start stack at top of user page
    uint8_t *sp = (uint8_t *) PHYS_BASE;
    // make a writable copy of cmdline to tokenize
    char *cmd_copy = palloc_get_page(0);
    if (cmd_copy == NULL) {
        return false;
    }
    strlcpy(cmd_copy, cmdline, PGSIZE);
    // tokenize cmdline into argv[]
    char *argv[128]; // assume no more than 128 args; mentioned in project specifications
    int argc = 0;
    char *save_ptr;
    char *token = strtok_r(cmd_copy, " ", &save_ptr);
    while (token != NULL && argc < 128) {
        argv[argc++] = token;
        token = strtok_r(NULL, " ", &save_ptr);
    }
    // push argument strings onto stack in reverse order
    char *arg_addrs[128]; // assume no more than 128 args; mentioned in project specifications
    for (int i = argc - 1; i >= 0; i--) {
        sp -= strlen(argv[i]) + 1; // + 1 for null terminator
        memcpy(sp, argv[i], strlen(argv[i]) + 1);
        arg_addrs[i] = (char *) sp;
    }

    // word-align to multiple of 4 (because we're pushing pointers, which are 4 bytes)
    while ((uintptr_t)sp % 4 != 0) {
        sp--;
    }
    // end the argv array with a null pointer
    sp -= sizeof(char *);
    *((char **) sp) = NULL;
    // push addresses of arguments in reverse order
    for (int i = argc - 1; i >= 0; i--) {
        sp -= sizeof(char *);
        *((char **) sp) = arg_addrs[i];
    }
    // push argv (pointer to argv[0])
    char **argv_ptr = (char **) sp;
    sp -= sizeof(char **);
    *((char ***) sp) = argv_ptr;
    // push argc (number of args)
    sp -= sizeof(int);
    *((int *) sp) = argc;
    // push fake return address (which is just 0)
    sp -= sizeof(void *);
    *((void **) sp) = NULL;
    // set the stack pointer
    *esp = sp;
    // hex dump for debugging (disabled by default; enable if needed)
    // hex_dump((uintptr_t) sp, sp, (char *) PHYS_BASE - (char*) sp, true);
    // free the temporary command line copy and return success
    palloc_free_page(cmd_copy);
    return true;
}


/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL &&
          pagedir_set_page (t->pagedir, upage, kpage, writable));
}
