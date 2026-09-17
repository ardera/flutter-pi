// SPDX-License-Identifier: MIT
/*
 * CEF bridge
 *
 * Implements the plain-C API declared in cef_bridge.h on top of CEF's C++ API.
 *
 * This file must not include any flutter-pi header -- see cef_bridge.h for why.
 *
 * Copyright (c) 2026, Bojidar Tonchev <bojidar.tonchev@gmail.com>
 */

#include "cef_bridge.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_task.h"
#include "include/cef_version.h"

// Written against CEF 132. CEF 126 added the popup_id parameter to OnBeforePopup,
// which would otherwise surface as a confusing "marked override but does not
// override" error, so fail loudly instead.
#if CEF_VERSION_MAJOR < 126
    #error "The webview_cef plugin requires CEF 126 or newer."
#endif

#define LOG_CEF(...) fprintf(stderr, "[webview_cef] " __VA_ARGS__)

namespace {

/// Runs a closure on a CEF thread. Written against CefTask directly rather than
/// base::BindOnce so it depends only on cef_task.h.
class FnTask : public CefTask {
public:
    explicit FnTask(std::function<void()> fn)
            : fn_(std::move(fn)) {
    }

    FnTask(const FnTask &) = delete;
    FnTask &operator=(const FnTask &) = delete;

    void Execute() override {
        fn_();
    }

private:
    std::function<void()> fn_;

    IMPLEMENT_REFCOUNTING(FnTask);
};

void post_to_ui(std::function<void()> fn) {
    CefPostTask(TID_UI, CefRefPtr<CefTask>(new FnTask(std::move(fn))));
}

// ---------------------------------------------------------------------------
// CefApp -- one per process.
// ---------------------------------------------------------------------------

class WebviewApp : public CefApp, public CefBrowserProcessHandler {
public:
    explicit WebviewApp(std::vector<std::string> switches)
            : switches_(std::move(switches)) {
    }

    WebviewApp(const WebviewApp &) = delete;
    WebviewApp &operator=(const WebviewApp &) = delete;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }

    void OnBeforeCommandLineProcessing(const CefString &process_type, CefRefPtr<CefCommandLine> command_line) override {
        // An empty process type means the browser process. Switches appended
        // here are inherited by the subprocesses, so only do it once.
        if (!process_type.empty()) {
            return;
        }

        for (const std::string &sw : switches_) {
            const size_t eq = sw.find('=');
            if (eq == std::string::npos) {
                command_line->AppendSwitch(sw);
            } else {
                command_line->AppendSwitchWithValue(sw.substr(0, eq), sw.substr(eq + 1));
            }
        }
    }

private:
    std::vector<std::string> switches_;

    IMPLEMENT_REFCOUNTING(WebviewApp);
};

// ---------------------------------------------------------------------------
// CefClient -- one per webview.
// ---------------------------------------------------------------------------

typedef void (*gone_cb_t)(void *userdata);

