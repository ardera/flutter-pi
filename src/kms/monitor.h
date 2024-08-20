#ifndef _FLUTTERPI_SRC_MODESETTING_RESOURCE_MONITOR_H
#define _FLUTTERPI_SRC_MODESETTING_RESOURCE_MONITOR_H

struct drm_resources;
struct udev;
struct drm_monitor;

struct drm_uevent {
    const char *sysnum;
    const char *action;
    bool hotplug;
    bool have_connector;
    uint32_t connector_id;
    bool have_property;
    uint32_t property_id;
};

struct drm_uevent_listener {
    void (*on_uevent)(const struct drm_uevent *uevent, void *userdata);
};

struct drm_monitor *drm_monitor_new(
    const char *sysnum_filter,
    struct udev *udev,
    const struct drm_uevent_listener *listener,
    void *listener_userdata
);

void drm_monitor_destroy(struct drm_monitor *m);

void drm_monitor_dispatch(struct drm_monitor *m);

int drm_monitor_get_fd(struct drm_monitor *m);

#endif
