#define QT_NO_KEYWORDS
#include "engine/webkit/WebKitScriptBridge.hpp"

#include "engine/WebView.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <wpe/webkit.h>
#include <jsc/jsc.h>

namespace iridium::engine::webkit {

namespace {

// Serializes a page-side value to JSON text.
//
// jsc_value_to_string cannot be used: on an object it returns JSC's own display
// form, e.g. "({a:1})", which is not JSON and fails to parse. JSON.stringify is
// run in the page's own context instead.
QString jscToJson(JscValue* opaque)
{
    if (!opaque)
        return {};
    auto* value = reinterpret_cast<JSCValue*>(opaque);
    auto* context = jsc_value_get_context(value);
    if (!context)
        return {};

    // Reach JSON.stringify through the JSC* API only: WebKit ships no
    // JavaScriptCore C++ headers, so JSObjectCallFunctionAsFunction is not
    // available.
    JSCValue* jsonNamespace = jsc_context_evaluate(context, "JSON", -1);
    if (!jsonNamespace)
        return {};
    JSCValue* function = jsc_value_object_get_property(jsonNamespace, "stringify");
    g_object_unref(jsonNamespace);
    if (!function)
        return {};

    JSCValue* arguments[] = { value };
    JSCValue* result = jsc_value_function_callv(function, 1, arguments);
    g_object_unref(function);

    QString json;
    if (result) {
        if (jsc_value_is_string(result)) {
            GBytes* bytes = jsc_value_to_string_as_bytes(result);
            if (bytes) {
                gsize size = 0;
                const char* data = static_cast<const char*>(
                    g_bytes_get_data(bytes, &size));
                json = QString::fromUtf8(data, static_cast<int>(size));
                g_bytes_unref(bytes);
            }
        }
        g_object_unref(result);
    }
    return json;
}

// Turns JSON text back into a page-side value by evaluating it as an
// expression. The text is wrapped in parentheses so a leading "{" is parsed as
// an object literal rather than a block.
JscValue* jsonToJsc(JscValue* opaque, const QString& json)
{
    if (!opaque)
        return nullptr;
    const QString source = json.isEmpty() ? QStringLiteral("undefined")
        : QStringLiteral("(") + json + QLatin1Char(')');
    return reinterpret_cast<JscValue*>(jsc_context_evaluate(
        jsc_value_get_context(reinterpret_cast<JSCValue*>(opaque)),
        source.toUtf8().constData(), -1));
}

} // namespace

WebKitScriptBridge::WebKitScriptBridge(WebView& view)
    : m_view(&view)
{
    // nativeWebView() is an untyped engine escape hatch; WebKit's own headers
    // supply the real type here.
    auto* webView = static_cast<WebKitWebView*>(view.nativeWebView());
    // Hold a reference: the manager is owned by the view, and the bridge must be
    // able to disconnect cleanly even if it is released after the view dies.
    m_manager = WEBKIT_USER_CONTENT_MANAGER(g_object_ref(
        webkit_web_view_get_user_content_manager(webView)));

    // Note the name: this build spells it script-message-with-reply-received,
    // not the ...-with-reply::name form the header docs describe. Connecting to
    // the documented spelling fails at runtime with an "invalid signal"
    // critical, so the working name is used deliberately.
    // The value arrives first and the reply second, which is the opposite of the
    // plain script-message-received signal; getting this backwards crashes in
    // jsc_value_to_string, so the order is deliberate.
    g_signal_connect(m_manager, "script-message-with-reply-received::iridium",
        G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* message,
                       WebKitScriptMessageReply* reply, gpointer data) {
            static_cast<WebKitScriptBridge*>(data)
                ->onMessage(reply, reinterpret_cast<JscValue*>(message));
        }), this);
    g_signal_connect(m_manager, "script-message-received::iridium",
        G_CALLBACK(+[](WebKitUserContentManager*, JSCValue* message, gpointer data) {
            static_cast<WebKitScriptBridge*>(data)
                ->onEvent(reinterpret_cast<JscValue*>(message));
        }), this);

    m_registered = webkit_user_content_manager_register_script_message_handler_with_reply(
        m_manager, "iridium", nullptr);
}

WebKitScriptBridge::~WebKitScriptBridge()
{
    if (!m_manager)
        return;
    g_signal_handlers_disconnect_by_data(m_manager, this);
    if (m_registered)
        webkit_user_content_manager_unregister_script_message_handler(
            m_manager, "iridium", nullptr);
    g_object_unref(m_manager);
    m_manager = nullptr;
}

bool WebKitScriptBridge::setHandler(const std::string& channel, RequestHandler handler)
{
    if (!m_registered)
        return false;
    m_handlers.insert(QString::fromStdString(channel), std::move(handler));
    return true;
}

void WebKitScriptBridge::setClosedHandler(ClosedHandler handler)
{
    m_closedHandler = std::move(handler);
}

void WebKitScriptBridge::sendEvent(const std::string& channel, const std::string& jsonPayload)
{
    // Events travel as a dispatch call into the page's own emitter. Evaluated
    // rather than posted so a dead page fails harmlessly.
    QJsonArray arguments { QString::fromStdString(channel),
        QString::fromStdString(jsonPayload) };
    // evaluateJavaScript takes std::string; the script argument is passed by const
    // reference so the temporary must outlive the call.
    const QString script = QStringLiteral("window.__iridiumEmit && window.__iridiumEmit(")
        + QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))
        + QStringLiteral(")");
    m_view->evaluateJavaScript(script.toStdString(), nullptr);
}

void WebKitScriptBridge::onMessage(WebKitScriptMessageReply* reply, JscValue* message)
{
    QJsonParseError parseError {};
    const QJsonDocument request = QJsonDocument::fromJson(jscToJson(message).toUtf8(),
        &parseError);
    if (parseError.error != QJsonParseError::NoError || !request.isObject()) {
        webkit_script_message_reply_return_error_message(reply,
            "malformed bridge message");
        return;
    }

    const QJsonObject object = request.object();
    const QString method = object.value(QStringLiteral("method")).toString();
    if (method.isEmpty()) {
        webkit_script_message_reply_return_error_message(reply, "bridge message has no method");
        return;
    }

    const auto handler = m_handlers.constFind(method);
    if (handler == m_handlers.constEnd()) {
        webkit_script_message_reply_return_error_message(reply,
            ("browser." + method + " is not implemented in this build").toUtf8().constData());
        return;
    }

    const QJsonValue paramsValue = object.value(QStringLiteral("params"));
    const QByteArray params = (paramsValue.isArray()
            ? QJsonDocument(paramsValue.toArray())
            : QJsonDocument(paramsValue.isObject()
                ? paramsValue.toObject() : QJsonObject()))
        .toJson(QJsonDocument::Compact);

    QString replyJson;
    try {
        replyJson = QString::fromStdString((*handler)(std::string(params.constData())));
    } catch (const std::exception& error) {
        webkit_script_message_reply_return_error_message(reply, error.what());
        return;
    }

    auto* replyValue = jsonToJsc(message, replyJson);
    if (!replyValue) {
        webkit_script_message_reply_return_error_message(reply,
            "handler returned a value that is not valid JSON");
        return;
    }
    webkit_script_message_reply_return_value(reply, reinterpret_cast<JSCValue*>(replyValue));
    g_object_unref(reinterpret_cast<JSCValue*>(replyValue));
}

void WebKitScriptBridge::onEvent(JscValue* message)
{
    Q_UNUSED(message);
}

} // namespace iridium::engine::webkit