#include "ui/decoration/windowdecoration.h"

#include <QAbstractButton>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QStyle>

// A single title-bar button. It draws the theme pixmap for its current state
// (active / hover / pressed / inactive), or the Qt style icon when the system
// theme ships no artwork.
class WindowButton : public QAbstractButton
{
public:
    WindowButton(DecorationTheme::Button button, const DecorationTheme *theme, QWidget *parent)
        : QAbstractButton(parent)
        , m_button(button)
        , m_theme(theme)
    {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setFixedSize(theme->buttonSize(button));
        setToolTip(toolTipFor(button, false));
    }

    void setWindowActive(bool active)
    {
        m_active = active;
        update();
    }

    void setMaximized(bool maximized)
    {
        m_maximized = maximized;
        setToolTip(toolTipFor(m_button, maximized));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        DecorationTheme::State state = DecorationTheme::State::Active;
        if (isDown())
            state = DecorationTheme::State::Pressed;
        else if (underMouse())
            state = DecorationTheme::State::Hover;
        else if (!isEnabled())
            state = DecorationTheme::State::Deactivated;

        QPainter painter(this);
        const QPixmap pixmap = m_theme->pixmap(m_button, m_maximized, m_active, state, size(),
                                               devicePixelRatioF());
        if (!pixmap.isNull())
            painter.drawPixmap(0, 0, pixmap);
        else
            fallbackIcon().paint(&painter, rect());
    }

    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    static QString toolTipFor(DecorationTheme::Button button, bool maximized)
    {
        switch (button) {
        case DecorationTheme::Button::Minimize:
            return tr("Minimize");
        case DecorationTheme::Button::Maximize:
            return maximized ? tr("Restore") : tr("Maximize");
        case DecorationTheme::Button::Close:
            return tr("Close");
        }
        return {};
    }

    QIcon fallbackIcon() const
    {
        switch (m_button) {
        case DecorationTheme::Button::Minimize:
            return style()->standardIcon(QStyle::SP_TitleBarMinButton);
        case DecorationTheme::Button::Maximize:
            return style()->standardIcon(m_maximized ? QStyle::SP_TitleBarNormalButton
                                                     : QStyle::SP_TitleBarMaxButton);
        case DecorationTheme::Button::Close:
            return style()->standardIcon(QStyle::SP_TitleBarCloseButton);
        }
        return {};
    }

    DecorationTheme::Button m_button;
    const DecorationTheme *m_theme = nullptr;
    bool m_active = true;
    bool m_maximized = false;
};

WindowDecoration::WindowDecoration(DecorationTheme::Side side, QWidget *parent)
    : QWidget(parent)
{
    const DecorationTheme &theme = DecorationTheme::system();

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(0, theme.buttonMarginTop(false), 0, 0);
    m_layout->setSpacing(theme.buttonSpacing());

    for (DecorationTheme::Button button : theme.buttons(side)) {
        auto *widget = new WindowButton(button, &theme, this);
        connect(widget, &QAbstractButton::clicked, this, [this, button] {
            switch (button) {
            case DecorationTheme::Button::Minimize:
                emit minimizeRequested();
                break;
            case DecorationTheme::Button::Maximize:
                emit maximizeRestoreRequested();
                break;
            case DecorationTheme::Button::Close:
                emit closeRequested();
                break;
            }
        });
        m_buttons.append(widget);
        m_layout->addWidget(widget);
    }
}

void WindowDecoration::setWindowActive(bool active)
{
    m_active = active;
    for (WindowButton *button : m_buttons)
        button->setWindowActive(active);
}

void WindowDecoration::setMaximized(bool maximized)
{
    m_maximized = maximized;
    const DecorationTheme &theme = DecorationTheme::system();
    m_layout->setContentsMargins(0, theme.buttonMarginTop(maximized), 0, 0);
    for (WindowButton *button : m_buttons)
        button->setMaximized(maximized);
}
