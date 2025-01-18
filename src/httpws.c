#include <libwebsockets.h>
#include <glib.h>
#include <jansson.h>

#include "dnafx-editor.h"
#include "httpws.h"
#include "tasks.h"
#include "mutex.h"
#include "debug.h"

/* Logging */
static int ws_log_level = LLL_ERR | LLL_WARN;
//~ static int ws_log_level = LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO |
	//~ LLL_DEBUG | LLL_PARSER | LLL_HEADER | LLL_EXT |
//~ #if (LWS_LIBRARY_VERSION_MAJOR >= 2 && LWS_LIBRARY_VERSION_MINOR >= 2) || (LWS_LIBRARY_VERSION_MAJOR >= 3)
	//~ LLL_CLIENT | LLL_LATENCY | LLL_USER | LLL_COUNT;
//~ #else
	//~ LLL_CLIENT | LLL_LATENCY | LLL_COUNT;
//~ #endif

static int dnafx_httpws_get_level(int level) {
	switch(level) {
		case LLL_ERR:
			return DNAFX_LOG_ERR;
		case LLL_WARN:
			return DNAFX_LOG_WARN;
		case LLL_NOTICE:
		case LLL_INFO:
		case LLL_DEBUG:
		case LLL_PARSER:
		case LLL_HEADER:
		case LLL_EXT:
		case LLL_CLIENT:
		case LLL_LATENCY:
#if (LWS_LIBRARY_VERSION_MAJOR >= 2 && LWS_LIBRARY_VERSION_MINOR >= 2) || (LWS_LIBRARY_VERSION_MAJOR >= 3)
		case LLL_USER:
#endif
		case LLL_COUNT:
		default:
			return DNAFX_LOG_INFO;
	}
}
static const char *dnafx_httpws_get_level_str(int level) {
	switch(level) {
		case LLL_ERR:
			return "ERR";
		case LLL_WARN:
			return "WARN";
		case LLL_NOTICE:
			return "NOTICE";
		case LLL_INFO:
			return "INFO";
		case LLL_DEBUG:
			return "DEBUG";
		case LLL_PARSER:
			return "PARSER";
		case LLL_HEADER:
			return "HEADER";
		case LLL_EXT:
			return "EXT";
		case LLL_CLIENT:
			return "CLIENT";
		case LLL_LATENCY:
			return "LATENCY";
#if (LWS_LIBRARY_VERSION_MAJOR >= 2 && LWS_LIBRARY_VERSION_MINOR >= 2) || (LWS_LIBRARY_VERSION_MAJOR >= 3)
		case LLL_USER:
			return "USER";
#endif
		case LLL_COUNT:
			return "COUNT";
		default:
			return NULL;
	}
}
static void dnafx_httpws_log_emit_function(int level, const char *line) {
	DNAFX_LOG(dnafx_httpws_get_level(level), "[libwebsockets][%s] %s", dnafx_httpws_get_level_str(level), line);
}

/* Error messages */
typedef enum dnafx_httpws_error {
	DNAFX_HTTPWS_OK = 0,
	DNAFX_HTTPWS_INVALID_JSON = -1,
	DNAFX_HTTPWS_NOT_JSON_OBJECT = -2,
	DNAFX_HTTPWS_INVALID_REQUEST = -3,
	DNAFX_HTTPWS_INVALID_ARGUMENTS = -4,
	DNAFX_HTTPWS_INVALID_ARGUMENT = -5,
	DNAFX_HTTPWS_INVALID_COMMAND = -6,
	DNAFX_HTTPWS_GENERIC_ERROR = -99,
} dnafx_httpws_error;
static const char *dnafx_httpws_error_str(dnafx_httpws_error type) {
	switch(type) {
		case DNAFX_HTTPWS_OK:
			return "OK";
		case DNAFX_HTTPWS_INVALID_JSON:
			return "Invalid JSON";
		case DNAFX_HTTPWS_NOT_JSON_OBJECT:
			return "Not a JSON object";
		case DNAFX_HTTPWS_INVALID_REQUEST:
			return "Invalid request";
		case DNAFX_HTTPWS_INVALID_ARGUMENTS:
			return "Invalid arguments";
		case DNAFX_HTTPWS_INVALID_ARGUMENT:
			return "Invalid argument (not a string)";
		case DNAFX_HTTPWS_INVALID_COMMAND:
			return "Invalid command";
		default:
			return NULL;
	}
};

