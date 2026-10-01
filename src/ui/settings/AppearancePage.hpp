#pragma once

#include <QWidget>

class QComboBox;
class QLabel;

namespace iridium {

// The appearance pane: the settings the browser currently honours.
//
// Colour scheme follows the system by default, which is what the engine emulates
// for pages. An explicit override exists because the system hint is the only
// source otherwise, and a user cannot express a preference that differs from it.
class AppearancePage final : public QWidget {
    Q_OBJECT
public:
    explicit AppearancePage(QWidget* parent = nullptr);

    // Re-reads the stored scheme. Called when the window is shown, because the
    // store is re-pointed at another profile's file on a profile switch and this
    // pane is built once.
    void refresh();

private:
    void showValues();

    QComboBox* m_scheme { nullptr };
    QLabel* m_status { nullptr };
    // True while the combo is being filled, so its changed signal is not read as
    // the user choosing something.
    bool m_showing { false };
};

} // namespace iridium
