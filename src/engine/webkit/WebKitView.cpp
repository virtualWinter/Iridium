#define QT_NO_KEYWORDS
#include "engine/webkit/WebKitView.hpp"

#include "engine/webkit/EglPresenter.hpp"
#include "engine/webkit/PreferredColorScheme.hpp"

#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/wpe.h>
#include <wayland-server-core.h>

#include <QMetaObject>
#include <QMouseEvent>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QKeySequence>
#include <QPointer>
#include <QStandardPaths>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>

#ifndef IRIDIUM_VERSION
#define IRIDIUM_VERSION "0.0.1"
#endif

namespace iridium::engine::webkit {

namespace {

void destroyFdoBackend(gpointer backend)
{
    wpe_view_backend_exportable_fdo_destroy(
        static_cast<wpe_view_backend_exportable_fdo*>(backend));
}

uint32_t inputModifiers(Qt::KeyboardModifiers modifiers)
{
    uint32_t result = 0;
    if (modifiers.testFlag(Qt::ControlModifier)) result |= wpe_input_keyboard_modifier_control;
    if (modifiers.testFlag(Qt::ShiftModifier)) result |= wpe_input_keyboard_modifier_shift;
    if (modifiers.testFlag(Qt::AltModifier)) result |= wpe_input_keyboard_modifier_alt;
    if (modifiers.testFlag(Qt::MetaModifier)) result |= wpe_input_keyboard_modifier_meta;
    return result;
}

uint32_t pointerModifiers(Qt::MouseButtons buttons)
{
    uint32_t result = 0;
    if (buttons.testFlag(Qt::LeftButton)) result |= wpe_input_pointer_modifier_button1;
    if (buttons.testFlag(Qt::MiddleButton)) result |= wpe_input_pointer_modifier_button2;
    if (buttons.testFlag(Qt::RightButton)) result |= wpe_input_pointer_modifier_button3;
    if (buttons.testFlag(Qt::BackButton)) result |= wpe_input_pointer_modifier_button4;
    if (buttons.testFlag(Qt::ForwardButton)) result |= wpe_input_pointer_modifier_button5;
    return result;
}

// WPE's legacy libwpe input events encode pointer buttons as a 1-based index
// (left = 1, right = 2, middle = 3, back = 4, forward = 5) rather than Linux
// input-event codes. WebKit's WebEventFactory::createWebMouseEvent() relies on
// that encoding; BTN_* values match no button and clicks are silently dropped.
uint32_t wpeButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::RightButton: return 2;
    case Qt::MiddleButton: return 3;
    case Qt::BackButton: return 4;
    case Qt::ForwardButton: return 5;
    default: return 0;
    }
}

// Declared before downloadDirectory() below uses it, defined below that
// function. Same anonymous namespace, so no linkage is needed.
std::function<QString()>& downloadOverrideStorage();

QString downloadDirectory()
{
    // Set from the settings pane so a user preference overrides the XDG
    // default. The engine must not depend on the UI layer, so the preference
    // arrives as a callback rather than an import.
    const std::function<QString()>& override = downloadOverrideStorage();
    if (override) {
        const QString configured = override();
        // An empty answer means "no preference set", not "save nowhere", so fall
        // through to the XDG default.
        if (!configured.isEmpty())
            return configured;
    }
    QString directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (directory.isEmpty())
        directory = QDir::homePath();
    return directory;
}

QString uniqueFileName(const QString& directory, const QString& suggested)
{
    QString name = suggested;
    if (name.isEmpty())
        name = QStringLiteral("download");
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    name.replace(QLatin1Char('\\'), QLatin1Char('_'));

    const QFileInfo info(name);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    QString candidate = name;
    for (int index = 1; QFileInfo::exists(directory + QLatin1Char('/') + candidate); ++index) {
        candidate = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(index)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix);
    }
    return candidate;
}

std::atomic<std::uint64_t> gNextDownloadId { 1 };

// Set by the application so the download preference can live in the settings
// layer without the engine depending on it.
std::function<QString()> g_downloadDirectoryOverride;

