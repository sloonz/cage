// TODO: implement region

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-client-protocol.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "pointer-constraints-unstable-v1-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"

#include "output.h"
#include "pointer_constraints.h"

enum cg_pointer_constraint_type {
	CG_POINTER_CONSTRAINT_LOCKED,
	CG_POINTER_CONSTRAINT_CONFINED,
};

struct cg_pointer_constraint {
	struct cg_pointer_constraints *constraints;
	struct wlr_surface *surface;
	struct wl_listener surface_destroy;
	struct wl_resource *resource;
	struct zwp_locked_pointer_v1 *remote_locked_pointer;
	struct zwp_confined_pointer_v1 *remote_confined_pointer;
	enum cg_pointer_constraint_type type;
};

struct cg_pointer_constraints {
	struct cg_server *server;
	struct wl_pointer *remote_pointer;
};

static void resource_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

/* Constrained pointer */

static void pointer_constraint_set_region(struct wl_client *client, struct wl_resource *constraint_res, struct wl_resource *region_res) {
	wlr_log(WLR_INFO, "set_region not yet supported");
}

static void pointer_constraint_set_cursor_position_hint(struct wl_client *client, struct wl_resource *constraint_res, wl_fixed_t x, wl_fixed_t y) {
	struct cg_pointer_constraint *constraint = wl_resource_get_user_data(constraint_res);
	if(constraint == NULL) {
		return;
	}

	if(constraint->type == CG_POINTER_CONSTRAINT_LOCKED && constraint->remote_locked_pointer) {
		wlr_log(WLR_INFO, "Warping to (%d, %d)", wl_fixed_to_int(x), wl_fixed_to_int(y));
		zwp_locked_pointer_v1_set_cursor_position_hint(constraint->remote_locked_pointer, x, y);
	}
}

static struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
	.destroy = resource_destroy,
	.set_region = pointer_constraint_set_region,
	.set_cursor_position_hint = pointer_constraint_set_cursor_position_hint,
};

static struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
	.destroy = resource_destroy,
	.set_region = pointer_constraint_set_region,
};

static void remote_locked_pointer_handle_locked(void *data, struct zwp_locked_pointer_v1 *locked_pointer) {
	zwp_locked_pointer_v1_send_locked(data);
}

static void remote_locked_pointer_handle_unlocked(void *data, struct zwp_locked_pointer_v1 *locked_pointer) {
	zwp_locked_pointer_v1_send_unlocked(data);
}

static void remote_confined_pointer_handle_confined(void *data, struct zwp_confined_pointer_v1 *confined_pointer) {
	zwp_confined_pointer_v1_send_confined(data);
}

static void remote_confined_pointer_handle_unconfined(void *data, struct zwp_confined_pointer_v1 *confined_pointer) {
	zwp_confined_pointer_v1_send_unconfined(data);
}

static struct zwp_locked_pointer_v1_listener remote_locked_pointer_listener = {
	.locked = remote_locked_pointer_handle_locked,
	.unlocked = remote_locked_pointer_handle_unlocked,
};

static struct zwp_confined_pointer_v1_listener remote_confined_pointer_listener = {
	.confined = remote_confined_pointer_handle_confined,
	.unconfined = remote_confined_pointer_handle_unconfined,
};

static void pointer_constraint_destroy(struct cg_pointer_constraint *constraint) {
	if(constraint == NULL) {
		return;
	}

	wl_resource_set_user_data(constraint->resource, NULL);
	wl_list_remove(&constraint->surface_destroy.link);

	if(constraint->remote_locked_pointer != NULL) {
		zwp_locked_pointer_v1_destroy(constraint->remote_locked_pointer);
		constraint->remote_locked_pointer = NULL;
		wl_display_flush(constraint->constraints->server->remote_display);
	}

	if(constraint->remote_confined_pointer != NULL) {
		zwp_confined_pointer_v1_destroy(constraint->remote_confined_pointer);
		constraint->remote_confined_pointer = NULL;
		wl_display_flush(constraint->constraints->server->remote_display);
	}

	free(constraint);
}

static void pointer_constraint_resource_destructor(struct wl_resource *resource) {
	pointer_constraint_destroy(wl_resource_get_user_data(resource));
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct cg_pointer_constraint *constraint = wl_container_of(listener, constraint, surface_destroy);
	pointer_constraint_destroy(constraint);
}

