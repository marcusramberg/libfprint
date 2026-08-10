/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * FocalTech match-on-chip sensors driven through a QSEE trusted application.
 *
 * The sensor's SPI instance belongs to the secure world, so this driver never
 * images anything: it opens a client session on the trusted application named
 * by the kernel, waits on the sensor's interrupt, and asks the application to
 * capture, enrol and match. Loading the application and serving its storage
 * are separate jobs, done by a supplicant this driver does not talk to.
 */
#define FP_COMPONENT "focaltechqsee"

#include "drivers_api.h"
#include "focaltech-qsee.h"
#include "focaltech-qsee-proto.h"

#include <errno.h>
#include <string.h>

#define FOCALTECH_QSEE_PROFILE_V1 1

/* One group is enough until something wants more than one user's fingers. */
#define GROUP_DEFAULT 0

/*
 * What the application calls its own storage. These are names inside its
 * sealed object store rather than paths on this filesystem: the supplicant
 * roots them under a directory of its own.
 */
#define STORE_PATH "/data/vendor_de/0/fpdata"

/*
 * How many touches an enrolment asks for. The application counts down its own
 * requirement and only counts a touch that adds coverage, so this matches the
 * max_enrolling_samples it is configured with.
 */
#define ENROLL_STAGES 8

/* A touch is worth waiting for, but not forever. */
#define TOUCH_TIMEOUT_MS 20000

/* SPI speed the application clocks the sensor at while bringing it up. */
#define SPI_SPEED 2000000

enum worker_operation
{
  WORK_OPEN,
  WORK_CLOSE,
  WORK_LIST,
  WORK_DELETE,
  WORK_ENROLL,
  WORK_IDENTIFY,
};

struct worker_data
{
  enum worker_operation operation;
  guint32               finger_id;
};

struct list_result
{
  gsize   count;
  guint32 fingers[16];
};

struct progress_event
{
  FpiDeviceFocaltechQsee *self;
  FpFingerStatusFlags     finger_status;
  gint                    completed;
  GError                 *retry;
};

G_DEFINE_TYPE (FpiDeviceFocaltechQsee, fpi_device_focaltechqsee, FP_TYPE_DEVICE)

static const FpIdEntry id_table[] = {
  { .udev_types = FPI_DEVICE_UDEV_SUBTYPE_MISC,
    .misc_name = "focaltech_fp", .misc_compatible = "focaltech,ft9362",
    .driver_data = FOCALTECH_QSEE_PROFILE_V1 },
  { .udev_types = 0 }
};

static GError *
io_error (const char *operation)
{
  return g_error_new (G_IO_ERROR, g_io_error_from_errno (errno),
                      "%s: %s", operation, g_strerror (errno));
}

static GError *
ta_error (const char *operation, gint32 result)
{
  return g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                      "%s: the trusted application answered %d",
                      operation, result);
}

/*
 * A message that never reached the application at all, as opposed to one it
 * answered badly. Worth keeping apart: they have nothing in common but the
 * call that reports them, and reading one as the other sends the search for a
 * cause into the application rather than into the transport.
 */
static GError *
invoke_error (const char *operation)
{
  return g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                      "%s: the trusted application never answered", operation);
}

/*
 * The configuration the application is sent before anything else. Every key
 * here is one it reads with a default of zero, and zero is wrong for all of
 * them: the geometry sizes an allocation it later dereferences, the finger
 * count refuses every enrolment, and a zero quality threshold accepts nothing.
 *
 * trustlet.enable_trusted_enrollment is the exception, and a deliberate one:
 * the application otherwise demands a credential token signed by Android's
 * Gatekeeper before it will accept a new finger. Nothing on a Linux system can
 * produce one, so enrolment is impossible with the check in place. Turning it
 * off means the application no longer requires proof that a user authenticated
 * before a finger is added -- whatever can reach the application can enrol.
 * That protection has to come from above this driver.
 */
