#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <alloca.h>
#include <inttypes.h>
#include <sys/epoll.h>
#include <limits.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <modesetting.h>

#define HAS_ACTIVE_CRTC(builder) (((builder)->active_crtc_info == NULL) || ((builder)->active_crtc_id == 0))
#define DEBUG_ASSERT_HAS_ACTIVE_CRTC(builder) DEBUG_ASSERT(HAS_ACTIVE_CRTC(builder))
#define CHECK_HAS_ACTIVE_CRTC_OR_RETURN_EINVAL(builder) do { \
        if (HAS_ACTIVE_CRTC(presenter)) { \
            LOG_MODESETTING_ERROR("%s: No active CRTC\n", __func__); \
            return EINVAL; \
        } \
    } while (false)

#define IS_VALID_ZPOS(builder, zpos) (((builder)->active_crtc_info->min_zpos <= zpos) && ((builder)->active_crtc_info->max_zpos >= zpos))
#define DEBUG_ASSERT_VALID_ZPOS(builder, zpos) DEBUG_ASSERT(IS_VALID_ZPOS(builder, zpos))
#define CHECK_VALID_ZPOS_OR_RETURN_EINVAL(builder, zpos) do { \
        if (IS_VALID_ZPOS(presenter, zpos)) { \
            LOG_MODESETTING_ERROR("%s: Invalid zpos\n", __func__); \
            return EINVAL; \
        } \
    } while (false)

static int drmdev_lock(struct drmdev *drmdev) {
    return pthread_mutex_lock(&drmdev->mutex);
}

static int drmdev_unlock(struct drmdev *drmdev) {
    return pthread_mutex_unlock(&drmdev->mutex);
}

static int fetch_connectors(struct drmdev *drmdev, struct drm_connector **connectors_out, size_t *n_connectors_out) {
    struct drm_connector *connectors;
    int n_allocated_connectors;
    int ok;

    connectors = calloc(drmdev->res->count_connectors, sizeof *connectors);
    if (connectors == NULL) {
        *connectors_out = NULL;
        return ENOMEM;
    }

    n_allocated_connectors = 0;
    for (int i = 0; i < drmdev->res->count_connectors; i++, n_allocated_connectors++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModeConnector *connector;

        connector = drmModeGetConnector(drmdev->fd, drmdev->res->connectors[i]);
        if (connector == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device connector. drmModeGetConnector");
            goto fail_free_connectors;
        }

        props = drmModeObjectGetProperties(drmdev->fd, drmdev->res->connectors[i], DRM_MODE_OBJECT_CONNECTOR);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device connectors properties. drmModeObjectGetProperties");
            drmModeFreeConnector(connector);
            goto fail_free_connectors;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreeConnector(connector);
            goto fail_free_connectors;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(drmdev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device connector properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreeConnector(connector);
                goto fail_free_connectors;
            }
        }

        connectors[i].connector = connector;
        connectors[i].props = props;
        connectors[i].props_info = props_info;
    }

    *connectors_out = connectors;
    *n_connectors_out = drmdev->res->count_connectors;

    return 0;

    fail_free_connectors:
    for (int i = 0; i < n_allocated_connectors; i++) {
        for (unsigned int j = 0; j < connectors[i].props->count_props; j++)
            drmModeFreeProperty(connectors[i].props_info[j]);
        free(connectors[i].props_info);
        drmModeFreeObjectProperties(connectors[i].props);
        drmModeFreeConnector(connectors[i].connector);
    }

    fail_free_result:
    free(connectors);

    *connectors_out = NULL;
    *n_connectors_out = 0;
    return ok;
}

static int free_connectors(struct drm_connector *connectors, size_t n_connectors) {
    for (unsigned int i = 0; i < n_connectors; i++) {
        for (unsigned int j = 0; j < connectors[i].props->count_props; j++)
            drmModeFreeProperty(connectors[i].props_info[j]);
        free(connectors[i].props_info);
        drmModeFreeObjectProperties(connectors[i].props);
        drmModeFreeConnector(connectors[i].connector);
    }

    free(connectors);

    return 0;
}

static int fetch_encoders(struct drmdev *drmdev, struct drm_encoder **encoders_out, size_t *n_encoders_out) {
    struct drm_encoder *encoders;
    int n_allocated_encoders;
    int ok;

    encoders = calloc(drmdev->res->count_encoders, sizeof *encoders);
    if (encoders == NULL) {
        *encoders_out = NULL;
        *n_encoders_out = 0;
        return ENOMEM;
    }

    n_allocated_encoders = 0;
    for (int i = 0; i < drmdev->res->count_encoders; i++, n_allocated_encoders++) {
        drmModeEncoder *encoder;

        encoder = drmModeGetEncoder(drmdev->fd, drmdev->res->encoders[i]);
        if (encoder == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device encoder. drmModeGetEncoder");
            goto fail_free_encoders;
        }

        encoders[i].encoder = encoder;
    }

    *encoders_out = encoders;
    *n_encoders_out = drmdev->res->count_encoders;

    return 0;

    fail_free_encoders:
    for (int i = 0; i < n_allocated_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }

    fail_free_result:
    free(encoders);

    *encoders_out = NULL;
    *n_encoders_out = 0;
    return ok;
}

static int free_encoders(struct drm_encoder *encoders, size_t n_encoders) {
    for (unsigned int i = 0; i < n_encoders; i++) {
        drmModeFreeEncoder(encoders[i].encoder);
    }

    free(encoders);

    return 0;
}

static int fetch_crtcs(struct drmdev *drmdev, struct drm_crtc **crtcs_out, size_t *n_crtcs_out) {
    struct drm_crtc *crtcs;
    int n_allocated_crtcs;
    int ok;

    crtcs = calloc(drmdev->res->count_crtcs, sizeof *crtcs);
    if (crtcs == NULL) {
        *crtcs_out = NULL;
        return ENOMEM;
    }

    n_allocated_crtcs = 0;
    for (int i = 0; i < drmdev->res->count_crtcs; i++, n_allocated_crtcs++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModeCrtc *crtc;

        crtc = drmModeGetCrtc(drmdev->fd, drmdev->res->crtcs[i]);
        if (crtc == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device CRTC. drmModeGetCrtc");
            goto fail_free_crtcs;
        }

        props = drmModeObjectGetProperties(drmdev->fd, drmdev->res->crtcs[i], DRM_MODE_OBJECT_CRTC);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device CRTCs properties. drmModeObjectGetProperties");
            drmModeFreeCrtc(crtc);
            goto fail_free_crtcs;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreeCrtc(crtc);
            goto fail_free_crtcs;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(drmdev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device CRTCs properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreeCrtc(crtc);
                goto fail_free_crtcs;
            }
        }
        
        crtcs[i].crtc = crtc;
        crtcs[i].props = props;
        crtcs[i].props_info = props_info;
        
        crtcs[i].index = i;
        crtcs[i].bitmask = 1 << i;
    }

    *crtcs_out = crtcs;
    *n_crtcs_out = drmdev->res->count_crtcs;

    return 0;


    fail_free_crtcs:
    for (int i = 0; i < n_allocated_crtcs; i++) {
        for (unsigned int j = 0; j < crtcs[i].props->count_props; j++)
            drmModeFreeProperty(crtcs[i].props_info[j]);
        free(crtcs[i].props_info);
        drmModeFreeObjectProperties(crtcs[i].props);
        drmModeFreeCrtc(crtcs[i].crtc);
    }

    fail_free_result:
    free(crtcs);

    *crtcs_out = NULL;
    *n_crtcs_out = 0;
    return ok;
}

