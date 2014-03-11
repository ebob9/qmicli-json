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
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include <libqmi-glib.h>

#include "qmicli.h"
#include "qmicli-helpers.h"

/* Context */
typedef struct {
    QmiDevice *device;
    QmiClientNas *client;
    GCancellable *cancellable;
} Context;
static Context *ctx;

/* Options */
static gboolean get_signal_strength_flag;
static gboolean get_signal_info_flag;
static gchar *get_tx_rx_info_str;
static gboolean get_home_network_flag;
static gboolean get_serving_system_flag;
static gboolean get_system_info_flag;
static gboolean get_technology_preference_flag;
static gboolean get_system_selection_preference_flag;
static gchar *set_system_selection_preference_str;
static gboolean network_scan_flag;
static gboolean reset_flag;
static gboolean noop_flag;

static GOptionEntry entries[] = {
    { "nas-get-signal-strength", 0, 0, G_OPTION_ARG_NONE, &get_signal_strength_flag,
      "Get signal strength",
      NULL
    },
    { "nas-get-signal-info", 0, 0, G_OPTION_ARG_NONE, &get_signal_info_flag,
      "Get signal info",
      NULL
    },
    { "nas-get-tx-rx-info", 0, 0, G_OPTION_ARG_STRING, &get_tx_rx_info_str,
      "Get TX/RX info",
      "[(Radio Interface)]",
    },
    { "nas-get-home-network", 0, 0, G_OPTION_ARG_NONE, &get_home_network_flag,
      "Get home network",
      NULL
    },
    { "nas-get-serving-system", 0, 0, G_OPTION_ARG_NONE, &get_serving_system_flag,
      "Get serving system",
      NULL
    },
    { "nas-get-system-info", 0, 0, G_OPTION_ARG_NONE, &get_system_info_flag,
      "Get system info",
      NULL
    },
    { "nas-get-technology-preference", 0, 0, G_OPTION_ARG_NONE, &get_technology_preference_flag,
      "Get technology preference",
      NULL
    },
    { "nas-get-system-selection-preference", 0, 0, G_OPTION_ARG_NONE, &get_system_selection_preference_flag,
      "Get system selection preference",
      NULL
    },
    { "nas-set-system-selection-preference", 0, 0, G_OPTION_ARG_STRING, &set_system_selection_preference_str,
      "Set system selection preference",
      "[cdma-1x|cdma-1xevdo|gsm|umts|lte|td-scdma]"
    },
    { "nas-network-scan", 0, 0, G_OPTION_ARG_NONE, &network_scan_flag,
      "Scan networks",
      NULL
    },
    { "nas-reset", 0, 0, G_OPTION_ARG_NONE, &reset_flag,
      "Reset the service state",
      NULL
    },
    { "nas-noop", 0, 0, G_OPTION_ARG_NONE, &noop_flag,
      "Just allocate or release a NAS client. Use with `--client-no-release-cid' and/or `--client-cid'",
      NULL
    },
    { NULL }
};

GOptionGroup *
qmicli_nas_get_option_group (void)
{
        GOptionGroup *group;

        group = g_option_group_new ("nas",
                                    "NAS options",
                                    "Show Network Access Service options",
                                    NULL,
                                    NULL);
        g_option_group_add_entries (group, entries);

        return group;
}

gboolean
qmicli_nas_options_enabled (void)
{
    static guint n_actions = 0;
    static gboolean checked = FALSE;

    if (checked)
        return !!n_actions;

    n_actions = (get_signal_strength_flag +
                 get_signal_info_flag +
                 !!get_tx_rx_info_str +
                 get_home_network_flag +
                 get_serving_system_flag +
                 get_system_info_flag +
                 get_technology_preference_flag +
                 get_system_selection_preference_flag +
                 !!set_system_selection_preference_str +
                 network_scan_flag +
                 reset_flag +
                 noop_flag);

    if (n_actions > 1) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", "too many NAS actions requested"
              ),json_print_flag));
        exit (EXIT_FAILURE);
    }

    checked = TRUE;
    return !!n_actions;
}

static void
context_free (Context *context)
{
    if (!context)
        return;

    if (context->cancellable)
        g_object_unref (context->cancellable);
    if (context->device)
        g_object_unref (context->device);
    if (context->client)
        g_object_unref (context->client);
    g_slice_free (Context, context);
}

static void
shutdown (gboolean operation_status)
{
    /* Cleanup context and finish async operation */
    context_free (ctx);
    qmicli_async_operation_done (operation_status);
}

static gdouble
get_db_from_sinr_level (QmiNasEvdoSinrLevel level)
{
    switch (level) {
    case QMI_NAS_EVDO_SINR_LEVEL_0: return -9.0;
    case QMI_NAS_EVDO_SINR_LEVEL_1: return -6;
    case QMI_NAS_EVDO_SINR_LEVEL_2: return -4.5;
    case QMI_NAS_EVDO_SINR_LEVEL_3: return -3;
    case QMI_NAS_EVDO_SINR_LEVEL_4: return -2;
    case QMI_NAS_EVDO_SINR_LEVEL_5: return 1;
    case QMI_NAS_EVDO_SINR_LEVEL_6: return 3;
    case QMI_NAS_EVDO_SINR_LEVEL_7: return 6;
    case QMI_NAS_EVDO_SINR_LEVEL_8: return +9;
    default:
        g_warning ("Invalid SINR level '%u'", level);
        return -G_MAXDOUBLE;
    }
}

static void
get_signal_info_ready (QmiClientNas *client,
                       GAsyncResult *res)
{
    json_t *json_output;
    QmiMessageNasGetSignalInfoOutput *output;
    GError *error = NULL;
    gint8 rssi;
    gint16 ecio;
    QmiNasEvdoSinrLevel sinr_level;
    gint32 io;
    gint8 rsrq;
    gint16 rsrp;
    gint16 snr;
    gint8 rscp;

    output = qmi_client_nas_get_signal_info_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_signal_info_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get signal info",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_signal_info_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );

    /* CDMA... */
    if (qmi_message_nas_get_signal_info_output_get_cdma_signal_strength (output,
                                                                         &rssi,
                                                                         &ecio,
                                                                         NULL)) {

       json_object_update(json_output, json_pack("{s{sisf}}",
            "cdma",
                 "rssi", rssi,
                 "ecio", (-0.5)*((gdouble)ecio))
            );
    }

    /* HDR... */
    if (qmi_message_nas_get_signal_info_output_get_hdr_signal_strength (output,
                                                                        &rssi,
                                                                        &ecio,
                                                                        &sinr_level,
                                                                        &io,
                                                                        NULL)) {

       json_object_update(json_output, json_pack("{s{sisfs{sisf}si}}",
            "hdr",
                 "rssi", rssi,
                 "ecio", (-0.5)*((gdouble)ecio),
                 "sinr",
                        "level", sinr_level,
                        "db", get_db_from_sinr_level (sinr_level),
                 "io", io
            ));
    }

    /* GSM */
    if (qmi_message_nas_get_signal_info_output_get_gsm_signal_strength (output,
                                                                        &rssi,
                                                                        NULL)) {
       json_object_update(json_output, json_pack("{s{si}}",
            "gsm",
                 "rssi", rssi
            ));
    }

    /* WCDMA... */
    if (qmi_message_nas_get_signal_info_output_get_wcdma_signal_strength (output,
                                                                          &rssi,
                                                                          &ecio,
                                                                          NULL)) {

       json_object_update(json_output, json_pack("{s{sisf}}",
            "wcdma",
                 "rssi", rssi,
                 "ecio", (-0.5)*((gdouble)ecio)
            ));
    }

    /* LTE... */
    if (qmi_message_nas_get_signal_info_output_get_lte_signal_strength (output,
                                                                        &rssi,
                                                                        &rsrq,
                                                                        &rsrp,
                                                                        &snr,
                                                                        NULL)) {

       json_object_update(json_output, json_pack("{s{sisisisf}}",
            "lte",
                 "rssi", rssi,
                 "rsrq", rsrq,
                 "rsrp", rsrp,
                 "snr",  (0.1) * ((gdouble)snr)
            ));
    }

    /* TDMA */
    if (qmi_message_nas_get_signal_info_output_get_tdma_signal_strength (output,
                                                                         &rscp,
                                                                         NULL)) {
       json_object_update(json_output, json_pack("{s{si}}",
            "tdma",
                 "rscp", rscp
            ));
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_signal_info_output_unref (output);
    shutdown (TRUE);
}

static QmiMessageNasGetSignalStrengthInput *
get_signal_strength_input_create (void)
{
    GError *error = NULL;
    QmiMessageNasGetSignalStrengthInput *input;
    QmiNasSignalStrengthRequest mask;

    mask = (QMI_NAS_SIGNAL_STRENGTH_REQUEST_RSSI |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_ECIO |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_IO |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_SINR |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_RSRQ |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_LTE_SNR |
            QMI_NAS_SIGNAL_STRENGTH_REQUEST_LTE_RSRP);

    input = qmi_message_nas_get_signal_strength_input_new ();
    if (!qmi_message_nas_get_signal_strength_input_set_request_mask (
            input,
            mask,
            &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't create input data bundle",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_signal_strength_input_unref (input);
        input = NULL;
    }

    return input;
}

static void
get_signal_strength_ready (QmiClientNas *client,
                           GAsyncResult *res)
{
    json_t *json_output;
    QmiMessageNasGetSignalStrengthOutput *output;
    GError *error = NULL;
    GArray *array;
    QmiNasRadioInterface radio_interface;
    gint8 strength;
    gint32 io;
    QmiNasEvdoSinrLevel sinr_level;
    gint8 rsrq;
    gint16 rsrp;
    gint16 snr;

    output = qmi_client_nas_get_signal_strength_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_signal_strength_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get signal strength",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_signal_strength_output_unref (output);
        shutdown (FALSE);
        return;
    }

    qmi_message_nas_get_signal_strength_output_get_signal_strength (output,
                                                                    &strength,
                                                                    &radio_interface,
                                                                    NULL);


    json_output = json_pack("{sbsss{sssi}}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device),
             "current",
                      "network", qmi_nas_radio_interface_get_string (radio_interface),
                      "dbm", strength
              );

    /* Other signal strengths in other networks... */
    if (qmi_message_nas_get_signal_strength_output_get_strength_list (output, &array, NULL)) {
        guint i;

        json_object_update(json_output, json_pack("{s{}}",
            "other"
            ));

        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputStrengthListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputStrengthListElement, i);
           json_array_append(json_object_get(json_output,"other"),json_pack("{si}",
                     qmi_nas_radio_interface_get_string (element->radio_interface),
                     element->strength
           ));

        }
    }

    /* RSSI... */
    if (qmi_message_nas_get_signal_strength_output_get_rssi_list (output, &array, NULL)) {
        guint i;

        json_object_update(json_output, json_pack("{s{}}",
             "rssi"
             ));

        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputRssiListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputRssiListElement, i);
            json_object_update(json_object_get(json_output,"rssi"),json_pack("{si}",
                     qmi_nas_radio_interface_get_string (element->radio_interface),
                     (-1) * element->rssi
           ));
        }
    }

    /* ECIO... */
    if (qmi_message_nas_get_signal_strength_output_get_ecio_list (output, &array, NULL)) {
        guint i;

        json_object_update(json_output, json_pack("{s{}}",
             "ecio"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessageNasGetSignalStrengthOutputEcioListElement *element;

            element = &g_array_index (array, QmiMessageNasGetSignalStrengthOutputEcioListElement, i);
            json_object_update(json_object_get(json_output,"ecio"),json_pack("{sf}",
                     qmi_nas_radio_interface_get_string (element->radio_interface),
                     (-0.5) * ((gdouble)element->ecio)
           ));
        }
    }

    /* IO... */
    if (qmi_message_nas_get_signal_strength_output_get_io (output, &io, NULL)) {
        json_object_update(json_output, json_pack("{si}",
             "io", io
             ));
    }

    /* SINR level */
    if (qmi_message_nas_get_signal_strength_output_get_sinr (output, &sinr_level, NULL)) {
        json_object_update(json_output, json_pack("{s{sisf}}",
                 "sinr",
                        "level", sinr_level,
                        "db", get_db_from_sinr_level (sinr_level)
             ));
    }

    /* RSRQ */
    if (qmi_message_nas_get_signal_strength_output_get_rsrq (output, &rsrq, &radio_interface, NULL)) {
        json_object_update(json_output, json_pack("{s{si}}",
                 "rsrq",
                        qmi_nas_radio_interface_get_string (radio_interface),
                        rsrq
             ));
    }

    /* LTE SNR */
    if (qmi_message_nas_get_signal_strength_output_get_lte_snr (output, &snr, NULL)) {
        json_object_update(json_output, json_pack("{s{sf}}",
                 "snr",
                        qmi_nas_radio_interface_get_string (QMI_NAS_RADIO_INTERFACE_LTE),
                        (0.1) * ((gdouble)snr)
             ));
    }

    /* LTE RSRP */
    if (qmi_message_nas_get_signal_strength_output_get_lte_rsrp (output, &rsrp, NULL)) {
        json_object_update(json_output, json_pack("{s{si}}",
                 "rsrp",
                        qmi_nas_radio_interface_get_string (QMI_NAS_RADIO_INTERFACE_LTE),
                        rsrp
             ));
    }

    /* Just skip others for now */

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_signal_strength_output_unref (output);
    shutdown (TRUE);
}

