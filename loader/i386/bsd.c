/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2008,2009,2010  Free Software Foundation, Inc.
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

#include <grub/loader.h>
#include <grub/i386/bsd.h>
#include <grub/i386/cpuid.h>
#include <grub/machine/memory.h>
#include <grub/memory.h>
#include <grub/file.h>
#include <grub/err.h>
#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/elfload.h>
#include <grub/env.h>
#include <grub/misc.h>
#include <grub/gzio.h>
#include <grub/aout.h>
#include <grub/command.h>
#include <grub/extcmd.h>
#include <grub/i18n.h>
#include <grub/i386/pc/serial.h>

#include <grub/video.h>
#ifdef GRUB_MACHINE_PCBIOS
#include <grub/machine/biosnum.h>
#endif
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/efi.h>
#define NETBSD_DEFAULT_VIDEO_MODE "800x600"
#else
#define NETBSD_DEFAULT_VIDEO_MODE "text"
#include <grub/i386/pc/vbe.h>
#endif
#include <grub/video.h>

#include <grub/disk.h>
#include <grub/device.h>
#include <grub/partition.h>
#include <grub/relocator.h>
#include <grub/i386/relocator.h>

#define ALIGN_DWORD(a)	ALIGN_UP (a, 4)
#define ALIGN_QWORD(a)	ALIGN_UP (a, 8)
#define ALIGN_VAR(a)	((is_64bit) ? (ALIGN_QWORD(a)) : (ALIGN_DWORD(a)))
#define ALIGN_PAGE(a)	ALIGN_UP (a, 4096)

static int kernel_type = KERNEL_TYPE_NONE;
static grub_dl_t my_mod;
static grub_addr_t entry, entry_hi, kern_start, kern_end;
static void *kern_chunk_src;
static grub_uint32_t bootflags;
static int is_elf_kernel, is_64bit;
static grub_uint32_t openbsd_root;
struct grub_relocator *relocator = NULL;

struct bsd_tag
{
  struct bsd_tag *next;
  grub_size_t len;
  grub_uint32_t type;
  union {
    grub_uint8_t a;
    grub_uint16_t b;
    grub_uint32_t c;
  } data[0];
};

struct bsd_tag *tags, *tags_last;

static const struct grub_arg_option freebsd_opts[] =
  {
    {"dual", 'D', 0, N_("Display output on all consoles."), 0, 0},
    {"serial", 'h', 0, N_("Use serial console."), 0, 0},
    {"askname", 'a', 0, N_("Ask for file name to reboot from."), 0, 0},
    {"cdrom", 'C', 0, N_("Use CDROM as root."), 0, 0},
    {"config", 'c', 0, N_("Invoke user configuration routing."), 0, 0},
    {"kdb", 'd', 0, N_("Enter in KDB on boot."), 0, 0},
    {"gdb", 'g', 0, N_("Use GDB remote debugger instead of DDB."), 0, 0},
    {"mute", 'm', 0, N_("Disable all boot output."), 0, 0},
    {"nointr", 'n', 0, "", 0, 0},
    {"pause", 'p', 0, N_("Wait for keypress after every line of output."), 0, 0},
    {"quiet", 'q', 0, "", 0, 0},
    {"dfltroot", 'r', 0, N_("Use compiled-in rootdev."), 0, 0},
    {"single", 's', 0, N_("Boot into single mode."), 0, 0},
    {"verbose", 'v', 0, N_("Boot with verbose messages."), 0, 0},
    {0, 0, 0, 0, 0, 0}
  };

static const grub_uint32_t freebsd_flags[] =
{
  FREEBSD_RB_DUAL, FREEBSD_RB_SERIAL, FREEBSD_RB_ASKNAME,
  FREEBSD_RB_CDROM, FREEBSD_RB_CONFIG, FREEBSD_RB_KDB,
  FREEBSD_RB_GDB, FREEBSD_RB_MUTE, FREEBSD_RB_NOINTR,
  FREEBSD_RB_PAUSE, FREEBSD_RB_QUIET, FREEBSD_RB_DFLTROOT,
  FREEBSD_RB_SINGLE, FREEBSD_RB_VERBOSE, 0
};

