/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

struct focaltech_qsee_sensor
{
  int fd;
};

int focaltech_qsee_sensor_open (struct focaltech_qsee_sensor *sensor,
                                const char                   *path);
void focaltech_qsee_sensor_close (struct focaltech_qsee_sensor *sensor);

/*
 * Arm the interrupt. The kernel driver requests it with IRQF_NO_AUTOEN, so the
 * line stays masked until this runs -- and a masked line looks exactly like a
 * sensor that never sees a finger. Arming also clears the edge latched during
 * power-on, which would otherwise arrive immediately as a phantom event.
 */
int focaltech_qsee_sensor_arm (struct focaltech_qsee_sensor *sensor);
int focaltech_qsee_sensor_disarm (struct focaltech_qsee_sensor *sensor);

/*
 * Wait for the sensor to report a finger. Returns 1 on an event, 0 on timeout
 * and -1 on error. A negative timeout waits indefinitely.
 */
int focaltech_qsee_sensor_wait (struct focaltech_qsee_sensor *sensor,
                                int                           timeout_ms);

/* Drop any event already queued, so a wait cannot return a stale one. */
void focaltech_qsee_sensor_drain (struct focaltech_qsee_sensor *sensor);

int focaltech_qsee_sensor_reset (struct focaltech_qsee_sensor *sensor);
