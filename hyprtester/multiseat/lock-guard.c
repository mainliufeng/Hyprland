// Independent trusted lock guardian, with no authentication or shell policy.
#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "lock-scope.h"

static struct hyprland_lock_scope_manager_v1* manager;
static void                                   global(void* data, struct wl_registry* registry, uint32_t id, const char* name, uint32_t version) {
    if (!strcmp(name, "hyprland_lock_scope_manager_v1"))
        manager = wl_registry_bind(registry, id, &hyprland_lock_scope_manager_v1_interface, 1);
}
static void                              removed(void* data, struct wl_registry* registry, uint32_t id) {}
static const struct wl_registry_listener registryListener = {global, removed};

int                                      main(void) {
    struct wl_display* display = wl_display_connect(NULL);
    if (!display)
        return 2;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, NULL);
    if (wl_display_roundtrip(display) < 0 || !manager)
        return 3;
    struct hyprland_lock_guard_v1* guard = hyprland_lock_scope_manager_v1_get_guard(manager);
    if (wl_display_roundtrip(display) < 0)
        return 4;
    puts("ready");
    fflush(stdout);
    struct pollfd descriptors[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
    for (;;) {
        wl_display_flush(display);
        if (poll(descriptors, 2, 100) < 0)
            return 5;
        if (descriptors[0].revents & POLLIN)
            if (wl_display_dispatch(display) < 0)
                return 6;
        if (descriptors[1].revents & (POLLIN | POLLHUP)) {
            char command[32];
            if (!fgets(command, sizeof(command), stdin))
                break;
            if (!strncmp(command, "destroy", 7)) {
                hyprland_lock_guard_v1_destroy(guard);
                if (wl_display_roundtrip(display) < 0)
                    return 7;
                puts("done");
                fflush(stdout);
                break;
            }
            return 8;
        }
    }
    wl_display_disconnect(display);
    return 0;
}
