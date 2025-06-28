#include "dnafx-editor.h"
#include "usb.h"
#include "tasks.h"
#include "presets.h"
#include "utils.h"
#include "debug.h"

/* Defines */
#define DNAFX_VENDOR_ID		0x0483
#define DNAFX_PRODUCT_ID	0x5703
#define DNAFX_ENDPOINT_IN	(LIBUSB_ENDPOINT_IN | 1)
#define DNAFX_ENDPOINT_OUT	(LIBUSB_ENDPOINT_OUT | 2)
#define DNAFX_TIMEOUT		1000
#define DNAFX_BUFFER_SIZE	40960

/* Resources */
static libusb_context *ctx = NULL;
static libusb_device_handle *usb = NULL;
static volatile int in_flight = 0;
static void dnafx_usb_cb(struct libusb_transfer *transfer);

/* Task status */
static void dnafx_usb_task_notify(dnafx_task *task, int code, json_t *result);
static void dnafx_usb_task_notify_error(dnafx_task *task, int code, char *text);
static void dnafx_usb_task_done(dnafx_task *task);

/* Static buffer */
static uint8_t buf[DNAFX_BUFFER_SIZE];
static size_t buf_size = 0;
static dnafx_preset *cur_preset = NULL;
static uint8_t cur_preset_bytes[DNAFX_PRESET_SIZE];

/* Helpers */
static const char *libusb_transfer_status_str(enum libusb_transfer_status status) {
	switch(status) {
		case LIBUSB_TRANSFER_COMPLETED:
			return "completed";
		case LIBUSB_TRANSFER_ERROR:
			return "error";
		case LIBUSB_TRANSFER_TIMED_OUT:
			return "timed out";
		case LIBUSB_TRANSFER_CANCELLED:
			return "cancelled";
		case LIBUSB_TRANSFER_STALL:
			return "stall";
		case LIBUSB_TRANSFER_NO_DEVICE:
			return "no device";
		case LIBUSB_TRANSFER_OVERFLOW:
			return "overflow";
		default:
			break;
	}
	return NULL;
}

/* Request payload templates */
static uint8_t init1[] = {
	0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t init2[] = {
	0x08, 0xaa, 0x55, 0x02, 0x00, 0x00, 0x00, 0x12, 0x97
};
static uint8_t get_preset1[] = {
	0x08, 0xaa, 0x55, 0x02, 0x00, 0x31, 0x01, 0x34, 0x12
};
static uint8_t get_preset2[] = {
	0x08, 0xaa, 0x55, 0x02, 0x00, 0xa0, 0x01, 0x1f, 0xc8
};
static uint8_t get_extras1[] = {
	0x08, 0xaa, 0x55, 0x02, 0x00, 0xc1, 0x01, 0x27, 0xd3
};
static uint8_t get_extras2[] = {
	0x08, 0xaa, 0x55, 0x02, 0x00, 0x8c, 0x01, 0x5c, 0x43
};
static uint8_t change_preset[] = {
	0x08, 0xaa, 0x55, 0x02, 0x00, 0x96
};
static uint8_t rename_preset[] = {
	0x3f, 0xaa, 0x55, 0xa0, 0x00, 0xc3
};
static uint8_t send_preset[] = {
	0x09, 0xaa, 0x55, 0x03, 0x00, 0xb4, 0x05, 0x00, 0xcc, 0xe7
};
static uint8_t send_preset_prefix[] = {
	0x3f, 0xaa, 0x55, 0xa0, 0x00, 0xc3
};

/* USB management */
int dnafx_usb_init(int debug_level) {
	if(ctx != NULL || usb != NULL)
		return -1;
	/* Initialize libusb */
	int ret = 0;
	/* ORIG int init = libusb_init_context(&ctx, NULL, 0); */
	/* See https://github.com/lminiero/dnafx-editor/issues/5 */
	int init = libusb_init(&ctx);	
	if(init < 0) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Error initializing libusb: %d (%s)\n", init, libusb_strerror(init));
		return -1;
    }
	ret = libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, debug_level);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_WARN, "Error enabling libusb debugging: %d (%s)\n", ret, libusb_strerror(ret));
	}

	/* Connect to the device */
    usb = libusb_open_device_with_vid_pid(ctx, DNAFX_VENDOR_ID, DNAFX_PRODUCT_ID);
	if(usb == NULL) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Error connecting to device\n");
		return -1;
    }
	libusb_device *dev = libusb_get_device(usb);
	if(usb == NULL) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Error getting device\n");
		return -1;
    }
	struct libusb_device_descriptor desc = { 0 };
	ret = libusb_get_device_descriptor(dev, &desc);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Error getting device descriptor: %d (%s)\n", ret, libusb_strerror(ret));
		return -1;
	}
	/* Print the device details */
	DNAFX_LOG(DNAFX_LOG_INFO, "\nConnected to the device\n");
	unsigned char text[256];
	if(desc.iManufacturer) {
		ret = libusb_get_string_descriptor_ascii(usb, desc.iManufacturer, text, sizeof(text));
		if(ret > 0)
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- Manufacturer:  %s\n", (char *)text);
	}
	if(desc.iProduct) {
		ret = libusb_get_string_descriptor_ascii(usb, desc.iProduct, text, sizeof(text));
		if(ret > 0)
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- Product:       %s\n", (char *)text);
	}
	if(desc.iSerialNumber) {
		ret = libusb_get_string_descriptor_ascii(usb, desc.iSerialNumber, text, sizeof(text));
		if(ret > 0)
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- Serial Number: %s\n", (char *)text);
	}
	DNAFX_LOG(DNAFX_LOG_INFO, "\n");

	/* Claim the device (needed?) */
	if(libusb_kernel_driver_active(usb, 0) == 1) {
		DNAFX_LOG(DNAFX_LOG_INFO, "Kernel driver active, detaching...\n");
		if(libusb_detach_kernel_driver(usb, 0) == 0)
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- Kernel driver detached!\n");
	}
	if(libusb_claim_interface(usb, 0) < 0) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Error claiming interface\n");
		return -1;
	}
	//~ libusb_clear_halt(usb, DNAFX_ENDPOINT_OUT);

	return 0;
}

