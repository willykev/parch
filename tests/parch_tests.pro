QT += core gui qml quick quickcontrols2 testlib dbus svg concurrent network
CONFIG += c++20 testcase link_pkgconfig
# fontconfig picks the fonts diagrams are drawn with; Hunspell checks spelling.
PKGCONFIG += fontconfig hunspell
# KSyntaxHighlighting colours the code in previews, as in parch.pro.
isEmpty(KSYNTAXHIGHLIGHTING_INCLUDE): KSYNTAXHIGHLIGHTING_INCLUDE = $$clean_path($$[QT_INSTALL_HEADERS]/../KF6/KSyntaxHighlighting)
INCLUDEPATH += $$KSYNTAXHIGHLIGHTING_INCLUDE
LIBS += -lKF6SyntaxHighlighting
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
    clitests.h \
    editingtests.h \
    librarytests.h \
    previewtests.h \
    stylingtests.h \
    rendertesting.h \
    markdown_tests.h \
    citationtests.h \
    settingstests.h \
    cslfixture.h \
    testfiles.h \
    vimtests.h \
    windowtesting.h \
    ../src/apppaths.h \
    ../src/appservice.h \
    ../src/attachments.h \
    ../src/autosave.h \
    ../src/backend.h \
    ../src/bibtex.h \
    ../src/benchmark.h \
    ../src/caretrest.h \
    ../src/citations.h \
    ../src/cli.h \
    ../src/codehighlight.h \
    ../src/clicommands.h \
    ../src/commandfilter.h \
    ../src/commandregistry.h \
    ../src/diagramrender.h \
    ../src/cslengine.h \
    ../src/cslreference.h \
    ../src/cslrichtext.h \
    ../src/cslstyle.h \
    ../src/dialectchoice.h \
    ../src/diskfile.h \
    ../src/document.h \
    ../src/documentcheck.h \
    ../src/documentcitations.h \
    ../src/documentfind.h \
    ../src/documentlinks.h \
    ../src/documentreading.h \
    ../src/documentreview.h \
    ../src/documentsettings.h \
    ../src/documentsettingsstore.h \
    ../src/documentstore.h \
    ../src/editorbinding.h \
    ../src/editordecorations.h \
    ../src/encodings.h \
    ../src/exporter.h \
    ../src/fileorganizer.h \
    ../src/filepicker.h \
    ../src/filerequests.h \
    ../src/filewatch.h \
    ../src/focusmode.h \
    ../src/foldersearch.h \
    ../src/frontmatter.h \
    ../src/history.h \
    ../src/htmlexport.h \
    ../src/htmlmarkdown.h \
    ../src/hyprland.h \
    ../src/librarymodel.h \
    ../src/markdownediting.h \
    ../src/markdownexport.h \
    ../src/librarywalk.h \
    ../src/markdownstyling.h \
    ../src/outline.h \
    ../src/mathrender.h \
    ../src/mdhtml.h \
    ../src/mdinclude.h \
    ../src/mdparser_p.h \
    ../src/mdtree.h \
    ../src/pathtext.h \
    ../src/portalfilepicker.h \
    ../src/preview.h \
    ../src/previewbuilder.h \
    ../src/previewresources.h \
    ../src/previewstyle.h \
    ../src/qmltypes.h \
    ../src/quitsignals.h \
    ../src/readingpositions.h \
    ../src/recentfiles.h \
    ../src/recovery.h \
    ../src/remotefetcher.h \
    ../src/rendercache.h \
    ../src/renderhelpers.h \
    ../src/renderrequest.h \
    ../src/renderservice.h \
    ../src/scrollsync.h \
    ../src/session.h \
    ../src/settings.h \
    ../src/sourceboundaries.h \
    ../src/sourcebuffer.h \
    ../src/sourcetypes.h \
    ../src/spellchecker.h \
    ../src/spellinglayer.h \
    ../src/tabmodel.h \
    ../src/textsearch.h \
    ../src/theme.h \
    ../src/treejson.h \
    ../src/themefolder.h \
    ../src/vimmode.h \
    ../src/windowcontext.h \
    ../src/wordcounter.h