std::function<QString()>& downloadOverrideStorage()
{
    return g_downloadDirectoryOverride;
}

} // namespace

WebKitView::WebKitView()
{
    setMinimumSize(320, 240);
    if (!wpe_loader_init("libWPEBackend-fdo-1.0.so"))
        throw std::runtime_error("Could not load the installed WPE FDO backend");

    // Prefer GPU presentation: initializing the FDO renderer for EGL gives the
    // web process a hardware EGL display (GPU rasterization), and the exported
    // EGL images are read back into the Qt widget. Fall back to the software
    // SHM path when EGL is unavailable or explicitly disabled.
    if (!qEnvironmentVariableIsSet("IRIDIUM_DISABLE_GPU_PRESENTATION"))
        m_eglPresenter = EglPresenter::create();
    if (m_eglPresenter && !wpe_fdo_initialize_for_egl_display(m_eglPresenter->display()))
        m_eglPresenter.reset();

    if (!m_eglPresenter) {
        // The SHM-only renderer cannot provide an EGL display to the web
        // process; when Mesa takes the hardware path, EGL display creation
        // fails and WebKit aborts the web process (PlatformDisplayLibWPE).
        // Force software GL until the EGL path is used. An explicit
        // LIBGL_ALWAYS_SOFTWARE setting always wins.
        if (!qEnvironmentVariableIsSet("LIBGL_ALWAYS_SOFTWARE"))
            qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
        if (!wpe_fdo_initialize_shm())
            throw std::runtime_error("Could not initialize WPE FDO SHM renderer");
    }

    if (m_eglPresenter) {
        static const wpe_view_backend_exportable_fdo_egl_client client = [] {
            wpe_view_backend_exportable_fdo_egl_client result {};
            result.export_egl_image = &WebKitView::exportEglImage;
            result.export_fdo_egl_image = &WebKitView::exportFdoEglImage;
            result.export_shm_buffer = &WebKitView::exportShmBuffer;
            return result;
        }();
        m_backend = wpe_view_backend_exportable_fdo_egl_create(&client, this, width(), height());
        std::fprintf(stderr, "iridium: EGL presentation (hardware rasterization)\n");
    } else {
        static const wpe_view_backend_exportable_fdo_client client = [] {
            wpe_view_backend_exportable_fdo_client result {};
            result.export_shm_buffer = &WebKitView::exportShmBuffer;
            return result;
        }();
        m_backend = wpe_view_backend_exportable_fdo_create(&client, this, width(), height());
        std::fprintf(stderr, "iridium: software SHM presentation\n");
    }
    if (!m_backend)
        throw std::runtime_error("Could not create WPE FDO exportable backend");

    auto* webViewBackend = webkit_web_view_backend_new(
        wpe_view_backend_exportable_fdo_get_view_backend(m_backend),
        &destroyFdoBackend, m_backend);
    if (!webViewBackend) {
        wpe_view_backend_exportable_fdo_destroy(m_backend);
        m_backend = nullptr;
        throw std::runtime_error("Could not create WebKit view backend");
    }

    m_webView = webkit_web_view_new(webViewBackend);
    if (!m_webView) {
        wpe_view_backend_exportable_fdo_destroy(m_backend);
        m_backend = nullptr;
        throw std::runtime_error("Could not create WebKit view");
    }
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    // Identify as Iridium only. This is intentionally not Mozilla/WebKit
    // shaped; sites that sniff for those tokens will treat the browser as
    // unknown. The GLib port's site-specific quirks override even a custom UA
    // on hosts such as DuckDuckGo and Google Accounts/Docs. Disable that policy
    // before any navigation so HTTP headers, frames and workers keep our UA.
    auto* settings = webkit_web_view_get_settings(m_webView);
    webkit_settings_set_enable_site_specific_quirks(settings, FALSE);
    const QString userAgent = QStringLiteral("Iridium/%1").arg(QStringLiteral(IRIDIUM_VERSION));
    webkit_settings_set_user_agent(settings, userAgent.toUtf8().constData());
    m_preferredColorScheme = std::make_unique<PreferredColorScheme>(m_webView);

    // Downloads are announced by the network session; each view tracks only
    // the downloads it started.
    m_networkSession = webkit_web_view_get_network_session(m_webView);
    if (m_networkSession) {
        g_signal_connect(m_networkSession, "download-started",
            G_CALLBACK(+[](WebKitNetworkSession*, WebKitDownload* download, gpointer data) {
                static_cast<WebKitView*>(data)->downloadStarted(download);
            }), this);
    }
    g_signal_connect(m_webView, "notify::title", G_CALLBACK(+[](WebKitWebView* view, GParamSpec*, gpointer data) {
        auto* self = static_cast<WebKitView*>(data);
        if (self->m_titleChanged) {
            const char* title = webkit_web_view_get_title(view);
            self->m_titleChanged(title ? title : "New Tab");
        }
    }), this);
    g_signal_connect(m_webView, "notify::uri", G_CALLBACK(+[](WebKitWebView* view, GParamSpec*, gpointer data) {
        auto* self = static_cast<WebKitView*>(data);
        const char* uri = webkit_web_view_get_uri(view);
        self->m_lastUri = QString::fromUtf8(uri ? uri : "");
        if (self->m_uriChanged)
            self->m_uriChanged(self->m_lastUri.toStdString());
    }), this);
    g_signal_connect(m_webView, "load-changed", G_CALLBACK(+[](WebKitWebView* view, WebKitLoadEvent event, gpointer data) {
        if (event != WEBKIT_LOAD_FINISHED)
            return;
        auto* self = static_cast<WebKitView*>(data);
        static constexpr char script[] =
            "(()=>{const i=document.querySelector('link[rel~=icon]');"
            "return i ? i.href : new URL('/favicon.ico', location.href).href})()";
        webkit_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr, nullptr,
            &WebKitView::faviconUrlReady, new QPointer<WebKitView>(self));
    }), this);

    // Qt owns the event loop; pump WebKit's GLib sources without introducing a
    // second application loop. Keep each iteration non-blocking. The interval
    // is tunable for benchmarking (IRIDIUM_GLIB_PUMP_MS).
    int pumpInterval = 5;
    if (qEnvironmentVariableIsSet("IRIDIUM_GLIB_PUMP_MS"))
        pumpInterval = qBound(1, qEnvironmentVariableIntValue("IRIDIUM_GLIB_PUMP_MS"), 100);
    auto* glibPump = new QTimer(this);
    glibPump->setInterval(pumpInterval);
    QObject::connect(glibPump, &QTimer::timeout, this, [] {
        while (g_main_context_iteration(nullptr, FALSE)) { }
    });
    glibPump->start();
}

