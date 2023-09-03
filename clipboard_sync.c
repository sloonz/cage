#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_data_device.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

#include "server.h"
#include "seat.h"
#include "clipboard_sync.h"

struct cg_clipboard_sync {
	struct cg_server *server;
	struct zwlr_data_control_device_v1 *data_control_device;

	struct zwlr_data_control_offer_v1 *remote_primary_selection_offer;
	struct zwlr_data_control_offer_v1 *remote_discarded_selection_offer;

	// A bit hackish
	//
	// If we copy from a caged client, we use data control manager to set selection on host.
	//
	// But then the host notifies us that a new selection is here, and we want to set cage
	// selection to that new selection... destroying the selection in the process.
	//
	// So just ignore remote selection events in the roundtrip setting up a selection from
	// a caged client
	bool ignore_remote_selection;
};

/* Copy from cage to host */

struct cg_client_data_source {
	struct wlr_data_source *client_source;
	struct zwlr_data_control_source_v1 *remote_source;
	struct wl_listener client_source_destroy_listener;
};

static void handle_client_data_source_destroy(struct wl_listener *listener, void *data) {
	struct cg_client_data_source *cg_client_data_source = wl_container_of(listener, cg_client_data_source, client_source_destroy_listener);
	wl_list_remove(&cg_client_data_source->client_source_destroy_listener.link);
	zwlr_data_control_source_v1_destroy(cg_client_data_source->remote_source);
}

static void remote_data_source_handle_send(void *data, struct zwlr_data_control_source_v1 *remote_data_source, const char *mime_type, int fd) {
	struct cg_client_data_source *cg_client_data_source = data;
	wlr_data_source_send(cg_client_data_source->client_source, mime_type, fd);
}

static void remote_data_source_handle_cancelled(void *data, struct zwlr_data_control_source_v1 *remote_data_source) {
	// We must not destroy it because wlr_seat took ownership of it
}

static const struct zwlr_data_control_source_v1_listener remote_data_source_listener = {
	.send = remote_data_source_handle_send,
	.cancelled = remote_data_source_handle_cancelled,
};

void clipboard_sync_handle_set_selection(struct cg_server *server, struct wlr_data_source *client_data_source) {
	struct cg_clipboard_sync *sync = server->remote_clipboard_sync;

	if(!sync) {
		return;
	}

	if(client_data_source != NULL) {
		struct cg_client_data_source *cg_client_data_source = calloc(1, sizeof(struct cg_client_data_source));
		if(cg_client_data_source == NULL) {
			wlr_log(WLR_ERROR, "calloc() failed");
			return;
		}

		cg_client_data_source->remote_source = zwlr_data_control_manager_v1_create_data_source(sync->server->remote_data_control_manager);
		cg_client_data_source->client_source = client_data_source;
		cg_client_data_source->client_source_destroy_listener.notify = handle_client_data_source_destroy;

		char **p;
		wl_array_for_each(p, &client_data_source->mime_types) {
			wlr_log(WLR_DEBUG, "sending mime type: %s", *p);
			zwlr_data_control_source_v1_offer(cg_client_data_source->remote_source, *p);
		}
		zwlr_data_control_device_v1_set_selection(sync->data_control_device, cg_client_data_source->remote_source);
		zwlr_data_control_source_v1_add_listener(cg_client_data_source->remote_source, &remote_data_source_listener, cg_client_data_source);
		sync->ignore_remote_selection = true;
		wl_display_roundtrip(server->remote_display);
		sync->ignore_remote_selection = false;

		wl_signal_add(&client_data_source->events.destroy, &cg_client_data_source->client_source_destroy_listener);
	}
}

/* Copy from host to cage */

struct cg_remote_data_source {
	struct wlr_data_source base;
	struct cg_server *server;
	struct zwlr_data_control_offer_v1 *offer;
};

static void remote_offer_handle_offer(void *data, struct zwlr_data_control_offer_v1 *offer, const char *mime_type) {
	struct cg_remote_data_source *cg_remote_data_source = data;

	char **p = wl_array_add(&cg_remote_data_source->base.mime_types, sizeof(*p));
	assert(p != NULL);
	*p = strdup(mime_type);
}

static const struct zwlr_data_control_offer_v1_listener remote_offer_listener = {
	.offer = remote_offer_handle_offer,
};

