/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2009  Bastien Nocera <hadess@hadess.net>
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <net/ethernet.h>
#include <netinet/ether.h>
#include <glib/gi18n-lib.h>

#include <gtk/gtk.h>
#include <bluetooth-plugin.h>
#include <nm-setting-connection.h>
#include <nm-setting-bluetooth.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-cdma.h>
#include <nm-setting-gsm.h>
#include <nm-setting-serial.h>
#include <nm-setting-ppp.h>
#include <nm-utils.h>
#include <nma-gconf-settings.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#include "nma-marshal.h"
#include "bling-spinner.h"
#include "mobile-wizard.h"

#define DBUS_TYPE_G_MAP_OF_VARIANT (dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE))

#define BLUEZ_SERVICE           "org.bluez"
#define BLUEZ_MANAGER_PATH      "/"
#define BLUEZ_MANAGER_INTERFACE "org.bluez.Manager"
#define BLUEZ_ADAPTER_INTERFACE "org.bluez.Adapter"
#define BLUEZ_DEVICE_INTERFACE  "org.bluez.Device"
#define BLUEZ_SERIAL_INTERFACE  "org.bluez.Serial"
#define BLUEZ_NETWORK_INTERFACE "org.bluez.Network"

#define MM_SERVICE         "org.freedesktop.ModemManager"
#define MM_PATH            "/org/freedesktop/ModemManager"
#define MM_INTERFACE       "org.freedesktop.ModemManager"
#define MM_MODEM_INTERFACE "org.freedesktop.ModemManager.Modem"

typedef enum {
	BT_METHOD_UNKNOWN = 0,
	BT_METHOD_PAN = 1,
	BT_METHOD_DUN = 2
} BtMethod;

typedef struct {
	char *bdaddr;
	BtMethod method;
	GtkWidget *button;
	guint toggled_id;

	GtkWidget *hbox;
	GtkWidget *label;
	GtkWidget *spinner;
	NMSettingsConnectionInterface *connection;

	/* DUN stuff */
	DBusGConnection *bus;
	DBusGProxy *bluez_proxy;
	DBusGProxy *adapter_proxy;
	DBusGProxy *dun_proxy;

	DBusGProxy *mm_proxy;
	GSList *modem_proxies;

	char *rfcomm_iface;
	guint dun_timeout_id;

	NMDeviceType devtype;

	MobileWizard *wizard;
	GtkWindowGroup *window_group;
} PluginInfo;

static BtMethod
get_best_method (const char *bdaddr, const char **uuids)
{
	guint i;
	gboolean has_nap = FALSE, has_dun = FALSE;

	for (i = 0; uuids && uuids[i] != NULL; i++) {
		g_message ("has_config_widget %s %s", bdaddr, uuids[i]);
		if (g_str_equal (uuids[i], "NAP"))
			has_nap = TRUE;
		if (g_str_equal (uuids[i], "DialupNetworking"))
			has_dun = TRUE;
	}

	if (has_nap)
		return BT_METHOD_PAN;
	else if (has_dun)
		return BT_METHOD_DUN;
	return BT_METHOD_UNKNOWN;
}

static gboolean
has_config_widget (const char *bdaddr, const char **uuids)
{
	return !!get_best_method (bdaddr, uuids);
}

static GByteArray *
get_array_from_bdaddr (const char *str)
{
	struct ether_addr *addr;
	GByteArray *array;

	addr = ether_aton (str);
	if (addr) {
		array = g_byte_array_sized_new (ETH_ALEN);
		g_byte_array_append (array, (const guint8 *) addr->ether_addr_octet, ETH_ALEN);
		return array;
	}

	return NULL;
}

/*******************************************************************/

