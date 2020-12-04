#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <semaphore.h>

#include <flutter_embedder.h>

#include <collection.h>
#include <texture_registry.h>
#include <flutter-pi.h>

struct texture_registry {
    FlutterEngine engine;
    flutter_engine_register_external_texture_t register_texture;
    flutter_engine_mark_external_texture_frame_available_t mark_frame_available;
    flutter_engine_unregister_external_texture_t unregister_texture;
    struct concurrent_pointer_set delayed_deletes;
    struct concurrent_pointer_set textures;
    int64_t next_texture_id;
};

struct flutter_texture_frame {
    struct texture_registry *reg;
    FlutterOpenGLTexture gl_texture;
    bool delay_delete_to_next_page_flip;
    unsigned int n_refs;
};

struct flutter_texture {
    int64_t texture_id;
    struct flutter_texture_frame *frame;
};


static struct flutter_texture_frame *frame_new(
    struct texture_registry *reg,
    const FlutterOpenGLTexture *texture,
    bool delay_delete_to_next_page_flip
) {
    struct flutter_texture_frame *frame;

    frame = malloc(sizeof *frame);
    if (frame == NULL) {
        return NULL;
    }

    frame->reg = reg;
    frame->gl_texture = *texture;
    frame->delay_delete_to_next_page_flip = delay_delete_to_next_page_flip;
    frame->n_refs = 1;

    return frame;
}

static struct flutter_texture_frame *frame_ref(struct flutter_texture_frame *frame) {
    frame->n_refs++;
    return frame;
}

static void frame_unref(struct flutter_texture_frame *frame) {
    frame->n_refs--;
    if (frame->n_refs == 0) {
        if (frame->delay_delete_to_next_page_flip) {
            cpset_put(&frame->reg->delayed_deletes, frame);
        } else {
            frame->gl_texture.destruction_callback(frame->gl_texture.user_data);
            free(frame);
        }
    }
}

static void frame_unrefp(struct flutter_texture_frame **pframe) {
    frame_unref(*pframe);
    *pframe = NULL;
}


static struct flutter_texture *find_texture_by_id_locked(struct texture_registry *reg, int64_t texture_id) {
    struct flutter_texture *texture;

    for_each_pointer_in_cpset(&reg->textures, texture) {
        if (texture->texture_id == texture_id) {
            break;
        }
    }

    return texture;
}

static void destroy_all_delete_delayed_textures(
    struct texture_registry *reg
) {
    struct flutter_texture_frame *frame;

    cpset_lock(&reg->delayed_deletes);

    for_each_pointer_in_cpset(&reg->delayed_deletes, frame) {
        frame->gl_texture.destruction_callback(frame->gl_texture.user_data);
        free(frame);
    }

    cpset_unlock(&reg->delayed_deletes);
}


struct texture_registry *texreg_new(
    FlutterEngine engine,
    flutter_engine_register_external_texture_t register_texture,
    flutter_engine_mark_external_texture_frame_available_t mark_frame_available,
    flutter_engine_unregister_external_texture_t unregister_texture
) {
    struct texture_registry *reg;
    int ok;

    reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }

    reg->engine = engine;
    reg->register_texture = register_texture;
    reg->mark_frame_available = mark_frame_available;
    reg->unregister_texture = unregister_texture;
    reg->next_texture_id = 1;
    
    ok = cpset_init(&reg->textures, CPSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        free(reg);
        return NULL;
    }

    ok = cpset_init(&reg->delayed_deletes, CPSET_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        free(reg);
        return NULL;
    }

    return reg;
}

void texreg_set_engine(
    struct texture_registry *reg,
    FlutterEngine engine
) {
    reg->engine = engine;
}

void texreg_set_callbacks(
    struct texture_registry *reg,
    flutter_engine_register_external_texture_t register_texture,
    flutter_engine_mark_external_texture_frame_available_t mark_frame_available,
    flutter_engine_unregister_external_texture_t unregister_texture
) {
    reg->register_texture = register_texture;
    reg->mark_frame_available = mark_frame_available;
    reg->unregister_texture = unregister_texture;
}

int texreg_on_external_texture_frame_callback(
    struct texture_registry *reg,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    struct flutter_texture *texture;

    cpset_lock(&reg->textures);

    texture = find_texture_by_id_locked(reg, texture_id);
    if (texture == NULL) {
        cpset_unlock(&reg->textures);
        return EINVAL;
    }

    if (texture->frame == NULL) {
        cpset_unlock(&reg->textures);
        return EINVAL;
    }

    *texture_out = texture->frame->gl_texture;
    texture_out->user_data = frame_ref(texture->frame);
    texture_out->destruction_callback = (VoidCallback) frame_unref;

    printf("[texture registry] fetched gl texture %u from flutter texture %lld\n", texture->frame->gl_texture.name, texture->texture_id);

    cpset_unlock(&reg->textures);

    return true;
}

