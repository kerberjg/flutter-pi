// SPDX-License-Identifier: MIT
/*
 * window object
 *
 * - a window is something where flutter graphics can be presented on.
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */

#define _GNU_SOURCE
#include "window.h"

#include <stdlib.h>

#include <pthread.h>

#include <flutter_embedder.h>

#include "compositor_ng.h"
#include "cursor.h"
#include "egl_gbm_render_surface.h"
#include "flutter-pi.h"
#include "frame_scheduler.h"
#include "gl_renderer.h"
#include "modesetting.h"
#include "render_surface.h"
#include "surface.h"
#include "tracer.h"
#include "util/collection.h"
#include "vk_gbm_render_surface.h"
#include "vk_renderer.h"

FILE_DESCR("native windows")

struct window {
    pthread_mutex_t lock;
    refcount_t n_refs;

    /**
     * @brief Event tracing interface.
     *
     * Used to report timing information to the dart observatory.
     *
     */
    struct tracer *tracer;

    /**
     * @brief Manages the frame scheduling for this window.
     *
     */
    struct frame_scheduler *frame_scheduler;

    /**
     * @brief Refresh rate of the selected video mode / display.
     *
     */
    double refresh_rate;

    /**
     * @brief Flutter device pixel ratio (in the horizontal axis). Number of physical pixels per logical pixel.
     *
     * There are always 38 logical pixels per cm, or 96 per inch. This is roughly equivalent to DPI / 100.
     * A device pixel ratio of 1.0 is roughly a dpi of 96, which is the most common dpi for full-hd desktop displays.
     * To calculate this, the physical dimensions of the display are required. If there are no physical dimensions,
     * this will default to 1.0.
     */
    double pixel_ratio;

    /**
     * @brief Whether we have physical screen dimensions and @ref width_mm and @ref height_mm contain usable values.
     *
     */
    bool has_dimensions;

    /**
     * @brief Width, height of the display in millimeters.
     *
     */
    int width_mm, height_mm;

    /**
     * @brief The size of the view, as reported to flutter, in pixels.
     *
     * If no rotation and scaling is applied, this probably equals the display size.
     * For example, if flutter-pi should render at 1/2 the resolution of a full-hd display, this would be
     * 960x540 and the display size 1920x1080.
     */
    struct vec2f view_size;

    /**
     * @brief The actual size of the view on the display, pixels.
     *
     */
    struct vec2f display_size;

    /**
     * @brief The rotation we should apply to the flutter layers to present them on screen.
     */
    drm_plane_transform_t rotation;

    /**
     * @brief The current device orientation and the original (startup) device orientation.
     *
     * @ref original_orientation is kLandscapeLeft for displays that are more wide than high, and kPortraitUp for displays that are
     * more high than wide. Though this can also be anything else theoretically, if the user specifies weird combinations of rotation
     * and orientation via cmdline arguments.
     *
     * @ref orientation should always equal to rotating @ref original_orientation clock-wise by the angle in the @ref rotation field.
     */
    enum device_orientation orientation, original_orientation;

    /**
     * @brief Matrix for transforming display coordinates to view coordinates.
     *
     * For example for transforming pointer events (which are in the display coordinate space) to flutter coordinates.
     * Useful if for example flutter has specified a custom device orientation (for example kPortraitDown), in that case we of course
     * also need to transform the touch coords.
     *
     */
    struct mat3f display_to_view_transform;

    /**
     * @brief Matrix for transforming view coordinates to display coordinates.
     *
     * Can be used as a root surface transform, for fitting the flutter view into the desired display frame.
     *
     * Useful if for example flutter has specified a custom device orientation (for example kPortraitDown),
     * because we need to rotate the flutter view in that case.
     */
    struct mat3f view_to_display_transform;

    /**
     * @brief True if we should use a specific pixel format.
     *
     */
    bool has_forced_pixel_format;

    /**
     * @brief The forced pixel format if @ref has_forced_pixel_format is true.
     *
     */
    enum pixfmt forced_pixel_format;

    /**
     * @brief The current flutter layer composition that should be output on screen.
     *
     */
    struct fl_layer_composition *composition;

    /**
     * @brief KMS-specific fields if this is a KMS window.
     *
     */
    struct {
        struct drmdev *drmdev;
        struct drm_connector *connector;
        struct drm_encoder *encoder;
        struct drm_crtc *crtc;
        drmModeModeInfo *mode;

        bool should_apply_mode;
        struct cursor_buffer *cursor;
    } kms;

    /**
     * @brief The type of rendering that should be used. (gl, vk)
     *
     */
    enum renderer_type renderer_type;

    /**
     * @brief The OpenGL renderer if OpenGL rendering should be used.
     *
     */
    struct gl_renderer *gl_renderer;

    /**
     * @brief The Vulkan renderer if Vulkan rendering should be used.
     *
     */
    struct vk_renderer *vk_renderer;

    /**
     * @brief Our main render surface, if we have one yet.
     *
     * Otherwise a new one should be created using the render surface interface.
     *
     */
    struct render_surface *render_surface;

    /**
     * @brief The EGLSurface of this window, if any.
     *
     * Should be EGL_NO_SURFACE if this window is not associated with any EGL surface.
     * This is really just a workaround because flutter doesn't support arbitrary EGL surfaces as render targets right now.
     * (Just one global EGLSurface)
     *
     */
    EGLSurface egl_surface;

    /**
     * @brief Whether this window currently shows a mouse cursor.
     *
     */
    bool cursor_enabled;

    /**
     * @brief The position of the mouse cursor.
     *
     */
    struct vec2i cursor_pos;

    int (*push_composition)(struct window *window, struct fl_layer_composition *composition);
    struct render_surface *(*get_render_surface)(struct window *window, struct vec2i size);
    EGLSurface (*get_egl_surface)(struct window *window);
    int (*enable_cursor_locked)(struct window *window, struct vec2i pos);
    int (*disable_cursor_locked)(struct window *window);
    int (*move_cursor_locked)(struct window *window, struct vec2i pos);
    void (*deinit)(struct window *window);
};