static const gchar default_config[] =
  "{"
  "\"driver\":{\"spi_bus_num\":14},"
  "\"device\":{\"preferred_device_id\":\"0x9391\",\"spi_default_bps\":2000000},"
  "\"trustlet\":{\"enable_trusted_enrollment\":false},"
  "\"common\":{\"image_processing_cols\":36,\"image_processing_rows\":144,"
  "\"max_enrolling_fingers\":5,\"max_enrolling_samples\":8},"
  "\"algorithm\":{\"min_enrolling_quality_threshold\":30,"
  "\"min_enrolling_coverage_threshold\":30,"
  "\"min_identify_quality_threshold\":30,"
  "\"min_identify_coverage_threshold\":30}"
  "}";

/* A machine's own file wins, so a board can be tuned without a rebuild. */
#define CONFIG_PATH "/etc/focaltech/ff_config.json"

static gchar *
read_config (void)
{
  gchar *contents = NULL;

  if (g_file_get_contents (CONFIG_PATH, &contents, NULL, NULL))
    return contents;

  return g_strdup (default_config);
}

static gchar *
read_ta_name (FpDevice *device, GError **error)
{
  const gchar *sysfs = fpi_device_get_udev_sysfs_path (device,
                                                       FPI_DEVICE_UDEV_SUBTYPE_MISC);
  g_autofree gchar *direct = NULL;
  g_autofree gchar *parent = NULL;
  gchar *contents = NULL;

  if (!sysfs)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "misc device has no sysfs path");
      return NULL;
    }

  direct = g_build_filename (sysfs, "firmware_name", NULL);
  parent = g_build_filename (sysfs, "device", "firmware_name", NULL);

  if (!g_file_get_contents (direct, &contents, NULL, NULL) &&
      !g_file_get_contents (parent, &contents, NULL, error))
    return NULL;

  g_strstrip (contents);

  if (!*contents)
    {
      g_free (contents);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "firmware_name is empty");
      return NULL;
    }

  return contents;
}

/* One command with no payload of its own. */
static gboolean
command (FpiDeviceFocaltechQsee *self, guint32 cmd, GError **error)
{
  guint8 payload[0x2e0] = { 0 };
  size_t size = focaltech_qsee_payload_size (cmd);
  gint32 result = 0;

  if (focaltech_qsee_tee_invoke (&self->tee, cmd, payload, size, &result))
    {
      g_propagate_error (error, invoke_error ("command"));
      return FALSE;
    }

  if (result)
    {
      g_propagate_error (error, ta_error ("command", result));
      return FALSE;
    }

  return TRUE;
}

/* One command with an argument in the first word of its payload. */
static gboolean
command_arg (FpiDeviceFocaltechQsee *self, guint32 cmd, guint32 arg,
             GError **error)
{
  guint8 payload[0x2e0] = { 0 };
  size_t size = focaltech_qsee_payload_size (cmd);
  gint32 result = 0;

  if (size < sizeof (arg))
    size = sizeof (arg);

  memcpy (payload, &arg, sizeof (arg));

  if (focaltech_qsee_tee_invoke (&self->tee, cmd, payload, size, &result))
    {
      g_propagate_error (error, invoke_error ("command"));
      return FALSE;
    }

  if (result)
    {
      g_propagate_error (error, ta_error ("command", result));
      return FALSE;
    }

  return TRUE;
}

static gboolean
sync_config (FpiDeviceFocaltechQsee *self, GError **error)
{
  g_autofree gchar *config = read_config ();
  gint32 result = 0;

  if (focaltech_qsee_tee_invoke (&self->tee, FOCALTECH_QSEE_CMD_SYNC_CONFIG,
                                 config, strlen (config) + 1, &result))
    {
      g_propagate_error (error, invoke_error ("sync config"));
      return FALSE;
    }

  if (result)
    {
      g_propagate_error (error, ta_error ("sync config", result));
      return FALSE;
    }

  return TRUE;
}

