/*
   Copyright (C) 1995,96,97,98,99,2000,01,02 Free Software Foundation, Inc.
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

#include "term.h"
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <fcntl.h>
#include <hurd/trivfs.h>
#include <cthreads.h>
#include <hurd.h>
#include <stdio.h>
#include <hurd/iohelp.h>
#include <hurd/fshelp.h>
#include "ourmsg_U.h"


#undef ECHO
#undef MDMBUF
#undef TOSTOP
#undef FLUSHO
#undef PENDIN
#undef NOFLSH

#include "term_S.h"
#include "tioctl_S.h"
#include <sys/ioctl.h>

#define TTYDEFCHARS
#include <sys/ttydefaults.h>

/* Count of active opens */
int nperopens;

/* io_async requests */
struct async_req
{
  mach_port_t notify;
  struct async_req *next;
};
struct async_req *async_requests;

/* Number of peropens that have set the ICKY_ASYNC flags.  */
static int num_icky_async_peropens = 0;

mach_port_t async_icky_id;
mach_port_t async_id;
struct port_info *cttyid;
int foreground_id;

struct winsize window_size;

static int sigs_in_progress;
static struct condition input_sig_wait = CONDITION_INITIALIZER;
static int input_sig_wakeup;

static error_t carrier_error;

/* Attach this on the hook of any protid that is a ctty. */
struct protid_hook
{
  int refcnt;
  pid_t pid, pgrp;
};

void
init_users ()
{
  errno = ports_create_port (cttyid_class, term_bucket,
			     sizeof (struct port_info), &cttyid);
  if (errno)
    {
      perror ("Allocating cttyid");
      exit (1);
    }

  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE,
		      &async_icky_id);
  /* Add a send right, since hurd_sig_post needs one.  */
  mach_port_insert_right (mach_task_self (),
			  async_icky_id, async_icky_id,
			  MACH_MSG_TYPE_MAKE_SEND);

  mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE, &async_id);
  /* Add a send right, since hurd_sig_post needs one.  */
  mach_port_insert_right (mach_task_self (),
			  async_id, async_id, MACH_MSG_TYPE_MAKE_SEND);
}


static error_t
check_access_hook (struct trivfs_control *cntl,
		   struct iouser *user,
		   mach_port_t realnode,
		   int *allowed)
{
  struct stat st;

  mutex_lock (&global_lock);

  st.st_uid = term_owner;
  st.st_gid = term_group;
  st.st_mode = term_mode;

  *allowed = 0;
  if (fshelp_access (&st, S_IREAD, user) == 0)
    *allowed |= O_READ;
  if (fshelp_access (&st, S_IWRITE, user) == 0)
    *allowed |= O_WRITE;

  mutex_unlock (&global_lock);
  return 0;
}
error_t (*trivfs_check_access_hook) (struct trivfs_control *, struct iouser *,
				     mach_port_t, int *)
     = check_access_hook;

static error_t
open_hook (struct trivfs_control *cntl,
	   struct iouser *user,
	   int flags)
{
  static int open_count = 0;	/* XXX debugging */
  int cancel = 0;
  error_t err;

  if (cntl == ptyctl)
    return pty_open_hook (cntl, user, flags);

  if ((flags & (O_READ|O_WRITE)) == 0)
    return 0;

  mutex_lock (&global_lock);

  if (!(termflags & TTY_OPEN))
    {
      memset (&termstate, 0, sizeof termstate);

      /* This is different from BSD: we don't turn on ISTRIP,
	 and we use CS8 rather than CS7|PARENB.  */
      termstate.c_iflag |= BRKINT | ICRNL | IMAXBEL | IXON | IXANY;
      termstate.c_oflag |= OPOST | ONLCR | OXTABS;
      termstate.c_lflag |= (ECHO | ICANON | ISIG | IEXTEN
			    | ECHOE|ECHOKE|ECHOCTL);
      termstate.c_cflag |= CREAD | CS8 | HUPCL;

      memcpy (termstate.c_cc, ttydefchars, NCCS);

      memset (&window_size, 0, sizeof window_size);

      termflags |= NO_OWNER;
    }
  else
    {
      assert (open_count > 0);	/* XXX debugging */

      if (termflags & EXCL_USE)
	{
	  mutex_unlock (&global_lock);
	  return EBUSY;
	}
    }

  open_count++;			/* XXX debugging */

  /* XXX debugging */
  assert (! (termstate.c_oflag & OTILDE));

  /* Assert DTR if necessary. */
  if (termflags & NO_CARRIER)
    {
      err = (*bottom->assert_dtr) ();
      if (err)
	{
	  mutex_unlock (&global_lock);
	  return err;
	}
    }

  /* Wait for carrier to turn on. */
  while (((termflags & NO_CARRIER) && !(termstate.c_cflag & CLOCAL))
	 && !(flags & O_NONBLOCK)
	 && !cancel)
    cancel = hurd_condition_wait (&carrier_alert, &global_lock);

  if (cancel)
    {
      mutex_unlock (&global_lock);
      return EINTR;
    }

  err = carrier_error;
  carrier_error = 0;

  if (!err)
    {
      struct termios state = termstate;
      err = (*bottom->set_bits) (&state);
      if (!err)
	{
	  termstate = state;
	  termflags |= TTY_OPEN;
	}
    }

  mutex_unlock (&global_lock);
  return err;
}
error_t (*trivfs_check_open_hook) (struct trivfs_control *,
				   struct iouser *, int)
     = open_hook;

