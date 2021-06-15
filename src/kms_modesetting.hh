#pragma once
#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <unordered_set>
#include <modesetting.hh>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct kms_connector_config {
    bool enable;

    bool has_explicit_dimensions;
    int width_mm;
    int height_mm;

    bool has_explicit_pixel_format;
    pixfmt pixel_format;

    bool has_explicit_mode;
    int width;
    int height;
    double refresh_rate;
};

struct kms_device_config {
    bool force_legacy_modesetting;
    bool use_blocking_commits;
    std::map<std::string, kms_connector_config> connector_configs;
    kms_connector_config default_connector_config;
};

struct std::default_delete<drmModeConnector> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeConnector* connector) const {
        drmModeFreeConnector(connector);
    }
};

struct std::default_delete<drmModeEncoder> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeEncoder* encoder) const {
        drmModeFreeEncoder(encoder);
    }
};

struct std::default_delete<drmModeCrtc> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeCrtc* crtc) const {
        drmModeFreeCrtc(crtc);
    }
};

struct std::default_delete<drmModePlane> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModePlane* res) const {
        drmModeFreePlane(res);
    }
};

struct std::default_delete<drmModeRes> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeRes* res) const {
        drmModeFreeResources(res);
    }
};

struct std::default_delete<drmModePlaneRes> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModePlaneRes* res) const {
        drmModeFreePlaneResources(res);
    }
};

struct std::default_delete<drmModeObjectProperties> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeObjectProperties* props) const {
        drmModeFreeObjectProperties(props);
    }
};

struct std::default_delete<drmModePropertyRes> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModePropertyRes* prop) const {
        drmModeFreeProperty(prop);
    }
};

struct std::default_delete<drmModeAtomicReq> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeAtomicReq* req) const {
        drmModeAtomicFree(req);
    }
};

struct kms_property_blob {
    kms_property_blob(const kms_interface &interface, uint32_t blob_id)
        : blob(interface.get_property_blob(blob_id)) {}

    kms_property_blob(const kms_interface &interface, const drmModePropertyRes& prop) {
        if (prop.count_blobs != 1) {
            // throw
        } else {
            blob = interface.get_property_blob(prop.blob_ids[0]);
        }
    }

    std::unique_ptr<drmModePropertyBlobRes> blob;

    const void *data() const {
        return blob->data;
    }

    void *data() {
        return blob->data;
    }
};

struct kms_props {
    kms_props(const kms_interface &interface, std::unique_ptr<drmModeObjectProperties> props_)
        : props(std::move(props_)),
          props_info(interface.get_all_property_res(*props)) {}

    std::unique_ptr<drmModeObjectProperties> props;
    std::vector<std::unique_ptr<drmModePropertyRes>> props_info;

    std::optional<const drmModePropertyRes&> find_prop(const std::string& name) const {
        auto result = std::find_if(
            props_info.begin(),
            props_info.end(),
            [&](const std::unique_ptr<drmModePropertyRes>& prop) {
                return std::strncmp(prop->name, name.c_str(), sizeof(prop->name)) == 0;
            }
        );
        if (result == props_info.end()) {
            return;
        }

        return **result;
    }

    bool has_prop(const std::string& name) const {
        return find_prop(name).has_value();
    }

    uint32_t find_prop_id(const std::string& name) const {
        auto result = find_prop(name);
        if (result.has_value()) {
            return result.value().prop_id;
        } else {
            return (uint32_t) 0xFFFFFFFF;
        }
    }
};

struct kms_connector {
    kms_connector(const kms_interface &dev, uint32_t connector_id)
        : connector(dev.get_connector(connector_id)),
          props(dev.get_connector_props(connector_id)),
          props_info(dev.get_all_property_res(*props)) {}
    
    std::unique_ptr<drmModeConnector> connector;
    std::unique_ptr<drmModeObjectProperties> props;
    std::vector<std::unique_ptr<drmModePropertyRes>> props_info;
};

struct kms_encoder {
    kms_encoder(const kms_interface &dev, uint32_t encoder_id)
        : encoder(dev.get_encoder(encoder_id)),
          props(dev.get_encoder_props(encoder_id)),
          props_info(dev.get_all_property_res(*props)) {}
    
    std::unique_ptr<drmModeEncoder> encoder;
    std::unique_ptr<drmModeObjectProperties> props;
    std::vector<std::unique_ptr<drmModePropertyRes>> props_info;
};

struct kms_crtc {
    kms_crtc(const kms_interface &dev, int index_, uint32_t crtc_id)
        : crtc(dev.get_crtc(crtc_id)),
          props(dev.get_crtc_props(crtc_id)),
          props_info(dev.get_all_property_res(*props)),
          bitmask(1 << index_),
          index(index_) {

    }

    std::unique_ptr<drmModeCrtc> crtc;
    std::unique_ptr<drmModeObjectProperties> props;
    std::vector<std::unique_ptr<drmModePropertyRes>> props_info;

    uint32_t bitmask;
    int index;

    int selected_connector_index;
    struct drm_connector *selected_connector;
    drmModeModeInfo selected_mode;
    uint32_t selected_mode_blob_id;