class WebviewClient : public CefClient,
                      public CefLifeSpanHandler,
                      public CefRenderHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler {
public:
    WebviewClient(int logical_width, int logical_height, double scale, const struct wvcef_host_callbacks *callbacks, void *userdata)
            : logical_width_(logical_width)
            , logical_height_(logical_height)
            , scale_(scale)
            , callbacks_(*callbacks)
            , userdata_(userdata) {
    }

    WebviewClient(const WebviewClient &) = delete;
    WebviewClient &operator=(const WebviewClient &) = delete;

    // -- CefClient ----------------------------------------------------------
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return this;
    }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
        return this;
    }

    // -- CefLifeSpanHandler -------------------------------------------------
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        browser_ = browser;
    }

    bool DoClose(CefRefPtr<CefBrowser> browser) override {
        (void) browser;
        // Let the close proceed; OnBeforeClose follows.
        return false;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        (void) browser;

        // Kept alive until `dying` goes out of scope at the end of this function,
        // so the last reference isn't dropped with the lock held.
        CefRefPtr<CefBrowser> dying;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            dying = browser_;
            browser_ = nullptr;
        }

        // This is the last callback for this browser, and the host frees the
        // object `userdata_` points at while handling it. Drop both before
        // calling out, so a stray later callback can't use a dangling pointer.
        const struct wvcef_host_callbacks callbacks = callbacks_;
        void *const userdata = userdata_;

        memset(&callbacks_, 0, sizeof callbacks_);
        userdata_ = nullptr;

        if (callbacks.on_closed != nullptr) {
            callbacks.on_closed(userdata);
        }

        // Hand the C handle to the bridge for disposal. It only queues it --
        // releasing the last reference to this client from inside one of its own
        // methods would destroy `this` while we're still running.
        if (on_gone_ != nullptr) {
            gone_cb_t cb = on_gone_;
            void *ud = on_gone_userdata_;
            on_gone_ = nullptr;
            cb(ud);
        }
    }

    /// Parameters we don't use are left unnamed -- this signature is long enough
    /// as it is.
    bool
    OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int /* popup_id */, const CefString &target_url, const CefString &, CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures &, CefWindowInfo &, CefRefPtr<CefClient> &, CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *)
        override {
        return redirect_popup(browser, target_url);
    }

    // -- CefRenderHandler ---------------------------------------------------
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect &rect) override {
        (void) browser;

        std::lock_guard<std::mutex> lock(state_mutex_);
        rect.x = 0;
        rect.y = 0;
        rect.width = logical_width_ > 0 ? logical_width_ : 1;
        rect.height = logical_height_ > 0 ? logical_height_ : 1;
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo &screen_info) override {
        (void) browser;

        std::lock_guard<std::mutex> lock(state_mutex_);

        screen_info.device_scale_factor = static_cast<float>(scale_);
        screen_info.depth = 32;
        screen_info.depth_per_component = 8;
        screen_info.is_monochrome = 0;
        screen_info.rect = CefRect(0, 0, logical_width_ > 0 ? logical_width_ : 1, logical_height_ > 0 ? logical_height_ : 1);
        screen_info.available_rect = screen_info.rect;
        return true;
    }

    bool GetScreenPoint(CefRefPtr<CefBrowser> browser, int view_x, int view_y, int &screen_x, int &screen_y) override {
        (void) browser;
        screen_x = view_x;
        screen_y = view_y;
        return true;
    }

    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override {
        popup_visible_ = show;

        if (!show) {
            popup_rect_ = CefRect();
            popup_buffer_.clear();
            view_buffer_.clear();
        }

        // Repaint so the popup appears / disappears right away. This also gives
        // us the full view frame we need to composite the popup onto.
        if (browser != nullptr) {
            browser->GetHost()->Invalidate(PET_VIEW);
        }
    }

    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect &rect) override {
        (void) browser;
        popup_rect_ = rect;
    }

    void
    OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList &dirty_rects, const void *buffer, int width, int height)
        override {
        (void) browser;
        // Dirty rects are ignored: GLES2 has no GL_UNPACK_ROW_LENGTH, so a
        // partial upload would need a per-row loop that costs more than it saves.
        (void) dirty_rects;

        if (callbacks_.on_paint == nullptr || buffer == nullptr || width <= 0 || height <= 0) {
            return;
        }

        const uint8_t *bytes = static_cast<const uint8_t *>(buffer);
        const size_t n_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;

        if (type == PET_POPUP) {
            popup_width_ = width;
            popup_height_ = height;
            popup_buffer_.assign(bytes, bytes + n_bytes);

            if (!view_buffer_.empty()) {
                composite_and_emit();
            }
            return;
        }

        if (!popup_visible_) {
            // Fast path: hand CEF's buffer straight to the host, no copy.
            callbacks_.on_paint(userdata_, buffer, width, height);
            return;
        }

        view_width_ = width;
        view_height_ = height;
        view_buffer_.assign(bytes, bytes + n_bytes);
        composite_and_emit();
    }

    bool
    OnCursorChange(CefRefPtr<CefBrowser> browser, CefCursorHandle cursor, cef_cursor_type_t type, const CefCursorInfo &custom_cursor_info)
        override {
        (void) browser;
        (void) cursor;
        (void) custom_cursor_info;

        if (callbacks_.on_cursor_changed != nullptr) {
            callbacks_.on_cursor_changed(userdata_, static_cast<int>(type));
        }

        // Return true to say we handled it -- there is no window system for CEF
        // to set a cursor on.
        return true;
    }

    // -- CefLoadHandler -----------------------------------------------------
    void OnLoadStart(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, TransitionType transition_type) override {
        (void) browser;
        (void) transition_type;

        if (frame == nullptr || !frame->IsMain() || callbacks_.on_load_start == nullptr) {
            return;
        }

        const std::string url = frame->GetURL().ToString();
        callbacks_.on_load_start(userdata_, url.c_str());
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int http_status_code) override {
        (void) browser;

        if (frame == nullptr || !frame->IsMain() || callbacks_.on_load_end == nullptr) {
            return;
        }

        const std::string url = frame->GetURL().ToString();
        callbacks_.on_load_end(userdata_, url.c_str(), http_status_code);
    }

    void OnLoadError(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        ErrorCode error_code,
        const CefString &error_text,
        const CefString &failed_url
    ) override {
        (void) browser;

        // Sub-frame errors are noise for the app; only report the main frame.
        if (frame != nullptr && !frame->IsMain()) {
            return;
        }

        if (callbacks_.on_load_error != nullptr) {
            const std::string text = error_text.ToString();
            const std::string url = failed_url.ToString();
            callbacks_.on_load_error(userdata_, static_cast<int>(error_code), text.c_str(), url.c_str());
        }
    }

    // -- CefDisplayHandler --------------------------------------------------
    void OnAddressChange(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &url) override {
        (void) browser;

        if (frame != nullptr && !frame->IsMain()) {
            return;
        }

        if (callbacks_.on_url_changed != nullptr) {
            const std::string str = url.ToString();
            callbacks_.on_url_changed(userdata_, str.c_str());
        }
    }

    void OnTitleChange(CefRefPtr<CefBrowser> browser, const CefString &title) override {
        (void) browser;
        if (callbacks_.on_title_changed != nullptr) {
            const std::string str = title.ToString();
            callbacks_.on_title_changed(userdata_, str.c_str());
        }
    }

    bool OnTooltip(CefRefPtr<CefBrowser> browser, CefString &text) override {
        (void) browser;

        if (callbacks_.on_tooltip != nullptr) {
            const std::string str = text.ToString();
            callbacks_.on_tooltip(userdata_, str.c_str());
        }

        // The dart side draws the tooltip.
        return true;
    }

    bool
    OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level, const CefString &message, const CefString &source, int line)
        override {
        (void) browser;

        if (callbacks_.on_console_message != nullptr) {
            const std::string message_str = message.ToString();
            const std::string source_str = source.ToString();
            callbacks_.on_console_message(userdata_, static_cast<int>(level), message_str.c_str(), source_str.c_str(), line);
        }

        // Let CEF log it as well.
        return false;
    }

    // -- Used by the C API --------------------------------------------------

    /// A strong reference, so the caller can keep using the browser even if CEF
    /// closes it in the meantime. Null once it's gone.
    CefRefPtr<CefBrowser> browser() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return browser_;
    }

    /// Only called while the browser is being set up or torn down, never
    /// concurrently with OnBeforeClose, so it needs no lock.
    void set_on_gone(gone_cb_t cb, void *userdata) {
        on_gone_ = cb;
        on_gone_userdata_ = userdata;
    }

    void set_size(int logical_width, int logical_height, double scale) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        logical_width_ = logical_width;
        logical_height_ = logical_height;
        scale_ = scale;
    }

