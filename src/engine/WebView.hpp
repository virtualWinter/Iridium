#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class QWidget;

namespace iridium::engine {

struct DownloadState {
    std::uint64_t id { 0 };
    std::string fileName;
    std::string destination;
    double progress { 0.0 };
    bool finished { false };
    bool failed { false };
    std::string error;
};

class WebView {
public:
    using DownloadStateHandler = std::function<void(const DownloadState&)>;
    using JavaScriptResultHandler = std::function<void(const std::string&)>;

    virtual ~WebView() = default;
    virtual void load(const std::string& uri) = 0;
    virtual void goBack() = 0;
    virtual void goForward() = 0;
    virtual void reload() = 0;
    virtual bool canGoBack() const = 0;
    virtual bool canGoForward() const = 0;
    virtual QWidget* widget() = 0;
    // The underlying WebKitWebView, for engine-internal collaborators that must
    // reach WebKit APIs not exposed here (the script bridge). No WebKit type
    // crosses this boundary in the signature.
    virtual void* nativeWebView() const = 0;
    virtual void setTitleChangedHandler(std::function<void(const std::string&)> handler) = 0;
    virtual void setUriChangedHandler(std::function<void(const std::string&)> handler) = 0;
    virtual void setFaviconUrlChangedHandler(std::function<void(const std::string&)> handler) = 0;
    virtual void setDownloadStateHandler(DownloadStateHandler handler) = 0;
    virtual void cancelDownload(std::uint64_t id) = 0;
    // Evaluates a script in the page and reports the result as a string.
    virtual void evaluateJavaScript(const std::string& script, JavaScriptResultHandler handler) = 0;
    // The URL currently loaded, used to match content scripts against a page.
    virtual std::string currentUrl() const = 0;
    // Overrides the emulated prefers-color-scheme reported to pages. nullopt
    // restores following the system hint.
    virtual void setForcedColorScheme(std::optional<bool> dark) = 0;
    // Injects `source` as a document-start script in every frame, replacing any
    // previously injected script. Empty clears it. Used by the extensions layer.
    virtual void setContentScriptSource(const std::string& source) = 0;
    virtual void clearContentScript() = 0;
};

} // namespace iridium::engine
