/* 
   Copyright (C) 1995 Free Software Foundation, Inc.
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

#include "netfs.h"

void
netfs_drop_node (struct node *np)
{
  mode_t savemode;
  
  if (np->dn_stat.st_nlink == 0)
    {
      assert (!netfs_readonly);
      
      if (np->allocsize != 0)
	{
	  np->references++;
	  spin_unlock (&netfs_node_refcnt_lock);
	  netfs_truncate (np, 0);
	  
	  netfs_nput (np);
	  return;
	}
      /* How to clear? */
    }
  
  fshelp_drop_transbox (&np->transbox);
  
  netfs_node_norefs (np);
  spin_unlock (&netfs_node_refcnt_lock);
}

  