private:
    /// There is nowhere to put a second window, so target=_blank and
    /// window.open() load into this view instead of spawning a popup browser
    /// that nothing would ever render.
    bool redirect_popup(CefRefPtr<CefBrowser> browser, const CefString &target_url) {
        if (browser != nullptr && !target_url.empty()) {
            browser->GetMainFrame()->LoadURL(target_url);
        }

        // Cancel the popup.
        return true;
    }

    /// Draws popup_buffer_ over a copy of view_buffer_ and emits the result.
    void composite_and_emit() {
        if (view_buffer_.empty() || view_width_ <= 0 || view_height_ <= 0) {
            return;
        }

        composite_buffer_ = view_buffer_;

        if (!popup_buffer_.empty() && popup_width_ > 0 && popup_height_ > 0) {
            // Read once, and not while the host callback below is running -- the
            // host takes locks of its own in on_paint.
            double scale;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                scale = scale_;
            }

            // popup_rect_ is logical, the buffers are physical pixels.
            const int off_x = static_cast<int>(popup_rect_.x * scale);
            const int off_y = static_cast<int>(popup_rect_.y * scale);

            for (int row = 0; row < popup_height_; row++) {
                const int dst_y = off_y + row;
                if (dst_y < 0 || dst_y >= view_height_) {
                    continue;
                }

                int dst_x = off_x;
                int src_x = 0;
                int n_px = popup_width_;

                if (dst_x < 0) {
                    src_x = -dst_x;
                    n_px -= src_x;
                    dst_x = 0;
                }
                if (dst_x + n_px > view_width_) {
                    n_px = view_width_ - dst_x;
                }
                if (n_px <= 0) {
                    continue;
                }

                memcpy(
                    composite_buffer_.data() + (static_cast<size_t>(dst_y) * view_width_ + dst_x) * 4,
                    popup_buffer_.data() + (static_cast<size_t>(row) * popup_width_ + src_x) * 4,
                    static_cast<size_t>(n_px) * 4
                );
            }
        }

        callbacks_.on_paint(userdata_, composite_buffer_.data(), view_width_, view_height_);
    }

    /// Guards `browser_` and the geometry below -- the only members touched from
    /// more than one thread. CEF sets and clears browser_ on its UI thread and
    /// reads the geometry there (GetViewRect, GetScreenInfo), while the host reads
    /// browser_ and writes the geometry (set_size) from its own thread.
    ///
    /// Never held while calling a host callback: those take locks of their own.
    mutable std::mutex state_mutex_;

    CefRefPtr<CefBrowser> browser_;

    int logical_width_;
    int logical_height_;
    double scale_;

    struct wvcef_host_callbacks callbacks_;
    void *userdata_;

    gone_cb_t on_gone_ = nullptr;
    void *on_gone_userdata_ = nullptr;

    bool popup_visible_ = false;
    CefRect popup_rect_;
    std::vector<uint8_t> popup_buffer_;
    int popup_width_ = 0;
    int popup_height_ = 0;

    std::vector<uint8_t> view_buffer_;
    std::vector<uint8_t> composite_buffer_;
    int view_width_ = 0;
    int view_height_ = 0;

    IMPLEMENT_REFCOUNTING(WebviewClient);
};