static const struct libusb_pollfd **fds = NULL;
const struct libusb_pollfd **dnafx_usb_fds(gboolean refresh) {
	if(ctx == NULL)
		return NULL;
	if(fds == NULL || refresh) {
		if(fds != NULL)
			libusb_free_pollfds(fds);
		fds = libusb_get_pollfds(ctx);
	}
	return fds;
}

int dnafx_usb_get_next_timeout(struct timeval *tv) {
	return ctx ? libusb_get_next_timeout(ctx, tv) : 0;
}

void dnafx_usb_step(void) {
	if(ctx != NULL) {
		struct timeval tv = { 0 };
		libusb_handle_events_timeout(ctx, &tv);
	}
	/* If there isn't any task running, check if we have a task waiting */
	dnafx_task *task = NULL;
	if(g_atomic_int_compare_and_exchange(&in_flight, 0, 1)) {
		buf_size = 0;
		task = dnafx_tasks_next();
		if(task == NULL) {
			/* Nothing to do */
			dnafx_usb_task_done(task);
		} else {
			/* Perform the new activity */
			if(task->type == DNAFX_TASK_CLI) {
				dnafx_cli();
				dnafx_usb_task_done(task);
			} else if(task->type == DNAFX_TASK_QUIT) {
				dnafx_usb_task_done(task);
				dnafx_quit();
			} else if(task->type == DNAFX_TASK_HELP) {
				if(task->context == NULL && task->callback == NULL) {
					/* Just print the help instrunctions */
					dnafx_task_show_help();
				} else {
					/* Return the list as a JSON object */
					json_t *help = dnafx_task_show_help_json();
					dnafx_usb_task_notify(task, 200, help);
				}
				dnafx_usb_task_done(task);
			} else if(task->type == DNAFX_TASK_INIT_1) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_init(task);
			} else if(task->type == DNAFX_TASK_GET_PRESETS_1) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_get_presets(task);
			} else if(task->type == DNAFX_TASK_GET_EXTRAS_1) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_get_extras(task);
			} else if(task->type == DNAFX_TASK_CHANGE_PRESET) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_change_preset(task);
			} else if(task->type == DNAFX_TASK_RENAME_PRESET) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_rename_preset(task);
			} else if(task->type == DNAFX_TASK_UPLOAD_PRESET_1) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_upload_preset(task);
			} else if(task->type == DNAFX_TASK_INTERRUPT) {
				if(ctx == NULL)
					goto disconnected;
				dnafx_send_interrupt(task);
			} else if(task->type == DNAFX_TASK_LIST_PRESETS) {
				if(task->context == NULL && task->callback == NULL) {
					/* Just print the results */
					dnafx_presets_print();
				} else {
					/* Return the list as a JSON object */
					json_t *list = dnafx_presets_list();
					dnafx_usb_task_notify(task, 200, list);
				}
				dnafx_usb_task_done(task);
			} else if(task->type == DNAFX_TASK_IMPORT_PRESET) {
				gboolean phb = !strcasecmp(task->text[0], "phb");
				dnafx_preset *preset = dnafx_preset_import(task->text[1], phb);
				if(preset != NULL)
					DNAFX_LOG(DNAFX_LOG_INFO, "  -- Successfully imported preset '%s'\n", preset->name);
				dnafx_usb_task_done(task);
			} else if(task->type == DNAFX_TASK_PARSE_PRESET) {
				dnafx_preset *preset = NULL;
				if(task->number[0] > 0)
					preset = dnafx_preset_find_byid(task->number[0]);
				else
					preset = dnafx_preset_find_byname(task->text[0]);
				if(preset == NULL) {
					DNAFX_LOG(DNAFX_LOG_WARN, "No such preset\n");
					dnafx_usb_task_notify_error(task, 404, "No such preset");
				} else {
					dnafx_preset_print_debug(preset);
				}
				dnafx_usb_task_done(task);
			} else if(task->type == DNAFX_TASK_EXPORT_PRESET) {
				dnafx_preset *preset = NULL;
				if(task->number[0] > 0)
					preset = dnafx_preset_find_byid(task->number[0]);
				else
					preset = dnafx_preset_find_byname(task->text[0]);
				if(preset == NULL) {
					DNAFX_LOG(DNAFX_LOG_WARN, "No such preset\n");
					dnafx_usb_task_notify_error(task, 404, "No such preset");
				} else {
					gboolean phb = !strcasecmp(task->text[1], "phb");
					if(task->text[2] != NULL) {
						/* We have a target filename */
						if(dnafx_preset_export(preset, task->text[2], phb) == 0) {
							DNAFX_LOG(DNAFX_LOG_INFO, "  -- Successfully exported preset '%s'\n", preset->name);
						} else {
							dnafx_usb_task_notify_error(task, 400, "Error exporting preset");
						}
					} else {
						/* No target filename, if this is coming from an API call send it there */
						if(task->context != NULL && task->callback != NULL) {
							json_t *json = NULL;
							if(phb) {
								json = dnafx_preset_to_phb_json(preset);
							} else {
								char *base64 = dnafx_preset_to_bytes_base64(preset);
								if(base64 != NULL) {
									json = json_object();
									json_object_set_new(json, "base64", json_string(base64));
									g_free(base64);
								}
							}
							if(json != NULL) {
								dnafx_usb_task_notify(task, 200, json);
							} else {
								dnafx_usb_task_notify_error(task, 400, "Error exporting preset");
							}
						} else {
							DNAFX_LOG(DNAFX_LOG_WARN, "Missing target filename\n");
						}
					}
				}
				dnafx_usb_task_done(task);
			} else {
				DNAFX_LOG(DNAFX_LOG_WARN, "Task '%s' currently unsupported\n",
					dnafx_task_type_str(task->type));
				dnafx_usb_task_notify_error(task, 400, "Task unsupported");
				dnafx_usb_task_done(task);
			}
		}
	}
	return;