WebKitView::~WebKitView()
{
    // Downloads and color scheme controllers reference the web view; tear them
    // down before releasing it.
    releasePendingFrame();
    if (m_networkSession) {
        g_signal_handlers_disconnect_by_data(m_networkSession, this);
        m_networkSession = nullptr;
    }
    for (auto& record : m_downloads) {
        if (!record.download)
            continue;
        g_signal_handlers_disconnect_by_data(record.download, this);
        webkit_download_cancel(record.download);
        g_object_unref(record.download);
    }
    m_downloads.clear();
    m_preferredColorScheme.reset();
    if (m_contentScript) {
        // Released before m_webView: the manager holding it dies with the view,
        // and this matches how PreferredColorScheme releases its own script.
        webkit_user_script_unref(m_contentScript);
        m_contentScript = nullptr;
    }
    if (m_webView) {
        // The view's own handlers hold `this`. WebKit can keep the view alive
        // past this destructor (an in-flight load or script evaluation holds a
        // reference), so drop them before releasing our reference.
        g_signal_handlers_disconnect_by_data(m_webView, this);
        g_object_unref(m_webView);
        m_webView = nullptr;
    }
    // WebKitWebViewBackend takes ownership of the WPE backend and invokes
    // destroyFdoBackend when its boxed backend is released. Do not destroy the
    // FDO wrapper a second time here.
    m_backend = nullptr;
    m_eglPresenter.reset();
}

