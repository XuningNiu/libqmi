/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * qmi-firmware-update -- Command line tool to update firmware in QMI devices
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
 * Copyright (C) 2016-2017 Zodiac Inflight Innovation
 * Copyright (C) 2016-2017 Aleksander Morgado <aleksander@aleksander.es>
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "qfu-log.h"
#include "qfu-operation.h"
#include "qfu-device-selection.h"
#include "qfu-udev-helpers.h"

#define PROGRAM_NAME    "qmi-firmware-update"
#define PROGRAM_VERSION PACKAGE_VERSION

/*****************************************************************************/
/* Options */

/* Generic device selections */
static guint      busnum;
static guint      devnum;
static guint16    vid;
static guint16    pid;
static gchar     *cdc_wdm_str;
static gchar     *tty_str;

/* Update */
static gboolean   action_update_flag;
static gchar     *firmware_version_str;
static gchar     *config_version_str;
static gchar     *carrier_str;
static gboolean   device_open_proxy_flag;
static gboolean   device_open_qmi_flag;
static gboolean   device_open_mbim_flag;
static gboolean   device_open_auto_flag;
static gboolean   ignore_version_errors_flag;
static gboolean   override_download_flag;
static gint       modem_storage_index_int;
static gboolean   skip_validation_flag;

/* Reset */
static gboolean   action_reset_flag;

/* Update (QDL mode) */
static gboolean   action_update_qdl_flag;

/* Verify */
static gboolean   action_verify_flag;

/* Main */
static gchar    **image_strv;
static gboolean   stdout_verbose_flag;
static gboolean   stdout_silent_flag;
static gchar     *verbose_log_str;
static gboolean   version_flag;
static gboolean   help_flag;
static gboolean   help_examples_flag;

static gboolean
parse_busnum_devnum (const gchar  *option_name,
                     const gchar  *value,
                     gpointer      data,
                     GError      **error)
{
    gchar    **strv;
    gint       busnum_idx = -1;
    gint       devnum_idx = 0;
    gulong     aux;
    gboolean   result = FALSE;

    strv = g_strsplit (value, ":", -1);
    g_assert (strv[0]);
    if (strv[1]) {
        busnum_idx = 0;
        devnum_idx = 1;
        if (strv[2]) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "invalid busnum-devnum string: too many fields");
            goto out;
        }
    }

    if (busnum_idx != -1) {
        aux = strtoul (strv[busnum_idx], NULL, 10);
        if (aux == 0 || aux > G_MAXUINT) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "invalid bus number: %s", strv[busnum_idx]);
            goto out;
        }
        busnum = (guint) aux;
    }

    aux = strtoul (strv[devnum_idx], NULL, 10);
    if (aux == 0 || aux > G_MAXUINT) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "invalid dev number: %s", strv[devnum_idx]);
        goto out;
    }
    devnum = (guint) aux;
    result = TRUE;

out:
    g_strfreev (strv);
    return result;
}

static gboolean
parse_vid_pid (const gchar  *option_name,
               const gchar  *value,
               gpointer      data,
               GError      **error)
{
    gchar    **strv;
    gint       vid_idx = 0;
    gint       pid_idx = -1;
    gulong     aux;
    gboolean   result = FALSE;

    strv = g_strsplit (value, ":", -1);
    g_assert (strv[0]);
    if (strv[1]) {
        pid_idx = 1;
        if (strv[2]) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "invalid vid-pid string: too many fields");
            goto out;
        }
    }

    if (pid_idx != -1) {
        aux = strtoul (strv[pid_idx], NULL, 16);
        if (aux == 0 || aux > G_MAXUINT16) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "invalid product id: %s", strv[pid_idx]);
            goto out;
        }
        pid = (guint) aux;
    }

    aux = strtoul (strv[vid_idx], NULL, 16);
    if (aux == 0 || aux > G_MAXUINT16) {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     "invalid vendor id: %s", strv[vid_idx]);
        goto out;
    }
    vid = (guint16) aux;
    result = TRUE;

out:
    g_strfreev (strv);
    return result;
}

