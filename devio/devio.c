/* A translator for doing I/O to mach kernel devices.

   Copyright (C) 1995 Free Software Foundation, Inc.

   Written by Miles Bader <miles@gnu.ai.mit.edu>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

#include <hurd.h>
#include <hurd/ports.h>
#include <hurd/pager.h>
#include <hurd/trivfs.h>
#include <hurd/fsys.h>

#include <stdio.h>
#include <error.h>
#include <getopt.h>
#include <assert.h>
#include <fcntl.h>

#include "open.h"
#include "dev.h"
#include "ptypes.h"

/* ---------------------------------------------------------------- */

#define USAGE "Usage: %s [OPTION...] DEVICE\n"

static void
usage(int status)
{
  if (status != 0)
    fprintf(stderr, "Try `%s --help' for more information.\n",
	    program_invocation_name);
  else
    {
      printf(USAGE, program_invocation_name);
      printf("\
\n\
  -d, --devnum=NUM           Give DEVICE a device number NUM\n\
  -r, --readonly             Disable writing to DEVICE\n\
  -p, --seekable             Enable seeking if DEVICE is serial\n\
  -s, --serial               Indicate that DEVICE has a single R/W point\n\
  -b, --buffered, --block    Open DEVICE in `block' mode, which allows reads\n\
                             or writes less than a single block and buffers\n\
                             I/O to the actual device.  By default, all reads\n\
                             and writes are made directly to the device,\n\
                             with no buffering, and any sub-block-size I/O\n\
                             is padded to the nearest full block.\n\
  -B NUM, --block-size=NUM   Use a block size of NUM, which must be an integer\n\
                             multiple of DEVICE's real block size\n\
");
    }

  exit(status);
}

#define SHORT_OPTIONS "bB:d:D:?rpsu"

static struct option options[] =
{
  {"block-size", required_argument, 0, 'B'},
  {"debug", required_argument,  0, 'D'},
  {"help", no_argument, 0, '?'},
  {"devnum", required_argument, 0, 'm'},
  {"block", no_argument, 0, 'b'},
  {"buffered", no_argument, 0, 'b'},
  {"readonly", no_argument, 0, 'r'},
  {"seekable", no_argument, 0, 'p'},
  {"serial", no_argument, 0, 's'},
  {0, 0, 0, 0}
};


/* ---------------------------------------------------------------- */

/* A struct dev for the open kernel device.  */
static struct dev *device = NULL;

/* Desired device parameters specified by the user.  */
static char *device_name = NULL;
static int device_flags = 0;
static int device_block_size = 0;

/* A unixy device number to return when the device is stat'd.  */
static int device_number = 0;

/* A stream on which we can print debugging message.  */
FILE  *debug = NULL;
/* A lock to use while doing so.  */
struct mutex debug_lock;

void main(int argc, char *argv[])
{
  error_t err;
  mach_port_t bootstrap;
  int opt;
  struct trivfs_control *trivfs_control;
  mach_port_t realnode, control;

  while ((opt = getopt_long(argc, argv, SHORT_OPTIONS, options, 0)) != EOF)
    switch (opt)
      {
      case 'r': device_flags |= DEV_READONLY; break;
      case 's': device_flags |= DEV_SERIAL; break;
      case 'b': device_flags |= DEV_BUFFERED; break;
      case 'p': device_flags |= DEV_SEEKABLE; break;
      case 'B': device_block_size = atoi(optarg); break;
      case 'd': device_number = atoi(optarg); break;
      case 'D': debug = fopen(optarg, "w+"); setlinebuf(debug); break;
      case '?': usage(0);
      default:  usage(1);
      }

  mutex_init(&debug_lock);

  if (device_flags & DEV_READONLY)
    /* Catch illegal writes at the point of open.  */
    trivfs_allow_open &= ~O_WRITE;

  if (argv[optind] == NULL || argv[optind + 1] != NULL)
    {
      fprintf(stderr, USAGE, program_invocation_name);
      usage(1);
    }

  device_name = argv[optind];

  _libports_initialize();

  task_get_bootstrap_port (mach_task_self (), &bootstrap);
  if (bootstrap == MACH_PORT_NULL)
    error(2, 0, "Must be started as a translator");
  
  /* Reply to our parent */
  control = trivfs_handle_port (MACH_PORT_NULL, PT_FSYS, PT_NODE);
  err = fsys_startup (bootstrap, control, MACH_MSG_TYPE_MAKE_SEND, &realnode);
  
  /* Install the returned realnode for trivfs's use */
  trivfs_control = ports_check_port_type (control, PT_FSYS);
  assert (trivfs_control);
  ports_change_hardsoft (trivfs_control, 1);
  trivfs_control->underlying = realnode;
  ports_done_with_port (trivfs_control);

  /* Open the device only when necessary.  */
  device = NULL;

  /* Launch. */
  ports_manage_port_operations_multithread ();

  exit(0);
}

