// A standalone controller: no desktop-shell policy and no implicit exemptions.
#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "lock-scope.h"
static struct hyprland_lock_scope_manager_v1* manager;
static int                                    secured;
static void                                   global(void* data, struct wl_registry* registry, uint32_t id, const char* name, uint32_t version) {
    if (!strcmp(name, "hyprland_lock_scope_manager_v1"))
        manager = wl_registry_bind(registry, id, &hyprland_lock_scope_manager_v1_interface, 1);
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
int                                                 main(void) {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 2;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager)
        return 3;
    struct hyprland_lock_scope_v1* lock = hyprland_lock_scope_manager_v1_create_lock(manager);
    hyprland_lock_scope_v1_add_listener(lock, &lock_listener, NULL);
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
