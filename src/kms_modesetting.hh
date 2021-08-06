#pragma once
#include <algorithm>
#include <cstring>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <bitset>
#include <system_error>
#include <modesetting.hh>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define DRM_MAX_PLANES 32
#define DRM_MAX_CRTCS 32

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

struct std::default_delete<drmModeFB> {
    constexpr default_delete() noexcept = default;
    void operator()(drmModeFB* fb) const {
        drmModeFreeFB(fb);
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

    std::optional<const drmModePropertyRes&> find_prop_info(const std::string& name) const {
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

    std::optional<uint64_t> get_prop_value(uint32_t id) const {
        auto i = std::find(
            props->props,
            props->props + props->count_props,
            id
        ) - props->props;

        if (i == props->count_props) {
            return {};
        }

        return props->prop_values[i];
    }

    std::optional<uint64_t> get_prop_value(const std::string& name) const {
        auto id = find_prop_id_opt(name);
        if (!id.has_value()) {
            return {};
        }

        return get_prop_value(*id);
    }

    bool has_prop(const std::string& name) const {
        return find_prop_info(name).has_value();
    }

    std::optional<uint32_t> find_prop_id_opt(const std::string& name) const {
        auto result = find_prop_info(name);
        if (result.has_value()) {
            return result.value().prop_id;
        } else {
            return std::nullopt;
        }
    }

    uint32_t find_prop_id(const std::string& name) const {
        return find_prop_id_opt(name).value();
    }
};

struct kms_connector {
    kms_connector(const kms_interface &face, uint32_t connector_id)
        : connector(face.get_connector(connector_id)),
          props(face.get_connector_props(connector_id)),
          props_info(face.get_all_property_res(*props)) {}
    
    std::unique_ptr<drmModeConnector> connector;
    std::unique_ptr<drmModeObjectProperties> props;
    std::vector<std::unique_ptr<drmModePropertyRes>> props_info;
};

struct kms_encoder {
    kms_encoder(const kms_interface &face, uint32_t encoder_id)
        : encoder(face.get_encoder(encoder_id)),
          props(face.get_encoder_props(encoder_id)),
          props_info(face.get_all_property_res(*props)) {}
    
    std::unique_ptr<drmModeEncoder> encoder;
    std::unique_ptr<drmModeObjectProperties> props;
    std::vector<std::unique_ptr<drmModePropertyRes>> props_info;
};

struct kms_crtc {
    kms_crtc(const kms_interface &face, int index_, uint32_t crtc_id)
        : crtc(face.get_crtc(crtc_id)),
          props(face.get_crtc_props(crtc_id)),
          props_info(face.get_all_property_res(*props)),
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

template<typename T>
struct iterable {
    T _begin;
    T _end;

    iterable(T begin, T end)
        : _begin(begin),
          _end(end) {}
    
    iterable(const T begin, const T end)
        : _begin(const_cast<T>(begin)),
          _end(const_cast<T>(end)) {}

    const T begin() const { return _begin; }
    T begin() { return _begin; }
    const T end() const { return _end; }
    T end() { return _end; }
};

template<typename T>
struct ptr_iterable {
    T* _begin;
    T* _end;

    ptr_iterable(T* begin, T* end)
        : _begin(begin),
          _end(end) {}

    ptr_iterable(const T* begin, const T* end)
        : _begin(const_cast<T>(begin)),
          _end(const_cast<T>(end)) {}

    const T* begin() const { return _begin; }
    T* begin() { return _begin; }
    const T* end() const { return _end; }
    T* end() { return _end; }
};

struct kms_plane {
    public:
        class modifier_iterator {
            public:
                uint64_t operator*() const { return cursor_->modifier; }

                modifier_iterator& operator++() {
                    do {
                        cursor_++;
                    } while (((cursor_->formats & format_bitmask) == 0) && (cursor_ != end_));
                    return *this;
                }

                modifier_iterator operator++(int) {
                    auto previous = *this;
                    this->operator++();
                    return previous;
                }

                bool operator==(const modifier_iterator& other) {
                    return format_bitmask == other.format_bitmask && cursor_ == other.cursor_;
                }

                bool operator!=(const modifier_iterator& other) {
                    return format_bitmask != other.format_bitmask || cursor_ != other.cursor_;
                }
            private:
                friend class kms_plane;
                modifier_iterator(uint64_t bmask, const drm_format_modifier *cursor, const drm_format_modifier *end)
                    : format_bitmask(bmask),
                      cursor_(cursor),
                      end_(end) {}

                uint64_t format_bitmask;
                const drm_format_modifier* cursor_;
                const drm_format_modifier* end_;
        };

        kms_plane(const kms_interface &face, int index, uint32_t plane_id)
            : plane(face.get_plane(plane_id)),
              props(face, face.get_plane_props(plane_id)),
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
                props.find_prop_id_opt("rotation"),
                props.find_prop_id_opt("zpos"),
                props.find_prop_id_opt("IN_FORMATS")
              }) {
                  auto type = props.get_prop_value("type");
                  this->type = static_cast<kms_plane_type>(type.value());

                  auto zpos = props.find_prop_info("zpos");
                  if (zpos.has_value()) {
                      min_zpos = zpos->values[0];
                      max_zpos = zpos->values[1];
                  } else {
                      min_zpos = 0;
                      max_zpos = 1;
                  }

                  auto rotation = props.find_prop_info("rotation");
                  if (rotation.has_value()) {
                      for (auto i = 0; i < rotation->count_values; i++) {
                          switch (rotation->values[i]) {
                              case 0:
                                supported_rotations.emplace(kDspBufLayerRotation0);
                                break;
                              case 1:
                                supported_rotations.emplace(kDspBufLayerRotation90);
                                break;
                              case 2:
                                supported_rotations.emplace(kDspBufLayerRotation180);
                                break;
                              case 3:
                                supported_rotations.emplace(kDspBufLayerRotation270);
                                break;
                              case 4:
                                supported_reflections.emplace(kDspBufLayerReflectX);
                                break;
                              case 5:
                                supported_reflections.emplace(kDspBufLayerReflectY);
                                break;
                              default:
                                __builtin_unreachable();
                          }
                      }
                  }

                  auto in_formats = props.find_prop_info("IN_FORMATS");
                  if (in_formats.has_value()) {
                      this->in_formats = {
                          face,
                          in_formats->blob_ids[0]
                      };
                  } else {
                      this->in_formats = {};
                  }
              };

        std::unique_ptr<drmModePlane> plane;
        kms_props props;

        struct {
            uint32_t crtc_id, fb_id,
                src_x, src_y, src_w, src_h,
                crtc_x, crtc_y, crtc_w, crtc_h;
            std::optional<uint32_t> rotation, zpos, in_formats;
        } property_ids;

        kms_plane_type type;
        int64_t min_zpos, max_zpos;
        std::unordered_set<display_buffer_layer_rotation> supported_rotations;
        std::unordered_set<display_buffer_layer_reflection> supported_reflections;
        std::optional<kms_property_blob> in_formats;

        std::optional<const ptr_iterable<uint32_t>> supported_formats() const {
            if (!in_formats.has_value()) {
                return {};
            }

            const auto& header = *format_modifier_blob();
            
            const uint32_t *begin = reinterpret_cast<const uint32_t*>(
                reinterpret_cast<const uint8_t*>(in_formats.value().data()) + header.formats_offset
            );

            const uint32_t *end = begin + header.count_formats;
            
            return ptr_iterable(begin, end);
        }

        std::optional<const iterable<modifier_iterator>> supported_modifiers_for_format(uint32_t format) const {
            if (!in_formats.has_value()) {
                return {};
            }

            auto fmts = *supported_formats();

            auto i = std::find(fmts.begin(), fmts.end(), format);
            if (i == fmts.end()) {
                return {};
            }

            uint64_t bitmask = 1 << (i - fmts.begin());

            const auto mods = *format_modifiers();
            return iterable(
                modifier_iterator(bitmask, mods.begin(), mods.end()),
                modifier_iterator(bitmask, mods.end(), mods.end())
            );
        }
    private:
        std::optional<const drm_format_modifier_blob&> format_modifier_blob() const {
            if (!in_formats.has_value()) {
                return {};
            }

            return *reinterpret_cast<const drm_format_modifier_blob*>((*in_formats).data());
        }

        std::optional<const ptr_iterable<drm_format_modifier>> format_modifiers() const {
            if (!in_formats.has_value()) {
                return {};
            }

            auto header = (*format_modifier_blob());
            
            auto begin = reinterpret_cast<const drm_format_modifier*>(
                reinterpret_cast<const uint8_t*>((*in_formats).data()) +
                header.modifiers_offset
            );
            auto end = begin + header.count_modifiers;

            return ptr_iterable(begin, end);
        }
};

struct kms_resources {
    kms_resources(const kms_interface &face)
        : res_(face.get_res()),
          plane_res_(face.get_plane_res()) {
        
        for (int i = 0; i < res_->count_connectors; i++) {
            connectors_.emplace_back(kms_connector(face, res_->connectors[i]));
        }
        for (int i = 0; i < res_->count_encoders; i++) {
            connectors_.emplace_back(kms_encoder(face, res_->encoders[i]));
        }
        for (int i = 0; i < res_->count_crtcs; i++) {
            connectors_.emplace_back(kms_crtc(face, i, res_->crtcs[i]));
        }
        for (int i = 0; i < plane_res_->count_planes; i++) {
            connectors_.emplace_back(kms_plane(face, i, plane_res_->planes[i]));
        }
    }