static NMSettingsConnectionInterface *
add_pan_connection (const char *bdaddr)
{
	NMConnection *connection;
	NMSetting *setting, *bt_setting, *ip_setting;
	NMAGConfSettings *gconf_settings;
	NMAGConfConnection *exported;
	GByteArray *mac;
	char *id, *uuid;

	mac = get_array_from_bdaddr (bdaddr);
	if (mac == NULL)
		return NULL;

	/* The connection */
	connection = nm_connection_new ();

	/* The connection settings */
	setting = nm_setting_connection_new ();
	id = g_strdup_printf ("%s %s", bdaddr, "PANU");
	uuid = nm_utils_uuid_generate ();
	g_object_set (G_OBJECT (setting),
	              NM_SETTING_CONNECTION_ID, id,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_BLUETOOTH_SETTING_NAME,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NULL);
	g_free (id);
	g_free (uuid);
	nm_connection_add_setting (connection, setting);

	/* The Bluetooth settings */
	bt_setting = nm_setting_bluetooth_new ();
	g_object_set (G_OBJECT (bt_setting),
	              NM_SETTING_BLUETOOTH_BDADDR, mac,
	              NM_SETTING_BLUETOOTH_TYPE, NM_SETTING_BLUETOOTH_TYPE_PANU,
	              NULL);
	g_byte_array_free (mac, TRUE);
	nm_connection_add_setting (connection, bt_setting);

	/* The IPv4 settings */
	ip_setting = nm_setting_ip4_config_new ();
	g_object_set (G_OBJECT (ip_setting),
	              NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
	              NULL);
	nm_connection_add_setting (connection, ip_setting);

	gconf_settings = nma_gconf_settings_new (NULL);
	exported = nma_gconf_settings_add_connection (gconf_settings, connection);

	if (exported != NULL)
		return NM_SETTINGS_CONNECTION_INTERFACE (exported);
	return NULL;
}

/*******************************************************************/

static void
dun_cleanup (PluginInfo *info, const char *message, gboolean uncheck)
{
	GSList *iter;

	for (iter = info->modem_proxies; iter; iter = g_slist_next (iter))
		g_object_unref (DBUS_G_PROXY (iter->data));
	g_slist_free (info->modem_proxies);
	info->modem_proxies = NULL;

	if (info->dun_proxy) {
		if (info->rfcomm_iface) {
			dbus_g_proxy_call_no_reply (info->dun_proxy, "Disconnect",
			                            G_TYPE_STRING, info->rfcomm_iface,
			                            G_TYPE_INVALID);
		}

		g_object_unref (info->dun_proxy);
		info->dun_proxy = NULL;
	}

	g_free (info->rfcomm_iface);
	info->rfcomm_iface = NULL;

	if (info->adapter_proxy) {
		g_object_unref (info->adapter_proxy);
		info->adapter_proxy = NULL;
	}

	if (info->bluez_proxy) {
		g_object_unref (info->bluez_proxy);
		info->bluez_proxy = NULL;
	}

	if (info->bus) {
		dbus_g_connection_unref (info->bus);
		info->bus = NULL;
	}

	if (info->dun_timeout_id) {
		g_source_remove (info->dun_timeout_id);
		info->dun_timeout_id = 0;
	}

	if (info->window_group) {
		g_object_unref (info->window_group);
		info->window_group = NULL;
	}

	if (info->wizard) {
		mobile_wizard_destroy (info->wizard);
		info->wizard = NULL;
	}

	info->devtype = NM_DEVICE_TYPE_UNKNOWN;

	if (info->spinner) {
		bling_spinner_stop (BLING_SPINNER (info->spinner));
		gtk_widget_hide (info->spinner);
	}
	gtk_label_set_text (GTK_LABEL (info->label), message);
	gtk_widget_set_sensitive (info->button, TRUE);

	if (uncheck) {
		g_signal_handler_block (info->button, info->toggled_id);
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (info->button), FALSE);
		g_signal_handler_unblock (info->button, info->toggled_id);
	}
}

static void
dun_error (PluginInfo *info, const char *func, GError *error, const char *fallback)
{
	char *message;

	message = g_strdup_printf (_("Error: %s"), (error && error->message) ? error->message : fallback);
	g_warning ("%s: %s", func, message);
	dun_cleanup (info, message, TRUE);
	g_free (message);
}

