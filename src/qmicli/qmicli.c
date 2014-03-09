/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * qmicli -- Command line interface to control QMI devices
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2012 Aleksander Morgado <aleksander@lanedo.com>
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gio.h>

#include <libqmi-glib.h>

#include "qmicli.h"
#include "qmicli-helpers.h"

#include <jansson.h>

#define PROGRAM_NAME    "qmicli"
#define PROGRAM_VERSION PACKAGE_VERSION

/* Globals */
static GMainLoop *loop;
static GCancellable *cancellable;
static QmiDevice *device;
static QmiClient *client;
static QmiService service;
static gboolean operation_status;

/* Main options */
static gchar *device_str;
static gboolean get_service_version_info_flag;
static gchar *device_set_instance_id_str;
static gboolean device_open_version_info_flag;
static gboolean device_open_sync_flag;
static gchar *device_open_net_str;
static gboolean device_open_proxy_flag;
static gchar *client_cid_str;
static gboolean client_no_release_cid_flag;
static gboolean verbose_flag;
static gboolean json_flag;
size_t json_print_flag = JSON_PRESERVE_ORDER + JSON_INDENT(4);
static gboolean silent_flag;
static gboolean version_flag;

static GOptionEntry main_entries[] = {
    { "device", 'd', 0, G_OPTION_ARG_STRING, &device_str,
      "Specify device path",
      "[PATH]"
    },
    { "get-service-version-info", 0, 0, G_OPTION_ARG_NONE, &get_service_version_info_flag,
      "Get service version info",
      NULL
    },
    { "device-set-instance-id", 0, 0, G_OPTION_ARG_STRING, &device_set_instance_id_str,
      "Set instance ID",
      "[Instance ID]"
    },
    { "device-open-version-info", 0, 0, G_OPTION_ARG_NONE, &device_open_version_info_flag,
      "Run version info check when opening device",
      NULL
    },
    { "device-open-sync", 0, 0, G_OPTION_ARG_NONE, &device_open_sync_flag,
      "Run sync operation when opening device",
      NULL
    },
    { "device-open-proxy", 'p', 0, G_OPTION_ARG_NONE, &device_open_proxy_flag,
      "Request to use the 'qmi-proxy' proxy",
      NULL
    },
    { "device-open-net", 0, 0, G_OPTION_ARG_STRING, &device_open_net_str,
      "Open device with specific link protocol and QoS flags",
      "[net-802-3|net-raw-ip|net-qos-header|net-no-qos-header]"
    },
    { "client-cid", 0, 0, G_OPTION_ARG_STRING, &client_cid_str,
      "Use the given CID, don't allocate a new one",
      "[CID]"
    },
    { "client-no-release-cid", 0, 0, G_OPTION_ARG_NONE, &client_no_release_cid_flag,
      "Do not release the CID when exiting",
      NULL
    },
    { "json", 'j', 0, G_OPTION_ARG_NONE, &json_flag,
      "Attempt to output COMPACT JSON for standard messages and errors",
      NULL
    },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose_flag,
      "Run action with verbose logs, including the debug ones",
      NULL
    },
    { "silent", 0, 0, G_OPTION_ARG_NONE, &silent_flag,
      "Run action with no logs; not even the error/warning ones",
      NULL
    },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &version_flag,
      "Print version",
      NULL
    },
    { NULL }
};

static void
signals_handler (int signum)
{
    if (cancellable) {
        /* Ignore consecutive requests of cancellation */
        if (!g_cancellable_is_cancelled (cancellable)) {
            g_printerr ("%s\n",
                        "cancelling the operation...\n");
            g_cancellable_cancel (cancellable);
        }
        return;
    }

    if (loop &&
        g_main_loop_is_running (loop)) {
        g_printerr ("%s\n",
                    "cancelling the main loop...\n");
        g_main_loop_quit (loop);
    }
}

static void
log_handler (const gchar *log_domain,
             GLogLevelFlags log_level,
             const gchar *message,
             gpointer user_data)
{
    const gchar *log_level_str;
    time_t now;
    gchar time_str[64];
    struct tm *local_time;
    gboolean err;

    /* Nothing to do if we're silent */
    if (silent_flag)
        return;

    now = time ((time_t *) NULL);
    local_time = localtime (&now);
    strftime (time_str, 64, "%d %b %Y, %H:%M:%S", local_time);
    err = FALSE;

    switch (log_level) {
    case G_LOG_LEVEL_WARNING:
        log_level_str = "-Warning **";
        err = TRUE;
        break;

    case G_LOG_LEVEL_CRITICAL:
    case G_LOG_FLAG_FATAL:
    case G_LOG_LEVEL_ERROR:
        log_level_str = "-Error **";
        err = TRUE;
        break;

    case G_LOG_LEVEL_DEBUG:
        log_level_str = "[Debug]";
        break;

    default:
        log_level_str = "";
        break;
    }

    if (!verbose_flag && !err)
        return;

    g_fprintf (err ? stderr : stdout,
               "[%s] %s %s\n",
               time_str,
               log_level_str,
               message);
}