/* libwebsockets WS context and thread */
static const char *user_agent = "dnafx-editor/0.0.1";
static struct lws_context *wsc = NULL;
static GThread *ws_thread = NULL;
static void *dnafx_httpws_thread(void *data);
/* Callbacks for HTTP */
static int dnafx_httpws_callback_http(struct lws *wsi,
	enum lws_callback_reasons reason, void *user, void *in, size_t len);
/* Callbacks for WebSockets-related events */
static int dnafx_httpws_callback_ws(struct lws *wsi,
	enum lws_callback_reasons reason, void *user, void *in, size_t len);

/* JSON serialization options */
static size_t json_format = JSON_PRESERVE_ORDER;

/* Client session */
typedef struct dnafx_httpws_client {
	struct lws *wsi;
	gboolean websocket;
	char *buffer;
	size_t offset;
	size_t size;
	GAsyncQueue *outgoing;
	unsigned char *outbuffer;
	size_t outbuflen;
	size_t outbufpending;
	size_t outbufoffset;
} dnafx_httpws_client;
static GHashTable *clients = NULL, *writable_clients = NULL;
static dnafx_mutex clients_mutex = DNAFX_MUTEX_INITIALIZER;

/* Protocol mappings */
#define WS_LIST_TERM 0, NULL, 0
#define MESSAGE_CHUNK_SIZE 2800
static struct lws_protocols protocols[] = {
	{ "http-only", dnafx_httpws_callback_http, sizeof(dnafx_httpws_client), 0, WS_LIST_TERM },
	{ "dnafx-protocol", dnafx_httpws_callback_ws, sizeof(dnafx_httpws_client), 0, WS_LIST_TERM },
	{ NULL, NULL, 0, 0, WS_LIST_TERM }
};

/* Helper for debugging reasons */
#define CASE_STR(name) case name: return #name
static const char *dnafx_httpws_reason_string(enum lws_callback_reasons reason) {
	switch(reason) {
		CASE_STR(LWS_CALLBACK_ESTABLISHED);
		CASE_STR(LWS_CALLBACK_CLIENT_CONNECTION_ERROR);
		CASE_STR(LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH);
		CASE_STR(LWS_CALLBACK_CLIENT_ESTABLISHED);
		CASE_STR(LWS_CALLBACK_CLOSED);
		CASE_STR(LWS_CALLBACK_CLOSED_HTTP);
		CASE_STR(LWS_CALLBACK_RECEIVE);
		CASE_STR(LWS_CALLBACK_CLIENT_RECEIVE);
		CASE_STR(LWS_CALLBACK_CLIENT_RECEIVE_PONG);
		CASE_STR(LWS_CALLBACK_CLIENT_WRITEABLE);
		CASE_STR(LWS_CALLBACK_SERVER_WRITEABLE);
		CASE_STR(LWS_CALLBACK_HTTP);
		CASE_STR(LWS_CALLBACK_HTTP_BODY);
		CASE_STR(LWS_CALLBACK_HTTP_BODY_COMPLETION);
		CASE_STR(LWS_CALLBACK_HTTP_FILE_COMPLETION);
		CASE_STR(LWS_CALLBACK_HTTP_WRITEABLE);
		CASE_STR(LWS_CALLBACK_HTTP_BIND_PROTOCOL);
		CASE_STR(LWS_CALLBACK_HTTP_DROP_PROTOCOL);
		CASE_STR(LWS_CALLBACK_HTTP_CONFIRM_UPGRADE);
		CASE_STR(LWS_CALLBACK_ADD_HEADERS);
		CASE_STR(LWS_CALLBACK_FILTER_NETWORK_CONNECTION);
		CASE_STR(LWS_CALLBACK_FILTER_HTTP_CONNECTION);
		CASE_STR(LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED);
		CASE_STR(LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION);
		CASE_STR(LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS);
		CASE_STR(LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS);
		CASE_STR(LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION);
		CASE_STR(LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER);
		CASE_STR(LWS_CALLBACK_CONFIRM_EXTENSION_OKAY);
		CASE_STR(LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED);
		CASE_STR(LWS_CALLBACK_PROTOCOL_INIT);
		CASE_STR(LWS_CALLBACK_PROTOCOL_DESTROY);
		CASE_STR(LWS_CALLBACK_WSI_CREATE);
		CASE_STR(LWS_CALLBACK_WSI_DESTROY);
		CASE_STR(LWS_CALLBACK_GET_THREAD_ID);
		CASE_STR(LWS_CALLBACK_ADD_POLL_FD);
		CASE_STR(LWS_CALLBACK_DEL_POLL_FD);
		CASE_STR(LWS_CALLBACK_CHANGE_MODE_POLL_FD);
		CASE_STR(LWS_CALLBACK_LOCK_POLL);
		CASE_STR(LWS_CALLBACK_UNLOCK_POLL);
		CASE_STR(LWS_CALLBACK_USER);
		CASE_STR(LWS_CALLBACK_RECEIVE_PONG);
		CASE_STR(LWS_CALLBACK_EVENT_WAIT_CANCELLED);
		default:
			break;
	}
	return NULL;
}

