#include "previewwatch.h"

#include "filewatch.h"

#include <QMetaObject>
#include <QSet>

PreviewWatch::PreviewWatch(QObject *parent) : QObject(parent) {}

PreviewWatch::~PreviewWatch()
{
    if (!m_home) return;
    // The watch goes on its own thread, after anything asked of it before,
    // and the thread ends with it.
    QMetaObject::invokeMethod(m_home, [home = m_home] {
        delete home;
        QThread::currentThread()->quit();
    });
    m_thread.wait();
}

void PreviewWatch::setFiles(const QStringList &paths)
{
    QStringList wanted;
    QSet<QString> seen;
    for (const QString &path : paths) {
        if (wanted.size() >= MaxFiles) break;
        if (path.isEmpty() || path.startsWith(u':') || !path.startsWith(u'/') || seen.contains(path)) continue;
        seen.insert(path);
        wanted.append(path);
    }
    if (wanted == m_files) return;
    const QSet<QString> before(m_files.cbegin(), m_files.cend());
    QStringList added, removed;
    for (const QString &path : std::as_const(wanted))
        if (!before.contains(path)) added.append(path);
    for (const QString &path : std::as_const(m_files))
        if (!seen.contains(path)) removed.append(path);
    m_files = wanted;
    if (!m_home) {
        if (added.isEmpty()) return;
        m_thread.setObjectName(QStringLiteral("parch preview watch"));
        m_thread.start();
        m_home = new QObject;
        m_home->moveToThread(&m_thread);
        QMetaObject::invokeMethod(m_home, [this, home = m_home] {
            auto *watch = new FileWatch(home);
            // Said on the thread this was made on.
            connect(watch, &FileWatch::changedOnDisk, this, &PreviewWatch::fileChanged);
            connect(watch, &FileWatch::removedFromDisk, this, &PreviewWatch::fileChanged);
        });
    }
    QMetaObject::invokeMethod(m_home, [home = m_home, added, removed] {
        auto *watch = home->findChild<FileWatch *>(Qt::FindDirectChildrenOnly);
        if (!watch) return;
        for (const QString &path : removed) watch->unwatch(path);
        for (const QString &path : added) watch->watch(path);
    });
}
