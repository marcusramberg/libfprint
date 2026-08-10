/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "focaltech-qsee-proto.h"

#include <string.h>

static void
put_u32 (void *base, size_t offset, uint32_t value)
{
  memcpy ((char *) base + offset, &value, sizeof (value));
}

static uint32_t
get_u32 (const void *base, size_t offset)
{
  uint32_t value;

  memcpy (&value, (const char *) base + offset, sizeof (value));
  return value;
}

size_t
focaltech_qsee_payload_size (uint32_t command)
{
  static const struct
  {
    uint32_t command;
    size_t   size;
  } sizes[] = {
    { FOCALTECH_QSEE_CMD_INIT, 0 },
    { FOCALTECH_QSEE_CMD_INIT_SPI, 0 },
    { FOCALTECH_QSEE_CMD_SET_SPI_SPEED, 4 },
    { FOCALTECH_QSEE_CMD_PROBE_DEVICE, 1 },
    { FOCALTECH_QSEE_CMD_INIT_DEVICE, 0 },
    { FOCALTECH_QSEE_CMD_SYNC_STATISTICS, 0x230 },
    { FOCALTECH_QSEE_CMD_START_SCANNING, 0x30 },
    { FOCALTECH_QSEE_CMD_CAPTURE_IMAGE, 0x24 },
    { FOCALTECH_QSEE_CMD_SAVE_DATA, 4 },
    { FOCALTECH_QSEE_CMD_REPORT_EVENT, 0x2e0 },
    { FOCALTECH_QSEE_CMD_WORK_MODE, 4 },
    { FOCALTECH_QSEE_CMD_PRE_ENROLL, 0 },
    { FOCALTECH_QSEE_CMD_ENROLL, 0x4a },
    { FOCALTECH_QSEE_CMD_POST_ENROLL, 0 },
    { FOCALTECH_QSEE_CMD_GET_AUTH_ID, 0 },
    { FOCALTECH_QSEE_CMD_CANCEL, 0 },
    { FOCALTECH_QSEE_CMD_ENUMERATE, 4 },
    { FOCALTECH_QSEE_CMD_REMOVE, 8 },
    { FOCALTECH_QSEE_CMD_AUTHENTICATE, 0x0e },
  };

  for (size_t i = 0; i < sizeof (sizes) / sizeof (sizes[0]); i++)
    if (sizes[i].command == command)
      return sizes[i].size;

  /* SYNC_CONFIG carries a JSON document and SET_ACTIVE_GROUP a path, so both
   * are sized by their caller rather than from this table. */
  return 0;
}

void
focaltech_qsee_build_event (void *payload, size_t size, uint32_t event)
{
  if (size < 0x2e0)
    return;

  memset (payload, 0, size);
  put_u32 (payload, 0x04, event);

  /*
   * A flag word the vendor's event thread always fills. Its meaning is not
   * decoded; it is set because the vendor sets it.
   */
  put_u32 (payload, 0x2d8, 4);
}

void
focaltech_qsee_build_capture (void *payload, size_t size, int hw_reset)
{
  if (size < 0x24)
    return;

  memset (payload, 0, size);

  put_u32 (payload, 0x00, hw_reset ? 1 : 0);  /* reset before scanning */
  put_u32 (payload, 0x0c, 1);                 /* frames to acquire */
  put_u32 (payload, 0x10, 1);
  put_u32 (payload, 0x1c, 0xc0040002);        /* a fixed constant in .rodata */
}

size_t
focaltech_qsee_build_set_active_group (void *payload, size_t size,
                                       uint32_t group, const char *path)
{
  size_t length = strlen (path);

  if (size < 4 + length + 1)
    return 0;

  memset (payload, 0, size);
  put_u32 (payload, 0, group);
  memcpy ((char *) payload + 4, path, length + 1);

  return 4 + length + 1;
}

void
focaltech_qsee_build_save (void *payload, size_t size, uint32_t what)
{
  if (size < 4)
    return;

  memset (payload, 0, size);
  put_u32 (payload, 0, what);
}

void
focaltech_qsee_build_remove (void *payload, size_t size, uint32_t finger)
{
  if (size < 8)
    return;

  memset (payload, 0, size);
  put_u32 (payload, 0, finger);
}

size_t
focaltech_qsee_parse_enumerate (const void *payload, size_t size,
                                uint32_t *fingers, size_t max)
{
  uint32_t count;
  size_t decoded = 0;

  if (size < 8)
    return 0;

  count = get_u32 (payload, 0);
  if (count > max)
    count = max;

  for (uint32_t i = 0; i < count; i++)
    {
      size_t offset = 8 + i * sizeof (uint32_t);

      if (offset + sizeof (uint32_t) > size)
        break;

      fingers[decoded++] = get_u32 (payload, offset);
    }

  return decoded;
}