static void
get_tx_rx_info_ready (QmiClientNas *client,
                      GAsyncResult *res,
                      gpointer user_data)
{
    QmiNasRadioInterface interface;
    QmiMessageNasGetTxRxInfoOutput *output;
    GError *error = NULL;
    gboolean is_radio_tuned;
    gboolean is_in_traffic;
    gint32 power;
    gint32 ecio;
    gint32 rscp;
    gint32 rsrp;
    guint32 phase;
    json_t *json_output;

    interface = GPOINTER_TO_UINT (user_data);

    output = qmi_client_nas_get_tx_rx_info_finish (client, res, &error);
    if (!output) {
        //g_printerr ("error: operation failed: %s\n", error->message);
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_tx_rx_info_output_get_result (output, &error)) {
        //g_printerr ("error: couldn't get TX/RX info: %s\n", error->message);
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get TX/RX info",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_tx_rx_info_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );

    /* RX Channel 0 */
    if (qmi_message_nas_get_tx_rx_info_output_get_rx_chain_0_info (
            output,
            &is_radio_tuned,
            &power,
            &ecio,
            &rscp,
            &rsrp,
            &phase,
            NULL)) {
           json_object_update(json_output, json_pack("{s{sbsf}}",
                 "rx chain 0",
                             "radio tuned", is_radio_tuned,
                             "power", (0.1) * ((gdouble)power)
                 ));


        if (interface == QMI_NAS_RADIO_INTERFACE_CDMA_1X ||
            interface == QMI_NAS_RADIO_INTERFACE_CDMA_1XEVDO ||
            interface == QMI_NAS_RADIO_INTERFACE_GSM ||
            interface == QMI_NAS_RADIO_INTERFACE_UMTS ||
            interface == QMI_NAS_RADIO_INTERFACE_LTE) {
            json_object_update(json_object_get(json_output,"rx chain 0"), json_pack("{sf}",
                "ecio", (0.1) * ((gdouble)ecio)
                 ));
           }

        if (interface == QMI_NAS_RADIO_INTERFACE_UMTS) {
            //g_print ("\tRSCP: '%.1lf dBm'\n", (0.1) * ((gdouble)rscp));
            json_object_update(json_object_get(json_output,"rx chain 0"), json_pack("{sf}",
                "rscp", (0.1) * ((gdouble)rscp)
                 ));
           }

        if (interface == QMI_NAS_RADIO_INTERFACE_LTE) {
            json_object_update(json_object_get(json_output,"rx chain 0"), json_pack("{sf}",
                "rsrp", (0.1) * ((gdouble)rsrp)
                 ));
           }

        if (interface == QMI_NAS_RADIO_INTERFACE_LTE) {
            if (phase == 0xFFFFFFFF) {
                //g_print ("\tPhase: 'unknown'\n");
                json_object_update(json_object_get(json_output,"rx chain 0"), json_pack("{ss}",
                "phase", "unknown"
                 ));
            }
            else {
                json_object_update(json_object_get(json_output,"rx chain 0"), json_pack("{sf}",
                "phase", (0.01) * ((gdouble)phase)
                 ));
            }
        }
    }

    /* RX Channel 1 */
    if (qmi_message_nas_get_tx_rx_info_output_get_rx_chain_1_info (
            output,
            &is_radio_tuned,
            &power,
            &ecio,
            &rscp,
            &rsrp,
            &phase,
            NULL)) {
           json_object_update(json_output, json_pack("{s{sbsf}}",
                 "rx chain 1",
                             "radio tuned", is_radio_tuned,
                             "power", (0.1) * ((gdouble)power)
                 ));
        if (interface == QMI_NAS_RADIO_INTERFACE_CDMA_1X ||
            interface == QMI_NAS_RADIO_INTERFACE_CDMA_1XEVDO ||
            interface == QMI_NAS_RADIO_INTERFACE_GSM ||
            interface == QMI_NAS_RADIO_INTERFACE_UMTS ||
            interface == QMI_NAS_RADIO_INTERFACE_LTE) {
            json_object_update(json_object_get(json_output,"rx chain 1"), json_pack("{sf}",
                "ecio", (0.1) * ((gdouble)ecio)
                 ));
           }

        if (interface == QMI_NAS_RADIO_INTERFACE_UMTS) {
            //g_print ("\tRSCP: '%.1lf dBm'\n", (0.1) * ((gdouble)rscp));
            json_object_update(json_object_get(json_output,"rx chain 1"), json_pack("{sf}",
                "rscp", (0.1) * ((gdouble)rscp)
                 ));
           }

        if (interface == QMI_NAS_RADIO_INTERFACE_LTE) {
            json_object_update(json_object_get(json_output,"rx chain 1"), json_pack("{sf}",
                "rsrp", (0.1) * ((gdouble)rsrp)
                 ));
           }

        if (interface == QMI_NAS_RADIO_INTERFACE_LTE) {
            if (phase == 0xFFFFFFFF) {
                //g_print ("\tPhase: 'unknown'\n");
                json_object_update(json_object_get(json_output,"rx chain 1"), json_pack("{ss}",
                "phase", "unknown"
                 ));
            }
            else {
                json_object_update(json_object_get(json_output,"rx chain 1"), json_pack("{sf}",
                "phase", (0.01) * ((gdouble)phase)
                 ));
            }
        }
    }

    /* TX Channel */
    if (qmi_message_nas_get_tx_rx_info_output_get_tx_info (
            output,
            &is_in_traffic,
            &power,
            NULL)) {
        if (is_in_traffic) {
            json_object_update(json_output, json_pack("{s{sbsf}}",
                 "tx",
                     "in traffic", 1,
                     "power", (0.1) * ((gdouble)power)
                 ));
            }
        else {
            //g_print ("\tIn traffic: 'no'\n");
            json_object_update(json_output, json_pack("{s{sb}}",
                 "tx",
                     "in traffic", 0
                 ));
        }
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_tx_rx_info_output_unref (output);
    shutdown (TRUE);
}

static QmiMessageNasGetTxRxInfoInput *
get_tx_rx_info_input_create (const gchar *str,
                             QmiNasRadioInterface *interface)
{
    QmiMessageNasGetTxRxInfoInput *input = NULL;

    g_assert (interface != NULL);

    if (qmicli_read_radio_interface_from_string (str, interface)) {
        GError *error = NULL;

        input = qmi_message_nas_get_tx_rx_info_input_new ();
        if (!qmi_message_nas_get_tx_rx_info_input_set_radio_interface (
                input,
                *interface,
                &error)) {
            /* g_printerr ("error: couldn't create input data bundle: '%s'\n",
                        error->message); */
             g_print ("%s\n", json_dumps(json_pack("{sbssss}",
                 "success", 0,
                 "error", "couldn't create input data bundle",
                 "message", error->message
                 ),json_print_flag));
            g_error_free (error);
            qmi_message_nas_get_tx_rx_info_input_unref (input);
            input = NULL;
        }
    }

    return input;
}

static void
get_home_network_ready (QmiClientNas *client,
                        GAsyncResult *res)
{
    QmiMessageNasGetHomeNetworkOutput *output;
    GError *error = NULL;
    json_t *json_output;

    output = qmi_client_nas_get_home_network_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_home_network_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get home network",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_home_network_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );

    {
        guint16 mcc;
        guint16 mnc;
        const gchar *description;

        qmi_message_nas_get_home_network_output_get_home_network (
            output,
            &mcc,
            &mnc,
            &description,
            NULL);

        json_object_update(json_output, json_pack("{s{sisiss}}",
                 "home network",
                               "mcc", mcc,
                               "mnc", mnc,
                               "description", description
                 ));
    }

    {
        guint16 sid;
        guint16 nid;

        if (qmi_message_nas_get_home_network_output_get_home_system_id (
                output,
                &sid,
                &nid,
                NULL)) {
           json_object_update(json_object_get(json_output, "home network"), json_pack("{sisi}",
                     "sid", sid,
                     "nid", nid
                     ));
        }
    }

    {
        guint16 mcc;
        guint16 mnc;

        if (qmi_message_nas_get_home_network_output_get_home_network_3gpp2 (
                output,
                &mcc,
                &mnc,
                NULL, /* display_description */
                NULL, /* description_encoding */
                NULL, /* description */
                NULL)) {
           json_object_update(json_output, json_pack("{s{sisisn}}",
                     "3gpp2 home network",
                               "mcc", mcc,
                               "mnc", mnc,
                               "description", NULL
                     ));

            /* TODO: convert description to UTF-8 and display */
        }
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_home_network_output_unref (output);
    shutdown (TRUE);
}

static void
get_serving_system_ready (QmiClientNas *client,
                          GAsyncResult *res)
{
    QmiMessageNasGetServingSystemOutput *output;
    GError *error = NULL;
    json_t *json_output;

    output = qmi_client_nas_get_serving_system_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_serving_system_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get serving system",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_serving_system_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );

    {
        QmiNasRegistrationState registration_state;
        QmiNasAttachState cs_attach_state;
        QmiNasAttachState ps_attach_state;
        QmiNasNetworkType selected_network;
        GArray *radio_interfaces;
        guint i;

        qmi_message_nas_get_serving_system_output_get_serving_system (
            output,
            &registration_state,
            &cs_attach_state,
            &ps_attach_state,
            &selected_network,
            &radio_interfaces,
            NULL);

                 /* Seperate calls to maintain hashtable order
                 for human readability*/
        json_object_update(json_output, json_pack("{ss}",
                 "registration state", qmi_nas_registration_state_get_string (registration_state)
                 ));
        json_object_update(json_output, json_pack("{ss}",
                 "cs", qmi_nas_registration_state_get_string (cs_attach_state)
                 ));
        json_object_update(json_output, json_pack("{ss}",
                 "ps", qmi_nas_registration_state_get_string (ps_attach_state)
                 ));
        json_object_update(json_output, json_pack("{ss}",
                 "selected network", qmi_nas_network_type_get_string (selected_network)
                 ));
        json_object_update(json_output, json_pack("{s[]}",
                 "radio interfaces"
                 ));

        for (i = 0; i < radio_interfaces->len; i++) {
            QmiNasRadioInterface iface;

            iface = g_array_index (radio_interfaces, QmiNasRadioInterface, i);
            json_array_append(json_object_get(json_output,"radio interfaces"),json_pack("s", qmi_nas_radio_interface_get_string (iface)));
        }
    }

    {
        QmiNasRoamingIndicatorStatus roaming;

        if (qmi_message_nas_get_serving_system_output_get_roaming_indicator (
                output,
                &roaming,
                NULL)) {
            json_object_update(json_output, json_pack("{ss}",
                 "roaming status", qmi_nas_roaming_indicator_status_get_string (roaming)
                 ));
        }
    }

    {
        GArray *data_service_capability;

        if (qmi_message_nas_get_serving_system_output_get_data_service_capability (
                output,
                &data_service_capability,
                NULL)) {
            guint i;

            json_object_update(json_output, json_pack("{s[]}",
                 "data service capabilites"
                 ));

            for (i = 0; i < data_service_capability->len; i++) {
                QmiNasDataCapability cap;

                cap = g_array_index (data_service_capability, QmiNasDataCapability, i);
                json_array_append(json_object_get(json_output,"data service capabilites"),json_pack("s", qmi_nas_data_capability_get_string (cap)));
            }
        }
    }

    {
        guint16 current_plmn_mcc;
        guint16 current_plmn_mnc;
        const gchar *current_plmn_description;

        if (qmi_message_nas_get_serving_system_output_get_current_plmn (
                output,
                &current_plmn_mcc,
                &current_plmn_mnc,
                &current_plmn_description,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sisiss}}",
                 "current plmn",
                                "mcc", current_plmn_mcc,
                                "mnc", current_plmn_mnc,
                                "description", current_plmn_description
                 ));
        }
    }

    {
        guint16 sid;
        guint16 nid;

        if (qmi_message_nas_get_serving_system_output_get_cdma_system_id (
                output,
                &sid,
                &nid,
                NULL)) {
            json_object_update(json_object_get(json_output,"current plmn"),json_pack("{sisi}",
                "sid", sid,
                "nid", nid
                ));

        }
    }

    {
        guint16 id;
        gint32 latitude;
        gint32 longitude;

        if (qmi_message_nas_get_serving_system_output_get_cdma_base_station_info (
                output,
                &id,
                &latitude,
                &longitude,
                NULL)) {
            gdouble latitude_degrees;
            gdouble longitude_degrees;

            /* TODO: give degrees, minutes, seconds */
            latitude_degrees = ((gdouble)latitude * 0.25)/3600.0;
            longitude_degrees = ((gdouble)longitude * 0.25)/3600.0;

            json_object_update(json_output, json_pack("{s{sisfsf}}",
                 "cdma base station info",
                        "base station id", id,
                        "latitude", latitude_degrees,
                        "longitude", longitude_degrees
                 ));
        }
    }

    {
        GArray *roaming_indicators;

        if (qmi_message_nas_get_serving_system_output_get_roaming_indicator_list (
                output,
                &roaming_indicators,
                NULL)) {
            guint i;

            json_object_update(json_output, json_pack("{s{}}",
                 "roaming indicators"
                 ));

            for (i = 0; i < roaming_indicators->len; i++) {
                QmiMessageNasGetServingSystemOutputRoamingIndicatorListElement *element;

                element = &g_array_index (roaming_indicators, QmiMessageNasGetServingSystemOutputRoamingIndicatorListElement, i);
                json_object_update(json_object_get(json_output,"roaming indicators"),json_pack("{ss}",
                        qmi_nas_radio_interface_get_string (element->radio_interface), qmi_nas_roaming_indicator_status_get_string (element->roaming_indicator)
                        ));

            }
        }
    }

    {
        QmiNasRoamingIndicatorStatus roaming;

        if (qmi_message_nas_get_serving_system_output_get_default_roaming_indicator (
                output,
                &roaming,
                NULL)) {
            json_object_update(json_output, json_pack("{ss}",
                 "default roaming status", qmi_nas_roaming_indicator_status_get_string (roaming)
                 ));
        }
    }

    {
        guint8 leap_seconds;
        gint8 local_time_offset;
        gboolean daylight_saving_time;

        if (qmi_message_nas_get_serving_system_output_get_time_zone_3gpp2 (
                output,
                &leap_seconds,
                &local_time_offset,
                &daylight_saving_time,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sisisb}",
                 "3gpp2 time zone",
                                "leap seconds", leap_seconds,
                                "local time offset", (gint)local_time_offset * 30,
                                "daylight savings time", daylight_saving_time
                 ));

        }
    }

    {
        guint8 cdma_p_rev;

        if (qmi_message_nas_get_serving_system_output_get_cdma_p_rev (
                output,
                &cdma_p_rev,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "cdma p_rev", cdma_p_rev
                 ));
        }
    }

    {
        gint8 time_zone;

        if (qmi_message_nas_get_serving_system_output_get_time_zone_3gpp (
                output,
                &time_zone,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "3gpp time zone offset", (gint)time_zone * 15
                 ));
        }
    }

    {
        guint8 adjustment;

        if (qmi_message_nas_get_serving_system_output_get_daylight_saving_time_adjustment_3gpp (
                output,
                &adjustment,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "3gpp daylight savings time adjustment", adjustment
                 ));
        }
    }

    {
        guint16 lac;

        if (qmi_message_nas_get_serving_system_output_get_lac_3gpp (
                output,
                &lac,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "3gpp location area code", lac
                 ));
        }
    }

    {
        guint32 cid;

        if (qmi_message_nas_get_serving_system_output_get_cid_3gpp (
                output,
                &cid,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "3gpp cell id", cid
                 ));
        }
    }

    {
        gboolean concurrent;

        if (qmi_message_nas_get_serving_system_output_get_concurrent_service_info_3gpp2 (
                output,
                &concurrent,
                NULL)) {
            json_object_update(json_output, json_pack("{sb}",
                 "3gpp2 concurrent service info", concurrent
                 ));
        }
    }

    {
        gboolean prl;

        if (qmi_message_nas_get_serving_system_output_get_prl_indicator_3gpp2 (
                output,
                &prl,
                NULL)) {
            json_object_update(json_output, json_pack("{sb}",
                 "3gpp2 prl indicator", prl
                 ));
        }
    }

    {
        gboolean supported;

        if (qmi_message_nas_get_serving_system_output_get_dtm_support (
                output,
                &supported,
                NULL)) {
            json_object_update(json_output, json_pack("{sb}",
                 "dual transfer mode", supported
                 ));
        }
    }

    {
        QmiNasServiceStatus status;
        QmiNasNetworkServiceDomain capability;
        QmiNasServiceStatus hdr_status;
        gboolean hdr_hybrid;
        gboolean forbidden;

        if (qmi_message_nas_get_serving_system_output_get_detailed_service_status (
                output,
                &status,
                &capability,
                &hdr_status,
                &hdr_hybrid,
                &forbidden,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssssssbsb}}",
                 "detailed status",
                                "status", qmi_nas_service_status_get_string (status),
                                "capability", qmi_nas_network_service_domain_get_string (capability),
                                "hdr status", qmi_nas_service_status_get_string (hdr_status),
                                "hdr hybrid", hdr_hybrid,
                                "forbidden", forbidden
                 ));
        }
    }

    {
        guint16 mcc;
        guint8 imsi_11_12;

        if (qmi_message_nas_get_serving_system_output_get_cdma_system_info (
                output,
                &mcc,
                &imsi_11_12,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sisi}}",
                 "cdma system info",
                                "mcc", mcc,
                                "imsi_11_12", imsi_11_12
                 ));
        }
    }

    {
        QmiNasHdrPersonality personality;

        if (qmi_message_nas_get_serving_system_output_get_hdr_personality (
                output,
                &personality,
                NULL)) {
            json_object_update(json_output, json_pack("{ss}",
                 "hdr personality", qmi_nas_hdr_personality_get_string (personality)
                 ));
        }
    }

    {
        guint16 tac;

        if (qmi_message_nas_get_serving_system_output_get_lte_tac (
                output,
                &tac,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "lte tracking area code", tac
                 ));
        }
    }

    {
        QmiNasCallBarringStatus cs_status;
        QmiNasCallBarringStatus ps_status;

        if (qmi_message_nas_get_serving_system_output_get_call_barring_status (
                output,
                &cs_status,
                &ps_status,
                NULL)) {
            json_object_update(json_output, json_pack("{s{ssss}}",
                 "call barring status",
                                "circuit switched", qmi_nas_call_barring_status_get_string (cs_status),
                                "packet switched", qmi_nas_call_barring_status_get_string (ps_status)
                 ));
        }
    }

    {
        guint16 code;

        if (qmi_message_nas_get_serving_system_output_get_umts_primary_scrambling_code (
                output,
                &code,
                NULL)) {
            json_object_update(json_output, json_pack("{si}",
                 "utms primary scrambling code", code
                 ));
        }
    }

    {
        guint16 mcc;
        guint16 mnc;
        gboolean has_pcs_digit;

        if (qmi_message_nas_get_serving_system_output_get_mnc_pcs_digit_include_status (
                output,
                &mcc,
                &mnc,
                &has_pcs_digit,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sisisb}}",
                 "full operator code info",
                                "mcc", mcc,
                                "mnc", mnc,
                                "mnc with pcs digit", has_pcs_digit
                 ));
        }
    }

    g_print ("%s\n", json_dumps(json_output, json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_serving_system_output_unref (output);
    shutdown (TRUE);
}

static void
get_system_info_ready (QmiClientNas *client,
                       GAsyncResult *res)
{
    QmiMessageNasGetSystemInfoOutput *output;
    GError *error = NULL;
    json_t *json_output;

    output = qmi_client_nas_get_system_info_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_system_info_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get system info",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_get_system_info_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );

    /* CDMA 1x */
    {
        QmiNasServiceStatus service_status;
        gboolean preferred_data_path;
        gboolean domain_valid;
        QmiNasNetworkServiceDomain domain;
        gboolean service_capability_valid;
        QmiNasNetworkServiceDomain service_capability;
        gboolean roaming_status_valid;
        QmiNasRoamingStatus roaming_status;
        gboolean forbidden_valid;
        gboolean forbidden;
        gboolean prl_match_valid;
        gboolean prl_match;
        gboolean p_rev_valid;
        guint8 p_rev;
        gboolean base_station_p_rev_valid;
        guint8 base_station_p_rev;
        gboolean concurrent_service_support_valid;
        gboolean concurrent_service_support;
        gboolean cdma_system_id_valid;
        guint16 sid;
        guint16 nid;
        gboolean base_station_info_valid;
        guint16 base_station_id;
        gint32 base_station_latitude;
        gint32 base_station_longitude;
        gboolean packet_zone_valid;
        guint16 packet_zone;
        gboolean network_id_valid;
        const gchar *mcc;
        const gchar *mnc;
        guint16 geo_system_index;
        guint16 registration_period;

        if (qmi_message_nas_get_system_info_output_get_cdma_service_status (
                output,
                &service_status,
                &preferred_data_path,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssb}}",
                 "cdma 1x service",
                                "status", qmi_nas_service_status_get_string (service_status),
                                "preferred data path", preferred_data_path
                 ));

            if (qmi_message_nas_get_system_info_output_get_cdma_system_info (
                    output,
                    &domain_valid, &domain,
                    &service_capability_valid, &service_capability,
                    &roaming_status_valid, &roaming_status,
                    &forbidden_valid, &forbidden,
                    &prl_match_valid, &prl_match,
                    &p_rev_valid, &p_rev,
                    &base_station_p_rev_valid, &base_station_p_rev,
                    &concurrent_service_support_valid, &concurrent_service_support,
                    &cdma_system_id_valid, &sid, &nid,
                    &base_station_info_valid, &base_station_id, &base_station_longitude, &base_station_latitude,
                    &packet_zone_valid, &packet_zone,
                    &network_id_valid, &mcc, &mnc,
                    NULL)) {
                if (domain_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{ss}",
                                "domain", qmi_nas_network_service_domain_get_string (domain)
                                ));
                }

                if (service_capability_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{ss}",
                                "service capability", qmi_nas_network_service_domain_get_string (service_capability)
                                ));
                }

                if (roaming_status_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{ss}",
                                "roaming status", qmi_nas_network_service_domain_get_string (roaming_status)
                                ));
                }

                if (forbidden_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{sb}",
                                "forbidden", forbidden
                                ));
                }

                if (prl_match_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{sb}",
                                "prl match", prl_match
                                ));
                }

                if (p_rev_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{si}",
                                "p-rev", p_rev
                                ));
                }

                if (base_station_p_rev_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{si}",
                                "base station p-rev", base_station_p_rev
                                ));
                }

                if (concurrent_service_support_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{sb}",
                                "concurrent service support", concurrent_service_support
                                ));
                }

                if (cdma_system_id_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{sisi}",
                                "sid", sid,
                                "nid", nid
                                ));
                }

                if (base_station_info_valid) {
                    gdouble latitude_degrees;
                    gdouble longitude_degrees;

                    /* TODO: give degrees, minutes, seconds */
                    latitude_degrees = ((gdouble)base_station_latitude * 0.25)/3600.0;
                    longitude_degrees = ((gdouble)base_station_longitude * 0.25)/3600.0;
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{sisfsf}",
                                "base station id", base_station_id,
                                "base station latitude", latitude_degrees,
                                "base station longitude", longitude_degrees
                                ));
                }

                if (packet_zone_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{si}",
                                "packet zone", packet_zone
                                ));
                }

                if (network_id_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{ssss}",
                                "mcc", mcc,
                                "mnc", mnc
                                ));
                }
            }

            if (qmi_message_nas_get_system_info_output_get_additional_cdma_system_info (
                    output,
                    &geo_system_index,
                    &registration_period,
                    NULL)) {
                if (geo_system_index != 0xFFFF) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{si}",
                                "geo system index", geo_system_index
                                ));
                }
                if (registration_period != 0xFFFF) {
                    json_object_update(json_object_get(json_output,"cdma 1x service"),json_pack("{si}",
                                "registration period", registration_period
                                ));
                }
            }
        }
    }

    /* CDMA 1xEV-DO */
    {
        QmiNasServiceStatus service_status;
        gboolean preferred_data_path;
        gboolean domain_valid;
        QmiNasNetworkServiceDomain domain;
        gboolean service_capability_valid;
        QmiNasNetworkServiceDomain service_capability;
        gboolean roaming_status_valid;
        QmiNasRoamingStatus roaming_status;
        gboolean forbidden_valid;
        gboolean forbidden;
        gboolean prl_match_valid;
        gboolean prl_match;
        gboolean personality_valid;
        QmiNasHdrPersonality personality;
        gboolean protocol_revision_valid;
        QmiNasHdrProtocolRevision protocol_revision;
        gboolean is_856_system_id_valid;
        const gchar *is_856_system_id;
        guint16 geo_system_index;

        if (qmi_message_nas_get_system_info_output_get_hdr_service_status (
                output,
                &service_status,
                &preferred_data_path,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssb}}",
                 "cdma 1xev-do service",
                                "status", qmi_nas_service_status_get_string (service_status),
                                "preferred data path", preferred_data_path
                 ));

            if (qmi_message_nas_get_system_info_output_get_hdr_system_info (
                    output,
                    &domain_valid, &domain,
                    &service_capability_valid, &service_capability,
                    &roaming_status_valid, &roaming_status,
                    &forbidden_valid, &forbidden,
                    &prl_match_valid, &prl_match,
                    &personality_valid, &personality,
                    &protocol_revision_valid, &protocol_revision,
                    &is_856_system_id_valid, &is_856_system_id,
                    NULL)) {
                if (domain_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{ss}",
                                "domain", qmi_nas_network_service_domain_get_string (domain)
                                ));
                }

                if (service_capability_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{ss}",
                                "service capability", qmi_nas_network_service_domain_get_string (service_capability)
                                ));
                }

                if (roaming_status_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{ss}",
                                "roaming status", qmi_nas_network_service_domain_get_string (roaming_status)
                                ));
                }

                if (forbidden_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{sb}",
                                "forbidden", forbidden
                                ));
                }

                if (prl_match_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{sb}",
                                "prl match", prl_match
                                ));
                }

                if (personality_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{ss}",
                                "personality", qmi_nas_hdr_personality_get_string (personality)
                                ));
                }

                if (protocol_revision_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{ss}",
                                "protocol revision", qmi_nas_hdr_protocol_revision_get_string (protocol_revision)
                                ));
                }

                if (is_856_system_id_valid) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{ss}",
                                "is-856 system id", is_856_system_id
                                ));
                }
            }

            if (qmi_message_nas_get_system_info_output_get_additional_hdr_system_info (
                    output,
                    &geo_system_index,
                    NULL)) {
                if (geo_system_index != 0xFFFF) {
                    json_object_update(json_object_get(json_output,"cdma 1xev-do service"),json_pack("{si}",
                                "geo system index", geo_system_index
                                ));
                }
            }
        }
    }

    /* GSM */
    {
        QmiNasServiceStatus service_status;
        QmiNasServiceStatus true_service_status;
        gboolean preferred_data_path;
        gboolean domain_valid;
        QmiNasNetworkServiceDomain domain;
        gboolean service_capability_valid;
        QmiNasNetworkServiceDomain service_capability;
        gboolean roaming_status_valid;
        QmiNasRoamingStatus roaming_status;
        gboolean forbidden_valid;
        gboolean forbidden;
        gboolean lac_valid;
        guint16 lac;
        gboolean cid_valid;
        guint32 cid;
        gboolean registration_reject_info_valid;
        QmiNasNetworkServiceDomain registration_reject_domain;
        guint8 registration_reject_cause;
        gboolean network_id_valid;
        const gchar *mcc;
        const gchar *mnc;
        gboolean egprs_support_valid;
        gboolean egprs_support;
        gboolean dtm_support_valid;
        gboolean dtm_support;
        guint16 geo_system_index;
        QmiNasCellBroadcastCapability cell_broadcast_support;
        QmiNasCallBarringStatus call_barring_status_cs;
        QmiNasCallBarringStatus call_barring_status_ps;
        QmiNasNetworkServiceDomain cipher_domain;

        if (qmi_message_nas_get_system_info_output_get_gsm_service_status (
                output,
                &service_status,
                &true_service_status,
                &preferred_data_path,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssssb}}",
                 "gsm service",
                                "status", qmi_nas_service_status_get_string (service_status),
                                "true status", qmi_nas_service_status_get_string (true_service_status),
                                "preferred data path", preferred_data_path
                 ));

            if (qmi_message_nas_get_system_info_output_get_gsm_system_info (
                    output,
                    &domain_valid, &domain,
                    &service_capability_valid, &service_capability,
                    &roaming_status_valid, &roaming_status,
                    &forbidden_valid, &forbidden,
                    &lac_valid, &lac,
                    &cid_valid, &cid,
                    &registration_reject_info_valid, &registration_reject_domain, &registration_reject_cause,
                    &network_id_valid, &mcc, &mnc,
                    &egprs_support_valid, &egprs_support,
                    &dtm_support_valid, &dtm_support,
                    NULL)) {
                if (domain_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ss}",
                                "domain", qmi_nas_network_service_domain_get_string (domain)
                                ));
                }

                if (service_capability_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ss}",
                                "service capability", qmi_nas_network_service_domain_get_string (service_capability)
                                ));
                }

                if (roaming_status_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ss}",
                                "roaming status", qmi_nas_network_service_domain_get_string (roaming_status)
                                ));
                }

                if (forbidden_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{sb}",
                                "forbidden", forbidden
                                ));
                }

                if (lac_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{si}",
                                "location area code", lac
                                ));
                }

                if (cid_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{si}",
                                "cell id", cid
                                ));
                }

                if (registration_reject_info_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{sssi}",
                                "registration reject", qmi_nas_network_service_domain_get_string (registration_reject_domain),
                                "registration reject cause", registration_reject_cause
                                ));
                }

                if (network_id_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ssss}",
                                "mcc", mcc,
                                "mnc", mnc
                                ));
                }
                if (egprs_support_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{sb}",
                                "e-gprs supported", egprs_support
                                ));
                }

                if (dtm_support_valid) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{sb}",
                                "dual transfer mode supported", dtm_support
                                ));
                }
            }

            if (qmi_message_nas_get_system_info_output_get_additional_gsm_system_info (
                    output,
                    &geo_system_index,
                    &cell_broadcast_support,
                    NULL)) {
                if (geo_system_index != 0xFFFF) {
                    json_object_update(json_object_get(json_output,"gsm service"),json_pack("{si}",
                                "geo system index", geo_system_index
                                ));
                }

                json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ss}",
                                "cell broadcast support", qmi_nas_cell_broadcast_capability_get_string (cell_broadcast_support)
                                ));
            }

            if (qmi_message_nas_get_system_info_output_get_gsm_call_barring_status (
                    output,
                    &call_barring_status_cs,
                    &call_barring_status_ps,
                    NULL)) {
                json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ssss}",
                                "call barring status cs", qmi_nas_call_barring_status_get_string (call_barring_status_cs),
                                "call barring status ps", qmi_nas_call_barring_status_get_string (call_barring_status_ps)
                                ));
            }

            if (qmi_message_nas_get_system_info_output_get_gsm_cipher_domain (
                    output,
                    &cipher_domain,
                    NULL)) {
                json_object_update(json_object_get(json_output,"gsm service"),json_pack("{ss}",
                                "cipher domain", qmi_nas_network_service_domain_get_string (cipher_domain)
                                ));
            }
        }
    }

    /* WCDMA */
    {
        QmiNasServiceStatus service_status;
        QmiNasServiceStatus true_service_status;
        gboolean preferred_data_path;
        gboolean domain_valid;
        QmiNasNetworkServiceDomain domain;
        gboolean service_capability_valid;
        QmiNasNetworkServiceDomain service_capability;
        gboolean roaming_status_valid;
        QmiNasRoamingStatus roaming_status;
        gboolean forbidden_valid;
        gboolean forbidden;
        gboolean lac_valid;
        guint16 lac;
        gboolean cid_valid;
        guint32 cid;
        gboolean registration_reject_info_valid;
        QmiNasNetworkServiceDomain registration_reject_domain;
        guint8 registration_reject_cause;
        gboolean network_id_valid;
        const gchar *mcc;
        const gchar *mnc;
        gboolean hs_call_status_valid;
        QmiNasWcdmaHsService hs_call_status;
        gboolean hs_service_valid;
        QmiNasWcdmaHsService hs_service;
        gboolean primary_scrambling_code_valid;
        guint16 primary_scrambling_code;
        guint16 geo_system_index;
        QmiNasCellBroadcastCapability cell_broadcast_support;
        QmiNasCallBarringStatus call_barring_status_cs;
        QmiNasCallBarringStatus call_barring_status_ps;
        QmiNasNetworkServiceDomain cipher_domain;

        if (qmi_message_nas_get_system_info_output_get_wcdma_service_status (
                output,
                &service_status,
                &true_service_status,
                &preferred_data_path,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssssb}}",
                 "wcdma service",
                                "status", qmi_nas_service_status_get_string (service_status),
                                "true status", qmi_nas_service_status_get_string (true_service_status),
                                "preferred data path", preferred_data_path
                 ));

            if (qmi_message_nas_get_system_info_output_get_wcdma_system_info (
                    output,
                    &domain_valid, &domain,
                    &service_capability_valid, &service_capability,
                    &roaming_status_valid, &roaming_status,
                    &forbidden_valid, &forbidden,
                    &lac_valid, &lac,
                    &cid_valid, &cid,
                    &registration_reject_info_valid, &registration_reject_domain, &registration_reject_cause,
                    &network_id_valid, &mcc, &mnc,
                    &hs_call_status_valid, &hs_call_status,
                    &hs_service_valid, &hs_service,
                    &primary_scrambling_code_valid, &primary_scrambling_code,
                NULL)) {
                if (domain_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "domain", qmi_nas_network_service_domain_get_string (domain)
                                ));
                }

                if (service_capability_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "service capability", qmi_nas_network_service_domain_get_string (service_capability)
                                ));
                }

                if (roaming_status_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "roaming status", qmi_nas_network_service_domain_get_string (roaming_status)
                                ));
                }

                if (forbidden_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{sb}",
                                "forbidden", forbidden
                                ));
                }

                if (lac_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{si}",
                                "location area code", lac
                                ));
                }

                if (cid_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{si}",
                                "cell id", cid
                                ));
                }

                if (registration_reject_info_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{sssi}",
                                "registration reject", qmi_nas_network_service_domain_get_string (registration_reject_domain),
                                "registration reject cause", registration_reject_cause
                                ));
                }

                if (network_id_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ssss}",
                                "mcc", mcc,
                                "mnc", mnc
                                ));
                }

                if (hs_call_status_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "hs call status", qmi_nas_wcdma_hs_service_get_string (hs_call_status)
                                ));
                }

                if (hs_service_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "hs service", qmi_nas_wcdma_hs_service_get_string (hs_service)
                                ));
                }

                if (primary_scrambling_code_valid) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{si}",
                                "primary_scrambling_code", primary_scrambling_code
                                ));
                }
            }

            if (qmi_message_nas_get_system_info_output_get_additional_wcdma_system_info (
                    output,
                    &geo_system_index,
                    &cell_broadcast_support,
                    NULL)) {
                if (geo_system_index != 0xFFFF) {
                    json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{si}",
                                "geo system index", geo_system_index
                                ));
                }

                json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "cell broadcast support", qmi_nas_cell_broadcast_capability_get_string (cell_broadcast_support)
                                ));
            }

            if (qmi_message_nas_get_system_info_output_get_wcdma_call_barring_status (
                    output,
                    &call_barring_status_cs,
                    &call_barring_status_ps,
                    NULL)) {
                json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ssss}",
                                "call barring status cs", qmi_nas_call_barring_status_get_string (call_barring_status_cs),
                                "call barring status ps", qmi_nas_call_barring_status_get_string (call_barring_status_ps)
                                ));
            }

            if (qmi_message_nas_get_system_info_output_get_wcdma_cipher_domain (
                    output,
                    &cipher_domain,
                    NULL)) {
                json_object_update(json_object_get(json_output,"wcdma service"),json_pack("{ss}",
                                "cipher domain", qmi_nas_network_service_domain_get_string (cipher_domain)
                                ));
            }
        }
    }

    /* LTE */
    {
        QmiNasServiceStatus service_status;
        QmiNasServiceStatus true_service_status;
        gboolean preferred_data_path;
        gboolean domain_valid;
        QmiNasNetworkServiceDomain domain;
        gboolean service_capability_valid;
        QmiNasNetworkServiceDomain service_capability;
        gboolean roaming_status_valid;
        QmiNasRoamingStatus roaming_status;
        gboolean forbidden_valid;
        gboolean forbidden;
        gboolean lac_valid;
        guint16 lac;
        gboolean cid_valid;
        guint32 cid;
        gboolean registration_reject_info_valid;
        QmiNasNetworkServiceDomain registration_reject_domain;
        guint8 registration_reject_cause;
        gboolean network_id_valid;
        const gchar *mcc;
        const gchar *mnc;
        gboolean tac_valid;
        guint16 tac;
        guint16 geo_system_index;
        gboolean voice_support;
        gboolean embms_coverage_info_support;

        if (qmi_message_nas_get_system_info_output_get_lte_service_status (
                output,
                &service_status,
                &true_service_status,
                &preferred_data_path,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssssb}}",
                 "lte service",
                                "status", qmi_nas_service_status_get_string (service_status),
                                "true status", qmi_nas_service_status_get_string (true_service_status),
                                "preferred data path", preferred_data_path
                 ));

            if (qmi_message_nas_get_system_info_output_get_lte_system_info (
                    output,
                    &domain_valid, &domain,
                    &service_capability_valid, &service_capability,
                    &roaming_status_valid, &roaming_status,
                    &forbidden_valid, &forbidden,
                    &lac_valid, &lac,
                    &cid_valid, &cid,
                    &registration_reject_info_valid,&registration_reject_domain,&registration_reject_cause,
                    &network_id_valid, &mcc, &mnc,
                    &tac_valid, &tac,
                    NULL)) {
                if (domain_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{ss}",
                                "domain", qmi_nas_network_service_domain_get_string (domain)
                                ));
                }

                if (service_capability_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{ss}",
                                "service capability", qmi_nas_network_service_domain_get_string (service_capability)
                                ));
                }

                if (roaming_status_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{ss}",
                                "roaming status", qmi_nas_network_service_domain_get_string (roaming_status)
                                ));
                }

                if (forbidden_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{sb}",
                                "forbidden", forbidden
                                ));
                }

                if (lac_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{si}",
                                "location area code", lac
                                ));
                }

                if (cid_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{si}",
                                "cell id", cid
                                ));
                }

                if (registration_reject_info_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{sssi}",
                                "registration reject", qmi_nas_network_service_domain_get_string (registration_reject_domain),
                                "registration reject cause", registration_reject_cause
                                ));
                }

                if (network_id_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{ssss}",
                                "mcc", mcc,
                                "mnc", mnc
                                ));
                }

                if (tac_valid) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{si}",
                                "tracking area code", tac
                                ));
                }
            }

            if (qmi_message_nas_get_system_info_output_get_additional_lte_system_info (
                    output,
                    &geo_system_index,
                    NULL)) {
                if (geo_system_index != 0xFFFF) {
                    json_object_update(json_object_get(json_output,"lte service"),json_pack("{si}",
                                "geo system index", geo_system_index
                                ));
                }
            }

            if (qmi_message_nas_get_system_info_output_get_lte_voice_support (
                    output,
                    &voice_support,
                    NULL)) {
                json_object_update(json_object_get(json_output,"lte service"),json_pack("{sb}",
                                "voice support", voice_support
                                ));
            }

            if (qmi_message_nas_get_system_info_output_get_lte_embms_coverage_info_support (
                    output,
                    &embms_coverage_info_support,
                    NULL)) {
                json_object_update(json_object_get(json_output,"lte service"),json_pack("{sb}",
                                "embms coverage info support", embms_coverage_info_support
                                ));
            }
        }
    }

    /* TD-SCDMA */
    {
        QmiNasServiceStatus service_status;
        QmiNasServiceStatus true_service_status;
        gboolean preferred_data_path;
        gboolean domain_valid;
        QmiNasNetworkServiceDomain domain;
        gboolean service_capability_valid;
        QmiNasNetworkServiceDomain service_capability;
        gboolean roaming_status_valid;
        QmiNasRoamingStatus roaming_status;
        gboolean forbidden_valid;
        gboolean forbidden;
        gboolean lac_valid;
        guint16 lac;
        gboolean cid_valid;
        guint32 cid;
        gboolean registration_reject_info_valid;
        QmiNasNetworkServiceDomain registration_reject_domain;
        guint8 registration_reject_cause;
        gboolean network_id_valid;
        const gchar *mcc;
        const gchar *mnc;
        gboolean hs_call_status_valid;
        QmiNasWcdmaHsService hs_call_status;
        gboolean hs_service_valid;
        QmiNasWcdmaHsService hs_service;
        gboolean cell_parameter_id_valid;
        guint16 cell_parameter_id;
        gboolean cell_broadcast_support_valid;
        QmiNasCellBroadcastCapability cell_broadcast_support;
        gboolean call_barring_status_cs_valid;
        QmiNasCallBarringStatus call_barring_status_cs;
        gboolean call_barring_status_ps_valid;
        QmiNasCallBarringStatus call_barring_status_ps;
        gboolean cipher_domain_valid;
        QmiNasNetworkServiceDomain cipher_domain;

        if (qmi_message_nas_get_system_info_output_get_td_scdma_service_status (
                output,
                &service_status,
                &true_service_status,
                &preferred_data_path,
                NULL)) {
            json_object_update(json_output, json_pack("{s{sssssb}}",
                 "td-scdma service",
                                "status", qmi_nas_service_status_get_string (service_status),
                                "true status", qmi_nas_service_status_get_string (true_service_status),
                                "preferred data path", preferred_data_path
                 ));

            if (qmi_message_nas_get_system_info_output_get_td_scdma_system_info (
                    output,
                    &domain_valid, &domain,
                    &service_capability_valid, &service_capability,
                    &roaming_status_valid, &roaming_status,
                    &forbidden_valid, &forbidden,
                    &lac_valid, &lac,
                    &cid_valid, &cid,
                    &registration_reject_info_valid, &registration_reject_domain, &registration_reject_cause,
                    &network_id_valid, &mcc, &mnc,
                    &hs_call_status_valid, &hs_call_status,
                    &hs_service_valid, &hs_service,
                    &cell_parameter_id_valid, &cell_parameter_id,
                    &cell_broadcast_support_valid, &cell_broadcast_support,
                    &call_barring_status_cs_valid, &call_barring_status_cs,
                    &call_barring_status_ps_valid, &call_barring_status_ps,
                    &cipher_domain_valid, &cipher_domain,
                    NULL)) {
                if (domain_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "domain", qmi_nas_network_service_domain_get_string (domain)
                                ));
                }

                if (service_capability_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "service capability", qmi_nas_network_service_domain_get_string (service_capability)
                                ));
                }

                if (roaming_status_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "roaming status", qmi_nas_network_service_domain_get_string (roaming_status)
                                ));
                }

                if (forbidden_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{sb}",
                                "forbidden", forbidden
                                ));
                }

                if (lac_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{si}",
                                "location area code", lac
                                ));
                }

                if (cid_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{si}",
                                "cell id", cid
                                ));
                }

                if (registration_reject_info_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{sssi}",
                                "registration reject", qmi_nas_network_service_domain_get_string (registration_reject_domain),
                                "registration reject cause", registration_reject_cause
                                ));
                }

                if (network_id_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ssss}",
                                "mcc", mcc,
                                "mnc", mnc
                                ));
                }

                if (hs_call_status_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "hs call status", qmi_nas_wcdma_hs_service_get_string (hs_call_status)
                                ));
                }

                if (hs_service_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "hs service", qmi_nas_wcdma_hs_service_get_string (hs_service)
                                ));
                }

                if (cell_parameter_id_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{si}",
                                "cell parameter id", cid
                                ));
                }

                if (cell_broadcast_support_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "cell broadcast support", qmi_nas_cell_broadcast_capability_get_string (cell_broadcast_support)
                                ));
                }

                if (call_barring_status_cs_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "call barring status cs", qmi_nas_call_barring_status_get_string (call_barring_status_cs)
                                ));
                }

                if (call_barring_status_ps_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "call barring status ps", qmi_nas_call_barring_status_get_string (call_barring_status_ps)
                                ));
                }

                if (cipher_domain_valid) {
                    json_object_update(json_object_get(json_output,"ts-scdma service"),json_pack("{ss}",
                                "cipher domain", qmi_nas_network_service_domain_get_string (cipher_domain)
                                ));
                }

            }
        }

        /* Common */
        {
            QmiNasSimRejectState sim_reject_info;

            if (qmi_message_nas_get_system_info_output_get_sim_reject_info (
                    output,
                    &sim_reject_info,
                    NULL)) {
                json_object_update(json_output, json_pack("{ss}",
                    "sim reject info", qmi_nas_sim_reject_state_get_string (sim_reject_info)
                    ));
            }
        }
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_system_info_output_unref (output);
    shutdown (TRUE);
}