static int free_crtcs(struct drm_crtc *crtcs, size_t n_crtcs) {
    for (unsigned int i = 0; i < n_crtcs; i++) {
        for (unsigned int j = 0; j < crtcs[i].props->count_props; j++)
            drmModeFreeProperty(crtcs[i].props_info[j]);
        free(crtcs[i].props_info);
        drmModeFreeObjectProperties(crtcs[i].props);
        drmModeFreeCrtc(crtcs[i].crtc);
    }

    free(crtcs);

    return 0;
}

static int fetch_planes(struct drmdev *drmdev, struct drm_plane **planes_out, size_t *n_planes_out) {
    struct drm_plane *planes;
    int n_allocated_planes;
    int ok;

    planes = calloc(drmdev->plane_res->count_planes, sizeof *planes);
    if (planes == NULL) {
        *planes_out = NULL;
        return ENOMEM;
    }

    n_allocated_planes = 0;
    for (unsigned int i = 0; i < drmdev->plane_res->count_planes; i++, n_allocated_planes++) {
        drmModeObjectProperties *props;
        drmModePropertyRes **props_info;
        drmModePlane *plane;

        memset(&planes[i].property_ids, 0xFF, sizeof(planes[i].property_ids));

        plane = drmModeGetPlane(drmdev->fd, drmdev->plane_res->planes[i]);
        if (plane == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device plane. drmModeGetPlane");
            goto fail_free_planes;
        }

        props = drmModeObjectGetProperties(drmdev->fd, drmdev->plane_res->planes[i], DRM_MODE_OBJECT_PLANE);
        if (props == NULL) {
            ok = errno;
            perror("[modesetting] Could not get DRM device planes' properties. drmModeObjectGetProperties");
            drmModeFreePlane(plane);
            goto fail_free_planes;
        }

        props_info = calloc(props->count_props, sizeof *props_info);
        if (props_info == NULL) {
            ok = ENOMEM;
            drmModeFreeObjectProperties(props);
            drmModeFreePlane(plane);
            goto fail_free_planes;
        }

        for (unsigned int j = 0; j < props->count_props; j++) {
            props_info[j] = drmModeGetProperty(drmdev->fd, props->props[j]);
            if (props_info[j] == NULL) {
                ok = errno;
                perror("[modesetting] Could not get DRM device planes' properties' info. drmModeGetProperty");
                for (unsigned int k = 0; k < (j-1); k++)
                    drmModeFreeProperty(props_info[j]);
                free(props_info);
                drmModeFreeObjectProperties(props);
                drmModeFreePlane(plane);
                goto fail_free_planes;
            }

            if (strcmp(props_info[j]->name, "type") == 0) {
                planes[i].type = 0;
                for (uint32_t k = 0; k < props->count_props; k++) {
                    if (props->props[k] == props_info[j]->prop_id) {
                        planes[i].type = props->prop_values[k];
                        break;
                    }
                }
            } else if (strcmp(props_info[j]->name, "CRTC_ID") == 0) {
                planes[i].property_ids.crtc_id = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "FB_ID") == 0) {
                planes[i].property_ids.fb_id = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_X") == 0) {
                planes[i].property_ids.src_x = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_Y") == 0) {
                planes[i].property_ids.src_y = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_W") == 0) {
                planes[i].property_ids.src_w = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "SRC_H") == 0) {
                planes[i].property_ids.src_h = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_X") == 0) {
                planes[i].property_ids.crtc_x = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_Y") == 0) {
                planes[i].property_ids.crtc_y = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_W") == 0) {
                planes[i].property_ids.crtc_w = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "CRTC_H") == 0) {
                planes[i].property_ids.crtc_h = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "zpos") == 0) {
                planes[i].property_ids.zpos = props_info[j]->prop_id;
            } else if (strcmp(props_info[j]->name, "rotation") == 0) {
                planes[i].property_ids.rotation = props_info[j]->prop_id;
            }
        }

        planes[i].plane = plane;
        planes[i].props = props;
        planes[i].props_info = props_info;
    }

    *planes_out = planes;
    *n_planes_out = drmdev->plane_res->count_planes;

    return 0;


    fail_free_planes:
    for (int i = 0; i < n_allocated_planes; i++) {
        for (unsigned int j = 0; j < planes[i].props->count_props; j++)
            drmModeFreeProperty(planes[i].props_info[j]);
        free(planes[i].props_info);
        drmModeFreeObjectProperties(planes[i].props);
        drmModeFreePlane(planes[i].plane);
    }

    fail_free_result:
    free(planes);

    *planes_out = NULL;
    *n_planes_out = 0;
    return ok;
}

static int free_planes(struct drm_plane *planes, size_t n_planes) {
    for (size_t i = 0; i < n_planes; i++) {
        for (uint32_t j = 0; j < planes[i].props->count_props; j++)
            drmModeFreeProperty(planes[i].props_info[j]);
        free(planes[i].props_info);
        drmModeFreeObjectProperties(planes[i].props);
        drmModeFreePlane(planes[i].plane);
    }

    free(planes);

    return 0;
}


float mode_get_vrefresh(const drmModeModeInfo *mode) {
    return mode->clock * 1000.0 / (mode->htotal * mode->vtotal);
}

struct drmdev *drmdev_new_from_fd(int fd) {
    struct gbm_device *gbmdev;
    struct drmdev *drmdev;
    int ok;

    drmdev = calloc(1, sizeof *drmdev);
    if (drmdev == NULL) {
        return NULL;
    }

    drmdev->fd = dup(fd);
    if (drmdev->fd < 0) {
        ok = errno;
        goto fail_free_drmdev;
    }

    ok = drmSetClientCap(drmdev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        ok = errno;
        goto fail_close_fd;
    } else {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client universal planes capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    }
    
    ok = drmSetClientCap(drmdev->fd, DRM_CLIENT_CAP_ATOMIC, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        drmdev->supports_atomic_modesetting = false;
    } else if (ok < 0) {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client atomic capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    } else {
        drmdev->supports_atomic_modesetting = true;
    }

    gbmdev = gbm_create_device(drmdev->fd);
    if (gbmdev == NULL) {
        ok = errno;
        goto fail_close_fd;
    }

    drmdev->res = drmModeGetResources(drmdev->fd);
    if (drmdev->res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device resources. drmModeGetResources");
        goto fail_close_fd;
    }

    drmdev->plane_res = drmModeGetPlaneResources(drmdev->fd);
    if (drmdev->plane_res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device planes resources. drmModeGetPlaneResources");
        goto fail_free_resources;
    }

    ok = fetch_connectors(drmdev, &drmdev->connectors, &drmdev->n_connectors);
    if (ok != 0) {
        goto fail_free_plane_resources;
    }

    ok = fetch_encoders(drmdev, &drmdev->encoders, &drmdev->n_encoders);
    if (ok != 0) {
        goto fail_free_connectors;
    }

    ok = fetch_crtcs(drmdev, &drmdev->crtcs, &drmdev->n_crtcs);
    if (ok != 0) {
        goto fail_free_encoders;
    }

    ok = fetch_planes(drmdev, &drmdev->planes, &drmdev->n_planes);
    if (ok != 0) {
        goto fail_free_crtcs;
    }

