#include "texture_registry.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <flutter_embedder.h>

#include "flutter-pi.h"
#include "util/list.h"
#include "util/lock_ops.h"
#include "util/logging.h"
#include "util/refcounting.h"

struct texture_registry {
    struct texture_registry_interface interface;
    void *userdata;

    pthread_mutex_t lock;

    atomic_int_least64_t next_unused_id;
    struct list_head textures;
};

DEFINE_STATIC_LOCK_OPS(texture_registry, lock)

struct counted_texture_frame {
    refcount_t n_refs;

    bool is_resolved;
    struct texture_frame frame;

    struct unresolved_texture_frame unresolved_frame;
};

void counted_texture_frame_destroy(struct counted_texture_frame *frame) {
    if (frame->is_resolved) {
        if (frame->frame.destroy != NULL) {
            frame->frame.destroy(&frame->frame, frame->frame.userdata);
        }
    } else if (frame->unresolved_frame.destroy != NULL) {
        frame->unresolved_frame.destroy(frame->unresolved_frame.userdata);
    }
    free(frame);
}

DEFINE_REF_OPS(counted_texture_frame, n_refs)

struct texture {
    struct texture_registry *registry;

    pthread_mutex_t lock;

    struct list_head entry;

    /// The texture id the flutter engine uses to identify this texture.
    int64_t id;

    /// The frame that's scheduled to be displayed next. The texture holds a reference to this frame.
    /// If a new frame is scheduled using @ref texture_push_frame, the reference will be dropped.
    struct counted_texture_frame *next_frame;

    /**
     * @brief True if next_frame was not yet fetched by the engine. So if @ref texture_push_frame is called,
     * we can just replace @ref next_frame with the new frame and don't need to call mark frame available again.
     *
     */
    bool dirty;
};

struct texture_registry *texture_registry_new(const struct texture_registry_interface *interface, void *userdata) {
    struct texture_registry *reg;

    reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }

    pthread_mutex_init(&reg->lock, get_default_mutex_attrs());

    memcpy(&reg->interface, interface, sizeof(*interface));
    reg->userdata = userdata;
    reg->next_unused_id = 1;
    list_inithead(&reg->textures);

    return reg;
}

void texture_registry_destroy(struct texture_registry *reg) {
#ifndef NDEBUG
    int count = list_length(&reg->textures);
    if (count > 0) {
        LOG_ERROR("Error destroying texture registry: There are still %d textures registered. This is an application bug.\n", count);
        assert(false);
    }
#endif

    pthread_mutex_destroy(&reg->lock);
    free(reg);
}

int64_t texture_registry_allocate_id(struct texture_registry *reg) {
    return atomic_fetch_add(&reg->next_unused_id, 1);
}

static int texture_registry_register_texture(struct texture_registry *reg, struct texture *texture) {
    int ok;

    texture_registry_lock(reg);
    list_add(&texture->entry, &reg->textures);
    texture_registry_unlock(reg);

    ok = reg->interface.register_texture(reg->userdata, texture->id);
    if (ok != 0) {
        texture_registry_lock(reg);
        list_del(&texture->entry);
        texture_registry_unlock(reg);
        return ok;
    }

    return 0;
}

static void texture_registry_unregister_texture(struct texture_registry *reg, struct texture *texture) {
    reg->interface.unregister_texture(reg->userdata, texture->id);

    texture_registry_lock(reg);
    list_del(&texture->entry);
    texture_registry_unlock(reg);
}

#ifdef HAVE_EGL_GLES2
static bool
texture_gl_external_texture_frame_callback(struct texture *texture, size_t width, size_t height, FlutterOpenGLTexture *texture_out);

bool texture_registry_gl_external_texture_frame_callback(
    struct texture_registry *reg,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    struct texture *texture;
    bool result;

    texture_registry_lock(reg);

    texture = NULL;
    list_for_each_entry_safe(struct texture, t, &reg->textures, entry) {
        if (t->id == texture_id) {
            texture = t;
            break;
        }
    }

    if (texture != NULL) {
        result = texture_gl_external_texture_frame_callback(texture, width, height, texture_out);
    } else {
        // the texture was destroyed after notifying the engine of a new texture frame.
        // just report an empty frame here.
        result = false;
        texture_out->target = GL_TEXTURE_2D;
        texture_out->name = 0;
        texture_out->format = GL_RGBA8_OES;
        texture_out->user_data = NULL;
        texture_out->destruction_callback = NULL;
        texture_out->width = 0;
        texture_out->height = 0;
    }

    texture_registry_unlock(reg);

    return result;
}
#endif

DEFINE_INLINE_LOCK_OPS(texture, lock)

struct texture *texture_new(struct texture_registry *reg) {
    struct texture *texture;
    int64_t id;
    int ok;

    texture = malloc(sizeof *texture);
    if (texture == NULL) {
        return NULL;
    }

    id = texture_registry_allocate_id(reg);

