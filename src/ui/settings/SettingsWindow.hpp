#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

class QListWidget;
class QListWidgetItem;
class QStackedWidget;

namespace iridium {
class MainWindow;
namespace extensions {
class ExtensionRegistry;
}
namespace history {
class HistoryPage;
class HistoryStore;
}

class ProfilesPage;

// The settings dialog: a category list on the left and one pane on the right.
//
// Built as a QDialog rather than a second top-level window so it stays modal to
// the browser window, gets the right parent for the taskbar, and is closed by
// the platform without the caller having to track it.
class SettingsWindow final : public QDialog {
    Q_OBJECT
public:
    SettingsWindow(MainWindow& window, extensions::ExtensionRegistry& registry,
        history::HistoryStore& history, QWidget* parent = nullptr);

    // Category ids, so a shortcut or test can select one by name. Functions
    // rather than constants because QLatin1String is not a constant expression.
    static QLatin1String generalCategory() { return QLatin1String("general"); }
    static QLatin1String historyCategory() { return QLatin1String("history"); }
    static QLatin1String extensionsCategory() { return QLatin1String("extensions"); }
    static QLatin1String profilesCategory() { return QLatin1String("profiles"); }
    static QLatin1String appearanceCategory() { return QLatin1String("appearance"); }

    void selectCategory(const QString& id);

protected:
    // Re-reads the pages that read live state, so the window shows what is true
    // now rather than what was true when it was built.
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void selectCategoryByIndex(int index);

    MainWindow& m_window;
    extensions::ExtensionRegistry& m_registry;
    history::HistoryStore& m_history;
    // Kept so showEvent can refresh them; both read state that changes while
    // this window is closed.
    history::HistoryPage* m_historyPage { nullptr };
    ProfilesPage* m_profilesPage { nullptr };
    QListWidget* m_categories { nullptr };
    QStackedWidget* m_panes { nullptr };
    QHash<QString, QWidget*> m_paneForCategory;
};

} // namespace iridium