    return drmdev;


    fail_free_crtcs:
    free_crtcs(drmdev->crtcs, drmdev->n_crtcs);

    fail_free_encoders:
    free_encoders(drmdev->encoders, drmdev->n_encoders);

    fail_free_connectors:
    free_connectors(drmdev->connectors, drmdev->n_connectors);

    fail_free_plane_resources:
    drmModeFreePlaneResources(drmdev->plane_res);

    fail_free_resources:
    drmModeFreeResources(drmdev->res);

    fail_close_fd:
    close(drmdev->fd);

    fail_free_drmdev:
    free(drmdev);

    return NULL;
}

struct drmdev *drmdev_new_from_path(const char *path) {
    struct drmdev *drmdev;
    int ok, fd;

    ok = open(path, O_RDWR);
    if (ok < 0) {
        perror("[modesetting] Could not open DRM device. open");
        return NULL;
    }

    fd = ok;

    drmdev = drmdev_new_from_fd(fd);
    if (drmdev == NULL) {
        close(fd);
        return NULL;
    }

    // drmdev_new_from_fd duplicates the fd internally, so we retain ownership of this fd.
    close(fd);

    return drmdev;
}

struct drmdev *drmdev_new_and_configure(void) {
    const struct drm_connector *connector;
	const struct drm_encoder *encoder;
	const struct drm_crtc *crtc;
	const drmModeModeInfo *mode, *mode_iter;
    struct drmdev *drmdev;
    drmDevice **devices;
	int ok, n_devices;

    // first, find out how many devices are attached
    ok = drmGetDevices2(0, NULL, 0);
    if (ok < 0) {
		LOG_MODESETTING_ERROR("Could not query connected GPUs. drmGetDevices2: %s\n", strerror(-ok));
		return NULL;
	}

    n_devices = ok;
    devices = alloca((sizeof *devices) * n_devices);

    // then actually query the attached devices
    ok = drmGetDevices2(0, devices, n_devices);
	if (ok < 0) {
		LOG_MODESETTING_ERROR("Could not query connected GPUs. drmGetDevices2: %s\n", strerror(-ok));
		return NULL;
	}

    n_devices = ok;
	
	// find a GPU that has a primary node
	for (int i = 0; i < n_devices; i++) {
		drmDevice *device = devices[i];

        // we need a primary node for video output.
        if (device->available_nodes & (1 << DRM_NODE_PRIMARY)) {
            drmdev = drmdev_new_from_path(device->nodes[DRM_NODE_PRIMARY]);
            if (ok != 0) {
                LOG_MODESETTING_ERROR("Could not open GPU at path \"%s\". Continuing.\n", device->nodes[DRM_NODE_PRIMARY]);
                continue;
            }

            break;
        }
	}

    drmFreeDevices(devices, n_devices);

	if (drmdev == NULL) {
		fprintf(stderr, "flutter-pi couldn't find a usable DRM device.\n"
						"Please make sure you've enabled the Fake-KMS driver in raspi-config.\n"
						"If you're not using a Raspberry Pi, please make sure there's KMS support for your graphics chip.\n");
		goto fail_return_null;
	}

	// find a connected connector
	for_each_connector_in_drmdev(drmdev, connector) {
		if (connector->connector->connection == DRM_MODE_CONNECTED) {
			// only update the physical size of the display if the values
			//   are not yet initialized / not set with a commandline option

            /// TODO: Move this to flutter-pi
            /*
			if ((flutterpi->display.width_mm == 0) || (flutterpi->display.height_mm == 0)) {
				if ((connector->connector->connector_type == DRM_MODE_CONNECTOR_DSI) &&
					(connector->connector->mmWidth == 0) &&
					(connector->connector->mmHeight == 0))
				{
					// if it's connected via DSI, and the width & height are 0,
					//   it's probably the official 7 inch touchscreen.
					flutterpi->display.width_mm = 155;
					flutterpi->display.height_mm = 86;
				} else if ((connector->connector->mmHeight % 10 == 0) &&
							(connector->connector->mmWidth % 10 == 0)) {
					// don't change anything.
				} else {
					flutterpi->display.width_mm = connector->connector->mmWidth;
					flutterpi->display.height_mm = connector->connector->mmHeight;
				}
			}
            */

			break;
		}
	}

	if (connector == NULL) {
        LOG_MODESETTING_ERROR("Could not find a connected display!\n");
		goto fail_free_drmdev;
	}

	// Find the preferred mode (GPU drivers _should_ always supply a preferred mode, but of course, they don't)
	// Alternatively, find the mode with the highest width*height. If there are multiple modes with the same w*h,
	// prefer higher refresh rates. After that, prefer progressive scanout modes.
	mode = NULL;
	for_each_mode_in_connector(connector, mode_iter) {
		if (mode_iter->type & DRM_MODE_TYPE_PREFERRED) {
			mode = mode_iter;
			break;
		} else if (mode == NULL) {
			mode = mode_iter;
		} else {
			int area = mode_iter->hdisplay * mode_iter->vdisplay;
			int old_area = mode->hdisplay * mode->vdisplay;

			if ((area > old_area) ||
				((area == old_area) && (mode_iter->vrefresh > mode->vrefresh)) ||
				((area == old_area) && (mode_iter->vrefresh == mode->vrefresh) && ((mode->flags & DRM_MODE_FLAG_INTERLACE) == 0))) {
				mode = mode_iter;
			}
		}
	}

	if (mode == NULL) {
        LOG_MODESETTING_ERROR("Could not find a suitable video output mode!\n");
		goto fail_free_drmdev;
	}

    /*
	flutterpi->display.width = mode->hdisplay;
	flutterpi->display.height = mode->vdisplay;
	flutterpi->display.refresh_rate = mode->vrefresh;
    */
    
    /// TODO: Move this to flutterpi
    /*
	if ((flutterpi->display.width_mm == 0) || (flutterpi->display.height_mm == 0)) {
		fprintf(
			stderr,
			"[flutter-pi] WARNING: display didn't provide valid physical dimensions.\n"
			"             The device-pixel ratio will default to 1.0, which may not be the fitting device-pixel ratio for your display.\n"
		);
		flutterpi->display.pixel_ratio = 1.0;
	} else {
		flutterpi->display.pixel_ratio = (10.0 * flutterpi->display.width) / (flutterpi->display.width_mm * 38.0);
		
		int horizontal_dpi = (int) (flutterpi->display.width / (flutterpi->display.width_mm / 25.4));
		int vertical_dpi = (int) (flutterpi->display.height / (flutterpi->display.height_mm / 25.4));

		if (horizontal_dpi != vertical_dpi) {
		        // See https://github.com/flutter/flutter/issues/71865 for current status of this issue.
			fprintf(stderr, "[flutter-pi] WARNING: display has non-square pixels. Non-square-pixels are not supported by flutter.\n");
		}
	}
    */
	
	for_each_encoder_in_drmdev(drmdev, encoder) {
		if (encoder->encoder->encoder_id == connector->connector->encoder_id) {
			break;
		}
	}
	
	if (encoder == NULL) {
		for (int i = 0; i < connector->connector->count_encoders; i++, encoder = NULL) {
			for_each_encoder_in_drmdev(drmdev, encoder) {
				if (encoder->encoder->encoder_id == connector->connector->encoders[i]) {
					break;
				}
			}

			if (encoder->encoder->possible_crtcs) {
				// only use this encoder if there's a crtc we can use with it
				break;
			}
		}
	}