static void remote_data_source_send(struct wlr_data_source *source, const char *mime_type, int fd) {
	struct cg_remote_data_source *cg_remote_data_source = (void*)source;
	zwlr_data_control_offer_v1_receive(cg_remote_data_source->offer, mime_type, fd);
	wl_display_flush(cg_remote_data_source->server->remote_display);
	close(fd);
}

static void remote_data_source_destroy(struct wlr_data_source *source) {
	struct cg_remote_data_source *cg_remote_data_source = (void*)source;
	zwlr_data_control_offer_v1_destroy(cg_remote_data_source->offer);
	free(source);
}

static const struct wlr_data_source_impl remote_data_source_impl = {
	.send = remote_data_source_send,
	.accept = NULL,
	.destroy = remote_data_source_destroy,
	.dnd_drop = NULL,
	.dnd_finish = NULL,
	.dnd_action = NULL,
};

static void remote_data_device_handle_data_offer(void *data, struct zwlr_data_control_device_v1 *device, struct zwlr_data_control_offer_v1 *offer) {
	struct cg_server *server = data;
	struct cg_clipboard_sync *sync = server->remote_clipboard_sync;

	if(!sync) {
		return;
	}

	struct cg_remote_data_source *cg_remote_data_source = calloc(1, sizeof(struct cg_remote_data_source));
	if(!cg_remote_data_source) {
		wlr_log(WLR_ERROR, "calloc() failed");
		return;
	}

	cg_remote_data_source->offer = offer;
	cg_remote_data_source->server = server;

	wlr_data_source_init(&cg_remote_data_source->base, &remote_data_source_impl);
	zwlr_data_control_offer_v1_add_listener(offer, &remote_offer_listener, cg_remote_data_source);
}

static void remote_data_device_handle_selection(void *data, struct zwlr_data_control_device_v1 *device, struct zwlr_data_control_offer_v1 *offer) {
	struct cg_server *server = data;
	struct cg_clipboard_sync *sync = server->remote_clipboard_sync;

	if(!sync) {
		return;
	}

	if(sync->remote_discarded_selection_offer) {
		zwlr_data_control_offer_v1_destroy(sync->remote_discarded_selection_offer);
		sync->remote_discarded_selection_offer = NULL;
	}

	if(offer != NULL) {
		if(sync->ignore_remote_selection) {
			sync->remote_discarded_selection_offer = offer;
		} else {
			struct cg_remote_data_source *remote_data_source = zwlr_data_control_offer_v1_get_user_data(offer);
			assert(remote_data_source && remote_data_source->base.impl == &remote_data_source_impl);
			wlr_seat_set_selection(server->seat->seat, &remote_data_source->base, wl_display_next_serial(server->wl_display));
		}
	}
}

static void remote_data_device_handle_primary_selection(void *data, struct zwlr_data_control_device_v1 *device, struct zwlr_data_control_offer_v1 *offer) {
	struct cg_server *server = data;
	struct cg_clipboard_sync *sync = server->remote_clipboard_sync;

	if(!sync) {
		return;
	}

	if(sync->remote_primary_selection_offer) {
		zwlr_data_control_offer_v1_destroy(sync->remote_primary_selection_offer);
	}

	sync->remote_primary_selection_offer = offer;
}

static void remote_data_device_handle_finished(void *data, struct zwlr_data_control_device_v1 *device) {
	struct cg_server *server = data;
	server->remote_clipboard_sync = NULL;
	wlr_log(WLR_ERROR, "remote zwlr_data_control_device_v1 disappeared");
	return;
}

static const struct zwlr_data_control_device_v1_listener data_control_device_listener = {
	.data_offer = remote_data_device_handle_data_offer,
	.selection = remote_data_device_handle_selection,
	.finished = remote_data_device_handle_finished,
	.primary_selection = remote_data_device_handle_primary_selection,
};

/* Common */

void clipboard_sync_init(struct cg_server *server) {
	if(!server->remote_data_control_manager) {
		wlr_log(WLR_ERROR, "zwlr_data_control_manager_v1 not available");
		return;
	}

	struct cg_clipboard_sync *sync = calloc(1, sizeof(struct cg_clipboard_sync));
	if(sync == NULL) {
		wlr_log(WLR_ERROR, "calloc() failed");
		return;
	}

	server->remote_clipboard_sync = sync;

	sync->server = server;
	sync->data_control_device = zwlr_data_control_manager_v1_get_data_device(server->remote_data_control_manager, server->remote_seat);
	zwlr_data_control_device_v1_add_listener(sync->data_control_device, &data_control_device_listener, server);
}
