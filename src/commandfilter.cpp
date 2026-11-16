#include "commandfilter.h"

#include <algorithm>

CommandFilter::CommandFilter(QObject *parent) : QAbstractListModel(parent) {}

void CommandFilter::setRegistry(CommandRegistry *registry)
{
    if (registry == m_registry) return;
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    m_registry = registry;
    if (registry) {
        m_connections = {
            connect(registry, &QAbstractItemModel::dataChanged, this, &CommandFilter::forward),
            connect(registry, &QAbstractItemModel::rowsInserted, this, &CommandFilter::rebuild),
            connect(registry, &QAbstractItemModel::rowsRemoved, this, &CommandFilter::rebuild),
            connect(registry, &QAbstractItemModel::modelReset, this, &CommandFilter::rebuild),
        };
    }
    rebuild();
    emit registryChanged();
}

void CommandFilter::setText(const QString &text)
{
    if (text == m_text) return;
    m_text = text;
    rebuild();
    emit textChanged();
}

int CommandFilter::score(const QString &typed, const QString &candidate)
{
    const QString query = typed.simplified().toCaseFolded();
    if (query.isEmpty()) return 0;
    const QString title = candidate.toCaseFolded();
    // The typed text as it is, best at the start or at a word's start.
    if (const qsizetype at = title.indexOf(query); at >= 0) {
        const bool wordStart = at == 0 || !title.at(at - 1).isLetterOrNumber();
        return 1000 + (at == 0 ? 200 : 0) + (wordStart ? 100 : 0) - int(at) - int(title.size());
    }
    // Otherwise each typed letter in order, spaces left out.
    int score = 0;
    qsizetype from = 0, last = -2;
    for (const QChar c : query) {
        if (c.isSpace()) continue;
        // A letter that starts a word is taken before an earlier one inside a word.
        qsizetype found = -1;
        for (qsizetype i = from; i < title.size(); ++i) {
            if (title.at(i) != c) continue;
            const bool starts = i == 0 || !title.at(i - 1).isLetterOrNumber();
            if (found < 0) found = i;
            if (starts || i == last + 1) {
                found = i;
                break;
            }
        }
        if (found < 0) return -1;
        const bool starts = found == 0 || !title.at(found - 1).isLetterOrNumber();
        if (found == 0) score += 40;
        else if (starts) score += 30;
        else if (found == last + 1) score += 20;
        else score += 5 - int(qMin<qsizetype>(found - last - 1, 5));
        last = found;
        from = found + 1;
    }
    return score - int(title.size());
}

void CommandFilter::rebuild()
{
    beginResetModel();
    m_rows.clear();
    if (m_registry) {
        QList<std::pair<int, int>> scored;
        const int rows = m_registry->rowCount();
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = m_registry->index(row);
            int best = score(m_text, index.data(CommandRegistry::TitleRole).toString());
            if (best < 0) {
                // The category and title together, below any match of the title alone.
                const int both = score(m_text, index.data(CommandRegistry::CategoryRole).toString() + u' '
                                                   + index.data(CommandRegistry::TitleRole).toString());
                if (both >= 0) best = both - 2000;
                else continue;
            }
            scored.append({best, row});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &[best, row] : std::as_const(scored)) m_rows.append(row);
    }
    endResetModel();
    emit countChanged();
}

void CommandFilter::forward(const QModelIndex &first, const QModelIndex &last, const QList<int> &roles)
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (m_rows[i] >= first.row() && m_rows[i] <= last.row()) emit dataChanged(index(i), index(i), roles);
}

QString CommandFilter::idAt(int row) const
{
    if (row < 0 || row >= m_rows.size() || !m_registry) return {};
    return m_registry->index(m_rows[row]).data(CommandRegistry::IdRole).toString();
}

int CommandFilter::rowOf(const QString &id) const
{
    for (int i = 0; i < m_rows.size(); ++i)
        if (idAt(i) == id) return i;
    return -1;
}

int CommandFilter::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }

QVariant CommandFilter::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size() || !m_registry) return {};
    return m_registry->index(m_rows[index.row()]).data(role);
}

QHash<int, QByteArray> CommandFilter::roleNames() const { return CommandRegistry::commandRoles(); }
