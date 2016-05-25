/* settrans.c - Set a file's translator.

   Copyright (C) 1995,96,97,98,2001,02,13,14,16
   Free Software Foundation, Inc.
   Written by Miles Bader <miles@gnu.org>
   Written by Manolis Fragkiskos Ragkousis <manolis837@gmail.com>.

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
   along with the GNU Hurd.  If not, see <http://www.gnu.org/licenses/>.*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <fcntl.h>
#include <argp.h>
#include <unistd.h>
#include <sys/wait.h>

#include <hurd.h>
#include <error.h>
#include <argz.h>
#include <hurd/fshelp.h>


#include "hurdutil.h"

#define DEFAULT_TIMEOUT 60

/* Authentication of the current process.  */
uid_t *uids;
gid_t *gids;
size_t uids_len, gids_len;

/* Initialize and populate the uids and gids vectors.  */
error_t
get_credentials (void)
{
  /* Fetch uids...  */
  uids_len = geteuids (0, 0);
  if (uids_len < 0)
    return errno;

  uids = malloc (uids_len * sizeof (uid_t));
  if (! uids)
    return ENOMEM;

  uids_len = geteuids (uids_len, uids);
  if (uids_len < 0)
    return errno;

  /* ... and gids.  */
  gids_len = getgroups (0, 0);
  if (gids_len < 0)
    return errno;

  gids = malloc (gids_len * sizeof (gid_t));
  if (! uids)
    return ENOMEM;

  gids_len = getgroups (gids_len, gids);
  if (gids_len < 0)
    return errno;

  return 0;
}

/* ---------------------------------------------------------------- */

void
settrans_context_init (struct settrans_context *context)
{
  context->node_name = 0;

  context->lookup_flags = O_NOTRANS;
  context->goaway_flags = 0;

  context->passive = 0;
  context->active = 0;
  context->keep_active = 0;
  context->pause = 0;
  context->kill_active = 0;
  context->orphan = 0;
  context->start = 0;
  context->stack = 0;
  context->excl = 0;
  context->timeout = DEFAULT_TIMEOUT * 1000; /* ms */
  context->pid_file = 0;
  context->underlying_node_name = NULL;
  context->chroot_command = 0;
  context->chroot_chdir = "/";

  context->argz;
  context->argz_len;
}

void
settrans_context_cleanup (struct settrans_context *context)
{
  free(context);
}

error_t
settrans_context_create (struct settrans_context **context)
{
  *context = malloc (sizeof (struct settrans_context));
  if (*context == NULL)
    return ENOMEM;

  settrans_context_init(*context);

  return 0;
}