static gboolean
set_active_group (FpiDeviceFocaltechQsee *self, guint32 group, GError **error)
{
  guint8 payload[0x40] = { 0 };
  size_t size = focaltech_qsee_build_set_active_group (payload, sizeof (payload),
                                                      group, STORE_PATH);
  gint32 result = 0;

  /*
   * A group that has never been enrolled into answers with an error while
   * still doing its work -- it reports that it loaded nothing -- so the
   * result is deliberately not treated as fatal here.
   */
  if (focaltech_qsee_tee_invoke (&self->tee, FOCALTECH_QSEE_CMD_SET_ACTIVE_GROUP,
                                 payload, size, &result))
    {
      g_propagate_error (error, invoke_error ("set active group"));
      return FALSE;
    }

  return TRUE;
}

static gboolean
bring_up (FpiDeviceFocaltechQsee *self, GError **error)
{
  if (!sync_config (self, error))
    return FALSE;

  if (!command (self, FOCALTECH_QSEE_CMD_INIT_SPI, error) ||
      !command_arg (self, FOCALTECH_QSEE_CMD_SET_SPI_SPEED, SPI_SPEED, error) ||
      !command_arg (self, FOCALTECH_QSEE_CMD_PROBE_DEVICE, 1, error) ||
      !command (self, FOCALTECH_QSEE_CMD_INIT_DEVICE, error) ||
      !command (self, FOCALTECH_QSEE_CMD_INIT, error))
    return FALSE;

  /*
   * Not optional, whatever its name suggests: this installs the object the
   * application's enrolment path writes into without checking. Skip it and the
   * first touch after an enrolment is armed takes the application down.
   */
  if (!command (self, FOCALTECH_QSEE_CMD_SYNC_STATISTICS, error))
    return FALSE;

  if (!command (self, FOCALTECH_QSEE_CMD_START_SCANNING, error))
    return FALSE;

  /*
   * Scanning is not detecting: until the chip is put into finger-detect mode
   * the interrupt never moves, however hard the sensor is pressed.
   */
  if (!command_arg (self, FOCALTECH_QSEE_CMD_WORK_MODE,
                    FOCALTECH_QSEE_MODE_DOWN_DETECT, error))
    return FALSE;

  return set_active_group (self, GROUP_DEFAULT, error);
}

/*
 * Wait for a finger, take an image, and tell the application about it. The
 * event is where the work happens: an armed enrolment takes its sample there,
 * and an armed authentication does its matching there.
 */
static gboolean
touch_cycle (FpiDeviceFocaltechQsee *self, guint32 event, GError **error)
{
  guint8 payload[0x2e0];
  gint32 result = 0;
  int ready;

  focaltech_qsee_sensor_drain (&self->sensor);

  if (focaltech_qsee_sensor_arm (&self->sensor))
    {
      g_propagate_error (error, io_error ("arm interrupt"));
      return FALSE;
    }

  ready = focaltech_qsee_sensor_wait (&self->sensor, TOUCH_TIMEOUT_MS);
  if (ready < 0)
    {
      g_propagate_error (error, io_error ("wait for finger"));
      return FALSE;
    }
  if (ready == 0)
    {
      g_propagate_error (error,
                         fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                   "no finger arrived"));
      return FALSE;
    }

  /*
   * Capture before reporting, which is the order the vendor uses: reporting a
   * touch the application has no image for sends it into its enrolment path
   * with nothing to work from.
   *
   * The reset-before-scanning the vendor asks for on an interrupt takes long
   * enough that a finger is usually gone before the sensor samples, so it is
   * skipped here.
   */
  focaltech_qsee_build_capture (payload, sizeof (payload), FALSE);
  if (focaltech_qsee_tee_invoke (&self->tee, FOCALTECH_QSEE_CMD_CAPTURE_IMAGE,
                                 payload, 0x24, &result))
    {
      g_propagate_error (error, invoke_error ("capture"));
      return FALSE;
    }

  if (result)
    {
      /* No usable image: a light or fast touch, worth asking again for. */
      g_propagate_error (error,
                         fpi_device_retry_new (FP_DEVICE_RETRY_TOO_SHORT));
      return FALSE;
    }

  focaltech_qsee_build_event (payload, sizeof (payload), event);
  if (focaltech_qsee_tee_invoke (&self->tee, FOCALTECH_QSEE_CMD_REPORT_EVENT,
                                 payload, 0x2e0, &result))
    {
      g_propagate_error (error, invoke_error ("report event"));
      return FALSE;
    }

  if (result)
    {
      g_propagate_error (error,
                         fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      return FALSE;
    }

  return TRUE;
}

