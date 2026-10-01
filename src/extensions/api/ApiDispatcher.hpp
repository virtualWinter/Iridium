#pragma once

#include "extensions/api/ApiRegistry.hpp"

#include <QJsonObject>
#include <QString>

namespace iridium::extensions {

class ExtensionRegistry;

// The one place an extension's JavaScript reaches the browser.
//
// Every call passes through here, in an order chosen so that a call which
// should never have been made is refused before it can do anything:
//
//   1. is the method known at all?
//   2. is it exposed in the calling context?
//   3. is the extension enabled?
//   4. does the manifest version allow this method?
//   5. does the extension hold the required permission?
//   6. do the arguments match the specification?
//   7. run the handler
//
// The extension id is a parameter rather than something read from the message.
// It is established by the context that owns the call — the injector for a
// content script, the host for a background context — and never from anything
// the calling script supplies, because the calling script is untrusted.
class ApiDispatcher {
public:
    // `registry` supplies the catalog metadata, `extensions` answers whether an
    // extension is enabled and holds a permission. Both are borrowed.
    ApiDispatcher(ApiRegistry& registry, ExtensionRegistry& extensions);

    struct Request {
        QString extensionId;
        QString method;                  // "tabs.query"
        QJsonObject arguments;
        ExtensionContext context { ExtensionContext::ContentScript };
        int manifestVersion { 2 };
    };

    // Runs the handler. The returned value is what the promise resolves with;
    // an error is reported by returning a single-key {"message": ...} object,
    // which the caller turns into a rejection. That shape is deliberate: the
    // native side cannot reject a promise from outside JavaScript.
    QJsonValue dispatch(const Request& request);

    // The validation step on its own, so a test can assert what is accepted and
    // what is refused without needing a handler behind it.
    static QString validateArguments(const ApiMethod& method, const QJsonObject& arguments);

    // Whether an extension may call a method at all, and why not when it may
    // not. Empty means it may.
    QString refusalReason(const Request& request) const;

private:
    ApiRegistry& m_registry;
    ExtensionRegistry& m_extensions;
};

} // namespace iridium::extensions