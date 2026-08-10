/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "focaltech-qsee-transport.h"
#include "focaltech-qsee-proto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/tee.h>

/* Which /dev/teeN is the QSEECOM one depends on what bound first, so ask. */
#define TEE_IMPL_ID_QSEECOM 5

/*
 * The request region's capacity, which is what the application is told it may
 * work in rather than the message's length. Large enough for the longest
 * payload with room to spare; the application rejects a payload longer than
 * capacity minus the header.
 */
#define REQUEST_CAPACITY 4096

/* The application's log arrives here. Nothing reads it, but it must exist. */
#define RESPONSE_CAPACITY 0x8040

struct shm
{
  int    id;
  void  *va;
  size_t size;
};

static int
shm_alloc (int fd, size_t size, struct shm *out)
{
  struct tee_ioctl_shm_alloc_data data = { .size = size };
  int shm_fd;

  shm_fd = ioctl (fd, TEE_IOC_SHM_ALLOC, &data);
  if (shm_fd < 0)
    return -1;

  out->va = mmap (NULL, data.size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  close (shm_fd);
  if (out->va == MAP_FAILED)
    return -1;

  out->id = data.id;
  out->size = data.size;
  memset (out->va, 0, data.size);

  return 0;
}

static int
open_client (void)
{
  for (int i = 0; i < 8; i++)
    {
      struct tee_ioctl_version_data version;
      char path[32];
      int fd;

      snprintf (path, sizeof (path), "/dev/tee%d", i);
      fd = open (path, O_RDWR | O_CLOEXEC);
      if (fd < 0)
        continue;

      if (!ioctl (fd, TEE_IOC_VERSION, &version) &&
          version.impl_id == TEE_IMPL_ID_QSEECOM)
        return fd;

      close (fd);
    }

  return -1;
}

int
focaltech_qsee_tee_open (struct focaltech_qsee_tee *tee, const char *ta_name)
{
  struct
  {
    struct tee_ioctl_open_session_arg arg;
    struct tee_ioctl_param            params[1];
  } request = { 0 };
  struct tee_ioctl_buf_data data = {
    .buf_ptr = (uintptr_t) &request,
    .buf_len = sizeof (request),
  };
  struct shm name = { 0 };
  size_t length = strlen (ta_name);

  tee->fd = -1;
  tee->session = 0;

  if (length >= 64)
    return -1;

  tee->fd = open_client ();
  if (tee->fd < 0)
    return -1;

  if (shm_alloc (tee->fd, 64, &name))
    goto fail;

  memcpy (name.va, ta_name, length + 1);

  request.arg.num_params = 1;
  request.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
  request.params[0].a = 0;
  request.params[0].b = length + 1;
  request.params[0].c = name.id;

  if (ioctl (tee->fd, TEE_IOC_OPEN_SESSION, &data) || request.arg.ret)
    {
      munmap (name.va, name.size);
      goto fail;
    }

  tee->session = request.arg.session;
  munmap (name.va, name.size);

  return 0;

fail:
  close (tee->fd);
  tee->fd = -1;

  return -1;
}

void
focaltech_qsee_tee_close (struct focaltech_qsee_tee *tee)
{
  if (tee->fd < 0)
    return;

  if (tee->session)
    {
      struct tee_ioctl_close_session_arg arg = { .session = tee->session };

      ioctl (tee->fd, TEE_IOC_CLOSE_SESSION, &arg);
    }

  close (tee->fd);
  tee->fd = -1;
  tee->session = 0;
}

int
focaltech_qsee_tee_invoke (struct focaltech_qsee_tee *tee,
                           uint32_t                   command,
                           void                      *payload,
                           size_t                     payload_size,
                           int32_t                   *result)
{
  struct
  {
    struct tee_ioctl_invoke_arg arg;
    struct tee_ioctl_param      params[2];
  } request = { 0 };
  struct tee_ioctl_buf_data data = {
    .buf_ptr = (uintptr_t) &request,
    .buf_len = sizeof (request),
  };
  struct shm req = { 0 }, rsp = { 0 };
  uint32_t answered;
  int ret = -1;

  if (tee->fd < 0 || FOCALTECH_QSEE_HEADER_SIZE + payload_size > REQUEST_CAPACITY)
    return -1;

  if (shm_alloc (tee->fd, REQUEST_CAPACITY, &req) ||
      shm_alloc (tee->fd, RESPONSE_CAPACITY, &rsp))
    goto out;

  memcpy ((char *) req.va + 0, &command, sizeof (command));
  memcpy ((char *) req.va + 4, &payload_size, sizeof (uint32_t));
  if (payload_size)
    memcpy ((char *) req.va + FOCALTECH_QSEE_HEADER_SIZE, payload, payload_size);

  request.arg.num_params = 2;

  /*
   * Inout, not input: the application answers in this buffer, and the driver
   * only copies it back when asked this way.
   */
  request.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
  request.params[0].b = req.size;
  request.params[0].c = req.id;

  request.params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
  request.params[1].b = rsp.size;
  request.params[1].c = rsp.id;

  if (ioctl (tee->fd, TEE_IOC_INVOKE, &data) || request.arg.ret)
    goto out;

  /* Bit 31 set on the command is the application saying it handled this. */
  memcpy (&answered, req.va, sizeof (answered));
  if (!(answered & FOCALTECH_QSEE_ANSWERED))
    goto out;

  if (result)
    memcpy (result, (char *) req.va + 8, sizeof (*result));

  if (payload_size)
    memcpy (payload, (char *) req.va + FOCALTECH_QSEE_HEADER_SIZE, payload_size);

  ret = 0;

out:
  if (req.va)
    munmap (req.va, req.size);
  if (rsp.va)
    munmap (rsp.va, rsp.size);

  return ret;
}