	if (encoder == NULL) {
        LOG_MODESETTING_ERROR("Could not find a suitable DRM encoder.\n");
		goto fail_free_drmdev;
	}

	for_each_crtc_in_drmdev(drmdev, crtc) {
		if (crtc->crtc->crtc_id == encoder->encoder->crtc_id) {
			break;
		}
	}

	if (crtc == NULL) {
		for_each_crtc_in_drmdev(drmdev, crtc) {
			if (encoder->encoder->possible_crtcs & crtc->bitmask) {
				// find a CRTC that is possible to use with this encoder
				break;
			}
		}
	}

	if (crtc == NULL) {
        LOG_MODESETTING_ERROR("Could not find a suitable DRM CRTC.\n");
		goto fail_free_drmdev;
	}

	ok = drmdev_configure(drmdev, connector->connector->connector_id, encoder->encoder->encoder_id, crtc->crtc->crtc_id, mode);
	if (ok != 0) {
        goto fail_free_drmdev;
    }

    return drmdev;


    fail_free_drmdev:
    drmdev_destroy(drmdev);

    fail_return_null:
    return NULL;
}


void drmdev_destroy(
    struct drmdev *drmdev
) {
    if (drmdev->selected_mode != NULL) {
        drmModeDestroyPropertyBlob(drmdev->fd, drmdev->selected_mode_blob_id);
    }
    free_planes(drmdev->planes, drmdev->n_planes);
    free_crtcs(drmdev->crtcs, drmdev->n_crtcs);
    free_encoders(drmdev->encoders, drmdev->n_encoders);
    free_connectors(drmdev->connectors, drmdev->n_connectors);
    drmModeFreePlaneResources(drmdev->plane_res);
    drmModeFreeResources(drmdev->res);
    gbm_device_destroy(drmdev->gbmdev);
    close(drmdev->fd);
    free(drmdev);
}

int drmdev_configure(
    struct drmdev *drmdev,
    uint32_t connector_id,
    uint32_t encoder_id,
    uint32_t crtc_id,
    const drmModeModeInfo *mode
) {
    struct drm_connector *connector;
    struct drm_encoder *encoder;
    struct drm_crtc *crtc;
    uint32_t mode_id;
    int ok;

    drmdev_lock(drmdev);

    for_each_connector_in_drmdev(drmdev, connector) {
        if (connector->connector->connector_id == connector_id) {
            break;
        }
    }

    if (connector == NULL) {
        drmdev_unlock(drmdev);
        return EINVAL;
    }

    for_each_encoder_in_drmdev(drmdev, encoder) {
        if (encoder->encoder->encoder_id == encoder_id) {
            break;
        }
    }

    if (encoder == NULL) {
        drmdev_unlock(drmdev);
        return EINVAL;
    }

    for_each_crtc_in_drmdev(drmdev, crtc) {
        if (crtc->crtc->crtc_id == crtc_id) {
            break;
        }
    }

    if (crtc == NULL) {
        drmdev_unlock(drmdev);
        return EINVAL;
    }

    mode_id = 0;
    if (drmdev->supports_atomic_modesetting) {
        ok = drmModeCreatePropertyBlob(drmdev->fd, mode, sizeof(*mode), &mode_id);
        if (ok < 0) {
            perror("[modesetting] Could not create property blob for DRM mode. drmModeCreatePropertyBlob");
            drmdev_unlock(drmdev);
            return errno;
        }

        if (drmdev->selected_mode != NULL) {
            ok = drmModeDestroyPropertyBlob(drmdev->fd, drmdev->selected_mode_blob_id);
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not destroy old DRM mode property blob. drmModeDestroyPropertyBlob");
                drmModeDestroyPropertyBlob(drmdev->fd, mode_id);
                drmdev_unlock(drmdev);
                return ok;
            }
        }
    }

    drmdev->selected_connector = connector;
    drmdev->selected_encoder = encoder;
    drmdev->selected_crtc = crtc;
    drmdev->selected_mode = mode;
    drmdev->selected_mode_blob_id = mode_id;

    drmdev->is_configured = true;

    drmdev_unlock(drmdev);

    return 0;
}

static struct drm_plane *get_plane_by_id(
    struct drmdev *drmdev,
    uint32_t plane_id
) {
    struct drm_plane *plane;

    plane = NULL;
    for (size_t i = 0; i < drmdev->n_planes; i++) {
        if (drmdev->planes[i].plane->plane_id == plane_id) {
            plane = drmdev->planes + i;
            break;
        }
    }

    return plane;
}

static int get_plane_property_index_by_name(
    struct drm_plane *plane,
    const char *property_name
) {
    if (plane == NULL) {
        return -1;
    }

    int prop_index = -1; 
    for (uint32_t i = 0; i < plane->props->count_props; i++) {
        if (strcmp(plane->props_info[i]->name, property_name) == 0) {
            prop_index = i;
            break;
        }
    }

    return prop_index;
}

int drmdev_plane_get_type(
    struct drmdev *drmdev,
    uint32_t plane_id
) {
    struct drm_plane *plane = get_plane_by_id(drmdev, plane_id);
    if (plane == NULL) {
        return -1;
    }

    return plane->type;
}

int drmdev_plane_supports_setting_rotation_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int drm_rotation,
    bool *result
) {
    struct drm_plane *plane = get_plane_by_id(drmdev, plane_id);
    if (plane == NULL) {
        return EINVAL;
    }
    
    int prop_index = get_plane_property_index_by_name(plane, "rotation");
    if (prop_index == -1) {
        *result = false;
        return 0;
    }

    if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_IMMUTABLE) {
        *result = false;
        return 0;
    }

    if (!(plane->props_info[prop_index]->flags & DRM_MODE_PROP_BITMASK)) {
        *result = false;
        return 0;
    }

    uint64_t value = drm_rotation;

    for (int i = 0; i < plane->props_info[prop_index]->count_enums; i++) {
        value &= ~(1 << plane->props_info[prop_index]->enums[i].value);
    }

    *result = !value;
    return 0;
}

int drmdev_plane_supports_setting_zpos_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int64_t zpos,
    bool *result
) {
    struct drm_plane *plane = get_plane_by_id(drmdev, plane_id);
    if (plane == NULL) {
        return EINVAL;
    }
    
    int prop_index = get_plane_property_index_by_name(plane, "zpos");
    if (prop_index == -1) {
        *result = false;
        return 0;
    }

    if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_IMMUTABLE) {
        *result = false;
        return 0;
    }

    if (plane->props_info[prop_index]->count_values != 2) {
        *result = false;
        return 0;
    }

    if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_SIGNED_RANGE) {
        int64_t min = *((int64_t*) (plane->props_info[prop_index]->values + 0));
        int64_t max = *((int64_t*) (plane->props_info[prop_index]->values + 1));

        if ((min <= zpos) && (max >= zpos)) {
            *result = true;
            return 0;
        } else {
            *result = false;
            return 0;
        }
    } else if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_RANGE) {
        uint64_t min = plane->props_info[prop_index]->values[0];
        uint64_t max = plane->props_info[prop_index]->values[1];

        if ((min <= (uint64_t) zpos) && (max >= (uint64_t) zpos)) {
            *result = true;
            return 0;
        } else {
            *result = false;
            return 0;
        }
    } else {
        *result = false;
        return 0;
    }
    
    return 0;
}

