#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
static void caps(void* d, struct wl_seat* s, uint32_t c) {
    printf("seat_caps=%u\n", c);
}
static void name(void* d, struct wl_seat* s, const char* n) {
    printf("seat_name=%s\n", n);
}
static const struct wl_seat_listener sl = {caps, name};
static void                          global(void* d, struct wl_registry* r, uint32_t n, const char* i, uint32_t v) {
    printf("global=%s version=%u\n", i, v);
    if (!strcmp(i, "wl_seat")) {
        struct wl_seat* s = wl_registry_bind(r, n, &wl_seat_interface, v < 9 ? v : 9);
        wl_seat_add_listener(s, &sl, NULL);
    }
}
static void removed(void* d, struct wl_registry* r, uint32_t n) {}
int         main() {
    struct wl_display* d = wl_display_connect(NULL);
    if (!d)
        return 1;
    struct wl_registry*               r = wl_display_get_registry(d);
    const struct wl_registry_listener l = {global, removed};
    wl_registry_add_listener(r, &l, NULL);
    wl_display_roundtrip(d);
    wl_display_roundtrip(d);
    // The socket's preferred seat is announced first. Binding it advertises
    // the shared seats; this roundtrip receives those newly bound seat names.
    wl_display_roundtrip(d);
    wl_display_disconnect(d);
    return 0;
}
