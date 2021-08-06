#pragma once
#include <memory>
#include <set>

#ifdef HAS_GBM
#   include <gbm.h>
#endif
#ifdef HAS_EGL
#   include <EGL/egl.h>
#endif

enum pixfmt {
    kRGB565,
    kARGB8888,
    kXRGB8888,
    kBGRA8888,
    kRGBA8888,
};

template<typename T>
struct point {
    T x, y;
};

template<typename T>
struct quadrangle {
    point<T> left_top, right_top, bottom_right, bottom_left;

    rect bounding_rect() const {
        return rect::make_ltrb(
            std::min({left_top.x, right_top.x, bottom_right.x, bottom_left.x}),
            std::min({left_top.y, right_top.y, bottom_right.y, bottom_left.y}),
            std::max({left_top.x, right_top.x, bottom_right.x, bottom_left.x}),
            std::max({left_top.y, right_top.y, bottom_right.y, bottom_left.y})
        );
    }
};

template<typename T>
struct rect {
    static rect make_ltwh(T l, T t, T w, T h) {
        return {
            {l, t},
            {l + w, l + h}
        };
    }

    static rect make_ltrb(T l, T t, T r, T b) {
        return {
            {l, t},
            {r, b}
        };
    }

    T left() const { return left_top.x; }
    T top() const { return left_top.y; }
    T right() const { return bottom_right.x; }
    T bottom() const { return bottom_right.y; }
    T width() const { return right() - left(); }
    T height() const { return bottom() - top(); }

    point<T> left_top, bottom_right;
};

enum display_buffer_layer_rotation {
    kDspBufLayerRotation0,
    kDspBufLayerRotation90,
    kDspBufLayerRotation180,
    kDspBufLayerRotation270,
};

enum display_buffer_layer_reflection {
    kDspBufLayerReflectX,
    kDspBufLayerReflectY
};

struct display_buffer_layer {
    display_buffer_layer(
        const rect<int>& buffer_rect_,
        const rect<int>& display_rect_,
        std::shared_ptr<display_buffer> buffer_
    )   : buffer_rect(buffer_rect_),
          display_rect(display_rect_),
          buffer(buffer_) {}
    
    display_buffer_layer(
        const rect<int>& buffer_rect_,
        const rect<int>& display_rect_,
        std::shared_ptr<display_buffer> buffer_,
        display_buffer_layer_rotation rotation_,
        display_buffer_layer_reflection reflection_
    )   : buffer_rect(buffer_rect_),
          display_rect(display_rect_),
          buffer(buffer_),
          rotation(rotation_),
          reflection(reflection_) {}

    rect<int> buffer_rect;
    rect<int> display_rect;
    std::shared_ptr<display_buffer> buffer;
    std::optional<display_buffer_layer_rotation> rotation;
    std::optional<display_buffer_layer_reflection> reflection;
}; 

struct software_buffer {
    int width, height, stride;
    enum pixfmt format;
    uint8_t *vmem;
};

struct gem_bo {
    int width, height, stride;
    enum pixfmt format;
    uint32_t gem_bo_handle;
};

class display_buffer {
    public:
        virtual void on_release() {}
        int width() const { return width_; }
        int height() const { return height_; }
        pixfmt format() const { return format_; }
        const class display& display() const { return display_; }
    protected:
        display_buffer(int width, int height, pixfmt format, const class display& display)
            : width_(width),
              height_(height),
              format_(format),
              display_(display) {}
        
        int width_;
        int height_;
        pixfmt format_;
        const class display& display_;

};

class display {
    public:
        virtual std::shared_ptr<presenter> make_presenter();
#ifdef HAS_GBM
        virtual std::shared_ptr<display_buffer> import(gbm_bo* bo);
#endif
        virtual std::shared_ptr<display_buffer> import(const software_buffer& sw_buffer);
        virtual std::shared_ptr<display_buffer> import(const gem_bo& gem_bo);
#ifdef HAS_EGL
        virtual std::shared_ptr<display_buffer> import(EGLImage egl_image);
#endif

        int width() const { return width_; }
        int height() const { return height_; }
        double refresh_rate() const { return refresh_rate_; }
        double pixel_ratio() const { return pixel_ratio_; }
        gbm_device *gbm_device() const { return gbm_device_; }
    private:
        int width_;
        int height_;
        double refresh_rate_;
        double pixel_ratio_;
#ifdef HAS_GBM
        struct gbm_device *gbm_device_;
#endif
};

class presenter {
    public:
        virtual void push_display_buffer_layer(const display_buffer_layer& layer) = 0;
        virtual void push_placeholder_layer() = 0;
        virtual void present() = 0;

        const class display& display() const { return display_; }
        class display& display() { return display_; }

    protected:
        presenter(class display& display)
            : display_(display) {}

        class display& display_;
};