int drmdev_plane_get_min_zpos_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int64_t *min_zpos_out
) {
    struct drm_plane *plane = get_plane_by_id(drmdev, plane_id);
    if (plane == NULL) {
        return EINVAL;
    }
    
    int prop_index = get_plane_property_index_by_name(plane, "zpos");
    if (prop_index == -1) {
        return EINVAL;
    }

    if (plane->props_info[prop_index]->count_values != 2) {
        return EINVAL;
    }

    if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_SIGNED_RANGE) {
        int64_t min = *((int64_t*) (plane->props_info[prop_index]->values + 0));
        
        *min_zpos_out = min;
        return 0;
    } else if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_RANGE) {
        uint64_t min = plane->props_info[prop_index]->values[0];

        *min_zpos_out = (int64_t) min;
        return 0;
    } else {
        return EINVAL;
    }
    
    return EINVAL;
}

int drmdev_plane_get_max_zpos_value(
    struct drmdev *drmdev,
    uint32_t plane_id,
    int64_t *max_zpos_out
) {
    struct drm_plane *plane = get_plane_by_id(drmdev, plane_id);
    if (plane == NULL) {
        return EINVAL;
    }
    
    int prop_index = get_plane_property_index_by_name(plane, "zpos");
    if (prop_index == -1) {
        return EINVAL;
    }

    if (plane->props_info[prop_index]->count_values != 2) {
        return EINVAL;
    }

    if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_SIGNED_RANGE) {
        int64_t max = *((int64_t*) (plane->props_info[prop_index]->values + 1));
        
        *max_zpos_out = max;
        return 0;
    } else if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_RANGE) {
        uint64_t max = plane->props_info[prop_index]->values[1];

        *max_zpos_out = (int64_t) max;
        return 0;
    } else {
        return EINVAL;
    }
    
    return EINVAL;
}

int drmdev_plane_supports_setting_zpos(
    struct drmdev *drmdev,
    uint32_t plane_id,
    bool *result
) {
    struct drm_plane *plane = get_plane_by_id(drmdev, plane_id);
    if (plane == NULL) {
        return EINVAL;
    }
    
    int prop_index = get_plane_property_index_by_name(plane, "zpos");
    if (prop_index == -1) {
        *result = false;
        return 0;
    }

    if (plane->props_info[prop_index]->count_values != 2) {
        *result = false;
        return 0;
    }

    if (plane->props_info[prop_index]->flags & DRM_MODE_PROP_IMMUTABLE) {
        *result = false;
        return 0;
    }

    if (!(plane->props_info[prop_index]->flags & (DRM_MODE_PROP_RANGE | DRM_MODE_PROP_SIGNED_RANGE))) {
        *result = false;
        return 0;
    }

    *result = true;
    return 0;
}

int drmdev_new_atomic_req(
    struct drmdev *drmdev,
    struct drmdev_atomic_req **req_out
) {
    struct drmdev_atomic_req *req;
    struct drm_plane *plane;

    if (drmdev->supports_atomic_modesetting == false) {
        return EOPNOTSUPP;
    }

    req = calloc(1, sizeof *req);
    if (req == NULL) {
        return ENOMEM;
    }

    req->drmdev = drmdev;

    req->atomic_req = drmModeAtomicAlloc();
    if (req->atomic_req == NULL) {
        free(req);
        return ENOMEM;
    }

    req->available_planes = PSET_INITIALIZER_STATIC(req->available_planes_storage, 32);

    for_each_plane_in_drmdev(drmdev, plane) {
        if (plane->plane->possible_crtcs & drmdev->selected_crtc->bitmask) {
            pset_put(&req->available_planes, plane);
        }
    }

    *req_out = req;
    
    return 0;
}

void drmdev_destroy_atomic_req(
    struct drmdev_atomic_req *req
) {
    drmModeAtomicFree(req->atomic_req);
    free(req);
}

int drmdev_atomic_req_put_connector_property(
    struct drmdev_atomic_req *req,
    const char *name,
    uint64_t value
) {
    int ok;

    drmdev_lock(req->drmdev);

    for (size_t i = 0; i < req->drmdev->selected_connector->props->count_props; i++) {
        drmModePropertyRes *prop = req->drmdev->selected_connector->props_info[i];
        if (strcmp(prop->name, name) == 0) {
            ok = drmModeAtomicAddProperty(
                req->atomic_req,
                req->drmdev->selected_connector->connector->connector_id,
                prop->prop_id, value
            );
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not add connector property to atomic request. drmModeAtomicAddProperty");
                drmdev_unlock(req->drmdev);
                return ok;
            }

            drmdev_unlock(req->drmdev);
            return 0;
        }
    }

    drmdev_unlock(req->drmdev);
    return EINVAL;
}

int drmdev_atomic_req_put_crtc_property(
    struct drmdev_atomic_req *req,
    const char *name,
    uint64_t value
) {
    int ok;

    drmdev_lock(req->drmdev);

    for (size_t i = 0; i < req->drmdev->selected_crtc->props->count_props; i++) {
        drmModePropertyRes *prop = req->drmdev->selected_crtc->props_info[i];
        if (strcmp(prop->name, name) == 0) {
            ok = drmModeAtomicAddProperty(
                req->atomic_req,
                req->drmdev->selected_crtc->crtc->crtc_id,
                prop->prop_id,
                value
            );
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not add crtc property to atomic request. drmModeAtomicAddProperty");
                drmdev_unlock(req->drmdev);
                return ok;
            }
            
            drmdev_unlock(req->drmdev);
            return 0;
        }
    }

    drmdev_unlock(req->drmdev);
    return EINVAL;
}

int drmdev_atomic_req_put_plane_property(
    struct drmdev_atomic_req *req,
    uint32_t plane_id,
    const char *name,
    uint64_t value
) {
    struct drm_plane *plane;
    int ok;

    drmdev_lock(req->drmdev);

    plane = NULL;
    for (size_t i = 0; i < req->drmdev->n_planes; i++) {
        if (req->drmdev->planes[i].plane->plane_id == plane_id) {
            plane = req->drmdev->planes + i;
            break;
        }
    }

    if (plane == NULL) {
        drmdev_unlock(req->drmdev);
        return EINVAL;
    }

    for (size_t i = 0; i < plane->props->count_props; i++) {
        drmModePropertyRes *prop;
        
        prop = plane->props_info[i];
        
        if (strcmp(prop->name, name) == 0) {
            ok = drmModeAtomicAddProperty(
                req->atomic_req,
                plane_id,
                prop->prop_id,
                value
            );
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not add plane property to atomic request. drmModeAtomicAddProperty");
                drmdev_unlock(req->drmdev);
                return ok;
            }
            
            drmdev_unlock(req->drmdev);
            return 0;
        }
    }

    drmdev_unlock(req->drmdev);
    return EINVAL;
}

