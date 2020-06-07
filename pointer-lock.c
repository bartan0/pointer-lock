#include <stdlib.h>
#include <xcb/render.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <error.h>

#define ERROR(fmt, ...) ( error(0, 0, fmt __VA_OPT__(,) __VA_ARGS__), 1 )
#define INFO(fmt, ...) error(0, 0, fmt __VA_OPT__(,) __VA_ARGS__)

#define CURSOR_SIZE 16

#ifdef DEBUG
	#define CURSOR_PIXMAP_FILL_OP XCB_GX_SET
#else
	#define CURSOR_PIXMAP_FILL_OP XCB_GX_CLEAR
#endif


typedef enum {
	MODE_SWITCH,
	MODE_TOGGLE
} lock_mode_t;

typedef struct {
	lock_mode_t mode;
	int pointer_lock;
	xcb_keycode_t
		keycode_lock,
		keycode_unlock;
	uint16_t
		modmask_lock,
		modmask_unlock;
} state_t;

static xcb_connection_t *conn;
static xcb_window_t root;
static xcb_cursor_t cursor;
static xcb_generic_error_t *xcberr;
static state_t state;


static void get_root (void) {
	xcb_screen_iterator_t iter;

	iter = xcb_setup_roots_iterator(xcb_get_setup(conn));
	root = iter.data -> root;
}

static uint8_t xcberr_free (void) {
	uint8_t res;

	res = xcberr -> error_code;
	free(xcberr);
	xcberr = NULL;

	return res;
}

static int create_pixmap_gc (
	xcb_pixmap_t pid,
	xcb_gcontext_t *gcid
) {
	const uint32_t values[] = { CURSOR_PIXMAP_FILL_OP };

	*gcid = xcb_generate_id(conn);

	if ((xcberr = xcb_request_check(conn,
		xcb_create_gc_checked(conn,
			*gcid,
			pid,
			XCB_GC_FUNCTION,
			values
		)
	)))
		return ERROR("%d: xcb_create_gc", xcberr_free());

	return 0;
}

static int create_cursor_pixmap (
	xcb_pixmap_t *pid
) {
	*pid = xcb_generate_id(conn);

	if ((xcberr = xcb_request_check(conn, xcb_create_pixmap_checked(conn,
		1,
		*pid,
		root,
		CURSOR_SIZE,
		CURSOR_SIZE
	))))
		return ERROR("%d: xcb_create_pixmap", xcberr_free());

	return 0;
}

static int fill_cursor_pixmap (
	xcb_pixmap_t pid,
	xcb_gcontext_t gcid
) {
	xcb_rectangle_t rect = {
		0, 0,
		CURSOR_SIZE, CURSOR_SIZE
	};

	if ((xcberr = xcb_request_check(conn,
		xcb_poly_fill_rectangle_checked(conn,
			pid,
			gcid,
			1,
			&rect
		)
	)))
		return ERROR("%d, xcb_poly_fill_rectangle", xcberr_free());

	return 0;
}

static int create_cursor (
	xcb_pixmap_t pid,
	xcb_cursor_t *cid
) {
	*cid = xcb_generate_id(conn);

	if ((xcberr = xcb_request_check(conn,
		xcb_create_cursor_checked(conn,
			*cid,
			pid,
			pid,
			0xffff, 0x0000, 0xffff,
			0x0000, 0x0000, 0x0000,
			0, 0
		)
	)))
		return ERROR("%d: xcb_create_cursor", xcberr_free());

	return 0;
}

static int init_cursor () {
	xcb_pixmap_t pid;
	xcb_gcontext_t gcid;

	if (
		create_cursor_pixmap(&pid) ||
		create_pixmap_gc(pid, &gcid) ||
		fill_cursor_pixmap(pid, gcid) ||
		create_cursor(pid, &cursor)
	)
		return ERROR("init_cursor");

	return 0;
}

