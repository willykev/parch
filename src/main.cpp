#include "apppaths.h"
#include "appservice.h"
#include "backend.h"
#include "benchmark.h"
#include "cli.h"
#include "clicommands.h"
#include "documentsettings.h"
#include "documentstore.h"
#include "portalfilepicker.h"
#include "printportal.h"
#include "qmltypes.h"
#include "quitsignals.h"
#include "remotefetcher.h"
#include "screenshot.h"
#include "settings.h"
#include "theme.h"
#include "themefolder.h"
#include "windowcontext.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStringDecoder>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <csignal>
#include <memory>
#include <optional>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool namesInput(const QString &output, const QStringList &inputs, const AppPaths *settings = nullptr)
{
    struct stat target {};
    const bool exists = ::stat(QFile::encodeName(output).constData(), &target) == 0;
    const auto matches = [&](const QString &input) {
        if (QDir::cleanPath(input) == QDir::cleanPath(output)) return true;
        struct stat source {};
        return exists && ::stat(QFile::encodeName(input).constData(), &source) == 0
               && source.st_dev == target.st_dev && source.st_ino == target.st_ino;
    };
    for (const QString &input : inputs) {
        if (matches(input)) return true;
        if (exists) {
            Dialect::Layers layers;
            if (settings) {
                DocumentSettings::Saved saved;
                QString why;
                if (DocumentSettings::read(*settings, QFileInfo(input).canonicalFilePath(), &saved, &why)) {
                    layers.document = saved.document;
                    layers.library = saved.library;
                    layers.application = saved.application;
                }
            }
            const QStringList dependencies = Benchmark::filesRead(input, layers);
            if (std::any_of(dependencies.cbegin(), dependencies.cend(), matches)) return true;
        }
    }
    return false;
}

