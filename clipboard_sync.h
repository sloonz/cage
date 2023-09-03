#ifndef CG_CLIPBOARD_SYNC_H
#define CG_CLIPBOARD_SYNC_H

struct cg_server;

struct cg_clipboad_sync {
	struct cg_server *server;
};

void clipboard_sync_init(struct cg_server *server);
void clipboard_sync_handle_set_selection(struct cg_server *server, struct wlr_data_source *data_source);

#endif
