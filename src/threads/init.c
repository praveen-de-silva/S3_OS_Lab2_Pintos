#include "threads/init.h"
#include <console.h>
#include <debug.h>
#include <inttypes.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/kbd.h"
#include "devices/input.h"
#include "devices/serial.h"
#include "devices/shutdown.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "devices/rtc.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#else
#include "tests/threads/tests.h"
#endif
#ifdef FILESYS
#include "devices/block.h"
#include "devices/ide.h"
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

/* Page directory with kernel mappings only. */
uint32_t *init_page_dir;

#ifdef FILESYS
/* -f: Format the file system? */
static bool format_filesys;

/* -filesys, -scratch, -swap: Names of block devices to use,
   overriding the defaults. */
static const char *filesys_bdev_name;
static const char *scratch_bdev_name;
#ifdef VM
static const char *swap_bdev_name;
#endif
#endif /* FILESYS */

/* -ul: Maximum number of pages to put into palloc's user pool. */
static size_t user_page_limit = SIZE_MAX;

static void bss_init (void);
static void paging_init (void);

/* Forward declarations of shell functions - now always defined */
static void shell_readline(char *buffer, size_t size);

static char **read_command_line (void);
static char **parse_options (char **argv);
static void run_actions (char **argv);
static void usage (void);

#ifdef FILESYS
static void locate_block_devices (void);
static void locate_block_device (enum block_type, const char *name);
#endif

int pintos_init (void) NO_RETURN;

/* Pintos main entry point. */
int
pintos_init (void)
{
  char **argv;

  /* Clear BSS. */  
  bss_init ();

  /* Break command line into arguments and parse options. */
  argv = read_command_line ();
  argv = parse_options (argv);

  /* Initialize ourselves as a thread so we can use locks,
     then enable console locking. */
  thread_init ();
  console_init ();  

  /* Greet user. */
  printf ("Pintos booting with %'"PRIu32" kB RAM...\n",
          init_ram_pages * PGSIZE / 1024);

  /* Initialize memory system. */
  palloc_init (user_page_limit);
  malloc_init ();
  paging_init ();

  /* Segmentation. */
#ifdef USERPROG
  tss_init ();
  gdt_init ();
#endif

  /* Initialize interrupt handlers. */
  intr_init ();
  timer_init ();
  kbd_init ();
  input_init ();
#ifdef USERPROG
  exception_init ();
  syscall_init ();
#endif

  /* Start thread scheduler and enable interrupts. */
  thread_start ();
  serial_init_queue ();
  timer_calibrate ();

#ifdef FILESYS
  /* Initialize file system. */
  ide_init ();
  locate_block_devices ();
  filesys_init (format_filesys);
#endif

  printf ("Boot complete.\n");
  if (*argv != NULL) {
    /* Run actions specified on kernel command line. */
    run_actions (argv);
  } else {
    // /* No command line passed to kernel. Run interactively. */
    // shell_run();
     /* ==== Direct shell loop here ==== */
    printf("\nCS2042 PintOS Shell\n");
    printf("Type 'help' for commands\n\n");

    while (true) {
      char cmd[128];
      printf("CS2042> ");
      shell_readline(cmd, sizeof(cmd));  // use your readline helper

      if (strcmp(cmd, "whoami") == 0) {
        printf("Praveen De Silva - 230123K\n");
      } else if (strcmp(cmd, "shutdown") == 0) {
        shutdown_power_off();
      } else if (strcmp(cmd, "time") == 0) {
        printf("Ticks since boot: %"PRId64"\n", timer_ticks());
      } else if (strcmp(cmd, "ram") == 0) {
        printf("RAM: %"PRIu32" kB\n", init_ram_pages * PGSIZE / 1024);
      } else if (strcmp(cmd, "thread") == 0) {
        thread_print_stats();
      } else if (strcmp(cmd, "priority") == 0) {
        printf("Priority: %d\n", thread_current()->priority);
      } else if (strcmp(cmd, "exit") == 0) {
        break;  // exit shell loop
      } else if (strcmp(cmd, "help") == 0) {
        printf("Commands: whoami, shutdown, time, ram, thread, priority, exit\n");
      } else if (strlen(cmd) > 0) {
        printf("Unknown command: %s\n", cmd);
      }
    }
  }

  /* Finish up. */
  shutdown ();
  thread_exit ();
}

