#include <ctype.h>
#include <unistd.h>
#include "tmate.h"
#include "tmate-protocol.h"
#include "window-copy.h"

char *tmate_left_status, *tmate_right_status;

static void tmate_header(struct tmate_session *session,
			 struct tmate_unpacker *uk)
{
	char port_arg[16] = {0};
	char *client_version = xstrdup("< 1.8.6");
	char tmp[512];

	session->client_protocol_version = unpack_int(uk);
	if (session->client_protocol_version <= 4) {
		session->daemon_encoder.mpac_version = 4;
	}

	if (session->client_protocol_version >= 3) {
		free(client_version);
		client_version = unpack_string(uk);
	}

	tmate_notice("Daemon header: client version: %s, protocol version: %d",
		     client_version, session->client_protocol_version);

#if 0
	if (strcmp(client_version, TMATE_LATEST_VERSION))
		tmate_notify_later(10, "A new version is available, please upgrade :)");
#endif

	free(client_version);

	if (tmate_settings->ssh_port != 22)
		sprintf(port_arg, " -p%d", tmate_settings->ssh_port);

	sprintf(tmp, "ssh%s ro-%s@%s", port_arg, session->session_token_ro, tmate_settings->tmate_host);
	tmate_notify("Remote session read only: %s (clear your screen if you share this)", tmp);
	tmate_send_env("tmate_ssh_ro", tmp);

	sprintf(tmp, "ssh%s %s@%s", port_arg, session->session_token, tmate_settings->tmate_host);
	tmate_notify("Remote session: %s", tmp);
	tmate_send_env("tmate_ssh", tmp);

	tmate_send_client_ready();
}

extern u_int next_window_pane_id;

static void tmate_sync_window_panes(struct window *w,
				    struct tmate_unpacker *w_uk)
{
	struct tmate_unpacker uk, tmp_uk;
	struct window_pane *wp, *wp_tmp;
	int active_pane_id;

	TAILQ_FOREACH(wp, &w->panes, entry)
		wp->flags |= PANE_KILL;

	unpack_each(&uk, &tmp_uk, w_uk) {
		int id = unpack_int(&uk);
		u_int sx = unpack_int(&uk);
		u_int sy = unpack_int(&uk);
		u_int xoff = unpack_int(&uk);
		u_int yoff = unpack_int(&uk);

		wp = window_pane_find_by_id(id);
		if (!wp) {
			next_window_pane_id = id;
			wp = window_add_pane(w, TMATE_HLIMIT);
			window_set_active_pane(w, wp);
		}
		wp->flags &= ~PANE_KILL;

		if (wp->xoff != xoff || wp->yoff != yoff ||
		    wp->sx != sx || wp->sx != sy) {
			wp->xoff = xoff;
			wp->yoff = yoff;
			window_pane_resize(wp, sx, sy);

			wp->flags |= PANE_REDRAW;
		}
	}

	TAILQ_FOREACH_SAFE(wp, &w->panes, entry, wp_tmp) {
		if (wp->flags & PANE_KILL)
			window_remove_pane(w, wp);
	}

	active_pane_id = unpack_int(w_uk);
	wp = window_pane_find_by_id(active_pane_id);
	window_set_active_pane(w, wp);
}

static void tmate_sync_windows(struct session *s,
			       struct tmate_unpacker *s_uk)
{
	struct tmate_unpacker uk, tmp_uk;
	struct winlink *wl, *wl_tmp;
	struct window *w;
	int active_window_idx;
	char *cause;

	RB_FOREACH(wl, winlinks, &s->windows)
		wl->flags |= WINLINK_KILL;

	unpack_each(&uk, &tmp_uk, s_uk) {
		int idx    = unpack_int(&uk);
		char *name = unpack_string(&uk);

		wl = winlink_find_by_index(&s->windows, idx);
		if (!wl) {
			wl = session_new(s, name, 0, NULL, NULL, NULL, idx, &cause);
			if (!wl)
				tmate_fatal("can't create window idx=%d", idx);
		}

		wl->flags &= ~WINLINK_KILL;
		w = wl->window;

		free(w->name);
		w->name = name;
		w->sx = s->sx;
		w->sy = s->sy;

		tmate_sync_window_panes(w, &uk);
	}

	RB_FOREACH_SAFE(wl, winlinks, &s->windows, wl_tmp) {
		if (wl->flags & WINLINK_KILL)
			session_detach(s, wl);
	}

	active_window_idx = unpack_int(s_uk);
	wl = winlink_find_by_index(&s->windows, active_window_idx);
	if (!wl)
		tmate_fatal("no valid active window");

	session_set_current(s, wl);
	server_redraw_window(wl->window);
}

static void tmate_sync_layout(__unused struct tmate_session *session,
			      struct tmate_unpacker *uk)
{
	struct session *s;
	char *cause;

	int sx = unpack_int(uk);
	int sy = unpack_int(uk);

	s = RB_MIN(sessions, &sessions);
	if (!s) {
		s = session_create("default", 0, NULL, "/", "/",
				   NULL, NULL, 0, sx, sy, &cause);
		if (!s)
			tmate_fatal("can't create main session");
	}

	s->sx = sx;
	s->sy = sy;

	tmate_sync_windows(s, uk);
}

static void tmate_pty_data(__unused struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	const char *buf;
	size_t len;
	int id;

	id = unpack_int(uk);
	unpack_buffer(uk, &buf, &len);

	wp = window_pane_find_by_id(id);
	if (!wp)
		tmate_fatal("can't find pane id=%d", id);

	evbuffer_add(wp->event_input, buf, len);
	input_parse(wp);

	wp->window->flags |= WINDOW_SILENCE;
}

