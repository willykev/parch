#pragma once

// The tests of the editing commands, the command registry, the palette, the
// toolbar, word counts and the writing surface's settings. ParchTests runs
// each of them as a test function of its own.
namespace EditingTests {

void textEditDrawsOnlyTheEditorCaret();
void replaceAllFinishesAfterFindCloses_data();
void replaceAllFinishesAfterFindCloses();
void guideDescribesTheConflictChoicesShown_data();
void guideDescribesTheConflictChoicesShown();
void keepBothRestoresTheCaretOfTheKeptText_data();
void keepBothRestoresTheCaretOfTheKeptText();

void sourceEditsGroupIntoOneUndoStep();
void inlineFormattingWrapsAndToggles();
void scriptsWriteWhatTheDialectReads();
void linksAndImagesSelectWhatComesNext();
void headingsFollowTheChosenSyntax();
void blockFormattingTogglesEveryLine();
void insertionsKeepTheirBlankLines();
void smartKeysContinueAndIndentLists();
void tablesNavigateAndFormat();
void everyCommandIsOneStepOrNothing();
void editorRunsCommandsAsOneUndoStep();
void editorKeysContinueListsAndTables();
void registryGivesEveryKeyOneCommand();
void registryReadsDamagedKeysAndRunsActions();
void paletteFindsAndRunsCommands();
void shortcutsDialogRebindsSwapsAndResets();
void preferencesApplyLiveToEveryWindow();
void toolbarShowsHidesAndRunsCommands();
void formattingCommandsRunFromEveryEntryPoint();
void undefinedNotesBecomeInlineOrDefined();
void noteMenuFindsItsReferenceAgain();
void assistiveTechnologyEditsTheSource();
void wordCountsFollowTextAndSelection();
void richTablePasteKeepsHorizontalMerges_data();
void richTablePasteKeepsHorizontalMerges();

} // namespace EditingTests