int drmdev_atomic_req_put_modeset_props(
    struct drmdev_atomic_req *req,
    uint32_t *flags
) {
    struct drmdev_atomic_req *augment;
    int ok;

    ok = drmdev_new_atomic_req(req->drmdev, &augment);
    if (ok != 0) {
        return ok;
    }

    ok = drmdev_atomic_req_put_connector_property(req, "CRTC_ID", req->drmdev->selected_crtc->crtc->crtc_id);
    if (ok != 0) {
        drmdev_destroy_atomic_req(augment);
        return ok;
    }

    ok = drmdev_atomic_req_put_crtc_property(req, "MODE_ID", req->drmdev->selected_mode_blob_id);
    if (ok != 0) {
        drmdev_destroy_atomic_req(augment);
        return ok;
    }

    ok = drmdev_atomic_req_put_crtc_property(req, "ACTIVE", 1);
    if (ok != 0) {
        drmdev_destroy_atomic_req(augment);
        return ok;
    }

    ok = drmModeAtomicMerge(req->atomic_req, augment->atomic_req);
    if (ok < 0) {
        ok = errno;
        perror("[modesetting] Could not apply modesetting properties to atomic request. drmModeAtomicMerge");
        drmdev_destroy_atomic_req(augment);
        return ok;
    }

    drmdev_destroy_atomic_req(augment);

    if (flags != NULL) {
        *flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }

    return 0;
}

int drmdev_atomic_req_commit(
    struct drmdev_atomic_req *req,
    uint32_t flags,
    void *userdata
) {
    int ok;

    drmdev_lock(req->drmdev);

    ok = drmModeAtomicCommit(req->drmdev->fd, req->atomic_req, flags, userdata);
    if (ok < 0) {
        ok = errno;
        perror("[modesetting] Could not commit atomic request. drmModeAtomicCommit");
        drmdev_unlock(req->drmdev);
        return ok;
    }

    drmdev_unlock(req->drmdev);
    return 0;
}

int drmdev_legacy_set_mode_and_fb(
    struct drmdev *drmdev,
    uint32_t fb_id
) {
    int ok;

    drmdev_lock(drmdev);

    ok = drmModeSetCrtc(
        drmdev->fd,
        drmdev->selected_crtc->crtc->crtc_id,
        fb_id,
        0,
        0,
        &drmdev->selected_connector->connector->connector_id,
        1,
        (drmModeModeInfoPtr) drmdev->selected_mode
    );
    if (ok < 0) {
        ok = errno;
        perror("[modesetting] Could not set CRTC mode and framebuffer. drmModeSetCrtc");
        drmdev_unlock(drmdev);
        return ok;
    }

    drmdev_unlock(drmdev);

    return 0;
}

int drmdev_legacy_primary_plane_pageflip(
    struct drmdev *drmdev,
    uint32_t fb_id,
    void *userdata
) {
    int ok;

    drmdev_lock(drmdev);

    ok = drmModePageFlip(
        drmdev->fd,
        drmdev->selected_crtc->crtc->crtc_id,
        fb_id,
        DRM_MODE_PAGE_FLIP_EVENT,
        userdata
    );
    if (ok < 0) {
        ok = errno;
        perror("[modesetting] Could not schedule pageflip on primary plane. drmModePageFlip");
        drmdev_unlock(drmdev);
        return ok;
    }

    drmdev_unlock(drmdev);

    return 0;
}

int drmdev_legacy_overlay_plane_pageflip(
    struct drmdev *drmdev,
    uint32_t plane_id,
    uint32_t fb_id,
    int32_t crtc_x,
    int32_t crtc_y,
    int32_t crtc_w,
    int32_t crtc_h,
    uint32_t src_x,
    uint32_t src_y,
    uint32_t src_w,
    uint32_t src_h
) {
    int ok;

    drmdev_lock(drmdev);

    ok = drmModeSetPlane(
        drmdev->fd,
        plane_id,
        drmdev->selected_crtc->crtc->crtc_id,
        fb_id,
        0,
        crtc_x, crtc_y, crtc_w, crtc_h,
        src_x, src_y, src_w, src_h
    );
    if (ok < 0) {
        ok = errno;
        perror("[modesetting] Could not do blocking pageflip on overlay plane. drmModeSetPlane");
        drmdev_unlock(drmdev);
        return ok;
    }

    drmdev_unlock(drmdev);

    return 0;
}

int drmdev_legacy_set_connector_property(
    struct drmdev *drmdev,
    const char *name,
    uint64_t value
) {
    int ok;

    drmdev_lock(drmdev);

    for (size_t i = 0; i < drmdev->selected_connector->props->count_props; i++) {
        drmModePropertyRes *prop = drmdev->selected_connector->props_info[i];
        if (strcmp(prop->name, name) == 0) {
            ok = drmModeConnectorSetProperty(
                drmdev->fd,
                drmdev->selected_connector->connector->connector_id,
                prop->prop_id,
                value
            );
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not set connector property. drmModeConnectorSetProperty");
                drmdev_unlock(drmdev);
                return ok;
            }

            drmdev_unlock(drmdev);
            return 0;
        }
    }

    drmdev_unlock(drmdev);
    return EINVAL;
}

int drmdev_legacy_set_crtc_property(
    struct drmdev *drmdev,
    const char *name,
    uint64_t value
) {
    int ok;

    drmdev_lock(drmdev);

    for (size_t i = 0; i < drmdev->selected_crtc->props->count_props; i++) {
        drmModePropertyRes *prop = drmdev->selected_crtc->props_info[i];
        if (strcmp(prop->name, name) == 0) {
            ok = drmModeObjectSetProperty(
                drmdev->fd,
                drmdev->selected_crtc->crtc->crtc_id,
                DRM_MODE_OBJECT_CRTC,
                prop->prop_id,
                value
            );
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not set CRTC property. drmModeObjectSetProperty");
                drmdev_unlock(drmdev);
                return ok;
            }

            drmdev_unlock(drmdev);
            return 0;
        }
    }

    drmdev_unlock(drmdev);
    return EINVAL;
}

int drmdev_legacy_set_plane_property(
    struct drmdev *drmdev,
    uint32_t plane_id,
    const char *name,
    uint64_t value
) {
    struct drm_plane *plane;
    int ok;

    drmdev_lock(drmdev);

    plane = NULL;
    for (size_t i = 0; i < drmdev->n_planes; i++) {
        if (drmdev->planes[i].plane->plane_id == plane_id) {
            plane = drmdev->planes + i;
            break;
        }
    }

    if (plane == NULL) {
        drmdev_unlock(drmdev);
        return EINVAL;
    }

    for (size_t i = 0; i < plane->props->count_props; i++) {
        drmModePropertyRes *prop;
        
        prop = plane->props_info[i];
        
        if (strcmp(prop->name, name) == 0) {
            ok = drmModeObjectSetProperty(
                drmdev->fd,
                plane_id,
                DRM_MODE_OBJECT_PLANE,
                prop->prop_id,
                value
            );
            if (ok < 0) {
                ok = errno;
                perror("[modesetting] Could not set plane property. drmModeObjectSetProperty");
                drmdev_unlock(drmdev);
                return ok;
            }
            
            drmdev_unlock(drmdev);
            return 0;
        }
    }

    drmdev_unlock(drmdev);
    return EINVAL;
}


struct kmsdev {
    int fd;
    int epoll_fd;
    bool use_atomic_modesetting;

    size_t n_connectors;
    struct drm_connector *connectors;

    size_t n_encoders;
    struct drm_encoder *encoders;

    size_t n_crtcs;
    struct drm_crtc *crtcs;
    struct kms_crtc_info *crtc_infos;

    size_t n_planes;
    struct drm_plane *planes;

    drmModeRes *res;
    drmModePlaneRes *plane_res;
    drmEventContext pageflip_evctx;
};

struct kms_cursor_buffer {

};

