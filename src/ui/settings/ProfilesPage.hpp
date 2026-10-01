#pragma once

#include "browser/ProfileManager.hpp"

#include <QSize>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace iridium {

// The profiles pane: choose the active profile, create one, and delete one.
//
// Switching profiles cannot be done in place: open tabs, extension state and
// history all belong to the profile that created them, and the engine holds
// per-view state that a swap would invalidate. So switching is offered as a
// restart, which is what actually applies it.
class ProfilesPage final : public QWidget {
    Q_OBJECT
public:
    explicit ProfilesPage(QWidget* parent = nullptr);

    // Re-reads the profile list. Public so the settings window can call it when
    // shown, in case a profile was created elsewhere in the session.
    void refresh();

private:
    void createProfile();
    void removeProfile();
    // Called after a switch so the running window knows it must reload.
    void requestRestart(const QString& name);
    void showStatus(const QString& message, bool isError);

    QComboBox* m_current { nullptr };
    QLineEdit* m_name { nullptr };
    QLabel* m_status { nullptr };
    QLabel* m_detail { nullptr };
    QPushButton* m_remove { nullptr };
};

} // namespace iridium