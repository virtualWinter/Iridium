#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;

namespace iridium {

// The appearance pane: the settings the browser currently honours.
//
// Colour scheme follows the system by default, which is what the engine
// emulates for pages. An explicit override exists because the system hint is
// the only source otherwise, and a user cannot express a preference that differs
// from it.
class AppearancePage final : public QWidget {
    Q_OBJECT
public:
    explicit AppearancePage(QWidget* parent = nullptr);

private:
    void load();
    void save();
    void apply();

    QComboBox* m_scheme { nullptr };
    QCheckBox* m_followSystem { nullptr };
    QLabel* m_note { nullptr };
};

} // namespace iridium