struct kms_presenter {
    struct kmsdev *dev;

    /**
     * @brief Bitmask of the planes that are currently free to use.
     */
    uint32_t free_planes;

    struct kms_crtc_info *active_crtc_info;
    int32_t active_crtc_id;
    
    int current_zpos;
    drmModeAtomicReq *req;

    bool has_primary_layer;
    struct kms_fb_layer primary_layer;
};

struct fd_callback {
    int fd;
    void *userdata;
    bool (*callback)(int fd, void *userdata);
};

struct fbdev {
    int fd;
};

struct kmsdev *kmsdev_new(void) {
    struct kmsdev *dev;
    int ok, epoll_fd;

    dev = malloc(sizeof *dev);
    if (dev == NULL) {
        return NULL;
    }

    ok = epoll_create1(EPOLL_CLOEXEC);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't create epoll fd. epoll_create1: %s\n", strerror(errno));
        free(dev);
        return NULL;
    }

    epoll_fd = ok;

    dev->fd = -1;
    dev->epoll_fd = epoll_fd;
    dev->use_atomic_modesetting = false;
    dev->n_connectors = 0;
    dev->connectors = NULL;
    dev->n_encoders = 0;
    dev->encoders = NULL;
    dev->n_crtcs = 0;
    dev->crtcs = NULL;
    dev->crtc_infos = NULL;
    dev->n_planes = 0;
    dev->planes = NULL;
    dev->res = NULL;
    dev->plane_res = NULL;

    return dev;
}

void kmsdev_destroy(struct kmsdev *dev) {
    if (dev->fd > 0) {
        close(dev->fd);
    }
    close(dev->epoll_fd);
    free(dev);
}

struct kmsdev *kmsdev_new_from_fd(int fd) {
    struct kmsdev *dev;
    int ok;

    dev = kmsdev_new();
    if (dev == NULL) {
        return NULL;
    }

    ok = epoll_ctl(dev->epoll_fd, EPOLL_CTL_ADD, fd, &(struct epoll_event) {.events = EPOLLIN | EPOLLPRI, .data = {.fd = fd}});
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add KMS device fd to epoll instance. epoll_ctl: %s\n", strerror(errno));
        kmsdev_destroy(dev);
        return NULL;
    }

    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        ok = errno;
        goto fail_close_fd;
    } else {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client universal planes capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    }
    
    ok = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1ull);
    if ((ok < 0) && (errno == EOPNOTSUPP)) {
        dev->use_atomic_modesetting = false;
    } else if (ok < 0) {
        ok = errno;
        LOG_MODESETTING_ERROR("Could not set DRM client atomic capable. drmSetClientCap: %s\n", strerror(ok));
        goto fail_close_fd;
    } else {
        dev->use_atomic_modesetting = true;
    }

    dev = gbm_create_device(fd);
    if (dev == NULL) {
        ok = errno;
        goto fail_close_fd;
    }

    dev->res = drmModeGetResources(fd);
    if (dev->res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device resources. drmModeGetResources");
        goto fail_close_fd;
    }

    dev->plane_res = drmModeGetPlaneResources(dev->fd);
    if (dev->plane_res == NULL) {
        ok = errno;
        perror("[modesetting] Could not get DRM device planes resources. drmModeGetPlaneResources");
        goto fail_free_resources;
    }

    ok = fetch_connectors(dev, &dev->connectors, &dev->n_connectors);
    if (ok != 0) {
        goto fail_free_plane_resources;
    }

    ok = fetch_encoders(dev, &dev->encoders, &dev->n_encoders);
    if (ok != 0) {
        goto fail_free_connectors;
    }

    ok = fetch_crtcs(dev, &dev->crtcs, &dev->n_crtcs);
    if (ok != 0) {
        goto fail_free_encoders;
    }

    ok = fetch_planes(dev, &dev->planes, &dev->n_planes);
    if (ok != 0) {
        goto fail_free_crtcs;
    }

    dev->fd = fd;
    /// TODO: Implement

    return dev;


    fail_free_crtcs:
    free_crtcs(dev->crtcs, dev->n_crtcs);

    fail_free_encoders:
    free_encoders(dev->encoders, dev->n_encoders);

    fail_free_connectors:
    free_connectors(dev->connectors, dev->n_connectors);

    fail_free_plane_resources:
    drmModeFreePlaneResources(dev->plane_res);

    fail_free_resources:
    drmModeFreeResources(dev->res);

    fail_close_fd:
    close(dev->fd);

    fail_free_drmdev:
    kmsdev_destroy(dev);

    return NULL;
}

int kmsdev_get_fd(struct kmsdev *dev) {
    return dev->fd;
}

int kmsdev_on_fd_ready(struct kmsdev *dev) {
    struct epoll_event events[16];
    struct fd_callback *cb;
    bool keep;
    int ok, n_events;

    ok = epoll_wait(dev->epoll_fd, events, 16, 0);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("kmsdev_on_fd_ready: Couldn't get epoll events. epoll_wait: %s\n", strerror(errno));
        return errno;
    }

    n_events = ok;

    for (int i = 0; i < n_events; i++) {
        cb = events[i].data.ptr;

        DEBUG_ASSERT(cb != NULL);

        keep = cb->callback(cb->fd, cb->userdata);
        if (!keep) {
            ok = epoll_ctl(dev->epoll_fd, EPOLL_CTL_DEL, cb->fd, NULL);
            if (ok < 0) {
                LOG_MODESETTING_ERROR("kmsdev_on_fd_ready: Couldn't remove fd from epoll instance. epoll_ctl: %s\n", strerror(errno));
            }

            free(cb);
        }
    }

    return 0;
}

int kmsdev_add_fb(
    struct kmsdev *dev,
    uint32_t width, uint32_t height,
    uint32_t pixel_format,
    const uint32_t bo_handles[4],
    const uint32_t pitches[4],
    const uint32_t offsets[4],
    const uint64_t modifier[4],
    uint32_t *buf_id,
    uint32_t flags
) {
    int ok;
    
    ok = drmModeAddFB2WithModifiers(
        dev->fd,
        width, height,
        pixel_format,
        bo_handles,
        pitches,
        offsets,
        modifier,
        buf_id,
        flags
    );

    return -ok;
}

int kmsdev_destroy_fb(
    struct kmsdev *dev,
    uint32_t buf_id
) {
    return -drmModeRmFB(dev->fd, buf_id);
}

struct kms_crtc_info *kmsdev_get_crtc_info(struct kmsdev *dev, int32_t crtc_id) {
    for (size_t i = 0; i < dev->n_crtcs; i++) {
        if (dev->crtc_infos[i].crtc_id == crtc_id) {
            return dev->crtc_infos + i;
        }
    }

    return NULL;
}

struct kms_cursor_buffer *kmsdev_load_cursor(struct kmsdev *dev, const uint8_t *icon, int32_t format) {
    /// TODO: implement
    return EINVAL;
}

void kms_cursor_buffer_destroy(struct kms_cursor_buffer *buffer) {
    /// TODO: Implement
}

int kmsdev_set_cursor_state(struct kmsdev *dev, int32_t crtc_id, bool enabled, struct kms_cursor_buffer *buffer) {
    /// TODO: implement
    return EINVAL;
}

int kmsdev_move_cursor(struct kmsdev *dev, int32_t crtc_id, int x, int y) {
    /// TODO: implement
    return EINVAL;
}

