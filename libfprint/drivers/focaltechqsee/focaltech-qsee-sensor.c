/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "focaltech-qsee-sensor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * The kernel driver's interface, from drivers/misc/focaltech-fp.c. Power and
 * reset are done at probe, so nothing here has to sequence them; what is left
 * is arming the interrupt and reading events off it.
 */
#define FF_IOC_RESET_DEVICE   _IO ('f', 0x02)
#define FF_IOC_ENABLE_IRQ     _IO ('f', 0x05)
#define FF_IOC_DISABLE_IRQ    _IO ('f', 0x06)

int
focaltech_qsee_sensor_open (struct focaltech_qsee_sensor *sensor,
                            const char                   *path)
{
  sensor->fd = open (path, O_RDWR | O_CLOEXEC);

  return sensor->fd < 0 ? -1 : 0;
}

void
focaltech_qsee_sensor_close (struct focaltech_qsee_sensor *sensor)
{
  if (sensor->fd < 0)
    return;

  ioctl (sensor->fd, FF_IOC_DISABLE_IRQ);
  close (sensor->fd);
  sensor->fd = -1;
}

int
focaltech_qsee_sensor_arm (struct focaltech_qsee_sensor *sensor)
{
  return ioctl (sensor->fd, FF_IOC_ENABLE_IRQ) ? -1 : 0;
}

int
focaltech_qsee_sensor_disarm (struct focaltech_qsee_sensor *sensor)
{
  return ioctl (sensor->fd, FF_IOC_DISABLE_IRQ) ? -1 : 0;
}

int
focaltech_qsee_sensor_reset (struct focaltech_qsee_sensor *sensor)
{
  return ioctl (sensor->fd, FF_IOC_RESET_DEVICE) ? -1 : 0;
}

void
focaltech_qsee_sensor_drain (struct focaltech_qsee_sensor *sensor)
{
  for (;;)
    {
      struct pollfd descriptor = { .fd = sensor->fd, .events = POLLIN };
      uint32_t sequence;
      int ready;

      do
        ready = poll (&descriptor, 1, 0);
      while (ready < 0 && errno == EINTR);

      if (ready <= 0)
        return;

      if (read (sensor->fd, &sequence, sizeof (sequence)) != sizeof (sequence))
        return;
    }
}

int
focaltech_qsee_sensor_wait (struct focaltech_qsee_sensor *sensor,
                            int                           timeout_ms)
{
  struct pollfd descriptor = { .fd = sensor->fd, .events = POLLIN };
  uint32_t sequence;
  int ready;

  do
    ready = poll (&descriptor, 1, timeout_ms);
  while (ready < 0 && errno == EINTR);

  if (ready < 0)
    return -1;
  if (ready == 0)
    return 0;

  /* The event is a sequence number; reading it is what consumes the event. */
  if (read (sensor->fd, &sequence, sizeof (sequence)) != sizeof (sequence))
    return -1;

  return 1;
}