void window_destroy(struct window *window);

DEFINE_STATIC_LOCK_OPS(window, lock)
DEFINE_REF_OPS(window, n_refs)

static void fill_view_matrices(
    drm_plane_transform_t transform,
    int display_width,
    int display_height,
    struct mat3f *display_to_view_transform_out,
    struct mat3f *view_to_display_transform_out
) {
    DEBUG_ASSERT(PLANE_TRANSFORM_IS_ONLY_ROTATION(transform));

    if (transform.rotate_0) {
        *view_to_display_transform_out = MAT3F_TRANSLATION(0, 0);

        *display_to_view_transform_out = MAT3F_TRANSLATION(0, 0);
    } else if (transform.rotate_90) {
        *view_to_display_transform_out = MAT3F_ROTZ(90);
        view_to_display_transform_out->transX = display_width;

        *display_to_view_transform_out = MAT3F_ROTZ(-90);
        display_to_view_transform_out->transY = display_width;
    } else if (transform.rotate_180) {
        *view_to_display_transform_out = MAT3F_ROTZ(180);
        view_to_display_transform_out->transX = display_width;
        view_to_display_transform_out->transY = display_height;

        *display_to_view_transform_out = MAT3F_ROTZ(-180);
        display_to_view_transform_out->transX = display_width;
        display_to_view_transform_out->transY = display_height;
    } else if (transform.rotate_270) {
        *view_to_display_transform_out = MAT3F_ROTZ(270);
        view_to_display_transform_out->transY = display_height;

        *display_to_view_transform_out = MAT3F_ROTZ(-270);
        display_to_view_transform_out->transX = display_height;
    }
}

static void window_deinit(struct window *window);

static int window_init(
    // clang-format off
    struct window *window,
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    int width, int height,
    bool has_dimensions, int width_mm, int height_mm,
    double refresh_rate,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format
    // clang-format on
) {
    enum device_orientation original_orientation;
    double pixel_ratio;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(tracer);
    DEBUG_ASSERT_NOT_NULL(scheduler);
    DEBUG_ASSERT(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));
    DEBUG_ASSERT(!has_orientation || ORIENTATION_IS_VALID(orientation));
    DEBUG_ASSERT(!has_dimensions || (width_mm > 0 && height_mm > 0));

    if (has_dimensions == false) {
        LOG_DEBUG(
            "WARNING: display didn't provide valid physical dimensions. The device-pixel ratio will default "
            "to 1.0, which may not be the fitting device-pixel ratio for your display. \n"
            "Use the `-d` commandline parameter to specify the physical dimensions of your display.\n"
        );
        pixel_ratio = 1.0;
    } else {
        pixel_ratio = (10.0 * width) / (width_mm * 38.0);

        int horizontal_dpi = (int) (width / (width_mm / 25.4));
        int vertical_dpi = (int) (height / (height_mm / 25.4));

        if (horizontal_dpi != vertical_dpi) {
            // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
            LOG_DEBUG("INFO: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
        }
    }

    DEBUG_ASSERT(!has_rotation || PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));

    if (width > height) {
        original_orientation = kLandscapeLeft;
    } else {
        original_orientation = kPortraitUp;
    }

    if (!has_rotation && !has_orientation) {
        rotation = PLANE_TRANSFORM_ROTATE_0;
        orientation = original_orientation;
        has_rotation = true;
        has_orientation = true;
    } else if (!has_orientation) {
        drm_plane_transform_t r = rotation;
        orientation = original_orientation;
        while (r.u64 != PLANE_TRANSFORM_ROTATE_0.u64) {
            orientation = ORIENTATION_ROTATE_CW(orientation);
            r = PLANE_TRANSFORM_ROTATE_CCW(r);
        }
        has_orientation = true;
    } else if (!has_rotation) {
        enum device_orientation o = orientation;
        rotation = PLANE_TRANSFORM_ROTATE_0;
        while (o != original_orientation) {
            rotation = PLANE_TRANSFORM_ROTATE_CW(rotation);
            o = ORIENTATION_ROTATE_CCW(o);
        }
        has_rotation = true;
    } else {
        enum device_orientation o = orientation;
        drm_plane_transform_t r = rotation;
        while (r.u64 != PLANE_TRANSFORM_ROTATE_0.u64) {
            r = PLANE_TRANSFORM_ROTATE_CCW(r);
            o = ORIENTATION_ROTATE_CCW(o);
        }

        if (ORIENTATION_IS_LANDSCAPE(o) && !(width >= height)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more high than wide, but de-rotated orientation is landscape)\n"
            );
        } else if (ORIENTATION_IS_PORTRAIT(o) && !(height >= width)) {
            LOG_DEBUG(
                "Explicit orientation and rotation given, but orientation is inconsistent with orientation. (display "
                "is more wide than high, but de-rotated orientation is portrait)\n"
            );
        }

        original_orientation = o;
    }

    DEBUG_ASSERT(has_orientation && has_rotation);

    fill_view_matrices(rotation, width, height, &window->display_to_view_transform, &window->view_to_display_transform);

    pthread_mutex_init(&window->lock, NULL);
    window->n_refs = REFCOUNT_INIT_1;
    window->tracer = tracer_ref(tracer);
    window->frame_scheduler = frame_scheduler_ref(scheduler);
    window->refresh_rate = refresh_rate;
    window->pixel_ratio = pixel_ratio;
    window->has_dimensions = has_dimensions;
    window->width_mm = width_mm;
    window->height_mm = height_mm;
    window->view_size = rotation.rotate_90 || rotation.rotate_270 ? VEC2F(height, width) : VEC2F(width, height);
    window->display_size = VEC2F(width, height);
    window->rotation = rotation;
    window->orientation = orientation;
    window->original_orientation = original_orientation;
    window->has_forced_pixel_format = has_forced_pixel_format;
    window->forced_pixel_format = forced_pixel_format;
    window->composition = NULL;
    window->renderer_type = kOpenGL_RendererType;
    window->gl_renderer = NULL;
    window->vk_renderer = NULL;
    window->render_surface = NULL;
    window->egl_surface = NULL;
    window->cursor_enabled = false;
    window->cursor_pos = VEC2I(0, 0);
    window->push_composition = NULL;
    window->get_render_surface = NULL;
    window->get_egl_surface = NULL;
    window->enable_cursor_locked = NULL;
    window->disable_cursor_locked = NULL;
    window->move_cursor_locked = NULL;
    window->deinit = window_deinit;
    return 0;
}