/* Server management */
int dnafx_httpws_init(uint16_t port) {
	if(port == 0)
		return 0;
	DNAFX_LOG(DNAFX_LOG_INFO, "Starting HTTP/WebSocket server on port %"SCNu16"\n", port);
	/* Initialize hashtables and mutex */
	clients = g_hash_table_new(NULL, NULL);
	writable_clients = g_hash_table_new(NULL, NULL);
	/* Logging */
	lws_set_log_level(ws_log_level, dnafx_httpws_log_emit_function);
	/* Prepare the common context */
	struct lws_context_creation_info wscinfo;
	memset(&wscinfo, 0, sizeof wscinfo);
	wscinfo.options |= LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
	wscinfo.count_threads = 1;
	/* Create the base context */
	wsc = lws_create_context(&wscinfo);
	if(wsc == NULL) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Error creating libwebsockets context\n");
		return -1;
	}
	/* Prepare the server context */
	struct lws_context_creation_info info = { 0 };
	info.port = port;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
#if (LWS_LIBRARY_VERSION_MAJOR == 3 && LWS_LIBRARY_VERSION_MINOR >= 2) || (LWS_LIBRARY_VERSION_MAJOR > 3)
	info.options |= LWS_SERVER_OPTION_FAIL_UPON_UNABLE_TO_BIND;
#endif
	/* Create the server context */
	struct lws_vhost *server = lws_create_vhost(wsc, &info);
	if(server == NULL) {
		DNAFX_LOG(DNAFX_LOG_FATAL, "Error creating HTTP/WebSocket server\n");
		return -1;
	}
	/* Start the service thread */
	GError *error = NULL;
	ws_thread = g_thread_try_new("dnafx-ws", &dnafx_httpws_thread, NULL, &error);
	if(error != NULL) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Got error %d (%s) trying to launch the WebSockets thread...\n",
			error->code, error->message ? error->message : "??");
		g_error_free(error);
		return -1;
	}
	/* Done */
	return 0;
}

void dnafx_httpws_deinit(void) {
	if(wsc == NULL)
		return;
#if ((LWS_LIBRARY_VERSION_MAJOR == 3 && LWS_LIBRARY_VERSION_MINOR >= 2) || LWS_LIBRARY_VERSION_MAJOR >= 4)
	lws_cancel_service(wsc);
#endif
	/* Stop the service thread */
	if(ws_thread != NULL) {
		g_thread_join(ws_thread);
		ws_thread = NULL;
	}
	/* Destroy the context */
	if(wsc != NULL) {
		lws_context_destroy(wsc);
		wsc = NULL;
	}

	dnafx_mutex_lock(&clients_mutex);
	g_hash_table_destroy(clients);
	clients = NULL;
	g_hash_table_destroy(writable_clients);
	writable_clients = NULL;
	dnafx_mutex_unlock(&clients_mutex);
}

