#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include "layer-shell.h"
#include "focus-grab.h"

static struct wl_display*                     display;
static struct wl_compositor*                  compositor;
static struct wl_shm*                         shm;
static struct wl_output*                      output;
static struct wl_seat*                        seat;
static struct zwlr_layer_shell_v1*            manager;
static struct hyprland_focus_grab_manager_v1* focus_manager;
static struct hyprland_focus_grab_v1*         grab;
static struct wl_surface*                     surface;
static unsigned                               keys;
static int                                    configured;
static void                                   keymap(void* d, struct wl_keyboard* k, unsigned f, int fd, unsigned s) {
    close(fd);
}
static void enter(void* d, struct wl_keyboard* k, unsigned s, struct wl_surface* w, struct wl_array* a) {}
static void leave(void* d, struct wl_keyboard* k, unsigned s, struct wl_surface* w) {}
static void key(void* d, struct wl_keyboard* k, unsigned s, unsigned t, unsigned c, unsigned state) {
    if (state)
        keys++;
}
static void                              mods(void* d, struct wl_keyboard* k, unsigned s, unsigned dep, unsigned lat, unsigned lock, unsigned group) {}
static const struct wl_keyboard_listener keyboard_listener = {keymap, enter, leave, key, mods};
static void                              configure(void* d, struct zwlr_layer_surface_v1* layer, unsigned serial, unsigned width, unsigned height) {
    int    fd     = memfd_create("private-test-lock", MFD_CLOEXEC);
    size_t length = width * height * 4;
    if (fd < 0 || ftruncate(fd, length) < 0)
        _exit(2);
    unsigned* pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        _exit(2);
    for (unsigned i = 0; i < width * height; i++)
        pixels[i] = 0xffc02040;
    struct wl_shm_pool* pool   = wl_shm_create_pool(shm, fd, length);
    struct wl_buffer*   buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, length);
    close(fd);
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    configured = 1;
}
static void closed(void* d, struct zwlr_layer_surface_v1* layer) {
    _exit(3);
}
static const struct zwlr_layer_surface_v1_listener  surface_listener = {configure, closed};
static void                                         cleared(void* d, struct hyprland_focus_grab_v1* g) {}
static const struct hyprland_focus_grab_v1_listener grab_listener = {cleared};
static void                                         global(void* d, struct wl_registry* r, unsigned id, const char* interface, unsigned version) {
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(interface, "wl_output"))
        output = wl_registry_bind(r, id, &wl_output_interface, 1);
    else if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
    else if (!strcmp(interface, "zwlr_layer_shell_v1"))
        manager = wl_registry_bind(r, id, &zwlr_layer_shell_v1_interface, 4);
    else if (!strcmp(interface, "hyprland_focus_grab_manager_v1"))
        focus_manager = wl_registry_bind(r, id, &hyprland_focus_grab_manager_v1_interface, 1);
}
static void                              removed(void* d, struct wl_registry* r, unsigned id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
int                                      main(void) {
    display = wl_display_connect(NULL);
    if (!display)
        return 1;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !manager || !focus_manager || !output || !seat)
        return 2;
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, NULL);
    surface                             = wl_compositor_create_surface(compositor);
    struct zwlr_layer_surface_v1* layer = zwlr_layer_shell_v1_get_layer_surface(manager, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "agent-seat-layer-test");
    zwlr_layer_surface_v1_add_listener(layer, &surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer, 320, 100);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(surface);
    while (!configured && wl_display_dispatch(display) >= 0) {}
    wl_display_roundtrip(display);
    puts("ready");
    fflush(stdout);
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (wl_display_roundtrip(display) < 0)
            return 1;
        if (!strncmp(line, "keys", 4))
            printf("%u\n", keys);
        else if (!strncmp(line, "grab", 4)) {
            grab = hyprland_focus_grab_manager_v1_create_grab(focus_manager);
            hyprland_focus_grab_v1_add_listener(grab, &grab_listener, NULL);
            hyprland_focus_grab_v1_add_surface(grab, surface);
            hyprland_focus_grab_v1_commit(grab);
            wl_display_roundtrip(display);
            puts("done");
        } else if (!strncmp(line, "stop", 4)) {
            puts("done");
            fflush(stdout);
            break;
        } else
            return 2;
        fflush(stdout);
    }
    if (grab)
        hyprland_focus_grab_v1_destroy(grab);
    zwlr_layer_surface_v1_destroy(layer);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