static error_t
pi_create_hook (struct trivfs_protid *cred)
{
  if (cred->pi.class == pty_class)
    return 0;

  mutex_lock (&global_lock);
  if (cred->hook)
    ((struct protid_hook *)cred->hook)->refcnt++;
  mutex_unlock (&global_lock);

  return 0;
}
error_t (*trivfs_protid_create_hook) (struct trivfs_protid *) = pi_create_hook;

static void
pi_destroy_hook (struct trivfs_protid *cred)
{
  if (cred->pi.class == pty_class)
    return;

  mutex_lock (&global_lock);
  if (cred->hook)
    {
      assert (((struct protid_hook *)cred->hook)->refcnt > 0);
      if (--((struct protid_hook *)cred->hook)->refcnt == 0)
	/* XXX don't free for now, so we can try and catch a multiple-freeing
	   bug.  */
	/* free (cred->hook) */;
    }
  mutex_unlock (&global_lock);
}
void (*trivfs_protid_destroy_hook) (struct trivfs_protid *) = pi_destroy_hook;

static error_t
po_create_hook (struct trivfs_peropen *po)
{
  if (po->cntl == ptyctl)
    return pty_po_create_hook (po);

  mutex_lock (&global_lock);
  nperopens++;
  if (po->openmodes & O_ASYNC)
    {
      termflags |= ICKY_ASYNC;
      num_icky_async_peropens++;
      call_asyncs (O_READ | O_WRITE);
    }
  mutex_unlock (&global_lock);
  return 0;
}
error_t (*trivfs_peropen_create_hook) (struct trivfs_peropen *) =
     po_create_hook;

static void
po_destroy_hook (struct trivfs_peropen *po)
{
  if (po->cntl == ptyctl)
    {
      pty_po_destroy_hook (po);
      return;
    }

  mutex_lock (&global_lock);

  if ((po->openmodes & O_ASYNC) && --num_icky_async_peropens == 0)
    termflags &= ~ICKY_ASYNC;

  nperopens--;
  if (!nperopens && (termflags & TTY_OPEN))
    {
      /* Empty queues */
      clear_queue (inputq);
      clear_queue (rawq);
      (*bottom->notice_input_flushed) ();

      drain_output ();

      /* Possibly drop carrier */
      if ((termstate.c_cflag & HUPCL) || (termflags & NO_CARRIER))
	(*bottom->desert_dtr) ();

      termflags &= ~TTY_OPEN;
    }

  mutex_unlock (&global_lock);
}
void (*trivfs_peropen_destroy_hook) (struct trivfs_peropen *)
     = po_destroy_hook;

/* Tell if CRED can do foreground terminal operations */
static inline int
fg_p (struct trivfs_protid *cred)
{
  struct protid_hook *hook = cred->hook;

  if (!hook || (termflags & NO_OWNER))
    return 1;

  if (hook->pid == foreground_id
      || hook->pgrp == -foreground_id)
    return 1;

  return 0;
}

void
trivfs_modify_stat (struct trivfs_protid *cred, struct stat *st)
{
  st->st_blksize = 512;
  st->st_fstype = FSTYPE_TERM;
  st->st_fsid = getpid ();
  st->st_ino = 0;
  st->st_rdev = rdev;
  st->st_mode = term_mode;
  st->st_uid = term_owner;
  st->st_gid = term_group;
}

/* Implement term_getctty as described in <hurd/term.defs>. */
kern_return_t
S_term_getctty (mach_port_t arg,
		mach_port_t *id,
		mach_msg_type_name_t *idtype)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket,
						  arg, tty_class);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      *id = ports_get_right (cttyid);
      *idtype = MACH_MSG_TYPE_MAKE_SEND;
      err = 0;
    }
  ports_port_deref (cred);
  mutex_unlock (&global_lock);
  return err;
}

/* Implement termctty_open_terminal as described in <hurd/term.defs>. */
kern_return_t
S_termctty_open_terminal (mach_port_t arg,
			  int flags,
			  mach_port_t *result,
			  mach_msg_type_name_t *resulttype)
{
  error_t err;
  mach_port_t new_realnode;
  struct iouser *user;
  struct trivfs_protid *newcred;
  struct port_info *pi = ports_lookup_port (term_bucket, arg, cttyid_class);

  if (!pi)
    return EOPNOTSUPP;

  assert (pi == cttyid);

  err = io_restrict_auth (termctl->underlying, &new_realnode, 0, 0, 0, 0);

  if (!err)
    {
      err = iohelp_create_empty_iouser (&user);
      if (! err)
        err = trivfs_open (termctl, user, flags, new_realnode, &newcred);
      if (!err)
	{
	  *result = ports_get_right (newcred);
	  *resulttype = MACH_MSG_TYPE_MAKE_SEND;
	  ports_port_deref (newcred);
	}
    }

  ports_port_deref (pi);
  return err;
}

