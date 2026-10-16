#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg.h"
#include "idle.h"

static struct wl_display*                  display;
static struct wl_compositor*               compositor;
static struct wl_shm*                      shm;
static struct xdg_wm_base*                 shell;
static struct zwp_idle_inhibit_manager_v1* idle;
static struct zwp_idle_inhibitor_v1*       inhibitors[64];
static struct wl_buffer*                   buffer;
struct Window {
    struct wl_surface*   surface;
    struct xdg_surface*  xdg;
    struct xdg_toplevel* top;
    int                  configured, wanted;
};
static struct Window windows[64];
static int           count;
static void          submit(struct Window* w) {
    if (!w->configured || !w->wanted)
        return;
    wl_surface_attach(w->surface, buffer, 0, 0);
    wl_surface_damage(w->surface, 0, 0, 64, 64);
    wl_surface_commit(w->surface);
}
static void configure(void* data, struct xdg_surface* xdg, uint32_t serial) {
    struct Window* w = data;
    xdg_surface_ack_configure(xdg, serial);
    w->configured = 1;
    submit(w);
}
static const struct xdg_surface_listener xdg_listener = {configure};
static void                              ping(void* data, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener  shell_listener = {ping};
static void                               top_configure(void* data, struct xdg_toplevel* top, int32_t x, int32_t y, struct wl_array* states) {}
static void                               top_close(void* data, struct xdg_toplevel* top) {}
static const struct xdg_toplevel_listener top_listener = {.configure = top_configure, .close = top_close};
static void                               global(void* data, struct wl_registry* registry, uint32_t id, const char* name, uint32_t version) {
    if (!strcmp(name, "wl_compositor"))
        compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    else if (!strcmp(name, "wl_shm"))
        shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    else if (!strcmp(name, "xdg_wm_base")) {
        shell = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    } else if (!strcmp(name, "zwp_idle_inhibit_manager_v1"))
        idle = wl_registry_bind(registry, id, &zwp_idle_inhibit_manager_v1_interface, 1);
}
static void                              removed(void* data, struct wl_registry* registry, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static void                              sync_display(void) {
    if (wl_display_roundtrip(display) < 0)
        exit(2);
}
int main(void) {
    display = wl_display_connect(NULL);
    if (!display)
        return 1;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    sync_display();
    sync_display();
    if (!compositor || !shm || !shell || !idle)
        return 3;
    int fd = memfd_create("synthetic-routing", MFD_CLOEXEC);
    ftruncate(fd, 64 * 64 * 4);
    void* pixels = mmap(NULL, 64 * 64 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < 64 * 64; ++i)
        ((unsigned*)pixels)[i] = 0xff4c71a9;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, 64 * 64 * 4);
    buffer                   = wl_shm_pool_create_buffer(pool, 0, 64, 64, 64 * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    puts("ready");
    fflush(stdout);
    char line[1024], argument[512];
    int  a, b;
    while (fgets(line, sizeof(line), stdin)) {
        if (sscanf(line, "create %511s", argument) == 1) {
            if (count == 64)
                return 4;
            struct Window* w = &windows[count++];
            w->surface       = wl_compositor_create_surface(compositor);
            w->xdg           = xdg_wm_base_get_xdg_surface(shell, w->surface);
            xdg_surface_add_listener(w->xdg, &xdg_listener, w);
            w->top = xdg_surface_get_toplevel(w->xdg);
            xdg_toplevel_add_listener(w->top, &top_listener, w);
            xdg_toplevel_set_title(w->top, argument);
            xdg_toplevel_set_app_id(w->top, "cornice-routing-synthetic");
            wl_surface_commit(w->surface);
        } else if (sscanf(line, "map %d", &a) == 1) {
            windows[a].wanted = 1;
            if (windows[a].configured)
                submit(&windows[a]);
            else
                wl_surface_commit(windows[a].surface);
        } else if (sscanf(line, "unmap %d", &a) == 1) {
            windows[a].wanted     = 0;
            windows[a].configured = 0;
            wl_surface_attach(windows[a].surface, NULL, 0, 0);
            wl_surface_commit(windows[a].surface);
        } else if (sscanf(line, "inhibit %d", &a) == 1)
            inhibitors[a] = zwp_idle_inhibit_manager_v1_create_inhibitor(idle, windows[a].surface);
        else if (sscanf(line, "release %d", &a) == 1) {
            zwp_idle_inhibitor_v1_destroy(inhibitors[a]);
            inhibitors[a] = NULL;
        } else if (sscanf(line, "destroy %d", &a) == 1) {
            xdg_toplevel_destroy(windows[a].top);
            xdg_surface_destroy(windows[a].xdg);
            wl_surface_destroy(windows[a].surface);
        } else if (!strncmp(line, "quit", 4))
            break;
        else if (strncmp(line, "sync", 4))
            return 5;
        sync_display();
        sync_display();
        puts("done");
        fflush(stdout);
    }
    wl_display_disconnect(display);
    return 0;
}
