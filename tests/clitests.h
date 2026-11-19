#pragma once

// The tests of the commands parch runs from the terminal without a window
// (src/clicommands.*, the dialect choice, the document checks, the exports
// and the agent skill). ParchTests runs each of them as a test function of
// its own, prefixed `cli`.
namespace CliTests {

void launchFindsCommands();
void helpExplainsEveryCommand();
void usageErrorsExitOne();
void checkReportsEveryKindOfProblem();
void checkPassesACleanDocument();
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
void exportLeavesTheSourceAlone();
void exportFormatsPlugInThroughOneInterface();
void statsCountWhatAReaderReads();
void statsCountVisibleCodeOnce();
void readingTextOmitsUnusedNotes();
void skillPrintsAndInstalls();
void skillLeavesOtherFilesAlone();
void jsonAndTextAreSafeToPrint();
void htmlDestinationsCanBeRewritten();
void commandsNeedNoDisplay();
void bibliographyLinksAreOnlyWebMailOrDoi();
void exportNeverWritesThroughALink();
void checkSaysWhetherHelpersAreIsolated();

} // namespace CliTests
