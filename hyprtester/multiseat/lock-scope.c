// A standalone controller: no desktop-shell policy and no implicit exemptions.
#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "lock-scope.h"
static struct hyprland_lock_scope_manager_v1* manager;
static int                                    secured;
static struct wl_output*                      private_output;
static const char*                            private_output_name;
static void                                   output_name(void* data, struct wl_output* output, const char* name) {
    if (private_output_name && !strcmp(name, private_output_name))
        private_output = output;
}
static void output_geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t transform) {}
static void output_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {}
static void output_done(void* d, struct wl_output* o) {}
static void output_scale(void* d, struct wl_output* o, int32_t value) {}
static void output_description(void* d, struct wl_output* o, const char* value) {}
static const struct wl_output_listener output_listener = {output_geometry, output_mode, output_done, output_scale, output_name, output_description};
static void                            global(void* data, struct wl_registry* registry, uint32_t id, const char* name, uint32_t version) {
    if (!strcmp(name, "hyprland_lock_scope_manager_v1"))
        manager = wl_registry_bind(registry, id, &hyprland_lock_scope_manager_v1_interface, 1);
    else if (!strcmp(name, "wl_output") && version >= 4) {
        struct wl_output* output = wl_registry_bind(registry, id, &wl_output_interface, 4);
        wl_output_add_listener(output, &output_listener, NULL);
    }
}
static void                              removed(void* data, struct wl_registry* registry, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void                              secure(void* data, struct hyprland_lock_scope_v1* lock) {
    secured = 1;
    puts("secure");
    fflush(stdout);
}
static void finished(void* data, struct hyprland_lock_scope_v1* lock) {
    puts("finished");
    fflush(stdout);
}
static const struct hyprland_lock_scope_v1_listener lock_listener = {secure, finished};
int                                                 main(int argc, char** argv) {
    if (argc != 1 && argc != 5)
        return 2;
    private_output_name        = argc == 5 ? argv[4] : NULL;
    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 2;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0)
        return 3;
    if (!manager) {
        fprintf(stderr, "Requested socket does not expose lock scope manager\n");
        return 3;
    }
    struct hyprland_lock_scope_v1* lock = hyprland_lock_scope_manager_v1_create_lock(manager);
    hyprland_lock_scope_v1_add_listener(lock, &lock_listener, NULL);
    if (argc == 5) {
        if (!private_output) {
            fprintf(stderr, "Lock controller cannot bind requested private output %s\n", private_output_name);
            return 3;
        }
        hyprland_lock_scope_v1_allow_seat(lock, argv[1], argv[2], argv[3]);
        hyprland_lock_scope_v1_exclude_output(lock, private_output);
    }
    hyprland_lock_scope_v1_activate(lock);
    if (wl_display_roundtrip(display) < 0)
        return 4;
    struct pollfd descriptors[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    for (;;) {
        wl_display_flush(display);
        if (poll(descriptors, 2, 100) < 0)
            return 5;
        if (descriptors[0].revents & POLLIN)
            if (wl_display_dispatch(display) < 0)
                return 6;
        if (descriptors[1].revents & POLLIN) {
            char command[32];
            if (!fgets(command, sizeof(command), stdin))
                break;
            if (!strncmp(command, "unlock", 6) && secured) {
                hyprland_lock_scope_v1_unlock_and_destroy(lock);
                wl_display_roundtrip(display);
                break;
            }
        }
    }
    wl_display_disconnect(display);
    return 0;
}
