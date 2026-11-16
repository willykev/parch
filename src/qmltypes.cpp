#include "qmltypes.h"

#include "attachments.h"
#include "backend.h"
#include "caretrest.h"
#include "commandfilter.h"
#include "commandregistry.h"
#include "document.h"
#include "documentfind.h"
#include "documentsettingsstore.h"
#include "editorbinding.h"
#include "editordecorations.h"
#include "fileorganizer.h"
#include "focusmode.h"
#include "foldersearch.h"
#include "librarymodel.h"
#include "markdownstyling.h"
#include "outline.h"
#include "preview.h"
#include "scrollsync.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "spellchecker.h"
#include "spellinglayer.h"
#include "tabmodel.h"
#include "theme.h"
#include "vimmode.h"
#include "windowcontext.h"
#include "wordcounter.h"

#include <QQmlEngine>

void registerQmlTypes()
{
    qmlRegisterType<EditorBinding>("Parch", 1, 0, "EditorBinding");
    qmlRegisterType<EditorDecorations>("Parch", 1, 0, "EditorDecorations");
    qmlRegisterType<SpellingLayer>("Parch", 1, 0, "SpellingLayer");
    qmlRegisterType<MarkdownStyling>("Parch", 1, 0, "MarkdownStyling");
    qmlRegisterType<FocusMode>("Parch", 1, 0, "FocusMode");
    qmlRegisterUncreatableType<SpellChecker>("Parch", 1, 0, "SpellChecker", QStringLiteral("The backend keeps spelling"));
    qmlRegisterType<CaretRest>("Parch", 1, 0, "CaretRest");
    qmlRegisterType<CommandFilter>("Parch", 1, 0, "CommandFilter");
    qmlRegisterType<WordCounter>("Parch", 1, 0, "WordCounter");
    qmlRegisterUncreatableType<CommandRegistry>("Parch", 1, 0, "CommandRegistry",
                                                QStringLiteral("The backend keeps the commands"));
    qmlRegisterType<DocumentFind>("Parch", 1, 0, "DocumentFind");
    qmlRegisterType<OutlineModel>("Parch", 1, 0, "OutlineModel");
    // QML reads documents, their buffers and their states by name, but never makes one.
    qmlRegisterUncreatableType<Document>("Parch", 1, 0, "Document", QStringLiteral("Documents come from the store"));
    qmlRegisterUncreatableType<SourceBuffer>("Parch", 1, 0, "SourceBuffer",
                                             QStringLiteral("A buffer belongs to its document"));
    qmlRegisterUncreatableType<Settings>("Parch", 1, 0, "Settings", QStringLiteral("The backend keeps the settings"));
    qmlRegisterUncreatableType<WindowContext>("Parch", 1, 0, "WindowContext",
                                              QStringLiteral("The backend makes one for each window"));
    qmlRegisterUncreatableType<VimMode>("Parch", 1, 0, "VimMode", QStringLiteral("An editor binding has its own"));
    qmlRegisterUncreatableType<TabModel>("Parch", 1, 0, "TabModel", QStringLiteral("A window owns its tabs"));
    qmlRegisterUncreatableType<LibraryModel>("Parch", 1, 0, "LibraryModel", QStringLiteral("A window owns its library"));
    qmlRegisterUncreatableType<FolderSearch>("Parch", 1, 0, "FolderSearch", QStringLiteral("A window owns its search"));
    qmlRegisterUncreatableType<FileOrganizer>("Parch", 1, 0, "FileOrganizer", QStringLiteral("The backend has one"));
    // QML reaches the one backend as a context property, and its choices by name.
    qmlRegisterUncreatableType<Backend>("Parch", 1, 0, "Backend", QStringLiteral("There is one backend"));
    qmlRegisterUncreatableType<DocumentReview>("Parch", 1, 0, "DocumentReview", QStringLiteral("The backend has one"));
    qmlRegisterUncreatableType<Attachments>("Parch", 1, 0, "Attachments", QStringLiteral("The backend has one"));
    // The preview of a view, what it places over its text, and what keeps
    // it and the editor at the same place.
    qmlRegisterType<Preview>("Parch", 1, 0, "Preview");
    qmlRegisterType<ScrollSync>("Parch", 1, 0, "ScrollSync");
    qmlRegisterUncreatableType<PreviewMarks>("Parch", 1, 0, "PreviewMarks", QStringLiteral("A preview has its marks"));
    // QML reaches the one theme as a context property, and hands it to a preview.
    qmlRegisterUncreatableType<Theme>("Parch", 1, 0, "Theme", QStringLiteral("There is one theme"));
    qmlRegisterUncreatableType<DocumentSettingsStore>("Parch", 1, 0, "DocumentSettingsStore",
                                                      QStringLiteral("The backend has one"));
}