disconnected:
	DNAFX_LOG(DNAFX_LOG_WARN, "Task '%s' currently unavailable (disconnected)\n",
		dnafx_task_type_str(task->type));
	dnafx_usb_task_notify_error(task, 400, "Currently unavailable (disconnected)");
	/* Nothing to do */
	dnafx_usb_task_done(task);
}

void dnafx_usb_deinit(void) {
	if(fds != NULL)
		libusb_free_pollfds(fds);
	fds = NULL;
	if(usb != NULL) {
		libusb_release_interface(usb, 0);
		libusb_close(usb);
		usb = NULL;
	}
	if(ctx != NULL)
		libusb_exit(ctx);
	ctx = NULL;
}

/* Sending messages */
void dnafx_send_init(dnafx_task *task) {
	if(task == NULL)
		return;
	size_t len = 64;
	uint8_t *message = NULL;
	size_t mlen = 0;
	if(task->type == DNAFX_TASK_INIT_1) {
		DNAFX_LOG(DNAFX_LOG_INFO, "Greeting the device\n");
		message = init1;
		mlen = sizeof(init1);
	} else {
		message = init2;
		mlen = sizeof(init2);
	}
	uint8_t *buffer = g_malloc0(len);
	memcpy(buffer, message, mlen);
	DNAFX_LOG(DNAFX_LOG_VERB, "Sending inizialitazion message of %zu bytes\n", len);
	dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buffer, len);
	struct libusb_transfer *init = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(init, usb, DNAFX_ENDPOINT_OUT, buffer, len, dnafx_usb_cb, task, DNAFX_TIMEOUT);
	int ret = libusb_submit_transfer(init);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting initialization transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(init);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