void WebKitView::setPreferredColorScheme(bool dark)
{
    if (m_preferredColorScheme)
        m_preferredColorScheme->setDark(dark);
}

bool WebKitView::preferredColorSchemeIsDark() const
{
    return m_preferredColorScheme && m_preferredColorScheme->isDark();
}

void WebKitView::setDownloadDirectoryProvider(std::function<QString()> provider)
{
    g_downloadDirectoryOverride = std::move(provider);
}

std::string WebKitView::currentUrl() const
{
    // Tracked from notify::uri rather than read back from the engine, so a
    // pooled view that is not attached to a live page reports its last URL.
    return m_lastUri.toStdString();
}

void WebKitView::setForcedColorScheme(std::optional<bool> dark)
{
    m_forcedColorScheme = dark;
    // The controller follows the system while no override is set, so dropping
    // the override must hand control back rather than freezing the last value.
    if (dark)
        m_preferredColorScheme->setDark(*dark);
}

void WebKitView::clearContentScript()
{
    setContentScriptSource({});
}

void WebKitView::setContentScriptSource(const std::string& source)
{
    const QString script = QString::fromStdString(source);
    m_contentScriptSource = script;

    auto* manager = m_webView
        ? webkit_web_view_get_user_content_manager(m_webView) : nullptr;
    if (!manager)
        return;

    if (m_contentScript) {
        webkit_user_content_manager_remove_script(manager, m_contentScript);
        webkit_user_script_unref(m_contentScript);
        m_contentScript = nullptr;
    }
    if (script.isEmpty())
        return;

    const QByteArray utf8 = script.toUtf8();
    m_contentScript = webkit_user_script_new(utf8.constData(),
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        nullptr, nullptr);
    if (m_contentScript)
        webkit_user_content_manager_add_script(manager, m_contentScript);
}

void WebKitView::cancelDownload(std::uint64_t id)
{
    auto found = m_downloads.find(id);
    if (found == m_downloads.end() || !found->download)
        return;
    webkit_download_cancel(found->download);
}

void WebKitView::evaluateJavaScript(const std::string& script, JavaScriptResultHandler handler)
{
    if (!m_webView || !handler)
        return;

    auto* callback = new JavaScriptResultHandler(std::move(handler));
    webkit_web_view_evaluate_javascript(m_webView, script.c_str(), -1, nullptr, nullptr, nullptr,
        +[](GObject* object, GAsyncResult* result, gpointer data) {
            std::unique_ptr<JavaScriptResultHandler> callback(
                static_cast<JavaScriptResultHandler*>(data));
            GError* error = nullptr;
            JSCValue* value = webkit_web_view_evaluate_javascript_finish(
                WEBKIT_WEB_VIEW(object), result, &error);
            std::string text;
            if (value) {
                gchar* string = jsc_value_to_string(value);
                text = string ? string : "";
                g_free(string);
                g_object_unref(value);
            } else if (error && error->message) {
                text = error->message;
            }
            g_clear_error(&error);
            if (*callback)
                (*callback)(text);
        }, callback);
}

