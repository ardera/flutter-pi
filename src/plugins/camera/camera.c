#include <stdlib.h>

struct camera_mgr {

};

struct camera_list {
    int n_descriptions;
    struct camera_description *descriptions;
};

struct camera_description {
    char *name;
};

struct camera_list *camera_mgr_get_camera_list() {

}


struct camera_description *camera_list_peek_description(struct camera_list *list, int index) {
    
}

int camera_list_get_n_descriptions(struct camera_list *list) {
    /// TODO:
    // DEBUG_ASSERT_NOT_NULL(list);
    return list->n_descriptions;
}

void camera_list_free(struct camera_list *list) {
    
}

struct camera_description *camera_list_peek_description_after(struct camera_list *list, struct camera_description *descr) {
    for (int i = 0; i < list->n_descriptions; i++) {
        if (descr == NULL) {
            return list->descriptions + i;
        } else if (descr == list->descriptions + i) {
            descr = NULL;
        }
    }
    return NULL;
}

#define NULL ((void*) 0)

#define for_each_description_in_camera_list(list, descr) for (struct camera_description *descr = camera_list_peek_description_after(list, NULL); descr != NULL; descr = camera_list_peek_description_after(list, descr))
