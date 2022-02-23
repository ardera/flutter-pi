// SPDX-License-Identifier: MIT
/*
 * compositor-ng
 *
 * - a reimplementation of the flutter compositor
 * - takes flutter layers as input, composits them into multiple hw planes, outputs them to the modesetting interface
 *
 * Copyright (c) 2022, Hannes Winkler <hanneswinkler2000@web.de>
 */


#include <stdlib.h>

#include <collection.h>

#include <flutter_embedder.h>

struct compositor {
    
};

struct layer {
    
};

struct compositor *compositor_new() {
    struct compositor *compositor;
    
    compositor = malloc(sizeof *compositor);
    if (compositor == NULL) {
        goto fail_return_null;
    }
    
    return compositor;
    
    
    fail_free_compositor:
    free(compositor);
    
    fail_return_null:
    return NULL;
}

void compositor_destroy(struct compositor *compositor) {
    free(compositor);
}


