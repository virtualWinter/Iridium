#include "ui/BrowserStyle.hpp"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace iridium::ui {

namespace {

// The corner radius of the window shell, and the one inside it. Two values
// because a nested surface has to be inset from the shell by a visible amount,
// or the two radii meet and read as one flat edge.
constexpr int kWindowRadius = 14;
constexpr int kInnerRadius = 11;

// The inset of a nested surface, and the gap between the shell and the window
// edge that the translucency shows through.
constexpr int kInset = 3;

double luminance(const QColor& color)
{
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92
                                  : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF())
        + 0.0722 * linear(color.blueF());
}

double contrast(const QColor& first, const QColor& second)
{
    const double a = luminance(first);
    const double b = luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

QColor opaque(QColor color)
{
    color = color.toRgb();
    color.setAlpha(255);
    return color;
}

QColor readableText(const QColor& background, const QColor& preferred)
{
    if (contrast(background, preferred) >= 4.5)
        return preferred;
    const QColor black(Qt::black);
    const QColor white(Qt::white);
    return contrast(background, black) >= contrast(background, white) ? black : white;
}

QColor blend(const QColor& background, const QColor& foreground, double amount)
{
    return QColor(qRound(background.red() * (1.0 - amount) + foreground.red() * amount),
        qRound(background.green() * (1.0 - amount) + foreground.green() * amount),
        qRound(background.blue() * (1.0 - amount) + foreground.blue() * amount));
}

} // namespace

TabColors tabColors(const QPalette& palette)
{
    TabColors colors;
    // Tabs live on Window, not Base. Keep the same readable colours when the
    // window is inactive instead of inheriting a theme's dimmed selection text.
    colors.background = opaque(palette.color(QPalette::Active, QPalette::Window));
    const QColor preferred = opaque(palette.color(QPalette::Active, QPalette::WindowText));
    colors.text = readableText(colors.background, preferred);
    colors.hoverBackground = blend(colors.background, colors.text, 0.06);
    colors.hoverText = readableText(colors.hoverBackground, preferred);
    colors.selectedBackground = blend(colors.background, colors.text, 0.12);
    colors.selectedText = readableText(colors.selectedBackground, preferred);

    // Selection is a neutral surface with a small accent indicator, not an
    // accent-coloured block that assumes white text will be readable on it.
    const auto indicatorContrast = [&colors](const QColor& indicator) {
        return std::min(contrast(indicator, colors.background),
            contrast(indicator, colors.selectedBackground));
    };
    colors.selectedIndicator = opaque(palette.color(QPalette::Active, QPalette::Highlight));
    if (indicatorContrast(colors.selectedIndicator) < 3.0) {
        const QColor black(Qt::black);
        const QColor white(Qt::white);
        colors.selectedIndicator = indicatorContrast(black) >= indicatorContrast(white)
            ? black : white;
    }
    return colors;
}

void makeFrameless(QWidget* window)
{
    if (!window)
        return;
    window->setAttribute(Qt::WA_TranslucentBackground);
    window->setWindowFlag(Qt::FramelessWindowHint, true);
}