static void pointer_constraint_create(struct wl_client *client,
		struct wl_resource *constraints_res, uint32_t id,
		struct wl_resource *surface_res, struct wl_resource *pointer_res,
		struct wl_resource *region_res, enum zwp_pointer_constraints_v1_lifetime lifetime,
		enum cg_pointer_constraint_type type) {
	struct wlr_surface *surface = wlr_surface_from_resource(surface_res);
	struct cg_pointer_constraints *constraints = wl_resource_get_user_data(constraints_res);

	if(constraints->server->remote_pointer_constraints == NULL) {
		wlr_log(WLR_ERROR, "Backend does not supports zwp_pointer_constraints_v1");
		return;
	}

	if(constraints->remote_pointer == NULL) {
		wlr_log(WLR_ERROR, "Could not find backend pointer");
		return;
	}

	int outputs = wl_list_length(&constraints->server->outputs);
	if(outputs != 1) {
		wlr_log(WLR_ERROR, "Backend has %d outputs, only single output is supported for now", outputs);
		return;
	}
	struct cg_output *remote_output = wl_container_of(constraints->server->outputs.next, remote_output, link);
	if(!wlr_output_is_wl(remote_output->wlr_output)) {
		wlr_log(WLR_ERROR, "Non-wayland output for wayland backend ?");
		return;
	}
	struct wl_surface *remote_surface = wlr_wl_output_get_surface(remote_output->wlr_output);

	struct cg_pointer_constraint *constraint = calloc(1, sizeof(struct cg_pointer_constraint));
	if(constraint == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	constraint->constraints = wl_resource_get_user_data(constraints_res);
	constraint->surface = surface;

	constraint->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &constraint->surface_destroy);

	uint32_t version = wl_resource_get_version(constraints_res);
	struct wl_resource *resource = wl_resource_create(client,
			(type == CG_POINTER_CONSTRAINT_LOCKED ? &zwp_locked_pointer_v1_interface : &zwp_confined_pointer_v1_interface),
			version, id);
	if(resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	constraint->resource = resource;
	constraint->type = type;

	wl_resource_set_implementation(resource,
			(type == CG_POINTER_CONSTRAINT_LOCKED ? (void*)&locked_pointer_impl : (void*)&confined_pointer_impl),
			constraint, pointer_constraint_resource_destructor);

	if(region_res != NULL) {
		wlr_log(WLR_INFO, "region not yet supported");
	}

	if(type == CG_POINTER_CONSTRAINT_LOCKED) {
		constraint->remote_locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
				constraints->server->remote_pointer_constraints,
				remote_surface,
				constraints->remote_pointer,
				NULL,
				lifetime);
		zwp_locked_pointer_v1_add_listener(constraint->remote_locked_pointer, &remote_locked_pointer_listener, resource);
	} else {
		constraint->remote_confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
				constraints->server->remote_pointer_constraints,
				remote_surface,
				constraints->remote_pointer,
				NULL,
				lifetime);
		zwp_confined_pointer_v1_add_listener(constraint->remote_confined_pointer, &remote_confined_pointer_listener, resource);
	}

	wl_display_flush(constraint->constraints->server->remote_display);
}

/* Instancied global */

static void pointer_constraints_lock_pointer(struct wl_client *client,
		struct wl_resource *constraints_res, uint32_t id,
		struct wl_resource *surface_res, struct wl_resource *pointer_res,
		struct wl_resource *region_res, enum zwp_pointer_constraints_v1_lifetime lifetime) {
	pointer_constraint_create(client, constraints_res, id, surface_res, pointer_res, region_res, lifetime, CG_POINTER_CONSTRAINT_LOCKED);
}

static void pointer_constraints_confine_pointer(struct wl_client *client,
		struct wl_resource *constraints_res, uint32_t id,
		struct wl_resource *surface_res, struct wl_resource *pointer_res,
		struct wl_resource *region_res,
		enum zwp_pointer_constraints_v1_lifetime lifetime) {
	pointer_constraint_create(client, constraints_res, id, surface_res, pointer_res, region_res, lifetime, CG_POINTER_CONSTRAINT_CONFINED);
}

static const struct zwp_pointer_constraints_v1_interface pointer_constraints_impl = {
	.destroy = resource_destroy,
	.lock_pointer = pointer_constraints_lock_pointer,
	.confine_pointer = pointer_constraints_confine_pointer,
};

static void pointer_constraints_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	assert(client && data);

	struct wl_resource *resource = wl_resource_create(client, &zwp_pointer_constraints_v1_interface, version, id);
	if(resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &pointer_constraints_impl, data, NULL);
	wlr_log(WLR_INFO, "created constraints");
}

/* Global */

void setup_pointer_constraints(struct cg_server *server) {
	if(server->remote_pointer_constraints == NULL) {
		wlr_log(WLR_ERROR, "pointer constraints not supported on remote compositor");
		return;
	}

	struct cg_pointer_constraints *constraints = calloc(1, sizeof(struct cg_pointer_constraints));
	if(constraints == NULL) {
		wlr_log(WLR_ERROR, "calloc() failed");
		return;
	}

	constraints->server = server;
	constraints->remote_pointer = wl_seat_get_pointer(server->remote_seat);

	wl_global_create(server->wl_display, &zwp_pointer_constraints_v1_interface, 1, constraints, pointer_constraints_bind);
}
