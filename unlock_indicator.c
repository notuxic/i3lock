/*
 * vim:ts=4:sw=4:expandtab
 *
 * © 2010 Michael Stapelberg
 *
 * See LICENSE for licensing information
 *
 */
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <xcb/xcb.h>
#include <ev.h>
#include <cairo.h>
#include <cairo/cairo-xcb.h>

#include "i3lock.h"
#include "xcb.h"
#include "unlock_indicator.h"
#include "randr.h"
#include "dpi.h"

#define BUTTON_RADIUS 90
#define BUTTON_SPACE (BUTTON_RADIUS + 5)
#define BUTTON_CENTER (BUTTON_RADIUS + 5)
#define BUTTON_DIAMETER (2 * BUTTON_SPACE)

/*******************************************************************************
 * Variables defined in i3lock.c.
 ******************************************************************************/

extern bool debug_mode;

/* The current position in the input buffer. Useful to determine if any
 * characters of the password have already been entered or not. */
extern int input_position;

/* The lock window. */
extern xcb_window_t win;

/* The current resolution of the X11 root window. */
extern uint32_t last_resolution[2];

/* Whether the unlock indicator is enabled (defaults to true). */
extern bool unlock_indicator;

/* List of pressed modifiers, or NULL if none are pressed. */
extern char *modifier_string;

/* A Cairo surface containing the specified image (-i), if any. */
extern cairo_surface_t *img;

/* Whether the image should be tiled. */
extern bool tile;
/* The colors to use (as rgb(a)). */
extern rgba_t background_color;
extern rgba_t normal_font_color;
extern rgba_t normal_ring_color;
extern rgba_t normal_interior_color;
extern rgba_t normal_border_color;
extern rgba_t verify_font_color;
extern rgba_t verify_ring_color;
extern rgba_t verify_interior_color;
extern rgba_t verify_border_color;
extern rgba_t fail_font_color;
extern rgba_t fail_ring_color;
extern rgba_t fail_interior_color;
extern rgba_t fail_border_color;
extern rgba_t attempt_font_color;
extern rgba_t keypress_color;
extern rgba_t backspace_color;
extern char font_family[64];
extern cairo_font_slant_t font_slant;
extern cairo_font_weight_t font_weight;
extern float font_size;

/* Whether the failed attempts should be displayed. */
extern bool show_failed_attempts;
/* Number of failed unlock attempts. */
extern int failed_attempts;

/*******************************************************************************
 * Variables defined in xcb.c.
 ******************************************************************************/

/* The root screen, to determine the DPI. */
extern xcb_screen_t *screen;

/*******************************************************************************
 * Local variables.
 ******************************************************************************/

/* Cache the screen’s visual, necessary for creating a Cairo context. */
static xcb_visualtype_t *vistype;

/* Maintain the current unlock/PAM state to draw the appropriate unlock
 * indicator. */
unlock_state_t unlock_state;
auth_state_t auth_state;

/*
 * Draws global image with fill color onto a pixmap with the given
 * resolution and returns it.
 *
 */