static GOptionEntry context_selection_entries[] = {
    { "busnum-devnum", 's', 0, G_OPTION_ARG_CALLBACK, &parse_busnum_devnum,
      "Select device by bus and device number (in decimal).",
      "[BUS:]DEV"
    },
    { "vid-pid", 'd', 0, G_OPTION_ARG_CALLBACK, &parse_vid_pid,
      "Select device by device vendor and product id (in hexadecimal).",
      "VID[:PID]"
    },
    { "cdc-wdm", 'w', 0, G_OPTION_ARG_FILENAME, &cdc_wdm_str,
      "Select device by QMI/MBIM cdc-wdm device path (e.g. /dev/cdc-wdm0).",
      "[PATH]"
    },
    { "tty", 't', 0, G_OPTION_ARG_FILENAME, &tty_str,
      "Select device by serial device path (e.g. /dev/ttyUSB2).",
      "[PATH]"
    },
    { NULL }
};

static GOptionEntry context_update_entries[] = {
    { "update", 'u', 0, G_OPTION_ARG_NONE, &action_update_flag,
      "Launch firmware update process.",
      NULL
    },
    { "firmware-version", 'f', 0, G_OPTION_ARG_STRING, &firmware_version_str,
      "Firmware version (e.g. '05.05.58.00').",
      "[VERSION]",
    },
    { "config-version", 'c', 0, G_OPTION_ARG_STRING, &config_version_str,
      "Config version (e.g. '005.025_002').",
      "[VERSION]",
    },
    { "carrier", 'C', 0, G_OPTION_ARG_STRING, &carrier_str,
      "Carrier name (e.g. 'Generic').",
      "[CARRIER]",
    },
    { "ignore-version-errors", 0, 0, G_OPTION_ARG_NONE, &ignore_version_errors_flag,
      "Run update operation even with version string errors.",
      NULL
    },
    { "override-download", 0, 0, G_OPTION_ARG_NONE, &override_download_flag,
      "Download images even if module says it already has them.",
      NULL
    },
    { "modem-storage-index", 0, 0, G_OPTION_ARG_INT, &modem_storage_index_int,
      "Index storage for the modem image.",
      "[INDEX]"
    },
    { "skip-validation", 0, 0, G_OPTION_ARG_NONE, &skip_validation_flag,
      "Don't wait to validate the running firmware after update.",
      NULL
    },
    { NULL }
};

static GOptionEntry context_reset_entries[] = {
    { "reset", 'b', 0, G_OPTION_ARG_NONE, &action_reset_flag,
      "Reset device into QDL download mode.",
      NULL
    },
    { NULL }
};

static GOptionEntry context_update_qdl_entries[] = {
    { "update-qdl", 'U', 0, G_OPTION_ARG_NONE, &action_update_qdl_flag,
      "Launch firmware update process in QDL mode.",
      NULL
    },
    { NULL }
};

static GOptionEntry context_verify_entries[] = {
    { "verify", 'z', 0, G_OPTION_ARG_NONE, &action_verify_flag,
      "Analyze and verify firmware images.",
      NULL
    },
    { NULL }
};

static GOptionEntry context_main_entries[] = {
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &image_strv, "",
      "FILE1 FILE2..."
    },
    { "device-open-proxy", 'p', 0, G_OPTION_ARG_NONE, &device_open_proxy_flag,
      "Request to use the 'qmi-proxy' proxy.",
      NULL
    },
    { "device-open-qmi", 0, 0, G_OPTION_ARG_NONE, &device_open_qmi_flag,
      "Open a cdc-wdm device explicitly in QMI mode",
      NULL
    },
    { "device-open-mbim", 0, 0, G_OPTION_ARG_NONE, &device_open_mbim_flag,
      "Open a cdc-wdm device explicitly in MBIM mode",
      NULL
    },
    { "device-open-auto", 0, 0, G_OPTION_ARG_NONE, &device_open_auto_flag,
      "Open a cdc-wdm device in either QMI or MBIM mode (default)",
      NULL
    },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &stdout_verbose_flag,
      "Run action with verbose messages in standard output, including the debug ones.",
      NULL
    },
    { "silent", 'S', 0, G_OPTION_ARG_NONE, &stdout_silent_flag,
      "Run action with no messages in standard output; not even the error ones.",
      NULL
    },
    { "verbose-log", 'L', 0, G_OPTION_ARG_FILENAME, &verbose_log_str,
      "Write verbose messages to an output file.",
      "[PATH]"
    },
    { "version", 'V', 0, G_OPTION_ARG_NONE, &version_flag,
      "Print version.",
      NULL
    },
    { "help", 'h', 0, G_OPTION_ARG_NONE, &help_flag,
      "Show help.",
      NULL
    },
    { "help-examples", 'H', 0, G_OPTION_ARG_NONE, &help_examples_flag,
      "Show help examples.",
      NULL
    },
    { NULL }
};

