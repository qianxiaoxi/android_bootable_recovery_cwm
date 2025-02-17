/*
 * Copyright (C) 2007 The Android Open Source Project
 * Copyright (C) 2014 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "cutils/android_reboot.h"
#include "cutils/properties.h"
#include "minui/minui.h"
#include "recovery_ui.h"
#include "voldclient/voldclient.h"

extern int __system(const char *command);
extern int volumes_changed();

static int cur_rainbow_color = 0;
static int gRainbowMode = 0;

#if defined(BOARD_HAS_NO_SELECT_BUTTON) || defined(BOARD_TOUCH_RECOVERY)
static int gShowBackButton = 1;
#else
static int gShowBackButton = 0;
#endif

#define MAX_COLS 96
#define MAX_ROWS 24
#define MENU_MAX_COLS 64
#define MENU_MAX_ROWS 250
#define MENU_ITEM_HEADER " - "
#define MENU_ITEM_HEADER_LENGTH strlen(MENU_ITEM_HEADER)

#define MIN_LOG_ROWS 3

#define CHAR_WIDTH BOARD_RECOVERY_CHAR_WIDTH
#define CHAR_HEIGHT BOARD_RECOVERY_CHAR_HEIGHT

// Delay in seconds to refresh clock and USB plugged volumes
#define REFRESH_TIME_USB_INTERVAL 5

#define UI_WAIT_KEY_TIMEOUT_SEC    3600
#define UI_KEY_REPEAT_INTERVAL 80
#define UI_KEY_WAIT_REPEAT 400
#define UI_MIN_PROG_DELTA_MS 200

UIParameters ui_parameters = {
    6,       // indeterminate progress bar frames
    20,      // fps
    7,       // installation icon frames (0 == static image)
    13, 190, // installation icon overlay offset
};

static pthread_mutex_t gUpdateMutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface *gInstallationOverlay;
static gr_surface *gProgressBarIndeterminate;
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;
static gr_surface gBackground;
static int ui_has_initialized = 0;
static int ui_log_stdout = 1;

static int boardEnableKeyRepeat = 0;
static int boardRepeatableKeys[64];
static int boardNumRepeatableKeys = 0;

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_ICON_INSTALLING],          "icon_installing" },
    { &gBackgroundIcon[BACKGROUND_ICON_ERROR],               "icon_error" },
    { &gBackgroundIcon[BACKGROUND_ICON_CLOCKWORK],           "icon_clockwork" },
    { &gBackgroundIcon[BACKGROUND_ICON_CID],                 "icon_cid" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_INSTALLING], "icon_firmware_install" },
    { &gBackgroundIcon[BACKGROUND_ICON_FIRMWARE_ERROR],      "icon_firmware_error" },
    { &gProgressBarEmpty,                                    "progress_empty" },
    { &gProgressBarFill,                                     "progress_fill" },
    { &gBackground,                                          "stitch" },
    { NULL,                                                  NULL },
};

static int gCurrentIcon = 0;
static int gInstallingFrame = 0;

//semi touch code
typedef struct {
    int x;
    int y;
} point;

typedef struct {
    int          fd;            // Initialize to -1
    int          touch_calibrated;
    int          slot_current;
    int          tracking_id;

    int          saw_pos_x;      // Did this sequence have an ABS_MT_POSITION_X?
    int          saw_pos_y;      // Did this sequence have an ABS_MT_POSITION_Y?
    int          saw_mt_report;  // Did this sequence have an SYN_MT_REPORT?
    int          saw_mt_tracking_id;
    int          slide_right;
    int          slide_left;
    point        touch_min;
    point        touch_max;
    point        touch_pos;      // Current touch coordinates
    point        touch_start;    // Coordinates of touch start
    point        touch_track;    // Last tracked coordinates

} input_device;

static int diff_x = 0;
static int diff_y = 0;
static int min_x_swipe_px = 100;
static int min_y_swipe_px = 80;
static int virtualkey_pressed = 0;
static int virtualkey_h = 0;
static int virtualkey_w = 0;

static int get_batt_stats(void) {
    int level = -1;
    int i = 0;
    char value[4];
    FILE * fd;
    const char *BATT_FILES[] = {
    #ifdef CUSTOM_BATT_FILE
        CUSTOM_BATT_FILE,
    #endif
        "/sys/class/power_supply/battery/capacity",
        "/sys/devices/platform/android-battery/power_supply/android-battery/capacity",
        NULL
    };

    while (BATT_FILES[i]) {
        if ((fd = fopen(BATT_FILES[i], "r"))) {
            fgets(value, 4, fd);
            fclose(fd);
            level = atoi(value);
            break;
        }
        i++;
    }

    if (level > 100)
        level = 100;
    if (level < 0)
        level = 0;
    return level;
}

static void set_min_swipe_lengths() {
    char value[PROPERTY_VALUE_MAX];
    property_get("ro.sf.lcd_density", value, "0");
    int screen_density = atoi(value);
    if(screen_density > 160) {
        min_x_swipe_px = (int)(0.5 * screen_density); // Roughly 0.5in
        min_y_swipe_px = (int)(0.5 * screen_density); // Roughly 0.5in
    } else {
        min_x_swipe_px = gr_fb_width()/4;
        min_y_swipe_px = 3*BOARD_RECOVERY_CHAR_HEIGHT;
    }
    fprintf(stdout, "min_x_swipe_px=%d,min_y_swipe_px=%d\n", min_x_swipe_px, min_y_swipe_px);
}

static void reset_touch(input_device *dev) {
    diff_x = 0;
    diff_y = 0;
    dev->touch_start.x  = 0;
    dev->touch_start.y  = 0;
    dev->touch_pos.x = 0;
    dev->touch_pos.y = 0;
    dev->slot_current = 0;
    dev->slide_right = 0;
    dev->slide_left = 0;
    ui_clear_key_queue();
}

static int calibrate_touch(input_device *dev) {
    struct input_absinfo info;

    dev->tracking_id = -1;
    dev->touch_start.x  = 0;
    dev->touch_start.y  = 0;
    dev->touch_pos.x = 0;
    dev->touch_pos.y = 0;
    dev->slot_current = 0;
    dev->slide_right = 0;
    dev->slide_left = 0;
    dev->saw_pos_x = 0;
    dev->saw_pos_y = 0;
    dev->saw_mt_tracking_id = 0;
    dev->saw_mt_report = 0;

    memset(&info, 0, sizeof(info));
    if (ioctl(dev->fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0) {
        dev->touch_min.x = info.minimum;
        dev->touch_max.x = info.maximum;
    }
    memset(&info, 0, sizeof(info));
    if (ioctl(dev->fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0) {
        dev->touch_min.y = info.minimum;
        dev->touch_max.y = info.maximum;
    }

    if (dev->touch_min.x == dev->touch_max.x
            || dev->touch_min.y == dev->touch_max.y)
        return 0; // Probably not a touch device

    LOGI("calibrate_touch: %d\n", dev->fd);
    return 1; // Success
}
#ifdef USE_VIRTUAL_KEY
static gr_surface gVirtualKeys; // surface for our virtual key buttons
static void ui_get_virtualkey_size() {
    if (virtualkey_h == 0) {
        int result = res_create_surface("virtual_keys", &gVirtualKeys);
        if (result == 0) {
            gr_surface surface = gVirtualKeys;
            virtualkey_h = gr_get_height(surface);
            virtualkey_w = gr_get_width(surface);
            LOGI("virtualkey size: %dx%d\n", virtualkey_w, virtualkey_h);
        }
    }
}

static int ui_get_virtualkey_pressed(input_device *dev) {
    int key_code = 0;
    if (gCurrentIcon == BACKGROUND_ICON_INSTALLING)
        return key_code;

    if (dev->touch_pos.y >= (gr_fb_height() - virtualkey_h) && dev->touch_pos.x > 0) {
        int start_draw = 0;
        int end_draw = 0;
        int keywidth = virtualkey_w / 4;
        int keyoffset = (gr_fb_width() - virtualkey_w) / 2;
        if (dev->touch_pos.x < (keywidth + keyoffset + 1)) {
            key_code = KEY_VOLUMEDOWN;
            start_draw = keyoffset;
            end_draw = keywidth + keyoffset;
        } else if (dev->touch_pos.x < ((keywidth * 2) + keyoffset + 1)) {
            key_code = KEY_VOLUMEUP;
            start_draw = keywidth + keyoffset + 1;
            end_draw = (keywidth * 2) + keyoffset;
        } else if (dev->touch_pos.x < ((keywidth * 3) + keyoffset + 1)) {
            key_code = KEY_BACK;
            start_draw = (keywidth * 2) + keyoffset + 1;
            end_draw = (keywidth * 3) + keyoffset;
        } else if (dev->touch_pos.x < ((keywidth * 4) + keyoffset + 1)) {
            key_code = KEY_ENTER;
            start_draw = (keywidth * 3) + keyoffset + 1;
            end_draw = (keywidth * 4) + keyoffset;
        } else
            return key_code;

        pthread_mutex_lock(&gUpdateMutex);
        gr_color(200, 200, 200, 255);
        gr_fill(start_draw, gr_fb_height()-virtualkey_h+2, end_draw, gr_fb_height());
        gr_flip();
        pthread_mutex_unlock(&gUpdateMutex);
        virtualkey_pressed = 1;
    }
    return key_code;
}
#endif

static struct timeval lastprogupd = (struct timeval) {0};

static enum ProgressBarType {
    PROGRESSBAR_TYPE_NONE,
    PROGRESSBAR_TYPE_INDETERMINATE,
    PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

// Progress bar scope of current operation
static float gProgressScopeStart = 0.0;
static float gProgressScopeSize = 0.0;
static float gProgress = 0.0;
static double gProgressScopeTime;
static double gProgressScopeDuration;

// Set to 1 when both graphics pages are the same (except for the progress bar)
static int gPagesIdentical = 0;

// Log text overlay, displayed when a magic key is pressed
static char text[MAX_ROWS][MAX_COLS];
static int text_cols = 0;
static int text_rows = 0;
static int text_col = 0;
static int text_row = 0;
static int text_top = 0;
static int show_text = 0;
static int show_text_ever = 0; // i.e. has show_text ever been 1?

static char menu[MENU_MAX_ROWS][MENU_MAX_COLS];
static int menuTextColor[4] = {MENU_TEXT_COLOR};
static int show_menu = 0;
static int menu_top = 0;
static int menu_items = 0;
static int menu_sel = 0;
static int menu_show_start = 0; // line at which menu display starts
static int max_menu_rows;

//static unsigned cur_rainbow_color = 0;
//static int gRainbowMode = 0;

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0;
static unsigned long key_last_repeat[KEY_MAX + 1];
static unsigned long key_press_time[KEY_MAX + 1];
static volatile char key_pressed[KEY_MAX + 1];

// Prototypes for static functions that are used before defined
static void update_screen_locked(void);
static int ui_wait_key_with_repeat();
static void ui_rainbow_mode();


// Current time
static double now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// Draw the given frame over the installation overlay animation.  The
// background is not cleared or draw with the base icon first; we
// assume that the frame already contains some other frame of the
// animation.  Does nothing if no overlay animation is defined.
// Should only be called with gUpdateMutex locked.
static void draw_install_overlay_locked(int frame) {
    if (gInstallationOverlay == NULL) return;
    gr_surface surface = gInstallationOverlay[frame];
    int iconWidth = gr_get_width(surface);
    int iconHeight = gr_get_height(surface);
    gr_blit(surface, 0, 0, iconWidth, iconHeight,
            ui_parameters.install_overlay_offset_x,
            ui_parameters.install_overlay_offset_y);
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with gUpdateMutex locked.
static void draw_background_locked(int icon) {
    gPagesIdentical = 0;

    int bw = gr_get_width(gBackground);
    int bh = gr_get_height(gBackground);
    int bx = 0;
    int by = 0;
    for (by = 0; by < gr_fb_height(); by += bh) {
        for (bx = 0; bx < gr_fb_width(); bx += bw) {
            gr_blit(gBackground, 0, 0, bw, bh, bx, by);
        }
    }

    if (icon) {
        gr_surface surface = gBackgroundIcon[icon];
        int iconWidth = gr_get_width(surface);
        int iconHeight = gr_get_height(surface);
        int iconX = (gr_fb_width() - iconWidth) / 2;
        int iconY = (gr_fb_height() - iconHeight) / 2;
        gr_blit(surface, 0, 0, iconWidth, iconHeight, iconX, iconY);
        if (icon == BACKGROUND_ICON_INSTALLING) {
            draw_install_overlay_locked(gInstallingFrame);
        }
    }
}

static void ui_increment_frame() {
    if (!ui_has_initialized) return;
    gInstallingFrame =
        (gInstallingFrame + 1) % ui_parameters.installing_frames;
}

static long delta_milliseconds(struct timeval from, struct timeval to) {
    long delta_sec = (to.tv_sec - from.tv_sec)*1000;
    long delta_usec = (to.tv_usec - from.tv_usec)/1000;
    return (delta_sec + delta_usec);
}

// Draw the progress bar (if any) on the screen; does not flip pages
// Should only be called with gUpdateMutex locked and if ui_has_initialized is true
static void draw_progress_locked() {
    if (gCurrentIcon == BACKGROUND_ICON_INSTALLING) {
        // update the installation animation, if active
        if (ui_parameters.installing_frames > 0)
            ui_increment_frame();
        draw_install_overlay_locked(gInstallingFrame);
    }

    if (gProgressBarType != PROGRESSBAR_TYPE_NONE) {
        int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
        int width = gr_get_width(gProgressBarEmpty);
        int height = gr_get_height(gProgressBarEmpty);

        int dx = (gr_fb_width() - width)/2;
        int dy = (3*gr_fb_height() + iconHeight - 2*height)/4;

        // Erase behind the progress bar (in case this was a progress-only update)
        gr_color(0, 0, 0, 255);
        gr_fill(dx, dy, width, height);

        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
            float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
            int pos = (int) (progress * width);

            if (pos > 0) {
                gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);
            }
            if (pos < width-1) {
                gr_blit(gProgressBarEmpty, pos, 0, width-pos, height, dx+pos, dy);
            }
        }

        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
            static int frame = 0;
            gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height, dx, dy);
            frame = (frame + 1) % ui_parameters.indeterminate_frames;
        }
    }

    gettimeofday(&lastprogupd, NULL);
}

#ifdef USE_VIRTUAL_KEY
// Draw the virtual keys on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_virtualkeys_locked()
{
    //gr_surface surface = gVirtualKeys;
    //virtualkey_w = gr_get_width(surface);
    //virtualkey_h = gr_get_height(surface);
    int iconX = (gr_fb_width() - virtualkey_w) / 2;
    int iconY = (gr_fb_height() - virtualkey_h);
    if (virtualkey_pressed) {
        //clear virtual key area alpha.
        //gr_color(0, 0, 0, 255);
        //gr_fill(0, iconY, gr_fb_width(), gr_fb_height());
        //draw virtual key area background.
        gr_blit(gBackground, 0, 0, virtualkey_w, virtualkey_h, 0, iconY);
    }
    //draw virtual key.
    gr_blit(gVirtualKeys, 0, 0, virtualkey_w, virtualkey_h, iconX, iconY);
}
#endif

#define LEFT_ALIGN 0
#define CENTER_ALIGN 1
#define RIGHT_ALIGN 2

static void draw_text_line(int row, const char* t, int align) {
    int col = 0;
    if (t[0] != '\0') {
        if (ui_get_rainbow_mode()) ui_rainbow_mode();
        int length = strnlen(t, MENU_MAX_COLS) * CHAR_WIDTH;
        switch(align)
        {
            case LEFT_ALIGN:
                col = 1;
                break;
            case CENTER_ALIGN:
                col = ((gr_fb_width() - length) / 2);
                break;
            case RIGHT_ALIGN:
                col = gr_fb_width() - length - 1;
                break;
        }
        gr_text(col, (row+1)*CHAR_HEIGHT-1, t, 0);
    }
}

void ui_setMenuTextColor(int r, int g, int b, int a) {
    menuTextColor[0] = r;
    menuTextColor[1] = g;
    menuTextColor[2] = b;
    menuTextColor[3] = a;
}

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_screen_locked(void) {
    if (!ui_has_initialized)
        return;

    draw_background_locked(gCurrentIcon);
    draw_progress_locked();

    if (show_text) {
        int total_rows = (gr_fb_height() - virtualkey_h) / CHAR_HEIGHT;
        int i = 0;
        int j = 0;
        int row = 0; // current row that we are drawing on
        if (show_menu) {
            int batt_level = 0;
            char batt_text[40];
            batt_level = get_batt_stats();

            struct tm *current;
            time_t now;
            now = time(NULL);
            current = localtime(&now);

            if (now != NULL && now > 946684800)  // timestamp of 2000-01-01 00:00:00
            sprintf(batt_text, "[%d%% %02D:%02D]", batt_level, current->tm_hour, current->tm_min);
            else
            sprintf(batt_text, " [%d%%]", batt_level);

            gr_color(menuTextColor[0], menuTextColor[1], menuTextColor[2], menuTextColor[3]);
            //gr_color(0, 255, 0, 255);
            if (batt_level < 21)
                gr_color(255, 0, 0, 255);
            draw_text_line(0, batt_text, RIGHT_ALIGN);

            gr_color(menuTextColor[0], menuTextColor[1], menuTextColor[2], menuTextColor[3]);
            gr_fill(0, (menu_top + menu_sel - menu_show_start) * CHAR_HEIGHT,
                    gr_fb_width(), (menu_top + menu_sel - menu_show_start + 1)*CHAR_HEIGHT+1);

            gr_color(HEADER_TEXT_COLOR);
            for (i = 0; i < menu_top; ++i) {
                draw_text_line(i, menu[i], LEFT_ALIGN);
                row++;
            }

            if (menu_items - menu_show_start + menu_top >= max_menu_rows)
                j = max_menu_rows - menu_top;
            else
                j = menu_items - menu_show_start;

            gr_color(menuTextColor[0], menuTextColor[1], menuTextColor[2], menuTextColor[3]);
            for (i = menu_show_start + menu_top; i < (menu_show_start + menu_top + j); ++i) {
                if (i == menu_top + menu_sel) {
                    gr_color(255, 255, 255, 255);
                    draw_text_line(i - menu_show_start , menu[i], LEFT_ALIGN);
                    gr_color(menuTextColor[0], menuTextColor[1], menuTextColor[2], menuTextColor[3]);
                } else {
                    gr_color(menuTextColor[0], menuTextColor[1], menuTextColor[2], menuTextColor[3]);
                    draw_text_line(i - menu_show_start, menu[i], LEFT_ALIGN);
                }
                row++;
                if (row >= max_menu_rows)
                    break;
            }

            gr_fill(0, row*CHAR_HEIGHT+CHAR_HEIGHT/2-1,
                    gr_fb_width(), row*CHAR_HEIGHT+CHAR_HEIGHT/2+1);
        }

        gr_color(NORMAL_TEXT_COLOR);
        int cur_row = text_row;
        int available_rows = total_rows - row - 1;
        int start_row = row + 1;
        if (available_rows < MAX_ROWS)
            cur_row = (cur_row + (MAX_ROWS - available_rows)) % MAX_ROWS;
        else
            start_row = total_rows - MAX_ROWS;

        int r;
        for (r = 0; r < (available_rows < MAX_ROWS ? available_rows : MAX_ROWS); r++) {
            draw_text_line(start_row + r, text[(cur_row + r) % MAX_ROWS], LEFT_ALIGN);
        }
    }
#ifdef USE_VIRTUAL_KEY
    draw_virtualkeys_locked();
#endif
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with gUpdateMutex locked.
static void update_screen_locked(void) {
    if (!ui_has_initialized)
        return;

    draw_screen_locked();
    gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with gUpdateMutex locked.
static void update_progress_locked(void) {
    if (!ui_has_initialized)
        return;

    // set minimum delay between progress updates if we have a text overlay
    // exception: gProgressScopeDuration != 0: to keep zip installer refresh behavior
    struct timeval curtime;
    gettimeofday(&curtime, NULL);
    long delta_ms = delta_milliseconds(lastprogupd, curtime);
    if (show_text && gProgressScopeDuration == 0 && lastprogupd.tv_sec > 0
            && delta_ms < UI_MIN_PROG_DELTA_MS) {
        return;
    }

    if (show_text || !gPagesIdentical) {
        draw_screen_locked();    // Must redraw the whole screen
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();  // Draw only the progress bar and overlays
    }
    gr_flip();
}

// Keeps the progress bar updated, even when the process is otherwise busy.
static void *progress_thread(void *cookie) {
    double interval = 1.0 / ui_parameters.update_fps;
    for (;;) {
        double start = now();
        pthread_mutex_lock(&gUpdateMutex);

        int redraw = 0;

        // update the progress bar animation, if active
        // update the spinning cube animation, even if no progress bar
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE ||
                gCurrentIcon == BACKGROUND_ICON_INSTALLING) {
            redraw = 1;
        }

        // move the progress bar forward on timed intervals, if configured
        int duration = gProgressScopeDuration;
        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
            if (duration > 0) {
                double elapsed = now() - gProgressScopeTime;
                float progress = 1.0 * elapsed / duration;
                if (progress > 1.0) progress = 1.0;
                if (progress > gProgress) {
                    gProgress = progress;
                    redraw = 1;
                }
            }
        }

        if (redraw) update_progress_locked();

        pthread_mutex_unlock(&gUpdateMutex);
        double end = now();
        // minimum of 20ms delay between frames
        double delay = interval - (end-start);
        if (delay < 0.02) delay = 0.02;
        usleep((long)(delay * 1000000));
    }
    return NULL;
}

static void handle_release(input_device *dev, struct input_event *ev) {
    if (dev->touch_pos.y < (gr_fb_height() - virtualkey_h)) {
        if(dev->slide_right == 1) {
            ev->type = EV_KEY;
            ev->code = KEY_ENTER;
            ev->value = 2;
            //vibrate(VIBRATOR_TIME_MS);
        } else if(dev->slide_left == 1) {
            ev->type = EV_KEY;
            ev->code = KEY_BACK;
            ev->value = 2;
            //vibrate(VIBRATOR_TIME_MS);
        }
    }
#ifdef USE_VIRTUAL_KEY
    else {
        ev->type = EV_KEY;
        ev->code=ui_get_virtualkey_pressed(dev);
        ev->value = 2;
        //vibrate(VIBRATOR_TIME_MS);
    }
    //clear button pressed down effect.
    if (virtualkey_pressed == 1 && ui_handle_key(ev->code, 1) == NO_ACTION) {
        pthread_mutex_lock(&gUpdateMutex);
        draw_virtualkeys_locked();
        gr_flip();
        pthread_mutex_unlock(&gUpdateMutex);
        virtualkey_pressed = 0;
    }
#endif
    reset_touch(dev);
}

static int rel_sum = 0;
static int input_callback(int fd, short revents, void *data) {
    struct input_event ev;
    int ret;
    int fake_key = 0;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;

    input_device dev;
    dev.fd = fd;
    if(!dev.touch_calibrated)
        dev.touch_calibrated = calibrate_touch(&dev);

    if (ev.type == EV_SYN) {
        if (ev.code == SYN_MT_REPORT) {
            dev.saw_mt_report = 1;
            if (!dev.saw_mt_tracking_id) {
                if (dev.saw_pos_x && dev.saw_pos_y) {
                    dev.saw_pos_x = 0;
                    dev.saw_pos_y = 0;
                } else
                    handle_release(&dev, &ev);
            }
        }
    } else if (ev.type == EV_REL) {
        if (ev.code == REL_Y) {
            // accumulate the up or down motion reported by
            // the trackball.  When it exceeds a threshold
            // (positive or negative), fake an up/down
            // key event.
            rel_sum += ev.value;
            if (rel_sum > 3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_DOWN;
                ev.value = 1;
                rel_sum = 0;
            } else if (rel_sum < -3) {
                fake_key = 1;
                ev.type = EV_KEY;
                ev.code = KEY_UP;
                ev.value = 1;
                rel_sum = 0;
            }
        }
    } else if (ev.type == EV_ABS) {
        switch(ev.code){
            case ABS_MT_SLOT:
                dev.slot_current = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                dev.saw_mt_tracking_id = 1;
                dev.tracking_id = ev.value;
                if (dev.tracking_id == -1 && dev.slot_current == 0)
                    handle_release(&dev, &ev);
                break;
            case ABS_MT_POSITION_X:
                dev.saw_pos_x = 1;
                if (dev.slot_current != 0) break;
                if(dev.touch_start.x == 0)
                    dev.touch_start.x = dev.touch_pos.x;
                float touch_rel = (float)ev.value / ((float)dev.touch_max.x - (float)dev.touch_min.x);
                dev.touch_pos.x = touch_rel * gr_fb_width();
                if (dev.touch_start.x == 0) break; //first touch.
                diff_x += dev.touch_pos.x - dev.touch_start.x;
                if (abs(diff_x) > abs(diff_y) && dev.touch_pos.y < (gr_fb_height() - virtualkey_h)) {
                    if(diff_x > min_x_swipe_px) {
                        dev.slide_right = 1;
                    } else if (diff_x < -min_x_swipe_px) {
                        dev.slide_left = 1;
                    }
                }
                break;
            case ABS_MT_POSITION_Y:
                dev.saw_pos_y = 1;
                if (dev.slot_current != 0) break;
                if(dev.touch_start.y == 0)
                    dev.touch_start.y = dev.touch_pos.y;
                touch_rel = (float)ev.value / ((float)dev.touch_max.y - (float)dev.touch_min.y);
                dev.touch_pos.y = touch_rel * gr_fb_height();
#ifdef USE_VIRTUAL_KEY
                ui_get_virtualkey_pressed(&dev);
#endif
                if (dev.touch_start.y == 0) break; //first touch.
                diff_y += dev.touch_pos.y - dev.touch_start.y;
                if (abs(diff_y) >= abs(diff_x) && dev.touch_pos.y < (gr_fb_height() - virtualkey_h)) {
                    if (diff_y > min_y_swipe_px) {
                        ev.type = EV_KEY;
                        ev.code = KEY_VOLUMEDOWN;
                        ev.value = 2;
                        reset_touch(&dev);
                    } else if (diff_y < -min_y_swipe_px) {
                        ev.type = EV_KEY;
                        ev.code = KEY_VOLUMEUP;
                        ev.value = 2;
                        reset_touch(&dev);
                    }
                }
                break;
            default:
                break;
        }
    } else if (ev.type == EV_KEY) {
        if (dev.saw_mt_report && dev.saw_mt_tracking_id && ev.code == BTN_TOUCH && ev.value == 0)
            handle_release(&dev, &ev);
    } else {
        rel_sum = 0;
    }

    if (ev.type != EV_KEY || ev.code > KEY_MAX)
        return 0;

    if (ev.value == 2) {
        boardEnableKeyRepeat = 0;
    }

    pthread_mutex_lock(&key_queue_mutex);
    if (!fake_key) {
        // our "fake" keys only report a key-down event (no
        // key-up), so don't record them in the key_pressed
        // table.
        key_pressed[ev.code] = ev.value;
    }
    const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
    if (ev.value > 0 && key_queue_len < queue_max) {
        key_queue[key_queue_len++] = ev.code;

        if (boardEnableKeyRepeat) {
            struct timeval now;
            gettimeofday(&now, NULL);

            key_press_time[ev.code] = (now.tv_sec * 1000) + (now.tv_usec / 1000);
            key_last_repeat[ev.code] = 0;
        }

        pthread_cond_signal(&key_queue_cond);
    }
    pthread_mutex_unlock(&key_queue_mutex);

    if (ev.value > 0 && device_reboot_now(key_pressed, ev.code)) {
        reboot_main_system(ANDROID_RB_RESTART, 0, 0);
    }

    return 0;
}

// Reads input events, handles special hot keys, and adds to the key queue.
static void *input_thread(void *cookie) {
    for (;;) {
        if (!ev_wait(-1))
            ev_dispatch();
    }
    return NULL;
}

void ui_init(void) {
    ui_has_initialized = 1;
    gr_init();
    set_min_swipe_lengths();
#ifdef USE_VIRTUAL_KEY
    ui_get_virtualkey_size();
#endif
    ev_init(input_callback, NULL);

    text_col = text_row = 0;
    text_rows = (gr_fb_height() - virtualkey_h) / CHAR_HEIGHT;
    max_menu_rows = text_rows - MIN_LOG_ROWS;
    if (max_menu_rows > MENU_MAX_ROWS)
        max_menu_rows = MENU_MAX_ROWS;
    if (text_rows > MAX_ROWS) text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / CHAR_WIDTH;
    if (text_cols > MAX_COLS - 1) text_cols = MAX_COLS - 1;

    int i;
    for (i = 0; BITMAPS[i].name != NULL; ++i) {
        int result = res_create_surface(BITMAPS[i].name, BITMAPS[i].surface);
        if (result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
        }
    }

    gProgressBarIndeterminate = malloc(ui_parameters.indeterminate_frames *
                                       sizeof(gr_surface));
    for (i = 0; i < ui_parameters.indeterminate_frames; ++i) {
        char filename[40];
        // "indeterminate01.png", "indeterminate02.png", ...
        sprintf(filename, "indeterminate%02d", i+1);
        int result = res_create_surface(filename, gProgressBarIndeterminate+i);
        if (result < 0) {
            LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
        }
    }

    if (ui_parameters.installing_frames > 0) {
        gInstallationOverlay = malloc(ui_parameters.installing_frames *
                                      sizeof(gr_surface));
        for (i = 0; i < ui_parameters.installing_frames; ++i) {
            char filename[40];
            // "icon_installing_overlay01.png",
            // "icon_installing_overlay02.png", ...
            sprintf(filename, "icon_installing_overlay%02d", i+1);
            int result = res_create_surface(filename, gInstallationOverlay+i);
            if (result < 0) {
                LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
            }
        }

        // Adjust the offset to account for the positioning of the
        // base image on the screen.
        if (gBackgroundIcon[BACKGROUND_ICON_INSTALLING] != NULL) {
            gr_surface bg = gBackgroundIcon[BACKGROUND_ICON_INSTALLING];
            ui_parameters.install_overlay_offset_x +=
                (gr_fb_width() - gr_get_width(bg)) / 2;
            ui_parameters.install_overlay_offset_y +=
                (gr_fb_height() - gr_get_height(bg)) / 2;
        }
    } else {
        gInstallationOverlay = NULL;
    }

    char enable_key_repeat[PROPERTY_VALUE_MAX];
    property_get("ro.cwm.enable_key_repeat", enable_key_repeat, "");
    if (!strcmp(enable_key_repeat, "true") || !strcmp(enable_key_repeat, "1")) {
        boardEnableKeyRepeat = 1;

        char key_list[PROPERTY_VALUE_MAX];
        property_get("ro.cwm.repeatable_keys", key_list, "");
        if (strlen(key_list) == 0) {
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_UP;
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_DOWN;
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_VOLUMEUP;
            boardRepeatableKeys[boardNumRepeatableKeys++] = KEY_VOLUMEDOWN;
        } else {
            char *pch = strtok(key_list, ",");
            while (pch != NULL) {
                boardRepeatableKeys[boardNumRepeatableKeys++] = atoi(pch);
                pch = strtok(NULL, ",");
            }
        }
    }

    pthread_t t;
    pthread_create(&t, NULL, progress_thread, NULL);
    pthread_create(&t, NULL, input_thread, NULL);
}

char *ui_copy_image(int icon, int *width, int *height, int *bpp) {
    pthread_mutex_lock(&gUpdateMutex);
    draw_background_locked(icon);
    *width = gr_fb_width();
    *height = gr_fb_height();
    *bpp = sizeof(gr_pixel) * 8;
    int size = *width * *height * sizeof(gr_pixel);
    char *ret = malloc(size);
    if (ret == NULL) {
        LOGE("Can't allocate %d bytes for image\n", size);
    } else {
        memcpy(ret, gr_fb_data(), size);
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return ret;
}

void ui_set_background(int icon) {
    pthread_mutex_lock(&gUpdateMutex);
    gCurrentIcon = icon;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_indeterminate_progress() {
    if (!ui_has_initialized)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE) {
        gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
        update_progress_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_progress(float portion, int seconds) {
    if (!ui_has_initialized)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
    gProgressScopeStart += gProgressScopeSize;
    gProgressScopeSize = portion;
    gProgressScopeTime = now();
    gProgressScopeDuration = seconds;
    gProgress = 0;
    update_progress_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_set_progress(float fraction) {
    if (!ui_has_initialized)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && fraction > gProgress) {
        // Skip updates that aren't visibly different.
        int width = gr_get_width(gProgressBarIndeterminate[0]);
        float scale = width * gProgressScopeSize;
        if ((int) (gProgress * scale) != (int) (fraction * scale)) {
            gProgress = fraction;
            update_progress_locked();
        }
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_reset_progress() {
    if (!ui_has_initialized)
        return;

    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NONE;
    gProgressScopeStart = 0;
    gProgressScopeSize = 0;
    gProgressScopeTime = 0;
    gProgressScopeDuration = 0;
    gProgress = 0;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_get_text_cols() {
    return text_cols;
}

void ui_print(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 256, fmt, ap);
    va_end(ap);

    if (ui_log_stdout)
        fputs(buf, stdout);

    // This can get called before ui_init(), so be careful.
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        char *ptr;
#ifdef USE_CHINESE_FONT
        int fwidth = 0, fwidth_sum = 0;
#endif
        for (ptr = buf; *ptr != '\0'; ++ptr) {
#ifdef USE_CHINESE_FONT
            fwidth = gr_measure(&*ptr);
            //LOGI("%d \n", fwidth);
            fwidth_sum += fwidth;

            if (*ptr == '\n' || fwidth_sum >= gr_fb_width()) {
                fwidth_sum = 0;
#else
            if (*ptr == '\n' || text_col >= text_cols) {
#endif
                text[text_row][text_col] = '\0';
                text_col = 0;
                text_row = (text_row + 1) % text_rows;
                if (text_row == text_top) text_top = (text_top + 1) % text_rows;
            }
            if (*ptr != '\n') text[text_row][text_col++] = *ptr;
        }
        text[text_row][text_col] = '\0';
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_printlogtail(int nb_lines) {
    char * log_data;
    char tmp[PATH_MAX];
    FILE * f;
    int line=0;
    //don't log output to recovery.log
    ui_log_stdout=0;
    sprintf(tmp, "tail -n %d /tmp/recovery.log > /tmp/tail.log", nb_lines);
    __system(tmp);
    f = fopen("/tmp/tail.log", "rb");
    if (f != NULL) {
        while (line < nb_lines) {
            log_data = fgets(tmp, PATH_MAX, f);
            if (log_data == NULL) break;
            ui_print("%s", tmp);
            line++;
        }
        fclose(f);
    }
    ui_print("Return to menu with any key.\n");
    ui_log_stdout=1;
}

int ui_start_menu(const char** headers, char** items, int initial_selection) {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        for (i = 0; i < text_rows; ++i) {
            if (headers[i] == NULL) break;
#ifndef USE_CHINESE_FONT
            strncpy(menu[i], headers[i], text_cols-1);
            menu[i][text_cols-1] = '\0';
#else
            int j = 0, fwidth = 0, fwidth_sum = 0;
            for(j=0; headers[i][j] != '\0' && fwidth_sum < gr_fb_width(); j++) {
                fwidth = gr_measure(&headers[i][j]);
                fwidth_sum += fwidth;
                //if (j == sizeof(menu[i])) break;
            }
            strncpy(menu[i], headers[i], j);
            menu[i][j] = '\0';
            //LOGI("%d %s\n", j, menu[i]);
#endif
        }
        menu_top = i;
        for (; i < MENU_MAX_ROWS; ++i) {
            if (items[i-menu_top] == NULL) break;
            strcpy(menu[i], MENU_ITEM_HEADER);
            strncpy(menu[i] + MENU_ITEM_HEADER_LENGTH, items[i-menu_top], MENU_MAX_COLS - 1 - MENU_ITEM_HEADER_LENGTH);
            menu[i][MENU_MAX_COLS-1] = '\0';
        }

        if (gShowBackButton && !ui_root_menu) {
#ifdef USE_CHINESE_FONT
            strcpy(menu[i], " < 返回");
#else
            strcpy(menu[i], " < Go Back");
#endif
            ++i;
        }

        menu_items = i - menu_top;
        show_menu = 1;
        menu_sel = menu_show_start = initial_selection;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    if (gShowBackButton && !ui_root_menu) {
        return menu_items - 1;
    }
    return menu_items;
}

int ui_menu_select(int sel) {
    int old_sel;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0) {
        old_sel = menu_sel;
        menu_sel = sel;

        if (menu_sel < 0) menu_sel = menu_items + menu_sel;
        if (menu_sel >= menu_items) menu_sel = menu_sel - menu_items;


        if (menu_sel < menu_show_start && menu_show_start > 0) {
            menu_show_start = menu_sel;
        }

        if (menu_sel - menu_show_start + menu_top >= max_menu_rows) {
            menu_show_start = menu_sel + menu_top - max_menu_rows + 1;
        }

        sel = menu_sel;

        if (menu_sel != old_sel) update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
    return sel;
}

void ui_end_menu() {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0 && text_rows > 0 && text_cols > 0) {
        show_menu = 0;
        update_screen_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_text_visible() {
    pthread_mutex_lock(&gUpdateMutex);
    int visible = show_text;
    pthread_mutex_unlock(&gUpdateMutex);
    return visible;
}

int ui_text_ever_visible() {
    pthread_mutex_lock(&gUpdateMutex);
    int ever_visible = show_text_ever;
    pthread_mutex_unlock(&gUpdateMutex);
    return ever_visible;
}

void ui_show_text(int visible) {
    pthread_mutex_lock(&gUpdateMutex);
    show_text = visible;
    if (show_text) show_text_ever = 1;
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

static int usb_connected() {
    int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
    if (fd < 0) {
        printf("failed to open /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
        return 0;
    }

    char buf;
    /* USB is connected if android_usb state is CONNECTED or CONFIGURED */
    int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
    if (close(fd) < 0) {
        printf("failed to close /sys/class/android_usb/android0/state: %s\n",
               strerror(errno));
    }
    return connected;
}

