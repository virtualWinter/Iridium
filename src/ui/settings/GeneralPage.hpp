#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QToolButton;

namespace iridium {

// The general pane: startup page, search, downloads and tab confirmation.
//
// Every field writes through to SettingsStore as it changes, so there is no
// Apply button and nothing to lose by closing the window. The text fields commit
// on every keystroke rather than on editingFinished, because editingFinished
// does not fire when the window is closed with Escape or its close button, which
// would silently discard whatever was typed.
class GeneralPage final : public QWidget {
    Q_OBJECT
public:
    explicit GeneralPage(QWidget* parent = nullptr);

    // Re-reads every field from the store. Called when the window is shown,
    // because a setting can be changed from somewhere else -- a profile switch
    // points the store at a different file, and the panes are built once.
    void refresh();

private:
    // Writes the three text fields. Called after any of them changes, so one
    // path commits all three and a partially edited form cannot be stored.
    void commitFields();
    // Puts the store's effective values back into the fields, without writing
    // them back. The guard is what stops this from looping through commitFields.
    void showValues();
    void setStatus(const QString& message, bool isError);

    QLineEdit* m_homePage { nullptr };
    QLineEdit* m_searchTemplate { nullptr };
    QLineEdit* m_downloadDirectory { nullptr };
    QCheckBox* m_confirmClose { nullptr };
    QToolButton* m_browseDownloads { nullptr };
    QToolButton* m_resetHomePage { nullptr };
    QToolButton* m_resetSearch { nullptr };
    QLabel* m_status { nullptr };
    // True while showValues() is filling the fields, so the textChanged handler
    // does not treat its own writes as user input.
    bool m_showing { false };
};

} // namespace iridium
