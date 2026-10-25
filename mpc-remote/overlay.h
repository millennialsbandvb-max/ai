/* The MPC browser on the MPC's own screen: shared memory between
 *   mpc-buttons.so inside MPC (overlay.c): finds the MPC Browser plugin's page on screen (an area painted
 *     OVERLAY_MARKER), shows the browser's picture over it on a spare hardware layer, and passes touches on it here;
 *   the MPC Browser plugin: its buttons (back, forward, reload, home);
 *   mpc-remote: drives the browser (DevTools), puts its picture here and hands it the touches and buttons.
 * Either side creates the file. */
#ifndef MPC_OVERLAY_H
#define MPC_OVERLAY_H
#include <stdint.h>

#define OVERLAY_PATH "/dev/shm/mpc-overlay"
#define OVERLAY_MAGIC 0x59414c4fu        /* "OLAY" */
#define OVERLAY_MARKER 0x123456u         /* XRGB: the plugin page's "browser goes here" colour */
#define OVERLAY_MAX_PIX (1280u * 800u)
#define OVERLAY_TOUCHES 64u

enum { OV_DOWN = 1, OV_MOVE = 2, OV_UP = 3, OV_WHEEL = 4 };   /* OV_WHEEL: slot holds the scroll (120 = one notch) */
enum { OV_CMD_BACK = 1, OV_CMD_FORWARD = 2, OV_CMD_RELOAD = 3, OV_CMD_HOME = 4, OV_CMD_KEYBOARD = 5 };

struct overlay_shm {
    uint32_t magic;
    /* MPC: */
    uint32_t visible;                  /* the plugin's page is on screen, at: */
    int32_t x, y, w, h;                /* ... as the screen is seen (landscape 1280x800) */
    uint32_t touch_wr;                 /* touches on that area, for the browser (ring of OVERLAY_TOUCHES) */
    struct { int16_t kind, x, y, slot; } touch[OVERLAY_TOUCHES];   /* x, y within the area */
    /* the plugin: */
    uint32_t cmd_seq, cmd;
    /* mpc-remote: */
    uint32_t frame_seq;                /* a new picture is in frame[frame_seq & 1] */
    uint32_t fw, fh;                   /* its size as stored, already turned for the panel (fw = h, fh = w) */
    uint32_t pad[8];
    uint32_t frame[2][OVERLAY_MAX_PIX];   /* XRGB rows of fw pixels */
};
#endif
