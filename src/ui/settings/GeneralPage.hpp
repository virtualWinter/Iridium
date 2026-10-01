#pragma once

#include <QWidget>

class QCheckBox;
class QLineEdit;
class QToolButton;

namespace iridium {

// The general pane: homepage, search, downloads and tab confirmation.
//
// Every field writes through to SettingsStore on change, so there is no Apply
// button and nothing to lose by closing the window.
class GeneralPage final : public QWidget {
    Q_OBJECT
public:
    explicit GeneralPage(QWidget* parent = nullptr);

private:
    void load();

    QLineEdit* m_homePage { nullptr };
    QLineEdit* m_searchTemplate { nullptr };
    QLineEdit* m_downloadDirectory { nullptr };
    QCheckBox* m_confirmClose { nullptr };
    QToolButton* m_browseDownloads { nullptr };
};

} // namespace iridium