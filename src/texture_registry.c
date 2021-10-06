#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>

#include <flutter_embedder.h>

#include <texture_registry.h>
#include <flutter-pi.h>

#define LOG_ERROR(...) fprintf(stderr, "[texture registry] " __VA_ARGS__)

struct {
    struct texture_map_entry *entries;
    size_t size_entries;
    size_t n_entries;
    int64_t last_id;
} texreg = {
    .entries = NULL,
    .size_entries = 0,
    .n_entries = 0,
    .last_id = 0
};

struct texture_registry {
    struct flutter_external_texture_interface texture_interface;
    pthread_mutex_t next_unused_id_mutex;
    int64_t next_unused_id;
    struct concurrent_pointer_set textures;
};

#define TEXTURE_N_FRAMES 4
#define TEXTURE_FRAME_INDEX_MASK 0b11
#define DEBUG_ASSERT_HAS_SPACE(texture) DEBUG_ASSERT((((texture)->end_frames + 1) & TEXTURE_FRAME_INDEX_MASK) != (texture)->start_frames)

struct counted_texture_frame {
    struct texture *texture;
    size_t n_uses;
    struct texture_frame frame;
};

struct texture {
    struct texture_registry *registry;

    pthread_mutex_t lock;

    int64_t id;
    
    // ring buffer of the frames
    struct counted_texture_frame frames[TEXTURE_N_FRAMES];

    // the index of the first frame inside [frames]
    size_t start_frames;
    
    // the index after the last frame inside [frames]
    size_t end_frames;

    // The frame that's scheduled to be displayed next.
    // The texture only holds a reference to this frame, not the frames inside the
    // frames ringbuffer.
    // If a new frame is scheduled while next_frame still was not acquired by the engine,
    // next_frame will be dereferenced (and since the use count is 1, it will then be destroyed).
    struct counted_texture_frame *next_frame;

    // userdata which will be passed to any frame destroy callback (as texture_userdata)
    void *userdata;
};

struct texture_registry *texture_registry_new(
    const struct flutter_external_texture_interface *texture_interface
) {
    struct texture_registry *reg;
    int ok;

    reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }

    pthread_mutex_init(&reg->next_unused_id_mutex, NULL);

    memcpy(&reg->texture_interface, texture_interface, sizeof(*texture_interface));
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

bool texture_gl_external_texture_frame_callback(
    struct texture *texture,
    size_t width,
    size_t height,
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
    result = texture_gl_external_texture_frame_callback(t, width, height, texture_out);
    cpset_unlock(&reg->textures);

    return result;
}

static int add_texture_details(const struct texture_details *details, int64_t *tex_id_out) {
    if (texreg.n_entries == texreg.size_entries) {
        // expand the texture map
        size_t new_size = texreg.size_entries? texreg.size_entries*2 : 1;
        
        struct texture_map_entry *new = realloc(texreg.entries, new_size*sizeof(struct texture_map_entry));

        if (new == NULL) {
            perror("[texture registry] Could not expand external texture map. realloc");
            return ENOMEM;
        }

        memset(new + texreg.size_entries, 0, (new_size - texreg.size_entries)*sizeof(struct texture_map_entry));

        texreg.entries = new;
        texreg.size_entries = new_size;
    }

    size_t index;
    for (index = 0; index < texreg.size_entries; index++) {
        if (texreg.entries[index].texture_id == 0) {
            break;
        }
    }

    texreg.entries[index].texture_id = ++(texreg.last_id);
    texreg.entries[index].details = *details;

    texreg.n_entries++;

    *tex_id_out = texreg.entries[index].texture_id;

    return 0;
}

static int remove_texture_details(int64_t tex_id) {
    size_t index;
    for (index = 0; index < texreg.size_entries; index++) {
        if (texreg.entries[index].texture_id == tex_id) {
            break;
        }
    }

    if (index == texreg.size_entries) {
        return EINVAL;
    }

    texreg.entries[index].texture_id = 0;

    texreg.n_entries--;

    return 0;
}

