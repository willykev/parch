#pragma once

#include "commandregistry.h"

#include <QAbstractListModel>
#include <QList>
#include <QPointer>
#include <QString>

// The commands of a registry whose title matches what was typed, the best
// match first, for the command palette and the shortcuts dialog. A match
// takes the typed letters in order, not necessarily side by side, so "nwd"
// finds "New window" and "New draft"; letters that start words, follow
// each other or start the title count most, and a title that holds the
// typed text as it is counts more still. A command's category is searched
// too, after its title. With nothing typed every command is listed, in the
// registry's order. Its rows have the registry's roles.
class CommandFilter : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(CommandRegistry *registry READ registry WRITE setRegistry NOTIFY registryChanged)
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit CommandFilter(QObject *parent = nullptr);

    CommandRegistry *registry() const { return m_registry; }
    void setRegistry(CommandRegistry *registry);
    QString text() const { return m_text; }
    void setText(const QString &text);
    int count() const { return int(m_rows.size()); }
    // The command id of a row, or empty.
    Q_INVOKABLE QString idAt(int row) const;
    // The row of a command, or -1.
    Q_INVOKABLE int rowOf(const QString &id) const;

    // How well the typed text matches a candidate: higher is better, and
    // negative is no match.
    static int score(const QString &typed, const QString &candidate);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void registryChanged();
    void textChanged();
    void countChanged();

private:
    void rebuild();
    void forward(const QModelIndex &first, const QModelIndex &last, const QList<int> &roles);

    QPointer<CommandRegistry> m_registry;
    QString m_text;
    // The registry's rows, in the order shown.
    QList<int> m_rows;
    QList<QMetaObject::Connection> m_connections;
};
