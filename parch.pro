QT += core gui qml quick quickcontrols2 dbus svg concurrent network

CONFIG += c++20 release link_pkgconfig
# fontconfig picks the fonts diagrams are drawn with; Hunspell checks spelling.
PKGCONFIG += fontconfig hunspell
TARGET = parch
TEMPLATE = app

VERSION = $$cat($$PWD/VERSION)
DEFINES += PARCH_VERSION=\\\"$$VERSION\\\"

HEADERS += \
    src/apppaths.h \
    src/appservice.h \
    src/attachments.h \
    src/autosave.h \
    src/backend.h \
    src/bibtex.h \
    src/benchmark.h \
    src/caretrest.h \
    src/citations.h \
    src/cli.h \
    src/clicommands.h \
    src/commandfilter.h \
    src/commandregistry.h \
    src/diagramrender.h \
    src/cslengine.h \
    src/cslreference.h \
    src/cslrichtext.h \
    src/cslstyle.h \
    src/dialectchoice.h \
    src/diskfile.h \
    src/document.h \
    src/documentcheck.h \
    src/documentfind.h \
    src/documentreview.h \
    src/documentsettings.h \
    src/documentstore.h \
    src/editorbinding.h \
    src/editordecorations.h \
    src/exporter.h \
    src/fileorganizer.h \
    src/filepicker.h \
    src/filerequests.h \
    src/filewatch.h \
    src/focusmode.h \
    src/foldersearch.h \
    src/frontmatter.h \
    src/history.h \
    src/htmlexport.h \
    src/htmlmarkdown.h \
    src/hyprland.h \
    src/librarymodel.h \
    src/linktargets.h \
    src/markdownediting.h \
    src/markdownexport.h \
    src/librarywalk.h \
    src/markdownlines.h \
    src/outline.h \
    src/mathrender.h \
    src/mdhtml.h \
    src/mdinclude.h \
    src/mdparser_p.h \
    src/mdtree.h \
    src/pathtext.h \
    src/portalfilepicker.h \
    src/qmltypes.h \
    src/quitsignals.h \
    src/readingpositions.h \
    src/recentfiles.h \
    src/recovery.h \
    src/references.h \
    src/rendercache.h \
    src/renderhelpers.h \
    src/renderrequest.h \
    src/renderservice.h \
    src/session.h \
    src/settings.h \
    src/sourceboundaries.h \
    src/sourcebuffer.h \
    src/sourcetypes.h \
    src/spellchecker.h \
    src/spellingexclusions.h \
    src/spellinglayer.h \
    src/tabmodel.h \
    src/textsearch.h \
    src/theme.h \
    src/treejson.h \
    src/themefolder.h \
    src/vimmode.h \
    src/windowcontext.h \
    src/wordcounter.h

SOURCES += \
    src/main.cpp \
    src/apppaths.cpp \
    src/appservice.cpp \
    src/attachments.cpp \
    src/autosave.cpp \
    src/backend.cpp \
    src/bibtex.cpp \
    src/benchmark.cpp \
    src/caretrest.cpp \
    src/citations.cpp \
    src/cli.cpp \
    src/clicommands.cpp \
    src/commandfilter.cpp \
    src/commandregistry.cpp \
    src/diagramrender.cpp \
    src/cslengine.cpp \
    src/cslreference.cpp \
    src/cslrichtext.cpp \
    src/cslstyle.cpp \
    src/dialectchoice.cpp \
    src/diskfile.cpp \
    src/document.cpp \
    src/documentcheck.cpp \
    src/documentfind.cpp \
    src/documentreview.cpp \
    src/documentsettings.cpp \
    src/documentstore.cpp \
    src/editorbinding.cpp \
    src/editordecorations.cpp \
    src/exporter.cpp \
    src/editorpaste.cpp \
    src/fileorganizer.cpp \
    src/filerequests.cpp \
    src/filewatch.cpp \
    src/focusmode.cpp \
    src/foldersearch.cpp \
    src/frontmatter.cpp \
    src/history.cpp \
    src/htmlexport.cpp \
    src/htmlmarkdown.cpp \
    src/hyprland.cpp \
    src/librarymodel.cpp \
    src/linktargets.cpp \
    src/markdownediting.cpp \
    src/markdownexport.cpp \
    src/librarywalk.cpp \
    src/markdownlines.cpp \
    src/outline.cpp \
    src/mathrender.cpp \
    src/mdblocks.cpp \
    src/mdentities.cpp \
    src/mdhtml.cpp \
    src/mdinclude.cpp \
    src/mdinlines.cpp \
    src/mdrawhtml.cpp \
    src/mdresolve.cpp \
    src/mdscan.cpp \
    src/mdtree.cpp \
    src/pathtext.cpp \
    src/portalfilepicker.cpp \
    src/qmltypes.cpp \
    src/quitsignals.cpp \
    src/readingpositions.cpp \
    src/recentfiles.cpp \
    src/recovery.cpp \
    src/references.cpp \
    src/rendercache.cpp \
    src/renderhelpers.cpp \
    src/renderrequest.cpp \
    src/renderservice.cpp \
    src/session.cpp \
    src/settings.cpp \
    src/sourceboundaries.cpp \
    src/sourcebuffer.cpp \
    src/spellchecker.cpp \
    src/spellingexclusions.cpp \
    src/spellinglayer.cpp \
    src/tabmodel.cpp \
    src/textsearch.cpp \
    src/theme.cpp \
    src/treejson.cpp \
    src/vimmode.cpp \
    src/windowcontext.cpp \
    src/wordcounter.cpp

RESOURCES += src/resources.qrc

# Citation styles and locales, from the official CSL repositories.
RESOURCES += src/csl/csl.qrc

# libyaml reads CSL YAML bibliographies.
CONFIG += link_pkgconfig
PKGCONFIG += yaml-0.1
