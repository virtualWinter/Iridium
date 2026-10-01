#include "ui/settings/ProfilesPage.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace iridium {

ProfilesPage::ProfilesPage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("profilesPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("Profiles"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* subtitle = new QLabel(
        tr("A profile keeps its own extensions, extension data, settings and "
           "history. Switching profiles needs a restart, because open tabs belong "
           "to the profile that created them."), this);
    subtitle->setObjectName(QStringLiteral("settingsSubtitle"));
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_current = new QComboBox(this);
    m_current->setObjectName(QStringLiteral("settingsCombo"));
    form->addRow(tr("Active profile"), m_current);

    auto* nameRow = new QWidget(this);
    auto* nameLayout = new QHBoxLayout(nameRow);
    nameLayout->setContentsMargins(0, 0, 0, 0);
    nameLayout->setSpacing(8);
    m_name = new QLineEdit(nameRow);
    m_name->setObjectName(QStringLiteral("settingsSearch"));
    m_name->setPlaceholderText(tr("Name for the new profile"));
    nameLayout->addWidget(m_name, 1);
    auto* createButton = new QPushButton(tr("Create"), nameRow);
    createButton->setObjectName(QStringLiteral("settingsButton"));
    nameLayout->addWidget(createButton);
    form->addRow(tr("New profile"), nameRow);
    root->addLayout(form);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto* switchButton = new QPushButton(tr("Switch to this profile"), this);
    switchButton->setObjectName(QStringLiteral("settingsButton"));
    actions->addWidget(switchButton);
    m_remove = new QPushButton(tr("Remove profile"), this);
    m_remove->setObjectName(QStringLiteral("settingsButton"));
    actions->addWidget(m_remove);
    actions->addStretch(1);
    root->addLayout(actions);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("settingsStatus"));
    m_status->setWordWrap(true);
    m_status->setVisible(false);
    root->addWidget(m_status);

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("settingsFootnote"));
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_detail);

    root->addStretch(1);

    connect(createButton, &QPushButton::clicked, this, &ProfilesPage::createProfile);
    connect(switchButton, &QPushButton::clicked, this, [this] {
        if (m_current->currentData().toString().isEmpty())
            return;
        requestRestart(m_current->currentData().toString());
    });
    connect(m_remove, &QPushButton::clicked, this, &ProfilesPage::removeProfile);
    // Enter in the name field creates, which is what the field looks like it does.
    connect(m_name, &QLineEdit::returnPressed, this, &ProfilesPage::createProfile);
    connect(m_current, &QComboBox::currentIndexChanged,
        this, [this](int) { refresh(); });

    refresh();
}

void ProfilesPage::showStatus(const QString& message, bool isError)
{
    if (message.isEmpty()) {
        m_status->setVisible(false);
        return;
    }
    m_status->setText(message);
    m_status->setProperty("error", isError);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setVisible(true);
}

void ProfilesPage::refresh()
{
    ProfileManager& manager = ProfileManager::instance();
    const QString previous = m_current->currentData().toString();

    m_current->blockSignals(true);
    m_current->clear();
    for (const Profile& profile : manager.profiles())
        m_current->addItem(profile.name, profile.name);
    const int index = m_current->findData(
        previous.isEmpty() ? manager.current().name : previous);
    m_current->setCurrentIndex(index >= 0 ? index : 0);
    m_current->blockSignals(false);

    const QString selected = m_current->currentData().toString();
    const Profile profile = manager.byName(selected);
    // The default is the fallback every time, so removing it is never possible.
    m_remove->setEnabled(profile.isValid() && !profile.builtin);

    m_detail->setText(profile.isValid()
        ? tr("Data directory:\n%1").arg(profile.path)
        : tr("Select a profile to see where its data lives."));
}

void ProfilesPage::createProfile()
{
    const QString name = m_name->text().trimmed();
    if (name.isEmpty()) {
        showStatus(tr("Give the profile a name first."), true);
        return;
    }

    QString error;
    if (!ProfileManager::instance().create(name, &error)) {
        showStatus(error, true);
        return;
    }

    m_name->clear();
    showStatus(tr("Created \"%1\".").arg(name), false);
    refresh();
    // Select the one just created, so the next action can be to switch to it.
    const int index = m_current->findData(name);
    if (index >= 0)
        m_current->setCurrentIndex(index);
}

void ProfilesPage::removeProfile()
{
    const QString name = m_current->currentData().toString();
    if (name.isEmpty())
        return;

    QString error;
    const Profile profile = ProfileManager::instance().byName(name);
    if (!profile.isValid() || profile.builtin) {
        showStatus(tr("That profile cannot be removed."), true);
        return;
    }

    const auto answer = QMessageBox::question(this, tr("Remove profile"),
        tr("Remove \"%1\"?\n\nIts extensions, extension data, settings and history "
           "are deleted. This cannot be undone.").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    if (!ProfileManager::instance().remove(name, &error)) {
        showStatus(error, true);
        return;
    }

    showStatus(tr("Removed \"%1\".").arg(name), false);
    refresh();
}

void ProfilesPage::requestRestart(const QString& name)
{
    ProfileManager& manager = ProfileManager::instance();
    if (name == manager.current().name) {
        showStatus(tr("\"%1\" is already the active profile.").arg(name), false);
        return;
    }

    const auto answer = QMessageBox::question(this, tr("Switch profile"),
        tr("Switch to \"%1\"?\n\nIridium needs to restart: open tabs and their "
           "history belong to the profile that created them.").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!manager.switchTo(name, &error)) {
        showStatus(error, true);
        return;
    }

    // Applied by restarting, which is the only way to rebuild the per-profile
    // state that open tabs hold.
    const QString program = QCoreApplication::applicationFilePath();
    const QStringList arguments = QCoreApplication::arguments().mid(1);
    QProcess::startDetached(program, arguments);
    QCoreApplication::quit();
}

} // namespace iridium