bool g_initialized = false;

/// Incremented on whichever thread creates a browser, decremented on CEF's UI
/// thread when one closes, read from both.
std::atomic<int> g_n_live_browsers{ 0 };

CefRefPtr<WebviewApp> g_app;

cef_log_severity_t to_log_severity(int severity) {
    switch (severity) {
        case 1: return LOGSEVERITY_VERBOSE;
        case 2: return LOGSEVERITY_INFO;
        case 3: return LOGSEVERITY_WARNING;
        case 4: return LOGSEVERITY_ERROR;
        case 5: return LOGSEVERITY_FATAL;
        case 99: return LOGSEVERITY_DISABLE;
        default: return LOGSEVERITY_DEFAULT;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

/// Handle handed out to the C side. Owned by the *caller*, which releases it with
/// wvcef_browser_destroy() once on_closed has told it the browser is gone.
///
/// Not thread safe in itself -- the owner serialises its own calls, which plugin.c
/// does by only ever touching a handle from flutter-pi's platform thread. What it
/// does survive is CEF closing the browser underneath it on another thread: the
/// client outlives that, its browser() goes null, and every call below turns into a
/// no-op until the owner gets around to destroying the handle.
struct wvcef_browser {
    CefRefPtr<WebviewClient> client;
    bool close_requested;
};

namespace {

/// Called on CEF's UI thread, from inside the client's own OnBeforeClose.
///
/// Deliberately does *not* free the handle. The owner is still holding that
/// pointer on another thread and has no way to know it just became invalid;
/// freeing it here is a use-after-free waiting for the next event the browser
/// happens to get. It goes away in wvcef_browser_destroy() instead.
void on_client_gone(void *userdata) {
    (void) userdata;

    // Only ever decremented here, and OnBeforeClose is always on the UI thread,
    // so the read and the decrement can't interleave with another decrement.
    if (g_n_live_browsers.load() > 0) {
        g_n_live_browsers--;
    }
}

CefRefPtr<CefBrowser> browser_of(struct wvcef_browser *browser) {
    if (browser == nullptr || browser->client == nullptr) {
        return nullptr;
    }
    return browser->client->browser();
}

}  // namespace

int wvcef_execute_subprocess(int argc, char **argv) {
    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, nullptr, nullptr);
}

int wvcef_initialize(const struct wvcef_init_options *options) {
    if (options == nullptr) {
        return 22 /* EINVAL */;
    }

    if (g_initialized) {
        return 0;
    }

    if (options->subprocess_path == nullptr || options->resources_dir == nullptr || options->locales_dir == nullptr) {
        LOG_CEF("wvcef_initialize: subprocess_path, resources_dir and locales_dir are required.\n");
        return 22 /* EINVAL */;
    }

    std::vector<std::string> switches;
    for (size_t i = 0; i < options->n_switches; i++) {
        if (options->switches[i] != nullptr && options->switches[i][0] != '\0') {
            switches.emplace_back(options->switches[i]);
        }
    }

    g_app = new WebviewApp(std::move(switches));

    // Don't hand flutter-pi's own argv to Chromium -- it would try to interpret
    // "--release" and the asset bundle path as Chromium switches. Everything we
    // want on the command line is appended in OnBeforeCommandLineProcessing.
    char argv0[] = "flutter-pi";
    char *fake_argv[] = { argv0, nullptr };
    CefMainArgs main_args(1, fake_argv);

    CefSettings settings;
    settings.no_sandbox = 1;
    settings.windowless_rendering_enabled = 1;

    // CEF gets its own thread for the browser process message loop.
    //
    // The alternative, external_message_pump, lets the host drive the loop from
    // its own event loop and makes the host thread CEF's UI thread -- which is
    // tempting, because then every callback can touch host state with no locks.
    // But a Chromium task on the UI thread will sometimes block waiting for more
    // UI thread work, and only a real message loop can nest and deliver it; an
    // external pump is stuck inside its one CefDoMessageLoopWork() call. When that
    // happens the process stops for good with the page still looking alive. It is
    // not a corner case -- a WebGL page reached it within seconds -- and CEF's own
    // documentation recommends against the option for exactly this sort of reason.
    settings.external_message_pump = 0;
    settings.multi_threaded_message_loop = 1;

    settings.command_line_args_disabled = 0;
    settings.log_severity = to_log_severity(options->log_severity);

    CefString(&settings.browser_subprocess_path).FromString(options->subprocess_path);
    CefString(&settings.resources_dir_path).FromString(options->resources_dir);
    CefString(&settings.locales_dir_path).FromString(options->locales_dir);

    if (options->cache_path != nullptr && options->cache_path[0] != '\0') {
        CefString(&settings.cache_path).FromString(options->cache_path);
        // CEF >= 120 wants root_cache_path set, with cache_path equal to it or
        // below it.
        CefString(&settings.root_cache_path).FromString(options->cache_path);
    }
    if (options->user_agent != nullptr && options->user_agent[0] != '\0') {
        CefString(&settings.user_agent).FromString(options->user_agent);
    }
    if (options->log_file != nullptr && options->log_file[0] != '\0') {
        CefString(&settings.log_file).FromString(options->log_file);
    }

    if (!CefInitialize(main_args, settings, g_app, nullptr)) {
        LOG_CEF("CefInitialize failed.\n");
        g_app = nullptr;
        return 5 /* EIO */;
    }

    g_initialized = true;
    // CEF_VERSION is the only version macro that's spelled the same across
    // releases; it reads like "132.3.2+g4997b2f+chromium-132.0.6834.161".
    LOG_CEF("CEF %s initialized.\n", CEF_VERSION);
    return 0;
}

bool wvcef_is_initialized(void) {
    return g_initialized;
}

int wvcef_n_live_browsers(void) {
    return g_n_live_browsers.load();
}

void wvcef_shutdown(void) {
    if (!g_initialized) {
        return;
    }

    g_initialized = false;

    // Stops CEF's message loop thread and joins it, so any handle-freeing task
    // still queued on the UI thread has run by the time this returns.
    CefShutdown();
    g_app = nullptr;
}

struct wvcef_browser *wvcef_browser_create(
    const char *url,
    int width,
    int height,
    double device_pixel_ratio,
    int frame_rate,
    const struct wvcef_host_callbacks *callbacks,
    void *userdata
) {
    if (!g_initialized || callbacks == nullptr) {
        return nullptr;
    }

    if (width <= 0) {
        width = 1;
    }
    if (height <= 0) {
        height = 1;
    }
    if (device_pixel_ratio <= 0.0) {
        device_pixel_ratio = 1.0;
    }
    if (frame_rate < 1) {
        frame_rate = 30;
    } else if (frame_rate > 60) {
        frame_rate = 60;
    }

    struct wvcef_browser *handle = new struct wvcef_browser();
    handle->close_requested = false;
    handle->client = new WebviewClient(width, height, device_pixel_ratio, callbacks, userdata);
    handle->client->set_on_gone(on_client_gone, handle);

    const std::string target = (url != nullptr && url[0] != '\0') ? url : "about:blank";

    // CreateBrowserSync is the one CEF call in this file that insists on the UI
    // thread -- everything else is documented as callable from any browser process
    // thread -- so it is posted there and this thread waits for the answer. The
    // caller needs the browser id to answer the channel call that asked for a
    // webview, and there is no id until the browser exists.
    //
    // Shared state on the heap, not captured by reference: on the timeout path
    // this function returns while the task may still be queued, and a task writing
    // into a stack frame that has gone away is a far worse problem than the one
    // the timeout is protecting against.
    struct create_state {
        std::mutex lock;
        std::condition_variable cv;
        bool done = false;
        bool created = false;
    };
    auto state = std::make_shared<create_state>();

    // Counted before the browser exists, not after: OnBeforeClose decrements, and
    // a browser that closes the instant it opens would otherwise decrement a zero
    // that this thread then raises to one -- leaving a phantom browser that
    // wvcef_shutdown() waits for forever.
    g_n_live_browsers++;

    post_to_ui([state, handle, target, frame_rate]() {
        CefWindowInfo window_info;
        window_info.SetAsWindowless(0);

        CefBrowserSettings browser_settings;
        browser_settings.windowless_frame_rate = frame_rate;
        browser_settings.background_color = CefColorSetARGB(255, 255, 255, 255);

        const bool ok =
            CefBrowserHost::CreateBrowserSync(window_info, handle->client, CefString(target), browser_settings, nullptr, nullptr) !=
            nullptr;

        std::lock_guard<std::mutex> guard(state->lock);
        state->created = ok;
        state->done = true;
        state->cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> guard(state->lock);
        if (!state->cv.wait_for(guard, std::chrono::seconds(15), [&state]() { return state->done; })) {
            LOG_CEF("Timed out waiting for CEF's UI thread to create a browser.\n");
            // The task may still run and use `handle`, so it can't be freed here.
            // One leaked handle is the lesser evil. The live count stays raised for
            // the same reason -- the browser may yet appear -- which costs shutdown
            // the couple of seconds it is prepared to wait.
            return nullptr;
        }

        if (!state->created) {
            LOG_CEF("CefBrowserHost::CreateBrowserSync failed.\n");
            // The task has run and no browser came of it, so OnBeforeClose will
            // never fire for this one: undo the count here instead.
            g_n_live_browsers--;
            handle->client->set_on_gone(nullptr, nullptr);
            handle->client = nullptr;
            delete handle;
            return nullptr;
        }
    }

    return handle;
}

void wvcef_browser_destroy(struct wvcef_browser *browser) {
    if (browser == nullptr) {
        return;
    }

    // CEF keeps its own reference to the client until the browser is completely
    // torn down, so this may not be the last one -- which is fine. The client stops
    // calling out in OnBeforeClose, well before this runs.
    browser->client = nullptr;
    delete browser;
}

int wvcef_browser_get_id(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return 0;
    }
    return b->GetIdentifier();
}

void wvcef_browser_close(struct wvcef_browser *browser) {
    if (browser == nullptr || browser->client == nullptr || browser->close_requested) {
        return;
    }

    browser->close_requested = true;

    CefRefPtr<CefBrowser> b = browser->client->browser();
    if (b != nullptr) {
        b->GetHost()->CloseBrowser(true);
    }
}

void wvcef_browser_load_url(struct wvcef_browser *browser, const char *url) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr || url == nullptr) {
        return;
    }
    b->GetMainFrame()->LoadURL(CefString(url));
}