static const struct grub_arg_option openbsd_opts[] =
  {
    {"askname", 'a', 0, N_("Ask for file name to reboot from."), 0, 0},
    {"halt", 'b', 0, N_("Don't reboot, just halt."), 0, 0},
    {"config", 'c', 0, N_("Change configured devices."), 0, 0},
    {"single", 's', 0, N_("Boot into single mode."), 0, 0},
    {"kdb", 'd', 0, N_("Enter in KDB on boot."), 0, 0},
    {"root", 'r', 0, N_("Set root device."), "wdXY", ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };

static const grub_uint32_t openbsd_flags[] =
{
  OPENBSD_RB_ASKNAME, OPENBSD_RB_HALT, OPENBSD_RB_CONFIG,
  OPENBSD_RB_SINGLE, OPENBSD_RB_KDB, 0
};

#define OPENBSD_ROOT_ARG (ARRAY_SIZE (openbsd_flags) - 1)

static const struct grub_arg_option netbsd_opts[] =
  {
    {"no-smp", '1', 0, N_("Disable SMP."), 0, 0},
    {"no-acpi", '2', 0, N_("Disable ACPI."), 0, 0},
    {"askname", 'a', 0, N_("Ask for file name to reboot from."), 0, 0},
    {"halt", 'b', 0, N_("Don't reboot, just halt."), 0, 0},
    {"config", 'c', 0, N_("Change configured devices."), 0, 0},
    {"kdb", 'd', 0, N_("Enter in KDB on boot."), 0, 0},
    {"miniroot", 'm', 0, "", 0, 0},
    {"quiet", 'q', 0, N_("Don't display boot diagnostic messages."), 0, 0},
    {"single", 's', 0, N_("Boot into single mode."), 0, 0},
    {"verbose", 'v', 0, N_("Boot with verbose messages."), 0, 0},
    {"debug", 'x', 0, N_("Boot with debug messages."), 0, 0},
    {"silent", 'z', 0, N_("Supress normal output (warnings remain)."), 0, 0},
    {"root", 'r', 0, N_("Set root device."), N_("DEVICE"), ARG_TYPE_STRING},
    {"serial", 'h', GRUB_ARG_OPTION_OPTIONAL, 
     N_("Use serial console."), N_("[ADDR|comUNIT][,SPEED]"), ARG_TYPE_STRING},
    {0, 0, 0, 0, 0, 0}
  };

static const grub_uint32_t netbsd_flags[] =
{
  NETBSD_AB_NOSMP, NETBSD_AB_NOACPI, NETBSD_RB_ASKNAME,
  NETBSD_RB_HALT, NETBSD_RB_USERCONFIG, NETBSD_RB_KDB,
  NETBSD_RB_MINIROOT, NETBSD_AB_QUIET, NETBSD_RB_SINGLE,
  NETBSD_AB_VERBOSE, NETBSD_AB_DEBUG, NETBSD_AB_SILENT, 0
};

#define NETBSD_ROOT_ARG (ARRAY_SIZE (netbsd_flags) - 1)
#define NETBSD_SERIAL_ARG (ARRAY_SIZE (netbsd_flags))

static void
grub_bsd_get_device (grub_uint32_t * biosdev,
		     grub_uint32_t * unit,
		     grub_uint32_t * slice, grub_uint32_t * part)
{
  char *p;
  grub_device_t dev;

#ifdef GRUB_MACHINE_PCBIOS
  *biosdev = grub_get_root_biosnumber () & 0xff;
#else
  *biosdev = 0xff;
#endif
  *unit = (*biosdev & 0x7f);
  *slice = 0xff;
  *part = 0xff;
  dev = grub_device_open (0);
  if (dev && dev->disk && dev->disk->partition)
    {

      p = dev->disk->partition->partmap->get_name (dev->disk->partition);
      if (p)
	{
	  if ((p[0] >= '0') && (p[0] <= '9'))
	    {
	      *slice = grub_strtoul (p, &p, 0);

	      if ((p) && (p[0] == ','))
		p++;
	    }

	  if ((p[0] >= 'a') && (p[0] <= 'z'))
	    *part = p[0] - 'a';
	}
    }
  if (dev)
    grub_device_close (dev);
}

grub_err_t
grub_bsd_add_meta (grub_uint32_t type, void *data, grub_uint32_t len)
{
  struct bsd_tag *newtag;

  newtag = grub_malloc (len + sizeof (struct bsd_tag));
  if (!newtag)
    return grub_errno;
  newtag->len = len;
  newtag->type = type;
  newtag->next = NULL;
  if (len)
    grub_memcpy (newtag->data, data, len);
  if (tags_last)
    tags_last->next = newtag;
  else
    tags = newtag;
  tags_last = newtag;

  return GRUB_ERR_NONE;
}

struct grub_e820_mmap
{
  grub_uint64_t addr;
  grub_uint64_t size;
  grub_uint32_t type;
} __attribute__((packed));
#define GRUB_E820_RAM        1
#define GRUB_E820_RESERVED   2
#define GRUB_E820_ACPI       3
#define GRUB_E820_NVS        4
#define GRUB_E820_EXEC_CODE  5

static void
generate_e820_mmap (grub_size_t *len, grub_size_t *cnt, void *buf)
{
  int count = 0;
  int isfirstrun = 1;
  struct grub_e820_mmap *mmap = buf;
  struct grub_e820_mmap prev, cur;

  auto int NESTED_FUNC_ATTR hook (grub_uint64_t, grub_uint64_t, grub_uint32_t);
  int NESTED_FUNC_ATTR hook (grub_uint64_t addr, grub_uint64_t size,
			     grub_uint32_t type)
    {
      /* FreeBSD assumes that first 64KiB are available.
	 Not always true but try to prevent panic somehow. */
      if (kernel_type == KERNEL_TYPE_FREEBSD && isfirstrun && addr != 0)
	{
	  cur.addr = 0;
	  cur.size = (addr < 0x10000) ? addr : 0x10000;
	  cur.type = GRUB_E820_RAM;
	  if (mmap)
	    *mmap++ = cur;

	  prev = cur;
	  count++;
	}
      isfirstrun = 0;

      cur.addr = addr;
      cur.size = size;
      switch (type)
	{
	case GRUB_MACHINE_MEMORY_AVAILABLE:
	  cur.type = GRUB_E820_RAM;
	  break;

#ifdef GRUB_MACHINE_MEMORY_ACPI
	case GRUB_MACHINE_MEMORY_ACPI:
	  cur.type = GRUB_E820_ACPI;
	  break;
#endif

#ifdef GRUB_MACHINE_MEMORY_NVS
	case GRUB_MACHINE_MEMORY_NVS:
	  cur.type = GRUB_E820_NVS;
	  break;
#endif

	default:
#ifdef GRUB_MACHINE_MEMORY_CODE
	case GRUB_MACHINE_MEMORY_CODE:
#endif
#ifdef GRUB_MACHINE_MEMORY_RESERVED
	case GRUB_MACHINE_MEMORY_RESERVED:
#endif
	  cur.type = GRUB_E820_RESERVED;
	  break;
	}

      /* Merge regions if possible. */
      if (count && cur.type == prev.type && cur.addr == prev.addr + prev.size)
	{
	  prev.size += cur.size;
	  if (mmap)
	    mmap[-1] = cur;
	}
      else
	{
	  if (mmap)
	    *mmap++ = cur;
	  prev = cur;
	  count++;
	}

      return 0;
    }

  isfirstrun = 1;
  grub_mmap_iterate (hook);

  if (len)
    *len = count * sizeof (struct grub_e820_mmap);
  *cnt = count;

  return;
}

static grub_err_t
grub_bsd_add_mmap (void)
{
  grub_size_t len, cnt;
  void *buf = NULL, *buf0;

  generate_e820_mmap (&len, &cnt, buf);

  if (kernel_type == KERNEL_TYPE_NETBSD)
    len += sizeof (grub_uint32_t);

  buf = grub_malloc (len);
  if (!buf)
    return grub_errno;

  buf0 = buf;
  if (kernel_type == KERNEL_TYPE_NETBSD)
    {
      *(grub_uint32_t *) buf = cnt;
      buf = ((grub_uint32_t *) buf + 1);
    }

  generate_e820_mmap (NULL, &cnt, buf);

  grub_dprintf ("bsd", "%u entries in smap\n", cnt);
  if (kernel_type == KERNEL_TYPE_NETBSD)
    grub_bsd_add_meta (NETBSD_BTINFO_MEMMAP, buf0, len);
  else
    grub_bsd_add_meta (FREEBSD_MODINFO_METADATA |
		       FREEBSD_MODINFOMD_SMAP, buf0, len);

  grub_free (buf0);

  return grub_errno;
}

grub_err_t
grub_freebsd_add_meta_module (char *filename, char *type, int argc, char **argv,
			      grub_addr_t addr, grub_uint32_t size)
{
  char *name;
  name = grub_strrchr (filename, '/');
  if (name)
    name++;
  else
    name = filename;
  if (grub_strcmp (type, "/boot/zfs/zpool.cache") == 0)
    name = "/boot/zfs/zpool.cache";

  if (grub_bsd_add_meta (FREEBSD_MODINFO_NAME, name, grub_strlen (name) + 1))
    return grub_errno;

  if (is_64bit)
    {
      grub_uint64_t addr64 = addr, size64 = size;
      if (grub_bsd_add_meta (FREEBSD_MODINFO_TYPE, type, grub_strlen (type) + 1)
	  || grub_bsd_add_meta (FREEBSD_MODINFO_ADDR, &addr64, sizeof (addr64)) 
	  || grub_bsd_add_meta (FREEBSD_MODINFO_SIZE, &size64, sizeof (size64)))
	return grub_errno;
    }
  else
    {
      if (grub_bsd_add_meta (FREEBSD_MODINFO_TYPE, type, grub_strlen (type) + 1)
	  || grub_bsd_add_meta (FREEBSD_MODINFO_ADDR, &addr, sizeof (addr))
	  || grub_bsd_add_meta (FREEBSD_MODINFO_SIZE, &size, sizeof (size)))
	return grub_errno;
    }

  if (argc)
    {
      int i, n;

      n = 0;
      for (i = 0; i < argc; i++)
	{
	  n += grub_strlen (argv[i]) + 1;
	}

      if (n)
	{
	  char cmdline[n], *p;

	  p = cmdline;
	  for (i = 0; i < argc; i++)
	    {
	      grub_strcpy (p, argv[i]);
	      p += grub_strlen (argv[i]);
	      *(p++) = ' ';
	    }
	  *p = 0;

	  if (grub_bsd_add_meta (FREEBSD_MODINFO_ARGS, cmdline, n))
	    return grub_errno;
	}
    }

  return GRUB_ERR_NONE;
}

static void
grub_freebsd_list_modules (void)
{
  struct bsd_tag *tag;

  grub_printf ("  %-18s  %-18s%14s%14s\n", "name", "type", "addr", "size");

  for (tag = tags; tag; tag = tag->next)
    {
      switch (tag->type)
	{
	case FREEBSD_MODINFO_NAME:
	case FREEBSD_MODINFO_TYPE:
	  grub_printf ("  %-18s", (char *) tag->data);
	  break;
	case FREEBSD_MODINFO_ADDR:
	  {
	    grub_uint32_t addr;

	    addr = *((grub_uint32_t *) tag->data);
	    grub_printf ("    0x%08x", addr);
	    break;
	  }
	case FREEBSD_MODINFO_SIZE:
	  {
	    grub_uint32_t len;

	    len = *((grub_uint32_t *) tag->data);
	    grub_printf ("    0x%08x\n", len);
	  }
	}
    }
}

/* This function would be here but it's under different license. */
#include "bsd_pagetable.c"

static grub_err_t
grub_freebsd_boot (void)
{
  struct grub_freebsd_bootinfo bi;
  grub_uint8_t *p, *p0;
  grub_addr_t p_target;
  grub_size_t p_size = 0;
  grub_uint32_t bootdev, biosdev, unit, slice, part;
  grub_err_t err;
  grub_size_t tag_buf_len = 0;

  auto int iterate_env (struct grub_env_var *var);
  int iterate_env (struct grub_env_var *var)
  {
    if ((!grub_memcmp (var->name, "kFreeBSD.", sizeof("kFreeBSD.") - 1)) && (var->name[sizeof("kFreeBSD.") - 1]))
      {
	grub_strcpy ((char *) p, &var->name[sizeof("kFreeBSD.") - 1]);
	p += grub_strlen ((char *) p);
	*(p++) = '=';
	grub_strcpy ((char *) p, var->value);
	p += grub_strlen ((char *) p) + 1;
      }

    return 0;
  }

  auto int iterate_env_count (struct grub_env_var *var);
  int iterate_env_count (struct grub_env_var *var)
  {
    if ((!grub_memcmp (var->name, "kFreeBSD.", sizeof("kFreeBSD.") - 1)) && (var->name[sizeof("kFreeBSD.") - 1]))
      {
	p_size += grub_strlen (&var->name[sizeof("kFreeBSD.") - 1]);
	p_size++;
	p_size += grub_strlen (var->value) + 1;
      }

    return 0;
  }

  grub_memset (&bi, 0, sizeof (bi));
  bi.bi_version = FREEBSD_BOOTINFO_VERSION;
  bi.bi_size = sizeof (bi);

  grub_bsd_get_device (&biosdev, &unit, &slice, &part);
  bootdev = (FREEBSD_B_DEVMAGIC + ((slice + 1) << FREEBSD_B_SLICESHIFT) +
	     (unit << FREEBSD_B_UNITSHIFT) + (part << FREEBSD_B_PARTSHIFT));

  bi.bi_bios_dev = biosdev;

  p_size = 0;
  grub_env_iterate (iterate_env_count);

  if (p_size)
    p_size = ALIGN_PAGE (kern_end + p_size + 1) - kern_end;

  if (is_elf_kernel)
    {
      struct bsd_tag *tag;

      err = grub_bsd_add_mmap ();
      if (err)
	return err;

      err = grub_bsd_add_meta (FREEBSD_MODINFO_END, 0, 0);
      if (err)
	return err;
      
      tag_buf_len = 0;
      for (tag = tags; tag; tag = tag->next)
	tag_buf_len = ALIGN_VAR (tag_buf_len
				 + sizeof (struct freebsd_tag_header)
				 + tag->len);
      p_size = ALIGN_PAGE (kern_end + p_size + tag_buf_len) - kern_end;
    }

  if (is_64bit)
    p_size += 4096 * 3;

  err = grub_relocator_alloc_chunk_addr (relocator, (void **) &p,
					 kern_end, p_size);
  if (err)
    return err;
  p_target = kern_end;
  p0 = p;
  kern_end += p_size;

  grub_env_iterate (iterate_env);

  if (p != p0)
    {
      *(p++) = 0;

      bi.bi_envp = p_target;
    }

  if (is_elf_kernel)
    {
      grub_uint8_t *p_tag = p;
      struct bsd_tag *tag;
      
      for (tag = tags; tag; tag = tag->next)
	{
	  struct freebsd_tag_header *head
	    = (struct freebsd_tag_header *) p_tag;
	  head->type = tag->type;
	  head->len = tag->len;
	  p_tag += sizeof (struct freebsd_tag_header);
	  switch (tag->type)
	    {
	    case FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_HOWTO:
	      if (is_64bit)
		*(grub_uint64_t *) p_tag = bootflags;
	      else
		*(grub_uint32_t *) p_tag = bootflags;
	      break;

	    case FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_ENVP:
	      if (is_64bit)
		*(grub_uint64_t *) p_tag = bi.bi_envp;
	      else
		*(grub_uint32_t *) p_tag = bi.bi_envp;
	      break;

	    case FREEBSD_MODINFO_METADATA | FREEBSD_MODINFOMD_KERNEND:
	      if (is_64bit)
		*(grub_uint64_t *) p_tag = kern_end;
	      else
		*(grub_uint32_t *) p_tag = kern_end;
	      break;

	    default:
	      grub_memcpy (p_tag, tag->data, tag->len);
	      break;
	    }
	  p_tag += tag->len;
	  p_tag = ALIGN_VAR (p_tag - p) + p;
	}

      bi.bi_modulep = (p - p0) + p_target;

      p = (ALIGN_PAGE ((p_tag - p0) + p_target) - p_target) + p0;
    }

  bi.bi_kernend = kern_end;

  grub_video_set_mode ("text", 0, 0);

  if (is_64bit)
    {
      struct grub_relocator64_state state;
      grub_uint8_t *pagetable;
      grub_uint32_t *stack;
      grub_addr_t stack_target;

      err = grub_relocator_alloc_chunk_align (relocator, (void **) &stack,
					      &stack_target,
					      0x10000, 0x90000,
					      3 * sizeof (grub_uint32_t)
					      + sizeof (bi), 4,
					      GRUB_RELOCATOR_PREFERENCE_NONE);
      if (err)
	return err;

#ifdef GRUB_MACHINE_EFI
      if (! grub_efi_finish_boot_services ())
	grub_fatal ("cannot exit boot services");
#endif

      pagetable = p;
      fill_bsd64_pagetable (pagetable, (pagetable - p0) + p_target);

      state.cr3 = (pagetable - p0) + p_target;
      state.rsp = stack_target;
      state.rip = (((grub_uint64_t) entry_hi) << 32) | entry;

      stack[0] = entry;
      stack[1] = bi.bi_modulep;
      stack[2] = kern_end;
      return grub_relocator64_boot (relocator, state, 0, 0x40000000);
    }
  else
    {
      struct grub_relocator32_state state;
      grub_uint32_t *stack;
      grub_addr_t stack_target;
      err = grub_relocator_alloc_chunk_align (relocator, (void **) &stack,
					      &stack_target,
					      0x10000, 0x90000,
					      9 * sizeof (grub_uint32_t)
					      + sizeof (bi), 4,
					      GRUB_RELOCATOR_PREFERENCE_NONE);
      if (err)
	return err;

#ifdef GRUB_MACHINE_EFI
      if (! grub_efi_finish_boot_services ())
	grub_fatal ("cannot exit boot services");
#endif

      grub_memcpy (&stack[8], &bi, sizeof (bi));
      state.eip = entry;
      state.esp = stack_target;
      stack[0] = entry; /* "Return" address.  */
      stack[1] = bootflags | FREEBSD_RB_BOOTINFO;
      stack[2] = bootdev;
      stack[3] = 0;
      stack[4] = 0;
      stack[5] = 0;
      stack[6] = stack_target + 9 * sizeof (grub_uint32_t);
      stack[7] = bi.bi_modulep;
      stack[8] = kern_end;
      return grub_relocator32_boot (relocator, state);
    }

  /* Not reached.  */
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_openbsd_boot (void)
{
  grub_uint8_t *buf, *buf0;
  grub_uint32_t *stack;
  grub_addr_t buf_target, argbuf_target_start, argbuf_target_end;
  grub_size_t buf_size;
  struct grub_openbsd_bios_mmap *pm;
  struct grub_openbsd_bootargs *pa;
  struct grub_relocator32_state state;
  grub_err_t err;

  auto int NESTED_FUNC_ATTR count_hook (grub_uint64_t, grub_uint64_t, grub_uint32_t);
  int NESTED_FUNC_ATTR count_hook (grub_uint64_t addr __attribute__ ((unused)),
				   grub_uint64_t size __attribute__ ((unused)),
				   grub_uint32_t type __attribute__ ((unused)))
  {
    buf_size += sizeof (struct grub_openbsd_bios_mmap);
    return 1;
  }

  auto int NESTED_FUNC_ATTR hook (grub_uint64_t, grub_uint64_t, grub_uint32_t);
  int NESTED_FUNC_ATTR hook (grub_uint64_t addr, grub_uint64_t size, grub_uint32_t type)
    {
      pm->addr = addr;
      pm->len = size;

      switch (type)
        {
        case GRUB_MACHINE_MEMORY_AVAILABLE:
	  pm->type = OPENBSD_MMAP_AVAILABLE;
	  break;

        case GRUB_MACHINE_MEMORY_ACPI:
	  pm->type = OPENBSD_MMAP_ACPI;
	  break;

        case GRUB_MACHINE_MEMORY_NVS:
	  pm->type = OPENBSD_MMAP_NVS;
	  break;

	default:
	  pm->type = OPENBSD_MMAP_RESERVED;
	  break;
	}
      pm++;

      return 0;
    }

  buf_target = GRUB_BSD_TEMP_BUFFER;
  buf_size = sizeof (struct grub_openbsd_bootargs) + 9 * sizeof (grub_uint32_t);
  grub_mmap_iterate (count_hook);
  buf_size += sizeof (struct grub_openbsd_bootargs);

  err = grub_relocator_alloc_chunk_addr (relocator, (void **) &buf,
					 buf_target, buf_size);
  if (err)
    return err;
  buf0 = buf;
  stack = (grub_uint32_t *) buf;
  buf = (grub_uint8_t *) (stack + 9);

  argbuf_target_start = buf - buf0 + buf_target;

  pa = (struct grub_openbsd_bootargs *) buf;

  pa->ba_type = OPENBSD_BOOTARG_MMAP;
  pm = (struct grub_openbsd_bios_mmap *) (pa + 1);
  grub_mmap_iterate (hook);

  /* Memory map terminator.  */
  pm->addr = 0;
  pm->len = 0;
  pm->type = 0;
  pm++;
  buf = (grub_uint8_t *) pm;

  pa->ba_size = (char *) pm - (char *) pa;
  pa->ba_next = (struct grub_openbsd_bootargs *) (buf - buf0 + buf_target);
  pa = pa->ba_next;
  pa->ba_type = OPENBSD_BOOTARG_END;
  pa++;
  buf = (grub_uint8_t *) pa;
  argbuf_target_end = buf - buf0 + buf_target;

#ifdef GRUB_MACHINE_EFI
  if (! grub_efi_finish_boot_services ())
    grub_fatal ("cannot exit boot services");
#endif

  state.eip = entry;
  state.esp = ((grub_uint8_t *) stack - buf0) + buf_target;
  stack[0] = entry;
  stack[1] = bootflags;
  stack[2] = openbsd_root;
  stack[3] = OPENBSD_BOOTARG_APIVER;
  stack[4] = 0;
  stack[5] = grub_mmap_get_upper () >> 10;
  stack[6] = grub_mmap_get_lower () >> 10;
  stack[7] = argbuf_target_end - argbuf_target_start;
  stack[8] = argbuf_target_start;

  grub_video_set_mode ("text", 0, 0);

  return grub_relocator32_boot (relocator, state);
}

static grub_err_t
grub_netbsd_setup_video (void)
{
  struct grub_video_mode_info mode_info;
  void *framebuffer;
  const char *modevar;
  struct grub_netbsd_btinfo_framebuf params;
  grub_err_t err;

  modevar = grub_env_get ("gfxpayload");

  /* Now all graphical modes are acceptable.
     May change in future if we have modes without framebuffer.  */
  if (modevar && *modevar != 0)
    {
      char *tmp;
      tmp = grub_malloc (grub_strlen (modevar)
			 + sizeof (";" NETBSD_DEFAULT_VIDEO_MODE));
      if (! tmp)
	return grub_errno;
      grub_sprintf (tmp, "%s;" NETBSD_DEFAULT_VIDEO_MODE, modevar);
      err = grub_video_set_mode (tmp, 0, 0);
      grub_free (tmp);
    }
  else
    err = grub_video_set_mode (NETBSD_DEFAULT_VIDEO_MODE, 0, 0);

  if (err)
    return err;

  err = grub_video_get_info_and_fini (&mode_info, &framebuffer);

  if (err)
    return err;

  params.width = mode_info.width;
  params.height = mode_info.height;
  params.bpp = mode_info.bpp;
  params.pitch = mode_info.pitch;
  params.flags = 0;

  params.fbaddr = (grub_addr_t) framebuffer;

  params.red_mask_size = mode_info.red_mask_size;
  params.red_field_pos = mode_info.red_field_pos;
  params.green_mask_size = mode_info.green_mask_size;
  params.green_field_pos = mode_info.green_field_pos;
  params.blue_mask_size = mode_info.blue_mask_size;
  params.blue_field_pos = mode_info.blue_field_pos;

#ifdef GRUB_MACHINE_PCBIOS
  /* VESA packed modes may come with zeroed mask sizes, which need
     to be set here according to DAC Palette width.  If we don't,
     this results in Linux displaying a black screen.  */
  if (mode_info.bpp <= 8)
    {
      struct grub_vbe_info_block controller_info;
      int status;
      int width = 8;

      status = grub_vbe_bios_get_controller_info (&controller_info);

      if (status == GRUB_VBE_STATUS_OK &&
	  (controller_info.capabilities & GRUB_VBE_CAPABILITY_DACWIDTH))
	status = grub_vbe_bios_set_dac_palette_width (&width);

      if (status != GRUB_VBE_STATUS_OK)
	/* 6 is default after mode reset.  */
	width = 6;

      params.red_mask_size = params.green_mask_size
	= params.blue_mask_size = width;
    }
#endif

  err = grub_bsd_add_meta (NETBSD_BTINFO_FRAMEBUF, &params, sizeof (params));
  return err;
}

static grub_err_t
grub_netbsd_boot (void)
{
  struct grub_netbsd_bootinfo *bootinfo;
  void *curarg, *arg0;
  grub_addr_t arg_target, stack_target;
  grub_uint32_t *stack;
  grub_err_t err;
  struct grub_relocator32_state state;
  grub_size_t tag_buf_len = 0;
  int tag_count = 0;

  err = grub_bsd_add_mmap ();
  if (err)
    return err;

  err = grub_netbsd_setup_video ();
  if (err)
    {
      grub_print_error ();
      grub_printf ("Booting however\n");
      grub_errno = GRUB_ERR_NONE;
    }

  {
    struct bsd_tag *tag;
    tag_buf_len = 0;
    for (tag = tags; tag; tag = tag->next)
      {
	tag_buf_len = ALIGN_VAR (tag_buf_len + 2 * sizeof (grub_uint32_t)
				 + tag->len);
	tag_count++;
      }
  }

  arg_target = kern_end;
  err = grub_relocator_alloc_chunk_addr (relocator, &curarg,
					 arg_target, tag_buf_len
					 + sizeof (struct grub_netbsd_bootinfo)
					 + tag_count * sizeof (grub_addr_t));
  if (err)
    return err;

  arg0 = curarg;
  bootinfo = (void *) ((grub_uint8_t *) arg0 + tag_buf_len);

  {
    struct bsd_tag *tag;
    unsigned i;

    bootinfo->bi_count = tag_count;
    for (tag = tags, i = 0; tag; i++, tag = tag->next)
      {
	struct grub_netbsd_btinfo_common *head = curarg;
	bootinfo->bi_data[i] = ((grub_uint8_t *) curarg - (grub_uint8_t *) arg0)
	  + arg_target;
	head->type = tag->type;
	head->len = tag->len + sizeof (*head);
	curarg = head + 1;
	grub_memcpy (curarg, tag->data, tag->len);
	curarg = (grub_uint8_t *) curarg + tag->len;
      }
  }

  err = grub_relocator_alloc_chunk_align (relocator, (void **) &stack,
					  &stack_target, 0x10000, 0x90000,
					  7 * sizeof (grub_uint32_t), 4,
					  GRUB_RELOCATOR_PREFERENCE_NONE);
  if (err)
    return err;

#ifdef GRUB_MACHINE_EFI
  if (! grub_efi_finish_boot_services ())
    grub_fatal ("cannot exit boot services");
#endif

  state.eip = entry;
  state.esp = stack_target;
  stack[0] = entry;
  stack[1] = bootflags;
  stack[2] = 0;
  stack[3] = ((grub_uint8_t *) bootinfo - (grub_uint8_t *) arg0) + arg_target;
  stack[4] = 0;
  stack[5] = grub_mmap_get_upper () >> 10;
  stack[6] = grub_mmap_get_lower () >> 10;

  return grub_relocator32_boot (relocator, state);
}

static grub_err_t
grub_bsd_unload (void)
{
  struct bsd_tag *tag, *next;
  for (tag = tags; tag; tag = next)
    {
      next = tag->next;
      grub_free (tag);
    }
  tags = NULL;
  tags_last = NULL;

  kernel_type = KERNEL_TYPE_NONE;
  grub_dl_unref (my_mod);

  grub_relocator_unload (relocator);
  relocator = NULL;

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_bsd_load_aout (grub_file_t file)
{
  grub_addr_t load_addr, load_end;
  int ofs, align_page;
  union grub_aout_header ah;
  grub_err_t err;
  grub_size_t bss_size;

  if ((grub_file_seek (file, 0)) == (grub_off_t) - 1)
    return grub_errno;

  if (grub_file_read (file, &ah, sizeof (ah)) != sizeof (ah))
    return grub_error (GRUB_ERR_READ_ERROR, "cannot read the a.out header");

  if (grub_aout_get_type (&ah) != AOUT_TYPE_AOUT32)
    return grub_error (GRUB_ERR_BAD_OS, "invalid a.out header");

  entry = ah.aout32.a_entry & 0xFFFFFF;

  if (AOUT_GETMAGIC (ah.aout32) == AOUT32_ZMAGIC)
    {
      load_addr = entry;
      ofs = 0x1000;
      align_page = 0;
    }
  else
    {
      load_addr = entry & 0xF00000;
      ofs = sizeof (struct grub_aout32_header);
      align_page = 1;
    }

  if (load_addr < 0x100000)
    return grub_error (GRUB_ERR_BAD_OS, "load address below 1M");

  kern_start = load_addr;
  load_end = kern_end = load_addr + ah.aout32.a_text + ah.aout32.a_data;
  if (align_page)
    kern_end = ALIGN_PAGE (kern_end);

  if (ah.aout32.a_bss)
    {
      kern_end += ah.aout32.a_bss;
      if (align_page)
	kern_end = ALIGN_PAGE (kern_end);

      bss_size = kern_end - load_end;
    }
  else
    bss_size = 0;

  relocator = grub_relocator_new ();
  if (!relocator)
    return grub_errno;

  err = grub_relocator_alloc_chunk_addr (relocator, &kern_chunk_src,
					 kern_start, kern_end - kern_start);
  if (err)
    return err;

  return grub_aout_load (file, ofs, kern_chunk_src,
			 ah.aout32.a_text + ah.aout32.a_data,
			 bss_size);
}

static int NESTED_FUNC_ATTR
grub_bsd_elf32_size_hook (grub_elf_t elf __attribute__ ((unused)),
			  Elf32_Phdr *phdr, void *arg __attribute__ ((unused)))
{
  Elf32_Addr paddr;

  if (phdr->p_type != PT_LOAD
      && phdr->p_type != PT_DYNAMIC)
      return 0;

  paddr = phdr->p_paddr & 0xFFFFFF;

  if (paddr < kern_start)
    kern_start = paddr;

  if (paddr + phdr->p_memsz > kern_end)
    kern_end = paddr + phdr->p_memsz;

  return 0;
}

static grub_err_t
grub_bsd_elf32_hook (Elf32_Phdr * phdr, grub_addr_t * addr, int *do_load)
{
  Elf32_Addr paddr;

  if (phdr->p_type != PT_LOAD
      && phdr->p_type != PT_DYNAMIC)
    {
      *do_load = 0;
      return 0;
    }

  *do_load = 1;
  phdr->p_paddr &= 0xFFFFFF;
  paddr = phdr->p_paddr;

  *addr = (grub_addr_t) (paddr - kern_start + (grub_uint8_t *) kern_chunk_src);

  return GRUB_ERR_NONE;
}

static int NESTED_FUNC_ATTR
grub_bsd_elf64_size_hook (grub_elf_t elf __attribute__ ((unused)),
			  Elf64_Phdr *phdr, void *arg __attribute__ ((unused)))
{
  Elf64_Addr paddr;

  if (phdr->p_type != PT_LOAD
      && phdr->p_type != PT_DYNAMIC)
    return 0;

  paddr = phdr->p_paddr & 0xffffff;

  if (paddr < kern_start)
    kern_start = paddr;

  if (paddr + phdr->p_memsz > kern_end)
    kern_end = paddr + phdr->p_memsz;

  return 0;
}

static grub_err_t
grub_bsd_elf64_hook (Elf64_Phdr * phdr, grub_addr_t * addr, int *do_load)
{
  Elf64_Addr paddr;

  if (phdr->p_type != PT_LOAD
      && phdr->p_type != PT_DYNAMIC)
    {
      *do_load = 0;
      return 0;
    }

  *do_load = 1;
  paddr = phdr->p_paddr & 0xffffff;

  *addr = (grub_addr_t) (paddr - kern_start + (grub_uint8_t *) kern_chunk_src);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_bsd_load_elf (grub_elf_t elf)
{
  grub_err_t err;

  kern_end = 0;
  kern_start = ~0;

  if (grub_elf_is_elf32 (elf))
    {
      entry = elf->ehdr.ehdr32.e_entry & 0xFFFFFF;
      err = grub_elf32_phdr_iterate (elf, grub_bsd_elf32_size_hook, NULL);
      if (err)
	return err;
      err = grub_relocator_alloc_chunk_addr (relocator, &kern_chunk_src,
					     kern_start, kern_end - kern_start);
      if (err)
	return err;

      return grub_elf32_load (elf, grub_bsd_elf32_hook, 0, 0);
    }
  else if (grub_elf_is_elf64 (elf))
    {
      is_64bit = 1;

      if (! grub_cpuid_has_longmode)
	return grub_error (GRUB_ERR_BAD_OS, "your CPU does not implement AMD64 architecture");

      /* FreeBSD has 64-bit entry point.  */
      if (kernel_type == KERNEL_TYPE_FREEBSD)
	{
	  entry = elf->ehdr.ehdr64.e_entry & 0xffffffff;
	  entry_hi = (elf->ehdr.ehdr64.e_entry >> 32) & 0xffffffff;
	}
      else
	{
	  entry = elf->ehdr.ehdr64.e_entry & 0x0fffffff;
	  entry_hi = 0;
	}

      err = grub_elf64_phdr_iterate (elf, grub_bsd_elf64_size_hook, NULL);
      if (err)
	return err;

      grub_dprintf ("bsd", "kern_start = %lx, kern_end = %lx\n",
		    (unsigned long) kern_start, (unsigned long) kern_end);
      err = grub_relocator_alloc_chunk_addr (relocator, &kern_chunk_src,
					     kern_start, kern_end - kern_start);
      if (err)
	return err;

      return grub_elf64_load (elf, grub_bsd_elf64_hook, 0, 0);
    }
  else
    return grub_error (GRUB_ERR_BAD_OS, "invalid ELF");
}

static grub_err_t
grub_bsd_load (int argc, char *argv[])
{
  grub_file_t file;
  grub_elf_t elf;

  grub_dl_ref (my_mod);

  grub_loader_unset ();

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "no kernel specified");
      goto fail;
    }

  file = grub_gzfile_open (argv[0], 1);
  if (!file)
    goto fail;

  relocator = grub_relocator_new ();

  elf = grub_elf_file (file);
  if (elf)
    {
      is_elf_kernel = 1;
      grub_bsd_load_elf (elf);
      grub_elf_close (elf);
    }
  else
    {
      is_elf_kernel = 0;
      grub_errno = 0;
      grub_bsd_load_aout (file);
      grub_file_close (file);
    }

fail:

  if (grub_errno != GRUB_ERR_NONE)
    grub_dl_unref (my_mod);

  return grub_errno;
}

static grub_uint32_t
grub_bsd_parse_flags (const struct grub_arg_list *state,
		      const grub_uint32_t * flags)
{
  grub_uint32_t result = 0;
  unsigned i;

  for (i = 0; flags[i]; i++)
    if (state[i].set)
      result |= flags[i];

  return result;
}

static grub_err_t
grub_cmd_freebsd (grub_extcmd_t cmd, int argc, char *argv[])
{
  kernel_type = KERNEL_TYPE_FREEBSD;
  bootflags = grub_bsd_parse_flags (cmd->state, freebsd_flags);

  if (grub_bsd_load (argc, argv) == GRUB_ERR_NONE)
    {
      kern_end = ALIGN_PAGE (kern_end);
      if (is_elf_kernel)
	{
	  grub_err_t err;
	  grub_uint64_t data = 0;
	  grub_file_t file;
	  int len = is_64bit ? 8 : 4;

	  err = grub_freebsd_add_meta_module (argv[0], is_64bit
					      ? FREEBSD_MODTYPE_KERNEL64
					      : FREEBSD_MODTYPE_KERNEL,
					      argc - 1, argv + 1,
					      kern_start,
					      kern_end - kern_start);
	  if (err)
	    return err;

	  file = grub_gzfile_open (argv[0], 1);
	  if (! file)
	    return grub_errno;

	  if (is_64bit)
	    err = grub_freebsd_load_elf_meta64 (relocator, file, &kern_end);
	  else
	    err = grub_freebsd_load_elf_meta32 (relocator, file, &kern_end);
	  if (err)
	    return err;

	  err = grub_bsd_add_meta (FREEBSD_MODINFO_METADATA |
				   FREEBSD_MODINFOMD_HOWTO, &data, 4);
	  if (err)
	    return err;

	  err = grub_bsd_add_meta (FREEBSD_MODINFO_METADATA |
				       FREEBSD_MODINFOMD_ENVP, &data, len);
	  if (err)
	    return err;

	  err = grub_bsd_add_meta (FREEBSD_MODINFO_METADATA |
				   FREEBSD_MODINFOMD_KERNEND, &data, len);
	  if (err)
	    return err;
	}
      grub_loader_set (grub_freebsd_boot, grub_bsd_unload, 0);
    }

  return grub_errno;
}

static grub_err_t
grub_cmd_openbsd (grub_extcmd_t cmd, int argc, char *argv[])
{
  grub_uint32_t bootdev;

  kernel_type = KERNEL_TYPE_OPENBSD;
  bootflags = grub_bsd_parse_flags (cmd->state, openbsd_flags);

  if (cmd->state[OPENBSD_ROOT_ARG].set)
    {
      const char *arg = cmd->state[OPENBSD_ROOT_ARG].arg;
      int unit, part;
      if (*(arg++) != 'w' || *(arg++) != 'd')
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   "only device specifications of form "
			   "wd<number><lowercase letter> are supported");

      unit = grub_strtoul (arg, (char **) &arg, 10);
      if (! (arg && *arg >= 'a' && *arg <= 'z'))
	return grub_error (GRUB_ERR_BAD_ARGUMENT,
			   "only device specifications of form "
			   "wd<number><lowercase letter> are supported");

      part = *arg - 'a';

      bootdev = (OPENBSD_B_DEVMAGIC + (unit << OPENBSD_B_UNITSHIFT) +
		 (part << OPENBSD_B_PARTSHIFT));
    }
  else
    bootdev = 0;

  if (grub_bsd_load (argc, argv) == GRUB_ERR_NONE)
    {
      grub_loader_set (grub_openbsd_boot, grub_bsd_unload, 1);
      openbsd_root = bootdev;
    }

  return grub_errno;
}

static grub_err_t
grub_cmd_netbsd (grub_extcmd_t cmd, int argc, char *argv[])
{
  grub_err_t err;
  kernel_type = KERNEL_TYPE_NETBSD;
  bootflags = grub_bsd_parse_flags (cmd->state, netbsd_flags);

  if (grub_bsd_load (argc, argv) == GRUB_ERR_NONE)
    {
      if (is_elf_kernel)
	{
	  grub_file_t file;

	  file = grub_gzfile_open (argv[0], 1);
	  if (! file)
	    return grub_errno;

	  if (is_64bit)
	    err = grub_netbsd_load_elf_meta64 (relocator, file, &kern_end);
	  else
	    err = grub_netbsd_load_elf_meta32 (relocator, file, &kern_end);
	  if (err)
	    return err;
	}

      {
	char bootpath[GRUB_NETBSD_MAX_BOOTPATH_LEN];
	char *name;
	name = grub_strrchr (argv[0], '/');
	if (name)
	  name++;
	else
	  name = argv[0];
	grub_memset (bootpath, 0, sizeof (bootpath));
	grub_strncpy (bootpath, name, sizeof (bootpath) - 1);
	grub_bsd_add_meta (NETBSD_BTINFO_BOOTPATH, bootpath, sizeof (bootpath));
      }

      if (cmd->state[NETBSD_ROOT_ARG].set)
	{
	  char root[GRUB_NETBSD_MAX_ROOTDEVICE_LEN];
	  grub_memset (root, 0, sizeof (root));
	  grub_strncpy (root, cmd->state[NETBSD_ROOT_ARG].arg,
			sizeof (root) - 1);
	  grub_bsd_add_meta (NETBSD_BTINFO_ROOTDEVICE, root, sizeof (root));
	}
      if (cmd->state[NETBSD_SERIAL_ARG].set)
	{
	  struct grub_netbsd_btinfo_serial serial;
	  char *ptr;

	  grub_memset (&serial, 0, sizeof (serial));
	  grub_strcpy (serial.devname, "com");

	  if (cmd->state[NETBSD_SERIAL_ARG].arg)
	    {
	      ptr = cmd->state[NETBSD_SERIAL_ARG].arg;
	      if (grub_memcmp (ptr, "com", sizeof ("com") - 1) == 0)
		{
		  ptr += sizeof ("com") - 1;
		  serial.addr 
		    = grub_serial_hw_get_port (grub_strtoul (ptr, &ptr, 0));
		}
	      else
		serial.addr = grub_strtoul (ptr, &ptr, 0);
	      if (grub_errno)
		return grub_errno;

	      if (*ptr == ',')
		{
		  ptr++;
		  serial.speed = grub_strtoul (ptr, &ptr, 0);
		  if (grub_errno)
		    return grub_errno;
		}
	    }
	  
 	  grub_bsd_add_meta (NETBSD_BTINFO_CONSOLE, &serial, sizeof (serial));
	}
      else
	{
	  struct grub_netbsd_btinfo_serial cons;

	  grub_memset (&cons, 0, sizeof (cons));
	  grub_strcpy (cons.devname, "pc");

	  grub_bsd_add_meta (NETBSD_BTINFO_CONSOLE, &cons, sizeof (cons));
	}

      grub_loader_set (grub_netbsd_boot, grub_bsd_unload, 0);
    }

  return grub_errno;
}

static grub_err_t
grub_cmd_freebsd_loadenv (grub_command_t cmd __attribute__ ((unused)),
			  int argc, char *argv[])
{
  grub_file_t file = 0;
  char *buf = 0, *curr, *next;
  int len;

  if (kernel_type == KERNEL_TYPE_NONE)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "you need to load the kernel first");

  if (kernel_type != KERNEL_TYPE_FREEBSD)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "only FreeBSD supports environment");

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, "no filename");
      goto fail;
    }

  file = grub_gzfile_open (argv[0], 1);
  if ((!file) || (!file->size))
    goto fail;

  len = file->size;
  buf = grub_malloc (len + 1);
  if (!buf)
    goto fail;

  if (grub_file_read (file, buf, len) != len)
    goto fail;

  buf[len] = 0;

  next = buf;
  while (next)
    {
      char *p;

      curr = next;
      next = grub_strchr (curr, '\n');
      if (next)
	{

	  p = next - 1;
	  while (p > curr)
	    {
	      if ((*p != '\r') && (*p != ' ') && (*p != '\t'))
		break;
	      p--;
	    }

	  if ((p > curr) && (*p == '"'))
	    p--;

	  *(p + 1) = 0;
	  next++;
	}

      if (*curr == '#')
	continue;

      p = grub_strchr (curr, '=');
      if (!p)
	continue;

      *(p++) = 0;

      if (*curr)
	{
	  char name[grub_strlen (curr) + sizeof("kFreeBSD.")];

	  if (*p == '"')
	    p++;

	  grub_sprintf (name, "kFreeBSD.%s", curr);
	  if (grub_env_set (name, p))
	    goto fail;
	}
    }

