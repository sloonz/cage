#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <wlr/util/log.h>

#include "ipc.h"
#include "server.h"
#include "output.h"
#include "seat.h"

static const char GET_CURSOR_POS[] = "get_cursor_pos";
static const char ENABLE_FORCE_REFRESH[] = "enable_force_refresh";
static const char DISABLE_FORCE_REFRESH[] = "disable_force_refresh";
static const char INVALID_COMMAND[] = "invalid_command";

struct cg_ipc_client {
	struct cg_server *server;
	int fd;

	char *write_buffer;
	size_t write_buffer_cap;
	size_t write_buffer_size;

	char *read_buffer;
	size_t read_buffer_cap;
	size_t read_buffer_size;

	struct wl_event_source *read_event_source;
	struct wl_event_source *write_event_source;
};

static void ipc_client_destroy(struct cg_ipc_client *client) {
	close(client->fd);
	wl_event_source_remove(client->read_event_source);
	if(client->write_event_source != NULL) {
		wl_event_source_remove(client->write_event_source);
	}
	free(client->write_buffer);
	free(client->read_buffer);
	free(client);
}

static int ipc_handle_write(int fd, uint32_t mask, void *data) {
	struct cg_ipc_client *client = data;

	if(mask & WL_EVENT_ERROR) {
		wlr_log(WLR_ERROR, "IPC client error");
		ipc_client_destroy(client);
		return 0;
	}

	if(mask & WL_EVENT_HANGUP) {
		ipc_client_destroy(client);
		return 0;
	}

	ssize_t size = send(client->fd, client->write_buffer, client->write_buffer_size, 0);
	if(size == -1) {
		wlr_log(WLR_ERROR, "IPC write error");
		ipc_client_destroy(client);
		return 0;
	}

	memmove(client->write_buffer, client->write_buffer + size, client->write_buffer_size - size);
	client->write_buffer_size -= size;
	if(client->write_buffer_size == 0 && client->write_event_source) {
		wl_event_source_remove(client->write_event_source);
		client->write_event_source = NULL;
	} else if(client->write_buffer_size > 0 && client->write_event_source == NULL) {
		client->write_event_source = wl_event_loop_add_fd(wl_display_get_event_loop(client->server->wl_display),
			client->fd, WL_EVENT_WRITABLE, ipc_handle_write, client);
	}

	return 0;
}

static void ipc_client_write(struct cg_ipc_client *client, const char *message, uint16_t size) {
	uint16_t full_size = size + sizeof(uint16_t);

	if(client->write_buffer_size + full_size > client->write_buffer_cap) {
		wlr_log(WLR_ERROR, "IPC client write overflow");
		ipc_client_destroy(client);
		return;
	}

	memcpy(client->write_buffer + client->write_buffer_size, &full_size, sizeof(uint16_t));
	client->write_buffer_size += sizeof(uint16_t);

	memcpy(client->write_buffer + client->write_buffer_size, message, size);
	client->write_buffer_size += size;

	ipc_handle_write(client->fd, 0, client);
}

static void ipc_client_handle_message(struct cg_ipc_client *client, char *message, size_t size) {
	if(!strncmp(message, GET_CURSOR_POS, sizeof(GET_CURSOR_POS)-1)) {
		uint32_t x = client->server->seat->cursor->x;
		uint32_t y = client->server->seat->cursor->y;
		uint32_t pos[2] = {x, y};
		ipc_client_write(client, (char*)&pos[0], sizeof(pos));
	} else if(!strncmp(message, ENABLE_FORCE_REFRESH, sizeof(ENABLE_FORCE_REFRESH)-1)) {
		client->server->force_refresh = true;
		struct cg_output *output;
		wl_list_for_each (output, &client->server->outputs, link) {
			wl_event_source_timer_update(output->timer, FORCED_REFRESH_DELAY);
		}
	} else if(!strncmp(message, DISABLE_FORCE_REFRESH, sizeof(DISABLE_FORCE_REFRESH)-1)) {
		client->server->force_refresh = false;
	} else {
		wlr_log(WLR_ERROR, "IPC invalid command");
		ipc_client_write(client, INVALID_COMMAND, sizeof(INVALID_COMMAND)-1);
	}
}