void wvcef_browser_reload(struct wvcef_browser *browser, bool ignore_cache) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }
    if (ignore_cache) {
        b->ReloadIgnoreCache();
    } else {
        b->Reload();
    }
}

void wvcef_browser_go_back(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr && b->CanGoBack()) {
        b->GoBack();
    }
}

void wvcef_browser_go_forward(struct wvcef_browser *browser) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr && b->CanGoForward()) {
        b->GoForward();
    }
}

void wvcef_browser_execute_javascript(struct wvcef_browser *browser, const char *code) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr || code == nullptr) {
        return;
    }
    CefRefPtr<CefFrame> frame = b->GetMainFrame();
    frame->ExecuteJavaScript(CefString(code), frame->GetURL(), 0);
}

void wvcef_browser_resize(struct wvcef_browser *browser, int width, int height, double device_pixel_ratio) {
    if (browser == nullptr || browser->client == nullptr) {
        return;
    }

    browser->client->set_size(width > 0 ? width : 1, height > 0 ? height : 1, device_pixel_ratio > 0.0 ? device_pixel_ratio : 1.0);

    CefRefPtr<CefBrowser> b = browser->client->browser();
    if (b != nullptr) {
        b->GetHost()->NotifyScreenInfoChanged();
        b->GetHost()->WasResized();
    }
}

