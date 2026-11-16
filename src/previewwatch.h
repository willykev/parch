#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

// The files a preview's text depends on, besides the text itself: its
// pictures, the files it includes, its bibliography and its citation
// style. They are watched with FileWatch on a thread of their own, so that
// neither starting a watch nor looking at a file after it changed ever
// waits on a file system on the UI thread; fileChanged() is said here, on
// the thread the watch was made on. The thread starts with the first file
// to watch, so that a document that depends on none costs nothing.
class PreviewWatch : public QObject {
    Q_OBJECT
public:
    // No preview watches more files than this: the first so many, in the
    // order given.
    static constexpr int MaxFiles = 256;

    explicit PreviewWatch(QObject *parent = nullptr);
    ~PreviewWatch() override;

    // Watches these paths, full and clean, and no others. A path in Qt's
    // resources never changes and is not watched.
    void setFiles(const QStringList &paths);
    QStringList files() const { return m_files; }

signals:
    // A watched file was changed, replaced, made or removed.
    void fileChanged(const QString &path);

private:
    QThread m_thread;
    // Lives on the thread, with the FileWatch it holds.
    QObject *m_home = nullptr;
    QStringList m_files;
};