static gboolean
enumerate (FpiDeviceFocaltechQsee *self, struct list_result *out, GError **error)
{
  guint8 payload[0x40] = { 0 };
  gint32 result = 0;

  if (focaltech_qsee_tee_invoke (&self->tee, FOCALTECH_QSEE_CMD_ENUMERATE,
                                 payload, sizeof (payload), &result))
    {
      g_propagate_error (error, invoke_error ("enumerate"));
      return FALSE;
    }

  if (result)
    {
      g_propagate_error (error, ta_error ("enumerate", result));
      return FALSE;
    }

  out->count = focaltech_qsee_parse_enumerate (payload, sizeof (payload),
                                               out->fingers,
                                               G_N_ELEMENTS (out->fingers));

  return TRUE;
}

static FpPrint *
make_print (FpiDeviceFocaltechQsee *self, guint32 group, guint32 finger)
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

  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE ("(uuu)")))
    return FALSE;

  g_variant_get (data, "(uuu)", profile, group, finger);

  return TRUE;
}

static gboolean
progress_in_main (gpointer user_data)
{
  struct progress_event *event = user_data;

  fpi_device_report_finger_status (FP_DEVICE (event->self), event->finger_status);

  if (event->completed >= 0 || event->retry)
    fpi_device_enroll_progress (FP_DEVICE (event->self),
                                MAX (event->completed, 0), NULL,
                                g_steal_pointer (&event->retry));

  g_object_unref (event->self);
  g_free (event);

  return G_SOURCE_REMOVE;
}

static void
queue_progress (FpiDeviceFocaltechQsee *self, FpFingerStatusFlags finger_status,
                gint completed, GError *retry)
{
  struct progress_event *event = g_new0 (struct progress_event, 1);

  event->self = g_object_ref (self);
  event->finger_status = finger_status;
  event->completed = completed;
  event->retry = retry;

  g_main_context_invoke (NULL, progress_in_main, event);
}

static gboolean
do_open (FpiDeviceFocaltechQsee *self, GError **error)
{
  const gchar *node = fpi_device_get_udev_data (FP_DEVICE (self),
                                                FPI_DEVICE_UDEV_SUBTYPE_MISC);

  if (!node)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "no misc device node");
      return FALSE;
    }

  self->ta_name = read_ta_name (FP_DEVICE (self), error);
  if (!self->ta_name)
    return FALSE;

  if (focaltech_qsee_sensor_open (&self->sensor, node))
    {
      g_propagate_error (error, io_error ("open sensor"));
      return FALSE;
    }

  /*
   * The application has to be loaded already: loading it is privileged and
   * belongs to a service of its own, while this only opens a client session.
   */
  if (focaltech_qsee_tee_open (&self->tee, self->ta_name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "no session on '%s'; is it loaded?", self->ta_name);
      focaltech_qsee_sensor_close (&self->sensor);
      return FALSE;
    }

  if (!bring_up (self, error))
    {
      focaltech_qsee_tee_close (&self->tee);
      focaltech_qsee_sensor_close (&self->sensor);
      return FALSE;
    }

  return TRUE;
}

