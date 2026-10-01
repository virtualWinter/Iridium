#pragma once

#include "extensions/generated/ApiCatalog.hpp"

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <functional>
#include <optional>

namespace iridium::extensions {

class ExtensionRegistry;

// Where an API call came from. The specification restricts several namespaces
// to particular contexts, and a context that should not see an API must not be
// able to reach it by naming it, so the distinction is made here rather than
// discovered per call site.
enum class ExtensionContext {
    Background,     // MV2 background page, MV3 service worker
    ExtensionPage,  // popup, options, sidebar, any extension URL
    ContentScript,
    DevTools,
    SandboxedPage,
};

generated::ContextFlag contextFlag(ExtensionContext context);
const char* contextName(ExtensionContext context);

// A registered method: the catalog's description of it plus the behaviour.
class ApiMethod {
public:
    using Handler = std::function<QJsonValue(const QString& extensionId,
                                             const QJsonObject& arguments)>;

    ApiMethod() = default;
    ApiMethod(generated::MemberKind kind, QString requiredPermission,
        generated::ContextFlag contexts, int minManifestVersion,
        int maxManifestVersion, const generated::ParameterSpec* parameters,
        int parameterCount)
        : m_kind(kind)
        , m_requiredPermission(std::move(requiredPermission))
        , m_contextMask(contexts)
        , m_minManifestVersion(minManifestVersion)
        , m_maxManifestVersion(maxManifestVersion)
        , m_parameters(parameters)
        , m_parameterCount(parameterCount)
    {
    }

    QString qualifiedName() const { return m_qualifiedName; }
    void setQualifiedName(QString name) { m_qualifiedName = std::move(name); }

    QString namespaceName() const { return m_namespace; }
    void setNamespaceName(QString name) { m_namespace = std::move(name); }

    generated::MemberKind kind() const { return m_kind; }
    QString requiredPermission() const { return m_requiredPermission; }
    generated::ContextFlag contexts() const { return m_contextMask; }
    int minManifestVersion() const { return m_minManifestVersion; }
    int maxManifestVersion() const { return m_maxManifestVersion; }
    const generated::ParameterSpec* parameters() const { return m_parameters; }
    int parameterCount() const { return m_parameterCount; }

    void setHandler(Handler handler) { m_handler = std::move(handler); }
    bool hasHandler() const { return static_cast<bool>(m_handler); }
    const Handler& handler() const { return m_handler; }

    // Declared in the specification but not implemented here. Kept as a method
    // so the JavaScript namespace can still expose it and reject with a reason,
    // rather than leaving it undefined and letting an extension read silence as
    // success.
    void setUnavailable(QString reason) { m_unavailableReason = std::move(reason); }
    bool isUnavailable() const { return !m_unavailableReason.isEmpty(); }
    QString unavailableReason() const { return m_unavailableReason; }

private:
    QString m_qualifiedName;
    QString m_namespace;
    generated::MemberKind m_kind { generated::MemberKind::Method };
    QString m_requiredPermission;
    generated::ContextFlag m_contextMask { generated::ContextEvery };
    int m_minManifestVersion { 2 };
    int m_maxManifestVersion { 0 };
    const generated::ParameterSpec* m_parameters { nullptr };
    int m_parameterCount { 0 };
    Handler m_handler;
    QString m_unavailableReason;
};

// Every method the browser knows about, keyed by its qualified name
// ("tabs.query"), each carrying the catalog's metadata plus its handler.
//
// The registry owns the metadata; it does not own the behaviour. Handlers are
// registered by the API namespaces and read only what the catalog says, so a
// member cannot disagree with the specification about its own permission,
// contexts or arguments.
class ApiRegistry {
public:
    ApiRegistry();

    // Looks up a member in the generated catalog and registers it. Returns
    // nullptr when the specification has no such member, which is a programming
    // error rather than something to tolerate at runtime.
    ApiMethod* declare(const QString& qualifiedName);

    // Registers the specification's metadata and behaviour together. This is
    // the normal path: one call, and the method cannot exist without both.
    ApiMethod* registerMethod(const QString& qualifiedName, ApiMethod::Handler handler);

    // Declares a method this build cannot provide, so the JavaScript namespace
    // can reject with the reason instead of leaving it undefined.
    ApiMethod* registerUnavailable(const QString& qualifiedName, const QString& reason);

    ApiMethod* find(const QString& qualifiedName);
    const ApiMethod* find(const QString& qualifiedName) const;
    bool contains(const QString& qualifiedName) const
    {
        return m_methods.contains(qualifiedName);
    }

    const QHash<QString, ApiMethod>& methods() const { return m_methods; }

private:
    QHash<QString, ApiMethod> m_methods;
};

} // namespace iridium::extensions