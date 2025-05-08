#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#include <libudev.h>

#include "resources.h"
#include "monitor.h"

struct drm_monitor {
    struct udev *udev;
    struct udev_monitor *monitor;
    char *sysnum_filter;

    struct drm_uevent_listener listener;
    void *listener_userdata;
};

struct drm_monitor *drm_monitor_new(
    const char *sysnum_filter,
    struct udev *udev,
    const struct drm_uevent_listener *listener,
    void *listener_userdata
) {
    struct drm_monitor *m;

    ASSERT_NOT_NULL(udev);
    ASSERT_NOT_NULL(listener);

    m = calloc(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }

    struct udev_monitor *monitor = udev_monitor_new_from_netlink(udev, "udev");
    if (monitor == NULL) {
        LOG_ERROR("Could not create udev monitor.\n");
        return NULL;
    }

    udev_monitor_filter_add_match_subsystem_devtype(monitor, "drm", NULL);
    udev_monitor_enable_receiving(monitor);

    m->udev = udev_ref(udev);
    m->monitor = monitor;
    m->sysnum_filter = sysnum_filter != NULL ? strdup(sysnum_filter) : NULL;
    m->listener = *listener;
    m->listener_userdata = listener_userdata;
    return m;
}

void drm_monitor_destroy(struct drm_monitor *m) {
    udev_monitor_unref(m->monitor);
    udev_unref(m->udev);
    free(m);
}

void drm_monitor_dispatch(struct drm_monitor *m) {
    bool hotplug, have_connector, have_property;
    uint32_t connector_id, property_id;
    const char *str, *sysnum, *action;

    struct udev_device *event_device = udev_monitor_receive_device(m->monitor);
    if (event_device == NULL) {
        LOG_ERROR("Could not receive udev device from monitor.\n");
        return;
    }

    // sysname is the filename of the sysfs device file, e.g. card1.
    // sysnum is the numeric digits at the end of the sysname, e.g. 1.
    // e.g. /sys/.../card1 -> sysname = card1, sysnum = 1
    //      /sys/.../spi0.0 -> sysname = spi0.0, sysnum = 0

    sysnum = udev_device_get_sysnum(event_device);

    if (m->sysnum_filter != NULL) {
        if (sysnum == NULL || !streq(sysnum, m->sysnum_filter)) {
            // This event is not for our drm device.
            udev_device_unref(event_device);
            return;
        }
    }

    action = udev_device_get_action(event_device);

    str = udev_device_get_property_value(event_device, "HOTPLUG");
    hotplug = str != NULL && streq(str, "1");

    // DRM subsystem uevents can have:
    //  - a CONNECTOR and PROPERTY property to signify that a specific drm connector property has changed
    //      see: https://github.com/torvalds/linux/blob/b311c1b497e51a628aa89e7cb954481e5f9dced2/drivers/gpu/drm/drm_sysfs.c#L460
    //
    //  - only a CONNECTOR property to signify that only this drm connector has changed
    //      see: https://github.com/torvalds/linux/blob/b311c1b497e51a628aa89e7cb954481e5f9dced2/drivers/gpu/drm/drm_sysfs.c#L487
    //
    //  - no properties at all
    //      see: https://github.com/torvalds/linux/blob/b311c1b497e51a628aa89e7cb954481e5f9dced2/drivers/gpu/drm/drm_sysfs.c#L441
    //
    // The additional properties are only given as hints, they're not authoritative. E.g. even if the uevent
    // has no CONNECTOR and PROPERTY properties, the event could still be that a single drm connector property changed.

    str = udev_device_get_property_value(event_device, "CONNECTOR");
    have_connector = str != NULL && safe_string_to_uint32(str, &connector_id);

    str = udev_device_get_property_value(event_device, "PROPERTY");
    have_property = str != NULL && safe_string_to_uint32(str, &property_id);

    struct drm_uevent uevent = {
        .action = action,
        .sysnum = sysnum,
        .hotplug = hotplug,
        .have_connector = have_connector,
        .connector_id = connector_id,
        .have_property = have_property,
        .property_id = property_id
    };

    m->listener.on_uevent(&uevent, m->listener_userdata);
    
    // The sysnum and action string is owned by the udev device, and we use it without dup-ing, so we
    // need to free the udev_device after on_drm_subsystem_change.
    udev_device_unref(event_device);
    return;
}

int drm_monitor_get_fd(struct drm_monitor *m) {
    return udev_monitor_get_fd(m->monitor);
}
