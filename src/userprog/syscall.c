#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "threads/synch.h"      // For locks
#include "userprog/process.h"   // For process_execute, process_wait

/* Global file system lock to ensure thread-safe access */
static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

static bool isUserPtrSafe(const void *userPtr);
static bool isUserStringSafe(const char *str);
static void terminateProcess(void);

/* Safe user memory access helpers */
static int get_user(const uint8_t *uaddr);
static bool put_user(uint8_t *udst, uint8_t byte);
static bool copy_in(void *dst, const void *usrc, size_t n);
static bool validate_buf(const void *uaddr, size_t n);
static bool validate_cstr(const char *s);

/* File descriptor helpers */
static int alloc_fd(struct thread *t);
static struct file* fd_get(int fd);

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  int syscall_num;
  
  // Safely read the system call number from user stack
  if (!copy_in(&syscall_num, f->esp, sizeof syscall_num)) {
    terminateProcess();
    return;
  }

  switch (syscall_num)
  {
    case SYS_HALT:
      shutdown_power_off();
      NOT_REACHED();
      break;

    case SYS_EXIT:
      {
        int exit_code;
        
        // Safely copy exit code from user stack
        if (!copy_in(&exit_code, f->esp + 4, sizeof exit_code)) {
          terminateProcess();
          return;
        }
        
        thread_current()->exitStatus = exit_code;
        thread_exit();
      }
      break;

    case SYS_CREATE:
      {
        uint32_t ufile;
        unsigned initial_size;
        
        // Safely copy arguments
        if (!copy_in(&ufile, f->esp + 4, sizeof ufile) ||
            !copy_in(&initial_size, f->esp + 8, sizeof initial_size)) {
          terminateProcess();
          return;
        }
        
        const char *file = (const char *)ufile;
        
        // Validate the filename string
        if (!validate_cstr(file)) {
          terminateProcess();
          return;
        }
        
        // Check for empty filename or name too long (NAME_MAX is 14)
        if (file[0] == '\0' || strlen(file) > 14) {
          f->eax = false;
          break;
        }
        
        lock_acquire(&filesys_lock);
        // Check if file already exists - if so, return false
        struct file *existing = filesys_open(file);
        if (existing) {
          file_close(existing);
          f->eax = false;
        } else {
          f->eax = filesys_create(file, initial_size);
        }
        lock_release(&filesys_lock);
      }
      break;

    case SYS_REMOVE:
      {
        uint32_t ufile;
        
        // Safely copy argument
        if (!copy_in(&ufile, f->esp + 4, sizeof ufile)) {
          terminateProcess();
          return;
        }
        
        const char *file = (const char *)ufile;
        
        // Validate the filename string
        if (!validate_cstr(file)) {
          terminateProcess();
          return;
        }
        
        // Check for empty filename or name too long (NAME_MAX is 14)
        if (file[0] == '\0' || strlen(file) > 14) {
          f->eax = false;
          break;
        }
        
        lock_acquire(&filesys_lock);
        f->eax = filesys_remove(file);
        lock_release(&filesys_lock);
      }
      break;

    case SYS_OPEN:
      {
        uint32_t ufile;
        
        // Safely copy argument
        if (!copy_in(&ufile, f->esp + 4, sizeof ufile)) {
          terminateProcess();
          return;
        }
        
        const char *file = (const char *)ufile;
        
        // Validate the filename string
        if (!validate_cstr(file)) {
          terminateProcess();
          return;
        }
        
        // Check for empty filename or name too long (NAME_MAX is 14)
        if (file[0] == '\0' || strlen(file) > 14) {
          f->eax = -1;
          break;
        }
        
        lock_acquire(&filesys_lock);
        struct file *opened_file = filesys_open(file);
        lock_release(&filesys_lock);
        
        if (opened_file == NULL) {
          f->eax = -1;
        } else {
          struct thread *cur = thread_current();
          int fd = alloc_fd(cur);
          
          if (fd == -1) {
            lock_acquire(&filesys_lock);
            file_close(opened_file);
            lock_release(&filesys_lock);
            f->eax = -1;
          } else {
            cur->fd_table[fd] = opened_file;
            f->eax = fd;
          }
        }
      }
      break;

    case SYS_FILESIZE:
      {
        int fd;
        
        // Safely copy argument
        if (!copy_in(&fd, f->esp + 4, sizeof fd)) {
          terminateProcess();
          return;
        }
        
        struct file *file = fd_get(fd);
        if (!file) {
          f->eax = -1;
        } else {
          lock_acquire(&filesys_lock);
          f->eax = file_length(file);
          lock_release(&filesys_lock);
        }
      }
      break;

    case SYS_READ:
      {
        int fd;
        uint32_t ubuf;
        unsigned size;
        
        // Safely copy arguments from user stack
        if (!copy_in(&fd, f->esp + 4, sizeof fd) ||
            !copy_in(&ubuf, f->esp + 8, sizeof ubuf) ||
            !copy_in(&size, f->esp + 12, sizeof size)) {
          terminateProcess();
          return;
        }
        
        void *buffer = (void*)ubuf;
        
        // Validate the buffer is accessible
        if (size > 0 && !validate_buf(buffer, size)) {
          terminateProcess();
          return;
        }
        
        if (fd == 0) {
          // Read from STDIN
          unsigned i;
          uint8_t *buf = (uint8_t *)buffer;
          for (i = 0; i < size; i++) {
            buf[i] = input_getc();
          }
          f->eax = size;
        } else if (fd == 1) {
          // Can't read from STDOUT
          f->eax = -1;
        } else {
          struct file *file = fd_get(fd);
          if (!file) {
            f->eax = -1;
          } else {
            lock_acquire(&filesys_lock);
            f->eax = file_read(file, buffer, size);
            lock_release(&filesys_lock);
          }
        }
      }
      break;

    case SYS_WRITE:
      {
        int fd;
        uint32_t ubuf;
        unsigned size;
        
        // Safely copy arguments from user stack
        if (!copy_in(&fd, f->esp + 4, sizeof fd) ||
            !copy_in(&ubuf, f->esp + 8, sizeof ubuf) ||
            !copy_in(&size, f->esp + 12, sizeof size)) {
          terminateProcess();
          return;
        }
        
        const void *buffer = (const void*)ubuf;
        
        // Handle size == 0 case
        if (size == 0) {
          f->eax = 0;
          break;
        }
        
        // Validate the buffer is accessible
        if (!validate_buf(buffer, size)) {
          terminateProcess();
          return;
        }
        
        if (fd == 1) {
          // Write to STDOUT
          putbuf(buffer, size);
          f->eax = size;
        } else if (fd == 0) {
          // Can't write to STDIN
          f->eax = -1;
        } else {
          struct file *file = fd_get(fd);
          if (!file) {
            f->eax = -1;
          } else {
            lock_acquire(&filesys_lock);
            f->eax = file_write(file, buffer, size);
            lock_release(&filesys_lock);
          }
        }
      }
      break;

    case SYS_SEEK:
      {
        int fd;
        unsigned position;
        
        // Safely copy arguments
        if (!copy_in(&fd, f->esp + 4, sizeof fd) ||
            !copy_in(&position, f->esp + 8, sizeof position)) {
          terminateProcess();
          return;
        }
        
        struct file *file = fd_get(fd);
        if (file) {
          lock_acquire(&filesys_lock);
          file_seek(file, position);
          lock_release(&filesys_lock);
        }
      }
      break;

    case SYS_TELL:
      {
        int fd;
        
        // Safely copy argument
        if (!copy_in(&fd, f->esp + 4, sizeof fd)) {
          terminateProcess();
          return;
        }
        
        struct file *file = fd_get(fd);
        if (!file) {
          f->eax = -1;
        } else {
          lock_acquire(&filesys_lock);
          f->eax = file_tell(file);
          lock_release(&filesys_lock);
        }
      }
      break;

    case SYS_CLOSE:
      {
        int fd;
        
        // Safely copy argument
        if (!copy_in(&fd, f->esp + 4, sizeof fd)) {
          terminateProcess();
          return;
        }
        
        struct thread *cur = thread_current();
        if (fd >= 2 && fd < 128 && cur->fd_table[fd] != NULL) {
          struct file *file = cur->fd_table[fd];
          cur->fd_table[fd] = NULL;
          lock_acquire(&filesys_lock);
          file_close(file);
          lock_release(&filesys_lock);
        }
      }
      break;

    case SYS_EXEC:
      {
        uint32_t ucmd;
        
        // Safely copy argument
        if (!copy_in(&ucmd, f->esp + 4, sizeof ucmd)) {
          terminateProcess();
          return;
        }
        
        const char *cmd_line = (const char *)ucmd;
        
        // Validate the command line string
        if (!validate_cstr(cmd_line)) {
          terminateProcess();
          return;
        }
        
        lock_acquire(&filesys_lock);
        f->eax = process_execute(cmd_line);
        lock_release(&filesys_lock);
      }
      break;

    case SYS_WAIT:
      {
        int pid;
        
        // Safely copy argument
        if (!copy_in(&pid, f->esp + 4, sizeof pid)) {
          terminateProcess();
          return;
        }
        
        f->eax = process_wait(pid);
      }
      break;

    default:
      // Unknown system call. Terminate the process.
      terminateProcess();
      break;
  }
}


