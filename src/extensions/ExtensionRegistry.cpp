#include "extensions/ExtensionRegistry.hpp"

#include "extensions/ExtensionPaths.hpp"
#include "extensions/JsSource.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace iridium::extensions {

namespace {

// A stable id for an unpacked extension.
//
// This must be the directory name: ExtensionInstaller installs and uninstalls
// by directory name, and the settings UI uses this id to enable and remove. An
// id derived from the manifest's display name would make an extension
// un-enableable and un-removable after a successful install. A manifest-declared
// id is still honoured, because that is how an author pins identity across
// renames, but only when the manifest actually declares one.
QString deriveId(const QString& directory, const Manifest& manifest)
{
    const QJsonObject raw = manifest.raw();
    const QString declared = raw.value(QStringLiteral("browser_specific_settings"))
        .toObject().value(QStringLiteral("gecko")).toObject()
        .value(QStringLiteral("id")).toString();
    if (!declared.isEmpty())
        return declared;

    return QFileInfo(directory).fileName();
}

} // namespace

Extension::Extension(QString id, QString directory, Manifest manifest)
    : m_id(std::move(id))
    , m_directory(std::move(directory))
    , m_manifest(std::move(manifest))
{
}

QString Extension::displayName() const
{
    return m_manifest.name().isEmpty() ? m_id : m_manifest.name();
}

QString Extension::readResource(const QString& relativePath) const
{
    const auto cached = m_resourceCache.constFind(relativePath);
    if (cached != m_resourceCache.constEnd())
        return cached.value();

    // Keep reads inside the extension directory: a manifest is untrusted input
    // and "../../etc/passwd" must not resolve. Cleaning the path first collapses
    // any "..", so the prefix test below cannot be bypassed by a traversal that
    // only appears to stay inside.
    const QString base = QDir(m_directory).absolutePath();
    if (QDir::isAbsolutePath(relativePath))
        return {};

    const QString resolved = QDir::cleanPath(QDir(base).absoluteFilePath(relativePath));
    const QString cleanedBase = QDir::cleanPath(base);
    if (resolved != cleanedBase && !resolved.startsWith(cleanedBase + QLatin1Char('/')))
        return {};

    QFile file(resolved);
    if (!file.open(QIODevice::ReadOnly)) {
        m_resourceCache.insert(relativePath, QString());
        return {};
    }
    const QString contents = QString::fromUtf8(file.readAll());
    m_resourceCache.insert(relativePath, contents);
    return contents;
}

bool Extension::isUserInstalled(const QString& userDirectory) const
{
    return QDir::cleanPath(m_directory).startsWith(QDir::cleanPath(userDirectory));
}

ExtensionRegistry::ExtensionRegistry(QString profileDirectory, QObject* parent)
    : QObject(parent)
    , m_profileDirectory(std::move(profileDirectory))
{
    loadEnabledState();
}

ExtensionRegistry::~ExtensionRegistry() = default;

void ExtensionRegistry::loadFrom(const QString& directory)
{
    if (!m_searchPaths.contains(directory))
        m_searchPaths.append(directory);
    scanInto(directory);
}

void ExtensionRegistry::reload()
{
    // Enabled state is kept: re-reading the manifests must not silently
    // disable everything the user turned on.
    m_extensions.clear();
    m_loadErrors.clear();
    m_storageCache.clear();
    for (const QString& path : m_searchPaths)
        scanInto(path);
    emit changed();
}

void ExtensionRegistry::scanInto(const QString& directory)
{
    QDir root(directory);
    if (!root.exists())
        return;

    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& entry : entries) {
        const QString path = root.absoluteFilePath(entry);
        QString error;
        auto manifest = Manifest::load(path, &error);
        if (!manifest) {
            m_loadErrors.insert(entry, error);
            continue;
        }

        const QString id = deriveId(path, *manifest);
        // A later directory winning is arbitrary but harmless; keep the first
        // so a duplicate id cannot silently shadow an installed extension.
        if (find(id))
            continue;

        m_extensions.push_back(std::make_unique<Extension>(id, path, std::move(*manifest)));
    }
}

QStringList ExtensionRegistry::sortedIds() const
{
    QStringList result;
    result.reserve(static_cast<int>(m_extensions.size()));

    // Explicit ordering first, but only for extensions that still exist: an
    // uninstalled extension must not leave a hole that hides the rest.
    for (const QString& id : m_order) {
        if (find(id) && !result.contains(id))
            result.append(id);
    }
    // Anything the user has not placed keeps discovery order, after the rest.
    for (const auto& extension : m_extensions) {
        if (!result.contains(extension->id()))
            result.append(extension->id());
    }
    return result;
}

bool ExtensionRegistry::moveUp(const QString& id)
{
    const QStringList current = sortedIds();
    const int index = current.indexOf(id);
    if (index <= 0)
        return false;

    QStringList next = current;
    next.swapItemsAt(index, index - 1);
    m_order = next;
    savePreferences();
    emit changed();
    return true;
}