static const gchar *context_description =
    "   ***************************************************************************\n"
    "                                Warning!\n"
    "   ***************************************************************************\n"
    "\n"
    "   Use this program with caution. The authors take *no* responsibility if any\n"
    "   device gets broken as a result of using this program.\n"
    "\n"
    "   Please report issues to the libqmi mailing list at:\n"
    "     libqmi-devel@lists.freedesktop.org\n";

/*****************************************************************************/

static void
print_version (void)
{
    g_print ("\n"
             PROGRAM_NAME " " PROGRAM_VERSION "\n"
             "\n"
             "  Copyright (C) 2016-2017 Bjørn Mork\n"
             "  Copyright (C) 2016-2017 Zodiac Inflight Innovations\n"
             "  Copyright (C) 2016-2017 Aleksander Morgado\n"
             "\n"
             "License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl-2.0.html>\n"
             "This is free software: you are free to change and redistribute it.\n"
             "There is NO WARRANTY, to the extent permitted by law.\n"
             "\n");
}

static void
print_help (GOptionContext *context)
{
    gchar *str;

    /* Always print --help-all */
    str = g_option_context_get_help (context, FALSE, NULL);
    g_print ("%s", str);
    g_free (str);
}

static void
print_help_examples (void)
{
    g_print ("\n"
             "********************************************************************************\n"
             "\n"
             " Example 1: Updating a Sierra Wireless MC7354.\n"
             "\n"
             " The MC7354 is a 9x15 device which requires the firmware updater to specify the\n"
             " firmware version string, the config version string and the carrier string, so\n"
             " that they are included as identifiers of the firmware images downloaded. The\n"
             " core logic in the application will try to automatically detect these strings,\n"
             " although the user can also use specific options to override them or if the\n"
             " automatic detection failed.\n"
             "\n"
             " While in normal operation, the device will expose multiple cdc-wdm ports, and\n"
             " the updater application just needs one of those cdc-wdm ports to start the\n"
             " operation. The user can explicitly specify the cdc-wdm port to use, or\n"
             " otherwise use the generic device selection options (i.e. --busnum-devnum or\n"
             " --vid-pid) to do that automatically.\n"
             "\n"
             " Note that the firmware for the MC7354 is usually composed of a core system image\n"
             " (.cwe) and a carrier-specific image (.nvu). These two images need to be flashed\n"
             " on the same operation, unless upgrading the carrier-specific image on a device\n"
             " which already has the matching firmware version. The two images may be given\n"
             " combined into a single image (.spk) file.\n"
             "\n"
             " 1a) An update operation specifying the vid:pid of the device (fails if multiple\n"
             "     devices with the same vid:pid are found):\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       --update \\\n"
             "       -d 1199:68c0 \\\n"
             "       SWI9X15C_05.05.58.00.cwe \\\n"
             "       SWI9X15C_05.05.58.00_Generic_005.025_002.nvu\n"
             "\n"
             " 1b) An update operation specifying an explicit QMI cdc-wdm device:\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       --update \\\n"
             "       --cdc-wdm /dev/cdc-wdm0 \\\n"
             "       SWI9X15C_05.05.58.00.cwe \\\n"
             "       SWI9X15C_05.05.58.00_Generic_005.025_002.nvu\n"
             "\n"
             " 1c) An update operation specifying explicit firmware, config and carrier strings:\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       --update \\\n"
             "       -d 1199:68c0 \\\n"
             "       --firmware-version 05.05.58.00 \\\n"
             "       --config-version 005.025_002 \\\n"
             "       --carrier Generic \\\n"
             "       SWI9X15C_05.05.58.00.cwe \\\n"
             "       SWI9X15C_05.05.58.00_Generic_005.025_002.nvu\n"
             "\n"
             " 1d) An update operation with a combined image containing both system and carrier\n"
             "     specific images::\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       --update \\\n"
             "       -d 1199:68c0 \\\n"
             "       9999999_9902574_SWI9X15C_05.05.66.00_00_GENNA-UMTS_005.028_000-field.spk\n");

    g_print ("\n"
             "********************************************************************************\n"
             "\n"
             " Example 2: Updating a Sierra Wireless MC7700.\n"
             "\n"
             " The MC7700 is a 9200 device which doesn't require the explicit firmware, config\n"
             " and carrier strings. Unlike the MC7354, which would reboot itself into QDL\n"
             " download mode once these previous strings were configured, the MC7700 requires\n"
             " a specific \"boot and hold\" command to be sent (either via QMI or AT) to request\n"
             " the reset in QDL download mode.\n"
             "\n"
             " 2a) An update operation specifying the vid:pid of the device (fails if multiple\n"
             "     devices with the same vid:pid are found):\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       --update \\\n"
             "       -d 1199:68a2 \\\n"
             "       9999999_9999999_9200_03.05.14.00_00_generic_000.000_001_SPKG_MC.cwe\n"
             "\n"
             " 2b) An update operation specifying an explicit QMI cdc-wdm device:\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       --update \\\n"
             "       --cdc-wdm /dev/cdc-wdm0 \\\n"
             "       9999999_9999999_9200_03.05.14.00_00_generic_000.000_001_SPKG_MC.cwe\n");

    g_print ("\n"
             "********************************************************************************\n"
             "\n"
             " Example 3: Manual process to update a Sierra Wireless MC7700.\n"
             "\n"
             " Instead of letting the " PROGRAM_NAME " manage the full firmware update\n"
             " operation (include the module reboots into the different modes), the user can\n"
             " trigger the actions manually as follows:\n"
             "\n"
             " 3a) Request device to go into QDL download mode:\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       -d 1199:68a2 \\\n"
             "       --reset\n"
             "\n"
             " 3b) Run updater operation while in QDL download mode:\n"
             " $ sudo " PROGRAM_NAME " \\\n"
             "       -d 1199:68a2 \\\n"
             "       --update-qdl \\\n"
             "       9999999_9999999_9200_03.05.14.00_00_generic_000.000_001_SPKG_MC.cwe\n");

    g_print ("\n"
             "********************************************************************************\n"
             "\n"
             " Example 4: Verify firmware images.\n"
             "\n"
             " 3a) Verify several images at once:\n"
             " $ " PROGRAM_NAME " \\\n"
             "       --verify \\\n"
             "       SWI9X15C_05.05.58.00.cwe \\\n"
             "       SWI9X15C_05.05.58.00_Generic_005.025_002.nvu\n"
             "\n"
             " 3b) Verify all .cwe, .nvu and .spk images inside a directory:\n"
             " $ find . -regex \".*\\.\\(nvu\\|spk\\|cwe\\)\" -exec " PROGRAM_NAME " -v -z {} \\;\n"
             "\n"
             " 3c) Image files may be given within .exe files; extract them with 7-Zip:\n"
             " $ 7z x SWI9200M_3.5-Release13-SWI9200X_03.05.29.03.exe\n"
             " $ ls *.{cwe,nvu,spk} 2>/dev/null\n"
             "   9999999_9999999_9200_03.05.29.03_00_generic_000.000_001_SPKG_MC.cwe\n"
             "\n");
}

