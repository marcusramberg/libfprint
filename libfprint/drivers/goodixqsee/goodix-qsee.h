/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "fpi-device.h"
#include "goodix-qsee-sensor.h"
#include "goodix-qsee-transport.h"
#include "goodix-qsee-token.h"

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixQsee, fpi_device_goodixqsee,
                      FPI, DEVICE_GOODIXQSEE, FpDevice)

struct _FpiDeviceGoodixQsee
{
  FpDevice parent;
  struct goodix_qsee_sensor sensor;
  struct goodix_qsee_qsee qsee;
  gchar *ta_name;
  guint profile;
  GoodixQseeTokenProvider token_provider;
  gpointer token_provider_data;
};