bool ExtensionRegistry::moveDown(const QString& id)
{
    const QStringList current = sortedIds();
    const int index = current.indexOf(id);
    if (index < 0 || index >= current.size() - 1)
        return false;

    QStringList next = current;
    next.swapItemsAt(index, index + 1);
    m_order = next;
    savePreferences();
    emit changed();
    return true;
}

QStringList ExtensionRegistry::grantedPermissions(const QString& id) const
{
    const auto found = m_grantedPermissions.constFind(id);
    if (found == m_grantedPermissions.constEnd())
        return {};
    // Sorted so the UI shows a stable list rather than set iteration order.
    // Sorted so the UI shows a stable list rather than set iteration order.
    QStringList result;
    result.reserve(found->size());
    for (const QString& permission : found.value())
        result.append(permission);
    result.sort();
    return result;
}

bool ExtensionRegistry::setPermissionGranted(const QString& id, const QString& permission,
    bool granted)
{
    Extension* extension = find(id);
    if (!extension)
        return false;

    // Only optional permissions can be toggled. Granting a required one is a
    // no-op by definition, and granting something undeclared would let the UI
    // hand an extension access its manifest never asked for.
    if (granted
        && !extension->manifest().optionalPermissions().contains(permission)) {
        return false;
    }

    auto& set = m_grantedPermissions[id];
    // Named to avoid shadowing the changed() signal.
    const bool stateChanged = granted ? !set.contains(permission) : set.contains(permission);
    if (granted)
        set.insert(permission);
    else
        set.remove(permission);
    if (set.isEmpty())
        m_grantedPermissions.remove(id);

    if (stateChanged) {
        savePreferences();
        emit changed();
    }
    return true;
}

QStringList ExtensionRegistry::effectivePermissions(const QString& id) const
{
    Extension* extension = find(id);
    if (!extension)
        return {};

    QStringList result = extension->manifest().permissions();
    result.append(grantedPermissions(id));
    result.sort();
    return result;
}

bool ExtensionRegistry::hasPermission(const QString& id, const QString& permission) const
{
    if (permission.isEmpty())
        return true;
    Extension* extension = find(id);
    if (!extension)
        return false;
    return extension->manifest().permissions().contains(permission)
        || grantedPermissions(id).contains(permission);
}

Extension* ExtensionRegistry::find(const QString& id) const
{
    for (const auto& extension : m_extensions) {
        if (extension->id() == id)
            return extension.get();
    }
    return nullptr;
}

bool ExtensionRegistry::enableWithDependencies(const QString& id, QSet<QString>* visited)
{
    if (visited->contains(id))
        return true;
    visited->insert(id);

    Extension* extension = find(id);
    if (!extension)
        return false;

    for (const QString& dependency : extension->manifest().dependencies()) {
        if (!enableWithDependencies(dependency, visited))
            return false;
    }

    m_enabled.insert(id);
    return true;
}

bool ExtensionRegistry::setEnabled(const QString& id, bool enabled)
{
    if (enabled) {
        QSet<QString> visited;
        if (!enableWithDependencies(id, &visited)) {
            // Roll back so a failed enable does not leave dependencies on.
            for (const QString& dependency : visited)
                m_enabled.remove(dependency);
            return false;
        }
    } else {
        m_enabled.remove(id);
    }

    saveEnabledState();
    emit changed();
    return true;
}

void ExtensionRegistry::setAllEnabled(bool enabled)
{
    if (!enabled) {
        m_enabled.clear();
    } else {
        for (const auto& extension : m_extensions)
            m_enabled.insert(extension->id());
    }
    saveEnabledState();
    emit changed();
}

