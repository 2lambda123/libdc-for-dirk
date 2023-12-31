/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include "suunto_eon.h"
#include "suunto_common.h"
#include "context-private.h"
#include "device-private.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &suunto_eon_device_vtable)

#define SZ_MEMORY 0x900

typedef struct suunto_eon_device_t {
	suunto_common_device_t base;
	dc_iostream_t *iostream;
} suunto_eon_device_t;

static dc_status_t suunto_eon_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t suunto_eon_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t suunto_eon_device_vtable = {
	sizeof(suunto_eon_device_t),
	DC_FAMILY_SUUNTO_EON,
	suunto_common_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	suunto_eon_device_dump, /* dump */
	suunto_eon_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};

static const suunto_common_layout_t suunto_eon_layout = {
	0, /* eop */
	0x100, /* rb_profile_begin */
	SZ_MEMORY, /* rb_profile_end */
	6, /* fp_offset */
	3 /* peek */
};


dc_status_t
suunto_eon_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (suunto_eon_device_t *) dc_device_allocate (context, &suunto_eon_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	suunto_common_device_init (&device->base);

	// Set the default values.
	device->iostream = iostream;

	// Set the serial communication protocol (1200 8N2).
	status = dc_iostream_configure (device->iostream, 1200, 8, DC_PARITY_NONE, DC_STOPBITS_TWO, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (1000ms).
	status = dc_iostream_set_timeout (device->iostream, 1000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the DTR/RTS line.");
		goto error_free;
	}

	*out = (dc_device_t*) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
suunto_eon_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	// Pre-allocate the required amount of memory.
	if (!dc_buffer_reserve (buffer, SZ_MEMORY)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = SZ_MEMORY + 1;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Send the command.
	unsigned char command[1] = {'P'};
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the answer.
	unsigned int nbytes = 0;
	unsigned char answer[SZ_MEMORY + 1] = {0};
	while (nbytes < sizeof(answer)) {
		// Set the minimum packet size.
		unsigned int len = 64;

		// Increase the packet size if more data is immediately available.
		size_t available = 0;
		status = dc_iostream_get_available (device->iostream, &available);
		if (status == DC_STATUS_SUCCESS && available > len)
			len = available;

		// Limit the packet size to the total size.
		if (nbytes + len > sizeof(answer))
			len = sizeof(answer) - nbytes;

		// Read the packet.
		status = dc_iostream_read (device->iostream, answer + nbytes, len, NULL);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to receive the answer.");
			return status;
		}

		// Update and emit a progress event.
		progress.current += len;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		nbytes += len;
	}

	// Verify the checksum of the package.
	unsigned char crc = answer[sizeof (answer) - 1];
	unsigned char ccrc = checksum_add_uint8 (answer, sizeof (answer) - 1, 0x00);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected answer checksum.");
		return DC_STATUS_PROTOCOL;
	}

	dc_buffer_append (buffer, answer, SZ_MEMORY);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = 0;
	devinfo.firmware = 0;
	devinfo.serial = array_convert_bcd2dec (answer + 244, 3);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	suunto_common_device_t *device = (suunto_common_device_t *) abstract;

	dc_buffer_t *buffer = dc_buffer_new (SZ_MEMORY);
	if (buffer == NULL)
		return DC_STATUS_NOMEMORY;

	dc_status_t rc = suunto_eon_device_dump (abstract, buffer);
	if (rc != DC_STATUS_SUCCESS) {
		dc_buffer_free (buffer);
		return rc;
	}

	rc = suunto_common_extract_dives (device, &suunto_eon_layout, dc_buffer_get_data (buffer),
		callback, userdata);

	dc_buffer_free (buffer);

	return rc;
}


dc_status_t
suunto_eon_device_write_name (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	if (size > 20)
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char command[21] = {'N'};
	memcpy (command + 1, data, size);
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_eon_device_write_interval (dc_device_t *abstract, unsigned char interval)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	suunto_eon_device_t *device = (suunto_eon_device_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	// Send the command.
	unsigned char command[2] = {'T', interval};
	status = dc_iostream_write (device->iostream, command, sizeof (command), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	return DC_STATUS_SUCCESS;
}
