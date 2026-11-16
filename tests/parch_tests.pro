QT += core gui qml quick quickcontrols2 testlib dbus svg concurrent network
CONFIG += c++20 testcase
TARGET = parch_tests
TEMPLATE = app

# The tests report the real version so that the version line can be checked.
VERSION = $$cat($$PWD/../VERSION)
DEFINES += PARCH_VERSION=\\\"$$VERSION\\\"
# Tests read repository files by absolute path because the build directory
# is not at a fixed depth below the repository root.
DEFINES += PARCH_SOURCE_DIR=\\\"$$clean_path($$PWD/..)\\\"

INCLUDEPATH += ../src

HEADERS += \
    editingtests.h \
    testfiles.h \
    windowtesting.h \
    ../src/apppaths.h \
    ../src/appservice.h \
    ../src/attachments.h \
    ../src/autosave.h \
    ../src/backend.h \
    ../src/benchmark.h \
    ../src/caretrest.h \
    ../src/cli.h \
    ../src/commandfilter.h \
    ../src/commandregistry.h \
    ../src/diskfile.h \
    ../src/document.h \
    ../src/documentfind.h \
    ../src/documentreview.h \
    ../src/documentstore.h \
    ../src/editorbinding.h \
    ../src/fileorganizer.h \
    ../src/filepicker.h \
    ../src/filerequests.h \
    ../src/filewatch.h \
    ../src/foldersearch.h \
    ../src/history.h \
    ../src/htmlmarkdown.h \
    ../src/hyprland.h \
    ../src/librarymodel.h \
    ../src/linktargets.h \
    ../src/markdownediting.h \
    ../src/librarywalk.h \
    ../src/markdownlines.h \
    ../src/outline.h \
    ../src/pathtext.h \
    ../src/portalfilepicker.h \
    ../src/qmltypes.h \
    ../src/quitsignals.h \
    ../src/readingpositions.h \
    ../src/recentfiles.h \
    ../src/recovery.h \
    ../src/references.h \
    ../src/session.h \
    ../src/settings.h \
    ../src/sourceboundaries.h \
    ../src/sourcebuffer.h \
    ../src/sourcetypes.h \
    ../src/tabmodel.h \
    ../src/textsearch.h \
    ../src/theme.h \
    ../src/themefolder.h \
    ../src/windowcontext.h \
    ../src/wordcounter.h

# Every source except main.cpp is compiled in, since the test provides its own main.
SOURCES += \
    parch_tests.cpp \
    editingtests.cpp \
    testfiles.cpp \
    windowtesting.cpp \
    ../src/apppaths.cpp \
    ../src/appservice.cpp \
    ../src/attachments.cpp \
    ../src/autosave.cpp \
    ../src/backend.cpp \
    ../src/benchmark.cpp \
    ../src/caretrest.cpp \
    ../src/cli.cpp \
    ../src/commandfilter.cpp \
    ../src/commandregistry.cpp \
    ../src/diskfile.cpp \
    ../src/document.cpp \
    ../src/documentfind.cpp \
    ../src/documentreview.cpp \
    ../src/documentstore.cpp \
    ../src/editorbinding.cpp \
    ../src/editorpaste.cpp \
    ../src/fileorganizer.cpp \
    ../src/filerequests.cpp \
    ../src/filewatch.cpp \
    ../src/foldersearch.cpp \
    ../src/history.cpp \
    ../src/htmlmarkdown.cpp \
    ../src/hyprland.cpp \
    ../src/librarymodel.cpp \
    ../src/linktargets.cpp \
    ../src/markdownediting.cpp \
    ../src/librarywalk.cpp \
    ../src/markdownlines.cpp \
    ../src/outline.cpp \
    ../src/pathtext.cpp \
    ../src/portalfilepicker.cpp \
    ../src/qmltypes.cpp \
    ../src/quitsignals.cpp \
    ../src/readingpositions.cpp \
    ../src/recentfiles.cpp \
    ../src/recovery.cpp \
    ../src/references.cpp \
    ../src/session.cpp \
    ../src/settings.cpp \
    ../src/sourceboundaries.cpp \
    ../src/sourcebuffer.cpp \
    ../src/tabmodel.cpp \
    ../src/textsearch.cpp \
    ../src/theme.cpp \
    ../src/windowcontext.cpp \
    ../src/wordcounter.cpp

RESOURCES += ../src/resources.qrc
