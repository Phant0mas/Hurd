/*
   Copyright (C) 1995, 1996, 1998, 1999 Free Software Foundation, Inc.
   Written by Michael I. Bushnell, p/BSG.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA. */

#include <cthreads.h>
#include <assert.h>
#include <errno.h>
#include <hurd/trivfs.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>

#undef MDMBUF
#undef ECHO
#undef TOSTOP
#undef FLUSHO
#undef PENDIN
#undef NOFLSH
#include <termios.h>

#define CHAR_EOT '\004'		/* C-d */
#define CHAR_DC1 '\021'		/* C-q */
#define CHAR_DC2 '\022'		/* C-r */
#define CHAR_DC3 '\023'		/* C-s */
#define CHAR_USER_QUOTE '\377'	/* break quoting, etc. */

/* This bit specifies control */
#define CTRL_BIT 0x40

/* XXX These belong in <termios.h> */
#ifdef	IUCLC
#define ILCASE	IUCLC
#else
#define ILCASE (1 << 14)
#endif
#ifdef	OLCUC
#define OLCASE	OLCUC
#else
#define OLCASE (1 << 9)
#endif
#define OTILDE (1 << 10)

/* used in mdmctl device call */
#define MDMCTL_BIS 0
#define MDMCTL_BIC 1
#define MDMCTL_SET 2

/* Directly user-visible state */
struct termios termstate;

/* Other state with the following bits: */
long termflags;

#define USER_OUTPUT_SUSP  0x00000001 /* user has suspended output */
#define TTY_OPEN	  0x00000002 /* someone has us open */
#define LAST_SLASH	  0x00000004 /* last input char was \ */
#define LAST_LNEXT        0x00000008 /* last input char was VLNEXT */
#define INSIDE_HDERASE    0x00000010 /* inside \.../ hardcopy erase pair */
#define SENT_VSTOP        0x00000020 /* we've sent VSTOP to IXOFF peer */
#define FLUSH_OUTPUT      0x00000040 /* user wants output flushed */
#define NO_CARRIER        0x00000080 /* carrier is absent */
#define EXCL_USE          0x00000100 /* user accessible exclusive use */
#define NO_OWNER          0x00000200 /* there is no foreground_id */
#define ICKY_ASYNC	  0x00000400 /* some user has set O_ASYNC */

#define QUEUE_LOWAT 100
#define QUEUE_HIWAT 300

/* Global lock */
struct mutex global_lock;

/* Wakeup when NO_CARRIER turns off */
struct condition carrier_alert;

/* Wakeup for select */
struct condition select_alert;

/* Bucket for all our ports. */
struct port_bucket *term_bucket;

/* Port class for tty control ports */
struct port_class *tty_cntl_class;

/* Port class for tty I/O ports */
struct port_class *tty_class;

/* Port class for ctty ID ports */
struct port_class *cttyid_class;

/* Port class for pty master ports */
struct port_class *pty_class;

/* Port class for pty control ports */
struct port_class *pty_cntl_class;

/* Trivfs control structure for the tty */
struct trivfs_control *termctl;

/* Trivfs control structure for the pty */
struct trivfs_control *ptyctl;

/* Mach device name for this terminal */
char *pterm_name;

/* The queues we use */
struct queue *inputq, *rawq, *outputq;

/* Plain pass-through input */
int remote_input_mode;

/* External processing mode */
int external_processing;

/* Terminal owner */
uid_t term_owner;

/* Terminal group */
uid_t term_group;

/* Terminal mode */
mode_t term_mode;


/* Functions a bottom half defines */
struct bottomhalf
{
  void (*start_output) (void);
  void (*set_break) (void);
  void (*clear_break) (void);
  void (*abandon_physical_output) (void);
  void (*suspend_physical_output) (void);
  int (*pending_output_size) (void);
  void (*notice_input_flushed) (void);
  error_t (*assert_dtr) (void);
  void (*desert_dtr) (void);
  void (*set_bits) (void);
  void (*mdmctl) (int, int);
  int (*mdmstate) (void);
};

struct bottomhalf *bottom;
extern struct bottomhalf devio_bottom, ptyio_bottom;


/* Character queues */
#define QUEUE_QUOTE_MARK 0xf000
typedef short quoted_char;
struct queue
{
  int susp;
  int lowat;
  int hiwat;
  short *cs, *ce;
  int arraylen;
  struct condition *wait;
  quoted_char array[0];
};

