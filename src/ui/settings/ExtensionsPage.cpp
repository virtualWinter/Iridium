#include "ui/settings/ExtensionsPage.hpp"

#include "extensions/ExtensionInstaller.hpp"
#include "extensions/ExtensionPaths.hpp"
#include "extensions/ExtensionRegistry.hpp"
#include "ui/MainWindow.hpp"

#include <QApplication>
#include <QFileDialog>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QToolButton>
#include <QVBoxLayout>

namespace iridium {

namespace {

constexpr int kIconSize = 32;

// Row data lives in the item, so the list does not need a parallel model.
// Item data is keyed by int, hence a role rather than a name.
constexpr int kIdRole = Qt::UserRole + 100;
// Secondary line showing enabled/disabled state.
constexpr int kStateRole = Qt::UserRole + 101;

QString itemId(const QListWidgetItem* item)
{
    return item ? item->data(kIdRole).toString() : QString();
}

// Icon decoding lives here rather than in the registry, which stays on QtCore
// so it can be tested without a GUI.
QPixmap loadIcon(const extensions::Extension& extension, const QSize& size)
{
    const QJsonObject icons = extension.manifest().action().icons;
    if (icons.isEmpty())
        return {};

    // Manifests key icons by pixel size. Prefer the smallest entry at least as
    // large as the request, so a 128px icon is not used where 32px is declared.
    QString best;
    int bestArea = 0;
    for (auto it = icons.begin(); it != icons.end(); ++it) {
        const QStringList dimensions = it.key().split(QLatin1Char('x'));
        if (dimensions.size() != 2)
            continue;
        bool okWidth = false;
        bool okHeight = false;
        const int width = dimensions.at(0).toInt(&okWidth);
        const int height = dimensions.at(1).toInt(&okHeight);
        if (!okWidth || !okHeight || width < size.width() || height < size.height())
            continue;
        if (width * height > bestArea) {
            bestArea = width * height;
            best = it.value().toString();
        }
    }
    // Fall back to any declared icon rather than showing none.
    if (best.isEmpty())
        best = icons.begin().value().toString();

    const QString contents = extension.readResource(best);
    if (contents.isEmpty())
        return {};

    QImage decoded;
    if (!decoded.loadFromData(contents.toUtf8()))
        return {};
    return QPixmap::fromImage(decoded.scaled(size, Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
}

// Draws the row: extension name, then the enabled/disabled state in a smaller,
// dimmer font. The default single-line item cannot show the state, and a
// disabled extension has to be distinguishable at a glance.
class ExtensionRowDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        painter->save();
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.widget->style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter);

        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const QColor nameColor = selected
            ? option.palette.color(QPalette::HighlightedText)
            : option.palette.color(QPalette::Text);
        const QColor stateColor = selected
            ? option.palette.color(QPalette::HighlightedText).lighter(120)
            : option.palette.color(QPalette::Disabled, QPalette::Text);

        const QStringList lines = index.data(Qt::DisplayRole).toString().split(
            QLatin1Char('\n'));
        const QRect rect = option.rect.adjusted(44, 6, -8, -6);

        QFont nameFont(option.font);
        nameFont.setBold(false);
        painter->setFont(nameFont);
        painter->setPen(nameColor);
        painter->drawText(QRect(rect.left(), rect.top(), rect.width(), rect.height() / 2),
            Qt::AlignLeft | Qt::AlignVCenter, lines.value(0));

        if (lines.size() > 1) {
            QFont stateFont(option.font);
            stateFont.setPointSizeF(option.font.pointSizeF() * 0.82);
            painter->setFont(stateFont);
            painter->setPen(stateColor);
            painter->drawText(
                QRect(rect.left(), rect.top() + rect.height() / 2, rect.width(),
                    rect.height() / 2),
                Qt::AlignLeft | Qt::AlignVCenter, lines.value(1));
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        // Two lines of text plus padding, and wide enough for an icon.
        Q_UNUSED(option);
        Q_UNUSED(index);
        return QSize(220, 52);
    }
};

} // namespace

ExtensionsPage::ExtensionsPage(extensions::ExtensionRegistry& registry, QWidget* parent)
    : QWidget(parent)
    , m_registry(registry)
{
    setObjectName(QStringLiteral("extensionsPage"));
    buildUi();

    connect(&m_registry, &extensions::ExtensionRegistry::changed,
        this, &ExtensionsPage::rebuild);
    rebuild();
}

void ExtensionsPage::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("Extensions"), this);
    heading->setObjectName(QStringLiteral("settingsHeading"));
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("settingsSearch"));
    m_filter->setPlaceholderText(tr("Search extensions"));
    m_filter->setClearButtonEnabled(true);
    toolbar->addWidget(m_filter, 1);
    connect(m_filter, &QLineEdit::textChanged, this, &ExtensionsPage::rebuild);

    auto* installButton = new QToolButton(this);
    // A folder of files is the input, not a single archive.
    installButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    installButton->setObjectName(QStringLiteral("settingsButton"));
    installButton->setText(tr("Install"));
    installButton->setToolTip(tr("Install an unpacked extension from a folder"));
    installButton->setCursor(Qt::PointingHandCursor);
    toolbar->addWidget(installButton);
    connect(installButton, &QToolButton::clicked, this, &ExtensionsPage::installUnpacked);

    root->addLayout(toolbar);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    root->addWidget(m_status);

    m_body = new QStackedWidget(this);

    auto* split = new QWidget(m_body);
    auto* splitLayout = new QHBoxLayout(split);
    splitLayout->setContentsMargins(0, 0, 0, 0);
    splitLayout->setSpacing(16);

    m_list = new QListWidget(split);
    m_list->setObjectName(QStringLiteral("settingsEntryList"));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIconSize(QSize(kIconSize, kIconSize));
    m_list->setSpacing(2);
    m_list->setUniformItemSizes(false);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMinimumWidth(300);
    m_list->setItemDelegate(new ExtensionRowDelegate(m_list));
    splitLayout->addWidget(m_list, 1);
    connect(m_list, &QListWidget::currentItemChanged,
        this, &ExtensionsPage::onSelectionChanged);

    // Details scroll independently so a long permission list does not stretch
    // the window.
    auto* scroll = new QScrollArea(split);
    scroll->setObjectName(QStringLiteral("settingsDetailsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(260);

    m_details = new QWidget(scroll);
    m_details->setObjectName(QStringLiteral("settingsDetails"));
    auto* detailsLayout = new QVBoxLayout(m_details);
    detailsLayout->setContentsMargins(4, 4, 4, 4);
    detailsLayout->setSpacing(6);

    auto addDetail = [this, detailsLayout](const QString& label) {
        auto* caption = new QLabel(label, m_details);
        caption->setObjectName(QStringLiteral("settingsCaption"));
        detailsLayout->addWidget(caption);
        auto* value = new QLabel(m_details);
        value->setObjectName(QStringLiteral("settingsValue"));
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailsLayout->addWidget(value);
        return value;
    };

    m_detailsName = addDetail(tr("Name"));
    m_detailsVersion = addDetail(tr("Version"));
    m_detailsId = addDetail(tr("Identifier"));
    m_detailsPermissions = addDetail(tr("Permissions"));
    m_detailsHosts = addDetail(tr("Site access"));
    m_detailsLocation = addDetail(tr("Location"));
    detailsLayout->addStretch(1);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    // Optional permissions, each a checkbox. Built per selection because the
    // set differs per extension.
    m_optionalCaption = new QLabel(tr("Optional permissions"), m_details);
    m_optionalCaption->setObjectName(QStringLiteral("settingsCaption"));
    detailsLayout->addWidget(m_optionalCaption);
    m_optionalPermissions = new QWidget(m_details);
    m_optionalPermissions->setObjectName(QStringLiteral("settingsPermissions"));
    auto* optionalLayout = new QVBoxLayout(m_optionalPermissions);
    optionalLayout->setContentsMargins(0, 0, 0, 0);
    optionalLayout->setSpacing(2);
    detailsLayout->addWidget(m_optionalPermissions);

    m_toggle = new QToolButton(m_details);
    m_toggle->setObjectName(QStringLiteral("settingsButton"));
    m_toggle->setCursor(Qt::PointingHandCursor);
    actions->addWidget(m_toggle);
    connect(m_toggle, &QToolButton::clicked, this, &ExtensionsPage::toggleSelected);

    auto* orderRow = new QHBoxLayout;
    orderRow->setSpacing(8);
    m_moveUp = new QToolButton(m_details);
    m_moveUp->setObjectName(QStringLiteral("settingsButton"));
    m_moveUp->setText(tr("Move up"));
    m_moveUp->setCursor(Qt::PointingHandCursor);
    m_moveUp->setToolTip(tr("Run this extension before the ones below it"));
    orderRow->addWidget(m_moveUp);
    connect(m_moveUp, &QToolButton::clicked, this, [this] { moveSelected(-1); });

    m_moveDown = new QToolButton(m_details);
    m_moveDown->setObjectName(QStringLiteral("settingsButton"));
    m_moveDown->setText(tr("Move down"));
    m_moveDown->setCursor(Qt::PointingHandCursor);
    m_moveDown->setToolTip(tr("Run this extension after the ones above it"));
    orderRow->addWidget(m_moveDown);
    connect(m_moveDown, &QToolButton::clicked, this, [this] { moveSelected(1); });
    detailsLayout->addLayout(orderRow);

    m_remove = new QToolButton(m_details);
    m_remove->setObjectName(QStringLiteral("settingsButton"));
    m_remove->setText(tr("Remove"));
    m_remove->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_remove->setCursor(Qt::PointingHandCursor);
    m_remove->setToolTip(tr("Remove this extension and its stored data"));
    actions->addWidget(m_remove);
    connect(m_remove, &QToolButton::clicked, this, &ExtensionsPage::removeSelected);

    detailsLayout->addLayout(actions);

    m_updateCheck = new QToolButton(m_details);
    m_updateCheck->setObjectName(QStringLiteral("settingsButton"));
    m_updateCheck->setText(tr("Check for updates"));
    m_updateCheck->setCursor(Qt::PointingHandCursor);
    m_updateCheck->setToolTip(tr("Compare the installed manifest against an update "
                                 "manifest you provide"));
    detailsLayout->addWidget(m_updateCheck);
    connect(m_updateCheck, &QToolButton::clicked, this, &ExtensionsPage::checkForUpdate);
    scroll->setWidget(m_details);
    splitLayout->addWidget(scroll, 1);

    m_body->addWidget(split);

    m_empty = new QLabel(m_body);
    m_empty->setObjectName(QStringLiteral("settingsEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_body->addWidget(m_empty);

    root->addWidget(m_body, 1);

    auto* note = new QLabel(
        tr("Background pages and service workers are not supported."), this);
    note->setObjectName(QStringLiteral("settingsFootnote"));
    note->setWordWrap(true);
    root->addWidget(note);
}

void ExtensionsPage::reportStatus(const QString& message, bool isError)
{
    if (message.isEmpty()) {
        m_status->setVisible(false);
        return;
    }
    m_status->setText(message);
    m_status->setProperty("error", isError);
    // Re-polish so a colour change takes effect without recreating the label.
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setVisible(true);
}

void ExtensionsPage::rebuild()
{
    const QString selected = m_selectedId;
    const QString filter = m_filter ? m_filter->text().trimmed() : QString();

    m_list->clear();

    int visible = 0;
    const int failures = static_cast<int>(m_registry.loadErrors().size());
    for (const auto& extension : m_registry.extensions()) {
        const QString name = extension->displayName();
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)
            && !extension->id().contains(filter, Qt::CaseInsensitive)
            && !extension->manifest().version().contains(filter, Qt::CaseInsensitive)) {
            continue;
        }

        auto* item = new QListWidgetItem(m_list);
        item->setData(kIdRole, extension->id());
        // Name plus state on separate lines, drawn by ExtensionRowDelegate.
        const bool enabled = m_registry.isEnabled(extension->id());
        item->setText(name + QLatin1Char('\n') + (enabled ? tr("Enabled") : tr("Disabled")));
        item->setData(kStateRole, enabled ? tr("Enabled") : tr("Disabled"));
        item->setToolTip(enabled
            ? tr("%1 — running").arg(extension->id())
            : tr("%1 — stopped").arg(extension->id()));
        const QPixmap icon = loadIcon(*extension, QSize(kIconSize, kIconSize));
        if (!icon.isNull()) {
            item->setIcon(QIcon(icon));
        } else {
            // No declared icon: draw a tinted square so rows stay aligned and
            // the list does not look half-empty.
            QPixmap fallback(kIconSize, kIconSize);
            fallback.fill(Qt::transparent);
            QPainter painter(&fallback);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette().color(QPalette::Highlight));
            painter.drawRoundedRect(QRectF(2, 2, kIconSize - 4, kIconSize - 4), 6, 6);
            item->setIcon(QIcon(fallback));
        }

        if (!enabled) {
            item->setForeground(palette().color(QPalette::Disabled, QPalette::Text));
        }
        ++visible;
    }

    if (m_list->count() > 0) {
        // Restore the previous selection when it survived the rebuild.
        QListWidgetItem* restore = nullptr;
        for (int row = 0; row < m_list->count(); ++row) {
            if (itemId(m_list->item(row)) == selected) {
                restore = m_list->item(row);
                break;
            }
        }
        m_list->setCurrentItem(restore ? restore : m_list->item(0));
    }

    if (visible > 0) {
        m_body->setCurrentIndex(0);
    } else {
        m_empty->setText(filter.isEmpty()
            ? tr("No extensions installed")
            : tr("No extension matches \"%1\".").arg(filter));
        m_body->setCurrentIndex(1);
    }

    // Load failures are worth surfacing: a silently skipped extension looks
    // like the browser ignoring the folder.
    if (failures > 0) {
        QStringList lines;
        for (auto it = m_registry.loadErrors().constBegin();
             it != m_registry.loadErrors().constEnd(); ++it) {
            lines.append(QStringLiteral("• %1 — %2").arg(it.key(), it.value()));
        }
        reportStatus(tr("Could not read %n extension(s):", "", failures)
            + QStringLiteral("\n") + lines.join(QLatin1Char('\n')), true);
    } else {
        // Clear any earlier warning: a fixed manifest must not leave a stale
        // error on screen.
        reportStatus({}, false);
    }

    updateDetails();
}

