#pragma once

#include <QString>
#include <QStringList>

namespace iridium::extensions {

// Where extensions live, following the XDG base directory spec.
//
// Unpacked extensions are directories containing manifest.json. System-wide
// directories are read-only; the user directory is where installations land.
struct ExtensionPaths final {
    // Directories scanned in order; earlier entries win on duplicate ids.
    static QStringList searchPaths();

    // XDG_DATA_HOME/iridium/extensions, created on demand when installing.
    static QString userDirectory();
    // XDG_DATA_DIRS entries that contain iridium/extensions.
    static QStringList systemDirectories();

    // Directory used for per-extension mutable state (storage.local, enabled
    // flags). XDG_DATA_HOME/iridium/profiles/<profile>.
    static QString profileDirectory(const QString& profile = QString());
    // Subdirectory of the profile for one extension's storage.local.
    static QString storageDirectory(const QString& extensionId,
        const QString& profile = QString());
};

} // namespace iridium::extensions