/* helper function to validate a user-provided pointer */
static bool
isUserPtrSafe (const void *userPtr)
{
  if (userPtr == NULL)
    return false;

  if (!is_user_vaddr (userPtr))
    return false;

  if (pagedir_get_page (thread_current ()->pagedir, userPtr) == NULL)
    return false;

  return true;
}

/* helper function to validate a user-provided string */
static bool
isUserStringSafe (const char *str)
{
  if (!isUserPtrSafe(str))
    return false;
  
  // Check each character until we find null terminator
  const char *ptr = str;
  while (isUserPtrSafe(ptr)) {
    if (*ptr == '\0')
      return true;
    ptr++;
  }
  
  return false;  // String not null-terminated in user memory
}

/*
 * Safely copies a buffer of a given size from user space to kernel space.
 * Checks each byte of the source buffer for validity before copying.
 * Returns true if success, false if any memory access violation.
 */
static bool
copyInUserBuffer (void *kernelDest, const void *userSrc, size_t size)
{
  uint8_t *srcByte = (uint8_t *) userSrc;

  // TODO: Loop 'size' times.
  // In each iteration, check if the current source byte (srcByte + i) is safe
  // using our isUserPtrSafe() helper.
  // If it's not safe, return false immediately.
  
  // If all bytes in the source range are safe, then perform the copy.
  memcpy (kernelDest, userSrc, size);
  
  return true;
}