static void
get_technology_preference_ready (QmiClientNas *client,
                                 GAsyncResult *res)
{
    QmiMessageNasGetTechnologyPreferenceOutput *output;
    GError *error = NULL;
    QmiNasRadioTechnologyPreference preference;
    QmiNasPreferenceDuration duration;
    gchar *preference_string;
    json_t *json_output;

    output = qmi_client_nas_get_technology_preference_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));

        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_technology_preference_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get technology preference",
             "message", error->message
              ),json_print_flag));

        g_error_free (error);
        qmi_message_nas_get_technology_preference_output_unref (output);
        shutdown (FALSE);
        return;
    }

    qmi_message_nas_get_technology_preference_output_get_active (
        output,
        &preference,
        &duration,
        NULL);

    preference_string = qmi_nas_radio_technology_preference_build_string_from_mask (preference);
    json_output = json_pack("{sbssssss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device),
             "active", preference_string,
             "duration", qmi_nas_preference_duration_get_string (duration)
              );
    g_free (preference_string);

    if (qmi_message_nas_get_technology_preference_output_get_persistent (
            output,
            &preference,
            NULL)) {
        preference_string = qmi_nas_radio_technology_preference_build_string_from_mask (preference);
        json_object_update(json_output, json_pack("{ss}",
                 "persistent", preference_string
                 ));
        g_free (preference_string);
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_technology_preference_output_unref (output);
    shutdown (TRUE);
}

