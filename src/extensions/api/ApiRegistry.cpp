#include "extensions/api/ApiRegistry.hpp"

#include <QStringList>

#include <string_view>

namespace iridium::extensions {

using generated::ContextFlag;

ContextFlag contextFlag(ExtensionContext context)
{
    switch (context) {
    case ExtensionContext::Background:
        return generated::ContextBackground;
    case ExtensionContext::ExtensionPage:
        return generated::ContextExtensionPage;
    case ExtensionContext::ContentScript:
        return generated::ContextContentScript;
    case ExtensionContext::DevTools:
        return generated::ContextDevTools;
    case ExtensionContext::SandboxedPage:
        return generated::ContextSandboxed;
    }
    return generated::ContextNone;
}

const char* contextName(ExtensionContext context)
{
    switch (context) {
    case ExtensionContext::Background:
        return "background";
    case ExtensionContext::ExtensionPage:
        return "extension page";
    case ExtensionContext::ContentScript:
        return "content script";
    case ExtensionContext::DevTools:
        return "devtools";
    case ExtensionContext::SandboxedPage:
        return "sandboxed page";
    }
    return "unknown";
}

namespace {

// Splits a qualified member name into its namespace and member.
//
// The split is at the *last* dot, because a namespace may itself contain one:
// "devtools.panels.create" is the member `create` of the namespace
// `devtools.panels`, not a member called "panels.create" of `devtools`.
bool splitQualifiedName(const QString& qualified, QString* namespaceName,
    QString* memberName)
{
    const int dot = qualified.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == qualified.size() - 1)
        return false;
    *namespaceName = qualified.left(dot);
    *memberName = qualified.mid(dot + 1);
    return true;
}

} // namespace

ApiRegistry::ApiRegistry() = default;

ApiMethod* ApiRegistry::declare(const QString& qualifiedName)
{
    if (m_methods.contains(qualifiedName))
        return &m_methods[qualifiedName];

    QString space;
    QString member;
    if (!splitQualifiedName(qualifiedName, &space, &member))
        return nullptr;

    // The catalog is keyed by std::string_view, so the lookups get one. The
    // temporaries live until the end of the statement, which is all the
    // catalog needs: it stores no references.
    const std::string spaceKey = space.toStdString();
    const std::string memberKey = member.toStdString();
    const generated::NamespaceSpec* namespaceSpec =
        generated::findNamespace(std::string_view(spaceKey));
    if (!namespaceSpec)
        return nullptr;
    const generated::MemberSpec* memberSpec =
        generated::findMember(std::string_view(spaceKey), std::string_view(memberKey));
    if (!memberSpec)
        return nullptr;

    // A sub-namespace inherits its parent's context restriction: the schemas
    // state allowedContexts on privacy and devtools, not on privacy.websites.
    ContextFlag contexts = namespaceSpec->contexts;
    int minVersion = namespaceSpec->minManifestVersion;
    int maxVersion = namespaceSpec->maxManifestVersion;
    const QStringList parts = space.split(QLatin1Char('.'));
    for (int i = 1; i < parts.size(); ++i) {
        const std::string parentKey = parts.mid(0, i).join(QLatin1Char('.')).toStdString();
        const generated::NamespaceSpec* parent =
            generated::findNamespace(std::string_view(parentKey));
        if (!parent)
            continue;
        contexts &= parent->contexts;
        if (parent->minManifestVersion > minVersion)
            minVersion = parent->minManifestVersion;
        if (parent->maxManifestVersion && (maxVersion == 0
                || parent->maxManifestVersion < maxVersion))
            maxVersion = parent->maxManifestVersion;
    }

    // A member may need a permission its namespace does not (tabs.hide needs
    // tabHide, tabs.captureTab needs <all_urls>), and may need one *instead* of
    // it, so the two are combined rather than one replacing the other.
    QStringList required;
    if (namespaceSpec->permission && *namespaceSpec->permission)
        required << QString::fromUtf8(namespaceSpec->permission);
    if (memberSpec->extraPermission && *memberSpec->extraPermission)
        required << QString::fromUtf8(memberSpec->extraPermission).split(QLatin1Char(','));

    ApiMethod method(memberSpec->kind,
        required.join(QLatin1Char('+')),
        contexts,
        minVersion,
        maxVersion,
        memberSpec->parameters,
        memberSpec->parameterCount);
    method.setQualifiedName(qualifiedName);
    method.setNamespaceName(space);
    return &m_methods.insert(qualifiedName, std::move(method)).value();
}

ApiMethod* ApiRegistry::registerMethod(const QString& qualifiedName, ApiMethod::Handler handler)
{
    ApiMethod* method = declare(qualifiedName);
    if (!method)
        return nullptr;
    method->setHandler(std::move(handler));
    return method;
}

ApiMethod* ApiRegistry::registerUnavailable(const QString& qualifiedName,
    const QString& reason)
{
    ApiMethod* method = declare(qualifiedName);
    if (!method)
        return nullptr;
    method->setUnavailable(reason);
    return method;
}

ApiMethod* ApiRegistry::find(const QString& qualifiedName)
{
    auto it = m_methods.find(qualifiedName);
    return it == m_methods.end() ? nullptr : &it.value();
}

const ApiMethod* ApiRegistry::find(const QString& qualifiedName) const
{
    auto it = m_methods.constFind(qualifiedName);
    return it == m_methods.constEnd() ? nullptr : &it.value();
}

} // namespace iridium::extensions