/* Loop thread */
static void *dnafx_httpws_thread(void *data) {
	if(wsc == NULL) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Invalid service\n");
		return NULL;
	}
	DNAFX_LOG(DNAFX_LOG_INFO, "HTTP/WebSocket server started\n");
	while(dnafx_is_running())
		lws_service(wsc, 50);
	/* Done */
	lws_cancel_service(wsc);
	DNAFX_LOG(DNAFX_LOG_INFO, "HTTP/WebSocket server stopped\n");
	return NULL;

}

/* Helper to write responses (HTTP) */
static int dnafx_httpws_write_http_response(struct lws *wsi, int code, const char *text, const char *ctype) {
	uint8_t payload[LWS_PRE + 256];
	uint8_t *start = &payload[LWS_PRE], *p = start,
		*end = &payload[sizeof(payload) - LWS_PRE];
	int res = lws_add_http_header_status(wsi, code, &p, end);
	if(res != 0)
		return res;
	res |= lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_USER_AGENT,
		(unsigned char *)user_agent, strlen(user_agent), &p, end);
	if(res != 0)
		return res;
	res |= lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
		(unsigned char *)ctype, strlen(ctype), &p, end);
	if(res != 0)
		return res;
	char cl[10];
	g_snprintf(cl, sizeof(cl), "%zu", strlen(text));
	res |= lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_LENGTH,
		(unsigned char *)cl, strlen(cl), &p, end);
	if(res != 0)
		return res;
	res |= lws_finalize_http_header(wsi, &p, end);
	if(res != 0)
		return res;
	size_t len = p-start;
	res = lws_write(wsi, start, len, LWS_WRITE_HTTP_HEADERS);
	if(res != len)
		return res;
	len = strlen(text);
	res = lws_write(wsi, (void *)text, len, LWS_WRITE_HTTP);
	if(res != len)
		return res;
	return 0;
}

static char *dnafx_httpws_create_reason(int code, const char *text) {
	json_t *response = json_object();
	json_object_set_new(response, "code", json_integer(code));
	if(text != NULL) {
		json_t *body = json_object();
		json_object_set_new(body, "reason", json_string(text));
		json_object_set_new(response, "payload", body);
	}
	char *json = json_dumps(response, json_format);
	json_decref(response);
	return json;
}

static char *dnafx_httpws_create_payload(int code, json_t *body) {
	json_t *response = json_object();
	json_object_set_new(response, "code", json_integer(code));
	if(body != NULL)
		json_object_set_new(response, "payload", body);
	char *json = json_dumps(response, json_format);
	json_decref(response);
	return json;
}

/* Helper to process an incoming command */
static dnafx_httpws_error dnafx_httpws_handle_request(dnafx_httpws_client *client) {
	if(client == NULL || client->buffer == NULL)
		return DNAFX_HTTPWS_GENERIC_ERROR;
	json_error_t error;
	json_t *json = json_loads(client->buffer, 0, &error);
	if(json == NULL) {
		DNAFX_LOG(DNAFX_LOG_ERR, "JSON error: on line %d: %s\n", error.line, error.text);
		return DNAFX_HTTPWS_INVALID_JSON;
	}
	if(!json_is_object(json)) {
		json_decref(json);
		DNAFX_LOG(DNAFX_LOG_ERR, "JSON error: not an object\n");
		return DNAFX_HTTPWS_NOT_JSON_OBJECT;
	}
	json_t *request = json_object_get(json, "request");
	if(request == NULL || !json_is_string(request)) {
		json_decref(json);
		DNAFX_LOG(DNAFX_LOG_ERR, "Invalid request\n");
		return DNAFX_HTTPWS_INVALID_REQUEST;
	}
	json_t *args = json_object_get(json, "arguments");
	if(args != NULL && !json_is_array(args)) {
		json_decref(json);
		DNAFX_LOG(DNAFX_LOG_ERR, "Invalid arguments\n");
		return DNAFX_HTTPWS_INVALID_ARGUMENTS;
	}
	size_t argc = 1 + json_array_size(args);
	char **argv = g_malloc(argc * sizeof(char *));
	argv[0] = (char *)json_string_value(request);
	size_t i = 0;
	json_t *arg = NULL;
	for(i=0; i<json_array_size(args); i++) {
		arg = json_array_get(args, i);
		if(arg == NULL || !json_is_string(arg)) {
			g_free(argv);
			json_decref(json);
			return DNAFX_HTTPWS_INVALID_ARGUMENT;
		}
		argv[i+1] = (char *)json_string_value(arg);
	}
	dnafx_task *task = dnafx_task_new(argc, argv);
	g_free(argv);
	json_decref(json);
	if(task == NULL) {
		DNAFX_LOG(DNAFX_LOG_ERR, "Invalid command\n");
		return DNAFX_HTTPWS_INVALID_COMMAND;
	}
	dnafx_task_add_context(task, client, &dnafx_httpws_task_done);
	dnafx_tasks_add(task);
	return DNAFX_HTTPWS_OK;
}

