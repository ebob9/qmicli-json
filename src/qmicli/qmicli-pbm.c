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
 * Copyright (C) 2013 Aleksander Morgado <aleksander@gnu.org>
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

/* Context */
typedef struct {
    QmiDevice *device;
    QmiClientPbm *client;
    GCancellable *cancellable;
} Context;
static Context *ctx;

/* Options */
static gboolean get_all_capabilities_flag;
static gboolean noop_flag;

static GOptionEntry entries[] = {
    { "pbm-get-all-capabilities", 0, 0, G_OPTION_ARG_NONE, &get_all_capabilities_flag,
      "Get all phonebook capabilities",
      NULL
    },
    { "pbm-noop", 0, 0, G_OPTION_ARG_NONE, &noop_flag,
      "Just allocate or release a PBM client. Use with `--client-no-release-cid' and/or `--client-cid'",
      NULL
    },
    { NULL }
};

GOptionGroup *
qmicli_pbm_get_option_group (void)
{
        GOptionGroup *group;

        group = g_option_group_new ("pbm",
                                    "PBM options",
                                    "Show Phonebook Management options",
                                    NULL,
                                    NULL);
        g_option_group_add_entries (group, entries);

        return group;
}

gboolean
qmicli_pbm_options_enabled (void)
{
    static guint n_actions = 0;
    static gboolean checked = FALSE;

    if (checked)
        return !!n_actions;

    n_actions = (get_all_capabilities_flag +
                 noop_flag);

    if (n_actions > 1) {
        g_print ("%s\n", json_dumps(json_pack("{sbss}",
             "success", 0,
             "error", "too many pbm actions requested"
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

    if (context->client)
        g_object_unref (context->client);
    g_object_unref (context->cancellable);
    g_object_unref (context->device);
    g_slice_free (Context, context);
}

static void
shutdown (gboolean operation_status)
{
    /* Cleanup context and finish async operation */
    context_free (ctx);
    qmicli_async_operation_done (operation_status);
}

static void
get_all_capabilities_ready (QmiClientPbm *client,
                            GAsyncResult *res)
{
    GError *error = NULL;
    QmiMessagePbmGetAllCapabilitiesOutput *output;
    GArray *array = NULL;
    guint i, j;
    json_t *json_output;

    output = qmi_client_pbm_get_all_capabilities_finish (client, res, &error);
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

    if (!qmi_message_pbm_get_all_capabilities_output_get_result (output, &error)) {
        g_print ("%s\n", json_dumps(json_pack("{sbssss}",
             "success", 0,
             "error", "couldn't get capabilities",
             "message", error->message
              ),json_print_flag));
        g_error_free (error);
        qmi_message_pbm_get_all_capabilities_output_unref (output);
        shutdown (FALSE);
        return;
    }

    json_output = json_pack("{sbss}",
             "success", 1,
             "device", qmi_device_get_path_display (ctx->device)
              );

    if (qmi_message_pbm_get_all_capabilities_output_get_capability_basic_information (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "capability basic information"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputCapabilityBasicInformationElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputCapabilityBasicInformationElement,
                                      i);
            json_object_update(json_object_get(json_output,"capability basic information"),json_pack("{s{}}",
                   qmi_pbm_session_type_get_string (session->session_type)
                   ));

            for (j = 0; j < session->phonebooks->len; j++) {
                QmiMessagePbmGetAllCapabilitiesOutputCapabilityBasicInformationElementPhonebooksElement *phonebook;
                gchar *phonebook_type_str;

                phonebook = &g_array_index (session->phonebooks,
                                            QmiMessagePbmGetAllCapabilitiesOutputCapabilityBasicInformationElementPhonebooksElement,
                                            j);
                phonebook_type_str = qmi_pbm_phonebook_type_build_string_from_mask (phonebook->phonebook_type);
                json_object_update(json_object_get(json_object_get(json_output,"capability basic information"), qmi_pbm_session_type_get_string (session->session_type)),json_pack("{s{sisisisi}}",
                    phonebook_type_str,
                        "used records", phonebook->used_records,
                        "maximum records", phonebook->maximum_records,
                        "maximum number length", phonebook->maximum_number_length,
                        "maximum name length", phonebook->maximum_name_length
                        ));
                g_free (phonebook_type_str);
            }
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_group_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "group capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputGroupCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputGroupCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"group capability"),json_pack("{s{sisi}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "maximum groups", session->maximum_groups,
                         "maximum group tag length", session->maximum_group_tag_length
                   ));
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_additional_number_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "additional number capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputAdditionalNumberCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputAdditionalNumberCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"additional number capability"),json_pack("{s{sisisi}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "maximum additional numbers", session->maximum_additional_numbers,
                         "maximum additional number length", session->maximum_additional_number_length,
                         "maximum additional number tag length", session->maximum_additional_number_tag_length
                   ));
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_email_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "email capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputEmailCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputEmailCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"email capability"),json_pack("{s{sisi}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "maximum emails", session->maximum_emails,
                         "maximum email address length", session->maximum_email_address_length
                   ));
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_second_name_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "second name capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputSecondNameCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputSecondNameCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"second name capability"),json_pack("{s{si}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "maximum second name length", session->maximum_second_name_length
                   ));
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_hidden_records_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "hidden records capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputHiddenRecordsCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputHiddenRecordsCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"hidden records capability"),json_pack("{s{sb}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "supported", session->supported
                   ));
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_grouping_information_alpha_string_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "alpha string capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputGroupingInformationAlphaStringCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputGroupingInformationAlphaStringCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"alpha string capability"),json_pack("{s{sisisi}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "maximum records", session->maximum_records,
                         "used records", session->used_records,
                         "maximum string length", session->maximum_string_length
                   ));
        }
    }

    if (qmi_message_pbm_get_all_capabilities_output_get_additional_number_alpha_string_capability (output, &array, NULL)) {
        json_object_update(json_output, json_pack("{s{}}",
             "additional number alpha string capability"
             ));
        for (i = 0; i < array->len; i++) {
            QmiMessagePbmGetAllCapabilitiesOutputAdditionalNumberAlphaStringCapabilityElement *session;

            session = &g_array_index (array,
                                      QmiMessagePbmGetAllCapabilitiesOutputAdditionalNumberAlphaStringCapabilityElement,
                                      i);
            json_object_update(json_object_get(json_output,"alpha string capability"),json_pack("{s{sisisi}}",
                   qmi_pbm_session_type_get_string (session->session_type),
                         "maximum records", session->maximum_records,
                         "used records", session->used_records,
                         "maximum string length", session->maximum_string_length
                   ));
        }
    }

    g_print ("%s\n", json_dumps(json_output,json_print_flag) ? : JSON_OUTPUT_ERROR);
    g_free(json_output);

    qmi_message_pbm_get_all_capabilities_output_unref (output);
    shutdown (TRUE);
}

static gboolean
noop_cb (gpointer unused)
{
    shutdown (TRUE);
    return FALSE;
}

void
qmicli_pbm_run (QmiDevice *device,
                QmiClientPbm *client,
                GCancellable *cancellable)
{
    /* Initialize context */
    ctx = g_slice_new (Context);
    ctx->device = g_object_ref (device);
    ctx->client = g_object_ref (client);
    ctx->cancellable = g_object_ref (cancellable);

    /* Request to get all capabilities? */
    if (get_all_capabilities_flag) {
        g_debug ("Asynchronously getting phonebook capabilities...");
        qmi_client_pbm_get_all_capabilities (ctx->client,
                                             NULL,
                                             10,
                                             ctx->cancellable,
                                             (GAsyncReadyCallback)get_all_capabilities_ready,
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
