#ifndef _FLUTTERPI_SRC_MODESETTING_REQ_BUILDER_H_
#define _FLUTTERPI_SRC_MODESETTING_REQ_BUILDER_H_

#include <stdbool.h>
#include <stdint.h>

#include "resources.h"

struct kms_fb_layer {
    uint32_t drm_fb_id;
    enum pixfmt format;
    bool has_modifier;
    uint64_t modifier;

    int32_t src_x, src_y, src_w, src_h;
    int32_t dst_x, dst_y, dst_w, dst_h;

    bool has_rotation;
    drm_plane_transform_t rotation;

    bool has_in_fence_fd;
    int in_fence_fd;

    bool prefer_cursor;
};

typedef void (*kmsreq_scanout_cb_t)(uint64_t vblank_ns, void *userdata);

typedef void (*kmsreq_syncfile_cb_t)(void *userdata, int syncfile_fd);


struct drmdev;
struct kms_req_builder;

struct kms_req_builder *kms_req_builder_new_atomic(struct drmdev *drmdev, struct drm_resources *resources, uint32_t crtc_id);

struct kms_req_builder *kms_req_builder_new_legacy(struct drmdev *drmdev, struct drm_resources *resources, uint32_t crtc_id);

DECLARE_REF_OPS(kms_req_builder)

struct drmdev *kms_req_builder_get_drmdev(struct kms_req_builder *builder);

struct drmdev *kms_req_builder_peek_drmdev(struct kms_req_builder *builder);

struct drm_resources *kms_req_builder_get_resources(struct kms_req_builder *builder);

struct drm_resources *kms_req_builder_peek_resources(struct kms_req_builder *builder);

/**
 * @brief Gets the CRTC associated with this KMS request builder.
 * 
 * @param builder The KMS request builder.
 * @returns The CRTC associated with this KMS request builder.
 */
struct drm_crtc *kms_req_builder_get_crtc(struct kms_req_builder *builder);

/**
 * @brief Adds a property to the KMS request that will set the given video mode
 * on this CRTC on commit, regardless of whether the currently committed output
 * mode is the same.
 * 
 * @param builder The KMS request builder.
 * @param mode The output mode to set (on @ref kms_req_commit)
 * @returns Zero if successful, positive errno-style error on failure.
 */
int kms_req_builder_set_mode(struct kms_req_builder *builder, const drmModeModeInfo *mode);

/**
 * @brief Adds a property to the KMS request that will unset the configured
 * output mode for this CRTC on commit, regardless of whether the currently
 * committed output mdoe is already unset.
 * 
 * @param builder The KMS request builder.
 * @returns Zero if successful, positive errno-style error on failure.
 */
int kms_req_builder_unset_mode(struct kms_req_builder *builder);

/**
 * @brief Adds a property to the KMS request that will change the connector
 * that this CRTC is displaying content on to @param connector_id.
 * 
 * @param builder The KMS request builder.
 * @param connector_id The connector that this CRTC should display contents on.
 * @returns Zero if successful, EINVAL if the @param connector_id is invalid.
 */
int kms_req_builder_set_connector(struct kms_req_builder *builder, uint32_t connector_id);

/**
 * @brief True if the next layer pushed using @ref kms_req_builder_push_fb_layer
 * should be opaque, i.e. use a framebuffer which has a pixel format that has no
 * alpha channel.
 * 
 * This is true for the bottom-most layer. There are some display controllers
 * that don't support non-opaque pixel formats for the bottom-most (primary)
 * plane. So ignoring this might lead to an EINVAL on commit.
 * 
 * @param builder The KMS request builder.
 * @returns True if the next layer should preferably be opaque, false if there's
 *          no preference.
 */
bool kms_req_builder_prefer_next_layer_opaque(struct kms_req_builder *builder);