static void on_collect_texture(void *userdata) {
    struct texture_details *details = (struct texture_details *) userdata;

    if (details->collection_cb) {
        details->collection_cb(
            details->gl_texture.target,
            details->gl_texture.name,
            details->gl_texture.format,
            details->collection_cb_userdata,
            details->gl_texture.width,
            details->gl_texture.height
        );
    }

    //free(details);
}

bool texreg_gl_external_texture_frame_callback(
    void *userdata,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    printf("[texture registry] gl_external_texture_frame_callback(\n"
           "  userdata: %p,\n"
           "  texture_id: %"PRId64",\n"
           "  width: %"PRIu32",\n"
           "  height: %"PRIu32",\n"
           "  texture_out: %p\n"
           ");\n",
           userdata, texture_id, width, height, texture_out
    );
    size_t index;
    for (index = 0; index < texreg.size_entries; index++) {
        printf("texreg.entries[%zu].texture_id = %" PRId64 "\n", index, texreg.entries[index].texture_id);
        if (texreg.entries[index].texture_id == texture_id) {
            break;
        }
    }

    if (index == texreg.size_entries)
        return false;
    
    *texture_out = texreg.entries[index].details.gl_texture;

    printf("texture_out = {\n"
           "  .target = %"PRIu32",\n"
           "  .name = %"PRIu32",\n"
           "  .format = %"PRIu32",\n"
           "  .user_data = %p,\n"
           "  .destruction_callback = %p,\n"
           "  .width = %"PRIu32",\n"
           "  .height = %"PRIu32",\n"
           "}\n",
           texture_out->target,
           texture_out->name,
           texture_out->format,
           texture_out->user_data,
           texture_out->destruction_callback,
           texture_out->width,
           texture_out->height
    );

    return true;
}

int texreg_register_texture(
    GLenum gl_texture_target,
    GLuint gl_texture_id,
    GLuint gl_texture_format,
    void *userdata,
    texreg_collect_gl_texture_cb collection_cb,
    size_t width,
    size_t height,
    int64_t *texture_id_out
) {
    struct texture_details *details;
    FlutterEngineResult engine_result;
    int64_t tex_id = 0;
    int ok;

    printf("[texture registry] texreg_register_texture(\n"
           "  gl_texture_target: %"PRIu32 ",\n"
           "  gl_texture_id: %"PRIu32 ",\n"
           "  gl_texture_format: %"PRIu32 ",\n"
           "  userdata: %p,\n"
           "  collection_cb: %p,\n"
           "  width: %"PRIu32",\n"
           "  height: %"PRIu32",\n"
           ");\n",
           gl_texture_target,
           gl_texture_id,
           gl_texture_format,
           userdata,
           collection_cb,
           width,
           height
    );
    
    details = malloc(sizeof(struct texture_details));

    *details = (struct texture_details) {
        .gl_texture = {
            .target = (uint32_t) gl_texture_target,
            .name = (uint32_t) gl_texture_id,
            .format = (uint32_t) gl_texture_format,
            .user_data = details,
            .destruction_callback = on_collect_texture,
            .width = width,
            .height = height,
        },
        .collection_cb = collection_cb,
        .collection_cb_userdata = userdata
    };

    ok = add_texture_details(
        details,
        &tex_id
    );

    if (ok != 0) {
        free(details);
        return ok;
    }

    engine_result = flutterpi.flutter.libflutter_engine.FlutterEngineRegisterExternalTexture(flutterpi.flutter.engine, tex_id);
    if (engine_result != kSuccess) {
        free(details);
        return EINVAL;
    }

    *texture_id_out = tex_id;

    return 0;
}



static inline void texture_lock(struct texture *t) {
    pthread_mutex_lock(&t->lock);
}

static inline void texture_unlock(struct texture *t) {
    pthread_mutex_unlock(&t->lock);
}


static inline bool has_frame_locked(struct texture *t) {
    return t->start_frames != t->end_frames;
}

static inline void push_locked(struct texture *t, const struct counted_texture_frame frame) {
    DEBUG_ASSERT_HAS_SPACE(t);
    t->frames[t->end_frames] = frame;
    t->end_frames = (t->end_frames + 1) & TEXTURE_FRAME_INDEX_MASK;
}