error_t
settrans(struct settrans_context *context)
{
  error_t err;

  /* The filesystem node we're putting a translator on.  */
  char *node_name = context->node_name;
  file_t node;

  /* The translator's arg vector, in '\0' separated format.  */
  char *argz = context->argz;
  size_t argz_len = context->argz_len;

  /* The control port for any active translator we start up.  */
  fsys_t active_control = MACH_PORT_NULL;

  /* Flags to pass to file_set_translator.  */
  int active_flags = 0;
  int passive_flags = 0;
  int lookup_flags = context->lookup_flags;
  int goaway_flags = context->goaway_flags;

  /* Various option flags.  */
  int passive = context->passive;
  int active = context->active;
  int keep_active = context->keep_active;
  int pause = context->pause;
  int kill_active = context->kill_active;
  int orphan = context->orphan;
  int start = context->start;
  int stack = context->stack;
  char *pid_file = context->pid_file;
  unsigned int excl = context->excl;
  int timeout = context->timeout; /* ms */
  char *underlying_node_name = context->underlying_node_name;
  int underlying_lookup_flags = context->underlying_lookup_flags;
  char **chroot_command = context->chroot_command;
  char *chroot_chdir = context->chroot_chdir;

  if (stack)
    {
      underlying_node_name = node_name;
      underlying_lookup_flags = lookup_flags && ~O_NOTRANS;
    }
  else
    underlying_lookup_flags = lookup_flags;


  if (!active && !passive && !chroot_command)
    passive = 1;		/* By default, set the passive translator.  */

  if (passive)
    passive_flags = FS_TRANS_SET | (excl ? FS_TRANS_EXCL : 0);
  if (active)
    active_flags = FS_TRANS_SET | (excl ? FS_TRANS_EXCL : 0)
      | (orphan ? FS_TRANS_ORPHAN : 0);

  if (passive && !active)
    {
      /* When setting just the passive, decide what to do with any active.  */
      if (kill_active)
        /* Make it go away.  */
        active_flags = FS_TRANS_SET;
      else if (! keep_active)
        /* Ensure that there isn't one.  */
        active_flags = FS_TRANS_SET | FS_TRANS_EXCL;
    }

  if (start)
    {
      /* Retrieve the passive translator record in argz.  */
      mach_port_t node = file_name_lookup (node_name, lookup_flags, 0);
      if (node == MACH_PORT_NULL)
        error (4, errno, "%s", node_name);

      char buf[1024];
      argz = buf;
      argz_len = sizeof (buf);

      err = file_get_translator (node, &argz, &argz_len);
      if (err == EINVAL)
        error (4, 0, "%s: no passive translator record found", node_name);
      if (err)
        error (4, err, "%s", node_name);

      mach_port_deallocate (mach_task_self (), node);
    }

  if ((active || chroot_command) && argz_len > 0)
    {
      /* Error during file lookup; we use this to avoid duplicating error
         messages.  */
      error_t open_err = 0;

      /* The callback to start_translator opens NODE as a side effect.  */
      error_t open_node (int flags,
                         mach_port_t *underlying,
                         mach_msg_type_name_t *underlying_type,
                         task_t task, void *cookie)
      {
        if (pause)
          {
            fprintf (stderr, "Translator pid: %d\nPausing...",
                     task2pid (task));
            getchar ();
          }

        if (pid_file != NULL)
          {
            FILE *h;
            h = fopen (pid_file, "w");
            if (h == NULL)
              error (4, errno, "Failed to open pid file");

            fprintf (h, "%i\n", task2pid (task));
            fclose (h);
          }

        node = file_name_lookup (node_name, flags | lookup_flags, 0666);
        if (node == MACH_PORT_NULL)
          {
            open_err = errno;
            return open_err;
          }

        if (underlying_node_name)
          {
            *underlying = file_name_lookup (underlying_node_name,
                                            flags | underlying_lookup_flags,
                                            0666);
            if (! MACH_PORT_VALID (*underlying))
              {
                /* For the error message.  */
                node_name = underlying_node_name;
                open_err = errno;
                return open_err;
              }
          }
        else
          *underlying = node;
        *underlying_type = MACH_MSG_TYPE_COPY_SEND;

        return 0;
      }
      err = fshelp_start_translator (open_node, NULL, argz, argz, argz_len,
                                     timeout, &active_control);
      if (err)
        /* If ERR is due to a problem opening the translated node, we print
           that name, otherwise, the name of the translator.  */
        error(4, err, "%s", (err == open_err) ? node_name : argz);
    }
  else
    {
      node = file_name_lookup(node_name, lookup_flags, 0666);
      if (node == MACH_PORT_NULL)
        error(1, errno, "%s", node_name);
    }

  if (active || passive)
    {
      err = file_set_translator (node,
                                 passive_flags, active_flags, goaway_flags,
                                 argz, argz_len,
                                 active_control, MACH_MSG_TYPE_COPY_SEND);
      if (err)
        {
          error (5, err, "%s", node_name);
        }

    }

  if (chroot_command)
    {
      pid_t child;
      int status;
      switch ((child = fork ()))
        {
        case -1:
          error (6, errno, "fork");

        case 0:; /* Child.  */
          /* We will act as the parent filesystem would for a lookup
             of the active translator's root node, then use this port
             as our root directory while we exec the command.  */

          char retry_name[1024];	/* XXX */
          retry_type do_retry;
          mach_port_t root;
          file_t executable;
          char *prefixed_name;

          err = get_credentials ();
          if (err)
            error (6, err, "getting credentials");

          err = fsys_getroot (active_control,
                              MACH_PORT_NULL, MACH_MSG_TYPE_COPY_SEND,
                              uids, uids_len, gids, gids_len, 0,
                              &do_retry, retry_name, &root);
          mach_port_deallocate (mach_task_self (), active_control);
          if (err)
            error (6, err, "fsys_getroot");
          err = hurd_file_name_lookup_retry (&_hurd_ports_use, &getdport, 0,
                                             do_retry, retry_name, 0, 0,
                                             &root);
          if (err)
            error (6, err, "cannot resolve root port");

          if (setcrdir (root))
            error (7, errno, "cannot install root port");
          mach_port_deallocate (mach_task_self (), root);
          if (chdir (chroot_chdir))
            error (8, errno, "%s", chroot_chdir);

          /* Lookup executable in PATH.  */
          executable = file_name_path_lookup (chroot_command[0],
                                              getenv ("PATH"),
                                              O_EXEC, 0,
                                              &prefixed_name);
          if (MACH_PORT_VALID (executable))
            {
              err = mach_port_deallocate (mach_task_self (), executable);
              assert_perror (err);
              if (prefixed_name)
                chroot_command[0] = prefixed_name;
            }

          execvp (chroot_command[0], chroot_command);
          error (8, errno, "cannot execute %s", chroot_command[0]);
          break;

        default: /* Parent.  */
          if (waitpid (child, &status, 0) != child)
            error (8, errno, "waitpid on %d", child);

          err = fsys_goaway (active_control, goaway_flags);
          if (err && err != EBUSY)
            error (9, err, "fsys_goaway");

          if (WIFSIGNALED (status))
            error (WTERMSIG (status) + 128, 0,
                   "%s for child %d", strsignal (WTERMSIG (status)), child);
          if (WEXITSTATUS (status) != 0)
            error (WEXITSTATUS (status), 0,
                   "Error %d for child %d", WEXITSTATUS (status), child);
        }
    }
  return 0;
}