static gboolean
do_enroll (FpiDeviceFocaltechQsee *self, guint32 *finger, GError **error)
{
  struct list_result before = { 0 }, after = { 0 };
  gint completed = 0;

  if (!enumerate (self, &before, error))
    return FALSE;

  if (!command (self, FOCALTECH_QSEE_CMD_PRE_ENROLL, error) ||
      !command (self, FOCALTECH_QSEE_CMD_ENROLL, error))
    return FALSE;

  while (completed < ENROLL_STAGES)
    {
      g_autoptr(GError) local = NULL;

      queue_progress (self, FP_FINGER_STATUS_NEEDED, -1, NULL);

      if (touch_cycle (self, FOCALTECH_QSEE_EVENT_FINGER_DOWN, &local))
        {
          completed++;
          queue_progress (self, FP_FINGER_STATUS_NONE, completed, NULL);
          continue;
        }

      /*
       * A touch the application would not take is not a failure: the finger
       * was too light, too fast, or landed where it already has coverage.
       * Anything else ends the enrolment.
       */
      if (local->domain != FP_DEVICE_RETRY)
        {
          g_propagate_error (error, g_steal_pointer (&local));
          return FALSE;
        }

      queue_progress (self, FP_FINGER_STATUS_NONE, completed,
                      g_steal_pointer (&local));
    }

  /*
   * Write it through. The payload is a mask of what to save, and with it zero
   * the application stores only its calibration and silently keeps the
   * template in memory, where the next boot will not find it.
   */
  if (!command_arg (self, FOCALTECH_QSEE_CMD_SAVE_DATA,
                    FOCALTECH_QSEE_SAVE_ALL, error))
    return FALSE;

  if (!command (self, FOCALTECH_QSEE_CMD_POST_ENROLL, error))
    return FALSE;

  /* The application allocates the id, so learn it by asking what is new. */
  if (!enumerate (self, &after, error))
    return FALSE;

  for (gsize i = 0; i < after.count; i++)
    {
      gboolean known = FALSE;

      for (gsize j = 0; j < before.count; j++)
        if (after.fingers[i] == before.fingers[j])
          known = TRUE;

      if (!known)
        {
          *finger = after.fingers[i];
          return TRUE;
        }
    }

  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "the enrolment stored no new finger");

  return FALSE;
}

static gboolean
do_identify (FpiDeviceFocaltechQsee *self, guint32 *finger, GError **error)
{
  struct list_result stored = { 0 };

  if (!enumerate (self, &stored, error))
    return FALSE;

  if (!stored.count)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           "nothing is enrolled");
      return FALSE;
    }

  /* Arming is all AUTHENTICATE does; the match happens on the event. */
  if (!command (self, FOCALTECH_QSEE_CMD_AUTHENTICATE, error))
    return FALSE;

  queue_progress (self, FP_FINGER_STATUS_NEEDED, -1, NULL);

  if (!touch_cycle (self, FOCALTECH_QSEE_EVENT_FINGER_DOWN, error))
    {
      queue_progress (self, FP_FINGER_STATUS_NONE, -1, NULL);
      return FALSE;
    }

  queue_progress (self, FP_FINGER_STATUS_NONE, -1, NULL);

  /*
   * Which finger matched is not decoded yet, so a match is reported against
   * the only enrolled finger and refused when there is more than one. Reading
   * the matched id out of the event's answer is what lifts this.
   */
  if (stored.count != 1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "matching with more than one finger enrolled needs "
                           "the matched id decoded from the event");
      return FALSE;
    }

  *finger = stored.fingers[0];

  return TRUE;
}

