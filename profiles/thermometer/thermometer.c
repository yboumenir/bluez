/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011 GSyC/LibreSoft, Universidad Rey Juan Carlos.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <errno.h>

#include <bluetooth/uuid.h>

#include <gdbus.h>

#include "dbus-common.h"
#include "adapter.h"
#include "device.h"
#include "error.h"
#include "log.h"
#include "gattrib.h"
#include "attio.h"
#include "att.h"
#include "gatt.h"
#include "thermometer.h"

#define THERMOMETER_INTERFACE		"org.bluez.Thermometer"
#define THERMOMETER_MANAGER_INTERFACE	"org.bluez.ThermometerManager"
#define THERMOMETER_WATCHER_INTERFACE	"org.bluez.ThermometerWatcher"

/* Temperature measurement flag fields */
#define TEMP_UNITS		0x01
#define TEMP_TIME_STAMP		0x02
#define TEMP_TYPE		0x04

#define FLOAT_MAX_MANTISSA	16777216 /* 2^24 */

#define VALID_RANGE_DESC_SIZE	4
#define TEMPERATURE_TYPE_SIZE	1
#define MEASUREMENT_INTERVAL_SIZE	2

struct thermometer_adapter {
	struct btd_adapter	*adapter;
	GSList			*devices;
	GSList			*fwatchers;	/* Final measurements */
	GSList			*iwatchers;	/* Intermediate measurements */
};

struct thermometer {
	struct btd_device		*dev;		/* Device reference */
	struct thermometer_adapter	*tadapter;
	GAttrib				*attrib;	/* GATT connection */
	struct att_range		*svc_range;	/* Thermometer range */
	guint				attioid;	/* Att watcher id */
	guint				attindid;	/* Att incications id */
	guint				attnotid;	/* Att notif id */
	GSList				*chars;		/* Characteristics */
	gboolean			intermediate;
	uint8_t				type;
	uint16_t			interval;
	uint16_t			max;
	uint16_t			min;
	gboolean			has_type;
	gboolean			has_interval;
};

struct characteristic {
	struct gatt_char	attr;	/* Characteristic */
	GSList			*desc;	/* Descriptors */
	struct thermometer	*t;	/* Thermometer where the char belongs */
};

struct descriptor {
	struct characteristic	*ch;
	uint16_t		handle;
	bt_uuid_t		uuid;
};

struct watcher {
	struct thermometer_adapter	*tadapter;
	guint				id;
	char				*srv;
	char				*path;
};

struct measurement {
	struct thermometer	*t;
	int16_t			exp;
	int32_t			mant;
	uint64_t		time;
	gboolean		suptime;
	char			*unit;
	char			*type;
	char			*value;
};

struct tmp_interval_data {
	struct thermometer	*thermometer;
	uint16_t		interval;
};

static GSList *thermometer_adapters = NULL;

const char *temp_type[] = {
	"<reserved>",
	"armpit",
	"body",
	"ear",
	"finger",
	"intestines",
	"mouth",
	"rectum",
	"toe",
	"tympanum"
};

static const gchar *temptype2str(uint8_t value)
{
	 if (value > 0 && value < G_N_ELEMENTS(temp_type))
		return temp_type[value];

	error("Temperature type %d reserved for future use", value);
	return NULL;
}

static void destroy_watcher(gpointer user_data)
{
	struct watcher *watcher = user_data;

	g_free(watcher->path);
	g_free(watcher->srv);
	g_free(watcher);
}

static void remove_watcher(gpointer user_data)
{
	struct watcher *watcher = user_data;

	g_dbus_remove_watch(btd_get_dbus_connection(), watcher->id);
}

static void destroy_char(gpointer user_data)
{
	struct characteristic *c = user_data;

	g_slist_free_full(c->desc, g_free);
	g_free(c);
}

static void destroy_thermometer(gpointer user_data)
{
	struct thermometer *t = user_data;

	if (t->attioid > 0)
		btd_device_remove_attio_callback(t->dev, t->attioid);

	if (t->attindid > 0)
		g_attrib_unregister(t->attrib, t->attindid);

	if (t->attnotid > 0)
		g_attrib_unregister(t->attrib, t->attnotid);

	if (t->attrib != NULL)
		g_attrib_unref(t->attrib);

	if (t->chars != NULL)
		g_slist_free_full(t->chars, destroy_char);

	btd_device_unref(t->dev);
	g_free(t->svc_range);
	g_free(t);
}