static inline struct counted_texture_frame *peek_front_locked(struct texture *t) {
    DEBUG_ASSERT(has_frame_locked(t));
    return t->frames + t->start_frames;
}

static inline struct counted_texture_frame *peek_back_locked(struct texture *t) {
    DEBUG_ASSERT(has_frame_locked(t));
    return t->frames + t->end_frames - 1;
}

static inline void pop_front_locked(struct texture *t) {
    DEBUG_ASSERT(has_frame_locked(t));
    t->start_frames = (t->start_frames + 1) & TEXTURE_FRAME_INDEX_MASK;
}

static inline void pop_back_locked(struct texture *t) {
    DEBUG_ASSERT(has_frame_locked(t));
    t->end_frames = (t->end_frames - 1) & TEXTURE_FRAME_INDEX_MASK;   
}

static inline bool frame_deref_locked(struct counted_texture_frame *frame) {
    if ((frame->n_uses == 0) || (frame->n_uses == 1)) {
        frame->frame.destroy(frame->frame, frame->texture->userdata, frame->frame.frame_userdata);
        frame->n_uses = 0;
        return true;
    } else {
        frame->n_uses--;
        return false;
    }
}

static inline void deref_front_locked(struct texture *t) {
    if (frame_deref_locked(peek_front_locked(t))) {
        pop_front_locked(t);
    }
}

static inline void deref_back_locked(struct texture *t) {
    if (frame_deref_locked(peek_back_locked(t))) {
        pop_back_locked(t);
    }
}


struct texture *texreg_create_texture(
    struct texture_registry *reg,
    void *userdata
) {
    FlutterEngineResult engine_result;
    struct texture *t;
    int64_t id;

    t = malloc(sizeof *t);

    id = texture_registry_allocate_id(reg);

    engine_result = reg->texture_interface.register_external_texture(reg->texture_interface.engine, id);
    if (engine_result != kSuccess) {
        LOG_ERROR("Couldn't add texture with id %" PRId64 " to flutter engine: %s\n", id, FLUTTER_RESULT_TO_STRING(engine_result));
        free(t);
        return NULL;
    }
    
    pthread_mutex_init(&t->lock, NULL);
    t->registry = reg;
    t->id = id;
    memset(t->frames, 0, sizeof(t->frames));
    t->start_frames = 0;
    t->end_frames = 0;
    t->next_frame = NULL;
    t->userdata = userdata;
    return t;
}

int64_t texture_get_id(struct texture *texture) {
    return texture->id;
}

void texture_push_frame(
    struct texture *texture,
    const struct texture_frame *frame
) {
    texture_lock(texture);

    if (has_frame_locked(texture)) {
        deref_back_locked(texture);
    }

    push_locked(
        texture,
        (struct counted_texture_frame) {
            .texture = texture,
            .frame = *frame,
            .n_uses = 0
        }
    );
}

void texture_destroy(
    struct texture *texture
) {
    /// TODO: Implement
    (void) texture;
}

static void on_deref_frame(void *userdata) {
    struct counted_texture_frame *frame;

    DEBUG_ASSERT(userdata != NULL);

    frame = userdata;
    (void) frame;
    
    /// TODO: Implement
}

bool texture_gl_external_texture_frame_callback(
    struct texture *texture,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
) {
    (void) texture;
    (void) width;
    (void) height;
    (void) texture_out;

    /// TODO: Implement
    return false;
}


int texreg_mark_texture_frame_available(int64_t texture_id) {
    FlutterEngineResult engine_result;

    engine_result = flutterpi.flutter.libflutter_engine.FlutterEngineMarkExternalTextureFrameAvailable(flutterpi.flutter.engine, texture_id);
    if (engine_result != kSuccess) {
        return EINVAL;
    }

    return 0;
}

int texreg_unregister_texture(int64_t texture_id) {
    FlutterEngineResult engine_result;
    int ok;

    ok = remove_texture_details(texture_id);
    if (ok != 0) {
        return ok;
    }
    
    engine_result = flutterpi.flutter.libflutter_engine.FlutterEngineUnregisterExternalTexture(flutterpi.flutter.engine, texture_id);
    if (engine_result != kSuccess) {
        return EINVAL;
    }

    return 0;
}