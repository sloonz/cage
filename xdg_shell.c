/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2019 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "server.h"
#include "view.h"
#include "xdg_shell.h"

static void
xdg_popup_destroy(struct cg_view_child *child)
{
	if (!child) {
		return;
	}

	struct cg_xdg_popup *popup = (struct cg_xdg_popup *) child;
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->new_popup.link);
	view_child_finish(&popup->view_child);
	free(popup);
}

static void
handle_xdg_popup_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xdg_popup *popup = wl_container_of(listener, popup, destroy);
	struct cg_view_child *view_child = (struct cg_view_child *) popup
	xdg_popup_destroy(view_child);
}

static void xdg_popup_create(struct cg_view *view, struct wlr_xdg_popup *wlr_popup);

static void
popup_handle_new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct cg_xdg_popup *popup = wl_container_of(listener, popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	xdg_popup_create(popup->view_child.view, wlr_popup);
}

static void
popup_unconstrain(struct cg_xdg_popup *popup)
{
	struct cg_view *view = popup->view_child.view;
	struct wlr_output *output = view->server->output->wlr_output;

	int width, height;
	wlr_output_effective_resolution(output, &width, &height);

	struct wlr_box output_toplevel_box = {
		.x = output->lx - view->x,
		.y = output->ly - view->y,
		.width = width,
		.height = height
	};

	wlr_xdg_popup_unconstrain_from_box(popup->wlr_popup, &output_toplevel_box);
}

static void
xdg_popup_create(struct cg_view *view, struct wlr_xdg_popup *wlr_popup)
{
	struct cg_xdg_popup *popup = calloc(1, sizeof(struct cg_xdg_popup));
	if (!popup) {
		return;
	}

	popup->wlr_popup = wlr_popup;
	view_child_init(&popup->view_child, view, wlr_popup->base->surface);
	popup->view_child.destroy = xdg_popup_destroy;
	popup->destroy.notify = handle_xdg_popup_destroy;
	wl_signal_add(&wlr_popup->base->events.destroy, &popup->destroy);
	popup->new_popup.notify = popup_handle_new_xdg_popup;
	wl_signal_add(&wlr_popup->base->events.new_popup, &popup->new_popup);

	popup_unconstrain(popup);
}

static void
handle_new_xdg_popup(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	xdg_popup_create(&xdg_shell_view->view, wlr_popup);
}

static struct cg_xdg_shell_view *
xdg_shell_view_from_view(struct cg_view *view)
{
	return (struct cg_xdg_shell_view *) view;
}

static char *
get_title(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	return xdg_shell_view->xdg_surface->toplevel->title;
}

static void
get_geometry(struct cg_view *view, int *width_out, int *height_out)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_box geom;

	wlr_xdg_surface_get_geometry(xdg_shell_view->xdg_surface, &geom);
	*width_out = geom.width;
	*height_out = geom.height;
}

static bool
is_primary(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	struct wlr_xdg_surface *parent = xdg_shell_view->xdg_surface->toplevel->parent;
	/* FIXME: role is 0? */
	return parent == NULL; /*&& role == WLR_XDG_SURFACE_ROLE_TOPLEVEL */
}

static bool
is_transient_for(struct cg_view *child, struct cg_view *parent)
{
	if (parent->type != CAGE_XDG_SHELL_VIEW) {
		return false;
	}
	struct cg_xdg_shell_view *_child = xdg_shell_view_from_view(child);
	struct wlr_xdg_surface *xdg_surface = _child->xdg_surface;
	struct cg_xdg_shell_view *_parent = xdg_shell_view_from_view(parent);
	struct wlr_xdg_surface *parent_xdg_surface = _parent->xdg_surface;
	while (xdg_surface) {
		if (xdg_surface->toplevel->parent == parent_xdg_surface) {
			return true;
		}
		xdg_surface = xdg_surface->toplevel->parent;
	}
	return false;
}

