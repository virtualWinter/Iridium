#pragma once

#include <QList>
#include <QWidget>

#include "ui/decoration/decorationtheme.h"

class WindowButton;
class QHBoxLayout;

// The window controls (minimize, maximize/restore, close) for a frameless
// window, drawn with the artwork of the system's decoration theme.
//
// It only emits intent; the window decides what to do with it. The set of
// buttons and their order come from the system theme, so a decoration is built
// per side (left and/or right).
class WindowDecoration : public QWidget
{
    Q_OBJECT

public:
    explicit WindowDecoration(DecorationTheme::Side side, QWidget *parent = nullptr);

    void setWindowActive(bool active);
    void setMaximized(bool maximized);

signals:
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

private:
    QList<WindowButton *> m_buttons;
    QHBoxLayout *m_layout = nullptr;
    bool m_active = true;
    bool m_maximized = false;
};
