#pragma once

#include "engine/WebView.hpp"

#include <QHash>
#include <QImage>
#include <QString>
#include <QWidget>

#include <functional>
#include <string>

#include <cstdint>
#include <functional>
#include <memory>

struct wpe_view_backend_exportable_fdo;
struct wpe_fdo_shm_exported_buffer;
struct wpe_fdo_egl_exported_image;
struct _GObject;
struct _GAsyncResult;
struct _WebKitDownload;
using WebKitDownload = _WebKitDownload;
struct _WebKitNetworkSession;
using WebKitNetworkSession = _WebKitNetworkSession;
struct _WebKitUserScript;
using WebKitUserScript = _WebKitUserScript;
struct _WebKitWebView;
using WebKitWebView = _WebKitWebView;

namespace iridium::engine::webkit {

class EglPresenter;
class PreferredColorScheme;

// Engine adapter owns the WebKit view and its temporary FDO SHM presentation
// backend. No WPE types escape this implementation boundary.
class WebKitView final : public QWidget, public WebView {
public:
    WebKitView();
    ~WebKitView() override;

    void load(const std::string& uri) override;
    void goBack() override;
    void goForward() override;
    void reload() override;
    bool canGoBack() const override;
    bool canGoForward() const override;
    QWidget* widget() override { return this; }
    void* nativeWebView() const override { return m_webView; }
    void setTitleChangedHandler(std::function<void(const std::string&)> handler) override
    {
        m_titleChanged = std::move(handler);
    }
    void setUriChangedHandler(std::function<void(const std::string&)> handler) override
    {
        m_uriChanged = std::move(handler);
    }
    void setFaviconUrlChangedHandler(std::function<void(const std::string&)> handler) override
    {
        m_faviconUrlChanged = std::move(handler);
    }
    void setDownloadStateHandler(DownloadStateHandler handler) override
    {
        m_downloadStateChanged = std::move(handler);
    }
    void cancelDownload(std::uint64_t id) override;
    void evaluateJavaScript(const std::string& script, JavaScriptResultHandler handler) override;

    // Overrides the emulated prefers-color-scheme reported to pages.
    void setPreferredColorScheme(bool dark);
    bool preferredColorSchemeIsDark() const;

    // Installs `source` as a document-start script in every frame, replacing
    // any script previously set here. Used by the extensions layer to inject
    // content scripts; empty clears it.
    // Installs `source` as a document-start script in every frame, replacing
    // any script previously set here. Used by the extensions layer to inject
    // content scripts; empty clears it.
    void setContentScriptSource(const std::string& source) override;
    void clearContentScript() override;
    void setForcedColorScheme(std::optional<bool> dark) override;
    std::optional<bool> forcedColorScheme() const { return m_forcedColorScheme; }
    std::string currentUrl() const override;

    // Overrides where downloads are saved. Set once at startup by the
    // application so the preference can live in the settings layer without the
    // engine depending on it. An empty provider restores the XDG default.
    static void setDownloadDirectoryProvider(std::function<QString()> provider);

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    static void faviconUrlReady(_GObject* object, _GAsyncResult* result, void* data);
    static void copySelectionReady(_GObject* object, _GAsyncResult* result, void* data);
    static void exportShmBuffer(void* data, struct wpe_fdo_shm_exported_buffer* buffer);
    static void exportEglImage(void* data, void* image);
    static void exportFdoEglImage(void* data, struct wpe_fdo_egl_exported_image* image);
    void presentBuffer(struct wpe_fdo_shm_exported_buffer* buffer);
    void presentFrame(QImage frame, struct wpe_fdo_shm_exported_buffer* buffer);
    void presentFdoEglImage(struct wpe_fdo_egl_exported_image* image);

    struct DownloadRecord {
        WebKitDownload* download { nullptr };
        DownloadState state;
    };

    void downloadStarted(WebKitDownload* download);
    DownloadRecord* findDownload(WebKitDownload* download);
    void notifyDownload(const DownloadState& state);
    void releaseDownload(DownloadRecord* record);

    struct wpe_view_backend_exportable_fdo* m_backend { nullptr };
    WebKitWebView* m_webView { nullptr };
    WebKitNetworkSession* m_networkSession { nullptr };
    struct PendingFrame {
        struct wpe_fdo_shm_exported_buffer* buffer { nullptr };
        QImage image;
    };
    void releasePendingFrame();
    std::unique_ptr<PendingFrame> m_pendingFrame;
    QImage m_lastFrame;
    std::unique_ptr<EglPresenter> m_eglPresenter;
    QString m_contentScriptSource;
    // Last URI reported by the engine, used by the extensions layer to match
    // content scripts against the page.
    QString m_lastUri;
    // Set by the settings UI; nullopt means follow the system hint.
    std::optional<bool> m_forcedColorScheme;
    // Owned reference to the user script installed from m_contentScriptSource.
    // The manager that holds it dies with the view, so it is released first in
    // the destructor, like the color scheme script.
    WebKitUserScript* m_contentScript { nullptr };
    std::function<void(const std::string&)> m_titleChanged;
    std::function<void(const std::string&)> m_uriChanged;
    std::function<void(const std::string&)> m_faviconUrlChanged;
    DownloadStateHandler m_downloadStateChanged;
    QHash<std::uint64_t, DownloadRecord> m_downloads;
    std::unique_ptr<PreferredColorScheme> m_preferredColorScheme;
};

} // namespace iridium::engine::webkit
