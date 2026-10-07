#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "virtual-pointer.h"
#include "virtual-keyboard.h"

static struct wl_display*                      display;
static struct zwlr_virtual_pointer_manager_v1* pointer_manager;
static struct zwp_virtual_keyboard_manager_v1* keyboard_manager;
static struct wl_seat*                         seat;
static struct wl_output*                       output;
static const char *                            seat_name, *output_name;

static void                                    capabilities(void* data, struct wl_seat* resource, uint32_t caps) {}
static void                                    name(void* data, struct wl_seat* resource, const char* value) {
    if (!strcmp(value, seat_name))
        seat = resource;
}
static const struct wl_seat_listener seat_listener = {capabilities, name};
static void geometry(void* d, struct wl_output* o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sub, const char* make, const char* model, int32_t transform) {}
static void mode(void* d, struct wl_output* o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {}
static void done(void* d, struct wl_output* o) {}
static void scale(void* d, struct wl_output* o, int32_t value) {}
static void output_named(void* d, struct wl_output* o, const char* value) {
    if (!strcmp(value, output_name))
        output = o;
}
static void                            description(void* d, struct wl_output* o, const char* value) {}
static const struct wl_output_listener output_listener = {geometry, mode, done, scale, output_named, description};
static void                            global(void* d, struct wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
    if (!strcmp(interface, "wl_seat")) {
        struct wl_seat* bound = wl_registry_bind(registry, id, &wl_seat_interface, 5);
        wl_seat_add_listener(bound, &seat_listener, NULL);
    } else if (!strcmp(interface, "wl_output") && version >= 4) {
        struct wl_output* bound = wl_registry_bind(registry, id, &wl_output_interface, 4);
        wl_output_add_listener(bound, &output_listener, NULL);
    } else if (!strcmp(interface, "zwlr_virtual_pointer_manager_v1"))
        pointer_manager = wl_registry_bind(registry, id, &zwlr_virtual_pointer_manager_v1_interface, 2);
    else if (!strcmp(interface, "zwp_virtual_keyboard_manager_v1"))
        keyboard_manager = wl_registry_bind(registry, id, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void                              removed(void* d, struct wl_registry* r, uint32_t id) {}
static const struct wl_registry_listener registry_listener = {global, removed};
static uint32_t                          now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
static void sync_display(void) {
    if (wl_display_roundtrip(display) < 0) {
        perror("Wayland roundtrip");
        exit(1);
    }
}
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    seat_name   = argv[1];
    output_name = argv[2];
    display     = wl_display_connect(NULL);
    if (!display)
        return 1;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    sync_display();
    sync_display();
    if (!seat || !output || !pointer_manager || !keyboard_manager) {
        fprintf(stderr, "Missing requested seat %s or output %s\n", seat_name, output_name);
        return 1;
    }
    struct zwlr_virtual_pointer_v1* pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(pointer_manager, getenv("MULTISEAT_NULL_POINTER_SEAT") ? NULL : seat, output);
    struct zwp_virtual_keyboard_v1* keyboard   = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
    struct xkb_context*             context    = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const struct xkb_rule_names     rules      = {.layout = "us"};
    struct xkb_keymap*              keymap     = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    char*                           text       = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    char                            filename[] = "/tmp/hyprland-seat-keymap.XXXXXX";
    int                             fd         = mkstemp(filename);
    unlink(filename);
    size_t length = strlen(text) + 1;
    if (fd < 0 || write(fd, text, length) != (ssize_t)length)
        return 1;
    zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, length);
    sync_display();
    close(fd);
    free(text);
    const uint32_t shift = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    puts("ready");
    fflush(stdout);
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        unsigned x, y, code, state, mask;
        int      dx, dy;
        if (sscanf(line, "motion %u %u", &x, &y) == 2) {
            zwlr_virtual_pointer_v1_motion_absolute(pointer, now(), x, y, 1280, 800);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "relative %d %d", &dx, &dy) == 2) {
            zwlr_virtual_pointer_v1_motion(pointer, now(), wl_fixed_from_int(dx), wl_fixed_from_int(dy));
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "button %u %u", &code, &state) == 2) {
            zwlr_virtual_pointer_v1_button(pointer, now(), code, state);
            zwlr_virtual_pointer_v1_frame(pointer);
        } else if (sscanf(line, "key %u %u", &code, &state) == 2)
            zwp_virtual_keyboard_v1_key(keyboard, now(), code, state);
        else if (sscanf(line, "mods %u", &mask) == 1)
            zwp_virtual_keyboard_v1_modifiers(keyboard, mask, 0, 0, 0);
        else if (!strcmp(line, "release-seat")) {
            wl_seat_release(seat);
            seat = NULL;
        } else if (!strncmp(line, "type ", 5)) {
            for (const unsigned char* ch = (const unsigned char*)line + 5; *ch; ++ch) {
                int found = 0;
                for (xkb_keycode_t key = xkb_keymap_min_keycode(keymap); key <= xkb_keymap_max_keycode(keymap) && !found; ++key) {
                    for (xkb_level_index_t level = 0; level < 2 && !found; ++level) {
                        const xkb_keysym_t* syms  = NULL;
                        int                 count = xkb_keymap_key_get_syms_by_level(keymap, key, 0, level, &syms);
                        if (count != 1 || xkb_keysym_to_utf32(syms[0]) != *ch)
                            continue;
                        zwp_virtual_keyboard_v1_modifiers(keyboard, level ? shift : 0, 0, 0, 0);
                        zwp_virtual_keyboard_v1_key(keyboard, now(), key - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
                        zwp_virtual_keyboard_v1_key(keyboard, now(), key - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
                        zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
                        found = 1;
                    }
                }
                if (!found) {
                    fprintf(stderr, "Cannot type character %u\n", *ch);
                    return 2;
                }
                sync_display();
            }
        } else
            return 2;
        sync_display();
        puts("done");
        fflush(stdout);
    }
    zwlr_virtual_pointer_v1_destroy(pointer);
    zwp_virtual_keyboard_v1_destroy(keyboard);
    sync_display();
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wl_display_disconnect(display);
    return 0;
}