# Every source except main.cpp is compiled in, since the test provides its own main.
SOURCES += \
    markdown_tests.cpp \
    parch_tests.cpp \
    clitests.cpp \
    editingtests.cpp \
    librarytests.cpp \
    previewtests.cpp \
    stylingtests.cpp \
    rendertesting.cpp \
    citationtests.cpp \
    settingstests.cpp \
    cslfixture.cpp \
    testfiles.cpp \
    vimtests.cpp \
    windowtesting.cpp \
    ../src/apppaths.cpp \
    ../src/appservice.cpp \
    ../src/attachments.cpp \
    ../src/autosave.cpp \
    ../src/backend.cpp \
    ../src/bibtex.cpp \
    ../src/benchmark.cpp \
    ../src/caretrest.cpp \
    ../src/citations.cpp \
    ../src/cli.cpp \
    ../src/codehighlight.cpp \
    ../src/clicommands.cpp \
    ../src/commandfilter.cpp \
    ../src/commandregistry.cpp \
    ../src/diagramrender.cpp \
    ../src/cslengine.cpp \
    ../src/cslreference.cpp \
    ../src/cslrichtext.cpp \
    ../src/cslstyle.cpp \
    ../src/dialectchoice.cpp \
    ../src/diskfile.cpp \
    ../src/document.cpp \
    ../src/documentcheck.cpp \
    ../src/documentcitations.cpp \
    ../src/documentfind.cpp \
    ../src/documentlinks.cpp \
    ../src/documentreading.cpp \
    ../src/documentreview.cpp \
    ../src/documentsettings.cpp \
    ../src/documentsettingsstore.cpp \
    ../src/documentstore.cpp \
    ../src/editorbinding.cpp \
    ../src/editordecorations.cpp \
    ../src/encodings.cpp \
    ../src/exporter.cpp \
    ../src/editorpaste.cpp \
    ../src/fileorganizer.cpp \
    ../src/filerequests.cpp \
    ../src/filewatch.cpp \
    ../src/focusmode.cpp \
    ../src/foldersearch.cpp \
    ../src/frontmatter.cpp \
    ../src/history.cpp \
    ../src/htmlexport.cpp \
    ../src/htmlmarkdown.cpp \
    ../src/hyprland.cpp \
    ../src/librarymodel.cpp \
    ../src/markdownediting.cpp \
    ../src/markdownexport.cpp \
    ../src/librarywalk.cpp \
    ../src/markdownstyling.cpp \
    ../src/outline.cpp \
    ../src/mathrender.cpp \
    ../src/mdblocks.cpp \
    ../src/mdentities.cpp \
    ../src/mdhtml.cpp \
    ../src/mdinclude.cpp \
    ../src/mdinlines.cpp \
    ../src/mdrawhtml.cpp \
    ../src/mdresolve.cpp \
    ../src/mdscan.cpp \
    ../src/mdtree.cpp \
    ../src/pathtext.cpp \
    ../src/portalfilepicker.cpp \
    ../src/preview.cpp \
    ../src/previewbuilder.cpp \
    ../src/previewresources.cpp \
    ../src/previewstyle.cpp \
    ../src/qmltypes.cpp \
    ../src/quitsignals.cpp \
    ../src/readingpositions.cpp \
    ../src/recentfiles.cpp \
    ../src/recovery.cpp \
    ../src/remotefetcher.cpp \
    ../src/rendercache.cpp \
    ../src/renderhelpers.cpp \
    ../src/renderrequest.cpp \
    ../src/renderservice.cpp \
    ../src/scrollsync.cpp \
    ../src/session.cpp \
    ../src/settings.cpp \
    ../src/sourceboundaries.cpp \
    ../src/sourcebuffer.cpp \
    ../src/spellchecker.cpp \
    ../src/spellinglayer.cpp \
    ../src/tabmodel.cpp \
    ../src/textsearch.cpp \
    ../src/theme.cpp \
    ../src/treejson.cpp \
    ../src/vimmode.cpp \
    ../src/windowcontext.cpp \
    ../src/wordcounter.cpp

RESOURCES += ../src/resources.qrc
RESOURCES += ../src/csl/csl.qrc

CONFIG += link_pkgconfig
PKGCONFIG += yaml-0.1
