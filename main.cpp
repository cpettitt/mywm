#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ev.h>
#include <xcb/xcb.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

static FILE *log;

const uint32_t WM_STATE_FLAG_RESTART     = 0x00000001;
const uint32_t WM_STATE_FLAG_MOVE_WINDOW = 0x00000002;
struct WMState {
    xcb_connection_t *conn;
    xcb_screen_t *    screen;
    uint32_t          flags;

    // Used on move event
    int16_t start_x;
    int16_t start_y;

    int16_t win_origin_x;
    int16_t win_origin_y;
};

void write_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log, fmt, ap);
    fflush(log);
    va_end(ap);
}

WMState *get_wm_state(struct ev_loop *l) {
    return (WMState *)ev_userdata(l);
}

void handle_sighup(struct ev_loop *l, ev_signal *w, int revents) {
    write_log("Got SIGHUP. Setting restart flag.\n");
    get_wm_state(l)->flags |= WM_STATE_FLAG_RESTART;
    ev_break(l, EVBREAK_ALL);
}

void handle_xcb_configure_request(struct ev_loop *l, xcb_configure_request_event_t *ev) {
    WMState *wm_state = get_wm_state(l);
    xcb_connection_t *conn = wm_state->conn;
    xcb_screen_t *screen = wm_state->screen;
    uint16_t mask = ev->value_mask;
    uint32_t values[8];
    uint8_t i = 0;

    if (mask & XCB_CONFIG_WINDOW_X) {
        mask |= XCB_CONFIG_WINDOW_X;
        values[i++] = ev->x;
    }

    if (mask & XCB_CONFIG_WINDOW_Y) {
        mask |= XCB_CONFIG_WINDOW_Y;
        values[i++] = ev->y;
    }

    if (mask & XCB_CONFIG_WINDOW_WIDTH) {
        mask |= XCB_CONFIG_WINDOW_WIDTH;
        values[i++] = ev->width;
    }

    if (mask & XCB_CONFIG_WINDOW_HEIGHT) {
        mask |= XCB_CONFIG_WINDOW_HEIGHT;
        values[i++] = ev->height;
    }

    if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
        mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
        values[i++] = ev->border_width;
    }

    if (mask & XCB_CONFIG_WINDOW_SIBLING) {
        mask |= XCB_CONFIG_WINDOW_SIBLING;
        values[i++] = ev->sibling;
    }

    if (mask & XCB_CONFIG_WINDOW_STACK_MODE) {
        mask |= XCB_CONFIG_WINDOW_STACK_MODE;
        values[i++] = ev->stack_mode;
    }

    if (mask & XCB_CONFIG_WINDOW_X) {
        mask |= XCB_CONFIG_WINDOW_X;
        values[i++] = ev->x;
    }

    xcb_configure_window(conn, ev->window, ev->value_mask, values);
}

void handle_xcb_map_request(struct ev_loop *l, xcb_map_request_event_t * ev) {
    uint32_t values[] = {
        XCB_EVENT_MASK_ENTER_WINDOW
    };

    xcb_connection_t *conn = get_wm_state(l)->conn;

    // TODO: check result
    xcb_change_window_attributes_checked(conn, ev->window, XCB_CW_EVENT_MASK, values);

    xcb_change_save_set(conn, XCB_SET_MODE_INSERT, ev->window);

    xcb_map_window(conn, ev->window);

    // TODO: do we need to do this?
    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, ev->window, XCB_CURRENT_TIME);
}

void handle_xcb_button_press(struct ev_loop *l, xcb_button_press_event_t *ev) {
    WMState *wm_state = get_wm_state(l);
    switch (ev->detail) {
        case XCB_BUTTON_INDEX_1:
            if (ev->state == XCB_MOD_MASK_4) {
                if (!ev->child) {
                    // If we got an event on the root window, ignore.
                    return;
                }

                xcb_query_pointer_cookie_t pointer_cookie = xcb_query_pointer(wm_state->conn, ev->root);
                xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(wm_state->conn, pointer_cookie, nullptr);

                if (!pointer) {
                    return;
                }

                wm_state->start_x = pointer->root_x;
                wm_state->start_y = pointer->root_y;
                free(pointer);

                xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(wm_state->conn, ev->child);
                xcb_get_geometry_reply_t *geometry        = xcb_get_geometry_reply(wm_state->conn, geometry_cookie, nullptr);

                if (!geometry) {
                    return;
                }

                wm_state->win_origin_x = geometry->x;
                wm_state->win_origin_y = geometry->y;

                free(geometry);

                xcb_grab_pointer_cookie_t grab_cookie = xcb_grab_pointer(
                    wm_state->conn,
                    0,
                    wm_state->screen->root,
                    XCB_EVENT_MASK_BUTTON_MOTION | XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_GRAB_MODE_ASYNC,
                    XCB_NONE,
                    XCB_NONE, // TODO: change cursor
                    XCB_CURRENT_TIME);
                xcb_grab_pointer_reply_t *grab = xcb_grab_pointer_reply(wm_state->conn, grab_cookie, nullptr);

                if (grab->status != XCB_GRAB_STATUS_SUCCESS) {
                    free(grab);
                    return;
                }
                free(grab);

                wm_state->flags |= WM_STATE_FLAG_MOVE_WINDOW;
                write_log("Setting window move state.\n");
            }
            break;
    }
}