void ui_cancel_wait_key() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue[key_queue_len] = -2;
    key_queue_len++;
    pthread_cond_signal(&key_queue_cond);
    pthread_mutex_unlock(&key_queue_mutex);
}

int ui_wait_key() {
    if (boardEnableKeyRepeat)
        return ui_wait_key_with_repeat();

    pthread_mutex_lock(&key_queue_mutex);
    int timeouts = UI_WAIT_KEY_TIMEOUT_SEC;

    // Time out after REFRESH_TIME_USB_INTERVAL seconds to catch volume changes, and loop for
    // UI_WAIT_KEY_TIMEOUT_SEC to restart a device not connected to USB
    do {
        struct timeval now;
        struct timespec timeout;
        gettimeofday(&now, NULL);
        timeout.tv_sec = now.tv_sec;
        timeout.tv_nsec = now.tv_usec * 1000;
        timeout.tv_sec += REFRESH_TIME_USB_INTERVAL;

        int rc = 0;
        while (key_queue_len == 0 && rc != ETIMEDOUT) {
            rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                        &timeout);
            if (volumes_changed()) {
                pthread_mutex_unlock(&key_queue_mutex);
                return REFRESH;
            }
        }
        timeouts -= REFRESH_TIME_USB_INTERVAL;
    } while ((timeouts > 0 || usb_connected()) && key_queue_len == 0);

    int key = -1;
    if (key_queue_len > 0) {
        key = key_queue[0];
        memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
    }
    pthread_mutex_unlock(&key_queue_mutex);
    return key;
}

