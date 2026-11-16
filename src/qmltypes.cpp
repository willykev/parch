#include "qmltypes.h"

#include "attachments.h"
#include "backend.h"
#include "caretrest.h"
#include "document.h"
#include "editorbinding.h"
#include "librarymodel.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "windowcontext.h"

#include <QQmlEngine>

void registerQmlTypes()
{
    qmlRegisterType<EditorBinding>("Parch", 1, 0, "EditorBinding");
    qmlRegisterType<CaretRest>("Parch", 1, 0, "CaretRest");
    // QML reads documents, their buffers and their states by name, but never makes one.
    qmlRegisterUncreatableType<Document>("Parch", 1, 0, "Document", QStringLiteral("Documents come from the store"));
    qmlRegisterUncreatableType<SourceBuffer>("Parch", 1, 0, "SourceBuffer",
                                             QStringLiteral("A buffer belongs to its document"));
    qmlRegisterUncreatableType<Settings>("Parch", 1, 0, "Settings", QStringLiteral("The backend keeps the settings"));
    qmlRegisterUncreatableType<WindowContext>("Parch", 1, 0, "WindowContext",
                                              QStringLiteral("The backend makes one for each window"));
    qmlRegisterUncreatableType<TabModel>("Parch", 1, 0, "TabModel", QStringLiteral("A window owns its tabs"));
    qmlRegisterUncreatableType<LibraryModel>("Parch", 1, 0, "LibraryModel", QStringLiteral("A window owns its library"));
    // QML reaches the one backend as a context property, and its choices by name.
    qmlRegisterUncreatableType<Backend>("Parch", 1, 0, "Backend", QStringLiteral("There is one backend"));
    qmlRegisterUncreatableType<DocumentReview>("Parch", 1, 0, "DocumentReview", QStringLiteral("The backend has one"));
    qmlRegisterUncreatableType<Attachments>("Parch", 1, 0, "Attachments", QStringLiteral("The backend has one"));
}
