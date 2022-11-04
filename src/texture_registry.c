#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>

#include <flutter_embedder.h>

#include <texture_registry.h>
#include <flutter-pi.h>

FILE_DESCR("texture registry")

struct texture_registry {
    struct texture_registry_interface interface;
    void *userdata;

    pthread_mutex_t next_unused_id_mutex;
    int64_t next_unused_id;
    struct concurrent_pointer_set textures;
};

struct counted_texture_frame {
    refcount_t n_refs;
    struct texture_frame frame;
};

void counted_texture_frame_destroy(struct counted_texture_frame *frame) {
    frame->frame.destroy(
        &frame->frame,
        frame->frame.userdata
    );
    free(frame);
}

DEFINE_REF_OPS(counted_texture_frame, n_refs)

struct texture {
    struct texture_registry *registry;

    pthread_mutex_t lock;

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

struct texture_registry *texture_registry_new(
    const struct texture_registry_interface *interface,
    void *userdata
) {
    struct texture_registry *reg;
    int ok;

    reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }

    pthread_mutex_init(&reg->next_unused_id_mutex, NULL);

    memcpy(&reg->interface, interface, sizeof(*interface));
    reg->userdata = userdata;
    reg->next_unused_id = 1;
    
    ok = cpset_init(&reg->textures, CPSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        free(reg);
        return NULL;
    }

    return reg;
}

void texture_registry_destroy(struct texture_registry *reg) {
    cpset_lock(&reg->textures);
    int count = cpset_get_count_pointers_locked(&reg->textures);
    if (count > 0) {
        LOG_ERROR("Error destroying texture registry: There are still %d textures registered. This is an application bug.\n", count);
    }
    cpset_unlock(&reg->textures);

    cpset_deinit(&reg->textures);
    pthread_mutex_destroy(&reg->next_unused_id_mutex);
    free(reg);
}

int64_t texture_registry_allocate_id(struct texture_registry *reg) {
    pthread_mutex_lock(&reg->next_unused_id_mutex);

    int64_t id = reg->next_unused_id++;

    pthread_mutex_unlock(&reg->next_unused_id_mutex);

    return id;
}

static int texture_registry_register_texture(struct texture_registry *reg, struct texture *texture) {
    int ok;

    ok = cpset_put(&reg->textures, texture);
    if (ok != 0) {
        return ok;
    }

    ok = reg->interface.register_texture(reg->userdata, texture->id);
    if (ok != 0) {
        cpset_remove(&reg->textures, texture);
        return ok;
    }

    return 0;
}

static void texture_registry_unregister_texture(struct texture_registry *reg, struct texture *texture) {
    reg->interface.unregister_texture(reg->userdata, texture->id);
    cpset_remove(&reg->textures, texture);
}

static bool texture_gl_external_texture_frame_callback(
    struct texture *texture,
    size_t width, size_t height,
    FlutterOpenGLTexture *texture_out
);

bool texture_registry_gl_external_texture_frame_callback(
    struct texture_registry *reg,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    struct texture *t;
    bool result;

    cpset_lock(&reg->textures);
    for_each_pointer_in_cpset(&reg->textures, t) {
        if (t->id == texture_id) {
            break;
        }
    }
    if (t != NULL) {
        result = texture_gl_external_texture_frame_callback(t, width, height, texture_out);
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
    cpset_unlock(&reg->textures);

    return result;
}


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
    
    pthread_mutex_init(&texture->lock, NULL);
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

int texture_push_frame(struct texture *texture, const struct texture_frame *frame) {
    struct counted_texture_frame *counted_frame;
    int ok;

    counted_frame = malloc(sizeof *counted_frame);
    if (counted_frame == NULL) {
        return ENOMEM;
    }

    counted_frame->n_refs = REFCOUNT_INIT_1;
    counted_frame->frame = *frame;

    texture_lock(texture);

    if (texture->next_frame != NULL) {
        counted_texture_frame_unref(texture->next_frame);
        texture->next_frame = NULL;
    }

    texture->next_frame = counted_frame;
    
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

void texture_destroy(struct texture *texture) {
    texture_registry_unregister_texture(texture->registry, texture);
    if (texture->next_frame != NULL) {
        counted_texture_frame_unref(texture->next_frame);
    }
    pthread_mutex_destroy(&texture->lock);
    free(texture);
}

static void on_flutter_acquired_frame_destroy(void *userdata) {
    struct counted_texture_frame *frame;

    DEBUG_ASSERT_NOT_NULL(userdata);

    frame = userdata;

    counted_texture_frame_unref(frame);
}

static bool texture_gl_external_texture_frame_callback(
    struct texture *texture,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    struct counted_texture_frame *frame;

    (void) width;
    (void) height;

    DEBUG_ASSERT_NOT_NULL(texture);
    DEBUG_ASSERT_NOT_NULL(texture_out);

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