/* Implement term_become_ctty as described in <hurd/term.defs>. */
kern_return_t
S_term_open_ctty (mach_port_t arg,
		    pid_t pid,
		    pid_t pgrp,
		    mach_port_t *newpt,
		    mach_msg_type_name_t *newpttype)
{
  error_t err;
  struct trivfs_protid *newcred;
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, arg, tty_class);

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  if (!cred->po->openmodes & (O_READ|O_WRITE))
    {
      mutex_unlock (&global_lock);
      err = EBADF;
    }
  else
    {
      mutex_unlock (&global_lock);
      err = trivfs_protid_dup (cred, &newcred);

      if (!err)
	{
	  struct protid_hook *hook = malloc (sizeof (struct protid_hook));

	  hook->pid = pid;
	  hook->pgrp = pgrp;
	  hook->refcnt = 1;

	  if (newcred->hook)
	    /* We inherited CRED's hook, get rid of our ref to it. */
	    pi_destroy_hook (newcred);
	  newcred->hook = hook;

	  *newpt = ports_get_right (newcred);
	  *newpttype = MACH_MSG_TYPE_MAKE_SEND;

	  ports_port_deref (newcred);
	}
    }

  ports_port_deref (cred);

  return err;
}

/* Implement chown locally; don't pass the value down to the
   underlying node. */
error_t
trivfs_S_file_chown (struct trivfs_protid *cred,
		     mach_port_t reply,
		     mach_msg_type_name_t reply_type,
		     uid_t uid,
		     gid_t gid)
{
  struct stat st;
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  /* XXX */
  st.st_uid = term_owner;
  st.st_gid = term_group;

  if (!cred->isroot)
    {
      err = fshelp_isowner (&st, cred->user);
      if (err)
	goto out;

      if ((uid != (uid_t) -1 && !idvec_contains (cred->user->uids, uid))
	  || (gid != (gid_t) -1 && !idvec_contains (cred->user->gids, gid)))
	{
	  err = EPERM;
	  goto out;
	}
    }

  /* Make the change */
  if (uid != (uid_t) -1)
    term_owner = uid;
  if (gid != (gid_t) -1)
    term_group = gid;
  err = 0;

out:
  mutex_unlock (&global_lock);
  return err;
}

/* Implement chmod locally */
error_t
trivfs_S_file_chmod (struct trivfs_protid *cred,
		     mach_port_t reply,
		     mach_msg_type_name_t reply_type,
		     mode_t mode)
{
  error_t err;
  struct stat st;

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  if (!cred->isroot)
    {
      /* XXX */
      st.st_uid = term_owner;
      st.st_gid = term_group;

      err = fshelp_isowner (&st, cred->user);
      if (err)
	goto out;

      mode &= ~S_ISVTX;

      if (!idvec_contains (cred->user->uids, term_owner))
	mode &= ~S_ISUID;

      if (!idvec_contains (cred->user->gids, term_group))
	mode &= ~S_ISUID;
    }

  term_mode = ((mode & ~S_IFMT & ~S_ITRANS & ~S_ISPARE) | S_IFCHR | S_IROOT);
  err = 0;

out:
  mutex_unlock (&global_lock);
  return err;
}


/* Called for user writes to the terminal as described
   in <hurd/io.defs>. */
error_t
trivfs_S_io_write (struct trivfs_protid *cred,
		   mach_port_t reply,
		   mach_msg_type_name_t replytype,
		   char *data,
		   u_int datalen,
		   off_t offset,
		   int *amt)
{
  int i;
  int cancel;
  error_t err = 0;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class == pty_class)
    return pty_io_write (cred, data, datalen, amt);

  mutex_lock (&global_lock);

  /* Check for errors first. */

  if ((cred->po->openmodes & O_WRITE) == 0)
    {
      mutex_unlock (&global_lock);
      return EBADF;
    }

  if ((termstate.c_lflag & TOSTOP) && !fg_p (cred))
    {
      mutex_unlock (&global_lock);
      return EBACKGROUND;
    }

  if ((termflags & NO_CARRIER) && !(termstate.c_cflag & CLOCAL))
    {
      mutex_unlock (&global_lock);
      return EIO;

    }

  cancel = 0;
  for (i = 0; i < datalen; i++)
    {
      while (!qavail (outputq) && !cancel)
	{
	  err = (*bottom->start_output) ();
	  if (err)
	    cancel = 1;
	  else
	    {
	      if (!qavail (outputq))
		cancel = hurd_condition_wait (outputq->wait, &global_lock);
	    }
	}
      if (cancel)
	break;

      write_character (data[i]);
    }

  *amt = i;

  if (!err && datalen)
    (*bottom->start_output) ();

  trivfs_set_mtime (termctl);

  call_asyncs (O_WRITE);

  mutex_unlock (&global_lock);

  return ((cancel && datalen && !*amt) ? (err ?: EINTR) : 0);
}

