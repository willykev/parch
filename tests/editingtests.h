#pragma once

// The tests of the editing commands, the command registry, the palette, the
// toolbar, word counts and the writing surface's settings. ParchTests runs
// each of them as a test function of its own.
namespace EditingTests {

void sourceEditsGroupIntoOneUndoStep();
void inlineFormattingWrapsAndToggles();
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
void wordCountsFollowTextAndSelection();

} // namespace EditingTests
