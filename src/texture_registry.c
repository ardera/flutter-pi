#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>

#include <flutter_embedder.h>

#include <texture_registry.h>
#include <flutter-pi.h>

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

static int add_texture_details(const struct texture_details const *details, int64_t *tex_id_out) {
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
           "  texture_id: %"PRIi64",\n"
           "  width: %"PRIu32",\n"
           "  height: %"PRIu32",\n"
           "  texture_out: %p\n"
           ");\n",
           userdata, texture_id, width, height, texture_out
    );
    size_t index;
    for (index = 0; index < texreg.size_entries; index++) {
        printf("texreg.entries[%lu].texture_id = %lu\n", index, texreg.entries[index].texture_id);
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
}