#pragma once

#include "engine/ScriptBridge.hpp"

#include <QHash>
#include <QString>
#include <QStringList>

// Opaque WebKit/JSC types, declared here so this header stays free of WebKit
// includes and the extensions layer can name it without pulling them in.
struct _WebKitUserContentManager;
using WebKitUserContentManager = _WebKitUserContentManager;
struct _WebKitScriptMessageReply;
using WebKitScriptMessageReply = _WebKitScriptMessageReply;
struct _WebKitWebView;
using WebKitWebView = _WebKitWebView;
// Deliberately not named JSCValue: that typedef belongs to WebKit's headers,
// which this header must not include. The .cpp casts it back.
struct JscValue;

namespace iridium::engine {
class WebView;
}

namespace iridium::engine::webkit {

// WebKit implementation of ScriptBridge.
//
// Injected scripts call window.webkit.messageHandlers.iridium.postMessage(),
// which lands here with-reply so the JS side gets a promise. Payloads are
// plain JSON text in both directions; no WebKit or JSC type crosses the boundary.
class WebKitScriptBridge final : public ScriptBridge {
public:
    explicit WebKitScriptBridge(WebView& view);
    ~WebKitScriptBridge() override;

    // False when the channel could not be registered, which leaves the shim
    // unable to call anything. Callers should check this before injecting.
    bool registered() const { return m_registered; }

    bool setHandler(const std::string& channel, ScriptBridge::RequestHandler handler) override;
    void setClosedHandler(ScriptBridge::ClosedHandler handler) override;
    void sendEvent(const std::string& channel, const std::string& jsonPayload) override;

private:
    void onMessage(WebKitScriptMessageReply* reply, JscValue* message);
    void onEvent(JscValue* message);

    WebView* m_view { nullptr };
    WebKitUserContentManager* m_manager { nullptr };
    bool m_registered { false };
    QHash<QString, ScriptBridge::RequestHandler> m_handlers;
    ScriptBridge::ClosedHandler m_closedHandler;
};

} // namespace iridium::engine::webkit