#pragma once

#include <QByteArray>
#include <QString>
#include <memory>

class QSemaphore;
class QThreadPool;

// Occupies every worker until released, without holding the test's thread.
class PoolBlock {
public:
    explicit PoolBlock(QThreadPool *pool = nullptr);
    ~PoolBlock();
    void release();

private:
    std::shared_ptr<QSemaphore> m_gate;
    int m_count = 0;
};

// Files and theme folders the tests write, always beneath a temporary folder.

// Writes a file, making its folder first.
void writeFile(const QString &path, const QByteArray &contents);
// The file's bytes, or nothing when it cannot be read.
QByteArray readFile(const QString &path);

// Responsiveness limits in tests, scaled by PARCH_TEST_TIME_FACTOR (1 when
// unset): a shared build machine is no measure of a desktop, so CI may
// loosen them while a run on the desktop keeps them as written.
double testTimeFactor();
inline double timeLimit(double ms) { return ms * testTimeFactor(); }
// Omarchy keeps the active theme in <dir>/theme/colors.toml.
void writeTheme(const QString &dir, const QString &mode, const QString &background, const QString &foreground,
                const QString &accent);
// A theme switch swaps the whole theme directory, as Omarchy does.
void replaceTheme(const QString &dir, const QString &mode, const QString &background, const QString &foreground,
                  const QString &accent);
