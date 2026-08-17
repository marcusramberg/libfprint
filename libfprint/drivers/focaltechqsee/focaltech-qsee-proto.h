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
 * request payload.
 *
 * This is *not* the `enrolled fid = %d, gid = %d, rem = %d` triple the vendor
 * HAL logs, whatever the reversed notes say: that layout was tried against a
 * live application and the finger id read back as the event id the request
 * carried, while the word where `rem` should sit was never written at all.
 * What actually moves is a pair -- an outcome, and what the application was
 * armed for when it happened, which is what the outcome has to be read
 * against:
 *
 *	armed = 1 (enrolling)      outcome 1, invoke result 0    sample taken
 *	armed = 2 (authenticating) outcome 0, invoke result 0    matched
 *	armed = 2 (authenticating) outcome 2, invoke result -11  no match
 *
 * So -11 out of an event is not a failure to report as one: on the matching
 * path it is the application saying the finger is not one it knows.
 *
 * An enrolment answer carries, at +0x24, how much of the finger the
 * application still wants -- the `rem` of its own `enrolled fid = %u, gid =
 * %u, rem = %u` log line. It is not a count of touches: it falls only for a
 * touch that covers part of the finger the template does not have yet, and in
 * practice about every second touch moves it. An enrolment that stops before
 * it reaches zero saves a template the application never finished, and an
 * unfinished template matches nothing -- not even the finger that built it.
 *
 * A match also names the finger, at +0x10, with the same id ENUMERATE lists.
 * The word is written only when there is a match to report -- a refused finger
 * leaves it at the zero the request carried -- so it means nothing unless the
 * outcome says the finger was recognised. Further in, from +0x38, sits what
 * the vendor calls the hw_auth_token; nothing here reads it, and nothing here
 * should log it.
 */
struct focaltech_qsee_event_result
{
  uint32_t outcome;             /* +0x00, read against `armed` */
  uint32_t event;               /* +0x04, the event id sent, echoed back */
  uint32_t armed;               /* +0x08 */
  uint32_t reserved;            /* +0x0c, never seen written */
  uint32_t finger;              /* +0x10, matching only, and only on a match */
  uint32_t remaining;           /* +0x24, enrolling only */
};

/* An enrolment touch the application took a sample from. */
#define FOCALTECH_QSEE_ENROLL_SAMPLED 1

/*
 * How a touch came out, when the application was armed for matching. A finger
 * it does not know is answered rather than refused -- the invocation carries
 * -11 with it, which is the application's way of saying so and not an error to
 * report as one.
 */
#define FOCALTECH_QSEE_IDENTIFY_MATCHED 0
#define FOCALTECH_QSEE_IDENTIFY_UNKNOWN 2

/* What `armed` says the application was doing when the event arrived. */
#define FOCALTECH_QSEE_ARMED_ENROLL   1
#define FOCALTECH_QSEE_ARMED_IDENTIFY 2

void focaltech_qsee_parse_event (const void                         *payload,
                                 size_t                              size,
                                 struct focaltech_qsee_event_result *out);

/*
 * CAPTURE_IMAGE's descriptor, as the vendor HAL fills it. `hw_reset` resets
 * the chip and recalibrates before scanning, which is what the vendor does on
 * an interrupt but takes long enough that a finger placed at a prompt is gone
 * before the sensor samples.
 */
/*
 * ENROLL's payload. The application's log names what it is enrolling into --
 * `FtEnrollByTemplate...finger_id = %d` -- and with the payload left zero that
 * is slot 0 on every enrolment, so a second finger's samples are merged into
 * the first finger's template and neither matches afterwards.
 */
void focaltech_qsee_build_enroll (void *payload, size_t size, uint32_t slot);

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
