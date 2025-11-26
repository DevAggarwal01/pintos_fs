#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);


/**
 * Struct to track a relationship between a parent and a child process.
 */
struct child_record {
    tid_t parent_tid;               // parent thread ID
    tid_t child_tid;                // child thread ID
    int exit_code;                  // exit status
    bool exited;                    // if the child has exited
    bool waited;                    // if the parent has already waited
    bool loaded;                    // if the child successfully loaded its executable
    struct semaphore start_sema;    // child waits until parent finishes setup
    struct semaphore load_sema;     // parent waits until child finishes loading
    struct semaphore exit_sema;     // parent waits until child exits
    struct list_elem elem_child;    // for representing in parent->children list
};

#endif /* userprog/process.h */
