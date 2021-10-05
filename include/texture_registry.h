#ifndef _TEXTURE_REGISTRY_H
#define _TEXTURE_REGISTRY_H

#include <GLES2/gl2.h>
#include <flutter_embedder.h>

typedef int (*texreg_collect_gl_texture_cb)(
    GLenum gl_texture_target,
    GLuint gl_texture_id,
    GLuint gl_texture_format,
    void *userdata,
    size_t width,
    size_t height
);

struct texture_details {
    FlutterOpenGLTexture gl_texture;
    texreg_collect_gl_texture_cb collection_cb;
    void *collection_cb_userdata;
};

struct texture_map_entry {
    int64_t texture_id;
    struct texture_details details;
};

struct flutter_external_texture_interface {
    FlutterEngineRegisterExternalTextureFnPtr register_external_texture;
    FlutterEngineUnregisterExternalTextureFnPtr unregister_external_texture;
    FlutterEngineMarkExternalTextureFrameAvailableFnPtr mark_external_texture_frame_available;
    FlutterEngine engine;
};

extern bool texreg_gl_external_texture_frame_callback(
    void *userdata,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
);

extern int texreg_register_texture(
    GLenum gl_texture_target,
    GLuint gl_texture_id,
    GLuint gl_texture_format,
    void *userdata,
    texreg_collect_gl_texture_cb collection_cb,
    size_t width,
    size_t height,
    int64_t *texture_id_out
);

extern int texreg_mark_texture_frame_available(int64_t texture_id);

extern int texreg_unregister_texture(int64_t texture_id);


struct texture_registry;
struct texture;

struct gl_texture_frame {
    GLenum target;
    GLuint name;
    GLuint format;
    size_t width;
    size_t height;
};

struct texture_frame {
    union {
        struct gl_texture_frame gl;
    };
    void (*destroy)(struct texture_frame frame, void *texture_userdata, void *frame_userdata);
    void *frame_userdata;
};

struct texture_registry *texture_registry_new(const struct flutter_external_texture_interface *texture_interface);

void texture_registry_destroy(struct texture_registry *reg);

bool texture_registry_gl_external_texture_frame_callback(
    struct texture_registry *reg,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
);

struct texture *texreg_create_texture(
    struct texture_registry *reg,
    void *userdata
);

int64_t texture_get_id(struct texture *texture);

void texture_push_frame(
    struct texture *texture,
    const struct texture_frame *frame
);

void texture_destroy(
    struct texture *texture
); 

#endif