static int grab_key (
	uint16_t modmask,
	xcb_keycode_t keycode
) {
	if ((xcberr = xcb_request_check(conn, xcb_grab_key_checked(conn,
		0,
		root,
		modmask,
		keycode,
		XCB_GRAB_MODE_ASYNC,
		XCB_GRAB_MODE_ASYNC
	))))
		return ERROR("%d: xcb_grab_key", xcberr_free());

	return 0;
}

static int grab_pointer (void) {
	xcb_grab_pointer_reply_t *reply;
	xcb_grab_status_t status;

	reply = xcb_grab_pointer_reply(conn,
		xcb_grab_pointer(conn,
			0,
			root,
			0,
			XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC,
			XCB_NONE,
			cursor,
			XCB_CURRENT_TIME
		),
		&xcberr
	);

	if (xcberr) {
		free(reply);

		return ERROR("[error] %d: xcb_grab_pointer", xcberr_free());
	}

	if ((status = reply -> status) != XCB_GRAB_STATUS_SUCCESS) {
		free(reply);

		return ERROR("[status] %d: xcb_grab_pointer", status);
	}

	return 0;
}

static int ungrab_pointer (void) {
	if ((xcberr = xcb_request_check(conn,
		xcb_ungrab_pointer_checked(conn,
			XCB_CURRENT_TIME
		)
	)))
		return ERROR("%d: xcb_ungrab_pointer", xcberr_free());

	return 0;
}

static void pointer_lock (void) {
	grab_pointer();
	state.pointer_lock = 1;
}

static void pointer_unlock () {
	ungrab_pointer();
	state.pointer_lock = 0;
}

static void pointer_toggle (void) {
	if (state.pointer_lock)
		pointer_unlock();
	else
		pointer_lock();
}

static void key_event (xcb_key_press_event_t *event) {
	uint16_t modmask;
	xcb_keycode_t keycode;

	modmask = event -> state;
	keycode = event -> detail;

	switch (state.mode) {

		case MODE_SWITCH:
			if (
				modmask == state.modmask_lock &&
				keycode == state.keycode_lock
			) {
				pointer_lock();
				return;
			}

			if (
				modmask == state.modmask_unlock &&
				keycode == state.keycode_unlock
			) {
				pointer_unlock();
				return;
			}

			break;

		case MODE_TOGGLE:
			if (
				modmask == state.modmask_lock &&
				keycode == state.keycode_lock
			) {
				pointer_toggle();
				return;
			}

			break;
	}

	INFO("%d + %d: Unexpected keys combination: key_event", modmask, keycode);
}

static int loop (void) {
	xcb_generic_event_t *event;

	if (!(event = xcb_wait_for_event(conn)))
		return ERROR("xcb_wait_for_event");

	switch (event -> response_type) {

		case XCB_KEY_PRESS:
			key_event((xcb_key_press_event_t*)event);
			break;

		case XCB_KEY_RELEASE:
			break;

		default:
			INFO("%d: Unexpected event: xcb_wait_for_event", event -> response_type);
			break;
	}

	free(event);

	return 0;
}


extern int main (int argc, char **argv) {
	conn = xcb_connect(NULL, NULL);
	get_root();

	state.modmask_lock = XCB_KEY_BUT_MASK_MOD_4;
	state.keycode_lock = 58; // 'm'
	state.modmask_unlock = XCB_KEY_BUT_MASK_MOD_4;
	state.keycode_unlock = 58; // 'm'
	state.mode = (
		state.modmask_unlock == state.modmask_lock &&
		state.keycode_unlock == state.keycode_lock
	)
		? MODE_TOGGLE
		: MODE_SWITCH;

	if (init_cursor())
		return ERROR("main");

	pointer_lock();
	grab_key(state.modmask_lock, state.keycode_lock);

	if (state.mode == MODE_SWITCH)
		grab_key(state.modmask_unlock, state.keycode_unlock);

	while (!loop());

	return 0;
}
