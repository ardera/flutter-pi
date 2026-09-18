// SPDX-License-Identifier: MIT
// Standalone test: compile with -ffunction-sections -fdata-sections,
// -Wl,--gc-sections, the project include paths, and pkg-config libdrm gbm.
// Only the DRM submission/event boundary is mocked; no display is accessed.
#define drmModeAtomicCommit test_atomic_commit
#define drmAuthMagic test_auth_magic
#define drmHandleEvent test_handle_event
#include "../src/modesetting.c"

static void *submitted;
static unsigned int submissions;
static int submit_error;
static unsigned int releases[10];
static bool draining;

int test_auth_magic(int fd, drm_magic_t magic) {
    (void) fd;
    (void) magic;
    return 0;
}

int test_atomic_commit(int fd, drmModeAtomicReq *req, uint32_t flags, void *userdata) {
    (void) fd;
    (void) req;
    assert(flags & DRM_MODE_ATOMIC_NONBLOCK);
    assert(flags & DRM_MODE_PAGE_FLIP_EVENT);
    assert(submitted == NULL);
    if (submit_error) {
        errno = submit_error;
        return -1;
    }
    submitted = userdata;
    submissions++;
    return 0;
}

int test_handle_event(int fd, drmEventContextPtr ctx) {
    // Asynchronous presentation must never read/wait for an event inline.
    assert(draining && submitted != NULL);
    void *req = submitted;
    submitted = NULL;
    ctx->page_flip_handler2(fd, 1, 1, 0, 1, req);
    return 0;
}

static void close_device(int fd, void *metadata, void *userdata) {
    (void) fd;
    (void) metadata;
    struct drmdev *dev = userdata;
    assert(!dev->per_crtc_state[0].flip_pending);
    assert(dev->per_crtc_state[0].queued == NULL);
}

static void release_layer(void *userdata) {
    (*(unsigned int *) userdata)++;
}

static struct kms_req *request(struct drmdev *dev, unsigned int index) {
    struct kms_req_builder *builder = calloc(1, sizeof *builder);
    assert(builder != NULL);
    builder->n_refs = REFCOUNT_INIT_1;
    builder->drmdev = drmdev_ref(dev);
    builder->crtc = dev->crtcs;
    builder->connector = dev->connectors;
    builder->req = drmModeAtomicAlloc();
    assert(builder->req != NULL);
    builder->n_layers = 1;
    builder->layers[0].plane = dev->planes;
    builder->layers[0].release_callback = release_layer;
    builder->layers[0].release_callback_userdata = &releases[index];
    return (struct kms_req *) builder;
}

static void present(struct drmdev *dev, unsigned int index, int expected) {
    struct kms_req *req = request(dev, index);
    assert(kms_req_present(req) == expected);
    kms_req_unref(req);
}

static void flip(struct drmdev *dev) {
    void *req = submitted;
    assert(req != NULL);
    submitted = NULL;
    drmdev_lock(dev);
    drmdev_on_page_flip_locked(0, 1, 1, 0, dev->crtcs->id, req);
    drmdev_unlock(dev);
}

int main(void) {
    struct drm_crtc crtc = { .id = 1, .index = 0 };
    struct drm_connector connector = {0};
    struct drm_plane plane = {0};
    struct drmdev dev = {
        .n_refs = REFCOUNT_INIT_1,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .master_fd = 1,
        .n_crtcs = 1,
        .crtcs = &crtc,
        .connectors = &connector,
        .planes = &plane,
        .interface.close = close_device,
    };
    dev.userdata = &dev;

    present(&dev, 0, 0);
    assert(submissions == 1 && releases[0] == 0);
    present(&dev, 1, 0);
    present(&dev, 2, 0);
    assert(submissions == 1 && releases[1] == 1 && releases[2] == 0);
    struct kms_req *extra = request(&dev, 9);
    assert(kms_req_commit_nonblocking(extra, NULL, NULL, NULL) == EBUSY);
    kms_req_unref(extra);
    flip(&dev);
    assert(submissions == 2 && releases[0] == 0);
    flip(&dev);
    assert(releases[0] == 1 && releases[2] == 0);
    assert(!dev.per_crtc_state[0].flip_pending && dev.per_crtc_state[0].queued == NULL);

    // A failed submission must release its buffers and allow the next frame.
    submit_error = EINVAL;
    present(&dev, 3, EINVAL);
    assert(releases[3] == 1 && !dev.per_crtc_state[0].flip_pending);
    submit_error = 0;
    present(&dev, 4, 0);
    present(&dev, 5, 0);
    submit_error = EBUSY;
    flip(&dev);
    assert(releases[2] == 1 && releases[5] == 1);
    assert(!dev.per_crtc_state[0].flip_pending && dev.per_crtc_state[0].queued == NULL);
    submit_error = 0;
    present(&dev, 6, 0);
    flip(&dev);
    assert(releases[4] == 1 && releases[6] == 0);

    // Suspending cancels the queued frame and consumes the outstanding flip.
    present(&dev, 7, 0);
    present(&dev, 8, 0);
    draining = true;
    drmdev_suspend(&dev);
    draining = false;
    assert(dev.master_fd == -1 && submitted == NULL);
    assert(releases[6] == 1 && releases[7] == 0 && releases[8] == 1);
    present(&dev, 9, EBUSY);
    assert(releases[9] == 2);

    kms_req_swap_ptrs(&dev.per_crtc_state[0].last_flipped, NULL);
    assert(releases[7] == 1 && refcount_is_one(&dev.n_refs));
    pthread_mutex_destroy(&dev.mutex);
    puts("KMS presentation tests passed");
    return 0;
}
