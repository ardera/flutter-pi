// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <stdatomic.h>

#include <fcntl.h>

#include <unity.h>

#include "../src/plugins/gstreamer_video_player/frame.c"

struct mock_bo {
    unsigned char bytes[256];
};
static atomic_int active_uploads, overlapping_uploads;
static atomic_int creates, destroys, exports;
static int fail_create, fail_map, fail_export, fail_image, buffers_destroyed;
static int image_destroyed, textures_deleted;
static struct gbm_device *device = (struct gbm_device *) 1;

struct gbm_bo *__wrap_gbm_bo_create(struct gbm_device *dev, uint32_t width, uint32_t height, uint32_t format, uint32_t flags) {
    (void) dev;
    (void) width;
    (void) height;
    (void) format;
    (void) flags;
    atomic_fetch_add(&creates, 1);
    if (fail_create)
        return NULL;
    if (atomic_fetch_add(&active_uploads, 1) != 0)
        atomic_fetch_add(&overlapping_uploads, 1);
    // Enlarge the overlap window without assuming thread scheduling order.
    usleep(1000);
    return (struct gbm_bo *) calloc(1, sizeof(struct mock_bo));
}

void *__wrap_gbm_bo_map(
    struct gbm_bo *bo,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t flags,
    uint32_t *stride,
    void **map_data
) {
    (void) x;
    (void) y;
    (void) height;
    (void) flags;
    *stride = width;
    *map_data = bo;
    return fail_map ? NULL : ((struct mock_bo *) bo)->bytes;
}
void __wrap_gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
    (void) bo;
    (void) map_data;
}
int __wrap_gbm_bo_get_fd(struct gbm_bo *bo) {
    (void) bo;
    atomic_fetch_add(&exports, 1);
    return fail_export ? -1 : open("/dev/null", O_RDONLY | O_CLOEXEC);
}
void __wrap_gbm_bo_destroy(struct gbm_bo *bo) {
    free(bo);
    atomic_fetch_add(&destroys, 1);
    atomic_fetch_sub(&active_uploads, 1);
}
EGLBoolean __wrap_eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
    (void) display;
    (void) draw;
    (void) read;
    (void) context;
    return EGL_TRUE;
}
void __wrap_glGenTextures(GLsizei n, GLuint *textures) {
    (void) n;
    *textures = 1;
}
void __wrap_glBindTexture(GLenum target, GLuint texture) {
    (void) target;
    (void) texture;
}
GLenum __wrap_glGetError(void) {
    return GL_NO_ERROR;
}
void __wrap_glDeleteTextures(GLsizei n, const GLuint *textures) {
    (void) n;
    (void) textures;
    textures_deleted++;
}
static EGLImageKHR create_image(EGLDisplay display, EGLContext context, EGLenum target, EGLClientBuffer buffer, const EGLint *attrs) {
    (void) display;
    (void) context;
    (void) target;
    (void) buffer;
    (void) attrs;
    return fail_image ? EGL_NO_IMAGE_KHR : (EGLImageKHR) 1;
}
static EGLBoolean destroy_image(EGLDisplay display, EGLImageKHR image) {
    (void) display;
    (void) image;
    image_destroyed++;
    return EGL_TRUE;
}
static void attach_image(GLenum target, GLeglImageOES image) {
    (void) target;
    (void) image;
}
static void on_buffer_destroy(gpointer data, GstMiniObject *object) {
    (void) data;
    (void) object;
    buffers_destroyed++;
}

void setUp(void) {
    active_uploads = overlapping_uploads = creates = destroys = exports = 0;
    fail_create = fail_map = fail_export = fail_image = buffers_destroyed = 0;
    image_destroyed = textures_deleted = 0;
}
void tearDown(void) {
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&active_uploads));
}

static struct frame_interface *make_interface(void) {
    struct frame_interface *interface = calloc(1, sizeof *interface);
    interface->gbm_device = device;
    interface->formats = malloc(sizeof *interface->formats);
    *interface->formats = (struct egl_modified_format){ DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, false };
    interface->n_formats = 1;
    interface->n_refs = REFCOUNT_INIT_1;
    interface->eglCreateImageKHR = create_image;
    interface->eglDestroyImageKHR = destroy_image;
    interface->glEGLImageTargetTexture2DOES = attach_image;
    pthread_mutex_init(&interface->context_lock, NULL);
    return interface;
}
static void free_interface(struct frame_interface *interface) {
    // No real EGL context was created by this test.
    pthread_mutex_destroy(&interface->context_lock);
    free(interface->formats);
    free(interface);
}

