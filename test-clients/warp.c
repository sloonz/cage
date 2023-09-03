#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

static Cursor get_empty_cursor(Display *display, Window root) {       
    static Cursor cursor;
    static const char data[] = { 0 };

    if (!cursor)
    {
        XColor bg;
        Pixmap pixmap;

        bg.red = bg.green = bg.blue = 0x0000;
        pixmap = XCreateBitmapFromData(display, root, data, 1, 1 );
        if(pixmap)
        {
            cursor = XCreatePixmapCursor(display, pixmap, pixmap, &bg, &bg, 0, 0);
            XFreePixmap(display, pixmap);
        }
    }

    return cursor;
}

int main(int argc, char *argv[]) {
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Error opening display\n");
        return 1;
    }

    fprintf(stderr, "Display: %s\n", getenv("DISPLAY"));

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Cursor emptyCursor = get_empty_cursor(display, root);

    Window window = XCreateSimpleWindow(display, root, 0, 0, 800, 600, 0, 0, 0);
    XMapWindow(display, window);
    XSelectInput(display, window, PointerMotionMask | ButtonPressMask | ButtonReleaseMask | StructureNotifyMask);

    XFlush(display);
    XEvent event;
    do {
        XNextEvent(display, &event);
    } while (event.type != MapNotify || event.xmap.event != window);

    int startX, startY;
    int moving = 0;
    int warp_serial = 0;
    for(;;) {
        XNextEvent(display, &event);
        if(event.type == ButtonPress) {
            moving = 1;
            startX = event.xbutton.x;
            startY = event.xbutton.y;
            XDefineCursor(display, window, emptyCursor);
            XFlush(display);
        }
        if(event.type == ButtonRelease) {
            moving = 0;
            XDefineCursor(display, window, None);
            XFlush(display);
        }
        if(event.type == MotionNotify && moving) {
            fprintf(stderr, "%d %d\n", abs(startX-event.xmotion.x), abs(startY-event.xmotion.y));
            if(event.xmotion.serial > warp_serial) {
                if(XGrabPointer(display, root, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime ) != GrabSuccess) {
                    return 1;
                }
                XWarpPointer(display, root, root, 0, 0, 0, 0, startX, startY);
                warp_serial = NextRequest(display);
                XUngrabPointer(display, CurrentTime);
                XNoOp(display);
                XFlush(display);
            }
        }
    }

    XFlush(display);
    XCloseDisplay(display);

    return 0;
}