static void
print_version_and_exit (void)
{
    g_print ("%s\n", json_dumps(json_pack("{sbssssssss}",
            "success", 1,
            "program_name", PROGRAM_NAME,
            "program_version", PROGRAM_VERSION,
            "copyright", "Copyright (2012) Aleksander Morgado\n",
            "license", "License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl-2.0.html>. This is free software: you are free to change and redistribute it. There is NO WARRANTY, to the extent permitted by law."
             ),json_print_flag));
    exit (EXIT_SUCCESS);
}

static gboolean
generic_options_enabled (void)
{
    static guint n_actions = 0;
    static gboolean checked = FALSE;

    if (checked)
        return !!n_actions;

    n_actions = (!!device_set_instance_id_str +
                 get_service_version_info_flag);

    if (n_actions > 1) {
    g_print ("%s\n", json_dumps(json_pack("{sbss}",
            "success", 0,
            "error", "too many generic actions requested"
             ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    checked = TRUE;
    return !!n_actions;
}

/*****************************************************************************/
/* Running asynchronously */

static void
release_client_ready (QmiDevice *dev,
                      GAsyncResult *res)
{
    GError *error = NULL;

    if (!qmi_device_release_client_finish (dev, res, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't release client",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
    } else
        g_debug ("Client released");

    g_main_loop_quit (loop);
}

void
qmicli_async_operation_done (gboolean reported_operation_status)
{
    QmiDeviceReleaseClientFlags flags = QMI_DEVICE_RELEASE_CLIENT_FLAGS_NONE;

    /* Keep the result of the operation */
    operation_status = reported_operation_status;

    if (cancellable) {
        g_object_unref (cancellable);
        cancellable = NULL;
    }

    /* If no client was allocated (e.g. generic action), just quit */
    if (!client) {
        g_main_loop_quit (loop);
        return;
    }

    if (!client_no_release_cid_flag)
        flags |= QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID;
    else
        g_print ("[%s] Client ID not released:\n"
                 "\tService: '%s'\n"
                 "\t    CID: '%u'\n",
                 qmi_device_get_path_display (device),
                 qmi_service_get_string (service),
                 qmi_client_get_cid (client));

    qmi_device_release_client (device,
                               client,
                               flags,
                               10,
                               NULL,
                               (GAsyncReadyCallback)release_client_ready,
                               NULL);
}

static void
allocate_client_ready (QmiDevice *dev,
                       GAsyncResult *res)
{
    GError *error = NULL;

    client = qmi_device_allocate_client_finish (dev, res, &error);
    if (!client) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't create client for the service",
             "message", error->message,
             "service", qmi_service_get_string (service)
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    /* Run the service-specific action */
    switch (service) {
    case QMI_SERVICE_DMS:
        qmicli_dms_run (dev, QMI_CLIENT_DMS (client), cancellable);
        return;
    case QMI_SERVICE_NAS:
        qmicli_nas_run (dev, QMI_CLIENT_NAS (client), cancellable);
        return;
    case QMI_SERVICE_WDS:
        qmicli_wds_run (dev, QMI_CLIENT_WDS (client), cancellable);
        return;
    case QMI_SERVICE_PBM:
        qmicli_pbm_run (dev, QMI_CLIENT_PBM (client), cancellable);
        return;
    case QMI_SERVICE_UIM:
        qmicli_uim_run (dev, QMI_CLIENT_UIM (client), cancellable);
        return;
    default:
        g_assert_not_reached ();
    }
}

static void
device_allocate_client (QmiDevice *dev)
{
    guint8 cid = QMI_CID_NONE;

    if (client_cid_str) {
        guint32 cid32;

        cid32 = atoi (client_cid_str);
        if (!cid32 || cid32 > G_MAXUINT8) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "invalid cid given",
             "message", client_cid_str
              ),json_print_flag));
            exit (EXIT_FAILURE);
        }

        cid = (guint8)cid32;
        g_debug ("Reusing CID '%u'", cid);
    }

    /* As soon as we get the QmiDevice, create a client for the requested
     * service */
    qmi_device_allocate_client (dev,
                                service,
                                cid,
                                10,
                                cancellable,
                                (GAsyncReadyCallback)allocate_client_ready,
                                NULL);
}

static void
set_instance_id_ready (QmiDevice *dev,
                       GAsyncResult *res)
{
    GError *error = NULL;
    guint16 link_id;

    if (!qmi_device_set_instance_id_finish (dev, res, &link_id, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't set instance id",
             "message", error->message
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    g_print ("[%s] Instance ID set:\n"
             "\tLink ID: '%" G_GUINT16_FORMAT "'\n",
             qmi_device_get_path_display (dev),
             link_id);

    /* We're done now */
    qmicli_async_operation_done (TRUE);
}

static void
device_set_instance_id (QmiDevice *dev)
{
    gint instance_id;

    if (g_str_equal (device_set_instance_id_str, "0"))
        instance_id = 0;
    else {
        instance_id = atoi (device_set_instance_id_str);
        if (instance_id == 0) {
            g_print ("%s\n", json_dumps(json_pack("{sbssss}",
               "success", 0,
               "error", "invalid instance id given",
               "message", device_set_instance_id_str
               ),json_print_flag));
            exit (EXIT_FAILURE);
        } else if (instance_id < 0 || instance_id > G_MAXUINT8) {
            g_print ("%s\n", json_dumps(json_pack("{sbsssssi}",
                        "success", 0,
                        "error", "given instance id is out of range",
                        "message", device_set_instance_id_str,
                        "max", G_MAXUINT8
                        ),json_print_flag));
            exit (EXIT_FAILURE);
        }
    }

    g_debug ("Setting instance ID '%d'...", instance_id);
    qmi_device_set_instance_id (dev,
                                (guint8)instance_id,
                                10,
                                cancellable,
                                (GAsyncReadyCallback)set_instance_id_ready,
                                NULL);
}

static void
get_service_version_info_ready (QmiDevice *dev,
                                GAsyncResult *res)
{
    GError *error = NULL;
    GArray *services;
    guint i;
    json_t *json_output;

    services = qmi_device_get_service_version_info_finish (dev, res, &error);
    if (!services) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get service version info",
             "message", error->message
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (dev)
              );
    for (i = 0; i < services->len; i++) {
        QmiDeviceServiceVersionInfo *info;
        const gchar *service_str;
        gchar unknownhex[15];

        info = &g_array_index (services, QmiDeviceServiceVersionInfo, i);
        service_str = qmi_service_get_string (info->service);
        if (service_str)
            json_object_update(json_output,json_pack("{s{sisi}}",
                     service_str,
                                "major", info->major_version,
                                "minor", info->minor_version
                     ));
        else {
            g_snprintf(unknownhex,14,"unknown 0x%02x", info->service);
            json_object_update(json_output,json_pack("s{sisi}",
                     unknownhex,
                                "major", info->major_version,
                                "minor", info->minor_version
                     ));
        }
    }
    g_print ("%s\n", json_dumps(json_output,json_print_flag));
    free(json_output);
    g_array_unref (services);

    /* We're done now */
    qmicli_async_operation_done (TRUE);
}

static void
device_get_service_version_info (QmiDevice *dev)
{
    g_debug ("Getting service version info...");
    qmi_device_get_service_version_info (dev,
                                         10,
                                         cancellable,
                                         (GAsyncReadyCallback)get_service_version_info_ready,
                                         NULL);
}

static void
device_open_ready (QmiDevice *dev,
                   GAsyncResult *res)
{
    GError *error = NULL;

    if (!qmi_device_open_finish (dev, res, &error)) {
            g_print ("%s\n", json_dumps(json_pack("{sbssss}",
                "success", 0,
                "error", "couldn't open the QmiDevice",
                "message", error->message
                ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    g_debug ("QMI Device at '%s' ready",
             qmi_device_get_path_display (dev));

    if (device_set_instance_id_str)
        device_set_instance_id (dev);
    else if (get_service_version_info_flag)
        device_get_service_version_info (dev);
    else
        device_allocate_client (dev);
}

static void
device_new_ready (GObject *unused,
                  GAsyncResult *res)
{
    QmiDeviceOpenFlags open_flags = QMI_DEVICE_OPEN_FLAGS_NONE;
    GError *error = NULL;

    device = qmi_device_new_finish (res, &error);
    if (!device) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
              "success", 0,
              "error", "couldn't create QmiDevice",
              "message", error->message
               ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    /* Setup device open flags */
    if (device_open_version_info_flag)
        open_flags |= QMI_DEVICE_OPEN_FLAGS_VERSION_INFO;
    if (device_open_sync_flag)
        open_flags |= QMI_DEVICE_OPEN_FLAGS_SYNC;
    if (device_open_proxy_flag)
        open_flags |= QMI_DEVICE_OPEN_FLAGS_PROXY;
    if (device_open_net_str)
        if (!qmicli_read_net_open_flags_from_string (device_open_net_str, &open_flags))
            exit (EXIT_FAILURE);

    /* Open the device */
    qmi_device_open (device,
                     open_flags,
                     15,
                     cancellable,
                     (GAsyncReadyCallback)device_open_ready,
                     NULL);
}

/*****************************************************************************/

static void
parse_actions (void)
{
    guint actions_enabled = 0;

    /* Generic options? */
    if (generic_options_enabled ()) {
        service = QMI_SERVICE_CTL;
        actions_enabled++;
    }

    /* DMS options? */
    if (qmicli_dms_options_enabled ()) {
        service = QMI_SERVICE_DMS;
        actions_enabled++;
    }

    /* NAS options? */
    if (qmicli_nas_options_enabled ()) {
        service = QMI_SERVICE_NAS;
        actions_enabled++;
    }

    /* WDS options? */
    if (qmicli_wds_options_enabled ()) {
        service = QMI_SERVICE_WDS;
        actions_enabled++;
    }

    /* PBM options? */
    if (qmicli_pbm_options_enabled ()) {
        service = QMI_SERVICE_PBM;
        actions_enabled++;
    }

    /* UIM options? */
    if (qmicli_uim_options_enabled ()) {
        service = QMI_SERVICE_UIM;
        actions_enabled++;
    }

    /* Cannot mix actions from different services */
    if (actions_enabled > 1) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", "cannot execute multiple actions of different services"
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    /* No options? */
    if (actions_enabled == 0) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", "no actions specified"
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    /* Go on! */
}

int main (int argc, char **argv)
{
    GError *error = NULL;
    GFile *file;
    GOptionContext *context;

    setlocale (LC_ALL, "");

    g_type_init ();

    /* Setup option context, process it and destroy it */
    context = g_option_context_new ("- Control QMI devices");
        g_option_context_add_group (context,
                                    qmicli_dms_get_option_group ());
        g_option_context_add_group (context,
                                    qmicli_nas_get_option_group ());
        g_option_context_add_group (context,
                                    qmicli_wds_get_option_group ());
        g_option_context_add_group (context,
                                    qmicli_pbm_get_option_group ());
        g_option_context_add_group (context,
                                    qmicli_uim_get_option_group ());
    g_option_context_add_main_entries (context, main_entries, NULL);
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", error->message
              ),JSON_PRESERVE_ORDER + JSON_INDENT(4)));
        exit (EXIT_FAILURE);
    }
        g_option_context_free (context);

    if (json_flag)
        json_print_flag = JSON_PRESERVE_ORDER + JSON_COMPACT;

    if (version_flag)
        print_version_and_exit ();

    g_log_set_handler (NULL, G_LOG_LEVEL_MASK, log_handler, NULL);
    g_log_set_handler ("Qmi", G_LOG_LEVEL_MASK, log_handler, NULL);
    if (verbose_flag)
        qmi_utils_set_traces_enabled (TRUE);

    /* No device path given? */
    if (!device_str) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", "no device path specified"
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    /* Build new GFile from the commandline arg */
    file = g_file_new_for_commandline_arg (device_str);

    /* Setup signals */
    signal (SIGINT, signals_handler);
    signal (SIGHUP, signals_handler);
    signal (SIGTERM, signals_handler);

    parse_actions ();

    /* Create requirements for async options */
    cancellable = g_cancellable_new ();
    loop = g_main_loop_new (NULL, FALSE);

    /* Launch QmiDevice creation */
    qmi_device_new (file,
                    cancellable,
                    (GAsyncReadyCallback)device_new_ready,
                    GUINT_TO_POINTER (service));
    g_main_loop_run (loop);

    if (cancellable)
        g_object_unref (cancellable);
    if (client)
        g_object_unref (client);
    if (device)
        g_object_unref (device);
    g_main_loop_unref (loop);
    g_object_unref (file);

    return (operation_status ? EXIT_SUCCESS : EXIT_FAILURE);
}