static void window_deinit(struct window *window) {
    frame_scheduler_unref(window->frame_scheduler);
    tracer_unref(window->tracer);
    pthread_mutex_destroy(&window->lock);
}

void window_destroy(struct window *window) {
    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(window->deinit);

    window->deinit(window);
    free(window);
}

int window_push_composition(struct window *window, struct fl_layer_composition *composition) {
    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(composition);
    DEBUG_ASSERT_NOT_NULL(window->push_composition);
    return window->push_composition(window, composition);
}

struct view_geometry window_get_view_geometry(struct window *window) {
    DEBUG_ASSERT_NOT_NULL(window);

    window_lock(window);
    struct view_geometry geometry = {
        .view_size = window->view_size,
        .display_size = window->display_size,
        .display_to_view_transform = window->display_to_view_transform,
        .view_to_display_transform = window->view_to_display_transform,
        .device_pixel_ratio = window->pixel_ratio,
    };
    window_unlock(window);

    return geometry;
}

double window_get_refresh_rate(struct window *window) {
    DEBUG_ASSERT_NOT_NULL(window);

    return window->refresh_rate;
}

int window_get_next_vblank(struct window *window, uint64_t *next_vblank_ns_out) {
    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(next_vblank_ns_out);
    (void) window;
    (void) next_vblank_ns_out;

    /// TODO: Implement
    UNIMPLEMENTED();

    return 0;
}

EGLSurface window_get_egl_surface(struct window *window) {
    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(window->get_egl_surface);
    return window->get_egl_surface(window);
}

struct render_surface *window_get_render_surface(struct window *window, struct vec2i size) {
    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(window->get_render_surface);
    return window->get_render_surface(window, size);
}

bool window_is_cursor_enabled(struct window *window) {
    bool enabled;

    DEBUG_ASSERT_NOT_NULL(window);

    window_lock(window);
    enabled = window->cursor_enabled;
    window_unlock(window);

    return enabled;
}

int window_set_cursor(struct window *window, bool has_enabled, bool enabled, bool has_pos, struct vec2i pos) {
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);

    if (!has_enabled && !has_pos) {
        return 0;
    }

    window_lock(window);

    if (has_enabled) {
        if (enabled && !window->cursor_enabled) {
            // enable cursor
            DEBUG_ASSERT_NOT_NULL(window->enable_cursor_locked);
            ok = window->enable_cursor_locked(window, pos);
        } else if (!enabled && window->cursor_enabled) {
            // disable cursor
            DEBUG_ASSERT_NOT_NULL(window->disable_cursor_locked);
            ok = window->disable_cursor_locked(window);
        }
    }

    if (has_pos) {
        if (window->cursor_enabled) {
            // move cursor
            DEBUG_ASSERT_NOT_NULL(window->move_cursor_locked);
            ok = window->move_cursor_locked(window, pos);
        } else {
            // !enabled && !window->cursor_enabled
            // move cursor while cursor is disabled
            LOG_ERROR("Attempted to move cursor while cursor is disabled.\n");
            ok = EINVAL;
        }
    }

    window_unlock(window);
    return ok;
}

enum cursor_size {
    k32x32_CursorSize,
    k48x48_CursorSize,
    k64x64_CursorSize,
    k96x96_CursorSize,
    k128x128_CursorSize,

    kMax_CursorSize = k128x128_CursorSize,
    kCount_CursorSize = kMax_CursorSize + 1
};

struct cursor_buffer {
    refcount_t n_refs;

    struct drmdev *drmdev;
    uint32_t gem_handle;
    int drm_fb_id;
    enum pixfmt format;
    int width, height;
    enum cursor_size size;
    drm_plane_transform_t rotation;

    int hot_x, hot_y;
};

static const int pixel_size_for_cursor_size
    [] = { [k32x32_CursorSize] = 32, [k48x48_CursorSize] = 48, [k64x64_CursorSize] = 64, [k96x96_CursorSize] = 96, [k128x128_CursorSize] = 128 };

COMPILE_ASSERT(ARRAY_SIZE(pixel_size_for_cursor_size) == kCount_CursorSize);

UNUSED static enum cursor_size cursor_size_from_pixel_ratio(double device_pixel_ratio) {
    double last_diff = INFINITY;
    enum cursor_size size;

    for (enum cursor_size size_iter = k32x32_CursorSize; size_iter < kCount_CursorSize; size_iter++) {
        double cursor_dpr = (pixel_size_for_cursor_size[size_iter] * 3 * 10.0) / (25.4 * 38);
        double cursor_screen_dpr_diff = device_pixel_ratio - cursor_dpr;
        if ((-last_diff < cursor_screen_dpr_diff) && (cursor_screen_dpr_diff < last_diff)) {
            size = size_iter;
            last_diff = cursor_screen_dpr_diff;
        }
    }

    return size;
}

