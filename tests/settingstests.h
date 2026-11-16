#pragma once

// The tests of document settings, and of citations formatted through them:
// the layers and their precedence, the store parch keeps them in, every
// view and output reading a document in its dialect, the Document settings
// dialog and the Preferences section, and citations in the HTML renderer,
// `parch render` and `parch check`. ParchTests runs each of them as a test
// function of its own.
namespace SettingsTests {

void layersFollowTheirPrecedence();
void frontMatterChoosesBibliographyAndStyle();
void settingsAreKeptBesideTheDocuments();
void openDocumentsFollowTheirSettings();
void profileChangesEveryReading();
void linkUpdatesReadEachFileInItsDialect();
void documentSettingsDialogShowsSources();
void preferencesChooseForEveryDocument();
void citationSuffixesAndSuppressedAuthors();
void citationsAuthorDateInHtml();
void citationsNumericThroughRender();
void citationsInANoteStyleAreNotes();
void citationsFromEveryBibliographyFormat();
void citationsWithAStyleFile();
void writerAndPandocCitationsTogether();
void checkReportsCitationProblems();

} // namespace SettingsTests
