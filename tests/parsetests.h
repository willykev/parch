#pragma once

// The tests of the parse every view of a document shares (DocumentParse):
// one parse for each revision, never an older text as current, and a new
// parse when the dialect changes. ParchTests runs each of them as a test
// function of its own.
namespace ParseTests {

void everyViewReadsOneParseOfEachRevision();
void staleParsesAreNeverHandedOver();
void aChangeOfDialectParsesAgain();
void viewsReadTheDocumentsOwnText();
void checkingAndExportingReadTheSharedParse();

} // namespace ParseTests