static int key_can_repeat(int key) {
    int k = 0;
    for (;k < boardNumRepeatableKeys; ++k) {
        if (boardRepeatableKeys[k] == key) {
            break;
        }
    }
    if (k < boardNumRepeatableKeys) return 1;
    return 0;
}

static int ui_wait_key_with_repeat() {
    int key = -1;

    // Loop to wait for more keys
    do {
        int timeouts = UI_WAIT_KEY_TIMEOUT_SEC;
        int rc = 0;
        struct timeval now;
        struct timespec timeout;
        pthread_mutex_lock(&key_queue_mutex);
        while (key_queue_len == 0 && timeouts > 0) {
            gettimeofday(&now, NULL);
            timeout.tv_sec = now.tv_sec;
            timeout.tv_nsec = now.tv_usec * 1000;
            timeout.tv_sec += REFRESH_TIME_USB_INTERVAL;

            rc = 0;
            while (key_queue_len == 0 && rc != ETIMEDOUT) {
                rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex,
                                            &timeout);
                if (volumes_changed()) {
                    pthread_mutex_unlock(&key_queue_mutex);
                    return REFRESH;
                }
            }
            timeouts -= REFRESH_TIME_USB_INTERVAL;
        }
        pthread_mutex_unlock(&key_queue_mutex);

        if (rc == ETIMEDOUT && !usb_connected()) {
            return -1;
        }

        // Loop to wait wait for more keys, or repeated keys to be ready.
        while (1) {
            unsigned long now_msec;

            gettimeofday(&now, NULL);
            now_msec = (now.tv_sec * 1000) + (now.tv_usec / 1000);

            pthread_mutex_lock(&key_queue_mutex);

            // Replacement for the while conditional, so we don't have to lock the entire
            // loop, because that prevents the input system from touching the variables while
            // the loop is running which causes problems.
            if (key_queue_len == 0) {
                pthread_mutex_unlock(&key_queue_mutex);
                break;
            }

            key = key_queue[0];
            memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);

            // sanity check the returned key.
            if (key < 0) {
                pthread_mutex_unlock(&key_queue_mutex);
                return key;
            }

            // Check for already released keys and drop them if they've repeated.
            if (!key_pressed[key] && key_last_repeat[key] > 0) {
                pthread_mutex_unlock(&key_queue_mutex);
                continue;
            }

            if (key_can_repeat(key)) {
                // Re-add the key if a repeat is expected, since we just popped it. The
                // if below will determine when the key is actually repeated (returned)
                // in the mean time, the key will be passed through the queue over and
                // over and re-evaluated each time.
                if (key_pressed[key]) {
                    key_queue[key_queue_len] = key;
                    key_queue_len++;
                }
                if ((now_msec > key_press_time[key] + UI_KEY_WAIT_REPEAT && now_msec > key_last_repeat[key] + UI_KEY_REPEAT_INTERVAL) ||
                        key_last_repeat[key] == 0) {
                    key_last_repeat[key] = now_msec;
                } else {
                    // Not ready
                    pthread_mutex_unlock(&key_queue_mutex);
                    continue;
                }
            }
            pthread_mutex_unlock(&key_queue_mutex);
            return key;
        }
    } while (1);

    return key;
}

