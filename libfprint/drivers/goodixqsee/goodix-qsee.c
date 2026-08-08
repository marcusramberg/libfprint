/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define FP_COMPONENT "goodixqsee"

#include "drivers_api.h"
#include "goodix-qsee.h"
#include "goodix-qsee-proto.h"

#include <errno.h>
#include <string.h>

#define CMD_DETECT_SENSOR 1000u
#define CMD_INIT 1001u
#define CMD_INIT_FINISHED 1005u
#define CMD_PRE_ENROLL 1006u
#define CMD_ENROLL 1007u
#define CMD_CANCEL 1009u
#define CMD_AUTHENTICATE 1010u
#define CMD_GET_AUTH_ID 1011u
#define CMD_SAVE 1012u
#define CMD_REMOVE 1013u
#define CMD_SET_ACTIVE_GROUP 1014u
#define CMD_ENUMERATE 1015u
#define CMD_GET_DEV_INFO 1089u
#define CMD_AUTHENTICATE_FINISH 1086u

#define IRQ_FINGER_DOWN (1u << 1)
#define IRQ_FINGER_UP (1u << 2)
#define IRQ_IMAGE (1u << 7)
#define IRQ_FRAME_DONE (1u << 10)
#define IRQ_MAX_DRAIN 12
#define ENROLL_STAGES_PROFILE_V1 25

enum worker_operation { WORK_OPEN, WORK_CLOSE, WORK_LIST, WORK_DELETE,
                        WORK_ENROLL, WORK_IDENTIFY, WORK_CLEAR };

struct worker_data {
  enum worker_operation operation;
  guint32 group_id;
  guint32 finger_id;
};

struct list_result {
  size_t count;
  bool count_mismatch;
  struct goodix_qsee_finger fingers[GOODIX_QSEE_ENUMERATE_MAX];
};

struct interactive_result {
  guint32 group_id;
  guint32 finger_id;
  guint32 status;
};

struct progress_event {
  FpiDeviceGoodixQsee *self;
  FpFingerStatusFlags finger_status;
  gint completed;
  GError *retry;
};

G_DEFINE_TYPE (FpiDeviceGoodixQsee, fpi_device_goodixqsee, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_MISC,
    .misc_name = "goodix_fp", .misc_compatible = "goodix,gf3626",
    .driver_data = GOODIX_QSEE_PROFILE_V1 },
  { .udev_types = 0 }
};

static GError *
io_error (const char *operation)
{
  return g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                      "%s: %s", operation, g_strerror (errno));
}

static gboolean
invoke (FpiDeviceGoodixQsee *self, guint32 command, guint8 *payload,
        gsize size, guint32 *status, GError **error)
{
  guint32 result = 0;

  if (goodix_qsee_qsee_invoke (&self->qsee, command, payload, size, &result))
    {
      g_propagate_error (error, io_error ("QSEE invoke"));
      return FALSE;
    }
  if (status) *status = result;
  return TRUE;
}

static gboolean
invoke_success (FpiDeviceGoodixQsee *self, guint32 command, guint8 *payload,
                gsize size, GError **error)
{
  guint32 status;
  if (!invoke (self, command, payload, size, &status, error))
    return FALSE;
  if (status)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Goodix command %u returned status %u", command, status);
      return FALSE;
    }
  return TRUE;
}