/* HTTP callback */
static int dnafx_httpws_callback_http(struct lws *wsi,
		enum lws_callback_reasons reason, void *user, void *in, size_t len) {
	dnafx_httpws_client *client = (dnafx_httpws_client *)user;
	switch(reason) {
		case LWS_CALLBACK_HTTP:
			if(lws_hdr_total_length(wsi, WSI_TOKEN_POST_URI)) {
				client->wsi = wsi;
				client->websocket = FALSE;
				client->buffer = NULL;
				client->offset = 0;
				client->size = 0;
				client->outgoing = g_async_queue_new_full((GDestroyNotify)g_free);
				client->outbuffer = NULL;
				/* Track this connection */
				dnafx_mutex_lock(&clients_mutex);
				g_hash_table_insert(clients, client, client);
				dnafx_mutex_unlock(&clients_mutex);
				lws_callback_on_writable(wsi);
				return 0;
			}
			/* If we got here, we reject it */
			dnafx_httpws_write_http_response(wsi, 404, "Use POST", "text/html");
			/* Close and free connection */
			return -1;
		case LWS_CALLBACK_HTTP_BODY: {
			if(client->buffer == NULL) {
				client->size = 1024;
				client->buffer = g_malloc0(client->size);
			}
			while((len + 1) > client->size) {
				client->size += 1024;
				client->buffer = g_realloc(client->buffer, client->size);
			}
			memcpy(client->buffer + client->offset, in, len);
			client->offset += len;
			return 0;
		}
		case LWS_CALLBACK_HTTP_BODY_COMPLETION: {
			*(client->buffer + client->offset) = '\0';
			DNAFX_LOG(DNAFX_LOG_INFO, "[HTTP] %s\n", client->buffer);
			dnafx_httpws_error res = dnafx_httpws_handle_request(client);
			if(res != DNAFX_HTTPWS_OK) {
				char *json = dnafx_httpws_create_reason(400, dnafx_httpws_error_str(res));
				dnafx_httpws_write_http_response(wsi, 200, json, "application/json");
				free(json);
				/* Close and free connection */
				return 1;
			}
			/* We'll wait for the callback to be called to send a response */
			return 0;
		}
		case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
			dnafx_mutex_lock(&clients_mutex);
			/* We iterate on all the clients we marked as writable and act on them */
			GHashTableIter iter;
			gpointer value;
			g_hash_table_iter_init(&iter, writable_clients);
			while(g_hash_table_iter_next(&iter, NULL, &value)) {
				dnafx_httpws_client *client = value;
				if(client == NULL || client->wsi == NULL)
					continue;
				lws_callback_on_writable(client->wsi);
			}
			g_hash_table_remove_all(writable_clients);
			dnafx_mutex_unlock(&clients_mutex);
			return 0;
		}
		case LWS_CALLBACK_HTTP_WRITEABLE: {
			/* See if there's a message to send */
			char *response = g_async_queue_try_pop(client->outgoing);
			if(response == NULL)
				return 0;
			dnafx_httpws_write_http_response(wsi, 200, response, "application/json");
			g_free(response);
			/* Close and free connection */
			return 1;
		}
		case LWS_CALLBACK_GET_THREAD_ID:
			return (uint64_t)pthread_self();
		case LWS_CALLBACK_CLOSED:
		case LWS_CALLBACK_WSI_DESTROY: {
			if(client != NULL) {
				g_free(client->buffer);
				client->buffer = NULL;
				g_free(client->outbuffer);
				client->outbuffer = NULL;
				if(client->outgoing != NULL)
					g_async_queue_unref(client->outgoing);
				client->outgoing = NULL;
			}
			return 0;
		}
		default:
			if(wsi != NULL) {
				DNAFX_LOG(DNAFX_LOG_HUGE, "[HTTP-%p] %d (%s)\n", wsi, reason, dnafx_httpws_reason_string(reason));
			} else {
				DNAFX_LOG(DNAFX_LOG_HUGE, "[HTTP] %d (%s)\n", reason, dnafx_httpws_reason_string(reason));
			}
			break;
	}
	return 0;
}

