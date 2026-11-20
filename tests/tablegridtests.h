#pragma once

// The tests of tables drawn as grids in the text view (TableGrids): the
// caret in a table's cells, writing in them, keys from cell to cell, and
// everything else the view does with a grid in it. ParchTests runs each of
// them as a test function of its own.
namespace TableGridTests {

void caretRestsInAHiddenRowAndNoKeyEjectsIt();
void typingInACellEditsTheSourceAndKeepsItTidy();
void inputMethodComposesAtTheGridCaret();
void vimLinesMoveFromCellToCell();
void draggingFromProseOverATableSelectsItWhole();
void clicksBelowATableLandInProseAfterItMoves();
void tableItemsPassTheWindowChecks();
void editsInsideACellBecomeCellEdits();
void editsAcrossTableSyntaxAreRefusedWithAReason();
void wholeTableEditsPassThrough();
void everyWayOfEditingKeepsTheTableTidy();
void replaceAndReplaceAllKeepTablesTidy();
void findSkipsTableSyntax();
void aSecondViewKeepsItsCaretWhenATableIsRetidied();
void aRetidyChangesOnlyTheLinesItMustAndStylesRowsShownAgain();
void arrowsWalkThroughCellsAndOutOfTheTable();
void tabAndEnterMoveAddAndLeave();
void selectionsAreACellABlockOrTheWholeTable();
void theLineAfterATableStaysBlank();
void tablesAtTheEndsOfTheTextHaveACaretBesideThem();
void backspaceAfterATableSelectsThenDeletesIt();
void vimMotionsFlowAcrossCells();
void vimOperatorsActOnCellsAndRows();
void everyTableCommandRunsFromMenuPaletteAndHandle();
void handlesStayPutAsThePointerReachesThem();
void handlesMoveRowsAndColumnsByDragging();
void theSizePickerInsertsATidyEmptyTable();
void tablesCanBeShownAsMarkdownPerView();
void readOnlyTablesTakeNoEdits();
void copyingCellsGivesTabsAndAnHtmlTable();
void pastingCellsFillsOverwritesAndGrows();
void wideTablesScrollSidewaysAndTheTextScrollsOver();
void handTypedTablesWaitUntilTheCaretLeaves();
void everyGeometryQuestionHasAnAnswerInATable();
void fontAndWidthChangesKeepTheGrid();
void tablesTheGridCannotHoldStayMarkdown();
void aColumnsHandleActsOnTheColumnItPicked();
void aRefusedPasteOfCellsChangesNothing();
void textWrittenBelowATableStaysText();
void anEditThatWouldJoinCellsIsRefused();
void slowRichPastesGoInAsACellsText();
void anEditReachingIntoTwoTablesIsRefused();
void aDropOnRowsNoLongerThereMovesNothing();
void anEmojiReplacesAnotherInACell();
void cellsAShortRowLeavesOutAreReachedOneByOne();
void aSecondViewsCaretStaysWithItsRow();
void aChangeOfDialectReadsTheTableAgain();
void vimRowCopiesKeepMergedCells();
void copiedCellsKeepTheirQuotes();
void inlineCommandsFormatEachSelectedCell();
void aColumnAlignedAnewIsDrawnAligned();
void aCompositionShowsInItsCell();
void findSeesATableShownAsMarkdown();
void doubleAndTripleClicksSelectInACell();
void homeAndEndKeepToTheLineInAWrappedCell();
void aSecondViewsCaretStaysWithItsRowAmongLikeOnes();
void selectAllTakesThreePressesToTheWholeText();
void selectAllInATableOfOneCellStillStopsAtTheCell();
void aSlowPasteGoesWhereItWasMadeAfterTheViewMovesOn();
void aSlowPasteLeavesACaretTakenElsewhere();
void anImagePastedInACellAsksForItsDescription();

} // namespace TableGridTests