static gchar *
read_ta_name (FpDevice *device, GError **error)
{
  const gchar *sysfs = fpi_device_get_udev_sysfs_path (device,
                                                       FPI_DEVICE_UDEV_SUBTYPE_MISC);
  g_autofree gchar *direct = NULL;
  g_autofree gchar *parent = NULL;
  g_autofree gchar *platform = NULL;
  gchar *contents = NULL;

  if (!sysfs)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "misc device has no sysfs path"); return NULL; }
  direct = g_build_filename (sysfs, "firmware_name", NULL);
  parent = g_build_filename (sysfs, "device", "firmware_name", NULL);
  platform = g_canonicalize_filename ("../firmware_name", sysfs);
  if (!g_file_get_contents (direct, &contents, NULL, NULL) &&
      !g_file_get_contents (parent, &contents, NULL, NULL) &&
      !g_file_get_contents (platform, &contents, NULL, error))
    return NULL;
  g_strstrip (contents);
  if (!*contents)
    { g_free (contents); g_set_error_literal (error, G_IO_ERROR,
                                              G_IO_ERROR_INVALID_DATA,
                                              "firmware_name is empty"); return NULL; }
  return contents;
}

static gboolean
bring_up (FpiDeviceGoodixQsee *self, GError **error)
{
  static const struct { guint32 command; gsize size; } sequence[] = {
    { CMD_DETECT_SENSOR, 520 }, { CMD_INIT, 348 },
    { CMD_INIT_FINISHED, 104 }, { CMD_GET_DEV_INFO, 5500 },
  };
  g_autofree guint8 *payload = g_malloc0 (5500);
  guint i;

  for (i = 0; i < G_N_ELEMENTS (sequence); i++)
    {
      memset (payload, 0, sequence[i].size);
      if (!invoke_success (self, sequence[i].command, payload,
                           sequence[i].size, error))
        return FALSE;
    }
  return TRUE;
}

static gboolean
set_group (FpiDeviceGoodixQsee *self, guint32 group, GError **error)
{
  guint8 payload[104] = {0};
  memcpy (payload + 100, &group, sizeof (group));
  return invoke_success (self, CMD_SET_ACTIVE_GROUP, payload, sizeof (payload), error);
}

static GError *
retry_for_status (guint32 status)
{
  switch (status)
    {
    case GOODIX_QSEE_TOO_FAST:
      return fpi_device_retry_new (FP_DEVICE_RETRY_TOO_FAST);
    case GOODIX_QSEE_TOO_SLOW:
      return fpi_device_retry_new (FP_DEVICE_RETRY_TOO_SHORT);
    case GOODIX_QSEE_DUPLICATE_AREA:
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                       "Fingerprint area already sampled");
    case GOODIX_QSEE_DUPLICATE_FINGER:
      return fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER,
                                       "Finger is already enrolled");
    case GOODIX_QSEE_ACQUIRED_PARTIAL:
    case GOODIX_QSEE_ACQUIRED_PARTIAL_1060:
      return fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL);
    case GOODIX_QSEE_IMAGER_DIRTY_1012:
    case GOODIX_QSEE_IMAGER_DIRTY_1052:
    case GOODIX_QSEE_IMAGER_DIRTY_1058:
    case GOODIX_QSEE_IMAGER_DIRTY_1101:
    case GOODIX_QSEE_IMAGER_DIRTY_1104:
      return fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER);
    default:
      return status ? fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                "Goodix sample status %u", status) : NULL;
    }
}

static gboolean
progress_in_main (gpointer user_data)
{
  struct progress_event *event = user_data;
  fpi_device_report_finger_status (FP_DEVICE (event->self), event->finger_status);
  if (event->completed >= 0 || event->retry)
    fpi_device_enroll_progress (FP_DEVICE (event->self), MAX (event->completed, 0),
                                NULL, g_steal_pointer (&event->retry));
  g_object_unref (event->self);
  g_free (event);
  return G_SOURCE_REMOVE;
}

static void
queue_progress (FpiDeviceGoodixQsee *self, FpFingerStatusFlags finger_status,
                gint completed, GError *retry)
{
  struct progress_event *event = g_new0 (struct progress_event, 1);
  event->self = g_object_ref (self);
  event->finger_status = finger_status;
  event->completed = completed;
  event->retry = retry;
  g_main_context_invoke (NULL, progress_in_main, event);
}

