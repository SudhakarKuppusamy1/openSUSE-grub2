/* editenv.c - tool to edit environment block.  */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008,2009,2010,2013 Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <grub/types.h>
#include <grub/emu/misc.h>
#include <grub/util/misc.h>
#include <grub/util/install.h>
#include <grub/lib/envblk.h>
#include <grub/i18n.h>
#include <grub/emu/hostfile.h>

#include <errno.h>
#include <string.h>

#define DEFAULT_ENVBLK_SIZE	1024
#define GRUB_ENVBLK_MESSAGE	"# WARNING: Do not edit this file other than by grub2-editenv\n"

void
grub_util_create_envblk_file (const char *name)
{
  FILE *fp;
  char *buf;
  char *pbuf;
  char *namenew;

  buf = xmalloc (DEFAULT_ENVBLK_SIZE);

  namenew = xasprintf ("%s.new", name);
  fp = grub_util_fopen (namenew, "wb");
  if (! fp)
    grub_util_error (_("cannot open `%s': %s"), namenew,
		     strerror (errno));

  pbuf = buf;
  memcpy (pbuf, GRUB_ENVBLK_SIGNATURE, sizeof (GRUB_ENVBLK_SIGNATURE) - 1);
  pbuf += sizeof (GRUB_ENVBLK_SIGNATURE) - 1;
  memcpy (pbuf, GRUB_ENVBLK_MESSAGE, sizeof (GRUB_ENVBLK_MESSAGE) - 1);
  pbuf += sizeof (GRUB_ENVBLK_MESSAGE) - 1;
  memset (pbuf , '#',
          DEFAULT_ENVBLK_SIZE - sizeof (GRUB_ENVBLK_SIGNATURE) - sizeof (GRUB_ENVBLK_MESSAGE) + 2);

  if (fwrite (buf, 1, DEFAULT_ENVBLK_SIZE, fp) != DEFAULT_ENVBLK_SIZE)
    grub_util_error (_("cannot write to `%s': %s"), namenew,
		     strerror (errno));


  if (grub_util_file_sync (fp) < 0)
    grub_util_error (_("cannot sync `%s': %s"), namenew, strerror (errno));
  free (buf);
  fclose (fp);

  if (grub_util_rename (namenew, name) < 0)
    grub_util_error (_("cannot rename the file %s to %s"), namenew, name);
  free (namenew);
}

struct fs_envblk_spec fs_envblk_spec[] = {
  { "btrfs", 256 * 1024, GRUB_DISK_SECTOR_SIZE },
  { NULL, 0, 0 }
};

static grub_envblk_t
open_envblk_file (const char *name)
{
  FILE *fp;
  char *buf;
  size_t size;
  grub_envblk_t envblk;

  fp = grub_util_fopen (name, "rb");
  if (! fp)
    {
      /* Create the file implicitly.  */
      grub_util_create_envblk_file (name);
      fp = grub_util_fopen (name, "rb");
      if (! fp)
        grub_util_error (_("cannot open `%s': %s"), name,
			 strerror (errno));
    }

  if (fseek (fp, 0, SEEK_END) < 0)
    grub_util_error (_("cannot seek `%s': %s"), name,
		     strerror (errno));

  size = (size_t) ftell (fp);

  if (fseek (fp, 0, SEEK_SET) < 0)
    grub_util_error (_("cannot seek `%s': %s"), name,
		     strerror (errno));

  buf = xmalloc (size);

  if (fread (buf, 1, size, fp) != size)
    grub_util_error (_("cannot read `%s': %s"), name,
		     strerror (errno));

  fclose (fp);

  envblk = grub_envblk_open (buf, size);
  if (! envblk)
    grub_util_error ("%s", _("invalid environment block"));

  return envblk;
}

static void
write_envblk (const char *name, grub_envblk_t envblk)
{
  FILE *fp;

  fp = grub_util_fopen (name, "wb");
  if (! fp)
    grub_util_error (_("cannot open `%s': %s"), name,
		     strerror (errno));

  if (fwrite (grub_envblk_buffer (envblk), 1, grub_envblk_size (envblk), fp)
      != grub_envblk_size (envblk))
    grub_util_error (_("cannot write to `%s': %s"), name,
		     strerror (errno));

  if (grub_util_file_sync (fp) < 0)
    grub_util_error (_("cannot sync `%s': %s"), name, strerror (errno));
  fclose (fp);
}

void
grub_util_create_envblk_fs_area (const char *name, const char *fs, const char *device)
{
  char *val;
  int offset, size;
  FILE *fp;
  char *buf;
  grub_envblk_t envblk;
  struct fs_envblk_spec *p;

  for (p = fs_envblk_spec; p->fs_name; p++)
    if (strcmp (fs, p->fs_name) == 0)
      break;

  if (!p)
    return;

  fp = grub_util_fopen (device, "r+b");
  if (! fp)
    grub_util_error (_("cannot open `%s': %s"), device, strerror (errno));

  buf = xmalloc (p->size);
  memcpy (buf, GRUB_ENVBLK_SIGNATURE, sizeof (GRUB_ENVBLK_SIGNATURE) - 1);
  memset (buf + sizeof (GRUB_ENVBLK_SIGNATURE) - 1, '#', p->size - sizeof (GRUB_ENVBLK_SIGNATURE) + 1);

  if (fseek (fp, p->offset, SEEK_SET) < 0)
    grub_util_error (_("cannot seek `%s': %s"), device, strerror (errno));

  if (fwrite (buf, 1, p->size, fp) != p->size)
    grub_util_error (_("cannot write to `%s': %s"), device, strerror (errno));

  grub_util_file_sync (fp);
  free (buf);
  fclose (fp);

  envblk = open_envblk_file (name);
  if (!envblk)
    return;

  offset = p->offset >> GRUB_DISK_SECTOR_BITS;
  size =  (p->size + GRUB_DISK_SECTOR_SIZE - 1) >> GRUB_DISK_SECTOR_BITS;

  val = xasprintf ("%d+%d", offset, size);
  if (! grub_envblk_set (envblk, "env_block", val))
    grub_util_error ("%s", _("environment block too small"));
  free (val);

  write_envblk (name, envblk);
  grub_envblk_close (envblk);
}