/* Called for user reads from the terminal. */
error_t
trivfs_S_io_read (struct trivfs_protid *cred,
		  mach_port_t reply,
		  mach_msg_type_name_t replytype,
		  char **data,
		  u_int *datalen,
		  off_t offset,
		  int amount)
{
  int cancel;
  int i, max;
  char *cp;
  int avail;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class == pty_class)
    return pty_io_read (cred, data, datalen, amount);

  mutex_lock (&global_lock);

  if ((cred->po->openmodes & O_READ) == 0)
    {
      mutex_unlock (&global_lock);
      return EBADF;
    }

  if (!fg_p (cred))
    {
      mutex_unlock (&global_lock);
      return EBACKGROUND;
    }

  while (!qsize (inputq))
    {
      if ((termflags & NO_CARRIER) && !(termstate.c_cflag & CLOCAL))
	{
	  /* Return EOF, Posix.1 7.1.1.10. */
	  mutex_unlock (&global_lock);
	  *datalen = 0;
	  return 0;
	}

      if (cred->po->openmodes & O_NONBLOCK)
	{
	  mutex_unlock (&global_lock);
	  return EWOULDBLOCK;
	}

      if (hurd_condition_wait (inputq->wait, &global_lock))
	{
	  mutex_unlock (&global_lock);
	  return EINTR;
	}

      /* If a signal is being delivered, and we got woken up by
	 arriving input, then there's a possible race; we have to not
	 read from the queue as long as the signal is in progress.
	 Now, you might think that we should not read from the queue
	 when a signal is in progress even if we didn't block, but
	 that's not so.  It's specifically that we have to get
	 *interrupted* by signals in progress (when the signallee is
	 this thread and wants to interrupt is) that is the race we
	 are avoiding.  A reader who gets in after the signal begins,
	 while the signal is in progress, is harmless, because this
	 case is indiscernable from one where the reader gets in after
	 the signal has completed.  */
      if (sigs_in_progress)
	{
	  input_sig_wakeup++;
	  if (hurd_condition_wait (&input_sig_wait, &global_lock))
	    {
	      mutex_unlock (&global_lock);
	      return EINTR;
	    }
	}
    }

  avail = qsize (inputq);
  if (remote_input_mode)
    avail--;

  max = (amount < avail) ? amount : avail;

  if (max > *datalen)
    *data = mmap (0, max, PROT_READ|PROT_WRITE, MAP_ANON, 0, 0);

  cancel = 0;
  cp = *data;
  for (i = 0; i < max; i++)
    {
      char c = dequeue (inputq);

      if (remote_input_mode)
	*cp++ = c;
      else
	{
	  /* Unless this is EOF, add it to the response. */
	  if (!(termstate.c_lflag & ICANON)
	      || !CCEQ (termstate.c_cc[VEOF], c))
	    *cp++ = c;

	  /* If this is a break character, then finish now. */
	  if ((termstate.c_lflag & ICANON)
	      && (c == '\n'
		  || CCEQ (termstate.c_cc[VEOF], c)
		  || CCEQ (termstate.c_cc[VEOL], c)
		  || CCEQ (termstate.c_cc[VEOL2], c)))
	    break;

	  /* If this is the delayed suspend character, then signal now. */
	  if ((termstate.c_lflag & ISIG)
	      && CCEQ (termstate.c_cc[VDSUSP], c))
	    {
	      /* The CANCEL flag is being used here to tell the return
		 below to make sure we don't signal EOF on a VDUSP that
		 happens at the front of a line. */
	      send_signal (SIGTSTP);
	      cancel = 1;
	      break;
	    }
	}
    }

  if (remote_input_mode && qsize (inputq) == 1)
    dequeue (inputq);

  *datalen = cp - *data;

  /* If we really read something, set atime */
  if (*datalen || !cancel)
    trivfs_set_atime (termctl);

  call_asyncs (O_READ);

  mutex_unlock (&global_lock);

  return !*datalen && cancel ? EINTR : 0;
}

error_t
trivfs_S_io_pathconf (struct trivfs_protid *cred,
		      mach_port_t reply,
		      mach_msg_type_name_t reply_type,
		      int name,
		      int *val)
{
  if (!cred)
    return EOPNOTSUPP;

  switch (name)
    {
    case _PC_LINK_MAX:
    case _PC_NAME_MAX:
    case _PC_PATH_MAX:
    case _PC_PIPE_BUF:
    case _PC_NO_TRUNC:
    default:
      return io_pathconf (cred->realnode, name, val);

    case _PC_MAX_CANON:
      *val = rawq->hiwat;
      return 0;

    case _PC_MAX_INPUT:
      *val = inputq->hiwat;
      return 0;

    case _PC_CHOWN_RESTRICTED:
      /* We implement this locally, remember... */
      *val = 1;
      return 0;

    case _PC_VDISABLE:
      *val = _POSIX_VDISABLE;
      return 0;
    }
}


error_t
trivfs_S_io_readable (struct trivfs_protid *cred,
		      mach_port_t reply,
		      mach_msg_type_name_t replytype,
		      int *amt)
{
  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class == pty_class)
    return pty_io_readable (amt);

  mutex_lock (&global_lock);
  if ((cred->po->openmodes & O_READ) == 0)
    {
      mutex_unlock (&global_lock);
      return EBADF;
    }
  *amt = qsize (inputq);
  if (remote_input_mode && *amt)
    --*amt;
  mutex_unlock (&global_lock);

  return 0;
}

error_t
trivfs_S_io_revoke (struct trivfs_protid *cred,
		    mach_port_t reply,
		    mach_msg_type_name_t replytype)
{
  struct stat st;

  error_t iterator_function (void *port)
    {
      struct trivfs_protid *user = port;

      if (user != cred)
	ports_destroy_right (user);
      return 0;
    }

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  if (!cred->isroot)
    {
      error_t err;

      /* XXX */
      st.st_uid = term_owner;
      st.st_gid = term_group;

      err = fshelp_isowner (&st, cred->user);
      if (err)
	{
	  mutex_unlock (&global_lock);
	  return err;
	}
    }

  mutex_unlock (&global_lock);

  ports_inhibit_bucket_rpcs (term_bucket);
  ports_class_iterate (cred->pi.class, iterator_function);
  ports_resume_bucket_rpcs (term_bucket);

  return 0;
}





