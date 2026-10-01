#pragma once

#include "browser/HistoryStore.hpp"

#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace iridium::history {

// The history pane: search, open, forget and clear.
//
// Reads straight from the store rather than keeping a copy, so an entry recorded
// by a tab while the pane is open appears without a refresh.
//
// Opening an entry is a callback rather than a MainWindow reference: this pane
// is settings, and depending on the browser window from it would invert the
// layering and drag the whole window into any test that exercises it.
class HistoryPage final : public QWidget {
    Q_OBJECT
public:
    using OpenUrlHandler = std::function<void(const QString&)>;

    // `store` is borrowed: the owner outlives this pane.
    HistoryPage(HistoryStore& store, OpenUrlHandler openUrl,
        QWidget* parent = nullptr);

    // Re-queries the store and rebuilds the list.
    //
    // Public because the pane holds no copy of the data: the settings window
    // calls it when the window is shown so the list reflects visits made while
    // it was closed, not just those made while it was open.
    void refresh();

private:
    void onSelectionChanged();
    void openSelected();
    void forgetSelected();
    void clearAll();
    void clearOlderThan(int days);
    void showCount();

    HistoryStore& m_store;
    OpenUrlHandler m_openUrl;
    QLineEdit* m_search { nullptr };
    QListWidget* m_list { nullptr };
    QLabel* m_summary { nullptr };
    QStackedWidget* m_body { nullptr };
    QLabel* m_empty { nullptr };
    QPushButton* m_open { nullptr };
    QPushButton* m_forget { nullptr };
};

} // namespace iridium::history