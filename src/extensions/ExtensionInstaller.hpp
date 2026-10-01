#pragma once

#include <QString>

namespace iridium::extensions {

// The outcome of an update check.
struct UpdateCheckResult {
    bool ok { false };
    bool updateAvailable { false };
    QString currentVersion;
    QString latestVersion;
    QString error;
};

// Version comparison for update checks: dotted components compared left to
// right. Numeric components compare as numbers; a non-numeric one compares as
// text, and only once the numeric parts are equal. A shorter version counts as
// having trailing zeros, so "1.2" equals "1.2.0". Returns <0, 0 or >0.
int compareVersions(const QString& left, const QString& right);

// Installs and removes unpacked extensions.
//
// Install copies into ExtensionPaths::userDirectory() (XDG_DATA_HOME), which
// is the only writable location; system directories are left alone.
class ExtensionInstaller final {
public:
    // Copies `sourceDirectory` into the user extension directory. Validates the
    // manifest first, then re-validates the copy. On any failure nothing is left
    // behind and `error` explains why.
    static bool install(const QString& sourceDirectory, QString* idOut, QString* error);

    // Removes the extension and its stored data from the user directory.
    // Refuses to touch system-installed extensions.
    static bool uninstall(const QString& id, QString* error);

    // Compares the installed `installedDirectory` against the JSON in
    // `updateManifestJson`. Pure, so it is testable without a network.
    static UpdateCheckResult compareAgainstUpdateManifest(
        const QString& installedDirectory, const QByteArray& updateManifestJson);

    // As above, but reads the manifest from `source`, which may be an http(s)
    // URL or a local path.
    static UpdateCheckResult checkForUpdate(const QString& installedDirectory,
        const QString& source);
};

} // namespace iridium::extensions