void WebKitView::downloadStarted(WebKitDownload* download)
{
    if (!download || webkit_download_get_web_view(download) != m_webView)
        return;

    DownloadRecord record;
    record.download = WEBKIT_DOWNLOAD(g_object_ref(download));
    record.state.id = gNextDownloadId.fetch_add(1);
    m_downloads.insert(record.state.id, record);

    g_signal_connect(download, "decide-destination",
        G_CALLBACK(+[](WebKitDownload* download, const gchar* suggestedFilename, gpointer data) -> gboolean {
            auto* self = static_cast<WebKitView*>(data);
            auto* record = self->findDownload(download);
            if (!record)
                return FALSE;

            const QString directory = downloadDirectory();
            QDir().mkpath(directory);
            const QString name = uniqueFileName(directory,
                QString::fromUtf8(suggestedFilename ? suggestedFilename : "download"));
            const QString path = directory + QLatin1Char('/') + name;
            webkit_download_set_destination(download, path.toUtf8().constData());

            record->state.fileName = name.toStdString();
            record->state.destination = path.toStdString();
            self->notifyDownload(record->state);
            return TRUE;
        }), this);

    g_signal_connect(download, "received-data",
        G_CALLBACK(+[](WebKitDownload* download, guint64, gpointer data) {
            auto* self = static_cast<WebKitView*>(data);
            auto* record = self->findDownload(download);
            if (!record)
                return;
            record->state.progress = webkit_download_get_estimated_progress(download);
            self->notifyDownload(record->state);
        }), this);

    g_signal_connect(download, "failed",
        G_CALLBACK(+[](WebKitDownload* download, GError* error, gpointer data) {
            auto* self = static_cast<WebKitView*>(data);
            auto* record = self->findDownload(download);
            if (!record)
                return;
            record->state.failed = true;
            record->state.error = error && error->message ? error->message : "download failed";
            self->notifyDownload(record->state);
            self->releaseDownload(record);
        }), this);

    g_signal_connect(download, "finished",
        G_CALLBACK(+[](WebKitDownload* download, gpointer data) {
            auto* self = static_cast<WebKitView*>(data);
            auto* record = self->findDownload(download);
            if (!record)
                return;
            record->state.progress = 1.0;
            record->state.finished = true;
            self->notifyDownload(record->state);
            self->releaseDownload(record);
        }), this);
}

WebKitView::DownloadRecord* WebKitView::findDownload(WebKitDownload* download)
{
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (it->download == download)
            return &it.value();
    }
    return nullptr;
}

void WebKitView::notifyDownload(const DownloadState& state)
{
    if (m_downloadStateChanged)
        m_downloadStateChanged(state);
}

void WebKitView::releaseDownload(DownloadRecord* record)
{
    if (!record || !record->download)
        return;
    WebKitDownload* download = record->download;
    g_signal_handlers_disconnect_by_data(download, this);
    g_object_unref(download);
    for (auto it = m_downloads.begin(); it != m_downloads.end(); ++it) {
        if (it->download == download) {
            m_downloads.erase(it);
            return;
        }
    }
}

void WebKitView::load(const std::string& uri)
{
    webkit_web_view_load_uri(m_webView, uri.c_str());
}

void WebKitView::goBack() { webkit_web_view_go_back(m_webView); }
void WebKitView::goForward() { webkit_web_view_go_forward(m_webView); }
void WebKitView::reload() { webkit_web_view_reload(m_webView); }
bool WebKitView::canGoBack() const { return webkit_web_view_can_go_back(m_webView); }
bool WebKitView::canGoForward() const { return webkit_web_view_can_go_forward(m_webView); }

void WebKitView::faviconUrlReady(GObject* object, GAsyncResult* result, gpointer data)
{
    std::unique_ptr<QPointer<WebKitView>> self(static_cast<QPointer<WebKitView>*>(data));
    GError* error = nullptr;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    if (value && self && !self->isNull() && jsc_value_is_string(value)) {
        gchar* url = jsc_value_to_string(value);
        if (url && *url && (*self)->m_faviconUrlChanged)
            (*self)->m_faviconUrlChanged(url);
        g_free(url);
    }
    if (value)
        g_object_unref(value);
    g_clear_error(&error);
}

void WebKitView::copySelectionReady(GObject* object, GAsyncResult* result, gpointer data)
{
    std::unique_ptr<QPointer<WebKitView>> self(static_cast<QPointer<WebKitView>*>(data));
    GError* error = nullptr;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(object), result, &error);
    if (value && self && !self->isNull() && jsc_value_is_string(value)) {
        gchar* text = jsc_value_to_string(value);
        if (text && *text)
            QApplication::clipboard()->setText(QString::fromUtf8(text));
        g_free(text);
    }
    if (value)
        g_object_unref(value);
    g_clear_error(&error);
}