void ExtensionsPage::onSelectionChanged()
{
    m_selectedId = itemId(m_list->currentItem());
    updateDetails();
}

void ExtensionsPage::updateDetails()
{
    extensions::Extension* extension = m_selectedId.isEmpty()
        ? nullptr : m_registry.find(m_selectedId);

    const bool have = extension != nullptr;
    m_details->setEnabled(have);
    if (!have) {
        m_selectedId.clear();
        return;
    }

    const extensions::Manifest& manifest = extension->manifest();

    m_detailsName->setText(extension->displayName());
    m_detailsVersion->setText(manifest.version().isEmpty()
        ? tr("unspecified") : manifest.version());
    m_detailsId->setText(extension->id());

    const QStringList permissions = manifest.permissions();
    m_detailsPermissions->setText(permissions.isEmpty()
        ? tr("None requested")
        : QStringLiteral("• ") + permissions.join(QStringLiteral("\n• ")));

    const QStringList hosts = manifest.hostPermissions();
    m_detailsHosts->setText(hosts.isEmpty()
        ? tr("No sites requested")
        : QStringLiteral("• ") + hosts.join(QStringLiteral("\n• ")));

    m_detailsLocation->setText(extension->directory());

    const bool enabled = m_registry.isEnabled(extension->id());
    m_toggle->setText(enabled ? tr("Disable") : tr("Enable"));
    m_toggle->setToolTip(enabled
        ? tr("Stop running this extension")
        : tr("Run this extension in the pages it requested"));

    // System extensions live in a read-only location, so removal is refused
    // rather than offered and then failing.
    const bool userInstalled = extension->isUserInstalled(
        extensions::ExtensionPaths::userDirectory());
    m_remove->setEnabled(userInstalled);
    m_remove->setToolTip(userInstalled
        ? tr("Remove this extension and its stored data")
        : tr("This extension is installed system-wide and cannot be removed here"));

    rebuildPermissionToggles(*extension);

    // Ordering controls, so injection order can be corrected from the UI.
    const int position = m_registry.sortedIds().indexOf(extension->id());
    m_moveUp->setEnabled(position > 0);
    m_moveDown->setEnabled(position >= 0
        && position < m_registry.sortedIds().size() - 1);

    // Repeat the check for the selected extension so it reads from the menu
    // rather than from a stale value.
    const QString remembered = m_updateSources.value(extension->id());
    m_updateCheck->setToolTip(remembered.isEmpty()
        ? tr("Compare the installed manifest against an update manifest")
        : tr("Update manifest: %1").arg(remembered));
}