void handle_xcb_button_release(struct ev_loop *l, xcb_button_release_event_t *ev) {
    WMState *wm_state = get_wm_state(l);
    switch (ev->detail) {
        case XCB_BUTTON_INDEX_1:
            if (wm_state->flags & WM_STATE_FLAG_MOVE_WINDOW) {
                xcb_ungrab_pointer(wm_state->conn, XCB_CURRENT_TIME);
                wm_state->flags &= ~WM_STATE_FLAG_MOVE_WINDOW;
                write_log("Clearing window move state.\n");
            }
            break;
    }
}

void handle_xcb_motion_notify(struct ev_loop *l, xcb_motion_notify_event_t *ev) {
    WMState *         wm_state = get_wm_state(l);
    xcb_connection_t *conn     = wm_state->conn;

    int16_t delta_x = ev->root_x - wm_state->start_x;
    int16_t delta_y = ev->root_y - wm_state->start_y;

    int16_t x = wm_state->win_origin_x + delta_x;
    int16_t y = wm_state->win_origin_y + delta_y;

    // TODO: constrain to boundaries
    uint32_t values[2] = {(uint32_t)MAX(1, x), (uint32_t)MAX(1, y)};
    xcb_configure_window(conn, ev->child, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
}

void handle_xcb_enter_notify(struct ev_loop *l, xcb_enter_notify_event_t *ev) {
    xcb_set_input_focus(get_wm_state(l)->conn, XCB_INPUT_FOCUS_POINTER_ROOT, ev->event, XCB_CURRENT_TIME);

    uint32_t values[] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(get_wm_state(l)->conn, ev->event, XCB_CONFIG_WINDOW_STACK_MODE, values);
}

#define IGNORE_XCB_EVENT(EVENT)                    \
    case EVENT:                                    \
        write_log("Ignoring " #EVENT " event.\n"); \
        break;

#define IGNORE_XCB_EVENT_SILENTLY(EVENT) \
    case EVENT:                          \
        break;

#define DISPATCH_XCB_EVENT(EVENT, TYPE, HANDLER)      \
    case EVENT:                                       \
        write_log("Dispatching " #EVENT " event.\n"); \
        HANDLER(l, (TYPE *)ev);                       \
        break;

#define DISPATCH_XCB_EVENT_SILENTLY(EVENT, TYPE, HANDLER) \
    case EVENT:                                           \
        HANDLER(l, (TYPE *)ev);                           \
        break;

void handle_xcb_event(struct ev_loop *l, int type, xcb_generic_event_t *ev) {
    // NOTE: These events come from xproto.h (https://xcb.freedesktop.org/manual/xproto_8h_source.html)
    switch (type) {
        IGNORE_XCB_EVENT(XCB_KEY_PRESS)
        IGNORE_XCB_EVENT(XCB_KEY_RELEASE)

        DISPATCH_XCB_EVENT(XCB_BUTTON_PRESS, xcb_button_press_event_t, handle_xcb_button_press)
        DISPATCH_XCB_EVENT(XCB_BUTTON_RELEASE, xcb_button_release_event_t, handle_xcb_button_release)
        DISPATCH_XCB_EVENT_SILENTLY(XCB_MOTION_NOTIFY, xcb_motion_notify_event_t, handle_xcb_motion_notify)
        DISPATCH_XCB_EVENT(XCB_ENTER_NOTIFY, xcb_enter_notify_event_t, handle_xcb_enter_notify)

        IGNORE_XCB_EVENT(XCB_LEAVE_NOTIFY)
        IGNORE_XCB_EVENT(XCB_FOCUS_IN)
        IGNORE_XCB_EVENT(XCB_FOCUS_OUT)
        IGNORE_XCB_EVENT(XCB_KEYMAP_NOTIFY)
        IGNORE_XCB_EVENT(XCB_EXPOSE)
        IGNORE_XCB_EVENT(XCB_GRAPHICS_EXPOSURE)
        IGNORE_XCB_EVENT(XCB_NO_EXPOSURE)
        IGNORE_XCB_EVENT(XCB_VISIBILITY_NOTIFY)
        IGNORE_XCB_EVENT(XCB_CREATE_NOTIFY)
        IGNORE_XCB_EVENT(XCB_DESTROY_NOTIFY)
        IGNORE_XCB_EVENT(XCB_UNMAP_NOTIFY)
        IGNORE_XCB_EVENT(XCB_MAP_NOTIFY)

        DISPATCH_XCB_EVENT(XCB_MAP_REQUEST, xcb_map_request_event_t, handle_xcb_map_request)

        IGNORE_XCB_EVENT(XCB_REPARENT_NOTIFY)
        IGNORE_XCB_EVENT_SILENTLY(XCB_CONFIGURE_NOTIFY)

        DISPATCH_XCB_EVENT(XCB_CONFIGURE_REQUEST, xcb_configure_request_event_t, handle_xcb_configure_request)

        IGNORE_XCB_EVENT(XCB_GRAVITY_NOTIFY)
        IGNORE_XCB_EVENT(XCB_RESIZE_REQUEST)
        IGNORE_XCB_EVENT(XCB_CIRCULATE_NOTIFY)
        IGNORE_XCB_EVENT(XCB_CIRCULATE_REQUEST)
        IGNORE_XCB_EVENT(XCB_PROPERTY_NOTIFY)
        IGNORE_XCB_EVENT(XCB_SELECTION_CLEAR)
        IGNORE_XCB_EVENT(XCB_SELECTION_REQUEST)
        IGNORE_XCB_EVENT(XCB_SELECTION_NOTIFY)
        IGNORE_XCB_EVENT(XCB_COLORMAP_NOTIFY)
        IGNORE_XCB_EVENT(XCB_CLIENT_MESSAGE)
        IGNORE_XCB_EVENT(XCB_MAPPING_NOTIFY)

        default:
            write_log("Unhandled xcb event: %d\n", type);
            break;
    }
}

void handle_xcb_socket_read(struct ev_loop *l, ev_io *w, int revents) {
//    write_log("Draining xcb events.\n");

    xcb_connection_t *   conn = get_wm_state(l)->conn;
    xcb_generic_event_t *event;

    while ((event = xcb_poll_for_event(conn))) {
        int type = event->response_type & 0x7F;
        handle_xcb_event(l, type, event);
        free(event);
    }

    xcb_flush(conn);

    if (xcb_connection_has_error(conn)) {
        write_log("Detected xcb connection error");
        ev_break(l, EVBREAK_ALL);
    }
}

int main(int argc, char *argv[]) {
    log = fopen("/tmp/mywm.log", "a");
    if (!log) {
        fprintf(stderr, "Could not open log!\n");
        return 1;
    }

    write_log("Starting up!\n");

    struct ev_loop *loop = EV_DEFAULT;

    ev_signal sighup_watcher;
    { // Install SIGHUP handler
        ev_signal_init(&sighup_watcher, handle_sighup, SIGHUP);
        ev_signal_start(loop, &sighup_watcher);
    }

    xcb_connection_t *conn;
    xcb_screen_t *screen;
    { // Install XCB event handler
        int screen_num = 0;
        conn           = xcb_connect(nullptr, &screen_num);

        // TODO: any way to get the error at this point?
        if (!conn || xcb_connection_has_error(conn)) {
            write_log("Could not connect to $DISPLAY. Aborting.\n");
            return 1;
        }

        const xcb_setup_t *   setup       = xcb_get_setup(conn);
        xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(setup);

        for (int i = 0; i < screen_num; ++i) {
            xcb_screen_next(&screen_iter);
        }

        screen = screen_iter.data;

        {
            uint32_t event_mask[] = {
                XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                XCB_EVENT_MASK_PROPERTY_CHANGE
            };
            xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(conn,
                                                                            screen->root,
                                                                            XCB_CW_EVENT_MASK,
                                                                            event_mask);
            xcb_generic_error_t *error  = xcb_request_check(conn, cookie);
            if (error) {
                write_log("Another window manager is running (X error: %d). Aborting.\n", error->error_code);
                return 1;
            }
        }

        ev_io xcb_read_watcher;
        ev_io_init(&xcb_read_watcher, handle_xcb_socket_read, xcb_get_file_descriptor(conn), EV_READ);
        ev_io_start(loop, &xcb_read_watcher);

        xcb_grab_button(conn,
                        1,
                        screen->root,
                        XCB_EVENT_MASK_BUTTON_PRESS,
                        XCB_GRAB_MODE_ASYNC,
                        XCB_GRAB_MODE_ASYNC,
                        screen->root,
                        XCB_NONE,
                        XCB_BUTTON_INDEX_1,
                        XCB_MOD_MASK_4);
    }

    write_log("Entering event loop.\n");

    WMState state = {conn, screen, 0};
    ev_set_userdata(loop, &state);
    ev_run(loop, 0);

    write_log("Leaving event loop.\n");

    ev_loop_destroy(loop);
    xcb_disconnect(conn);

    if (state.flags & WM_STATE_FLAG_RESTART) {
        write_log("Attempting restart\n");

        execvp(argv[0], argv);
        write_log("Failed to restart process: %s\n", strerror(errno));
        return 1;
    }

    write_log("Shutting down!\n");

    fclose(log);
    return 0;
}