/* TIOCMODG ioctl -- Get modem state */
kern_return_t
S_tioctl_tiocmodg (io_t port,
		   int *state)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err = 0;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  err = (*bottom->mdmstate) (state);
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCMODS ioctl -- Set modem state */
kern_return_t
S_tioctl_tiocmods (io_t port,
		   int state)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;
  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->mdmctl) (MDMCTL_SET, state);

  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCEXCL ioctl -- Set exclusive use */
kern_return_t
S_tioctl_tiocexcl (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;
  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      termflags |= EXCL_USE;
      err = 0;
    }

  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}

/* TIOCNXCL ioctl -- Clear exclusive use */
kern_return_t
S_tioctl_tiocnxcl (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      termflags &= ~EXCL_USE;
      err = 0;
    }

  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}

/* TIOCFLUSH ioctl -- Flush input, output, or both */
kern_return_t
S_tioctl_tiocflush (io_t port,
		    int flags)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err = 0;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err =  EBADF;
  else
    {
      if (flags == 0)
	flags = O_READ|O_WRITE;

      if (flags & O_READ)
	{
	  err = (*bottom->notice_input_flushed) ();
	  if (!err)
	    clear_queue (inputq);
	}

      if (!err && (flags & O_WRITE))
	err = drop_output ();
    }

  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}

/* TIOCGETA ioctl -- Get termios state */
kern_return_t
S_tioctl_tiocgeta (io_t port,
		   tcflag_t *modes,
		   cc_t *ccs,
		   speed_t *speeds)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  modes[0] = termstate.c_iflag;
  modes[1] = termstate.c_oflag;
  modes[2] = termstate.c_cflag;
  modes[3] = termstate.c_lflag;
  memcpy (ccs, termstate.c_cc, NCCS);
  speeds[0] = termstate.__ispeed;
  speeds[1] = termstate.__ospeed;
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return 0;
}

/* Common code for the varios TIOCSET* commands. */
static error_t
set_state (io_t port,
	   tcflag_t *modes,
	   cc_t *ccs,
	   speed_t *speeds,
	   int draino,
	   int flushi)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;
  int oldlflag;
  struct termios state;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err =  EBADF;
  else if (!fg_p (cred))
    err = EBACKGROUND;
  else
    {
      if (cred->pi.class == pty_class)
	{
	  err = (*bottom->abandon_physical_output) ();
	  if (err)
	    goto leave;
	  clear_queue (outputq);
	}

      if (draino)
	{
	  err = drain_output ();
	  if (err)
	    goto leave;
	}

      if (flushi)
	{
	  err = (*bottom->notice_input_flushed) ();
	  if (err)
	    goto leave;
	  clear_queue (inputq);
	}

      state = termstate;
      state.c_iflag = modes[0];
      state.c_oflag = modes[1];
      state.c_cflag = modes[2];
      state.c_lflag = modes[3];
      memcpy (state.c_cc, ccs, NCCS);
      state.__ispeed = speeds[0];
      state.__ospeed = speeds[1];

      if (external_processing)
	state.c_lflag |= EXTPROC;
      else
	state.c_lflag &= ~EXTPROC;

      err = (*bottom->set_bits) (&state);
      if (err)
	goto leave;

      oldlflag = termstate.c_lflag;
      termstate = state;

      if (oldlflag & ICANON)
	{
	  if (!(termstate.c_lflag & ICANON))
	    copy_rawq ();
	}
      else
	{
	  if (termstate.c_lflag & ICANON)
	    rescan_inputq ();
	}
    }

 leave:
  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}


/* TIOCSETA -- Set termios state */
kern_return_t
S_tioctl_tiocseta (io_t port,
		   tcflag_t *modes,
		   cc_t *ccs,
		   speed_t *speeds)
{
  return set_state (port, modes, ccs, speeds, 0, 0);
}

/* Drain output, then set term state.  */
kern_return_t
S_tioctl_tiocsetaw (io_t port,
		    tcflag_t *modes,
		    cc_t *ccs,
		    speed_t *speeds)
{
  return set_state (port, modes, ccs, speeds, 1, 0);
}

/* Flush input, drain output, then set term state.  */
kern_return_t
S_tioctl_tiocsetaf (io_t port,
		    tcflag_t *modes,
		    cc_t *ccs,
		    speed_t *speeds)

{
  return set_state (port, modes, ccs, speeds, 1, 1);
}

/* TIOCGETD -- Return line discipline */
kern_return_t
S_tioctl_tiocgetd (io_t port,
		   int *disc)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  *disc = 0;

  ports_port_deref (cred);
  return 0;
}

/* TIOCSETD -- Set line discipline */
kern_return_t
S_tioctl_tiocsetd (io_t port,
		   int disc)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  mutex_unlock (&global_lock);

  if (disc != 0)
    err = ENXIO;
  else
    err = 0;

  ports_port_deref (cred);
  return 0;
}

/* TIOCDRAIN -- Wait for output to drain */
kern_return_t
S_tioctl_tiocdrain (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & O_WRITE))
    {
      mutex_unlock (&global_lock);
      ports_port_deref (cred);
      return EBADF;
    }

  err = drain_output ();
  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}