static NMConnection *
dun_new_cdma (MobileWizardAccessMethod *method)
{
	NMConnection *connection;
	NMSetting *setting;
	char *uuid, *id;

	connection = nm_connection_new ();

	setting = nm_setting_cdma_new ();
	g_object_set (setting,
	              NM_SETTING_CDMA_NUMBER, "#777",
	              NM_SETTING_CDMA_USERNAME, method->username,
	              NM_SETTING_CDMA_PASSWORD, method->password,
	              NULL);
	nm_connection_add_setting (connection, setting);

	/* Serial setting */
	setting = nm_setting_serial_new ();
	g_object_set (setting,
	              NM_SETTING_SERIAL_BAUD, 115200,
	              NM_SETTING_SERIAL_BITS, 8,
	              NM_SETTING_SERIAL_PARITY, 'n',
	              NM_SETTING_SERIAL_STOPBITS, 1,
	              NULL);
	nm_connection_add_setting (connection, setting);

	nm_connection_add_setting (connection, nm_setting_ppp_new ());

	setting = nm_setting_connection_new ();
	if (method->plan_name)
		id = g_strdup_printf ("%s %s", method->provider_name, method->plan_name);
	else
		id = g_strdup_printf ("%s connection", method->provider_name);
	uuid = nm_utils_uuid_generate ();
	g_object_set (setting,
	              NM_SETTING_CONNECTION_ID, id,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_CDMA_SETTING_NAME,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NULL);
	g_free (uuid);
	g_free (id);
	nm_connection_add_setting (connection, setting);

	return connection;
}

static NMConnection *
dun_new_gsm (MobileWizardAccessMethod *method)
{
	NMConnection *connection;
	NMSetting *setting;
	char *uuid, *id;

	connection = nm_connection_new ();

	setting = nm_setting_gsm_new ();
	g_object_set (setting,
	              NM_SETTING_GSM_NUMBER, "*99#",
	              NM_SETTING_GSM_USERNAME, method->username,
	              NM_SETTING_GSM_PASSWORD, method->password,
	              NM_SETTING_GSM_APN, method->gsm_apn,
	              NULL);
	nm_connection_add_setting (connection, setting);

	/* Serial setting */
	setting = nm_setting_serial_new ();
	g_object_set (setting,
	              NM_SETTING_SERIAL_BAUD, 115200,
	              NM_SETTING_SERIAL_BITS, 8,
	              NM_SETTING_SERIAL_PARITY, 'n',
	              NM_SETTING_SERIAL_STOPBITS, 1,
	              NULL);
	nm_connection_add_setting (connection, setting);

	nm_connection_add_setting (connection, nm_setting_ppp_new ());

	setting = nm_setting_connection_new ();
	if (method->plan_name)
		id = g_strdup_printf ("%s %s", method->provider_name, method->plan_name);
	else
		id = g_strdup_printf ("%s connection", method->provider_name);
	uuid = nm_utils_uuid_generate ();
	g_object_set (setting,
	              NM_SETTING_CONNECTION_ID, id,
	              NM_SETTING_CONNECTION_TYPE, NM_SETTING_GSM_SETTING_NAME,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NULL);
	g_free (uuid);
	g_free (id);
	nm_connection_add_setting (connection, setting);

	return connection;
}

static void
wizard_done_cb (MobileWizard *self,
                gboolean canceled,
                MobileWizardAccessMethod *method,
                gpointer user_data)
{
	PluginInfo *info = user_data;
	NMConnection *connection = NULL;
	NMAGConfSettings *gconf_settings;
	NMAGConfConnection *exported;
	GByteArray *mac;
	NMSetting *s_bt;

	g_message ("%s: mobile wizard done", __func__);

	if (canceled || !method) {
		dun_error (info, __func__, NULL, _("Mobile wizard was canceled"));
		return;
	}

	if (method->devtype == NM_DEVICE_TYPE_CDMA)
		connection = dun_new_cdma (method);
	else if (method->devtype == NM_DEVICE_TYPE_GSM)
		connection = dun_new_gsm (method);
	else {
		dun_error (info, __func__, NULL, _("Unknown phone device type (not GSM or CDMA)"));
		return;
	}

	mobile_wizard_destroy (info->wizard);
	info->wizard = NULL;

	g_assert (connection);

	/* The Bluetooth settings */
	mac = get_array_from_bdaddr (info->bdaddr);
	g_assert (mac);
	s_bt = nm_setting_bluetooth_new ();
	g_object_set (G_OBJECT (s_bt),
	              NM_SETTING_BLUETOOTH_BDADDR, mac,
	              NM_SETTING_BLUETOOTH_TYPE, NM_SETTING_BLUETOOTH_TYPE_DUN,
	              NULL);
	g_byte_array_free (mac, TRUE);
	nm_connection_add_setting (connection, s_bt);

	g_message ("%s: adding new setting to GConf", __func__);

	gconf_settings = nma_gconf_settings_new (NULL);
	exported = nma_gconf_settings_add_connection (gconf_settings, connection);
	if (exported)
		info->connection = NM_SETTINGS_CONNECTION_INTERFACE (exported);

	g_message ("%s: success!", __func__);
	dun_cleanup (info, _("Your phone is now ready to use!"), FALSE);
}

