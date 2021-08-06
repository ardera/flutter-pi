#include <config.h>

struct config *config_new_empty() {
    struct config *config;
    
    config = malloc(sizeof *config);
    if (config == NULL) {
        goto fail_return_null;
    }

    config->n_kms_configs = 0;
    config->kms_configs = NULL;
    config->n_fbdev_configs = 0;
    config->fbdev_configs = NULL;
    config->n_headless_configs = 0;
    config->headless_configs = NULL;
    
    return config;
    
    free(config);
    fail_return_null:
    return NULL;
}

struct config *config_new_from_json(const char *json) {
    struct config *config;

    config = config_new_empty();
    if (config == NULL) {
        return NULL;
    }

    /// TODO: Parse json

    return config;
}

struct config *config_new_from_file(const char *path) {
    struct config *config;
    const char *json;

    /// TODO: Load json from file

    config = config_new_from_json(json);

    free(json);

    return config;
}

struct kms_config *kms_config_new_empty() {
    struct kms_config *config;
    
    config = malloc(sizeof *config);
    if (config == NULL) {
        goto fail_return_null;
    }

    config->device_path = NULL;
    config->has_use_hwcursor = false;
    config->use_hwcursor = false;
    config->n_output_configs = 0;
    config->output_configs = NULL;
    return config;
    
    
    free(config);
    fail_return_null:
    return NULL;
}

struct kms_config *kms_config_new_from_json(const char *json) {
    struct kms_config *config;

    config = kms_config_new_empty();
    if (config == NULL) {
        return NULL;
    }

    /// TODO: Parse json

    return config;
}

struct kms_output_config *kms_output_config_new_empty() {
    struct kms_output_config *config;
    
    config = malloc(sizeof *config);
    if (config == NULL) {
        goto fail_return_null;
    }

    config->connector_name = NULL;
    config->mode = NULL;
    config->framebuffer_size = NULL;
    config->has_clones = false;
    config->n_clones = 0;
    config->clones = NULL;
    config->has_width_mm = false;
    config->width_mm = 0;
    config->has_height_mm = false;
    config->height_mm = 0; 
    return config;
    
    free(config);
    fail_return_null:
    return NULL;
}

struct kms_output_config *kms_output_config_new_from_json(const char *json) {
    struct kms_output_config *config;

    config = kms_output_config_new_empty();
    if (config == NULL) {
        return NULL;
    }

    /// TODO: Parse json

    return config;
}

struct fbdev_config *fbdev_config_new_empty() {
    struct fbdev_config *config;
    
    config = malloc(sizeof *config);
    if (config == NULL) {
        goto fail_return_null;
    }

    config->has_width_mm = false;
    config->width_mm = 0;
    config->has_height_mm = false;
    config->height_mm = 0;
    
    return config;
    
    free(config);
    fail_return_null:
    return NULL;
}

struct fbdev_config *fbdev_config_new_from_json(const char *json) {
    struct fbdev_config *config;

    config = fbdev_config_new_empty();
    if (config == NULL) {
        return NULL;
    }

    /// TODO: Parse json

    return config;
}

struct headless_config *headless_config_new_empty() {
    struct headless_config *config;
    
    config = malloc(sizeof *config);
    if (config == NULL) {
        goto fail_return_null;
    }

    config->has_width_mm = false;
    config->width_mm = 0;
    config->has_height_mm = false;
    config->height_mm = 0;
    
    return config;
    
    free(config);
    fail_return_null:
    return NULL;
}

struct headless_config *headless_config_new_from_json(const char *json) {
    struct headless_config *config;

    config = headless_config_new_empty();
    if (config == NULL) {
        return NULL;
    }

    /// TODO: Parse json

    return config;
}
