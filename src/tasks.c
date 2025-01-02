#include "tasks.h"
#include "presets.h"
#include "utils.h"
#include "debug.h"

/* Queue of tasks */
static GAsyncQueue *tasks = NULL;

/* Stringify task type */
const char *dnafx_task_type_str(dnafx_task_type type) {
	switch(type) {
		case DNAFX_TASK_INIT_1:
			return "init 1";
		case DNAFX_TASK_INIT_2:
			return "init 2";
		case DNAFX_TASK_INIT_RESPONSE:
			return "init resp";
		case DNAFX_TASK_GET_PRESETS_1:
			return "presets 1";
		case DNAFX_TASK_GET_PRESETS_2:
			return "presets 2";
		case DNAFX_TASK_GET_PRESETS_RESPONSE:
			return "presets resp";
		case DNAFX_TASK_GET_EXTRAS_1:
			return "extras 1";
		case DNAFX_TASK_GET_EXTRAS_2:
			return "extras 2";
		case DNAFX_TASK_GET_EXTRAS_RESPONSE:
			return "extras resp";
		case DNAFX_TASK_CHANGE_PRESET:
			return "change";
		case DNAFX_TASK_RENAME_PRESET:
			return "rename";
		case DNAFX_TASK_UPLOAD_PRESET_1:
			return "upload 1";
		case DNAFX_TASK_UPLOAD_PRESET_2:
			return "upload 2";
		case DNAFX_TASK_UPLOAD_PRESET_3:
			return "upload 3";
		case DNAFX_TASK_UPLOAD_PRESET_4:
			return "upload 4";
		case DNAFX_TASK_UPLOAD_PRESET_RESPONSE:
			return "upload resp";
		case DNAFX_TASK_INTERRUPT:
			return "interrupt";
		case DNAFX_TASK_NONE:
		default:
			break;
	}
	return NULL;
};

/* Create a new task out of a command */
dnafx_task *dnafx_task_new(char *command) {
	if(command == NULL)
		return NULL;
	/* Trim the command, if needed */
	char *command_copy = g_strdup(command);
	dnafx_trim_string(command_copy);
	if(strlen(command_copy) == 0) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Empty command\n");
		free(command_copy);
		return NULL;
	}
	dnafx_task *task = g_malloc0(sizeof(dnafx_task));
	if(!strcasecmp(command_copy, "init")) {
		DNAFX_LOG(DNAFX_LOG_INFO, "[task] init\n");
		task->type = DNAFX_TASK_INIT_1;
	} else if(!strcasecmp(command_copy, "get presets")) {
		DNAFX_LOG(DNAFX_LOG_INFO, "[task] get presets\n");
		task->type = DNAFX_TASK_GET_PRESETS_1;
	} else if(!strcasecmp(command_copy, "get extras")) {
		DNAFX_LOG(DNAFX_LOG_INFO, "[task] get extras\n");
		task->type = DNAFX_TASK_GET_EXTRAS_1;
	} else if(strstr(command_copy, "change preset") == command_copy) {
		int preset_number = 0;
		if(sscanf(command_copy, "change preset %d\n", &preset_number) != 1 ||
				preset_number < 1 || preset_number > 200) {
			DNAFX_LOG(DNAFX_LOG_ERR, "Invalid 'change preset' format\n");
			dnafx_task_free(task);
			task = NULL;
		} else {
			DNAFX_LOG(DNAFX_LOG_INFO, "[task] change preset %d\n", preset_number);
			task->type = DNAFX_TASK_CHANGE_PRESET;
			task->preset1 = preset_number;
		}
	} else if(strstr(command_copy, "rename preset") == command_copy) {
		/* TODO */
		DNAFX_LOG(DNAFX_LOG_WARN, "[task] Rename preset TBD.\n");
	} else if(strstr(command_copy, "upload preset") == command_copy) {
		int preset_number = 0;
		char preset_name[DNAFX_PRESET_NAME_SIZE+1];
		preset_name[0] = '\0';
		if(sscanf(command_copy, "upload preset %d %[^\t\n]\n", &preset_number, preset_name) != 2 ||
				preset_number < 1 || preset_number > 200) {
			DNAFX_LOG(DNAFX_LOG_ERR, "Invalid 'upload preset' format\n");
			dnafx_task_free(task);
			task = NULL;
		} else {
			DNAFX_LOG(DNAFX_LOG_INFO, "[task] upload preset %d %s\n", preset_number, preset_name);
			task->type = DNAFX_TASK_UPLOAD_PRESET_1;
			task->preset1 = preset_number;
			task->preset_name = g_strdup(preset_name);
		}
	} else if(!strcasecmp(command_copy, "interrupt")) {
		DNAFX_LOG(DNAFX_LOG_INFO, "[task] interrupt\n");
		task->type = DNAFX_TASK_INTERRUPT;
	} else {
		DNAFX_LOG(DNAFX_LOG_ERR, "Unsupported command '%s'\n", command_copy);
		dnafx_task_free(task);
		task = NULL;
	}
	free(command_copy);
	return task;
}

/* Free a task */
void dnafx_task_free(dnafx_task *task) {
	if(task) {
		g_free(task->preset_name);
		g_free(task);
	}
}

/* Tasks management */
void dnafx_tasks_init(void) {
	tasks = g_async_queue_new_full((GDestroyNotify)dnafx_task_free);
}

void dnafx_tasks_add(dnafx_task *task) {
	if(tasks != NULL && task != NULL)
		g_async_queue_push(tasks, task);
}

dnafx_task *dnafx_tasks_next(void) {
	dnafx_task *task = tasks ? g_async_queue_try_pop(tasks) : NULL;
	return task;
}

void dnafx_tasks_deinit(void) {
	if(tasks != NULL)
		g_async_queue_unref(tasks);
	tasks = NULL;
}