static void
modem_get_all_cb (DBusGProxy *proxy, DBusGProxyCall *call, gpointer user_data)
{
	PluginInfo *info = user_data;
	const char *path;
	GHashTable *properties = NULL;
	GError *error = NULL;
	GValue *value;

	path = dbus_g_proxy_get_path (proxy);
	g_message ("%s: (%s) processing GetAll reply", __func__, path);

	if (!dbus_g_proxy_end_call (proxy, call, &error,
	                            DBUS_TYPE_G_MAP_OF_VARIANT, &properties,
	                            G_TYPE_INVALID)) {
		g_warning ("%s: (%s) Error getting modem properties: (%d) %s",
		           __func__,
		           path,
		           error ? error->code : -1,
		           (error && error->message) ? error->message : "(unknown)");
		g_error_free (error);
		goto out;
	}

	/* check whether this is the device we care about */
	value = g_hash_table_lookup (properties, "Device");
	if (value && G_VALUE_HOLDS_STRING (value) && g_value_get_string (value)) {
		char *basename = g_path_get_basename (info->rfcomm_iface);
		const char *modem_iface = g_value_get_string (value);

		if (!strcmp (basename, modem_iface)) {
			/* yay, found it! */

			value = g_hash_table_lookup (properties, "Type");
			if (value && G_VALUE_HOLDS_UINT (value)) {
				switch (g_value_get_uint (value)) {
				case 1:
					info->devtype = NM_DEVICE_TYPE_GSM;
					break;
				case 2:
					info->devtype = NM_DEVICE_TYPE_CDMA;
					break;
				default:
					g_message ("%s: (%s) unknown modem type", __func__, path);
					break;
				}
			}
		} else {
			g_message ("%s: (%s) (%s) not the modem we're looking for (%s)",
			           __func__,
			           path,
			           modem_iface,
			           basename);
		}

		g_free (basename);
	} else
		g_message ("%s: (%s) modem had no 'Device' property", __func__, path);

	g_hash_table_unref (properties);

	if (info->devtype != NM_DEVICE_TYPE_UNKNOWN) {
		GtkWidget *parent;

		g_message ("%s: (%s) starting the mobile wizard", __func__, path);

		g_source_remove (info->dun_timeout_id);
		info->dun_timeout_id = 0;

		parent = gtk_widget_get_toplevel (info->hbox);
		if (GTK_WIDGET_TOPLEVEL (parent)) {
			info->window_group = gtk_window_group_new ();
			gtk_window_group_add_window (info->window_group, GTK_WINDOW (parent));
		} else {
			parent = NULL;
			info->window_group = NULL;
		}

		/* Start the mobile wizard */
		info->wizard = mobile_wizard_new (parent ? GTK_WINDOW (parent) : NULL,
		                                  info->window_group,
		                                  info->devtype,
		                                  FALSE,
		                                  wizard_done_cb,
		                                  info);
		mobile_wizard_present (info->wizard);
	}

out:
	g_message ("%s: finished", __func__);
}

static void
modem_added (DBusGProxy *proxy, const char *path, gpointer user_data)
{
	PluginInfo *info = user_data;
	DBusGProxy *props_proxy;

	g_return_if_fail (path != NULL);

	g_message ("%s: (%s) modem found", __func__, path);

	/* Create a proxy for the modem and get its properties */
	props_proxy = dbus_g_proxy_new_for_name (info->bus,
	                                         MM_SERVICE,
	                                         path,
	                                         "org.freedesktop.DBus.Properties");
	g_assert (proxy);
	info->modem_proxies = g_slist_append (info->modem_proxies, props_proxy);

	g_message ("%s: (%s) calling GetAll...", __func__, path);

	dbus_g_proxy_begin_call (props_proxy, "GetAll",
	                         modem_get_all_cb,
	                         info,
	                         NULL,
	                         G_TYPE_STRING, MM_MODEM_INTERFACE,
	                         G_TYPE_INVALID);
}