static void destroy_thermometer_adapter(gpointer user_data)
{
	struct thermometer_adapter *tadapter = user_data;

	if (tadapter->devices != NULL)
		g_slist_free_full(tadapter->devices, destroy_thermometer);

	if (tadapter->fwatchers != NULL)
		g_slist_free_full(tadapter->fwatchers, remove_watcher);

	g_free(tadapter);
}

static gint cmp_adapter(gconstpointer a, gconstpointer b)
{
	const struct thermometer_adapter *tadapter = a;
	const struct btd_adapter *adapter = b;

	if (adapter == tadapter->adapter)
		return 0;

	return -1;
}

static gint cmp_device(gconstpointer a, gconstpointer b)
{
	const struct thermometer *t = a;
	const struct btd_device *dev = b;

	if (dev == t->dev)
		return 0;

	return -1;
}

static gint cmp_watcher(gconstpointer a, gconstpointer b)
{
	const struct watcher *watcher = a;
	const struct watcher *match = b;
	int ret;

	ret = g_strcmp0(watcher->srv, match->srv);
	if (ret != 0)
		return ret;

	return g_strcmp0(watcher->path, match->path);
}

static gint cmp_char_uuid(gconstpointer a, gconstpointer b)
{
	const struct characteristic *ch = a;
	const char *uuid = b;

	return g_strcmp0(ch->attr.uuid, uuid);
}

static gint cmp_char_val_handle(gconstpointer a, gconstpointer b)
{
	const struct characteristic *ch = a;
	const uint16_t *handle = b;

	return ch->attr.value_handle - *handle;
}

static gint cmp_descriptor(gconstpointer a, gconstpointer b)
{
	const struct descriptor *desc = a;
	const bt_uuid_t *uuid = b;

	return bt_uuid_cmp(&desc->uuid, uuid);
}

static struct thermometer_adapter *
find_thermometer_adapter(struct btd_adapter *adapter)
{
	GSList *l = g_slist_find_custom(thermometer_adapters, adapter,
								cmp_adapter);
	if (!l)
		return NULL;

	return l->data;
}

static struct characteristic *get_characteristic(struct thermometer *t,
							const char *uuid)
{
	GSList *l;

	l = g_slist_find_custom(t->chars, uuid, cmp_char_uuid);
	if (l == NULL)
		return NULL;

	return l->data;
}

static struct descriptor *get_descriptor(struct characteristic *ch,
							const bt_uuid_t *uuid)
{
	GSList *l;

	l = g_slist_find_custom(ch->desc, uuid, cmp_descriptor);
	if (l == NULL)
		return NULL;

	return l->data;
}

static void change_property(struct thermometer *t, const char *name,
							gpointer value) {
	if (g_strcmp0(name, "Intermediate") == 0) {
		gboolean *intermediate = value;
		if (t->intermediate == *intermediate)
			return;

		t->intermediate = *intermediate;
		emit_property_changed(device_get_path(t->dev),
					THERMOMETER_INTERFACE, name,
					DBUS_TYPE_BOOLEAN, &t->intermediate);
	} else if (g_strcmp0(name, "Interval") == 0) {
		uint16_t *interval = value;
		if (t->has_interval && t->interval == *interval)
			return;

		t->has_interval = TRUE;
		t->interval = *interval;
		emit_property_changed(device_get_path(t->dev),
					THERMOMETER_INTERFACE, name,
					DBUS_TYPE_UINT16, &t->interval);
	} else if (g_strcmp0(name, "Maximum") == 0) {
		uint16_t *max = value;
		if (t->max == *max)
			return;

		t->max = *max;
		emit_property_changed(device_get_path(t->dev),
					THERMOMETER_INTERFACE, name,
					DBUS_TYPE_UINT16, &t->max);
	} else if (g_strcmp0(name, "Minimum") == 0) {
		uint16_t *min = value;
		if (t->min == *min)
			return;

		t->min = *min;
		emit_property_changed(device_get_path(t->dev),
					THERMOMETER_INTERFACE, name,
					DBUS_TYPE_UINT16, &t->min);
	} else {
		DBG("%s is not a thermometer property", name);
	}
}

