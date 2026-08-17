/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The FocalTech trusted application's message protocol.
 *
 * Every message is a 16-byte header followed by a payload of a length the
 * application fixes per command:
 *
 *	+0x00  u32  command
 *	+0x04  u32  payload length
 *	+0x08  8 bytes the client leaves zero
 *	+0x10  payload
 *
 * The application answers in the same buffer: it writes its result at +0x08 of
 * the header and sets bit 31 of the command to say it did. Its log arrives in
 * a second, output-only buffer.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FOCALTECH_QSEE_HEADER_SIZE      0x10
#define FOCALTECH_QSEE_ANSWERED         0x80000000u

/* Bringing the sensor up. */
#define FOCALTECH_QSEE_CMD_INIT         0x1004
#define FOCALTECH_QSEE_CMD_INIT_SPI     0x1006

/*
 * FF_CMD_TA_FREE_SPI, as the application names it in its own log. The bus it
 * opened stays open for as long as the application is loaded -- which outlives
 * any one client -- and asking it to open a bus it already has returns -5. So a
 * client that opened the bus has to give it back, or it is the last one that
 * ever gets it.
 */
#define FOCALTECH_QSEE_CMD_FREE_SPI     0x1007
#define FOCALTECH_QSEE_CMD_SET_SPI_SPEED 0x1008
#define FOCALTECH_QSEE_CMD_PROBE_DEVICE 0x100a
#define FOCALTECH_QSEE_CMD_INIT_DEVICE  0x100b
#define FOCALTECH_QSEE_CMD_SYNC_CONFIG  0x100d

/*
 * Not optional, whatever the name suggests. This is the only caller of the
 * application's statistics setup, and its enrolment path writes a timing into
 * that object unconditionally -- so without this the write goes through a null
 * pointer and the application takes a data abort, which the kernel reports
 * only as an SCM call failing.
 */
#define FOCALTECH_QSEE_CMD_SYNC_STATISTICS 0x100e

#define FOCALTECH_QSEE_CMD_START_SCANNING 0x1012
#define FOCALTECH_QSEE_CMD_CAPTURE_IMAGE  0x1013
#define FOCALTECH_QSEE_CMD_SAVE_DATA      0x1014
#define FOCALTECH_QSEE_CMD_REPORT_EVENT   0x1017
#define FOCALTECH_QSEE_CMD_WORK_MODE      0x101f

/* The framework-facing family. */
#define FOCALTECH_QSEE_CMD_PRE_ENROLL     0x2000
#define FOCALTECH_QSEE_CMD_ENROLL         0x2001
#define FOCALTECH_QSEE_CMD_POST_ENROLL    0x2002
#define FOCALTECH_QSEE_CMD_GET_AUTH_ID    0x2003
#define FOCALTECH_QSEE_CMD_CANCEL         0x2004
#define FOCALTECH_QSEE_CMD_ENUMERATE      0x2005
#define FOCALTECH_QSEE_CMD_REMOVE         0x2006
#define FOCALTECH_QSEE_CMD_SET_ACTIVE_GROUP 0x2007
#define FOCALTECH_QSEE_CMD_AUTHENTICATE   0x2008

/* Payload lengths, as the vendor client's own table gives them. */
size_t focaltech_qsee_payload_size (uint32_t command);

/*
 * Chip modes for WORK_MODE. Detection has to be armed explicitly: after
 * START_SCANNING the sensor images but does not detect, and the interrupt
 * never moves until FDT_DOWN_DETECT is set.
 */
#define FOCALTECH_QSEE_MODE_SLEEP       0
#define FOCALTECH_QSEE_MODE_DOWN_DETECT 1
#define FOCALTECH_QSEE_MODE_UP_DETECT   2

/*
 * REPORT_EVENT tells the application what happened; it is not the application
 * telling us. The id sits at payload +4, and the work -- matching, or taking
 * an enrolment sample -- happens inside this call, not in the capture.
 */
#define FOCALTECH_QSEE_EVENT_FINGER_DOWN 5
#define FOCALTECH_QSEE_EVENT_FINGER_UP   6
#define FOCALTECH_QSEE_EVENT_IMAGE_READY 7

void focaltech_qsee_build_event (void *payload, size_t size, uint32_t event);

/*
 * What the application answers a REPORT_EVENT with, written back over the
 * request payload. The vendor HAL reads the same four words and prints them as
 * `enrolled fid = %d, gid = %d, rem = %d`, which is how the layout was found.
 *
 * `remaining` is the application's own view of how much of the finger it still
 * needs, and it is not a touch counter: it falls only for a touch that adds
 * coverage the template does not already have. A touch that lands where one
 * already did is accepted and changes nothing.
 */
struct focaltech_qsee_event_result
{
  uint32_t status;              /* +0x00, 1 when the application acted */
  uint32_t finger;              /* +0x04, the id being enrolled or matched */
  uint32_t group;               /* +0x08 */
  uint32_t remaining;           /* +0x0c */
};

#define FOCALTECH_QSEE_EVENT_STATUS_OK 1

void focaltech_qsee_parse_event (const void                         *payload,
                                 size_t                              size,
                                 struct focaltech_qsee_event_result *out);

/*
 * CAPTURE_IMAGE's descriptor, as the vendor HAL fills it. `hw_reset` resets
 * the chip and recalibrates before scanning, which is what the vendor does on
 * an interrupt but takes long enough that a finger placed at a prompt is gone
 * before the sensor samples.
 */
void focaltech_qsee_build_capture (void *payload, size_t size, int hw_reset);

/*
 * SET_ACTIVE_GROUP: a group id, then the store path the application names its
 * objects under. Templates are filed per group, and nothing can be enumerated
 * or enrolled until a group is active.
 */
size_t focaltech_qsee_build_set_active_group (void *payload, size_t size,
                                              uint32_t group, const char *path);

/*
 * SAVE_DATA's payload is a mask of what to write, not a flag. Sent as zero it
 * saves only the chip's calibration and silently skips the templates, which
 * looks exactly like a successful save until the next boot has nothing to
 * load.
 */
#define FOCALTECH_QSEE_SAVE_ALL 0x60000000u

void focaltech_qsee_build_save (void *payload, size_t size, uint32_t what);

/* REMOVE takes the finger id in the first word. */
/*
 * REMOVE names the finger the way the rest of the storage side does: the group
 * first, then the finger within it. With the finger alone in the first word the
 * application answers -200 and deletes nothing.
 */
void focaltech_qsee_build_remove (void *payload, size_t size, uint32_t group,
                                  uint32_t finger);

/*
 * ENUMERATE answers in the request payload: a count, then the finger ids.
 * Returns the number decoded, up to `max`.
 */
size_t focaltech_qsee_parse_enumerate (const void *payload, size_t size,
                                       uint32_t *fingers, size_t max);