/* Clear the "BSS", a segment that should be initialized to zeros. */
static void
bss_init (void) 
{
  extern char _start_bss, _end_bss;
  memset (&_start_bss, 0, &_end_bss - &_start_bss);
}

/* Set up paging structures */
static void
paging_init (void)
{
  uint32_t *pd, *pt;
  size_t page;
  extern char _start, _end_kernel_text;

  pd = init_page_dir = palloc_get_page (PAL_ASSERT | PAL_ZERO);
  pt = NULL;
  for (page = 0; page < init_ram_pages; page++)
    {
      uintptr_t paddr = page * PGSIZE;
      char *vaddr = ptov (paddr);
      size_t pde_idx = pd_no (vaddr);
      size_t pte_idx = pt_no (vaddr);
      bool in_kernel_text = &_start <= vaddr && vaddr < &_end_kernel_text;

      if (pd[pde_idx] == 0)
        {
          pt = palloc_get_page (PAL_ASSERT | PAL_ZERO);
          pd[pde_idx] = pde_create (pt);
        }

      pt[pte_idx] = pte_create_kernel (vaddr, !in_kernel_text);
    }

  asm volatile ("movl %0, %%cr3" : : "r" (vtop (init_page_dir)));
}

/* Reads command line */
static char **
read_command_line (void) 
{
  static char *argv[LOADER_ARGS_LEN / 2 + 1];
  char *p, *end;
  int argc;
  int i;

  argc = *(uint32_t *) ptov (LOADER_ARG_CNT);
  p = ptov (LOADER_ARGS);
  end = p + LOADER_ARGS_LEN;
  for (i = 0; i < argc; i++) 
    {
      if (p >= end)
        PANIC ("command line arguments overflow");

      argv[i] = p;
      p += strnlen (p, end - p) + 1;
    }
  argv[argc] = NULL;

  printf ("Kernel command line:");
  for (i = 0; i < argc; i++)
    if (strchr (argv[i], ' ') == NULL)
      printf (" %s", argv[i]);
    else
      printf (" '%s'", argv[i]);
  printf ("\n");

  return argv;
}

/* Parses command line options */
static char **
parse_options (char **argv) 
{
  for (; *argv != NULL && **argv == '-'; argv++)
    {
      char *save_ptr;
      char *name = strtok_r (*argv, "=", &save_ptr);
      char *value = strtok_r (NULL, "", &save_ptr);
      
      if (!strcmp (name, "-h"))
        usage ();
      else if (!strcmp (name, "-q"))
        shutdown_configure (SHUTDOWN_POWER_OFF);
      else if (!strcmp (name, "-r"))
        shutdown_configure (SHUTDOWN_REBOOT);
#ifdef FILESYS
      else if (!strcmp (name, "-f"))
        format_filesys = true;
      else if (!strcmp (name, "-filesys"))
        filesys_bdev_name = value;
      else if (!strcmp (name, "-scratch"))
        scratch_bdev_name = value;
#ifdef VM
      else if (!strcmp (name, "-swap"))
        swap_bdev_name = value;
#endif
#endif
      else if (!strcmp (name, "-rs"))
        random_init (atoi (value));
      else if (!strcmp (name, "-mlfqs"))
        thread_mlfqs = true;
#ifdef USERPROG
      else if (!strcmp (name, "-ul"))
        user_page_limit = atoi (value);
#endif
      else
        PANIC ("unknown option `%s' (use -h for help)", name);
    }

  random_init (rtc_get_time ());
  
  return argv;
}

/* Runs a task or test */
static void
run_task (char **argv)
{
  const char *task = argv[1];
  
  printf ("Executing '%s':\n", task);
#ifdef USERPROG
  process_wait (process_execute (task));
#else
  run_test (task);
#endif
  printf ("Execution of '%s' complete.\n", task);
}