/* TIOCSWINSZ -- Set window size */
kern_return_t
S_tioctl_tiocswinsz (io_t port,
		     struct winsize size)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = 0;

  ports_port_deref (cred);

  if (! err
      && (size.ws_row != window_size.ws_row
	  || size.ws_col != window_size.ws_col
	  || size.ws_xpixel != window_size.ws_xpixel
	  || size.ws_ypixel != window_size.ws_ypixel))
    {
      /* The size is actually changing.  Record the new size and notify the
	 process group.  */
      window_size = size;
      send_signal (SIGWINCH);
    }

  mutex_unlock (&global_lock);
  return err;
}

/* TIOCGWINSZ -- Fetch window size */
kern_return_t
S_tioctl_tiocgwinsz (io_t port,
		     struct winsize *size)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  *size = window_size;
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return 0;
}

/* TIOCMGET -- Fetch all modem bits */
kern_return_t
S_tioctl_tiocmget (io_t port,
		   int *bits)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err = 0;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  err = (*bottom->mdmstate) (bits);
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCMSET -- Set all modem bits */
kern_return_t
S_tioctl_tiocmset (io_t port,
		   int bits)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->mdmctl) (MDMCTL_SET, bits);

  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}

/* TIOCMBIC -- Clear some modem bits */
kern_return_t
S_tioctl_tiocmbic (io_t port,
		   int bits)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->mdmctl) (MDMCTL_BIC, bits);
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCMBIS -- Set some modem bits */
kern_return_t
S_tioctl_tiocmbis (io_t port,
		   int bits)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->mdmctl) (MDMCTL_BIS, bits);
  mutex_unlock (&global_lock);
  ports_port_deref (cred);
  return err;
}

/* TIOCSTART -- start output as if VSTART were typed */
kern_return_t
S_tioctl_tiocstart (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      int old_termflags = termflags;

      termflags &= ~USER_OUTPUT_SUSP;
      err = (*bottom->start_output) ();
      if (err)
	termflags = old_termflags;
    }
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCSTOP -- stop output as if VSTOP were typed */
kern_return_t
S_tioctl_tiocstop (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }
  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      int old_termflags = termflags;
      termflags |= USER_OUTPUT_SUSP;
      err = (*bottom->suspend_physical_output) ();
      if (err)
	termflags = old_termflags;
    }
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCSTI -- Simulate terminal input */
kern_return_t
S_tioctl_tiocsti (io_t port,
		  char c)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  /* BSD returns EACCES if this is not our controlling terminal,
     but we have no way to do that.  (And I don't think it actually
     provides any security there, either.) */

  if (!(cred->po->openmodes & O_READ))
    err = EPERM;
  else
    {
      input_character (c);
      err = 0;
    }
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCOUTQ -- return output queue size */
kern_return_t
S_tioctl_tiocoutq (io_t port,
		   int *queue_size)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);

  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      *queue_size = qsize (outputq) + (*bottom->pending_output_size) ();
      err = 0;
    }
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCSPGRP -- set pgrp of terminal */
kern_return_t
S_tioctl_tiocspgrp (io_t port,
		    int pgrp)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    {
      termflags &= ~NO_OWNER;
      foreground_id = -pgrp;
      err = 0;
    }
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCGPGRP --- fetch pgrp of terminal */
kern_return_t
S_tioctl_tiocgpgrp (io_t port,
		    int *pgrp)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t ret;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (termflags & NO_OWNER)
    ret = ENOTTY;		/* that's what BSD says... */
  else
    {
      *pgrp = - foreground_id;
      ret = 0;
    }
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return ret;
}

/* TIOCCDTR -- clear DTR */
kern_return_t
S_tioctl_tioccdtr (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->mdmctl) (MDMCTL_BIC, TIOCM_DTR);
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCSDTR -- set DTR */
kern_return_t
S_tioctl_tiocsdtr (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->mdmctl) (MDMCTL_BIS, TIOCM_DTR);
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCCBRK -- Clear break condition */
kern_return_t
S_tioctl_tioccbrk (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->clear_break) ();
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

/* TIOCSBRK -- Set break condition */
kern_return_t
S_tioctl_tiocsbrk (io_t port)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, port, 0);
  error_t err;

  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class != pty_class
      && cred->pi.class != tty_class)
    {
      ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    err = EBADF;
  else
    err = (*bottom->set_break) ();
  mutex_unlock (&global_lock);

  ports_port_deref (cred);
  return err;
}

error_t
trivfs_S_file_set_size (struct trivfs_protid *cred,
			mach_port_t reply, mach_msg_type_name_t reply_type,
			off_t size)
{
  if (!cred)
    return EOPNOTSUPP;
  mutex_lock (&global_lock);
  if ((cred->po->openmodes & O_WRITE) == 0)
    {
      mutex_unlock (&global_lock);
      return EBADF;
    }
  mutex_unlock (&global_lock);
  return 0;
}

error_t
trivfs_S_io_seek (struct trivfs_protid *cred,
		  mach_port_t reply, mach_msg_type_name_t reply_type,
		  off_t off,
		  int whence,
		  off_t *newp)
{
  return ESPIPE;
}

error_t
trivfs_S_io_get_openmodes (struct trivfs_protid *cred,
			   mach_port_t reply,
			   mach_msg_type_name_t replytype,
			   int *bits)
{
  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  *bits = cred->po->openmodes;
  mutex_unlock (&global_lock);
  return 0;
}