void dnafx_send_get_presets(dnafx_task *task) {
	if(task == NULL)
		return;
	size_t len = 64;
	uint8_t *message = NULL;
	size_t mlen = 0;
	if(task->type == DNAFX_TASK_GET_PRESETS_1) {
		DNAFX_LOG(DNAFX_LOG_INFO, "Getting all existing presets\n");
		message = get_preset1;
		mlen = sizeof(get_preset1);
	} else {
		message = get_preset2;
		mlen = sizeof(get_preset2);
	}
	uint8_t *buffer = g_malloc0(len);
	memcpy(buffer, message, mlen);
	DNAFX_LOG(DNAFX_LOG_VERB, "Sending inizialitazion message of %zu bytes\n", len);
	dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buffer, len);
	struct libusb_transfer *gp = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(gp, usb, DNAFX_ENDPOINT_OUT, buffer, len, dnafx_usb_cb, task, DNAFX_TIMEOUT);
	int ret = libusb_submit_transfer(gp);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting presets retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(gp);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

void dnafx_send_get_extras(dnafx_task *task) {
	if(task == NULL)
		return;
	size_t len = 64;
	uint8_t *message = NULL;
	size_t mlen = 0;
	if(task->type == DNAFX_TASK_GET_EXTRAS_1) {
		DNAFX_LOG(DNAFX_LOG_INFO, "Getting all existing extras (IRs?)\n");
		message = get_extras1;
		mlen = sizeof(get_extras1);
	} else {
		message = get_extras2;
		mlen = sizeof(get_extras2);
	}
	uint8_t *buffer = g_malloc0(len);
	memcpy(buffer, message, mlen);
	DNAFX_LOG(DNAFX_LOG_VERB, "Sending inizialitazion message of %zu bytes\n", len);
	dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buffer, len);
	struct libusb_transfer *ge = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(ge, usb, DNAFX_ENDPOINT_OUT, buffer, len, dnafx_usb_cb, task, DNAFX_TIMEOUT);
	int ret = libusb_submit_transfer(ge);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting extras retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(ge);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

void dnafx_send_change_preset(dnafx_task *task) {
	if(task == NULL)
		return;
	int preset = task->number[0];
	DNAFX_LOG(DNAFX_LOG_INFO, "Changing current preset to %d\n", preset);
	uint8_t *buffer = g_malloc0(10);
	size_t len = sizeof(change_preset);
	memcpy(buffer, change_preset, len);
	buffer[len] = (uint8_t)preset;
	len++;
	DNAFX_LOG(DNAFX_LOG_VERB, "Sending preset change message of %zu bytes\n", len);
	dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buffer, len);
	/* Send the message */
	struct libusb_transfer *cp = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(cp, usb, DNAFX_ENDPOINT_OUT, buffer, len, dnafx_usb_cb, task, DNAFX_TIMEOUT);
	int ret = libusb_submit_transfer(cp);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting preset change transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(cp);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

void dnafx_send_rename_preset(dnafx_task *task) {
	if(task == NULL)
		return;
	int preset = task->number[0];
	char *name = task->text[0];
	DNAFX_LOG(DNAFX_LOG_INFO, "Renaming preset to %d: '%s'\n", preset, name);
	size_t len = 64;
	uint8_t *buffer = g_malloc0(len);
	memcpy(buffer, rename_preset, sizeof(rename_preset));
	buffer[6] = (uint8_t)preset;
	memset(buffer + 7, 0, 14);
	size_t namelen = strlen(name);
	if(namelen > 14)
		namelen = 14;
	memcpy(buffer + 7, name, namelen);
	DNAFX_LOG(DNAFX_LOG_VERB, "Sending preset change message of %zu bytes\n", len);
	dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buffer, len);
	/* Send the message */
	struct libusb_transfer *rp = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(rp, usb, DNAFX_ENDPOINT_OUT, buffer, len, dnafx_usb_cb, task, 0);
	int ret = libusb_submit_transfer(rp);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting preset rename transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(rp);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