static void
worker_thread (GTask *task, gpointer source, gpointer task_data,
               GCancellable *cancellable)
{
  FpiDeviceFocaltechQsee *self = source;
  struct worker_data *work = task_data;
  g_autoptr(GError) error = NULL;

  (void) cancellable;

  switch (work->operation)
    {
    case WORK_OPEN:
      if (!do_open (self, &error))
        break;
      g_task_return_boolean (task, TRUE);
      return;

    case WORK_CLOSE:
      focaltech_qsee_sensor_close (&self->sensor);
      focaltech_qsee_tee_close (&self->tee);
      g_clear_pointer (&self->ta_name, g_free);
      g_task_return_boolean (task, TRUE);
      return;

    case WORK_LIST:
      {
        struct list_result *result = g_new0 (struct list_result, 1);

        if (!enumerate (self, result, &error))
          {
            g_free (result);
            break;
          }

        g_task_return_pointer (task, result, g_free);
        return;
      }

    case WORK_DELETE:
      {
        guint8 payload[8] = { 0 };
        gint32 result = 0;

        focaltech_qsee_build_remove (payload, sizeof (payload), work->finger_id);

        if (focaltech_qsee_tee_invoke (&self->tee, FOCALTECH_QSEE_CMD_REMOVE,
                                       payload, sizeof (payload), &result) ||
            result)
          {
            error = ta_error ("remove", result);
            break;
          }

        if (!command_arg (self, FOCALTECH_QSEE_CMD_SAVE_DATA,
                          FOCALTECH_QSEE_SAVE_ALL, &error))
          break;

        g_task_return_boolean (task, TRUE);
        return;
      }

    case WORK_ENROLL:
      {
        guint32 *finger = g_new0 (guint32, 1);

        if (!do_enroll (self, finger, &error))
          {
            g_free (finger);
            break;
          }

        g_task_return_pointer (task, finger, g_free);
        return;
      }

    case WORK_IDENTIFY:
      {
        guint32 *finger = g_new0 (guint32, 1);

        if (!do_identify (self, finger, &error))
          {
            g_free (finger);
            break;
          }

        g_task_return_pointer (task, finger, g_free);
        return;
      }
    }

  g_task_return_error (task, g_steal_pointer (&error));
}

static void
start_worker (FpiDeviceFocaltechQsee *self, enum worker_operation operation,
              guint32 finger, GAsyncReadyCallback callback)
{
  GTask *task = g_task_new (self,
                            operation == WORK_OPEN || operation == WORK_CLOSE ?
                            NULL : fpi_device_get_cancellable (FP_DEVICE (self)),
                            callback, NULL);
  struct worker_data *work = g_new0 (struct worker_data, 1);

  work->operation = operation;
  work->finger_id = finger;

  g_task_set_task_data (task, work, g_free);
  g_task_run_in_thread (task, worker_thread);
  g_object_unref (task);
}

static void
open_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  g_autoptr(GError) error = NULL;

  (void) unused;
  g_task_propagate_boolean (G_TASK (result), &error);
  fpi_device_open_complete (FP_DEVICE (source), g_steal_pointer (&error));
}

static void
close_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  g_autoptr(GError) error = NULL;

  (void) unused;
  g_task_propagate_boolean (G_TASK (result), &error);
  fpi_device_close_complete (FP_DEVICE (source), g_steal_pointer (&error));
}

static void
delete_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  g_autoptr(GError) error = NULL;

  (void) unused;
  g_task_propagate_boolean (G_TASK (result), &error);
  fpi_device_delete_complete (FP_DEVICE (source), g_steal_pointer (&error));
}

static void
list_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  FpiDeviceFocaltechQsee *self = FPI_DEVICE_FOCALTECHQSEE (source);
  g_autoptr(GError) error = NULL;
  g_autofree struct list_result *listed = NULL;
  GPtrArray *prints;

  (void) unused;
  listed = g_task_propagate_pointer (G_TASK (result), &error);

  if (!listed)
    {
      fpi_device_list_complete (FP_DEVICE (self), NULL, g_steal_pointer (&error));
      return;
    }

  prints = g_ptr_array_new_with_free_func (g_object_unref);

  for (gsize i = 0; i < listed->count; i++)
    g_ptr_array_add (prints,
                     g_object_ref_sink (make_print (self, GROUP_DEFAULT,
                                                    listed->fingers[i])));

  fpi_device_list_complete (FP_DEVICE (self), prints, NULL);
}

static void
enroll_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  FpiDeviceFocaltechQsee *self = FPI_DEVICE_FOCALTECHQSEE (source);
  g_autoptr(GError) error = NULL;
  g_autofree guint32 *finger = NULL;
  FpPrint *print = NULL;

  (void) unused;
  finger = g_task_propagate_pointer (G_TASK (result), &error);

  if (!finger)
    {
      fpi_device_enroll_complete (FP_DEVICE (self), NULL,
                                  g_steal_pointer (&error));
      return;
    }

  fpi_device_get_enroll_data (FP_DEVICE (self), &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data",
                g_variant_new ("(uuu)", self->profile, GROUP_DEFAULT, *finger),
                NULL);

  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
}