fail:
  grub_free (buf);

  if (file)
    grub_file_close (file);

  return grub_errno;
}

static grub_err_t
grub_cmd_freebsd_module (grub_command_t cmd __attribute__ ((unused)),
			 int argc, char *argv[])
{
  grub_file_t file = 0;
  int modargc;
  char **modargv;
  char *type;
  grub_err_t err;
  void *src;

  if (kernel_type == KERNEL_TYPE_NONE)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "you need to load the kernel first");

  if (kernel_type != KERNEL_TYPE_FREEBSD)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "only FreeBSD supports module");

  if (!is_elf_kernel)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "only ELF kernel supports module");

  /* List the current modules if no parameter.  */
  if (!argc)
    {
      grub_freebsd_list_modules ();
      return 0;
    }

  file = grub_gzfile_open (argv[0], 1);
  if ((!file) || (!file->size))
    goto fail;

  err = grub_relocator_alloc_chunk_addr (relocator, &src, kern_end, 
					 file->size);
  if (err)
    goto fail;

  grub_file_read (file, src, file->size);
  if (grub_errno)
    goto fail;

  modargc = argc - 1;
  modargv = argv + 1;

  if (modargc && (! grub_memcmp (modargv[0], "type=", 5)))
    {
      type = &modargv[0][5];
      modargc--;
      modargv++;
    }
  else
    type = FREEBSD_MODTYPE_RAW;

  err = grub_freebsd_add_meta_module (argv[0], type, modargc, modargv,
				      kern_end, file->size);
  if (err)
    goto fail;

  kern_end = ALIGN_PAGE (kern_end + file->size);