void dnafx_send_upload_preset(dnafx_task *task) {
	if(task == NULL)
		return;
	if(task->type == DNAFX_TASK_UPLOAD_PRESET_1) {
		cur_preset = dnafx_preset_find_byname(task->text[0]);
		if(cur_preset == NULL) {
			DNAFX_LOG(DNAFX_LOG_WARN, "Can't upload preset named '%s' (no such preset)\n", task->text[0]);
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 404, "No such preset");
			dnafx_usb_task_done(task);
			return;
		} else {
			cur_preset->id = task->number[0];
			dnafx_preset_to_bytes(cur_preset, cur_preset_bytes, sizeof(cur_preset_bytes));
		}
	}
	size_t len = 64;
	uint8_t *buffer = g_malloc0(len);
	if(task->type == DNAFX_TASK_UPLOAD_PRESET_1) {
		/* First request */
		DNAFX_LOG(DNAFX_LOG_INFO, "Uploading preset '%s' to slot %d\n", cur_preset->name, cur_preset->id);
		dnafx_print_hex(DNAFX_LOG_HUGE, NULL, cur_preset_bytes, sizeof(cur_preset_bytes));
		memcpy(buffer, send_preset, sizeof(send_preset));
	} else if(task->type == DNAFX_TASK_UPLOAD_PRESET_2) {
		/* First preset portion */
		memcpy(buffer, send_preset_prefix, sizeof(send_preset_prefix));
		memcpy(buffer + 6, cur_preset_bytes, len - 6);
	} else if(task->type == DNAFX_TASK_UPLOAD_PRESET_3) {
		/* Second preset portion */
		*buffer = 0x3f;
		memcpy(buffer + 1, cur_preset_bytes + len - 6, len - 1);
	} else if(task->type == DNAFX_TASK_UPLOAD_PRESET_4) {
		/* Third (and last) preset portion */
		*buffer = 0x28;
		memcpy(buffer + 1, cur_preset_bytes + 2*len - 6 - 1, len - 1);
	}
	DNAFX_LOG(DNAFX_LOG_VERB, "Sending upload message of %zu bytes\n", len);
	dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buffer, len);
	struct libusb_transfer *gp = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(gp, usb, DNAFX_ENDPOINT_OUT, buffer, len, dnafx_usb_cb, task, DNAFX_TIMEOUT);
	int ret = libusb_submit_transfer(gp);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting preset upload transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(gp);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

void dnafx_send_interrupt(dnafx_task *task) {
	if(task == NULL)
		return;
	DNAFX_LOG(DNAFX_LOG_INFO, "Sending interrupt request\n");
	size_t len = 64;
	uint8_t *buffer = g_malloc0(len);
	/* Send the message */
	struct libusb_transfer *ir = libusb_alloc_transfer(0);
	libusb_fill_bulk_transfer(ir, usb, DNAFX_ENDPOINT_IN, buffer, len, dnafx_usb_cb, task, DNAFX_TIMEOUT);
	int ret = libusb_submit_transfer(ir);
	if(ret < 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting interrupt transfer: %d (%s)\n", ret, libusb_strerror(ret));
		g_free(buffer);
		libusb_free_transfer(ir);
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_notify_error(task, 500, "libusb error");
		dnafx_usb_task_done(task);
	}
}

