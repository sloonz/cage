all: test-warp test-client

test-client: main.c xdg-shell-protocol.c zwp-pointer-constraints-v1-protocol.c
	cc -o test-client main.c xdg-shell-protocol.c zwp-pointer-constraints-v1-protocol.c -lwayland-client

test-warp: warp.c
	cc -lX11 warp.c -o test-warp

xdg-shell-protocol.c:
	wayland-scanner private-code < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml > xdg-shell-protocol.c
	wayland-scanner client-header < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml > xdg-shell-client-protocol.h

zwp-pointer-constraints-v1-protocol.c:
	wayland-scanner private-code < /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml > zwp-pointer-constraints-v1-protocol.c
	wayland-scanner client-header < /usr/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml > zwp-pointer-constraints-v1-client-protocol.h