UNUSED static struct cursor_buffer *cursor_buffer_new(struct drmdev *drmdev, enum cursor_size size, drm_plane_transform_t rotation) {
    const struct cursor_icon *icon;
    struct cursor_buffer *b;
    uint32_t gem_handle, pitch;
    uint32_t fb_id;
    size_t buffer_size;
    void *map_void;
    int pixel_size, hot_x, hot_y;
    int ok;

    DEBUG_ASSERT_NOT_NULL(drmdev);
    DEBUG_ASSERT(PLANE_TRANSFORM_IS_ONLY_ROTATION(rotation));

    if (!drmdev_supports_dumb_buffers(drmdev)) {
        LOG_ERROR("KMS doesn't support dumb buffers. Can't upload mouse cursor icon.\n");
        return NULL;
    }

    b = malloc(sizeof *b);
    if (b == NULL) {
        return NULL;
    }

    pixel_size = pixel_size_for_cursor_size[size];

    ok = drmdev_create_dumb_buffer(drmdev, pixel_size, pixel_size, 32, &gem_handle, &pitch, &buffer_size);
    if (ok != 0) {
        goto fail_free_b;
    }

    map_void = drmdev_map_dumb_buffer(drmdev, gem_handle, buffer_size);
    if (map_void == NULL) {
        goto fail_destroy_dumb_buffer;
    }

    icon = cursors + size;
    DEBUG_ASSERT_EQUALS(pixel_size, icon->width);
    DEBUG_ASSERT_EQUALS(pixel_size, icon->height);

    if (rotation.rotate_0 == 0) {
        DEBUG_ASSERT_EQUALS(pixel_size * 4, pitch);
        memcpy(map_void, icon->data, buffer_size);
        hot_x = icon->hot_x;
        hot_y = icon->hot_y;
    } else if (rotation.rotate_90 || rotation.rotate_180 || rotation.rotate_270) {
        uint32_t *map_uint32 = (uint32_t *) map_void;

        for (int y = 0; y < pixel_size; y++) {
            for (int x = 0; x < pixel_size; x++) {
                int buffer_x, buffer_y;
                if (rotation.rotate_90) {
                    buffer_x = pixel_size - y - 1;
                    buffer_y = x;
                } else if (rotation.rotate_180) {
                    buffer_x = pixel_size - y - 1;
                    buffer_y = pixel_size - x - 1;
                } else {
                    DEBUG_ASSERT(rotation.rotate_270);
                    buffer_x = y;
                    buffer_y = pixel_size - x - 1;
                }

                int buffer_offset = pitch * buffer_y + 4 * buffer_x;
                int cursor_offset = pixel_size * y + x;

                map_uint32[buffer_offset / 4] = icon->data[cursor_offset];
            }
        }

        if (rotation.rotate_90) {
            hot_x = pixel_size - icon->hot_y - 1;
            hot_y = icon->hot_x;
        } else if (rotation.rotate_180) {
            hot_x = pixel_size - icon->hot_x - 1;
            hot_y = pixel_size - icon->hot_y - 1;
        } else {
            DEBUG_ASSERT(rotation.rotate_270);
            hot_x = icon->hot_y;
            hot_y = pixel_size - icon->hot_x - 1;
        }
    }

    drmdev_unmap_dumb_buffer(drmdev, map_void, size);

    fb_id = drmdev_add_fb(drmdev, pixel_size, pixel_size, kARGB8888_FpiPixelFormat, gem_handle, pitch, 0, true, DRM_FORMAT_MOD_LINEAR);
    if (fb_id == 0) {
        LOG_ERROR("Couldn't add mouse cursor buffer as KMS framebuffer.\n");
        goto fail_destroy_dumb_buffer;
    }

    b->n_refs = REFCOUNT_INIT_1;
    b->drmdev = drmdev_ref(drmdev);
    b->gem_handle = gem_handle;
    b->drm_fb_id = fb_id;
    b->format = kARGB8888_FpiPixelFormat;
    b->width = pixel_size;
    b->height = pixel_size;
    b->size = size;
    b->rotation = rotation;
    b->hot_x = hot_x;
    b->hot_y = hot_y;
    return b;

fail_destroy_dumb_buffer:
    drmdev_destroy_dumb_buffer(drmdev, gem_handle);

fail_free_b:
    free(b);
    return NULL;
}

static void cursor_buffer_destroy(struct cursor_buffer *buffer) {
    drmdev_rm_fb(buffer->drmdev, buffer->drm_fb_id);
    drmdev_destroy_dumb_buffer(buffer->drmdev, buffer->gem_handle);
    drmdev_unref(buffer->drmdev);
    free(buffer);
}

DEFINE_STATIC_REF_OPS(cursor_buffer, n_refs)