static void
cancel_armed (FpiDeviceGoodixQsee *self)
{
  guint8 payload[104] = {0};
  guint32 ignored;
  goodix_qsee_qsee_invoke (&self->qsee, CMD_CANCEL, payload, sizeof (payload), &ignored);
}

static gboolean
finish_authentication (FpiDeviceGoodixQsee *self, const guint8 *context,
                       GError **error)
{
  guint8 payload[124] = {0};
  guint32 value;
  memcpy (&value, context + 100, 4); value = GUINT32_TO_BE (value); memcpy (payload + 104, &value, 4);
  memcpy (&value, context + 104, 4); value = GUINT32_TO_BE (value); memcpy (payload + 108, &value, 4);
  memcpy (payload + 112, context + 0x4fce4, 4);
  memcpy (payload + 116, context + 0x4fce8, 4);
  return invoke_success (self, CMD_AUTHENTICATE_FINISH, payload, sizeof (payload), error);
}

static struct interactive_result *
capture_operation (FpiDeviceGoodixQsee *self, gboolean enrolling,
                   GCancellable *cancellable, GError **error)
{
  g_autofree guint8 *irq = g_malloc0 (GOODIX_QSEE_IRQ_SIZE);
  struct interactive_result *result = NULL;
  gint64 deadline = g_get_monotonic_time () + (enrolling ? 120 : 30) * G_TIME_SPAN_SECOND;
  gboolean armed = FALSE;
  guint32 status = 0;

  if (!set_group (self, GOODIX_QSEE_GROUP_DEFAULT, error)) return NULL;
  if (enrolling)
    {
      guint8 pre[112] = {0}, enroll[180], hat[GOODIX_QSEE_HAT_SIZE];
      guint64 challenge;
      if (!invoke_success (self, CMD_PRE_ENROLL, pre, sizeof (pre), error)) return NULL;
      memcpy (&challenge, pre + 104, sizeof (challenge));
      if (!challenge || !self->token_provider (challenge, hat, error, self->token_provider_data) ||
          !goodix_qsee_build_enroll (enroll, sizeof (enroll), GOODIX_QSEE_GROUP_DEFAULT, hat) ||
          !invoke_success (self, CMD_ENROLL, enroll, sizeof (enroll), error)) return NULL;
    }
  else
    {
      guint8 auth[112] = {0};
      memcpy (auth + 100, &(guint32){GOODIX_QSEE_GROUP_DEFAULT}, 4);
      if (!invoke_success (self, CMD_AUTHENTICATE, auth, sizeof (auth), error)) return NULL;
    }
  armed = TRUE;
  if (goodix_qsee_sensor_enable_irq (&self->sensor))
    { g_propagate_error (error, io_error ("enabling Goodix IRQ")); goto out; }
  goodix_qsee_prepare_irq (irq);
  while (g_get_monotonic_time () < deadline)
    {
      int wait, drain;
      if (g_cancellable_is_cancelled (cancellable))
        { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Operation cancelled"); goto out; }
      wait = goodix_qsee_sensor_wait_irq (&self->sensor, 250);
      if (wait < 0) { g_propagate_error (error, io_error ("waiting for Goodix IRQ")); goto out; }
      if (!wait) continue;
      for (drain = 0; drain < IRQ_MAX_DRAIN; drain++)
        {
          struct goodix_qsee_irq_result event;
          if (!invoke (self, 1016, irq, GOODIX_QSEE_IRQ_SIZE, &status, error)) goto out;
          goodix_qsee_parse_irq (irq, &event);
          event.status = status;
          if (!event.mask) break;
          if (event.mask & IRQ_FINGER_DOWN)
            queue_progress (self, FP_FINGER_STATUS_PRESENT, -1, NULL);
          if (event.mask & IRQ_FINGER_UP)
            queue_progress (self, FP_FINGER_STATUS_NONE, -1, NULL);
          if (!(event.mask & (IRQ_IMAGE | IRQ_FRAME_DONE))) continue;
          if (enrolling)
            {
              gint completed = CLAMP (ENROLL_STAGES_PROFILE_V1 -
                                      (gint) event.samples_remaining,
                                      0, ENROLL_STAGES_PROFILE_V1);
              queue_progress (self, FP_FINGER_STATUS_PRESENT, completed,
                              retry_for_status (event.status));
              if (!event.samples_remaining && event.finger_id)
                {
                  guint8 save[112];
                  goodix_qsee_build_finger_command (save, sizeof (save), event.group_id, event.finger_id);
                  if (!invoke_success (self, CMD_SAVE, save, sizeof (save), error)) goto out;
                  {
                    g_autoptr(GError) refresh_error = NULL;
                    guint8 auth_id[112] = {0};
                    if (!set_group (self, event.group_id, &refresh_error) ||
                        !invoke_success (self, CMD_GET_AUTH_ID, auth_id,
                                         sizeof (auth_id), &refresh_error))
                      fp_warn ("Enrollment saved but auth-id refresh failed: %s",
                               refresh_error->message);
                  }
                  result = g_new0 (struct interactive_result, 1);
                  result->group_id = event.group_id; result->finger_id = event.finger_id;
                  armed = FALSE;
                  goto out;
                }
            }
          else
            {
              result = g_new0 (struct interactive_result, 1);
              result->status = event.status;
              result->group_id = event.group_id; result->finger_id = event.finger_id;
              if (!event.status && !finish_authentication (self, irq, error))
                { g_clear_pointer (&result, g_free); goto out; }
              armed = FALSE;
              goto out;
            }
        }
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Fingerprint operation timed out");
out:
  if (armed) cancel_armed (self);
  queue_progress (self, FP_FINGER_STATUS_NONE, -1, NULL);
  return result;
}

static void
worker_thread (GTask *task, gpointer object, gpointer task_data,
               GCancellable *cancellable)
{
  FpiDeviceGoodixQsee *self = object;
  struct worker_data *work = task_data;
  g_autoptr(GError) error = NULL;

  if (g_task_return_error_if_cancelled (task)) return;
  switch (work->operation)
    {
    case WORK_OPEN:
      self->profile = fpi_device_get_driver_data (FP_DEVICE (self));
      if (!self->ta_name)
        self->ta_name = read_ta_name (FP_DEVICE (self), &error);
      if (!self->ta_name) break;
      if (goodix_qsee_sensor_open (&self->sensor,
                                   fpi_device_get_udev_data (FP_DEVICE (self),
                                                            FPI_DEVICE_UDEV_SUBTYPE_MISC)))
        { error = io_error ("opening Goodix sensor"); break; }
      if (goodix_qsee_qsee_open (&self->qsee, self->ta_name))
        { error = io_error ("opening Goodix TA"); break; }
      if (!bring_up (self, &error)) break;
      g_task_return_boolean (task, TRUE);
      return;
    case WORK_CLOSE:
      goodix_qsee_qsee_close (&self->qsee);
      goodix_qsee_sensor_close (&self->sensor);
      g_task_return_boolean (task, TRUE);
      return;
    case WORK_LIST:
      {
        g_autofree guint8 *payload = g_malloc0 (GOODIX_QSEE_ENUMERATE_SIZE);
        struct list_result *result = g_new0 (struct list_result, 1);
        if (!set_group (self, work->group_id, &error) ||
            !invoke_success (self, CMD_ENUMERATE, payload,
                             GOODIX_QSEE_ENUMERATE_SIZE, &error))
          { g_free (result); break; }
        result->count = goodix_qsee_parse_enumerate (payload, result->fingers,
                                                     &result->count_mismatch);
        g_task_return_pointer (task, result, g_free);
        return;
      }
    case WORK_DELETE:
      {
        guint8 payload[112];
        goodix_qsee_build_finger_command (payload, sizeof (payload),
                                          work->group_id, work->finger_id);
        if (!invoke_success (self, CMD_REMOVE, payload, sizeof (payload), &error))
          break;
        g_task_return_boolean (task, TRUE);
        return;
      }
    case WORK_ENROLL:
    case WORK_IDENTIFY:
      {
        struct interactive_result *result = capture_operation (self,
            work->operation == WORK_ENROLL, cancellable, &error);
        if (!result) break;
        g_task_return_pointer (task, result, g_free);
        return;
      }
    case WORK_CLEAR:
      {
        guint8 enumerate[GOODIX_QSEE_ENUMERATE_SIZE] = {0};
        struct goodix_qsee_finger fingers[GOODIX_QSEE_ENUMERATE_MAX];
        size_t count, i;
        if (!set_group (self, GOODIX_QSEE_GROUP_DEFAULT, &error) ||
            !invoke_success (self, CMD_ENUMERATE, enumerate, sizeof (enumerate), &error))
          break;
        count = goodix_qsee_parse_enumerate (enumerate, fingers, NULL);
        for (i = 0; i < count; i++)
          {
            guint8 remove[112];
            if (g_cancellable_is_cancelled (cancellable))
              { g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                     "Clear storage cancelled"); break; }
            goodix_qsee_build_finger_command (remove, sizeof (remove),
                                               fingers[i].group_id, fingers[i].finger_id);
            if (!invoke_success (self, CMD_REMOVE, remove, sizeof (remove), &error)) break;
          }
        if (error) break;
        g_task_return_boolean (task, TRUE);
        return;
      }
    }
  if (work->operation == WORK_OPEN)
    {
      goodix_qsee_qsee_close (&self->qsee);
      goodix_qsee_sensor_close (&self->sensor);
    }
  g_task_return_error (task, g_steal_pointer (&error));
}

static void
start_worker (FpiDeviceGoodixQsee *self, enum worker_operation operation,
              guint32 group, guint32 finger, GAsyncReadyCallback callback)
{
  g_autoptr(GTask) task = g_task_new (self,
      operation == WORK_OPEN || operation == WORK_CLOSE ? NULL :
      fpi_device_get_cancellable (FP_DEVICE (self)), callback, NULL);
  struct worker_data *work = g_new0 (struct worker_data, 1);
  work->operation = operation; work->group_id = group; work->finger_id = finger;
  g_task_set_task_data (task, work, g_free);
  g_task_run_in_thread (task, worker_thread);
}

static FpPrint *
make_print (FpiDeviceGoodixQsee *self, guint32 group, guint32 finger)
{
  FpPrint *print = fp_print_new (FP_DEVICE (self));
  GVariant *data = g_variant_new ("(uuu)", self->profile, group, finger);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
  return print;
}

static gboolean
parse_print (FpPrint *print, guint32 *profile, guint32 *group, guint32 *finger)
{
  g_autoptr(GVariant) data = NULL;
  g_object_get (print, "fpi-data", &data, NULL);
  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(uuu)"))) return FALSE;
  g_variant_get (data, "(uuu)", profile, group, finger);
  return TRUE;
}

static void open_done (GObject *o, GAsyncResult *r, gpointer unused)
{ g_autoptr(GError) e = NULL; (void) unused; g_task_propagate_boolean (G_TASK (r), &e); fpi_device_open_complete (FP_DEVICE (o), g_steal_pointer (&e)); }
static void close_done (GObject *o, GAsyncResult *r, gpointer unused)
{ g_autoptr(GError) e = NULL; (void) unused; g_task_propagate_boolean (G_TASK (r), &e); fpi_device_close_complete (FP_DEVICE (o), g_steal_pointer (&e)); }
static void delete_done (GObject *o, GAsyncResult *r, gpointer unused)
{ g_autoptr(GError) e = NULL; (void) unused; g_task_propagate_boolean (G_TASK (r), &e); fpi_device_delete_complete (FP_DEVICE (o), g_steal_pointer (&e)); }
static void clear_done (GObject *o, GAsyncResult *r, gpointer unused)
{ g_autoptr(GError) e = NULL; (void) unused; g_task_propagate_boolean (G_TASK (r), &e); fpi_device_clear_storage_complete (FP_DEVICE (o), g_steal_pointer (&e)); }

static void
list_done (GObject *object, GAsyncResult *async_result, gpointer unused)
{
  FpiDeviceGoodixQsee *self = FPI_DEVICE_GOODIXQSEE (object);
  g_autoptr(GError) error = NULL;
  g_autofree struct list_result *result = g_task_propagate_pointer (G_TASK (async_result), &error);
  GPtrArray *prints = NULL;
  size_t i;
  (void) unused;
  if (!result) { fpi_device_list_complete (FP_DEVICE (self), NULL, g_steal_pointer (&error)); return; }
  if (result->count_mismatch) fp_warn ("ENUMERATE count disagrees with occupied slots");
  prints = g_ptr_array_new_with_free_func (g_object_unref);
  for (i = 0; i < result->count; i++)
    g_ptr_array_add (prints, g_object_ref_sink (make_print (self,
                         result->fingers[i].group_id, result->fingers[i].finger_id)));
  fpi_device_list_complete (FP_DEVICE (self), prints, NULL);
}

static void
enroll_done (GObject *object, GAsyncResult *async_result, gpointer unused)
{
  FpiDeviceGoodixQsee *self = FPI_DEVICE_GOODIXQSEE (object);
  g_autoptr(GError) error = NULL;
  g_autofree struct interactive_result *result = g_task_propagate_pointer (G_TASK (async_result), &error);
  FpPrint *print = NULL;
  GVariant *data;
  (void) unused;
  if (!result) { fpi_device_enroll_complete (FP_DEVICE (self), NULL, g_steal_pointer (&error)); return; }
  fpi_device_get_enroll_data (FP_DEVICE (self), &print);
  data = g_variant_new ("(uuu)", self->profile, result->group_id, result->finger_id);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
}

static void
identify_done (GObject *object, GAsyncResult *async_result, gpointer unused)
{
  FpiDeviceGoodixQsee *self = FPI_DEVICE_GOODIXQSEE (object);
  g_autoptr(GError) error = NULL;
  g_autofree struct interactive_result *result = g_task_propagate_pointer (G_TASK (async_result), &error);
  GPtrArray *gallery = NULL;
  FpPrint *match = NULL, *scan = NULL;
  guint i;
  (void) unused;
  if (!result) { fpi_device_identify_complete (FP_DEVICE (self), g_steal_pointer (&error)); return; }
  if (result->status && result->status != GOODIX_QSEE_MATCH_FAIL_AND_RETRY)
    {
      fpi_device_identify_report (FP_DEVICE (self), NULL, NULL,
                                  retry_for_status (result->status));
      fpi_device_identify_complete (FP_DEVICE (self), NULL);
      return;
    }
  if (!result->status)
    {
      scan = g_object_ref_sink (make_print (self, result->group_id, result->finger_id));
      fpi_device_get_identify_data (FP_DEVICE (self), &gallery);
      for (i = 0; i < gallery->len; i++)
        {
          guint32 profile, group, finger;
          if (parse_print (g_ptr_array_index (gallery, i), &profile, &group, &finger) &&
              profile == self->profile && group == result->group_id &&
              finger == result->finger_id)
            { match = g_ptr_array_index (gallery, i); break; }
        }
    }
  fpi_device_identify_report (FP_DEVICE (self), match, scan, NULL);
  fpi_device_identify_complete (FP_DEVICE (self), NULL);
  g_clear_object (&scan);
}

static void goodix_open (FpDevice *d) { start_worker (FPI_DEVICE_GOODIXQSEE (d), WORK_OPEN, 0, 0, open_done); }
static void goodix_close (FpDevice *d) { start_worker (FPI_DEVICE_GOODIXQSEE (d), WORK_CLOSE, 0, 0, close_done); }
static void goodix_list (FpDevice *d) { start_worker (FPI_DEVICE_GOODIXQSEE (d), WORK_LIST, GOODIX_QSEE_GROUP_DEFAULT, 0, list_done); }
static void goodix_enroll (FpDevice *d) { start_worker (FPI_DEVICE_GOODIXQSEE (d), WORK_ENROLL, 0, 0, enroll_done); }
static void goodix_identify (FpDevice *d) { start_worker (FPI_DEVICE_GOODIXQSEE (d), WORK_IDENTIFY, 0, 0, identify_done); }
static void goodix_clear (FpDevice *d) { start_worker (FPI_DEVICE_GOODIXQSEE (d), WORK_CLEAR, 0, 0, clear_done); }

static void
goodix_delete (FpDevice *device)
{
  FpPrint *print = NULL; guint32 profile, group, finger;
  fpi_device_get_delete_data (device, &print);
  if (!parse_print (print, &profile, &group, &finger) ||
      profile != FPI_DEVICE_GOODIXQSEE (device)->profile)
    { fpi_device_delete_complete (device, fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID)); return; }
  start_worker (FPI_DEVICE_GOODIXQSEE (device), WORK_DELETE, group, finger, delete_done);
}