/* Called whenever someone tries to open our node (even for a stat).  We
   delay opening the kernel device until this point, as we can usefully
   return errors from here.  */
static error_t
check_open_hook (struct trivfs_control *trivfs_control,
		 uid_t *uids, u_int nuids,
		 gid_t *gids, u_int ngids,
		 int flags)
{
  error_t err = 0;

  if (device == NULL)
    /* Try and open the device.  */
    {
      err = dev_open(device_name, device_flags, device_block_size, &device);
      if (err)
	device = NULL;
      if (err && (flags & (O_READ|O_WRITE)) == 0)
	/* If we're not opening for read or write, then just ignore the
	   error, as this allows stat to word correctly.  XXX  */
	err = 0;
    }

  return err;
}

static void
open_hook(struct trivfs_peropen *peropen)
{
  if (device)
    open_create(device, (struct open **)&peropen->hook);
}

static void
close_hook(struct trivfs_peropen *peropen)
{
  if (peropen->hook)
    open_free(peropen->hook);
}

static void
clean_exit(int status)
{
#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "cleaning up and exiting (status = %d)...\n", status);
      mutex_unlock(&debug_lock);
    }
#endif
  if (device)
    {
      dev_close(device);
      device = NULL;
    }
#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "Bye!\n");
      fclose(debug);
      debug = NULL;
      mutex_unlock(&debug_lock);
    }
#endif
}

/* ---------------------------------------------------------------- */
/* Trivfs hooks  */

int trivfs_fstype = FSTYPE_DEV;
int trivfs_fsid = 0; /* ??? */

int trivfs_support_read = 1;
int trivfs_support_write = 1;
int trivfs_support_exec = 0;

int trivfs_allow_open = O_READ | O_WRITE;

int trivfs_protid_porttypes[] = {PT_NODE};
int trivfs_cntl_porttypes[] = {PT_FSYS};
int trivfs_protid_nporttypes = 1;
int trivfs_cntl_nporttypes = 1;

void
trivfs_modify_stat (struct stat *st)
{
  if (device)
    {
      vm_size_t size = device->size;

      if (device->block_size > 1)
	st->st_blksize = device->block_size;

      st->st_size = size;
      st->st_blocks = size / 512;

      if (dev_is(device, DEV_READONLY))
	st->st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);

      st->st_mode &= ~S_IFMT;
      st->st_mode |= dev_is(device, DEV_BUFFERED) ? S_IFBLK : S_IFCHR;
    }
  else
    /* Try and do things without an open device...  */
    {
      st->st_blksize = device_block_size;
      st->st_size = 0;
      st->st_blocks = 0;
      st->st_mode &= ~S_IFMT;
      st->st_mode |= (device_flags & DEV_BUFFERED) ? S_IFBLK : S_IFCHR;
      if (device_flags & DEV_READONLY)
	st->st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    }

  st->st_fstype = FSTYPE_DEV;
  st->st_rdev = device_number;
}

error_t
trivfs_goaway (int flags, mach_port_t realnode, int ctltype, int pitype)
{
  if (device != NULL && !(flags & FSYS_GOAWAY_FORCE))
    /* By default, don't go away if there are still opens on this device.  */
    return EBUSY;

#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "trivfs_goaway(0x%x, %d, %d, %d)\n",
	      flags, realnode, ctltype, pitype);
      mutex_unlock(&debug_lock);
    }