/* Runs all actions on the command line */
static void
run_actions (char **argv) 
{
  struct action 
    {
      char *name;
      int argc;
      void (*function) (char **argv);
    };

  static const struct action actions[] = 
    {
      {"run", 2, run_task},
#ifdef FILESYS
      {"ls", 1, fsutil_ls},
      {"cat", 2, fsutil_cat},
      {"rm", 2, fsutil_rm},
      {"extract", 1, fsutil_extract},
      {"append", 2, fsutil_append},
#endif
      {NULL, 0, NULL},
    };

  while (*argv != NULL)
    {
      const struct action *a;
      int i;

      for (a = actions; ; a++)
        if (a->name == NULL)
          PANIC ("unknown action `%s' (use -h for help)", *argv);
        else if (!strcmp (*argv, a->name))
          break;

      for (i = 1; i < a->argc; i++)
        if (argv[i] == NULL)
          PANIC ("action `%s' requires %d argument(s)", *argv, a->argc - 1);

      a->function (argv);
      argv += a->argc;
    }
}

/* Prints usage help and powers off */
static void
usage (void)
{
  printf ("\nCommand line syntax: [OPTION...] [ACTION...]\n"
          "Options must precede actions.\n"
          "Actions are executed in the order specified.\n"
          "\nAvailable actions:\n"
#ifdef USERPROG
          "  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
          "  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
          "  ls                 List files in the root directory.\n"
          "  cat FILE           Print FILE to the console.\n"
          "  rm FILE            Delete FILE.\n"
          "Use these actions indirectly via `pintos' -g and -p options:\n"
          "  extract            Untar from scratch device into file system.\n"
          "  append FILE        Append FILE to tar file on scratch device.\n"
#endif
          "\nOptions:\n"
          "  -h                 Print this help message and power off.\n"
          "  -q                 Power off VM after actions or on panic.\n"
          "  -r                 Reboot after actions.\n"
#ifdef FILESYS
          "  -f                 Format file system device during startup.\n"
          "  -filesys=BDEV      Use BDEV for file system instead of default.\n"
          "  -scratch=BDEV      Use BDEV for scratch instead of default.\n"
#ifdef VM
          "  -swap=BDEV         Use BDEV for swap instead of default.\n"
#endif
#endif
          "  -rs=SEED           Set random number seed to SEED.\n"
          "  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
          "  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
          );
  shutdown_power_off ();
}

#ifdef FILESYS
static void
locate_block_devices (void)
{
  locate_block_device (BLOCK_FILESYS, filesys_bdev_name);
  locate_block_device (BLOCK_SCRATCH, scratch_bdev_name);
#ifdef VM
  locate_block_device (BLOCK_SWAP, swap_bdev_name);
#endif
}

static void
locate_block_device (enum block_type role, const char *name)
{
  struct block *block = NULL;

  if (name != NULL)
    {
      block = block_get_by_name (name);
      if (block == NULL)
        PANIC ("No such block device \"%s\"", name);
    }
  else
    {
      for (block = block_first (); block != NULL; block = block_next (block))
        if (block_type (block) == role)
          break;
    }

  if (block != NULL)
    {
      printf ("%s: using %s\n", block_type_name (role), block_name (block));
      block_set_role (role, block);
    }
}
#endif

/* ==== Shell implementation (always compiled) ==== */
static void shell_readline(char *buf, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        char c = input_getc();

        /* Handle backspace */
        if (c == '\b' && i > 0) {
            i--;
            printf("\b \b"); // Erase character from console
        }
        /* Handle Enter */
        else if (c == '\n' || c == '\r') {
            buf[i] = '\0';
            printf("\n");
            return;
        }
        /* Handle printable characters */
        else if (c >= ' ' && c <= '~') {
            buf[i++] = c;
            putbuf(&c, 1);  // Use putbuf instead of console_putc
        }
    }
    buf[size-1] = '\0'; // Ensure null termination
}
