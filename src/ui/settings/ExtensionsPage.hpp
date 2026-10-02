#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QToolButton;

namespace iridium {
namespace extensions {
class Extension;
class ExtensionRegistry;
}

// The extensions pane of the settings window: lists what is installed, lets the
// user enable or disable it, and installs or removes unpacked extensions.
//
// Row state is driven entirely from the registry, so toggling a switch here and
// toggling it from anywhere else stay in agreement.
class ExtensionsPage final : public QWidget {
    Q_OBJECT
public:
    explicit ExtensionsPage(extensions::ExtensionRegistry& registry, QWidget* parent = nullptr);

private:
    void buildUi();
    void rebuild();
    void showDetails();
    void updateDetails();
    // Rebuilds the optional-permission checkboxes for the given extension.
    void rebuildPermissionToggles(const extensions::Extension& extension);
    // Moves the selected extension by `delta` rows in the run order.
    void moveSelected(int delta);
    // Asks for an update manifest location and reports the comparison.
    void checkForUpdate();
    void onSelectionChanged();
    void installFromDirectory(const QString& directory);
    void installUnpacked();
    void removeSelected();
    void toggleSelected();
    void reportStatus(const QString& message, bool isError);

    extensions::ExtensionRegistry& m_registry;

    QLineEdit* m_filter { nullptr };
    QListWidget* m_list { nullptr };
    QWidget* m_details { nullptr };
    QLabel* m_detailsName { nullptr };
    QLabel* m_detailsVersion { nullptr };
    QLabel* m_detailsId { nullptr };
    QLabel* m_detailsPermissions { nullptr };
    QLabel* m_detailsHosts { nullptr };
    QLabel* m_detailsLocation { nullptr };
    QToolButton* m_toggle { nullptr };
    QToolButton* m_remove { nullptr };
    QToolButton* m_moveUp { nullptr };
    QToolButton* m_moveDown { nullptr };
    QToolButton* m_updateCheck { nullptr };
    // Update manifest location per extension, so the dialog remembers it.
    QHash<QString, QString> m_updateSources;
    // Holds one checkbox per optional permission of the selected extension.
    QLabel* m_optionalCaption { nullptr };
    QWidget* m_optionalPermissions { nullptr };
    QLabel* m_status { nullptr };
    // Shown when the filter matches nothing, or nothing is installed at all.
    QLabel* m_empty { nullptr };
    QStackedWidget* m_body { nullptr };

    QString m_selectedId;
};

} // namespace iridium
