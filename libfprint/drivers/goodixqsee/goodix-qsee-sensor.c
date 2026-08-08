/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "goodix-qsee-sensor.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define GF_IOC_MAGIC 'g'
#define GF_IOC_RESET _IO (GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ _IO (GF_IOC_MAGIC, 3)
#define GF_IOC_ENABLE_SPI_CLK _IOW (GF_IOC_MAGIC, 5, uint32_t)
#define GF_IOC_ENABLE_POWER _IO (GF_IOC_MAGIC, 7)
#define GF_NETLINK_PROTOCOL 25

static int
event_open (void)
{
  struct sockaddr_nl address = { .nl_family = AF_NETLINK, .nl_pid = getpid () };
  struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
  struct { struct nlmsghdr header; char body[16]; } message = {0};
  int fd = socket (AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, GF_NETLINK_PROTOCOL);

  if (fd < 0 || bind (fd, (struct sockaddr *) &address, sizeof (address)))
    goto fail;
  message.header.nlmsg_len = NLMSG_LENGTH (sizeof (message.body));
  message.header.nlmsg_pid = getpid ();
  memcpy (message.body, "hello", 6);
  if (sendto (fd, &message, message.header.nlmsg_len, 0,
              (struct sockaddr *) &kernel, sizeof (kernel)) < 0)
    goto fail;
  return fd;
fail:
  if (fd >= 0) close (fd);
  return -1;
}

int
goodix_qsee_sensor_open (struct goodix_qsee_sensor *sensor, const char *device_path)
{
  uint32_t clock = 0;

  if (!sensor || !device_path)
    { errno = EINVAL; return -1; }
  sensor->device_fd = -1;
  sensor->event_fd = -1;
  sensor->device_fd = open (device_path, O_RDWR | O_CLOEXEC);
  if (sensor->device_fd < 0 ||
      ioctl (sensor->device_fd, GF_IOC_ENABLE_POWER) ||
      ioctl (sensor->device_fd, GF_IOC_RESET))
    goto fail;
  usleep (20000);
  if (ioctl (sensor->device_fd, GF_IOC_ENABLE_SPI_CLK, &clock))
    goto fail;
  usleep (500000);
  sensor->event_fd = event_open ();
  if (sensor->event_fd < 0)
    goto fail;
  return 0;
fail:
  goodix_qsee_sensor_close (sensor);
  return -1;
}

int
goodix_qsee_sensor_enable_irq (struct goodix_qsee_sensor *sensor)
{
  if (!sensor || sensor->device_fd < 0)
    { errno = EINVAL; return -1; }
  return ioctl (sensor->device_fd, GF_IOC_ENABLE_IRQ);
}

int
goodix_qsee_sensor_wait_irq (struct goodix_qsee_sensor *sensor, int timeout_ms)
{
  struct pollfd descriptor;
  char buffer[512];
  int result;

  if (!sensor || sensor->event_fd < 0)
    { errno = EINVAL; return -1; }
  descriptor.fd = sensor->event_fd;
  descriptor.events = POLLIN;
  do result = poll (&descriptor, 1, timeout_ms); while (result < 0 && errno == EINTR);
  if (result <= 0)
    return result;
  if (!(descriptor.revents & POLLIN))
    { errno = EIO; return -1; }
  if (recv (sensor->event_fd, buffer, sizeof (buffer), 0) < 0)
    return -1;
  return 1;
}

void
goodix_qsee_sensor_close (struct goodix_qsee_sensor *sensor)
{
  if (!sensor) return;
  if (sensor->event_fd >= 0) close (sensor->event_fd);
  if (sensor->device_fd >= 0) close (sensor->device_fd);
  sensor->event_fd = sensor->device_fd = -1;
}
