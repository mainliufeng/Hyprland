#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>
#include <unistd.h>
#include "input-method.h"

static struct wl_display*                        display;
static struct wl_seat*                           seat;
static struct zwp_input_method_manager_v2*       manager;
static unsigned                                  serial;
static int                                       active;
static struct zwp_input_method_keyboard_grab_v2* grab;
static unsigned                                  grabbed_keys;
static void                                      seat_caps(void* d, struct wl_seat* resource, unsigned caps) {}
static void                                      seat_named(void* d, struct wl_seat* resource, const char* value) {
    const char* requested = getenv("MULTISEAT_SEAT");
    if (!strcmp(value, requested ? requested : "Hyprland"))
        seat = resource;
}
static const struct wl_seat_listener seat_listener = {seat_caps, seat_named};
static void                          grab_keymap(void* d, struct zwp_input_method_keyboard_grab_v2* g, unsigned f, int fd, unsigned size) {
    close(fd);
}
static void grab_key(void* d, struct zwp_input_method_keyboard_grab_v2* g, unsigned serial, unsigned time, unsigned key, unsigned state) {
    if (state)
        grabbed_keys++;
}
static void grab_mods(void* d, struct zwp_input_method_keyboard_grab_v2* g, unsigned serial, unsigned dep, unsigned lat, unsigned locked, unsigned group) {}
static void grab_repeat(void* d, struct zwp_input_method_keyboard_grab_v2* g, int rate, int delay) {}
static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {grab_keymap, grab_key, grab_mods, grab_repeat};
static void                                                    activate(void* d, struct zwp_input_method_v2* i) {
    active = 1;
}
static void deactivate(void* d, struct zwp_input_method_v2* i) {
    active = 0;
}
static void surrounding(void* d, struct zwp_input_method_v2* i, const char* s, unsigned c, unsigned a) {}
static void cause(void* d, struct zwp_input_method_v2* i, unsigned c) {}
static void content(void* d, struct zwp_input_method_v2* i, unsigned h, unsigned p) {}
static void done(void* d, struct zwp_input_method_v2* i) {
    serial++;
}
static void unavailable(void* d, struct zwp_input_method_v2* i) {
    fprintf(stderr, "IME unavailable\n");
}
static const struct zwp_input_method_v2_listener listener = {activate, deactivate, surrounding, cause, content, done, unavailable};
static void                                      global(void* d, struct wl_registry* r, unsigned id, const char* interface, unsigned version) {
    if (!strcmp(interface, "wl_seat")) {
        struct wl_seat* bound = wl_registry_bind(r, id, &wl_seat_interface, 5);
        wl_seat_add_listener(bound, &seat_listener, NULL);
    } else if (!strcmp(interface, "zwp_input_method_manager_v2"))
        manager = wl_registry_bind(r, id, &zwp_input_method_manager_v2_interface, 1);
}
static void                              removed(void* d, struct wl_registry* r, unsigned id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
int                                      main(void) {
    display = wl_display_connect(NULL);
    if (!display)
        return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!manager || !seat)
        return 2;
    struct zwp_input_method_v2* ime = zwp_input_method_manager_v2_get_input_method(manager, seat);
    zwp_input_method_v2_add_listener(ime, &listener, NULL);
    wl_display_roundtrip(display);
    puts("ready");
    fflush(stdout);
    struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wl_display_get_fd(display), POLLIN, 0}};
    while (1) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(fds, 2, -1) < 0)
            return 1;
        if (fds[1].revents && wl_display_dispatch(display) < 0)
            return 1;
        if (fds[0].revents) {
            char text[1024];
            if (!fgets(text, sizeof(text), stdin))
                break;
            text[strcspn(text, "\n")] = 0;
            wl_display_roundtrip(display);
            if (!strcmp(text, "@grab")) {
                grab = zwp_input_method_v2_grab_keyboard(ime);
                zwp_input_method_keyboard_grab_v2_add_listener(grab, &grab_listener, NULL);
            } else if (!strcmp(text, "@ungrab")) {
                zwp_input_method_keyboard_grab_v2_release(grab);
                grab = NULL;
            } else if (!strcmp(text, "@keys")) {
                printf("%u\n", grabbed_keys);
                fflush(stdout);
                continue;
            } else {
                if (!active) {
                    fprintf(stderr, "IME is inactive\n");
                    return 3;
                }
                zwp_input_method_v2_commit_string(ime, text);
                zwp_input_method_v2_commit(ime, serial);
            }
            wl_display_roundtrip(display);
            puts("done");
            fflush(stdout);
        }
    }
    zwp_input_method_v2_destroy(ime);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