fail:
  if (file)
    grub_file_close (file);

  return grub_errno;
}

static grub_err_t
grub_cmd_freebsd_module_elf (grub_command_t cmd __attribute__ ((unused)),
			     int argc, char *argv[])
{
  grub_file_t file = 0;
  grub_err_t err;

  if (kernel_type == KERNEL_TYPE_NONE)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "you need to load the kernel first");

  if (kernel_type != KERNEL_TYPE_FREEBSD)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "only FreeBSD supports module");

  if (! is_elf_kernel)
    return grub_error (GRUB_ERR_BAD_ARGUMENT,
		       "only ELF kernel supports module");

  /* List the current modules if no parameter.  */
  if (! argc)
    {
      grub_freebsd_list_modules ();
      return 0;
    }

  file = grub_gzfile_open (argv[0], 1);
  if (!file)
    return grub_errno;
  if (!file->size)
    {
      grub_file_close (file);
      return grub_errno;
    }

  if (is_64bit)
    err = grub_freebsd_load_elfmodule_obj64 (relocator, file,
					     argc, argv, &kern_end);
  else
    err = grub_freebsd_load_elfmodule32 (relocator, file,
					 argc, argv, &kern_end);
  grub_file_close (file);

  return err;
}


static grub_extcmd_t cmd_freebsd, cmd_openbsd, cmd_netbsd;
static grub_command_t cmd_freebsd_loadenv, cmd_freebsd_module;
static grub_command_t cmd_freebsd_module_elf;