static int select_mode(
    struct drmdev *drmdev,
    struct drm_connector **connector_out,
    struct drm_encoder **encoder_out,
    struct drm_crtc **crtc_out,
    drmModeModeInfo **mode_out,
    const char *desired_videomode
) {
    struct drm_connector *connector;
    struct drm_encoder *encoder;
    struct drm_crtc *crtc;
    drmModeModeInfo *mode, *mode_iter;
    int ok;

    // find any connected connector
    for_each_connector_in_drmdev(drmdev, connector) {
        if (connector->variable_state.connection_state == kConnected_DrmConnectionState) {
            break;
        }
    }

    if (connector == NULL) {
        LOG_ERROR("Could not find a connected connector!\n");
        return EINVAL;
    }

    mode = NULL;
    if (desired_videomode != NULL) {
        for_each_mode_in_connector(connector, mode_iter) {
            char *modeline = NULL, *modeline_nohz = NULL;

            ok = asprintf(&modeline, "%" PRIu16 "x%" PRIu16 "@%" PRIu32, mode_iter->hdisplay, mode_iter->vdisplay, mode_iter->vrefresh);
            if (ok < 0) {
                return ENOMEM;
            }

            ok = asprintf(&modeline_nohz, "%" PRIu16 "x%" PRIu16, mode_iter->hdisplay, mode_iter->vdisplay);
            if (ok < 0) {
                return ENOMEM;
            }

            if (strcmp(modeline, desired_videomode) == 0) {
                // Probably a bit superfluos, but the refresh rate can still vary in the decimal places.
                if (mode == NULL || (mode_get_vrefresh(mode_iter) > mode_get_vrefresh(mode))) {
                    mode = mode_iter;
                }
            } else if (strcmp(modeline_nohz, desired_videomode) == 0) {
                if (mode == NULL || (mode_get_vrefresh(mode_iter) > mode_get_vrefresh(mode))) {
                    mode = mode_iter;
                }
            }

            free(modeline);
            free(modeline_nohz);
        }

        if (mode == NULL) {
            LOG_ERROR("Didn't find a videomode matching \"%s\"! Falling back to display preferred mode.\n", desired_videomode);
        }
    }

    // Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
    // Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
    // prefer higher refresh rates. After that, prefer progressive scanout modes.
    if (mode == NULL) {
        for_each_mode_in_connector(connector, mode_iter) {
            if (mode_iter->type & DRM_MODE_TYPE_PREFERRED) {
                mode = mode_iter;
                break;
            } else if (mode == NULL) {
                mode = mode_iter;
            } else {
                int area = mode_iter->hdisplay * mode_iter->vdisplay;
                int old_area = mode->hdisplay * mode->vdisplay;

                if ((area > old_area) || ((area == old_area) && (mode_iter->vrefresh > mode->vrefresh)) ||
                    ((area == old_area) && (mode_iter->vrefresh == mode->vrefresh) && ((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0))) {
                    mode = mode_iter;
                }
            }
        }

        if (mode == NULL) {
            LOG_ERROR("Could not find a preferred output mode!\n");
            return EINVAL;
        }
    }

    DEBUG_ASSERT_NOT_NULL(mode);

    // Find the encoder that's linked to the connector right now
    for_each_encoder_in_drmdev(drmdev, encoder) {
        if (encoder->encoder->encoder_id == connector->committed_state.encoder_id) {
            break;
        }
    }

    // Otherwise use use any encoder that the connector supports linking to
    if (encoder == NULL) {
        for (int i = 0; i < connector->n_encoders; i++, encoder = NULL) {
            for_each_encoder_in_drmdev(drmdev, encoder) {
                if (encoder->encoder->encoder_id == connector->encoders[i]) {
                    break;
                }
            }

            if (encoder->encoder->possible_crtcs) {
                // only use this encoder if there's a crtc we can use with it
                break;
            }
        }
    }

    if (encoder == NULL) {
        LOG_ERROR("Could not find a suitable DRM encoder.\n");
        return EINVAL;
    }

    // Find the CRTC that's currently linked to this encoder
    for_each_crtc_in_drmdev(drmdev, crtc) {
        if (crtc->id == encoder->encoder->crtc_id) {
            break;
        }
    }

    // Otherwise use any CRTC that this encoder supports linking to
    if (crtc == NULL) {
        for_each_crtc_in_drmdev(drmdev, crtc) {
            if (encoder->encoder->possible_crtcs & crtc->bitmask) {
                // find a CRTC that is possible to use with this encoder
                break;
            }
        }
    }

    if (crtc == NULL) {
        LOG_ERROR("Could not find a suitable DRM CRTC.\n");
        return EINVAL;
    }

    *connector_out = connector;
    *encoder_out = encoder;
    *crtc_out = crtc;
    *mode_out = mode;
    return 0;
}

static int kms_window_push_composition(struct window *window, struct fl_layer_composition *composition);
static struct render_surface *kms_window_get_render_surface(struct window *window, struct vec2i size);
static EGLSurface kms_window_get_egl_surface(struct window *window);
static void kms_window_deinit(struct window *window);
static int kms_window_enable_cursor_locked(struct window *window, struct vec2i pos);
static int kms_window_disable_cursor_locked(struct window *window);
static int kms_window_move_cursor_locked(struct window *window, struct vec2i pos);

MUST_CHECK struct window *kms_window_new(
    // clang-format off
    struct tracer *tracer,
    struct frame_scheduler *scheduler,
    enum renderer_type renderer_type,
    struct gl_renderer *gl_renderer,
    struct vk_renderer *vk_renderer,
    bool has_rotation, drm_plane_transform_t rotation,
    bool has_orientation, enum device_orientation orientation,
    bool has_explicit_dimensions, int width_mm, int height_mm,
    bool has_forced_pixel_format, enum pixfmt forced_pixel_format,
    struct drmdev *drmdev,
    const char *desired_videomode
    // clang-format on
) {
    struct window *window;
    struct drm_connector *selected_connector;
    struct drm_encoder *selected_encoder;
    struct drm_crtc *selected_crtc;
    drmModeModeInfo *selected_mode;
    bool has_dimensions;
    int ok;

    DEBUG_ASSERT_NOT_NULL(drmdev);

#if !defined(HAS_VULKAN)
    DEBUG_ASSERT(renderer_type != kVulkan_RendererType);
#endif

#if !defined(HAS_EGL) || !defined(HAS_GL)
    DEBUG_ASSERT(renderer_type != kOpenGL_RendererType);
#endif

    // if opengl --> gl_renderer != NULL && vk_renderer == NULL
    DEBUG_ASSERT(renderer_type != kOpenGL_RendererType || (gl_renderer != NULL && vk_renderer == NULL));

    // if vulkan --> vk_renderer != NULL && gl_renderer == NULL
    DEBUG_ASSERT(renderer_type != kVulkan_RendererType || (vk_renderer != NULL && gl_renderer == NULL));

    window = malloc(sizeof *window);
    if (window == NULL) {
        return NULL;
    }

    ok = select_mode(drmdev, &selected_connector, &selected_encoder, &selected_crtc, &selected_mode, desired_videomode);
    if (ok != 0) {
        goto fail_free_window;
    }

    if (has_explicit_dimensions) {
        has_dimensions = true;
    } else if (selected_connector->variable_state.width_mm % 10 || selected_connector->variable_state.height_mm % 10) {
        // as a heuristic, assume the physical dimensions are valid if they're not both multiples of 10.
        // dimensions like 160x90mm, 150x100mm are often bogus.
        has_dimensions = true;
        width_mm = selected_connector->variable_state.width_mm;
        height_mm = selected_connector->variable_state.height_mm;
    } else if (selected_connector->type == DRM_MODE_CONNECTOR_DSI
        && selected_connector->variable_state.width_mm == 0
        && selected_connector->variable_state.height_mm == 0) {
        // assume this is the official Raspberry Pi DSI display.
        has_dimensions = true;
        width_mm = 155;
        height_mm = 86;
    } else {
        has_dimensions = false;
    }

    ok = window_init(
        // clang-format off
        window,
        tracer,
        scheduler,
        has_rotation, rotation,
        has_orientation, orientation,
        selected_mode->hdisplay, selected_mode->vdisplay,
        has_dimensions, width_mm, height_mm,
        mode_get_vrefresh(selected_mode),
        has_forced_pixel_format, forced_pixel_format
        // clang-format on
    );
    if (ok != 0) {
        free(window);
        return NULL;
    }

    LOG_DEBUG_UNPREFIXED(
        "display mode:\n"
        "  resolution: %" PRIu16 " x %" PRIu16
        "\n"
        "  refresh rate: %fHz\n"
        "  physical size: %dmm x %dmm\n"
        "  flutter device pixel ratio: %f\n"
        "  pixel format: %s\n",
        selected_mode->hdisplay,
        selected_mode->vdisplay,
        mode_get_vrefresh(selected_mode),
        width_mm,
        height_mm,
        window->pixel_ratio,
        has_forced_pixel_format ? get_pixfmt_info(forced_pixel_format)->name : "(any)"
    );

    window->kms.drmdev = drmdev_ref(drmdev);
    window->kms.connector = selected_connector;
    window->kms.encoder = selected_encoder;
    window->kms.crtc = selected_crtc;
    window->kms.mode = selected_mode;
    window->kms.should_apply_mode = true;
    window->kms.cursor = NULL;
    window->renderer_type = renderer_type;
    window->gl_renderer = gl_renderer != NULL ? gl_renderer_ref(gl_renderer) : NULL;
    if (vk_renderer != NULL) {
#ifdef HAS_VULKAN
        window->vk_renderer = vk_renderer_ref(vk_renderer);
#else
        UNREACHABLE();
#endif
    } else {
        window->vk_renderer = NULL;
    }
    window->push_composition = kms_window_push_composition;
    window->get_render_surface = kms_window_get_render_surface;
    window->get_egl_surface = kms_window_get_egl_surface;
    window->deinit = kms_window_deinit;
    window->enable_cursor_locked = kms_window_enable_cursor_locked;
    window->disable_cursor_locked = kms_window_disable_cursor_locked;
    window->move_cursor_locked = kms_window_move_cursor_locked;
    return window;

fail_free_window:
    free(window);
    return NULL;
}

void kms_window_deinit(struct window *window) {
    /// TODO: Do we really need to do this?
    /*
    struct kms_req_builder *builder;
    struct kms_req *req;
    int ok;

    builder = drmdev_create_request_builder(window->kms.drmdev, window->kms.crtc->id);
    DEBUG_ASSERT_NOT_NULL(builder);

    ok = kms_req_builder_unset_mode(builder);
    DEBUG_ASSERT_EQUALS(ok, 0);

    req = kms_req_builder_build(builder);
    DEBUG_ASSERT_NOT_NULL(req);

    kms_req_builder_unref(builder);

    ok = kms_req_commit_blocking(req, NULL);
    DEBUG_ASSERT_EQUALS(ok, 0);
    (void) ok;

    kms_req_unref(req);
    */
    if (window->render_surface != NULL) {
        surface_unref(CAST_SURFACE(window->render_surface));
    }
    if (window->gl_renderer != NULL) {
        gl_renderer_unref(window->gl_renderer);
    }
    if (window->vk_renderer != NULL) {
#ifdef HAS_VULKAN
        vk_renderer_unref(window->vk_renderer);
#else
        UNREACHABLE();
#endif
    }
    drmdev_unref(window->kms.drmdev);
    window_deinit(window);
}

struct frame {
    struct tracer *tracer;
    struct kms_req *req;
    bool unset_should_apply_mode_on_commit;
};

UNUSED static void on_scanout(struct drmdev *drmdev, uint64_t vblank_ns, void *userdata) {
    DEBUG_ASSERT_NOT_NULL(drmdev);
    (void) drmdev;
    (void) vblank_ns;
    (void) userdata;

    /// TODO: What should we do here?
}

static void on_present_frame(void *userdata) {
    struct frame *frame;
    int ok;

    DEBUG_ASSERT_NOT_NULL(userdata);

    frame = userdata;

    TRACER_BEGIN(frame->tracer, "kms_req_commit_nonblocking");
    ok = kms_req_commit_blocking(frame->req, NULL);
    TRACER_END(frame->tracer, "kms_req_commit_nonblocking");

    if (ok != 0) {
        LOG_ERROR("Could not commit frame request.\n");
    }

    tracer_unref(frame->tracer);
    kms_req_unref(frame->req);
    free(frame);
}

static void on_cancel_frame(void *userdata) {
    struct frame *frame;
    DEBUG_ASSERT_NOT_NULL(userdata);

    frame = userdata;

    tracer_unref(frame->tracer);
    kms_req_unref(frame->req);
    free(frame);
}

static int kms_window_push_composition(struct window *window, struct fl_layer_composition *composition) {
    struct kms_req_builder *builder;
    struct kms_req *req;
    struct frame *frame;
    int ok;

    DEBUG_ASSERT_NOT_NULL(window);
    DEBUG_ASSERT_NOT_NULL(composition);

    // If flutter won't request frames (because the vsync callback is broken),
    // we'll wait here for the previous frame to be presented / rendered.
    // Otherwise the surface_swap_buffers at the bottom might allocate an
    // additional buffer and we'll potentially use more buffers than we're
    // trying to use.
    // if (!window->use_frame_requests) {
    //     TRACER_BEGIN(window->tracer, "window_request_frame_and_wait_for_begin");
    //     ok = window_request_frame_and_wait_for_begin(window);
    //     TRACER_END(window->tracer, "window_request_frame_and_wait_for_begin");
    //     if (ok != 0) {
    //         LOG_ERROR("Could not wait for frame begin.\n");
    //         return ok;
    //     }
    // }

    window_lock(window);

    /// TODO: If we don't have new revisions, we don't need to scanout anything.
    fl_layer_composition_swap_ptrs(&window->composition, composition);

    builder = drmdev_create_request_builder(window->kms.drmdev, window->kms.crtc->id);
    if (builder == NULL) {
        ok = ENOMEM;
        goto fail_unlock;
    }

    // We only set the mode once, at the first atomic request.
    if (window->kms.should_apply_mode) {
        ok = kms_req_builder_set_connector(builder, window->kms.connector->id);
        if (ok != 0) {
            LOG_ERROR("Couldn't select connector.\n");
            goto fail_unref_builder;
        }

        ok = kms_req_builder_set_mode(builder, window->kms.mode);
        if (ok != 0) {
            LOG_ERROR("Couldn't apply output mode.\n");
            goto fail_unref_builder;
        }
    }

    for (size_t i = 0; i < fl_layer_composition_get_n_layers(composition); i++) {
        struct fl_layer *layer = fl_layer_composition_peek_layer(composition, i);

        ok = surface_present_kms(layer->surface, &layer->props, builder);
        if (ok != 0) {
            LOG_ERROR("Couldn't present flutter layer on screen. surface_present_kms: %s\n", strerror(ok));
            goto fail_unref_builder;
        }
    }

    // add cursor infos
    if (window->kms.cursor != NULL) {
        ok = kms_req_builder_push_fb_layer(
            builder,
            &(const struct kms_fb_layer){
                .drm_fb_id = window->kms.cursor->drm_fb_id,
                .format = window->kms.cursor->format,
                .has_modifier = true,
                .modifier = DRM_FORMAT_MOD_LINEAR,
                .src_x = 0,
                .src_y = 0,
                .src_w = ((uint16_t) window->kms.cursor->width) << 16,
                .src_h = ((uint16_t) window->kms.cursor->height) << 16,
                .dst_x = window->cursor_pos.x - window->kms.cursor->hot_x,
                .dst_y = window->cursor_pos.y - window->kms.cursor->hot_y,
                .dst_w = window->kms.cursor->width,
                .dst_h = window->kms.cursor->height,
                .has_rotation = false,
                .rotation = PLANE_TRANSFORM_NONE,
                .has_in_fence_fd = false,
                .in_fence_fd = 0,
                .prefer_cursor = true,
            },
            cursor_buffer_unref_void,
            NULL,
            window->kms.cursor
        );
        if (ok != 0) {
            LOG_ERROR("Couldn't present cursor.\n");
        } else {
            cursor_buffer_ref(window->kms.cursor);
        }
    }

    req = kms_req_builder_build(builder);
    if (req == NULL) {
        goto fail_unref_builder;
    }

    kms_req_builder_unref(builder);
    builder = NULL;

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        goto fail_unref_req;
    }

    frame->req = req;
    frame->tracer = tracer_ref(window->tracer);
    frame->unset_should_apply_mode_on_commit = window->kms.should_apply_mode;

    frame_scheduler_present_frame(window->frame_scheduler, on_present_frame, frame, on_cancel_frame);

    // if (window->present_mode == kDoubleBufferedVsync_PresentMode) {
    //     TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
    //     ok = kms_req_commit(req, /* blocking: */ false);
    //     TRACER_END(window->tracer, "kms_req_builder_commit");
    //
    //     if (ok != 0) {
    //         LOG_ERROR("Could not commit frame request.\n");
    //         goto fail_unref_window2;
    //     }
    //
    //     if (window->set_set_mode) {
    //         window->set_mode = false;
    //         window->set_set_mode = false;
    //     }
    // } else {
    //     DEBUG_ASSERT_EQUALS(window->present_mode, kTripleBufferedVsync_PresentMode);
    //
    //     if (window->present_immediately) {
    //         TRACER_BEGIN(window->tracer, "kms_req_builder_commit");
    //         ok = kms_req_commit(req, /* blocking: */ false);
    //         TRACER_END(window->tracer, "kms_req_builder_commit");
    //
    //         if (ok != 0) {
    //             LOG_ERROR("Could not commit frame request.\n");
    //             goto fail_unref_window2;
    //         }
    //
    //         if (window->set_set_mode) {
    //             window->set_mode = false;
    //             window->set_set_mode = false;
    //         }
    //
    //         window->present_immediately = false;
    //     } else {
    //         if (window->next_frame != NULL) {
    //             /// FIXME: Call the release callbacks when the kms_req is destroyed, not when it's unrefed.
    //             /// Not sure this here will lead to the release callbacks being called multiple times.
    //             kms_req_call_release_callbacks(window->next_frame);
    //             kms_req_unref(window->next_frame);
    //         }
    //
    //         window->next_frame = kms_req_ref(req);
    //         window->set_set_mode = window->set_mode;
    //     }
    // }

    window_unlock(window);

    // KMS Req is committed now and drmdev keeps a ref
    // on it internally, so we don't need to keep this one.
    // kms_req_unref(req);

    // window_on_rendering_complete(window);

    return 0;

fail_unref_req:
    kms_req_unref(req);
    goto fail_unlock;

fail_unref_builder:
    kms_req_builder_unref(builder);

fail_unlock:
    window_unlock(window);
    return ok;
}