static void valid_range_desc_cb(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct descriptor *desc = user_data;
	uint8_t value[VALID_RANGE_DESC_SIZE];
	uint16_t max, min;
	ssize_t vlen;

	if (status != 0) {
		DBG("Valid Range descriptor read failed: %s",
							att_ecode2str(status));
		return;
	}

	vlen = dec_read_resp(pdu, len, value, sizeof(value));
	if (vlen < 0) {
		DBG("Protocol error\n");
		return;
	}

	if (vlen < 4) {
		DBG("Invalid range received");
		return;
	}

	min = att_get_u16(&value[0]);
	max = att_get_u16(&value[2]);

	if (min == 0 || min > max) {
		DBG("Invalid range");
		return;
	}

	change_property(desc->ch->t, "Maximum", &max);
	change_property(desc->ch->t, "Minimum", &min);
}

static void write_ccc_cb(guint8 status, const guint8 *pdu,
						guint16 len, gpointer user_data)
{
	char *msg = user_data;

	if (status != 0)
		error("%s failed", msg);

	g_free(msg);
}

static void process_thermometer_desc(struct descriptor *desc)
{
	struct characteristic *ch = desc->ch;
	char uuidstr[MAX_LEN_UUID_STR];
	bt_uuid_t btuuid;

	bt_uuid16_create(&btuuid, GATT_CLIENT_CHARAC_CFG_UUID);

	if (bt_uuid_cmp(&desc->uuid, &btuuid) == 0) {
		uint8_t atval[2];
		uint16_t val;
		char *msg;

		if (g_strcmp0(ch->attr.uuid,
					TEMPERATURE_MEASUREMENT_UUID) == 0) {
			if (g_slist_length(ch->t->tadapter->fwatchers) == 0)
				return;

			val = GATT_CLIENT_CHARAC_CFG_IND_BIT;
			msg = g_strdup("Enable Temperature Measurement "
								"indication");
		} else if (g_strcmp0(ch->attr.uuid,
					INTERMEDIATE_TEMPERATURE_UUID) == 0) {
			if (g_slist_length(ch->t->tadapter->iwatchers) == 0)
				return;

			val = GATT_CLIENT_CHARAC_CFG_NOTIF_BIT;
			msg = g_strdup("Enable Intermediate Temperature "
								"notification");
		} else if (g_strcmp0(ch->attr.uuid,
					MEASUREMENT_INTERVAL_UUID) == 0) {
			val = GATT_CLIENT_CHARAC_CFG_IND_BIT;
			msg = g_strdup("Enable Measurement Interval "
								"indication");
		} else {
			goto done;
		}

		att_put_u16(val, atval);
		gatt_write_char(ch->t->attrib, desc->handle, atval, 2,
							write_ccc_cb, msg);
		return;
	}

	bt_uuid16_create(&btuuid, GATT_CHARAC_VALID_RANGE_UUID);

	if (bt_uuid_cmp(&desc->uuid, &btuuid) == 0 && g_strcmp0(ch->attr.uuid,
					MEASUREMENT_INTERVAL_UUID) == 0) {
		gatt_read_char(ch->t->attrib, desc->handle, valid_range_desc_cb,
									desc);
		return;
	}

done:
	bt_uuid_to_string(&desc->uuid, uuidstr, MAX_LEN_UUID_STR);
	DBG("Ignored descriptor %s in characteristic %s", uuidstr,
								ch->attr.uuid);
}

static void discover_desc_cb(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct characteristic *ch = user_data;
	struct att_data_list *list;
	uint8_t format;
	int i;

	if (status != 0) {
		error("Discover all characteristic descriptors failed [%s]: %s",
					ch->attr.uuid, att_ecode2str(status));
		return;
	}

	list = dec_find_info_resp(pdu, len, &format);
	if (list == NULL)
		return;

	for (i = 0; i < list->num; i++) {
		struct descriptor *desc;
		uint8_t *value;

		value = list->data[i];
		desc = g_new0(struct descriptor, 1);
		desc->handle = att_get_u16(value);
		desc->ch = ch;

		if (format == ATT_FIND_INFO_RESP_FMT_16BIT)
			desc->uuid = att_get_uuid16(&value[2]);
		else
			desc->uuid = att_get_uuid128(&value[2]);

		ch->desc = g_slist_append(ch->desc, desc);
		process_thermometer_desc(desc);
	}

	att_data_list_free(list);
}