static void
modem_removed (DBusGProxy *proxy, const char *path, gpointer user_data)
{
	PluginInfo *info = user_data;
	GSList *iter;
	DBusGProxy *found = NULL;

	g_return_if_fail (path != NULL);

	g_message ("%s: (%s) modem removed", __func__, path);

	/* Clean up if a modem gets removed */

	for (iter = info->modem_proxies; iter; iter = g_slist_next (iter)) {
		if (!strcmp (path, dbus_g_proxy_get_path (DBUS_G_PROXY (iter->data)))) {
			found = iter->data;
			break;
		}
	}

	if (found) {
		info->modem_proxies = g_slist_remove (info->modem_proxies, found);
		g_object_unref (found);
	}
}

static void
dun_connect_cb (DBusGProxy *proxy,
                DBusGProxyCall *call,
                void *user_data)
{
	PluginInfo *info = user_data;
	GError *error = NULL;
	char *device;

	g_message ("%s: processing Connect reply", __func__);

	if (!dbus_g_proxy_end_call (proxy, call, &error,
	                            G_TYPE_STRING, &device,
	                            G_TYPE_INVALID)) {
		dun_error (info, __func__, error, _("failed to connect to the phone."));
		g_clear_error (&error);
		goto out;
	}

	if (!device || !strlen (device)) {
		dun_error (info, __func__, NULL, _("failed to connect to the phone."));
		g_free (device);
		goto out;
	}

	info->rfcomm_iface = device;
	g_message ("%s: new rfcomm interface '%s'", __func__, device);

out:
	g_message ("%s: finished", __func__);
}

static void
dun_property_changed (DBusGProxy *proxy,
                         const char *property,
                         GValue *value,
                         gpointer user_data)
{
	PluginInfo *info = user_data;
	gboolean connected;

	if (strcmp (property, "Connected"))
		return;

	connected = g_value_get_boolean (value);

	g_message ("%s: device property Connected changed to %s",
	           __func__,
	           connected ? "TRUE" : "FALSE");

	if (connected) {
		/* Wait for MM here ? */
	} else
		dun_error (info, __func__, NULL, _("unexpectedly disconnected from the phone."));
}

