/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2012  Free Software Foundation, Inc.
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
#include <grub/file.h>
#include <grub/err.h>
#include <grub/types.h>
#include <grub/mm.h>
#include <grub/cpu/linux.h>
#include <grub/command.h>
#include <grub/i18n.h>
#include <grub/lib/cmdline.h>
#include <grub/efi/efi.h>
#include <grub/efi/linux.h>

GRUB_MOD_LICENSE ("GPLv3+");

static grub_dl_t my_mod;
static int loaded;
static void *kernel_mem;
static grub_uint64_t kernel_size;
static grub_uint8_t *initrd_mem;
static grub_uint32_t handover_offset;
struct linux_kernel_params *params;
static char *linux_cmdline;

#define BYTES_TO_PAGES(bytes)   (((bytes) + 0xfff) >> 12)

static grub_err_t
grub_linuxefi_boot (void)
{
  int offset = 0;

#ifdef __x86_64__
  offset = 512;
#endif
  asm volatile ("cli");

  return grub_efi_linux_boot ((char *)kernel_mem, handover_offset + offset,
			      params);
}

static grub_err_t
grub_linuxefi_unload (void)
{
  grub_dl_unref (my_mod);
  loaded = 0;
  if (initrd_mem)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)initrd_mem,
			 BYTES_TO_PAGES(params->ramdisk_size));
  if (linux_cmdline)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)
			 linux_cmdline,
			 BYTES_TO_PAGES(params->cmdline_size + 1));
  if (kernel_mem)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)kernel_mem,
			 BYTES_TO_PAGES(kernel_size));
  if (params)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)params,
			 BYTES_TO_PAGES(16384));
  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_initrd (grub_command_t cmd __attribute__ ((unused)),
                 int argc, char *argv[])
{
  grub_file_t *files = 0;
  int i, nfiles = 0;
  grub_size_t size = 0;
  grub_uint8_t *ptr;

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
      goto fail;
    }

  if (!loaded)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("you need to load the kernel first"));
      goto fail;
    }

  files = grub_zalloc (argc * sizeof (files[0]));
  if (!files)
    goto fail;

  for (i = 0; i < argc; i++)
    {
      grub_file_filter_disable_compression ();
      files[i] = grub_file_open (argv[i]);
      if (! files[i])
        goto fail;
      nfiles++;
      size += ALIGN_UP (grub_file_size (files[i]), 4);
    }

  initrd_mem = grub_efi_allocate_pages_max (0x3fffffff, BYTES_TO_PAGES(size));
  if (!initrd_mem)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("can't allocate initrd"));
      goto fail;
    }

  params->ramdisk_size = size;
  params->ramdisk_image = (grub_uint32_t)(grub_addr_t) initrd_mem;

  ptr = initrd_mem;

  for (i = 0; i < nfiles; i++)
    {
      grub_ssize_t cursize = grub_file_size (files[i]);
      if (grub_file_read (files[i], ptr, cursize) != cursize)
        {
          if (!grub_errno)
            grub_error (GRUB_ERR_FILE_READ_ERROR, N_("premature end of file %s"),
                        argv[i]);
          goto fail;
        }
      ptr += cursize;
      grub_memset (ptr, 0, ALIGN_UP_OVERHEAD (cursize, 4));
      ptr += ALIGN_UP_OVERHEAD (cursize, 4);
    }

  params->ramdisk_size = size;

 fail:
  for (i = 0; i < nfiles; i++)
    grub_file_close (files[i]);
  grub_free (files);

  if (initrd_mem && grub_errno)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)initrd_mem,
			 BYTES_TO_PAGES(size));

  return grub_errno;
}