static struct render_surface *kms_window_get_render_surface_internal(struct window *window, bool has_size, struct vec2i size) {
    struct render_surface *render_surface;

    DEBUG_ASSERT_NOT_NULL(window);

    if (window->render_surface != NULL) {
        return window->render_surface;
    }

    if (!has_size) {
        // Flutter wants a render surface, but hasn't told us the backing store dimensions yet.
        // Just make a good guess about the dimensions.
        LOG_DEBUG("Flutter requested render surface before supplying surface dimensions.\n");
        size = VEC2I(window->kms.mode->hdisplay, window->kms.mode->vdisplay);
    }

    enum pixfmt pixel_format = window->has_forced_pixel_format ? window->forced_pixel_format : kARGB8888_FpiPixelFormat;

    // Possibly populate this with the supported modifiers for this pixel format.
    // If no plane lists modifiers for this pixel format, this will be left at NULL,
    // and egl_gbm_render_surface_new... will create the GBM surface using usage flags
    // (GBM_USE_SCANOUT | GBM_USE_RENDER) instead.
    uint64_t *allowed_modifiers = NULL;
    size_t n_allowed_modifiers = 0;

    // For now just set the supported modifiers for the first plane that supports this pixel format
    // as the allowed modifiers.
    {
        struct drm_plane *plane;
        for_each_plane_in_drmdev(window->kms.drmdev, plane) {
            if (!(plane->possible_crtcs & window->kms.crtc->bitmask)) {
                continue;
            }

            if (plane->type != kPrimary_DrmPlaneType && plane->type != kOverlay_DrmPlaneType) {
                continue;
            }

            for (int i = 0; i < plane->n_supported_modified_formats; i++) {
                if (plane->supported_modified_formats[i].format == pixel_format) {
                    n_allowed_modifiers++;
                }
            }

            allowed_modifiers = malloc(n_allowed_modifiers * sizeof *allowed_modifiers);
            for (int i = 0, j = 0; i < plane->n_supported_modified_formats; i++) {
                if (plane->supported_modified_formats[i].format == pixel_format) {
                    allowed_modifiers[j++] = plane->supported_modified_formats[i].modifier;
                }
            }

            break;
        }
    }

