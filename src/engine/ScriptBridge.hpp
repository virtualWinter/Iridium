#pragma once

#include <functional>
#include <memory>
#include <string>

namespace iridium::engine {

// JSON-shaped request/response transport between injected page scripts and the
// UI process. Abstract so tests can substitute a recording implementation.
// The extensions layer needs an async bridge (browser.tabs.query and friends
// return promises). WebKit's script message handler with reply provides exactly
// that, but it speaks JSCValue rather than JSON, so this interface hides the
// engine boundary: callers exchange QJsonObject-equivalent text and never see a
// WebKit or JSC type.
class ScriptBridge {
public:
    // A request from a page. `payload` is the JSON text of the message.
    using RequestHandler = std::function<std::string(const std::string& payload)>;
    // Notified when the page goes away, so per-view state can be dropped.
    using ClosedHandler = std::function<void()>;

    virtual ~ScriptBridge() = default;

    // Installs `handler` for messages sent to `channel`. Returns false if the
    // channel could not be registered (already taken, for instance).
    virtual bool setHandler(const std::string& channel, RequestHandler handler) = 0;
    virtual void setClosedHandler(ClosedHandler handler) = 0;
    // Pushes an event to the page. Silently ignored when the page is gone.
    // Named sendEvent rather than emit: Qt defines `emit` as an empty macro, so
    // a member with that name would not compile wherever Qt headers are seen.
    virtual void sendEvent(const std::string& channel, const std::string& jsonPayload) = 0;
};

} // namespace iridium::engine