void draw_image(xcb_pixmap_t bg_pixmap, uint32_t *resolution) {
    const double scaling_factor = get_dpi_value() / 96.0;
    int button_diameter_physical = ceil(scaling_factor * BUTTON_DIAMETER);
    DEBUG("scaling_factor is %.f, physical diameter is %d px\n",
          scaling_factor, button_diameter_physical);

    if (!vistype)
        vistype = get_root_visual_type(screen);

    /* Initialize cairo: Create one in-memory surface to render the unlock
     * indicator on, create one XCB surface to actually draw (one or more,
     * depending on the amount of screens) unlock indicators on. */
    cairo_surface_t *output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, button_diameter_physical, button_diameter_physical);
    cairo_t *ctx = cairo_create(output);

    cairo_surface_t *xcb_output = cairo_xcb_surface_create(conn, bg_pixmap, vistype, resolution[0], resolution[1]);
    cairo_t *xcb_ctx = cairo_create(xcb_output);

    cairo_font_face_t *font_face = cairo_toy_font_face_create(font_family, font_slant, font_weight);

    /* After the first iteration, the pixmap will still contain the previous
     * contents. Explicitly clear the entire pixmap with the background color
     * first to get back into a defined state: */
    cairo_set_source_rgb(xcb_ctx, background_color.red / 255.0, background_color.green / 255.0, background_color.blue / 255.0);
    cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
    cairo_fill(xcb_ctx);

    if (img) {
        if (!tile) {
            cairo_set_source_surface(xcb_ctx, img, 0, 0);
            cairo_paint(xcb_ctx);
        } else {
            /* create a pattern and fill a rectangle as big as the screen */
            cairo_pattern_t *pattern;
            pattern = cairo_pattern_create_for_surface(img);
            cairo_set_source(xcb_ctx, pattern);
            cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
            cairo_rectangle(xcb_ctx, 0, 0, resolution[0], resolution[1]);
            cairo_fill(xcb_ctx);
            cairo_pattern_destroy(pattern);
        }
    }

    if (unlock_indicator &&
        (unlock_state >= STATE_KEY_PRESSED || auth_state > STATE_AUTH_IDLE)) {
        cairo_scale(ctx, scaling_factor, scaling_factor);
        /* Draw a (centered) circle with transparent background. */
        cairo_set_line_width(ctx, 10.0);
        cairo_arc(ctx,
                  BUTTON_CENTER /* x */,
                  BUTTON_CENTER /* y */,
                  BUTTON_RADIUS /* radius */,
                  0 /* start */,
                  2 * M_PI /* end */);

        /* Use the appropriate color for the different PAM states
         * (currently verifying, wrong password, or default) */
        switch (auth_state) {
            case STATE_AUTH_VERIFY:
            case STATE_AUTH_LOCK:
                cairo_set_source_rgba(ctx, verify_interior_color.red / 255.0, verify_interior_color.green / 255.0,
                                      verify_interior_color.blue / 255.0, verify_interior_color.alpha);
                break;
            case STATE_AUTH_WRONG:
            case STATE_I3LOCK_LOCK_FAILED:
                cairo_set_source_rgba(ctx, fail_interior_color.red / 255.0, fail_interior_color.green / 255.0,
                                      fail_interior_color.blue / 255.0, fail_interior_color.alpha);
                break;
            default:
                if (unlock_state == STATE_NOTHING_TO_DELETE) {
                    cairo_set_source_rgba(ctx, fail_interior_color.red / 255.0, fail_interior_color.green / 255.0,
                                          fail_interior_color.blue / 255.0, fail_interior_color.alpha);
                    break;
                }
                cairo_set_source_rgba(ctx, normal_interior_color.red / 255.0, normal_interior_color.green / 255.0,
                                      normal_interior_color.blue / 255.0, normal_interior_color.alpha);
                break;
        }
        cairo_fill_preserve(ctx);

        switch (auth_state) {
            case STATE_AUTH_VERIFY:
            case STATE_AUTH_LOCK:
                cairo_set_source_rgba(ctx, verify_ring_color.red / 255.0, verify_ring_color.green / 255.0,
                                      verify_ring_color.blue / 255.0, verify_ring_color.alpha);
                break;
            case STATE_AUTH_WRONG:
            case STATE_I3LOCK_LOCK_FAILED:
                cairo_set_source_rgba(ctx, fail_ring_color.red / 255.0, fail_ring_color.green / 255.0,
                                      fail_ring_color.blue / 255.0, fail_ring_color.alpha);
                break;
            case STATE_AUTH_IDLE:
                if (unlock_state == STATE_NOTHING_TO_DELETE) {
                    cairo_set_source_rgba(ctx, fail_ring_color.red / 255.0, fail_ring_color.green / 255.0,
                                          fail_ring_color.blue / 255.0, fail_ring_color.alpha);
                    break;
                }

                cairo_set_source_rgba(ctx, normal_ring_color.red / 255.0, normal_ring_color.green / 255.0,
                                      normal_ring_color.blue / 255.0, normal_ring_color.alpha);
                break;
        }
        cairo_stroke(ctx);

        /* Draw an inner seperator line. */
        cairo_set_line_width(ctx, 2.0);
        cairo_arc(ctx,
                  BUTTON_CENTER /* x */,
                  BUTTON_CENTER /* y */,
                  BUTTON_RADIUS - 5 /* radius */,
                  0,
                  2 * M_PI);

        switch (auth_state) {
            case STATE_AUTH_VERIFY:
            case STATE_AUTH_LOCK:
                cairo_set_source_rgba(ctx, verify_border_color.red / 255.0, verify_border_color.green / 255.0,
                                      verify_border_color.blue / 255.0, verify_border_color.alpha);
                break;
            case STATE_AUTH_WRONG:
            case STATE_I3LOCK_LOCK_FAILED:
                cairo_set_source_rgba(ctx, fail_border_color.red / 255.0, fail_border_color.green / 255.0,
                                      fail_border_color.blue / 255.0, fail_border_color.alpha);
                break;
            case STATE_AUTH_IDLE:
                if (unlock_state == STATE_NOTHING_TO_DELETE) {
                    cairo_set_source_rgba(ctx, fail_border_color.red / 255.0, fail_border_color.green / 255.0,
                                          fail_border_color.blue / 255.0, fail_border_color.alpha);
                    break;
                }

                cairo_set_source_rgba(ctx, normal_border_color.red / 255.0, normal_border_color.green / 255.0,
                                      normal_border_color.blue / 255.0, normal_border_color.alpha);
                break;
        }
        cairo_stroke(ctx);

        cairo_set_line_width(ctx, 10.0);

        /* Display a (centered) text of the current PAM state. */
        char *text = NULL;
        /* We don't want to show more than a 3-digit number. */
        char buf[4];

        cairo_set_font_face(ctx, font_face);
        cairo_set_font_size(ctx, font_size);
        switch (auth_state) {
            case STATE_AUTH_VERIFY:
                cairo_set_source_rgba(ctx, verify_font_color.red / 255.0, verify_font_color.green / 255.0,
                                      verify_font_color.blue / 255.0, verify_font_color.alpha);
                text = "Verifying…";
                break;
            case STATE_AUTH_LOCK:
                cairo_set_source_rgba(ctx, verify_font_color.red / 255.0, verify_font_color.green / 255.0,
                                      verify_font_color.blue / 255.0, verify_font_color.alpha);
                text = "Locking…";
                break;
            case STATE_AUTH_WRONG:
                cairo_set_source_rgba(ctx, fail_font_color.red / 255.0, fail_font_color.green / 255.0,
                                      fail_font_color.blue / 255.0, fail_font_color.alpha);
                text = "Wrong!";
                break;
            case STATE_I3LOCK_LOCK_FAILED:
                cairo_set_source_rgba(ctx, fail_font_color.red / 255.0, fail_font_color.green / 255.0,
                                      fail_font_color.blue / 255.0, fail_font_color.alpha);
                text = "Lock failed!";
                break;
            default:
                cairo_set_source_rgba(ctx, normal_font_color.red / 255.0, normal_font_color.green / 255.0,
                                      normal_font_color.blue / 255.0, normal_font_color.alpha);
                if (unlock_state == STATE_NOTHING_TO_DELETE) {
                    cairo_set_source_rgba(ctx, fail_font_color.red / 255.0, fail_font_color.green / 255.0,
                                          fail_font_color.blue / 255.0, fail_font_color.alpha);
                    text = "No input";
                }
                if (show_failed_attempts && failed_attempts > 0) {
                    if (failed_attempts > 999) {
                        text = "> 999";
                    } else {
                        snprintf(buf, sizeof(buf), "%d", failed_attempts);
                        text = buf;
                    }
                    cairo_set_source_rgba(ctx, attempt_font_color.red / 255.0, attempt_font_color.green / 255.0,
                                          attempt_font_color.blue / 255.0, attempt_font_color.alpha);
                    cairo_set_font_size(ctx, font_size * 1.15);
                }
                break;
        }

        if (text) {
            cairo_text_extents_t extents;
            double x, y;

            cairo_text_extents(ctx, text, &extents);
            x = BUTTON_CENTER - ((extents.width / 2) + extents.x_bearing);
            y = BUTTON_CENTER - ((extents.height / 2) + extents.y_bearing);

            cairo_move_to(ctx, x, y);
            cairo_show_text(ctx, text);
            cairo_close_path(ctx);
        }

        if (auth_state == STATE_AUTH_WRONG && (modifier_string != NULL)) {
            cairo_text_extents_t extents;
            double x, y;

            cairo_set_font_size(ctx, font_size / 2.0);

            cairo_text_extents(ctx, modifier_string, &extents);
            x = BUTTON_CENTER - ((extents.width / 2) + extents.x_bearing);
            y = BUTTON_CENTER - ((extents.height / 2) + extents.y_bearing) + 28.0;

            cairo_move_to(ctx, x, y);
            cairo_show_text(ctx, modifier_string);
            cairo_close_path(ctx);
        }

        /* After the user pressed any valid key or the backspace key, we
         * highlight a random part of the unlock indicator to confirm this
         * keypress. */
        if (unlock_state == STATE_KEY_ACTIVE ||
            unlock_state == STATE_BACKSPACE_ACTIVE) {
            cairo_new_sub_path(ctx);
            double highlight_start = (rand() % (int)(2 * M_PI * 100)) / 100.0;
            cairo_arc(ctx,
                      BUTTON_CENTER /* x */,
                      BUTTON_CENTER /* y */,
                      BUTTON_RADIUS /* radius */,
                      highlight_start,
                      highlight_start + (M_PI / 3.0));
            if (unlock_state == STATE_KEY_ACTIVE) {
                /* For normal keys, we use a lighter green. */
                cairo_set_source_rgba(ctx, keypress_color.red / 255.0, keypress_color.green / 255.0,
                                      keypress_color.blue / 255.0, keypress_color.alpha);
            } else {
                /* For backspace, we use red. */
                cairo_set_source_rgba(ctx, backspace_color.red / 255.0, backspace_color.green / 255.0,
                                      backspace_color.blue / 255.0, backspace_color.alpha);
            }
            cairo_stroke(ctx);

            /* Draw two little separators for the highlighted part of the
             * unlock indicator. */
            cairo_set_source_rgba(ctx, normal_border_color.red / 255.0, normal_border_color.green / 255.0,
                                  normal_border_color.blue / 255.0, normal_border_color.alpha);
            cairo_arc(ctx,
                      BUTTON_CENTER /* x */,
                      BUTTON_CENTER /* y */,
                      BUTTON_RADIUS /* radius */,
                      highlight_start /* start */,
                      highlight_start + (M_PI / 128.0) /* end */);
            cairo_stroke(ctx);
            cairo_arc(ctx,
                      BUTTON_CENTER /* x */,
                      BUTTON_CENTER /* y */,
                      BUTTON_RADIUS /* radius */,
                      (highlight_start + (M_PI / 3.0)) - (M_PI / 128.0) /* start */,
                      highlight_start + (M_PI / 3.0) /* end */);
            cairo_stroke(ctx);
        }
    }

    if (xr_screens > 0) {
        /* Composite the unlock indicator in the middle of each screen. */
        for (int screen = 0; screen < xr_screens; screen++) {
            int x = (xr_resolutions[screen].x + ((xr_resolutions[screen].width / 2) - (button_diameter_physical / 2)));
            int y = (xr_resolutions[screen].y + ((xr_resolutions[screen].height / 2) - (button_diameter_physical / 2)));
            cairo_set_source_surface(xcb_ctx, output, x, y);
            cairo_rectangle(xcb_ctx, x, y, button_diameter_physical, button_diameter_physical);
            cairo_fill(xcb_ctx);
        }
    } else {
        /* We have no information about the screen sizes/positions, so we just
         * place the unlock indicator in the middle of the X root window and
         * hope for the best. */
        int x = (last_resolution[0] / 2) - (button_diameter_physical / 2);
        int y = (last_resolution[1] / 2) - (button_diameter_physical / 2);
        cairo_set_source_surface(xcb_ctx, output, x, y);
        cairo_rectangle(xcb_ctx, x, y, button_diameter_physical, button_diameter_physical);
        cairo_fill(xcb_ctx);
    }

    cairo_font_face_destroy(font_face);
    cairo_surface_destroy(xcb_output);
    cairo_surface_destroy(output);
    cairo_destroy(ctx);
    cairo_destroy(xcb_ctx);
}