/* Callback */
static void dnafx_usb_cb(struct libusb_transfer *transfer) {
	/* We use a single callback for all interactions: we'll check
	 * the user_data portion to check how we should then handle that */
	dnafx_task *task = (dnafx_task *)transfer->user_data;
	dnafx_task_type what = task->type;
	if(transfer->status == LIBUSB_TRANSFER_COMPLETED || transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
		DNAFX_LOG(DNAFX_LOG_VERB, "USB transfer status [%s]: %d (%s)\n", dnafx_task_type_str(what),
			transfer->status, libusb_transfer_status_str(transfer->status));
	} else {
		DNAFX_LOG(DNAFX_LOG_WARN, "USB transfer status [%s]: %d (%s)\n", dnafx_task_type_str(what),
			transfer->status, libusb_transfer_status_str(transfer->status));
	}
	if(what == DNAFX_TASK_INIT_1) {
		/* First initialization message sent, send the second */
		task->type = DNAFX_TASK_INIT_2;
		dnafx_send_init(task);
	} else if(what == DNAFX_TASK_INIT_2) {
		/* Second initialization message sent, wait for a response */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			/* Wait for a response from the device */
			struct libusb_transfer *init = libusb_alloc_transfer(0);
			size_t blen = 64;
			uint8_t *buffer = g_malloc0(blen);
			task->type = DNAFX_TASK_INIT_RESPONSE;
			libusb_fill_bulk_transfer(init, usb, DNAFX_ENDPOINT_IN, buffer, blen, dnafx_usb_cb, task, DNAFX_TIMEOUT);
			int ret = libusb_submit_transfer(init);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting initialization transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(buffer);
				libusb_free_transfer(init);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_INIT_RESPONSE) {
		/* We asked for a response, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			/* We got a response from the device */
			DNAFX_LOG(DNAFX_LOG_VERB, "Received %d bytes\n", transfer->actual_length);
			if(transfer->actual_length > 0) {
				/* FIXME Append to the buffer */
				dnafx_print_hex(DNAFX_LOG_HUGE, NULL, transfer->buffer, transfer->actual_length);
				uint8_t *info = transfer->buffer;
				size_t ilen = transfer->actual_length;
				if(buf_size == 0 && info[0] != 0x3f && info[1] != 0xaa && info[2] != 0x55 &&
						info[3] != 0x3f && info[4] != 0x00 && info[5] != 0x01) {
					DNAFX_LOG(DNAFX_LOG_WARN, "Skipping unexpected data frame\n");
				} else {
					if(buf_size == 0) {
						/* Framing prefix, skip */
						info += 6;
						ilen -= 6;
					} else {
						/* Framing byte, skip */
						info++;
						ilen--;
					}
					memcpy(buf + buf_size, info, ilen);
					buf_size += ilen;
				}
			}
			int ret = libusb_submit_transfer(transfer);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting presets retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(transfer->buffer);
				libusb_free_transfer(transfer);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
			return;
		} else {
			/* Parse the payload */
			DNAFX_LOG(DNAFX_LOG_VERB, "Info (%zu bytes)\n", buf_size);
			dnafx_print_hex(DNAFX_LOG_HUGE, NULL, buf, buf_size);
			char *info = (char *)buf;
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- %.*s\n", 31, info);
			info += 32;
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- %.*s\n", 6, info);
			info += 7;
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- %.*s\n", 6, info);
			info += 7;
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- %.*s\n", 6, info);
		}
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_done(task);
	} else if(what == DNAFX_TASK_GET_PRESETS_1) {
		/* First presets retrieval message sent, send the second */
		task->type = DNAFX_TASK_GET_PRESETS_2;
		dnafx_send_get_presets(task);
	} else if(what == DNAFX_TASK_GET_PRESETS_2) {
		/* We sent our request, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			/* Wait for a response from the device */
			struct libusb_transfer *gp = libusb_alloc_transfer(0);
			size_t blen = 64;
			uint8_t *buffer = g_malloc0(blen);
			task->type = DNAFX_TASK_GET_PRESETS_RESPONSE;
			libusb_fill_bulk_transfer(gp, usb, DNAFX_ENDPOINT_IN, buffer, blen, dnafx_usb_cb, task, DNAFX_TIMEOUT);
			int ret = libusb_submit_transfer(gp);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting presets retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(buffer);
				libusb_free_transfer(gp);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_GET_PRESETS_RESPONSE) {
		/* We asked for a response, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			/* We got a response from the device */
			DNAFX_LOG(DNAFX_LOG_VERB, "Received %d bytes\n", transfer->actual_length);
			if(transfer->actual_length > 0) {
				/* FIXME Append to the buffer */
				dnafx_print_hex(DNAFX_LOG_HUGE, NULL, transfer->buffer, transfer->actual_length);
				uint8_t *preset = transfer->buffer;
				size_t plen = transfer->actual_length;
				if(preset[0] == 0x3f && preset[1] == 0xaa && preset[2] == 0x55 &&
						preset[3] == 0xa0 && preset[4] == 0x00 && preset[5] == 0x20) {
					/* Framing prefix, skip */
					preset += 6;
					plen -= 6;
				} else if(preset[0] == 0x3f || preset[0] == 0x28) {
					/* Framing, skip */
					preset++;
					plen--;
				}
				memcpy(buf + buf_size, preset, plen);
				buf_size += plen;
			}
			int ret = libusb_submit_transfer(transfer);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting presets retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(transfer->buffer);
				libusb_free_transfer(transfer);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
			return;
		} else {
			/* Parse the payload */
			DNAFX_LOG(DNAFX_LOG_VERB, "Presets (%zu bytes)\n", buf_size);
			uint8_t *preset = NULL;
			size_t offset = 0;
			size_t count = 0;
			dnafx_preset *p = NULL;
			while(offset + DNAFX_PRESET_SIZE < buf_size && count < DNAFX_PRESETS_NUM) {
				preset = &buf[offset];
				p = dnafx_preset_from_bytes(preset, DNAFX_PRESET_SIZE);
				if(p != NULL) {
					/* Keep track of the preset */
					if(dnafx_preset_add(p) == 0) {
						dnafx_preset_set_id(p, p->id);
						/* Check if we need to also save it locally */
						if(dnafx_presets_folder() != NULL) {
							/* FIXME */
							char filename[256];
							g_snprintf(filename, sizeof(filename), "%s/%03d-%s.bhb", dnafx_presets_folder(), p->id, p->name);
							dnafx_write_file(filename, FALSE, preset, DNAFX_PRESET_SIZE);
						}
					}
				}
				offset += DNAFX_PRESET_SIZE;
				count++;
			}
			DNAFX_LOG(DNAFX_LOG_INFO, "  -- Received %zu presets\n", count);
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_GET_EXTRAS_1) {
		/* First extras retrieval message sent, send the second */
		task->type = DNAFX_TASK_GET_EXTRAS_2;
		dnafx_send_get_extras(task);
	} else if(what == DNAFX_TASK_GET_EXTRAS_2) {
		/* We sent our request, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			/* Wait for a response from the device */
			struct libusb_transfer *ge = libusb_alloc_transfer(0);
			size_t blen = 64;
			uint8_t *buffer = g_malloc0(blen);
			task->type = DNAFX_TASK_GET_EXTRAS_RESPONSE;
			libusb_fill_bulk_transfer(ge, usb, DNAFX_ENDPOINT_IN, buffer, blen, dnafx_usb_cb, task, DNAFX_TIMEOUT);
			int ret = libusb_submit_transfer(ge);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting extras retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(buffer);
				libusb_free_transfer(ge);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_GET_EXTRAS_RESPONSE) {
		/* We asked for a response, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			/* We got a response from the device */
			DNAFX_LOG(DNAFX_LOG_VERB, "Received %d bytes\n", transfer->actual_length);
			if(transfer->actual_length > 0) {
				/* FIXME Append to the buffer, skipping the first byte (which is always 0x3F) */
				dnafx_print_hex(DNAFX_LOG_HUGE, NULL, transfer->buffer, transfer->actual_length);
				uint8_t *extra = transfer->buffer;
				size_t elen = transfer->actual_length;
				if(extra[0] == 0x3f || extra[0] == 0x0d || extra[0] == 0x0c) {
					/* Framing, skip */
					extra++;
					elen--;
				}
				memcpy(buf + buf_size, extra, elen);
				buf_size += elen;
			}
			int ret = libusb_submit_transfer(transfer);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting extras retrieval transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(transfer->buffer);
				libusb_free_transfer(transfer);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
			return;
		} else {
			/* Parse the payload */
			DNAFX_LOG(DNAFX_LOG_VERB, "Extras (%zu bytes)\n", buf_size);
			size_t offset = 5, count = 0;
			while(offset + 16 < buf_size && buf[offset] != 0 && count < 20) {
				DNAFX_LOG(DNAFX_LOG_INFO, "  -- %.*s\n", 16, (char *)&buf[offset]);
				offset += 16;
				count++;
			}
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_CHANGE_PRESET) {
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
		} else {
			dnafx_usb_task_notify_error(task, 500, "libusb error");
		}
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_done(task);
	} else if(what == DNAFX_TASK_RENAME_PRESET) {
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			/* FIXME We need to send the same message three times, for it to have effect */
			task->number[1]++;
			if(task->number[1] == 3) {
				/* Wait for a final event from the device */
				struct libusb_transfer *rp = libusb_alloc_transfer(0);
				size_t blen = 64;
				uint8_t *buffer = g_malloc0(blen);
				task->type = DNAFX_TASK_RENAME_PRESET_RESPONSE;
				libusb_fill_bulk_transfer(rp, usb, DNAFX_ENDPOINT_IN, buffer, blen, dnafx_usb_cb, task, DNAFX_TIMEOUT);
				int ret = libusb_submit_transfer(rp);
				if(ret < 0) {
					DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting rename preset feedback: %d (%s)\n", ret, libusb_strerror(ret));
					g_free(buffer);
					libusb_free_transfer(rp);
					/* This transaction is over, we're ready for another task */
					dnafx_usb_task_notify_error(task, 500, "libusb error");
					dnafx_usb_task_done(task);
				}
			} else {
				int ret = libusb_submit_transfer(transfer);
				if(ret < 0) {
					DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting rename preset request: %d (%s)\n", ret, libusb_strerror(ret));
					g_free(transfer->buffer);
					libusb_free_transfer(transfer);
					/* This transaction is over, we're ready for another task */
					dnafx_usb_task_notify_error(task, 500, "libusb error");
					dnafx_usb_task_done(task);
				}
				return;
			}
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_RENAME_PRESET_RESPONSE) {
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_done(task);
	} else if(what == DNAFX_TASK_UPLOAD_PRESET_1) {
		/* First upload message sent, send the second */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			task->type = DNAFX_TASK_UPLOAD_PRESET_2;
			dnafx_send_upload_preset(task);
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_UPLOAD_PRESET_2) {
		/* Second upload message sent, send the third */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			task->type = DNAFX_TASK_UPLOAD_PRESET_3;
			dnafx_send_upload_preset(task);
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_UPLOAD_PRESET_3) {
		/* Third upload message sent, send the fourth (and last) */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			task->type = DNAFX_TASK_UPLOAD_PRESET_4;
			dnafx_send_upload_preset(task);
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_UPLOAD_PRESET_4) {
		/* We uploaded the preset, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			DNAFX_LOG(DNAFX_LOG_VERB, "  -- Sent %d/%d bytes\n", transfer->actual_length, transfer->length);
			/* Wait for a final event from the device */
			struct libusb_transfer *gu = libusb_alloc_transfer(0);
			size_t blen = 64;
			uint8_t *buffer = g_malloc0(blen);
			task->type = DNAFX_TASK_UPLOAD_PRESET_RESPONSE;
			libusb_fill_bulk_transfer(gu, usb, DNAFX_ENDPOINT_IN, buffer, blen, dnafx_usb_cb, task, DNAFX_TIMEOUT);
			int ret = libusb_submit_transfer(gu);
			if(ret < 0) {
				DNAFX_LOG(DNAFX_LOG_ERR, "Error submitting upload transfer: %d (%s)\n", ret, libusb_strerror(ret));
				g_free(buffer);
				libusb_free_transfer(gu);
				/* This transaction is over, we're ready for another task */
				dnafx_usb_task_notify_error(task, 500, "libusb error");
				dnafx_usb_task_done(task);
			}
		} else {
			/* This transaction is over, we're ready for another task */
			dnafx_usb_task_notify_error(task, 500, "libusb error");
			dnafx_usb_task_done(task);
		}
	} else if(what == DNAFX_TASK_UPLOAD_PRESET_RESPONSE) {
		/* We asked for a response, check if it worked */
		if(transfer->status == LIBUSB_TRANSFER_COMPLETED) {
			/* We got a response from the device */
			DNAFX_LOG(DNAFX_LOG_VERB, "Received %d bytes\n", transfer->actual_length);
			if(transfer->actual_length > 0) {
				dnafx_print_hex(DNAFX_LOG_HUGE, NULL, transfer->buffer, transfer->actual_length);
			}
			/* Update the local view of presets */
			dnafx_preset_set_id(cur_preset, cur_preset->id);
			cur_preset = 0;
		} else {
			dnafx_usb_task_notify_error(task, 500, "libusb error");
		}
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_done(task);
	} else if(what == DNAFX_TASK_INTERRUPT) {
		/* This transaction is over, we're ready for another task */
		dnafx_usb_task_done(task);
	} else {
		/* Unknown task? Shouldn't happen... */
		dnafx_usb_task_notify_error(task, 400, "Unknown task");
		dnafx_usb_task_done(task);
	}
	/* Free the libusb transfer instance, we're done */
	g_free(transfer->buffer);
	libusb_free_transfer(transfer);
}

/* Task status */
static void dnafx_usb_task_notify(dnafx_task *task, int code, json_t *result) {
	if(task && task->context && task->callback) {
		task->callback(code, result, task->context);
		/* Clear the callback after done, to avoid double events */
		task->context = NULL;
		task->callback = NULL;
	}
}

static void dnafx_usb_task_notify_error(dnafx_task *task, int code, char *text) {
	if(task && task->context && task->callback) {
		json_t *body = NULL;
		if(text != NULL) {
			body = json_object();
			json_object_set_new(body, "reason", json_string(text));
		}
		dnafx_usb_task_notify(task, code, body);
	}
}

static void dnafx_usb_task_done(dnafx_task *task) {
	if(task && task->context && task->callback) {
		/* If there's a callback and it hasn't been triggered yet, it
		 * means we can just report a success with no further info */
		dnafx_usb_task_notify(task, 200, NULL);
	}
	dnafx_task_free(task);
	g_atomic_int_set(&in_flight, 0);
}