static int ipc_handle_read(int fd, uint32_t mask, void *data) {
	struct cg_ipc_client *client = data;

	if(mask & WL_EVENT_ERROR) {
		wlr_log(WLR_ERROR, "IPC client error");
		ipc_client_destroy(client);
		return 0;
	}

	if(mask & WL_EVENT_HANGUP) {
		ipc_client_destroy(client);
		return 0;
	}

	ssize_t sz = recv(client->fd,
			client->read_buffer + client->read_buffer_size,
			client->read_buffer_cap - client->read_buffer_size, 0);

	if(sz == -1) {
		wlr_log(WLR_ERROR, "Failed to read from client");
		ipc_client_destroy(client);
		return 0;
	}

	client->read_buffer_size += sz;

	while(client->read_buffer_size >= sizeof(uint16_t)) {
		uint16_t msg_size = *(uint16_t*)client->read_buffer;
		if(client->read_buffer_size >= msg_size) {
			ipc_client_handle_message(client, client->read_buffer + sizeof(uint16_t), msg_size - sizeof(uint16_t));
			memmove(client->read_buffer, client->read_buffer + msg_size, client->read_buffer_size - msg_size);
			client->read_buffer_size -= msg_size;
		} else {
			break;
		}
	}

	return 0;
}

static int ipc_handle_connection(int fd, uint32_t mask, void *data) {
	struct cg_server *server = data;

	int client_fd = accept(fd, NULL, NULL);
	if(client_fd == -1) {
		wlr_log(WLR_ERROR, "IPC: failed to accept");
		return 0;
	}

	struct cg_ipc_client *client = calloc(1, sizeof(struct cg_ipc_client));
	if(client == NULL) {
		wlr_log(WLR_ERROR, "calloc() failed");
		close(client_fd);
		return 0;
	}

	client->server = server;
	client->fd = client_fd;
	client->write_buffer_cap = 512;
	client->read_buffer_cap = 512;
	client->write_buffer = malloc(client->write_buffer_cap);
	client->read_buffer = malloc(client->read_buffer_cap);
	if(client->write_buffer == NULL || client->read_buffer == NULL) {
		wlr_log(WLR_ERROR, "malloc() failed");
		close(client_fd);
		return 0;
	}

	client->read_event_source = wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display),
		client_fd, WL_EVENT_READABLE, ipc_handle_read, client);

	return 0;
}

void ipc_init(struct cg_server *server) {
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if(sock == -1) {
		wlr_log(WLR_ERROR, "Failed to create ipc socket");
		return;
	}

	struct sockaddr_un *sockaddr = calloc(1, sizeof(struct sockaddr_un));
	sockaddr->sun_family = AF_UNIX;
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if(!dir) {
		wlr_log(WLR_ERROR, "Cannot create ipc socket: missing $XDG_RUNTIME_DIR");
		return;
	}
	if((size_t)snprintf(sockaddr->sun_path, sizeof(sockaddr->sun_path), "%s/cage-ipc.sock", dir) >= sizeof(sockaddr->sun_path)) {
		wlr_log(WLR_ERROR, "Cannot create ipc socket: missing $XDG_RUNTIME_DIR");
		return;
	}

	unlink(sockaddr->sun_path);

	if(bind(sock, (struct sockaddr*)sockaddr, sizeof(struct sockaddr_un)) == -1) {
		wlr_log(WLR_ERROR, "Cannot bind IPC socket");
		return;
	}

	if(listen(sock, 1) == -1) {
		wlr_log(WLR_ERROR, "Cannot listen IPC socket");
		return;
	}

	wl_event_loop_add_fd(wl_display_get_event_loop(server->wl_display),
		sock, WL_EVENT_READABLE, ipc_handle_connection, server);
}