    bool supports_hardware_cursor;

    bool supports_zpos;
    int min_zpos, max_zpos;
    
    size_t n_formats;
    enum pixfmt *formats2;
};

enum kms_plane_type {
    kOverlay = DRM_PLANE_TYPE_OVERLAY,
    kPrimary = DRM_PLANE_TYPE_PRIMARY,
    kCursor = DRM_PLANE_TYPE_CURSOR
};

enum kms_plane_rotation {
    kRotate0 = DRM_MODE_ROTATE_0,
    kRotate90 = DRM_MODE_ROTATE_90,
    kRotate180 = DRM_MODE_ROTATE_180,
    kRotate270 = DRM_MODE_ROTATE_270,
    kReflectX = DRM_MODE_REFLECT_X,
    kReflectY = DRM_MODE_REFLECT_Y
};

struct kms_plane {
    public:
        kms_plane(const kms_interface &dev, int index, uint32_t plane_id)
            : plane(dev.get_plane(plane_id)),
              props(dev, dev.get_plane_props(plane_id)),
              property_ids({
                props.find_prop_id("CRTC_ID"),
                props.find_prop_id("FB_ID"),
                props.find_prop_id("SRC_X"),
                props.find_prop_id("SRC_Y"),
                props.find_prop_id("SRC_W"),
                props.find_prop_id("SRC_H"),
                props.find_prop_id("CRTC_X"),
                props.find_prop_id("CRTC_Y"),
                props.find_prop_id("CRTC_W"),
                props.find_prop_id("CRTC_H"),
                props.find_prop_id("rotation"),
                props.find_prop_id("zpos"),
                props.find_prop_id("IN_FORMATS")
              }) {
            
            if (props.has_prop("IN_FORMATS")) {
                in_formats.emplace(kms_property_blob(dev, props.find_prop("IN_FORMAT").value()));
            } else {
                in_formats.reset();
            }
        }

        std::unique_ptr<drmModePlane> plane;
        kms_props props;

        struct {
            uint32_t crtc_id, fb_id,
                src_x, src_y, src_w, src_h,
                crtc_x, crtc_y, crtc_w, crtc_h,
                rotation, zpos, in_formats;
        } property_ids;

        kms_plane_type type;
        int min_zpos, max_zpos;
        std::unordered_set<kms_plane_rotation> supported_rotations;

        std::optional<kms_property_blob> in_formats;

        bool supports_format(uint32_t drm_format) const {
            auto header = format_modifier_blob();
            const uint32_t *formats = (const uint32_t*) (((const uint8_t*) (in_formats.value().data())) + header.formats_offset);
            for (unsigned i = 0; i < header.count_formats; i++) {
                if (formats[i] == drm_format) {
                    return true;
                }
            }

            return false;
        }

        bool supports_format(uint32_t drm_format, uint64_t modifier) const {
            auto header = format_modifier_blob();

            const uint32_t *formats = (const uint32_t*) (((const uint8_t*) (in_formats.value().data())) + header.formats_offset);
            uint64_t bitmask = 0;

            for (unsigned i = 0; i < header.count_formats; i++) {
                if (formats[i] == drm_format) {
                    bitmask = 1 << i;
                    break;
                }
            }

            const struct drm_format_modifier *modifiers = (const struct drm_format_modifier *) (((const uint8_t*) (in_formats.value().data())) + header.modifiers_offset);
            for (unsigned i = 0; i < header.count_modifiers; i++) {
                if ((modifiers[i].modifier == modifier) && (modifiers[i].formats & bitmask)) {
                    return true;
                }
            }

            return false;
        }

    private:
        const drm_format_modifier_blob &format_modifier_blob() const {
            return *reinterpret_cast<const drm_format_modifier_blob*>(in_formats.value().data());
        }
};

struct kms_resources {
    kms_resources(const kms_interface &dev)
        : res_(dev.get_res()),
          plane_res_(dev.get_plane_res()) {
        
        for (int i = 0; i < res_->count_connectors; i++) {
            connectors_.emplace_back(kms_connector(dev, res_->connectors[i]));
        }
        for (int i = 0; i < res_->count_encoders; i++) {
            connectors_.emplace_back(kms_encoder(dev, res_->encoders[i]));
        }
        for (int i = 0; i < res_->count_crtcs; i++) {
            connectors_.emplace_back(kms_crtc(dev, i, res_->crtcs[i]));
        }
        for (int i = 0; i < plane_res_->count_planes; i++) {
            connectors_.emplace_back(kms_plane(dev, i, plane_res_->planes[i]));
        }
    }


    std::unique_ptr<drmModeRes> res_;
    std::unique_ptr<drmModePlaneRes> plane_res_;
    std::vector<kms_connector> connectors_;
    std::vector<kms_encoder> encoders_;
    std::vector<kms_crtc> crtcs_;
    std::vector<kms_plane> planes_;
};

class kms_interface {
    public:
        kms_interface(int fd)
            : fd_(fd) {}

        std::unique_ptr<drmModeRes> get_res() const {
            return std::unique_ptr<drmModeRes>(drmModeGetResources(fd_));
        }

