#pragma once

// The tests of Markdown tables as text (MdTable): reading a table's cells as
// the parser does, writing it back tidy, and every change made to a table
// through its cells, rows and columns. ParchTests runs each of them as a
// test function of its own.
namespace TableTests {

void formatKeepsMergedCellsWhole();
void scannerAgreesWithTheParser();
void formatIsIdempotentAndKeepsPayloadBytes();
void formatKeepsPrefixesAndLineEndings();
void formatAlignsWideAndCombiningText();
void tablesTheGridCannotHoldSayWhy();

} // namespace TableTests
