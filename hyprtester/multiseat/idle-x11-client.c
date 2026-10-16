#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    Display* display = XOpenDisplay(NULL);
    if (!display)
        return 1;
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 80, 80, 0, 0, 0x315072);
    XStoreName(display, window, "idle-x11-retained");
    XClassHint hint = {"idle-x11", "idle-x11"};
    XSetClassHint(display, window, &hint);
    XSync(display, False);
    puts("ready");
    fflush(stdout);
    char command[32];
    while (fgets(command, sizeof(command), stdin)) {
        if (!strncmp(command, "map", 3))
            XMapWindow(display, window);
        else if (!strncmp(command, "unmap", 5))
            XUnmapWindow(display, window);
        else if (!strncmp(command, "quit", 4))
            break;
        else
            return 2;
        XSync(display, False);
        puts("done");
        fflush(stdout);
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