GRUB_MOD_INIT (bsd)
{
  cmd_freebsd = grub_register_extcmd ("kfreebsd", grub_cmd_freebsd,
				      GRUB_COMMAND_FLAG_BOTH,
				      N_("FILE"), N_("Load kernel of FreeBSD."),
				      freebsd_opts);
  cmd_openbsd = grub_register_extcmd ("kopenbsd", grub_cmd_openbsd,
				      GRUB_COMMAND_FLAG_BOTH,
				      N_("FILE"), N_("Load kernel of OpenBSD."),
				      openbsd_opts);
  cmd_netbsd = grub_register_extcmd ("knetbsd", grub_cmd_netbsd,
				     GRUB_COMMAND_FLAG_BOTH,
				     N_("FILE"), N_("Load kernel of NetBSD."),
				     netbsd_opts);
  cmd_freebsd_loadenv =
    grub_register_command ("kfreebsd_loadenv", grub_cmd_freebsd_loadenv,
			   0, N_("Load FreeBSD env."));
  cmd_freebsd_module =
    grub_register_command ("kfreebsd_module", grub_cmd_freebsd_module,
			   0, N_("Load FreeBSD kernel module."));
  cmd_freebsd_module_elf =
    grub_register_command ("kfreebsd_module_elf", grub_cmd_freebsd_module_elf,
			   0, N_("Load FreeBSD kernel module (ELF)."));

  my_mod = mod;
}

GRUB_MOD_FINI (bsd)
{
  grub_unregister_extcmd (cmd_freebsd);
  grub_unregister_extcmd (cmd_openbsd);
  grub_unregister_extcmd (cmd_netbsd);

  grub_unregister_command (cmd_freebsd_loadenv);
  grub_unregister_command (cmd_freebsd_module);
  grub_unregister_command (cmd_freebsd_module_elf);

  grub_bsd_unload ();
}