QString sharedStyleSheet(const QPalette& palette)
{
    // Resolve the shell and tab states together. Palette changes rebuild these
    // values so the text and close controls always match the painted surface.
    const QString windowBackground = palette.color(QPalette::Active, QPalette::Window).name();
    const TabColors tabs = tabColors(palette);

    return QStringLiteral(R"(
        QWidget#browserRoot, QWidget#settingsRoot {
            background: %1; border-radius: 14px; }
        QWidget#arcSidebar, QWidget#settingsCategories {
            background: %1; border-top-right-radius: 14px;
            border-bottom-right-radius: 14px; }
        QFrame#rendererFrame, QFrame#settingsContent {
            border-width: 3px; border-right-width: 0px; border-style: solid;
            border-color: %1; border-radius: 11px;
            background: %1; }
        QStackedWidget#rendererPages, QStackedWidget#settingsPanes {
            background: palette(window); border-radius: 8px; }
        QLabel#noTabsState, QWidget#settingsDetails {
            background: palette(base); border-radius: 8px; }
        QLabel#noTabsState, QLabel#settingsValue { color: palette(text); }
        QLineEdit#addressBar, QLineEdit#settingsSearch {
            background: palette(base); border: 1px solid palette(mid);
            color: palette(text); }
        QLineEdit#addressBar { border-radius: 16px; padding: 8px 12px; }
        QLineEdit#settingsSearch { border-radius: 8px; padding: 6px 10px; }
        QLineEdit#addressBar:focus, QLineEdit#settingsSearch:focus {
            border-color: palette(highlight); }
        QToolButton#chromeButton, QToolButton#sidebarAction,
        QToolButton#settingsButton {
            border: 0; border-radius: 8px; color: palette(button-text); }
        QToolButton#chromeButton { padding: 7px; }
        QToolButton#sidebarAction, QToolButton#settingsButton {
            padding: 8px; text-align: left; }
        QToolButton#chromeButton:hover, QToolButton#sidebarAction:hover,
        QToolButton#settingsButton:hover { background: palette(midlight); }
        QToolButton#settingsButton:disabled {
            color: palette(disabled, palette(text)); }
        QListWidget#tabList {
            background: transparent; border: 0; outline: 0; }
        QListWidget#tabList::item {
            color: %2; border: 0; border-left: 2px solid transparent;
            border-radius: 8px; padding: 8px 32px 8px 8px; }
        QListWidget#tabList::item:hover { background: %3; color: %4; }
        QListWidget#tabList::item:selected,
        QListWidget#tabList::item:selected:hover {
            background: %5; color: %6; border-left-color: %7; font-weight: 600; }
        QPushButton#settingsButton {
            background: palette(base); color: palette(button-text);
            border: 1px solid palette(mid); border-radius: 8px; padding: 6px 14px; }
        QPushButton#settingsButton:hover { background: palette(midlight); }
        QPushButton#settingsButton:disabled {
            color: palette(disabled, palette(text));
            border-color: palette(mid); }
        QComboBox#settingsCombo {
            background: palette(base); border: 1px solid palette(mid);
            border-radius: 8px; padding: 5px 10px; color: palette(text); }
        QComboBox#settingsCombo:focus { border-color: palette(highlight); }
        QComboBox#settingsCombo::drop-down { border: 0; width: 20px; }
        QComboBox#settingsCombo QAbstractItemView {
            background: palette(base); border: 1px solid palette(mid);
            selection-background-color: palette(highlight);
            selection-color: palette(highlighted-text); outline: 0; }
        QCheckBox#settingsCheck { color: palette(text); spacing: 8px; }
        QCheckBox#settingsCheck::indicator { width: 16px; height: 16px; }
        QLabel#settingsSubtitle, QLabel#settingsFootnote,
        QLabel#settingsEmpty { color: palette(mid); }
        QLabel#settingsEmpty { padding: 24px; }
        QLabel#settingsCaption { color: palette(mid); font-weight: 600; }
        QLabel#settingsStatus {
            color: palette(mid); padding: 6px 8px; border-radius: 6px;
            background: palette(base); }
        QLabel#settingsStatus[error="true"] {
            color: palette(brightText); background: #b3261e;
            border: 1px solid #8c1d18; }
        QScrollArea#settingsDetailsScroll { background: transparent; border: 0; }
        QWidget#settingsEntryList, QListWidget#settingsCategoryList {
            background: transparent; border: 0; outline: 0; }
        QListWidget#settingsCategoryList::item, QListWidget#settingsEntryList::item {
            color: palette(text); border-radius: 8px; padding: 8px 10px;
            margin: 2px 0; }
        QListWidget#settingsCategoryList::item:hover,
        QListWidget#settingsEntryList::item:hover { background: palette(midlight); }
        QListWidget#settingsCategoryList::item:selected,
        QListWidget#settingsEntryList::item:selected {
            background: palette(highlight); color: palette(highlighted-text); }
        QListWidget#settingsEntryList::item:disabled {
            color: palette(disabled, palette(text)); }
        QToolTip { background: palette(base); color: palette(text);
            border: 1px solid palette(mid); padding: 4px 6px; }
    )").arg(windowBackground, tabs.text.name(), tabs.hoverBackground.name(),
        tabs.hoverText.name(), tabs.selectedBackground.name(), tabs.selectedText.name(),
        tabs.selectedIndicator.name());
}

FramelessChrome::FramelessChrome(QWidget* window)
    : QObject(window)
    , m_window(window)
{
    if (!m_window)
        return;
    makeFrameless(m_window);
    // The application, not the window: a press within a few pixels of an edge
    // usually lands on a child widget, and a per-window filter would only see
    // the events that reached the window itself.
    qApp->installEventFilter(this);
}

QWidget* FramelessChrome::dragHandle()
{
    if (m_dragHandle)
        return m_dragHandle;

    // Built here rather than in each window so both windows drag the same way.
    class DragHandle final : public QWidget {
    public:
        using QWidget::QWidget;

    protected:
        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() == Qt::LeftButton)
                m_origin = event->globalPosition().toPoint();
        }

        void mouseMoveEvent(QMouseEvent* event) override
        {
            if (!(event->buttons() & Qt::LeftButton)
                || (event->globalPosition().toPoint() - m_origin).manhattanLength()
                    < QApplication::startDragDistance()) {
                return;
            }
            // The platform moves the window, so it follows the pointer correctly
            // on Wayland, where a client cannot set its own position.
            if (window() && window()->windowHandle())
                window()->windowHandle()->startSystemMove();
        }

    private:
        QPoint m_origin;
    };

    m_dragHandle = new DragHandle(m_window);
    return m_dragHandle;
}

bool FramelessChrome::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() != QEvent::MouseButtonPress || !m_window
        || m_window->isMaximized()) {
        return QObject::eventFilter(watched, event);
    }

    // Only presses that belong to this window: the filter is on the
    // application, so it sees every window's events.
    auto* widget = qobject_cast<QWidget*>(watched);
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (!widget || widget->window() != m_window || mouse->button() != Qt::LeftButton)
        return QObject::eventFilter(watched, event);

    const QPoint position = m_window->mapFromGlobal(
        mouse->globalPosition().toPoint());
    Qt::Edges edges;
    if (position.x() < m_resizeMargin)
        edges |= Qt::LeftEdge;
    else if (position.x() >= m_window->width() - m_resizeMargin)
        edges |= Qt::RightEdge;
    if (position.y() < m_resizeMargin)
        edges |= Qt::TopEdge;
    else if (position.y() >= m_window->height() - m_resizeMargin)
        edges |= Qt::BottomEdge;
    if (!edges)
        return QObject::eventFilter(watched, event);

    // startSystemResize asks the compositor to do it, which is the only way on
    // Wayland. It can refuse, in which case the event is passed on rather than
    // swallowed, so a press is never silently dropped.
    if (m_window->windowHandle() && m_window->windowHandle()->startSystemResize(edges)) {
        event->accept();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

} // namespace iridium::ui
