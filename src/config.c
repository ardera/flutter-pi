#include "../include/thirdparty/cJSON.h"

#include <stdio.h>
#include <stdbool.h>
/*
    Example JSON config:
    {
    "enableHwCursor": true,
    "windows": [
        {
        "drmdev": "/dev/dri/card0",
        "mode": "1920x1080@60p",
        "dimensions": "160x90",
        "framebufferSize": "960x540",
        "pixelformat": "RGB565"
        },
        { ... }
    ]
    }
*/

struct window {
    char *drmdev;
    char *mode;
    char *dimensions;
    char *framebufferSize;
    char *pixelformat;
};

struct configuration {
    bool enableHwCursor;
    struct window *windows;
    size_t n_windows;
};

struct configuration *configuration_new(void) {
    struct configuration *c;
    c = malloc(sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    c->enableHwCursor = false;
    c->windows = NULL;
    c->n_windows = 0;
    return c;
}

void configuration_destroy(struct configuration *c) {
    size_t index;
    for (index = 0; index < c->n_windows; index++) {
        free(c->windows[index].drmdev);
        free(c->windows[index].mode);
        free(c->windows[index].dimensions);
        free(c->windows[index].framebufferSize);
        free(c->windows[index].pixelformat);
    }
    free(c->windows);
    free(c);
}

void configuration_dump(struct configuration *c) {
    size_t window;
    for (window = 0; window < c->n_windows; window++) {
        printf("%s %s %s %s %s\n", c->windows[window].drmdev, c->windows[window].mode, c->windows[window].dimensions, c->windows[window].framebufferSize, c->windows[window].pixelformat);
    }
}

struct configuration *configuration_parse(const char *json) {
    cJSON *root, *windows, *window, *item;
    struct configuration *c;
    size_t index;
    char *value;
    root = cJSON_Parse(json);
    if (root == NULL) {
        return NULL;
    }
    c = configuration_new();
    if (c == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    item = cJSON_GetObjectItem(root, "enableHwCursor");
    if (item != NULL && cJSON_IsBool(item)) {
        c->enableHwCursor = cJSON_IsTrue(item);
    }
    windows = cJSON_GetObjectItem(root, "windows");
    if (windows != NULL) {
        c->n_windows = cJSON_GetArraySize(windows);
        c->windows = malloc(c->n_windows * sizeof *c->windows);
        if (c->windows == NULL) {
            configuration_destroy(c);
            cJSON_Delete(root);
            return NULL;
        }
        for (index = 0; index < c->n_windows; index++) {
            window = cJSON_GetArrayItem(windows, index);
            c->windows[index].drmdev = NULL;
            c->windows[index].mode = NULL;
            c->windows[index].dimensions = NULL;
            c->windows[index].framebufferSize = NULL;
            c->windows[index].pixelformat = NULL;
            item = cJSON_GetObjectItem(window, "drmdev");
            if (item != NULL) {
                value = item->valuestring;
                c->windows[index].drmdev = strdup(value);
            }
            item = cJSON_GetObjectItem(window, "mode");
            if (item != NULL) {
                value = item->valuestring;
                c->windows[index].mode = strdup(value);
            }
            item = cJSON_GetObjectItem(window, "dimensions");
            if (item != NULL) {
                value = item->valuestring;
                c->windows[index].dimensions = strdup(value);
            }
            item = cJSON_GetObjectItem(window, "framebufferSize");
            if (item != NULL) {
                value = item->valuestring;
                c->windows[index].framebufferSize = strdup(value);
            }
            item = cJSON_GetObjectItem(window, "pixelformat");
            if (item != NULL) {
                value = item->valuestring;
                c->windows[index].pixelformat = strdup(value);
            }
        }
    }
    cJSON_Delete(root);
    return c;
}


#ifdef TEST
int main(int argc, char *argv[]) {
    struct configuration *c;
    c = configuration_parse(argv[1]);
    configuration_dump(c);
    configuration_destroy(c);
    return 0;
}
#endif

// Load config json from path
struct configuration *configuration_load(const char *path) {
    FILE *f;
    char *json;
    size_t size;
    struct configuration *c;
    f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    json = malloc(size + 1);
    if (json == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(json, 1, size, f) != size) {
        fclose(f);
        free(json);
        return NULL;
    }
    json[size] = '\0';
    fclose(f);
    c = configuration_parse(json);
    free(json);
    return c;
}

// xdg path
char *configuration_path(void) {
    char *path;
    path = getenv("XDG_CONFIG_HOME");
    if (path == NULL) {
        path = getenv("HOME");
        if (path == NULL) {
            return NULL;
        }
        path = malloc(strlen(path) + strlen("/.config/") + 1);
        if (path == NULL) {
            return NULL;
        }
        strcpy(path, path);
        strcat(path, "/.config/");
    } else {
        path = malloc(strlen(path) + strlen("/config.json") + 1);
        if (path == NULL) {
            return NULL;
        }
        strcpy(path, path);
        strcat(path, "/config.json");
    }
    return path;
}