#define HONORED_STATE_MODES (O_APPEND|O_ASYNC|O_FSYNC|O_NONBLOCK|O_NOATIME)

error_t
trivfs_S_io_set_all_openmodes (struct trivfs_protid *cred,
			       mach_port_t reply,
			       mach_msg_type_name_t replytype,
			       int bits)
{
  int obits;

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);

  obits = cred->po->openmodes;
  if ((obits & O_ASYNC) && --num_icky_async_peropens == 0)
    termflags &= ~ICKY_ASYNC;

  cred->po->openmodes &= ~HONORED_STATE_MODES;
  cred->po->openmodes |= (bits & HONORED_STATE_MODES);

  if ((bits & O_ASYNC) && !(obits & O_ASYNC))
    {
      termflags |= ICKY_ASYNC;
      num_icky_async_peropens++;
      call_asyncs (O_READ | O_WRITE);
    }

  mutex_unlock (&global_lock);

  return 0;
}

error_t
trivfs_S_io_set_some_openmodes (struct trivfs_protid *cred,
			     mach_port_t reply,
			     mach_msg_type_name_t reply_type,
			     int bits)
{
  int obits;

  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  obits = cred->po->openmodes;
  cred->po->openmodes |= (bits & HONORED_STATE_MODES);
  if ((bits & O_ASYNC) && !(obits & O_ASYNC))
    {
      termflags |= ICKY_ASYNC;
      num_icky_async_peropens++;
      call_asyncs (O_READ | O_WRITE);
    }
  mutex_unlock (&global_lock);
  return 0;
}

error_t
trivfs_S_io_clear_some_openmodes (struct trivfs_protid *cred,
			       mach_port_t reply,
			       mach_msg_type_name_t reply_type,
			       int bits)
{
  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  if ((cred->po->openmodes & O_ASYNC) && --num_icky_async_peropens == 0)
    termflags &= ~ICKY_ASYNC;
  cred->po->openmodes &= ~(bits & HONORED_STATE_MODES);
  mutex_unlock (&global_lock);

  return 0;
}

error_t
trivfs_S_io_mod_owner (struct trivfs_protid *cred,
		    mach_port_t reply,
		    mach_msg_type_name_t reply_type,
		    pid_t owner)
{
  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  termflags &= ~NO_OWNER;
  foreground_id = owner;
  mutex_unlock (&global_lock);
  return 0;
}

error_t
trivfs_S_io_get_owner (struct trivfs_protid *cred,
		    mach_port_t erply,
		    mach_msg_type_name_t reply_type,
		    pid_t *owner)
{
  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  if (termflags & NO_OWNER)
    {
      mutex_unlock (&global_lock);
      return ENOTTY;
    }
  *owner = foreground_id;
  mutex_unlock (&global_lock);
  return 0;
}

error_t
trivfs_S_io_get_icky_async_id (struct trivfs_protid *cred,
			       mach_port_t reply,
			       mach_msg_type_name_t reply_type,
			       mach_port_t *id, mach_msg_type_name_t *idtype)
{
  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    {
      mutex_unlock (&global_lock);
      return EBADF;
    }
  *id = async_icky_id;
  *idtype = MACH_MSG_TYPE_MAKE_SEND;
  mutex_unlock (&global_lock);
  return 0;
}

error_t
trivfs_S_io_async (struct trivfs_protid *cred,
		   mach_port_t reply, mach_msg_type_name_t reply_type,
		   mach_port_t notify,
		   mach_port_t *id, mach_msg_type_name_t *idtype)
{
  struct async_req *ar;
  if (!cred)
    return EOPNOTSUPP;

  mutex_lock (&global_lock);
  if (!(cred->po->openmodes & (O_READ|O_WRITE)))
    {
      mutex_unlock (&global_lock);
      return EBADF;
    }
  ar = malloc (sizeof (struct async_req));
  ar->notify = notify;
  ar->next = async_requests;
  async_requests = ar;
  *id = async_id;
  *idtype = MACH_MSG_TYPE_MAKE_SEND;
  mutex_unlock (&global_lock);
  return 0;
}

error_t
trivfs_S_io_select (struct trivfs_protid *cred,
		    mach_port_t reply,
		    mach_msg_type_name_t reply_type,
		    int *type)
{
  if (!cred)
    return EOPNOTSUPP;

  if (cred->pi.class == pty_class)
    return pty_io_select (cred, reply, type);

  if ((cred->po->openmodes & O_READ) == 0)
    *type &= ~SELECT_READ;
  if ((cred->po->openmodes & O_WRITE) == 0)
    *type &= ~SELECT_WRITE;

  mutex_lock (&global_lock);

  while (1)
    {
      int available = 0;
      if ((*type & SELECT_READ) && qsize (inputq))
	available |= SELECT_READ;
      if ((*type & SELECT_WRITE) && qavail (outputq))
	available |= SELECT_WRITE;

      if (available == 0)
	{
	  ports_interrupt_self_on_port_death (cred, reply);
	  if (hurd_condition_wait (&select_alert, &global_lock) == 0)
	    continue;
	}

      *type = available;
      mutex_unlock (&global_lock);
      return available ? 0 : EINTR;
    }
}

kern_return_t
trivfs_S_io_map (struct trivfs_protid *cred,
		 mach_port_t *rdobj,
		 mach_msg_type_name_t *rdtype,
		 mach_port_t *wrobj,
		 mach_msg_type_name_t *wrtype)
{
  return EOPNOTSUPP;
}

