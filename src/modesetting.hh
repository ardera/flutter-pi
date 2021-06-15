#pragma once
#include <memory>

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

struct display_buffer_layer {
    int buffer_x, buffer_y, buffer_w, buffer_h;
    int display_x, display_y, display_w, display_h;
    std::shared_ptr<display_buffer> buffer;
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
        virtual void push_display_buffer_layer(const display_buffer_layer& layer);
        virtual void push_placeholder_layer();
        const display& display() const { return display_; }
    
    protected:
        presenter(const class display& display)
            : display_(display) {}

        const class display& display_;
};