static void tmate_exec_cmd(__unused struct tmate_session *session,
			   struct tmate_unpacker *uk)
{
	struct cmd_q *cmd_q;
	struct cmd_list *cmdlist;
	char *cmd_str;
	char *cause;

	cmd_str = unpack_string(uk);

	tmate_info("Local cmd: %s", cmd_str);

	if (cmd_string_parse(cmd_str, &cmdlist, NULL, 0, &cause) != 0) {
		free(cause);
		goto out;
	}

	cmd_q = cmdq_new(NULL);
	cmdq_run(cmd_q, cmdlist, NULL);
	cmd_list_free(cmdlist);
	cmdq_free(cmd_q);
out:
	free(cmd_str);
}

static void tmate_failed_cmd(__unused struct tmate_session *session,
			     struct tmate_unpacker *uk)
{
	struct client *c;
	int client_id;
	char *cause;

	client_id = unpack_int(uk);
	cause = unpack_string(uk);

	TAILQ_FOREACH(c, &clients, entry) {
		if (c && c->id == client_id) {
			*cause = toupper((u_char) *cause);
			status_message_set(c, "%s", cause);
			break;
		}
	}

	free(cause);
}

static void tmate_status(__unused struct tmate_session *session,
			 struct tmate_unpacker *uk)
{
	struct client *c;

	free(tmate_left_status);
	free(tmate_right_status);
	tmate_left_status = unpack_string(uk);
	tmate_right_status = unpack_string(uk);

	TAILQ_FOREACH(c, &clients, entry)
		c->flags |= CLIENT_STATUS;
}

static void tmate_sync_copy_mode(struct tmate_session *session,
				 struct tmate_unpacker *uk)
{
	struct tmate_unpacker cm_uk, sel_uk, input_uk;
	struct window_copy_mode_data *data;
	struct window_pane *wp;
	int pane_id;
	int base_backing = 1;

	pane_id = unpack_int(uk);
	wp = window_pane_find_by_id(pane_id);
	if (!wp)
		tmate_fatal("can't find window pane=%d", pane_id);

	unpack_array(uk, &cm_uk);

	if (cm_uk.argc == 0) {
		if (wp->mode) {
			data = wp->modedata;
			free((char *)data->inputprompt);
			window_pane_reset_mode(wp);
		}
		return;
	}

	if (session->client_protocol_version >= 2)
		base_backing = unpack_int(&cm_uk);

	if (window_pane_set_mode(wp, &window_copy_mode) == 0) {
		if (base_backing)
			window_copy_init_from_pane(wp, 0);
		else
			window_copy_init_for_output(wp);
	}
	data = wp->modedata;

	data->oy = unpack_int(&cm_uk);
	data->cx = unpack_int(&cm_uk);
	data->cy = unpack_int(&cm_uk);

	unpack_array(&cm_uk, &sel_uk);

	if (sel_uk.argc) {
		data->screen.sel.flag = 1;
		data->selx = unpack_int(&sel_uk);
		if (session->client_protocol_version >= 2) {
			data->sely = -unpack_int(&sel_uk) + screen_hsize(data->backing)
							  + screen_size_y(data->backing)
							  - 1;
		} else
			data->sely = unpack_int(&sel_uk);
		data->rectflag = unpack_int(&sel_uk);
	} else
		data->screen.sel.flag = 0;

	unpack_array(&cm_uk, &input_uk);

	if (input_uk.argc) {
		/*
		 * XXX In the original tmux code, inputprompt is not a
		 * malloced string, the two piece of code must not run at the
		 * same time, otherwise, we'll either get a memory leak, or a
		 * crash.
		 */
		data->inputtype = unpack_int(&input_uk);

		free((char *)data->inputprompt);
		data->inputprompt = unpack_string(&input_uk);

		free(data->inputstr);
		data->inputstr = unpack_string(&input_uk);
	} else {
		data->inputtype = WINDOW_COPY_OFF;
		free((char *)data->inputprompt);
		data->inputprompt = NULL;
	}

	window_copy_update_selection(wp, 1);
	window_copy_redraw_screen(wp);
}

static void tmate_write_copy_mode(__unused struct tmate_session *session,
				  struct tmate_unpacker *uk)
{
	struct window_pane *wp;
	int id;
	char *str;

	id = unpack_int(uk);
	wp = window_pane_find_by_id(id);
	if (!wp)
		tmate_fatal("can't find pane id=%d", id);

	str = unpack_string(uk);

	if (window_pane_set_mode(wp, &window_copy_mode) == 0)
		window_copy_init_for_output(wp);

	window_copy_add(wp, "%s", str);
	free(str);
}

static void tmate_fin(__unused struct tmate_session *session,
		      __unused struct tmate_unpacker *uk)
{
	request_server_termination();
}

void tmate_dispatch_daemon_message(struct tmate_session *session,
				   struct tmate_unpacker *uk)
{
	int cmd = unpack_int(uk);
	switch (cmd) {
#define dispatch(c, f) case c: f(session, uk); break
	dispatch(TMATE_OUT_HEADER,		tmate_header);
	dispatch(TMATE_OUT_SYNC_LAYOUT,		tmate_sync_layout);
	dispatch(TMATE_OUT_PTY_DATA,		tmate_pty_data);
	dispatch(TMATE_OUT_EXEC_CMD,		tmate_exec_cmd);
	dispatch(TMATE_OUT_FAILED_CMD,		tmate_failed_cmd);
	dispatch(TMATE_OUT_STATUS,		tmate_status);
	dispatch(TMATE_OUT_SYNC_COPY_MODE,	tmate_sync_copy_mode);
	dispatch(TMATE_OUT_WRITE_COPY_MODE,	tmate_write_copy_mode);
	dispatch(TMATE_OUT_FIN,			tmate_fin);
	default: tmate_fatal("Bad message type: %d", cmd);
	}
}