    std::unique_ptr<drmModeRes> res_;
    std::unique_ptr<drmModePlaneRes> plane_res_;
    std::vector<kms_connector> connectors_;
    std::vector<kms_encoder> encoders_;
    std::vector<kms_crtc> crtcs_;
    std::vector<kms_plane> planes_;
};

[[ noreturn ]] void throw_errno_exception(const char *what_arg) {
    throw std::system_error(errno, std::generic_category(), what_arg);
}

class kms_interface {
    public:
        kms_interface(int fd)
            : fd_(fd) {}

        std::unique_ptr<drmModeRes> get_res() const {
            auto ptr = drmModeGetResources(fd_);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetResources");
            }

            return std::unique_ptr<drmModeRes>(ptr);
        }

        std::unique_ptr<drmModePlaneRes> get_plane_res() const {
            auto ptr = drmModeGetPlaneResources(fd_);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetPlaneResources");
            }

            return std::unique_ptr<drmModePlaneRes>(ptr);
        }

        std::unique_ptr<drmModeConnector> get_connector(uint32_t id) const {
            auto ptr = drmModeGetConnector(fd_, id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetConnector");
            }

            return std::unique_ptr<drmModeConnector>(ptr);
        }

        std::unique_ptr<drmModeEncoder> get_encoder(uint32_t id) const {
            auto ptr = drmModeGetEncoder(fd_, id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetEncoder");
            }

            return std::unique_ptr<drmModeEncoder>(ptr);
        }

        std::unique_ptr<drmModeCrtc> get_crtc(uint32_t id) const {
            auto ptr = drmModeGetCrtc(fd_, id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetCrtc");
            }

            return std::unique_ptr<drmModeCrtc>(ptr);
        }

        std::unique_ptr<drmModePlane> get_plane(uint32_t id) const {
            auto ptr = drmModeGetPlane(fd_, id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetPlane");
            }

            return std::unique_ptr<drmModePlane>(ptr);
        }

        std::unique_ptr<drmModeFB> get_fb(uint32_t id) const {
            auto ptr = drmModeGetFB(fd_, id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetFB");
            }

            return std::unique_ptr<drmModeFB>(ptr);
        }

        std::unique_ptr<drmModeObjectProperties> get_obj_props(uint32_t id, uint32_t type) const {
            auto ptr = drmModeObjectGetProperties(fd_, id, type);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeObjectGetProperties");
            }

            return std::unique_ptr<drmModeObjectProperties>(ptr);
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
            auto ptr = drmModeGetProperty(fd_, prop_id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetProperty");
            }

            return std::unique_ptr<drmModePropertyRes>(ptr);
        }

        std::vector<std::unique_ptr<drmModePropertyRes>> get_all_property_res(const drmModeObjectProperties& props) const {
            std::vector<std::unique_ptr<drmModePropertyRes>> result;

            for (int i = 0; i < props.count_props; i++) {
                auto ptr = drmModeGetProperty(fd_, props.props[i]);
                if (ptr == nullptr) {
                    throw_errno_exception("drmModeGetProperty");
                }

                result.emplace_back(std::unique_ptr<drmModePropertyRes>(ptr));
            }

            return result;
        }

        std::unique_ptr<drmModePropertyBlobRes> get_property_blob(uint32_t blob_id) const {
            auto ptr = drmModeGetPropertyBlob(fd_, blob_id);
            if (ptr == nullptr) {
                throw_errno_exception("drmModeGetPropertyBlob");
            }

            return std::unique_ptr<drmModePropertyBlobRes>(ptr);
        }

        std::unique_ptr<drmModeAtomicReq> new_atomic_request() const {
            auto ptr = drmModeAtomicAlloc();
            if (ptr == nullptr) {
                throw_errno_exception("drmModeAtomicAlloc");
            }

            return std::unique_ptr<drmModeAtomicReq>(ptr);
        }

        void atomic_add_prop(std::unique_ptr<drmModeAtomicReq>& req, uint32_t object_id, uint32_t property_id, uint64_t value) const {
            int ok = drmModeAtomicAddProperty(req.get(), object_id, property_id, value);
            if (ok < 0) {
                throw_errno_exception("drmModeAtomicAddProperty");
            }
        }

        void atomic_add_prop(std::unique_ptr<drmModeAtomicReq>& req, const kms_connector &connector, uint32_t property_id, uint64_t value) const {
            return atomic_add_prop(req, connector.connector->connector_id, property_id, value);
        }

        void atomic_add_prop(std::unique_ptr<drmModeAtomicReq>& req, const kms_crtc &crtc, uint32_t property_id, uint64_t value) const {
            return atomic_add_prop(req, crtc.crtc->crtc_id, property_id, value);
        }

        void atomic_add_prop(std::unique_ptr<drmModeAtomicReq>& req, const kms_plane &plane, uint32_t property_id, uint64_t value) const {
            return atomic_add_prop(req, plane.plane->plane_id, property_id, value);
        }

        void atomic_commit(drmModeAtomicReq *req, uint32_t flags, void *userdata) const {
            int ok = drmModeAtomicCommit(fd_, req, flags, userdata);
            if (ok < 0) {
                throw_errno_exception("drmModeAtomicCommit");
            }
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

        const std::vector<kms_display>& displays() const {
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
        uint32_t drm_fb_id;
        int pitch;
        int bpp;
        int depth;
        uint32_t gem_handle;

        kms_display_buffer(
            int width, int height,
            pixfmt format,
            const class display& display,
            uint32_t drm_fb_id_,
            int pitch_, int bpp_, int depth_,
            uint32_t gem_handle_
        ) : display_buffer(width, height, format, display),
            drm_fb_id(drm_fb_id_),
            pitch(pitch_),
            bpp(bpp_),
            depth(depth_),
            gem_handle(gem_handle_) {}

        static kms_display_buffer from_fb_id(
            const kms_interface &face,
            const class display& display,
            uint32_t drm_fb_id
        ) {
            auto fb = face.get_fb(drm_fb_id);
            
            return kms_display_buffer(
                static_cast<int>(fb->width),
                static_cast<int>(fb->height),
                kARGB8888, // TODO: implement,
                display,
                drm_fb_id,
                static_cast<int>(fb->pitch),
                static_cast<int>(fb->bpp),
                static_cast<int>(fb->depth),
                static_cast<int>(fb->handle)
            );
        }
};

class kms_presenter;

class kms_display : display {
    public:
        std::shared_ptr<presenter> make_presenter() override {
            return std::dynamic_pointer_cast<presenter>(std::make_shared<kms_presenter>(*this));
        }

    private:
        friend kmsdev;
        friend kms_presenter;

        kms_display(kmsdev& kmsdev, kms_crtc &crtc)
            : kmsdev_(kmsdev),
              crtc_(crtc),
              allocated_planes_(DRM_MAX_PLANES) {}
        
        const kms_crtc &crtc() const { return crtc_; }
        kms_crtc &crtc() { return crtc_; }

        uint32_t crtc_id() const { return crtc_.crtc->crtc_id; }

        kmsdev& kmsdev_;
        kms_crtc &crtc_;
        std::unordered_set<kms_plane&> allocated_planes_;
};

static uint64_t get_rotation_value(
    std::optional<display_buffer_layer_rotation> rotation,
    std::optional<display_buffer_layer_reflection> reflection
) {
    uint64_t result = 0;
    if (rotation.has_value()) {
        result |= 1 << rotation.value();
    }
    if (reflection.has_value()) {
        result |= 1 << (reflection.value() + 5);
    }
    return result;
}

class kms_presenter : presenter {
    public:
        virtual void push_display_buffer_layer(const display_buffer_layer& layer) override {
            auto buffer = std::dynamic_pointer_cast<kms_display_buffer>(layer.buffer);

            const auto& plane = reserve_plane();
            
            auto req = atomic_req_.get();
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.fb_id, buffer->drm_fb_id);
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.crtc_id, kms_display().crtc_id());
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.src_x, layer.buffer_rect.left() << 16);
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.src_y, layer.buffer_rect.top() << 16);
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.src_w, layer.buffer_rect.width() << 16);
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.src_h, layer.buffer_rect.height() << 16);
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.crtc_x, layer.display_rect.left());
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.crtc_y, layer.display_rect.top());
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.crtc_w, layer.display_rect.width());
            interface_.atomic_add_prop(atomic_req_, plane, plane.property_ids.crtc_h, layer.display_rect.height());

            if (plane.property_ids.zpos.has_value()) {
                interface_.atomic_add_prop(atomic_req_, plane, *plane.property_ids.zpos, current_zpos_);
            }

            if (layer.rotation.has_value() || layer.reflection.has_value()) {
                interface_.atomic_add_prop(
                    atomic_req_,
                    plane,
                    plane.property_ids.rotation.value(),
                    get_rotation_value(layer.rotation, layer.reflection)
                );
            }

            current_zpos_++;
        }

        virtual void push_placeholder_layer() override {
            current_zpos_++;
        }

        virtual void present() override {
            interface_.atomic_commit(atomic_req_.get(), 0, nullptr);
        }

    private:
        friend kms_display;

        kms_interface &interface_; 
        std::unique_ptr<drmModeAtomicReq> atomic_req_;
        int64_t current_zpos_;
        std::unordered_set<kms_plane&> available_planes_;

        const class kms_display& kms_display() const { return static_cast<const class kms_display&>(display()); }
        class kms_display& kms_display() { return static_cast<class kms_display&>(display()); }

        kms_plane &reserve_plane() {
            const auto iter = available_planes_.begin();
            auto& plane = *iter;
            available_planes_.erase(iter);
            return plane;
        };

        kms_presenter(class kms_display &display, kms_interface &interface)
            : presenter(static_cast<class display&>(display)),
              interface_(interface),
              atomic_req_(interface.new_atomic_request()),
              current_zpos_(kms_display().crtc().min_zpos),
              available_planes_(display.allocated_planes_) {}
};
