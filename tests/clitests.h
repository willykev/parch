#pragma once

// The tests of the commands parch runs from the terminal without a window
// (src/clicommands.*, the dialect choice, the document checks, the exports
// and the agent skill). ParchTests runs each of them as a test function of
// its own, prefixed `cli`.
namespace CliTests {
void aDeepTreeWritesLinearJson();


void launchFindsCommands();
void helpExplainsEveryCommand();
void usageErrorsExitOne();
void checkReportsEveryKindOfProblem();
void checkPassesACleanDocument();
void checkChecksAnchorsInTextDocuments();
void checkingRelativeLinksIgnoresTheQueryForFileLookup();
void checkSaysWhatCannotBeRead();
void rejectsNonUtf8FileNames();
void distinguishesNonregularPictures();
void profileAndExtensionsFollowTheFlags();
void savedSettingsApplyOnlyWhenAsked();
void renderWritesEveryForm();
void renderWritesFilesOnlyWhereAllowed();
void renderPreservesOutputWithAMissingInclude();
void exportAssemblesMarkdown();
void exportAssemblyKeepsMeaning();
void assembledHeadingLinksKeepTheirTarget();
void assembledLinksReachChaptersWithoutTheirFiles();
void assembledDestinationsKeepEncodedFragments();
void assembledChaptersKeepIndependentBlocks_data();
void assembledChaptersKeepIndependentBlocks();
void assembledLinksKeepTheirDefinitionScope();
void assembledRepeatedChaptersShareTheirInlineNote();
void epubReportsOmittedLocalLinksOnTheCli();
void assembledListsKeepTheirNumbering();
void htmlHeadingLinksResolveInTheirChapter();
void inheritedReferencesUseTheirDefinitionFolder();
void htmlAndEpubUseDistinctFootnoteIds();
void docxLinksUseExistingBookmarks();
void exportWritesAStandalonePage();
void exportRefusesWhatItMustNot();
void nothingADocumentIsMadeFromIsWrittenOver();
void assembledMarkdownKeepsItsCitations();
void assembledMarkdownKeepsEffectiveBibliography();
void assembledMarkdownOmitsCitationFilesOutsideReach();
void assembledMarkdownWithNoCitationsKeepsItsText();
void exportLeavesTheSourceAlone();
void exportFormatsPlugInThroughOneInterface();
void statsCountWhatAReaderReads();
void statsCountVisibleCodeOnce();
void plainTextAndStatsRespectHtmlBreaks_data();
void plainTextAndStatsRespectHtmlBreaks();
void plainTextAndStatsIncludeCellInlineNotes_data();
void plainTextAndStatsIncludeCellInlineNotes();
void plainTextRendersCitationsAndBibliography_data();
void plainTextRendersCitationsAndBibliography();
void plainTextKeepsCodeIndentation_data();
void plainTextKeepsCodeIndentation();
void readingTextOmitsUnusedNotes();
void skillPrintsAndInstalls();
void skillLeavesOtherFilesAlone();
void jsonAndTextAreSafeToPrint();
void htmlDestinationsCanBeRewritten();
void commandsNeedNoDisplay();
void reportsWhenStdoutCannotBeWritten_data();
void reportsWhenStdoutCannotBeWritten();
void writesThroughShortWrites();
void keepsQtNamedFilesAfterDoubleDash_data();
void keepsQtNamedFilesAfterDoubleDash();
void qtOptionsBeforeDoubleDash();
void quietExportSwitchesDoNotLogWarnings_data();
void quietExportSwitchesDoNotLogWarnings();
void quietExportsKeepUsageAndErrors();
void nonfiniteExportNumbersAreUsageErrors_data();
void nonfiniteExportNumbersAreUsageErrors();
void bibliographyLinksAreOnlyWebMailOrDoi();
void exportNeverWritesThroughALink();
void checkSaysWhetherHelpersAreIsolated();

void qtOptionsInFileArgumentsAreNotPassedToQt();
void checkReportsWhetherHelpersAreConfined();

} // namespace CliTests