/**
 * @brief Adds a new framebuffer (display) layer on top of the last layer.
 * 
 * If this is the first layer, the framebuffer should cover the entire screen
 * (CRTC).
 * 
 * To allow the use of explicit fencing, specify an in_fence_fd in @param layer
 * and a @param deferred_release_callback.
 * 
 * If explicit fencing is supported:
 *   - the in_fence_fd should be a DRM syncobj fd that signals
 *     when the GPU has finished rendering to the framebuffer and is ready
 *     to be scanned out.
 *   - @param deferred_release_callback will be called
 *     with a DRM syncobj fd that is signaled once the framebuffer is no longer
 *     being displayed on screen (and can be rendered into again)
 * 
 * If explicit fencing is not supported:
 *   - the in_fence_fd in @param layer will be closed by this procedure.
 *   - @param deferred_release_callback will NOT be called and
 *     @param release_callback will be called instead.
 * 
 * Explicit fencing is supported: When atomic modesetting is being used and
 * the driver supports it. (Driver has IN_FENCE_FD plane and OUT_FENCE_PTR crtc
 * properties)
 * 
 * @param builder          The KMS request builder.
 * @param layer            The exact details (src pos, output pos, rotation,
 *                         framebuffer) of the layer that should be shown on
 *                         screen.
 * @param release_callback Called when the framebuffer of this layer is no
 *                         longer being shown on screen. This is called with the
 *                         drmdev locked, so make sure to use _locked variants
 *                         of any drmdev calls.
 * @param deferred_release_callback (Unimplemented right now) If this is present,
 *                                  this callback might be called instead of
 *                                  @param release_callback.
 *                                  This is called with a DRM syncobj fd that is
 *                                  signaled when the framebuffer is no longer
 *                                  shown on screen.
 *                                  Legacy DRM modesetting does not support
 *                                  explicit fencing, in which case
 *                                  @param release_callback will be called
 *                                  instead.
 * @param userdata Userdata pointer that's passed to the release_callback or
 *                 deferred_release_callback as-is.
 * @returns Zero on success, otherwise:
 *            - EINVAL: if attempting to push a second framebuffer layer, if
 *                driver supports atomic modesetting but legacy modesetting is
 *                being used.
 *            - EIO: if no DRM plane could be found that supports displaying
 *                this framebuffer layer. Either the pixel format is not
 *                supported, the modifier, the rotation or the drm device
 *                doesn't have enough planes.
 *            - The error returned by @ref close if closing the in_fence_fd
 *              fails.
 */
int kms_req_builder_push_fb_layer(
    struct kms_req_builder *builder,
    const struct kms_fb_layer *layer,
    void_callback_t release_callback,
    kmsreq_syncfile_cb_t deferred_release_callback,
    void *userdata
);

/**
 * @brief Push a "fake" layer that just keeps one zpos free, incase something
 * other than KMS wants to display contents there. (e.g. omxplayer)
 * 
 * @param builder The KMS request builder.
 * @param zpos_out Filled with the zpos that won't be occupied by the request
 *                 builder.
 * @returns Zero.
 */
int kms_req_builder_push_zpos_placeholder_layer(struct kms_req_builder *builder, int64_t *zpos_out);

/**
 * @brief A KMS request (atomic or legacy modesetting) that can be committed to
 * change the state of a single CRTC.
 * 
 * Only way to construct this is by building a KMS request using
 * @ref kms_req_builder and then calling @ref kms_req_builder_build.
 */
struct kms_req;

DECLARE_REF_OPS(kms_req)

/**
 * @brief Build the KMS request builder into an actual, immutable KMS request
 * that can be committed. Internally this doesn't do much at all.
 * 
 * @param builder The KMS request builder that should be built.
 * @returns KMS request that can be committed using @ref kms_req_commit_blocking
 *          or @ref kms_req_commit_nonblocking.
 *          The returned KMS request has refcount 1. Unref using
 *          @ref kms_req_unref after usage.
 */
struct kms_req *kms_req_builder_build(struct kms_req_builder *builder);

int kms_req_commit_blocking(struct kms_req *req, struct drmdev *drmdev, uint64_t *vblank_ns_out);

int kms_req_commit_nonblocking(struct kms_req *req, struct drmdev *drmdev, kmsreq_scanout_cb_t scanout_cb, void *userdata, void_callback_t destroy_cb);

#endif // _FLUTTERPI_SRC_MODESETTING_REQ_BUILDER_H_
