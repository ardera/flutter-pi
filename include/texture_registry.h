#ifndef _TEXTURE_REGISTRY_H
#define _TEXTURE_REGISTRY_H

#include <stdint.h>
#include <flutter_embedder.h>

struct texture_registry;

typedef FlutterEngineResult (*flutter_engine_register_external_texture_t)(FlutterEngine engine, int64_t texture_identifier);

typedef FlutterEngineResult (*flutter_engine_mark_external_texture_frame_available_t)(FlutterEngine engine, int64_t texture_identifier);

typedef FlutterEngineResult (*flutter_engine_unregister_external_texture_t)(FlutterEngine engine, int64_t texture_identifier);

struct texture_registry *texreg_new(
    FlutterEngine engine,
    flutter_engine_register_external_texture_t register_texture,
    flutter_engine_mark_external_texture_frame_available_t mark_frame_available,
    flutter_engine_unregister_external_texture_t unregister_texture
);

void texreg_set_engine(
    struct texture_registry *reg,
    FlutterEngine engine
);

void texreg_set_callbacks(
    struct texture_registry *reg,
    flutter_engine_register_external_texture_t register_texture,
    flutter_engine_mark_external_texture_frame_available_t mark_frame_available,
    flutter_engine_unregister_external_texture_t unregister_texture
);

int texreg_on_external_texture_frame_callback(
    struct texture_registry *reg,
    int64_t texture_id,
    size_t width,
    size_t height,
    FlutterOpenGLTexture *texture_out
);

int texreg_new_texture(
    struct texture_registry *reg,
    int64_t *texture_id_out,
    const FlutterOpenGLTexture *initial_frame,
    bool delay_delete_to_next_page_flip
);

int texreg_update_texture(
    struct texture_registry *reg,
    int64_t texture_id,
    const FlutterOpenGLTexture *new_gl_texture,
    bool delay_delete_to_next_page_flip
);

int texreg_delete_texture(
    struct texture_registry *reg,
    int64_t texture_id
);

int texreg_on_page_flip(
    struct texture_registry *reg
);

void texreg_destroy(
    struct texture_registry *reg
);

#endif