static void
find_device_cb (DBusGProxy *proxy, DBusGProxyCall *call, gpointer user_data)
{
	PluginInfo *info = user_data;
	char *dev_path = NULL;
	GError *error = NULL;

	g_message ("%s: processing FindDevice reply", __func__);

	if (!dbus_g_proxy_end_call (proxy, call, &error,
	                            DBUS_TYPE_G_OBJECT_PATH, &dev_path,
	                            G_TYPE_INVALID)) {
		dun_error (info, __func__, error, _("failed to discover the phone."));
		g_error_free (error);
		goto out;
	}


	/* Request a connection to the device and get the port */
	info->dun_proxy = dbus_g_proxy_new_for_name (info->bus,
	                                             BLUEZ_SERVICE,
	                                             dev_path,
	                                             BLUEZ_SERIAL_INTERFACE);
	g_assert (info->dun_proxy);

	g_message ("%s: calling Connect...", __func__);

	dbus_g_proxy_begin_call_with_timeout (info->dun_proxy, "Connect",
	                                      dun_connect_cb,
	                                      info,
	                                      NULL,
	                                      20000,
	                                      G_TYPE_STRING, "dun",
	                                      G_TYPE_INVALID);

	/* Watch for BT device property changes */
	dbus_g_object_register_marshaller (nma_marshal_VOID__STRING_BOXED,
	                                   G_TYPE_NONE,
	                                   G_TYPE_STRING, G_TYPE_VALUE,
	                                   G_TYPE_INVALID);
	dbus_g_proxy_add_signal (info->dun_proxy, "PropertyChanged",
	                         G_TYPE_STRING, G_TYPE_VALUE, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (info->dun_proxy, "PropertyChanged",
	                             G_CALLBACK (dun_property_changed), info, NULL);

out:
	g_message ("%s: finished", __func__);
}

static void
default_adapter_cb (DBusGProxy *proxy, DBusGProxyCall *call, gpointer user_data)
{
	PluginInfo *info = user_data;
	const char *default_adapter = NULL;
	GError *error = NULL;

	g_message ("%s: processing DefaultAdapter reply", __func__);

	if (!dbus_g_proxy_end_call (proxy, call, &error,
	                            DBUS_TYPE_G_OBJECT_PATH, &default_adapter,
	                            G_TYPE_INVALID)) {
		dun_error (info, __func__, error, _("could not discover Bluetooth adapter."));
		g_error_free (error);
		goto out;
	}

	info->adapter_proxy = dbus_g_proxy_new_for_name (info->bus,
	                                                 BLUEZ_SERVICE,
	                                                 default_adapter,
	                                                 BLUEZ_ADAPTER_INTERFACE);
	g_assert (info->adapter_proxy);

	g_message ("%s: calling FindDevice...", __func__);

	dbus_g_proxy_begin_call (info->adapter_proxy, "FindDevice",
	                         find_device_cb,
	                         info, NULL,
	                         G_TYPE_STRING, info->bdaddr,
	                         G_TYPE_INVALID);

out:
	g_message ("%s: finished", __func__);
}

static gboolean
dun_timeout_cb (gpointer user_data)
{
	PluginInfo *info = user_data;

	info->dun_timeout_id = 0;
	dun_error (info, __func__, NULL, _("timed out detecting phone details."));
	return FALSE;
}

static void
dun_start (PluginInfo *info)
{
	GError *error = NULL;

	g_message ("%s: starting DUN device discovery...", __func__);

	/* Set up dbus */
	info->bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error || !info->bus) {
		dun_error (info, __func__, error, _("could not connect to the system bus."));
		g_clear_error (&error);
		goto out;
	}

	gtk_label_set_text (GTK_LABEL (info->label), _("Detecting phone configuration..."));

	/* Start the spinner */
	if (!info->spinner) {
		info->spinner = bling_spinner_new ();
		gtk_box_pack_start (GTK_BOX (info->hbox), info->spinner, FALSE, FALSE, 6);
	}
	bling_spinner_start (BLING_SPINNER (info->spinner));
	gtk_widget_show_all (info->hbox);

	gtk_widget_set_sensitive (info->button, FALSE);

	/* ModemManager stuff */
	info->mm_proxy = dbus_g_proxy_new_for_name (info->bus,
	                                            MM_SERVICE,
	                                            MM_PATH,
	                                            MM_INTERFACE);
	g_assert (info->mm_proxy);

	dbus_g_object_register_marshaller (g_cclosure_marshal_VOID__BOXED,
	                                   G_TYPE_NONE,
	                                   G_TYPE_BOXED,
	                                   G_TYPE_INVALID);
	dbus_g_proxy_add_signal (info->mm_proxy, "DeviceAdded",
	                         DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (info->mm_proxy, "DeviceAdded",
								 G_CALLBACK (modem_added), info,
								 NULL);

	dbus_g_proxy_add_signal (info->mm_proxy, "DeviceRemoved",
	                         DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (info->mm_proxy, "DeviceRemoved",
								 G_CALLBACK (modem_removed), info,
								 NULL);

	/* Bluez stuff */
	info->bluez_proxy = dbus_g_proxy_new_for_name (info->bus,
	                                               BLUEZ_SERVICE,
	                                               BLUEZ_MANAGER_PATH,
	                                               BLUEZ_MANAGER_INTERFACE);
	g_assert (info->bluez_proxy);

	g_message ("%s: calling DefaultAdapter...", __func__);
	dbus_g_proxy_begin_call (info->bluez_proxy, "DefaultAdapter",
	                         default_adapter_cb,
	                         info,
	                         NULL, G_TYPE_INVALID);

	info->dun_timeout_id = g_timeout_add_seconds (30, dun_timeout_cb, info);

out:
	g_message ("%s: finished", __func__);
}

/*******************************************************************/

static void
delete_cb (NMSettingsConnectionInterface *connection,
           GError *error,
           gpointer user_data)
{
	if (error) {
		g_warning ("Error deleting connection: (%d) %s",
		           error ? error->code : -1,
		           error && error->message ? error->message : "(unknown)");
	}
}

static void
button_toggled (GtkToggleButton *button, gpointer user_data)
{
	PluginInfo *info = user_data;

	if (gtk_toggle_button_get_active (button) == FALSE) {
		nm_settings_connection_interface_delete (info->connection, delete_cb, NULL);
		info->connection = NULL;
	} else {
		if (info->method == BT_METHOD_PAN)
			info->connection = add_pan_connection (info->bdaddr);
		else if (info->method == BT_METHOD_DUN)
			dun_start (info);
	}
}

static NMSettingsConnectionInterface *
get_connection_for_bdaddr (const char *bdaddr, BtMethod method)
{
	NMSettingsConnectionInterface *found = NULL;
	NMSettingsInterface *settings;
	GSList *list, *l;
	GByteArray *array;

	array = get_array_from_bdaddr (bdaddr);
	if (array == NULL)
		return NULL;

	settings = NM_SETTINGS_INTERFACE (nma_gconf_settings_new (NULL));
	list = nm_settings_interface_list_connections (settings);
	for (l = list; l != NULL; l = l->next) {
		NMSettingsConnectionInterface *candidate = l->data;
		NMSetting *setting;
		const char *type;
		const GByteArray *addr;

		setting = nm_connection_get_setting_by_name (NM_CONNECTION (candidate), NM_SETTING_BLUETOOTH_SETTING_NAME);
		if (setting == NULL)
			continue;

		type = nm_setting_bluetooth_get_connection_type (NM_SETTING_BLUETOOTH (setting));
		if (method == BT_METHOD_PAN) {
			if (g_strcmp0 (type, NM_SETTING_BLUETOOTH_TYPE_PANU) != 0)
				continue;
		} else if (method == BT_METHOD_DUN) {
			if (g_strcmp0 (type, NM_SETTING_BLUETOOTH_TYPE_DUN) != 0)
				continue;
		}

		addr = nm_setting_bluetooth_get_bdaddr (NM_SETTING_BLUETOOTH (setting));
		if (addr == NULL || memcmp (addr->data, array->data, addr->len) != 0)
			continue;
		found = candidate;
		break;
	}
	g_slist_free (list);

	// FIXME: we intentionally don't free 'settings' here because that does bad things
	return found;
}

static void
plugin_info_destroy (gpointer data)
{
	PluginInfo *info = data;

	g_free (info->bdaddr);
	g_free (info->rfcomm_iface);
	if (info->connection)
		g_object_unref (info->connection);
	if (info->spinner)
		bling_spinner_stop (BLING_SPINNER (info->spinner));
	memset (info, 0, sizeof (PluginInfo));
	g_free (info);
}

static GtkWidget *
get_config_widgets (const char *bdaddr, const char **uuids)
{
	PluginInfo *info;
	GtkWidget *vbox, *hbox;
	BtMethod method;

	method = get_best_method (bdaddr, uuids);
	if (method == BT_METHOD_UNKNOWN)
		return NULL;

	info = g_malloc0 (sizeof (PluginInfo));
	info->bdaddr = g_strdup (bdaddr);
	info->method = method;
	info->connection = get_connection_for_bdaddr (bdaddr, method);

	vbox = gtk_vbox_new (FALSE, 6);
	g_object_set_data_full (G_OBJECT (vbox), "info", info, plugin_info_destroy);

	info->button = gtk_check_button_new_with_label (_("Access the Internet using your mobile phone"));
	if (info->connection)
		gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (info->button), TRUE);

	info->toggled_id = g_signal_connect (G_OBJECT (info->button), "toggled", G_CALLBACK (button_toggled), info);

	gtk_box_pack_start (GTK_BOX (vbox), info->button, FALSE, TRUE, 6);

	hbox = gtk_hbox_new (FALSE, 6);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, TRUE, 6);

	/* Spinner's hbox */
	info->hbox = gtk_hbox_new (FALSE, 6);
	gtk_box_pack_start (GTK_BOX (hbox), info->hbox, FALSE, FALSE, 0);

	info->label = gtk_label_new ("");
	gtk_box_pack_start (GTK_BOX (hbox), info->label, FALSE, TRUE, 6);

	return vbox;
}

static void
device_removed (const char *bdaddr)
{
	NMSettingsConnectionInterface *connection;

	g_message ("Device '%s' got removed", bdaddr);

	// FIXME: don't just delete any random PAN conenction for this
	// bdaddr, actually delete the one this plugin created
	do {
		connection = get_connection_for_bdaddr (bdaddr, BT_METHOD_UNKNOWN);
		if (connection)
			nm_settings_connection_interface_delete (connection, delete_cb, NULL);
	} while (connection);
}

static GbtPluginInfo plugin_info = {
	"network-manager-applet",
	has_config_widget,
	get_config_widgets,
	device_removed
};

GBT_INIT_PLUGIN(plugin_info)

