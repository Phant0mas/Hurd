/* Set a file's translator.

   Copyright (C) 1995,96,97,98,2001,02,13,14
     Free Software Foundation, Inc.
   Written by Miles Bader <miles@gnu.org>
   Revised by Manolis Fragkiskos Ragkousis <manolis837@gmail.com>.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with the GNU Hurd.  If not, see <http://www.gnu.org/licenses/>.*/


#include <assert.h>
#include <hurd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <argp.h>
#include <error.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include <error.h>
#include <argz.h>
#include <hurd/fshelp.h>
#include <hurd/process.h>
#include <version.h>

#include <hurd/lookup.h>
#include <hurd/fsys.h>

#include <hurd/hurdutil.h>

const char *argp_program_version = STANDARD_HURD_VERSION (settrans);

#define _STRINGIFY(arg) #arg
#define STRINGIFY(arg) _STRINGIFY (arg)

#define OPT_CHROOT_CHDIR	-1
#define OPT_STACK		-2

static struct argp_option options[] =
{
  {"active",      'a', 0, 0, "Start TRANSLATOR and set it as NODE's active translator" },
  {"start",       's', 0, 0, "Start the translator specified by the NODE's passive translator record and set it as NODE's active translator" },
  {"passive",     'p', 0, 0, "Change NODE's passive translator record (default)" },
  {"create",      'c', 0, 0, "Create NODE if it doesn't exist" },
  {"dereference", 'L', 0, 0, "If a translator exists, put the new one on top"},
  {"pid-file",    'F', "FILENAME", 0, "When starting an active translator,"
     " write its pid to this file"},
  {"pause",       'P', 0, 0, "When starting an active translator, prompt and"
     " wait for a newline on stdin before completing the startup handshake"},
  {"timeout",     't',"SEC",0, "Timeout for translator startup, in seconds"
     " (default " STRINGIFY (DEFAULT_TIMEOUT) "); 0 means no timeout"},
  {"exclusive",   'x', 0, 0, "Only set the translator if there is not one already"},
  {"orphan",      'o', 0, 0, "Disconnect old translator from the filesystem "
			     "(do not ask it to go away)"},
  {"underlying",  'U', "NODE", 0, "Open NODE and hand it to the translator "
				  "as the underlying node"},
  {"stack", OPT_STACK, 0, 0, "Replace an existing translator, but keep it "
			     "running, and put the new one on top"},

  {"chroot",      'C', 0, 0,
   "Instead of setting the node's translator, take following arguments up to"
   " `--' and run that command chroot'd to the translated node."},
  {"chroot-chdir",      OPT_CHROOT_CHDIR, "DIR", 0,
   "Change to DIR before running the chrooted command.  "
   "DIR must be an absolute path."},

  {0,0,0,0, "When setting the passive translator, if there's an active translator:"},
  {"goaway",      'g', 0, 0, "Ask the active translator to go away"},
  {"keep-active", 'k', 0, 0, "Leave any existing active translator running"},

  {0,0,0,0, "When an active translator is told to go away:"},
  {"recursive",   'R', 0, 0, "Shutdown its children too"},
  {"force",       'f', 0, 0, "Ask it to ignore current users and shutdown "
			     "anyway." },
  {"nosync",      'S', 0, 0, "Don't sync it before killing it"},

  {0, 0}
};
static char *args_doc = "NODE [TRANSLATOR ARG...]";
static char *doc = "Set the passive/active translator on NODE."
"\vBy default the passive translator is set.";

/* The context to be used to create the translator. */
struct settrans_context *context;

/* ---------------------------------------------------------------- */

/* Parse our options...  */
error_t parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case ARGP_KEY_ARG:
      if (state->arg_num == 0)
        context->node_name = arg;
      else			/* command */
        {
          if (context->start)
            argp_error (state, "both --start and TRANSLATOR given");

          error_t err =
            argz_create (state->argv + state->next - 1, &context->argz, &context->argz_len);
          if (err)
            error(3, err, "Can't create options vector");
          state->next = state->argc; /* stop parsing */
        }
      break;

    case ARGP_KEY_NO_ARGS:
      argp_usage (state);
      return EINVAL;

    case 'a': context->active = 1; break;
    case 's':
      context->start = 1;
      context->active = 1;	/* start implies active */
      break;
    case OPT_STACK:
      context->stack = 1;
      context->active = 1;   /* stack implies active */
      context->orphan = 1;   /* stack implies orphan */
      break;
    case 'p': context->passive = 1; break;
    case 'k': context->keep_active = 1; break;
    case 'g': context->kill_active = 1; break;
    case 'x': context->excl = 1; break;
    case 'P': context->pause = 1; break;
    case 'F':
      context->pid_file = strdup (arg);
      if (context->pid_file == NULL)
        error(3, ENOMEM, "Failed to duplicate argument");
      break;

    case 'o': context->orphan = 1; break;
    case 'U':
      context->underlying_node_name = strdup (arg);
      if (context->underlying_node_name == NULL)
        error(3, ENOMEM, "Failed to duplicate argument");
      break;

    case 'C':
      if (context->chroot_command)
        {
          argp_error (state, "--chroot given twice");
          return EINVAL;
        }
      context->chroot_command = &state->argv[state->next];
      while (state->next < state->argc)
        {
          if (!strcmp (state->argv[state->next], "--"))
            {
              state->argv[state->next++] = 0;
              if (context->chroot_command[0] == 0)
                {
                  argp_error (state,
                              "--chroot must be followed by a command");
                  return EINVAL;
                }
              return 0;
            }
          ++state->next;
        }
      argp_error (state, "--chroot command must be terminated with `--'");
      return EINVAL;

    case OPT_CHROOT_CHDIR:
      if (arg[0] != '/')
        argp_error (state, "--chroot-chdir must be absolute");
      context->chroot_chdir = arg;
      break;

    case 'c': context->lookup_flags |= O_CREAT; break;
    case 'L': context->lookup_flags &= ~O_NOTRANS; break;

    case 'R': context->goaway_flags |= FSYS_GOAWAY_RECURSE; break;
    case 'S': context->goaway_flags |= FSYS_GOAWAY_NOSYNC; break;
    case 'f': context->goaway_flags |= FSYS_GOAWAY_FORCE; break;

      /* Use atof so the user can specifiy fractional timeouts.  */
    case 't': context->timeout = atof (arg) * 1000.0; break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

int
main(int argc, char *argv[])
{
  error_t err;

  settrans_context_create(&context);

  struct argp argp = {options, parse_opt, args_doc, doc};

  argp_parse (&argp, argc, argv, ARGP_IN_ORDER, 0, 0);

  err = settrans(context);
  if (err)
    {
      settrans_context_cleanup(context);
      error(1, err, "Could not set translator");
    }

  settrans_context_cleanup(context);

  return 0;
}
