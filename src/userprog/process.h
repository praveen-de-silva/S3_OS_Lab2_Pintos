#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

struct child_rec {
    tid_t tid;                    /* Child thread id. */
    int exit_status;              /* Child exit status. */
    bool exited;                  /* True if child has exited. */
    bool load_success;            /* True if child loaded successfully. */
    struct semaphore sema;        /* Semaphore to wait for child. */
    struct semaphore load_sema;   /* Semaphore to wait for load completion. */
    struct list_elem elem;        /* List element for children list. */
};

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
