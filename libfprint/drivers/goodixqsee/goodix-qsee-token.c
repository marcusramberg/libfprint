/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gio/gio.h>
#include <string.h>

#include "goodix-qsee-token.h"

gboolean
goodix_qsee_challenge_token (guint64 challenge,
                             guint8 hat[GOODIX_QSEE_HAT_SIZE],
                             GError **error,
                             gpointer user_data)
{
  (void) error;
  (void) user_data;
  memset (hat, 0, GOODIX_QSEE_HAT_SIZE);
  memcpy (hat + 1, &challenge, sizeof (challenge));
  return TRUE;
}
