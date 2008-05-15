/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
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

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hidp.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "logging.h"

#include "device.h"
#include "server.h"
#include "storage.h"
#include "dbus-service.h"
#include "glib-helper.h"

static const char* HID_UUID = "00001124-0000-1000-8000-00805f9b34fb";

static DBusConnection *connection = NULL;

static void cancel_authorization(const char *addr)
{
	DBusMessage *msg;

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
						"org.bluez.Database",
						"CancelAuthorizationRequest");
	if (!msg) {
		error("Unable to allocate new method call");
		return;
	}

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &addr,
			DBUS_TYPE_STRING, &HID_UUID,
			DBUS_TYPE_INVALID);

	send_message_and_unref(connection, msg);
}

struct authorization_data {
	bdaddr_t src;
	bdaddr_t dst;
};

static void authorization_callback(DBusPendingCall *pcall, void *data)
{
	struct authorization_data *auth = data;
	DBusMessage *reply = dbus_pending_call_steal_reply(pcall);
	DBusError derr;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply) != TRUE) {
		dbus_message_unref(reply);
		input_device_connadd(&auth->src, &auth->dst);
		return;
	}

	error("Authorization denied: %s", derr.message);
	if (dbus_error_has_name(&derr, DBUS_ERROR_NO_REPLY)) {
		char addr[18];
		memset(addr, 0, sizeof(addr));
		ba2str(&auth->dst, addr);
		cancel_authorization(addr);
	}

	input_device_close_channels(&auth->src, &auth->dst);

	dbus_error_free(&derr);
	dbus_message_unref(reply);
}

static void auth_callback(DBusError *derr, void *user_data)
{
	struct authorization_data *auth = user_data;

	if (derr) {
		error("Access denied: %s", derr->message);
		if (dbus_error_has_name(derr, DBUS_ERROR_NO_REPLY))
			service_cancel_auth(&auth->dst);

		input_device_close_channels(&auth->src, &auth->dst);
	} else
		input_device_connadd(&auth->src, &auth->dst);

	g_free(auth);
}

static int authorize_device(bdaddr_t *src, bdaddr_t *dst)
{
	struct authorization_data *auth;
	DBusMessage *msg;
	DBusPendingCall *pending;
	char addr[18];
	const char *paddr = addr;
	int retval;

	auth = g_new0(struct authorization_data, 1);
	bacpy(&auth->src, src);
	bacpy(&auth->dst, dst);

	retval = service_req_auth(src, dst, HID_UUID,
				auth_callback, auth);
	if (retval < 0)
		goto fallback;

	return retval;

fallback:
	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
				"org.bluez.Database", "RequestAuthorization");
	if (!msg) {
		error("Unable to allocate new RequestAuthorization method call");
		return -ENOMEM;
	}

	memset(addr, 0, sizeof(addr));
	ba2str(dst, addr);
	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &paddr,
			DBUS_TYPE_STRING, &HID_UUID,
			DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(connection,
				msg, &pending, -1) == FALSE)
		return -EACCES;

	dbus_pending_call_set_notify(pending, authorization_callback, auth, g_free);
	dbus_pending_call_unref(pending);
	dbus_message_unref(msg);

	return 0;
}

static void connect_event_cb(GIOChannel *chan, int err, const bdaddr_t *src,
				const bdaddr_t *dst, gpointer data)
{
	int sk, psm = GPOINTER_TO_UINT(data);

	if (err < 0) {
		error("accept: %s (%d)", strerror(-err), -err);
		return;
	}

	sk = g_io_channel_unix_get_fd(chan);

	debug("Incoming connection on PSM %d", psm);

	if (input_device_set_channel(src, dst, psm, sk) < 0) {
		/* Send unplug virtual cable to unknown devices */
		if (psm == L2CAP_PSM_HIDP_CTRL) {
			unsigned char unplug[] = { 0x15 };
			int err;
			err = write(sk, unplug, sizeof(unplug));
		}
		close(sk);
		return;
	}

	if ((psm == L2CAP_PSM_HIDP_INTR) && (authorize_device(src, dst) < 0))
		input_device_close_channels(src, dst);

	return;
}

static GIOChannel *ctrl_io = NULL;
static GIOChannel *intr_io = NULL;

int server_start(DBusConnection *conn)
{
	ctrl_io = bt_l2cap_listen(BDADDR_ANY, L2CAP_PSM_HIDP_CTRL, 0, 0,
				connect_event_cb,
				GUINT_TO_POINTER(L2CAP_PSM_HIDP_CTRL));
	if (!ctrl_io) {
		error("Failed to listen on control channel");
		return -1;
	}
	g_io_channel_set_close_on_unref(ctrl_io, TRUE);

	intr_io = bt_l2cap_listen(BDADDR_ANY, L2CAP_PSM_HIDP_INTR, 0, 0,
				connect_event_cb,
				GUINT_TO_POINTER(L2CAP_PSM_HIDP_INTR));
	if (!intr_io) {
		error("Failed to listen on interrupt channel");
		g_io_channel_unref(ctrl_io);
		ctrl_io = NULL;
	}
	g_io_channel_set_close_on_unref(intr_io, TRUE);

	connection = conn;

	return 0;
}

void server_stop(void)
{
	if (intr_io)
		g_io_channel_unref(intr_io);

	if (ctrl_io)
		g_io_channel_unref(ctrl_io);
}