    pthread_mutex_init(&texture->lock, get_default_mutex_attrs());
    texture->registry = reg;
    texture->id = id;
    texture->next_frame = NULL;
    texture->dirty = false;

    ok = texture_registry_register_texture(reg, texture);
    if (ok != 0) {
        pthread_mutex_destroy(&texture->lock);
        free(texture);
    }

    return texture;
}

int64_t texture_get_id(struct texture *texture) {
    return texture->id;
}

static int push_frame(
    struct texture *texture,
    bool is_resolved,
    const struct texture_frame *frame,
    const struct unresolved_texture_frame *unresolved_frame
) {
    struct counted_texture_frame *counted_frame;
    int ok;

    // I know there's memdup, but let's just be explicit here.
    counted_frame = malloc(sizeof *counted_frame);
    if (counted_frame == NULL) {
        return ENOMEM;
    }

    counted_frame->n_refs = REFCOUNT_INIT_0;
    counted_frame->is_resolved = is_resolved;
    if (frame != NULL) {
        counted_frame->frame = *frame;
    }
    if (unresolved_frame != NULL) {
        counted_frame->unresolved_frame = *unresolved_frame;
    }

    texture_lock(texture);

    counted_texture_frame_swap_ptrs(&texture->next_frame, counted_frame);

    if (texture->dirty == false) {
        ok = texture->registry->interface.mark_frame_available(texture->registry->userdata, texture->id);
        if (ok != 0) {
            /// TODO: Don't really know what do to here.
        }

        /// We have now called @ref texture_registry_engine_notify_frame_available available.
        /// If a new frame is pushed with @ref texture_push_frame, and the engine
        /// hasn't yet fetched the frame with the @ref texture_gl_external_texture_frame_callback,
        /// we can just replace the next frame without notifying the engine again.
        texture->dirty = true;
    }

    texture_unlock(texture);

    return 0;
}

int texture_push_frame(struct texture *texture, const struct texture_frame *frame) {
    return push_frame(texture, true, frame, NULL);
}

int texture_push_unresolved_frame(struct texture *texture, const struct unresolved_texture_frame *frame) {
    return push_frame(texture, false, NULL, frame);
}

void texture_destroy(struct texture *texture) {
    texture_registry_unregister_texture(texture->registry, texture);
    if (texture->next_frame != NULL) {
        counted_texture_frame_unref(texture->next_frame);
    }
    pthread_mutex_destroy(&texture->lock);
    free(texture);
}

UNUSED static void on_flutter_acquired_frame_destroy(void *userdata) {
    struct counted_texture_frame *frame;

    ASSERT_NOT_NULL(userdata);

    frame = userdata;

    counted_texture_frame_unref(frame);
}

#ifdef HAVE_EGL_GLES2
static bool
texture_gl_external_texture_frame_callback(struct texture *texture, size_t width, size_t height, FlutterOpenGLTexture *texture_out) {
    struct counted_texture_frame *frame;
    int ok;

    (void) width;
    (void) height;

    ASSERT_NOT_NULL(texture);
    ASSERT_NOT_NULL(texture_out);

    texture_lock(texture);

    if (texture->next_frame != NULL) {
        /// TODO: If acquiring the texture frame fails, flutter will destroy the texture frame two times.
        /// So we'll probably have a segfault if that happens.
        frame = counted_texture_frame_ref(texture->next_frame);
    } else {
        frame = NULL;
    }

    /// flutter has now fetched the texture, so if we want to present a new frame
    /// we need to call @ref texture_registry_engine_notify_frame_available again.
    texture->dirty = false;

    if (frame != NULL && !frame->is_resolved) {
        // resolve the frame to an actual OpenGL frame.
        ok = frame->unresolved_frame.resolve(width, height, frame->unresolved_frame.userdata, &frame->frame);
        if (ok != 0) {
            LOG_ERROR("Couldn't resolve texture frame.\n");
            counted_texture_frame_unrefp(&frame);
            counted_texture_frame_unrefp(&texture->next_frame);
        }

        frame->unresolved_frame.destroy(frame->unresolved_frame.userdata);
        frame->is_resolved = true;
    }

    texture_unlock(texture);

    // only actually fill out the frame info when we have a frame.
    // could be this method is called before the native code has called texture_push_frame.
    if (frame != NULL) {
        texture_out->target = frame->frame.gl.target;
        texture_out->name = frame->frame.gl.name;
        texture_out->format = frame->frame.gl.format;
        texture_out->user_data = frame;
        texture_out->destruction_callback = on_flutter_acquired_frame_destroy;
        texture_out->width = frame->frame.gl.width;
        texture_out->height = frame->frame.gl.height;
        return true;
    } else {
        texture_out->target = GL_TEXTURE_2D;
        texture_out->name = 0;
        texture_out->format = GL_RGBA8_OES;
        texture_out->user_data = NULL;
        texture_out->destruction_callback = NULL;
        texture_out->width = 0;
        texture_out->height = 0;

        /// TODO: Maybe we should return true here anyway?
        return false;
    }
}
#endif
