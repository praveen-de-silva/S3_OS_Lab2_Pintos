#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define MAX_ARGS 128

struct start_process_args {
  char *file_name;
  struct thread *parent;
};

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool setup_stack (void **esp, const char *cmdline);
static struct child_rec *get_child_rec (tid_t tid);
static struct child_rec *get_child_rec_in_list (struct thread *t, tid_t tid);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;
  char *prog_name, *save_ptr;
  struct child_rec *child_rec;
  struct start_process_args *args;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Make another copy to parse just the program name. */
  char *file_name_for_parse = palloc_get_page(0);
  if(file_name_for_parse == NULL)
    {
      palloc_free_page(fn_copy);
      return TID_ERROR;
    }
  strlcpy(file_name_for_parse, file_name, PGSIZE);

  prog_name = strtok_r(file_name_for_parse, " ", &save_ptr);

  /* Create args struct for start_process */
  args = palloc_get_page(0);
  if (args == NULL)
    {
      palloc_free_page(file_name_for_parse);
      palloc_free_page(fn_copy);
      return TID_ERROR;
    }
  args->file_name = fn_copy;
  args->parent = thread_current();

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, args);

  palloc_free_page(file_name_for_parse);

  if (tid == TID_ERROR)
    {
      palloc_free_page(args);
      palloc_free_page (fn_copy);
    }
  else
    {
      /* Create child record and add to parent's children list. */
      child_rec = malloc(sizeof(struct child_rec));
      if (child_rec == NULL)
        {
          palloc_free_page(args);
          palloc_free_page (fn_copy);
          return TID_ERROR;
        }
      child_rec->tid = tid;
      child_rec->exit_status = -1;
      child_rec->exited = false;
      child_rec->load_success = false;
      sema_init(&child_rec->sema, 0);
      sema_init(&child_rec->load_sema, 0);
      list_push_back(&thread_current()->children, &child_rec->elem);

      /* Wait for child to finish loading */
      sema_down(&child_rec->load_sema);

      if (!child_rec->load_success)
        {
          /* Load failed, remove child record and return error */
          list_remove(&child_rec->elem);
          free(child_rec);
          return TID_ERROR;
        }
    }

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args_)
{
  struct start_process_args *args = args_;
  char *file_name = args->file_name;
  struct intr_frame if_;
  bool success;

  /* Set parent pointer */
  thread_current()->parent = args->parent;

  /* Free the args struct */
  palloc_free_page(args);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* Signal parent about load result */
  struct child_rec *child_rec = get_child_rec_in_list(thread_current()->parent, thread_current()->tid);
  if (child_rec != NULL)
    {
      child_rec->load_success = success;
      sema_up(&child_rec->load_sema);
    }

  /* If load failed, quit. */
  if (!success) 
    {
      thread_current()->exitStatus = -1;
      thread_exit ();
    }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a struct intr_frame,
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t child_tid) 
{
  struct child_rec *child_rec = get_child_rec(child_tid);
  
  if (child_rec == NULL)
    return -1;
  
  if (child_rec->exited)
    {
      /* Child already exited, return status immediately */
      int status = child_rec->exit_status;
      list_remove(&child_rec->elem);
      free(child_rec);
      return status;
    }
  else
    {
      /* Wait for child to exit */
      sema_down(&child_rec->sema);
      int status = child_rec->exit_status;
      list_remove(&child_rec->elem);
      free(child_rec);
      return status;
    }
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Print the exit message. */
  printf ("%s: exit(%d)\n", cur->name, cur->exitStatus);

  /* Signal parent if we have one */
  if (cur->parent != NULL)
    {
      struct child_rec *child_rec = get_child_rec_in_list(cur->parent, cur->tid);
      if (child_rec != NULL)
        {
          child_rec->exit_status = cur->exitStatus;
          child_rec->exited = true;
          sema_up(&child_rec->sema);
        }
    }

  /* Close all open files. */
  filesys_lock_acquire();
  
  /* Close the executable file */
  if (cur->executable != NULL) {
    file_allow_write(cur->executable);
    file_close(cur->executable);
    cur->executable = NULL;
  }
  
  /* Close all file descriptors */
  int i;
  for (i = 2; i < 128; i++) {
    if (cur->fd_table[i] != NULL) {
      file_close(cur->fd_table[i]);
      cur->fd_table[i] = NULL;
    }
  }
  filesys_lock_release();

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is important.  We must set
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
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more or less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE16Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff. */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, const char *cmdline);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *cmdline, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  char *cmdline_copy = palloc_get_page(0);
  if(cmdline_copy == NULL)
    return false;
  strlcpy(cmdline_copy, cmdline, PGSIZE);

  char *prog_name, *save_ptr;
  prog_name = strtok_r(cmdline_copy, " ", &save_ptr);

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (prog_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", prog_name);
      goto done; 
    }

  /* Deny writes to the executable file */
  file_deny_write(file);
  thread_current()->executable = file;

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", prog_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
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
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, cmdline))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  palloc_free_page(cmdline_copy);
  /* Don't close the file here - we keep it open to deny writes.
     It will be closed in process_exit(). */
  if (!success && file != NULL) {
    file_close(file);
    thread_current()->executable = NULL;
  }
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
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
     could quite likely access user data by accident.  We don't
     want to make it easy for user code to accidentally pass
     system calls code that will execute even if passed null
     pointers. */
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
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
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

