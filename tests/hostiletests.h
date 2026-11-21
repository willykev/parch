#pragma once

// Tests of files and text made to harm parch: a document, a bibliography,
// a style or a picture that names a device, a FIFO or a file of the
// system, nesting deep enough to exhaust a stack, strings that expand
// without end, input that takes quadratic time, and a worker that fails.
// Every hostile file is small and made in a temporary folder. The test
// class's slots call these.
namespace HostileTests {
void aFileThatGrowsPastTheLimitIsNotReadWhole();
void aThemeFileThatIsNotRegularIsIgnored();
void themeValuesAreClamped();
void folderSearchResultsShowBoundedContext();


void readsStopAtTheirLimit();
void bibliographiesAndStylesAreReadSafely();
void documentsAndIncludesAreReadSafely();
void picturesAreReadSafely();
void folderSearchReadsSafely();
void includesStayInTheirLibrary();
void frontMatterFilesStayInTheirLibrary();
void deepStylesAreRefused();
void deepBibliographiesAreRefused();
void deepMarkdownStaysWithinTheStack();
void bibtexStringsCannotExplode();
void quadraticInputsTakeLinearTime();
void workerFailuresAreReported();
void previewSurvivesAFailedRead();
void searchesSurviveAFailedJob();
void restoredTabsAreReadWhenShown();

} // namespace HostileTests