static void read_temp_type_cb(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct characteristic *ch = user_data;
	struct thermometer *t = ch->t;
	uint8_t value[TEMPERATURE_TYPE_SIZE];
	ssize_t vlen;

	if (status != 0) {
		DBG("Temperature Type value read failed: %s",
							att_ecode2str(status));
		return;
	}

	vlen = dec_read_resp(pdu, len, value, sizeof(value));
	if (vlen < 0) {
		DBG("Protocol error.");
		return;
	}

	if (vlen != 1) {
		DBG("Invalid length for Temperature type");
		return;
	}

	t->has_type = TRUE;
	t->type = value[0];
}

static void read_interval_cb(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct characteristic *ch = user_data;
	uint8_t value[MEASUREMENT_INTERVAL_SIZE];
	uint16_t interval;
	ssize_t vlen;

	if (status != 0) {
		DBG("Measurement Interval value read failed: %s",
							att_ecode2str(status));
		return;
	}

	vlen = dec_read_resp(pdu, len, value, sizeof(value));
	if (vlen < 0) {
		DBG("Protocol error\n");
		return;
	}

	if (vlen < 2) {
		DBG("Invalid Interval received");
		return;
	}

	interval = att_get_u16(&value[0]);
	change_property(ch->t, "Interval", &interval);
}

static void process_thermometer_char(struct characteristic *ch)
{
	if (g_strcmp0(ch->attr.uuid, INTERMEDIATE_TEMPERATURE_UUID) == 0) {
		gboolean intermediate = TRUE;
		change_property(ch->t, "Intermediate", &intermediate);
		return;
	} else if (g_strcmp0(ch->attr.uuid, TEMPERATURE_TYPE_UUID) == 0) {
		gatt_read_char(ch->t->attrib, ch->attr.value_handle,
							read_temp_type_cb, ch);
	} else if (g_strcmp0(ch->attr.uuid, MEASUREMENT_INTERVAL_UUID) == 0) {
		gatt_read_char(ch->t->attrib, ch->attr.value_handle,
							read_interval_cb, ch);
	}
}

static void configure_thermometer_cb(GSList *characteristics, guint8 status,
							gpointer user_data)
{
	struct thermometer *t = user_data;
	GSList *l;

	if (status != 0) {
		error("Discover thermometer characteristics: %s",
							att_ecode2str(status));
		return;
	}

	for (l = characteristics; l; l = l->next) {
		struct gatt_char *c = l->data;
		struct characteristic *ch;
		uint16_t start, end;

		ch = g_new0(struct characteristic, 1);
		ch->attr.handle = c->handle;
		ch->attr.properties = c->properties;
		ch->attr.value_handle = c->value_handle;
		memcpy(ch->attr.uuid, c->uuid, MAX_LEN_UUID_STR + 1);
		ch->t = t;

		t->chars = g_slist_append(t->chars, ch);

		process_thermometer_char(ch);

		start = c->value_handle + 1;

		if (l->next != NULL) {
			struct gatt_char *c = l->next->data;
			if (start == c->handle)
				continue;
			end = c->handle - 1;
		} else if (c->value_handle != t->svc_range->end) {
			end = t->svc_range->end;
		} else {
			continue;
		}

		gatt_find_info(t->attrib, start, end, discover_desc_cb, ch);
	}
}