/* WebSocket callback */
static int dnafx_httpws_callback_ws(struct lws *wsi,
		enum lws_callback_reasons reason, void *user, void *in, size_t len) {
	dnafx_httpws_client *client = (dnafx_httpws_client *)user;
	switch(reason) {
		case LWS_CALLBACK_ESTABLISHED: {
			client->wsi = wsi;
			client->websocket = TRUE;
			client->buffer = NULL;
			client->offset = 0;
			client->size = 0;
			client->outgoing = g_async_queue_new_full((GDestroyNotify)g_free);
			client->outbuffer = NULL;
			/* Let us know when the WebSocket channel becomes writeable */
			dnafx_mutex_lock(&clients_mutex);
			g_hash_table_insert(clients, client, client);
			dnafx_mutex_unlock(&clients_mutex);
			lws_callback_on_writable(wsi);
			return 0;
		}
		case LWS_CALLBACK_ADD_HEADERS: {
			/* Add the user agent header */
			struct lws_process_html_args *args = (struct lws_process_html_args *)in;
			if(lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_USER_AGENT,
					(unsigned char *)user_agent, strlen(user_agent),
					(unsigned char **)&args->p, (unsigned char *)args->p + args->max_len))
				return 1;
			return 0;
		}
		case LWS_CALLBACK_RECEIVE: {
			const size_t remaining = lws_remaining_packet_payload(wsi);
			if(client->buffer == NULL) {
				client->size = 1024;
				client->buffer = g_malloc0(client->size);
			}
			while((len + 1) > client->size) {
				client->size += 1024;
				client->buffer = g_realloc(client->buffer, client->size);
			}
			memcpy(client->buffer + client->offset, in, len);
			client->offset += len;
			if(remaining > 0 || !lws_is_final_fragment(wsi)) {
				/* Still waiting for some more fragments */
				DNAFX_LOG(DNAFX_LOG_INFO, "[WS-%p] Waiting for more fragments\n", wsi);
				lws_callback_on_writable(wsi);
				return 0;
			}
			*(client->buffer + client->offset) = '\0';
			DNAFX_LOG(DNAFX_LOG_INFO, "[WS] %s\n", client->buffer);
			dnafx_httpws_error res = dnafx_httpws_handle_request(client);
			if(res != DNAFX_HTTPWS_OK) {
				char *json = dnafx_httpws_create_reason(400, dnafx_httpws_error_str(res));
				g_async_queue_push(client->outgoing, g_strdup(json));
				free(json);
			} else {
				/* FIXME */
				char *json = dnafx_httpws_create_reason(200, "Command queued");
				g_async_queue_push(client->outgoing, g_strdup(json));
				free(json);
			}
			client->offset = 0;
			lws_callback_on_writable(wsi);
			return 0;
		}
		case LWS_CALLBACK_EVENT_WAIT_CANCELLED: {
			dnafx_mutex_lock(&clients_mutex);
			/* We iterate on all the clients we marked as writable and act on them */
			GHashTableIter iter;
			gpointer value;
			g_hash_table_iter_init(&iter, writable_clients);
			while(g_hash_table_iter_next(&iter, NULL, &value)) {
				dnafx_httpws_client *client = value;
				if(client == NULL || client->wsi == NULL)
					continue;
				lws_callback_on_writable(client->wsi);
			}
			g_hash_table_remove_all(writable_clients);
			dnafx_mutex_unlock(&clients_mutex);
			return 0;
		}
		case LWS_CALLBACK_SERVER_WRITEABLE: {
			if(client == NULL || client->wsi == NULL) {
				DNAFX_LOG(DNAFX_LOG_ERR, "[WS-%p] Invalid WebSocket client instance\n", wsi);
				return -1;
			}
			/* Check if Websockets send pipe is choked */
			if(lws_send_pipe_choked(wsi)) {
				if(client->outbuffer && client->outbufpending > 0 && client->outbufoffset > 0) {
					DNAFX_LOG(DNAFX_LOG_WARN, "[WS-%p] Websockets choked with buffer: %zu, trying again\n", wsi, client->outbufpending);
					lws_callback_on_writable(wsi);
				} else {
					gint qlen = g_async_queue_length(client->outgoing);
					DNAFX_LOG(DNAFX_LOG_WARN, "[WS-%p] Websockets choked with queue: %d, trying again\n", wsi, qlen);
					if(qlen > 0) {
						lws_callback_on_writable(wsi);
					}
				}
				return 0;
			}
			/* Check if we have a pending/partial write to complete first */
			if(client->outbuffer && client->outbufpending > 0 && client->outbufoffset > 0) {
				DNAFX_LOG(DNAFX_LOG_INFO, "[WS-%p] Completing pending WebSocket write (still need to write last %zu bytes)...\n",
					wsi, client->outbufpending);
			} else {
				/* Shoot all the pending messages */
				char *response = g_async_queue_try_pop(client->outgoing);
				if(response == NULL)
					return 0;
				size_t buflen = LWS_PRE + strlen(response);
				if(buflen > client->outbuflen) {
					client->outbuflen = buflen;
					client->outbuffer = g_realloc(client->outbuffer, buflen);
				}
				memcpy(client->outbuffer + LWS_PRE, response, strlen(response));
				client->outbufpending = strlen(response);
				client->outbufoffset = LWS_PRE;
				g_free(response);
			}
			int amount = client->outbufpending <= MESSAGE_CHUNK_SIZE ? client->outbufpending : MESSAGE_CHUNK_SIZE;
			int flags = lws_write_ws_flags(LWS_WRITE_TEXT, client->outbufoffset == LWS_PRE, client->outbufpending <= (size_t)amount);
			int sent = lws_write(wsi, client->outbuffer + client->outbufoffset, (size_t)amount, flags);
			if(sent < amount) {
				DNAFX_LOG(DNAFX_LOG_WARN, "[WS] Only sent %d bytes (expected %d)\n", sent, amount);
				client->outbufpending = 0;
				client->outbufoffset = 0;
			} else {
				client->outbufpending -= amount;
				client->outbufoffset += amount;
			}
			lws_callback_on_writable(wsi);
			return 0;
		}
		case LWS_CALLBACK_GET_THREAD_ID:
			return (uint64_t)pthread_self();
		case LWS_CALLBACK_CLOSED:
		case LWS_CALLBACK_WSI_DESTROY: {
			if(client != NULL) {
				dnafx_mutex_lock(&clients_mutex);
				g_hash_table_remove(clients, client);
				g_hash_table_remove(writable_clients, client);
				dnafx_mutex_unlock(&clients_mutex);
				g_free(client->buffer);
				client->buffer = NULL;
				g_free(client->outbuffer);
				client->outbuffer = NULL;
				if(client->outgoing != NULL)
					g_async_queue_unref(client->outgoing);
				client->outgoing = NULL;
			}
			return 0;
		}
		default:
			if(wsi != NULL) {
				DNAFX_LOG(DNAFX_LOG_HUGE, "[WS-%p] %d (%s)\n", wsi, reason, dnafx_httpws_reason_string(reason));
			} else {
				DNAFX_LOG(DNAFX_LOG_HUGE, "[WS] %d (%s)\n", reason, dnafx_httpws_reason_string(reason));
			}
			break;
	}
	return 0;
}

/* Task completion */
void dnafx_httpws_task_done(int code, void *result, void *user_data) {
	dnafx_httpws_client *client = (dnafx_httpws_client *)user_data;
	if(client == NULL)
		return;
	dnafx_mutex_lock(&clients_mutex);
	if(g_hash_table_lookup(clients, client) == client) {
		char *json = dnafx_httpws_create_payload(code, result);
		g_async_queue_push(client->outgoing, g_strdup(json));
		free(json);
		g_hash_table_insert(writable_clients, client, client);
	}
	dnafx_mutex_unlock(&clients_mutex);
	lws_cancel_service(wsc);
}