static void
get_system_selection_preference_ready (QmiClientNas *client,
                                 GAsyncResult *res)
{
    QmiMessageNasGetSystemSelectionPreferenceOutput *output;
    GError *error = NULL;
    gboolean emergency_mode;
    QmiNasRatModePreference mode_preference;
    QmiNasBandPreference band_preference;
    QmiNasLteBandPreference lte_band_preference;
    QmiNasTdScdmaBandPreference td_scdma_band_preference;
    QmiNasCdmaPrlPreference cdma_prl_preference;
    QmiNasRoamingPreference roaming_preference;
    QmiNasNetworkSelectionPreference network_selection_preference;
    QmiNasServiceDomainPreference service_domain_preference;
    QmiNasGsmWcdmaAcquisitionOrderPreference gsm_wcdma_acquisition_order_preference;
    guint16 mcc;
    guint16 mnc;
    gboolean has_pcs_digit;
    json_t *json_output;

    output = qmi_client_nas_get_system_selection_preference_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));

        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_get_system_selection_preference_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get system selection preference",
             "message", error->message
              ),json_print_flag));

        g_error_free (error);
        qmi_message_nas_get_system_selection_preference_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );


    if (qmi_message_nas_get_system_selection_preference_output_get_emergency_mode (
            output,
            &emergency_mode,
            NULL)) {
        json_object_update(json_output, json_pack("{sb}",
             "emergency mode", emergency_mode
             ));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_mode_preference (
            output,
            &mode_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_rat_mode_preference_build_string_from_mask (mode_preference);
        json_object_update(json_output, json_pack("{ss}",
             "mode preference", str
             ));
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_band_preference (
            output,
            &band_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_band_preference_build_string_from_mask (band_preference);
        json_object_update(json_output, json_pack("{ss}",
             "band preference", str
             ));
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_lte_band_preference (
            output,
            &lte_band_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_lte_band_preference_build_string_from_mask (lte_band_preference);
        json_object_update(json_output, json_pack("{ss}",
             "lte band preference", str
             ));
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_td_scdma_band_preference (
            output,
            &td_scdma_band_preference,
            NULL)) {
        gchar *str;

        str = qmi_nas_td_scdma_band_preference_build_string_from_mask (td_scdma_band_preference);
        json_object_update(json_output, json_pack("{ss}",
             "td-scdma band preference", str
             ));
        g_free (str);
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_cdma_prl_preference (
            output,
            &cdma_prl_preference,
            NULL)) {
        json_object_update(json_output, json_pack("{ss}",
             "cdma prl preference", qmi_nas_cdma_prl_preference_get_string (cdma_prl_preference)
             ));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_roaming_preference (
            output,
            &roaming_preference,
            NULL)) {
        json_object_update(json_output, json_pack("{ss}",
             "roaming preference", qmi_nas_roaming_preference_get_string (roaming_preference)
             ));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_network_selection_preference (
            output,
            &network_selection_preference,
            NULL)) {
        json_object_update(json_output, json_pack("{ss}",
             "network selection preference", qmi_nas_network_selection_preference_get_string (network_selection_preference)
             ));
    }


    if (qmi_message_nas_get_system_selection_preference_output_get_service_domain_preference (
            output,
            &service_domain_preference,
            NULL)) {
        json_object_update(json_output, json_pack("{ss}",
             "service domain preference", qmi_nas_service_domain_preference_get_string (service_domain_preference)
             ));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_gsm_wcdma_acquisition_order_preference (
            output,
            &gsm_wcdma_acquisition_order_preference,
            NULL)) {
        json_object_update(json_output, json_pack("{ss}",
             "service selection preference", qmi_nas_gsm_wcdma_acquisition_order_preference_get_string (gsm_wcdma_acquisition_order_preference)
             ));
    }

    if (qmi_message_nas_get_system_selection_preference_output_get_manual_network_selection (
            output,
            &mcc,
            &mnc,
            &has_pcs_digit,
            NULL)) {
        json_object_update(json_output, json_pack("{s{sisisb}}",
                "manual network selection",
                        "mcc", mcc,
                        "mnc", mnc,
                        "mcc with pcs digit", has_pcs_digit
             ));
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_get_system_selection_preference_output_unref (output);
    shutdown (TRUE);
}

static QmiMessageNasSetSystemSelectionPreferenceInput *
set_system_selection_preference_input_create (const gchar *str)
{
    QmiMessageNasSetSystemSelectionPreferenceInput *input = NULL;
    QmiNasRatModePreference pref;
    GError *error = NULL;

    if (!qmicli_read_rat_mode_pref_from_string (str, &pref)) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", "failed to parse mode pref"
              ),json_print_flag));
        return NULL;
    }

    input = qmi_message_nas_set_system_selection_preference_input_new ();
    if (!qmi_message_nas_set_system_selection_preference_input_set_mode_preference (
            input,
            pref,
            &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't create input data bundle",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_set_system_selection_preference_input_unref (input);
        return NULL;
    }

    if (!qmi_message_nas_set_system_selection_preference_input_set_change_duration (
            input,
            QMI_NAS_CHANGE_DURATION_PERMANENT,
            &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't create input data bundle",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_set_system_selection_preference_input_unref (input);
        return NULL;
    }

    if (pref & (QMI_NAS_RAT_MODE_PREFERENCE_GSM |
                QMI_NAS_RAT_MODE_PREFERENCE_UMTS |
                QMI_NAS_RAT_MODE_PREFERENCE_LTE)) {
        if (!qmi_message_nas_set_system_selection_preference_input_set_gsm_wcdma_acquisition_order_preference (
                input,
                QMI_NAS_GSM_WCDMA_ACQUISITION_ORDER_PREFERENCE_AUTOMATIC,
                &error)) {
            g_print ("%s\n", json_dumps(json_pack("{sbssss}",
                 "success", 0,
                 "error", "couldn't create input data bundle",
                 "message", error->message
                 ),json_print_flag));
            g_error_free (error);
            qmi_message_nas_set_system_selection_preference_input_unref (input);
            return NULL;
        }
    }

    return input;
}

static void
set_system_selection_preference_ready (QmiClientNas *client,
                                       GAsyncResult *res)
{
    QmiMessageNasSetSystemSelectionPreferenceOutput *output = NULL;
    GError *error = NULL;

    output = qmi_client_nas_set_system_selection_preference_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_set_system_selection_preference_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't set operating mode",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_set_system_selection_preference_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("%s\n", json_dumps(json_pack("{sbsssb}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device),
             "reset required", 1
              ),json_print_flag));

    qmi_message_nas_set_system_selection_preference_output_unref (output);
    shutdown (TRUE);
}

