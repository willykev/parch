#pragma once

// The tests of how much history keeps, files in other encodings, the
// library's width in each window, files renamed by other programs, recent
// files and drafts taken out of the library, searching a large library, and
// the user guide. ParchTests runs each of them as a test function of its own.
namespace LibraryTests {
void linkedFoldersOutsideTheLibraryAreNotWalked();
void linkUpdatesNeverRewriteFilesOutsideTheLibrary();
void aLinkedFolderOutsideIsNotAMoveDestination();
void renameAndTrashRefuseAPathThroughALinkOutside();
void linksInsideTheLibraryStillWork();
void aNewSaveAsNameUsesTheUmask();
void closingAMissingLinkReleasesItsName();
void aLinkEntryCanBeRenamedAndTrashed();
void aLibraryUnderALinkScansItsDocuments();
void linkUpdatesKeepOtherHardLinksIntact();
void openingAMovingDocumentSharesItsBuffer();
void replacingALinkKeepsAConcurrentFile();
void aCopyCanUseAnExclusiveFileOnALinklessVolume();
void aMoveRemovesOnlyTheRevisionsItClaimed();
void aFolderMoveCanExchangeAnExclusiveName();
void saveAsOntoADanglingLinkAsks();
void savingAtALinkOffersTheAvailableChoices();
void aDanglingLinkNamedAtLaunchCreatesNothing();
void savingADocumentOpenedThroughALinkStillWritesItsTarget();
void bibliographyReadsStayWithinTheDocumentsFolders();
void aCrossDeviceMoveOfALargeFileIsRefused();
void oneBusyDocumentDoesNotPruneOthersHistory();
void outsideReloadsShareTheHistoryInterval();
void withoutARuntimeFolderParchUsesItsOwn();
void renameUpdatesTextFilesAndHtmlAttributes_data();
void renameUpdatesTextFilesAndHtmlAttributes();
void undoRenameRestoresExactReferenceSpelling_data();
void undoRenameRestoresExactReferenceSpelling();
void undoMoveWaitsForAnOpenReview();
void undoNoticeWaitsForAMoveReview();
void moveKeepsFrontMatterCitationFilesReachable_data();
void moveKeepsFrontMatterCitationFilesReachable();
void movingADocumentLeavesUnchangedCitationPathsAlone();

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
void restoringBytesEndsAsASaveDoes();
void quitLeavesAtOnceOnlyForAStuckSave();
void savingAsOverAReencodedFileAsksFirst();
void restoringReadsHistoryOffTheUiThread();
void repeatedLibrarySearchesReadLittleAgain();

} // namespace LibraryTests
