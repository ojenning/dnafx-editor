#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "usb.h"
#include "tasks.h"
#include "presets.h"
#include "utils.h"
#include "options.h"
#include "debug.h"

/* Command line options */
static dnafx_options options = { 0 };

/* Logging */
int dnafx_log_level = DNAFX_LOG_INFO;
gboolean dnafx_log_timestamps = FALSE;
gboolean dnafx_log_colors = TRUE;

/* Signal */
static int stop = 0;
static void handle_signal(int signum) {
	stop = 1;
}

/* Main */
int main(int argc, char *argv[]) {
	int res = 0;
	dnafx_tasks_init();

	DNAFX_PRINT("\nOpen source DNAfx GiT editor (experimental and WIP)\n\n");
	DNAFX_PRINT("  ####################################################################\n");
	DNAFX_PRINT("  #                                                                  #\n");
	DNAFX_PRINT("  #  NOTE WELL: Not affiliated with, nor endorsed by, Harley Benton  #\n");
	DNAFX_PRINT("  #                                                                  #\n");
	DNAFX_PRINT("  ####################################################################\n\n");

	/* Initialize some command line options defaults */
	options.debug_level = DNAFX_LOG_INFO;
	/* Let's call our cmdline parser */
	if(!dnafx_options_parse(&options, argc, argv)) {
		dnafx_options_show_usage();
		dnafx_options_destroy();
		res = 1;
		goto done;
	}
	/* Logging level */
	dnafx_log_level = options.debug_level;
	dnafx_log_timestamps = options.debug_timestamps;
	dnafx_log_colors = !options.disable_colors;

	/* Presets management */
	if(dnafx_presets_init(options.save_presets_folder) < 0) {
		res = 1;
		goto done;
	}

	/* Check if we need to parse a preset file */
	if(options.preset_file_in != NULL && options.phb_file_in != NULL) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Can't provide both binary and PHB file as preset input\n");
		res = 1;
		goto done;
	}
	if((options.preset_file_out != NULL || options.phb_file_out != NULL) &&
			options.preset_file_in == NULL && options.phb_file_in == NULL) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Can't convert preset to a different format, no input preset provided\n");
		res = 1;
		goto done;
	}
	/* Parse the preset file, if any, to our internal format */
	dnafx_preset *preset = NULL;
	if(options.preset_file_in != NULL) {
		/* Open the provided binary file and parse it */
		uint8_t buf[DNAFX_PRESET_SIZE];
		if(dnafx_read_file(options.preset_file_in, FALSE, buf, sizeof(buf)) <= 0) {
			res = 1;
			goto done;
		}
		preset = dnafx_preset_from_bytes(buf, sizeof(buf));
		if(preset == NULL) {
			res = 1;
			goto done;
		}
	} else if(options.phb_file_in != NULL) {
		/* Open the provided PHB file and parse it */
		char buf[4096];
		int blen = dnafx_read_file(options.phb_file_in, TRUE, (uint8_t *)buf, sizeof(buf));
		if(blen <= 0) {
			res = 1;
			goto done;
		}
		preset = dnafx_preset_from_phb(buf);
		if(preset == NULL) {
			res = 1;
			goto done;
		}
	}
	/* Check if we need to convert this preset to something else */
	if(options.preset_file_out != NULL) {
		/* Convert the preset to the binary format */
		uint8_t buf[DNAFX_PRESET_SIZE];
		if(dnafx_preset_to_bytes(preset, buf, sizeof(buf)) < 0) {
			dnafx_preset_free(preset);
			res = 1;
			goto done;
		}
		if(dnafx_write_file(options.preset_file_out, FALSE, buf, sizeof(buf)) <= 0) {
			dnafx_preset_free(preset);
			res = 1;
			goto done;
		}
	}
	if(options.phb_file_out != NULL) {
		/* Convert the preset to the PHB (JSON) format */
		char *json_text = dnafx_preset_to_phb(preset);
		if(json_text == NULL) {
			dnafx_preset_free(preset);
			res = 1;
			goto done;
		}
		if(dnafx_write_file(options.phb_file_out, TRUE, (uint8_t *)json_text, strlen(json_text)) <= 0) {
			free(json_text);
			dnafx_preset_free(preset);
			res = 1;
			goto done;
		}
		free(json_text);
	}
	if(options.preset_file_out == NULL && options.phb_file_out == NULL && preset != NULL)
		dnafx_preset_print_debug(preset);
	if(preset != NULL)
		dnafx_preset_add_byname(preset);

	if(options.offline) {
		/* We're done, no need to connect to the device */
		goto done;
	}
	res = 0;

	/* Check what tasks should be queued at startup */
	if(!options.no_init)
		dnafx_tasks_add(dnafx_task_new("init"));
	if(!options.no_get_presets)
		dnafx_tasks_add(dnafx_task_new("get presets"));
	if(!options.no_get_extras)
		dnafx_tasks_add(dnafx_task_new("get extras"));
	if(options.change_preset > 0) {
		if(options.change_preset > 200) {
			DNAFX_LOG(DNAFX_LOG_WARN, "Invalid preset number %d\n", options.change_preset);
		} else {
			char command[100];
			g_snprintf(command, sizeof(command), "change preset %d", options.change_preset);
			dnafx_tasks_add(dnafx_task_new(command));
		}
	}
	if(options.upload_preset > 0) {
		if(preset == NULL) {
			DNAFX_LOG(DNAFX_LOG_WARN, "Can't upload a preset, none was imported\n");
		} else if(options.upload_preset > 200) {
			DNAFX_LOG(DNAFX_LOG_WARN, "Invalid preset number %d\n", options.upload_preset);
		} else {
			char command[256];
			g_snprintf(command, sizeof(command), "upload preset %d %s", options.upload_preset, preset->name);
			dnafx_tasks_add(dnafx_task_new(command));
		}
	}

	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/* Connect to the device */
	if(dnafx_usb_init(options.debug_libusb) < 0) {
		res = 1;
		goto done;
	}

	/* Loop */
	while(!stop)
		dnafx_usb_step();

done:
	/* Cleanup */
	dnafx_tasks_deinit();
	dnafx_presets_deinit();
	dnafx_usb_deinit();

	/* Done */
	dnafx_options_destroy();
	DNAFX_PRINT("\nBye!\n");
	exit(res);
}
