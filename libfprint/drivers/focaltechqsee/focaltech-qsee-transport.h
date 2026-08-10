/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct focaltech_qsee_tee
{
  int      fd;
  uint32_t session;
};

/*
 * Open a client session on the trusted application, which must already be
 * loaded: loading is privileged and belongs to a separate service, while this
 * only needs /dev/teeN.
 */
int focaltech_qsee_tee_open (struct focaltech_qsee_tee *tee, const char *ta_name);
void focaltech_qsee_tee_close (struct focaltech_qsee_tee *tee);

/*
 * Send one command. `payload` is copied into the request, and the application
 * answers in place -- so on return it holds whatever the application wrote
 * back, which for ENUMERATE and GET_AUTH_ID is the answer itself.
 *
 * Returns 0 when the application handled the message, and writes its own
 * result to `result` when it reported one.
 */
int focaltech_qsee_tee_invoke (struct focaltech_qsee_tee *tee,
                               uint32_t                   command,
                               void                      *payload,
                               size_t                     payload_size,
                               int32_t                   *result);
