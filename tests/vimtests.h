#pragma once

// The tests of Vim keys: the undo steps they make, their motions, operators,
// text objects, registers, repeats, Visual mode, search and command line,
// and how the window shows and leaves them. ParchTests runs each of them as
// a test function of its own.
namespace VimTests {

void undoStepsJoinIntoOneChange();
void vimKeysAreOffUntilChosenAndKept();
void motionsMoveAsVimDoes();
void operatorsActOnMotions();
void commandsEditAsVimDoes();
void textObjectsTakeTheirText();
void visualModeSelectsAndActs();
void searchFindsAndRepeats();
void registersKeepYanksAndPuts();
void dotRepeatsTheLastChange();
void undoTakesBackWholeChanges();
void modifierPressesKeepPendingCommands();
void deadKeysKeepPendingCharacterCommands();
void controlRRedoesWithVimEditorFocus();
void escapeAlwaysReachesNormalMode();
void insertTypesAsWithoutVimKeys();
void commandLineRunsWindowCommands();
void commandLineSavesAndClosesTabs();
void windowShowsAndLeavesVimKeys();

} // namespace VimTests