// Shows a file selected in its folder, through the file manager's
// org.freedesktop.FileManager1 service, without waiting for its answer.
// Where no file manager answers, the folder that holds the file opens in
// the desktop's handler for folders; the file itself never does.
bool showInFileManager(const QString &path)
{
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    call << QStringList{QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)} << QString();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, [path](QDBusPendingCallWatcher *answered) {
        if (answered->isError()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
        answered->deleteLater();
    });
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // A benchmark counts its launch from here; anything else ignores it.
    const qint64 launched = Benchmark::now();
    // Version, help and usage errors are answered before any GUI application
    // exists so that they work without a display, as in a build check or CI.
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        QStringDecoder decode(QStringDecoder::Utf8, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
        const QString argument = decode(QByteArrayView(argv[i]));
        if (i > 0 && decode.hasError()) {
            std::signal(SIGPIPE, SIG_IGN);
            QTextStream(stderr) << QCoreApplication::translate("main", "parch: argument %1 is not valid UTF-8").arg(i)
                                << Qt::endl;
            return Cli::Io;
        }
        arguments.append(argument);
    }
    const Launch launch = parseLaunch(arguments);
    // Launch holds the positional arguments. Qt receives only the prefix
    // where its own options can occur.
    if (const qsizetype separator = arguments.indexOf(QStringLiteral("--"), 1); separator >= 0) {
        argc = int(separator);
        argv[argc] = nullptr;
    }
    if (launch.kind != Launch::Gui) {
        std::signal(SIGPIPE, SIG_IGN);
        if (launch.kind == Launch::Invalid) {
            QTextStream(stderr) << launch.error << Qt::endl;
            return Cli::Usage;
        }

        // Capture the inherited streams before Qt can reuse a closed descriptor.
        QFile out, err;
        const auto mode = QIODevice::WriteOnly | QIODevice::Unbuffered;
        if (::fcntl(STDOUT_FILENO, F_GETFD) >= 0) out.open(STDOUT_FILENO, mode);
        if (::fcntl(STDERR_FILENO, F_GETFD) >= 0) err.open(STDERR_FILENO, mode);

        int status;
        if (launch.kind == Launch::Command) {
            // Exports set type and draw pictures with Qt's offscreen platform.
            std::unique_ptr<QCoreApplication> application;
            if (launch.commandArguments.value(1) == u"export") {
                qputenv("QT_QPA_PLATFORM", "offscreen");
                qputenv("QT_QPA_PLATFORMTHEME", "generic");
                application = std::make_unique<QGuiApplication>(argc, argv);
            } else {
                application = std::make_unique<QCoreApplication>(argc, argv);
            }
            application->setApplicationName(QStringLiteral("parch"));
            const Cli::Context context{QDir::currentPath(), QDir::homePath(), AppPaths::standard(), &out, &err};
            status = Cli::run(launch.commandArguments, context);
        } else {
            const QString text = launch.kind == Launch::Version ? versionLine() + u'\n' : helpText();
            status = Cli::printResult(text, &out, &err);
        }
        if (out.isOpen() && !out.flush() && status != Cli::Io) return Cli::outputError(&err, out.errorString());
        return status;
    }

    const auto refused = [](const QString &reason) {
        QTextStream(stderr) << QCoreApplication::translate("main", "parch: %1").arg(reason) << Qt::endl;
        return 1;
    };
    AppPaths paths = AppPaths::standard();
    QStringList files = absoluteFiles(launch.files, QDir::currentPath());

    // A picture of the window, which goes where it is asked, in a folder
    // that is there.
    const bool picturing = !launch.picture.file.isEmpty();
    Picture picture = launch.picture;
    if (picturing) {
        picture.file = QDir::current().absoluteFilePath(picture.file);
        if (!QFileInfo(QFileInfo(picture.file).absolutePath()).isDir())
            return refused(QCoreApplication::translate("main", "there is no folder for %1").arg(picture.file));
    }

    // A theme asked for is made to stand in for Omarchy's current one: a
    // folder whose `theme` link points at it, which must outlive the Theme.
    // The person's own shell settings still go over it, as over the current
    // theme. It is settled before anything needs a display, so that a theme
    // that is not there is said even where no window could open. Omarchy's
    // default as it ships, Tokyo Night, is the Theme's own look: it stands
    // in for a folder with no theme, and none of the person's settings go
    // over it.
    QTemporaryDir themeHome;
    QString themeFolder, userShell;
    if (launch.defaultTheme) {
        if (!themeHome.isValid())
            return refused(QCoreApplication::translate("cli", "cannot make a temporary folder for the theme (%1)")
                               .arg(themeHome.errorString()));
        themeFolder = themeHome.path();
    } else if (!launch.themeDirectory.isEmpty()) {
        const QString found = findTheme(launch.themeDirectory);
        if (found.isEmpty())
            return refused(QCoreApplication::translate("cli", "there is no theme %1").arg(launch.themeDirectory));
        if (!themeHome.isValid())
            return refused(QCoreApplication::translate("cli", "cannot make a temporary folder for the theme %1 (%2)")
                               .arg(launch.themeDirectory, themeHome.errorString()));
        QFile theme(found);
        if (!theme.link(themeHome.filePath(QStringLiteral("theme"))))
            return refused(QCoreApplication::translate("cli", "cannot use the theme %1 (%2)")
                               .arg(launch.themeDirectory, theme.errorString()));
        themeFolder = themeHome.path();
        userShell = Theme::userShellFile();
    }

    // A benchmark keeps every file parch writes beneath a temporary folder of
    // its own and opens a copy of its one file there. It never meets a
    // running parch: it neither hands its file over nor listens for others.
    const bool benchmarking = !launch.benchmarkOutput.isEmpty();
    std::optional<QTemporaryDir> scratchRoot;
    QString benchmarkOutput, benchmarkLabel;
    if (benchmarking) {
        if (files.size() != 1)
            return refused(QCoreApplication::translate("main", "--benchmark measures exactly one FILE"));
        benchmarkOutput = QDir::current().absoluteFilePath(launch.benchmarkOutput);
        if (namesInput(benchmarkOutput, files))
            return refused(QCoreApplication::translate("main", "%1 is an input document").arg(benchmarkOutput));
        if (!QFileInfo(QFileInfo(benchmarkOutput).absolutePath()).isDir())
            return refused(QCoreApplication::translate("main", "there is no folder for %1").arg(benchmarkOutput));
        scratchRoot.emplace(QDir::tempPath() + QStringLiteral("/parch-benchmark-XXXXXX"));
        if (!scratchRoot->isValid())
            return refused(QCoreApplication::translate("main", "cannot make a temporary folder to measure in"));
        paths = AppPaths::under(scratchRoot->path());
        // Qt's and the graphics driver's caches go there too, so a measurement
        // leaves nothing in the person's own cache.
        qputenv("XDG_CACHE_HOME", QFile::encodeName(scratchRoot->filePath(QStringLiteral("cache"))));
        benchmarkLabel = QFileInfo(files.first()).completeBaseName();
        QString error;
        const QString copy = Benchmark::copyIn(files.first(), scratchRoot->filePath(QStringLiteral("document")), &error);
        if (copy.isEmpty()) return refused(error);
        files = {copy};
    } else if (picturing) {
        // A picture keeps every file parch writes beneath a temporary folder
        // of its own, which starts with copies of the person's settings, so
        // that the window looks as theirs do and nothing they keep changes.
        // The documents are only read. It never meets a running parch.
        scratchRoot.emplace(QDir::tempPath() + QStringLiteral("/parch-screenshot-XXXXXX"));
        if (!scratchRoot->isValid())
            return refused(QCoreApplication::translate("main", "cannot make a temporary folder to draw in"));
        const AppPaths own = paths;
        paths = AppPaths::under(scratchRoot->path());
        const QList<std::pair<QString, QString>> kept{{own.settingsFile(), paths.settingsFile()},
                                                      {own.documentSettingsFile(), paths.documentSettingsFile()},
                                                      {own.personalDictionaryFile(), paths.personalDictionaryFile()}};
        for (const auto &[from, to] : kept) {
            if (!QFileInfo(from).isFile()) continue;
            QDir().mkpath(QFileInfo(to).absolutePath());
            if (QFile::copy(from, to)) QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
        }
        if (namesInput(picture.file, files, &paths))
            return refused(QCoreApplication::translate("main", "%1 is an input document").arg(picture.file));
        qputenv("XDG_CACHE_HOME", QFile::encodeName(scratchRoot->filePath(QStringLiteral("cache"))));
        // Drawn off screen by the processor, with Qt's plainest platform
        // theme: no window shows, and the picture is the same on any machine.
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
    } else {
        // A parch already running takes the files, in full paths since its
        // working folder is not this one. This is asked before any
        // application object exists, so a launch that hands its files over
        // never opens a display, and it ends once the running instance has
        // taken them.
        QString why;
        if (AppService::forward(paths, files, &why)) {
            // The running parch keeps the theme it started with.
            if (!launch.themeDirectory.isEmpty())
                QTextStream(stderr) << QCoreApplication::translate(
                                           "main", "parch: parch is already running, so --theme %1 does not apply")
                                           .arg(launch.themeDirectory)
                                    << Qt::endl;
            if (launch.defaultTheme)
                QTextStream(stderr) << QCoreApplication::translate(
                                           "main", "parch: parch is already running, so --default-theme does not apply")
                                    << Qt::endl;
            return 0;
        }
        // An instance answered and did not take them.
        if (!why.isEmpty()) return refused(why);
    }

    QGuiApplication app(argc, argv);
    app.setDesktopFileName(QStringLiteral("parch"));
    app.setApplicationName(QStringLiteral("parch"));
    if (picturing) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        // In a picture the caret is lit, never caught between two blinks.
        QGuiApplication::styleHints()->setCursorFlashTime(0);
    }

    AppService service(paths);
    if (!benchmarking && !picturing && !service.listen()) {
        QString why;
        if (!service.error().isEmpty()) return refused(service.error());
        // Another launch has begun listening since the first try.
        if (AppService::forward(paths, files, &why)) return 0;
        return refused(why.isEmpty() ? QCoreApplication::translate("main", "parch is already running but did not answer")
                                     : why);
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
    // A save still running then has its text in recovery, and waiting for it
    // as parch ends could take for ever, so parch leaves at once, as at the
    // end of the session, once an export has stopped and the files' ids are
    // written. Other work ends as the store and the exports are deleted.
    QObject::connect(&backend, &Backend::quitRequested, &app, [&backend] {
        if (backend.store()->savesRunning()) {
            backend.endJobsBeforeLeaving(Backend::EndWaitMs);
            ::_exit(0);
        }
        QCoreApplication::exit(0);
    }, Qt::QueuedConnection);
    // The desktop's own open and save dialogs. A benchmark asks nothing.
    std::optional<PortalFilePicker> picker;
    // The desktop's print dialog and printing.
    std::optional<PortalPrinter> printer;
    // What a preview hands on: links to the desktop's handler for them, and
    // the pictures a document names by web address to the network. A
    // benchmark asks for none of them, and nor does a picture, which reaches
    // nothing beyond the files it shows.
    std::optional<NetworkFetcher> fetcher;
    if (!benchmarking && !picturing) {
        picker.emplace();
        backend.setFilePicker(&*picker);
        printer.emplace();
        backend.setPrinter(&*printer);
        backend.setLinkOpener([](const QUrl &url) { return QDesktopServices::openUrl(url); });
        backend.setFolderShower(showInFileManager);
        fetcher.emplace();
        backend.setRemoteFetcher(&*fetcher);
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
    // whatever the session around it, so that its runs compare (beside its
    // preview the text keeps a column that wide); a picture's has the size
    // asked for, and keeps it.
    QVariantMap initial;
    const QSize fixed = benchmarking ? QSize(launch.benchmarkView == QLatin1String("split") ? 1800 : 1000, 800)
                                     : picture.size;
    if (fixed.isValid()) {
        for (const char *size : {"width", "minimumWidth", "maximumWidth"}) initial.insert(QLatin1String(size), fixed.width());
        for (const char *size : {"height", "minimumHeight", "maximumHeight"}) initial.insert(QLatin1String(size), fixed.height());
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
        benchmark->setOptions({launch.benchmarkView, launch.benchmarkWindows, launch.benchmarkAdversarial});
        benchmark->setBackend(&backend);
        benchmark->setLaunchTime(launched);
        benchmark->opening();
    }
    backend.start(files);
    if (windows.isEmpty())
        return 1;
    if (picturing) {
        // Ends by itself, once the picture is saved or cannot be taken.
        Screenshot screenshot(picture, &backend);
        QObject::connect(&screenshot, &Screenshot::finished, &app, [](int code, const QString &message) {
            if (code != 0)
                QTextStream(stderr) << QCoreApplication::translate("main", "parch: --screenshot: %1").arg(message) << Qt::endl;
            QCoreApplication::exit(code);
        });
        screenshot.start(windows.value(backend.windows().constFirst()));
        return app.exec();
    }
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