std::vector<ExtensionRegistry::InjectedScript> ExtensionRegistry::scriptsForUrl(
    const QString& url) const
{
    std::vector<InjectedScript> result;

    // In user-defined order, then discovery order for anything not placed. The
    // order decides which extension's content script sees a page first.
    for (const QString& id : sortedIds()) {
        Extension* extension = find(id);
        if (!extension || !m_enabled.contains(extension->id()))
            continue;

        for (const ContentScript& script : extension->manifest().contentScripts()) {
            bool included = script.matches.isEmpty();
            for (const QString& pattern : script.matches) {
                MatchPattern parsed;
                if (MatchPattern::parse(pattern, &parsed, nullptr) && parsed.matches(url)) {
                    included = true;
                    break;
                }
            }
            if (!included)
                continue;

            bool excluded = false;
            for (const QString& pattern : script.excludeMatches) {
                MatchPattern parsed;
                if (MatchPattern::parse(pattern, &parsed, nullptr) && parsed.matches(url)) {
                    excluded = true;
                    break;
                }
            }
            if (excluded)
                continue;

            QVector<QString> sources;
            sources.reserve(script.js.size() + script.css.size());
            for (const QString& js : script.js) {
                const QString source = extension->readResource(js);
                if (!source.isEmpty())
                    sources.append(source);
            }
            for (const QString& css : script.css) {
                const QString source = extension->readResource(css);
                if (!source.isEmpty())
                    sources.append(QStringLiteral("/* iridium-css:%1 */\n%2")
                        .arg(css, source));
            }
            if (sources.isEmpty())
                continue;

            // The manifest travels with the bundle so the shim can answer
            // getManifest() synchronously without a round trip.
            InjectedScript entry;
            entry.extensionId = extension->id();
            entry.sources = std::move(sources);
            const QString manifest = QString::fromUtf8(
                QJsonDocument(extension->manifest().raw()).toJson(QJsonDocument::Compact));
            entry.identity = QStringLiteral("window.__iridiumBeginExtension(%1, %2);\n")
                .arg(jsStringLiteral(entry.extensionId),
                    manifest.isEmpty() ? QStringLiteral("{}") : manifest);
            result.push_back(std::move(entry));
        }
    }

    return result;
}

bool ExtensionRegistry::wantsDocumentStart(const ContentScript& script)
{
    return script.runAt == QLatin1String("document_start");
}

QString ExtensionRegistry::manifestJson(const QString& id) const
{
    Extension* extension = find(id);
    if (!extension)
        return QStringLiteral("{}");
    return QString::fromUtf8(
        QJsonDocument(extension->manifest().raw()).toJson(QJsonDocument::Compact));
}

QJsonObject ExtensionRegistry::loadStorage(const QString& id) const
{
    const auto cached = m_storageCache.constFind(id);
    if (cached != m_storageCache.constEnd())
        return cached.value();

    const QString path = ExtensionPaths::storageDirectory(id,
        QDir(m_profileDirectory).dirName()) + QStringLiteral("/local.json");
    QJsonObject storage;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (document.isObject())
            storage = document.object();
    }
    m_storageCache.insert(id, storage);
    return storage;
}

void ExtensionRegistry::saveStorage(const QString& id, const QJsonObject& storage)
{
    m_storageCache.insert(id, storage);

    const QString directory = ExtensionPaths::storageDirectory(id,
        QDir(m_profileDirectory).dirName());
    QDir().mkpath(directory);
    // QSaveFile so a crash mid-write cannot truncate stored data.
    QSaveFile file(directory + QStringLiteral("/local.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(storage).toJson(QJsonDocument::Indented));
    file.commit();
}

void ExtensionRegistry::invalidateStorageCache()
{
    m_storageCache.clear();
}

void ExtensionRegistry::loadEnabledState()
{
    QFile file(m_profileDirectory + QStringLiteral("/extensions.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();

    const QJsonArray enabled = root.value(QStringLiteral("enabled")).toArray();
    for (const QJsonValue& value : enabled) {
        if (value.isString())
            m_enabled.insert(value.toString());
    }

    const QJsonObject permissions =
        root.value(QStringLiteral("grantedPermissions")).toObject();
    for (auto it = permissions.begin(); it != permissions.end(); ++it) {
        QSet<QString> granted;
        for (const QJsonValue& value : it.value().toArray()) {
            if (value.isString())
                granted.insert(value.toString());
        }
        if (!granted.isEmpty())
            m_grantedPermissions.insert(it.key(), granted);
    }

    const QJsonArray order = root.value(QStringLiteral("order")).toArray();
    for (const QJsonValue& value : order) {
        if (value.isString())
            m_order.append(value.toString());
    }
}

void ExtensionRegistry::saveEnabledState() const
{
    savePreferences();
}

void ExtensionRegistry::savePreferences() const
{
    QDir().mkpath(m_profileDirectory);

    QJsonArray enabled;
    // Sorted so the file does not churn between writes that changed nothing.
    QStringList enabledIds(m_enabled.begin(), m_enabled.end());
    enabledIds.sort();
    for (const QString& id : enabledIds)
        enabled.append(id);

    QJsonObject permissions;
    for (auto it = m_grantedPermissions.constBegin();
         it != m_grantedPermissions.constEnd(); ++it) {
        if (it.value().isEmpty())
            continue;
        QStringList names(it.value().begin(), it.value().end());
        names.sort();
        permissions.insert(it.key(), QJsonArray::fromStringList(names));
    }

    QJsonArray order;
    for (const QString& id : m_order)
        order.append(id);

    QJsonObject root;
    root.insert(QStringLiteral("enabled"), enabled);
    root.insert(QStringLiteral("grantedPermissions"), permissions);
    root.insert(QStringLiteral("order"), order);

    // QSaveFile so an interrupted write cannot leave a truncated profile.
    QSaveFile file(m_profileDirectory + QStringLiteral("/extensions.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.commit();
}

} // namespace iridium::extensions