/*****************************************************************************/

int main (int argc, char **argv)
{
    GError             *error = NULL;
    GOptionContext     *context;
    GOptionGroup       *group;
    guint               n_actions;
    gboolean            result = FALSE;
    QfuDeviceSelection *device_selection = NULL;
    QmiDeviceOpenFlags  device_open_flags = QMI_DEVICE_OPEN_FLAGS_NONE;

    setlocale (LC_ALL, "");

    g_type_init ();

    /* Setup option context, process it and destroy it */
    context = g_option_context_new ("- Update firmware in QMI devices");
    g_option_context_set_description (context, context_description);

    group = g_option_group_new ("selection", "Generic device selection options", "", NULL, NULL);
    g_option_group_add_entries (group, context_selection_entries);
    g_option_context_add_group (context, group);

    group = g_option_group_new ("update", "Update options (normal mode)", "", NULL, NULL);
    g_option_group_add_entries (group, context_update_entries);
    g_option_context_add_group (context, group);

    group = g_option_group_new ("reset", "Reset options (normal mode)", "", NULL, NULL);
    g_option_group_add_entries (group, context_reset_entries);
    g_option_context_add_group (context, group);

    group = g_option_group_new ("update-qdl", "Update options (QDL mode)", "", NULL, NULL);
    g_option_group_add_entries (group, context_update_qdl_entries);
    g_option_context_add_group (context, group);

    group = g_option_group_new ("verify", "Verify options", "", NULL, NULL);
    g_option_group_add_entries (group, context_verify_entries);
    g_option_context_add_group (context, group);

    g_option_context_add_main_entries (context, context_main_entries, NULL);
    g_option_context_set_help_enabled (context, FALSE);

    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_printerr ("error: couldn't parse option context: %s\n", error->message);
        g_error_free (error);
        goto out;
    }

    if (version_flag) {
        print_version ();
        result = TRUE;
        goto out;
    }

    if (help_flag) {
        print_help (context);
        result = TRUE;
        goto out;
    }

    if (help_examples_flag) {
        print_help_examples ();
        result = TRUE;
        goto out;
    }

    /* Initialize logging */
    qfu_log_init (stdout_verbose_flag, stdout_silent_flag, verbose_log_str);

    /* We don't allow multiple actions at the same time */
    n_actions = (action_verify_flag +
                 action_update_flag +
                 action_update_qdl_flag +
                 action_reset_flag);
    if (n_actions == 0) {
        g_printerr ("error: no actions specified\n");
        goto out;
    }
    if (n_actions > 1) {
        g_printerr ("error: too many actions specified\n");
        goto out;
    }

    /* A list of images must be provided for update and verify operations */
    if ((action_verify_flag || action_update_flag || action_update_qdl_flag) && !image_strv) {
        g_printerr ("error: no firmware images specified\n");
        goto out;
    }

    /* device selection must be performed for update and reset operations */
    if (action_update_flag || action_update_qdl_flag || action_reset_flag) {
        device_selection = qfu_device_selection_new (cdc_wdm_str, tty_str, vid, pid, busnum, devnum, &error);
        if (!device_selection) {
            g_printerr ("error: couldn't select device:: %s\n", error->message);
            g_error_free (error);
            goto out;
        }
    }

    /* Validate device open flags */
    if (action_update_flag || action_reset_flag) {
        if (device_open_mbim_flag + device_open_qmi_flag + device_open_auto_flag > 1) {
            g_printerr ("error: cannot specify multiple mode flags to open device\n");
            goto out;
        }
        if (device_open_proxy_flag)
            device_open_flags |= QMI_DEVICE_OPEN_FLAGS_PROXY;
        if (device_open_mbim_flag)
            device_open_flags |= QMI_DEVICE_OPEN_FLAGS_MBIM;
        if (device_open_auto_flag || (!device_open_qmi_flag && !device_open_mbim_flag))
            device_open_flags |= QMI_DEVICE_OPEN_FLAGS_AUTO;
    }

    /* Run */

    if (action_update_flag) {
        g_assert (QFU_IS_DEVICE_SELECTION (device_selection));

        /* Validate storage index, just (0,G_MAXUINT8] for now. The value 0 is also not
         * valid, but we use it to flag when no specific index has been requested. */
        if (modem_storage_index_int < 0 || modem_storage_index_int > G_MAXUINT8) {
            g_printerr ("error: invalid modem storage index\n");
            goto out;
        }

        result = qfu_operation_update_run ((const gchar **) image_strv,
                                           device_selection,
                                           firmware_version_str,
                                           config_version_str,
                                           carrier_str,
                                           device_open_flags,
                                           ignore_version_errors_flag,
                                           override_download_flag,
                                           (guint8) modem_storage_index_int,
                                           skip_validation_flag);
        goto out;
    }

    if (action_update_qdl_flag) {
        g_assert (QFU_IS_DEVICE_SELECTION (device_selection));
        result = qfu_operation_update_qdl_run ((const gchar **) image_strv,
                                               device_selection);
        goto out;
    }

    if (action_reset_flag) {
        g_assert (QFU_IS_DEVICE_SELECTION (device_selection));
        result = qfu_operation_reset_run (device_selection, device_open_flags);
        goto out;
    }

    if (action_verify_flag) {
        result = qfu_operation_verify_run ((const gchar **) image_strv);
        goto out;
    }

    g_assert_not_reached ();

out:

    qfu_log_shutdown ();

    /* Clean exit for a clean memleak report */
    if (context)
        g_option_context_free (context);
    if (device_selection)
        g_object_unref (device_selection);

    return (result ? EXIT_SUCCESS : EXIT_FAILURE);
}