/* Safely read a byte from user memory.
   Returns the byte value (0-255) if successful, -1 if invalid address. */
static int
get_user (const uint8_t *uaddr)
{
  if (!is_user_vaddr(uaddr))
    return -1;
  
  void *page = pagedir_get_page(thread_current()->pagedir, uaddr);
  if (!page)
    return -1;
  
  return *uaddr;
}

/* Safely write a byte to user memory.
   Returns true if successful, false if invalid address. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  if (!is_user_vaddr(udst))
    return false;
  
  void *page = pagedir_get_page(thread_current()->pagedir, udst);
  if (!page)
    return false;
  
  *udst = byte;
  return true;
}

/* Safely copy data from user space to kernel space.
   Returns true if successful, false if any address is invalid. */
static bool
copy_in (void *dst, const void *usrc, size_t n)
{
  uint8_t *d = dst;
  const uint8_t *s = usrc;
  
  for (size_t i = 0; i < n; i++) {
    int ch = get_user(s + i);
    if (ch < 0)
      return false;
    d[i] = (uint8_t)ch;
  }
  
  return true;
}

/* Validate that a buffer in user space is fully accessible.
   Returns true if all bytes are readable, false otherwise. */
static bool
validate_buf (const void *uaddr, size_t n)
{
  const uint8_t *p = uaddr;
  
  for (size_t i = 0; i < n; i++) {
    if (get_user(p + i) < 0)
      return false;
  }
  
  return true;
}

/* Validate that a C-string in user space is fully accessible and null-terminated.
   Returns true if valid, false otherwise. */
static bool
validate_cstr (const char *s)
{
  for (;; s++) {
    int ch = get_user((const uint8_t*)s);
    if (ch < 0)
      return false;
    if (ch == '\0')
      return true;
  }
}

/* Allocate a file descriptor for the current thread.
   Returns fd number (>= 2) on success, -1 if table is full. */
static int
alloc_fd (struct thread *t)
{
  for (int i = 2; i < 128; i++) {
    if (t->fd_table[i] == NULL)
      return i;
  }
  return -1;
}

/* Get the file pointer for a given fd in the current thread.
   Returns file pointer or NULL if fd is invalid/closed. */
static struct file*
fd_get (int fd)
{
  struct thread *t = thread_current();
  
  if (fd < 2 || fd >= 128)
    return NULL;
  
  return t->fd_table[fd];
}

/* Terminates the current user process (terminating status : -1) */
static void
terminateProcess (void)
{
  thread_current ()->exitStatus = -1;
  thread_exit ();
}

/* Acquire the global file system lock */
void
filesys_lock_acquire (void)
{
  lock_acquire(&filesys_lock);
}

/* Release the global file system lock */
void
filesys_lock_release (void)
{
  lock_release(&filesys_lock);
}