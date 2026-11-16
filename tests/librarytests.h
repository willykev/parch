#pragma once

// The tests of how much history keeps, files in other encodings, the
// library's width in each window, files renamed by other programs, recent
// files and drafts taken out of the library, searching a large library, and
// the user guide. ParchTests runs each of them as a test function of its own.
namespace LibraryTests {

void historyLimitsChangeWhileHistoryRuns();
void historyRetentionIsAPreference();
void captureHookOutlivesItsReplacement();
void recentFilesCanBeClearedOrForgotten();
void deletedDraftsGoToHistoryAndCanComeBack();
void draftsSetAsideAreForgottenAtLaunch();
void userGuideOpensAsReadOnlyHelp();
void eachWindowKeepsItsLibraryWidth();
void renamesByOtherProgramsAreFollowed();
void encodingsDecodeWhatUtf8CannotRead();
void reopeningInAnotherEncodingSavesOnlyOnceConfirmed();
void reopeningRefusesAFileThatChanged();
void historyRestoresBytesThatAreNotUtf8();
void repeatedLibrarySearchesReadLittleAgain();

} // namespace LibraryTests