void test_frame_retains_pixels_without_retaining_appsink_sample(void) {
    struct frame_interface *interface = make_interface();
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_RGBA, 4, 4);
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, info.size, NULL);
    gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), on_buffer_destroy, NULL);
    GstSample *sample = gst_sample_new(buffer, NULL, NULL, NULL);
    gst_buffer_unref(buffer);
    struct video_frame *frame = frame_new(interface, sample, &info);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE(gst_sample_is_writable(sample));
    int fd = frame->dmabuf_fds[0];
    TEST_ASSERT_NOT_EQUAL(-1, fcntl(fd, F_GETFD));
    gst_sample_unref(sample);
    TEST_ASSERT_EQUAL_INT(0, buffers_destroyed);
    frame_destroy(frame);
    TEST_ASSERT_EQUAL_INT(1, buffers_destroyed);
    TEST_ASSERT_EQUAL_INT(-1, fcntl(fd, F_GETFD));
    TEST_ASSERT_EQUAL_INT(1, image_destroyed);
    TEST_ASSERT_EQUAL_INT(1, textures_deleted);
    free_interface(interface);
}

void test_upload_failures_release_bo_and_allow_the_next_upload(void) {
    GstMemory *memory = gst_allocator_alloc(NULL, 64, NULL);
    for (int failure = 0; failure < 3; failure++) {
        fail_create = failure == 0;
        fail_map = failure == 1;
        fail_export = failure == 2;
        TEST_ASSERT_EQUAL_INT(-1, dup_gst_memory_as_dmabuf(device, memory));
        TEST_ASSERT_EQUAL_INT(0, atomic_load(&active_uploads));
        fail_create = fail_map = fail_export = 0;
        int fd = dup_gst_memory_as_dmabuf(device, memory);
        TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
        close(fd);
    }
    gst_memory_unref(memory);
    TEST_ASSERT_EQUAL_INT(5, atomic_load(&destroys));
}

static void *upload_thread(void *arg) {
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 64, NULL);
    for (int i = 0; i < 30; i++) {
        int fd = arg ? dup_gst_buffer_range_as_dmabuf(device, buffer, 0, 1) :
                       dup_gst_memory_as_dmabuf(device, gst_buffer_peek_memory(buffer, 0));
        if (fd >= 0)
            close(fd);
    }
    gst_buffer_unref(buffer);
    return NULL;
}
void test_players_and_both_upload_paths_share_one_gbm_critical_section(void) {
    pthread_t a, b;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&a, NULL, upload_thread, NULL));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&b, NULL, upload_thread, (void *) 1));
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&overlapping_uploads));
    TEST_ASSERT_EQUAL_INT(60, atomic_load(&destroys));
}

void test_partial_plane_failure_closes_every_previously_exported_fd(void) {
    GstVideoInfo info;
    gst_video_info_set_format(&info, GST_VIDEO_FORMAT_I420, 4, 4);
    GstBuffer *buffer = gst_buffer_new();
    // First plane exists, second doesn't. Its fd must be closed on failure.
    gst_buffer_append_memory(buffer, gst_allocator_alloc(NULL, 16, NULL));
    struct plane_info planes[MAX_N_PLANES] = { 0 };
    TEST_ASSERT_NOT_EQUAL(0, get_plane_infos(buffer, &info, device, planes));
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&exports));
    TEST_ASSERT_EQUAL_INT(-1, fcntl(planes[0].fd, F_GETFD));
    gst_buffer_unref(buffer);
}

int main(void) {
    gst_init(NULL, NULL);
    UNITY_BEGIN();
    RUN_TEST(test_frame_retains_pixels_without_retaining_appsink_sample);
    RUN_TEST(test_upload_failures_release_bo_and_allow_the_next_upload);
    RUN_TEST(test_players_and_both_upload_paths_share_one_gbm_critical_section);
    RUN_TEST(test_partial_plane_failure_closes_every_previously_exported_fd);
    int result = UNITY_END();
    gst_deinit();
    return result;
}