        std::unique_ptr<drmModePlaneRes> get_plane_res() const {
            return std::unique_ptr<drmModePlaneRes>(drmModeGetPlaneResources(fd_));
        }

        std::unique_ptr<drmModeConnector> get_connector(uint32_t id) const {
            return std::unique_ptr<drmModeConnector>(drmModeGetConnector(fd_, id));
        }

        std::unique_ptr<drmModeEncoder> get_encoder(uint32_t id) const {
            return std::unique_ptr<drmModeEncoder>(drmModeGetEncoder(fd_, id));
        }

        std::unique_ptr<drmModeCrtc> get_crtc(uint32_t id) const {
            return std::unique_ptr<drmModeCrtc>(drmModeGetCrtc(fd_, id));
        }

        std::unique_ptr<drmModePlane> get_plane(uint32_t id) const {
            return std::unique_ptr<drmModePlane>(drmModeGetPlane(fd_, id));
        }

        std::unique_ptr<drmModeObjectProperties> get_obj_props(uint32_t id, uint32_t type) const {
            return std::unique_ptr<drmModeObjectProperties>(drmModeObjectGetProperties(fd_, id, type));
        }

        std::unique_ptr<drmModeObjectProperties> get_connector_props(uint32_t connector_id) const {
            return get_obj_props(connector_id, DRM_MODE_OBJECT_CONNECTOR);
        }

        std::unique_ptr<drmModeObjectProperties> get_encoder_props(uint32_t encoder_id) const {
            return get_obj_props(encoder_id, DRM_MODE_OBJECT_ENCODER);
        }

        std::unique_ptr<drmModeObjectProperties> get_crtc_props(uint32_t crtc_id) const {
            return get_obj_props(crtc_id, DRM_MODE_OBJECT_CRTC);
        }

        std::unique_ptr<drmModeObjectProperties> get_plane_props(uint32_t plane_id) const {
            return get_obj_props(plane_id, DRM_MODE_OBJECT_PLANE);
        }

        std::unique_ptr<drmModePropertyRes> get_property_res(uint32_t prop_id) const {
            return std::unique_ptr<drmModePropertyRes>(drmModeGetProperty(fd_, prop_id));
        }

        std::vector<std::unique_ptr<drmModePropertyRes>> get_all_property_res(const drmModeObjectProperties& props) const {
            std::vector<std::unique_ptr<drmModePropertyRes>> result;

            for (int i = 0; i < props.count_props; i++) {
                result.emplace_back(drmModeGetProperty(fd_, props.props[i]));
            }

            return result;
        }

        std::unique_ptr<drmModePropertyBlobRes> get_property_blob(uint32_t blob_id) const {
            return std::unique_ptr<drmModePropertyBlobRes>(drmModeGetPropertyBlob(fd_, blob_id));
        }

        std::unique_ptr<drmModeAtomicReq> new_atomic_request() const {
            return std::unique_ptr<drmModeAtomicReq>(drmModeAtomicAlloc());
        }

    private:
        int fd_;
};

class kmsdev {
    public:
        kmsdev(const kms_interface& interface, const struct kms_device_config& config)
            : interface_(interface),
              resources_(interface),
              config_(config),
              displays_(make_displays(resources_, config_)) {}

        kmsdev(int fd, const struct kms_device_config& config)
            : interface_(fd),
              resources_(interface_),
              config_(config),
              displays_(make_displays(resources_, config_)) {}

        const std::vector<kms_display>& displays() {
            return displays_;
        }

    private:
        static std::vector<kms_display> make_displays(const kms_resources& res, const kms_device_config& config) {
            
        }

        kms_interface interface_;
        kms_resources resources_;
        kms_device_config config_;
        std::vector<kms_display> displays_;
};

class kms_display_buffer : display_buffer {
    public:
        uint32_t drm_fb_id() const { return drm_fb_id_; }

    private:
        friend kms_display;

        kms_display_buffer(int width, int height, pixfmt format, const class display& display, uint32_t drm_fb_id)
            : display_buffer(width, height, format, display),
              drm_fb_id_(drm_fb_id) {}

        uint32_t drm_fb_id_;
};

class kms_display : display {
    public:
        std::shared_ptr<presenter> make_presenter() override {
            return std::dynamic_pointer_cast<presenter>(std::make_shared<kms_presenter>(*this));
        }

    private:
        friend kmsdev;

        kms_display(kmsdev& kmsdev, uint32_t crtc_id)
            : kmsdev_(kmsdev),
              crtc_id_(crtc_id),
              allocated_planes_(0) {}

        kmsdev& kmsdev_;
        uint32_t crtc_id_;
        uint32_t allocated_planes_;
};

class kms_presenter : presenter {
    public:
        void push_display_buffer_layer(const display_buffer_layer& layer) override {

        }

        virtual void push_placeholder_layer() override {

        }

    private:
        friend kms_display;

        const class kms_display& kms_display() const { return (const class kms_display&) display(); }
        const kms_interface& kms_interface() const { return kms_display(). }

        kms_presenter(const class kms_display &display)
            : presenter((const class display&) display) {}
};