/* Create a minimal stack by mapping a zeroed page at the top of user virtual memory. */
static bool
setup_stack (void **esp, const char *cmdline)
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        {
          char *kpage_ptr = (char *)kpage + PGSIZE;
          char *user_esp = (char *)PHYS_BASE;

          char *cmdline_copy = palloc_get_page(0);
          if(cmdline_copy == NULL) {
              palloc_free_page(kpage);
              return false;
          }
          strlcpy(cmdline_copy, cmdline, PGSIZE);

          char *token, *save_ptr;
          int arg_count = 0;
          char *argument_pointers[MAX_ARGS];

          // 1. Tokenize command line
          for (token = strtok_r (cmdline_copy, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr))
            {
              argument_pointers[arg_count++] = token;
            }

          // 2. Push argument strings onto the stack (from right to left)
          for (int i = arg_count - 1; i >= 0; i--)
            {
              int len = strlen(argument_pointers[i]) + 1;
              user_esp -= len;
              kpage_ptr -= len;
              memcpy(kpage_ptr, argument_pointers[i], len);
              argument_pointers[i] = user_esp; // Update pointer to be the user stack address
            }
          
          palloc_free_page(cmdline_copy);

          // 3. Align stack pointer to a multiple of 4
          while ((uintptr_t)kpage_ptr % 4 != 0)
            {
              kpage_ptr--;
              user_esp--;
              *kpage_ptr = 0;
            }

          // 4. Push null pointer sentinel for argv
          kpage_ptr -= sizeof(char *);
          user_esp -= sizeof(char *);
          *((char **)kpage_ptr) = NULL;

          // 5. Push argument pointers (argv)
          for (int i = arg_count - 1; i >= 0; i--)
            {
              kpage_ptr -= sizeof(char *);
              user_esp -= sizeof(char *);
              *((char **)kpage_ptr) = argument_pointers[i];
            }

          // 6. Push argv start address
          char **argv_start = (char **)user_esp;
          kpage_ptr -= sizeof(char **);
          user_esp -= sizeof(char **);
          *((char ***)kpage_ptr) = argv_start;

          // 7. Push argc
          kpage_ptr -= sizeof(int);
          user_esp -= sizeof(int);
          *((int *)kpage_ptr) = arg_count;

          // 8. Push fake return address
          kpage_ptr -= sizeof(void *);
          user_esp -= sizeof(void *);
          *((void **)kpage_ptr) = NULL;

          *esp = user_esp;
        }
      else
        palloc_free_page (kpage);
    }
  return success;
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
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map it. */
  return (pagedir_get_page (t->pagedir, upage) == NULL && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* Returns the child record for the given TID in the current thread's
   children list, or NULL if not found. */
static struct child_rec *
get_child_rec (tid_t tid)
{
  return get_child_rec_in_list(thread_current(), tid);
}

/* Returns the child record for the given TID in the specified thread's
   children list, or NULL if not found. */
static struct child_rec *
get_child_rec_in_list (struct thread *t, tid_t tid)
{
  struct list_elem *e;
  
  for (e = list_begin (&t->children); e != list_end (&t->children);
       e = list_next (e))
    {
      struct child_rec *child_rec = list_entry (e, struct child_rec, elem);
      if (child_rec->tid == tid)
        return child_rec;
    }
  
  return NULL;
}