static void
identify_done (GObject *source, GAsyncResult *result, gpointer unused)
{
  FpiDeviceFocaltechQsee *self = FPI_DEVICE_FOCALTECHQSEE (source);
  g_autoptr(GError) error = NULL;
  g_autofree guint32 *finger = NULL;
  g_autoptr(FpPrint) scan = NULL;
  GPtrArray *gallery = NULL;
  FpPrint *matched = NULL;

  (void) unused;
  finger = g_task_propagate_pointer (G_TASK (result), &error);

  if (!finger)
    {
      fpi_device_identify_complete (FP_DEVICE (self), g_steal_pointer (&error));
      return;
    }

  scan = g_object_ref_sink (make_print (self, GROUP_DEFAULT, *finger));
  fpi_device_get_identify_data (FP_DEVICE (self), &gallery);

  if (gallery)
    {
      for (guint i = 0; i < gallery->len; i++)
        {
          FpPrint *candidate = g_ptr_array_index (gallery, i);
          guint32 profile, group, id;

          if (parse_print (candidate, &profile, &group, &id) &&
              group == GROUP_DEFAULT && id == *finger)
            {
              matched = candidate;
              break;
            }
        }
    }

  fpi_device_identify_report (FP_DEVICE (self), matched, scan, NULL);
  fpi_device_identify_complete (FP_DEVICE (self), NULL);
}

static void
focaltech_open (FpDevice *device)
{
  start_worker (FPI_DEVICE_FOCALTECHQSEE (device), WORK_OPEN, 0, open_done);
}

static void
focaltech_close (FpDevice *device)
{
  start_worker (FPI_DEVICE_FOCALTECHQSEE (device), WORK_CLOSE, 0, close_done);
}

static void
focaltech_list (FpDevice *device)
{
  start_worker (FPI_DEVICE_FOCALTECHQSEE (device), WORK_LIST, 0, list_done);
}

static void
focaltech_enroll (FpDevice *device)
{
  start_worker (FPI_DEVICE_FOCALTECHQSEE (device), WORK_ENROLL, 0, enroll_done);
}

static void
focaltech_identify (FpDevice *device)
{
  start_worker (FPI_DEVICE_FOCALTECHQSEE (device), WORK_IDENTIFY, 0,
                identify_done);
}

static void
focaltech_delete (FpDevice *device)
{
  FpPrint *print = NULL;
  guint32 profile, group, finger;

  fpi_device_get_delete_data (device, &print);

  if (!parse_print (print, &profile, &group, &finger))
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  start_worker (FPI_DEVICE_FOCALTECHQSEE (device), WORK_DELETE, finger,
                delete_done);
}

static void
fpi_device_focaltechqsee_init (FpiDeviceFocaltechQsee *self)
{
  self->sensor.fd = -1;
  self->tee.fd = -1;
  self->profile = FOCALTECH_QSEE_PROFILE_V1;
}

static void
fpi_device_focaltechqsee_finalize (GObject *object)
{
  FpiDeviceFocaltechQsee *self = FPI_DEVICE_FOCALTECHQSEE (object);

  g_clear_pointer (&self->ta_name, g_free);

  G_OBJECT_CLASS (fpi_device_focaltechqsee_parent_class)->finalize (object);
}

static void
fpi_device_focaltechqsee_class_init (FpiDeviceFocaltechQseeClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "FocalTech match-on-chip sensor (QSEE)";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = focaltech_open;
  dev_class->close = focaltech_close;
  dev_class->enroll = focaltech_enroll;
  dev_class->verify = focaltech_identify;
  dev_class->identify = focaltech_identify;
  dev_class->delete = focaltech_delete;
  dev_class->list = focaltech_list;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_focaltechqsee_finalize;

  fpi_device_class_auto_initialize_features (dev_class);
}
