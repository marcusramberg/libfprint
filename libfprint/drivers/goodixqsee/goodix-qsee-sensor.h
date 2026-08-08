/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

struct goodix_qsee_sensor {
  int device_fd;
  int event_fd;
};

int goodix_qsee_sensor_open (struct goodix_qsee_sensor *sensor, const char *device_path);
int goodix_qsee_sensor_enable_irq (struct goodix_qsee_sensor *sensor);
int goodix_qsee_sensor_wait_irq (struct goodix_qsee_sensor *sensor, int timeout_ms);
void goodix_qsee_sensor_close (struct goodix_qsee_sensor *sensor);
