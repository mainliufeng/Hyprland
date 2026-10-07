#define _GNU_SOURCE
#include <wayland-client.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include "session-lock.h"

static struct wl_display*                  display;
static struct wl_compositor*               compositor;
static struct wl_shm*                      shm;
static struct wl_output*                   output;
static struct wl_seat*                     seat;
static struct ext_session_lock_manager_v1* manager;
static struct wl_surface*                  surface;
static unsigned                            keys;
static int                                 locked;
static void                                keymap(void* d, struct wl_keyboard* k, unsigned f, int fd, unsigned s) {
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
static void                              configure(void* d, struct ext_session_lock_surface_v1* lock, unsigned serial, unsigned width, unsigned height) {
    int    fd     = memfd_create("private-test-lock", MFD_CLOEXEC);
    size_t length = width * height * 4;
    if (fd < 0 || ftruncate(fd, length) < 0)
        _exit(2);
    unsigned* pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        _exit(2);
    for (unsigned i = 0; i < width * height; i++)
        pixels[i] = 0xff183048;
    struct wl_shm_pool* pool   = wl_shm_create_pool(shm, fd, length);
    struct wl_buffer*   buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    munmap(pixels, length);
    close(fd);
    ext_session_lock_surface_v1_ack_configure(lock, serial);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, width, height);
    wl_surface_commit(surface);
}
static const struct ext_session_lock_surface_v1_listener surface_listener = {configure};
static void                                              on_locked(void* d, struct ext_session_lock_v1* l) {
    locked = 1;
    puts("ready");
    fflush(stdout);
}
static void finished(void* d, struct ext_session_lock_v1* l) {
    _exit(3);
}
static const struct ext_session_lock_v1_listener lock_listener = {on_locked, finished};
static void geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t transform) {}
static void mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {}
static void done(void* d, struct wl_output* o) {}
static void scale(void* d, struct wl_output* o, int32_t value) {}
static void output_named(void* d, struct wl_output* o, const char* value) {
    if (!strcmp(value, "human"))
        output = o;
}
static void                            description(void* d, struct wl_output* o, const char* value) {}
static const struct wl_output_listener output_listener = {geometry, mode, done, scale, output_named, description};
static void                            global(void* d, struct wl_registry* r, unsigned id, const char* interface, unsigned version) {
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(r, id, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
    else if (!strcmp(interface, "wl_output") && version >= 4) {
        struct wl_output* bound = wl_registry_bind(r, id, &wl_output_interface, 4);
        wl_output_add_listener(bound, &output_listener, NULL);
    } else if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
    else if (!strcmp(interface, "ext_session_lock_manager_v1"))
        manager = wl_registry_bind(r, id, &ext_session_lock_manager_v1_interface, 1);
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
    if (!compositor || !shm || !manager || !output || !seat)
        return 2;
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, NULL);
    struct ext_session_lock_v1* lock = ext_session_lock_manager_v1_lock(manager);
    ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
    surface                                          = wl_compositor_create_surface(compositor);
    struct ext_session_lock_surface_v1* lock_surface = ext_session_lock_v1_get_lock_surface(lock, surface, output);
    ext_session_lock_surface_v1_add_listener(lock_surface, &surface_listener, NULL);
    while (!locked && wl_display_dispatch(display) >= 0) {}
    char line[64];
    while (fgets(line, sizeof(line), stdin)) {
        if (wl_display_roundtrip(display) < 0)
            return 1;
        if (!strncmp(line, "keys", 4))
            printf("%u\n", keys);
        else if (!strncmp(line, "unlock", 6)) {
            ext_session_lock_v1_unlock_and_destroy(lock);
            wl_display_roundtrip(display);
            puts("done");
            fflush(stdout);
            break;
        } else
            return 2;
        fflush(stdout);
    }
    ext_session_lock_surface_v1_destroy(lock_surface);
    wl_surface_destroy(surface);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