#endif

  if (flags & FSYS_GOAWAY_NOSYNC)
    exit(0);
  else
    clean_exit(0);
}

/* If this variable is set, it is called every time an open happens.
   UIDS, GIDS, and FLAGS are from the open; CNTL identifies the
   node being opened.  This call need not check permissions on the underlying
   node.  If the open call should block, then return EWOULDBLOCK.  Other
   errors are immediately reflected to the user.  If O_NONBLOCK 
   is not set in FLAGS and EWOULDBLOCK is returned, then call 
   trivfs_complete_open when all pending open requests for this 
   file can complete. */
error_t (*trivfs_check_open_hook)(struct trivfs_control *trivfs_control,
				  uid_t *uids, u_int nuids,
				  gid_t *gids, u_int ngids,
				  int flags)
     = check_open_hook;

/* If this variable is set, it is called every time a new peropen
   structure is created and initialized. */
void (*trivfs_peropen_create_hook)(struct trivfs_peropen *) = open_hook;

/* If this variable is set, it is called every time a peropen structure
   is about to be destroyed. */
void (*trivfs_peropen_destroy_hook) (struct trivfs_peropen *) = close_hook;

/* Sync this filesystem.  */
kern_return_t
trivfs_S_fsys_syncfs (struct trivfs_control *cntl,
		      mach_port_t reply, mach_msg_type_name_t replytype,
		      int wait, int dochildren)
{
#ifdef MSG
  if (debug && device)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "syncing filesystem...\n");
      mutex_unlock(&debug_lock);
    }
#endif
  if (device)
    return dev_sync(device, wait);
  else
    return 0;
}

/* ---------------------------------------------------------------- */
/* Ports hooks  */

void (*ports_cleanroutines[])(void *) =
{
  [PT_FSYS] = trivfs_clean_cntl,
  [PT_NODE] = trivfs_clean_protid,
};

int
ports_demuxer (mach_msg_header_t *inp, mach_msg_header_t *outp)
{
  error_t err;
#ifdef MSG
  static int next_msg_num = 0;
  int msg_num;

  if (debug)
    {
      mutex_lock(&debug_lock);
      msg_num = next_msg_num++;
      fprintf(debug, "port_demuxer(%d) [%d]\n", inp->msgh_id, msg_num);
      mutex_unlock(&debug_lock);
    }
#endif

  err = pager_demuxer(inp, outp) || trivfs_demuxer(inp, outp);

#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "port_demuxer(%d) [%d] done!\n", inp->msgh_id, msg_num);
      mutex_unlock(&debug_lock);
    }
#endif

  return err;
}

/* This will be called whenever there have been no requests to the server for
   a significant period of time.  NHARD is the number of live hard ports;
   NSOFT is the number of live soft ports.  This function is called while an
   internal lock is held, so it cannot reliably call any other functions of
   the ports library. */
void
ports_notice_idle (int nhard, int nsoft)
{
#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "ports_notice_idle(%d, %d)\n", nhard, nsoft);
      mutex_unlock(&debug_lock);
    }
  else
#endif
    if (nhard == 0)
      clean_exit(0);
}

/* This will be called whenever there are no hard ports or soft ports
   allocated.  This function is called while an internal lock is held, so it
   cannot reliably call any other functions of the ports library. */
void
ports_no_live_ports ()
{
#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "ports_no_live_ports()\n");
      mutex_unlock(&debug_lock);
    }
  else
#endif
    clean_exit(0);
}

/* This will be called whenever there are no hard ports allocated but there
   are still some soft ports.  This function is called while an internal lock
   is held, so it cannot reliably call any other functions of the ports
   library. */
void
ports_no_hard_ports ()
{
#ifdef MSG
  if (debug)
    {
      mutex_lock(&debug_lock);
      fprintf(debug, "ports_no_hard_ports()\n");
      mutex_unlock(&debug_lock);
    }
#endif
  if (device != NULL)
    {
      dev_close(device);
      device = NULL;
    }
}
