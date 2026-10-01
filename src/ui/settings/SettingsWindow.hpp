#pragma once

#include <QDialog>
#include <QHash>
#include <QString>

#include <memory>

class QLayout;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QToolButton;
class WindowDecoration;

namespace iridium {
class MainWindow;
namespace ui {
class FramelessChrome;
}
namespace extensions {
class ExtensionRegistry;
}
namespace history {
class HistoryPage;
class HistoryStore;
}
class GeneralPage;
class AppearancePage;

class ProfilesPage;

// The settings dialog: a category rail on the left, one pane on the right.
// Window controls stay in a separate top-right header.
//
// Built as a QDialog rather than a second top-level window so it stays modal to
// the browser window, gets the right parent for the taskbar, and is closed by
// the platform without the caller having to track it.
//
// It looks like the browser window rather than like a platform dialog: same
// rounded translucent shell, same palette-driven stylesheet, same system window
// controls, same drag region and edge resizing. All of that comes from
// FramelessChrome and sharedStyleSheet rather than being restated here.
class SettingsWindow final : public QDialog {
    Q_OBJECT
public:
    SettingsWindow(MainWindow& window, extensions::ExtensionRegistry& registry,
        history::HistoryStore& history, QWidget* parent = nullptr);
    ~SettingsWindow() override;

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
    // Keeps the window controls' artwork in step with activation and
    // maximisation, as the browser window does with its own.
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void selectCategoryByIndex(int index);
    // The window controls, added to `layout` in the top-right header. A frameless
    // window has no title bar, so without these there is no way to dismiss it
    // except Escape.
    void addWindowControls(QLayout* layout);
    void updateDecorationState();

    MainWindow& m_window;
    extensions::ExtensionRegistry& m_registry;
    history::HistoryStore& m_history;
    // The shell: translucency, drag region, edge resizing, shared stylesheet.
    std::unique_ptr<ui::FramelessChrome> m_chrome;
    // Kept so showEvent can refresh them; both read state that changes while
    // this window is closed.
    history::HistoryPage* m_historyPage { nullptr };
    ProfilesPage* m_profilesPage { nullptr };
    GeneralPage* m_generalPage { nullptr };
    AppearancePage* m_appearancePage { nullptr };
    QListWidget* m_categories { nullptr };
    QStackedWidget* m_panes { nullptr };
    QHash<QString, QWidget*> m_paneForCategory;
    // The system window controls, so their state can follow activation and
    // maximisation the way the browser window's do.
    QList<WindowDecoration*> m_decorations;
};

} // namespace iridium
