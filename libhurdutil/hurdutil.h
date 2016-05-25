/* hurdutil.h - Hurd utils interface.
   Copyright (C) 2016 Free Software Foundation, Inc.
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

#ifndef _HURD_UTIL_H
#define _HURD_UTIL_H

#include <errno.h>
#include <sys/types.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#include <hurd/fsys.h>

struct settrans_context
{
  /* The name of the node we're putting the translator on. */
  char *node_name;

  /* Flags to pass to file_set_translator.  */
  int lookup_flags;
  int goaway_flags;

  /* Various option flags.  */
  int passive : 1;
  int active : 1;
  int keep_active : 1;
  int pause : 1;
  int kill_active : 1;
  int orphan : 1;
  int start : 1;
  int stack : 1;
  int excl : 1;
  int timeout;
  char *pid_file;
  char *underlying_node_name;
  int underlying_lookup_flags;
  char **chroot_command;
  char *chroot_chdir;

  /* The translator's arg vector, in '\0' separated format.  */
  char *argz;
  size_t argz_len;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the flags to be used. */
void settrans_context_init (struct settrans_context *context);

/* Release the memory allocated. */
void settrans_context_cleanup (struct settrans_context *context);

/* Create the struct containing the flags and initialize them.
   If a memory allocation error occurs, ENOMEM is returned,
   otherwise 0.*/
error_t settrans_context_create (struct settrans_context **context);

/* Set a translator according to the flags passed. On success return 0. */
error_t settrans(struct settrans_context *context);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* _HURD_UTIL_H */