void WebKitView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_backend) {
        wpe_input_pointer_event input {};
        input.type = wpe_input_pointer_event_type_motion;
        input.time = event->timestamp();
        input.x = static_cast<int>(event->position().x());
        input.y = static_cast<int>(event->position().y());
        input.modifiers = inputModifiers(event->modifiers()) | pointerModifiers(event->buttons());
        wpe_view_backend_dispatch_pointer_event(
            wpe_view_backend_exportable_fdo_get_view_backend(m_backend), &input);
    }
    event->accept();
}

void WebKitView::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);
    if (m_backend && wpeButton(event->button())) {
        auto* backend = wpe_view_backend_exportable_fdo_get_view_backend(m_backend);
        wpe_input_pointer_event motion {};
        motion.type = wpe_input_pointer_event_type_motion;
        motion.time = event->timestamp();
        motion.x = static_cast<int>(event->position().x());
        motion.y = static_cast<int>(event->position().y());
        motion.modifiers = inputModifiers(event->modifiers())
            | pointerModifiers(event->buttons() | event->button());
        wpe_view_backend_dispatch_pointer_event(backend, &motion);

        wpe_input_pointer_event input {};
        input.type = wpe_input_pointer_event_type_button;
        input.time = event->timestamp();
        input.x = static_cast<int>(event->position().x());
        input.y = static_cast<int>(event->position().y());
        input.button = wpeButton(event->button());
        input.state = 1;
        input.modifiers = inputModifiers(event->modifiers())
            | pointerModifiers(event->buttons() | event->button());
        wpe_view_backend_dispatch_pointer_event(backend, &input);
    }
    event->accept();
}

void WebKitView::mouseDoubleClickEvent(QMouseEvent* event)
{
    // Qt reports the second click as a distinct event type. Treat it as the
    // corresponding pointer-button press for the WPE backend.
    mousePressEvent(event);
}

void WebKitView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_backend && wpeButton(event->button())) {
        auto* backend = wpe_view_backend_exportable_fdo_get_view_backend(m_backend);
        wpe_input_pointer_event motion {};
        motion.type = wpe_input_pointer_event_type_motion;
        motion.time = event->timestamp();
        motion.x = static_cast<int>(event->position().x());
        motion.y = static_cast<int>(event->position().y());
        const Qt::MouseButtons buttonsAfterRelease = event->buttons() & ~Qt::MouseButtons(event->button());
        motion.modifiers = inputModifiers(event->modifiers()) | pointerModifiers(buttonsAfterRelease);
        wpe_view_backend_dispatch_pointer_event(backend, &motion);

        wpe_input_pointer_event input {};
        input.type = wpe_input_pointer_event_type_button;
        input.time = event->timestamp();
        input.x = static_cast<int>(event->position().x());
        input.y = static_cast<int>(event->position().y());
        input.button = wpeButton(event->button());
        input.state = 0;
        input.modifiers = inputModifiers(event->modifiers()) | pointerModifiers(buttonsAfterRelease);
        wpe_view_backend_dispatch_pointer_event(backend, &input);
    }
    event->accept();
}

void WebKitView::wheelEvent(QWheelEvent* event)
{
    if (!m_backend) {
        event->ignore();
        return;
    }
    const QPointF delta = event->pixelDelta().isNull()
        ? QPointF(event->angleDelta()) * (40.0 / 120.0)
        : QPointF(event->pixelDelta());
    wpe_input_axis_2d_event input {};
    input.base.type = static_cast<wpe_input_axis_event_type>(
        wpe_input_axis_event_type_motion_smooth | wpe_input_axis_event_type_mask_2d);
    input.base.time = event->timestamp();
    input.base.x = static_cast<int>(event->position().x());
    input.base.y = static_cast<int>(event->position().y());
    input.base.modifiers = inputModifiers(event->modifiers());
    input.x_axis = delta.x();
    input.y_axis = delta.y();
    wpe_view_backend_dispatch_axis_event(
        wpe_view_backend_exportable_fdo_get_view_backend(m_backend), &input.base);
    event->accept();
}

