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
static void output_geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t transform) {}
static void output_mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {}
static void output_done(void* d, struct wl_output* o) {}
static void output_scale(void* d, struct wl_output* o, int32_t value) {}
static void output_name(void* d, struct wl_output* o, const char* value) {
    printf("output_name=%s\n", value);
}
static void                            output_description(void* d, struct wl_output* o, const char* value) {}
static const struct wl_output_listener output_listener = {output_geometry, output_mode, output_done, output_scale, output_name, output_description};
static void                            global(void* d, struct wl_registry* r, uint32_t n, const char* i, uint32_t v) {
    printf("global=%s version=%u\n", i, v);
    if (!strcmp(i, "wl_seat")) {
        struct wl_seat* s = wl_registry_bind(r, n, &wl_seat_interface, v < 9 ? v : 9);
        wl_seat_add_listener(s, &sl, NULL);
    } else if (!strcmp(i, "wl_output") && v >= 4) {
        struct wl_output* output = wl_registry_bind(r, n, &wl_output_interface, 4);
        wl_output_add_listener(output, &output_listener, NULL);
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
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0)
        return 2;
    // The socket's preferred seat is announced first. Binding it advertises
    // the shared seats; this roundtrip receives those newly bound seat names.
    if (wl_display_roundtrip(d) < 0)
        return 2;
    wl_display_disconnect(d);
    return 0;
}