    if (window->renderer_type == kOpenGL_RendererType) {
        // opengl
        struct egl_gbm_render_surface *egl_surface = egl_gbm_render_surface_new_with_egl_config(
            window->tracer,
            size,
            gl_renderer_get_gbm_device(window->gl_renderer),
            window->gl_renderer,
            window->has_forced_pixel_format ? window->forced_pixel_format : kARGB8888_FpiPixelFormat,
            EGL_NO_CONFIG_KHR,
            allowed_modifiers,
            n_allowed_modifiers
        );
        if (egl_surface == NULL) {
            LOG_ERROR("Couldn't create EGL GBM rendering surface.\n");
            goto fail_free_allowed_modifiers;
        }

        render_surface = CAST_RENDER_SURFACE(egl_surface);
    } else {
        // vulkan
#ifdef HAS_VULKAN
        struct vk_gbm_render_surface *vk_surface = vk_gbm_render_surface_new(
            window->tracer,
            size,
            drmdev_get_gbm_device(window->kms.drmdev),
            window->vk_renderer,
            window->has_forced_pixel_format ? window->forced_pixel_format : kARGB8888_FpiPixelFormat
        );
        if (vk_surface == NULL) {
            LOG_ERROR("Couldn't create Vulkan GBM rendering surface.\n");
            goto fail_free_allowed_modifiers;
        }

        render_surface = CAST_RENDER_SURFACE(vk_surface);
#else
        UNREACHABLE();
#endif
    }