void WebKitView::keyPressEvent(QKeyEvent* event)
{
    if (m_backend) {
        const uint32_t hardwareCode = event->nativeScanCode();
        wpe_input_keyboard_event input {};
        input.time = event->timestamp();
        input.hardware_key_code = hardwareCode;
        input.key_code = wpe_input_xkb_context_get_key_code(
            wpe_input_xkb_context_get_default(), hardwareCode, true);
        input.pressed = true;
        input.modifiers = inputModifiers(event->modifiers());
        wpe_view_backend_dispatch_keyboard_event(
            wpe_view_backend_exportable_fdo_get_view_backend(m_backend), &input);
        if (event->matches(QKeySequence::Copy)) {
            static constexpr char script[] =
                "(()=>{const e=document.activeElement;"
                "if(e&&typeof e.selectionStart==='number')"
                "return e.value.slice(e.selectionStart,e.selectionEnd);"
                "return String(window.getSelection()||'')})()";
            webkit_web_view_evaluate_javascript(m_webView, script, -1, nullptr, nullptr, nullptr,
                &WebKitView::copySelectionReady, new QPointer<WebKitView>(this));
        }
    }
    event->accept();
}

void WebKitView::keyReleaseEvent(QKeyEvent* event)
{
    if (m_backend) {
        const uint32_t hardwareCode = event->nativeScanCode();
        wpe_input_keyboard_event input {};
        input.time = event->timestamp();
        input.hardware_key_code = hardwareCode;
        input.key_code = wpe_input_xkb_context_get_key_code(
            wpe_input_xkb_context_get_default(), hardwareCode, false);
        input.pressed = false;
        input.modifiers = inputModifiers(event->modifiers());
        wpe_view_backend_dispatch_keyboard_event(
            wpe_view_backend_exportable_fdo_get_view_backend(m_backend), &input);
    }
    event->accept();
}

void WebKitView::exportShmBuffer(void* data, wpe_fdo_shm_exported_buffer* buffer)
{
    static_cast<WebKitView*>(data)->presentBuffer(buffer);
}

void WebKitView::exportEglImage(void* data, void* image)
{
    auto* self = static_cast<WebKitView*>(data);
    if (self->m_eglPresenter) {
        QImage frame = self->m_eglPresenter->readImage(static_cast<EGLImageKHR>(image),
            self->width(), self->height());
        if (!frame.isNull())
            self->presentFrame(std::move(frame), nullptr);
    }
    if (self->m_backend)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_image(
            self->m_backend, static_cast<EGLImageKHR>(image));
}

void WebKitView::exportFdoEglImage(void* data, wpe_fdo_egl_exported_image* image)
{
    static_cast<WebKitView*>(data)->presentFdoEglImage(image);
}

void WebKitView::presentFdoEglImage(wpe_fdo_egl_exported_image* image)
{
    if (!m_eglPresenter || !m_backend)
        return;

    const int width = static_cast<int>(wpe_fdo_egl_exported_image_get_width(image));
    const int height = static_cast<int>(wpe_fdo_egl_exported_image_get_height(image));
    QImage frame = m_eglPresenter->readImage(
        wpe_fdo_egl_exported_image_get_egl_image(image), width, height);
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(m_backend, image);
    if (!frame.isNull())
        presentFrame(std::move(frame), nullptr);
}

