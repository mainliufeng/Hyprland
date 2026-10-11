#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <glib-unix.h>
#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "constraints.h"
#include "relative-pointer.h"
#include "activation.h"
#include "shortcut-inhibit.h"

static struct zwp_keyboard_shortcuts_inhibit_manager_v1* shortcut_manager;
static struct zwp_keyboard_shortcuts_inhibitor_v1*       inhibitor;
static struct zwp_keyboard_shortcuts_inhibitor_v1*       other_inhibitor;
static struct wl_seat*                                   other_seat;
static void                                              shortcut_active(void* data, struct zwp_keyboard_shortcuts_inhibitor_v1* resource) {
    fprintf(stderr, "shortcut-active-%s\n", (const char*)data);
}
static void shortcut_inactive(void* data, struct zwp_keyboard_shortcuts_inhibitor_v1* resource) {
    fprintf(stderr, "shortcut-inactive-%s\n", (const char*)data);
}
static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener shortcut_listener = {shortcut_active, shortcut_inactive};
static struct xdg_activation_v1*                                 activation;
static unsigned                                                  activation_serial;
static unsigned                                                  key_presses;
static unsigned                                                  pointer_serial;
static int                                                       pointer_focused;
static char                                                      activation_token[256];
static void                                                      keymap(void* d, struct wl_keyboard* k, unsigned f, int fd, unsigned s) {
    close(fd);
}
static void enter(void* d, struct wl_keyboard* k, unsigned serial, struct wl_surface* s, struct wl_array* a) {
    activation_serial = serial;
}
static void leave(void* d, struct wl_keyboard* k, unsigned serial, struct wl_surface* s) {}
static void key(void* d, struct wl_keyboard* k, unsigned serial, unsigned time, unsigned code, unsigned state) {
    activation_serial = serial;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        key_presses++;
}
static void                              mods(void* d, struct wl_keyboard* k, unsigned serial, unsigned dep, unsigned lat, unsigned locked, unsigned group) {}
static void                              repeat_info(void* d, struct wl_keyboard* k, int rate, int delay) {}
static const struct wl_keyboard_listener keyboard_listener = {keymap, enter, leave, key, mods, repeat_info};
static void                              pointer_enter(void* d, struct wl_pointer* p, unsigned serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    pointer_serial  = serial;
    pointer_focused = 1;
    fprintf(stderr, "wayland-pointer-enter serial=%u\n", serial);
    fflush(stderr);
}
static void pointer_leave(void* d, struct wl_pointer* p, unsigned serial, struct wl_surface* s) {
    pointer_focused = 0;
    fprintf(stderr, "wayland-pointer-leave serial=%u\n", serial);
    fflush(stderr);
}
static void pointer_motion(void* d, struct wl_pointer* p, unsigned time, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_button(void* d, struct wl_pointer* p, unsigned serial, unsigned time, unsigned button, unsigned state) {
    fprintf(stderr, "wayland-button serial=%u button=%u state=%u\n", serial, button, state);
    fflush(stderr);
}
static void                             pointer_axis(void* d, struct wl_pointer* p, unsigned time, unsigned axis, wl_fixed_t value) {}
static void                             pointer_frame(void* d, struct wl_pointer* p) {}
static void                             pointer_source(void* d, struct wl_pointer* p, unsigned source) {}
static void                             pointer_stop(void* d, struct wl_pointer* p, unsigned time, unsigned axis) {}
static void                             pointer_discrete(void* d, struct wl_pointer* p, unsigned axis, int discrete) {}
static const struct wl_pointer_listener pointer_listener = {pointer_enter, pointer_leave,  pointer_motion, pointer_button,  pointer_axis,
                                                            pointer_frame, pointer_source, pointer_stop,   pointer_discrete};
static void                             token_done(void* d, struct xdg_activation_token_v1* t, const char* token) {
    snprintf(activation_token, sizeof(activation_token), "%s", token);
}
static const struct xdg_activation_token_v1_listener token_listener = {token_done};

static struct wl_display*                            display;
static struct wl_compositor*                         compositor;
static struct wl_seat*                               seat;
static void                                          seat_caps(void* d, struct wl_seat* resource, unsigned caps) {}
static void                                          seat_named(void* d, struct wl_seat* resource, const char* value) {
    const char* requested = getenv("MULTISEAT_SEAT");
    if (!strcmp(value, requested ? requested : "Hyprland"))
        seat = resource;
    const char* other = getenv("MULTISEAT_OTHER_SEAT");
    if (other && !strcmp(value, other))
        other_seat = resource;
}
static const struct wl_seat_listener           seat_listener = {seat_caps, seat_named};
static struct zwp_pointer_constraints_v1*      constraints;
static struct zwp_relative_pointer_manager_v1* relative_manager;
static struct zwp_locked_pointer_v1*           locked;
static struct zwp_confined_pointer_v1*         confined;
static GtkWidget*                              window;
static unsigned                                relative_events;
static int                                     drag_mode;
static unsigned                                drag_requests;
static int                                     drag_last_mode;
static void                                    drag_done(void* data, struct wl_callback* callback, uint32_t serial) {
    const int mode = GPOINTER_TO_INT(data);
    wl_callback_destroy(callback);
    drag_last_mode = mode;
    ++drag_requests;
    fprintf(stderr, "drag-requested action=%s count=%u\n", mode == 1 ? "move" : "resize", drag_requests);
    fflush(stderr);
}
static const struct wl_callback_listener drag_listener = {drag_done};
static gboolean                          button_press(GtkWidget* widget, GdkEventButton* event, gpointer data) {
    GdkDevice* device = gdk_event_get_device((GdkEvent*)event);
    fprintf(stderr, "gtk-button button=%u drag=%d device=%s\n", event->button, drag_mode, device ? gdk_device_get_name(device) : "none");
    fflush(stderr);
    if (event->button != 1 || !drag_mode)
        return FALSE;
    const int mode = drag_mode;
    drag_mode      = 0;
    if (mode == 1)
        gdk_window_begin_move_drag_for_device(gtk_widget_get_window(window), gdk_event_get_device((GdkEvent*)event), 1, event->x_root, event->y_root, event->time);
    else
        gdk_window_begin_resize_drag_for_device(gtk_widget_get_window(window), GDK_WINDOW_EDGE_SOUTH_EAST, gdk_event_get_device((GdkEvent*)event), 1, event->x_root, event->y_root,
                                                event->time);
    fprintf(stderr, "drag-begin-returned action=%s time=%" G_GINT64_FORMAT "\n", mode == 1 ? "move" : "resize", g_get_monotonic_time());
    fflush(stderr);
    // Fence the real xdg_toplevel request without blocking a GDK event callback.
    // Only the server's sync done event acknowledges the request to the driver.
    struct wl_callback* callback = wl_display_sync(display);
    if (!callback) {
        fprintf(stderr, "drag request sync allocation failed\n");
        exit(1);
    }
    wl_callback_add_listener(callback, &drag_listener, GINT_TO_POINTER(mode));
    fprintf(stderr, "drag-sync-created action=%s time=%" G_GINT64_FORMAT "\n", mode == 1 ? "move" : "resize", g_get_monotonic_time());
    fflush(stderr);
    gdk_display_flush(gdk_display_get_default());
    fprintf(stderr, "drag-flush-returned action=%s time=%" G_GINT64_FORMAT "\n", mode == 1 ? "move" : "resize", g_get_monotonic_time());
    fflush(stderr);
    return TRUE;
}
static void relative(void* d, struct zwp_relative_pointer_v1* r, unsigned hi, unsigned lo, wl_fixed_t x, wl_fixed_t y, wl_fixed_t ux, wl_fixed_t uy) {
    relative_events++;
}
static const struct zwp_relative_pointer_v1_listener relative_listener = {relative};
static void                                          locked_event(void* d, struct zwp_locked_pointer_v1* p) {
    fprintf(stderr, "locked\n");
}
static void unlocked_event(void* d, struct zwp_locked_pointer_v1* p) {
    fprintf(stderr, "unlocked\n");
}
static const struct zwp_locked_pointer_v1_listener locked_listener = {locked_event, unlocked_event};
static void                                        confined_event(void* d, struct zwp_confined_pointer_v1* p) {
    fprintf(stderr, "confined\n");
}
static void unconfined_event(void* d, struct zwp_confined_pointer_v1* p) {
    fprintf(stderr, "unconfined\n");
}
static const struct zwp_confined_pointer_v1_listener confined_listener = {confined_event, unconfined_event};
static void                                          global(void* d, struct wl_registry* r, unsigned id, const char* interface, unsigned version) {
    if (!strcmp(interface, "wl_seat")) {
        struct wl_seat* bound = wl_registry_bind(r, id, &wl_seat_interface, 5);
        wl_seat_add_listener(bound, &seat_listener, NULL);
    } else if (!strcmp(interface, "zwp_keyboard_shortcuts_inhibit_manager_v1"))
        shortcut_manager = wl_registry_bind(r, id, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
    else if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(r, id, &wl_compositor_interface, 1);
    else if (!strcmp(interface, "zwp_pointer_constraints_v1"))
        constraints = wl_registry_bind(r, id, &zwp_pointer_constraints_v1_interface, 1);
    else if (!strcmp(interface, "zwp_relative_pointer_manager_v1"))
        relative_manager = wl_registry_bind(r, id, &zwp_relative_pointer_manager_v1_interface, 1);
    else if (!strcmp(interface, "xdg_activation_v1"))
        activation = wl_registry_bind(r, id, &xdg_activation_v1_interface, 1);
}
static void                              removed(void* d, struct wl_registry* r, unsigned id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static gboolean                          input(gint fd, GIOCondition condition, gpointer pointer) {
    char text[64];
    if (!fgets(text, sizeof(text), stdin)) {
        gtk_main_quit();
        return FALSE;
    }
    struct wl_surface* surface = gdk_wayland_window_get_wl_surface(gtk_widget_get_window(window));
    if (!strncmp(text, "hide-cursor", 11)) {
        if (!pointer_serial)
            return FALSE;
        wl_pointer_set_cursor(pointer, pointer_serial, NULL, 0, 0);
    } else if (!strncmp(text, "activate", 8)) {
        struct xdg_activation_token_v1* token = xdg_activation_v1_get_activation_token(activation);
        activation_token[0]                   = 0;
        xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
        xdg_activation_token_v1_set_serial(token, activation_serial, seat);
        xdg_activation_token_v1_commit(token);
        wl_display_roundtrip(display);
        if (!activation_token[0])
            return FALSE;
        xdg_activation_v1_activate(activation, activation_token, surface);
        xdg_activation_token_v1_destroy(token);
    } else if (!strncmp(text, "inhibit-other", 13)) {
        if (!shortcut_manager || !other_seat || other_inhibitor)
            return FALSE;
        other_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(shortcut_manager, surface, other_seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(other_inhibitor, &shortcut_listener, "other");
    } else if (!strncmp(text, "inhibit", 7)) {
        if (!shortcut_manager || !seat || inhibitor)
            return FALSE;
        inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(shortcut_manager, surface, seat);
        zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &shortcut_listener, "primary");
    } else if (!strncmp(text, "uninhibit-other", 15)) {
        if (!other_inhibitor)
            return FALSE;
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(other_inhibitor);
        other_inhibitor = NULL;
    } else if (!strncmp(text, "uninhibit", 9)) {
        if (!inhibitor)
            return FALSE;
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor);
        inhibitor = NULL;
    } else if (!strncmp(text, "release-seat", 12)) {
        wl_seat_release(seat);
        seat = NULL;
    } else if (!strncmp(text, "pointer-focused", 15)) {
        printf("%d\n", pointer_focused);
        fflush(stdout);
        return TRUE;
    } else if (!strncmp(text, "drag-state", 10)) {
        fprintf(stderr, "drag-state count=%u mode=%d time=%" G_GINT64_FORMAT "\n", drag_requests, drag_last_mode, g_get_monotonic_time());
        fflush(stderr);
        printf("%u %s\n", drag_requests, drag_last_mode == 1 ? "move" : drag_last_mode == 2 ? "resize" : "none");
        fflush(stdout);
        return TRUE;
    } else if (!strncmp(text, "keys", 4)) {
        wl_display_roundtrip(display);
        printf("%u\n", key_presses);
        fflush(stdout);
        return TRUE;
    } else if (!strncmp(text, "move", 4)) {
        drag_mode = 1;
    } else if (!strncmp(text, "resize", 6)) {
        drag_mode = 2;
    } else if (!strncmp(text, "lock", 4)) {
        locked = zwp_pointer_constraints_v1_lock_pointer(constraints, surface, pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_locked_pointer_v1_add_listener(locked, &locked_listener, NULL);
    } else if (!strncmp(text, "confine", 7)) {
        struct wl_region* region = wl_compositor_create_region(compositor);
        wl_region_add(region, 50, 50, 100, 100);
        confined = zwp_pointer_constraints_v1_confine_pointer(constraints, surface, pointer, region, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_confined_pointer_v1_add_listener(confined, &confined_listener, NULL);
        wl_region_destroy(region);
    } else if (!strncmp(text, "unlock", 6)) {
        if (locked) {
            zwp_locked_pointer_v1_destroy(locked);
            locked = NULL;
        }
        if (confined) {
            zwp_confined_pointer_v1_destroy(confined);
            confined = NULL;
        }
    } else if (!strncmp(text, "relative", 8)) {
        wl_display_roundtrip(display);
        printf("%u\n", relative_events);
        fflush(stdout);
        return TRUE;
    } else
        return FALSE;
    if (wl_display_roundtrip(display) < 0)
        return FALSE;
    puts("done");
    fflush(stdout);
    return TRUE;
}
int main(int argc, char** argv) {
    gtk_init(&argc, &argv);
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "constraints-window");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 350);
    gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(window, "button-press-event", G_CALLBACK(button_press), NULL);
    gtk_container_add(GTK_CONTAINER(window), gtk_label_new("Real per-seat locked and confined pointer"));
    gtk_widget_show_all(window);
    display = gdk_wayland_display_get_wl_display(gdk_display_get_default());
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!seat || !constraints || !compositor || !relative_manager || !activation)
        return 2;
    wl_display_roundtrip(display);
    struct wl_pointer* pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, NULL);
    struct zwp_relative_pointer_v1* rp = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, pointer);
    zwp_relative_pointer_v1_add_listener(rp, &relative_listener, NULL);
    g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP, input, pointer);
    puts("ready");
    fflush(stdout);
    gtk_main();
    zwp_relative_pointer_v1_destroy(rp);
    return 0;
}