static DBusMessage *get_properties(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct thermometer *t = data;
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	dict_append_entry(&dict, "Intermediate", DBUS_TYPE_BOOLEAN,
							&t->intermediate);

	if (t->has_interval) {
		dict_append_entry(&dict, "Interval", DBUS_TYPE_UINT16,
								&t->interval);
		dict_append_entry(&dict, "Maximum", DBUS_TYPE_UINT16, &t->max);
		dict_append_entry(&dict, "Minimum", DBUS_TYPE_UINT16, &t->min);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void write_interval_cb(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct tmp_interval_data *data = user_data;

	if (status != 0) {
		error("Interval Write Request failed %s",
							att_ecode2str(status));
		goto done;
	}

	if (!dec_write_resp(pdu, len)) {
		error("Interval Write Request: protocol error");
		goto done;
	}

	change_property(data->thermometer, "Interval", &data->interval);

done:
	g_free(user_data);
}

static DBusMessage *write_attr_interval(struct thermometer *t, DBusMessage *msg,
								uint16_t value)
{
	struct tmp_interval_data *data;
	struct characteristic *ch;
	uint8_t atval[2];

	if (t->attrib == NULL)
		return btd_error_not_connected(msg);

	ch = get_characteristic(t, MEASUREMENT_INTERVAL_UUID);
	if (ch == NULL)
		return btd_error_not_available(msg);

	if (value < t->min || value > t->max)
		return btd_error_invalid_args(msg);

	att_put_u16(value, &atval[0]);

	data = g_new0(struct tmp_interval_data, 1);
	data->thermometer = t;
	data->interval = value;
	gatt_write_char(t->attrib, ch->attr.value_handle, atval, 2,
						write_interval_cb, data);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_property(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct thermometer *t = data;
	const char *property;
	DBusMessageIter iter;
	DBusMessageIter sub;
	uint16_t value;

	if (!dbus_message_iter_init(msg, &iter))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	if (g_strcmp0("Interval", property) != 0)
		return btd_error_invalid_args(msg);

	if (!t->has_interval)
		return btd_error_not_available(msg);

	dbus_message_iter_next(&iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return btd_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &sub);

	if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_UINT16)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&sub, &value);

	return write_attr_interval(t, msg, value);
}

static void write_ccc(struct thermometer *t, const char *uuid, uint16_t value)
{
	struct characteristic *ch;
	struct descriptor *desc;
	bt_uuid_t btuuid;
	uint8_t atval[2];
	char *msg;

	if (t->attrib == NULL)
		return;

	ch = get_characteristic(t, uuid);
	if (ch == NULL) {
		DBG("Characteristic %s not found", uuid);
		return;
	}

	bt_uuid16_create(&btuuid, GATT_CLIENT_CHARAC_CFG_UUID);
	desc = get_descriptor(ch, &btuuid);
	if (desc == NULL) {
		DBG("CCC descriptor for %s not found", uuid);
		return;
	}

	att_put_u16(value, atval);

	msg = g_strdup_printf("Write CCC: %04x for %s", value, uuid);

	gatt_write_char(t->attrib, desc->handle, atval, sizeof(atval),
							write_ccc_cb, msg);
}

static void enable_final_measurement(gpointer data, gpointer user_data)
{
	struct thermometer *t = data;

	write_ccc(t, TEMPERATURE_MEASUREMENT_UUID,
					GATT_CLIENT_CHARAC_CFG_IND_BIT);
}

static void enable_intermediate_measurement(gpointer data, gpointer user_data)
{
	struct thermometer *t = data;

	write_ccc(t, INTERMEDIATE_TEMPERATURE_UUID,
					GATT_CLIENT_CHARAC_CFG_NOTIF_BIT);
}

static void disable_final_measurement(gpointer data, gpointer user_data)
{
	struct thermometer *t = data;

	write_ccc(t, TEMPERATURE_MEASUREMENT_UUID, 0x0000);
}

static void disable_intermediate_measurement(gpointer data, gpointer user_data)
{
	struct thermometer *t = data;

	write_ccc(t, INTERMEDIATE_TEMPERATURE_UUID, 0x0000);
}

static void remove_int_watcher(struct thermometer_adapter *tadapter,
							struct watcher *w)
{
	if (!g_slist_find(tadapter->iwatchers, w))
		return;

	tadapter->iwatchers = g_slist_remove(tadapter->iwatchers, w);

	if (g_slist_length(tadapter->iwatchers) == 0)
		g_slist_foreach(tadapter->devices,
					disable_intermediate_measurement, 0);
}

static void watcher_exit(DBusConnection *conn, void *user_data)
{
	struct watcher *watcher = user_data;
	struct thermometer_adapter *tadapter = watcher->tadapter;

	DBG("Thermometer watcher %s disconnected", watcher->path);

	remove_int_watcher(tadapter, watcher);

	tadapter->fwatchers = g_slist_remove(tadapter->fwatchers, watcher);
	g_dbus_remove_watch(btd_get_dbus_connection(), watcher->id);

	if (g_slist_length(tadapter->fwatchers) == 0)
		g_slist_foreach(tadapter->devices,
					disable_final_measurement, 0);
}

static struct watcher *find_watcher(GSList *list, const char *sender,
							const char *path)
{
	struct watcher *match;
	GSList *l;

	match = g_new0(struct watcher, 1);
	match->srv = g_strdup(sender);
	match->path = g_strdup(path);

	l = g_slist_find_custom(list, match, cmp_watcher);
	destroy_watcher(match);

	if (l != NULL)
		return l->data;

	return NULL;
}

static DBusMessage *register_watcher(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct thermometer_adapter *tadapter = data;
	struct watcher *watcher;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	watcher = find_watcher(tadapter->fwatchers, sender, path);
	if (watcher != NULL)
		return btd_error_already_exists(msg);

	DBG("Thermometer watcher %s registered", path);

	watcher = g_new0(struct watcher, 1);
	watcher->srv = g_strdup(sender);
	watcher->path = g_strdup(path);
	watcher->tadapter = tadapter;
	watcher->id = g_dbus_add_disconnect_watch(conn, sender, watcher_exit,
						watcher, destroy_watcher);

	if (g_slist_length(tadapter->fwatchers) == 0)
		g_slist_foreach(tadapter->devices, enable_final_measurement, 0);

	tadapter->fwatchers = g_slist_prepend(tadapter->fwatchers, watcher);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *unregister_watcher(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct thermometer_adapter *tadapter = data;
	struct watcher *watcher;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	watcher = find_watcher(tadapter->fwatchers, sender, path);
	if (watcher == NULL)
		return btd_error_does_not_exist(msg);

	DBG("Thermometer watcher %s unregistered", path);

	remove_int_watcher(tadapter, watcher);

	tadapter->fwatchers = g_slist_remove(tadapter->fwatchers, watcher);
	g_dbus_remove_watch(btd_get_dbus_connection(), watcher->id);

	if (g_slist_length(tadapter->fwatchers) == 0)
		g_slist_foreach(tadapter->devices,
					disable_final_measurement, 0);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *enable_intermediate(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct thermometer_adapter *ta = data;
	struct watcher *watcher;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	watcher = find_watcher(ta->fwatchers, sender, path);
	if (watcher == NULL)
		return btd_error_does_not_exist(msg);

	if (find_watcher(ta->iwatchers, sender, path))
		return btd_error_already_exists(msg);

	DBG("Intermediate measurement watcher %s registered", path);

	if (g_slist_length(ta->iwatchers) == 0)
		g_slist_foreach(ta->devices,
					enable_intermediate_measurement, 0);

	ta->iwatchers = g_slist_prepend(ta->iwatchers, watcher);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *disable_intermediate(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct thermometer_adapter *ta = data;
	struct watcher *watcher;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	watcher = find_watcher(ta->iwatchers, sender, path);
	if (watcher == NULL)
		return btd_error_does_not_exist(msg);

	DBG("Intermediate measurement %s unregistered", path);

	remove_int_watcher(ta, watcher);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable thermometer_methods[] = {
	{ GDBUS_METHOD("GetProperties",
			NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
			get_properties) },
	{ GDBUS_ASYNC_METHOD("SetProperty",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" }), NULL,
			set_property) },
	{ }
};

static const GDBusSignalTable thermometer_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

static void update_watcher(gpointer data, gpointer user_data)
{
	struct watcher *w = data;
	struct measurement *m = user_data;
	const gchar *path = device_get_path(m->t->dev);
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessage *msg;

	msg = dbus_message_new_method_call(w->srv, w->path,
				THERMOMETER_WATCHER_INTERFACE,
				"MeasurementReceived");
	if (msg == NULL)
		return;

	dbus_message_iter_init_append(msg, &iter);

	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH , &path);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	dict_append_entry(&dict, "Exponent", DBUS_TYPE_INT16, &m->exp);
	dict_append_entry(&dict, "Mantissa", DBUS_TYPE_INT32, &m->mant);
	dict_append_entry(&dict, "Unit", DBUS_TYPE_STRING, &m->unit);

	if (m->suptime)
		dict_append_entry(&dict, "Time", DBUS_TYPE_UINT64, &m->time);

	dict_append_entry(&dict, "Type", DBUS_TYPE_STRING, &m->type);
	dict_append_entry(&dict, "Measurement", DBUS_TYPE_STRING, &m->value);

	dbus_message_iter_close_container(&iter, &dict);

	dbus_message_set_no_reply(msg, TRUE);
	g_dbus_send_message(btd_get_dbus_connection(), msg);
}

static void recv_measurement(struct thermometer *t, struct measurement *m)
{
	GSList *wlist;

	m->t = t;

	if (g_strcmp0(m->value, "intermediate") == 0)
		wlist = t->tadapter->iwatchers;
	else
		wlist = t->tadapter->fwatchers;

	g_slist_foreach(wlist, update_watcher, m);
}

static void proc_measurement(struct thermometer *t, const uint8_t *pdu,
						uint16_t len, gboolean final)
{
	struct measurement m;
	const char *type = NULL;
	uint8_t flags;
	uint32_t raw;

	/* skip opcode and handle */
	pdu += 3;
	len -= 3;

	if (len < 1) {
		DBG("Mandatory flags are not provided");
		return;
	}

	memset(&m, 0, sizeof(m));

	flags = *pdu;

	if (flags & TEMP_UNITS)
		m.unit = "fahrenheit";
	else
		m.unit = "celsius";

	pdu++;
	len--;

	if (len < 4) {
		DBG("Mandatory temperature measurement value is not provided");
		return;
	}

	raw = att_get_u32(pdu);
	m.mant = raw & 0x00FFFFFF;
	m.exp = ((int32_t) raw) >> 24;

	if (m.mant & 0x00800000) {
		/* convert to C2 negative value */
		m.mant = m.mant - FLOAT_MAX_MANTISSA;
	}

	pdu += 4;
	len -= 4;

	if (flags & TEMP_TIME_STAMP) {
		struct tm ts;
		time_t time;

		if (len < 7) {
			DBG("Time stamp is not provided");
			return;
		}

		ts.tm_year = att_get_u16(pdu) - 1900;
		ts.tm_mon = *(pdu + 2) - 1;
		ts.tm_mday = *(pdu + 3);
		ts.tm_hour = *(pdu + 4);
		ts.tm_min = *(pdu + 5);
		ts.tm_sec = *(pdu + 6);
		ts.tm_isdst = -1;

		time = mktime(&ts);
		m.time = (uint64_t) time;
		m.suptime = TRUE;

		pdu += 7;
		len -= 7;
	}

	if (flags & TEMP_TYPE) {
		if (len < 1) {
			DBG("Temperature type is not provided");
			return;
		}

		type = temptype2str(*pdu);
	} else if (t->has_type) {
		type = temptype2str(t->type);
	}

	m.type = g_strdup(type);
	m.value = final ? "final" : "intermediate";

	recv_measurement(t, &m);
	g_free(m.type);
}

static void proc_measurement_interval(struct thermometer *t, const uint8_t *pdu,
								uint16_t len)
{
	uint16_t interval;

	if (len < 5) {
		DBG("Measurement interval value is not provided");
		return;
	}

	interval = att_get_u16(&pdu[3]);

	change_property(t, "Interval", &interval);
}

static void ind_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	struct thermometer *t = user_data;
	const struct characteristic *ch;
	uint8_t *opdu;
	uint16_t handle, olen;
	GSList *l;
	size_t plen;

	if (len < 3) {
		DBG("Bad pdu received");
		return;
	}

	handle = att_get_u16(&pdu[1]);
	l = g_slist_find_custom(t->chars, &handle, cmp_char_val_handle);
	if (l == NULL) {
		DBG("Unexpected handle: 0x%04x", handle);
		return;
	}

	ch = l->data;

	if (g_strcmp0(ch->attr.uuid, TEMPERATURE_MEASUREMENT_UUID) == 0)
		proc_measurement(t, pdu, len, TRUE);
	else if (g_strcmp0(ch->attr.uuid, MEASUREMENT_INTERVAL_UUID) == 0)
		proc_measurement_interval(t, pdu, len);

	opdu = g_attrib_get_buffer(t->attrib, &plen);
	olen = enc_confirmation(opdu, plen);

	if (olen > 0)
		g_attrib_send(t->attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void notif_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	struct thermometer *t = user_data;
	const struct characteristic *ch;
	uint16_t handle;
	GSList *l;

	if (len < 3) {
		DBG("Bad pdu received");
		return;
	}

	handle = att_get_u16(&pdu[1]);
	l = g_slist_find_custom(t->chars, &handle, cmp_char_val_handle);
	if (l == NULL) {
		DBG("Unexpected handle: 0x%04x", handle);
		return;
	}

	ch = l->data;
	if (g_strcmp0(ch->attr.uuid, INTERMEDIATE_TEMPERATURE_UUID) == 0)
		proc_measurement(t, pdu, len, FALSE);
}

static void attio_connected_cb(GAttrib *attrib, gpointer user_data)
{
	struct thermometer *t = user_data;

	t->attrib = g_attrib_ref(attrib);

	t->attindid = g_attrib_register(t->attrib, ATT_OP_HANDLE_IND,
							GATTRIB_ALL_HANDLES,
							ind_handler, t, NULL);
	t->attnotid = g_attrib_register(t->attrib, ATT_OP_HANDLE_NOTIFY,
							GATTRIB_ALL_HANDLES,
							notif_handler, t, NULL);
	gatt_discover_char(t->attrib, t->svc_range->start, t->svc_range->end,
					NULL, configure_thermometer_cb, t);
}

static void attio_disconnected_cb(gpointer user_data)
{
	struct thermometer *t = user_data;

	DBG("GATT Disconnected");

	if (t->attindid > 0) {
		g_attrib_unregister(t->attrib, t->attindid);
		t->attindid = 0;
	}

	if (t->attnotid > 0) {
		g_attrib_unregister(t->attrib, t->attnotid);
		t->attnotid = 0;
	}

	g_attrib_unref(t->attrib);
	t->attrib = NULL;
}

int thermometer_register(struct btd_device *device, struct gatt_primary *tattr)
{
	const gchar *path = device_get_path(device);
	struct thermometer *t;
	struct btd_adapter *adapter;
	struct thermometer_adapter *tadapter;

	adapter = device_get_adapter(device);

	tadapter = find_thermometer_adapter(adapter);

	if (tadapter == NULL)
		return -1;

	t = g_new0(struct thermometer, 1);
	t->dev = btd_device_ref(device);
	t->tadapter = tadapter;
	t->svc_range = g_new0(struct att_range, 1);
	t->svc_range->start = tattr->range.start;
	t->svc_range->end = tattr->range.end;

	tadapter->devices = g_slist_prepend(tadapter->devices, t);

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
				path, THERMOMETER_INTERFACE,
				thermometer_methods, thermometer_signals,
				NULL, t, destroy_thermometer)) {
		error("D-Bus failed to register %s interface",
							THERMOMETER_INTERFACE);
		destroy_thermometer(t);
		return -EIO;
	}

	t->attioid = btd_device_add_attio_callback(device, attio_connected_cb,
						attio_disconnected_cb, t);
	return 0;
}

void thermometer_unregister(struct btd_device *device)
{
	struct thermometer *t;
	struct btd_adapter *adapter;
	struct thermometer_adapter *tadapter;
	GSList *l;

	adapter = device_get_adapter(device);

	tadapter = find_thermometer_adapter(adapter);

	if (tadapter == NULL)
		return;

	l = g_slist_find_custom(tadapter->devices, device, cmp_device);
	if (l == NULL)
		return;

	t = l->data;

	tadapter->devices = g_slist_remove(tadapter->devices, t);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				device_get_path(t->dev), THERMOMETER_INTERFACE);
}

static const GDBusMethodTable thermometer_manager_methods[] = {
	{ GDBUS_METHOD("RegisterWatcher",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			register_watcher) },
	{ GDBUS_METHOD("UnregisterWatcher",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			unregister_watcher) },
	{ GDBUS_METHOD("EnableIntermediateMeasurement",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			enable_intermediate) },
	{ GDBUS_METHOD("DisableIntermediateMeasurement",
			GDBUS_ARGS({ "agent", "o" }), NULL,
			disable_intermediate) },
	{ }
};

int thermometer_adapter_register(struct btd_adapter *adapter)
{
	struct thermometer_adapter *tadapter;

	tadapter = g_new0(struct thermometer_adapter, 1);
	tadapter->adapter = adapter;

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
						adapter_get_path(adapter),
						THERMOMETER_MANAGER_INTERFACE,
						thermometer_manager_methods,
						NULL, NULL, tadapter,
						destroy_thermometer_adapter)) {
		error("D-Bus failed to register %s interface",
						THERMOMETER_MANAGER_INTERFACE);
		destroy_thermometer_adapter(tadapter);
		return -EIO;
	}

	thermometer_adapters = g_slist_prepend(thermometer_adapters, tadapter);

	return 0;
}

void thermometer_adapter_unregister(struct btd_adapter *adapter)
{
	struct thermometer_adapter *tadapter;

	tadapter = find_thermometer_adapter(adapter);
	if (tadapter == NULL)
		return;

	thermometer_adapters = g_slist_remove(thermometer_adapters, tadapter);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
					adapter_get_path(tadapter->adapter),
					THERMOMETER_MANAGER_INTERFACE);
}