void ExtensionsPage::rebuildPermissionToggles(const extensions::Extension& extension)
{
    // Rebuild only actual controls; an extension with no optional permissions
    // needs neither an empty section nor a repeated explanatory label.
    while (auto* item = m_optionalPermissions->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QStringList optional = extension.manifest().optionalPermissions();
    const QStringList granted = m_registry.grantedPermissions(extension.id());

    m_optionalCaption->setVisible(!optional.isEmpty());
    m_optionalPermissions->setVisible(!optional.isEmpty());
    if (optional.isEmpty())
        return;

    for (const QString& permission : optional) {
        auto* box = new QCheckBox(permission, m_optionalPermissions);
        box->setObjectName(QStringLiteral("settingsCheck"));
        box->setChecked(granted.contains(permission));
        box->setToolTip(tr("Allow %1 to use the \"%2\" API")
            .arg(extension.displayName(), permission));
        const QString id = extension.id();
        connect(box, &QCheckBox::toggled, this, [this, id, permission](bool on) {
            if (m_registry.setPermissionGranted(id, permission, on)) {
                reportStatus(on
                    ? tr("Granted \"%1\" to %2.").arg(permission, id)
                    : tr("Revoked \"%1\" from %2.").arg(permission, id), false);
            } else {
                reportStatus(tr("Could not change \"%1\" for %2.").arg(permission, id),
                    true);
            }
        });
        m_optionalPermissions->layout()->addWidget(box);
    }
}

void ExtensionsPage::checkForUpdate()
{
    extensions::Extension* extension = m_selectedId.isEmpty()
        ? nullptr : m_registry.find(m_selectedId);
    if (!extension)
        return;

    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Check for updates — %1").arg(extension->displayName()));
    dialog.setLabelText(tr("Enter the URL or path of an update manifest. "
                           "It must be a JSON object containing a \"version\" field."));
    dialog.setTextValue(m_updateSources.value(m_selectedId));
    dialog.setInputMode(QInputDialog::TextInput);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString source = dialog.textValue().trimmed();
    m_updateSources.insert(m_selectedId, source);
    if (source.isEmpty()) {
        reportStatus({}, false);
        return;
    }

    // Network checks are synchronous here, so tell the user why the UI pauses.
    reportStatus(tr("Checking %1…").arg(source), false);
    QApplication::processEvents();

    const extensions::UpdateCheckResult result =
        extensions::ExtensionInstaller::checkForUpdate(extension->directory(), source);
    if (!result.ok) {
        reportStatus(tr("Update check failed: %1").arg(result.error), true);
        return;
    }
    if (result.updateAvailable) {
        reportStatus(tr("Version %1 is available; %2 is installed. Iridium does "
                        "not fetch updates itself, so install the new version "
                        "from its source.")
            .arg(result.latestVersion, result.currentVersion), false);
    } else {
        reportStatus(tr("%1 is up to date (version %2).")
            .arg(extension->displayName(), result.currentVersion), false);
    }
}

void ExtensionsPage::moveSelected(int delta)
{
    if (m_selectedId.isEmpty())
        return;
    const bool moved = delta < 0
        ? m_registry.moveUp(m_selectedId) : m_registry.moveDown(m_selectedId);
    if (!moved)
        return;
    reportStatus(tr("Moved %1 %2.").arg(m_selectedId,
        delta < 0 ? tr("up") : tr("down")), false);
}

void ExtensionsPage::toggleSelected()
{
    if (m_selectedId.isEmpty())
        return;
    const bool wanted = !m_registry.isEnabled(m_selectedId);
    if (m_registry.setEnabled(m_selectedId, wanted)) {
        reportStatus(wanted
            ? tr("Enabled %1.").arg(m_selectedId)
            : tr("Disabled %1.").arg(m_selectedId), false);
    } else {
        reportStatus(tr("Could not enable %1: a dependency is missing.").arg(m_selectedId),
            true);
    }
}

void ExtensionsPage::installUnpacked()
{
    // Start in the user extension directory so a repeat install is one click.
    const QString start = extensions::ExtensionPaths::userDirectory().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        : extensions::ExtensionPaths::userDirectory();
    const QString directory = QFileDialog::getExistingDirectory(this,
        tr("Choose an unpacked extension folder"), start);
    if (directory.isEmpty())
        return;
    installFromDirectory(directory);
}

void ExtensionsPage::installFromDirectory(const QString& directory)
{
    QString error;
    QString id;
    if (!extensions::ExtensionInstaller::install(directory, &id, &error)) {
        reportStatus(tr("Could not install: %1").arg(error), true);
        return;
    }

    m_registry.reload();
    // A fresh install is enabled by default: the user asked for it.
    m_registry.setEnabled(id, true);
    m_selectedId = id;
    reportStatus(tr("Installed %1.").arg(id), false);
    rebuild();
}

void ExtensionsPage::removeSelected()
{
    if (m_selectedId.isEmpty())
        return;

    extensions::Extension* extension = m_registry.find(m_selectedId);
    if (!extension)
        return;

    const QString id = m_selectedId;
    const QString name = extension->displayName();

    // QMessageBox has no Remove button, so build one: a destructive action should
    // not share a button with Cancel.
    QMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Warning);
    confirm.setWindowTitle(tr("Remove extension"));
    confirm.setText(tr("Remove %1?").arg(name));
    confirm.setInformativeText(tr("Its files and stored data are deleted. "
                                  "This cannot be undone."));
    QPushButton* removeButton = confirm.addButton(tr("Remove"),
        QMessageBox::DestructiveRole);
    confirm.addButton(QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Cancel);
    confirm.exec();
    if (confirm.clickedButton() != removeButton)
        return;

    QString error;
    if (!extensions::ExtensionInstaller::uninstall(id, &error)) {
        reportStatus(tr("Could not remove: %1").arg(error), true);
        return;
    }

    m_selectedId.clear();
    m_registry.reload();
    reportStatus(tr("Removed %1.").arg(name), false);
}

} // namespace iridium
