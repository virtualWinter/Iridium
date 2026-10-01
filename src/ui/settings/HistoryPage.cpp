#include "ui/settings/HistoryPage.hpp"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace iridium::history {

namespace {

// Row data travels in the item: the URL is needed to open or forget an entry.
constexpr int kUrlRole = Qt::UserRole + 100;

// What a user sees for a visit, grouped the way a history list usually reads.
QString describeWhen(const QDateTime& when)
{
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 seconds = when.secsTo(now);
    if (seconds < 60)
        return QObject::tr("just now");
    if (seconds < 3600)
        return QObject::tr("%1 minutes ago").arg(seconds / 60);
    if (when.date() == now.date())
        return QObject::tr("today at %1").arg(when.toString(QStringLiteral("HH:mm")));
    if (when.date() == now.date().addDays(-1))
        return QObject::tr("yesterday at %1").arg(when.toString(QStringLiteral("HH:mm")));
    if (when.date().year() == now.date().year())
        return when.toString(QStringLiteral("d MMM, HH:mm"));
    return when.toString(QStringLiteral("d MMM yyyy"));
}

} // namespace

HistoryPage::HistoryPage(HistoryStore& store, OpenUrlHandler openUrl, QWidget* parent)
    : QWidget(parent)
    , m_store(store)
    , m_openUrl(std::move(openUrl))
{
    setObjectName(QStringLiteral("historyPage"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    auto* heading = new QLabel(tr("History"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("settingsSearch"));
    m_search->setPlaceholderText(tr("Search history"));
    m_search->setClearButtonEnabled(true);
    toolbar->addWidget(m_search, 1);
    root->addLayout(toolbar);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("settingsSubtitle"));
    root->addWidget(m_summary);

    m_body = new QStackedWidget(this);

    m_list = new QListWidget(m_body);
    m_list->setObjectName(QStringLiteral("extensionsList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_body->addWidget(m_list);

    m_empty = new QLabel(m_body);
    m_empty->setObjectName(QStringLiteral("settingsEmpty"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_body->addWidget(m_empty);

    root->addWidget(m_body, 1);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);

    m_open = new QPushButton(tr("Open"), this);
    m_open->setObjectName(QStringLiteral("settingsButton"));
    actions->addWidget(m_open);
    connect(m_open, &QPushButton::clicked, this, &HistoryPage::openSelected);

    m_forget = new QPushButton(tr("Forget"), this);
    m_forget->setObjectName(QStringLiteral("settingsButton"));
    m_forget->setToolTip(tr("Remove the selected entry from history"));
    actions->addWidget(m_forget);
    connect(m_forget, &QPushButton::clicked, this, &HistoryPage::forgetSelected);

    actions->addStretch(1);

    auto* olderButton = new QPushButton(tr("Delete older than 30 days"), this);
    olderButton->setObjectName(QStringLiteral("settingsButton"));
    actions->addWidget(olderButton);
    connect(olderButton, &QPushButton::clicked, this,
        [this] { clearOlderThan(30); });

    auto* clearButton = new QPushButton(tr("Clear all"), this);
    clearButton->setObjectName(QStringLiteral("settingsButton"));
    actions->addWidget(clearButton);
    connect(clearButton, &QPushButton::clicked, this, &HistoryPage::clearAll);

    root->addLayout(actions);

    auto* note = new QLabel(tr("History belongs to this profile and is never "
                               "synced anywhere."), this);
    note->setObjectName(QStringLiteral("settingsFootnote"));
    note->setWordWrap(true);
    root->addWidget(note);

    connect(m_list, &QListWidget::itemActivated, this, &HistoryPage::openSelected);
    connect(m_list, &QListWidget::currentItemChanged,
        this, &HistoryPage::onSelectionChanged);

    // Typing filters as the user goes, but not on every keystroke: the store is
    // queried per search and a debounce keeps that off the typing path.
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, debounce,
        [debounce] { debounce->start(); });
    connect(debounce, &QTimer::timeout, this, &HistoryPage::refresh);

    refresh();
}

void HistoryPage::refresh()
{
    // Keep the selection across a reload so refining a search does not lose it.
    const QString selectedUrl = m_list->currentItem()
        ? m_list->currentItem()->data(kUrlRole).toString() : QString();

    Query query;
    query.text = m_search->text().trimmed();
    query.limit = 500;

    QString error;
    const std::vector<Visit> visits = m_store.search(query, &error);
    if (!error.isEmpty()) {
        m_summary->setText(tr("History could not be read: %1").arg(error));
        return;
    }

    m_list->clear();
    QListWidgetItem* restore = nullptr;
    for (const Visit& visit : visits) {
        const QString title = visit.title.isEmpty() ? visit.url : visit.title;
        auto* item = new QListWidgetItem(
            QStringLiteral("%1\n%2  •  %3 visits  •  %4")
                .arg(title, visit.url).arg(visit.visitCount).arg(describeWhen(visit.lastVisit)),
            m_list);
        item->setData(kUrlRole, visit.url);
        item->setToolTip(visit.url);
        if (visit.url == selectedUrl)
            restore = item;
    }
    if (restore)
        m_list->setCurrentItem(restore);

    showCount();

    const bool hasResults = !visits.empty();
    m_body->setCurrentIndex(hasResults ? 0 : 1);
    if (!hasResults) {
        m_empty->setText(query.text.isEmpty()
            ? tr("Nothing has been visited in this profile yet.")
            : tr("No history entry matches \"%1\".").arg(query.text));
    }
    onSelectionChanged();
}

void HistoryPage::showCount()
{
    const int total = m_store.entryCount();
    const QString search = m_search->text().trimmed();
    if (search.isEmpty()) {
        m_summary->setText(total == 1 ? tr("1 entry") : tr("%1 entries").arg(total));
    } else {
        const int shown = m_list->count();
        m_summary->setText(tr("Showing %1 of %2 entries").arg(shown).arg(total));
    }
}

void HistoryPage::onSelectionChanged()
{
    // Both act on the selected row, so they are meaningless without one.
    const bool have = m_list->currentItem() != nullptr;
    m_open->setEnabled(have);
    m_forget->setEnabled(have);
}

void HistoryPage::openSelected()
{
    if (!m_list->currentItem())
        return;
    const QString url = m_list->currentItem()->data(kUrlRole).toString();
    if (url.isEmpty() || !m_openUrl)
        return;
    m_openUrl(url);
}

void HistoryPage::forgetSelected()
{
    if (!m_list->currentItem())
        return;
    const QString url = m_list->currentItem()->data(kUrlRole).toString();
    QString error;
    if (!m_store.forget(url, &error)) {
        m_summary->setText(tr("Could not remove the entry: %1").arg(error));
        return;
    }
    refresh();
}

void HistoryPage::clearOlderThan(int days)
{
    if (days <= 0)
        return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    const int affected = m_store.countOlderThan(cutoff);
    if (affected == 0) {
        m_summary->setText(tr("Nothing older than %1 days.").arg(days));
        return;
    }

    const auto answer = QMessageBox::question(this, tr("Delete history"),
        tr("Delete %n entry/entries older than %1 days?", "", affected).arg(days),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_store.clear(cutoff, &error)) {
        m_summary->setText(tr("Could not delete history: %1").arg(error));
        return;
    }
    refresh();
}

void HistoryPage::clearAll()
{
    const int total = m_store.entryCount();
    if (total == 0)
        return;

    const auto answer = QMessageBox::question(this, tr("Clear history"),
        tr("Delete all %n entries in this profile's history?", "", total),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString error;
    if (!m_store.clear(std::nullopt, &error)) {
        m_summary->setText(tr("Could not clear history: %1").arg(error));
        return;
    }
    refresh();
}

} // namespace iridium::history