struct queue *create_queue (int size, int lowat, int hiwat);

/* Return the number of characters in Q. */
extern inline int
qsize (struct queue *q)
{
  return q->ce - q->cs;
}

/* Return nonzero if characters can be added to Q. */
extern inline int
qavail (struct queue *q)
{
  return !q->susp;
}

/* Flush all the characters from Q. */
extern inline void
clear_queue (struct queue *q)
{
  q->susp = 0;
  q->cs = q->ce = q->array;
  condition_broadcast (q->wait);
}

/* Should be below, but inlines need it. */
void call_asyncs (int dir);

/* Return the next character off Q; leave the quoting bit on. */
extern inline quoted_char
dequeue_quote (struct queue *q)
{
  int beep = 0;

  assert (qsize (q));
  if (q->susp && (qsize (q) < q->lowat))
    {
      q->susp = 0;
      beep = 1;
    }
  if (qsize (q) == 1)
    beep = 1;
  if (beep)
    {
      condition_broadcast (q->wait);
      if (q == outputq)
	call_asyncs (O_WRITE);
    }
  return *q->cs++;
}

/* Return the next character off Q. */
extern inline char
dequeue (struct queue *q)
{
  return dequeue_quote (q) & ~QUEUE_QUOTE_MARK;
}

struct queue *reallocate_queue (struct queue *);

/* Add C to *QP. */
extern inline void
enqueue_internal (struct queue **qp, quoted_char c)
{
  struct queue *q = *qp;

  if (q->ce - q->array == q->arraylen)
    q = *qp = reallocate_queue (q);

  *q->ce++ = c;

  if (qsize (q) == 1)
    {
      condition_broadcast (q->wait);
      if (q == inputq)
	call_asyncs (O_READ);
    }

  if (!q->susp && (qsize (q) > q->hiwat))
    q->susp = 1;
}

/* Add C to *QP. */
extern inline void
enqueue (struct queue **qp, char c)
{
  enqueue_internal (qp, c);
}

/* Add C to *QP, marking it with a quote. */
extern inline void
enqueue_quote (struct queue **qp, char c)
{
  enqueue_internal (qp, c | QUEUE_QUOTE_MARK);
}

/* Return the unquoted version of a quoted_char. */
extern inline char
unquote_char (quoted_char c)
{
  return c & ~QUEUE_QUOTE_MARK;
}

/* Tell if a quoted_char is actually quoted. */
extern inline int
char_quoted_p (quoted_char c)
{
  return c & QUEUE_QUOTE_MARK;
}

/* Remove the most recently enqueue character from Q; leaving
   the quote mark on. */
extern inline short
queue_erase (struct queue *q)
{
  short answer;
  int beep = 0;

  assert (qsize (q));
  answer = *--q->ce;
  if (q->susp && (qsize (q) < q->lowat))
    {
      q->susp = 0;
      beep = 1;
    }
  if (qsize (q) == 0)
    beep = 1;
  if (beep)
    condition_broadcast (q->wait);
  return answer;
}


/* Functions devio is supposed to call */
int input_character (int);
void report_carrier_on (void);
void report_carrier_off (void);
void report_carrier_error (error_t);


/* Other decls */
void drop_output (void);
void send_signal (int);
error_t drain_output ();
void output_character (int);
void copy_rawq (void);
void rescan_inputq (void);
void write_character (int);
void init_users (void);

/* Call this before using ptyio_bottom.  */
void ptyio_init (void);

/* kludge--these are pty versions of trivfs_S_io_* functions called by
   the real functions in users.c to do work for ptys.  */
error_t pty_io_write (struct trivfs_protid *, char *,
		      mach_msg_type_number_t, mach_msg_type_number_t *);
error_t pty_io_read (struct trivfs_protid *, char **,
		     mach_msg_type_number_t *, mach_msg_type_number_t);
error_t pty_io_readable (int *);
error_t pty_io_select (struct trivfs_protid *, mach_port_t, int *, int *);
error_t pty_open_hook (struct trivfs_control *, struct iouser *, int);
error_t pty_po_create_hook (struct trivfs_peropen *);
error_t pty_po_destroy_hook (struct trivfs_peropen *);
