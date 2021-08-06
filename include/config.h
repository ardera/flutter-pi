#ifndef _FLUTTERPI_INCLUDE_CONFIG_H
#define _FLUTTERPI_INCLUDE_CONFIG_H

#include <stdlib.h>

struct kms_output_config {
    const char *connector_name;

    const char *mode;
    
    const char *framebuffer_size;

    bool has_clones;
    size_t n_clones;
    const char **clones;

    bool has_width_mm;
    int width_mm;

    bool has_height_mm;
    int height_mm;
};

struct kms_config {
    const char *device_path;
    bool has_use_hwcursor, use_hwcursor;
    size_t n_output_configs;
    struct kms_output_config **output_configs;
};

struct fbdev_config {
    bool has_width_mm;
    int width_mm;

    bool has_height_mm;
    int height_mm;
};

struct headless_config {
    bool has_width_mm;
    int width_mm;

    bool has_height_mm;
    int height_mm;

    int width, height;
};

struct config {
    size_t n_kms_configs;
    struct kms_config **kms_configs;
    size_t n_fbdev_configs;
    struct fbdev_config **fbdev_configs;
    size_t n_headless_configs;
    struct headless_config **headless_configs;
};

struct config *config_new_empty();
struct config *config_new_from_json(const char *json);
struct config *config_new_from_file(const char *path);

struct kms_config *kms_config_new_empty();
struct kms_config *kms_config_new_from_json(const char *json);

struct kms_output_config *kms_output_config_new_empty();
struct kms_output_config *kms_output_config_new_from_json(const char *json);

struct fbdev_config *fbdev_config_new_empty();
struct fbdev_config *fbdev_config_new_from_json(const char *json);

struct headless_config *headless_config_new_empty();
struct headless_config *headless_config_new_from_json(const char *json);

#endif // _FLUTTERPI_INCLUDE_CONFIG_H