    if (allowed_modifiers != NULL) {
        free(allowed_modifiers);
    }

    window->render_surface = render_surface;
    return render_surface;

fail_free_allowed_modifiers:
    if (allowed_modifiers != NULL) {
        free(allowed_modifiers);
    }

    return NULL;
}

static struct render_surface *kms_window_get_render_surface(struct window *window, struct vec2i size) {
    DEBUG_ASSERT_NOT_NULL(window);
    return kms_window_get_render_surface_internal(window, true, size);
}

static EGLSurface kms_window_get_egl_surface(struct window *window) {
    if (window->renderer_type == kOpenGL_RendererType) {
        struct render_surface *render_surface = kms_window_get_render_surface_internal(window, false, VEC2I(0, 0));
        return egl_gbm_render_surface_get_egl_surface(CAST_EGL_GBM_RENDER_SURFACE(render_surface));
    } else {
        return EGL_NO_SURFACE;
    }
}

static int kms_window_enable_cursor_locked(struct window *window, struct vec2i pos) {
    struct cursor_buffer *cursor;

    DEBUG_ASSERT_EQUALS(window->kms.cursor, NULL);

    cursor = cursor_buffer_new(window->kms.drmdev, cursor_size_from_pixel_ratio(window->pixel_ratio), window->rotation);
    if (cursor == NULL) {
        return EIO;
    }

    window->cursor_pos = pos;

    window->kms.cursor = cursor;
    return 0;
}

static int kms_window_disable_cursor_locked(struct window *window) {
    cursor_buffer_unrefp(&window->kms.cursor);
    return 0;
}

static int kms_window_move_cursor_locked(struct window *window, struct vec2i pos) {
    window->cursor_pos = pos;

    /// TODO: Draw a new frame or redraw the last frame with new cursor pos here.

    return 0;
}