int texreg_new_texture(
    struct texture_registry *reg,
    int64_t *texture_id_out,
    const FlutterOpenGLTexture *initial_frame,
    bool delay_delete_to_next_page_flip
) {
    struct flutter_texture_frame *frame;
    struct flutter_texture *texture;
    FlutterEngineResult engine_result;
    int64_t texture_id;
    int ok;

    texture = malloc(sizeof *texture);
    if (texture == NULL) {
        return ENOMEM;
    }

    texture_id = reg->next_texture_id++;

    if (initial_frame == NULL) {
        frame = NULL;
    } else {
        printf("[texture registry] adding  gl texture %u  to  flutter texture %lld\n", initial_frame->name, texture_id);

        frame = frame_new(initial_frame);
        if (frame == NULL) {
            free(texture);
            return ENOMEM;
        }
    }

    texture->texture_id = texture_id;
    texture->frame = frame;

    ok = cpset_put(&reg->textures, texture);
    if (ok != 0) {
        if (frame != NULL)
            frame_unref(frame);
        free(texture);
        return ok;
    }

    if (texture_id_out != NULL) {
        *texture_id_out = texture_id;
    }

    engine_result = reg->register_texture(reg->engine, texture_id);
    if (engine_result != kSuccess) {
        fprintf(stderr, "[texture registry] Could not register external texture for engine. FlutterEngineRegisterExternalTexture: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        return EINVAL;
    }

    return 0;
}

int texreg_update_texture(
    struct texture_registry *reg,
    int64_t texture_id,
    const FlutterOpenGLTexture *new_gl_texture,
    bool delay_delete_to_next_page_flip
) {
    struct flutter_texture *texture;
    FlutterEngineResult engine_result;

    if (new_gl_texture) {
        printf("[texture registry] adding  gl texture %u  to  flutter texture %lld\n", new_gl_texture->name, texture_id);
    }

    cpset_lock(&reg->textures);

    texture = find_texture_by_id_locked(reg, texture_id);
    if (texture == NULL) {
        cpset_unlock(&reg->textures);
        return EINVAL;
    }

    engine_result = reg->mark_frame_available(reg->engine, texture_id);
    if (engine_result != kSuccess) {
        fprintf(stderr, "[texture registry] Could not update external texture. FlutterEngineMarkExternalTextureFrameAvailable: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
        cpset_unlock(&reg->textures);
        return EINVAL;
    }

    if (texture->frame != NULL) {
        frame_unrefp(&texture->frame);
    }

    if (new_gl_texture == NULL) {
        texture->frame = NULL;
    } else {
        texture->frame = frame_new(new_gl_texture);
    }

    cpset_unlock(&reg->textures);

    return 0;
}

int texreg_delete_texture(
    struct texture_registry *reg,
    int64_t texture_id
) {
    struct flutter_texture *texture;
    FlutterEngineResult engine_result;

    cpset_lock(&reg->textures);

    texture = find_texture_by_id_locked(reg, texture_id);
    if (texture == NULL) {
        cpset_unlock(&reg->textures);
        return EINVAL;
    }

    cpset_remove_locked(&reg->textures, texture);

    if (texture->frame != NULL) {
        frame_unrefp(&texture->frame);
    }
    
    free(texture);

    engine_result = reg->unregister_texture(reg->engine, texture_id);
    if (engine_result != kSuccess) {
        fprintf(stderr, "[texture registry] Could not unregister external texture. FlutterEngineUnregisterExternalTexture: %s\n", FLUTTER_RESULT_TO_STRING(engine_result));
    }

    cpset_unlock(&reg->textures);

    return 0;
}

int texreg_on_page_flip(
    struct texture_registry *reg
) {
    destroy_all_delete_delayed_textures(reg);
    return 0;
}

void texreg_destroy(
    struct texture_registry *reg
) {
    struct flutter_texture *texture;
    
    cpset_lock(&reg->textures);

    for_each_pointer_in_cpset(&reg->textures, texture) {
        cpset_remove_locked(&reg->textures, texture);

        if (texture->frame != NULL) {
            frame_unrefp(&texture->frame);
        }

        reg->unregister_texture(reg->engine, texture->texture_id);

        free(texture);

        texture = NULL;
    }

    cpset_unlock(&reg->textures);

    destroy_all_delete_delayed_textures(reg);

    free(reg);
}