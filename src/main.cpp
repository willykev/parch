#include "apppaths.h"
#include "appservice.h"
#include "backend.h"
#include "benchmark.h"
#include "cli.h"
#include "portalfilepicker.h"
#include "qmltypes.h"
#include "quitsignals.h"
#include "settings.h"
#include "theme.h"
#include "themefolder.h"
#include "windowcontext.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <optional>
#include <unistd.h>

int main(int argc, char *argv[])
{
    // A benchmark counts its launch from here; anything else ignores it.
    const qint64 launched = Benchmark::now();
    // Version, help and usage errors are answered before any GUI application
    // exists so that they work without a display, as in a build check or CI.
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments.append(QString::fromLocal8Bit(argv[i]));
    const Launch launch = parseLaunch(arguments);
    switch (launch.kind) {
    case Launch::Version:
        QTextStream(stdout) << versionLine() << Qt::endl;
        return 0;
    case Launch::Help:
        QTextStream(stdout) << helpText();
        return 0;
    case Launch::Invalid:
        QTextStream(stderr) << launch.error << Qt::endl;
        return 1;
    case Launch::Gui:
        break;
    }

    const auto refused = [](const QString &reason) {
        QTextStream(stderr) << QCoreApplication::translate("main", "parch: %1").arg(reason) << Qt::endl;
        return 1;
    };
    AppPaths paths = AppPaths::standard();
    QStringList files = absoluteFiles(launch.files, QDir::currentPath());

    // A benchmark keeps every file parch writes beneath a temporary folder of
    // its own and opens a copy of its one file there. It never meets a
    // running parch: it neither hands its file over nor listens for others.
    const bool benchmarking = !launch.benchmarkOutput.isEmpty();
    std::optional<QTemporaryDir> benchmarkRoot;
    QString benchmarkOutput, benchmarkLabel;
    if (benchmarking) {
        if (files.size() != 1)
            return refused(QCoreApplication::translate("main", "--benchmark measures exactly one FILE"));
        benchmarkOutput = QDir::current().absoluteFilePath(launch.benchmarkOutput);
        if (!QFileInfo(QFileInfo(benchmarkOutput).absolutePath()).isDir())
            return refused(QCoreApplication::translate("main", "there is no folder for %1").arg(benchmarkOutput));
        benchmarkRoot.emplace(QDir::tempPath() + QStringLiteral("/parch-benchmark-XXXXXX"));
        if (!benchmarkRoot->isValid())
            return refused(QCoreApplication::translate("main", "cannot make a temporary folder to measure in"));
        paths = AppPaths::under(benchmarkRoot->path());
        benchmarkLabel = QFileInfo(files.first()).completeBaseName();
        QString error;
        const QString copy = Benchmark::copyIn(files.first(), benchmarkRoot->filePath(QStringLiteral("document")), &error);
        if (copy.isEmpty()) return refused(error);
        files = {copy};
    } else {
        // A parch already running takes the files, in full paths since its
        // working folder is not this one. This is asked before any
        // application object exists, so a launch that hands its files over
        // never opens a display, and it ends once the running instance has
        // taken them.
        QString why;
        if (AppService::forward(paths, files, &why)) return 0;
        // An instance answered and did not take them.
        if (!why.isEmpty()) return refused(why);
    }

    QGuiApplication app(argc, argv);
    app.setDesktopFileName(QStringLiteral("parch"));
    app.setApplicationName(QStringLiteral("parch"));

    AppService service(paths);
    if (!benchmarking && !service.listen()) {
        QString why;
        if (!service.error().isEmpty()) return refused(service.error());
        // Another launch has begun listening since the first try.
        if (AppService::forward(paths, files, &why)) return 0;
        return refused(why.isEmpty() ? QCoreApplication::translate("main", "parch is already running but did not answer")
                                     : why);
    }

    // A theme asked for is made to stand in for Omarchy's current one: a
    // folder whose `theme` link points at it, which must outlive the Theme.
    // The person's own shell settings still go over it, as over the current
    // theme.
    QTemporaryDir themeHome;
    QString themeFolder, userShell;
    if (!launch.themeDirectory.isEmpty()) {
        const QString found = findTheme(launch.themeDirectory);
        if (found.isEmpty() || !themeHome.isValid() || !QFile::link(found, themeHome.filePath("theme"))) {
            QTextStream(stderr) << QCoreApplication::translate("cli", "parch: there is no theme %1")
                                       .arg(launch.themeDirectory)
                                << Qt::endl;
            return 1;
        }
        themeFolder = themeHome.path();
        userShell = Theme::userShellFile();
    }
    // The theme exists only on this path because it needs the application
    // object for its watcher and processes.
    Theme theme(themeFolder, nullptr, {}, userShell);
    // "monospace" is the alias `omarchy font set` points at the chosen
    // family; the size follows the shell's own text size as it changes.
    auto adoptFont = [&theme] { QGuiApplication::setFont(theme.font()); };
    adoptFont();
    QObject::connect(&theme, &Theme::changed, &app, adoptFont);

    // The plainest style: every control is drawn by parch's own components
    // in the colours and states of the Omarchy theme.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    registerQmlTypes();

    // The backend outlives the engine and its windows.
    Backend backend(paths);
    backend.settings()->followTheme(&theme);
    // The end of the session, or a service manager stopping parch, ends it
    // as quitting does, except that nothing can be refused (QuitSignals).
    std::optional<QSocketNotifier> quitSignals;
    if (const int heard = QuitSignals::install(); heard >= 0) {
        quitSignals.emplace(heard, QSocketNotifier::Read);
        QObject::connect(&*quitSignals, &QSocketNotifier::activated, &app, [&, heard] {
            quitSignals->setEnabled(false);
            unsigned char byte = 0;
            while (::read(heard, &byte, 1) > 0) {
            }
            if (QuitSignals::end(backend, service, Backend::EndWaitMs)) QCoreApplication::exit(0);
        });
    }
    // Only the backend decides that quitting is safe; it asks once it is. From
    // then on no request is taken, so a later launch starts its own instance
    // rather than handing its files to one that is ending. The event loop is
    // left with exit(), not quit(): quit() first asks every window to close,
    // and each refuses, since closing goes through the backend, which would
    // ask to quit again, for ever.
    QObject::connect(&backend, &Backend::quitRequested, &service, &AppService::close);
    QObject::connect(&backend, &Backend::quitRequested, &app, [] { QCoreApplication::exit(0); }, Qt::QueuedConnection);
    // The desktop's own open and save dialogs. A benchmark asks nothing.
    std::optional<PortalFilePicker> picker;
    if (!benchmarking) {
        picker.emplace();
        backend.setFilePicker(&*picker);
    }

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("settings"), backend.settings());
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/Main.qml")));

    // One window for each of the backend's, with its context.
    QHash<WindowContext *, QQuickWindow *> windows;
    const auto closeAll = qScopeGuard([&windows] {
        // Windows closed just before the end are deleted too, before the engine.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qDeleteAll(std::exchange(windows, {}));
    });
    // A benchmark's window has the size of the whole-document measurement
    // whatever the session around it, so that its runs compare.
    QVariantMap initial;
    if (benchmarking) {
        for (const char *size : {"width", "minimumWidth", "maximumWidth"}) initial.insert(QLatin1String(size), 1000);
        for (const char *size : {"height", "minimumHeight", "maximumHeight"}) initial.insert(QLatin1String(size), 800);
    }
    QObject::connect(&backend, &Backend::windowAdded, &app, [&](WindowContext *context) {
        QVariantMap properties = initial;
        properties.insert(QStringLiteral("context"), QVariant::fromValue(context));
        QObject *made = component.createWithInitialProperties(properties);
        auto *window = qobject_cast<QQuickWindow *>(made);
        if (!window) {
            delete made;
            QTextStream(stderr) << component.errorString() << Qt::endl;
            return;
        }
        windows.insert(context, window);
    });
    QObject::connect(&backend, &Backend::windowRemoved, &app, [&windows](WindowContext *context) {
        // The window may be the one whose close started this.
        if (QQuickWindow *window = windows.take(context)) window->deleteLater();
    });
    std::unique_ptr<Benchmark> benchmark;
    if (benchmarking) {
        benchmark = std::make_unique<Benchmark>(benchmarkOutput, benchmarkLabel);
        benchmark->setLaunchTime(launched);
        benchmark->opening();
    }
    backend.start(files);
    if (windows.isEmpty())
        return 1;
    if (!benchmark) {
        QObject::connect(&service, &AppService::openRequested, &backend, &Backend::openForwarded);
        return app.exec();
    }

    // A benchmark ends by itself: once its results are written it quits as
    // parch always does, saving its copy, and a failure ends it at once.
    QObject::connect(benchmark.get(), &Benchmark::finished, &app, [&](int code, const QString &error) {
        if (code != 0) {
            QTextStream(stderr) << QCoreApplication::translate("main", "parch: benchmark: %1").arg(error) << Qt::endl;
            QCoreApplication::exit(code);
            return;
        }
        backend.quit();
        if (!backend.problem().isEmpty()) {
            QTextStream(stderr) << QCoreApplication::translate("main", "parch: benchmark: %1: %2")
                                       .arg(backend.problem(), backend.problemDetail())
                                << Qt::endl;
            QCoreApplication::exit(4);
            return;
        }
        QTimer::singleShot(30000, &app, [] {
            QTextStream(stderr) << QCoreApplication::translate("main", "parch: benchmark: parch did not quit") << Qt::endl;
            QCoreApplication::exit(4);
        });
    });
    benchmark->start(windows.value(backend.windows().constFirst()));
    return app.exec();
}