int ui_key_pressed(int key) {
    // This is a volatile static array, don't bother locking
    return key_pressed[key];
}

void ui_clear_key_queue() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

void ui_set_log_stdout(int enabled) {
    ui_log_stdout = enabled;
}

int ui_should_log_stdout() {
    return ui_log_stdout;
}

void ui_set_show_text(int value) {
    show_text = value;
}

void ui_set_showing_back_button(int showBackButton) {
    gShowBackButton = showBackButton;
}

int ui_is_showing_back_button() {
    return gShowBackButton && !ui_root_menu;
}

int ui_get_selected_item() {
  return menu_sel;
}

int ui_handle_key(int key, int visible) {
    return device_handle_key(key, visible);
}

void ui_delete_line() {
    pthread_mutex_lock(&gUpdateMutex);
    text[text_row][0] = '\0';
    text_row = (text_row - 1 + text_rows) % text_rows;
    text_col = 0;
    pthread_mutex_unlock(&gUpdateMutex);
}

static void ui_rainbow_mode() {
    static int colors[] = { 255, 0, 0,        // red
                            255, 127, 0,      // orange
                            255, 255, 0,      // yellow
                            0, 255, 0,        // green
                            60, 80, 255,      // blue
                            143, 0, 255 };    // violet

    gr_color(colors[cur_rainbow_color], colors[cur_rainbow_color+1], colors[cur_rainbow_color+2], 255);
    cur_rainbow_color += 3;
    if (cur_rainbow_color >= (sizeof(colors) / sizeof(colors[0]))) cur_rainbow_color = 0;
}

int ui_get_rainbow_mode() {
    return gRainbowMode;
}

void ui_set_rainbow_mode(int rainbowMode) {
    gRainbowMode = rainbowMode;

    pthread_mutex_lock(&gUpdateMutex);
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

int is_ui_initialized() {
    return ui_has_initialized;
}
