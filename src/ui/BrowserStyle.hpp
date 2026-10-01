#pragma once

#include <QColor>
#include <QObject>
#include <QString>

class QPalette;
class QWidget;

namespace iridium::ui {

// The window chrome the browser window and the settings window share.
//
// The two windows are built independently but must look like one application:
// same rounded translucent shell, same palette, same corner radii. Two copies of
// that styling is how they would drift, so both read it from here instead.

// The rounded translucent shell with no native title bar. A frameless window
// has to ask for both parts: a window that sets FramelessWindowHint without
// WA_TranslucentBackground renders as an opaque rectangle, and one that sets the
// translucency alone still has a title bar.
void makeFrameless(QWidget* window);

// The stylesheet both windows apply. `palette` supplies the colours, so the
// result follows the system colour scheme rather than hard-coding one.
QString sharedStyleSheet(const QPalette& palette);

// The stylesheet and the painted close control must use the same foreground
// for each tab state, rather than unrelated native selection palette roles.
// Text pairs meet 4.5:1; the selection indicator meets 3:1 against both surfaces.
struct TabColors {
    QColor background;
    QColor text;
    QColor hoverBackground;
    QColor hoverText;
    QColor selectedBackground;
    QColor selectedText;
    QColor selectedIndicator;
};

TabColors tabColors(const QPalette& palette);

// Gives a frameless window the two things the platform would otherwise have
// provided: a place to drag the window by, and a way to resize it from its
// edges.
//
// Both are separate from the styling because they are behaviour, not paint, and
// because a window missing either is unusable rather than merely ugly.
class FramelessChrome : public QObject {
    Q_OBJECT
public:
    // `window` must outlive this, which it does: the chrome is parented to it.
    explicit FramelessChrome(QWidget* window);

    // A widget that moves the window while the left button is held and dragged
    // past the system's drag threshold. Put it where a title bar would be: an
    // empty stretch in a layout is not a drag region.
    QWidget* dragHandle();

    // `margin` is how close to an edge a press starts a resize. The browser
    // window uses 8.
    void setResizeMargin(int margin) { m_resizeMargin = margin; }

protected:
    // Installed on the application rather than the window, so a press near an
    // edge is caught wherever inside the window it lands, instead of being
    // swallowed by whichever widget is under the pointer.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* m_window { nullptr };
    QWidget* m_dragHandle { nullptr };
    int m_resizeMargin { 8 };
};

} // namespace iridium::ui