static grub_err_t
grub_cmd_linux (grub_command_t cmd __attribute__ ((unused)),
		int argc, char *argv[])
{
  grub_file_t file = 0;
  struct linux_kernel_header lh;
  grub_ssize_t len, start, filelen;
  void *kernel = NULL;

  grub_dl_ref (my_mod);

  if (argc == 0)
    {
      grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));
      goto fail;
    }

  file = grub_file_open (argv[0]);
  if (! file)
    goto fail;

  filelen = grub_file_size (file);

  kernel = grub_malloc(filelen);

  if (!kernel)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("cannot allocate kernel buffer"));
      goto fail;
    }

  if (grub_file_read (file, kernel, filelen) != filelen)
    {
      grub_error (GRUB_ERR_FILE_READ_ERROR, N_("Can't read kernel %s"), argv[0]);
      goto fail;
    }

  if (! grub_linuxefi_secure_validate (kernel, filelen))
    {
      grub_error (GRUB_ERR_INVALID_COMMAND, N_("%s has invalid signature"),
		  argv[0]);
      goto fail;
    }

  params = grub_efi_allocate_pages_max (0x3fffffff, BYTES_TO_PAGES(16384));

  if (! params)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, "cannot allocate kernel parameters");
      goto fail;
    }

  grub_memset (params, 0, 16384);

  grub_memcpy (&lh, kernel, sizeof (lh));

  if (lh.boot_flag != grub_cpu_to_le16 (0xaa55))
    {
      grub_error (GRUB_ERR_BAD_OS, N_("invalid magic number"));
      goto fail;
    }

  if (lh.setup_sects > GRUB_LINUX_MAX_SETUP_SECTS)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("too many setup sectors"));
      goto fail;
    }

  if (lh.version < grub_cpu_to_le16 (0x020b))
    {
      grub_error (GRUB_ERR_BAD_OS, N_("kernel too old"));
      goto fail;
    }

  if (!lh.handover_offset)
    {
      grub_error (GRUB_ERR_BAD_OS, N_("kernel doesn't support EFI handover"));
      goto fail;
    }

  grub_dprintf ("linux", "setting up cmdline\n");
  linux_cmdline = grub_efi_allocate_pages_max(0x3fffffff,
					 BYTES_TO_PAGES(lh.cmdline_size + 1));

  if (!linux_cmdline)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("can't allocate cmdline"));
      goto fail;
    }

  grub_memcpy (linux_cmdline, LINUX_IMAGE, sizeof (LINUX_IMAGE));
  grub_create_loader_cmdline (argc, argv,
                              linux_cmdline + sizeof (LINUX_IMAGE) - 1,
			      lh.cmdline_size - (sizeof (LINUX_IMAGE) - 1));

  lh.cmd_line_ptr = (grub_uint32_t)(grub_addr_t)linux_cmdline;

  handover_offset = lh.handover_offset;

  start = (lh.setup_sects + 1) * 512;
  len = grub_file_size(file) - start;

  kernel_mem = grub_efi_allocate_pages_max(lh.pref_address,
					   BYTES_TO_PAGES(lh.init_size));

  if (!kernel_mem)
    kernel_mem = grub_efi_allocate_pages_max(0x3fffffff,
					     BYTES_TO_PAGES(lh.init_size));

  if (!kernel_mem)
    {
      grub_error (GRUB_ERR_OUT_OF_MEMORY, N_("can't allocate kernel"));
      goto fail;
    }

  grub_memcpy (kernel_mem, (char *)kernel + start, len);
  grub_loader_set (grub_linuxefi_boot, grub_linuxefi_unload, 0);
  loaded=1;

  lh.code32_start = (grub_uint32_t)(grub_uint64_t) kernel_mem;
  grub_memcpy (params, &lh, 2 * 512);

  params->type_of_loader = 0x21;

 fail:

  if (file)
    grub_file_close (file);

  if (kernel)
    grub_free (kernel);

  if (grub_errno != GRUB_ERR_NONE)
    {
      grub_dl_unref (my_mod);
      loaded = 0;
    }

  if (linux_cmdline && !loaded)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)
			 linux_cmdline,
			 BYTES_TO_PAGES(lh.cmdline_size + 1));

  if (kernel_mem && !loaded)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)kernel_mem,
			 BYTES_TO_PAGES(kernel_size));

  if (params && !loaded)
    grub_efi_free_pages ((grub_efi_physical_address_t)(grub_addr_t)params,
			 BYTES_TO_PAGES(16384));

  return grub_errno;
}

static grub_command_t cmd_linux, cmd_initrd;
static grub_command_t cmd_linuxefi, cmd_initrdefi;

GRUB_MOD_INIT(linux)
{
  cmd_linux =
    grub_register_command ("linux", grub_cmd_linux,
                           0, N_("Load Linux."));
  cmd_linuxefi =
    grub_register_command ("linuxefi", grub_cmd_linux,
                           0, N_("Load Linux."));
  cmd_initrd =
    grub_register_command ("initrd", grub_cmd_initrd,
                           0, N_("Load initrd."));
  cmd_initrdefi =
    grub_register_command ("initrdefi", grub_cmd_initrd,
                           0, N_("Load initrd."));
  my_mod = mod;
}

GRUB_MOD_FINI(linux)
{
  grub_unregister_command (cmd_linux);
  grub_unregister_command (cmd_linuxefi);
  grub_unregister_command (cmd_initrd);
  grub_unregister_command (cmd_initrdefi);
}