static void
activate(struct cg_view *view, bool activate)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_activated(xdg_shell_view->xdg_surface, activate);
}

static void
maximize(struct cg_view *view, int output_width, int output_height)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_toplevel_set_size(xdg_shell_view->xdg_surface, output_width, output_height);
	wlr_xdg_toplevel_set_maximized(xdg_shell_view->xdg_surface, true);
}

static void
destroy(struct cg_view *view)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	free(xdg_shell_view);
}

static void
for_each_surface(struct cg_view *view, wlr_surface_iterator_func_t iterator, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	wlr_xdg_surface_for_each_surface(xdg_shell_view->xdg_surface, iterator, data);
}

static struct wlr_surface *
wlr_surface_at(struct cg_view *view, double sx, double sy, double *sub_x, double *sub_y)
{
	struct cg_xdg_shell_view *xdg_shell_view = xdg_shell_view_from_view(view);
	return wlr_xdg_surface_surface_at(xdg_shell_view->xdg_surface, sx, sy, sub_x, sub_y);
}

static void
handle_xdg_shell_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, commit);
	struct cg_view *view = &xdg_shell_view->view;
	view_damage_surface(view);
}

static void
handle_xdg_shell_surface_unmap(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, unmap);
	struct cg_view *view = &xdg_shell_view->view;

	wl_list_remove(&xdg_shell_view->commit.link);

	view_unmap(view);
}

static void
handle_xdg_shell_surface_map(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, map);
	struct cg_view *view = &xdg_shell_view->view;

	xdg_shell_view->commit.notify = handle_xdg_shell_surface_commit;
	wl_signal_add(&xdg_shell_view->xdg_surface->surface->events.commit, &xdg_shell_view->commit);

	view_map(view, xdg_shell_view->xdg_surface->surface);
}

static void
handle_xdg_shell_surface_destroy(struct wl_listener *listener, void *data)
{
	struct cg_xdg_shell_view *xdg_shell_view = wl_container_of(listener, xdg_shell_view, destroy);
	struct cg_view *view = &xdg_shell_view->view;

	wl_list_remove(&xdg_shell_view->map.link);
	wl_list_remove(&xdg_shell_view->unmap.link);
	wl_list_remove(&xdg_shell_view->destroy.link);
	wl_list_remove(&xdg_shell_view->new_popup.link);
	xdg_shell_view->xdg_surface = NULL;

	view_destroy(view);
}

static const struct cg_view_impl xdg_shell_view_impl = {
	.get_title = get_title,
	.get_geometry = get_geometry,
	.is_primary = is_primary,
	.is_transient_for = is_transient_for,
	.activate = activate,
	.maximize = maximize,
	.destroy = destroy,
	.for_each_surface = for_each_surface,
	.wlr_surface_at = wlr_surface_at,
};

void
handle_xdg_shell_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xdg_shell_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	struct cg_xdg_shell_view *xdg_shell_view = calloc(1, sizeof(struct cg_xdg_shell_view));
	if (!xdg_shell_view) {
		wlr_log(WLR_ERROR, "Failed to allocate XDG Shell view");
		return;
	}

	view_init(&xdg_shell_view->view, server, CAGE_XDG_SHELL_VIEW, &xdg_shell_view_impl);
	xdg_shell_view->xdg_surface = xdg_surface;

	xdg_shell_view->map.notify = handle_xdg_shell_surface_map;
	wl_signal_add(&xdg_surface->events.map, &xdg_shell_view->map);
	xdg_shell_view->unmap.notify = handle_xdg_shell_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &xdg_shell_view->unmap);
	xdg_shell_view->destroy.notify = handle_xdg_shell_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &xdg_shell_view->destroy);
	xdg_shell_view->new_popup.notify = handle_new_xdg_popup;
	wl_signal_add(&xdg_surface->events.new_popup, &xdg_shell_view->new_popup);
}
