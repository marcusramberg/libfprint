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

/*
 * The application writes its own log into this region as it runs, and keeps
 * writing for as long as a command does. It has to be generous: when a command
 * never returns, what it managed to write before it died is the only view the
 * normal world has into what happened.
 */
#define RESPONSE_CAPACITY 0x40000

static int
shm_alloc (int fd, size_t size, struct focaltech_qsee_shm *out)
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
  struct focaltech_qsee_shm name = { 0 };
  size_t length = strlen (ta_name);

  memset (tee, 0, sizeof (*tee));
  tee->fd = -1;

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

  /*
   * Allocated once, here, for the reason described where the members are
   * declared: the application remembers this memory between commands.
   */
  if (shm_alloc (tee->fd, REQUEST_CAPACITY, &tee->request) ||
      shm_alloc (tee->fd, RESPONSE_CAPACITY, &tee->response))
    {
      focaltech_qsee_tee_close (tee);
      return -1;
    }

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

  if (tee->request.va)
    munmap (tee->request.va, tee->request.size);
  if (tee->response.va)
    munmap (tee->response.va, tee->response.size);

  if (tee->session)
    {
      struct tee_ioctl_close_session_arg arg = { .session = tee->session };

      ioctl (tee->fd, TEE_IOC_CLOSE_SESSION, &arg);
    }

  close (tee->fd);
  memset (tee, 0, sizeof (*tee));
  tee->fd = -1;
}

const char *
focaltech_qsee_tee_log (const struct focaltech_qsee_tee *tee, size_t *size)
{
  if (!tee->response.va)
    {
      *size = 0;
      return NULL;
    }

  *size = tee->response.size;

  return tee->response.va;
}

int
focaltech_qsee_tee_invoke (struct focaltech_qsee_tee *tee,
                           uint32_t                   command,
                           void                      *payload,
                           size_t                     payload_size,
                           int32_t                   *result)
{
  return focaltech_qsee_tee_invoke_full (tee, command, payload, payload_size,
                                         payload_size, result);
}

int
focaltech_qsee_tee_invoke_full (struct focaltech_qsee_tee *tee,
                                uint32_t                   command,
                                void                      *payload,
                                size_t                     send_size,
                                size_t                     answer_size,
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
  uint32_t answered;

  size_t staged = send_size > answer_size ? send_size : answer_size;

  if (tee->fd < 0 || !tee->request.va ||
      FOCALTECH_QSEE_HEADER_SIZE + staged > tee->request.size)
    return -1;

  /*
   * Only what this command owns is cleared. Whatever an earlier command left
   * further into the region stays, which is what the vendor's client does --
   * and what the application expects, since it reads back what it stored.
   */
  memset (tee->request.va, 0, FOCALTECH_QSEE_HEADER_SIZE + staged);
  memset (tee->response.va, 0, tee->response.size);

  memcpy ((char *) tee->request.va + 0, &command, sizeof (command));
  memcpy ((char *) tee->request.va + 4, &send_size, sizeof (uint32_t));
  if (send_size)
    memcpy ((char *) tee->request.va + FOCALTECH_QSEE_HEADER_SIZE, payload,
            send_size);

  request.arg.session = tee->session;
  request.arg.num_params = 2;

  /*
   * Inout, not input: the application answers in this buffer, and the driver
   * only copies it back when asked this way.
   *
   * The length is the region's capacity rather than the message's own: the
   * application reads it as the buffer it may work in, and refuses a payload
   * longer than capacity minus the header.
   */
  request.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
  request.params[0].b = tee->request.size;
  request.params[0].c = tee->request.id;

  request.params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
  request.params[1].b = tee->response.size;
  request.params[1].c = tee->response.id;

  if (ioctl (tee->fd, TEE_IOC_INVOKE, &data) || request.arg.ret)
    return -1;

  /* Bit 31 set on the command is the application saying it handled this. */
  memcpy (&answered, tee->request.va, sizeof (answered));
  if (!(answered & FOCALTECH_QSEE_ANSWERED))
    return -1;

  if (result)
    memcpy (result, (char *) tee->request.va + 8, sizeof (*result));

  if (answer_size)
    memcpy (payload, (char *) tee->request.va + FOCALTECH_QSEE_HEADER_SIZE,
            answer_size);

  return 0;
}
