QT += core gui qml quick quickcontrols2 dbus svg concurrent network

CONFIG += c++20 release
TARGET = parch
TEMPLATE = app

VERSION = $$cat($$PWD/VERSION)
DEFINES += PARCH_VERSION=\\\"$$VERSION\\\"

HEADERS += \
    src/apppaths.h \
    src/appservice.h \
    src/autosave.h \
    src/backend.h \
    src/benchmark.h \
    src/caretrest.h \
    src/cli.h \
    src/diskfile.h \
    src/document.h \
    src/documentreview.h \
    src/documentstore.h \
    src/editorbinding.h \
    src/filepicker.h \
    src/filerequests.h \
    src/filewatch.h \
    src/history.h \
    src/hyprland.h \
    src/librarymodel.h \
    src/pathtext.h \
    src/portalfilepicker.h \
    src/qmltypes.h \
    src/quitsignals.h \
    src/recentfiles.h \
    src/recovery.h \
    src/session.h \
    src/settings.h \
    src/sourceboundaries.h \
    src/sourcebuffer.h \
    src/sourcetypes.h \
    src/tabmodel.h \
    src/theme.h \
    src/themefolder.h \
    src/windowcontext.h

SOURCES += \
    src/main.cpp \
    src/apppaths.cpp \
    src/appservice.cpp \
    src/autosave.cpp \
    src/backend.cpp \
    src/benchmark.cpp \
    src/caretrest.cpp \
    src/cli.cpp \
    src/diskfile.cpp \
    src/document.cpp \
    src/documentreview.cpp \
    src/documentstore.cpp \
    src/editorbinding.cpp \
    src/filerequests.cpp \
    src/filewatch.cpp \
    src/history.cpp \
    src/hyprland.cpp \
    src/librarymodel.cpp \
    src/pathtext.cpp \
    src/portalfilepicker.cpp \
    src/qmltypes.cpp \
    src/quitsignals.cpp \
    src/recentfiles.cpp \
    src/recovery.cpp \
    src/session.cpp \
    src/settings.cpp \
    src/sourceboundaries.cpp \
    src/sourcebuffer.cpp \
    src/tabmodel.cpp \
    src/theme.cpp \
    src/windowcontext.cpp

RESOURCES += src/resources.qrc