static void
network_scan_ready (QmiClientNas *client,
                    GAsyncResult *res)
{
    QmiMessageNasNetworkScanOutput *output;
    GError *error = NULL;
    GArray *array;
    json_t *json_output;

    output = qmi_client_nas_network_scan_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_network_scan_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't scan networks",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_network_scan_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbsss{}}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device),
             "network"
              );

    array = NULL;
    if (qmi_message_nas_network_scan_output_get_network_information (output, &array, NULL)) {
        guint i;

        for (i = 0; i < array->len; i++) {
            QmiMessageNasNetworkScanOutputNetworkInformationElement *element;
            gchar *status_str;
            gchar itostr[22];

            element = &g_array_index (array, QmiMessageNasNetworkScanOutputNetworkInformationElement, i);
            status_str = qmi_nas_network_status_build_string_from_mask (element->network_status);
            g_snprintf(itostr,21,"%d",i);
            json_object_update(json_object_get(json_output,"network"),json_pack("{s{sisissss}}",
                        itostr,
                                "mcc", element->mcc,
                                "mnc", element->mnc,
                                "status", status_str,
                                "description", element->description
                        ));
            g_free (status_str);
        }
    }

    array = NULL;
    if (qmi_message_nas_network_scan_output_get_radio_access_technology (output, &array, NULL)) {
        guint i;
        gchar itostr[22];

        for (i = 0; i < array->len; i++) {
            QmiMessageNasNetworkScanOutputRadioAccessTechnologyElement *element;

            element = &g_array_index (array, QmiMessageNasNetworkScanOutputRadioAccessTechnologyElement, i);
            g_snprintf(itostr,21,"%d",i);
            json_object_update(json_object_get(json_object_get(json_output,"network"),itostr),json_pack("{sisiss}",
                        "mcc", element->mcc,
                        "mnc", element->mnc,
                        "rat", qmi_nas_radio_interface_get_string (element->radio_interface)
                        ));
        }
    }

    array = NULL;
    if (qmi_message_nas_network_scan_output_get_mnc_pcs_digit_include_status (output, &array, NULL)) {
        guint i;
        gchar itostr[22];

        for (i = 0; i < array->len; i++) {
            QmiMessageNasNetworkScanOutputMncPcsDigitIncludeStatusElement *element;

            element = &g_array_index (array, QmiMessageNasNetworkScanOutputMncPcsDigitIncludeStatusElement, i);
            g_snprintf(itostr,21,"%d",i);
            json_object_update(json_object_get(json_object_get(json_output,"network"),itostr),json_pack("{sisisb}",
                        "mcc", element->mcc,
                        "mnc", element->mnc,
                        "mcc with pcs digit", element->includes_pcs_digit
                        ));
        }
    }
    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_nas_network_scan_output_unref (output);
    shutdown (TRUE);
}