static void
report_sig_start ()
{
  sigs_in_progress++;
}

static void
report_sig_end ()
{
  sigs_in_progress--;
  if ((sigs_in_progress == 0) && input_sig_wakeup)
    {
      input_sig_wakeup = 0;
      condition_broadcast (&input_sig_wait);
    }
}

/* Call all the scheduled async I/O handlers.  DIR is a mask of O_READ &
   O_WRITE; the asyncs will only be called if output is possible in one of
   the directions given in DIR.  */
void
call_asyncs (int dir)
{
  struct async_req *ar, *nxt, **prevp;
  mach_port_t err;

  /* If nobody wants async messages, don't bother further. */
  if (!(termflags & ICKY_ASYNC) && !async_requests)
    return;

  if ((!(dir & O_READ) || qsize (inputq) == 0)
      && (!(dir & O_WRITE) && qavail (outputq) == 0))
    /* Output isn't possible in the desired directions.  */
    return;

  if ((termflags & ICKY_ASYNC) && !(termflags & NO_OWNER))
    {
      report_sig_start ();
      mutex_unlock (&global_lock);
      hurd_sig_post (foreground_id, SIGIO, async_icky_id);
      mutex_lock (&global_lock);
      report_sig_end ();
    }

  for (ar = async_requests, prevp = &async_requests;
       ar;
       ar = nxt)
    {
      nxt = ar->next;
      err = nowait_msg_sig_post (ar->notify, SIGIO, 0, async_id);
      if (err == MACH_SEND_INVALID_DEST)
	{
	  /* Receiver died; remove the notification request.  */
	  *prevp = ar->next;
	  mach_port_deallocate (mach_task_self (), ar->notify);
	  free (ar);
	}
      else
	prevp = &ar->next;
    }
}

/* Send a signal to the current process (group) of the terminal. */
void
send_signal (int signo)
{
  mach_port_t right;

  if (!(termflags & NO_OWNER))
    {
      right = ports_get_send_right (cttyid);
      report_sig_start ();
      mutex_unlock (&global_lock);
      hurd_sig_post (foreground_id, signo, right);
      mutex_lock (&global_lock);
      report_sig_end ();
      mach_port_deallocate (mach_task_self (), right);
    }
}

void
report_carrier_off ()
{
  clear_queue (inputq);
  (*bottom->notice_input_flushed) ();
  drop_output ();
  termflags |= NO_CARRIER;
  if (!(termstate.c_cflag & CLOCAL))
    send_signal (SIGHUP);
}

void
report_carrier_on ()
{
  termflags &= ~NO_CARRIER;
  condition_broadcast (&carrier_alert);
}

void
report_carrier_error (error_t err)
{
  carrier_error = err;
  condition_broadcast (&carrier_alert);
}

kern_return_t
S_term_get_nodename (io_t arg,
		     char *name)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, arg,
						  tty_class);
  if (!cred)
    return EOPNOTSUPP;

  if (!cred->po->cntl->hook)
    {
      ports_port_deref (cred);
      return ENOENT;
    }

  strcpy (name, (char *)cred->po->cntl->hook);

  ports_port_deref (cred);
  return 0;
}

kern_return_t
S_term_set_nodename (io_t arg,
		     char *name)
{
  error_t err = 0;
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, arg, tty_class);
  if (!cred)
    return EOPNOTSUPP;

  if (strcmp (name, (char *)cred->po->cntl->hook) != 0)
    err = EINVAL;

  ports_port_deref (cred);
  return err;
}

kern_return_t
S_term_set_filenode (io_t arg,
		     file_t filenode)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, arg,
						  tty_class);
  if (!cred)
    return EOPNOTSUPP;
  ports_port_deref (cred);

  return EINVAL;
}

kern_return_t
S_term_get_peername (io_t arg,
		     char *name)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, arg, 0);
  struct trivfs_control *peer;

  if (!cred || (cred->pi.class != tty_class && cred->pi.class != pty_class))
    {
      if (cred)
	ports_port_deref (cred);
      return EOPNOTSUPP;
    }

  peer = (cred->pi.class == tty_class) ? ptyctl : termctl;

  if (bottom != &ptyio_bottom || !peer->hook)
    {
      ports_port_deref (cred);
      return ENOENT;
    }

  strcpy (name, (char *) peer->hook);
  ports_port_deref (cred);

  return 0;
}

kern_return_t
S_term_get_bottom_type (io_t arg,
			int *ttype)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket,
						  arg, tty_class);
  if (!cred)
    return EOPNOTSUPP;

  ports_port_deref (cred);
  *ttype = bottom->type;
  return 0;
}

kern_return_t
S_term_on_machdev (io_t arg,
		   device_t machdev)
{
  struct trivfs_protid *cred = ports_lookup_port (term_bucket, arg,
						  tty_class);
  if (!cred)
    return EOPNOTSUPP;
  ports_port_deref (cred);
  return EINVAL;
}

kern_return_t
S_term_on_hurddev (io_t arg,
		   io_t hurddev)
{
  return EOPNOTSUPP;
}

kern_return_t
S_term_on_pty (io_t arg,
	       mach_port_t *master)
{
  return EOPNOTSUPP;
}

error_t
trivfs_goaway (struct trivfs_control *cntl, int flags)
{
  return EBUSY;
}