void wvcef_browser_set_focus(struct wvcef_browser *browser, bool focused) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b != nullptr) {
        b->GetHost()->SetFocus(focused);
    }
}

void wvcef_browser_send_mouse_move(struct wvcef_browser *browser, int x, int y, bool dragging) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = dragging ? EVENTFLAG_LEFT_MOUSE_BUTTON : EVENTFLAG_NONE;

    b->GetHost()->SendMouseMoveEvent(event, false);
}

void wvcef_browser_send_mouse_click(struct wvcef_browser *browser, int x, int y, bool is_up) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = EVENTFLAG_LEFT_MOUSE_BUTTON;

    if (!is_up) {
        // Move there before pressing. A mouse always arrives via hover events, so
        // the page already knows where the cursor is -- but a touchscreen has no
        // hover, so the press would otherwise be the first the page hears of that
        // position, and anything that resolves its target from the last move sees
        // the press somewhere else entirely.
        b->GetHost()->SendMouseMoveEvent(event, false);
    }

    b->GetHost()->SendMouseClickEvent(event, MBT_LEFT, is_up, 1);
}

void wvcef_browser_send_mouse_wheel(struct wvcef_browser *browser, int x, int y, int delta_x, int delta_y) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr) {
        return;
    }

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = EVENTFLAG_NONE;

    // Flutter's scroll deltas grow downwards and are much finer grained than a
    // mouse wheel click. Same conversion the webview_cef package applies on its
    // non-Apple platforms, so scrolling feels identical.
    b->GetHost()->SendMouseWheelEvent(event, delta_x * 10, -delta_y * 10);
}

namespace {

/// CEF's "no range" sentinel, the same one cefclient uses for IME input.
CefRange invalid_range() {
    return CefRange(UINT32_MAX, UINT32_MAX);
}

}  // namespace

void wvcef_browser_ime_commit_text(struct wvcef_browser *browser, const char *text) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr || text == nullptr) {
        return;
    }

    b->GetHost()->ImeCommitText(CefString(text), invalid_range(), 0);
}

void wvcef_browser_ime_set_composition(struct wvcef_browser *browser, const char *text) {
    CefRefPtr<CefBrowser> b = browser_of(browser);
    if (b == nullptr || text == nullptr) {
        return;
    }

    const CefString composition(text);
    const std::vector<CefCompositionUnderline> underlines;
    const uint32_t cursor = static_cast<uint32_t>(composition.length());

    b->GetHost()->ImeSetComposition(composition, underlines, invalid_range(), CefRange(cursor, cursor));
}
