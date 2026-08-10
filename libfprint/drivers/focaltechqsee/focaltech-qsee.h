/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fpi-device.h"
#include "focaltech-qsee-sensor.h"
#include "focaltech-qsee-transport.h"

G_DECLARE_FINAL_TYPE (FpiDeviceFocaltechQsee, fpi_device_focaltechqsee,
                      FPI, DEVICE_FOCALTECHQSEE, FpDevice)

struct _FpiDeviceFocaltechQsee
{
  FpDevice parent;

  struct focaltech_qsee_sensor sensor;
  struct focaltech_qsee_tee    tee;

  gchar *ta_name;
  guint  profile;
};
