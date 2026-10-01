#pragma once

#include "extensions/Manifest.hpp"
#include "extensions/MatchPattern.hpp"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

namespace iridium::extensions {

// One installed extension: its identity, on-disk location, parsed manifest and
// the content scripts that should be injected into matching pages.
class Extension final {
public:
    Extension(QString id, QString directory, Manifest manifest);

    const QString& id() const { return m_id; }
    const QString& directory() const { return m_directory; }
    const Manifest& manifest() const { return m_manifest; }

    QString displayName() const;
    // Resolves and caches a script listed in the manifest, relative to the
    // extension directory. Returns empty when the file is missing.
    QString readResource(const QString& relativePath) const;

    // True when this extension lives in the writable user directory, which is
    // the only place removal is allowed.
    bool isUserInstalled(const QString& userDirectory) const;

private:
    QString m_id;
    QString m_directory;
    Manifest m_manifest;
    mutable QHash<QString, QString> m_resourceCache;
};

// Loads extensions from disk and tracks which are enabled.
//
// Enabled state is persisted per profile so it survives restarts, and
// enable/disable emits changed() so the UI and the engine can react.
class ExtensionRegistry final : public QObject {
    Q_OBJECT
public:
    explicit ExtensionRegistry(QString profileDirectory, QObject* parent = nullptr);
    ~ExtensionRegistry() override;

    // Scans `directory` for unpacked extensions, one per subdirectory with a
    // manifest.json. Unparseable ones are skipped and reported through
    // loadErrors() rather than aborting the scan.
    void loadFrom(const QString& directory);

    const std::vector<std::unique_ptr<Extension>>& extensions() const { return m_extensions; }
    // Ids that failed to load, with the reason.
    const QHash<QString, QString>& loadErrors() const { return m_loadErrors; }

    Extension* find(const QString& id) const;
    bool isEnabled(const QString& id) const { return m_enabled.contains(id); }

    // Optional permissions the user has granted. Empty by default: an extension
    // gets nothing beyond its required permissions until asked.
    QStringList grantedPermissions(const QString& id) const;
    // Grants or revokes an optional permission. Refuses one the manifest does
    // not declare as optional, so the UI cannot widen an extension's reach
    // beyond what it asked for.
    bool setPermissionGranted(const QString& id, const QString& permission, bool granted);
    // Everything the extension may use: its required permissions plus the
    // optional ones that have been granted.
    QStringList effectivePermissions(const QString& id) const;
    // True when `id` may call something gated on `permission`. Required
    // permissions always pass; an optional one only once granted.
    bool hasPermission(const QString& id, const QString& permission) const;

    // User-defined ordering, which decides the order content scripts are
    // injected in. Ids absent from the list keep their discovery order after the
    // ones present.
    QStringList order() const { return m_order; }
    // Ids in run order, with unplaced ones appended in discovery order. This is
    // what injection iterates, and what the settings UI reorders.
    QStringList sortedIds() const;
    bool moveUp(const QString& id);
    bool moveDown(const QString& id);

    // Enabling an extension also enables its dependencies first. Returns false
    // and leaves state unchanged if a dependency is missing.
    bool setEnabled(const QString& id, bool enabled);
    void setAllEnabled(bool enabled);

    // The content scripts to inject for `url`, honouring exclude_matches and
    // each script's matches. Order follows install order, then manifest order.
    struct InjectedScript {
        QString extensionId;
        // Identity declaration emitted before this extension's first script.
        QString identity;
        QVector<QString> sources;
    };
    std::vector<InjectedScript> scriptsForUrl(const QString& url) const;

    // Resolved from a content script's run_at, for the caller's injection point.
    static bool wantsDocumentStart(const ContentScript& script);

    // The manifest as JSON text, handed to injected scripts.
    QString manifestJson(const QString& id) const;

    // storage.local for one extension, cached in memory and written through to
    // the profile directory. The cache is what makes reads cheap; an empty
    // object means "nothing stored yet", not an error.
    QJsonObject loadStorage(const QString& id) const;
    void saveStorage(const QString& id, const QJsonObject& storage);
    void invalidateStorageCache();

// Re-scans every path added by loadFrom. Used by the settings UI after an
    // install or uninstall so the list reflects what is on disk.
    void reload();

    // The paths loadFrom has been called with, so reload can re-scan them.
    const QStringList& searchPaths() const { return m_searchPaths; }

signals:
    void changed();

private:
    void loadEnabledState();
    void saveEnabledState() const;
    // One file for enabled ids, granted optional permissions and ordering;
    // splitting them would let a write to one lose another's data.
    void savePreferences() const;
    bool enableWithDependencies(const QString& id, QSet<QString>* visited);
    void scanInto(const QString& directory);

    QString m_profileDirectory;
    QStringList m_searchPaths;
    std::vector<std::unique_ptr<Extension>> m_extensions;
    QHash<QString, QString> m_loadErrors;
    QSet<QString> m_enabled;
    // Optional permissions the user granted, per extension.
    QHash<QString, QSet<QString>> m_grantedPermissions;
    // Explicit user ordering of extension ids.
    QStringList m_order;
    mutable QHash<QString, QJsonObject> m_storageCache;
};

} // namespace iridium::extensions