static void
goodix_cancel (FpDevice *device)
{
  /* The worker observes the action cancellable; interactive workers send
   * CMD_CANCEL before completing.  No second QSEE invoke races this thread. */
  (void) device;
}

static void
goodix_probe (FpDevice *device)
{
  FpiDeviceGoodixQsee *self = FPI_DEVICE_GOODIXQSEE (device);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *device_id = NULL;

  self->profile = fpi_device_get_driver_data (device);
  self->ta_name = read_ta_name (device, &error);
  if (self->ta_name)
    device_id = g_strdup_printf ("profile-%u:%s", self->profile, self->ta_name);
  fpi_device_probe_complete (device, device_id, NULL, g_steal_pointer (&error));
}

static void
fpi_device_goodixqsee_init (FpiDeviceGoodixQsee *self)
{
  self->sensor.device_fd = self->sensor.event_fd = -1;
  self->qsee.fd = -1;
  self->token_provider = goodix_qsee_challenge_token;
}

static void
fpi_device_goodixqsee_finalize (GObject *object)
{
  FpiDeviceGoodixQsee *self = FPI_DEVICE_GOODIXQSEE (object);
  g_clear_pointer (&self->ta_name, g_free);
  G_OBJECT_CLASS (fpi_device_goodixqsee_parent_class)->finalize (object);
}

static void
fpi_device_goodixqsee_class_init (FpiDeviceGoodixQseeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *device_class = FP_DEVICE_CLASS (klass);
  object_class->finalize = fpi_device_goodixqsee_finalize;
  device_class->id = FP_COMPONENT;
  device_class->full_name = "Goodix QSEE match-on-chip sensor";
  device_class->type = FP_DEVICE_TYPE_UDEV;
  device_class->id_table = id_table;
  device_class->scan_type = FP_SCAN_TYPE_PRESS;
  device_class->nr_enroll_stages = ENROLL_STAGES_PROFILE_V1;
  device_class->temp_hot_seconds = -1;
  device_class->open = goodix_open;
  device_class->probe = goodix_probe;
  device_class->close = goodix_close;
  device_class->list = goodix_list;
  device_class->delete = goodix_delete;
  device_class->clear_storage = goodix_clear;
  device_class->enroll = goodix_enroll;
  device_class->identify = goodix_identify;
  device_class->cancel = goodix_cancel;
  fpi_device_class_auto_initialize_features (device_class);
}