static xcb_pixmap_t bg_pixmap = XCB_NONE;

/*
 * Releases the current background pixmap so that the next redraw_screen() call
 * will allocate a new one with the updated resolution.
 *
 */
void free_bg_pixmap(void) {
    xcb_free_pixmap(conn, bg_pixmap);
    bg_pixmap = XCB_NONE;
}

/*
 * Calls draw_image on a new pixmap and swaps that with the current pixmap
 *
 */
void redraw_screen(void) {
    DEBUG("redraw_screen(unlock_state = %d, auth_state = %d)\n", unlock_state, auth_state);
    if (bg_pixmap == XCB_NONE) {
        DEBUG("allocating pixmap for %d x %d px\n", last_resolution[0], last_resolution[1]);
        bg_pixmap = create_bg_pixmap(conn, screen, last_resolution, background_color);
    }

    draw_image(bg_pixmap, last_resolution);
    xcb_change_window_attributes(conn, win, XCB_CW_BACK_PIXMAP, (uint32_t[1]){bg_pixmap});
    /* XXX: Possible optimization: Only update the area in the middle of the
     * screen instead of the whole screen. */
    xcb_clear_area(conn, 0, win, 0, 0, last_resolution[0], last_resolution[1]);
    xcb_flush(conn);
}

/*
 * Hides the unlock indicator completely when there is no content in the
 * password buffer.
 *
 */
void clear_indicator(void) {
    if (input_position == 0) {
        unlock_state = STATE_STARTED;
    } else
        unlock_state = STATE_KEY_PRESSED;
    redraw_screen();
}