void WebKitView::presentBuffer(wpe_fdo_shm_exported_buffer* buffer)
{
    auto* shmBuffer = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    if (!shmBuffer) {
        if (m_backend) {
            if (m_eglPresenter)
                wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(m_backend, buffer);
            else
                wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(m_backend, buffer);
        }
        return;
    }

    const int width = wl_shm_buffer_get_width(shmBuffer);
    const int height = wl_shm_buffer_get_height(shmBuffer);
    const int stride = wl_shm_buffer_get_stride(shmBuffer);
    wl_shm_buffer_begin_access(shmBuffer);
    const auto* pixels = static_cast<const uchar*>(wl_shm_buffer_get_data(shmBuffer));
    QImage image(pixels, width, height, stride, QImage::Format_ARGB32);
    // Copy while access is held and hand the buffer straight back: holding it
    // until paint throttles WebKit's frame production.
    QImage frame = image.copy();
    wl_shm_buffer_end_access(shmBuffer);
    if (m_backend) {
        if (m_eglPresenter)
            wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(m_backend, buffer);
        else
            wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(m_backend, buffer);
    }

    presentFrame(std::move(frame), nullptr);
}

void WebKitView::presentFrame(QImage frame, wpe_fdo_shm_exported_buffer* buffer)
{
    if (frame.isNull() && !buffer)
        return;

    // Latest frame wins: release a frame that has not been painted yet.
    if (m_pendingFrame)
        releasePendingFrame();

    m_pendingFrame = std::make_unique<PendingFrame>();
    m_pendingFrame->buffer = buffer;
    m_pendingFrame->image = std::move(frame);
    QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
}

void WebKitView::releasePendingFrame()
{
    if (!m_pendingFrame)
        return;
    if (m_backend) {
        if (m_pendingFrame->buffer) {
            if (m_eglPresenter)
                wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(m_backend, m_pendingFrame->buffer);
            else
                wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(m_backend, m_pendingFrame->buffer);
        }
        wpe_view_backend_exportable_fdo_dispatch_frame_complete(m_backend);
    }
    m_pendingFrame.reset();
}

void WebKitView::paintEvent(QPaintEvent*)
{
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(rect(), 8, 8);
        painter.setClipPath(clip);
        painter.fillRect(rect(), palette().color(QPalette::Window));

        const QImage* frame = nullptr;
        if (m_pendingFrame && !m_pendingFrame->image.isNull())
            frame = &m_pendingFrame->image;
        else if (!m_lastFrame.isNull())
            frame = &m_lastFrame;
        if (frame)
            painter.drawImage(rect(), *frame);
    }

    // Keep the painted frame for later repaints, then hand the buffer back and
    // acknowledge the frame.
    if (m_pendingFrame) {
        if (!m_pendingFrame->image.isNull())
            m_lastFrame = m_pendingFrame->image;
        releasePendingFrame();
    }
}

void WebKitView::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!m_backend)
        return;
    auto* backend = wpe_view_backend_exportable_fdo_get_view_backend(m_backend);
    wpe_view_backend_add_activity_state(backend,
        wpe_view_activity_state_visible | wpe_view_activity_state_in_window);
}

void WebKitView::hideEvent(QHideEvent* event)
{
    // Do not hold a frame back while the widget is not painted.
    releasePendingFrame();
    if (m_backend) {
        auto* backend = wpe_view_backend_exportable_fdo_get_view_backend(m_backend);
        wpe_view_backend_remove_activity_state(backend,
            wpe_view_activity_state_visible | wpe_view_activity_state_in_window);
    }
    QWidget::hideEvent(event);
}

void WebKitView::focusInEvent(QFocusEvent* event)
{
    QWidget::focusInEvent(event);
    if (m_backend)
        wpe_view_backend_add_activity_state(
            wpe_view_backend_exportable_fdo_get_view_backend(m_backend),
            wpe_view_activity_state_focused);
}

void WebKitView::focusOutEvent(QFocusEvent* event)
{
    if (m_backend)
        wpe_view_backend_remove_activity_state(
            wpe_view_backend_exportable_fdo_get_view_backend(m_backend),
            wpe_view_activity_state_focused);
    QWidget::focusOutEvent(event);
}

void WebKitView::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_backend)
        wpe_view_backend_dispatch_set_size(
            wpe_view_backend_exportable_fdo_get_view_backend(m_backend),
            static_cast<uint32_t>(event->size().width()),
            static_cast<uint32_t>(event->size().height()));
}

} // namespace iridium::engine::webkit
