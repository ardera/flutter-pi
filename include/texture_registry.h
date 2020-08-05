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

#endif