static void
reset_ready (QmiClientNas *client,
             GAsyncResult *res)
{
    QmiMessageNasResetOutput *output;
    GError *error = NULL;

    output = qmi_client_nas_reset_finish (client, res, &error);
    if (!output) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "operation failed",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        shutdown (FALSE);
        return;
    }

    if (!qmi_message_nas_reset_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't reset the nas service",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_nas_reset_output_unref (output);
        shutdown (FALSE);
        return;
    }

    g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device),
             "message", "successfully performed nas service reset"
              ),json_print_flag));

    qmi_message_nas_reset_output_unref (output);
    shutdown (TRUE);
}

static gboolean
noop_cb (gpointer unused)
{
    shutdown (TRUE);
    return FALSE;
}

void
qmicli_nas_run (QmiDevice *device,
                QmiClientNas *client,
                GCancellable *cancellable)
{
    /* Initialize context */
    ctx = g_slice_new (Context);
    ctx->device = g_object_ref (device);
    ctx->client = g_object_ref (client);
    if (cancellable)
        ctx->cancellable = g_object_ref (cancellable);

    /* Request to get signal strength? */
    if (get_signal_strength_flag) {
        QmiMessageNasGetSignalStrengthInput *input;

        input = get_signal_strength_input_create ();

        g_debug ("Asynchronously getting signal strength...");
        qmi_client_nas_get_signal_strength (ctx->client,
                                            input,
                                            10,
                                            ctx->cancellable,
                                            (GAsyncReadyCallback)get_signal_strength_ready,
                                            NULL);
        qmi_message_nas_get_signal_strength_input_unref (input);
        return;
    }

    /* Request to get signal info? */
    if (get_signal_info_flag) {
        g_debug ("Asynchronously getting signal info...");
        qmi_client_nas_get_signal_info (ctx->client,
                                        NULL,
                                        10,
                                        ctx->cancellable,
                                        (GAsyncReadyCallback)get_signal_info_ready,
                                        NULL);
        return;
    }

    /* Request to get tx/rx info? */
    if (get_tx_rx_info_str) {
        QmiMessageNasGetTxRxInfoInput *input;
        QmiNasRadioInterface interface;

        input = get_tx_rx_info_input_create (get_tx_rx_info_str,
                                             &interface);
        if (!input) {
            shutdown (FALSE);
            return;
        }

        g_debug ("Asynchronously getting TX/RX info...");
        qmi_client_nas_get_tx_rx_info (ctx->client,
                                       input,
                                       10,
                                       ctx->cancellable,
                                       (GAsyncReadyCallback)get_tx_rx_info_ready,
                                       GUINT_TO_POINTER (interface));
        qmi_message_nas_get_tx_rx_info_input_unref (input);
        return;
    }

    /* Request to get home network? */
    if (get_home_network_flag) {
        g_debug ("Asynchronously getting home network...");
        qmi_client_nas_get_home_network (ctx->client,
                                         NULL,
                                         10,
                                         ctx->cancellable,
                                         (GAsyncReadyCallback)get_home_network_ready,
                                         NULL);
        return;
    }

    /* Request to get serving system? */
    if (get_serving_system_flag) {
        g_debug ("Asynchronously getting serving system...");
        qmi_client_nas_get_serving_system (ctx->client,
                                           NULL,
                                           10,
                                           ctx->cancellable,
                                           (GAsyncReadyCallback)get_serving_system_ready,
                                           NULL);
        return;
    }

    /* Request to get system info? */
    if (get_system_info_flag) {
        g_debug ("Asynchronously getting system info...");
        qmi_client_nas_get_system_info (ctx->client,
                                        NULL,
                                        10,
                                        ctx->cancellable,
                                        (GAsyncReadyCallback)get_system_info_ready,
                                        NULL);
        return;
    }

    /* Request to get technology preference? */
    if (get_technology_preference_flag) {
        g_debug ("Asynchronously getting technology preference...");
        qmi_client_nas_get_technology_preference (ctx->client,
                                                  NULL,
                                                  10,
                                                  ctx->cancellable,
                                                  (GAsyncReadyCallback)get_technology_preference_ready,
                                                  NULL);
        return;
    }

    /* Request to get system_selection preference? */
    if (get_system_selection_preference_flag) {
        g_debug ("Asynchronously getting system selection preference...");
        qmi_client_nas_get_system_selection_preference (ctx->client,
                                                        NULL,
                                                        10,
                                                        ctx->cancellable,
                                                        (GAsyncReadyCallback)get_system_selection_preference_ready,
                                                        NULL);
        return;
    }

    /* Request to set system_selection preference? */
    if (set_system_selection_preference_str) {
        QmiMessageNasSetSystemSelectionPreferenceInput *input;
        g_debug ("Asynchronously setting system selection preference...");

        input = set_system_selection_preference_input_create (set_system_selection_preference_str);
        if (!input) {
            shutdown (FALSE);
            return;
        }

        qmi_client_nas_set_system_selection_preference (ctx->client,
                                                        input,
                                                        10,
                                                        ctx->cancellable,
                                                        (GAsyncReadyCallback)set_system_selection_preference_ready,
                                                        NULL);
        qmi_message_nas_set_system_selection_preference_input_unref (input);
        return;
    }

    /* Request to scan networks? */
    if (network_scan_flag) {
        g_debug ("Asynchronously scanning networks...");
        qmi_client_nas_network_scan (ctx->client,
                                     NULL,
                                     300, /* this operation takes a lot of time! */
                                     ctx->cancellable,
                                     (GAsyncReadyCallback)network_scan_ready,
                                     NULL);
        return;
    }

    /* Request to reset NAS service? */
    if (reset_flag) {
        g_debug ("Asynchronously resetting NAS service...");
        qmi_client_nas_reset (ctx->client,
                              NULL,
                              10,
                              ctx->cancellable,
                              (GAsyncReadyCallback)reset_ready,
                              NULL);
        return;
    }

    /* Just client allocate/release? */
    if (noop_flag) {
        g_idle_add (noop_cb, NULL);
        return;
    }

    g_warn_if_reached ();
}