static int put_fd_callback(
    struct kmsdev *dev,
    int fd,
    uint32_t events,
    bool (*callback)(struct kmsdev *dev, int fd, void *userdata),
    void *userdata
) {
    struct fd_callback *cb;
    int ok;

    cb = malloc(sizeof *cb);
    if (cb == NULL) {
        return ENOMEM;
    }

    cb->fd = fd;
    cb->callback = callback;
    cb->userdata = userdata;

    ok = epoll_ctl(dev->epoll_fd, EPOLL_CTL_ADD, fd, &(struct epoll_event) {.events = events, .data.ptr = cb});
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add callback to epoll instance. epoll_ctl: %s\n", strerror(errno));
        free(cb);
        return errno;
    }

    return 0;
}

static bool on_pageflip(
    struct kmsdev *dev,
    int fd,
    void *userdata
) {
    drmEventContext context = {
        .version = DRM_EVENT_CONTEXT_VERSION,
        .vblank_handler = NULL,
        .page_flip_handler = NULL,
        .page_flip_handler2 = userdata,
        .sequence_handler = NULL,
    };
    int ok;

    ok = drmHandleEvent(fd, &context);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't handle page flip event. drmHandleEvent: %s\n", strerror(-ok));
    }

    // don't keep the handler registered.
    return false;
}

static int put_pageflip_callback(
    struct kmsdev *dev,
    bool (*callback)(struct kmsdev *dev, int fd, void *userdata),
    void (*page_flip_handler)(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, unsigned int crtc_id, void *user_data)
) {
    return put_fd_callback(dev, dev->fd, EPOLLIN, on_pageflip, page_flip_handler);
}

int kmsdev_commit_scene(
    struct kmsdev *dev,
    const struct kms_presenter *scene,
    uint32_t flags,
    void *userdata
) {
    /// TODO: hookup callback
    if (dev->use_atomic_modesetting) {
        return drmModeAtomicCommit(
            dev->fd,
            scene->req,
            flags,
            userdata
        );
    } else {
        return drmModePageFlip(
            dev->fd,
            scene->active_crtc_id,
            scene->primary_layer.fb_id,
            DRM_MODE_PAGE_FLIP_EVENT,
            userdata
        );
    }
}


int kms_presenter_set_active_crtc(struct kms_presenter *presenter, int32_t crtc_id) {
    struct kms_crtc_info *info;

    info = kmsdev_get_crtc_info(presenter->dev, crtc_id);
    if (info == NULL) {
        LOG_MODESETTING_ERROR("kms_scene_builder_set_active_crtc: Invalid CRTC id: %" PRId32 "\n", crtc_id);
        return EINVAL;
    }

    presenter->active_crtc_id = crtc_id;
    presenter->active_crtc_info = info;

    return 0;
}

void kms_presenter_set_logical_zpos(struct kms_presenter *presenter, int zpos) {
    DEBUG_ASSERT_HAS_ACTIVE_CRTC(presenter);

    if (zpos >= 0) {
        zpos = zpos + presenter->active_crtc_info->min_zpos;
    } else {
        zpos = presenter->active_crtc_info->max_zpos - zpos + 1;
    }

    DEBUG_ASSERT_VALID_ZPOS(presenter, zpos);
    presenter->current_zpos = zpos;
}

void kms_presenter_set_zpos(struct kms_presenter *presenter, int zpos) {
    DEBUG_ASSERT_HAS_ACTIVE_CRTC(presenter);
    DEBUG_ASSERT_VALID_ZPOS(presenter, zpos);
    presenter->current_zpos = zpos;
}

int kms_presenter_get_zpos(struct kms_presenter *presenter) {
    return presenter->current_zpos;
}

/**
 * @brief Tries to find an unused DRM plane and returns its index in kmsdev->planes. Otherwise returns -1.
 */
static inline int reserve_plane(struct kms_presenter *presenter, int zpos_hint) {
    for (int i = 0; i < sizeof(presenter->free_planes) * CHAR_BIT; i++) {
        if (presenter->free_planes & (1 << i)) {
            presenter->free_planes &= ~(1 << i);
            return i;
        }
    }
    return -1;
}

/**
 * @brief Unreserves a plane index so it may be reserved using reserve_plane again.
 */
static inline int unreserve_plane(struct kms_presenter *presenter, int plane_index) {
    presenter->free_planes |= (1 << plane_index);
}

int kms_presenter_push_fb_layer(
    struct kms_presenter *presenter,
    const struct kms_fb_layer *layer
) {
    struct drm_plane *plane;
    uint32_t plane_id, plane_index;
    int ok;
    
    DEBUG_ASSERT(layer != NULL);
    DEBUG_ASSERT_HAS_ACTIVE_CRTC(presenter);
    CHECK_VALID_ZPOS_OR_RETURN_EINVAL(presenter, presenter->current_zpos);

    plane_index = reserve_plane(presenter, presenter->has_primary_layer? 1 : 0);
    if (plane_index < 0) {
        LOG_MODESETTING_ERROR("Couldn't find unused plane for framebuffer layer.\n");
        return EINVAL;
    }

    plane = &presenter->dev->planes[plane_index];
    plane_id = plane->plane->plane_id;

    if (layer && (plane->property_ids.rotation == DRM_NO_PROPERTY_ID)) {
        LOG_MODESETTING_ERROR("Rotation was requested but is not supported.\n");
        ok = EINVAL;
        goto fail_unreserve_plane;
    }

    if ((plane->property_ids.zpos == DRM_NO_PROPERTY_ID) && presenter->has_primary_layer) {
        LOG_MODESETTING_ERROR("Plane doesn't support zpos but that's necessary for overlay planes.\n");
        ok = EINVAL;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.fb_id, layer->fb_id);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.crtc_id, presenter->active_crtc_id);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.src_x, layer->src_x);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.src_y, layer->src_y);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.src_w, layer->src_w);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.src_h, layer->src_h);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.crtc_x, layer->crtc_x);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.crtc_y, layer->crtc_y);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }
    
    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.crtc_w, layer->crtc_w);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.crtc_h, layer->crtc_h);
    if (ok < 0) {
        LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
        ok = -ok;
        goto fail_unreserve_plane;
    }

    if (plane->property_ids.zpos != DRM_NO_PROPERTY_ID) {
        ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.zpos, presenter->current_zpos);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
            ok = -ok;
            goto fail_unreserve_plane;
        }
    }

    if (layer->has_rotation) {
        ok = drmModeAtomicAddProperty(presenter->req, plane_id, plane->property_ids.rotation, layer->rotation);
        if (ok < 0) {
            LOG_MODESETTING_ERROR("Couldn't add property to atomic request. drmModeAtomicAddProperty: %s\n", strerror(-ok));
            ok = -ok;
            goto fail_unreserve_plane;
        }
    }

    if (presenter->has_primary_layer == false) {
        memcpy(&presenter->primary_layer, layer, sizeof(struct kms_fb_layer));
    }

    return 0;


    fail_unreserve_plane:
    unreserve_plane(presenter, plane_index);

    fail_return_ok:
    return ok;
}

void kms_presenter_push_placeholder_layer(struct kms_presenter *presenter, int n_reserved_layers) {
    presenter->current_zpos += n_reserved_layers;
}

void kms_presenter_destroy(struct kms_presenter *presenter) {
    drmModeAtomicFree(presenter->req);
    free(presenter);
}
