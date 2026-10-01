#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace iridium::extensions {

// A content script, MV2 style. MV3 shares the shape, so one struct covers both
// until the runtime needs to tell them apart.
struct ContentScript {
    QStringList matches;
    QStringList excludeMatches;
    QStringList js;
    QStringList css;
    // "document_start", "document_end" or "document_idle".
    QString runAt { QStringLiteral("document_idle") };
    bool allFrames { false };
};

// The background context. MV2 names scripts, MV3 names a service worker; this
// build hosts neither, but the manifest is parsed faithfully so callers can
// report what an extension asked for instead of silently dropping it.
struct Background {
    QStringList scripts;
    QString serviceWorker;
    bool persistent { false };
};

struct Action {
    bool present { false };
    QString defaultTitle;
    QString defaultPopup;
    QJsonObject icons;
};

// A parsed and validated manifest.json.
//
// WebKit ships a WebKitWebExtension type whose accessors describe exactly this
// data, but in the installed WPE 2.52 build its constructors return NULL for
// every input and leave GError unset, so the metadata is unreadable. Iridium
// therefore parses manifests itself; this is the only place that knows the
// manifest schema.
class Manifest final {
public:
    // Parses `path`/manifest.json. Returns nullopt and fills `error` on any
    // problem, so callers can surface a reason to the user.
    static std::optional<Manifest> load(const QString& directory, QString* error);
    static std::optional<Manifest> fromJson(const QByteArray& json, QString* error);

    int manifestVersion() const { return m_manifestVersion; }
    QString name() const { return m_name; }
    QString version() const { return m_version; }
    QString description() const { return m_description; }
    // Raw manifest, for fields not modelled above.
    const QJsonObject& raw() const { return m_raw; }

    // Declared API permissions ("storage", "tabs", ...), host patterns removed.
    const QStringList& permissions() const { return m_permissions; }
    // Permissions the extension asked for but which need not be granted. These
    // start denied and are enabled by the user.
    const QStringList& optionalPermissions() const { return m_optionalPermissions; }
    // Optional host patterns, from optional_host_permissions (MV3).
    const QStringList& optionalHostPermissions() const { return m_optionalHostPermissions; }
    // Declared host patterns, from host_permissions (MV3) or permissions (MV2).
    const QStringList& hostPermissions() const { return m_hostPermissions; }
    const std::vector<ContentScript>& contentScripts() const { return m_contentScripts; }
    const Background& background() const { return m_background; }
    const Action& action() const { return m_action; }
    QString optionsPage() const { return m_optionsPage; }
    // Extensions this one depends on, by id.
    const QStringList& dependencies() const { return m_dependencies; }

private:
    static std::optional<Manifest> parse(const QJsonObject& object, QString* error);

    QJsonObject m_raw;
    int m_manifestVersion { 0 };
    QString m_name;
    QString m_version;
    QString m_description;
    QStringList m_permissions;
    QStringList m_optionalPermissions;
    QStringList m_optionalHostPermissions;
    QStringList m_hostPermissions;
    std::vector<ContentScript> m_contentScripts;
    Background m_background;
    Action m_action;
    QString m_optionsPage;
    QStringList m_dependencies;
};

} // namespace iridium::extensions
