/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <gio/gio.h>
#include "goodix-qsee-proto.h"

typedef gboolean (*GoodixQseeTokenProvider) (guint64 challenge,
                                             guint8 hat[GOODIX_QSEE_HAT_SIZE],
                                             GError **error,
                                             gpointer user_data);

gboolean goodix_qsee_challenge_token (guint64 challenge,
                                      guint8 hat[GOODIX_QSEE_HAT_SIZE],
                                      GError **error,
                                      gpointer user_data);
