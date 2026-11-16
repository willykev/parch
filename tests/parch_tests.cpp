#include "apppaths.h"
#include "appservice.h"
#include "autosave.h"
#include "backend.h"
#include "benchmark.h"
#include "caretrest.h"
#include "cli.h"
#include "diskfile.h"
#include "document.h"
#include "documentreview.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "filepicker.h"
#include "filerequests.h"
#include "filewatch.h"
#include "history.h"
#include "hyprland.h"
#include "librarymodel.h"
#include "pathtext.h"
#include "portalfilepicker.h"
#include "qmltypes.h"
#include "quitsignals.h"
#include "recentfiles.h"
#include "recovery.h"
#include "session.h"
#include "settings.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "theme.h"
#include "themefolder.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QAbstractEventDispatcher>
#include <QStyleHints>
#include <QAbstractItemModelTester>
#include <QAccessible>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlListReference>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRunnable>
#include <QSemaphore>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QTranslator>
#include <QUuid>
#include <QWindow>
#include <QTimeZone>
#include <QtEndian>
#include <QtTest>
#include <algorithm>
#include <chrono>
#include <functional>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <poll.h>
#include <fcntl.h>
#include <atomic>
#include <memory>
#include <thread>
#include <unistd.h>

namespace {

// Occupies every thread of the global pool until released, so that work the
// store queues there starts only when the test lets it.
class PoolBlock {
public:
    explicit PoolBlock(QThreadPool *pool = QThreadPool::globalInstance())
        : m_pool(pool), m_gate(std::make_shared<QSemaphore>())
    {
        m_count = m_pool->maxThreadCount();
        // Each blocker owns the gate, so that it outlives this object: a
        // thread woken by release() is still inside the semaphore afterwards.
        for (int i = 0; i < m_count; ++i)
            m_pool->start([gate = m_gate] { gate->acquire(); });
    }
    ~PoolBlock() { release(); }
    void release()
    {
        if (const int count = std::exchange(m_count, 0); count > 0) m_gate->release(count);
    }

private:
    QThreadPool *m_pool;
    std::shared_ptr<QSemaphore> m_gate;
    int m_count = 0;
};

// A draft never written in gives its tab to the next document opened in
// front of it. Tests that keep the first launch's draft as a tab of its own
// give it undo history, so that it stays, while it stays empty and clean.
void keepDraft(Document *draft)
{
    draft->buffer()->replace({0, 0}, QStringLiteral("x"));
    draft->buffer()->undo();
}

// The text of every version history holds of a document.
QStringList versionTexts(const History *history, const QString &documentId)
{
    QStringList texts;
    for (const History::Version &version : history->versions(documentId))
        texts.append(QString::fromUtf8(history->read(documentId, version.id)));
    return texts;
}

// The text recovery keeps for a document, or "(none)".
QString recoveryText(const AppPaths &paths, const QString &documentId)
{
    for (const Recovery::Entry &entry : Recovery(paths).pending())
        if (entry.documentId == documentId) return entry.text;
    return QStringLiteral("(none)");
}

// What a launch finds when text recovery keeps cannot be shown: plan.md and
// other.md each ended with text only recovery holds, other.md's written
// first, each in a tab of the session, and a link to other.md has taken
// plan.md's place since. The launch restores other.md's text first, so
// plan.md's path leads to that document's file, and its entry is held.
struct HeldEntry {
    QString plan, other, planId, otherId;
    // The entry's own file in recovery.
    QString file;
};

void leaveAHeldEntry(const AppPaths &paths, const QString &folder, HeldEntry &left)
{
    left.plan = folder + QStringLiteral("/plan.md");
    left.other = folder + QStringLiteral("/other.md");
    writeFile(left.plan, "plan\n");
    writeFile(left.other, "other\n");
    {
        DocumentStore store(paths);
        Document *first = store.open(left.other);
        Document *second = store.open(left.plan);
        QVERIFY(first && second);
        left.otherId = first->id();
        left.planId = second->id();
        first->buffer()->replace({0, 0}, QStringLiteral("edited "));
        second->buffer()->replace({0, 0}, QStringLiteral("mine "));
        Recovery recovery(paths);
        QVERIFY(recovery.write(left.otherId, left.other, *first->buffer()->snapshot(), first->diskVersion()));
        QTest::qWait(20);
        QVERIFY(recovery.write(left.planId, left.plan, *second->buffer()->snapshot(), second->diskVersion()));
    }
    left.file = paths.recovery() + u'/' + left.planId + QStringLiteral(".recovery");
    SessionState state{{WindowState{{{left.otherId, left.other, 0, 0, 0}, {left.planId, left.plan, 0, 0, 0}}, 0, false,
                                    QSize(900, 700)}}};
    QVERIFY(Session::save(paths, state));
    QVERIFY(QFile::remove(left.plan));
    QVERIFY(QFile::link(left.other, left.plan));
}

// A Unix socket's path holds at most 107 bytes, less the 8 Qt adds while
// it makes one in private. A test whose temporary folder leaves no room for
// its socket is skipped, saying why, rather than failed for a reason that
// is not parch's; bin/test keeps its temporary folders short.
bool socketFits(const QString &path)
{
    return QFile::encodeName(path).size() <= qsizetype(sizeof(sockaddr_un::sun_path)) - 1 - 8;
}

#define SKIP_UNLESS_SOCKET_FITS(path)                                                                                  \
    do {                                                                                                               \
        const QString socketPath_ = (path);                                                                            \
        if (!socketFits(socketPath_))                                                                                  \
            QSKIP(qPrintable(QStringLiteral("The temporary folder is too long for a socket: ") + socketPath_));       \
    } while (false)

QStringList sourceBoundaryFixtures()
{
    const auto prepend = QString(180, QChar(0x0600));
    const auto combining = QString(320, QChar(0x0301));
    QString regional;
    for (int i = 0; i < 181; ++i) regional += QString::fromUcs4(U"🇦");
    return {QString(), QStringLiteral("plain printable ASCII !~"),
        QStringLiteral("a\r\nb\rc\nd\u2029e\u2028f\u00a0g\n"),
        QString::fromUtf8("👩🏽‍💻🇺🇸🇨🇦áक्षि العربية עברית \u2067RTL\u2069"),
        prepend + "a" + combining + "b\r\n" + regional + "\u2029" + prepend + "z",
        "a" + combining + QString::fromUtf8("👨‍👩‍👧‍👦") + combining,
        QStringLiteral("before\n\u0600\u0301x\r\n\u0301after\u2029\u0600z"),
        QString(QChar(0xd800)) + "x" + QChar(0xdc00)};
}
} // namespace

class ParchTests : public QObject {
    Q_OBJECT

public:
    // Before the application object exists: from here on Qt's standard
    // locations are its test ones, so that nothing the tests do reads or
    // writes the real configuration, data, state or cache folders.
    static void initMain();

private slots:
    void initTestCase();
    void launchParsing();
    void launchMessages();
    void launchLeavesQtOptionsToQt();
    void helpColumnsLineUp();
    void testsKeepToTestLocations();
    void desktopEntryOpensMarkdown();
    void benchmarkReportsEveryMeasure();
    void benchmarkThatCannotStartEndsTheLoop();
    void themeReadsLightAndDarkPalettes();
    void themeFollowsReplacementTwice();
    void themeFallsBackWithoutOmarchy();
    void themeLookupResolvesNamesAndPaths();
    void themeTakesGeometryFromHyprland();
    void themeFollowsHyprlandEventsAndActivation();
    void themeAsksHyprlandNothingWhileIdle();
    void themeListensToHyprlandAgainAfterItGoes();
    void themeWithoutHyprlandKeepsDefaults();
    void hyprlandHearsTheQuestionBeforeTheEventLoopRuns();
    void hyprlandNeverWaitsOnABusyUiThread();
    void hyprlandEventsAreOnlyReadAndTakenAtOnce();
    void sourceEditsPreserveUnicodeAndSnapshots();
    void undoRestoresCombiningEdits();
    void sourceBoundaryOracle();
    void sourceBoundaryNavigation();
    void sourceParagraphRanges();
    void sourceEditOutcomes();
    void typingCoalescesIntoOneUndoStep();
    void deletingCoalescesAdjacentRanges();
    void modifiedTracksSavedPosition();
    void editNeverJoinsTheSavedStep();
    void untouchedFilesSaveByteIdentical();
    void invalidUtf8OpensReadOnlyWithBytesKept();
    void saveRefusesWhenDiskChanged();
    void saveKeepsPermissionsAndSymlink();
    void saveWritesAHardLinkedFileInPlace();
    void inPlaceSavesKeepNoMoreVersionsThanOthers();
    void saveCarriesAttributesAndMode();
    void anAttributeThatCannotBeCopiedKeepsTheFile();
    void saveWritesAnotherUsersFileInPlace();
    void saveFailureLeavesTargetAndNoLitter();
    void missingFileSavesOnlyWhileStillMissing();
    void conflictLeavesNoTemporaryFile();
    void createNeverFollowsOrReplaces();
    void createWithoutRenameOrLinks();
    void aFailedDirectWriteRemovesOnlyItsOwnFile();
    void fifoDoesNotBlockLoadOrSave();
    void savedVersionMatchesTheWrittenFile();
    void aChangeOfTheSameSizeAndTimeIsSeen();
    void aStampTooCloseToItsTimesIsNotTrusted();
    void aSaveReadsTheFileAtMostOnce();
    void aStampRecordedSoonAfterItsTimesIsNotTrusted();
    void openingReadsTheFileOnce();
    void danglingLinkSavesTwice();
    void danglingLinkThroughLinkedDirectoryKeepsUnrelatedFile();
    void linkLoopFailsWithoutTouchingAnything();
    void dotDotInsideLinkTargetKeepsUnrelatedFile();
    void dotDotAfterSymlinkedDirectoryUsesKernelFile();
    void sameFileThroughSymlinkIsOneDocument();
    void documentIdentitySurvivesRestart();
    void draftCreatesNoFileUntilEdited();
    void namingDraftKeepsTextUndoAndIdentity();
    void namingDraftCarriesAssetsAndRefusesCollisions();
    void aLinkAtAssetsReceivesNothing();
    void assetsGoThroughTheFoldersOpened();
    void failedSaveStaysVisibleAndRetryable();
    void lastViewClosingKeepsUnsavedDocument();
    void draftReopensByIdentity();
    void saveFinishingAfterNewEditsStaysModified();
    void conflictAndReadOnlyNeverWrite();
    void openDuringNamingReturnsTheNamingDocument();
    void twoDocumentsCannotTakeOneName();
    void reopenedDocumentOutlivesItsSave();
    void recoveryRoundTripsAnyText();
    void recoveryFollowsIdleAndMaximumIntervals();
    void autosaveWritesAfterIdle();
    void draftWhoseFolderWentIsMadeAgain();
    void crashRestoresUnsavedText();
    void cleanCloseFlushesAndClearsRecovery();
    void recoveryWriteFailureIsReported();
    void conflictKeepsRecoveryAndNeverSaves();
    void failedSaveIsNotClearedByAutosave();
    void flushKeepsRecoveryForFailedSave();
    void restoredNamedFileSavesAndClearsRecovery();
    void restoreAfterExternalEditWaitsInConflict();
    void restoreOfDeletedFileWaitsInConflict();
    void restoreOfUnreadableFileSaysWhyAndReadsAgain();
    void restoreOfIdenticalTextOpensClean();
    void restoreOfInvalidUtf8FileWaitsInConflict();
    void restoreOfDraftNamedLaterOpensTheNamedFile();
    void conflictAtCleanCloseStaysConflictAfterRestart();
    void flushSurvivesDocumentsClosedWhileWaiting();
    void restoreIgnoresDeviceAndInodeChanges();
    void restoreOfEntryWithoutBaseWaitsInConflict();
    void crashAfterOverlappedSaveRestoresTheNewestText();
    void restoreAfterOwnSaveIsNoConflict();
    void outsideWriteOfAnEarlierSaveStaysAConflict();
    void savesThatNeverLandedOrCameBeforeAreNotOwn();
    void ownSavesOutlastAFailedOrUnfinishedOne();
    void newerMeansLaterInTheEditsNotOnTheClock();
    void savesGoWithTheirDocument();
    void namingThatMeetsAChangeKeepsOwnSaves();
    void draftBytesNothingKeptAreNeverSavedOver();
    void draftConflictResolvesAgainstItsOwnFile();
    void namingDraftKeepsItsOwnFileInHistory();
    void fileWatchReportsABurstOnceAndSurvivesReplacement();
    void fileWatchIgnoresNeighboursAndReportsRemoval();
    void fileWatchLooksOffTheUiThread();
    void cleanDocumentReloadsExternalEditKeepingUndo();
    void reloadKeepsGraphemesWhole();
    void dirtyDocumentEntersConflictAndPausesAutosave();
    void deletedFileKeepsText();
    void conflictChoices();
    void deletedFileOnlyAllowsOverwrite();
    void restoredConflictResolvesAgainstTheFileNow();
    void invalidUtf8ConflictReloadsReadOnly();
    void ownSavesDoNotRaiseConflicts();
    void replacedFileIsNoticedRepeatedly();
    void renamedDocumentWatchesItsNewPath();
    void keepBothNeverReplacesExistingFiles();
    void keepBothTwiceKeepsBothCopies();
    void keepBothNeverFollowsALinkAtItsName();
    void keepBothTakesTheNextNameWhenOneAppears();
    void aPathThatNowLeadsElsewhereIsNeverTaken();
    void saveAsItsOwnNameNeverWritesThroughALink();
    void savingAsItsOwnNameWaitsWhenSomethingTookIt();
    void filesMadeAgainKeepTheirMode();
    void aConflictsReasonFollowsWhatIsAtTheName();
    void putThisTextReplacesTheLink();
    void aSessionTabNeverReopensThroughASwappedLink();
    void aRecordedPathSpelledAnotherWayOpensWithoutText();
    void anUncleanSpellingThroughAFolderLinkOpensWithoutText();
    void aTextlessDocumentTakesItsFileWhenItComesBack();
    void closingATextlessTabKeepsNothingInRecovery();
    void aRecoveryEntryThatCannotBeOpenedOutlastsTheSession();
    void aHeldEntryComesBackWhenItsFileDoes();
    void aHeldEntryIsRestoredHoweverItsPathIsOpened_data();
    void aHeldEntryIsRestoredHoweverItsPathIsOpened();
    void aHeldEntryStaysAsItIsWhileItsPathLeadsElsewhere();
    void aHeldEntrysPathIsRecordedWithoutTheRegistry();
    void aHeldEntrysTextIsInHistoryBeforeItsIdIsADocuments();
    void aSessionTabAsksForItsFilesNameOnce();
    void aRecordedPathThatLeadsElsewhereOpensWithoutText();
    void aRecoveredLinkConflictKeepsTheFilesConventions();
    void aRecoveredDocumentKeepsItsFilesMode();
    void keepBothRunsOffTheUiThread();
    void keepBothThatLandsLateLeavesTheDocumentAlone();
    void aKeepBothCopyOfAClosedDocumentIsSaid();
    void followRunsOffTheUiThread();
    void followKeepsThisTextInHistoryOnAWorker();
    void followReachesOtherFilesInTheRuntimeFolder();
    void followTakesOnlyTheFileShown();
    void followKeepsTextTypedMeanwhile();
    void followNeverTakesOneOfParchsOwnFiles();
    void keepBothOpensTheCopyItMade();
    void saveAgainNamesAFolderThatIsGone();
    void saveAgainNeverFollowsALinkWhereTheFileWas();
    void conflictOverMissingFileKeepsRecovery();
    void reappearingFileRefreshesTheReason();
    void fileWatchRenewsWatchAfterRenameOverHeldFile();
    void typingDuringACheckKeepsTheTextInConflict();
    void changeDuringQueuedSaveIsAConflict();
    void conflictNoticesAFileGoneAgain();
    void historyCapturesOnInterval();
    void historyPrunesByCountAgeAndSizeButKeepsNamed();
    void historyPausesWhenNamedVersionsFillBudget();
    void historyBudgetCoversAllDocuments();
    void aCaptureReadsOnlyItsOwnIndex();
    void historyTotalsFollowEveryChange();
    void historyPrunesOldVersionsAsItStarts();
    void removingNamedVersionsLiftsThePause();
    void historyRejectsIdsThatAreNotUuids();
    void idsAndTextsAreCheckedOneWay();
    void historyReadsBackAndSurvivesRestart();
    void historyFailureIsNotPause();
    void saveCapturesHistory();
    void historyKeepsTheTextAFileWasOpenedWith();
    void versionsSayWhyTheyWereKept();
    void restoreAfterASaveKeepsNoSecondCopy();
    void restoringTheSameBytesChangesNothing();
    void restoreIsUndoable();
    void restoreRefusedWhenHistoryCannotWrite();
    void restoreProceedsWhenHistoryIsPaused();
    void captureThatCannotBeKeptPauses();
    void restoreRefusalIsReported();
    void unfitCaptureDeletesNothing();
    void captureReportsPauseAndFailureApart();
    void failedNameWriteFailsTheCapture();
    void namedBytesOverBudgetLeaveUnnamedVersionsAlone();
    void sizePruningStopsAtTheOldestThatMakesRoom();
    void rebuiltIndexReadsTimesAsUtc();
    void versionTimesInALocalGapStayExact();
    void namesSurviveALostIndex();
    void unindexedVersionIsAdopted();
    void collisionsNeverRenameOrLoseVersions();
    void emptyVersionCanBeRestored();
    void nativeInputRoundTripsSource();
    void duplicateViewsKeepIndependentPositions();
    void plainKeysTypeText();
    void enterUsesDocumentNewline();
    void insertedBreaksFollowTheDocument();
    void remoteEditDuringCompositionCancelsOnlyPreedit();
    void remoteEditDuringCompositionIsOneDelta();
    void preeditUpdatesKeepTheComposition();
    void widthChangeKeepsTopAnchor();
    void caretStaysInViewAsTheTextAreaShrinks();
    void largeDocumentDefersRewrap();
    void largeTextDrawsOnlyWhatIsInView();
    void lineBreaksMapExactly();
    void editsReachTheViewAsDeltas();
    void editsNeverCopyTheWholeText();
    void readOnlyTakesNoEditsAndConflictDoes();
    void nativeWritesReturnToSource();
    void imeCommitsLandOnGraphemes();
    void keysEditWholeGraphemes();
    void editorDrawsFromTheme();
    void caretBlinksThenRestsAndDrawsNothing();
    void caretIsSteadyWithoutMotionOrFlash();
    void textFieldsRestTheirCaretToo();
    void columnHoldsTheLineLength();
    void linesSitTheLineHeightApart();
    void bindingOutlivesItsEditor();
    void bindingOutlivesItsDocument();
    void editorLookupsFailVisibly();
    void layoutHelpersCatchDeliberateBreaks();
    void shellLaysOutAtFourWidths();
    void singleTabHidesStrip();
    void tabStripKeepsTabsReadableAndScrolls();
    void everyTabClearsTheNewTabButton();
    void tabsMarkDocumentsThatNeedAttention();
    void everyControlHasAccessibleNameAndFocus();
    void shortcutsAlwaysCarryAModifier();
    void undoAndRedoKeysActOnceWhereverFocusIs();
    void windowShortcutsReachPastTheEditor();
    void plainKeysTypeInTheWindow();
    void themeSwitchKeepsCursorSelectionAndUndo();
    void fontZoomReflowsWithinWindow();
    void fontSizeFollowsThemeUntilZoomed();
    void zoomPastALimitKeepsFollowingTheTheme();
    void settingsSurviveRestartAndDamage();
    void libraryDocksOrOpensAsDrawer();
    void theTextIsNamedAsItsTab();
    void headerNamesTheDocumentAndItsPlace();
    void statusLineNamesEverySaveState();
    void quitKeepsEveryTextSafe();
    void endingAtOnceKeepsEveryTextWithinItsTime();
    void noColourLiteralsInQml();
    void reducedMotionRemovesAnimation();
    void libraryDrawerShowsItsEdgeAndCorners();
    void quitRefusalReadsInFullAndClears();
    void keyboardFocusIsPlainToSee();
    void settingsFollowTheThemeFromTheStart();
    void keysShownAreTheShortcutsOwn();
    void openingAlreadyOpenFileFocusesExistingTab();
    void newViewSharesTextButNotCursor();
    void closingOneViewKeepsDocumentOpenElsewhere();
    void sessionRestoresWindowsTabsAndAnchors();
    void missingFileOnRestoreIsReportedNotRecreated();
    void firstLaunchOpensOneBlankDraft();
    void anUntouchedDraftGivesWayToTheDocumentOpened();
    void secondLaunchForwardsFilesToRunningInstance();
    void processForwardsAndEndsOnSignal();
    void endingOnASignalRefusesAndLeavesInTime();
    void processRefusesAMissingTheme();
    void processBenchmarkEndsByItself();
    void tabsLoadWhenFirstShown();
    void damagedSessionStartsFreshAndIsKept();
    void restoredDocumentsGoToTheirSessionWindow();
    void recoveryOnlyTextReturnsAfterRelaunch();
    void launchSaysWhatCameBackFromRecovery();
    void aLaunchThatOnlyKeepsTextInRecoverySaysSo();
    void sessionIsWrittenLaterAndOffTheUiThread();
    void quitRefusalIgnoresRecoveryEvidenceFromBeforeIt();
    void closingTabsAndWindowsKeepsText();
    void failedSaveOfAClosedTabComesBack();
    void conflictCopyOpensInActiveWindow();
    void newFileBecomesItsFileOnFirstEdit();
    void forwardedFilesOpenInTheMostRecentWindow();
    void filesThatCannotBeReadAreNamedWhenOpened();
    void eachRefusalToOpenSaysItsOwnReason();
    void relativeLaunchPathsBecomeFull();
    void staleSocketIsReplacedAndBadRequestsDropped();
    void forwardedOpenRaisesWithItsToken();
    void runtimeFolderMustBeTheAccountsOwn();
    void secondLaunchWaitsForOneStillStarting();
    void viewsComeBackWhereTheyWere();
    void newWindowHasItsOwnTabsAndKeys();
    void pageKeysSwitchTabsAsTabKeysDo();
    void tabKeysCycleAndCloseInTheWindow();
    void newFileThatAppearsIsNeverReplaced();
    void forwardWaitsUntilTheInstanceTakesTheFiles();
    void alreadyOpenFileComesToFrontInItsWindow();
    void backgroundViewsFollowEditsElsewhere();
    void closingUnsavableTextKeepsItsTab();
    void windowsReachTabsOnlyThroughTheBackend();
    void errorsShownCanBeTranslated();
    void openingFindsATabNotYetReadInAnyWindow();
    void libraryListsDraftsRecentAndFolders();
    void folderShowsOnlyTextDocumentsAndSubfolders();
    void filterNarrowsByNameIgnoringCase();
    void searchListsFoldersNotListedYet();
    void searchListingIsBounded();
    void searchListsAgainAfterTheFieldIsEmptied();
    void anOlderSearchsListingsHoldNothingUp();
    void aListingFromBeforeTheFieldWasEmptiedIsUsed();
    void aProbeThatNeverAnswersStopsSearchingInTime();
    void searchFollowsFoldersAddedAndRemoved();
    void unreadableFolderShowsErrorRow();
    void recentPersistsAndFlagsMissingFiles();
    void libraryFoldersPersistAndGoneOnesStay();
    void expandedFoldersFollowTheDisk();
    void listingStaysOffTheUiThread();
    void draftsFollowCreationNamingAndRemoval();
    void recentIsNotedOnOpenSaveAndName();
    void libraryRowsOpenThroughTheBackend();
    void libraryKeyboardFindsAndOpens();
    void libraryDrawerClosesOnOpening();
    void searchOpensAndWatchesDeepFolders();
    void searchSaysSearchingAndPutsExpansionBack();
    void searchSaysSearchingWhileItsListingsAreOut();
    void searchKeepsTheWindowResponsiveAtTheLibraryWorkload();
    void libraryWorkNeverHoldsUpSaving();
    void libraryKeyGoesToTheFieldAndBack();
    void librarySearchEnterSpaceAndCursor();
    void listedEmptyFolderShowsNoChevronWhileWatched();
    void saveOnDraftAsksForNameThenPromotes();
    void cancelledPickerChangesNothing();
    void fileDialogClosedByItsWindowIsACancel();
    void closingFailedSaveAsksFirst();
    void conflictDialogOffersAllChoices();
    void conflictDifferenceIsMarkedByLines();
    void conflictDifferenceFindsEachRun();
    void conflictReviewMarksAndShowsTheDifference();
    void typingGoesOnInAConflict();
    void keepBothOpensTheCopyWhereTheWriterWas();
    void historyDialogRestoresAsUndoableEdit();
    void foldersJoinTheLibraryAndShowThere();
    void aFileOpenedWithFoldersKeepsTheKeys();
    void firstLaunchGivesTheKeysToWhatItOpens();
    void historyNamesAndRemovesVersions();
    void historyDialogNamesDeletesAndGuardsRestore();
    void historyDialogSaysWhyAVersionWasKept();
    void cancelledPromptsGiveTheKeysBackToTheirButton();
    void restorePressedAsVersionsAreReadAgain();
    void deletedFileConflictOffersSaveAgainOrElsewhere();
    void aLinkInTheFilesPlaceOffersToFollowIt();
    void aDocumentWithoutTextOffersItsHistory();
    void aHeldTabNamesWhereItsPathLeadsFromHome();
    void saveAgainSaysWhatTookTheName();
    void unreadableFileOffersRetryOrSaveAs();
    void draftConflictDialogShowsItsOwnFile();
    void noticeBarShowsEachStateUntilResolved();
    void noticesReadAsShownToAssistiveTechnology();
    void damagedRegistryIsKeptAside();
    void suggestedNamesAreSafeFileNames();
    void closeConfirmationOffersEveryChoice();
    void closingAWindowAsksAboutItsUnsavedText();
    void failedSaveOnClosingAsksInstead();
    void pickerAnswersGoWhereAsked();
    void failedNamingIsTriedAgainThere();
    void pickedFileAlreadyOpenComesToItsTab();
    void dialogsTakeTheKeysAndGiveThemBack();
    void dialogsKeepTheKeysUntilTheyClose();
    void keysReachEveryRegionAndNotice();
    void windowMenuOpensFromTheKeyboard();
    void windowMenuActsOnItsOwnWindow();
    void failedSaveNamesItsFolderAndOffersSaveAs();
    void noticesMarkFailuresApartFromNews();
    void noticeBarAndDialogsFitEveryWidth();
    void namedVersionsSurviveAnUnfinishedRename();
    void closingWhenHistoryPausedAsksToDiscard();
    void textTypedWhileClosingIsKeptInHistory();
    void restoreRefusalsAlwaysSayWhy();
    void staleReadsNeverReplaceNewerOnes();
    void saveAsOverItsOwnConflictedFileOverwrites();
    void saveAsFromAConflictWaitsForItsOutcome();
    void dialogsStartInTheFolderLastUsed();
    void pausedHistorySaysWhy();
    void messagesNameParchsFoldersPlainly();
    void versionTimesReadNaturally();
    void historyActionsActOnTheirDocument();
};

void ParchTests::initMain()
{
    QStandardPaths::setTestModeEnabled(true);
}

void ParchTests::initTestCase()
{
    // The style and QML types main() sets up, so that the window loads as it does in the application.
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    registerQmlTypes();
}

void ParchTests::launchParsing()
{
    QCOMPARE(parseLaunch({"parch"}).kind, Launch::Gui);
    QCOMPARE(parseLaunch({"parch", "open", "a.md", "b.md"}).files, (QStringList{"a.md", "b.md"}));
    QCOMPARE(parseLaunch({"parch", "notes.md"}).files, QStringList{"notes.md"});
    QCOMPARE(parseLaunch({"parch", "open", "--theme", "/t", "a.md"}).themeDirectory, QStringLiteral("/t"));
    QCOMPARE(parseLaunch({"parch", "open", "--benchmark", "out.json", "a.md"}).benchmarkOutput, QStringLiteral("out.json"));
    QCOMPARE(parseLaunch({"parch", "--version"}).kind, Launch::Version);
    QCOMPARE(parseLaunch({"parch", "--help"}).kind, Launch::Help);
    QCOMPARE(parseLaunch({"parch", "--bogus"}).kind, Launch::Invalid);
    QVERIFY(!parseLaunch({"parch", "--bogus"}).error.isEmpty());
    QCOMPARE(versionLine(), QStringLiteral("parch 0.1.0"));
    // A value can follow '='; an option is never taken as another's value.
    QCOMPARE(parseLaunch({"parch", "--theme=nord", "a.md"}).themeDirectory, QStringLiteral("nord"));
    QCOMPARE(parseLaunch({"parch", "--benchmark=out.json", "a.md"}).benchmarkOutput, QStringLiteral("out.json"));
    QCOMPARE(parseLaunch({"parch", "--theme", "--version"}).kind, Launch::Invalid);
    QCOMPARE(parseLaunch({"parch", "--theme="}).kind, Launch::Invalid);
    // A single dash is an option too, refused unless it is -h.
    QCOMPARE(parseLaunch({"parch", "-x"}).kind, Launch::Invalid);
    QCOMPARE(parseLaunch({"parch", "-h"}).kind, Launch::Help);
    // After "--" every argument is a file, dashes and all.
    QCOMPARE(parseLaunch({"parch", "--", "-notes.md", "--version"}).files, (QStringList{"-notes.md", "--version"}));
    // "open" is a subcommand only first; later it is a file's name.
    QCOMPARE(parseLaunch({"parch", "a.md", "open"}).files, (QStringList{"a.md", "open"}));
}

void ParchTests::launchMessages()
{
    QVERIFY(helpText().startsWith(QStringLiteral("Usage: parch")));
    QVERIFY(parseLaunch({"parch", "--bogus"}).error.contains(QStringLiteral("--bogus")));
    const Launch missing = parseLaunch({"parch", "--theme"});
    QCOMPARE(missing.kind, Launch::Invalid);
    QVERIFY(missing.error.contains(QStringLiteral("--theme")));
}

void ParchTests::launchLeavesQtOptionsToQt()
{
    // Qt takes its own options out of the arguments, with one dash or two,
    // and the value of those that have one; what is left is parch's.
    const Launch platform = parseLaunch({"parch", "-platform", "offscreen", "x.md"});
    QCOMPARE(platform.kind, Launch::Gui);
    QCOMPARE(platform.files, QStringList{"x.md"});
    const Launch doubled = parseLaunch({"parch", "--platform", "offscreen", "open", "x.md"});
    QCOMPARE(doubled.kind, Launch::Gui);
    QCOMPARE(doubled.files, QStringList{"x.md"});
    const Launch many = parseLaunch({"parch", "open", "-style", "Basic", "-qwindowgeometry", "800x600+0+0", "-reverse",
                                     "-style=Fusion", "--platformtheme", "generic", "-qwindowtitle", "Notes", "a.md",
                                     "--theme", "/t", "b.md"});
    QCOMPARE(many.kind, Launch::Gui);
    QCOMPARE(many.files, (QStringList{"a.md", "b.md"}));
    QCOMPARE(many.themeDirectory, QStringLiteral("/t"));
    // Anything else that looks like an option is still refused.
    QCOMPARE(parseLaunch({"parch", "--platformz", "x"}).kind, Launch::Invalid);
}

void ParchTests::helpColumnsLineUp()
{
    // Every option's description starts in one column.
    QSet<qsizetype> columns;
    static const QRegularExpression gap(QStringLiteral("^  \\S+(?: [A-Z]+)? +(\\S)"));
    for (const QString &line : helpText().split(u'\n')) {
        if (!line.startsWith(QLatin1String("  "))) continue;
        const QRegularExpressionMatch match = gap.match(line);
        QVERIFY2(match.hasMatch(), qPrintable(line));
        columns.insert(match.capturedStart(1));
    }
    QVERIFY(columns.size() == 1);
}

void ParchTests::testsKeepToTestLocations()
{
    // Set by initMain(), before any test runs.
    QVERIFY(QStandardPaths::isTestModeEnabled());
    // The Documents folder is read from the test configuration, never the person's.
    const QString realConfig = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config"));
    QVERIFY(!QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation).startsWith(realConfig));
}

void ParchTests::desktopEntryOpensMarkdown()
{
    const QSettings entry(QStringLiteral(PARCH_SOURCE_DIR "/pkgbuild/parch.desktop"), QSettings::IniFormat);
    QCOMPARE(entry.value("Desktop Entry/Name").toString(), QStringLiteral("parch"));
    QCOMPARE(entry.value("Desktop Entry/Exec").toString(), QStringLiteral("parch %F"));
    QCOMPARE(entry.value("Desktop Entry/Icon").toString(), QStringLiteral("parch"));
    QCOMPARE(entry.value("Desktop Entry/StartupWMClass").toString(), QStringLiteral("parch"));
    QVERIFY(entry.value("Desktop Entry/MimeType").toString().contains("text/markdown"));
}

void ParchTests::themeReadsLightAndDarkPalettes()
{
    QTemporaryDir dir;
    writeTheme(dir.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    Theme light(dir.path());
    QCOMPARE(light.dark(), false);
    QCOMPARE(light.background(), QColor("#F8F1E3"));
    QCOMPARE(light.foreground(), QColor("#342B27"));
    QCOMPARE(light.accent(), QColor("#A7353A"));
    QVERIFY(light.selection().isValid());

    QTemporaryDir darkDir;
    writeTheme(darkDir.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    Theme dark(darkDir.path());
    QCOMPARE(dark.dark(), true);
    QCOMPARE(dark.background(), QColor("#1a1b26"));
}

void ParchTests::themeFollowsReplacementTwice()
{
    QTemporaryDir dir;
    writeTheme(dir.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    Theme theme(dir.path());
    QCOMPARE(theme.dark(), false);
    replaceTheme(dir.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    QTRY_COMPARE(theme.background(), QColor("#1a1b26"));
    QCOMPARE(theme.dark(), true);
    // The second switch is only seen if the watcher was armed again after the first.
    replaceTheme(dir.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    QTRY_COMPARE(theme.dark(), false);
    QCOMPARE(theme.background(), QColor("#F8F1E3"));
}

void ParchTests::themeFallsBackWithoutOmarchy()
{
    Theme missing(QStringLiteral("/nonexistent/theme"));
    QVERIFY(missing.background().isValid());
    QCOMPARE(missing.radius(), 0);
    QCOMPARE(missing.fontFamily(), QStringLiteral("monospace"));
}

void ParchTests::themeLookupResolvesNamesAndPaths()
{
    QTemporaryDir root;
    const QString themes = root.filePath("themes");
    writeFile(themes + "/dusk/colors.toml", "background = \"#101010\"\n");
    writeFile(root.filePath("loose/colors.toml"), "background = \"#202020\"\n");
    writeFile(root.filePath("elsewhere/colors.toml"), "background = \"#303030\"\n");
    const QStringList folders{root.filePath("missing"), themes};

    QCOMPARE(findTheme("dusk", folders), themes + "/dusk");
    QCOMPARE(findTheme(root.filePath("loose"), folders), root.filePath("loose"));
    QCOMPARE(findTheme(root.filePath("loose/"), folders), root.filePath("loose"));
    QCOMPARE(findTheme("unknown", folders), QString());
    QCOMPARE(findTheme(QString(), folders), QString());
    QCOMPARE(findTheme(root.filePath("nowhere"), folders), QString());
    // A name never reaches out of the theme folders, so these are only paths.
    QCOMPARE(findTheme("../elsewhere", {themes}), QString());
    QCOMPARE(findTheme("..", {themes}), QString());
    QCOMPARE(findTheme(".", {themes}), QString());
    // A relative path with a slash is a path, found from the current folder.
    const QString current = QDir::currentPath();
    const auto restoreCurrent = qScopeGuard([&current] { QDir::setCurrent(current); });
    QVERIFY(QDir::setCurrent(root.path()));
    QCOMPARE(findTheme("elsewhere/", {themes}), root.filePath("elsewhere"));
    QCOMPARE(findTheme("./loose", {themes}), root.filePath("loose"));
}

namespace {
// Counts the timers that fire for an object or anything it owns.
class TimerCount : public QObject {
public:
    explicit TimerCount(QObject *owner) : m_owner(owner) { QCoreApplication::instance()->installEventFilter(this); }
    ~TimerCount() override { QCoreApplication::instance()->removeEventFilter(this); }
    int fired = 0;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Timer)
            for (QObject *object = watched; object; object = object->parent())
                if (object == m_owner) {
                    ++fired;
                    break;
                }
        return false;
    }

private:
    QObject *m_owner;
};
} // namespace

void ParchTests::themeTakesGeometryFromHyprland()
{
    QTemporaryDir dir, sockets;
    writeTheme(dir.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    // A fake stands in for Hyprland so that the real compositor is never asked.
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 8);
    hyprland.setInt(QStringLiteral("general:border_size"), 3);
    hyprland.setInt(QStringLiteral("animations:enabled"), 0);
    Theme rounded(dir.path(), nullptr, sockets.path());
    QTRY_COMPARE(rounded.radius(), 8);
    QTRY_COMPARE(rounded.borderWidth(), 3);
    QTRY_VERIFY(rounded.reduceMotion());
}

void ParchTests::themeFollowsHyprlandEventsAndActivation()
{
    QTemporaryDir dir, sockets;
    writeTheme(dir.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 8);
    hyprland.setInt(QStringLiteral("animations:enabled"), 1);
    Theme theme(dir.path(), nullptr, sockets.path());
    QTRY_COMPARE(theme.radius(), 8);
    QTRY_COMPARE(hyprland.listeners(), 1);
    QVERIFY(!theme.reduceMotion());

    // Other events ask nothing.
    QTest::qWait(50);
    const int asked = hyprland.questions();
    hyprland.send("activewindow>>kitty,notes");
    hyprland.send("workspace>>2");
    QTest::qWait(300);
    QCOMPARE(hyprland.questions(), asked);

    // A configuration read again, as a theme switch or a toggle makes, is asked again.
    hyprland.setInt(QStringLiteral("decoration:rounding"), 4);
    hyprland.setInt(QStringLiteral("animations:enabled"), 0);
    hyprland.send("configreloaded>>");
    QTRY_COMPARE(theme.radius(), 4);
    QTRY_VERIFY(theme.reduceMotion());

    // `hyprctl keyword` sends no event: a window coming to the front asks again.
    hyprland.setInt(QStringLiteral("decoration:rounding"), 6);
    QTest::qWait(300);
    QCOMPARE(theme.radius(), 4);
    QWindow window;
    window.resize(100, 100);
    window.show();
    window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTRY_COMPARE(theme.radius(), 6);
}

void ParchTests::themeAsksHyprlandNothingWhileIdle()
{
    QTemporaryDir root, sockets, bin;
    // The theme watches its folder's parent, which is the test's own here
    // rather than the temporary folder other programs write in.
    const QString dir = root.filePath(QStringLiteral("omarchy"));
    writeTheme(dir, "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    // A hyprctl that leaves a mark, first on the path: no process is started for Hyprland.
    const QString mark = bin.filePath(QStringLiteral("ran"));
    writeFile(bin.filePath(QStringLiteral("hyprctl")), "#!/bin/sh\ntouch '" + mark.toUtf8() + "'\n");
    QVERIFY(QFile::setPermissions(bin.filePath(QStringLiteral("hyprctl")),
                                  QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QByteArray path = qgetenv("PATH");
    const auto restorePath = qScopeGuard([&path] { qputenv("PATH", path); });
    qputenv("PATH", bin.path().toUtf8() + ':' + path);

    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 8);
    Theme theme(dir, nullptr, sockets.path());
    QTRY_COMPARE(theme.radius(), 8);
    QTRY_COMPARE(hyprland.listeners(), 1);
    // Longer than the theme waits after its window comes to the front.
    QTest::qWait(400);

    // Idle for longer than two seconds: no question, no timer of the
    // theme's, no process.
    const int asked = hyprland.questions();
    TimerCount timers(&theme);
    QTest::qWait(2500);
    QCOMPARE(hyprland.questions(), asked);
    QCOMPARE(timers.fired, 0);
    QVERIFY(!QFile::exists(mark));
    QVERIFY(theme.findChildren<QProcess *>().isEmpty());
    // Still listening, and still following.
    hyprland.setInt(QStringLiteral("decoration:rounding"), 2);
    hyprland.send("configreloaded>>");
    QTRY_COMPARE(theme.radius(), 2);
    QVERIFY(!QFile::exists(mark));
}

void ParchTests::themeListensToHyprlandAgainAfterItGoes()
{
    QTemporaryDir dir, sockets;
    writeTheme(dir.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 8);
    Theme theme(dir.path(), nullptr, sockets.path());
    QTRY_COMPARE(hyprland.listeners(), 1);
    QTRY_COMPARE(theme.radius(), 8);

    // The event connection drops: it listens again after a wait, not in a loop.
    QElapsedTimer waited;
    waited.start();
    hyprland.dropListeners();
    QTRY_COMPARE(hyprland.listeners(), 0);
    // A reload meanwhile is an event nobody heard: listening again asks again.
    hyprland.setInt(QStringLiteral("decoration:rounding"), 5);
    QTRY_COMPARE_WITH_TIMEOUT(hyprland.listeners(), 1, 5000);
    QVERIFY2(waited.elapsed() >= HyprlandOptions::FirstRetryMs - 100, qPrintable(QString::number(waited.elapsed())));
    QTRY_COMPARE(theme.radius(), 5);
    hyprland.setInt(QStringLiteral("decoration:rounding"), 3);
    hyprland.send("configreloaded>>");
    QTRY_COMPARE(theme.radius(), 3);
}

void ParchTests::themeWithoutHyprlandKeepsDefaults()
{
    QTemporaryDir dir, empty;
    writeTheme(dir.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    // A folder without Hyprland's sockets is no Hyprland: nothing is asked and nothing waits.
    Theme theme(dir.path(), nullptr, empty.path());
    TimerCount timers(&theme);
    QTest::qWait(300);
    QCOMPARE(theme.radius(), 0);
    QCOMPARE(theme.borderWidth(), 2);
    QVERIFY(!theme.reduceMotion());
    QCOMPARE(timers.fired, 0);
    QVERIFY(theme.findChildren<QLocalSocket *>(Qt::FindChildrenRecursively).isEmpty());
}

namespace {
// A listening Unix socket at the path, as Hyprland's request socket is, or -1.
int listenAt(const QString &path)
{
    const int server = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray name = QFile::encodeName(path);
    std::memcpy(address.sun_path, name.constData(), size_t(name.size()));
    if (::bind(server, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0 || ::listen(server, 8) != 0) {
        ::close(server);
        return -1;
    }
    return server;
}
} // namespace

void ParchTests::hyprlandHearsTheQuestionBeforeTheEventLoopRuns()
{
    // Hyprland reads a question as soon as it takes the connection and stops
    // everything else until it arrives. parch makes its first window before
    // its event loop runs, so the question must already be on its way: this
    // stand-in waits as Hyprland does, with no event processed meanwhile.
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    const QString path = sockets.filePath(QStringLiteral(".socket.sock"));
    SKIP_UNLESS_SOCKET_FITS(path);
    const int server = listenAt(path);
    QVERIFY(server >= 0);
    const auto closeServer = qScopeGuard([server] { ::close(server); });

    HyprlandOptions options(sockets.path(), {QStringLiteral("decoration:rounding")});
    // A connection left to the event loop never comes, and the test fails
    // rather than waits in accept for ever.
    pollfd waiting{server, POLLIN, 0};
    QVERIFY2(::poll(&waiting, 1, 2000) == 1, "no connection came without the event loop");
    const int connection = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    QVERIFY(connection >= 0);
    const auto closeConnection = qScopeGuard([connection] { ::close(connection); });
    pollfd ready{connection, POLLIN, 0};
    QCOMPARE(::poll(&ready, 1, 1000), 1);
    char question[64] = {};
    const ssize_t got = ::read(connection, question, sizeof question - 1);
    QCOMPARE(QByteArray(question, int(qMax<ssize_t>(got, 0))), QByteArrayLiteral("j/getoption decoration:rounding"));
}

void ParchTests::hyprlandNeverWaitsOnABusyUiThread()
{
    // Hyprland serves nothing else while a connection it has taken is
    // silent. So no question may wait for parch's UI thread, which can be
    // busy at any moment: here it is busy, its event loop stopped, right
    // after it asks, both at the start and when it asks again. A stand-in
    // on a thread of its own serves the request socket as Hyprland does,
    // reads each question once, and notes how long the connection was
    // silent. It waits less long than the UI thread is busy.
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    const QString path = sockets.filePath(QStringLiteral(".socket.sock"));
    SKIP_UNLESS_SOCKET_FITS(path);
    const int server = listenAt(path);
    QVERIFY(server >= 0);
    const auto closeServer = qScopeGuard([server] { ::close(server); });
    constexpr int busyMs = 1500, patienceMs = 1000;
    const QStringList asked{QStringLiteral("decoration:rounding"), QStringLiteral("general:border_size"),
                            QStringLiteral("animations:enabled")};
    struct Heard {
        QByteArray question;
        qint64 silentMs;
    };
    QList<Heard> heard;
    std::thread hyprland([&] {
        for (int served = 0; served < 2 * asked.size(); ++served) {
            pollfd waiting{server, POLLIN, 0};
            if (::poll(&waiting, 1, 5000) != 1) return;
            const int connection = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (connection < 0) return;
            QElapsedTimer silent;
            silent.start();
            pollfd ready{connection, POLLIN, 0};
            char question[128] = {};
            const ssize_t got = ::poll(&ready, 1, patienceMs) == 1 ? ::read(connection, question, sizeof question) : 0;
            heard.append({QByteArray(question, int(qMax<ssize_t>(got, 0))), silent.elapsed()});
            const QByteArray answer = "{\"int\":1,\"set\":true}";
            (void)::send(connection, answer.constData(), size_t(answer.size()), MSG_NOSIGNAL);
            ::close(connection);
        }
    });
    const auto joined = qScopeGuard([&hyprland] {
        if (hyprland.joinable()) hyprland.join();
    });

    HyprlandOptions options(sockets.path(), asked);
    QSignalSpy answered(&options, &HyprlandOptions::answered);
    QThread::msleep(busyMs);
    QTRY_COMPARE(answered.size(), asked.size());
    options.ask();
    QThread::msleep(busyMs);
    QTRY_COMPARE(answered.size(), 2 * asked.size());
    hyprland.join();

    QCOMPARE(heard.size(), 2 * asked.size());
    QStringList questions;
    for (const Heard &one : std::as_const(heard)) {
        QVERIFY2(one.silentMs < 200, qPrintable(QStringLiteral("silent for %1 ms").arg(one.silentMs)));
        questions.append(QString::fromUtf8(one.question));
    }
    questions.sort();
    QStringList expected;
    for (const QString &option : asked) expected << QStringLiteral("j/getoption ") + option << QStringLiteral("j/getoption ") + option;
    expected.sort();
    QCOMPARE(questions, expected);

    // A Hyprland that takes the questions and never answers holds up
    // neither the UI thread nor parch's end.
    QElapsedTimer ending;
    {
        auto silent = std::make_unique<HyprlandOptions>(sockets.path(), asked);
        QTest::qWait(100);
        ending.start();
    }
    QVERIFY2(ending.elapsed() < 500, qPrintable(QString::number(ending.elapsed())));
}

void ParchTests::hyprlandEventsAreOnlyReadAndTakenAtOnce()
{
    // parch never writes to Hyprland's event socket, and takes every event
    // there is whenever its event loop runs, however many have come, so
    // that Hyprland never holds events for it for long.
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 8);
    HyprlandOptions options(sockets.path(), {QStringLiteral("decoration:rounding")});
    QSignalSpy answered(&options, &HyprlandOptions::answered);
    QTRY_COMPARE(hyprland.listeners(), 1);
    QTRY_COMPARE(answered.size(), 1);

    // More events than a socket holds, and a line longer than any event.
    const QByteArray event = "activewindow>>kitty," + QByteArray(100, 'x');
    for (int i = 0; i < 4000; ++i) hyprland.send(event);
    hyprland.send(QByteArray(100000, 'y'));
    hyprland.send("configreloaded>>");
    QTRY_COMPARE_WITH_TIMEOUT(answered.size(), 2, 5000);
    QTRY_COMPARE(hyprland.untaken(), 0);
    QCOMPARE(hyprland.listeners(), 1);
    QCOMPARE(hyprland.heardFromListeners(), 0);
}

void ParchTests::sourceBoundaryOracle()
{
    // Catches accepting UTF-16 interiors or dropping long GCB context at a slice edge.
    for (const auto &source : sourceBoundaryFixtures()) {
        QTextBoundaryFinder oracle(QTextBoundaryFinder::Grapheme, source);
        for (int p = -1; p <= source.size() + 1; ++p) {
            bool expected = false;
            if (p >= 0 && p <= source.size()) {
                oracle.setPosition(p);
                expected = source.isEmpty() || oracle.isAtBoundary();
            }
            QVERIFY2(sourceGraphemeBoundary(source, p) == expected,
                     qPrintable(QString("offset %1 in %2 UTF-16 units").arg(p).arg(source.size())));
            QCOMPARE(sourceGraphemeRange(source, {p, 0}), expected);
        }
        QVERIFY(!sourceGraphemeRange(source, {0, -1}));
        QVERIFY(!sourceGraphemeRange(source, {1, std::numeric_limits<int>::max()}));
        QVERIFY(!sourceGraphemeRange(source, {std::numeric_limits<int>::max(), 1}));
        QTextBoundaryFinder ends(QTextBoundaryFinder::Grapheme, source);
        for (int start = 0; start <= source.size(); ++start) {
            ends.setPosition(start);
            const bool first = source.isEmpty() || ends.isAtBoundary();
            // Cross-paragraph as well as zero-length range validation.
            QCOMPARE(sourceGraphemeRange(source, {start, int(source.size()) - start}), first);
        }
    }
}

void ParchTests::sourceBoundaryNavigation()
{
    // Catches nearest-boundary errors from interior surrogates, CRLF and paragraph edges.
    for (const auto &source : sourceBoundaryFixtures()) {
        QTextBoundaryFinder oracle(QTextBoundaryFinder::Grapheme, source);
        for (int p = -1; p <= source.size() + 1; ++p) {
            int before = -1, after = -1;
            if (p >= 0 && p <= source.size() && !source.isEmpty()) {
                oracle.setPosition(p); before = oracle.toPreviousBoundary();
                oracle.setPosition(p); after = oracle.toNextBoundary();
            }
            QCOMPARE(previousSourceGrapheme(source, p), before);
            QCOMPARE(nextSourceGrapheme(source, p), after);
        }
    }
}

void ParchTests::sourceParagraphRanges()
{
    // Literal expectations catch CRLF-half ownership and empty paragraphs at EOF.
    const QString source = QStringLiteral("ab\r\nc\rd\ne\u2029f\u2028g\n");
    const QVector<SourceRange> wanted = {{0,2},{0,2},{0,2},{0,2}, {4,1},{4,1},
        {6,1},{6,1}, {8,1},{8,1}, {10,3},{10,3},{10,3},{10,3},{14,0}};
    for (int p = 0; p < wanted.size(); ++p) {
        const auto actual = sourceParagraphRange(source, p);
        QCOMPARE(actual.start, wanted[p].start);
        QCOMPARE(actual.length, wanted[p].length);
    }
    QCOMPARE(sourceParagraphRange({}, 0).start, 0);
    QCOMPARE(sourceParagraphRange({}, 0).length, 0);
    QCOMPARE(sourceParagraphRange(source, -1).start, -1);
    QCOMPARE(sourceParagraphRange(source, 15).start, -1);
    QCOMPARE(sourceParagraphRange(source, std::numeric_limits<int>::max()).start, -1);
    QCOMPARE(sourceParagraphRange(QStringLiteral("\r\n\n\u2029"), 1).length, 0);
}

void ParchTests::sourceEditOutcomes()
{
    // Catches rejected/no-op edits mutating revisions, emitting changes or truncating redo.
    using Outcome = SourceBuffer::EditOutcome;
    SourceBuffer buffer(QStringLiteral("a\r\nb"));
    QSignalSpy changed(&buffer, &SourceBuffer::sourceChanged);
    QCOMPARE(buffer.replace({1, 0}, QStringLiteral("x")), Outcome::Applied);
    QCOMPARE(buffer.text(), QStringLiteral("ax\r\nb"));
    QCOMPARE(buffer.revision(), Revision(1)); QCOMPARE(changed.size(), 1);
    buffer.undo();
    QCOMPARE(buffer.revision(), Revision(2)); QCOMPARE(changed.size(), 2);
    for (const auto range : QVector<SourceRange>{{2,0},{-1,0},{0,-1},{1,std::numeric_limits<int>::max()}})
        QCOMPARE(buffer.replace(range, {}), Outcome::Rejected);
    QCOMPARE(buffer.replace({0,1}, QStringLiteral("a")), Outcome::Unchanged);
    QCOMPARE(buffer.text(), QStringLiteral("a\r\nb"));
    QCOMPARE(buffer.revision(), Revision(2)); QCOMPARE(changed.size(), 2);
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("ax\r\nb"));
    QCOMPARE(buffer.revision(), Revision(3)); QCOMPARE(changed.size(), 3);
    QCOMPARE(buffer.replace({0,1}, QStringLiteral("a")), Outcome::Unchanged);
    QCOMPARE(buffer.replace({3,0}, {}), Outcome::Rejected);
    QCOMPARE(buffer.replace({0,1}, QStringLiteral("z")), Outcome::Applied);
    for (const auto &pair : QVector<QPair<QString,QString>>{{QStringLiteral("a"),QStringLiteral("\u0301")},
                                                            {QStringLiteral("\r"),QStringLiteral("\n")}}) {
        SourceBuffer historic(pair.first);
        QSignalSpy historicChanges(&historic, &SourceBuffer::sourceChanged);
        QCOMPARE(historic.replace({int(pair.first.size()),0}, pair.second), Outcome::Applied);
        historic.undo(); QCOMPARE(historic.text(), pair.first);
        historic.redo(); QCOMPARE(historic.text(), pair.first + pair.second);
        QCOMPARE(historic.revision(), Revision(3)); QCOMPARE(historicChanges.size(), 3);
    }
}

void ParchTests::sourceEditsPreserveUnicodeAndSnapshots()
{
    // Accepting a UTF-16 interior or a grapheme interior corrupts source editing.
    const auto original = QStringLiteral("A😀e\u0301\u00a0B");
    SourceBuffer buffer(original);
    const auto before = buffer.snapshot();
    QCOMPARE(buffer.revision(), Revision(0));
    QCOMPARE(before->revision, Revision(0));
    QCOMPARE(before->text, original);
    const QList<SourceRange> invalid = {
        {-1, 0}, {0, -1}, {8, 0}, {7, 1},
        {std::numeric_limits<int>::max(), 1}, {1, std::numeric_limits<int>::max()},
        {2, 0}, {2, 1}, {1, 1}, {4, 0}, {3, 1}, {4, 1}
    };
    for (const auto range : invalid) {
        QCOMPARE(buffer.replace(range, QStringLiteral("x")), SourceBuffer::EditOutcome::Rejected);
        QCOMPARE(buffer.text(), original);
        QCOMPARE(buffer.revision(), Revision(0));
    }
    QCOMPARE(buffer.replace({0, 0}, QString()), SourceBuffer::EditOutcome::Unchanged);
    QCOMPARE(buffer.replace({1, 2}, QStringLiteral("😀")), SourceBuffer::EditOutcome::Unchanged);
    QCOMPARE(buffer.revision(), Revision(0));
    QCOMPARE(buffer.replace({0, 1}, QStringLiteral("Z")), SourceBuffer::EditOutcome::Applied);
    QCOMPARE(buffer.text(), QStringLiteral("Z😀e\u0301\u00a0B"));
    QCOMPARE(buffer.text().at(5).unicode(), ushort(0x00a0));
    QCOMPARE(buffer.revision(), Revision(1));
    QCOMPARE(before->text, original);
    QCOMPARE(before->revision, Revision(0));
    const auto edited = buffer.snapshot();
    buffer.undo();
    QCOMPARE(buffer.text(), original);
    QCOMPARE(buffer.revision(), Revision(2));
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("Z😀e\u0301\u00a0B"));
    QCOMPARE(buffer.revision(), Revision(3));
    QCOMPARE(edited->text, QStringLiteral("Z😀e\u0301\u00a0B"));
    QCOMPARE(edited->revision, Revision(1));
    const auto surviving = [] {
        SourceBuffer temporary(QStringLiteral("untouched\u00a0e\u0301😀"));
        return temporary.snapshot();
    }();
    QCOMPARE(surviving->text, QStringLiteral("untouched\u00a0e\u0301😀"));
    QCOMPARE(surviving->revision, Revision(0));
}

void ParchTests::undoRestoresCombiningEdits()
{
    // Revalidating historic edits would reject removal of the newly joined mark.
    SourceBuffer buffer(QStringLiteral("e"));
    QCOMPARE(buffer.replace({1, 0}, QStringLiteral("\u0301")), SourceBuffer::EditOutcome::Applied);
    QCOMPARE(buffer.text(), QStringLiteral("e\u0301"));
    QCOMPARE(buffer.revision(), Revision(1));
    QCOMPARE(buffer.replace({1, 1}, QString()), SourceBuffer::EditOutcome::Rejected);
    QCOMPARE(buffer.revision(), Revision(1));
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("e"));
    QCOMPARE(buffer.revision(), Revision(2));
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("e\u0301"));
    QCOMPARE(buffer.revision(), Revision(3));
    QCOMPARE(buffer.replace({0, 2}, QStringLiteral("😀\u00a0")), SourceBuffer::EditOutcome::Applied);
    QCOMPARE(buffer.revision(), Revision(4));
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("e\u0301"));
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("e"));
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("e\u0301"));
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("😀\u00a0"));
    QCOMPARE(buffer.revision(), Revision(8));
}

void ParchTests::typingCoalescesIntoOneUndoStep()
{
    SourceBuffer buffer(QStringLiteral("ab"));
    qint64 now = 0;
    buffer.setClock([&] { return now; });
    buffer.replace({2, 0}, "c", EditKind::Typing);
    now += 300;
    buffer.replace({3, 0}, "d", EditKind::Typing);
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("ab"));
    buffer.redo();
    now += 2500;
    buffer.replace({4, 0}, "e", EditKind::Typing);
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("abcd")); // a pause starts a step
    buffer.replace({4, 0}, "\n", EditKind::Typing);
    buffer.replace({5, 0}, "x", EditKind::Typing);
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("abcd\n")); // a line break ends a step
}

void ParchTests::deletingCoalescesAdjacentRanges()
{
    SourceBuffer backspace(QStringLiteral("abcdef"));
    qint64 now = 0;
    backspace.setClock([&] { return now; });
    backspace.replace({5, 1}, {}, EditKind::Deleting);
    backspace.replace({4, 1}, {}, EditKind::Deleting);
    backspace.replace({3, 1}, {}, EditKind::Deleting);
    QCOMPARE(backspace.text(), QStringLiteral("abc"));
    backspace.undo();
    QCOMPARE(backspace.text(), QStringLiteral("abcdef"));
    QVERIFY(!backspace.canUndo());

    SourceBuffer forward(QStringLiteral("abcdef"));
    forward.setClock([&] { return now; });
    forward.replace({1, 1}, {}, EditKind::Deleting);
    forward.replace({1, 1}, {}, EditKind::Deleting);
    forward.undo();
    QCOMPARE(forward.text(), QStringLiteral("abcdef"));

    // A deletion elsewhere is a new step.
    SourceBuffer apart(QStringLiteral("abcdef"));
    apart.setClock([&] { return now; });
    apart.replace({0, 1}, {}, EditKind::Deleting);
    apart.replace({3, 1}, {}, EditKind::Deleting);
    apart.undo();
    QCOMPARE(apart.text(), QStringLiteral("bcdef"));

    // Nothing joins a step that holds a line break.
    SourceBuffer lines(QStringLiteral("a\nbc"));
    lines.setClock([&] { return now; });
    lines.replace({3, 1}, {}, EditKind::Deleting);
    lines.replace({1, 1}, {}, EditKind::Deleting);
    lines.undo();
    QCOMPARE(lines.text(), QStringLiteral("a\nb"));
}

void ParchTests::modifiedTracksSavedPosition()
{
    SourceBuffer saved(QStringLiteral("a"));
    QSignalSpy flips(&saved, &SourceBuffer::modifiedChanged);
    QVERIFY(!saved.modified());
    saved.replace({1, 0}, "b");
    QVERIFY(saved.modified());
    QCOMPARE(flips.size(), 1);
    saved.markSaved();
    QVERIFY(!saved.modified());
    QCOMPARE(flips.size(), 2);
    saved.markSaved();
    QCOMPARE(flips.size(), 2);
    saved.undo();
    QVERIFY(saved.modified());
    saved.redo();
    QVERIFY(!saved.modified());
    QCOMPARE(flips.size(), 4);
    saved.replace({2, 0}, "c");
    saved.replace({3, 0}, "d");
    QCOMPARE(flips.size(), 5); // a second edit does not flip again
}

void ParchTests::editNeverJoinsTheSavedStep()
{
    SourceBuffer buffer;
    qint64 now = 0;
    buffer.setClock([&] { return now; });
    buffer.replace({0, 0}, "a", EditKind::Typing);
    buffer.markSaved();
    now += 100;
    buffer.replace({1, 0}, "b", EditKind::Typing);
    QVERIFY(buffer.modified());
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("a"));
    QVERIFY(!buffer.modified());
}

void ParchTests::untouchedFilesSaveByteIdentical()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    const QByteArray original = QByteArray("\xEF\xBB\xBF") + QStringLiteral("# T\r\nnbsp here\nmixed\r\n{{unknown}} x\n").toUtf8();
    writeFile(path, original);
    const LoadedFile file = DiskFile::load(path);
    QVERIFY(file.ok);
    QVERIFY(!file.readOnly);
    QVERIFY(file.convention.bom);
    // Two CRLF breaks against two bare LF breaks is a tie, and ties go to CRLF.
    QCOMPARE(file.convention.newline, QStringLiteral("\r\n"));
    QCOMPARE(file.originalBytes, original);
    QVERIFY(file.version.exists);
    QCOMPARE(DiskFile::encode(file.text, file.convention), original);

    // A second BOM is text, not a convention, and must survive.
    const QByteArray doubled = QByteArray("\xEF\xBB\xBF\xEF\xBB\xBFx\n");
    writeFile(path, doubled);
    const LoadedFile twice = DiskFile::load(path);
    QVERIFY(!twice.readOnly);
    QCOMPARE(DiskFile::encode(twice.text, twice.convention), doubled);

    writeFile(path, "a\nb\nc\r\n");
    QCOMPARE(DiskFile::load(path).convention.newline, QStringLiteral("\n"));
    writeFile(path, "no breaks");
    QCOMPARE(DiskFile::load(path).convention.newline, QStringLiteral("\n"));
}

void ParchTests::invalidUtf8OpensReadOnlyWithBytesKept()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, QByteArray("ok \xC3\x28 bad\n"));
    const LoadedFile bad = DiskFile::load(path);
    QVERIFY(bad.ok);
    QVERIFY(bad.readOnly);
    QCOMPARE(bad.originalBytes, QByteArray("ok \xC3\x28 bad\n"));
    QVERIFY(bad.text.contains(QChar::ReplacementCharacter));
}

void ParchTests::saveRefusesWhenDiskChanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "original\n");
    const DiskVersion opened = DiskFile::fingerprint(path);
    writeFile(path, "outside edit\n");
    const SaveResult refused = DiskFile::save(path, "mine\n", opened);
    QCOMPARE(refused.status, SaveStatus::Conflict);
    QCOMPARE(readFile(path), QByteArray("outside edit\n"));
}

void ParchTests::conflictLeavesNoTemporaryFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "original\n");
    const DiskVersion opened = DiskFile::fingerprint(path);
    writeFile(path, "outside edit\n");
    QCOMPARE(DiskFile::save(path, "mine\n", opened).status, SaveStatus::Conflict);
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ParchTests::saveKeepsPermissionsAndSymlink()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath("target.md"), link = dir.filePath("link.md");
    writeFile(target, "old\n");
    QVERIFY(QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner));
    QVERIFY(QFile::link(target, link));
    QCOMPARE(DiskFile::save(link, "new\n", DiskFile::fingerprint(link)).status, SaveStatus::Saved);
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(readFile(target), QByteArray("new\n"));
    // The POSIX mode is checked directly because Qt's permission flags use their own bit layout.
    struct stat st {};
    QCOMPARE(::stat(qPrintable(target), &st), 0);
    QCOMPARE(int(st.st_mode & 0777), 0600);

    // A linked file written in place is reached through the link, which stays one.
    const QString other = dir.filePath("other.md");
    QCOMPARE(::link(qPrintable(target), qPrintable(other)), 0);
    QCOMPARE(::stat(qPrintable(target), &st), 0);
    const DiskFile::KeepCurrent keep = [](const QByteArray &, QString *) { return true; };
    QCOMPARE(DiskFile::save(link, "newer\n", DiskFile::fingerprint(link), keep).status, SaveStatus::Saved);
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(readFile(other), QByteArray("newer\n"));
    struct stat after {};
    QCOMPARE(::stat(qPrintable(target), &after), 0);
    QCOMPARE(after.st_ino, st.st_ino);
}

void ParchTests::saveWritesAHardLinkedFileInPlace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath("a.md"), second = dir.filePath("b.md");
    writeFile(first, "old text\n");
    QCOMPARE(::link(qPrintable(first), qPrintable(second)), 0);
    struct stat before {};
    QCOMPARE(::stat(qPrintable(first), &before), 0);
    QByteArrayList kept;
    const DiskFile::KeepCurrent keep = [&kept](const QByteArray &current, QString *) {
        kept.append(current);
        return true;
    };
    // Both names show the new text, which is in the same file as before,
    // and the text it replaced was kept first.
    const SaveResult saved = DiskFile::save(first, "new\n", DiskFile::fingerprint(first), keep);
    QCOMPARE(saved.status, SaveStatus::Saved);
    QCOMPARE(readFile(first), QByteArray("new\n"));
    QCOMPARE(readFile(second), QByteArray("new\n"));
    struct stat after {};
    QCOMPARE(::stat(qPrintable(first), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(int(after.st_nlink), 2);
    QCOMPARE(kept, QByteArrayList{"old text\n"});
    QCOMPARE(saved.version, DiskFile::fingerprint(first));

    // Nothing is written in place unless the current text is kept first.
    const SaveResult unkept = DiskFile::save(first, "lost\n", DiskFile::fingerprint(first));
    QCOMPARE(unkept.status, SaveStatus::Failed);
    QVERIFY2(unkept.error.contains(QStringLiteral("Version history")), qPrintable(unkept.error));
    const SaveResult refused = DiskFile::save(first, "lost\n", DiskFile::fingerprint(first),
                                              [](const QByteArray &, QString *reason) {
        *reason = QStringLiteral("no room");
        return false;
    });
    QCOMPARE(refused.status, SaveStatus::Failed);
    QVERIFY2(refused.error.contains(QStringLiteral("no room")), qPrintable(refused.error));
    QCOMPARE(readFile(second), QByteArray("new\n"));
    // A file changed since its version was taken is not written over.
    const DiskVersion stale = DiskFile::fingerprint(first);
    writeFile(second, "theirs\n");
    QCOMPARE(DiskFile::save(first, "mine\n", stale, keep).status, SaveStatus::Conflict);
    QCOMPARE(readFile(first), QByteArray("theirs\n"));

    // A write that fails says that the earlier text is in history.
    if (::geteuid() != 0) {
        QVERIFY(QFile::setPermissions(first, QFile::ReadOwner));
        const SaveResult failed = DiskFile::save(first, "mine\n", DiskFile::fingerprint(first), keep);
        QVERIFY(QFile::setPermissions(first, QFile::ReadOwner | QFile::WriteOwner));
        QCOMPARE(failed.status, SaveStatus::Failed);
        QVERIFY2(failed.error.contains(QStringLiteral("Version history")), qPrintable(failed.error));
        QCOMPARE(kept.last(), QByteArray("theirs\n"));
        QCOMPARE(readFile(first), QByteArray("theirs\n"));
    }

    // Through the store, the text the file was opened with is kept before the
    // first save writes over it in place; parch's own saves are not kept
    // again before the next one, and follow history's interval.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    Document *document = store.open(first);
    QVERIFY(document);
    for (const QString &text : {QStringLiteral("one\n"), QStringLiteral("two\n"), QStringLiteral("three\n")}) {
        document->buffer()->replace({0, document->buffer()->length()}, text);
        store.save(document);
        store.waitForSaves();
        QCOMPARE(document->state(), Document::State::Clean);
    }
    QCOMPARE(readFile(second), QByteArray("three\n"));
    QCOMPARE(::stat(qPrintable(first), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    const QStringList texts = versionTexts(store.history(), document->id());
    QCOMPARE(texts, (QStringList{QStringLiteral("one\n"), QStringLiteral("theirs\n")}));
}

void ParchTests::inPlaceSavesKeepNoMoreVersionsThanOthers()
{
    // Saved twenty times over, a file with a second name keeps no more
    // versions than a file without one, and the text it was opened with
    // stays among them.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString linked = root.filePath("linked.md"), plain = root.filePath("plain.md");
    writeFile(linked, "as opened\n");
    writeFile(plain, "as opened\n");
    QCOMPARE(::link(qPrintable(linked), qPrintable(root.filePath("other name.md"))), 0);
    DocumentStore store(paths);
    QList<Document *> documents{store.open(linked), store.open(plain)};
    QVERIFY(documents.at(0) && documents.at(1));
    struct stat before {};
    QCOMPARE(::stat(qPrintable(linked), &before), 0);
    for (int i = 0; i < 20; ++i) {
        for (Document *document : std::as_const(documents)) {
            document->buffer()->replace({0, 0}, QStringLiteral("%1 ").arg(i));
            store.save(document);
        }
        store.waitForSaves();
        for (Document *document : std::as_const(documents)) QCOMPARE(document->state(), Document::State::Clean);
    }
    struct stat after {};
    QCOMPARE(::stat(qPrintable(linked), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    const auto versions = [&](Document *document) { return store.history()->versions(document->id()); };
    QVERIFY2(versions(documents.at(0)).size() <= versions(documents.at(1)).size(),
             qPrintable(QStringLiteral("%1 against %2").arg(versions(documents.at(0)).size()).arg(versions(documents.at(1)).size())));
    QVERIFY(versionTexts(store.history(), documents.at(0)->id()).contains(QStringLiteral("as opened\n")));
    bool opened = false;
    for (const History::Version &version : versions(documents.at(0))) opened |= version.reason == History::Reason::Opened;
    QVERIFY(opened);
    // With history paused, what parch wrote itself is still written over in
    // place, since nothing in it needs keeping.
    const QString full = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString versionId = QStringLiteral("20200101T000000000-0123456789ab");
    writeFile(paths.history() + u'/' + full + u'/' + versionId + QStringLiteral(".md"), "big");
    writeFile(paths.history() + u'/' + full + u'/' + versionId + QStringLiteral(".name"), "Kept");
    QJsonObject entry{{QStringLiteral("id"), versionId},
                      {QStringLiteral("taken"), QStringLiteral("2020-01-01T00:00:00.000Z")},
                      {QStringLiteral("name"), QStringLiteral("Kept")},
                      {QStringLiteral("bytes"), double(qint64(2) << 30)}};
    writeFile(paths.history() + u'/' + full + QStringLiteral("/index.json"), QJsonDocument(QJsonArray{entry}).toJson());
    QCOMPARE(store.history()->versions(full).size(), 1);
    QVERIFY(store.history()->pause() == History::Pause::NamedVersionsFill);
    documents.at(0)->buffer()->replace({0, 0}, QStringLiteral("paused "));
    store.save(documents.at(0));
    store.waitForSaves();
    QCOMPARE(documents.at(0)->state(), Document::State::Clean);
    QVERIFY(readFile(root.filePath("other name.md")).startsWith("paused "));
}

void ParchTests::saveCarriesAttributesAndMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "old\n");
    // A setgid file in another of the process's groups than its own, when there is one.
    gid_t groups[256];
    const int count = ::getgroups(256, groups);
    gid_t group = ::getegid();
    for (int i = 0; i < count; ++i)
        if (groups[i] != ::getegid()) group = groups[i];
    QCOMPARE(::chown(qPrintable(path), uid_t(-1), group), 0);
    QCOMPARE(::chmod(qPrintable(path), 02640), 0);
    struct stat before {};
    QCOMPARE(::stat(qPrintable(path), &before), 0);
    if (::setxattr(qPrintable(path), "user.parch.test", "kept", 4, 0) != 0)
        QSKIP("This file system takes no extended attributes");
    QCOMPARE(DiskFile::save(path, "new\n", DiskFile::fingerprint(path)).status, SaveStatus::Saved);
    QCOMPARE(readFile(path), QByteArray("new\n"));
    struct stat after {};
    QCOMPARE(::stat(qPrintable(path), &after), 0);
    // A new file took the old one's place, with all that made it what it was.
    QVERIFY(after.st_ino != before.st_ino);
    QCOMPARE(after.st_gid, before.st_gid);
    QCOMPARE(int(after.st_mode & 07777), int(before.st_mode & 07777));
    char value[16] = {};
    QCOMPARE(int(::getxattr(qPrintable(path), "user.parch.test", value, sizeof value)), 4);
    QCOMPARE(QByteArray(value), QByteArray("kept"));
}

void ParchTests::anAttributeThatCannotBeCopiedKeepsTheFile()
{
    // An attribute the new file cannot take, as a security module or a
    // network file system can refuse one, leaves the file as it is and has
    // the text written into it, with every attribute it had.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "old\n");
    if (::setxattr(qPrintable(path), "user.parch.test", "kept", 4, 0) != 0)
        QSKIP("This file system takes no extended attributes");
    struct stat before {};
    QCOMPARE(::stat(qPrintable(path), &before), 0);
    QByteArrayList kept;
    const DiskFile::KeepCurrent keep = [&kept](const QByteArray &current, QString *) {
        kept.append(current);
        return true;
    };
    DiskFile::failAttributeCopies(EINVAL);
    const auto restore = qScopeGuard([] { DiskFile::failAttributeCopies(0); });
    const SaveResult saved = DiskFile::save(path, "new\n", DiskFile::fingerprint(path), keep);
    QVERIFY2(saved.status == SaveStatus::Saved, qPrintable(saved.error));
    QCOMPARE(readFile(path), QByteArray("new\n"));
    struct stat after {};
    QCOMPARE(::stat(qPrintable(path), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    char value[16] = {};
    QCOMPARE(int(::getxattr(qPrintable(path), "user.parch.test", value, sizeof value)), 4);
    QCOMPARE(QByteArray(value), QByteArray("kept"));
    QCOMPARE(kept, QByteArrayList{"old\n"});
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ParchTests::saveWritesAnotherUsersFileInPlace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "old\n");
    struct stat before {};
    QCOMPARE(::stat(qPrintable(path), &before), 0);
    QByteArrayList kept;
    const DiskFile::KeepCurrent keep = [&kept](const QByteArray &current, QString *) {
        kept.append(current);
        return true;
    };
    // Saved as another user, the file would lose its owner to a new file.
    DiskFile::setSavingUser(qint64(::geteuid()) + 1);
    const auto restore = qScopeGuard([] { DiskFile::setSavingUser(-1); });
    QCOMPARE(DiskFile::save(path, "new\n", DiskFile::fingerprint(path), keep).status, SaveStatus::Saved);
    struct stat after {};
    QCOMPARE(::stat(qPrintable(path), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(kept, QByteArrayList{"old\n"});
    QCOMPARE(readFile(path), QByteArray("new\n"));
    // As the file's owner, a new file takes its place.
    DiskFile::setSavingUser(-1);
    QCOMPARE(DiskFile::save(path, "newer\n", DiskFile::fingerprint(path), keep).status, SaveStatus::Saved);
    QCOMPARE(::stat(qPrintable(path), &after), 0);
    QVERIFY(after.st_ino != before.st_ino);
    QCOMPARE(kept.size(), 1);
}

void ParchTests::saveFailureLeavesTargetAndNoLitter()
{
    if (::geteuid() == 0) QSKIP("A read-only directory does not stop root.");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString directory = dir.path(), path = dir.filePath("a.md");
    writeFile(path, "old\n");
    const DiskVersion opened = DiskFile::fingerprint(path);
    QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::ExeOwner));
    const SaveResult failed = DiskFile::save(path, "new\n", opened);
    // Write permission comes back first so that the temporary directory can be removed.
    QVERIFY(QFile::setPermissions(directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QCOMPARE(failed.status, SaveStatus::Failed);
    QVERIFY(!failed.error.isEmpty());
    QCOMPARE(readFile(path), QByteArray("old\n"));
    QCOMPARE(QDir(directory).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

void ParchTests::missingFileSavesOnlyWhileStillMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("new.md");
    const DiskVersion missing = DiskFile::fingerprint(path);
    QVERIFY(!missing.exists);
    QVERIFY(!missing.canonicalPath.isEmpty());
    QCOMPARE(DiskFile::save(path, "first\n", missing).status, SaveStatus::Saved);
    QCOMPARE(readFile(path), QByteArray("first\n"));
    QCOMPARE(DiskFile::save(path, "second\n", missing).status, SaveStatus::Conflict);
    QCOMPARE(readFile(path), QByteArray("first\n"));
}

void ParchTests::createNeverFollowsOrReplaces()
{
    QTemporaryDir dir, elsewhere;
    QVERIFY(dir.isValid() && elsewhere.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    const QString path = folder + "/new.md", target = elsewhere.filePath("target");
    // A link that leads nowhere takes the name.
    QVERIFY(QFile::link(target, path));
    QCOMPARE(DiskFile::literalPath(path), path);
    QCOMPARE(DiskFile::create(path, "text\n").status, SaveStatus::Conflict);
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(QFile::remove(path));
    // So does a file, which is left as it is.
    writeFile(path, "theirs\n");
    QCOMPARE(DiskFile::create(path, "text\n").status, SaveStatus::Conflict);
    QCOMPARE(readFile(path), QByteArray("theirs\n"));
    QVERIFY(QFile::remove(path));
    // A free name is made, and nothing else is left in the folder.
    const SaveResult made = DiskFile::create(path, "text\n");
    QCOMPARE(made.status, SaveStatus::Saved);
    QCOMPARE(readFile(path), QByteArray("text\n"));
    QCOMPARE(made.version, DiskFile::fingerprint(path));
    QCOMPARE(QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 1);
}

void ParchTests::createWithoutRenameOrLinks()
{
    // On a file system that can neither rename without replacing nor make a
    // second link, the file is made at its name, only while nothing has it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    const QString path = folder + "/new.md", taken = folder + "/taken.md";
    writeFile(taken, "theirs\n");
    DiskFile::setRenameWithoutReplacing(false);
    DiskFile::setLinking(false);
    const auto restore = qScopeGuard([] {
        DiskFile::setRenameWithoutReplacing(true);
        DiskFile::setLinking(true);
    });
    const SaveResult made = DiskFile::create(path, "text\n");
    QVERIFY2(made.status == SaveStatus::Saved, qPrintable(made.error));
    QCOMPARE(readFile(path), QByteArray("text\n"));
    QCOMPARE(made.version, DiskFile::fingerprint(path));
    QCOMPARE(DiskFile::create(taken, "mine\n").status, SaveStatus::Conflict);
    QCOMPARE(readFile(taken), QByteArray("theirs\n"));
    QCOMPARE(QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 2);
}

void ParchTests::aFailedDirectWriteRemovesOnlyItsOwnFile()
{
    // A file made at its name directly, whose write fails, is removed; but
    // only while the name still holds that very file. One put there since
    // stays.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString folder = QFileInfo(dir.path()).canonicalFilePath();
    const QString path = folder + "/new.md", other = folder + "/other.md";
    DiskFile::setRenameWithoutReplacing(false);
    DiskFile::setLinking(false);
    const auto restore = qScopeGuard([] {
        DiskFile::setRenameWithoutReplacing(true);
        DiskFile::setLinking(true);
        DiskFile::setDirectWriteFailure({});
    });
    // Its own file goes.
    DiskFile::setDirectWriteFailure([](const QString &) { return EIO; });
    SaveResult made = DiskFile::create(path, "text\n");
    QCOMPARE(made.status, SaveStatus::Failed);
    QVERIFY(!QFileInfo::exists(path));
    // Another put at the name meanwhile stays.
    writeFile(other, "theirs\n");
    DiskFile::setDirectWriteFailure([&](const QString &name) {
        ::rename(QFile::encodeName(other).constData(), QFile::encodeName(name).constData());
        return EIO;
    });
    made = DiskFile::create(path, "text\n");
    QCOMPARE(made.status, SaveStatus::Failed);
    QCOMPARE(readFile(path), QByteArray("theirs\n"));
    QCOMPARE(QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList{"new.md"});
}

void ParchTests::fifoDoesNotBlockLoadOrSave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("pipe.md");
    QVERIFY(::mkfifo(qPrintable(path), 0600) == 0);
    // If a call blocks on the open, this thread opens a writer after two seconds
    // and keeps it open until the calls are done, which frees every blocked open
    // so that a regression fails on the elapsed time instead of hanging.
    std::atomic<bool> done{false};
    std::thread releaser([&] {
        for (int i = 0; i < 40 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const int fd = done ? -1 : ::open(qPrintable(path), O_RDWR | O_NONBLOCK);
        while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (fd >= 0) ::close(fd);
    });
    QElapsedTimer timer;
    timer.start();
    const LoadedFile loaded = DiskFile::load(path);
    const DiskVersion version = DiskFile::fingerprint(path);
    const SaveResult saved = DiskFile::save(path, "x\n", version);
    const qint64 elapsed = timer.elapsed();
    done = true;
    releaser.join();
    QVERIFY2(elapsed < 1000, "a FIFO blocked the file functions");
    QVERIFY(!loaded.ok);
    QCOMPARE(saved.status, SaveStatus::Failed);
}

void ParchTests::savedVersionMatchesTheWrittenFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "old\n");
    const SaveResult saved = DiskFile::save(path, "new text\n", DiskFile::fingerprint(path));
    QCOMPARE(saved.status, SaveStatus::Saved);
    QCOMPARE(saved.version, DiskFile::fingerprint(path));
    QCOMPARE(saved.version.sha256, QCryptographicHash::hash(QByteArray("new text\n"), QCryptographicHash::Sha256));
    // The returned version lets the next save go through without a conflict.
    QCOMPARE(DiskFile::save(path, "again\n", saved.version).status, SaveStatus::Saved);
}

void ParchTests::aChangeOfTheSameSizeAndTimeIsSeen()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "original\n");
    // Once its times lie more than a second back, a stamp stands for it.
    QTest::qWait(1100);
    const DiskVersion read = DiskFile::fingerprint(path);
    struct stat before {};
    QCOMPARE(::stat(qPrintable(path), &before), 0);
    // While nothing touches the file, it is the version read, without reading it.
    const quint64 reads = DiskFile::contentReads();
    QCOMPARE(DiskFile::fingerprint(path, read), read);
    QCOMPARE(DiskFile::contentReads(), reads);
    // Written in place to the same size, its times put back: its ctime moved,
    // so it is read again and is another version.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int descriptor = ::open(qPrintable(path), O_WRONLY);
    QVERIFY(descriptor >= 0);
    QCOMPARE(int(::write(descriptor, "changed!\n", 9)), 9);
    ::close(descriptor);
    const struct timespec times[2] = {before.st_atim, before.st_mtim};
    QCOMPARE(::utimensat(AT_FDCWD, qPrintable(path), times, 0), 0);
    struct stat after {};
    QCOMPARE(::stat(qPrintable(path), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(after.st_size, before.st_size);
    QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
    QVERIFY(DiskFile::fingerprint(path, read) != read);
    QCOMPARE(DiskFile::save(path, "mine\n", read).status, SaveStatus::Conflict);
    QCOMPARE(readFile(path), QByteArray("changed!\n"));
}

void ParchTests::aStampTooCloseToItsTimesIsNotTrusted()
{
    // On a file system that keeps times in whole seconds, a write in the
    // second a file was read leaves its stamp as it was: until its times lie
    // 2 s back, the file is read again whenever it is looked at.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    DiskFile::setWholeSecondTimes(true);
    const auto restore = qScopeGuard([] { DiskFile::setWholeSecondTimes(false); });
    // Just after a second begins, so that all of this is in that second.
    QElapsedTimer waiting;
    waiting.start();
    while (QDateTime::currentMSecsSinceEpoch() % 1000 > 100 && waiting.elapsed() < 3000)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const qint64 second = QDateTime::currentMSecsSinceEpoch() / 1000;
    writeFile(path, "original\n");
    const DiskVersion read = DiskFile::fingerprint(path);
    const int descriptor = ::open(qPrintable(path), O_WRONLY);
    QVERIFY(descriptor >= 0);
    QCOMPARE(int(::write(descriptor, "changed!\n", 9)), 9);
    ::close(descriptor);
    const quint64 reads = DiskFile::contentReads();
    QVERIFY(DiskFile::fingerprint(path, read) != read);
    QCOMPARE(DiskFile::contentReads(), reads + 1);
    QCOMPARE(DiskFile::save(path, "mine\n", read).status, SaveStatus::Conflict);
    QCOMPARE(readFile(path), QByteArray("changed!\n"));
    if (QDateTime::currentMSecsSinceEpoch() / 1000 != second)
        QSKIP("The second turned while the file was being changed");
    // Once its times lie 2 s back, a file read again stands for itself.
    QTest::qWait(2100);
    const DiskVersion settled = DiskFile::fingerprint(path);
    const quint64 before = DiskFile::contentReads();
    QCOMPARE(DiskFile::fingerprint(path, settled), settled);
    QCOMPARE(DiskFile::contentReads(), before);
}

void ParchTests::aSaveReadsTheFileAtMostOnce()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, QByteArray(100000, 'a') + '\n');
    // A stamp stands for a file only once its times lie more than a second
    // back: a file written less than a second ago is read again, on the
    // worker, whenever it is looked at.
    QTest::qWait(1100);
    DocumentStore store(paths);
    Document *document = store.open(file);
    QVERIFY(document);
    QSignalSpy touched(&store, &DocumentStore::diskChanged);
    for (int i = 0; i < 3; ++i) {
        const quint64 before = DiskFile::contentReads();
        document->buffer()->replace({0, 0}, QStringLiteral("x"));
        store.save(document);
        store.waitForSaves();
        QCOMPARE(document->state(), Document::State::Clean);
        const quint64 saving = DiskFile::contentReads() - before;
        QVERIFY2(saving <= 1, qPrintable(QStringLiteral("save %1 read the file %2 times").arg(i).arg(saving)));
        // The look that parch's own rename brings on reads the file it has
        // just written once at most.
        QTRY_VERIFY(touched.count() > i);
        QTest::qWait(300);
        QCOMPARE(document->state(), Document::State::Clean);
        const quint64 looking = DiskFile::contentReads() - before - saving;
        QVERIFY2(looking <= 1, qPrintable(QStringLiteral("the look after save %1 read the file %2 times").arg(i).arg(looking)));
        QTest::qWait(1100);
    }
    // Looked at once more, the file stands for itself, and is not read again.
    const DiskVersion settled = DiskFile::fingerprint(file);
    const quint64 reads = DiskFile::contentReads();
    QCOMPARE(DiskFile::fingerprint(file, settled), settled);
    QCOMPARE(DiskFile::contentReads(), reads);
}

void ParchTests::aStampRecordedSoonAfterItsTimesIsNotTrusted()
{
    // A stamp taken half a second after its file was written does not stand
    // for the file, which is read again; one taken over a second after does.
    // Times in whole seconds need more than two. The moment each stamp is
    // taken is given, so that how busy the machine is changes nothing.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("a.md");
    writeFile(path, "text\n");
    struct stat state {};
    QCOMPARE(::stat(QFile::encodeName(path).constData(), &state), 0);
    const qint64 second = 1000000000;
    const qint64 written = std::max(qint64(state.st_mtim.tv_sec) * second + state.st_mtim.tv_nsec,
                                    qint64(state.st_ctim.tv_sec) * second + state.st_ctim.tv_nsec);
    const auto restore = qScopeGuard([] {
        DiskFile::setClock({});
        DiskFile::setWholeSecondTimes(false);
    });
    const auto stands = [&](qint64 taken) {
        DiskFile::setClock([taken] { return taken; });
        const DiskVersion version = DiskFile::fingerprint(path);
        const quint64 reads = DiskFile::contentReads();
        const bool same = DiskFile::fingerprint(path, version) == version;
        return same && DiskFile::contentReads() == reads;
    };
    QVERIFY(!stands(written + second / 2));
    QVERIFY(stands(written + second + second / 5));
    DiskFile::setWholeSecondTimes(true);
    const qint64 whole = written / second * second;
    QVERIFY(!stands(whole + 2 * second - second / 10));
    QVERIFY(stands(whole + 2 * second + second / 10));
}

void ParchTests::openingReadsTheFileOnce()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md"), other = root.filePath("b.md"), link = root.filePath("link.md");
    writeFile(file, QByteArray(1 << 20, 'a') + '\n');
    writeFile(other, "other\n");
    QVERIFY(QFile::link(file, link));
    const QString otherId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(Recovery(paths).write(otherId, other, SourceSnapshot{1, QStringLiteral("edited\n")},
                                  DiskFile::fingerprint(other)));
    DocumentStore store(paths);
    quint64 before = DiskFile::contentReads();
    Document *document = store.open(file);
    QVERIFY(document);
    QCOMPARE(DiskFile::contentReads() - before, quint64(1));
    // Opened again, through a link, it is found without reading it.
    before = DiskFile::contentReads();
    QCOMPARE(store.open(link), document);
    QCOMPARE(DiskFile::contentReads() - before, quint64(0));
    // A document restored from recovery reads its file once too.
    const QList<Recovery::Entry> entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 1);
    before = DiskFile::contentReads();
    Document *restored = store.restore(entries.first());
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Modified);
    QCOMPARE(DiskFile::contentReads() - before, quint64(1));
    // Naming asks only for the name here; the file there is read by the save.
    before = DiskFile::contentReads();
    store.saveAs(restored, root.filePath("c.md"));
    QCOMPARE(DiskFile::contentReads() - before, quint64(0));
    store.waitForSaves();
    QCOMPARE(restored->state(), Document::State::Clean);
    QCOMPARE(readFile(root.filePath("c.md")), QByteArray("edited\n"));
}

void ParchTests::danglingLinkSavesTwice()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString link = dir.filePath("link.md");
    QVERIFY(QFile::link("real.md", link)); // a relative target, which does not exist yet
    const SaveResult first = DiskFile::save(link, "one\n", DiskFile::fingerprint(link));
    QCOMPARE(first.status, SaveStatus::Saved);
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(readFile(dir.filePath("real.md")), QByteArray("one\n"));
    QCOMPARE(first.version, DiskFile::fingerprint(link));
    QCOMPARE(DiskFile::save(link, "two\n", first.version).status, SaveStatus::Saved);
    QCOMPARE(readFile(dir.filePath("real.md")), QByteArray("two\n"));
}

void ParchTests::danglingLinkThroughLinkedDirectoryKeepsUnrelatedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(QDir().mkpath(root + "/store/deep"));
    writeFile(root + "/draft.md", "PRECIOUS unrelated text");
    QVERIFY(QFile::link("store/deep", root + "/notes"));
    QVERIFY(QFile::link("../draft.md", root + "/store/deep/draft.md"));
    // The kernel resolves the link from its real directory, so ".." leaves
    // store/deep; read as text from notes/ it would reach the file above notes.
    const QString path = root + "/notes/draft.md";
    const SaveResult saved = DiskFile::save(path, "mine\n", DiskFile::fingerprint(path));
    QCOMPARE(saved.status, SaveStatus::Saved);
    QCOMPARE(readFile(root + "/draft.md"), QByteArray("PRECIOUS unrelated text"));
    QCOMPARE(readFile(root + "/store/draft.md"), QByteArray("mine\n"));
    QVERIFY(QFileInfo(root + "/store/deep/draft.md").isSymLink());
    QCOMPARE(saved.version, DiskFile::fingerprint(path));
}

void ParchTests::linkLoopFailsWithoutTouchingAnything()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath("a.md"), b = dir.filePath("b.md");
    QVERIFY(QFile::link("b.md", a));
    QVERIFY(QFile::link("a.md", b));
    const SaveResult saved = DiskFile::save(a, "x\n", DiskFile::fingerprint(a));
    QCOMPARE(saved.status, SaveStatus::Failed);
    QVERIFY(!saved.error.isEmpty());
    QVERIFY(QFileInfo(a).isSymLink());
    QVERIFY(QFileInfo(b).isSymLink());
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot).size(), 2);
    QVERIFY(!DiskFile::load(a).ok);
}

void ParchTests::dotDotInsideLinkTargetKeepsUnrelatedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(QDir().mkpath(root + "/D"));
    QVERIFY(QDir().mkpath(root + "/elsewhere/a"));
    writeFile(root + "/D/t.md", "PRECIOUS unrelated text");
    QVERIFY(QFile::link("../elsewhere/a", root + "/D/w"));
    // The kernel applies ".." to the real directory behind w, so the link
    // reaches elsewhere/t.md; read as text it would reach D/t.md.
    QVERIFY(QFile::link("w/../t.md", root + "/D/L.md"));
    const QString path = root + "/D/L.md";
    const SaveResult saved = DiskFile::save(path, "mine\n", DiskFile::fingerprint(path));
    QCOMPARE(saved.status, SaveStatus::Saved);
    QCOMPARE(readFile(root + "/D/t.md"), QByteArray("PRECIOUS unrelated text"));
    QCOMPARE(readFile(root + "/elsewhere/t.md"), QByteArray("mine\n"));
    QVERIFY(QFileInfo(path).isSymLink());
    QCOMPARE(saved.version, DiskFile::fingerprint(path));
}

void ParchTests::dotDotAfterSymlinkedDirectoryUsesKernelFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(QDir().mkpath(root + "/D"));
    QVERIFY(QDir().mkpath(root + "/elsewhere/a"));
    QVERIFY(QFile::link("../elsewhere/a", root + "/D/w"));
    writeFile(root + "/D/missing.md", "PRECIOUS missing");
    writeFile(root + "/D/present.md", "PRECIOUS present");
    writeFile(root + "/elsewhere/present.md", "kernel text\n");

    // The kernel file exists: its text is what loads, and it is what a save replaces.
    const QString present = root + "/D/w/../present.md";
    const LoadedFile loaded = DiskFile::load(present);
    QVERIFY(loaded.ok);
    QCOMPARE(loaded.text, QStringLiteral("kernel text\n"));
    const SaveResult first = DiskFile::save(present, "mine\n", loaded.version);
    QCOMPARE(first.status, SaveStatus::Saved);
    QCOMPARE(readFile(root + "/D/present.md"), QByteArray("PRECIOUS present"));
    QCOMPARE(readFile(root + "/elsewhere/present.md"), QByteArray("mine\n"));
    QCOMPARE(first.version, DiskFile::fingerprint(present));

    // The kernel file is missing: the save creates it and leaves the existing D file alone.
    const QString missing = root + "/D/w/../missing.md";
    QVERIFY(!DiskFile::fingerprint(missing).exists);
    const SaveResult second = DiskFile::save(missing, "mine\n", DiskFile::fingerprint(missing));
    QCOMPARE(second.status, SaveStatus::Saved);
    QCOMPARE(readFile(root + "/D/missing.md"), QByteArray("PRECIOUS missing"));
    QCOMPARE(readFile(root + "/elsewhere/missing.md"), QByteArray("mine\n"));
    QCOMPARE(second.version, DiskFile::fingerprint(missing));
}

void ParchTests::sameFileThroughSymlinkIsOneDocument()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath("notes/a.md"), link = root.filePath("link.md");
    writeFile(file, "text\n");
    QVERIFY(QFile::link(file, link));
    DocumentStore store(AppPaths::under(root.path()));
    Document *direct = store.open(file);
    QVERIFY(direct);
    QCOMPARE(store.open(link), direct);
    QCOMPARE(store.documents().size(), 1);
    QCOMPARE(direct->path(), QFileInfo(file).canonicalFilePath());
    QCOMPARE(direct->title(), QStringLiteral("a.md"));
    QVERIFY(!direct->isDraft());
    QCOMPARE(direct->state(), Document::State::Clean);

    QString error;
    QVERIFY(!store.open(root.filePath("missing.md"), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.documents().size(), 1);
}

void ParchTests::documentIdentitySurvivesRestart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath("a.md"), other = root.filePath("b.md"), link = root.filePath("link.md");
    writeFile(file, "a\n");
    writeFile(other, "b\n");
    QVERIFY(QFile::link(file, link));
    const AppPaths paths = AppPaths::under(root.path());
    QString id;
    {
        DocumentStore store(paths);
        Document *document = store.open(file);
        QVERIFY(document);
        id = document->id();
        QVERIFY(!QUuid::fromString(id).isNull());
        Document *second = store.open(other);
        QVERIFY(second);
        QVERIFY(second->id() != id);
    }
    DocumentStore again(paths);
    Document *reopened = again.open(link);
    QVERIFY(reopened);
    QCOMPARE(reopened->id(), id);
}

void ParchTests::draftCreatesNoFileUntilEdited()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    Document *draft = store.newDraft();
    QVERIFY(draft->isDraft());
    QVERIFY(draft->path().isEmpty());
    QCOMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(draft->title(), QStringLiteral("Untitled"));
    store.save(draft);
    QCOMPARE(draft->state(), Document::State::Clean);
    QVERIFY(!QDir(paths.drafts()).exists(draft->id()));

    draft->buffer()->replace({0, 0}, "hello");
    QCOMPARE(draft->state(), Document::State::Modified);
    QCOMPARE(draft->title(), QStringLiteral("hello"));
    store.save(draft);
    const QString file = paths.drafts() + "/" + draft->id() + "/draft.md";
    QTRY_VERIFY(QFile::exists(file));
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(readFile(file), QByteArray("hello"));

    // A draft is titled by its first line with text, without heading marks, and kept short.
    Document *titled = store.newDraft();
    titled->buffer()->replace({0, 0}, "\n   \n  ## " + QString(70, u'w') + "  \nbody");
    QCOMPARE(titled->title(), QString(60, u'w'));
}

void ParchTests::namingDraftKeepsTextUndoAndIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString target = root.filePath("named.md");
    DocumentStore store(paths);
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, "hello");
    store.save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    const QString before = draft->id();
    QVERIFY(QDir(paths.drafts()).exists(before));

    store.saveAs(draft, target);
    QCOMPARE(draft->state(), Document::State::Saving);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(draft->id(), before);
    QVERIFY(!draft->isDraft());
    QCOMPARE(draft->path(), QFileInfo(target).canonicalFilePath());
    QCOMPARE(draft->title(), QStringLiteral("named.md"));
    QCOMPARE(readFile(target), QByteArray("hello"));
    QCOMPARE(store.open(target), draft);
    draft->buffer()->undo();
    QCOMPARE(draft->buffer()->text(), QString());
    QVERIFY(!QDir(paths.drafts()).exists(before));

    // The named file keeps the draft's identity after a restart too.
    DocumentStore again(paths);
    Document *reopened = again.open(target);
    QVERIFY(reopened);
    QCOMPARE(reopened->id(), before);
}

void ParchTests::namingDraftCarriesAssetsAndRefusesCollisions()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    Document *draft = store.newDraft();
    const QString text = QStringLiteral("![a](assets/a.png) ![c](assets/img/c.png) ![z](assets/z.png)\n");
    draft->buffer()->replace({0, 0}, text);
    store.save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    const QString id = draft->id(), draftFolder = paths.drafts() + "/" + id;
    const QByteArray a("first image"), c("nested image"), z("last image"), otherBytes("an unrelated image");
    writeFile(draftFolder + "/assets/a.png", a);
    writeFile(draftFolder + "/assets/img/c.png", c);
    writeFile(draftFolder + "/assets/z.png", z);

    // Assets are placed in name order, so a.png and img/c.png are copied before z.png collides.
    const QString blocked = root.filePath("blocked"), targetAsset = blocked + "/assets/z.png";
    writeFile(targetAsset, otherBytes);
    store.saveAs(draft, blocked + "/doc.md");
    QTRY_COMPARE(draft->state(), Document::State::SaveFailed);
    QVERIFY(draft->isDraft());
    QVERIFY(draft->lastError().contains(QStringLiteral("z.png")));
    QCOMPARE(readFile(targetAsset), otherBytes);
    QVERIFY(!QFileInfo::exists(blocked + "/doc.md"));
    QVERIFY(!QFileInfo::exists(blocked + "/assets/a.png"));
    QVERIFY(!QFileInfo::exists(blocked + "/assets/img"));
    QCOMPARE(QDir(blocked + "/assets").entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList{"z.png"});
    QCOMPARE(readFile(draftFolder + "/draft.md"), text.toUtf8());
    QCOMPARE(readFile(draftFolder + "/assets/a.png"), a);

    // An identical file already in place is used as it is, and the draft folder goes last.
    const QString named = root.filePath("named");
    writeFile(named + "/assets/a.png", a);
    writeFile(named + "/assets/unrelated.png", otherBytes);
    struct stat existing {};
    QCOMPARE(::stat(qPrintable(named + "/assets/a.png"), &existing), 0);
    store.saveAs(draft, named + "/doc.md");
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QVERIFY(!draft->isDraft());
    QCOMPARE(draft->id(), id);
    QVERIFY(draft->lastError().isEmpty());
    QCOMPARE(readFile(named + "/doc.md"), text.toUtf8());
    QCOMPARE(readFile(named + "/assets/a.png"), a);
    QCOMPARE(readFile(named + "/assets/img/c.png"), c);
    QCOMPARE(readFile(named + "/assets/z.png"), z);
    QCOMPARE(readFile(named + "/assets/unrelated.png"), otherBytes);
    struct stat reused {};
    QCOMPARE(::stat(qPrintable(named + "/assets/a.png"), &reused), 0);
    QCOMPARE(reused.st_ino, existing.st_ino);
    QVERIFY(!QDir(paths.drafts()).exists(id));
}

void ParchTests::aLinkAtAssetsReceivesNothing()
{
    // An "assets" beside the new name that is not a folder of its own, such
    // as a link to one elsewhere, is never written into: the draft is named,
    // its assets stay in its own folder, and that is said.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("![a](assets/a.png)\n"));
    store.save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    const QString draftFolder = paths.drafts() + u'/' + draft->id();
    writeFile(draftFolder + QStringLiteral("/assets/a.png"), "image");
    const QString named = root.filePath("named");
    QVERIFY(QDir().mkpath(named));
    QVERIFY(QFile::link(elsewhere.path(), named + QStringLiteral("/assets")));
    QSignalSpy kept(&store, &DocumentStore::assetsKept);
    store.saveAs(draft, named + QStringLiteral("/doc.md"));
    store.waitForSaves();
    QCOMPARE(draft->state(), Document::State::Clean);
    QVERIFY(!draft->isDraft());
    QCOMPARE(readFile(named + QStringLiteral("/doc.md")), QByteArray("![a](assets/a.png)\n"));
    QVERIFY(QDir(elsewhere.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    QCOMPARE(readFile(draftFolder + QStringLiteral("/assets/a.png")), QByteArray("image"));
    QCOMPARE(kept.count(), 1);
    QCOMPARE(kept.first().at(1).toString(), draftFolder + QStringLiteral("/assets"));
}

void ParchTests::assetsGoThroughTheFoldersOpened()
{
    // A draft's asset is written through the folder parch opened for it,
    // never by a path that a link put on the way meanwhile could lead
    // elsewhere: here "assets" is swapped for a link just before the asset
    // is written, and nothing reaches the link's folder.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("![a](assets/a.png)\n"));
    store.save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    const QString draftFolder = paths.drafts() + u'/' + draft->id();
    writeFile(draftFolder + QStringLiteral("/assets/a.png"), "image");
    const QString named = QFileInfo(root.path()).canonicalFilePath() + QStringLiteral("/named");
    QVERIFY(QDir().mkpath(named));
    // On the save's worker, so with no test macros.
    QStringList placing;
    DocumentStore::setBeforePlacing([&](const QString &path) {
        placing << path;
        QFile::rename(named + QStringLiteral("/assets"), named + QStringLiteral("/moved"));
        QFile::link(elsewhere.path(), named + QStringLiteral("/assets"));
    });
    const auto restore = qScopeGuard([] { DocumentStore::setBeforePlacing({}); });
    store.saveAs(draft, named + QStringLiteral("/doc.md"));
    store.waitForSaves();
    DocumentStore::setBeforePlacing({});
    QCOMPARE(placing, QStringList{named + QStringLiteral("/assets/a.png")});
    QVERIFY(QDir(elsewhere.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    QCOMPARE(readFile(named + QStringLiteral("/moved/a.png")), QByteArray("image"));
    QCOMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(readFile(named + QStringLiteral("/doc.md")), QByteArray("![a](assets/a.png)\n"));
}

void ParchTests::failedSaveStaysVisibleAndRetryable()
{
    if (::geteuid() == 0) QSKIP("A read-only directory does not stop root.");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = root.filePath("docs"), file = folder + "/a.md";
    writeFile(file, "original\n");
    DocumentStore store(AppPaths::under(root.path()));
    Document *document = store.open(file);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, "edited ");
    const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    // Write permission comes back on every exit so that the temporary directory can be removed.
    const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
    QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::SaveFailed);
    QVERIFY(!document->lastError().isEmpty());
    QCOMPARE(readFile(file), QByteArray("original\n"));

    // The failure stays visible while writing goes on, and nothing is lost.
    document->buffer()->replace({0, 0}, "more ");
    QCOMPARE(document->state(), Document::State::SaveFailed);
    const QString edited = document->buffer()->text();
    QCOMPARE(edited, QStringLiteral("more edited original\n"));

    QVERIFY(QFile::setPermissions(folder, writable));
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QVERIFY(document->lastError().isEmpty());
    QCOMPARE(readFile(file), edited.toUtf8());
}

void ParchTests::lastViewClosingKeepsUnsavedDocument()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath("a.md"), other = root.filePath("b.md");
    writeFile(file, "a\n");
    writeFile(other, "b\n");
    DocumentStore store(AppPaths::under(root.path()));
    QSignalSpy closed(&store, &DocumentStore::documentClosed);

    Document *document = store.open(file);
    QVERIFY(document);
    store.retain(document);
    document->buffer()->replace({0, 0}, "x");
    store.release(document);
    QVERIFY(store.documents().contains(document));
    QCOMPARE(closed.size(), 0);

    // A clean document closes with its last view, and not before.
    Document *clean = store.open(other);
    QVERIFY(clean);
    const QString cleanId = clean->id();
    store.retain(clean);
    store.retain(clean);
    store.release(clean);
    QVERIFY(store.documents().contains(clean));
    store.release(clean);
    QVERIFY(!store.documents().contains(clean));
    QCOMPARE(closed.size(), 1);
    QCOMPARE(closed.first().first().toString(), cleanId);

    // The unsaved document, which no view shows any more, closes once it is saved.
    const QString id = document->id();
    store.save(document);
    QTRY_VERIFY(!store.documents().contains(document));
    QCOMPARE(closed.size(), 2);
    QCOMPARE(closed.last().first().toString(), id);
    QCOMPARE(readFile(file), QByteArray("xa\n"));
}

void ParchTests::draftReopensByIdentity()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QString id;
    {
        DocumentStore store(paths);
        Document *draft = store.newDraft();
        draft->buffer()->replace({0, 0}, "# Plans\r\nsecond");
        store.save(draft);
        QTRY_COMPARE(draft->state(), Document::State::Clean);
        id = draft->id();
        QCOMPARE(store.openDraft(id), draft);
    }
    DocumentStore again(paths);
    Document *restored = again.openDraft(id);
    QVERIFY(restored);
    QCOMPARE(restored->id(), id);
    QVERIFY(restored->isDraft());
    QVERIFY(restored->path().isEmpty());
    QCOMPARE(restored->buffer()->text(), QStringLiteral("# Plans\r\nsecond"));
    QCOMPARE(restored->state(), Document::State::Clean);
    QCOMPARE(restored->title(), QStringLiteral("Plans"));
    QCOMPARE(restored->newline(), QStringLiteral("\r\n"));
    QCOMPARE(again.openDraft(id), restored);
    QCOMPARE(again.documents().size(), 1);

    // Saving the restored draft replaces the same file.
    restored->buffer()->replace({0, 0}, "x");
    again.save(restored);
    QTRY_COMPARE(restored->state(), Document::State::Clean);
    QCOMPARE(readFile(paths.drafts() + "/" + id + "/draft.md"), QByteArray("x# Plans\r\nsecond"));

    QVERIFY(!again.openDraft(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    // An id is never used as a path, so text from a damaged session cannot reach outside the drafts.
    writeFile(root.filePath("outside/draft.md"), "outside\n");
    QVERIFY(!again.openDraft(QStringLiteral("../../outside")));
    QCOMPARE(again.documents().size(), 1);
}

void ParchTests::saveFinishingAfterNewEditsStaysModified()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath("a.md");
    writeFile(file, "one\n");
    QList<Document::State> seen;
    DocumentStore store(AppPaths::under(root.path()));
    Document *document = store.open(file);
    QVERIFY(document);
    connect(document, &Document::changed, this, [&] { seen.append(document->state()); });

    // The result is delivered on this thread, so an edit made now is always newer than the save.
    document->buffer()->replace({0, 0}, "a");
    store.save(document);
    QCOMPARE(document->state(), Document::State::Saving);
    document->buffer()->replace({1, 0}, "b");
    QTRY_COMPARE(document->state(), Document::State::Modified);
    QCOMPARE(readFile(file), QByteArray("aone\n"));

    // Requests made while a save runs become one more save of the newest text, never an overlapping one.
    seen.clear();
    store.save(document);
    document->buffer()->replace({2, 0}, "c");
    store.save(document);
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(file), QByteArray("abcone\n"));
    QCOMPARE(seen, (QList<Document::State>{Document::State::Saving, Document::State::Clean}));
}

void ParchTests::conflictAndReadOnlyNeverWrite()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath("a.md"), invalid = root.filePath("bad.md");
    writeFile(file, "one\n");
    writeFile(invalid, QByteArray("ok \xC3\x28 bad\n"));
    DocumentStore store(AppPaths::under(root.path()));

    Document *document = store.open(file);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::Conflict);
    QVERIFY(!document->lastError().isEmpty());
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine one\n"));
    QCOMPARE(readFile(file), QByteArray("theirs\n"));
    store.save(document);
    QCOMPARE(document->state(), Document::State::Conflict);

    Document *readOnly = store.open(invalid);
    QVERIFY(readOnly);
    QCOMPARE(readOnly->state(), Document::State::ReadOnly);
    readOnly->buffer()->replace({0, 0}, "x");
    store.save(readOnly);
    QCOMPARE(readOnly->state(), Document::State::ReadOnly);
    QCOMPARE(readFile(invalid), QByteArray("ok \xC3\x28 bad\n"));
}

void ParchTests::openDuringNamingReturnsTheNamingDocument()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString target = root.filePath("t.md");
    writeFile(target, "old\n");
    DocumentStore store(AppPaths::under(root.path()));
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, "draft text\n");
    store.saveAs(draft, target);
    // A name and an open of the same file can arrive in one turn of the event loop.
    QCOMPARE(store.open(target), draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(draft->path(), QFileInfo(target).canonicalFilePath());
    QCOMPARE(store.open(target), draft);
    QCOMPARE(store.documents().size(), 1);
    QCOMPARE(readFile(target), QByteArray("draft text\n"));
}

void ParchTests::twoDocumentsCannotTakeOneName()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString target = root.filePath("shared.md");
    DocumentStore store(paths);
    const auto owners = [&store](const QString &path) {
        int count = 0;
        for (Document *document : store.documents()) count += document->path() == path;
        return count;
    };

    Document *first = store.newDraft();
    Document *second = store.newDraft();
    first->buffer()->replace({0, 0}, "first\n");
    second->buffer()->replace({0, 0}, "second\n");
    store.saveAs(first, target);
    store.saveAs(second, target);
    QCOMPARE(second->state(), Document::State::SaveFailed);
    QVERIFY(!second->lastError().isEmpty());
    QTRY_COMPARE(first->state(), Document::State::Clean);
    QVERIFY(second->isDraft());
    QCOMPARE(readFile(target), QByteArray("first\n"));
    QCOMPARE(owners(first->path()), 1);

    // A name requested while a save runs is checked when it starts, so a
    // name another document took in the meantime is refused then.
    const QString later = root.filePath("later.md");
    Document *waiting = store.newDraft();
    Document *quick = store.newDraft();
    waiting->buffer()->replace({0, 0}, "waiting\n");
    quick->buffer()->replace({0, 0}, "quick\n");
    store.save(waiting);
    store.saveAs(waiting, later);
    store.saveAs(quick, later);
    QTRY_COMPARE(quick->state(), Document::State::Clean);
    QTRY_COMPARE(waiting->state(), Document::State::SaveFailed);
    QVERIFY(waiting->isDraft());
    QCOMPARE(readFile(later), QByteArray("quick\n"));
    QCOMPARE(owners(quick->path()), 1);

    // A naming that fails gives the name back.
    const QString blocked = root.filePath("blocked"), blockedTarget = blocked + "/doc.md";
    Document *withAsset = store.newDraft();
    withAsset->buffer()->replace({0, 0}, "![a](assets/a.png)\n");
    store.save(withAsset);
    QTRY_COMPARE(withAsset->state(), Document::State::Clean);
    writeFile(paths.drafts() + "/" + withAsset->id() + "/assets/a.png", "mine");
    writeFile(blocked + "/assets/a.png", "theirs");
    store.saveAs(withAsset, blockedTarget);
    QTRY_COMPARE(withAsset->state(), Document::State::SaveFailed);
    store.saveAs(second, blockedTarget);
    QTRY_COMPARE(second->state(), Document::State::Clean);
    QCOMPARE(second->path(), QFileInfo(blockedTarget).canonicalFilePath());
    QVERIFY(withAsset->isDraft());
}

void ParchTests::reopenedDocumentOutlivesItsSave()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = root.filePath("a.md");
    writeFile(file, "a\n");
    DocumentStore store(AppPaths::under(root.path()));
    QSignalSpy closed(&store, &DocumentStore::documentClosed);

    // Handed out again while its save runs, a document is wanted again and
    // must not close under the caller that just received it.
    Document *document = store.open(file);
    QVERIFY(document);
    store.retain(document);
    document->buffer()->replace({0, 0}, "x");
    store.release(document);
    store.save(document);
    QPointer<Document> reopened = store.open(file);
    QCOMPARE(reopened.data(), document);
    QTRY_VERIFY(!reopened || reopened->state() == Document::State::Clean);
    QVERIFY(reopened);
    QVERIFY(store.documents().contains(document));

    Document *draft = store.newDraft();
    store.retain(draft);
    draft->buffer()->replace({0, 0}, "d");
    store.release(draft);
    store.save(draft);
    QPointer<Document> restored = store.openDraft(draft->id());
    QCOMPARE(restored.data(), draft);
    QTRY_VERIFY(!restored || restored->state() == Document::State::Clean);
    QVERIFY(restored);
    QVERIFY(store.documents().contains(draft));
    QCOMPARE(closed.size(), 0);

    // A view taken afterwards counts, so its release closes the clean document.
    store.retain(reopened);
    store.release(reopened);
    QVERIFY(!store.documents().contains(document));
    QCOMPARE(closed.size(), 1);
}

namespace {
// A regular file where the drafts folder belongs makes every draft save fail,
// even for root, so a draft stays unsaved for as long as a test needs.
void blockDrafts(const AppPaths &paths)
{
    writeFile(paths.drafts(), "in the way");
}
}

void ParchTests::recoveryRoundTripsAnyText()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Recovery recovery(paths);
    QStringList texts = sourceBoundaryFixtures();
    texts << QStringLiteral("line\r\nbreak\n");
    for (const QString &text : std::as_const(texts)) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString error;
        QVERIFY2(recovery.write(id, "/some/path.md", SourceSnapshot{7, text}, DiskVersion{}, &error), qPrintable(error));
        const auto entries = recovery.pending();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().documentId, id);
        QCOMPARE(entries.first().text, text);
        QCOMPARE(entries.first().path, QStringLiteral("/some/path.md"));
        QCOMPARE(entries.first().revision, Revision(7));
        QVERIFY(entries.first().base == DiskVersion{});
        QVERIFY(entries.first().hasBase);
        QVERIFY(entries.first().written.isValid());
        recovery.discard(id);
    }
    QVERIFY(recovery.pending().isEmpty());
    // A damaged file is skipped and the others still read.
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(recovery.write(id, {}, SourceSnapshot{1, "ok"}, DiskVersion{}));
    writeFile(paths.recovery() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".recovery", "garbage");
    QCOMPARE(recovery.pending().size(), 1);
}

void ParchTests::recoveryFollowsIdleAndMaximumIntervals()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    blockDrafts(paths);
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.newDraft();
    autosave.watch(document);
    document->buffer()->replace({0, 0}, "a");
    QTest::qWait(300);
    QVERIFY(recovery.pending().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!recovery.pending().isEmpty(), 700);
    const QDateTime first = recovery.pending().first().written;

    // Typing never pauses for the idle delay, so only the maximum interval can write.
    QElapsedTimer typing;
    typing.start();
    while (typing.elapsed() < 2600) {
        document->buffer()->replace({0, 0}, "b");
        QTest::qWait(100);
    }
    const auto entries = recovery.pending();
    QCOMPARE(entries.size(), 1);
    QVERIFY(entries.first().written > first);
    QVERIFY(entries.first().text.size() > 1);
}

void ParchTests::autosaveWritesAfterIdle()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("named.md");
    const QByteArray original("original\n");
    writeFile(file, original);
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *named = store.open(file);
    QVERIFY(named);
    autosave.watch(named);
    named->buffer()->replace({0, 0}, "x");
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("x") + original, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(recovery.pending().isEmpty(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(named->state(), Document::State::Clean, 2000);

    // A draft is saved to its own file, and then needs no recovery either.
    Document *draft = store.newDraft();
    autosave.watch(draft);
    draft->buffer()->replace({0, 0}, "draft");
    const QString draftFile = paths.drafts() + "/" + draft->id() + "/draft.md";
    QTRY_COMPARE_WITH_TIMEOUT(readFile(draftFile), QByteArray("draft"), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(recovery.pending().isEmpty(), 2000);
}

void ParchTests::draftWhoseFolderWentIsMadeAgain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *draft = store.newDraft();
    autosave.watch(draft);
    draft->buffer()->replace({0, 0}, "first");
    const QString folder = paths.drafts() + u'/' + draft->id();
    const QString file = folder + QStringLiteral("/draft.md");
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("first"), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(draft->state(), Document::State::Clean, 2000);

    // The folder goes, outside parch: the next save makes it again.
    QVERIFY(QDir(folder).removeRecursively());
    draft->buffer()->replace({5, 0}, " second");
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("first second"), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(draft->state(), Document::State::Clean, 2000);
    QCOMPARE(draft->statusText(), QStringLiteral("Saved"));
    // Only its file goes: it is made again too.
    QVERIFY(QFile::remove(file));
    draft->buffer()->replace({0, 0}, "third ");
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("third first second"), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(draft->state(), Document::State::Clean, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(recovery.pending().isEmpty(), 2000);
    // Made again as a draft's folder is made: for its owner alone.
    struct stat state {};
    QCOMPARE(::stat(qPrintable(folder), &state), 0);
    QCOMPARE(int(state.st_mode & 0777), 0700);
}

void ParchTests::crashRestoresUnsavedText()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    blockDrafts(paths);
    const QString unsaved = QString::fromUtf8("unsaved \xF0\x9F\x91\xA9 text\n");
    QString id;
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *document = store.newDraft();
        id = document->id();
        autosave.watch(document);
        document->buffer()->replace({0, 0}, unsaved);
        // The save cannot succeed, so the entry stays and the failure is visible.
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::SaveFailed, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.pending().isEmpty(), 1000);
        // Both go away together, without flush(), as in a crash.
    }
    const auto entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 1);
    DocumentStore second(paths);
    Document *restored = second.restore(entries.first());
    QVERIFY(restored);
    QCOMPARE(restored->id(), id);
    QVERIFY(restored->isDraft());
    QCOMPARE(restored->buffer()->text(), unsaved);
    QCOMPARE(restored->state(), Document::State::Modified);
    QVERIFY(!second.restore(entries.first()));
}

void ParchTests::cleanCloseFlushesAndClearsRecovery()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("named.md");
    writeFile(file, "one\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *named = store.open(file);
    Document *draft = store.newDraft();
    autosave.watch(named);
    autosave.watch(draft);
    named->buffer()->replace({0, 0}, "edited ");
    draft->buffer()->replace({0, 0}, "draft text");
    // The entries exist before the flush, so that their removal is what is shown.
    QVERIFY(recovery.write(named->id(), named->path(), *named->buffer()->snapshot(), named->diskVersion()));
    QVERIFY(recovery.write(draft->id(), {}, *draft->buffer()->snapshot(), draft->diskVersion()));
    QCOMPARE(Recovery(paths).pending().size(), 2);
    QString error;
    QVERIFY2(autosave.flush(&error), qPrintable(error));
    QVERIFY(Recovery(paths).pending().isEmpty());
    QCOMPARE(readFile(file), QByteArray("edited one\n"));
    QCOMPARE(readFile(paths.drafts() + "/" + draft->id() + "/draft.md"), QByteArray("draft text"));
    QCOMPARE(named->state(), Document::State::Clean);
    QCOMPARE(draft->state(), Document::State::Clean);
}

void ParchTests::recoveryWriteFailureIsReported()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    // A file where the recovery folder belongs cannot be written beneath, even by root.
    writeFile(paths.recovery(), "in the way");
    blockDrafts(paths);
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    QSignalSpy failures(&autosave, &Autosave::recoveryFailed);
    Document *document = store.newDraft();
    autosave.watch(document);
    document->buffer()->replace({0, 0}, "a");
    QTRY_COMPARE_WITH_TIMEOUT(failures.count(), 1, 1500);
    QCOMPARE(failures.first().first().toString(), document->id());
    QVERIFY(!failures.first().at(1).toString().isEmpty());
    QCOMPARE(document->buffer()->text(), QStringLiteral("a"));
    QString error;
    QVERIFY(!autosave.flush(&error));
    QVERIFY(!error.isEmpty());
}

void ParchTests::conflictKeepsRecoveryAndNeverSaves()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.open(file);
    autosave.watch(document);
    writeFile(file, "theirs\n");
    document->buffer()->replace({0, 0}, "edit ");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 2000);
    document->buffer()->replace({0, 0}, "more ");
    QTest::qWait(Autosave::SaveIdleMs + 500);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(readFile(file), QByteArray("theirs\n"));
    QTRY_COMPARE_WITH_TIMEOUT(recovery.pending().size(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(recovery.pending().first().text, QStringLiteral("more edit mine\n"), 1000);
}

void ParchTests::failedSaveIsNotClearedByAutosave()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    blockDrafts(paths);
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.newDraft();
    autosave.watch(document);
    document->buffer()->replace({0, 0}, "a");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::SaveFailed, 2000);
    const QString error = document->lastError();
    document->buffer()->replace({0, 0}, "b");
    QTest::qWait(Autosave::SaveIdleMs + 300);
    QCOMPARE(document->state(), Document::State::SaveFailed);
    QCOMPARE(document->lastError(), error);
}

void ParchTests::flushKeepsRecoveryForFailedSave()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    blockDrafts(paths);
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.newDraft();
    autosave.watch(document);
    document->buffer()->replace({0, 0}, "precious");
    QString error;
    // The text is kept in recovery, but a close must still warn that it is not in its file.
    QVERIFY(!autosave.flush(&error));
    QVERIFY(error.contains("kept in recovery"));
    QCOMPARE(document->state(), Document::State::SaveFailed);
    const auto entries = recovery.pending();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().text, QStringLiteral("precious"));
}

void ParchTests::restoredNamedFileSavesAndClearsRecovery()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "on disk\n");
    Recovery recovery(paths);
    QString id, canonical;
    {
        DocumentStore first(paths);
        Document *document = first.open(file);
        id = document->id();
        canonical = document->path();
    }
    QVERIFY(recovery.write(id, canonical, SourceSnapshot{3, "recovered\n"}, DiskFile::fingerprint(file)));
    DocumentStore store(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.restore(recovery.pending().first());
    QVERIFY(document);
    QCOMPARE(document->id(), id);
    QCOMPARE(document->path(), canonical);
    QCOMPARE(document->state(), Document::State::Modified);
    QCOMPARE(readFile(file), QByteArray("on disk\n"));
    autosave.watch(document);
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("recovered\n"), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(recovery.pending().isEmpty(), 2000);
}

namespace {
// An entry for a file as it was, holding text written after that version.
Recovery::Entry entryFor(const AppPaths &paths, DocumentStore &store, const QString &file, const QString &text)
{
    Document *document = store.open(file);
    Recovery recovery(paths);
    recovery.write(document->id(), document->path(), SourceSnapshot{5, text}, document->diskVersion());
    return recovery.pending().first();
}
}

void ParchTests::restoreAfterExternalEditWaitsInConflict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "local edit\n");
    }
    writeFile(file, "theirs\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("local edit\n"));
    autosave.watch(document);
    QTest::qWait(Autosave::SaveIdleMs + 750);
    QCOMPARE(readFile(file), QByteArray("theirs\n"));
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(recovery.pending().size(), 1);
}

void ParchTests::restoreOfDeletedFileWaitsInConflict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "local edit\n");
    }
    QVERIFY(QFile::remove(file));
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Conflict);
    autosave.watch(document);
    QTest::qWait(Autosave::SaveIdleMs + 750);
    QVERIFY(!QFileInfo::exists(file));
    QCOMPARE(recovery.pending().size(), 1);
}

void ParchTests::restoreOfUnreadableFileSaysWhyAndReadsAgain()
{
    if (::geteuid() == 0) QSKIP("Root reads a file whatever its mode.");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "local edit\n");
    }
    const auto readable = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
    const auto restoreMode = qScopeGuard([&] { QFile::setPermissions(file, readable); });
    QVERIFY(QFile::setPermissions(file, {}));
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    // The system cannot watch a file it cannot read, and Qt says so; its
    // folder is watched all the same.
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("inotify_add_watch\\(.*/a\\.md\\) failed")));
    Document *document = store.restore(entry);
    QVERIFY(document);
    // It is there, so it is not missing: it waits, saying why it cannot be read.
    QCOMPARE(document->state(), Document::State::Conflict);
    QVERIFY(!document->fileMissing());
    QVERIFY(document->fileUnreadable());
    QCOMPARE(document->statusText(), QStringLiteral("File unreadable"));
    const QString denied = QString::fromLocal8Bit(std::strerror(EACCES));
    // In the words of a file that cannot be opened: by its name and its folder's.
    QCOMPARE(document->lastError(), QStringLiteral("“a.md” in “%1” cannot be read (%2)")
                                        .arg(QFileInfo(root.path()).fileName(), denied));
    QCOMPARE(document->buffer()->text(), QStringLiteral("local edit\n"));
    // Nothing is saved over it, and the text stays in recovery.
    autosave.watch(document);
    QTest::qWait(Autosave::SaveIdleMs + 750);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(recovery.pending().size(), 1);
    // Read again once it can be read, it is still what the text was made on,
    // so the text is saved over it.
    QVERIFY(QFile::setPermissions(file, readable));
    store.readAgain(document);
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("local edit\n"), 3000);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QTRY_VERIFY(recovery.pending().isEmpty());
}

void ParchTests::restoreOfIdenticalTextOpensClean()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "same\n");
    }
    writeFile(file, "same\n");
    DocumentStore store(paths);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Clean);
    QVERIFY(Recovery(paths).pending().isEmpty());
}

void ParchTests::restoreOfInvalidUtf8FileWaitsInConflict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "local\n");
    }
    writeFile(file, "bad \xC3\x28\n");
    DocumentStore store(paths);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("local\n"));
}

void ParchTests::restoreOfDraftNamedLaterOpensTheNamedFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString target = root.filePath("named.md");
    Recovery::Entry entry;
    QString namedPath;
    {
        DocumentStore first(paths);
        Document *draft = first.newDraft();
        draft->buffer()->replace({0, 0}, "text\n");
        Recovery recovery(paths);
        recovery.write(draft->id(), {}, SourceSnapshot{2, "text\n"}, DiskVersion{});
        first.saveAs(draft, target);
        QTRY_COMPARE(draft->state(), Document::State::Clean);
        namedPath = draft->path();
        entry = recovery.pending().first();
    }
    DocumentStore store(paths);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QVERIFY(!document->isDraft());
    QCOMPARE(document->path(), namedPath);
    QCOMPARE(document->id(), entry.documentId);
}

void ParchTests::conflictAtCleanCloseStaysConflictAfterRestart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *document = store.open(file);
        autosave.watch(document);
        writeFile(file, "theirs\n");
        document->buffer()->replace({0, 0}, "edit ");
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 2000);
        QString error;
        QVERIFY(!autosave.flush(&error));
        QVERIFY(!error.isEmpty());
    }
    const auto entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 1);
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.restore(entries.first());
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Conflict);
    autosave.watch(document);
    QTest::qWait(Autosave::SaveIdleMs + 750);
    QCOMPARE(readFile(file), QByteArray("theirs\n"));
    // Snapshotting goes on after the restart, and the conflict is still known to the next one.
    QTRY_VERIFY_WITH_TIMEOUT(recovery.pending().first().written > entries.first().written, 1000);
    QVERIFY(recovery.pending().first().base == entries.first().base);
    QCOMPARE(recovery.pending().first().text, entries.first().text);
}

void ParchTests::flushSurvivesDocumentsClosedWhileWaiting()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    writeFile(root.filePath("a.md"), "a\n");
    writeFile(root.filePath("b.md"), "b\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *first = store.open(root.filePath("a.md"));
    Document *second = store.open(root.filePath("b.md"));
    store.retain(first);
    store.retain(second);
    autosave.watch(first);
    autosave.watch(second);
    first->buffer()->replace({0, 0}, "x");
    second->buffer()->replace({0, 0}, "y");
    // The first has no view left, so it closes as soon as it is saved, while
    // the second is still being waited for.
    store.release(first);
    QVERIFY(recovery.write(first->id(), first->path(), *first->buffer()->snapshot(), first->diskVersion()));
    QVERIFY(recovery.write(second->id(), second->path(), *second->buffer()->snapshot(), second->diskVersion()));
    QPointer<Document> watched(first);
    QString error;
    QVERIFY2(autosave.flush(&error), qPrintable(error));
    // The scenario happened: the released document was deleted while flush() waited.
    QVERIFY(watched.isNull());
    QCOMPARE(readFile(root.filePath("a.md")), QByteArray("xa\n"));
    QCOMPARE(readFile(root.filePath("b.md")), QByteArray("yb\n"));
    QVERIFY(Recovery(paths).pending().isEmpty());
}

void ParchTests::restoreIgnoresDeviceAndInodeChanges()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery recovery(paths);
    DocumentStore first(paths);
    Document *original = first.open(file);
    // After a reboot the same file can come back under other device and inode numbers.
    DiskVersion base = original->diskVersion();
    base.device += 1;
    base.inode += 7;
    QVERIFY(!(base == original->diskVersion()));
    QVERIFY(sameContentIdentity(base, original->diskVersion()));
    QVERIFY(recovery.write(original->id(), original->path(), SourceSnapshot{4, "after reboot\n"}, base));
    const auto entry = recovery.pending().first();
    DocumentStore store(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Modified);
    autosave.watch(document);
    QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("after reboot\n"), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Clean, 2000);
}

void ParchTests::restoreOfEntryWithoutBaseWaitsInConflict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "mine\n");
    Recovery recovery(paths);
    QString id, canonical;
    DiskVersion version;
    {
        DocumentStore first(paths);
        Document *document = first.open(file);
        id = document->id();
        canonical = document->path();
        version = document->diskVersion();
    }
    QVERIFY(recovery.write(id, canonical, SourceSnapshot{2, "local\n"}, version));
    // An older format had no base; remove it from the header to read as one.
    const QString entryFile = paths.recovery() + "/" + id + ".recovery";
    const QByteArray all = readFile(entryFile);
    const qsizetype end = all.indexOf('\n');
    QJsonObject header = QJsonDocument::fromJson(all.left(end)).object();
    header.remove("base");
    writeFile(entryFile, QJsonDocument(header).toJson(QJsonDocument::Compact) + all.mid(end));
    const auto entries = recovery.pending();
    QCOMPARE(entries.size(), 1);
    QVERIFY(!entries.first().hasBase);
    for (const bool deleted : {false, true}) {
        if (deleted) QVERIFY(QFile::remove(file));
        DocumentStore store(paths);
        Document *document = store.restore(entries.first());
        QVERIFY(document);
        QCOMPARE(document->state(), Document::State::Conflict);
        QCOMPARE(document->buffer()->text(), QStringLiteral("local\n"));
    }
}

void ParchTests::crashAfterOverlappedSaveRestoresTheNewestText()
{
    // A save lands while typing goes on, and parch ends before the next timed
    // snapshot. What comes back is the newest text, over the file that save
    // wrote, and never a conflict with parch's own save.
    for (const bool draft : {false, true}) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        const QString named = root.filePath("a.md");
        const QString start = draft ? QString() : QStringLiteral("start\n");
        if (!draft) writeFile(named, start.toUtf8());
        QString id;
        {
            DocumentStore store(paths);
            Recovery recovery(paths);
            Autosave autosave(&store, &recovery);
            Document *document = draft ? store.newDraft() : store.open(named);
            QVERIFY(document);
            id = document->id();
            autosave.watch(document);
            document->buffer()->replace({0, 0}, QStringLiteral("T1 "));
            // Recovery has the text, as its timer would have taken it.
            QVERIFY(autosave.keep(document));
            document->buffer()->replace({0, 0}, QStringLiteral("T2 "));
            {
                // The save waits until a key has been typed while it runs.
                PoolBlock block;
                store.save(document);
                QCOMPARE(document->state(), Document::State::Saving);
                document->buffer()->replace({0, 0}, QStringLiteral("T3 "));
            }
            QTRY_COMPARE(document->state(), Document::State::Modified);
            const QString file = draft ? DocumentStore::draftFile(paths, id) : named;
            QCOMPARE(readFile(file), ("T2 T1 " + start).toUtf8());
            // Parch ends here, long before the next timed snapshot: only the
            // writes already started finish.
        }
        const auto entries = Recovery(paths).pending();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().text, "T3 T2 T1 " + start);
        DocumentStore store(paths);
        Document *restored = store.restore(entries.first());
        QVERIFY(restored);
        QCOMPARE(restored->id(), id);
        QCOMPARE(restored->state(), Document::State::Modified);
        QCOMPARE(restored->buffer()->text(), "T3 T2 T1 " + start);
    }
}

void ParchTests::restoreAfterOwnSaveIsNoConflict()
{
    // Recovery last took an older text, on an older version of the file, and
    // parch's own save changed the file before parch ended. Whichever text is
    // newer comes back, without a conflict, and the other is a version in
    // history. A draft's own file is only ever parch's.
    for (const bool draft : {false, true}) {
        for (const bool entryNewer : {false, true}) {
            const QString label = QStringLiteral("draft %1, entry newer %2").arg(draft).arg(entryNewer);
            QTemporaryDir root;
            QVERIFY(root.isValid());
            const AppPaths paths = AppPaths::under(root.path());
            const QString named = root.filePath("a.md");
            const QString start = draft ? QString() : QStringLiteral("start\n");
            if (!draft) writeFile(named, start.toUtf8());
            QString id;
            {
                DocumentStore store(paths);
                Recovery recovery(paths);
                Document *document = draft ? store.newDraft() : store.open(named);
                QVERIFY(document);
                id = document->id();
                const DiskVersion before = document->diskVersion();
                document->buffer()->replace({0, 0}, QStringLiteral("T1 "));
                if (!entryNewer) {
                    QVERIFY(recovery.write(id, document->path(), *document->buffer()->snapshot(), before));
                    // The clock moves on between the entry and the save.
                    QTest::qWait(20);
                    document->buffer()->replace({0, 0}, QStringLiteral("T2 "));
                }
                store.save(document);
                QTRY_COMPARE(document->state(), Document::State::Clean);
                if (entryNewer) {
                    QTest::qWait(20);
                    // A snapshot taken before the save landed, and written after it.
                    document->buffer()->replace({0, 0}, QStringLiteral("T2 "));
                    QVERIFY(recovery.write(id, document->path(), *document->buffer()->snapshot(), before));
                }
            }
            const auto entries = Recovery(paths).pending();
            QVERIFY2(entries.size() == 1, qPrintable(label));
            DocumentStore store(paths);
            Document *restored = store.restore(entries.first());
            QVERIFY2(restored, qPrintable(label));
            QVERIFY2(restored->state() == (entryNewer ? Document::State::Modified : Document::State::Clean),
                     qPrintable(label + u' ' + restored->statusText()));
            QCOMPARE(restored->buffer()->text(), "T2 T1 " + start);
            // The older text is kept as a version, so nothing is lost.
            QVERIFY2(versionTexts(store.history(), id).contains("T1 " + start), qPrintable(label));
            QCOMPARE(Recovery(paths).pending().isEmpty(), !entryNewer);
        }
    }

    // A change from another program, after parch's own save, is still a conflict.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString named = root.filePath("a.md");
    writeFile(named, "start\n");
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Document *document = store.open(named);
        const DiskVersion before = document->diskVersion();
        document->buffer()->replace({0, 0}, QStringLiteral("T1 "));
        QVERIFY(recovery.write(document->id(), document->path(), *document->buffer()->snapshot(), before));
        document->buffer()->replace({0, 0}, QStringLiteral("T2 "));
        store.save(document);
        QTRY_COMPARE(document->state(), Document::State::Clean);
    }
    writeFile(named, "theirs\n");
    DocumentStore store(paths);
    Document *restored = store.restore(Recovery(paths).pending().first());
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Conflict);
    QCOMPARE(restored->buffer()->text(), QStringLiteral("T1 start\n"));
    QCOMPARE(readFile(named), QByteArray("theirs\n"));
}

void ParchTests::outsideWriteOfAnEarlierSaveStaysAConflict()
{
    // parch saves X as Y, and the file goes back to X: another program puts
    // it back and parch takes it, or parch saves X again. After an edit,
    // another program writes Y once more. Y is content parch once wrote, but
    // not what it wrote last, so after a quit or a crash it is still someone
    // else's change and must never be saved over.
    enum class End { Quit, CrashAfterConflict, CrashBeforeTheChangeIsSeen };
    QStringList wrong;
    for (const bool revertedOutside : {true, false}) {
        for (const End end : {End::Quit, End::CrashAfterConflict, End::CrashBeforeTheChangeIsSeen}) {
            const QString label = QStringLiteral("%1, %2").arg(
                revertedOutside ? QStringLiteral("put back outside") : QStringLiteral("saved back"),
                end == End::Quit                 ? QStringLiteral("quit")
                : end == End::CrashAfterConflict ? QStringLiteral("crash in conflict")
                                                 : QStringLiteral("crash before the change is seen"));
            QTemporaryDir root;
            QVERIFY(root.isValid());
            const AppPaths paths = AppPaths::under(root.path());
            const QString file = root.filePath("a.md");
            writeFile(file, "x\n");
            {
                DocumentStore store(paths);
                Recovery recovery(paths);
                Autosave autosave(&store, &recovery);
                Document *document = store.open(file);
                QVERIFY(document);
                autosave.watch(document);
                document->buffer()->replace({0, 1}, QStringLiteral("y"));
                store.save(document);
                QTRY_COMPARE(document->state(), Document::State::Clean);
                QCOMPARE(readFile(file), QByteArray("y\n"));
                if (revertedOutside) {
                    writeFile(file, "x\n");
                    QTRY_COMPARE_WITH_TIMEOUT(document->buffer()->text(), QStringLiteral("x\n"), 3000);
                    QCOMPARE(document->state(), Document::State::Clean);
                } else {
                    document->buffer()->replace({0, 1}, QStringLiteral("x"));
                    store.save(document);
                    QTRY_COMPARE(document->state(), Document::State::Clean);
                    QCOMPARE(readFile(file), QByteArray("x\n"));
                }
                document->buffer()->replace({0, 0}, QStringLiteral("edit "));
                // Recovery has the text, as its timer would have taken it.
                QVERIFY(autosave.keep(document));
                writeFile(file, "y\n");
                // A crash can come before parch has seen the change at all.
                if (end != End::CrashBeforeTheChangeIsSeen) {
                    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
                    if (end == End::Quit) QVERIFY(!autosave.flush());
                    else QVERIFY(autosave.keep(document));
                }
            }
            const auto entries = Recovery(paths).pending();
            QVERIFY2(entries.size() == 1, qPrintable(label));
            DocumentStore store(paths);
            Recovery recovery(paths);
            Autosave autosave(&store, &recovery);
            Document *restored = store.restore(entries.first());
            QVERIFY2(restored, qPrintable(label));
            autosave.watch(restored);
            QTest::qWait(Autosave::SaveIdleMs + 250);
            // Every case is tried, and every one that goes wrong is named.
            if (restored->state() != Document::State::Conflict)
                wrong.append(label + QStringLiteral(": ") + restored->statusText());
            if (restored->buffer()->text() != QStringLiteral("edit x\n"))
                wrong.append(label + QStringLiteral(": text ") + restored->buffer()->text());
            if (readFile(file) != QByteArray("y\n"))
                wrong.append(label + QStringLiteral(": file ") + QString::fromUtf8(readFile(file)));
        }
    }
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("; "))));

    // The same after a restart in between: the text comes back on X, which
    // parch did not write last, and another program then writes Y.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "x\n");
    {
        DocumentStore store(paths);
        Document *document = store.open(file);
        const DiskVersion before = document->diskVersion();
        // Recovery took the edit on X, and then a save of other text landed.
        QVERIFY(Recovery(paths).write(document->id(), document->path(), SourceSnapshot{9, "edit x\n"}, before));
        document->buffer()->replace({0, 1}, QStringLiteral("y"));
        store.save(document);
        QTRY_COMPARE(document->state(), Document::State::Clean);
        writeFile(file, "x\n");
    }
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *restored = store.restore(recovery.pending().first());
        QVERIFY(restored);
        QCOMPARE(restored->state(), Document::State::Modified);
        autosave.watch(restored);
        QVERIFY(autosave.keep(restored));
        writeFile(file, "y\n");
    }
    DocumentStore store(paths);
    Document *restored = store.restore(Recovery(paths).pending().first());
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Conflict);
    QCOMPARE(readFile(file), QByteArray("y\n"));
}

void ParchTests::ownSavesOutlastAFailedOrUnfinishedOne()
{
    // Recovery last took the text on the file's first content B, and parch's
    // own save then wrote X. A later save that fails, or that was noted but
    // had not yet replaced the file when parch ended, leaves the file at X:
    // still parch's own, so a restart shows no conflict.
    if (::geteuid() == 0) QSKIP("A read-only directory does not stop root.");
    const auto sha = [](const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); };
    for (const bool failed : {true, false}) {
        const QString label = failed ? QStringLiteral("a save that failed") : QStringLiteral("a save not yet landed");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        const QString folder = root.filePath("docs"), file = folder + "/a.md";
        writeFile(file, "base\n");
        QString id;
        {
            DocumentStore store(paths);
            Recovery recovery(paths);
            Document *document = store.open(file);
            QVERIFY(document);
            id = document->id();
            const DiskVersion base = document->diskVersion();
            document->buffer()->replace({0, 0}, QStringLiteral("one "));
            QVERIFY(recovery.write(id, document->path(), *document->buffer()->snapshot(), base));
            document->buffer()->replace({0, 0}, QStringLiteral("two "));
            store.save(document);
            QTRY_COMPARE(document->state(), Document::State::Clean);
            QCOMPARE(readFile(file), QByteArray("two one base\n"));
            document->buffer()->replace({0, 0}, QStringLiteral("three "));
            if (failed) {
                const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
                const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
                QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
                store.save(document);
                QTRY_COMPARE(document->state(), Document::State::SaveFailed);
            } else {
                // As a save does before it writes; parch ends before the file changes.
                QVERIFY(recovery.noteSave(id, sha("two one base\n"), sha("three two one base\n"),
                                          document->buffer()->revision()));
            }
        }
        QCOMPARE(readFile(file), QByteArray("two one base\n"));
        const auto entries = Recovery(paths).pending();
        QVERIFY2(entries.size() == 1, qPrintable(label));
        DocumentStore store(paths);
        Document *restored = store.restore(entries.first());
        QVERIFY2(restored, qPrintable(label));
        QVERIFY2(restored->state() != Document::State::Conflict, qPrintable(label + u' ' + restored->statusText()));
        QCOMPARE(restored->buffer()->text(), QStringLiteral("two one base\n"));
        QVERIFY2(versionTexts(store.history(), id).contains("one base\n"), qPrintable(label));
    }
}

void ParchTests::savesThatNeverLandedOrCameBeforeAreNotOwn()
{
    // Content counts as parch's own only when a save of this opening of the
    // document wrote it. Another program can write the bytes of a save that
    // failed, or put back a file parch saved before it was last opened.
    const auto restoredState = [](const AppPaths &paths) {
        DocumentStore store(paths);
        const auto entries = Recovery(paths).pending();
        Document *restored = entries.size() == 1 ? store.restore(entries.first()) : nullptr;
        return restored ? restored->state() : Document::State::ReadOnly;
    };
    {
        if (::geteuid() == 0) QSKIP("A read-only directory does not stop root.");
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        const QString folder = root.filePath("docs"), file = folder + "/a.md";
        writeFile(file, "x\n");
        {
            DocumentStore store(paths);
            Recovery recovery(paths);
            Autosave autosave(&store, &recovery);
            Document *document = store.open(file);
            QVERIFY(document);
            document->buffer()->replace({0, 1}, QStringLiteral("y"));
            const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
            const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
            QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
            store.save(document);
            QTRY_COMPARE(document->state(), Document::State::SaveFailed);
            QVERIFY(QFile::setPermissions(folder, writable));
            document->buffer()->replace({0, 0}, QStringLiteral("more "));
            QVERIFY(autosave.keep(document));
            // The bytes that save meant to write, from somewhere else.
            writeFile(file, "y\n");
        }
        QCOMPARE(restoredState(paths), Document::State::Conflict);
        QCOMPARE(readFile(file), QByteArray("y\n"));
    }
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "x\n");
    {
        DocumentStore store(paths);
        Document *document = store.open(file);
        document->buffer()->replace({0, 1}, QStringLiteral("y"));
        store.save(document);
        QTRY_COMPARE(document->state(), Document::State::Clean);
        // parch ends while the file holds its text, so recovery has nothing.
    }
    writeFile(file, "x\n");
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *document = store.open(file);
        document->buffer()->replace({0, 0}, QStringLiteral("edit "));
        QVERIFY(autosave.keep(document));
        writeFile(file, "y\n");
    }
    QCOMPARE(restoredState(paths), Document::State::Conflict);
    QCOMPARE(readFile(file), QByteArray("y\n"));
}

void ParchTests::newerMeansLaterInTheEditsNotOnTheClock()
{
    // After a restart, the newer of the entry's text and the file parch
    // saved is the later one in the order of the edits, whatever the clock
    // says: a snapshot taken before a save can be written after it, and a
    // save of older text can land after a snapshot of newer text.
    for (const bool draft : {false, true}) {
        for (const bool entryNewer : {false, true}) {
            const QString label = QStringLiteral("draft %1, entry newer %2").arg(draft).arg(entryNewer);
            QTemporaryDir root;
            QVERIFY(root.isValid());
            const AppPaths paths = AppPaths::under(root.path());
            const QString named = root.filePath("a.md");
            if (!draft) writeFile(named, "start\n");
            QString id, file;
            {
                DocumentStore store(paths);
                Document *document = draft ? store.newDraft() : store.open(named);
                QVERIFY(document);
                id = document->id();
                const DiskVersion before = document->diskVersion();
                // The file gets one text and the entry the other, each at
                // its own revision, and the clock says the opposite.
                document->buffer()->replace({0, 0}, QStringLiteral("T1 "));
                const SourceSnapshot older = *document->buffer()->snapshot();
                if (entryNewer) {
                    store.save(document);
                    QTRY_COMPARE(document->state(), Document::State::Clean);
                }
                document->buffer()->replace({0, 0}, QStringLiteral("T2 "));
                const SourceSnapshot newer = *document->buffer()->snapshot();
                if (!entryNewer) {
                    store.save(document);
                    QTRY_COMPARE(document->state(), Document::State::Clean);
                }
                file = draft ? DocumentStore::draftFile(paths, id) : named;
                QTest::qWait(20);
                QVERIFY(Recovery(paths).write(id, document->path(), entryNewer ? newer : older, before));
                const QDateTime written = Recovery(paths).pending().first().written;
                // The save landed after the snapshot, by the clock, when the snapshot is newer.
                QFile touched(file);
                QVERIFY(touched.open(QIODevice::ReadWrite));
                QVERIFY(touched.setFileTime(entryNewer ? written.addSecs(5) : written.addSecs(-5),
                                            QFileDevice::FileModificationTime));
            }
            DocumentStore store(paths);
            DocumentStore::Recovered how = DocumentStore::Recovered::AlreadySaved;
            const auto entry = Recovery(paths).pending().first();
            Document *restored = store.restore(entry, &how);
            QVERIFY2(restored, qPrintable(label));
            const QString start = draft ? QString() : QStringLiteral("start\n");
            QCOMPARE(restored->buffer()->text(), QStringLiteral("T2 T1 ") + start);
            QVERIFY2(how == (entryNewer ? DocumentStore::Recovered::Shown : DocumentStore::Recovered::InHistory),
                     qPrintable(label));
            QVERIFY2(versionTexts(store.history(), id).contains(QStringLiteral("T1 ") + start), qPrintable(label));
            // Its revisions go on from those before the restart.
            QVERIFY2(restored->buffer()->revision() > entry.revision, qPrintable(label));
        }
    }

    // Across a second restart: the restored text is saved with more typed,
    // and parch ends before recovery took it. The file is newer.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString named = root.filePath("a.md");
    writeFile(named, "start\n");
    {
        DocumentStore store(paths);
        Document *document = store.open(named);
        const DiskVersion before = document->diskVersion();
        for (int i = 0; i < 40; ++i) document->buffer()->replace({0, 0}, QStringLiteral("x"));
        QVERIFY(Recovery(paths).write(document->id(), document->path(), *document->buffer()->snapshot(), before));
    }
    {
        DocumentStore store(paths);
        Document *restored = store.restore(Recovery(paths).pending().first());
        QVERIFY(restored);
        QCOMPARE(restored->state(), Document::State::Modified);
        restored->buffer()->replace({0, 0}, QStringLiteral("more "));
        store.save(restored);
        QTRY_COMPARE(restored->state(), Document::State::Clean);
    }
    DocumentStore store(paths);
    DocumentStore::Recovered how = DocumentStore::Recovered::AlreadySaved;
    Document *restored = store.restore(Recovery(paths).pending().first(), &how);
    QVERIFY(restored);
    QCOMPARE(how, DocumentStore::Recovered::InHistory);
    QCOMPARE(restored->state(), Document::State::Clean);
    QVERIFY(restored->buffer()->text().startsWith(QStringLiteral("more x")));
}

void ParchTests::savesGoWithTheirDocument()
{
    // The notes of parch's own saves last while their document is open or
    // in recovery, and go with it: when it closes, and at the next start
    // for one that parch left with its file holding its text.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const auto savesOf = [&](const QString &id) { return paths.recovery() + u'/' + id + QStringLiteral(".saves"); };
    const QString kept = root.filePath("kept.md"), closed = root.filePath("closed.md"), left = root.filePath("left.md");
    for (const QString &file : {kept, closed, left}) writeFile(file, "start\n");
    QString keptId, closedId, leftId;
    {
        DocumentStore store(paths);
        Document *keptDocument = store.open(kept);
        Document *closedDocument = store.open(closed);
        Document *leftDocument = store.open(left);
        keptId = keptDocument->id();
        closedId = closedDocument->id();
        leftId = leftDocument->id();
        for (Document *document : {keptDocument, closedDocument, leftDocument}) {
            document->buffer()->replace({0, 0}, QStringLiteral("saved "));
            store.save(document);
            QTRY_COMPARE(document->state(), Document::State::Clean);
            QVERIFY(QFileInfo::exists(savesOf(document->id())));
        }
        // Closed, a document's file holds its text: its notes go.
        QVERIFY(store.drop(closedDocument));
        QVERIFY(!QFileInfo::exists(savesOf(closedId)));
        // One still has text recovery holds; the other is clean when parch ends.
        keptDocument->buffer()->replace({0, 0}, QStringLiteral("more "));
        QVERIFY(Recovery(paths).write(keptId, keptDocument->path(), *keptDocument->buffer()->snapshot(),
                                      keptDocument->diskVersion()));
    }
    {
        Backend backend(paths);
        backend.start({});
        QVERIFY(backend.store()->documents().size() >= 1);
        QVERIFY(QFileInfo::exists(savesOf(keptId)));
        QVERIFY(!QFileInfo::exists(savesOf(leftId)));
    }

    // Notes longer than parch ever writes are no notes at all: the file
    // they end on does not count as parch's.
    const QByteArray hash = QCryptographicHash::hash("text\n", QCryptographicHash::Sha256);
    QString lines;
    for (int i = 0; i < 1000; ++i) lines += QStringLiteral("- %1 %2\n").arg(QString::fromLatin1(hash.toHex())).arg(i);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    writeFile(savesOf(id), lines.toUtf8());
    QVERIFY(!Recovery(paths).savedLast(id, hash));
    QVERIFY(!Recovery(paths).lastSave(id));
}

void ParchTests::namingThatMeetsAChangeKeepsOwnSaves()
{
    // A naming whose target changed before it was written leaves the
    // document's own file as parch's saves left it, and their notes stay, so
    // that a restart does not take that file for someone else's.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md"), target = root.filePath("b.md");
    writeFile(file, "base\n");
    const QByteArray saved = QCryptographicHash::hash("one base\n", QCryptographicHash::Sha256);
    {
        DocumentStore store(paths);
        Document *document = store.open(file);
        QVERIFY(document);
        const DiskVersion opened = document->diskVersion();
        document->buffer()->replace({0, 0}, QStringLiteral("one "));
        store.save(document);
        store.waitForSaves();
        QCOMPARE(document->state(), Document::State::Clean);
        QVERIFY(Recovery(paths).savedLast(document->id(), saved));
        const DiskVersion absent = DiskFile::fingerprint(target);
        QVERIFY(!absent.exists);
        writeFile(target, "theirs\n");
        document->buffer()->replace({0, 0}, QStringLiteral("two "));
        store.saveAs(document, target, absent);
        store.waitForSaves();
        QCOMPARE(document->state(), Document::State::SaveFailed);
        QCOMPARE(readFile(target), QByteArray("theirs\n"));
        QCOMPARE(readFile(file), QByteArray("one base\n"));
        QCOMPARE(document->path(), file);
        QVERIFY(Recovery(paths).savedLast(document->id(), saved));
        // Recovery holds the newest text on the file as it was opened, as
        // when parch ends before the snapshot that follows a save.
        QVERIFY(Recovery(paths).write(document->id(), document->path(), *document->buffer()->snapshot(), opened));
    }
    // parch ended without closing the document, as in a crash.
    DocumentStore store(paths);
    const QList<Recovery::Entry> entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 1);
    Document *restored = store.restore(entries.first());
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Modified);
    QCOMPARE(restored->buffer()->text(), QStringLiteral("two one base\n"));
}

void ParchTests::draftBytesNothingKeptAreNeverSavedOver()
{
    // A draft's own file that is not valid text is replaced by the recovered
    // text only once history keeps its bytes. When history cannot, the draft
    // waits in conflict, and autosave leaves the bytes where they are.
    for (const bool historyWorks : {true, false}) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        // A file where history's folder belongs makes every capture fail.
        if (!historyWorks) writeFile(paths.history(), "in the way");
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(Recovery(paths).write(id, {}, SourceSnapshot{3, QStringLiteral("recovered\n")}, DiskVersion{}));
        const QByteArray invalid("caf\xe9 kept\n");
        const QString file = DocumentStore::draftFile(paths, id);
        writeFile(file, invalid);
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *restored = store.restore(recovery.pending().first());
        QVERIFY(restored);
        QCOMPARE(restored->buffer()->text(), QStringLiteral("recovered\n"));
        autosave.watch(restored);
        if (historyWorks) {
            QCOMPARE(restored->state(), Document::State::Modified);
            bool found = false;
            for (const History::Version &version : store.history()->versions(id))
                found = found || store.history()->read(id, version.id) == invalid;
            QVERIFY(found);
            QTRY_COMPARE_WITH_TIMEOUT(readFile(file), QByteArray("recovered\n"), 3000);
        } else {
            QCOMPARE(restored->state(), Document::State::Conflict);
            QTest::qWait(Autosave::SaveIdleMs + 250);
            QCOMPARE(readFile(file), invalid);
        }
    }
}

void ParchTests::draftConflictResolvesAgainstItsOwnFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    DocumentReview review(&store);
    QSignalSpy problems(&review, &DocumentReview::problem);
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("mine\n"));
    store.save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    const QString file = DocumentStore::draftFile(paths, draft->id());

    // A choice for a document that is not in conflict is refused, and says why.
    review.resolveConflict(draft, DocumentReview::Reload);
    QCOMPARE(problems.size(), 1);
    QVERIFY(!problems.first().at(1).toString().isEmpty());
    QCOMPARE(draft->buffer()->text(), QStringLiteral("mine\n"));

    // Something else writes the draft's own file, and the next save meets it.
    const auto conflicted = [&](const QByteArray &theirs) {
        writeFile(file, theirs);
        draft->buffer()->replace({0, 0}, QStringLiteral("more "));
        store.save(draft);
        QTRY_COMPARE(draft->state(), Document::State::Conflict);
    };
    conflicted("theirs\n");
    QVERIFY(!draft->fileMissing());
    // The draft's own file is what the conflict shows beside the text.
    QSignalSpy read(&review, &DocumentReview::diskTextRead);
    QVERIFY(review.readDiskText(draft) != 0);
    QTRY_COMPARE(read.size(), 1);
    QCOMPARE(read.first().at(2).toString(), QStringLiteral("theirs\n"));
    QVERIFY(read.first().at(3).toString().isEmpty());

    // Keep both has nowhere beside a draft's own file to put a copy: it is
    // refused, with the reason shown on the document.
    const QString reason = draft->lastError();
    review.resolveConflict(draft, DocumentReview::KeepBoth);
    QCOMPARE(draft->state(), Document::State::Conflict);
    QVERIFY(!draft->lastError().isEmpty());
    QVERIFY(draft->lastError() != reason);
    QCOMPARE(draft->buffer()->text(), QStringLiteral("more mine\n"));

    // Reload takes the file's text; this text stays in history and undo.
    review.resolveConflict(draft, DocumentReview::Reload);
    QCOMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(draft->buffer()->text(), QStringLiteral("theirs\n"));
    QVERIFY(versionTexts(store.history(), draft->id()).contains(QStringLiteral("more mine\n")));

    // Overwrite saves this text over the file, which stays in history.
    conflicted("other\n");
    review.resolveConflict(draft, DocumentReview::Overwrite);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(readFile(file), QByteArray("more theirs\n"));
    QVERIFY(versionTexts(store.history(), draft->id()).contains(QStringLiteral("other\n")));
}

void ParchTests::namingDraftKeepsItsOwnFileInHistory()
{
    // Naming a draft removes its folder. Its own file can hold text that is
    // not the text being named, which must not go with it: it becomes a
    // version in history, or, when history cannot keep it, it stays as a
    // draft of its own, listed with the drafts, and a notice says so.
    for (const bool historyWorks : {true, false}) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        // A file where history's folder belongs makes every capture fail.
        if (!historyWorks) writeFile(paths.history(), "in the way");
        Backend backend(paths);
        DocumentStore &store = *backend.store();
        QSignalSpy kept(&store, &DocumentStore::draftKept);
        Document *draft = store.newDraft();
        draft->buffer()->replace({0, 0}, QStringLiteral("named text\n"));
        store.save(draft);
        QTRY_COMPARE(draft->state(), Document::State::Clean);
        const QString file = DocumentStore::draftFile(paths, draft->id());
        writeFile(file, "newer on disk\n");
        const QString target = root.filePath("named.md");
        store.saveAs(draft, target);
        QTRY_COMPARE(draft->state(), Document::State::Clean);
        QVERIFY(!draft->isDraft());
        QCOMPARE(readFile(target), QByteArray("named text\n"));
        if (historyWorks) {
            QVERIFY(versionTexts(store.history(), draft->id()).contains(QStringLiteral("newer on disk\n")));
            QVERIFY(!QFileInfo::exists(PathText::folderOf(file)));
            QCOMPARE(kept.size(), 0);
        } else {
            QCOMPARE(kept.size(), 1);
            const QString keptId = kept.first().at(1).toString();
            QVERIFY(keptId != draft->id());
            QVERIFY(store.isUnnamedDraft(keptId));
            QCOMPARE(readFile(DocumentStore::draftFile(paths, keptId)), QByteArray("newer on disk\n"));
            QVERIFY(!QFileInfo::exists(PathText::folderOf(file)));
            // The library lists every unnamed draft's folder, by its text's title.
            QCOMPARE(Document::draftTitle(QString::fromUtf8(readFile(DocumentStore::draftFile(paths, keptId)))),
                     QStringLiteral("newer on disk"));
            QVERIFY(backend.notice());
            QVERIFY2(backend.problemDetail().contains(QStringLiteral("newer on disk")), qPrintable(backend.problemDetail()));
            // History failed it, so it is no news, though nothing was lost.
            QVERIFY(!backend.noticeInforms());
            QCOMPARE(backend.noticeTone(), Backend::Warning);
            // It opens as any other draft.
            Document *reopened = store.openDraft(keptId);
            QVERIFY(reopened);
            QCOMPARE(reopened->buffer()->text(), QStringLiteral("newer on disk\n"));
        }
    }
}

void ParchTests::fileWatchReportsABurstOnceAndSurvivesReplacement()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = QFileInfo(root.path()).canonicalFilePath() + "/a.md";
    writeFile(file, "one\n");
    FileWatch watch;
    QStringList changed, removed;
    connect(&watch, &FileWatch::changedOnDisk, this, [&](const QString &p) { changed << p; });
    connect(&watch, &FileWatch::removedFromDisk, this, [&](const QString &p) { removed << p; });
    watch.watch(file);
    // Several writes close together are one burst.
    for (int i = 0; i < 4; ++i) {
        writeFile(file, QByteArray::number(i) + "\n");
        QTest::qWait(5);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 3000);
    QTest::qWait(300);
    QCOMPARE(changed, QStringList{file});
    // A rename over the file drops the file's own watch; the next change must still arrive.
    for (int round = 0; round < 3; ++round) {
        changed.clear();
        writeFile(file + ".tmp", "replacement " + QByteArray::number(round) + "\n");
        QVERIFY(QFile::rename(file + ".tmp", file) || (QFile::remove(file) && QFile::rename(file + ".tmp", file)));
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 3000);
        changed.clear();
        writeFile(file, "after " + QByteArray::number(round) + "\n");
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 3000);
    }
    QVERIFY(removed.isEmpty());
}

void ParchTests::fileWatchIgnoresNeighboursAndReportsRemoval()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/a.md";
    writeFile(file, "one\n");
    FileWatch watch;
    QStringList changed, removed;
    connect(&watch, &FileWatch::changedOnDisk, this, [&](const QString &p) { changed << p; });
    connect(&watch, &FileWatch::removedFromDisk, this, [&](const QString &p) { removed << p; });
    watch.watch(file);
    writeFile(folder + "/neighbour.md", "x\n");
    QTest::qWait(300);
    QVERIFY(changed.isEmpty());
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE_WITH_TIMEOUT(removed, QStringList{file}, 3000);
    // The file coming back is noticed by its folder.
    writeFile(file, "back\n");
    QTRY_COMPARE_WITH_TIMEOUT(changed, QStringList{file}, 3000);
    watch.unwatch(file);
    changed.clear();
    writeFile(file, "ignored\n");
    QTest::qWait(300);
    QVERIFY(changed.isEmpty());
}

void ParchTests::fileWatchLooksOffTheUiThread()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = QFileInfo(root.path()).canonicalFilePath() + "/a.md";
    writeFile(file, "one\n");
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    FileWatch watch(&pool);
    QStringList changed, removed;
    QList<bool> onUiThread;
    const auto uiThread = [] { return QThread::currentThread() == QCoreApplication::instance()->thread(); };
    connect(&watch, &FileWatch::changedOnDisk, this, [&](const QString &path) {
        changed << path;
        onUiThread << uiThread();
    });
    connect(&watch, &FileWatch::removedFromDisk, this, [&](const QString &path) {
        removed << path;
        onUiThread << uiThread();
    });
    watch.watch(file);
    // Each look at the file waits behind a gate the test opens, so that
    // what it sees, and when, are the test's to decide.
    const auto gate = [&pool] {
        auto held = std::make_shared<QSemaphore>();
        pool.start([held] { held->acquire(); });
        return held;
    };
    const auto first = gate();
    // Every gate opens however the test ends, or the watch would wait for ever.
    const auto openFirst = qScopeGuard([first] { first->release(); });
    writeFile(file, "two\n");
    // The burst has ended and its look waits: nothing is reported, since
    // nothing is looked at on the UI thread.
    QTest::qWait(FileWatch::DebounceMs + 300);
    QVERIFY(changed.isEmpty());
    const auto second = gate();
    const auto openSecond = qScopeGuard([second] { second->release(); });
    writeFile(file, "three, a longer line\n");
    QTest::qWait(FileWatch::DebounceMs + 300);
    // The first look runs only now, after a newer one was asked for: what
    // it saw is out of date and dropped.
    first->release();
    QTest::qWait(300);
    QVERIFY(changed.isEmpty());
    // The file goes before the newer look runs, and its going is what is reported.
    QVERIFY(QFile::remove(file));
    QTest::qWait(FileWatch::DebounceMs + 300);
    second->release();
    QTRY_VERIFY(!removed.isEmpty());
    QTest::qWait(300);
    QVERIFY(changed.isEmpty());
    QCOMPARE(removed.count(file), removed.size());
    QVERIFY(!onUiThread.contains(false));
}

void ParchTests::cleanDocumentReloadsExternalEditKeepingUndo()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "one\ntwo\nthree\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    QVERIFY(document);
    QList<std::tuple<int, int, QString>> edits;
    connect(document->buffer(), &SourceBuffer::sourceChanged, this,
            [&](Revision, SourceRange range, const QString &text) { edits.append({range.start, range.length, text}); });
    writeFile(path, "one\nTWO\nthree\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->buffer()->text(), QStringLiteral("one\nTWO\nthree\n"), 3000);
    QCOMPARE(document->state(), Document::State::Clean);
    // Only the changed middle is replaced, so the rest keeps its place for the view.
    QCOMPARE(edits.size(), 1);
    QCOMPARE(std::get<0>(edits.first()), 4);
    QCOMPARE(std::get<1>(edits.first()), 3);
    QCOMPARE(std::get<2>(edits.first()), QStringLiteral("TWO"));
    QCOMPARE(document->diskVersion().sha256, DiskFile::fingerprint(path).sha256);
    document->buffer()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("one\ntwo\nthree\n"));
    // The reloaded text is what the file holds, so autosave has nothing of its own to write back.
    document->buffer()->redo();
    QCOMPARE(document->state(), Document::State::Clean);
}

void ParchTests::reloadKeepsGraphemesWhole()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    // The file's "e" gains an accent, so the shared prefix ends inside a grapheme.
    writeFile(path, QStringLiteral("café cafe x\n").toUtf8());
    DocumentStore store(paths);
    Document *document = store.open(path);
    QList<std::pair<int, int>> edits;
    connect(document->buffer(), &SourceBuffer::sourceChanged, this,
            [&](Revision, SourceRange range, const QString &) { edits.append({range.start, range.length}); });
    const QString next = QStringLiteral("café café x\n");
    writeFile(path, next.toUtf8());
    QTRY_COMPARE_WITH_TIMEOUT(document->buffer()->text(), next, 3000);
    QCOMPARE(edits.size(), 1);
    // "cafe" is shared; the edit must start before the "e" that now carries a mark.
    QCOMPARE(edits.first().first, 8);
}

void ParchTests::dirtyDocumentEntersConflictAndPausesAutosave()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.open(path);
    autosave.watch(document);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(path, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
    QTest::qWait(Autosave::SaveMaxMs + 500);
    QCOMPARE(readFile(path), QByteArray("theirs\n"));
    QVERIFY(!recovery.pending().isEmpty());
}

void ParchTests::deletedFileKeepsText()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    const QString text = QStringLiteral("keep me\n");
    writeFile(path, text.toUtf8());
    DocumentStore store(paths);
    Document *document = store.open(path);
    QVERIFY(QFile::remove(path));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QCOMPARE(document->buffer()->text(), text);
}

void ParchTests::conflictChoices()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    DocumentStore store(paths);
    QList<Document *> copies;
    connect(&store, &DocumentStore::conflictCopyOpened, this, [&](Document *copy) { copies << copy; });

    auto conflicted = [&](const QString &name) {
        const QString file = folder + "/" + name;
        writeFile(file, "base\n");
        Document *document = store.open(file);
        document->buffer()->replace({0, 0}, "mine ");
        writeFile(file, "theirs\n");
        return document;
    };
    Document *a = conflicted("a.md");
    Document *b = conflicted("b.md");
    Document *c = conflicted("c.md");
    for (Document *document : {a, b, c}) QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);

    store.resolveConflict(a, ConflictChoice::Reload);
    QTRY_COMPARE(a->buffer()->text(), QStringLiteral("theirs\n"));
    QCOMPARE(a->state(), Document::State::Clean);
    QCOMPARE(store.history()->versions(a->id()).size(), 1);
    QCOMPARE(store.history()->read(a->id(), store.history()->versions(a->id()).last().id), QByteArray("mine base\n"));
    // The local text can still be reached by undo.
    a->buffer()->undo();
    QCOMPARE(a->buffer()->text(), QStringLiteral("mine base\n"));

    store.resolveConflict(b, ConflictChoice::Overwrite);
    QTRY_COMPARE(readFile(folder + "/b.md"), QByteArray("mine base\n"));
    QTRY_COMPARE(b->state(), Document::State::Clean);
    // The file that was replaced is the oldest version, and the text saved,
    // the first save since the file was opened, the newest.
    QCOMPARE(store.history()->versions(b->id()).size(), 2);
    QCOMPARE(store.history()->read(b->id(), store.history()->versions(b->id()).last().id), QByteArray("theirs\n"));
    QCOMPARE(store.history()->read(b->id(), store.history()->versions(b->id()).first().id), QByteArray("mine base\n"));

    store.resolveConflict(c, ConflictChoice::KeepBoth);
    QTRY_COMPARE(c->buffer()->text(), QStringLiteral("theirs\n"));
    QCOMPARE(c->state(), Document::State::Clean);
    const QStringList names = QDir(folder).entryList({"c (conflict *).md"});
    QCOMPARE(names.size(), 1);
    QVERIFY(QRegularExpression(R"(^c \(conflict \d{4}-\d{2}-\d{2} \d{6}\)\.md$)").match(names.first()).hasMatch());
    QCOMPARE(readFile(folder + "/" + names.first()), QByteArray("mine base\n"));
    QCOMPARE(copies.size(), 1);
    QCOMPARE(copies.first()->path(), folder + "/" + names.first());
    QCOMPARE(copies.first()->buffer()->text(), QStringLiteral("mine base\n"));
    QCOMPARE(readFile(folder + "/c.md"), QByteArray("theirs\n"));
}

void ParchTests::deletedFileOnlyAllowsOverwrite()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    QList<Document *> copies;
    connect(&store, &DocumentStore::conflictCopyOpened, this, [&](Document *copy) { copies << copy; });
    Document *document = store.open(path);
    document->buffer()->replace({0, 0}, "mine ");
    QVERIFY(QFile::remove(path));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    for (const ConflictChoice choice : {ConflictChoice::Reload, ConflictChoice::KeepBoth}) {
        store.resolveConflict(document, choice);
        // Keep both looks at the file on a worker, and says so when it ends.
        store.waitForSaves();
        QCOMPARE(document->state(), Document::State::Conflict);
        QCOMPARE(document->lastError(), QStringLiteral("The file no longer exists"));
        QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
    }
    QVERIFY(copies.isEmpty());
    QCOMPARE(QDir(root.path()).entryList({"*conflict*"}).size(), 0);
    QVERIFY(store.history()->versions(document->id()).isEmpty());
    store.resolveConflict(document, ConflictChoice::Overwrite);
    QTRY_COMPARE(readFile(path), QByteArray("mine base\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    // Nothing was on disk to replace, so the only version is the save's own.
    QCOMPARE(store.history()->versions(document->id()).size(), 1);
    QCOMPARE(store.history()->read(document->id(), store.history()->versions(document->id()).first().id),
             QByteArray("mine base\n"));
}

void ParchTests::restoredConflictResolvesAgainstTheFileNow()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "base\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "local edit\n");
    }
    writeFile(file, "theirs\n");
    DocumentStore store(paths);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Conflict);
    // The file changes once more before the writer chooses; the choice meets the file as it is now.
    writeFile(file, "newest\n");
    store.resolveConflict(document, ConflictChoice::Overwrite);
    QTRY_COMPARE(readFile(file), QByteArray("local edit\n"));
    const auto versions = store.history()->versions(document->id());
    QVERIFY(!versions.isEmpty());
    QCOMPARE(store.history()->read(document->id(), versions.last().id), QByteArray("newest\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
}

void ParchTests::invalidUtf8ConflictReloadsReadOnly()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    writeFile(file, "base\n");
    Recovery::Entry entry;
    {
        DocumentStore first(paths);
        entry = entryFor(paths, first, file, "local edit\n");
    }
    const QByteArray invalid("bad \xff bytes\n");
    writeFile(file, invalid);
    DocumentStore store(paths);
    Document *document = store.restore(entry);
    QVERIFY(document);
    QCOMPARE(document->state(), Document::State::Conflict);
    store.resolveConflict(document, ConflictChoice::Reload);
    QCOMPARE(document->state(), Document::State::ReadOnly);
    QCOMPARE(readFile(file), invalid);
    store.save(document);
    store.waitForSaves();
    QCOMPARE(readFile(file), invalid);

    // Keep both and Overwrite work as usual on such a file.
    for (const ConflictChoice choice : {ConflictChoice::KeepBoth, ConflictChoice::Overwrite}) {
        writeFile(file, invalid);
        DocumentStore again(paths);
        Document *conflicted = again.restore(entry);
        QVERIFY(conflicted);
        QCOMPARE(conflicted->state(), Document::State::Conflict);
        again.resolveConflict(conflicted, choice);
        again.waitForSaves();
        if (choice == ConflictChoice::Overwrite) {
            QTRY_COMPARE(readFile(file), QByteArray("local edit\n"));
        } else {
            QCOMPARE(conflicted->state(), Document::State::ReadOnly);
            QCOMPARE(readFile(file), invalid);
            QCOMPARE(QDir(root.path()).entryList({"a (conflict *).md"}).size(), 1);
        }
    }
}

void ParchTests::ownSavesDoNotRaiseConflicts()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    document->buffer()->replace({0, 0}, "x");
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QTest::qWait(300);
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->buffer()->text(), QStringLiteral("xbase\n"));
}

void ParchTests::replacedFileIsNoticedRepeatedly()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "v0\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    for (int round = 1; round <= 4; ++round) {
        // Written beside the file and renamed over it, as Git and sync tools do.
        const QString text = QStringLiteral("v%1\n").arg(round);
        writeFile(path + ".new", text.toUtf8());
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::rename(path + ".new", path));
        QTRY_COMPARE_WITH_TIMEOUT(document->buffer()->text(), text, 3000);
        QCOMPARE(document->state(), Document::State::Clean);
    }
}

void ParchTests::renamedDocumentWatchesItsNewPath()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString first = root.filePath("a.md"), second = root.filePath("b.md");
    writeFile(first, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(first);
    store.saveAs(document, second);
    store.waitForSaves();
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), QFileInfo(second).canonicalFilePath());
    writeFile(second, "changed\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->buffer()->text(), QStringLiteral("changed\n"), 3000);
    // The old name is no longer this document's concern.
    writeFile(first, "other\n");
    QTest::qWait(300);
    QCOMPARE(document->buffer()->text(), QStringLiteral("changed\n"));
    QCOMPARE(document->state(), Document::State::Clean);
}

void ParchTests::keepBothNeverReplacesExistingFiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md";
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    // Files at every name the copy could take in the next seconds.
    const QDateTime now = QDateTime::currentDateTime();
    QStringList existing;
    for (int offset = -1; offset < 6; ++offset) {
        existing << folder + "/c (conflict " + now.addSecs(offset).toString("yyyy-MM-dd HHmmss") + ").md";
        writeFile(existing.last(), "PRE-EXISTING " + QByteArray::number(offset) + "\n");
    }
    store.resolveConflict(document, ConflictChoice::KeepBoth);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    for (int i = 0; i < existing.size(); ++i)
        QCOMPARE(readFile(existing.at(i)), "PRE-EXISTING " + QByteArray::number(i - 1) + "\n");
    int copies = 0;
    for (const QString &name : QDir(folder).entryList({"c (conflict *).md"}))
        if (readFile(folder + "/" + name) == "mine base\n") ++copies;
    QCOMPARE(copies, 1);
}

void ParchTests::keepBothTwiceKeepsBothCopies()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md";
    writeFile(file, "base\n");
    DocumentStore store(paths);
    QList<Document *> copies;
    connect(&store, &DocumentStore::conflictCopyOpened, this, [&](Document *copy) { copies << copy; });
    Document *document = store.open(file);
    // Both choices come within a second of each other, so both want one name.
    for (const QString &mine : {QStringLiteral("first\n"), QStringLiteral("second\n")}) {
        document->buffer()->replace({0, document->buffer()->length()}, mine);
        writeFile(file, "theirs " + mine.toUtf8());
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
        store.resolveConflict(document, ConflictChoice::KeepBoth);
        // The copy is made on a worker, and the file taken once it is.
        QTRY_COMPARE(document->state(), Document::State::Clean);
    }
    QCOMPARE(copies.size(), 2);
    QVERIFY(copies.at(0) != copies.at(1));
    QCOMPARE(copies.at(0)->buffer()->text(), QStringLiteral("first\n"));
    QCOMPARE(copies.at(1)->buffer()->text(), QStringLiteral("second\n"));
    QCOMPARE(readFile(copies.at(0)->path()), QByteArray("first\n"));
    QCOMPARE(readFile(copies.at(1)->path()), QByteArray("second\n"));
    // The copies stay what they were once the watcher has had its say.
    QTest::qWait(300);
    QCOMPARE(copies.at(0)->buffer()->text(), QStringLiteral("first\n"));
}

void ParchTests::keepBothNeverFollowsALinkAtItsName()
{
    // A link that leads nowhere, put at the name the copy would take, is a
    // name taken: the copy takes the next, and the link's target is never made.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md";
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    const QDateTime now = QDateTime::currentDateTime();
    QStringList links, targets;
    for (int offset = -1; offset < 6; ++offset) {
        links << folder + "/c (conflict " + now.addSecs(offset).toString("yyyy-MM-dd HHmmss") + ").md";
        targets << elsewhere.filePath(QStringLiteral("target %1").arg(offset));
        QVERIFY(QFile::link(targets.last(), links.last()));
    }
    QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    for (int i = 0; i < links.size(); ++i) {
        QVERIFY(QFileInfo(links.at(i)).isSymLink());
        QVERIFY(!QFileInfo::exists(targets.at(i)));
    }
    QVERIFY(QDir(elsewhere.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    QStringList copies;
    for (const QString &name : QDir(folder).entryList({"c (conflict *).md"}, QDir::Files))
        if (readFile(folder + "/" + name) == "mine base\n") copies << name;
    QCOMPARE(copies.size(), 1);
    QVERIFY2(copies.first().endsWith(QStringLiteral(" 2).md")), qPrintable(copies.first()));
}

void ParchTests::keepBothTakesTheNextNameWhenOneAppears()
{
    // A file that takes the copy's name while the copy is being written is
    // left as it is, and the copy takes the next name, on a file system that
    // renames without replacing, on one that cannot but links, and on one
    // that does neither.
    for (const int way : {0, 1, 2}) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        const QString folder = QFileInfo(root.path()).canonicalFilePath();
        const QString file = folder + "/c.md";
        writeFile(file, "base\n");
        DocumentStore store(paths);
        Document *document = store.open(file);
        document->buffer()->replace({0, 0}, "mine ");
        writeFile(file, "theirs\n");
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
        QStringList planted;
        DiskFile::setRenameWithoutReplacing(way == 0);
        DiskFile::setLinking(way < 2);
        // On the worker that makes the copy, so with no test macros.
        DiskFile::setBeforeNaming([&planted](const QString &path) {
            if (!planted.isEmpty()) return;
            planted << path;
            QFile appeared(path);
            if (appeared.open(QIODevice::WriteOnly)) appeared.write("appeared\n");
        });
        const auto restore = qScopeGuard([] {
            DiskFile::setBeforeNaming({});
            DiskFile::setRenameWithoutReplacing(true);
            DiskFile::setLinking(true);
        });
        QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth));
        QTRY_COMPARE(document->state(), Document::State::Clean);
        QCOMPARE(planted.size(), 1);
        QCOMPARE(readFile(planted.first()), QByteArray("appeared\n"));
        // c.md, the file that appeared, and the copy under the next name: nothing else.
        const QStringList names = QDir(folder).entryList(QDir::Files | QDir::Hidden);
        QCOMPARE(names.size(), 3);
        const QStringList copies = names.filter(QStringLiteral(" 2).md"));
        QCOMPARE(copies.size(), 1);
        QCOMPARE(readFile(folder + u'/' + copies.first()), QByteArray("mine base\n"));
    }
}

void ParchTests::aPathThatNowLeadsElsewhereIsNeverTaken()
{
    // A document's file replaced by a link to another file of the writer's
    // waits in conflict, saying where its path leads now. Nothing is written
    // there and nothing taken from there, unless the writer follows the link.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString edited = folder + QStringLiteral("/plan.md"), clean = folder + QStringLiteral("/notes.md");
    writeFile(edited, "base\n");
    writeFile(clean, "notes\n");
    QString entryId;
    {
        DocumentStore store(paths);
        Document *document = store.open(edited);
        Document *untouched = store.open(clean);
        QVERIFY(document && untouched);
        entryId = document->id();
        document->buffer()->replace({0, 0}, QStringLiteral("mine "));
        QVERIFY(Recovery(paths).write(document->id(), document->path(), *document->buffer()->snapshot(),
                                      document->diskVersion()));
        for (const QString &file : {edited, clean}) {
            QVERIFY(QFile::remove(file));
            QVERIFY(QFile::link(target, file));
        }
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(untouched->state(), Document::State::Conflict, 3000);
        const QString said = QStringLiteral("“plan.md” now leads to “%1”").arg(target);
        QCOMPARE(document->leadsTo(), target);
        QCOMPARE(document->lastError(), said);
        QCOMPARE(document->path(), edited);
        // A clean document is not taken over by the file either.
        QCOMPARE(untouched->leadsTo(), target);
        QCOMPARE(untouched->buffer()->text(), QStringLiteral("notes\n"));
        QCOMPARE(untouched->path(), clean);
        // Overwrite and Reload are refused, and nothing reaches the file.
        QString refusal;
        QVERIFY(!store.resolveConflict(document, ConflictChoice::Overwrite, &refusal));
        QCOMPARE(refusal, said);
        QVERIFY(!store.resolveConflict(document, ConflictChoice::Reload, &refusal));
        store.save(document);
        store.waitForSaves();
        QCOMPARE(readFile(target), QByteArray("precious\n"));
        QCOMPARE(document->state(), Document::State::Conflict);
        // Keep both saves a copy in the document's own folder and leaves the
        // document on its own text, still waiting.
        QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth, &refusal));
        store.waitForSaves();
        QTRY_COMPARE(QDir(folder).entryList({QStringLiteral("plan (conflict *).md")}, QDir::Files).size(), 1);
        const QString copy = folder + u'/' + QDir(folder).entryList({QStringLiteral("plan (conflict *).md")}, QDir::Files).first();
        QCOMPARE(readFile(copy), QByteArray("mine base\n"));
        QVERIFY(!QFileInfo(copy).isSymLink());
        QCOMPARE(readFile(target), QByteArray("precious\n"));
        QCOMPARE(document->state(), Document::State::Conflict);
        QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
        QCOMPARE(document->leadsTo(), target);
        // Following the link is the writer's choice, and takes the file.
        QVERIFY(store.resolveConflict(untouched, ConflictChoice::Follow, &refusal));
        store.waitForSaves();
        QCOMPARE(untouched->path(), target);
        QCOMPARE(untouched->buffer()->text(), QStringLiteral("precious\n"));
        QCOMPARE(untouched->state(), Document::State::Clean);
    }
    // After a restart, the text recovery kept waits the same way.
    DocumentStore store(paths);
    const QList<Recovery::Entry> entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().documentId, entryId);
    Document *restored = store.restore(entries.first());
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Conflict);
    QCOMPARE(restored->path(), edited);
    QCOMPARE(restored->leadsTo(), target);
    QCOMPARE(restored->buffer()->text(), QStringLiteral("mine base\n"));
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::saveAsItsOwnNameNeverWritesThroughALink()
{
    // Save as… from the conflict of a path that now leads to another file,
    // answered with the document's own name, as the picker suggests, puts
    // the text at that name in place of the link: the document stays on its
    // own path, and the file the link led to is never written or taken. So
    // does the same name reached through a link to its folder.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md");
    writeFile(plan, "base\n");
    const QString via = folder + QStringLiteral("/via");
    QVERIFY(QFile::link(folder, via));
    DocumentStore store(paths);
    Document *document = store.open(plan);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    for (const QString &name : {plan, via + QStringLiteral("/plan.md")}) {
        QVERIFY(QFile::remove(plan));
        QVERIFY(QFile::link(target, plan));
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
        QCOMPARE(document->leadsTo(), target);
        store.saveAs(document, name);
        store.waitForSaves();
        QCOMPARE(readFile(target), QByteArray("precious\n"));
        QCOMPARE(document->path(), plan);
        QVERIFY(!QFileInfo(plan).isSymLink());
        QCOMPARE(readFile(plan), QByteArray("mine base\n"));
        QCOMPARE(document->state(), Document::State::Clean);
        // The watch looks at the file the save put there before it is swapped again.
        QTest::qWait(300);
    }
}

void ParchTests::savingAsItsOwnNameWaitsWhenSomethingTookIt()
{
    // Save as… to the document's own name, by its own path or through a
    // link to its folder, that finds a file put at the name meanwhile waits
    // in the same conflict, with the same one sentence.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md"), via = folder + QStringLiteral("/via");
    writeFile(plan, "base\n");
    QVERIFY(QFile::link(folder, via));
    DocumentStore store(paths);
    Document *document = store.open(plan);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    const QString said = QStringLiteral("Something else is now at “plan.md” in “%1”, so parch did not save over it.")
                             .arg(QFileInfo(folder).fileName());
    DiskFile::setBeforeNaming([](const QString &path) {
        QFile::remove(path);
        QFile planted(path);
        if (planted.open(QIODevice::WriteOnly)) planted.write("planted\n");
    });
    const auto restore = qScopeGuard([] { DiskFile::setBeforeNaming({}); });
    for (const QString &name : {plan, via + QStringLiteral("/plan.md")}) {
        QVERIFY(QFile::remove(plan));
        QVERIFY(QFile::link(target, plan));
        QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), target, 3000);
        store.saveAs(document, name);
        store.waitForSaves();
        QCOMPARE(document->state(), Document::State::Conflict);
        QVERIFY(document->nameTaken());
        QCOMPARE(document->lastError(), said);
        QCOMPARE(document->path(), plan);
        QCOMPARE(readFile(plan), QByteArray("planted\n"));
        QCOMPARE(readFile(target), QByteArray("precious\n"));
        QTest::qWait(300);
    }
}

void ParchTests::filesMadeAgainKeepTheirMode()
{
    // A file parch makes in place of the document's own, by Put this text or
    // Save again, has that file's last known mode, not the default one; a
    // Keep both copy has the mode of the file it is kept beside.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const mode_t before = ::umask(022);
    const auto restoreMask = qScopeGuard([before] { ::umask(before); });
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md");
    writeFile(plan, "base\n");
    QVERIFY(QFile::setPermissions(plan, QFile::ReadOwner | QFile::WriteOwner));
    const auto modeOf = [](const QString &path) {
        struct stat state {};
        return ::lstat(QFile::encodeName(path).constData(), &state) == 0 ? int(state.st_mode & 07777) : -1;
    };
    DocumentStore store(paths);
    Document *document = store.open(plan);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    // Put this text in place of a link.
    QVERIFY(QFile::remove(plan));
    QVERIFY(QFile::link(target, plan));
    QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), target, 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::PutHere));
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(modeOf(plan), 0600);
    QTest::qWait(300);
    // Save again where the file was.
    document->buffer()->replace({0, 0}, QStringLiteral("more "));
    QVERIFY(QFile::remove(plan));
    QTRY_VERIFY_WITH_TIMEOUT(document->fileMissing(), 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Overwrite));
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(plan), QByteArray("more mine base\n"));
    QCOMPARE(modeOf(plan), 0600);
    QTest::qWait(300);
    // A copy kept beside it.
    document->buffer()->replace({0, 0}, QStringLiteral("again "));
    writeFile(plan, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth));
    store.waitForSaves();
    const QStringList copies = QDir(folder).entryList({QStringLiteral("plan (conflict *).md")}, QDir::Files);
    QCOMPARE(copies.size(), 1);
    QCOMPARE(modeOf(folder + u'/' + copies.first()), 0600);
}

void ParchTests::aConflictsReasonFollowsWhatIsAtTheName()
{
    // The reason a conflict gives follows its cause: once the link that took
    // the name goes, the document says its file no longer exists, and once a
    // file is back, that the file changed; and it can be saved again.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    QVERIFY(QFile::remove(file));
    QTRY_VERIFY_WITH_TIMEOUT(document->fileMissing(), 3000);
    QVERIFY(QFile::link(elsewhere.filePath(QStringLiteral("planted")), file));
    QTest::qWait(200);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Overwrite));
    store.waitForSaves();
    QVERIFY(document->nameTaken());
    QVERIFY2(document->lastError().startsWith(QStringLiteral("Something else is now at")), qPrintable(document->lastError()));
    QVERIFY(QFile::remove(file));
    QTRY_VERIFY_WITH_TIMEOUT(!document->nameTaken(), 3000);
    QCOMPARE(document->lastError(), QStringLiteral("The file no longer exists"));
    QVERIFY(document->fileMissing());
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->lastError(), QStringLiteral("The file has changed since this text was last saved"),
                              3000);
    QVERIFY(!document->nameTaken());
    QCOMPARE(document->state(), Document::State::Conflict);
}

void ParchTests::putThisTextReplacesTheLink()
{
    // Put this text writes the text at the document's own name, in place of
    // the link there: the document stays on its path, clean, and the file
    // the link led to is untouched. A folder at the name is left as it is.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md");
    writeFile(plan, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(plan);
    QVERIFY(document);
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    const auto swap = [&] {
        QVERIFY(QFile::remove(plan));
        QVERIFY(QFile::link(target, plan));
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
        QCOMPARE(document->leadsTo(), target);
    };
    swap();
    QString refusal;
    QVERIFY(store.resolveConflict(document, ConflictChoice::PutHere, &refusal));
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), plan);
    QVERIFY(!QFileInfo(plan).isSymLink());
    QCOMPARE(readFile(plan), QByteArray("mine base\n"));
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    // Only a link gives way: a folder put there meanwhile stays.
    QTest::qWait(300);
    document->buffer()->replace({0, 0}, QStringLiteral("more "));
    swap();
    DiskFile::setBeforeNaming([&](const QString &path) {
        QFile::remove(path);
        QDir().mkdir(path);
    });
    const auto restore = qScopeGuard([] { DiskFile::setBeforeNaming({}); });
    QVERIFY(store.resolveConflict(document, ConflictChoice::PutHere, &refusal));
    store.waitForSaves();
    DiskFile::setBeforeNaming({});
    QVERIFY(QFileInfo(plan).isDir());
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("more mine base\n"));
    QVERIFY(document->nameTaken());
    // It is chosen only while the path leads to another file.
    QVERIFY(!store.resolveConflict(document, ConflictChoice::PutHere, &refusal));
    store.waitForSaves();
    QVERIFY(QFileInfo(plan).isDir());
}

void ParchTests::aSessionTabNeverReopensThroughASwappedLink()
{
    // A tab the session recorded at a path that leads to another file now,
    // as when a link took the file's place while parch was closed, opens in
    // conflict with no text: nothing is read into it from that file. Its
    // last kept version can be restored, and then put at its name; or the
    // link can be followed.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md");
    writeFile(plan, "plan\n");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString kept;
    {
        History history(paths);
        kept = history.capture(id, "kept\n");
        QVERIFY(!kept.isEmpty());
    }
    SessionState state{{WindowState{{{id, plan, 0, 0, 0}}, 0, false, QSize(900, 700)}}};
    QVERIFY(Session::save(paths, state));
    QVERIFY(QFile::remove(plan));
    QVERIFY(QFile::link(target, plan));
    Backend backend(paths);
    backend.restoreSession();
    TabModel *tabs = backend.windows().first()->tabs();
    Document *document = tabs->documentAt(0);
    QVERIFY(document);
    QCOMPARE(document->path(), plan);
    QCOMPARE(document->id(), id);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->leadsTo(), target);
    QCOMPARE(document->lastError(), QStringLiteral("“plan.md” now leads to “%1”").arg(target));
    QCOMPARE(document->buffer()->text(), QString());
    QVERIFY(document->isTextless());
    // It has nothing to save, keep or put anywhere.
    DocumentStore *store = backend.store();
    QString refusal;
    QVERIFY(!store->resolveConflict(document, ConflictChoice::PutHere, &refusal));
    QVERIFY(!store->resolveConflict(document, ConflictChoice::KeepBoth, &refusal));
    store->saveAs(document, plan);
    store->waitForSaves();
    QVERIFY(QFileInfo(plan).isSymLink());
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    // Its last kept version comes back, and can then be put at its name.
    QCOMPARE(store->restoreVersion(document, kept), DocumentStore::Restore::Restored);
    QCOMPARE(document->buffer()->text(), QStringLiteral("kept\n"));
    QVERIFY(!document->isTextless());
    QCOMPARE(document->state(), Document::State::Conflict);
    QVERIFY(store->resolveConflict(document, ConflictChoice::PutHere, &refusal));
    store->waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    QVERIFY(!QFileInfo(plan).isSymLink());
    QCOMPARE(readFile(plan), QByteArray("kept\n"));
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::aRecordedPathSpelledAnotherWayOpensWithoutText()
{
    // A launch that spells a recorded path another way, with ./ or .., from
    // the folder it is in, reaches the same document: the path leads to
    // another file now, so it opens with no text of its own.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString notes = folder + QStringLiteral("/notes.md");
    writeFile(notes, "notes\n");
    QString id;
    {
        DocumentStore store(paths);
        Document *document = store.open(notes);
        QVERIFY(document);
        id = document->id();
    }
    QVERIFY(QFile::remove(notes));
    QVERIFY(QFile::link(target, notes));
    const QString up = QStringLiteral("../") + QFileInfo(folder).fileName() + QStringLiteral("/notes.md");
    for (const QString &spelled : {QStringLiteral("./notes.md"), up}) {
        DocumentStore store(paths);
        // As a launch from that folder gives it.
        const QStringList files = absoluteFiles({spelled}, folder);
        Document *document = store.open(files.first());
        QVERIFY(document);
        QCOMPARE(document->path(), notes);
        QCOMPARE(document->id(), id);
        QVERIFY(document->isTextless());
        // The launch gives the path cleaned.
        QCOMPARE(files, QStringList{notes});
        // The store looks a path up as cleaned, however it is spelled.
        QVERIFY(store.drop(document));
        Document *again = store.open(folder + u'/' + spelled);
        QVERIFY(again);
        QCOMPARE(again->path(), notes);
        QVERIFY(again->isTextless());
    }
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::anUncleanSpellingThroughAFolderLinkOpensWithoutText()
{
    // A folder on a recorded path is replaced by a link to another folder,
    // which holds a file of the same name. However a launch spells the
    // recorded path, clean or with "." or ".." or a doubled "/", it reaches
    // the recorded document, which opens with no text of its own: the file
    // in the other folder never opens as a document of its own.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString base = QFileInfo(root.path()).canonicalFilePath();
    const QString other = QFileInfo(elsewhere.path()).canonicalFilePath();
    QVERIFY(QDir(base).mkdir(QStringLiteral("notes")));
    QVERIFY(QDir(base).mkdir(QStringLiteral("sub")));
    const QString notes = base + QStringLiteral("/notes/notes.md");
    writeFile(notes, "notes\n");
    writeFile(other + QStringLiteral("/notes.md"), "precious\n");
    // A second recorded file, which a spelling below names only as text.
    const QString plain = base + QStringLiteral("/plain.md");
    writeFile(plain, "plain\n");
    writeFile(other + QStringLiteral("/plain.md"), "theirs\n");
    QString id, plainId;
    {
        DocumentStore store(paths);
        Document *document = store.open(notes);
        Document *second = store.open(plain);
        QVERIFY(document && second);
        id = document->id();
        plainId = second->id();
    }
    QVERIFY(QFile::remove(notes));
    QVERIFY(QDir(base).rmdir(QStringLiteral("notes")));
    QVERIFY(QFile::link(other, base + QStringLiteral("/notes")));
    const QStringList spellings{notes, base + QStringLiteral("/notes/./notes.md"), base + QStringLiteral("//notes/notes.md"),
                                base + QStringLiteral("/sub/../notes/notes.md")};
    for (const QString &spelled : spellings) {
        DocumentStore store(paths);
        // As a launch gives it, and as it is spelled.
        for (const QString &given : {absoluteFiles({spelled}, base).first(), spelled}) {
            Document *document = store.open(given);
            QVERIFY2(document, qPrintable(given));
            QVERIFY2(document->isTextless(), qPrintable(given + QStringLiteral(" opened ") + document->path()));
            QCOMPARE(document->path(), notes);
            QCOMPARE(document->id(), id);
            QCOMPARE(document->leadsTo(), other + QStringLiteral("/notes.md"));
            QVERIFY(store.drop(document));
        }
    }
    QCOMPARE(readFile(other + QStringLiteral("/notes.md")), QByteArray("precious\n"));
    // ".." after a link names what the kernel names, which text alone
    // would take for the recorded file beside the link: that file is
    // opened, and the recorded one is left as it is.
    QVERIFY(QDir(other).mkdir(QStringLiteral("deep")));
    QVERIFY(QFile::link(other + QStringLiteral("/deep"), base + QStringLiteral("/far")));
    const QString past = base + QStringLiteral("/far/../plain.md");
    QCOMPARE(absoluteFiles({past}, base), QStringList{other + QStringLiteral("/plain.md")});
    DocumentStore store(paths);
    for (const QString &given : {absoluteFiles({past}, base).first(), past}) {
        Document *document = store.open(given);
        QVERIFY(document);
        QCOMPARE(document->path(), other + QStringLiteral("/plain.md"));
        QCOMPARE(document->buffer()->text(), QStringLiteral("theirs\n"));
        QVERIFY(document->id() != plainId);
        QVERIFY(!document->isTextless());
    }
    QCOMPARE(readFile(plain), QByteArray("plain\n"));
}

void ParchTests::aTextlessDocumentTakesItsFileWhenItComesBack()
{
    // A document with no text of its own whose link goes, and whose file
    // comes back at its name, can take that file: it is its own name, with
    // no link. While nothing is there, its refusals mention no link.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString notes = folder + QStringLiteral("/notes.md");
    writeFile(notes, "notes\n");
    {
        DocumentStore store(paths);
        QVERIFY(store.open(notes));
    }
    QVERIFY(QFile::remove(notes));
    QVERIFY(QFile::link(target, notes));
    DocumentStore store(paths);
    Document *document = store.open(notes);
    QVERIFY(document && document->isTextless());
    QVERIFY(QFile::remove(notes));
    QTRY_VERIFY_WITH_TIMEOUT(document->fileMissing(), 3000);
    QString refusal;
    QVERIFY(!store.resolveConflict(document, ConflictChoice::Overwrite, &refusal));
    QCOMPARE(refusal, QStringLiteral("This document has no text of its own: restore a version from its history"));
    writeFile(notes, "back\n");
    QTRY_VERIFY_WITH_TIMEOUT(!document->fileMissing(), 3000);
    QTest::qWait(300);
    QVERIFY(!store.resolveConflict(document, ConflictChoice::Overwrite, &refusal));
    QVERIFY2(!refusal.contains(QStringLiteral("link")), qPrintable(refusal));
    const bool reloaded = store.resolveConflict(document, ConflictChoice::Reload, &refusal);
    QVERIFY2(reloaded, qPrintable(refusal));
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->buffer()->text(), QStringLiteral("back\n"));
    QVERIFY(!document->isTextless());
    QCOMPARE(document->path(), notes);
}

void ParchTests::closingATextlessTabKeepsNothingInRecovery()
{
    // A document with no text of its own never reaches recovery: closing its
    // tab keeps nothing there, and the next launch says nothing came back.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md"), other = folder + QStringLiteral("/other.md");
    writeFile(plan, "plan\n");
    writeFile(other, "other\n");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString id2 = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        History history(paths);
        QVERIFY(!history.capture(id, "kept\n").isEmpty());
    }
    SessionState state{{WindowState{{{id, plan, 0, 0, 0}, {id2, other, 0, 0, 0}}, 0, false, QSize(900, 700)}}};
    QVERIFY(Session::save(paths, state));
    QVERIFY(QFile::remove(plan));
    QVERIFY(QFile::link(target, plan));
    {
        Backend backend(paths);
        backend.restoreSession();
        TabModel *tabs = backend.windows().first()->tabs();
        Document *document = tabs->documentAt(0);
        QVERIFY(document);
        QVERIFY(document->isTextless());
        QCOMPARE(Recovery(paths).pending().size(), 0);
        backend.requestCloseTab(0);
        QTRY_COMPARE(tabs->rowCount(), 1);
        QCOMPARE(Recovery(paths).pending().size(), 0);
    }
    QCOMPARE(Recovery(paths).pending().size(), 0);
    {
        Backend backend(paths);
        QCOMPARE(backend.restoreSession(), QStringList());
        for (Document *document : backend.store()->documents()) QVERIFY(document->id() != id || document->isTextless());
    }
    // Nor does letting go of one opened again touch what recovery notes for it.
    QVERIFY(Recovery(paths).noteSave(id, QByteArray(), QByteArray(32, 'x'), 1));
    DocumentStore store(paths);
    Document *again = store.open(plan);
    QVERIFY(again && again->isTextless());
    QVERIFY(store.drop(again));
    QVERIFY(Recovery(paths).lastSave(id).has_value());
    QCOMPARE(Recovery(paths).pending().size(), 0);
}

void ParchTests::aRecoveryEntryThatCannotBeOpenedOutlastsTheSession()
{
    // Text recovery kept for a path that now leads to a file another
    // document has open stays in recovery, untouched, and the launch says
    // so; its tab does not open without text over it.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString plan = folder + QStringLiteral("/plan.md"), other = folder + QStringLiteral("/other.md");
    writeFile(plan, "plan\n");
    writeFile(other, "other\n");
    QString planId, otherId;
    {
        DocumentStore store(paths);
        Document *first = store.open(other);
        Document *second = store.open(plan);
        QVERIFY(first && second);
        otherId = first->id();
        planId = second->id();
        first->buffer()->replace({0, 0}, QStringLiteral("edited "));
        second->buffer()->replace({0, 0}, QStringLiteral("mine "));
        Recovery recovery(paths);
        QVERIFY(recovery.write(otherId, other, *first->buffer()->snapshot(), first->diskVersion()));
        QTest::qWait(20);
        QVERIFY(recovery.write(planId, plan, *second->buffer()->snapshot(), second->diskVersion()));
    }
    SessionState state{{WindowState{{{otherId, other, 0, 0, 0}, {planId, plan, 0, 0, 0}}, 0, false, QSize(900, 700)}}};
    QVERIFY(Session::save(paths, state));
    QVERIFY(QFile::remove(plan));
    QVERIFY(QFile::link(other, plan));
    const auto entryFor = [&](const QString &id) {
        for (const Recovery::Entry &entry : Recovery(paths).pending())
            if (entry.documentId == id) return entry.text;
        return QStringLiteral("(none)");
    };
    {
        Backend backend(paths);
        const QStringList said = backend.restoreSession();
        QVERIFY2(said.join(u'\n').contains(QStringLiteral("plan.md")), qPrintable(said.join(u'\n')));
        TabModel *tabs = backend.windows().first()->tabs();
        QCOMPARE(tabs->rowCount(), 2);
        tabs->setActiveIndex(1);
        QVERIFY(!tabs->documentAt(1));
        for (Document *document : backend.store()->documents()) QVERIFY(!document->isTextless());
        QCOMPARE(entryFor(planId), QStringLiteral("mine plan\n"));
        backend.requestCloseTab(1);
        QTRY_COMPARE(tabs->rowCount(), 1);
        QCOMPARE(entryFor(planId), QStringLiteral("mine plan\n"));
    }
    QCOMPARE(entryFor(planId), QStringLiteral("mine plan\n"));
}

void ParchTests::aHeldEntryComesBackWhenItsFileDoes()
{
    // Text recovery holds since the launch, because its path led to another
    // document's file then, is not lost when a file is at that path again
    // in the same session: opening it brings the text back, in conflict
    // with the file, and writing on leaves it in the document and in
    // history, never in neither.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    HeldEntry held;
    leaveAHeldEntry(paths, QFileInfo(root.path()).canonicalFilePath(), held);
    if (QTest::currentTestFailed()) return;
    Backend backend(paths);
    backend.restoreSession();
    QCOMPARE(recoveryText(paths, held.planId), QStringLiteral("mine plan\n"));
    TabModel *tabs = backend.windows().first()->tabs();
    // The link goes, and a file is at plan.md again.
    QVERIFY(QFile::remove(held.plan));
    writeFile(held.plan, "fixed\n");
    QString error;
    Document *document = backend.store()->open(held.plan, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->id(), held.planId);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine plan\n"));
    QCOMPARE(document->state(), Document::State::Conflict);
    tabs->openDocument(document);
    document->buffer()->replace({0, 0}, QStringLiteral("typed "));
    // Autosave has its turn: a snapshot, and a save if it made one.
    QTRY_COMPARE_WITH_TIMEOUT(recoveryText(paths, held.planId), QStringLiteral("typed mine plan\n"), 5000);
    QTest::qWait(Autosave::SaveIdleMs + 300);
    backend.store()->waitForSaves();
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("typed mine plan\n"));
    QCOMPARE(readFile(held.plan), QByteArray("fixed\n"));
    const QStringList kept = versionTexts(backend.history(), held.planId);
    QVERIFY2(kept.contains(QStringLiteral("mine plan\n")), qPrintable(kept.join(u'|')));
    QVERIFY(document->buffer()->text().contains(QStringLiteral("mine plan")) || kept.contains(QStringLiteral("mine plan\n")));
}

void ParchTests::aHeldEntryIsRestoredHoweverItsPathIsOpened_data()
{
    QTest::addColumn<QString>("route");
    QTest::addColumn<QByteArray>("back");
    // Recent and the library's folders open a file the same way.
    QTest::newRow("the held tab, shown again") << QStringLiteral("tab") << QByteArray("fixed\n");
    QTest::newRow("Recent, or the library") << QStringLiteral("library") << QByteArray("fixed\n");
    QTest::newRow("a launch") << QStringLiteral("launch") << QByteArray("fixed\n");
    QTest::newRow("another name that leads to the file") << QStringLiteral("alias") << QByteArray("fixed\n");
    QTest::newRow("a file that holds what the text was made on") << QStringLiteral("tab") << QByteArray("plan\n");
    QTest::newRow("a file that holds the text already") << QStringLiteral("library") << QByteArray("mine plan\n");
}

void ParchTests::aHeldEntryIsRestoredHoweverItsPathIsOpened()
{
    // Once the path of text recovery holds leads to its own file again,
    // whichever way a document is opened there brings that text back as a
    // restart would: in conflict with a file that changed, as unsaved text
    // over the file it was made on, or clean when the file holds it. The
    // entry is the document's own from then on.
    QFETCH(QString, route);
    QFETCH(QByteArray, back);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    HeldEntry held;
    leaveAHeldEntry(paths, folder, held);
    if (QTest::currentTestFailed()) return;
    Backend backend(paths);
    backend.restoreSession();
    TabModel *tabs = backend.windows().first()->tabs();
    // While the link is there, its tab holds no document.
    tabs->setActiveIndex(1);
    QVERIFY(!tabs->documentAt(1));
    tabs->setActiveIndex(0);
    QCOMPARE(recoveryText(paths, held.planId), QStringLiteral("mine plan\n"));
    QVERIFY(QFile::remove(held.plan));
    writeFile(held.plan, back);
    if (route == QLatin1String("tab")) {
        tabs->setActiveIndex(1);
    } else if (route == QLatin1String("library")) {
        backend.openFromLibrary(held.plan);
    } else if (route == QLatin1String("launch")) {
        backend.openForwarded({held.plan}, {});
    } else {
        const QString alias = folder + QStringLiteral("/alias.md");
        QVERIFY(QFile::link(held.plan, alias));
        backend.openFiles({alias});
    }
    // The tab that waited for it shows it, in front.
    QCOMPARE(tabs->rowCount(), 2);
    QCOMPARE(tabs->activeIndex(), 1);
    Document *document = tabs->documentAt(1);
    QVERIFY(document);
    QCOMPARE(document->id(), held.planId);
    QCOMPARE(document->path(), held.plan);
    QCOMPARE(backend.store()->documents().size(), 2);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine plan\n"));
    if (back == "mine plan\n") {
        // The file holds the text, so nothing is left to keep or to say.
        QCOMPARE(document->state(), Document::State::Clean);
        QCOMPARE(recoveryText(paths, held.planId), QStringLiteral("(none)"));
        return;
    }
    QVERIFY2(backend.problemDetail().contains(QStringLiteral("came back from recovery: plan.md")),
             qPrintable(backend.problem() + u'\n' + backend.problemDetail()));
    if (back == "plan\n") {
        // The text is saved over the file it was made on, as any edit is.
        QVERIFY(document->state() != Document::State::Conflict);
        QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Clean, 5000);
        QCOMPARE(readFile(held.plan), QByteArray("mine plan\n"));
        QTRY_COMPARE(recoveryText(paths, held.planId), QStringLiteral("(none)"));
        return;
    }
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->lastError(), QStringLiteral("The file has changed since this text was last saved"));
    QCOMPARE(readFile(held.plan), back);
    // Recovery follows the document's text again.
    document->buffer()->replace({0, 0}, QStringLiteral("more "));
    QTRY_COMPARE_WITH_TIMEOUT(recoveryText(paths, held.planId), QStringLiteral("more mine plan\n"), 5000);
    QCOMPARE(readFile(held.plan), back);
}

void ParchTests::aHeldEntryStaysAsItIsWhileItsPathLeadsElsewhere()
{
    // While the path of text recovery holds still leads elsewhere, nothing
    // opens there and nothing takes its id, whichever way it is asked for,
    // so the entry stays in recovery exactly as it was; its text is in
    // history from the launch on.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    HeldEntry held;
    leaveAHeldEntry(paths, folder, held);
    if (QTest::currentTestFailed()) return;
    // A second path the registry gives the same id, as a naming whose
    // record a crash cut short can leave.
    const QString second = folder + QStringLiteral("/second.md");
    writeFile(second, "second\n");
    {
        QFile registry(paths.registryFile());
        QVERIFY(registry.open(QIODevice::ReadOnly));
        QJsonObject entries = QJsonDocument::fromJson(registry.readAll()).object();
        registry.close();
        QCOMPARE(entries.value(held.plan).toString(), held.planId);
        entries.insert(second, held.planId);
        writeFile(paths.registryFile(), QJsonDocument(entries).toJson());
    }
    const QByteArray entry = readFile(held.file);
    QVERIFY(!entry.isEmpty());
    {
        Backend backend(paths);
        const QStringList said = backend.restoreSession();
        QVERIFY2(said.join(u'\n').contains(QStringLiteral("plan.md")), qPrintable(said.join(u'\n')));
        backend.store()->waitForSaves();
        const QList<History::Version> versions = backend.history()->versions(held.planId);
        QCOMPARE(versions.size(), 1);
        QCOMPARE(versions.first().reason, History::Reason::Recovery);
        QCOMPARE(backend.history()->read(held.planId, versions.first().id), QByteArray("mine plan\n"));
        const auto taken = [&] {
            for (const Document *document : backend.store()->documents())
                if (document->id() == held.planId || document->path() == held.plan) return true;
            return false;
        };
        TabModel *tabs = backend.windows().first()->tabs();
        tabs->setActiveIndex(1);
        QVERIFY(!tabs->documentAt(1));
        QVERIFY2(tabs->activeStatus().contains(QStringLiteral("kept in recovery")), qPrintable(tabs->activeStatus()));
        backend.openFromLibrary(held.plan);
        QVERIFY2(backend.problem().contains(QStringLiteral("kept in recovery")), qPrintable(backend.problem()));
        backend.openForwarded({held.plan}, {});
        QString error;
        QVERIFY(!backend.store()->open(held.plan, &error));
        QVERIFY2(error.contains(QStringLiteral("kept in recovery")), qPrintable(error));
        QVERIFY(!backend.store()->reopen(held.plan, held.planId));
        QVERIFY(!backend.store()->openDraft(held.planId));
        QVERIFY(!taken());
        QCOMPARE(tabs->rowCount(), 2);
        // The second path opens under an id of its own, and what is written
        // there goes to recovery, and leaves it, under that id.
        Document *other = backend.store()->open(second);
        QVERIFY(other);
        QVERIFY(other->id() != held.planId);
        tabs->openDocument(other);
        other->buffer()->replace({0, 0}, QStringLiteral("typed "));
        QTRY_COMPARE_WITH_TIMEOUT(recoveryText(paths, other->id()), QStringLiteral("typed second\n"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(other->state(), Document::State::Clean, 5000);
        QTRY_COMPARE(recoveryText(paths, other->id()), QStringLiteral("(none)"));
        QVERIFY(!taken());
        QCOMPARE(readFile(held.file), entry);
        // Nor does closing its tab touch it.
        backend.requestCloseTab(1);
        QTRY_COMPARE(tabs->rowCount(), 2);
        QCOMPARE(readFile(held.file), entry);
    }
    QCOMPARE(readFile(held.file), entry);
    QCOMPARE(readFile(held.other).right(6), QByteArray("other\n"));
}

void ParchTests::aHeldEntrysPathIsRecordedWithoutTheRegistry()
{
    // The registry gone, as when it was damaged and moved aside, the path
    // of text recovery holds is a recorded one still: asked for while it
    // leads elsewhere, it is not opened, nor is the file it leads to given
    // in its place, and the entry stays as it was. Once its own file is
    // back, opening it brings the text back.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    HeldEntry held;
    leaveAHeldEntry(paths, QFileInfo(root.path()).canonicalFilePath(), held);
    if (QTest::currentTestFailed()) return;
    QVERIFY(QFile::remove(paths.registryFile()));
    const QByteArray entry = readFile(held.file);
    Backend backend(paths);
    backend.restoreSession();
    QString error;
    QVERIFY(!backend.store()->open(held.plan, &error));
    QVERIFY2(error.contains(QStringLiteral("kept in recovery")), qPrintable(error));
    QVERIFY(!backend.store()->reopen(held.plan, QStringLiteral("not an id")));
    QVERIFY(!backend.store()->reopen(held.plan, held.planId));
    QCOMPARE(readFile(held.file), entry);
    QVERIFY(QFile::remove(held.plan));
    writeFile(held.plan, "fixed\n");
    Document *document = backend.store()->open(held.plan, &error);
    QVERIFY2(document, qPrintable(error));
    QCOMPARE(document->id(), held.planId);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine plan\n"));
    QCOMPARE(document->state(), Document::State::Conflict);
}

void ParchTests::aHeldEntrysTextIsInHistoryBeforeItsIdIsADocuments()
{
    // The text of an entry recovery holds goes into history on a worker,
    // begun at the launch, which does not wait for it; a document that
    // takes the entry's id before that has ended waits for it first.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    HeldEntry held;
    leaveAHeldEntry(paths, QFileInfo(root.path()).canonicalFilePath(), held);
    if (QTest::currentTestFailed()) return;
    Backend backend(paths);
    PoolBlock block;
    backend.restoreSession();
    QVERIFY(versionTexts(backend.history(), held.planId).isEmpty());
    QVERIFY(QFile::remove(held.plan));
    writeFile(held.plan, "fixed\n");
    Document *document = backend.store()->open(held.plan);
    QVERIFY(document);
    QCOMPARE(document->id(), held.planId);
    QCOMPARE(versionTexts(backend.history(), held.planId), QStringList{QStringLiteral("mine plan\n")});
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine plan\n"));
}

void ParchTests::aSessionTabAsksForItsFilesNameOnce()
{
    // Reopening a session's tab asks the kernel for its file's canonical
    // name once, whether the path leads to its own file or another now.
    // Nor is a path's folder resolved more than once for one opening,
    // however the path is spelled and whichever way it is opened, and not
    // at all for a clean path the registry holds.
    QTemporaryDir root, elsewhere, second;
    QVERIFY(root.isValid() && elsewhere.isValid() && second.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plain = folder + QStringLiteral("/plain.md"), linked = folder + QStringLiteral("/linked.md");
    writeFile(plain, "plain\n");
    QVERIFY(QFile::link(target, linked));
    QVERIFY(QDir(folder).mkdir(QStringLiteral("sub")));
    for (const char *name : {"one.md", "two.md", "three.md", "four.md"}) writeFile(folder + u'/' + QLatin1String(name), "text\n");
    // How often an opening has a folder resolved.
    const auto resolved = [](const std::function<void()> &opening) {
        const quint64 before = DiskFile::folderLookups();
        opening();
        return DiskFile::folderLookups() - before;
    };
    {
        DocumentStore store(paths);
        const QString tabId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (const QString &path : {plain, linked}) {
            const quint64 before = DiskFile::canonicalLookups();
            const quint64 folders = DiskFile::folderLookups();
            QString problem;
            Document *document = store.reopen(path, tabId, &problem);
            QVERIFY2(document, qPrintable(problem));
            QCOMPARE(DiskFile::canonicalLookups() - before, quint64(1));
            // The registry holds neither yet.
            QCOMPARE(DiskFile::folderLookups() - folders, quint64(1));
            QVERIFY(store.drop(document));
        }
        // The registry holds plain.md now: nothing is resolved for it, as
        // it is recorded or spelled another way, which text alone tidies.
        const QStringList spellings{plain, folder + QStringLiteral("/./plain.md"), folder + QStringLiteral("/sub/../plain.md")};
        for (const QString &spelled : spellings) {
            Document *document = nullptr;
            const quint64 before = DiskFile::canonicalLookups();
            QCOMPARE(resolved([&] { document = store.reopen(spelled, tabId); }), quint64(0));
            QCOMPARE(DiskFile::canonicalLookups() - before, quint64(1));
            QVERIFY(document);
            QCOMPARE(document->path(), plain);
            QVERIFY(store.drop(document));
            QCOMPARE(resolved([&] { document = store.open(spelled); }), quint64(0));
            QVERIFY(document && store.drop(document));
        }
        // A file the registry does not hold: once, however it is spelled.
        QCOMPARE(resolved([&] { QVERIFY(store.open(folder + QStringLiteral("/./one.md"))); }), quint64(1));
        QCOMPARE(resolved([&] { QVERIFY(store.open(folder + QStringLiteral("/sub/../two.md"))); }), quint64(1));
    }
    // The same when the application opens them.
    Backend backend(AppPaths::under(second.path()));
    backend.start({});
    TabModel *tabs = backend.windows().first()->tabs();
    const auto opens = [&](const QString &given) { return resolved([&] { backend.openFiles({given}); }); };
    QCOMPARE(opens(plain), quint64(1));
    QCOMPARE(tabs->activePath(), plain);
    QCOMPARE(opens(plain), quint64(0));
    QCOMPARE(opens(folder + QStringLiteral("/sub/../plain.md")), quint64(0));
    QCOMPARE(opens(folder + QStringLiteral("/./three.md")), quint64(1));
    QCOMPARE(tabs->activePath(), folder + QStringLiteral("/three.md"));
    QCOMPARE(opens(folder + QStringLiteral("/sub/../four.md")), quint64(1));
    QCOMPARE(tabs->activePath(), folder + QStringLiteral("/four.md"));
    // A file that is not there, and a folder, spelled unclean.
    QCOMPARE(opens(folder + QStringLiteral("/sub/../absent.md")), quint64(1));
    QCOMPARE(tabs->activePath(), folder + QStringLiteral("/absent.md"));
    QVERIFY(opens(folder + QStringLiteral("/sub/../sub")) <= quint64(1));
}

void ParchTests::aRecordedPathThatLeadsElsewhereOpensWithoutText()
{
    // A path parch recorded as a document's file, opened again from Recent,
    // the library or a launch, opens the same way when it leads to another
    // file now; following the link takes that file. A path that already led
    // through a link when it was first opened still opens through it.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString notes = folder + QStringLiteral("/notes.md"), through = folder + QStringLiteral("/through.md");
    writeFile(notes, "notes\n");
    QVERIFY(QFile::link(target, through));
    QString id;
    {
        DocumentStore store(paths);
        Document *document = store.open(notes);
        QVERIFY(document);
        id = document->id();
    }
    QVERIFY(QFile::remove(notes));
    QVERIFY(QFile::link(target, notes));
    DocumentStore store(paths);
    Document *document = store.open(notes);
    QVERIFY(document);
    QCOMPARE(document->id(), id);
    QCOMPARE(document->path(), notes);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->leadsTo(), target);
    QCOMPARE(document->buffer()->text(), QString());
    QVERIFY(document->isTextless());
    QString refusal;
    QVERIFY(store.resolveConflict(document, ConflictChoice::Follow, &refusal));
    store.waitForSaves();
    QCOMPARE(document->path(), target);
    QCOMPARE(document->buffer()->text(), QStringLiteral("precious\n"));
    QCOMPARE(document->state(), Document::State::Clean);
    QVERIFY(!document->isTextless());
    QVERIFY(store.drop(document));
    // A path named freshly through a link reaches the file it leads to.
    Document *named = store.open(through);
    QVERIFY(named);
    QCOMPARE(named->path(), target);
    QCOMPARE(named->buffer()->text(), QStringLiteral("precious\n"));
}

void ParchTests::aRecoveredLinkConflictKeepsTheFilesConventions()
{
    // Text recovery kept for a file with CRLF line endings and a BOM, brought
    // back while a link stood in the file's place, is saved with them once
    // the file comes back as it was.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const QString plan = folder + QStringLiteral("/plan.md");
    const QByteArray original = QByteArray("\xEF\xBB\xBF") + "one\r\ntwo\r\n";
    writeFile(plan, original);
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *document = store.open(plan);
        QVERIFY(document);
        document->buffer()->replace({0, 0}, QStringLiteral("x"));
        QString error;
        QVERIFY2(autosave.keep(document, &error), qPrintable(error));
    }
    QCOMPARE(readFile(plan), original);
    QVERIFY(QFile::rename(plan, plan + QStringLiteral(".away")));
    QVERIFY(QFile::link(target, plan));
    DocumentStore store(paths);
    const QList<Recovery::Entry> entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 1);
    Document *restored = store.restore(entries.first());
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Conflict);
    QCOMPARE(restored->leadsTo(), target);
    QVERIFY(QFile::remove(plan));
    QVERIFY(QFile::rename(plan + QStringLiteral(".away"), plan));
    QTRY_COMPARE_WITH_TIMEOUT(restored->state(), Document::State::Clean, 3000);
    store.waitForSaves();
    QCOMPARE(readFile(plan), QByteArray("\xEF\xBB\xBF") + "xone\r\ntwo\r\n");
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::aRecoveredDocumentKeepsItsFilesMode()
{
    // Recovery records the mode of the document's file with its text, so
    // that a file parch makes in its place after a restart, before it has
    // read the file again, has that mode and not the default one: a file
    // for its owner alone that a link replaced, or that went, while parch
    // was not running. An entry that recorded no mode gives the default.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const mode_t before = ::umask(022);
    const auto restoreMask = qScopeGuard([before] { ::umask(before); });
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    const auto modeOf = [](const QString &path) {
        struct stat state {};
        return ::lstat(QFile::encodeName(path).constData(), &state) == 0 ? int(state.st_mode & 07777) : -1;
    };
    // Each file ends with text only recovery holds, and is then replaced
    // or removed before the next start.
    const auto crashWith = [&](const QString &name) {
        const QString file = folder + u'/' + name;
        writeFile(file, "base\n");
        QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner);
        DocumentStore store(paths);
        Recovery recovery(paths);
        Autosave autosave(&store, &recovery);
        Document *document = store.open(file);
        document->buffer()->replace({0, 0}, QStringLiteral("mine "));
        autosave.keep(document);
        return document->id();
    };
    const QString linked = folder + QStringLiteral("/linked.md"), gone = folder + QStringLiteral("/gone.md");
    const QString old = folder + QStringLiteral("/old.md");
    const QString linkedId = crashWith(QStringLiteral("linked.md"));
    const QString goneId = crashWith(QStringLiteral("gone.md"));
    const QString oldId = crashWith(QStringLiteral("old.md"));
    QCOMPARE(modeOf(linked), 0600);
    for (const QString &file : {linked, old}) {
        QVERIFY(QFile::remove(file));
        QVERIFY(QFile::link(target, file));
    }
    QVERIFY(QFile::remove(gone));
    // The third entry is as one written before the mode was recorded.
    {
        const QString entryFile = paths.recovery() + u'/' + oldId + QStringLiteral(".recovery");
        const QByteArray all = readFile(entryFile);
        const qsizetype end = all.indexOf('\n');
        QVERIFY(end > 0);
        QJsonObject header = QJsonDocument::fromJson(all.left(end)).object();
        QJsonObject base = header.value(QStringLiteral("base")).toObject();
        base.remove(QStringLiteral("mode"));
        header.insert(QStringLiteral("base"), base);
        writeFile(entryFile, QJsonDocument(header).toJson(QJsonDocument::Compact) + all.mid(end));
    }
    DocumentStore store(paths);
    QHash<QString, Document *> restored;
    const QList<Recovery::Entry> entries = Recovery(paths).pending();
    QCOMPARE(entries.size(), 3);
    for (const Recovery::Entry &entry : entries) restored.insert(entry.documentId, store.restore(entry));
    QVERIFY(restored.value(linkedId) && restored.value(goneId) && restored.value(oldId));
    // Put this text, in place of the link.
    QCOMPARE(restored.value(linkedId)->leadsTo(), target);
    QVERIFY(store.resolveConflict(restored.value(linkedId), ConflictChoice::PutHere));
    store.waitForSaves();
    QCOMPARE(restored.value(linkedId)->state(), Document::State::Clean);
    QCOMPARE(readFile(linked), QByteArray("mine base\n"));
    QCOMPARE(modeOf(linked), 0600);
    // Save again, where the file was.
    QVERIFY(restored.value(goneId)->fileMissing());
    QVERIFY(store.resolveConflict(restored.value(goneId), ConflictChoice::Overwrite));
    store.waitForSaves();
    QCOMPARE(restored.value(goneId)->state(), Document::State::Clean);
    QCOMPARE(readFile(gone), QByteArray("mine base\n"));
    QCOMPARE(modeOf(gone), 0600);
    // The entry without a mode: the default, less the umask.
    QVERIFY(store.resolveConflict(restored.value(oldId), ConflictChoice::PutHere));
    store.waitForSaves();
    QCOMPARE(restored.value(oldId)->state(), Document::State::Clean);
    QCOMPARE(modeOf(old), 0644);
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    for (const Recovery::Entry &entry : entries)
        QCOMPARE(entry.base.mode, quint32(entry.documentId == oldId ? 0 : 0600));
}

void ParchTests::keepBothRunsOffTheUiThread()
{
    // Keep both reads the file and writes the copy on a worker: with the
    // workers held, choosing it returns at once, and the copy comes once
    // they are let go.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md";
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    const auto copies = [&] { return QDir(folder).entryList({QStringLiteral("c (conflict *).md")}, QDir::Files); };
    {
        PoolBlock block;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth));
        QVERIFY(clock.elapsed() < 500);
        QTest::qWait(100);
        QVERIFY(copies().isEmpty());
        QCOMPARE(document->state(), Document::State::Conflict);
        // A second choice waits for the first.
        QString refusal;
        QVERIFY(!store.resolveConflict(document, ConflictChoice::Reload, &refusal));
    }
    QTRY_COMPARE(copies().size(), 1);
    QCOMPARE(readFile(folder + u'/' + copies().first()), QByteArray("mine base\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->buffer()->text(), QStringLiteral("theirs\n"));
}

void ParchTests::keepBothThatLandsLateLeavesTheDocumentAlone()
{
    // Keep both's copy that lands after the document has moved on, here to
    // a new name, stays on disk with the text it was made with, and is
    // said; the document keeps its new name and its text.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md", named = folder + "/named.md";
    writeFile(file, "base\n");
    Backend backend(paths);
    DocumentStore &store = *backend.store();
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QSignalSpy left(&store, &DocumentStore::conflictCopyLeft);
    QList<Document *> opened;
    connect(&store, &DocumentStore::conflictCopyOpened, this, [&](Document *copy) { opened << copy; });
    {
        PoolBlock block;
        QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth));
        store.saveAs(document, named);
    }
    store.waitForSaves();
    QCOMPARE(document->path(), named);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(named), QByteArray("mine base\n"));
    const QStringList copies = QDir(folder).entryList({QStringLiteral("c (conflict *).md")}, QDir::Files);
    QCOMPARE(copies.size(), 1);
    QCOMPARE(readFile(folder + u'/' + copies.first()), QByteArray("mine base\n"));
    QVERIFY(opened.isEmpty());
    QCOMPARE(left.count(), 1);
    QCOMPARE(left.first().at(1).toString(), folder + u'/' + copies.first());
    // The notice names the copy.
    QCOMPARE(backend.problem(), QStringLiteral("Conflict copy kept: %1").arg(copies.first()));
    QVERIFY2(backend.problemDetail().startsWith(QStringLiteral("“%1” in “").arg(copies.first())),
             qPrintable(backend.problemDetail()));
    // Nothing more is saved anywhere.
    QTest::qWait(300);
    store.waitForSaves();
    QCOMPARE(readFile(named), QByteArray("mine base\n"));
    QCOMPARE(readFile(file), QByteArray("theirs\n"));
}

void ParchTests::aKeepBothCopyOfAClosedDocumentIsSaid()
{
    // A copy Keep both makes for a document closed before it lands stays on
    // disk, and the notice names it.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md";
    writeFile(file, "base\n");
    Backend backend(paths);
    DocumentStore &store = *backend.store();
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    {
        PoolBlock block;
        QVERIFY(store.resolveConflict(document, ConflictChoice::KeepBoth));
        QVERIFY(store.drop(document));
    }
    store.waitForSaves();
    QCoreApplication::processEvents();
    const QStringList copies = QDir(folder).entryList({QStringLiteral("c (conflict *).md")}, QDir::Files);
    QCOMPARE(copies.size(), 1);
    QCOMPARE(readFile(folder + u'/' + copies.first()), QByteArray("mine base\n"));
    QCOMPARE(backend.problem(), QStringLiteral("Conflict copy kept: %1").arg(copies.first()));
    QVERIFY2(backend.problemDetail().contains(QStringLiteral("c.md")), qPrintable(backend.problemDetail()));
}

void ParchTests::followRunsOffTheUiThread()
{
    // Follow reads the file the link leads to, and keeps this text in
    // history, on a worker: with the workers held, choosing it returns at
    // once, and the document takes that file once they are let go.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/plan.md", target = QFileInfo(elsewhere.path()).canonicalFilePath() + "/precious.txt";
    writeFile(file, "base\n");
    writeFile(target, "precious\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(target, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    const qsizetype versions = store.history()->versions(document->id()).size();
    {
        PoolBlock block;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(store.resolveConflict(document, ConflictChoice::Follow));
        QVERIFY(clock.elapsed() < 500);
        QTest::qWait(100);
        QCOMPARE(document->path(), file);
        QCOMPARE(document->state(), Document::State::Conflict);
        QCOMPARE(store.history()->versions(document->id()).size(), versions);
        // A second choice waits for the first.
        QString refusal;
        QVERIFY(!store.resolveConflict(document, ConflictChoice::KeepBoth, &refusal));
    }
    store.waitForSaves();
    QCOMPARE(document->path(), target);
    QCOMPARE(document->buffer()->text(), QStringLiteral("precious\n"));
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(store.history()->versions(document->id()).size(), versions + 1);
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::followKeepsThisTextInHistoryOnAWorker()
{
    // Once nothing refuses it, Follow keeps this text in history on a
    // worker, and takes the file only when that has ended and the document
    // is still as it was: while history is held, the landing has returned,
    // the document waits as it was, and a second choice is refused; text
    // typed meanwhile stays, and the link is not followed.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString target = QFileInfo(elsewhere.path()).canonicalFilePath() + QStringLiteral("/precious.txt");
    writeFile(target, "precious\n");
    DocumentStore store(paths);
    // History, held as a capture begins on a worker; one on the UI thread
    // cannot be held without stopping the test, and is noted instead.
    QSemaphore begun, gate;
    std::atomic<bool> onUiThread{false};
    History::setBeforeCapture([&] {
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) {
            onUiThread = true;
            return;
        }
        begun.release();
        gate.acquire();
    });
    const auto letGo = qScopeGuard([&] {
        gate.release(100);
        store.waitForSaves();
        History::setBeforeCapture({});
    });
    const auto linked = [&](const QString &name) -> Document * {
        const QString file = folder + u'/' + name;
        writeFile(file, "base\n");
        Document *document = store.open(file);
        document->buffer()->replace({0, 0}, QStringLiteral("mine "));
        QFile::remove(file);
        QFile::link(target, file);
        return document;
    };
    Document *document = linked(QStringLiteral("plan.md"));
    const QString file = document->path();
    QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), target, 3000);
    const qsizetype versions = store.history()->versions(document->id()).size();
    QVERIFY(store.resolveConflict(document, ConflictChoice::Follow));
    QTRY_VERIFY(onUiThread || begun.available() > 0);
    QVERIFY2(!onUiThread, "the text was kept in history on the UI thread");
    // The landing that began the capture has returned, and nothing is taken yet.
    QTest::qWait(100);
    QCOMPARE(document->path(), file);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
    QCOMPARE(store.history()->versions(document->id()).size(), versions);
    QString refusal;
    QVERIFY(!store.resolveConflict(document, ConflictChoice::KeepBoth, &refusal));
    QVERIFY(begun.tryAcquire());
    gate.release();
    store.waitForSaves();
    QCOMPARE(document->path(), target);
    QCOMPARE(document->buffer()->text(), QStringLiteral("precious\n"));
    QCOMPARE(document->state(), Document::State::Clean);
    const QList<History::Version> kept = store.history()->versions(document->id());
    QCOMPARE(kept.size(), versions + 1);
    QCOMPARE(kept.first().reason, History::Reason::Reload);
    QCOMPARE(store.history()->read(document->id(), kept.first().id), QByteArray("mine base\n"));
    QVERIFY(store.drop(document));
    // Written to while history takes the text: the text stays, and the
    // link is not followed.
    Document *second = linked(QStringLiteral("notes.md"));
    const QString notes = second->path();
    QTRY_COMPARE_WITH_TIMEOUT(second->leadsTo(), target, 3000);
    QVERIFY(store.resolveConflict(second, ConflictChoice::Follow));
    QTRY_VERIFY(onUiThread || begun.available() > 0);
    QVERIFY(!onUiThread);
    second->buffer()->replace({0, 0}, QStringLiteral("typed "));
    gate.release();
    store.waitForSaves();
    QCOMPARE(second->path(), notes);
    QCOMPARE(second->state(), Document::State::Conflict);
    QCOMPARE(second->leadsTo(), target);
    QCOMPARE(second->buffer()->text(), QStringLiteral("typed mine base\n"));
    QVERIFY2(second->lastError().contains(QStringLiteral("review it again")), qPrintable(second->lastError()));
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::followReachesOtherFilesInTheRuntimeFolder()
{
    // In the session's runtime folder only parch's own files are refused,
    // its socket and lock; a file elsewhere there, as a mounted share or a
    // portal puts there, can be followed.
    QTemporaryDir root, files;
    QVERIFY(root.isValid() && files.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString runtime = QFileInfo(root.path()).canonicalFilePath() + QStringLiteral("/runtime");
    const QString shared = runtime + QStringLiteral("/gvfs/share/doc.txt"), lock = runtime + QStringLiteral("/parch.sock.lock");
    writeFile(shared, "shared\n");
    writeFile(lock, "1\n");
    const QString file = QFileInfo(files.path()).canonicalFilePath() + QStringLiteral("/plan.md");
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    QVERIFY(document);
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(lock, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), lock, 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Follow));
    store.waitForSaves();
    QCOMPARE(document->path(), file);
    QCOMPARE(document->lastError(),
             QStringLiteral("“%1” is one of parch's own files; parch does not follow a link there.").arg(lock));
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(shared, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), shared, 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Follow));
    store.waitForSaves();
    QCOMPARE(document->path(), shared);
    QCOMPARE(document->buffer()->text(), QStringLiteral("shared\n"));
    QCOMPARE(document->state(), Document::State::Clean);
}

void ParchTests::followTakesOnlyTheFileShown()
{
    // Follow takes the file the link led to when it was chosen: a link
    // that leads elsewhere by the time it lands is not followed, nothing is
    // kept in history for it, and the writer is asked to review again.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/plan.md";
    const QString first = QFileInfo(elsewhere.path()).canonicalFilePath() + "/first.txt";
    const QString second = QFileInfo(elsewhere.path()).canonicalFilePath() + "/second.txt";
    writeFile(file, "base\n");
    writeFile(first, "first\n");
    writeFile(second, "second\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(first, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), first, 3000);
    const qsizetype versions = store.history()->versions(document->id()).size();
    {
        PoolBlock block;
        QVERIFY(store.resolveConflict(document, ConflictChoice::Follow));
        QVERIFY(QFile::remove(file));
        QVERIFY(QFile::link(second, file));
        QTest::qWait(200);
    }
    store.waitForSaves();
    QTest::qWait(300);
    store.waitForSaves();
    QVERIFY(document->path() != second);
    QCOMPARE(document->path(), file);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
    QVERIFY2(document->lastError().contains(QStringLiteral("review it again")), qPrintable(document->lastError()));
    QCOMPARE(store.history()->versions(document->id()).size(), versions);
    QCOMPARE(readFile(first), QByteArray("first\n"));
    QCOMPARE(readFile(second), QByteArray("second\n"));
}

void ParchTests::followKeepsTextTypedMeanwhile()
{
    // Text typed while Follow runs is never lost: the link is not
    // followed, and the document keeps its text, waiting.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/plan.md", target = QFileInfo(elsewhere.path()).canonicalFilePath() + "/precious.txt";
    writeFile(file, "base\n");
    writeFile(target, "precious\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(target, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    const qsizetype versions = store.history()->versions(document->id()).size();
    {
        PoolBlock block;
        QVERIFY(store.resolveConflict(document, ConflictChoice::Follow));
        document->buffer()->replace({0, 0}, QStringLiteral("typed "));
    }
    store.waitForSaves();
    QCOMPARE(document->buffer()->text(), QStringLiteral("typed mine base\n"));
    QCOMPARE(document->path(), file);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->leadsTo(), target);
    QVERIFY2(document->lastError().contains(QStringLiteral("review it again")), qPrintable(document->lastError()));
    QCOMPARE(store.history()->versions(document->id()).size(), versions);
    QCOMPARE(readFile(target), QByteArray("precious\n"));
}

void ParchTests::followNeverTakesOneOfParchsOwnFiles()
{
    // A link to one of parch's own files, such as an open draft's file or
    // the registry, is never followed: that file would have two writers.
    QTemporaryDir root, files;
    QVERIFY(root.isValid() && files.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(files.path()).canonicalFilePath();
    const QString file = folder + "/plan.md";
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("draft words\n"));
    store.save(draft);
    store.waitForSaves();
    Document *document = store.open(file);
    QVERIFY(document);
    store.waitForSaves();
    const QString draftFile = QFileInfo(DocumentStore::draftFile(paths, draft->id())).canonicalFilePath();
    const QString registry = QFileInfo(paths.registryFile()).canonicalFilePath();
    QVERIFY(!draftFile.isEmpty() && !registry.isEmpty());
    for (const QString &own : {draftFile, registry}) {
        QVERIFY(QFile::remove(file));
        QVERIFY(QFile::link(own, file));
        QTRY_COMPARE_WITH_TIMEOUT(document->leadsTo(), own, 3000);
        const QByteArray before = readFile(own);
        QString refusal;
        QVERIFY(store.resolveConflict(document, ConflictChoice::Follow, &refusal));
        store.waitForSaves();
        QCOMPARE(document->path(), file);
        QCOMPARE(document->state(), Document::State::Conflict);
        QCOMPARE(document->lastError(),
                 QStringLiteral("“%1” is one of parch's own files; parch does not follow a link there.").arg(own));
        QCOMPARE(readFile(own), before);
    }
    QCOMPARE(draft->buffer()->text(), QStringLiteral("draft words\n"));
}

void ParchTests::keepBothOpensTheCopyItMade()
{
    // A link put at the copy's name once the copy is made is never opened in
    // its place: the copy is opened as the file that was made, or not at all.
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/c.md", target = QFileInfo(elsewhere.path()).canonicalFilePath() + "/precious.txt";
    writeFile(file, "base\n");
    writeFile(target, "precious\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    // On the worker that makes the copy, so with no test macros.
    QStringList swapped;
    DiskFile::setAfterNaming([&](const QString &path) {
        swapped << path;
        QFile::rename(path, path + QStringLiteral(".moved"));
        QFile::link(target, path);
    });
    const auto restore = qScopeGuard([] { DiskFile::setAfterNaming({}); });
    QList<Document *> copies;
    connect(&store, &DocumentStore::conflictCopyOpened, this, [&](Document *copy) { copies << copy; });
    QString refusal;
    store.resolveConflict(document, ConflictChoice::KeepBoth, &refusal);
    store.waitForSaves();
    QCoreApplication::processEvents();
    QCOMPARE(swapped.size(), 1);
    QVERIFY(copies.isEmpty());
    for (Document *open : store.documents()) QVERIFY(open->path() != target);
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    QCOMPARE(readFile(swapped.first() + QStringLiteral(".moved")), QByteArray("mine base\n"));
    QCOMPARE(document->state(), Document::State::Conflict);
    QVERIFY2(document->lastError().startsWith(QStringLiteral("Cannot open the conflict copy")), qPrintable(document->lastError()));
}

void ParchTests::saveAgainNamesAFolderThatIsGone()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath() + QStringLiteral("/Essays");
    const QString file = folder + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    QVERIFY(QDir(folder).removeRecursively());
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    store.resolveConflict(document, ConflictChoice::Overwrite);
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::SaveFailed);
    QCOMPARE(document->lastError(),
             QStringLiteral("parch cannot write in “%1” (%2)").arg(folder, QString::fromLocal8Bit(std::strerror(ENOENT))));
}

void ParchTests::saveAgainNeverFollowsALinkWhereTheFileWas()
{
    QTemporaryDir root, elsewhere;
    QVERIFY(root.isValid() && elsewhere.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(root.path()).canonicalFilePath();
    const QString file = folder + "/a.md", target = elsewhere.filePath("planted");
    writeFile(file, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(file);
    document->buffer()->replace({0, 0}, "mine ");
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(document->fileMissing());
    // A link leading somewhere else now stands where the file was: saving
    // again, which the save's worker refuses, says why, and makes nothing
    // at its target.
    QVERIFY(QFile::link(target, file));
    QTest::qWait(200);
    QString refusal;
    store.resolveConflict(document, ConflictChoice::Overwrite, &refusal);
    store.waitForSaves();
    const QString said = QStringLiteral("Something else is now at “a.md” in “%1”, so parch did not save over it.")
                             .arg(QFileInfo(folder).fileName());
    QCOMPARE(document->lastError(), said);
    QVERIFY(document->nameTaken());
    QCOMPARE(document->state(), Document::State::Conflict);
    QVERIFY(!QFileInfo::exists(target));
    QVERIFY(QFileInfo(file).isSymLink());
    // Once nothing is there, saving again makes the file at its own path.
    QVERIFY(QFile::remove(file));
    QTest::qWait(200);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Overwrite, &refusal));
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    QVERIFY(!QFileInfo(file).isSymLink());
    QCOMPARE(readFile(file), QByteArray("mine base\n"));
    QVERIFY(!QFileInfo::exists(target));

    // A document opened through a link still saves through it.
    const QString real = elsewhere.filePath("real.md"), link = folder + "/link.md";
    writeFile(real, "real\n");
    QVERIFY(QFile::link(real, link));
    Document *linked = store.open(link);
    QVERIFY(linked);
    linked->buffer()->replace({0, 0}, "more ");
    store.save(linked);
    store.waitForSaves();
    QCOMPARE(linked->state(), Document::State::Clean);
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(readFile(real), QByteArray("more real\n"));
}

void ParchTests::conflictOverMissingFileKeepsRecovery()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "only copy\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.open(path);
    autosave.watch(document);
    QVERIFY(QFile::remove(path));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(!document->buffer()->modified());
    QTRY_COMPARE_WITH_TIMEOUT(recovery.pending().size(), 1, Autosave::RecoveryMaxMs + 2000);
    QCOMPARE(recovery.pending().first().text, QStringLiteral("only copy\n"));
}

void ParchTests::reappearingFileRefreshesTheReason()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    QVERIFY(QFile::remove(path));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QCOMPARE(document->lastError(), QStringLiteral("The file no longer exists"));
    writeFile(path, "back\n");
    QTRY_VERIFY_WITH_TIMEOUT(document->lastError() != QStringLiteral("The file no longer exists"), 3000);
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("base\n"));
    // Reload is possible again now.
    store.resolveConflict(document, ConflictChoice::Reload);
    QCOMPARE(document->buffer()->text(), QStringLiteral("back\n"));
    QCOMPARE(document->state(), Document::State::Clean);
}

void ParchTests::fileWatchRenewsWatchAfterRenameOverHeldFile()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = QFileInfo(root.path()).canonicalFilePath() + "/a.md";
    writeFile(file, "one\n");
    FileWatch watch;
    QStringList changed;
    connect(&watch, &FileWatch::changedOnDisk, this, [&](const QString &p) { changed << p; });
    watch.watch(file);
    // The old file stays open, so the system keeps its watch alive after it is replaced.
    const int held = ::open(QFile::encodeName(file).constData(), O_RDONLY);
    QVERIFY(held >= 0);
    const auto closeHeld = qScopeGuard([held] { ::close(held); });
    writeFile(file + ".new", "two\n");
    QCOMPARE(::rename(QFile::encodeName(file + ".new").constData(), QFile::encodeName(file).constData()), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 3000);
    QTest::qWait(150);
    changed.clear();
    writeFile(file, "three\n");
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 3000);
}

void ParchTests::typingDuringACheckKeepsTheTextInConflict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    {
        PoolBlock block;
        writeFile(path, "theirs\n");
        // The change is seen and its check queued, but cannot run yet.
        QTest::qWait(300);
        QCOMPARE(document->state(), Document::State::Clean);
        document->buffer()->replace({0, 0}, "typed ");
        block.release();
    }
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QCOMPARE(document->buffer()->text(), QStringLiteral("typed base\n"));
    QCOMPARE(readFile(path), QByteArray("theirs\n"));
}

void ParchTests::changeDuringQueuedSaveIsAConflict()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    document->buffer()->replace({0, 0}, "mine ");
    {
        PoolBlock block;
        store.save(document);
        QCOMPARE(document->state(), Document::State::Saving);
        // The file changes while the save is waiting, and its event meets a running save.
        writeFile(path, "theirs\n");
        QTest::qWait(300);
        block.release();
    }
    store.waitForSaves();
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine base\n"));
    QCOMPARE(readFile(path), QByteArray("theirs\n"));
}

void ParchTests::conflictNoticesAFileGoneAgain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "only copy\n");
    DocumentStore store(paths);
    Recovery recovery(paths);
    Autosave autosave(&store, &recovery);
    Document *document = store.open(path);
    autosave.watch(document);
    // An unmodified buffer enters conflict only because its file is gone.
    QVERIFY(QFile::remove(path));
    QTRY_VERIFY_WITH_TIMEOUT(document->fileMissing(), 3000);
    writeFile(path, "back\n");
    QTRY_VERIFY_WITH_TIMEOUT(!document->fileMissing(), 3000);
    QVERIFY(QFile::remove(path));
    QTRY_VERIFY_WITH_TIMEOUT(document->fileMissing(), 3000);
    QCOMPARE(document->lastError(), QStringLiteral("The file no longer exists"));
    QTRY_COMPARE_WITH_TIMEOUT(recovery.pending().size(), 1, Autosave::RecoveryMaxMs + 2000);
}

namespace {
QString newDocumentId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
}

void ParchTests::historyCapturesOnInterval()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), {}, [&] { return clock; });
    const QString id = newDocumentId();
    QVERIFY(!history.captureIfDue(id, "v1").isEmpty());
    clock = clock.addSecs(60);
    QVERIFY(history.captureIfDue(id, "v2").isEmpty());
    clock = clock.addSecs(5 * 60);
    QVERIFY(!history.captureIfDue(id, "v3").isEmpty());
    QCOMPARE(history.versions(id).size(), 2);
    QCOMPARE(history.versions(id).first().taken, clock);
}

void ParchTests::historyPrunesByCountAgeAndSizeButKeepsNamed()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    const AppPaths paths = AppPaths::under(root.path());
    History history(paths, HistoryLimits{3, 30, 1000}, [&] { return clock; });
    const QString id = newDocumentId();
    history.capture(id, "keep", "Draft one");
    for (int i = 0; i < 5; ++i) {
        clock = clock.addSecs(600);
        history.capture(id, QByteArray(100, 'a' + i));
    }
    QCOMPARE(history.versions(id).size(), 4);
    // The oldest unnamed versions went, and their files with them.
    QCOMPARE(QDir(paths.history() + "/" + id).entryList({"*.md"}).size(), 4);
    clock = clock.addDays(31);
    history.capture(id, "late");
    QCOMPARE(history.versions(id).size(), 2);
    QCOMPARE(history.versions(id).last().name, QStringLiteral("Draft one"));
    QCOMPARE(history.read(id, history.versions(id).last().id), QByteArray("keep"));

    // Size: the oldest unnamed versions go first until the total fits.
    History small(AppPaths::under(root.path() + "/size"), HistoryLimits{100, 30, 250}, [&] { return clock; });
    for (int i = 0; i < 4; ++i) {
        clock = clock.addSecs(600);
        small.capture(id, QByteArray(100, 'a' + i));
    }
    const auto kept = small.versions(id);
    QCOMPARE(kept.size(), 2);
    QCOMPARE(small.read(id, kept.first().id), QByteArray(100, 'd'));
    QCOMPARE(small.read(id, kept.last().id), QByteArray(100, 'c'));
}

void ParchTests::historyPausesWhenNamedVersionsFillBudget()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 150}, [&] { return clock; });
    const QString id = newDocumentId();
    history.capture(id, QByteArray(100, 'a'), "one");
    QVERIFY(!history.paused());
    history.capture(id, QByteArray(100, 'b'), "two");
    QVERIFY(history.paused());
    QVERIFY(history.capture(id, "more").isEmpty());
    // Both named versions are still there.
    QCOMPARE(history.versions(id).size(), 2);
}

void ParchTests::historyBudgetCoversAllDocuments()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 250}, [&] { return clock; });
    const QString first = newDocumentId(), second = newDocumentId();
    history.capture(first, QByteArray(100, 'a'));
    clock = clock.addSecs(600);
    history.capture(first, QByteArray(100, 'b'));
    clock = clock.addSecs(600);
    // Another document's capture pushes the total over, so the oldest version of any document goes.
    history.capture(second, QByteArray(100, 'c'));
    QCOMPARE(history.versions(first).size(), 1);
    QCOMPARE(history.read(first, history.versions(first).first().id), QByteArray(100, 'b'));
    QCOMPARE(history.versions(second).size(), 1);
}

void ParchTests::aCaptureReadsOnlyItsOwnIndex()
{
    // Three hundred documents with a version each, written as history writes them.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    for (int i = 0; i < 300; ++i) {
        const QString folder = paths.history() + u'/' + newDocumentId();
        const QString versionId = QStringLiteral("20260101T120000000-%1").arg(i, 12, 16, QLatin1Char('0'));
        writeFile(folder + u'/' + versionId + QStringLiteral(".md"), "text\n");
        QJsonObject entry{{QStringLiteral("id"), versionId},
                          {QStringLiteral("taken"), QStringLiteral("2026-01-01T12:00:00.000Z")},
                          {QStringLiteral("bytes"), 5}};
        writeFile(folder + QStringLiteral("/index.json"), QJsonDocument(QJsonArray{entry}).toJson());
    }
    const QDateTime clock(QDate(2026, 1, 2), QTime(12, 0), QTimeZone::UTC);
    History history(paths, {}, [&] { return clock; });
    QCOMPARE(history.usage().bytes, qint64(300 * 5));
    const QString id = newDocumentId();
    for (const QByteArray &text : {QByteArray("one\n"), QByteArray("two\n")}) {
        const int all = history.indexReads(), own = history.indexReads(id);
        QVERIFY(!history.captureUnlessNewest(id, text, History::Reason::Save).isEmpty());
        QVERIFY(!history.capture(id, text + text).isEmpty());
        QCOMPARE(history.indexReads() - all, history.indexReads(id) - own);
    }
    QCOMPARE(history.versions(id).size(), 4);
    QCOMPARE(history.usage().bytes, qint64(300 * 5 + 2 * (4 + 8)));
}

void ParchTests::historyTotalsFollowEveryChange()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QDateTime clock(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(paths, HistoryLimits{3, 30, 1000}, [&] { return clock; });
    // The totals kept as things change, against those of a fresh look at
    // every index, by a history whose clock is far behind, so that its own
    // start removes nothing by age.
    const auto agrees = [&] {
        const History::Usage kept = history.usage();
        const History::Usage found =
            History(paths, {}, [] { return QDateTime(QDate(2000, 1, 1), QTime(0, 0), QTimeZone::UTC); }).usage();
        return kept.bytes == found.bytes && kept.named == found.named;
    };
    const QString first = newDocumentId(), second = newDocumentId();
    QVERIFY(!history.capture(first, QByteArray(100, 'a')).isEmpty());
    clock = clock.addSecs(60);
    const QString named = history.capture(first, QByteArray(50, 'b'), QStringLiteral("Named"));
    clock = clock.addSecs(60);
    const QString later = history.capture(second, QByteArray(70, 'c'));
    QVERIFY(agrees());
    QCOMPARE(history.usage().named, qint64(50));
    // Naming, naming again and removing.
    QVERIFY(history.rename(second, later, QStringLiteral("Second")));
    QVERIFY(history.rename(second, later, QStringLiteral("Renamed")));
    QVERIFY(agrees());
    QCOMPARE(history.usage().named, qint64(120));
    QVERIFY(history.remove(first, named));
    QVERIFY(agrees());
    QCOMPARE(history.usage().named, qint64(70));
    // Pruned by count, then by age.
    for (int i = 0; i < 5; ++i) {
        clock = clock.addSecs(60);
        QVERIFY(!history.capture(first, QByteArray(10 + i, 'd')).isEmpty());
    }
    QCOMPARE(history.versions(first).size(), 3);
    QVERIFY(agrees());
    clock = clock.addDays(31);
    QVERIFY(!history.capture(second, QByteArray(20, 'e')).isEmpty());
    QVERIFY(agrees());
    // Pruned across documents by size: the oldest unnamed versions of any
    // document go, the named one stays.
    clock = clock.addSecs(60);
    QVERIFY(!history.capture(second, QByteArray(900, 'f')).isEmpty());
    QVERIFY(agrees());
    QVERIFY(history.usage().bytes <= 1000);
    QVERIFY(history.versions(first).isEmpty());
    QCOMPARE(history.usage().named, qint64(70));
}

void ParchTests::historyPrunesOldVersionsAsItStarts()
{
    // A document not captured again still loses its unnamed versions past
    // the age limit once history starts; its named ones stay.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QDateTime clock(QDate(2026, 3, 1), QTime(12, 0), QTimeZone::UTC);
    const QString id = newDocumentId();
    const QString folder = paths.history() + u'/' + id;
    QJsonArray index;
    const auto version = [&](const QDateTime &taken, const QString &name, int number) {
        const QString versionId = taken.toString(QStringLiteral("yyyyMMddTHHmmsszzz")) + QStringLiteral("-%1").arg(number, 12, 16, QLatin1Char('0'));
        writeFile(folder + u'/' + versionId + QStringLiteral(".md"), "text\n");
        QJsonObject entry{{QStringLiteral("id"), versionId},
                          {QStringLiteral("taken"), taken.toString(Qt::ISODateWithMs)},
                          {QStringLiteral("bytes"), 5}};
        if (!name.isEmpty()) {
            entry.insert(QStringLiteral("name"), name);
            writeFile(folder + u'/' + versionId + QStringLiteral(".name"), name.toUtf8());
        }
        index.append(entry);
        return versionId;
    };
    const QString recent = version(clock.addDays(-2), {}, 1);
    const QString named = version(clock.addDays(-40), QStringLiteral("Kept"), 2);
    const QString old = version(clock.addDays(-31), {}, 3);
    const QString older = version(clock.addDays(-60), {}, 4);
    writeFile(folder + QStringLiteral("/index.json"), QJsonDocument(index).toJson());
    History history(paths, {}, [&] { return clock; });
    QStringList kept;
    for (const History::Version &v : history.versions(id)) kept << v.id;
    QCOMPARE(kept, (QStringList{recent, named}));
    QVERIFY(!QFileInfo::exists(folder + u'/' + old + QStringLiteral(".md")));
    QVERIFY(!QFileInfo::exists(folder + u'/' + older + QStringLiteral(".md")));
    QCOMPARE(history.usage().bytes, qint64(10));
}

void ParchTests::removingNamedVersionsLiftsThePause()
{
    // Named versions that fill history pause it; once enough of them go,
    // removed in parch or outside it, the pause lifts without a restart.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QDateTime clock(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(paths, HistoryLimits{100, 30, 150}, [&] { return clock; });
    const QString first = newDocumentId(), second = newDocumentId();
    QVERIFY(!history.capture(first, QByteArray(100, 'a'), QStringLiteral("one")).isEmpty());
    const QString two = history.capture(second, QByteArray(100, 'b'), QStringLiteral("two"));
    QVERIFY(!two.isEmpty());
    QVERIFY(history.pause() == History::Pause::NamedVersionsFill);
    QVERIFY(history.capture(first, "more").isEmpty());
    // Removed in parch.
    QVERIFY(history.remove(second, two));
    QVERIFY(history.pause() == History::Pause::None);
    clock = clock.addSecs(60);
    QVERIFY(!history.capture(first, "more").isEmpty());
    // Paused, a capture decides from the totals kept, and reads no other
    // document's history.
    QVERIFY(!history.capture(second, QByteArray(100, 'c'), QStringLiteral("three")).isEmpty());
    QVERIFY(history.pause() == History::Pause::NamedVersionsFill);
    const QString third = newDocumentId();
    const int all = history.indexReads(), own = history.indexReads(third);
    QVERIFY(history.capture(third, "more").isEmpty());
    QCOMPARE(history.indexReads() - all, history.indexReads(third) - own);
    // Removed outside parch, from a document whose history is not read
    // again, the versions are counted until history starts again.
    QVERIFY(QDir(paths.history() + u'/' + second).removeRecursively());
    QVERIFY(history.pause() == History::Pause::NamedVersionsFill);
    History again(paths, HistoryLimits{100, 30, 150}, [&] { return clock; });
    QVERIFY(again.pause() == History::Pause::None);
}

void ParchTests::historyRejectsIdsThatAreNotUuids()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    History history(AppPaths::under(root.path()));
    for (const QString &bad : {QString("../escape"), QString("a/b"), QString(), QString("caf\u00e9")}) {
        QVERIFY(history.capture(bad, "x").isEmpty());
        QVERIFY(history.versions(bad).isEmpty());
    }
    QVERIFY(!QFileInfo::exists(root.path() + "/escape"));
    const QString id = newDocumentId();
    QVERIFY(history.read(id, "../../x").isEmpty());
}

void ParchTests::idsAndTextsAreCheckedOneWay()
{
    // Drafts, recovery and history accept the same ids, and nothing else.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = newDocumentId();
    QVERIFY(AppPaths::isDocumentId(id));
    DocumentStore store(paths);
    Recovery recovery(paths);
    History history(paths);
    for (const QString &bad : {QString(), QStringLiteral("../escape"), id.toUpper(), u'{' + id + u'}', id + u'x'}) {
        QVERIFY2(!AppPaths::isDocumentId(bad), qPrintable(bad));
        QVERIFY(!store.openDraft(bad));
        QVERIFY(!store.isUnnamedDraft(bad));
        QVERIFY(!recovery.write(bad, {}, SourceSnapshot{1, QStringLiteral("x")}, {}));
        QVERIFY(history.capture(bad, "x").isEmpty());
    }
    QVERIFY(recovery.pending().isEmpty());
    // Text that starts with U+FEFF, once or twice, comes back whole from
    // recovery and from history, as from a file.
    const QString bom(QChar(0xFEFF));
    for (const QString &text : {bom + QStringLiteral("x\n"), bom + bom + QStringLiteral("x\n")}) {
        QVERIFY(recovery.write(id, {}, SourceSnapshot{2, text}, {}));
        QCOMPARE(recovery.pending().value(0).text, text);
        const QString versionId = history.capture(id, text.toUtf8());
        QCOMPARE(DiskFile::decode(history.read(id, versionId)).text, text.mid(1));
        QCOMPARE(DiskFile::decodeUtf8(text.toUtf8()), text);
    }
}

void ParchTests::historyReadsBackAndSurvivesRestart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QDateTime clock(QDate(2026, 1, 1), QTime(12, 0, 0, 123), QTimeZone::UTC);
    const QString id = newDocumentId();
    QString versionId;
    {
        History history(paths, {}, [&] { return clock; });
        versionId = history.capture(id, "caf\xc3\xa9\n", "Named");
    }
    QVERIFY(QRegularExpression("^20260101T120000123-[0-9a-f]{12}$").match(versionId).hasMatch());
    QVERIFY(QFileInfo::exists(paths.history() + "/" + id + "/" + versionId + ".md"));
    QVERIFY(QFileInfo::exists(paths.history() + "/" + id + "/index.json"));
    History again(paths, {}, [&] { return clock; });
    const auto versions = again.versions(id);
    QCOMPARE(versions.size(), 1);
    QCOMPARE(versions.first().id, versionId);
    QCOMPARE(versions.first().name, QStringLiteral("Named"));
    QCOMPARE(versions.first().bytes, qint64(6));
    QCOMPARE(versions.first().taken, clock);
    QCOMPARE(again.read(id, versionId), QByteArray("caf\xc3\xa9\n"));
    // A lost index is rebuilt from the files, which are the versions.
    QVERIFY(QFile::remove(paths.history() + "/" + id + "/index.json"));
    QCOMPARE(again.versions(id).size(), 1);
    QCOMPARE(again.read(id, again.versions(id).first().id), QByteArray("caf\xc3\xa9\n"));
}

void ParchTests::historyFailureIsNotPause()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = newDocumentId();
    // A regular file where the document's folder belongs.
    writeFile(paths.history() + "/" + id, "in the way");
    History history(paths);
    QVERIFY(history.capture(id, "x").isEmpty());
    QVERIFY(!history.paused());
}

void ParchTests::saveCapturesHistory()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "base\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    document->buffer()->replace({0, 0}, "one ");
    store.save(document);
    store.waitForSaves();
    QCOMPARE(document->state(), Document::State::Clean);
    // The text saved, newest, and the text the file was opened with.
    auto versions = store.history()->versions(document->id());
    QCOMPARE(versions.size(), 2);
    QCOMPARE(store.history()->read(document->id(), versions.first().id), QByteArray("one base\n"));
    QCOMPARE(store.history()->read(document->id(), versions.last().id), QByteArray("base\n"));
    // A second save inside the interval adds nothing.
    document->buffer()->replace({0, 0}, "two ");
    store.save(document);
    store.waitForSaves();
    QCOMPARE(store.history()->versions(document->id()).size(), 2);

    // A draft is kept too.
    Document *draft = store.newDraft();
    draft->buffer()->replace({0, 0}, "draft text");
    store.save(draft);
    store.waitForSaves();
    QCOMPARE(store.history()->versions(draft->id()).size(), 1);
}

void ParchTests::historyKeepsTheTextAFileWasOpenedWith()
{
    // The bytes a file held when it was opened, or reloaded, become a
    // version before the first save replaces them, unless history's newest
    // version holds them already, and off the UI thread.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    const QByteArray opened("as opened\r\n");
    writeFile(path, opened);
    const auto count = [](const QStringList &texts, const QString &text) { return texts.count(text); };
    QString id;
    {
        DocumentStore store(paths);
        Document *document = store.open(path);
        id = document->id();
        QVERIFY(store.history()->versions(id).isEmpty());
        document->buffer()->replace({0, 2}, QStringLiteral("AS"));
        {
            // Nothing is recorded on the UI thread while the save waits.
            PoolBlock block;
            store.save(document);
            QCOMPARE(document->state(), Document::State::Saving);
            QVERIFY(store.history()->versions(id).isEmpty());
        }
        QTRY_COMPARE(document->state(), Document::State::Clean);
        QStringList texts = versionTexts(store.history(), id);
        QCOMPARE(count(texts, QStringLiteral("as opened\r\n")), 1);
        QCOMPARE(count(texts, QStringLiteral("AS opened\r\n")), 1);
        // A second save does not record the opened text again.
        document->buffer()->replace({0, 2}, QStringLiteral("So"));
        store.save(document);
        QTRY_COMPARE(document->state(), Document::State::Clean);
        texts = versionTexts(store.history(), id);
        QCOMPARE(count(texts, QStringLiteral("as opened\r\n")), 1);
        QCOMPARE(readFile(path), QByteArray("So opened\r\n"));
    }
    {
        // Opened again, and reloaded with the text history's newest version
        // holds: a reload counts as opening, and its first save adds no copy.
        DocumentStore store(paths);
        Document *document = store.open(path);
        const int before = int(store.history()->versions(id).size());
        QCOMPARE(QString::fromUtf8(store.history()->read(id, store.history()->versions(id).first().id)),
                 QStringLiteral("AS opened\r\n"));
        writeFile(path, "AS opened\r\n");
        QTRY_COMPARE_WITH_TIMEOUT(document->buffer()->text(), QStringLiteral("AS opened\r\n"), 3000);
        document->buffer()->replace({0, 0}, QStringLiteral("More: "));
        store.save(document);
        QTRY_COMPARE(document->state(), Document::State::Clean);
        const QStringList texts = versionTexts(store.history(), id);
        QCOMPARE(count(texts, QStringLiteral("AS opened\r\n")), 1);
        QCOMPARE(count(texts, QStringLiteral("More: AS opened\r\n")), 1);
        QCOMPARE(int(store.history()->versions(id).size()), before + 1);
    }
    {
        // An unchanged file opened and left alone adds nothing.
        DocumentStore store(paths);
        const int before = int(store.history()->versions(id).size());
        Document *document = store.open(path);
        QVERIFY(document);
        QTest::qWait(50);
        QCOMPARE(int(store.history()->versions(id).size()), before);
        QCOMPARE(readFile(path), QByteArray("More: AS opened\r\n"));
    }
}

void ParchTests::versionsSayWhyTheyWereKept()
{
    // A version history keeps by itself, for a reason other than a save,
    // says why; a save says nothing, nor does an entry written without it.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "opened\n");
    DocumentStore store(paths);
    DocumentReview review(&store);
    History *history = store.history();
    const auto reasons = [&](Document *document) {
        QSignalSpy read(&review, &DocumentReview::versionsRead);
        review.readVersions(document);
        QStringList said;
        if (!read.wait(3000)) return QStringList{QStringLiteral("<no answer>")};
        for (const QVariant &row : read.first().at(2).toList())
            said << row.toMap().value(QStringLiteral("reason")).toString();
        return said;
    };
    Document *document = store.open(path);
    document->buffer()->replace({0, 6}, QStringLiteral("edited"));
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    // Newest first: the save, then the text the file was opened with.
    QCOMPARE(reasons(document), (QStringList{QString(), QStringLiteral("as opened")}));
    QCOMPARE(history->versions(document->id()).last().reason, History::Reason::Opened);

    // Reload keeps this text first, and Overwrite the file's.
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    writeFile(path, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Reload));
    QCOMPARE(history->versions(document->id()).first().reason, History::Reason::Reload);
    QCOMPARE(reasons(document).first(), QStringLiteral("before reload"));
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    writeFile(path, "theirs again\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(store.resolveConflict(document, ConflictChoice::Overwrite));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    bool overwritten = false;
    for (const History::Version &version : history->versions(document->id()))
        overwritten = overwritten
                      || (version.reason == History::Reason::Overwrite
                          && history->read(document->id(), version.id) == QByteArray("theirs again\n"));
    QVERIFY(overwritten);
    QVERIFY(reasons(document).contains(QStringLiteral("before overwrite")));
    // Restore keeps the text it replaces.
    QString older;
    for (const History::Version &version : history->versions(document->id()))
        if (history->read(document->id(), version.id) == QByteArray("opened\n")) older = version.id;
    QVERIFY(!older.isEmpty());
    document->buffer()->replace({0, 0}, QStringLiteral("unsaved "));
    QCOMPARE(store.restoreVersion(document, older), DocumentStore::Restore::Restored);
    QCOMPARE(history->versions(document->id()).first().reason, History::Reason::Restore);
    QCOMPARE(reasons(document).first(), QStringLiteral("before restore"));

    // An index written before reasons were kept reads every entry as a save.
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    History plain(paths);
    const QString version = plain.capture(id, "kept");
    QVERIFY(!version.isEmpty());
    const QString index = paths.history() + u'/' + id + QStringLiteral("/index.json");
    QJsonArray entries = QJsonDocument::fromJson(readFile(index)).array();
    QJsonObject entry = entries.first().toObject();
    entry.remove(QStringLiteral("reason"));
    entries[0] = entry;
    writeFile(index, QJsonDocument(entries).toJson());
    QCOMPARE(plain.versions(id).first().reason, History::Reason::Save);
}

void ParchTests::restoreAfterASaveKeepsNoSecondCopy()
{
    // Restoring right after a save: history's newest version holds the text
    // being replaced already, so no copy of it is added; the restore goes on
    // and undo takes it back.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "one\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    document->buffer()->replace({0, 3}, QStringLiteral("two"));
    store.save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    History *history = store.history();
    QCOMPARE(history->versions(document->id()).size(), 2);
    QString opened;
    for (const History::Version &version : history->versions(document->id()))
        if (history->read(document->id(), version.id) == QByteArray("one\n")) opened = version.id;
    QVERIFY(!opened.isEmpty());
    QCOMPARE(store.restoreVersion(document, opened), DocumentStore::Restore::Restored);
    QCOMPARE(document->buffer()->text(), QStringLiteral("one\n"));
    QCOMPARE(history->versions(document->id()).size(), 2);
    document->buffer()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("two\n"));
}

void ParchTests::restoringTheSameBytesChangesNothing()
{
    // A second BOM is text, in a version as in the file, so restoring the
    // bytes the file holds changes nothing, and its preview is the text.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath("a.md");
    const QByteArray doubled("\xEF\xBB\xBF\xEF\xBB\xBFx\n");
    writeFile(file, doubled);
    DocumentStore store(paths);
    Document *document = store.open(file);
    QVERIFY(document);
    QCOMPARE(document->buffer()->text(), QString(QChar(0xFEFF)) + QStringLiteral("x\n"));
    const QString versionId = store.history()->capture(document->id(), doubled);
    QVERIFY(!versionId.isEmpty());
    QSignalSpy saved(&store, &DocumentStore::documentSaved);
    QCOMPARE(store.restoreVersion(document, versionId), DocumentStore::Restore::Unchanged);
    QVERIFY(!document->buffer()->modified());
    QCOMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->bytes(), doubled);
    store.waitForSaves();
    QCOMPARE(saved.count(), 0);
    QCOMPARE(readFile(file), doubled);
    DocumentReview review(&store);
    QSignalSpy read(&review, &DocumentReview::versionRead);
    review.readVersion(document, versionId);
    QTRY_COMPARE(read.count(), 1);
    QCOMPARE(read.first().at(3).toString(), document->buffer()->text());
    QCOMPARE(read.first().at(4).toString(), QString());
}

void ParchTests::restoreIsUndoable()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "older\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    const QString olderId = store.history()->capture(document->id(), "older\n");
    QVERIFY(!olderId.isEmpty());
    document->buffer()->replace({0, 5}, "newer");
    QCOMPARE(document->buffer()->text(), QStringLiteral("newer\n"));
    store.restoreVersion(document, olderId);
    QCOMPARE(document->buffer()->text(), QStringLiteral("older\n"));
    // The file already holds this text, but the edit is a step beyond the saved one, so a save follows.
    QCOMPARE(document->state(), Document::State::Modified);
    document->buffer()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("newer\n"));
    QCOMPARE(document->state(), Document::State::Modified);
    document->buffer()->redo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("older\n"));
    // The text that was replaced is in history.
    bool found = false;
    for (const auto &version : store.history()->versions(document->id()))
        found = found || store.history()->read(document->id(), version.id) == QByteArray("newer\n");
    QVERIFY(found);
    // An unknown version changes nothing.
    store.restoreVersion(document, "20200101T000000000-000000000000");
    QCOMPARE(document->buffer()->text(), QStringLiteral("older\n"));
}

void ParchTests::restoreRefusedWhenHistoryCannotWrite()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "older\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    const QString olderId = store.history()->capture(document->id(), "older\n");
    QVERIFY(!olderId.isEmpty());
    document->buffer()->replace({0, 5}, "newer");
    // The version stays readable, but its index becomes a folder, so no new
    // version can be recorded: the current text cannot be kept, and the
    // restore must not replace it.
    const QString index = paths.history() + "/" + document->id() + "/index.json";
    QVERIFY(QFile::remove(index));
    QVERIFY(QDir().mkpath(index));
    QCOMPARE(store.history()->read(document->id(), olderId), QByteArray("older\n"));
    store.restoreVersion(document, olderId);
    QVERIFY(!store.history()->paused());
    QCOMPARE(document->buffer()->text(), QStringLiteral("newer\n"));
    QCOMPARE(document->state(), Document::State::Modified);
    QVERIFY(!document->lastError().isEmpty());
}

void ParchTests::restoreRefusalIsReported()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    DocumentStore store(paths);
    // Each refusal is checked on a document of its own, so that one reason
    // cannot stand in for another.
    auto openNew = [&](const QString &name, const QByteArray &contents) {
        writeFile(root.filePath(name), contents);
        Document *document = store.open(root.filePath(name));
        return document;
    };
    Document *unknown = openNew("a.md", "text\n");
    QVERIFY(unknown->lastError().isEmpty());
    store.restoreVersion(unknown, "20200101T000000000-000000000000");
    QVERIFY(!unknown->lastError().isEmpty());
    QCOMPARE(unknown->buffer()->text(), QStringLiteral("text\n"));

    // Text that is not valid UTF-8 cannot become the buffer.
    Document *invalid = openNew("b.md", "text\n");
    const QString bad = store.history()->capture(invalid->id(), "bad \xff bytes");
    QVERIFY(!bad.isEmpty());
    QVERIFY(invalid->lastError().isEmpty());
    store.restoreVersion(invalid, bad);
    QCOMPARE(invalid->buffer()->text(), QStringLiteral("text\n"));
    QVERIFY(!invalid->lastError().isEmpty());

    // A read-only document is never replaced.
    Document *readOnly = openNew("c.md", "bad \xff bytes\n");
    QCOMPARE(readOnly->state(), Document::State::ReadOnly);
    const QString version = store.history()->capture(readOnly->id(), "fine");
    QVERIFY(!version.isEmpty());
    const QString before = readOnly->buffer()->text();
    QVERIFY(readOnly->lastError().isEmpty());
    store.restoreVersion(readOnly, version);
    QCOMPARE(readOnly->buffer()->text(), before);
    QVERIFY(!readOnly->lastError().isEmpty());
}

void ParchTests::restoreProceedsWhenHistoryIsPaused()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "older\n");
    DocumentStore store(paths, HistoryLimits{100, 30, 5});
    Document *document = store.open(path);
    const QString olderId = store.history()->capture(document->id(), "older\n", "Kept");
    QVERIFY(!olderId.isEmpty());
    QVERIFY(store.history()->paused());
    document->buffer()->replace({0, 5}, "newer");
    store.restoreVersion(document, olderId);
    QCOMPARE(document->buffer()->text(), QStringLiteral("older\n"));
}

void ParchTests::captureThatCannotBeKeptPauses()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 150}, [&] { return clock; });
    const QString id = newDocumentId();
    QVERIFY(!history.capture(id, QByteArray(100, 'a'), "named").isEmpty());
    QVERIFY(!history.paused());
    // Pruning every unnamed version still leaves no room for this one, so it
    // is not reported as recorded and the pause is visible.
    QVERIFY(history.capture(id, QByteArray(100, 'b')).isEmpty());
    QVERIFY(history.paused());
    QCOMPARE(history.versions(id).size(), 1);

    // The same through a restore: it proceeds under the visible pause.
    const AppPaths paths = AppPaths::under(root.path() + "/store");
    const QString path = root.filePath("a.md");
    writeFile(path, "x\n");
    DocumentStore store(paths, HistoryLimits{100, 30, 150});
    Document *document = store.open(path);
    const QString kept = store.history()->capture(document->id(), QByteArray(100, 'a'), "named");
    document->buffer()->replace({0, 1}, QString(60, u'n'));
    store.restoreVersion(document, kept);
    QCOMPARE(document->buffer()->text(), QString(100, u'a'));
    QVERIFY(store.history()->paused());
}

void ParchTests::rebuiltIndexReadsTimesAsUtc()
{
    const QByteArray oldZone = qgetenv("TZ");
    qputenv("TZ", "America/New_York");
    tzset();
    auto restore = qScopeGuard([&] {
        if (oldZone.isNull()) qunsetenv("TZ"); else qputenv("TZ", oldZone);
        tzset();
    });
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QDateTime clock(QDate(2026, 1, 1), QTime(12, 10), QTimeZone::UTC);
    History history(paths, {}, [&] { return clock; });
    const QString id = newDocumentId();
    QVERIFY(!history.capture(id, "one").isEmpty());
    QVERIFY(QFile::remove(paths.history() + "/" + id + "/index.json"));
    QCOMPARE(history.versions(id).first().taken.toUTC(), clock);
    // Not due yet, however the zone reads the digits.
    clock = clock.addSecs(60);
    QVERIFY(history.captureIfDue(id, "two").isEmpty());
}

void ParchTests::versionTimesInALocalGapStayExact()
{
    // A version id's digits are UTC. In a zone whose clocks skip the hour
    // they name, here the hour Berlin skips on the last Sunday of March,
    // they still give that UTC time, to the millisecond.
    const QByteArray oldZone = qgetenv("TZ");
    qputenv("TZ", "Europe/Berlin");
    tzset();
    auto restore = qScopeGuard([&] {
        if (oldZone.isNull()) qunsetenv("TZ");
        else qputenv("TZ", oldZone);
        tzset();
    });
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = newDocumentId();
    const QString folder = paths.history() + u'/' + id;
    for (const QString &versionId : {QStringLiteral("20260329T023000250-0123456789ab"),
                                     QStringLiteral("20261025T023000250-0123456789ac")})
        writeFile(folder + u'/' + versionId + QStringLiteral(".md"), "kept\n");
    // Kept however old they are, whenever this runs.
    History history(paths, HistoryLimits{100, 365000, qint64(1) << 30});
    const QList<History::Version> versions = history.versions(id);
    QCOMPARE(versions.size(), 2);
    // Newest first: the October one, in the hour Berlin goes through twice.
    QCOMPARE(versions.at(0).taken, QDateTime(QDate(2026, 10, 25), QTime(2, 30, 0, 250), QTimeZone::UTC));
    QCOMPARE(versions.at(1).taken, QDateTime(QDate(2026, 3, 29), QTime(2, 30, 0, 250), QTimeZone::UTC));
}

void ParchTests::namesSurviveALostIndex()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QDateTime clock(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(paths, HistoryLimits{2, 30, 1000000}, [&] { return clock; });
    const QString id = newDocumentId();
    QVERIFY(!history.capture(id, "keep", "Checkpoint").isEmpty());
    QVERIFY(QFile::remove(paths.history() + "/" + id + "/index.json"));
    for (int i = 0; i < 4; ++i) {
        clock = clock.addSecs(600);
        QVERIFY(!history.capture(id, QByteArray(1, 'a' + i)).isEmpty());
    }
    const auto versions = history.versions(id);
    QCOMPARE(versions.size(), 3);
    QCOMPARE(versions.last().name, QStringLiteral("Checkpoint"));
    QCOMPARE(history.read(id, versions.last().id), QByteArray("keep"));
}

void ParchTests::unindexedVersionIsAdopted()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QDateTime clock(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(paths, HistoryLimits{2, 30, 1000000}, [&] { return clock; });
    const QString id = newDocumentId();
    QVERIFY(!history.capture(id, "indexed").isEmpty());
    // A crash between the version file and the index leaves a file alone.
    writeFile(paths.history() + "/" + id + "/20260101T115000000-0123456789ab.md", "orphan");
    const auto versions = history.versions(id);
    QCOMPARE(versions.size(), 2);
    QCOMPARE(history.read(id, "20260101T115000000-0123456789ab"), QByteArray("orphan"));
    // It is counted and pruned like any other.
    for (int i = 0; i < 2; ++i) {
        clock = clock.addSecs(600);
        history.capture(id, QByteArray(1, 'a' + i));
    }
    QCOMPARE(history.versions(id).size(), 2);
    QVERIFY(!QFileInfo::exists(paths.history() + "/" + id + "/20260101T115000000-0123456789ab.md"));
}

void ParchTests::collisionsNeverRenameOrLoseVersions()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QDateTime clock(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(paths, {}, [&] { return clock; });
    const QString id = newDocumentId();
    const QString first = history.capture(id, "same", "First checkpoint");
    const QString second = history.capture(id, "same", "Second checkpoint");
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    QVERIFY(first != second);
    QStringList names;
    for (const auto &version : history.versions(id)) names << version.name;
    QVERIFY(names.contains("First checkpoint"));
    QVERIFY(names.contains("Second checkpoint"));

    // Different bytes under an id that is already taken get an id of their own.
    const QString other = newDocumentId();
    const QString plain = history.capture(other, "alpha");
    QVERIFY(!plain.isEmpty());
    writeFile(paths.history() + "/" + other + "/" + plain + ".md", "different");
    const QString again = history.capture(other, "alpha");
    QVERIFY(!again.isEmpty());
    QVERIFY(again != plain);
    QCOMPARE(history.read(other, plain), QByteArray("different"));
    QCOMPARE(history.read(other, again), QByteArray("alpha"));
}

void ParchTests::emptyVersionCanBeRestored()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString path = root.filePath("a.md");
    writeFile(path, "text\n");
    DocumentStore store(paths);
    Document *document = store.open(path);
    const QString empty = store.history()->capture(document->id(), QByteArray());
    QVERIFY(!empty.isEmpty());
    store.restoreVersion(document, empty);
    QCOMPARE(document->buffer()->text(), QString());
    document->buffer()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("text\n"));
}

void ParchTests::unfitCaptureDeletesNothing()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 150}, [&] { return clock; });
    const QString a = newDocumentId(), b = newDocumentId();
    QVERIFY(!history.capture(a, QByteArray(100, 'a'), "named").isEmpty());
    clock = clock.addSecs(600);
    const QString kept = history.capture(b, QByteArray(40, 'b'));
    QVERIFY(!kept.isEmpty());
    clock = clock.addSecs(600);
    // The text cannot fit beside the named version, so it is refused before
    // anything is written, and the versions that did fit stay.
    QVERIFY(history.capture(a, QByteArray(100, 'c')).isEmpty());
    QVERIFY(history.paused());
    QCOMPARE(history.versions(b).size(), 1);
    QCOMPARE(history.read(b, kept), QByteArray(40, 'b'));
    QCOMPARE(history.versions(a).size(), 1);
}

void ParchTests::captureReportsPauseAndFailureApart()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    History history(paths, HistoryLimits{100, 30, 150});
    const QString id = newDocumentId();
    History::CaptureStatus status = History::CaptureStatus::Failed;
    QVERIFY(!history.capture(id, "x", {}, &status).isEmpty());
    QCOMPARE(status, History::CaptureStatus::Recorded);
    QVERIFY(history.capture(id, QByteArray(200, 'y'), {}, &status).isEmpty());
    QCOMPARE(status, History::CaptureStatus::Paused);
    QVERIFY(history.capture("not an id", "x", {}, &status).isEmpty());
    QCOMPARE(status, History::CaptureStatus::Failed);
    // An index that cannot be written is a failure, never a pause.
    const QString other = newDocumentId();
    QVERIFY(QDir().mkpath(paths.history() + "/" + other + "/index.json"));
    QVERIFY(history.capture(other, "x", {}, &status).isEmpty());
    QCOMPARE(status, History::CaptureStatus::Failed);
    QVERIFY(!history.paused());
}

void ParchTests::failedNameWriteFailsTheCapture()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QDateTime clock(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(paths, {}, [&] { return clock; });
    const QString id = newDocumentId();
    const QString versionId = QStringLiteral("20260101T120000000-")
        + QString::fromLatin1(QCryptographicHash::hash("x", QCryptographicHash::Sha256).toHex().left(12));
    // A folder where the name belongs, so the name cannot be written.
    QVERIFY(QDir().mkpath(paths.history() + "/" + id + "/" + versionId + ".name"));
    QVERIFY(history.capture(id, "x", "Checkpoint").isEmpty());
    QVERIFY(!history.paused());
    QVERIFY(!QFileInfo::exists(paths.history() + "/" + id + "/" + versionId + ".md"));
    QVERIFY(history.versions(id).isEmpty());
}

void ParchTests::namedBytesOverBudgetLeaveUnnamedVersionsAlone()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 150}, [&] { return clock; });
    const QString a = newDocumentId(), b = newDocumentId(), c = newDocumentId();
    QVERIFY(!history.capture(a, QByteArray(100, 'a'), "first").isEmpty());
    clock = clock.addSecs(600);
    QVERIFY(!history.capture(b, QByteArray(20, 'b')).isEmpty());
    QVERIFY(!history.capture(c, QByteArray(20, 'c')).isEmpty());
    clock = clock.addSecs(600);
    // Named bytes alone now exceed the budget, so removing every unnamed
    // version could not restore it: they stay, and history pauses.
    QVERIFY(!history.capture(a, QByteArray(100, 'd'), "second").isEmpty());
    QVERIFY(history.paused());
    QCOMPARE(history.versions(a).size(), 2);
    QCOMPARE(history.versions(b).size(), 1);
    QCOMPARE(history.versions(c).size(), 1);
}

void ParchTests::sizePruningStopsAtTheOldestThatMakesRoom()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QDateTime clock = QDateTime(QDate(2026, 1, 1), QTime(12, 0), QTimeZone::UTC);
    History history(AppPaths::under(root.path()), HistoryLimits{100, 30, 150}, [&] { return clock; });
    const QString a = newDocumentId(), b = newDocumentId(), c = newDocumentId();
    QVERIFY(!history.capture(a, QByteArray(100, 'a'), "first").isEmpty());
    clock = clock.addSecs(600);
    QVERIFY(!history.capture(b, QByteArray(20, 'b')).isEmpty());
    clock = clock.addSecs(600);
    QVERIFY(!history.capture(c, QByteArray(20, 'c')).isEmpty());
    clock = clock.addSecs(600);
    QVERIFY(!history.capture(a, QByteArray(30, 'd'), "second").isEmpty());
    QVERIFY(!history.paused());
    QCOMPARE(history.versions(a).size(), 2);
    QCOMPARE(history.versions(b).size(), 0);
    QCOMPARE(history.versions(c).size(), 1);
}

namespace {

bool isProjectedBreak(QChar c)
{
    // QTextCursor::insertText turns each of these into one paragraph separator.
    return c == u'\r' || c == u'\n' || c == QChar(0x2029) || c == QChar(0xfdd0) || c == QChar(0xfdd1);
}

// What Qt shows for a source, worked out directly: CRLF becomes one paragraph
// separator, every other line break one separator, and U+2028 stays itself.
QString projectionOf(const QString &source)
{
    QString projection;
    for (qsizetype i = 0; i < source.size(); ++i) {
        const QChar c = source.at(i);
        if (c == u'\r' && i + 1 < source.size() && source.at(i + 1) == u'\n') ++i;
        projection += isProjectedBreak(c) ? QChar(0x2029) : c;
    }
    return projection;
}

// Where each source position falls in the projection; both sides of the LF
// in a CRLF pair fall on the pair's separator, as the position between them does.
QList<int> projectedPositions(const QString &source)
{
    QList<int> positions;
    int editor = 0;
    for (qsizetype i = 0; i < source.size(); ++i) {
        positions.append(editor);
        if (source.at(i) == u'\r' && i + 1 < source.size() && source.at(i + 1) == u'\n') {
            positions.append(editor);
            ++i;
        }
        ++editor;
    }
    positions.append(editor);
    return positions;
}

// A theme on a temporary folder, a store on temporary paths and an engine
// that has the theme, which is everything an EditorPane needs.
struct EditorFixture {
    QTemporaryDir root;
    QTemporaryDir themeFolder;
    std::unique_ptr<Theme> theme;
    std::unique_ptr<DocumentStore> store;
    QQmlEngine engine;

    EditorFixture()
    {
        writeTheme(themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
        theme = std::make_unique<Theme>(themeFolder.path());
        store = std::make_unique<DocumentStore>(AppPaths::under(root.path()));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), theme.get());
    }

    Document *open(const QString &name, const QString &text)
    {
        writeFile(root.filePath(name), text.toUtf8());
        return store->open(root.filePath(name));
    }
};

// One view of a document: an EditorPane in its own offscreen window, as a tab
// or a second window shows it.
struct EditorView {
    QQuickWindow window;
    QQuickItem *pane = nullptr;
    QQuickItem *flickable = nullptr;
    QQuickItem *editor = nullptr;
    EditorBinding *binding = nullptr;
    QString error;

    EditorView(QQmlEngine &engine, Document *document, int width = 400, int height = 180)
    {
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/EditorPane.qml")));
        pane = qobject_cast<QQuickItem *>(component.create());
        if (!pane) {
            error = component.errorString();
            return;
        }
        pane->setParent(&window);
        pane->setParentItem(window.contentItem());
        pane->setSize(QSizeF(width, height));
        window.resize(width, height);
        pane->setProperty("document", QVariant::fromValue(document));
        flickable = pane->findChild<QQuickItem *>(QStringLiteral("flickable"));
        editor = pane->findChild<QQuickItem *>(QStringLiteral("editor"));
        binding = pane->findChild<EditorBinding *>();
        window.show();
        if (editor) editor->forceActiveFocus();
        QCoreApplication::processEvents();
    }

    bool ok() const { return pane && flickable && editor && binding; }
    QTextDocument *textDocument() const
    {
        return editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    }
    void cursor(int position) { editor->setProperty("cursorPosition", position); }
    void select(int start, int end) { QMetaObject::invokeMethod(editor, "select", Q_ARG(int, start), Q_ARG(int, end)); }
    // A key that types this text, as a keyboard layout or a compose sequence delivers it.
    void type(const QString &text)
    {
        QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, text);
        QCoreApplication::sendEvent(editor, &event);
    }
    void ime(const QString &preedit, const QString &commit = {}, int start = 0, int length = 0)
    {
        QInputMethodEvent event(preedit, {});
        event.setCommitString(commit, start, length);
        QCoreApplication::sendEvent(editor, &event);
    }
    int selectionStart() const { return editor->property("selectionStart").toInt(); }
    int selectionEnd() const { return editor->property("selectionEnd").toInt(); }
    qreal contentY() const { return flickable->property("contentY").toReal(); }
};

// The view shows exactly the projection of the source and maps every
// position in both directions as projectedPositions() does.
void verifyProjection(const EditorView &view, const QString &source)
{
    QCOMPARE(view.textDocument()->toRawText(), projectionOf(source));
    const QList<int> positions = projectedPositions(source);
    for (int i = 0; i < positions.size(); ++i)
        QVERIFY2(view.binding->sourceToEditor(i) == positions.at(i), qPrintable(QStringLiteral("source %1").arg(i)));
    for (int e = 0; e <= positions.last(); ++e)
        QVERIFY2(view.binding->editorToSource(e) == positions.indexOf(e), qPrintable(QStringLiteral("editor %1").arg(e)));
    QCOMPARE(view.binding->sourceToEditor(-1), -1);
    QCOMPARE(view.binding->sourceToEditor(int(source.size()) + 1), -1);
    QCOMPARE(view.binding->editorToSource(-1), -1);
    QCOMPARE(view.binding->editorToSource(positions.last() + 1), -1);
}

// The line holding a source position is the first line whose text is in
// view: its text reaches below the top of the viewport and the text of the
// line before it does not.
void verifyTopLine(const EditorView &view, int position)
{
    const auto lineAt = [&](int shown) {
        QRectF line;
        QMetaObject::invokeMethod(view.editor, "positionToRectangle", Q_RETURN_ARG(QRectF, line), Q_ARG(int, shown));
        return line.translated(0, view.editor->y());
    };
    const int shown = view.binding->sourceToEditor(position);
    const QRectF line = lineAt(shown);
    QVERIFY2(line.bottom() > view.contentY() && line.top() < view.contentY() + view.flickable->height(),
             qPrintable(QStringLiteral("line at %1, viewport at %2").arg(line.top()).arg(view.contentY())));
    // After a re-wrap the position can sit inside its line; the line before starts before that line does.
    const QTextBlock block = view.textDocument()->findBlock(shown);
    const int lineStart = block.position() + block.layout()->lineForTextPosition(shown - block.position()).textStart();
    if (lineStart > 0) {
        const QRectF before = lineAt(lineStart - 1);
        QVERIFY2(before.top() < line.top() && before.bottom() <= view.contentY() + 0.5,
                 qPrintable(QStringLiteral("line before ends at %1, viewport at %2").arg(before.bottom()).arg(view.contentY())));
    }
}

// Every way of editing, none of which may change a document that cannot be edited.
void tryEveryEdit(EditorView &view)
{
    view.cursor(2);
    sendKey(&view.window, 'x');
    view.type(QStringLiteral("typed"));
    sendKey(&view.window, Qt::Key_Return);
    sendKey(&view.window, Qt::Key_Tab);
    sendKey(&view.window, Qt::Key_Backspace);
    sendKey(&view.window, Qt::Key_Delete);
    sendKey(&view.window, Qt::Key_Backspace, Qt::ControlModifier);
    QGuiApplication::clipboard()->setText(QStringLiteral("pasted"));
    sendKey(&view.window, Qt::Key_V, Qt::ControlModifier);
    view.select(0, 2);
    sendKey(&view.window, Qt::Key_X, Qt::ControlModifier);
    view.ime({}, QStringLiteral("committed"));
    view.ime(QStringLiteral("仮"));
    view.ime({}, QStringLiteral("確"));
    QMetaObject::invokeMethod(view.editor, "insert", Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("inserted")));
    QMetaObject::invokeMethod(view.editor, "remove", Q_ARG(int, 0), Q_ARG(int, 1));
    QMetaObject::invokeMethod(view.editor, "paste");
    QMetaObject::invokeMethod(view.editor, "cut");
    QMetaObject::invokeMethod(view.editor, "clear");
    sendKey(&view.window, Qt::Key_Z, Qt::ControlModifier);
    sendKey(&view.window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    view.binding->undo();
    view.binding->redo();
    QCoreApplication::processEvents();
}

QString largeText(int units)
{
    QString text;
    const QString sentence = QStringLiteral("The quick brown fox jumps over the lazy dog. ");
    while (text.size() < units) text += sentence.repeated(8) + u'\n';
    return text.left(units);
}

} // namespace

void ParchTests::nativeInputRoundTripsSource()
{
    // Qt's own normalization must never rewrite untouched source or what was typed.
    EditorFixture fixture;
    const QString original = QStringLiteral("a\u00a0b\nc\u2028d\u2029e\r\nf 😀e\u0301 العربية עברית");
    Document *document = fixture.open("round.md", original);
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    SourceBuffer *buffer = document->buffer();
    view.cursor(1);
    QTest::keyClick(&view.window, Qt::Key_X);
    QCOMPARE(buffer->text(), QStringLiteral("ax\u00a0b\nc\u2028d\u2029e\r\nf 😀e\u0301 العربية עברית"));
    buffer->undo();
    QCOMPARE(buffer->text(), original);
    QCOMPARE(view.binding->sourceToEditor(10), 9); // CRLF contracts in the projection.
    QCOMPARE(view.binding->sourceToEditor(11), 10);
    QCOMPARE(view.binding->editorToSource(10), 11);
    view.cursor(0);
    const QString inserted = QStringLiteral("😀e\u0301\u00a0\u2028\u2029\r\nעברית");
    QGuiApplication::clipboard()->setText(inserted);
    QTest::keyClick(&view.window, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(buffer->text(), inserted + original);
    QTest::keyClick(&view.window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(buffer->text(), original);
    view.cursor(0);
    QTest::keyClick(&view.window, Qt::Key_Return);
    view.type(QStringLiteral("\u2029"));
    // The file has as many CRLF breaks as LF breaks, so Enter writes CRLF.
    const QString newline = document->newline();
    QCOMPARE(newline, QStringLiteral("\r\n"));
    QCOMPARE(buffer->text(), newline + QStringLiteral("\u2029") + original);
    buffer->undo();
    QCOMPARE(buffer->text(), newline + original);
    buffer->undo();
    QCOMPARE(buffer->text(), original);
    buffer->redo();
    buffer->redo();
    QCOMPARE(buffer->text(), newline + QStringLiteral("\u2029") + original);
    verifyProjection(view, buffer->text());

    Document *composition = fixture.open("compose.md", QStringLiteral("A😀e\u0301Z"));
    EditorView imeView(fixture.engine, composition);
    QVERIFY2(imeView.ok(), qPrintable(imeView.error));
    SourceBuffer *composed = composition->buffer();
    imeView.select(1, 5);
    imeView.ime(QStringLiteral("仮"));
    QCOMPARE(composed->text(), QStringLiteral("A😀e\u0301Z"));
    QVERIFY(imeView.editor->property("inputMethodComposing").toBool());
    imeView.ime({});
    QCOMPARE(composed->text(), QStringLiteral("A😀e\u0301Z"));
    QCOMPARE(composed->revision(), Revision(0));
    imeView.select(1, 5);
    imeView.ime(QStringLiteral("仮"));
    imeView.ime({}, QStringLiteral("確😀\u00a0\u2029"));
    QCOMPARE(composed->text(), QStringLiteral("A確😀\u00a0\u2029Z"));
    composed->undo();
    QCOMPARE(composed->text(), QStringLiteral("A😀e\u0301Z"));
    imeView.cursor(5);
    imeView.ime({}, QStringLiteral("é"), -2, 2);
    QCOMPARE(composed->text(), QStringLiteral("A😀éZ"));
    QCOMPARE(imeView.editor->property("cursorPosition").toInt(), 4);
}

void ParchTests::duplicateViewsKeepIndependentPositions()
{
    // Two views share a document's text, never its wrap width, caret or scroll.
    EditorFixture fixture;
    Document *document = fixture.open("shared.md", QStringLiteral("one two three four five six seven eight nine ten\n").repeated(60));
    QVERIFY(document);
    EditorView narrow(fixture.engine, document, 180);
    EditorView wide(fixture.engine, document, 600);
    QVERIFY2(narrow.ok() && wide.ok(), qPrintable(narrow.error + wide.error));
    SourceBuffer *buffer = document->buffer();
    QVERIFY(narrow.textDocument() != wide.textDocument());
    QVERIFY(narrow.textDocument()->textWidth() < wide.textDocument()->textWidth());
    narrow.cursor(0);
    wide.select(80, 85);
    narrow.flickable->setProperty("contentY", 24);
    wide.flickable->setProperty("contentY", 120);
    narrow.type("X");
    QVERIFY(buffer->text().startsWith("Xone"));
    QVERIFY(wide.textDocument()->toRawText().startsWith("Xone"));
    QCOMPARE(wide.selectionStart(), 81);
    QCOMPARE(wide.selectionEnd(), 86);
    QCOMPARE(wide.flickable->property("contentY").toInt(), 120);
    sendKey(&wide.window, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(buffer->text().startsWith("one"));
    QVERIFY(narrow.textDocument()->toRawText().startsWith("one"));
    QCOMPARE(wide.flickable->property("contentY").toInt(), 120);
    QVERIFY(narrow.textDocument()->textWidth() < wide.textDocument()->textWidth());
    // Each view holds the whole document.
    QCOMPARE(narrow.textDocument()->characterCount() - 1, buffer->length());
    QCOMPARE(wide.textDocument()->characterCount() - 1, buffer->length());
}

void ParchTests::plainKeysTypeText()
{
    // Unmodified keys type, including ones a careless shortcut could take.
    EditorFixture fixture;
    Document *document = fixture.store->newDraft();
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    for (const QChar key : QStringLiteral("q ?")) sendKey(&view.window, key);
    QCOMPARE(document->buffer()->text(), QStringLiteral("q ?"));
    // Keys are typing, so they undo as one step.
    view.binding->undo();
    QCOMPARE(document->buffer()->text(), QString());
}

void ParchTests::enterUsesDocumentNewline()
{
    EditorFixture fixture;
    Document *document = fixture.open("crlf.md", QStringLiteral("a\r\nb\r\n"));
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.cursor(0);
    sendKey(&view.window, Qt::Key_Return);
    QVERIFY(document->buffer()->text().startsWith(QStringLiteral("\r\n")));
    QVERIFY(!document->buffer()->text().contains(QRegularExpression("(?<!\r)\n")));
    // Shift+Enter is a line break too, never an invisible U+2028.
    sendKey(&view.window, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(document->buffer()->text(), QStringLiteral("\r\n\r\na\r\nb\r\n"));
    verifyProjection(view, document->buffer()->text());

    Document *lfDocument = fixture.open("lf.md", QStringLiteral("a\nb\n"));
    EditorView lfView(fixture.engine, lfDocument);
    QVERIFY2(lfView.ok(), qPrintable(lfView.error));
    lfView.cursor(1);
    sendKey(&lfView.window, Qt::Key_Enter, Qt::KeypadModifier);
    QCOMPARE(lfDocument->buffer()->text(), QStringLiteral("a\n\nb\n"));
}

void ParchTests::insertedBreaksFollowTheDocument()
{
    // Text from outside, with breaks of every kind, takes the document's own:
    // pasted, inserted or appended by the TextEdit's methods, or committed by
    // an input method.
    EditorFixture fixture;
    const QString pasted = QStringLiteral("x\ny\r\nz\rw");
    for (const QString &newline : {QStringLiteral("\r\n"), QStringLiteral("\n")}) {
        const QString name = newline.size() == 2 ? QStringLiteral("crlf.md") : QStringLiteral("lf.md");
        Document *document = fixture.open(name, QStringLiteral("a") + newline + QStringLiteral("b") + newline);
        QVERIFY(document);
        QCOMPARE(document->newline(), newline);
        EditorView view(fixture.engine, document);
        QVERIFY2(view.ok(), qPrintable(view.error));
        const QString converted = QStringList{"x", "y", "z", "w"}.join(newline);
        view.cursor(0);
        QGuiApplication::clipboard()->setText(pasted);
        sendKey(&view.window, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(document->buffer()->text(), converted + QStringLiteral("a") + newline + QStringLiteral("b") + newline);
        QMetaObject::invokeMethod(view.editor, "insert", Q_ARG(int, 0), Q_ARG(QString, pasted));
        QMetaObject::invokeMethod(view.editor, "append", Q_ARG(QString, pasted));
        view.cursor(0);
        view.ime({}, pasted);
        const QString text = document->buffer()->text();
        QCOMPARE(text.count(converted), 4);
        verifyProjection(view, text);
        fixture.store->save(document);
        fixture.store->waitForSaves();
        const QByteArray saved = readFile(fixture.root.filePath(name));
        QCOMPARE(saved, text.toUtf8());
        if (newline.size() == 2) {
            QCOMPARE(saved.count('\r'), saved.count('\n'));
            QCOMPARE(saved.count("\r\n"), saved.count('\n'));
        } else {
            QVERIFY(!saved.contains('\r'));
        }
    }
}

void ParchTests::remoteEditDuringCompositionCancelsOnlyPreedit()
{
    EditorFixture fixture;
    const QString before = QStringLiteral("shared text\n");
    Document *document = fixture.open("compose.md", before);
    QVERIFY(document);
    EditorView first(fixture.engine, document);
    EditorView second(fixture.engine, document, 600);
    QVERIFY2(first.ok() && second.ok(), qPrintable(first.error + second.error));
    QQuickItem *firstEditor = first.editor;
    EditorBinding *secondBinding = second.binding;
    first.cursor(6);
    sendPreedit(&first.window, QStringLiteral("にほ"));
    QCOMPARE(firstEditor->property("preeditText").toString(), QStringLiteral("にほ"));
    secondBinding->document()->buffer()->replace({0, 0}, "x");
    QCOMPARE(document->buffer()->text(), QStringLiteral("x") + before);
    QCOMPARE(firstEditor->property("preeditText").toString(), QString());
    QVERIFY(!firstEditor->property("inputMethodComposing").toBool());
    // The view keeps its place in the text, and nothing it composed was written.
    QCOMPARE(firstEditor->property("cursorPosition").toInt(), 7);
    verifyProjection(first, document->buffer()->text());
    // A later composition starts afresh at the caret.
    sendPreedit(&first.window, QStringLiteral("にほん"));
    first.ime({}, QStringLiteral("日本"));
    QCOMPARE(document->buffer()->text(), QStringLiteral("xshared日本 text\n"));
    // A composition over a selection gives the selection back, moved by the other view's edit.
    first.select(1, 7);
    sendPreedit(&first.window, QStringLiteral("か"));
    secondBinding->document()->buffer()->replace({0, 0}, "y");
    QCOMPARE(firstEditor->property("preeditText").toString(), QString());
    QCOMPARE(first.selectionStart(), 2);
    QCOMPARE(first.selectionEnd(), 8);
    QCOMPARE(document->buffer()->text(), QStringLiteral("yxshared日本 text\n"));
}

void ParchTests::remoteEditDuringCompositionIsOneDelta()
{
    // Dropping a composition for another view's edit is not a write past the
    // source: the composing view takes that edit as one delta and nothing
    // more, and never reports a selection from before the edit.
    EditorFixture fixture;
    Document *document = fixture.open("delta.md", QStringLiteral("shared text\nsecond line\n"));
    QVERIFY(document);
    EditorView first(fixture.engine, document);
    EditorView second(fixture.engine, document, 600);
    QVERIFY2(first.ok() && second.ok(), qPrintable(first.error + second.error));
    first.select(1, 7);
    sendPreedit(&first.window, QStringLiteral("にほ"));
    QList<QList<int>> changes;
    connect(first.textDocument(), &QTextDocument::contentsChange, first.textDocument(),
            [&](int position, int removed, int added) { changes.append({position, removed, added}); });
    QList<QList<int>> reported;
    connect(first.binding, &EditorBinding::cursorChanged, first.binding,
            [&] { reported.append({first.binding->anchorSource(), first.binding->cursorSource()}); });
    second.binding->document()->buffer()->replace({0, 0}, QStringLiteral("x"));
    // Qt reports its preedit leaving as a change of equal length; the text
    // itself changes once, by the other view's edit.
    QCOMPARE(std::count_if(changes.cbegin(), changes.cend(), [](const QList<int> &change) { return change[1] != change[2]; }),
             1);
    QCOMPARE(changes.last(), (QList<int>{0, 0, 1}));
    changes.clear();
    QTest::qWait(50);
    QCOMPARE(changes, QList<QList<int>>());
    QCOMPARE(reported, (QList<QList<int>>{{2, 8}}));
    QCOMPARE(first.selectionStart(), 2);
    QCOMPARE(first.selectionEnd(), 8);
    verifyProjection(first, document->buffer()->text());
}

void ParchTests::preeditUpdatesKeepTheComposition()
{
    // Moving Qt's selection under a preedit makes Qt ask the input method to
    // commit what is being composed, so while a composition changes the
    // selection stays put and the composition stays open until it is committed.
    EditorFixture fixture;
    Document *document = fixture.open("kana.md", QStringLiteral("abc def\n"));
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QSignalSpy composing(view.editor, SIGNAL(inputMethodComposingChanged()));
    view.select(4, 7);
    for (const QString &preedit : {QStringLiteral("に"), QStringLiteral("にほ"), QStringLiteral("にほん")}) {
        sendPreedit(&view.window, preedit);
        QCOMPARE(view.editor->property("preeditText").toString(), preedit);
    }
    QCOMPARE(composing.count(), 1);
    QCOMPARE(document->buffer()->text(), QStringLiteral("abc def\n"));
    view.ime({}, QStringLiteral("日本"));
    QCOMPARE(document->buffer()->text(), QStringLiteral("abc 日本\n"));
    QCOMPARE(document->buffer()->revision(), Revision(1));
    QCOMPARE(composing.count(), 2);
}

void ParchTests::widthChangeKeepsTopAnchor()
{
    EditorFixture fixture;
    QString text;
    for (int i = 0; i < 80; ++i)
        text += QStringLiteral("Paragraph %1 ").arg(i)
            + QStringLiteral("runs on with enough words to wrap across several lines of the column. ").repeated(3)
            + u'\n';
    Document *document = fixture.open("anchor.md", text);
    QVERIFY(document);
    EditorView view(fixture.engine, document, 700, 300);
    QVERIFY2(view.ok(), qPrintable(view.error));
    EditorBinding *binding = view.binding;
    QQuickItem *pane = view.pane;
    // A long line limit lets the column follow the pane's width.
    pane->setProperty("maxLineLength", 400);
    view.flickable->setProperty("contentY", 1500);
    const int top = binding->topSourcePosition();
    QVERIFY(top > 0);
    QVERIFY2(text.at(top - 1) != u'\n', "the first visible line should start inside a paragraph");
    verifyTopLine(view, top);
    pane->setWidth(pane->width() - 200);
    QTRY_COMPARE(binding->topSourcePosition(), top);
    verifyTopLine(view, top);
    // Widening again keeps the same place rather than drifting to an earlier line.
    pane->setWidth(pane->width() + 200);
    QTRY_COMPARE(binding->topSourcePosition(), top);
    verifyTopLine(view, top);
    // Scrolling moves the anchor with the view.
    view.flickable->setProperty("contentY", view.contentY() + 300);
    QVERIFY(binding->topSourcePosition() > top);
}

void ParchTests::largeDocumentDefersRewrap()
{
    EditorFixture fixture;
    const QString text = largeText(100001);
    Document *document = fixture.open("large.md", text);
    QVERIFY(document);
    QCOMPARE(document->buffer()->length(), 100001);
    EditorView view(fixture.engine, document, 900, 400);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QQuickItem *pane = view.pane;
    QQuickItem *textEdit = view.editor;
    QObject *settle = pane->findChild<QObject *>(QStringLiteral("settle"));
    QVERIFY(settle);
    QCOMPARE(settle->property("interval").toInt(), 80);
    pane->setProperty("maxLineLength", 400);
    QTRY_COMPARE(textEdit->width(), pane->property("columnWidth").toReal());
    const qreal widthBefore = textEdit->width();
    QSignalSpy widths(textEdit, &QQuickItem::widthChanged);
    // A timer fires only from the event loop, and these steps never return to
    // it, so what each step leaves is seen exactly: the old width, and the
    // settle timer started again.
    for (int i = 1; i < 10; ++i) {
        pane->setWidth(900 - i * 30);
        QCOMPARE(textEdit->width(), widthBefore);
        QVERIFY(settle->property("running").toBool());
    }
    // Once the width stays, the column is laid out once, at the last width.
    QTRY_COMPARE_WITH_TIMEOUT(textEdit->width(), pane->property("columnWidth").toReal(), 2000);
    QCOMPARE(widths.count(), 1);
    QVERIFY(!settle->property("running").toBool());

    Document *small = fixture.open("small.md", text.left(1000));
    EditorView smallView(fixture.engine, small, 900, 400);
    QVERIFY2(smallView.ok(), qPrintable(smallView.error));
    QObject *smallSettle = smallView.pane->findChild<QObject *>(QStringLiteral("settle"));
    QVERIFY(smallSettle);
    smallView.pane->setProperty("maxLineLength", 400);
    for (int i = 1; i < 10; ++i) {
        smallView.pane->setWidth(900 - i * 30);
        QCOMPARE(smallView.editor->width(), smallView.pane->property("columnWidth").toReal());
        QVERIFY(!smallSettle->property("running").toBool());
    }
}

void ParchTests::largeTextDrawsOnlyWhatIsInView()
{
    // Qt's TextEdit builds scene nodes only for what is in view once its text
    // passes 10,000 units, but decides so only when its text property is set.
    // The projection is edited directly instead, so the binding makes the same
    // decision; without it every frame rebuilt the nodes of the whole document.
    EditorFixture fixture;
    Document *large = fixture.open("large.md", largeText(10001));
    QVERIFY(large);
    EditorView view(fixture.engine, large);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.editor->flags() & QQuickItem::ItemObservesViewport);
    Document *small = fixture.open("small.md", largeText(9999));
    EditorView smallView(fixture.engine, small);
    QVERIFY2(smallView.ok(), qPrintable(smallView.error));
    QVERIFY(!(smallView.editor->flags() & QQuickItem::ItemObservesViewport));
    smallView.cursor(0);
    sendKey(&smallView.window, 'a');
    QVERIFY(!(smallView.editor->flags() & QQuickItem::ItemObservesViewport));
    sendKey(&smallView.window, 'b');
    QCOMPARE(small->buffer()->length(), 10001);
    QVERIFY(smallView.editor->flags() & QQuickItem::ItemObservesViewport);
    smallView.binding->undo();
    QCOMPARE(small->buffer()->length(), 9999);
    QVERIFY(!(smallView.editor->flags() & QQuickItem::ItemObservesViewport));
}

void ParchTests::lineBreaksMapExactly()
{
    // Qt shows CRLF as one separator, LF, CR, U+2029 and its frame markers as
    // one separator each, and keeps U+2028; the mapping must follow exactly.
    EditorFixture fixture;
    const QString source = QStringLiteral("a\rb\nc\u2028d\u2029e\r\nf\ufdd0g");
    Document *document = fixture.open("breaks.md", source);
    QVERIFY(document);
    QCOMPARE(document->buffer()->text(), source);
    EditorView view(fixture.engine, document);
    EditorView other(fixture.engine, document, 600);
    QVERIFY2(view.ok() && other.ok(), qPrintable(view.error + other.error));
    QCOMPARE(view.textDocument()->toRawText(), QStringLiteral("a\u2029b\u2029c\u2028d\u2029e\u2029f\u2029g"));
    QCOMPARE(view.binding->sourceToEditor(9), 9);
    QCOMPARE(view.binding->sourceToEditor(10), 9);
    QCOMPARE(view.binding->sourceToEditor(11), 10);
    QCOMPARE(view.binding->editorToSource(9), 9);
    QCOMPARE(view.binding->editorToSource(10), 11);
    verifyProjection(view, source);

    // An LF after the lone CR forms a pair: Qt shows the same, the mapping changes.
    SourceBuffer *buffer = document->buffer();
    QCOMPARE(buffer->replace({2, 0}, QStringLiteral("\n")), SourceBuffer::EditOutcome::Applied);
    verifyProjection(view, buffer->text());
    QCOMPARE(view.binding->editorToSource(2), 3);
    buffer->undo();
    verifyProjection(view, buffer->text());
    buffer->redo();
    verifyProjection(other, buffer->text());
    // Removing what stands between a CR and an LF joins them; undoing splits them again.
    Document *joined = fixture.open("joined.md", QStringLiteral("\rX\nY"));
    EditorView joinedView(fixture.engine, joined);
    QVERIFY2(joinedView.ok(), qPrintable(joinedView.error));
    joinedView.select(4, 0);
    QCOMPARE(joined->buffer()->replace({1, 1}, {}), SourceBuffer::EditOutcome::Applied);
    verifyProjection(joinedView, joined->buffer()->text());
    QCOMPARE(joinedView.selectionStart(), 0);
    QCOMPARE(joinedView.selectionEnd(), 2);
    QCOMPARE(joinedView.editor->property("cursorPosition").toInt(), 0);
    joined->buffer()->undo();
    verifyProjection(joinedView, joined->buffer()->text());
    QCOMPARE(joinedView.selectionStart(), 0);
    QCOMPARE(joinedView.selectionEnd(), 4);

    // Random edits, undo and redo through both views keep both exact, and
    // each view's selection follows every source change: positions before it
    // stay, positions after it move by its length, positions inside it go to
    // its end, and one that lands between CR and LF, which Qt cannot hold,
    // falls back before the CR.
    struct Expected {
        EditorView *view;
        int anchor = 0;
        int cursor = 0;
    };
    QList<Expected> expected{{&view}, {&other}};
    QObject follower;
    connect(buffer, &SourceBuffer::sourceChanged, &follower, [&](Revision, SourceRange range, const QString &replacement) {
        const QString text = buffer->text();
        const auto follow = [&](int position) {
            int moved = position < range.start                  ? position
                : position >= range.start + range.length ? position + int(replacement.size()) - range.length
                                                         : range.start + int(replacement.size());
            if (moved > 0 && moved < text.size() && text.at(moved - 1) == u'\r' && text.at(moved) == u'\n') --moved;
            return moved;
        };
        for (Expected &place : expected) {
            place.anchor = follow(place.anchor);
            place.cursor = follow(place.cursor);
        }
    });
    QRandomGenerator random(20261114);
    const QString alphabet = QStringLiteral("ab\r\n\r\n\u2028\u2029\u0301");
    const EditKind kinds[] = {EditKind::Typing, EditKind::Deleting, EditKind::Other};
    for (int step = 0; step < 400; ++step) {
        // Each view gets a new selection now and then, read back without the binding.
        for (Expected &place : expected) {
            const int shownLength = place.view->textDocument()->characterCount() - 1;
            if (random.bounded(3) == 0)
                place.view->select(random.bounded(shownLength + 1), random.bounded(shownLength + 1));
            const QList<int> positions = projectedPositions(buffer->text());
            const int cursor = place.view->editor->property("cursorPosition").toInt();
            const int start = place.view->selectionStart(), end = place.view->selectionEnd();
            place.cursor = positions.indexOf(cursor);
            place.anchor = positions.indexOf(cursor == start ? end : start);
        }
        const int length = buffer->length();
        const int choice = random.bounded(10);
        if (choice == 0) {
            buffer->undo();
        } else if (choice == 1) {
            buffer->redo();
        } else {
            const int start = random.bounded(length + 1);
            const int removed = random.bounded(qMin(3, length - start) + 1);
            QString inserted;
            for (int n = random.bounded(4); n > 0; --n) inserted += alphabet.at(random.bounded(alphabet.size()));
            buffer->replace({start, removed}, inserted, kinds[random.bounded(3)]);
        }
        verifyProjection(view, buffer->text());
        verifyProjection(other, buffer->text());
        const QList<int> positions = projectedPositions(buffer->text());
        for (const Expected &place : expected) {
            const int cursor = place.view->editor->property("cursorPosition").toInt();
            const int start = place.view->selectionStart(), end = place.view->selectionEnd();
            QCOMPARE(place.view->binding->cursorSource(), place.cursor);
            QCOMPARE(place.view->binding->anchorSource(), place.anchor);
            QCOMPARE(cursor, positions.at(place.cursor));
            QCOMPARE(cursor == start ? end : start, positions.at(place.anchor));
        }
        if (QTest::currentTestFailed()) QFAIL(qPrintable(QStringLiteral("after step %1").arg(step)));
    }
}

void ParchTests::editsReachTheViewAsDeltas()
{
    // Each source change is one edit of the projection where it happened, so
    // typing never costs a copy of the whole document.
    EditorFixture fixture;
    const QString text = QStringLiteral("A paragraph of ordinary words.\n").repeated(100);
    Document *document = fixture.open("deltas.md", text);
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    EditorView other(fixture.engine, document, 600);
    QVERIFY2(view.ok() && other.ok(), qPrintable(view.error + other.error));
    struct Change {
        int position, removed, added;
    };
    QList<Change> own, seen;
    connect(view.textDocument(), &QTextDocument::contentsChange, view.textDocument(),
            [&](int position, int removed, int added) { own.append({position, removed, added}); });
    connect(other.textDocument(), &QTextDocument::contentsChange, other.textDocument(),
            [&](int position, int removed, int added) { seen.append({position, removed, added}); });
    view.cursor(1500);
    other.cursor(1600);
    for (const QChar key : QStringLiteral("abc")) sendKey(&view.window, key);
    QCOMPARE(document->buffer()->text(), text.left(1500) + "abc" + text.mid(1500));
    for (const QList<Change> &changes : {own, seen}) {
        QCOMPARE(changes.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(changes.at(i).position, 1500 + i);
            QCOMPARE(changes.at(i).removed, 0);
            QCOMPARE(changes.at(i).added, 1);
        }
    }
    QCOMPARE(view.editor->property("cursorPosition").toInt(), 1503);
    QCOMPARE(other.editor->property("cursorPosition").toInt(), 1603);
    // One undo of the joined step replays its three edits one by one.
    own.clear();
    seen.clear();
    view.binding->undo();
    QCOMPARE(document->buffer()->text(), text);
    for (const QList<Change> &changes : {own, seen}) {
        QCOMPARE(changes.size(), 3);
        for (const Change &change : changes) {
            QCOMPARE(change.removed, 1);
            QCOMPARE(change.added, 0);
        }
    }
    QCOMPARE(other.editor->property("cursorPosition").toInt(), 1600);
    own.clear();
    seen.clear();
    QGuiApplication::clipboard()->setText(QStringLiteral("pasted\ntext"));
    sendKey(&view.window, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(seen.size(), 1);
    QCOMPARE(seen.first().position, 1500);
    QCOMPARE(seen.first().added, 11);
    QCOMPARE(view.editor->property("cursorPosition").toInt(), 1511);
    verifyProjection(other, document->buffer()->text());
}

void ParchTests::editsNeverCopyTheWholeText()
{
    // An edit that needs no more room changes the buffer's text where it is,
    // unless something still holds a copy of it and makes the buffer copy all
    // of it first. No edit path of the binding may hold one.
    EditorFixture fixture;
    Document *document = fixture.open("copies.md", largeText(20000));
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    SourceBuffer *buffer = document->buffer();
    const QChar *const data = buffer->text().constData();
    const auto unmoved = [&] { return buffer->text().constData() == data; };
    view.select(100, 101);
    sendKey(&view.window, 'x');
    QCOMPARE(buffer->text().at(100), u'x');
    QVERIFY(unmoved());
    sendKey(&view.window, Qt::Key_Backspace);
    QVERIFY(unmoved());
    view.cursor(201);
    view.ime({}, QStringLiteral("y"), -1, 1);
    QCOMPARE(buffer->text().at(200), u'y');
    QVERIFY(unmoved());
    view.binding->replaceEditorRange(300, 301, QStringLiteral("z"));
    QCOMPARE(buffer->text().at(300), u'z');
    QVERIFY(unmoved());
    QGuiApplication::clipboard()->setText(QStringLiteral("w"));
    view.select(400, 401);
    sendKey(&view.window, Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(buffer->text().at(400), u'w');
    QVERIFY(unmoved());
    view.select(500, 501);
    sendKey(&view.window, Qt::Key_X, Qt::ControlModifier);
    QVERIFY(unmoved());
    view.binding->undo();
    view.binding->redo();
    QVERIFY(unmoved());
}

void ParchTests::readOnlyTakesNoEditsAndConflictDoes()
{
    EditorFixture fixture;
    writeFile(fixture.root.filePath("bad.md"), QByteArray("ok \xC3\x28 bad\n"));
    Document *readOnly = fixture.store->open(fixture.root.filePath("bad.md"));
    QVERIFY(readOnly);
    QCOMPARE(readOnly->state(), Document::State::ReadOnly);
    EditorView view(fixture.engine, readOnly);
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString unreadable = readOnly->buffer()->text();
    QVERIFY(view.editor->property("readOnly").toBool());
    tryEveryEdit(view);
    QCOMPARE(readOnly->buffer()->text(), unreadable);
    QCOMPARE(readOnly->buffer()->revision(), Revision(0));
    verifyProjection(view, unreadable);
    // Reading still works: select and copy.
    view.select(0, 2);
    QGuiApplication::clipboard()->clear();
    sendKey(&view.window, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("ok"));

    const QString path = fixture.root.filePath("conflict.md");
    Document *document = fixture.open("conflict.md", QStringLiteral("base\n"));
    QVERIFY(document);
    EditorView conflictView(fixture.engine, document);
    QVERIFY2(conflictView.ok(), qPrintable(conflictView.error));
    conflictView.cursor(0);
    conflictView.type(QStringLiteral("mine "));
    QVERIFY(!conflictView.editor->property("readOnly").toBool());
    writeFile(path, "theirs\n");
    fixture.store->save(document);
    QTRY_COMPARE(document->state(), Document::State::Conflict);
    // A conflict pauses saving, not writing.
    QVERIFY(!conflictView.editor->property("readOnly").toBool());
    conflictView.cursor(5);
    conflictView.type(QStringLiteral("late "));
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine late base\n"));
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(readFile(path), QByteArray("theirs\n"));
    // Resolving the conflict shows the chosen text.
    fixture.store->resolveConflict(document, ConflictChoice::Reload);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    verifyProjection(conflictView, QStringLiteral("theirs\n"));
    QVERIFY(!conflictView.editor->property("readOnly").toBool());
    conflictView.cursor(0);
    sendKey(&conflictView.window, 'x');
    QCOMPARE(document->buffer()->text(), QStringLiteral("xtheirs\n"));
}

void ParchTests::nativeWritesReturnToSource()
{
    EditorFixture fixture;
    const QString original = QStringLiteral("one\r\ntwo\n");
    Document *document = fixture.open("native.md", original);
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    SourceBuffer *buffer = document->buffer();
    // The TextEdit's own editing methods edit the source, so they undo there too.
    QVERIFY(QMetaObject::invokeMethod(view.editor, "insert", Q_ARG(int, 0), Q_ARG(QString, QStringLiteral("zero "))));
    QCOMPARE(buffer->text(), QStringLiteral("zero ") + original);
    QVERIFY(QMetaObject::invokeMethod(view.editor, "remove", Q_ARG(int, 0), Q_ARG(int, 5)));
    QCOMPARE(buffer->text(), original);
    QVERIFY(QMetaObject::invokeMethod(view.editor, "undo"));
    QCOMPARE(buffer->text(), QStringLiteral("zero ") + original);
    QVERIFY(QMetaObject::invokeMethod(view.editor, "redo"));
    QCOMPARE(buffer->text(), original);
    view.select(0, 4);
    QVERIFY(QMetaObject::invokeMethod(view.editor, "cut"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("one\r\n"));
    QCOMPARE(buffer->text(), QStringLiteral("two\n"));
    QVERIFY(QMetaObject::invokeMethod(view.editor, "paste"));
    QCOMPARE(buffer->text(), original);
    QVERIFY(QMetaObject::invokeMethod(view.editor, "append", Q_ARG(QString, QStringLiteral("three"))));
    QCOMPARE(buffer->text(), original + QStringLiteral("\r\nthree"));
    QVERIFY(QMetaObject::invokeMethod(view.editor, "clear"));
    QCOMPARE(buffer->text(), QString());
    buffer->undo();
    buffer->undo();
    QCOMPARE(buffer->text(), original);
    // Undo lives in the buffer alone; the TextEdit keeps no history of its own.
    QVERIFY(!view.textDocument()->isUndoRedoEnabled());
    QVERIFY(!view.editor->property("canUndo").toBool());
    QVERIFY(!view.editor->property("canRedo").toBool());
    // A write straight into the TextEdit cannot reach the source; the view returns to it.
    const Revision revision = buffer->revision();
    view.editor->setProperty("text", QStringLiteral("replaced"));
    QTRY_COMPARE(view.textDocument()->toRawText(), projectionOf(original));
    QCOMPARE(buffer->text(), original);
    QCOMPARE(buffer->revision(), revision);
    verifyProjection(view, original);
}

void ParchTests::imeCommitsLandOnGraphemes()
{
    EditorFixture fixture;
    // After the selected A is removed, replacing one unit after the caret would split the emoji.
    Document *document = fixture.open("emoji.md", QStringLiteral("A😀Z"));
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.select(0, 1);
    view.ime({}, QStringLiteral("x"), 1, 1);
    QCOMPARE(document->buffer()->text(), QStringLiteral("A😀Z"));
    QCOMPARE(document->buffer()->revision(), Revision(0));
    // A replacement that covers the selection and whole graphemes after it is one edit.
    view.select(0, 1);
    view.ime({}, QStringLiteral("x"), 0, 2);
    QCOMPARE(document->buffer()->text(), QStringLiteral("xZ"));
    QCOMPARE(document->buffer()->revision(), Revision(1));
    QCOMPARE(view.editor->property("cursorPosition").toInt(), 1);

    // A surrounding-text selection moves the caret within the paragraph after the commit, as Qt does.
    Document *paragraphs = fixture.open("selection.md", QStringLiteral("abc\r\ndef"));
    EditorView selectionView(fixture.engine, paragraphs);
    QVERIFY2(selectionView.ok(), qPrintable(selectionView.error));
    selectionView.cursor(7);
    QInputMethodEvent event({}, {QInputMethodEvent::Attribute(QInputMethodEvent::Selection, 1, 2, {})});
    event.setCommitString(QStringLiteral("X"));
    QCoreApplication::sendEvent(selectionView.editor, &event);
    QCOMPARE(paragraphs->buffer()->text(), QStringLiteral("abc\r\ndefX"));
    QCOMPARE(selectionView.selectionStart(), 5);
    QCOMPARE(selectionView.selectionEnd(), 7);
    QCOMPARE(selectionView.editor->property("cursorPosition").toInt(), 7);
}

void ParchTests::keysEditWholeGraphemes()
{
    // A key never splits what a reader sees as one character, even where Qt's
    // caret was put inside one from code.
    EditorFixture fixture;
    Document *document = fixture.open("graphemes.md", QStringLiteral("e\u0301Z\r\nA👩🏽\u200d💻B"));
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    SourceBuffer *buffer = document->buffer();
    view.cursor(1);
    sendKey(&view.window, 'x');
    QCOMPARE(buffer->text(), QStringLiteral("xe\u0301Z\r\nA👩🏽\u200d💻B"));
    QCOMPARE(view.editor->property("cursorPosition").toInt(), 1);
    view.select(1, 2);
    sendKey(&view.window, 'y');
    QCOMPARE(buffer->text(), QStringLiteral("xyZ\r\nA👩🏽\u200d💻B"));
    // Delete before CRLF takes the pair; Backspace after an emoji sequence takes all of it.
    view.cursor(3);
    sendKey(&view.window, Qt::Key_Delete);
    QCOMPARE(buffer->text(), QStringLiteral("xyZA👩🏽\u200d💻B"));
    view.cursor(11);
    sendKey(&view.window, Qt::Key_Backspace);
    QCOMPARE(buffer->text(), QStringLiteral("xyZAB"));
    sendKey(&view.window, Qt::Key_Backspace);
    QCOMPARE(buffer->text(), QStringLiteral("xyZB"));
    // Backspaces are one deletion, so one undo restores both.
    view.binding->undo();
    QCOMPARE(buffer->text(), QStringLiteral("xyZA👩🏽\u200d💻B"));

    Document *words = fixture.open("words.md", QStringLiteral("one two three"));
    EditorView wordView(fixture.engine, words);
    QVERIFY2(wordView.ok(), qPrintable(wordView.error));
    wordView.cursor(13);
    sendKey(&wordView.window, Qt::Key_Backspace, Qt::ControlModifier);
    QCOMPARE(words->buffer()->text(), QStringLiteral("one two "));
    wordView.cursor(0);
    sendKey(&wordView.window, Qt::Key_Delete, Qt::ControlModifier);
    QCOMPARE(words->buffer()->text(), QStringLiteral("two "));
    sendKey(&wordView.window, Qt::Key_Tab);
    QCOMPARE(words->buffer()->text(), QStringLiteral("\ttwo "));
}

void ParchTests::editorDrawsFromTheme()
{
    EditorFixture fixture;
    Document *document = fixture.store->newDraft();
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    Theme &theme = *fixture.theme;
    QCOMPARE(view.pane->property("color").value<QColor>(), theme.background());
    QCOMPARE(view.editor->property("color").value<QColor>(), theme.foreground());
    QCOMPARE(view.editor->property("selectionColor").value<QColor>(), theme.selection());
    QCOMPARE(view.editor->property("font").value<QFont>().family(), theme.fontFamily());
    // The offscreen screen has a whole device-pixel ratio, so glyphs are hinted natively.
    QCOMPARE(view.editor->property("renderType").toInt(), 1);
    QQuickItem *caret = findItem(view.editor, QStringLiteral("caret"));
    QVERIFY(caret);
    QCOMPARE(caret->property("color").value<QColor>(), theme.accent());
    replaceTheme(fixture.themeFolder.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    QTRY_COMPARE(view.editor->property("color").value<QColor>(), QColor("#342B27"));
    QCOMPARE(view.pane->property("color").value<QColor>(), QColor("#F8F1E3"));
    QCOMPARE(caret->property("color").value<QColor>(), QColor("#A7353A"));
    QCOMPARE(view.editor->property("selectionColor").value<QColor>(), theme.selection());
}

namespace {
// Qt's text control inside a TextEdit, found by its class name since the
// class is not public.
QObject *textControlOf(QQuickItem *editor)
{
    for (QObject *child : editor->children())
        if (qstrcmp(child->metaObject()->className(), "QQuickTextControl") == 0) return child;
    return nullptr;
}

// How many timers Qt's text control runs: its only one blinks its cursor.
qsizetype controlTimers(QObject *control)
{
    return QAbstractEventDispatcher::instance()->timersForObject(control).size();
}
} // namespace

void ParchTests::caretBlinksThenRestsAndDrawsNothing()
{
    // The caret blinks at the desktop's cursor flash time, rests lit once
    // the writer pauses, and from then on nothing at all is drawn. A key, a
    // move of the caret, a click or the focus lights it at once and starts
    // the blinking again. Qt's text control never runs its own blink timer,
    // each of whose ticks would draw the editor again.
    QStyleHints *hints = QGuiApplication::styleHints();
    const int flash = hints->cursorFlashTime();
    const auto restoreFlash = qScopeGuard([hints, flash] { hints->setCursorFlashTime(flash); });
    hints->setCursorFlashTime(200);
    EditorFixture fixture;
    Document *document = fixture.open("caret.md", QStringLiteral("some text\n"));
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    // The writer pauses for ten seconds before the caret rests; the test waits less.
    QCOMPARE(view.pane->property("caretRestMs").toInt(), 10000);
    // The same rest as the window's text fields, from one constant.
    QCOMPARE(view.pane->property("caretRestMs").toInt(), int(CaretRest::RestMs));
    view.pane->setProperty("caretRestMs", 1000);
    view.window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&view.window));
    view.editor->forceActiveFocus();
    QQuickItem *caret = findItem(view.editor, QStringLiteral("caret"));
    QObject *control = textControlOf(view.editor);
    auto *blink = view.pane->findChild<QObject *>(QStringLiteral("caretBlink"));
    QVERIFY(caret && control && blink);
    int frames = 0, changes = 0;
    connect(&view.window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; }, Qt::DirectConnection);
    connect(caret, &QQuickItem::visibleChanged, this, [&changes] { ++changes; });
    const auto blinking = [blink] { return blink->property("running").toBool(); };

    // It blinks at half the flash time, and Qt's control runs no timer.
    view.type(QStringLiteral("a"));
    QVERIFY(caret->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(changes >= 4, 900);
    QCOMPARE(controlTimers(control), 0);

    // A key lights it at once, even halfway through a blink.
    QTRY_VERIFY(!caret->isVisible());
    view.type(QStringLiteral("b"));
    QVERIFY(caret->isVisible());
    QVERIFY(document->buffer()->text().contains(QStringLiteral("ab")));

    // After the pause it rests lit, and nothing is drawn while nothing changes.
    QTRY_VERIFY_WITH_TIMEOUT(!blinking(), 2000);
    QVERIFY(caret->isVisible());
    QTest::qWait(100);
    frames = changes = 0;
    TimerCount timers(&view.window);
    QTest::qWait(1500);
    QCOMPARE(frames, 0);
    QCOMPARE(changes, 0);
    QCOMPARE(timers.fired, 0);
    QCOMPARE(controlTimers(control), 0);

    // Moving the caret wakes it.
    QTest::keyClick(&view.window, Qt::Key_Right);
    QVERIFY(caret->isVisible());
    QVERIFY(blinking());
    QTRY_VERIFY_WITH_TIMEOUT(!blinking(), 2000);

    // So does a click.
    const QPointF inside = view.editor->mapToScene(QPointF(5, 5));
    QTest::mouseClick(&view.window, Qt::LeftButton, {}, inside.toPoint());
    QVERIFY(blinking());
    QTRY_VERIFY_WITH_TIMEOUT(!blinking(), 2000);

    // And a key that leaves the caret where it is.
    const int cursor = view.editor->property("cursorPosition").toInt();
    QTest::keyClick(&view.window, Qt::Key_Delete);
    QCOMPARE(view.editor->property("cursorPosition").toInt(), cursor);
    QVERIFY(blinking());
    QTRY_VERIFY_WITH_TIMEOUT(!blinking(), 2000);

    // Another window in front hides it and stops it; coming back lights it
    // and starts it, and Qt's control, which took the focus again, still
    // runs no timer.
    QWindow other;
    other.resize(50, 50);
    other.show();
    other.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&other));
    QTRY_VERIFY(!caret->isVisible());
    QVERIFY(!blinking());
    QTest::qWait(100);
    frames = 0;
    QTest::qWait(500);
    QCOMPARE(frames, 0);
    view.window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&view.window));
    QTRY_VERIFY(caret->isVisible());
    QVERIFY(blinking());
    QTest::qWait(50);
    QCOMPARE(controlTimers(control), 0);

    // A document that becomes read-only, as one that is not valid UTF-8 is,
    // shows no caret, and Qt's control is kept from blinking through the change.
    view.editor->setProperty("readOnly", true);
    QTRY_VERIFY(!caret->isVisible());
    QVERIFY(!blinking());
    view.editor->setProperty("readOnly", false);
    QTRY_VERIFY(caret->isVisible());
    QTest::qWait(50);
    QCOMPARE(controlTimers(control), 0);
}

void ParchTests::caretIsSteadyWithoutMotionOrFlash()
{
    // A flash time of 0 means a steady caret, and so does reduced motion.
    QStyleHints *hints = QGuiApplication::styleHints();
    const int flash = hints->cursorFlashTime();
    const auto restoreFlash = qScopeGuard([hints, flash] { hints->setCursorFlashTime(flash); });
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("animations:enabled"), 1);
    EditorFixture fixture;
    fixture.theme = std::make_unique<Theme>(fixture.themeFolder.path(), nullptr, sockets.path());
    fixture.engine.rootContext()->setContextProperty(QStringLiteral("theme"), fixture.theme.get());
    Document *document = fixture.open("steady.md", QStringLiteral("text\n"));
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&view.window));
    view.editor->forceActiveFocus();
    QQuickItem *caret = findItem(view.editor, QStringLiteral("caret"));
    auto *blink = view.pane->findChild<QObject *>(QStringLiteral("caretBlink"));
    QVERIFY(caret && blink);
    QTRY_VERIFY(hyprland.listeners() == 1);
    int changes = 0;
    connect(caret, &QQuickItem::visibleChanged, this, [&changes] { ++changes; });
    const auto steady = [&](const char *why) {
        view.type(QStringLiteral("x"));
        QTest::qWait(50);
        changes = 0;
        QTest::qWait(400);
        QVERIFY2(caret->isVisible(), why);
        QVERIFY2(!blink->property("running").toBool(), why);
        QVERIFY2(changes == 0, why);
    };

    hints->setCursorFlashTime(0);
    steady("flash time 0");
    hints->setCursorFlashTime(200);
    view.type(QStringLiteral("x"));
    QVERIFY(blink->property("running").toBool());

    hyprland.setInt(QStringLiteral("animations:enabled"), 0);
    hyprland.send("configreloaded>>");
    QTRY_VERIFY(fixture.theme->reduceMotion());
    steady("reduced motion");
}

void ParchTests::columnHoldsTheLineLength()
{
    // The column holds maxLineLength characters at most and stays centred; a
    // narrower pane narrows it to leave 24 pixels each side. Every paragraph,
    // a new one too, takes the line height.
    EditorFixture fixture;
    Document *document = fixture.open("column.md", QStringLiteral("first\nsecond\n"));
    QVERIFY(document);
    EditorView view(fixture.engine, document, 1400, 300);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QQuickItem *pane = view.pane;
    const qreal character = pane->property("characterWidth").toReal();
    QVERIFY(character > 0);
    QCOMPARE(pane->property("columnWidth").toReal(), 72 * character);
    QCOMPARE(view.editor->width(), 72 * character);
    QCOMPARE(view.editor->x(), qreal(qRound((1400 - 72 * character) / 2)));
    pane->setWidth(400);
    QCOMPARE(view.editor->width(), qreal(352));
    QCOMPARE(view.editor->x(), qreal(24));
    pane->setProperty("maxLineLength", 20);
    QCOMPARE(view.editor->width(), 20 * character);
    const auto lineHeights = [&] {
        QList<qreal> heights;
        for (QTextBlock block = view.textDocument()->begin(); block.isValid(); block = block.next())
            heights.append(block.blockFormat().lineHeight());
        return heights;
    };
    // Lines sit a multiple of the font's pixel size apart.
    const qreal pixels = pane->property("fontPixelSize").toInt();
    QCOMPARE(lineHeights(), QList<qreal>(3, qRound(1.5 * pixels)));
    view.cursor(5);
    sendKey(&view.window, Qt::Key_Return);
    QCOMPARE(lineHeights(), QList<qreal>(4, qRound(1.5 * pixels)));
    pane->setProperty("lineHeight", 2);
    QTRY_COMPARE(lineHeights(), QList<qreal>(4, qRound(2 * pixels)));
    // A bigger font widens the column with it.
    pane->setProperty("fontPixelSize", pane->property("fontPixelSize").toInt() + 6);
    QTRY_VERIFY(pane->property("characterWidth").toReal() > character);
    QTRY_COMPARE(view.editor->width(), pane->property("columnWidth").toReal());
    QCOMPARE(view.editor->property("font").value<QFont>().pixelSize(), pane->property("fontPixelSize").toInt());
}

void ParchTests::linesSitTheLineHeightApart()
{
    // The line height is a multiple of the editor font's own size, as the
    // visual target sets it: lines sit round(lineHeight × pixel size) apart,
    // whatever the font's own line spacing, at every zoom and line height.
    EditorFixture fixture;
    QString text;
    for (int i = 0; i < 60; ++i) text += QStringLiteral("word ");
    Document *document = fixture.open("wrap.md", text + QStringLiteral("\nnext\n"));
    QVERIFY(document);
    EditorView view(fixture.engine, document, 360, 600);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QObject *pane = view.pane;
    // The baselines of a wrapped paragraph's first three lines, as laid out,
    // and the tops of the caret on them, as the editor gives them.
    const auto spacing = [&](int pixelSize, qreal lineHeight) {
        pane->setProperty("fontPixelSize", pixelSize);
        pane->setProperty("lineHeight", lineHeight);
        QTRY_COMPARE(view.editor->property("font").value<QFont>().pixelSize(), pixelSize);
        QTest::qWait(20);
        const QTextBlock block = view.textDocument()->begin();
        QTextLayout *layout = block.layout();
        QVERIFY(layout && layout->lineCount() >= 3);
        const qreal expected = qRound(lineHeight * pixelSize);
        QList<qreal> baselines, carets;
        for (int i = 0; i < 3; ++i) {
            const QTextLine line = layout->lineAt(i);
            baselines << line.y() + line.ascent();
            QRectF caret;
            QMetaObject::invokeMethod(view.editor, "positionToRectangle", Q_RETURN_ARG(QRectF, caret),
                                      Q_ARG(int, line.textStart()));
            carets << caret.y();
        }
        QCOMPARE(baselines.at(1) - baselines.at(0), expected);
        QCOMPARE(baselines.at(2) - baselines.at(1), expected);
        QCOMPARE(carets.at(1) - carets.at(0), expected);
        // A point on the second line finds a position on it.
        int hit = -1;
        QMetaObject::invokeMethod(view.editor, "positionAt", Q_RETURN_ARG(int, hit), Q_ARG(qreal, 1),
                                  Q_ARG(qreal, carets.at(1) + 2));
        QCOMPARE(hit, layout->lineAt(1).textStart());
    };
    spacing(20, 1.5);
    if (QTest::currentTestFailed()) return;
    // Zooming and another line height apply it again.
    spacing(26, 1.5);
    if (QTest::currentTestFailed()) return;
    spacing(26, 2);
    if (QTest::currentTestFailed()) return;
    // A paragraph Enter makes takes the same.
    view.cursor(view.editor->property("length").toInt());
    sendKey(&view.window, Qt::Key_Return);
    for (QTextBlock block = view.textDocument()->begin(); block.isValid(); block = block.next()) {
        QCOMPARE(block.blockFormat().lineHeightType(), int(QTextBlockFormat::FixedHeight));
        QCOMPARE(block.blockFormat().lineHeight(), qreal(52));
    }
}

void ParchTests::caretStaysInViewAsTheTextAreaShrinks()
{
    // A notice bar, the tab strip or the window's own size changes the text
    // area's height. A caret that was in view stays in view, by the least
    // scroll that shows it; otherwise the view keeps its top line.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    QByteArray text;
    for (int line = 1; line <= 150; ++line) text += "Line " + QByteArray::number(line) + " of the text\n";
    writeFile(file, text);
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *flick = shell.item(QStringLiteral("flickable"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    const auto caret = [&] {
        return editor->property("cursorRectangle").toRectF().translated(editor->position());
    };
    const auto top = [&] { return flick->property("contentY").toReal(); };
    const auto inView = [&] { return caret().top() >= top() - 0.5 && caret().bottom() <= top() + flick->height() + 0.5; };
    // The caret on the last line in view.
    editor->setProperty("cursorPosition", int(QString::fromUtf8(text).indexOf(QStringLiteral("Line 100 "))));
    QTRY_VERIFY(inView());
    flick->setProperty("contentY", caret().bottom() - flick->height());
    QVERIFY(inView());
    // A notice takes room from the top of the text area.
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QTRY_VERIFY(shell.item(QStringLiteral("noticeBar"))->isVisible());
    QTRY_VERIFY2(inView(), qPrintable(QStringLiteral("caret %1..%2, view %3..%4")
                                         .arg(caret().top()).arg(caret().bottom())
                                         .arg(top()).arg(top() + flick->height())));
    QVERIFY(caret().bottom() >= top() + flick->height() - caret().height() - 1);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!shell.item(QStringLiteral("noticeBar"))->isVisible());

    // The caret near the top: the top line stays where it is.
    flick->setProperty("contentY", std::floor(caret().top()));
    const qreal before = top();
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QTRY_VERIFY(shell.item(QStringLiteral("noticeBar"))->isVisible());
    QTest::qWait(50);
    QCOMPARE(top(), before);
    QVERIFY(inView());
    // The window grows: nothing scrolls.
    window->setHeight(window->height() + 200);
    QTRY_COMPARE(window->contentItem()->height(), qreal(window->height()));
    QTest::qWait(50);
    QCOMPARE(top(), before);
}

void ParchTests::bindingOutlivesItsEditor()
{
    // EditorBinding is a QML type of its own, so its TextEdit can go first, as
    // under a Loader. The binding then lets go of the document and answers
    // every call as a binding without an editor; a new editor shows the
    // document again.
    EditorFixture fixture;
    const QString original = QStringLiteral("hello\r\nworld\n");
    Document *document = fixture.open("first.md", original);
    QVERIFY(document);
    SourceBuffer *buffer = document->buffer();
    const auto makeEditor = [&] {
        QQmlComponent component(&fixture.engine);
        component.setData("import QtQuick\nTextEdit { width: 300; textFormat: TextEdit.PlainText }", QUrl());
        return qobject_cast<QQuickItem *>(component.create());
    };
    for (const bool composing : {false, true}) {
        EditorBinding binding;
        QSignalSpy editorChanged(&binding, &EditorBinding::editorChanged);
        QQuickItem *editor = makeEditor();
        QVERIFY(editor);
        binding.setDocument(document);
        binding.setEditor(editor);
        QVERIFY(binding.editable());
        if (composing) {
            QInputMethodEvent preedit(QStringLiteral("か"), {});
            QCoreApplication::sendEvent(editor, &preedit);
            QVERIFY(editor->property("inputMethodComposing").toBool());
        }
        delete editor;
        QCOMPARE(binding.editor(), nullptr);
        QVERIFY(!binding.editable());
        QCOMPARE(editorChanged.count(), 2);
        buffer->replace({0, 0}, QStringLiteral("x"));
        buffer->undo();
        buffer->redo();
        binding.undo();
        binding.redo();
        binding.copy();
        binding.cut();
        binding.paste();
        binding.replaceEditorRange(0, 1, QStringLiteral("y"));
        binding.appendParagraph(QStringLiteral("z"));
        binding.restore(1, 1, 0);
        binding.setLineHeight(2);
        QCOMPARE(binding.topSourcePosition(), 0);
        QCOMPARE(binding.sourceToEditor(0), -1);
        QCOMPARE(binding.editorToSource(0), -1);
        QCOMPARE(buffer->text(), QStringLiteral("x") + original);
        // QML setting the editor to nothing afterwards changes nothing more.
        binding.setEditor(nullptr);
        QCOMPARE(editorChanged.count(), 2);
        QCoreApplication::processEvents();
        std::unique_ptr<QQuickItem> next(makeEditor());
        binding.setEditor(next.get());
        QVERIFY(binding.editable());
        QCOMPARE(next->property("textDocument").value<QQuickTextDocument *>()->textDocument()->toRawText(),
                 projectionOf(buffer->text()));
        binding.setEditor(nullptr);
        buffer->undo();
        QCOMPARE(buffer->text(), original);
    }
}

void ParchTests::bindingOutlivesItsDocument()
{
    // A document the store closes while a view still shows it leaves that
    // view empty and read-only, composition included, and nothing in the view
    // reaches for the buffer that went with it.
    EditorFixture fixture;
    Document *document = fixture.open("gone.md", QStringLiteral("hello\r\nworld\n"));
    QVERIFY(document);
    EditorView view(fixture.engine, document);
    QVERIFY2(view.ok(), qPrintable(view.error));
    fixture.store->retain(document);
    view.cursor(3);
    sendPreedit(&view.window, QStringLiteral("か"));
    QVERIFY(view.editor->property("inputMethodComposing").toBool());
    QPointer<Document> watched = document;
    fixture.store->release(document);
    QTRY_VERIFY(watched.isNull());
    QCOMPARE(view.binding->document(), nullptr);
    QVERIFY(!view.editor->property("inputMethodComposing").toBool());
    QVERIFY(view.editor->property("readOnly").toBool());
    QCOMPARE(view.textDocument()->toRawText(), QString());
    sendKey(&view.window, 'x');
    view.ime({}, QStringLiteral("y"));
    sendKey(&view.window, Qt::Key_Return);
    view.binding->undo();
    view.binding->copy();
    view.binding->paste();
    view.binding->restore(1, 1, 0);
    QCOMPARE(view.binding->topSourcePosition(), 0);
    QCOMPARE(view.textDocument()->toRawText(), QString());
}

void ParchTests::editorLookupsFailVisibly()
{
    // The binding finds the TextEdit's members by name. An editor that lacks
    // one is refused with a warning that names what is missing, never used
    // half-way.
    EditorFixture fixture;
    Document *document = fixture.open("odd.md", QStringLiteral("text\n"));
    QVERIFY(document);
    EditorBinding binding;
    binding.setDocument(document);
    QQmlComponent lookalike(&fixture.engine);
    lookalike.setData("import QtQuick\nItem { width: 300; readonly property QtObject textDocument: inner.textDocument\n"
                      "TextEdit { id: inner; width: 300 } }",
                      QUrl());
    std::unique_ptr<QQuickItem> item(qobject_cast<QQuickItem *>(lookalike.create()));
    QVERIFY(item);
    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression("EditorBinding: .*cursorPosition, selectionStart, selectionEnd, "
                                            "cursorRectangle, select\\(int,int\\), positionAt\\(qreal,qreal\\), "
                                            "positionToRectangle\\(int\\)"));
    binding.setEditor(item.get());
    QVERIFY(!binding.editable());
    QCOMPARE(binding.sourceToEditor(0), -1);
    QQmlComponent plain(&fixture.engine);
    plain.setData("import QtQuick\nItem { width: 300 }", QUrl());
    std::unique_ptr<QQuickItem> other(qobject_cast<QQuickItem *>(plain.create()));
    QVERIFY(other);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("EditorBinding: .*textDocument"));
    binding.setEditor(other.get());
    QVERIFY(!binding.editable());
    binding.setEditor(nullptr);
}


void ParchTests::layoutHelpersCatchDeliberateBreaks()
{
    // Each check the window tests make fails on a scene built to break it, for
    // the reason it was built to fail, so that a window that passes them has
    // been checked.
    QQmlEngine engine;
    QString error;
    std::unique_ptr<QQuickWindow> scene;
    const auto load = [&](const QByteArray &qml) {
        scene = sceneWindow(engine, qml, &error);
        return scene != nullptr;
    };
    const auto mentions = [](const QStringList &problems, const QString &reason) {
        return problems.join(u';').contains(reason);
    };
    QStringList problems;

    QVERIFY2(load("Item { width: 200; height: 100\n"
                  "  Text { width: 60; elide: Text.ElideRight; text: \"a label far too long for its box\" }\n"
                  "  Item { y: 40; width: 200; height: 50; clip: true; Rectangle { x: 150; width: 300; height: 10 } } }"),
             qPrintable(error));
    QVERIFY2(everyVisibleItemFitsWithin(scene.get(), &problems), qPrintable(problems.join(u';')));
    QVERIFY2(load("Item { width: 200; height: 100; Rectangle { x: 180; width: 50; height: 10 } }"), qPrintable(error));
    QVERIFY(!everyVisibleItemFitsWithin(scene.get(), &problems));
    QVERIFY2(mentions(problems, QStringLiteral("QQuickRectangle at 180,0 50x10")), qPrintable(problems.join(u';')));
    QVERIFY2(load("Item { width: 200; height: 100; Text { width: 40; text: \"a label wider than its box\" } }"),
             qPrintable(error));
    QVERIFY(!everyVisibleItemFitsWithin(scene.get(), &problems));
    QVERIFY2(mentions(problems, QStringLiteral("in 40x")), qPrintable(problems.join(u';')));
    QVERIFY2(load("Item { width: 200; height: 100; Text { width: 190; height: 4; text: \"tall\" } }"), qPrintable(error));
    QVERIFY(!everyVisibleItemFitsWithin(scene.get(), &problems));
    QVERIFY2(mentions(problems, QStringLiteral("in 190x4")), qPrintable(problems.join(u';')));

    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QVERIFY2(load("Column { Button { text: \"Named\" } Rectangle { width: 10; height: 10; Accessible.role: Accessible.Button } }"),
             qPrintable(error));
    const QStringList unnamed = unnamedAccessibleItems(scene.get());
    QCOMPARE(unnamed.size(), 1);
    QVERIFY2(unnamed.first().startsWith(QStringLiteral("QQuickRectangle")), qPrintable(unnamed.first()));

    const auto walk = [&](const QByteArray &qml) {
        if (!load(qml)) return false;
        activate(scene.get());
        return tabKeyReachesEveryControl(scene.get(), &problems);
    };
    QVERIFY2(walk("Column { Button { text: \"One\" } Button { text: \"Two\" } }"), qPrintable(error + problems.join(u';')));
    QVERIFY(!walk("Column { Button { text: \"One\" } Button { text: \"Two\"; focusPolicy: Qt.NoFocus } }"));
    QVERIFY2(scene && mentions(problems, QStringLiteral("\"Two\" is not reached by Tab")), qPrintable(error + problems.join(u';')));
    QVERIFY(!walk("Column { Button { text: \"One\"; background: Rectangle { implicitWidth: 40; implicitHeight: 20 } }"
                  " Button { text: \"Two\" } }"));
    QVERIFY2(scene && mentions(problems, QStringLiteral("\"One\" draws no focus")), qPrintable(error + problems.join(u';')));
    QVERIFY(!walk("Item { width: 100; height: 40 }"));
    QVERIFY2(scene && mentions(problems, QStringLiteral("no control takes focus")), qPrintable(error + problems.join(u';')));

    for (const char *typing : {"Q", "Space", "?", "Shift+Q", "Return", ""})
        QVERIFY2(!isModifiedOrFunctionKey(QKeySequence(QString::fromLatin1(typing))), typing);
    for (const char *shortcut : {"Ctrl+Q", "Ctrl+Shift+Tab", "Alt+Left", "F11", "Escape"})
        QVERIFY2(isModifiedOrFunctionKey(QKeySequence(QString::fromLatin1(shortcut))), shortcut);

    int count = 0;
    QVERIFY2(load("Rectangle { width: 10; height: 10; Behavior on x { NumberAnimation { duration: 0 } } }"), qPrintable(error));
    QVERIFY(allBehaviorDurationsAreZero(scene.get(), &problems, &count));
    QCOMPARE(count, 1);
    QVERIFY2(load("Rectangle { width: 10; height: 10; Behavior on x { NumberAnimation { duration: 100 } } }"), qPrintable(error));
    QVERIFY(!allBehaviorDurationsAreZero(scene.get(), &problems, &count));
    QVERIFY2(mentions(problems, QStringLiteral("takes 100 ms")), qPrintable(problems.join(u';')));
    QVERIFY2(load("Rectangle { width: 10; height: 10; Behavior on x { SmoothedAnimation { velocity: 5 } } }"), qPrintable(error));
    QVERIFY(!allBehaviorDurationsAreZero(scene.get(), &problems, &count));
    QVERIFY2(mentions(problems, QStringLiteral("QQuickSmoothedAnimation takes -1 ms")), qPrintable(problems.join(u';')));

    const QStringList literals = colourLiterals(QStringLiteral("color: \"#fff\"\ncolor: Qt.rgba(1, 0, 0, 1)\ncolor: \"red\"\n"
                                                               "border.color: 'Blue'\ncolor: \"transparent\"\n"
                                                               "color: theme.accent\ntext: qsTr(\"Saved\")"),
                                                QStringLiteral("scene.qml"));
    QCOMPARE(literals, (QStringList{QStringLiteral("scene.qml:1: color: \"#fff\""),
                                    QStringLiteral("scene.qml:2: color: Qt.rgba(1, 0, 0, 1)"),
                                    QStringLiteral("scene.qml:3: color: \"red\""),
                                    QStringLiteral("scene.qml:4: border.color: 'Blue'")}));
}

void ParchTests::shellLaysOutAtFourWidths()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    QQuickItem *library = shell.item(QStringLiteral("library"));
    QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QVERIFY(library);
    for (int width : {360, 640, 960, 1440}) {
        resizeWindow(window, width);
        QTest::qWait(50);
        QStringList problems;
        QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(QStringLiteral("%1: %2").arg(width).arg(problems.join("; "))));
        QVERIFY2(editor->width() >= qMin(300, width - 48), qPrintable(QStringLiteral("%1: %2").arg(width).arg(editor->width())));
    }
    resizeWindow(window, 360);
    QTRY_VERIFY(!library->property("docked").toBool());
    QVERIFY(!library->isVisible());
    resizeWindow(window, 1440);
    QTRY_VERIFY(library->property("docked").toBool());
    QVERIFY(library->isVisible());
    QCOMPARE(library->width(), qreal(shell.settings()->libraryWidth()));
    QVERIFY(pane->mapToScene(QPointF(0, 0)).x() >= library->mapToScene(QPointF(library->width(), 0)).x());

    // The character width is the editor font's own, at the size the settings give.
    const qreal characterWidth = pane->property("characterWidth").toReal();
    QFont font(shell.settings()->editorFontFamily());
    font.setPixelSize(shell.settings()->fontPixelSize());
    QVERIFY2(qAbs(QFontMetricsF(font).horizontalAdvance(u'0') - characterWidth) < 0.5, qPrintable(QString::number(characterWidth)));
    QCOMPARE(qRound(pane->property("columnWidth").toReal()), qRound(72 * characterWidth));

    // The library docks exactly when the rest of the window keeps 45 characters and the margins.
    const int boundary = qCeil(shell.settings()->libraryWidth() + 45 * characterWidth + 48);
    resizeWindow(window, boundary);
    QVERIFY(library->property("docked").toBool());
    resizeWindow(window, boundary - 1);
    QVERIFY(!library->property("docked").toBool());
}

void ParchTests::singleTabHidesStrip()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickItem *tabStrip = shell.item(QStringLiteral("tabStrip"));
    QVERIFY(tabStrip);
    // The strip holds the window's one tab, and hides it.
    QCOMPARE(tabStrip->property("count").toInt(), 1);
    QVERIFY(!tabStrip->isVisible());
    // The editor takes the strip's place, right below the header.
    QQuickItem *header = shell.item(QStringLiteral("header"));
    QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
    QVERIFY(header);
    QVERIFY(pane->mapToScene(QPointF(0, 0)).y() <= header->mapToScene(QPointF(0, header->height())).y() + 1);
}

void ParchTests::tabStripKeepsTabsReadableAndScrolls()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 360);
    QQuickItem *tabStrip = shell.item(QStringLiteral("tabStrip"));
    TabModel *tabs = shell.context()->tabs();
    QStringList titles;
    for (int i = 0; i < 8; ++i) {
        Document *document = shell.backend->store()->newDraft();
        // One name is short, which must not make its tab narrower than the rest.
        document->buffer()->replace({0, 0}, i == 3 ? QStringLiteral("Brief")
                                                   : QStringLiteral("A document with a rather long first line %1").arg(i));
        titles << document->title();
        tabs->openDocument(document);
    }
    // The first launch's blank draft goes, leaving the eight, the first in front.
    tabs->closeTab(0);
    tabs->setActiveIndex(0);
    QTRY_VERIFY(tabStrip->isVisible());
    QCOMPARE(tabStrip->property("count").toInt(), 8);
    QQuickItem *list = findItem(tabStrip, QStringLiteral("tabs"));
    QVERIFY(list);
    // A tab keeps room for a readable name, measured in the interface font.
    const qreal minimum = tabStrip->property("minimumTabWidth").toReal();
    QVERIFY2(minimum >= 8 * QFontMetricsF(shell.theme->font()).horizontalAdvance(u'0'), qPrintable(QString::number(minimum)));
    const auto tabAt = [&](int index) {
        QQuickItem *tab = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tab), Q_ARG(int, index));
        return tab;
    };
    for (int i = 0; i < 8; ++i) {
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, i), Q_ARG(int, 2 /* ListView.Contain */));
        QTRY_VERIFY(tabAt(i));
        QVERIFY2(tabAt(i)->width() >= minimum, qPrintable(QStringLiteral("tab %1: %2").arg(i).arg(tabAt(i)->width())));
    }
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 0), Q_ARG(int, 1 /* ListView.Beginning */));
    // The tabs do not fit even at that width, so the strip scrolls sideways.
    QVERIFY(list->property("contentWidth").toReal() > list->width());
    tabs->setActiveIndex(7);
    QCOMPARE(tabStrip->property("currentIndex").toInt(), 7);
    QTRY_VERIFY(list->property("contentX").toReal() > 0);
    QTRY_VERIFY(tabAt(7));
    QQuickItem *last = tabAt(7);
    QVERIFY(last->width() >= minimum);
    // The strip settles once its tabs have their final widths.
    const auto shownRect = [&] { return list->mapRectToScene(QRectF(0, 0, list->width(), list->height())); };
    const auto lastRect = [&] { return last->mapRectToScene(QRectF(0, 0, last->width(), last->height())); };
    const auto describeRect = [](const QRectF &r) {
        return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    };
    QTRY_VERIFY2(shownRect().adjusted(-0.5, -0.5, 0.5, 0.5).contains(lastRect()),
                 qPrintable(QStringLiteral("strip %1, last tab %2, contentX %3")
                                .arg(describeRect(shownRect()), describeRect(lastRect()))
                                .arg(list->property("contentX").toReal())));
    // Each tab is named in full, however much of its name the strip shows.
    QCOMPARE(last->property("text").toString(), titles.at(7));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));

    // An edge with more tabs beyond it is marked, from the theme's colours.
    QQuickItem *before = findItem(tabStrip, QStringLiteral("tabsBeforeMark"));
    QQuickItem *after = findItem(tabStrip, QStringLiteral("tabsAfterMark"));
    QVERIFY(before && after);
    const auto marked = [&] { return QPair<bool, bool>(before->isVisible(), after->isVisible()); };
    QTRY_COMPARE(marked(), qMakePair(true, false));
    tabs->setActiveIndex(0);
    QTRY_COMPARE(marked(), qMakePair(false, true));
    QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 3), Q_ARG(int, 2 /* ListView.Contain */));
    QTRY_COMPARE(marked(), qMakePair(true, true));
    // Each mark is the theme's dim role, in a gutter of its own beside the
    // tabs, so it covers none of them.
    const QRectF view = list->mapRectToScene(QRectF(0, 0, list->width(), list->height()));
    const QRectF strip = tabStrip->mapRectToScene(QRectF(0, 0, tabStrip->width(), tabStrip->height()));
    for (QQuickItem *mark : {before, after}) {
        QCOMPARE(mark->property("color").value<QColor>().rgba(), shell.theme->dim().rgba());
        const QRectF box = mark->mapRectToScene(QRectF(0, 0, mark->width(), mark->height()));
        QVERIFY2(!box.intersects(view.adjusted(0.5, 0.5, -0.5, -0.5)), qPrintable(describe(mark)));
        QVERIFY(strip.contains(box));
    }
    QVERIFY(before->mapToScene(QPointF(0, 0)).x() < view.left());
    QVERIFY(after->mapToScene(QPointF(0, 0)).x() >= view.right() - 0.5);
    QVERIFY(everyVisibleItemFitsWithin(window, &problems));
    // With room for every tab, nothing is beyond either edge. How wide that
    // is depends on the interface font, so the window grows until it is.
    for (int width = 1440; width <= 4096 && tabStrip->property("overflowing").toBool(); width += 320)
        resizeWindow(window, width);
    QVERIFY2(!tabStrip->property("overflowing").toBool(), qPrintable(QString::number(window->width())));
    QTRY_COMPARE(marked(), qMakePair(false, false));
}

void ParchTests::everyTabClearsTheNewTabButton()
{
    // However narrow the window and however many tabs, no tab is cut off at
    // the new tab button or at the strip's other end: a tab in view is
    // wholly in view, and the current one always is. Tabs that all fit keep
    // the width of their names.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *tabStrip = shell.item(QStringLiteral("tabStrip"));
    QQuickItem *list = findItem(tabStrip, QStringLiteral("tabs"));
    QQuickItem *plus = shell.item(QStringLiteral("newTabButton"));
    QVERIFY(tabStrip && list && plus);
    TabModel *tabs = shell.context()->tabs();
    const auto tabAt = [&](int index) {
        QQuickItem *tab = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tab), Q_ARG(int, index));
        return tab;
    };
    const auto sceneRect = [](QQuickItem *item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); };
    // Every tab that shows at all shows whole, the current one among them
    // unless the strip was scrolled away from it, and none lies under the button.
    const auto problems = [&](bool currentShown = true) {
        QStringList found;
        const QRectF view = sceneRect(list);
        const QRectF button = sceneRect(plus);
        for (int i = 0; i < tabs->rowCount(); ++i) {
            QQuickItem *tab = tabAt(i);
            const QRectF shown = tab ? sceneRect(tab).intersected(view) : QRectF();
            const bool whole = tab && view.adjusted(-0.5, -0.5, 0.5, 0.5).contains(sceneRect(tab));
            if (currentShown && i == tabs->activeIndex() && !whole)
                found << QStringLiteral("the current tab %1 is not wholly in view").arg(i);
            else if (!whole && shown.width() > 0.5) found << QStringLiteral("tab %1 is cut off").arg(i);
            if (shown.intersected(button).width() > 0.5) found << QStringLiteral("tab %1 lies under the button").arg(i);
        }
        return found;
    };
    QStringList titles{QStringLiteral("Brief"), QStringLiteral("A document with a rather long first line"),
                       QStringLiteral("Morning pages"), QStringLiteral("Letter to the editor of the town paper"),
                       QStringLiteral("Notes"), QStringLiteral("Another long name for a document"),
                       QStringLiteral("Seventh"), QStringLiteral("The eighth and last of them")};
    // The first launch's blank draft is the first tab.
    for (int count = 2; count <= 8; ++count) {
        Document *document = shell.backend->store()->newDraft();
        document->buffer()->replace({0, 0}, titles.at(count - 2));
        tabs->openDocument(document);
        if (count != 3 && count != 4 && count != 8) continue;
        for (const int width : {360, 640, 1440}) {
            resizeWindow(window, width);
            for (int active = 0; active < count; ++active) {
                tabs->setActiveIndex(active);
                QTest::qWait(20);
                const QString label = QStringLiteral("%1 tabs at %2, tab %3 in front").arg(count).arg(width).arg(active);
                QTRY_VERIFY2(problems().isEmpty(), qPrintable(label + u' ' + problems().join(QStringLiteral("; "))));
                QVERIFY2(tabAt(active)->width() >= tabStrip->property("minimumTabWidth").toReal() - 0.5, qPrintable(label));
            }
        }
    }
    // Flicked, the strip comes to rest a whole tab at a time.
    resizeWindow(window, 360);
    tabs->setActiveIndex(0);
    QTest::qWait(50);
    const qreal start = list->property("contentX").toReal();
    QMetaObject::invokeMethod(list, "flick", Q_ARG(qreal, -700), Q_ARG(qreal, 0));
    QTRY_VERIFY(list->property("contentX").toReal() > start);
    QTRY_VERIFY(!list->property("moving").toBool());
    QTRY_VERIFY2(problems(false).isEmpty(), qPrintable(problems(false).join(QStringLiteral("; "))));
    // Wide enough for every name, each tab keeps the width of its own.
    resizeWindow(window, 1440);
    while (tabs->rowCount() > 3) tabs->closeTab(tabs->rowCount() - 1);
    QTest::qWait(50);
    QVERIFY(!tabStrip->property("squeezed").toBool());
    QVERIFY(tabAt(1)->width() < tabAt(2)->width());
}

void ParchTests::tabsMarkDocumentsThatNeedAttention()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("changed.md")), invalid = files.filePath(QStringLiteral("invalid.md"));
    writeFile(file, "base\n");
    writeFile(invalid, QByteArray("caf\xe9\n"));
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    DocumentStore *store = shell.backend->store();
    // A draft whose save failed, a file in conflict and a read-only file,
    // with a clean draft in front of them all.
    Document *failed = shell.document();
    writeFile(shell.paths().drafts() + u'/' + failed->id(), "in the way");
    failed->buffer()->replace({0, 0}, QStringLiteral("words"));
    store->save(failed);
    QTRY_COMPARE(failed->state(), Document::State::SaveFailed);
    shell.backend->openFiles({file});
    Document *conflicted = shell.document();
    conflicted->buffer()->replace({0, 0}, QStringLiteral("mine "));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(conflicted->state(), Document::State::Conflict, 3000);
    shell.backend->openFiles({invalid});
    QCOMPARE(shell.document()->state(), Document::State::ReadOnly);
    shell.backend->newDraft();
    QCOMPARE(shell.document()->state(), Document::State::Clean);
    // The bar speaks only of the document in front, so the tabs must speak of the rest.
    QTRY_VERIFY(!shell.item(QStringLiteral("noticeBar"))->isVisible());

    QQuickItem *list = findItem(shell.item(QStringLiteral("tabStrip")), QStringLiteral("tabs"));
    QVERIFY(list);
    const auto tabAt = [&](int index) {
        QQuickItem *tab = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, tab), Q_ARG(int, index));
        return tab;
    };
    Theme *theme = shell.theme.get();
    // Each in turn: in the light theme the window starts in, and in a dark one.
    for (const bool dark : {false, true}) {
        if (dark) {
            replaceTheme(shell.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
            QTRY_COMPARE(window->color(), QColor("#1a1b26"));
        }
        const QList<QColor> expected{theme->danger(), theme->danger(), theme->warning(), QColor()};
        for (int i = 0; i < 4; ++i) {
            const QString label = QStringLiteral("dark %1, tab %2").arg(dark).arg(i);
            QTRY_VERIFY(tabAt(i));
            QQuickItem *mark = findItem(tabAt(i), QStringLiteral("tabMark"));
            QVERIFY2(mark, qPrintable(label));
            const bool marked = expected.at(i).isValid();
            QTRY_VERIFY2(mark->isVisible() == marked, qPrintable(label));
            if (!marked) continue;
            QCOMPARE(mark->property("color").value<QColor>(), expected.at(i));
            // Its edge is drawn as the text is, so the mark reads against the
            // strip whatever the theme's red and yellow are.
            auto *edge = mark->property("border").value<QObject *>();
            QVERIFY2(edge && edge->property("width").toReal() >= 1, qPrintable(label));
            const QColor ring = edge->property("color").value<QColor>();
            QVERIFY2(contrastRatio(over(ring, theme->panel()), theme->panel()) >= 3, qPrintable(label));
            // Assistive technology hears which tab needs attention, and why.
            QAccessibleInterface *access = QAccessible::queryAccessibleInterface(mark);
            QVERIFY2(access, qPrintable(label));
            const QString description = access->text(QAccessible::Description);
            QVERIFY2(description.contains(tabAt(i)->property("text").toString())
                         && description.contains(tabAt(i)->property("status").toString()),
                     qPrintable(label + u' ' + description));
        }
    }
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join("; ")));
}

void ParchTests::everyControlHasAccessibleNameAndFocus()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    // Several tabs, so that the tabs, their close buttons and the new tab button are checked too.
    for (const char *title : {"Second", "Third"}) {
        Document *document = shell.backend->store()->newDraft();
        document->buffer()->replace({0, 0}, QString::fromLatin1(title));
        shell.context()->tabs()->openDocument(document);
    }
    QTRY_VERIFY(shell.item(QStringLiteral("tabStrip"))->isVisible());
    for (int width : {360, 1440}) {
        resizeWindow(window, width);
        // The tab strip brings the current tab into view once its tabs have their widths.
        QTest::qWait(20);
        const QStringList unnamed = unnamedAccessibleItems(window);
        QVERIFY2(unnamed.isEmpty(), qPrintable(unnamed.join(", ")));
        QStringList missed;
        QVERIFY2(tabKeyReachesEveryControl(window, &missed), qPrintable(missed.join(", ")));
        int tabsReached = 0;
        for (QQuickItem *item : visibleItems(window))
            if (item->objectName() == QStringLiteral("tab") || item->objectName() == QStringLiteral("newTabButton"))
                ++tabsReached;
        QVERIFY(tabsReached >= 3);
    }
    // The library drawer, open over the editor in a narrow window, is named too.
    resizeWindow(window, 360);
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QVERIFY(drawer);
    shell.item(QStringLiteral("editor"))->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    const QStringList unnamed = unnamedAccessibleItems(window);
    QVERIFY2(unnamed.isEmpty(), qPrintable(unnamed.join(", ")));
}

void ParchTests::shortcutsAlwaysCarryAModifier()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    const QList<QObject *> shortcuts = shortcutsIn(shell.window.get());
    QVERIFY(shortcuts.size() >= 15);
    QList<QKeySequence> all;
    for (QObject *shortcut : shortcuts) {
        const QList<QKeySequence> sequences = shortcutSequences(shortcut);
        QVERIFY2(!sequences.isEmpty(), qPrintable(describe(shortcut)));
        QCOMPARE(shortcut->property("context").toInt(), int(Qt::ApplicationShortcut));
        for (const QKeySequence &sequence : sequences) {
            QVERIFY2(isModifiedOrFunctionKey(sequence), qPrintable(sequence.toString()));
            all << sequence;
        }
    }
    for (const char *wanted : {"Ctrl+N", "Ctrl+Shift+N", "Ctrl+O", "Ctrl+S", "Ctrl+Shift+S", "Ctrl+W", "Ctrl+Z",
                               "Ctrl+Shift+Z", "Ctrl+Tab", "Ctrl+Shift+Tab", "Ctrl+PgDown", "Ctrl+PgUp", "Ctrl+=",
                               "Ctrl+-", "Ctrl+0", "Ctrl+B", "Ctrl+Q"})
        QVERIFY2(all.contains(QKeySequence(QString::fromLatin1(wanted))), wanted);
    // No key is claimed twice, which would leave Qt to pick neither.
    QCOMPARE(QSet<QKeySequence>(all.begin(), all.end()).size(), all.size());
}

void ParchTests::undoAndRedoKeysActOnceWhereverFocusIs()
{
    // The editor takes Ctrl+Z and Ctrl+Shift+Z itself while it has focus, and
    // the window's shortcuts take them everywhere else: one step either way.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    SourceBuffer *buffer = shell.document()->buffer();
    for (const QString &step : {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})
        buffer->replace({int(buffer->length()), 0}, step);
    QCOMPARE(buffer->text(), QStringLiteral("abc"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *menuButton = shell.item(QStringLiteral("menuButton"));
    QVERIFY(menuButton);
    QObject *undo = shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Z")));
    QObject *redo = shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
    QVERIFY(undo && redo);
    QSignalSpy undone(undo, SIGNAL(activated()));
    QSignalSpy redone(redo, SIGNAL(activated()));

    editor->forceActiveFocus();
    QCOMPARE(window->activeFocusItem(), editor);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("ab"));
    QCOMPARE(undone.count(), 0);

    menuButton->forceActiveFocus(Qt::TabFocusReason);
    QCOMPARE(window->activeFocusItem(), menuButton);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("a"));
    QCOMPARE(undone.count(), 1);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(buffer->text(), QStringLiteral("ab"));
    QCOMPARE(redone.count(), 1);

    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(buffer->text(), QStringLiteral("abc"));
    QCOMPARE(redone.count(), 1);

    // Ctrl+Y redoes too, once a press, from the editor and from elsewhere.
    QObject *redoAlso = shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Y")));
    QVERIFY(redoAlso);
    QSignalSpy redoneAlso(redoAlso, SIGNAL(activated()));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("a"));
    QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("ab"));
    menuButton->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("abc"));
    QCOMPARE(redoneAlso.count(), 1);
    editor->forceActiveFocus();

    // With focus on nothing in particular the window's shortcut still undoes once.
    editor->setFocus(false);
    QVERIFY(window->activeFocusItem() != editor);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("ab"));
    QCOMPARE(undone.count(), 2);
}

void ParchTests::windowShortcutsReachPastTheEditor()
{
    // Every other shortcut reaches the window while the editor has focus, and
    // none of them puts anything into the text.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QSignalSpy quits(shell.backend.get(), &Backend::quitRequested);
    int tried = 0;
    for (QObject *shortcut : shortcutsIn(window)) {
        for (const QKeySequence &sequence : shortcutSequences(shortcut)) {
            if (sequence == QKeySequence(QStringLiteral("Ctrl+Z")) || sequence == QKeySequence(QStringLiteral("Ctrl+Shift+Z"))
                || sequence == QKeySequence(QStringLiteral("Ctrl+Y")))
                continue;
            // Ctrl+Shift+N opens a window that comes to the front, and only
            // the window in front answers keys.
            activate(window);
            editor->forceActiveFocus();
            QCOMPARE(window->activeFocusItem(), editor);
            QSignalSpy activated(shortcut, SIGNAL(activated()));
            QTest::keySequence(window, sequence);
            QVERIFY2(activated.count() == 1, qPrintable(sequence.toString()));
            QVERIFY2(shell.document()->buffer()->text().isEmpty(), qPrintable(sequence.toString()));
            ++tried;
        }
    }
    QVERIFY(tried >= 13);
    // A keyboard sends Ctrl+Shift+Tab as Backtab.
    QSignalSpy previous(shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Shift+Tab"))), SIGNAL(activated()));
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(previous.count(), 1);
    QVERIFY(shell.document()->buffer()->text().isEmpty());
    // Ctrl+Q asked to quit once; quitting is main()'s to do, so the test goes on.
    QCOMPARE(quits.count(), 1);
}

void ParchTests::plainKeysTypeInTheWindow()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    std::vector<std::unique_ptr<QSignalSpy>> spies;
    for (QObject *shortcut : shortcutsIn(window)) spies.push_back(std::make_unique<QSignalSpy>(shortcut, SIGNAL(activated())));
    QSignalSpy quits(shell.backend.get(), &Backend::quitRequested);
    QCOMPARE(window->activeFocusItem(), shell.item(QStringLiteral("editor")));
    typeText(window, QStringLiteral("q ?Q"));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("q ?Q"));
    for (const auto &spy : spies) QCOMPARE(spy->count(), 0);
    QCOMPARE(quits.count(), 0);
}

void ParchTests::themeSwitchKeepsCursorSelectionAndUndo()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QCOMPARE(window->color(), QColor("#F8F1E3"));
    typeText(window, QStringLiteral("hello world"));
    selectRange(editor, 2, 7);
    replaceTheme(shell.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    QTRY_COMPARE(window->color(), QColor("#1a1b26"));
    QCOMPARE(editor->property("color").value<QColor>(), QColor("#a9b1d6"));
    QCOMPARE(editor->property("selectionStart").toInt(), 2);
    QCOMPARE(editor->property("selectionEnd").toInt(), 7);
    QCOMPARE(window->activeFocusItem(), editor);
    EditorBinding *binding = shell.binding();
    QVERIFY(binding);
    binding->undo();
    QCOMPARE(shell.document()->buffer()->text(), QString());
}

void ParchTests::fontZoomReflowsWithinWindow()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    Settings *settings = shell.settings();
    QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    const int before = settings->fontPixelSize();
    settings->zoom(6);
    QCOMPARE(settings->fontPixelSize(), before + 6);
    for (int width : {360, 640, 960, 1440}) {
        resizeWindow(window, width);
        QTest::qWait(50);
        QTRY_COMPARE(editor->property("font").value<QFont>().pixelSize(), before + 6);
        QStringList problems;
        QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(QStringLiteral("%1: %2").arg(width).arg(problems.join("; "))));
        QTRY_COMPARE(qRound(editor->width()),
                     qRound(qMin(pane->width() - 48, 72 * pane->property("characterWidth").toReal())));
    }
    // The largest text still fits the narrowest window.
    settings->zoom(100);
    QCOMPARE(settings->fontPixelSize(), Settings::MaxFontPixelSize);
    resizeWindow(window, 360);
    QTest::qWait(50);
    QTRY_COMPARE(editor->property("font").value<QFont>().pixelSize(), Settings::MaxFontPixelSize);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY(editor->width() >= 300);
}

void ParchTests::fontSizeFollowsThemeUntilZoomed()
{
    ShellFixture shell({.shell = "[font]\nbase-size = 14\n"});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    Settings *settings = shell.settings();
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    const auto editorSize = [&] { return editor->property("font").value<QFont>().pixelSize(); };
    QCOMPARE(shell.theme->fontSize(), 14);
    QCOMPARE(settings->fontPixelSize(), 18);
    QVERIFY(!settings->zoomed());
    QTRY_COMPARE(editorSize(), 18);
    // An unzoomed editor follows the shell's text size as it changes.
    writeFile(shell.themeFolder.filePath("theme/shell.toml"), "[font]\nbase-size = 16\n");
    QTRY_COMPARE(shell.theme->fontSize(), 16);
    QCOMPARE(settings->fontPixelSize(), 20);
    QTRY_COMPARE(editorSize(), 20);
    // A zoomed one keeps its size.
    QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(settings->fontPixelSize(), 18);
    QVERIFY(settings->zoomed());
    writeFile(shell.themeFolder.filePath("theme/shell.toml"), "[font]\nbase-size = 10\n");
    QTRY_COMPARE(shell.theme->fontSize(), 10);
    QCOMPARE(settings->fontPixelSize(), 18);
    QTRY_COMPARE(editorSize(), 18);
    // The zoom outlasts a restart; Ctrl+0 returns to following the theme.
    QCOMPARE(Settings(shell.paths().settingsFile()).fontPixelSize(), 18);
    QTest::keyClick(window, Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(settings->fontPixelSize(), 14);
    QVERIFY(!settings->zoomed());
    QTRY_COMPARE(editorSize(), 14);
    QVERIFY(!Settings(shell.paths().settingsFile()).zoomed());
    QTest::keyClick(window, Qt::Key_Equal, Qt::ControlModifier);
    QCOMPARE(settings->fontPixelSize(), 15);
}

void ParchTests::zoomPastALimitKeepsFollowingTheTheme()
{
    // The theme's text size makes the editor's the smallest there is.
    ShellFixture shell({.shell = "[font]\nbase-size = 6\n"});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    Settings *settings = shell.settings();
    QCOMPARE(settings->fontPixelSize(), Settings::MinFontPixelSize);
    QTest::keyClick(window, Qt::Key_Minus, Qt::ControlModifier);
    QCOMPARE(settings->fontPixelSize(), Settings::MinFontPixelSize);
    QVERIFY(!settings->zoomed());
    QVERIFY(!readFile(shell.paths().settingsFile()).contains("fontPixelSize"));
    // It still follows the theme.
    writeFile(shell.themeFolder.filePath("theme/shell.toml"), "[font]\nbase-size = 12\n");
    QTRY_COMPARE(settings->fontPixelSize(), 16);
    // So at the largest, a step up changes nothing either.
    Settings largest(shell.paths().config + QStringLiteral("/largest.json"));
    largest.setThemeFontSize(Settings::MaxFontPixelSize);
    largest.zoom(1);
    QCOMPARE(largest.fontPixelSize(), Settings::MaxFontPixelSize);
    QVERIFY(!largest.zoomed());
    QVERIFY(!readFile(shell.paths().config + QStringLiteral("/largest.json")).contains("fontPixelSize"));
}

void ParchTests::settingsSurviveRestartAndDamage()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString file = AppPaths::under(root.path()).settingsFile();
    const auto verifyDefaults = [](const Settings &settings) {
        QCOMPARE(settings.fontPixelSize(), settings.themeFontSize() + 4);
        QVERIFY(!settings.zoomed());
        QCOMPARE(settings.lineHeight(), 1.5);
        QCOMPARE(settings.maxLineLength(), 72);
        QCOMPARE(settings.editorFontFamily(), QStringLiteral("monospace"));
        QCOMPARE(settings.freshDraftOnLaunch(), false);
        QCOMPARE(settings.libraryWidth(), 260);
    };
    {
        Settings settings(file);
        verifyDefaults(settings);
        QCOMPARE(settings.fontPixelSize(), 16);
        // Nothing is written until something changes.
        QVERIFY(!QFile::exists(file));
        settings.zoom(3);
        QCOMPARE(settings.fontPixelSize(), 19);
        QVERIFY(QFile::exists(file));
    }
    {
        Settings settings(file);
        QCOMPARE(settings.fontPixelSize(), 19);
        QVERIFY(settings.zoomed());
        settings.zoom(100);
        QCOMPARE(settings.fontPixelSize(), 40);
        // A zoom past the limit writes nothing: a write would replace the file with another.
        const auto inode = [&] {
            struct stat info {};
            return ::stat(QFile::encodeName(file).constData(), &info) == 0 ? info.st_ino : ino_t(0);
        };
        const ino_t before = inode();
        QVERIFY(before != 0);
        settings.zoom(1);
        QCOMPARE(inode(), before);
        settings.zoom(-1);
        QVERIFY(inode() != before);
        settings.zoom(1);
        settings.zoom(-100);
        QCOMPARE(settings.fontPixelSize(), 10);
        settings.resetZoom();
        verifyDefaults(settings);
    }
    QVERIFY(!Settings(file).zoomed());

    // Values set by hand, and keys this version does not know, survive a change.
    writeFile(file, R"({"maxLineLength": 80, "lineHeight": 1.8, "libraryWidth": 300, "freshDraftOnLaunch": true,
                       "editorFontFamily": "serif", "later": "kept"})");
    {
        Settings settings(file);
        QCOMPARE(settings.maxLineLength(), 80);
        QCOMPARE(settings.lineHeight(), 1.8);
        QCOMPARE(settings.libraryWidth(), 300);
        QCOMPARE(settings.freshDraftOnLaunch(), true);
        QCOMPARE(settings.editorFontFamily(), QStringLiteral("serif"));
        settings.zoom(1);
    }
    const QJsonObject stored = QJsonDocument::fromJson(readFile(file)).object();
    QCOMPARE(stored.value("later").toString(), QStringLiteral("kept"));
    QCOMPARE(stored.value("maxLineLength").toInt(), 80);
    QCOMPARE(stored.value("fontPixelSize").toInt(), 17);

    // A damaged file, or values out of range, give the defaults.
    for (const QByteArray &damaged : {QByteArray("{not json"), QByteArray("[1, 2]"), QByteArray("\xff\xfe"),
                                       QByteArray(R"({"fontPixelSize": 400, "lineHeight": "tall", "maxLineLength": -3,
                                                      "libraryWidth": 1e9, "editorFontFamily": "", "freshDraftOnLaunch": 1})")}) {
        writeFile(file, damaged);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("parch: .*settings")));
        Settings settings(file);
        verifyDefaults(settings);
    }
    // A damaged file is never written over: the first change moves it aside,
    // beside any copy moved aside before, and then writes the settings anew.
    writeFile(file + QStringLiteral(".bad"), "older damage");
    writeFile(file, "{\"maxLineLength\": 80, oops");
    {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("parch: .*settings")));
        Settings settings(file);
        settings.zoom(2);
    }
    QCOMPARE(readFile(file + QStringLiteral(".bad")), QByteArray("older damage"));
    QCOMPARE(readFile(file + QStringLiteral(".bad.2")), QByteArray("{\"maxLineLength\": 80, oops"));
    QCOMPARE(QJsonDocument::fromJson(readFile(file)).object().value("fontPixelSize").toInt(), 18);

    // A settings folder that cannot be written still zooms, for this session.
    QTemporaryDir blocked;
    const AppPaths paths = AppPaths::under(blocked.path());
    writeFile(paths.config, "in the way");
    Settings settings(paths.settingsFile());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("parch: .*settings")));
    settings.zoom(2);
    QCOMPARE(settings.fontPixelSize(), 18);

    // A damaged file never costs the window.
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("parch: .*settings")));
    ShellFixture shell({.settings = "{not json"});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QCOMPARE(shell.settings()->maxLineLength(), 72);
}

void ParchTests::libraryDocksOrOpensAsDrawer()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *library = shell.item(QStringLiteral("library"));
    QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *menuButton = shell.item(QStringLiteral("menuButton"));
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QVERIFY(library && menuButton && drawer);

    // Docked: the key and the button hide and show it beside the editor.
    resizeWindow(window, 1440);
    QVERIFY(library->property("docked").toBool());
    QVERIFY(library->isVisible());
    const qreal beside = pane->width();
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(!library->isVisible());
    QVERIFY(pane->width() > beside);
    QCOMPARE(window->activeFocusItem(), editor);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(menuButton));
    QTRY_VERIFY(library->isVisible());
    QCOMPARE(pane->width(), beside);

    // Narrow: a drawer over the editor, from the same button or key, that leaves the editor its width.
    resizeWindow(window, 360);
    QVERIFY(!library->property("docked").toBool());
    QVERIFY(!library->isVisible());
    QVERIFY(!drawer->property("opened").toBool());
    const qreal alone = pane->width();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(menuButton));
    QTRY_VERIFY(drawer->property("opened").toBool());
    QQuickItem *heading = shell.item(QStringLiteral("libraryHeading"));
    QVERIFY(heading && heading->isVisible());
    QCOMPARE(pane->width(), alone);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(!drawer->property("visible").toBool());

    // Widening the window with the drawer open docks the library instead.
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    resizeWindow(window, 1440);
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_VERIFY(library->isVisible());
}

void ParchTests::theTextIsNamedAsItsTab()
{
    // Launched with a file that is not there yet, the text is named as its
    // tab and the window are: by that file's name, not as an untitled draft.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString missing = QFileInfo(files.path()).canonicalFilePath() + QStringLiteral("/Not yet.md");
    ShellFixture shell({.files = {missing}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QCOMPARE(shell.context()->tabs()->activeTitle(), QStringLiteral("Not yet.md"));
    QCOMPARE(window->title(), QStringLiteral("Not yet.md — parch"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QVERIFY(editor);
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(editor);
    QVERIFY(access);
    QCOMPARE(access->text(QAccessible::Name), QStringLiteral("Not yet.md"));
}

void ParchTests::headerNamesTheDocumentAndItsPlace()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *place = shell.item(QStringLiteral("place"));
    QQuickItem *title = shell.item(QStringLiteral("documentTitle"));
    QVERIFY(place && title);
    QCOMPARE(place->property("text").toString(), QStringLiteral("Drafts"));
    QCOMPARE(title->property("text").toString(), QStringLiteral("Untitled"));
    QCOMPARE(window->title(), QStringLiteral("Untitled — parch"));
    typeText(window, QStringLiteral("# Notes on rivers"));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Notes on rivers"));
    QCOMPARE(window->title(), QStringLiteral("Notes on rivers — parch"));
    // A long name is cut short on screen and given whole to assistive technology.
    typeText(window, QStringLiteral(" and the long meandering ways of water"));
    resizeWindow(window, 360);
    QTRY_VERIFY(title->property("truncated").toBool());
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(title);
    QVERIFY(access);
    QCOMPARE(access->text(QAccessible::Name), shell.document()->title());
    // A named file's place is its folder, its parts joined by " / ": from
    // the library folder that holds it, by that folder's own name, the
    // nearest when several do; otherwise from the home folder, written as ~,
    // or from the root.
    const QString home = QDir::homePath();
    const auto label = [&](const QString &path, const QStringList &library = {}) {
        return shell.backend->folderLabel(path, library);
    };
    QCOMPARE(label(home + QStringLiteral("/Writing/Essays/a.md")), QStringLiteral("~ / Writing / Essays"));
    QCOMPARE(label(home + QStringLiteral("/a.md")), QStringLiteral("~"));
    QCOMPARE(label(QStringLiteral("/srv/notes/a.md")), QStringLiteral("/ srv / notes"));
    QCOMPARE(label(QStringLiteral("/a.md")), QStringLiteral("/"));
    const QStringList library{home + QStringLiteral("/Writing"), QStringLiteral("/srv/notes"),
                              QStringLiteral("/srv/notes/old")};
    QCOMPARE(label(home + QStringLiteral("/Writing/Essays/a.md"), library), QStringLiteral("Writing / Essays"));
    QCOMPARE(label(home + QStringLiteral("/Writing/a.md"), library), QStringLiteral("Writing"));
    QCOMPARE(label(QStringLiteral("/srv/notes/old/2019/a.md"), library), QStringLiteral("old / 2019"));
    QCOMPARE(label(home + QStringLiteral("/Writings/a.md"), library), QStringLiteral("~ / Writings"));
    QCOMPARE(label(home + QStringLiteral("/Notes/a.md"), {home}), QStringLiteral("~ / Notes"));
    QCOMPARE(label(QString(), library), QStringLiteral("Drafts"));
    QCOMPARE(label(QStringLiteral("a.md")), QString());

    // The header follows the library: a document in one of its folders is
    // placed from that folder, and the place follows the folder's joining.
    QTemporaryDir writing;
    QVERIFY(writing.isValid());
    const QString writingRoot = QFileInfo(writing.path()).canonicalFilePath();
    const QString essays = writingRoot + QStringLiteral("/Writing/Essays");
    writeFile(essays + QStringLiteral("/river.md"), "river\n");
    shell.backend->openFiles({essays + QStringLiteral("/river.md")});
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("river.md"));
    QVERIFY(place->property("text").toString().endsWith(QStringLiteral(" / Writing / Essays")));
    shell.context()->library()->addFolder(writingRoot + QStringLiteral("/Writing"));
    QTRY_COMPARE(place->property("text").toString(), QStringLiteral("Writing / Essays"));

    // Narrow, a place cut short from its start still ends at the slash, with
    // no gap before it but the slash's own space.
    const QString deep = essays + QStringLiteral("/Seasons of the northern coast/Drafts kept for later/b.md");
    writeFile(deep, "b\n");
    shell.backend->openFiles({deep});
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("b.md"));
    resizeWindow(window, 360);
    QTRY_VERIFY(place->property("truncated").toBool());
    QVERIFY(place->isVisible());
    QTest::qWait(50);
    const QImage image = window->grabWindow();
    const QRect placed = place->mapRectToScene(QRectF(0, 0, place->width(), place->height())).toAlignedRect();
    const QColor panel = shell.theme->panel();
    int textRight = placed.left();
    for (int x = placed.right(); x >= placed.left() && textRight == placed.left(); --x)
        for (int y = placed.top(); y <= placed.bottom(); ++y)
            if (colourDistance(image.pixelColor(x, y), panel) > 60) {
                textRight = x + 1;
                break;
            }
    QVERIFY2(placed.right() + 1 - textRight <= 2,
             qPrintable(QStringLiteral("%1 px between the place and its slash").arg(placed.right() + 1 - textRight)));
}

void ParchTests::statusLineNamesEverySaveState()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *state = shell.item(QStringLiteral("saveState"));
    QVERIFY(state);
    Document *document = shell.document();
    const auto shown = [&] { return state->property("text").toString(); };
    QQuickItem *light = shell.item(QStringLiteral("saveLight"));
    QVERIFY(light);
    const auto lightColour = [&] { return light->property("color").value<QColor>().rgba(); };
    const auto textColour = [&] { return state->property("color").value<QColor>().rgba(); };
    // A new draft holds nothing, so nothing is saved: it says so quietly.
    QCOMPARE(shown(), QStringLiteral("Empty draft"));
    QCOMPARE(lightColour(), shell.theme->dim().rgba());
    QCOMPARE(textColour(), shell.theme->dim().rgba());
    typeText(window, QStringLiteral("words"));
    QCOMPARE(document->state(), Document::State::Modified);
    QCOMPARE(shown(), QStringLiteral("Edited"));
    // A draft is saved to its own file as autosave saves it; Save would ask for a name.
    {
        PoolBlock block;
        shell.backend->store()->save(document);
        QCOMPARE(document->state(), Document::State::Saving);
        QCOMPARE(shown(), QStringLiteral("Saving…"));
    }
    // Saved, once there is text and it is in the draft's file.
    QTRY_COMPARE(shown(), QStringLiteral("Saved"));
    QCOMPARE(lightColour(), shell.theme->good().rgba());
    QCOMPARE(textColour(), shell.theme->foreground().rgba());
    // A draft whose folder cannot be written to cannot be saved, and says so.
    if (::geteuid() != 0) {
        const QString folder = shell.paths().drafts() + u'/' + document->id();
        const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        typeText(window, QStringLiteral(" more"));
        shell.backend->store()->save(document);
        QTRY_COMPARE(document->state(), Document::State::SaveFailed);
        QCOMPARE(shown(), QStringLiteral("Save failed"));
        QVERIFY(QFile::setPermissions(folder, writable));
        shell.backend->retrySave();
        QTRY_COMPARE(shown(), QStringLiteral("Saved"));
        // Only a clean empty draft is said quietly: emptied, it is edited,
        // and a save of it that fails says so as plainly as any other.
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        document->buffer()->replace({0, document->buffer()->length()}, QString());
        QCOMPARE(shown(), QStringLiteral("Edited"));
        QCOMPARE(textColour(), shell.theme->foreground().rgba());
        shell.backend->store()->save(document);
        QTRY_COMPARE(document->state(), Document::State::SaveFailed);
        QCOMPARE(shown(), QStringLiteral("Save failed"));
        QCOMPARE(textColour(), shell.theme->foreground().rgba());
        QCOMPARE(lightColour(), shell.theme->danger().rgba());
        QVERIFY(QFile::setPermissions(folder, writable));
        shell.backend->retrySave();
        QTRY_COMPARE(shown(), QStringLiteral("Empty draft"));
        QCOMPARE(textColour(), shell.theme->dim().rgba());
    }
    // A file that went has words of its own, with the conflict's mark.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString gone = QFileInfo(files.path()).canonicalFilePath() + QStringLiteral("/gone.md");
    writeFile(gone, "here\n");
    shell.backend->openFiles({gone});
    shell.document()->buffer()->replace({0, 0}, QStringLiteral("mine "));
    QVERIFY(QFile::remove(gone));
    QTRY_COMPARE_WITH_TIMEOUT(shell.document()->state(), Document::State::Conflict, 3000);
    QVERIFY(shell.document()->fileMissing());
    QCOMPARE(shown(), QStringLiteral("File missing"));
    QCOMPARE(lightColour(), shell.theme->danger().rgba());
    QCOMPARE(shell.context()->tabs()->activeStatus(), QStringLiteral("File missing"));
    // Back again, it is a change on disk.
    writeFile(gone, "back\n");
    QTRY_COMPARE_WITH_TIMEOUT(shown(), QStringLiteral("Changed on disk"), 3000);

    // Conflict and read-only come only to named files; the status text is the document's own.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    writeFile(root.filePath("broken.md"), QByteArray("caf\xe9\n"));
    Document *broken = store.open(root.filePath("broken.md"));
    QVERIFY(broken);
    QCOMPARE(broken->state(), Document::State::ReadOnly);
    QCOMPARE(broken->statusText(), QStringLiteral("Read-only"));
    writeFile(root.filePath("a.md"), "base\n");
    Document *named = store.open(root.filePath("a.md"));
    QVERIFY(named);
    QCOMPARE(named->statusText(), QStringLiteral("Saved"));
    named->buffer()->replace({0, 0}, QStringLiteral("mine "));
    QCOMPARE(named->statusText(), QStringLiteral("Edited"));
    writeFile(root.filePath("a.md"), "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(named->state(), Document::State::Conflict, 3000);
    QCOMPARE(named->statusText(), QStringLiteral("Changed on disk"));
}

void ParchTests::quitKeepsEveryTextSafe()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    QSignalSpy quits(&backend, &Backend::quitRequested);
    backend.newDraft();
    Document *draft = backend.document();
    QVERIFY(draft);
    // A clean draft nobody wrote in leaves no file behind.
    backend.quit();
    QCOMPARE(quits.count(), 1);
    QVERIFY(!QFileInfo::exists(paths.drafts() + u'/' + draft->id()));

    // Unsaved text is in its file before the quit is announced.
    draft->buffer()->replace({0, 0}, QStringLiteral("unsaved words"));
    backend.quit();
    QCOMPARE(quits.count(), 2);
    QCOMPARE(readFile(paths.drafts() + u'/' + draft->id() + QStringLiteral("/draft.md")), QByteArray("unsaved words"));
    QCOMPARE(draft->state(), Document::State::Clean);

    // Text that neither its file nor recovery can hold keeps parch open, and it says why.
    backend.newDraft();
    Document *second = backend.document();
    QVERIFY(second != draft);
    writeFile(paths.drafts() + u'/' + second->id(), "in the way");
    // The recovery folder, which the first draft's save made, gives way to a file.
    QVERIFY(QDir(paths.recovery()).removeRecursively());
    writeFile(paths.recovery(), "in the way");
    second->buffer()->replace({0, 0}, QStringLiteral("only copy"));
    backend.quit();
    QCOMPARE(quits.count(), 2);
    QVERIFY(!backend.problem().isEmpty());
    QVERIFY(backend.problemDetail().contains(second->title()));
    QCOMPARE(second->buffer()->text(), QStringLiteral("only copy"));
    // Once recovery holds the text again the refusal goes by itself, though
    // the file still cannot be written, and the next quit goes ahead.
    QSignalSpy problems(&backend, &Backend::problemChanged);
    QVERIFY(QFile::remove(paths.recovery()));
    second->buffer()->replace({second->buffer()->length(), 0}, QStringLiteral("."));
    QTRY_VERIFY(backend.problem().isEmpty());
    QVERIFY(backend.problemDetail().isEmpty());
    QCOMPARE(problems.count(), 1);
    QCOMPARE(second->state(), Document::State::SaveFailed);
    backend.quit();
    QCOMPARE(quits.count(), 3);
    QVERIFY(backend.problem().isEmpty());
    bool kept = false;
    for (const Recovery::Entry &entry : Recovery(paths).pending())
        kept = kept || (entry.documentId == second->id() && entry.text == QStringLiteral("only copy."));
    QVERIFY(kept);
}

void ParchTests::endingAtOnceKeepsEveryTextWithinItsTime()
{
    // As at the end of the session: nothing can be refused, so every text
    // its file lacks goes to recovery, the session is written, and saves get
    // only so long to land.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    const auto inRecovery = [&](const QString &text) {
        for (const Recovery::Entry &entry : Recovery(paths).pending())
            if (entry.text == text) return true;
        return false;
    };
    {
        Backend backend(paths);
        backend.start({file});
        backend.document()->buffer()->replace({0, 0}, QStringLiteral("edited "));
        backend.newDraft();
        Document *blocked = backend.document();
        writeFile(paths.drafts() + u'/' + blocked->id(), "in the way");
        blocked->buffer()->replace({0, 0}, QStringLiteral("only copy"));
        PoolBlock block;
        QElapsedTimer clock;
        clock.start();
        // No save can land while the pool is held: the end does not wait past its time.
        QVERIFY(!backend.end(200));
        QVERIFY2(clock.elapsed() < 2000, qPrintable(QString::number(clock.elapsed())));
        QVERIFY(inRecovery(QStringLiteral("edited alpha\n")));
        QVERIFY(inRecovery(QStringLiteral("only copy")));
        QVERIFY(QFileInfo::exists(paths.sessionFile()));
        QCOMPARE(readFile(file), QByteArray("alpha\n"));
        block.release();
        backend.store()->waitForSaves();
    }
    // With time enough the file is saved and recovery keeps only what it could not take.
    {
        Backend backend(paths);
        backend.start({});
        TabModel *tabs = backend.windows().first()->tabs();
        Document *named = nullptr;
        for (int i = 0; i < tabs->rowCount(); ++i) {
            tabs->setActiveIndex(i);
            if (backend.document() && !backend.document()->isDraft()) named = backend.document();
        }
        QVERIFY(named);
        named->buffer()->replace({0, 0}, QStringLiteral("again "));
        QVERIFY(backend.end(Backend::EndWaitMs));
        QCOMPARE(readFile(file), QByteArray("again edited alpha\n"));
        QVERIFY(!inRecovery(QStringLiteral("again edited alpha\n")));
        QVERIFY(inRecovery(QStringLiteral("only copy")));
    }
}

void ParchTests::noColourLiteralsInQml()
{
    QFile qrc(QStringLiteral(PARCH_SOURCE_DIR "/src/resources.qrc"));
    QVERIFY(qrc.open(QIODevice::ReadOnly));
    static const QRegularExpression entry(QStringLiteral("<file>([^<]+)</file>"));
    QStringList scanned, literals;
    for (auto match = entry.globalMatch(QString::fromUtf8(qrc.readAll())); match.hasNext();) {
        const QString name = match.next().captured(1).trimmed();
        if (!name.endsWith(QStringLiteral(".qml")) && !name.endsWith(QStringLiteral(".js"))) continue;
        QFile file(u":/" + name);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(name));
        literals += colourLiterals(QString::fromUtf8(file.readAll()), name);
        scanned << name;
    }
    for (const char *wanted : {"Main.qml", "EditorPane.qml", "HeaderBar.qml", "WindowMenu.qml", "TabStrip.qml",
                               "StatusLine.qml", "ThemedButton.qml", "Icon.qml", "icons.js", "LibraryPanel.qml",
                               "NoticeBar.qml", "ThemedDialog.qml", "ConflictDialog.qml", "HistoryDialog.qml",
                               "CloseConfirmation.qml"})
        QVERIFY2(scanned.contains(QString::fromLatin1(wanted)), wanted);
    QVERIFY2(literals.isEmpty(), qPrintable(literals.join(u'\n')));
}


void ParchTests::libraryDrawerShowsItsEdgeAndCorners()
{
    // Open over the editor, the library draws a fine edge toward the text and
    // rounds the corners on that side, in a light and a dark theme.
    // Hyprland rounds windows by 8 pixels and does not animate, so that a
    // drawer is in place as soon as it opens.
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 8);
    hyprland.setInt(QStringLiteral("general:border_size"), 1);
    hyprland.setInt(QStringLiteral("animations:enabled"), 0);
    ShellFixture shell({.hyprland = sockets.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QTRY_COMPARE_WITH_TIMEOUT(shell.theme->radius(), 8, 6000);
    QTRY_VERIFY_WITH_TIMEOUT(shell.theme->reduceMotion(), 6000);
    resizeWindow(window, 360);
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QVERIFY(drawer);
    for (const bool dark : {false, true}) {
        if (dark) {
            replaceTheme(shell.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
            QTRY_COMPARE(window->color(), QColor("#1a1b26"));
        }
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(drawer->property("opened").toBool());
        QTest::qWait(50);
        const int right = qCeil(drawer->property("width").toReal()) - 1;
        const int top = qRound(drawer->property("y").toReal());
        const int middle = top + qRound(drawer->property("height").toReal() / 2);
        // Whatever the library holds stops short of that edge, so nothing in it can cover the edge.
        auto *content = drawer->property("contentItem").value<QQuickItem *>();
        QVERIFY(content);
        const QRectF held = content->mapRectToScene(QRectF(0, 0, content->width(), content->height()));
        QVERIFY2(held.right() <= right + 0.5, qPrintable(QStringLiteral("content reaches %1").arg(held.right())));
        const QImage image = window->grabWindow();
        const QColor edge = image.pixelColor(right, middle), inside = image.pixelColor(right / 2, middle);
        // A line is drawn where the drawer meets the editor.
        QVERIFY2(colourDistance(edge, inside) >= 40,
                 qPrintable(QStringLiteral("edge %1, inside %2").arg(edge.name(), inside.name())));
        // The corner by the header is rounded: there the editor shows through.
        const QColor corner = image.pixelColor(right, top + 1), beside = image.pixelColor(right + 4, top + 1);
        QVERIFY2(colourDistance(corner, beside) <= 12,
                 qPrintable(QStringLiteral("corner %1, beside %2").arg(corner.name(), beside.name())));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!drawer->property("visible").toBool());
    }
}

void ParchTests::quitRefusalReadsInFullAndClears()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    const AppPaths paths = shell.paths();
    const QString blocker = paths.drafts() + u'/' + shell.document()->id();
    // Neither the draft's folder nor recovery can be written.
    writeFile(blocker, "in the way");
    writeFile(paths.recovery(), "in the way");
    typeText(window, QStringLiteral("only copy"));
    QTest::keyClick(window, Qt::Key_Q, Qt::ControlModifier);
    QVERIFY(!shell.backend->problem().isEmpty());
    const QString detail = shell.backend->problemDetail();
    QVERIFY2(detail.contains(QStringLiteral("only copy")), qPrintable(detail));

    // In the narrowest window the refusal fits, takes focus from the keyboard,
    // and gives its whole reason on focus and to assistive technology.
    resizeWindow(window, 360);
    QQuickItem *refusal = shell.item(QStringLiteral("problem"));
    QVERIFY(refusal && refusal->isVisible());
    QVERIFY(refusal->activeFocusOnTab());
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(refusal);
    QVERIFY(access);
    QCOMPARE(access->text(QAccessible::Name), shell.backend->problem());
    QCOMPARE(access->text(QAccessible::Description), detail);
    QVERIFY2(tabKeyReachesEveryControl(window, &problems), qPrintable(problems.join("; ")));
    refusal->forceActiveFocus(Qt::TabFocusReason);
    QObject *tip = toolTipOf(refusal);
    QVERIFY(tip);
    QTRY_VERIFY(tip->property("visible").toBool());
    QCOMPARE(tip->property("text").toString(), detail);
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));

    // Once the text is safe again the refusal goes, before any new quit.
    QVERIFY(QFile::remove(blocker));
    shell.backend->retrySave();
    QTRY_VERIFY(shell.backend->problem().isEmpty());
    QVERIFY(!refusal->isVisible());
    QCOMPARE(shell.document()->state(), Document::State::Clean);
}

void ParchTests::keyboardFocusIsPlainToSee()
{
    // Focus that came from the keyboard stands out from the panel at 3:1 or
    // more in a light and a dark theme, and an icon button then says what
    // it is, as hovering would.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    for (const char *title : {"First", "Second"}) {
        Document *document = shell.backend->store()->newDraft();
        document->buffer()->replace({0, 0}, QString::fromLatin1(title));
        shell.context()->tabs()->openDocument(document);
    }
    QQuickItem *list = shell.item(QStringLiteral("tabs"));
    QTRY_VERIFY(list && list->isVisible());
    QQuickItem *firstTab = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, firstTab), Q_ARG(int, 0));
    QVERIFY(firstTab);
    for (const bool dark : {false, true}) {
        if (dark) {
            replaceTheme(shell.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
            QTRY_COMPARE(window->color(), QColor("#1a1b26"));
        }
        const QColor panel = shell.theme->panel();
        for (QQuickItem *control : {shell.item(QStringLiteral("menuButton")), shell.item(QStringLiteral("moreButton")),
                                    shell.item(QStringLiteral("newTabButton")), firstTab}) {
            QVERIFY(control);
            control->forceActiveFocus(Qt::TabFocusReason);
            QVERIFY(control->property("visualFocus").toBool());
            auto *background = control->property("background").value<QQuickItem *>();
            QObject *border = background ? background->property("border").value<QObject *>() : nullptr;
            QVERIFY(border && border->property("width").toReal() >= 1);
            const QColor ring = over(border->property("color").value<QColor>(), panel);
            QVERIFY2(contrastRatio(ring, panel) >= 3.0,
                     qPrintable(QStringLiteral("%1: %2 on %3 is %4:1").arg(describe(control), ring.name(), panel.name())
                                    .arg(contrastRatio(ring, panel))));
            if (control == firstTab) continue;
            QObject *tip = toolTipOf(control);
            QVERIFY(tip);
            QTRY_VERIFY2(tip->property("visible").toBool(), qPrintable(describe(control)));
            QVERIFY(tip->property("text").toString().startsWith(control->property("text").toString()));
        }
    }
}

void ParchTests::settingsFollowTheThemeFromTheStart()
{
    // The editor's size comes from the theme in C++, before any window
    // exists, so the first layout is at the right size and no window feeds it.
    QTemporaryDir root, themeFolder;
    QVERIFY(root.isValid() && themeFolder.isValid());
    writeTheme(themeFolder.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    writeFile(themeFolder.filePath(QStringLiteral("theme/shell.toml")), "[font]\nbase-size = 14\n");
    Theme theme(themeFolder.path());
    Settings settings(AppPaths::under(root.path()).settingsFile());
    QCOMPARE(settings.fontPixelSize(), 16);
    settings.followTheme(&theme);
    QCOMPARE(settings.fontPixelSize(), 18);
    writeFile(themeFolder.filePath(QStringLiteral("theme/shell.toml")), "[font]\nbase-size = 16\n");
    QTRY_COMPARE(settings.fontPixelSize(), 20);

    ShellFixture shell({.shell = "[font]\nbase-size = 14\n"});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QCOMPARE(shell.item(QStringLiteral("editor"))->property("font").value<QFont>().pixelSize(), 18);
    for (QObject *object : everyObjectIn(shell.window.get()))
        QVERIFY2(!object->inherits("QQmlBind"), "the window binds a value into the settings");
}

void ParchTests::keysShownAreTheShortcutsOwn()
{
    // Every key the window names, in its menu or its tooltips, is the text of
    // the shortcut that acts on it, so the two cannot disagree.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    for (const char *title : {"First", "Second"}) {
        Document *document = shell.backend->store()->newDraft();
        document->buffer()->replace({0, 0}, QString::fromLatin1(title));
        shell.context()->tabs()->openDocument(document);
    }
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(shell.item(QStringLiteral("moreButton"))));
    QTRY_VERIFY(shell.object(QStringLiteral("windowMenu"))->property("opened").toBool());
    QStringList shortcutTexts;
    for (QObject *shortcut : shortcutsIn(window)) shortcutTexts << shortcut->property("nativeText").toString();
    int named = 0;
    for (QObject *object : everyObjectIn(window)) {
        const QVariant keys = object->property("keys");
        if (keys.metaType().id() != QMetaType::QString || keys.toString().isEmpty()) continue;
        QVERIFY2(shortcutTexts.contains(keys.toString()), qPrintable(describe(object) + ": " + keys.toString()));
        ++named;
    }
    // The nine keyed menu entries, the library and menu buttons, the new tab
    // button and the current tab's close button.
    QVERIFY2(named >= 13, qPrintable(QString::number(named)));
}

void ParchTests::reducedMotionRemovesAnimation()
{
    {
        // With motion on the window does animate, so the check below has something to find.
        ShellFixture moving;
        QVERIFY2(moving.ok(), qPrintable(moving.error));
        int count = 0;
        QVERIFY(!allBehaviorDurationsAreZero(moving.window.get(), nullptr, &count));
        QVERIFY(count > 0);
    }
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    // Hyprland answers that its animations are off.
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("decoration:rounding"), 0);
    hyprland.setInt(QStringLiteral("general:border_size"), 0);
    hyprland.setInt(QStringLiteral("animations:enabled"), 0);
    ShellFixture still({.hyprland = sockets.path()});
    QVERIFY2(still.ok(), qPrintable(still.error));
    QTRY_VERIFY_WITH_TIMEOUT(still.theme->reduceMotion(), 6000);
    QStringList moving;
    int count = 0;
    QVERIFY2(allBehaviorDurationsAreZero(still.window.get(), &moving, &count), qPrintable(moving.join(", ")));
    QVERIFY(count > 0);
}

namespace {
QString canonical(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// A request as AppService frames one: a 32-bit big-endian length, then the bytes.
QByteArray frame(const QByteArray &payload, quint32 length)
{
    QByteArray framed(4, '\0');
    qToBigEndian(length, framed.data());
    return framed + payload;
}

QByteArray frame(const QByteArray &payload)
{
    return frame(payload, quint32(payload.size()));
}

// Sends raw bytes and reports whether the instance closed the connection
// within `timeout`, as it must for anything it will not take.
bool closedAfterSending(const QString &socketPath, const QByteArray &bytes, int timeout)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(1000)) return false;
    socket.write(bytes);
    socket.flush();
    QElapsedTimer clock;
    clock.start();
    while (socket.state() != QLocalSocket::UnconnectedState && clock.elapsed() < timeout)
        QTest::qWait(20);
    return socket.state() == QLocalSocket::UnconnectedState;
}

// Sends a request and returns the instance's answer, or nothing when it closed without one.
QJsonObject exchange(const QString &socketPath, const QByteArray &bytes)
{
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(1000)) return {};
    socket.write(bytes);
    socket.flush();
    QByteArray answer;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 3000) {
        QTest::qWait(10);
        answer += socket.readAll();
        if (answer.size() >= 4 && answer.size() >= 4 + qFromBigEndian<quint32>(answer.constData())) break;
        if (socket.state() == QLocalSocket::UnconnectedState) break;
    }
    if (answer.size() < 4) return {};
    return QJsonDocument::fromJson(answer.mid(4, qFromBigEndian<quint32>(answer.constData()))).object();
}

// A socket file with nobody listening, as a crash leaves one.
bool leaveStaleSocket(const QString &path)
{
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray name = QFile::encodeName(path);
    if (name.size() >= qsizetype(sizeof(address.sun_path))) return false;
    std::memcpy(address.sun_path, name.constData(), size_t(name.size()));
    const bool bound = ::bind(descriptor, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    ::close(descriptor);
    return bound && QFileInfo::exists(path);
}

bool sessionNames(const AppPaths &paths, const QString &path)
{
    for (const WindowState &window : Session::load(paths).windows)
        for (const ViewState &view : window.tabs)
            if (view.path == path) return true;
    return false;
}
} // namespace

void ParchTests::openingAlreadyOpenFileFocusesExistingTab()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    TabModel tabs(&store);
    const int first = tabs.openDocument(store.open(file));
    tabs.openDocument(store.newDraft());
    QCOMPARE(tabs.openDocument(store.open(file)), first);
    QCOMPARE(tabs.rowCount(), 2);
    QCOMPARE(tabs.activeIndex(), first);

    // What a tab shows comes from its document, by the roles QML reads.
    const QModelIndex index = tabs.index(first);
    Document *document = store.open(file);
    QCOMPARE(tabs.data(index, TabModel::TitleRole).toString(), QStringLiteral("a.md"));
    QCOMPARE(tabs.data(index, TabModel::PathRole).toString(), canonical(file));
    QCOMPARE(tabs.data(index, TabModel::StateRole).toString(), QStringLiteral("Saved"));
    QCOMPARE(tabs.data(index, TabModel::DocumentIdRole).toString(), document->id());
    QCOMPARE(tabs.data(index, TabModel::DocumentRole).value<Document *>(), document);
    const QHash<int, QByteArray> names = tabs.roleNames();
    QCOMPARE(names.value(TabModel::TitleRole), QByteArray("title"));
    QCOMPARE(names.value(TabModel::PathRole), QByteArray("path"));
    QCOMPARE(names.value(TabModel::StateRole), QByteArray("status"));
    QCOMPARE(names.value(TabModel::DocumentIdRole), QByteArray("documentId"));
    QCOMPARE(names.value(TabModel::DocumentRole), QByteArray("document"));
    QCOMPARE(tabs.activeDocument(), document);
    QCOMPARE(tabs.activeTitle(), QStringLiteral("a.md"));
    // An edit shows in the tab, in the words the status line uses.
    QSignalSpy changes(&tabs, &QAbstractItemModel::dataChanged);
    document->buffer()->replace({0, 0}, QStringLiteral("x"));
    QVERIFY(changes.count() >= 1);
    QCOMPARE(tabs.data(index, TabModel::StateRole).toString(), document->statusText());
    QCOMPARE(tabs.activeStatus(), QStringLiteral("Edited"));
}

void ParchTests::newViewSharesTextButNotCursor()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha beta\n");
    TabModel tabs(&store);
    const int first = tabs.openDocument(store.open(file));
    const QString id = store.open(file)->id();
    tabs.setViewState(first, {id, file, 0, 0, 0});
    const int second = tabs.newView(first);
    QCOMPARE(tabs.rowCount(), 2);
    QCOMPARE(second, first + 1);
    QCOMPARE(tabs.activeIndex(), second);
    tabs.setViewState(second, {id, file, 5, 5, 0});
    store.open(file)->buffer()->replace({0, 0}, "x");
    QCOMPARE(tabs.viewState(first).cursor, 0);
    // A view after the edit moves with the text; one at its start stays.
    QCOMPARE(tabs.viewState(second).cursor, 6);
    QCOMPARE(tabs.data(tabs.index(second), TabModel::DocumentRole).value<Document *>(), store.open(file));
    QCOMPARE(tabs.data(tabs.index(first), TabModel::DocumentRole).value<Document *>(), store.open(file));
    QCOMPARE(tabs.viewState(first).documentId, id);
    QCOMPARE(tabs.viewState(second).path, canonical(file));
    // A tab without a document has no second view.
    QCOMPARE(tabs.newView(tabs.openMissing(root.filePath(QStringLiteral("gone/x.md")))), -1);
}

void ParchTests::closingOneViewKeepsDocumentOpenElsewhere()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    TabModel tabs(&store);
    const int first = tabs.openDocument(store.open(file));
    const int second = tabs.newView(first);
    Document *document = store.open(file);
    QSignalSpy closed(&store, &DocumentStore::documentClosed);
    tabs.closeTab(second);
    QVERIFY(store.documents().contains(store.open(file)));
    QCOMPARE(tabs.rowCount(), 1);
    QCOMPARE(tabs.activeIndex(), first);
    QCOMPARE(tabs.activeDocument(), document);
    QCOMPARE(closed.count(), 0);
    // The last view going lets the clean document go.
    tabs.closeTab(first);
    QCOMPARE(tabs.rowCount(), 0);
    QCOMPARE(tabs.activeIndex(), -1);
    QCOMPARE(tabs.activeDocument(), nullptr);
    QCOMPARE(closed.count(), 1);

    // Closing the tab in front brings its neighbour: the one after it, or the
    // one before when it was the last.
    QList<Document *> drafts;
    for (int i = 0; i < 3; ++i) {
        drafts << store.newDraft();
        tabs.openDocument(drafts.last());
    }
    tabs.setActiveIndex(1);
    tabs.closeTab(1);
    QCOMPARE(tabs.activeDocument(), drafts.at(2));
    tabs.closeTab(1);
    QCOMPARE(tabs.activeDocument(), drafts.at(0));
    // Closing a tab before the one in front keeps that one in front.
    drafts[1] = store.newDraft();
    tabs.openDocument(drafts.at(1));
    tabs.closeTab(0);
    QCOMPARE(tabs.activeIndex(), 0);
    QCOMPARE(tabs.activeDocument(), drafts.at(1));
}

void ParchTests::sessionRestoresWindowsTabsAndAnchors()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString idA = newUuid(), idB = newUuid(), idC = newUuid();
    const QString fileA = root.filePath(QStringLiteral("a.md")), fileB = root.filePath(QStringLiteral("b.md"));
    SessionState state{{WindowState{{{idA, fileA, 12, 12, 3}, {idB, fileB, 0, 0, 0}}, 1, false, QSize(900, 700)}}};
    state.windows.append(WindowState{{{idC, QString(), 4, 2, 1, QStringLiteral("A draft")}}, 0, true, QSize(640, 480)});
    QVERIFY(Session::save(paths, state));
    const SessionState loaded = Session::load(paths);
    QCOMPARE(loaded.windows.first().tabs.first().cursor, 12);
    QCOMPARE(loaded.windows.first().activeTab, 1);
    QCOMPARE(loaded.windows.first().libraryOpen, false);

    QCOMPARE(loaded.windows.size(), 2);
    const ViewState &a = loaded.windows.first().tabs.first();
    QCOMPARE(a.documentId, idA);
    QCOMPARE(a.path, fileA);
    QCOMPARE(a.anchor, 12);
    QCOMPARE(a.top, 3);
    QCOMPARE(loaded.windows.first().tabs.at(1).path, fileB);
    QCOMPARE(loaded.windows.first().size, QSize(900, 700));
    const ViewState &draft = loaded.windows.at(1).tabs.first();
    QCOMPARE(draft.documentId, idC);
    QVERIFY(draft.path.isEmpty());
    QCOMPARE(draft.cursor, 4);
    QCOMPARE(draft.anchor, 2);
    QCOMPARE(draft.top, 1);
    QCOMPARE(draft.title, QStringLiteral("A draft"));
    QCOMPARE(loaded.windows.at(1).libraryOpen, true);
    QCOMPARE(loaded.windows.at(1).size, QSize(640, 480));
    // No session is no windows, and reading it creates nothing.
    QTemporaryDir empty;
    QVERIFY(Session::load(AppPaths::under(empty.path())).windows.isEmpty());
    QVERIFY(QDir(empty.path()).isEmpty());
}

void ParchTests::missingFileOnRestoreIsReportedNotRecreated()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString idA = newUuid(), idB = newUuid();
    const QString fileA = root.filePath(QStringLiteral("a.md")), fileB = root.filePath(QStringLiteral("b.md"));
    writeFile(fileA, "alpha\n");
    writeFile(fileB, "beta\n");
    SessionState state{{WindowState{{{idA, fileA, 12, 12, 3}, {idB, fileB, 0, 0, 0}}, 1, false, QSize(900, 700)}}};
    QVERIFY(Session::save(paths, state));
    Backend backend(paths);
    QFile::remove(fileA);  backend.restoreSession();
    WindowContext *firstWindow = backend.windows().first();
    QCOMPARE(firstWindow->tabs()->data(firstWindow->tabs()->index(0), TabModel::StateRole).toString(), QStringLiteral("File not found"));
    QVERIFY(!QFile::exists(fileA));
    // The tab keeps the file's name and path, and holds no document.
    TabModel *tabs = firstWindow->tabs();
    QCOMPARE(tabs->data(tabs->index(0), TabModel::TitleRole).toString(), QStringLiteral("a.md"));
    QCOMPARE(tabs->data(tabs->index(0), TabModel::PathRole).toString(), fileA);
    QCOMPARE(tabs->documentAt(0), nullptr);
    QVERIFY(tabs->documentAt(1));
    // Showing it looks again, and creates nothing.
    tabs->setActiveIndex(0);
    QCOMPARE(tabs->activeDocument(), nullptr);
    QCOMPARE(tabs->activeStatus(), QStringLiteral("File not found"));
    QVERIFY(!QFile::exists(fileA));
    // Nor does quitting, and the tab is kept in the session.
    QSignalSpy quits(&backend, &Backend::quitRequested);
    backend.quit();
    QCOMPARE(quits.count(), 1);
    QVERIFY(!QFile::exists(fileA));
    QVERIFY(sessionNames(paths, fileA));
    // A tab whose file comes back shows it when it is next shown.
    writeFile(fileA, "back\n");
    tabs->setActiveIndex(1);
    tabs->setActiveIndex(0);
    QVERIFY(tabs->activeDocument());
    QCOMPARE(tabs->activeDocument()->buffer()->text(), QStringLiteral("back\n"));
}

void ParchTests::firstLaunchOpensOneBlankDraft()
{
    QTemporaryDir emptyRoot;
    QVERIFY(emptyRoot.isValid());
    {
        Backend fresh(AppPaths::under(emptyRoot.path()));  fresh.start({});
        QCOMPARE(fresh.windows().first()->tabs()->rowCount(), 1);
        QVERIFY(QDir(AppPaths::under(emptyRoot.path()).drafts()).isEmpty());
        QCOMPARE(fresh.windows().size(), 1);
        QVERIFY(fresh.document() && fresh.document()->isDraft());
        QVERIFY(fresh.document()->buffer()->text().isEmpty());
    }

    // With freshDraftOnLaunch the session comes back with a blank draft in front.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    QVERIFY(Session::save(paths, SessionState{{WindowState{{{QString(), file, 0, 0, 0}}, 0, true, {}}}}));
    writeFile(paths.settingsFile(), R"({"freshDraftOnLaunch": true})");
    {
        Backend again(paths);
        again.start({});
        QCOMPARE(again.windows().size(), 1);
        TabModel *tabs = again.windows().first()->tabs();
        QCOMPARE(tabs->rowCount(), 2);
        QVERIFY(again.document() && again.document()->isDraft());
        QCOMPARE(tabs->data(tabs->index(0), TabModel::PathRole).toString(), canonical(file));
    }
    // Without it, the session alone.
    writeFile(paths.settingsFile(), R"({"freshDraftOnLaunch": false})");
    Backend third(paths);
    third.start({});
    QCOMPARE(third.windows().first()->tabs()->rowCount(), 1);
    QVERIFY(third.document());
    QCOMPARE(third.document()->path(), canonical(file));
    QVERIFY(QDir(paths.drafts()).isEmpty());
}

void ParchTests::secondLaunchForwardsFilesToRunningInstance()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    AppService service(paths);  QVERIFY(service.listen());
    QSignalSpy requests(&service, &AppService::openRequested);
    QVERIFY(AppService::forward(paths, {file}));
    QTRY_COMPARE(requests.count(), 1);
    QCOMPARE(requests.first().first().toStringList(), QStringList{file});
    QVERIFY(!AppService(paths).listen());
    // That second service left the socket to the first, which still answers.
    const QStringList two{file, root.filePath(QStringLiteral("b.md"))};
    QVERIFY(AppService::forward(paths, two));
    QTRY_COMPARE(requests.count(), 2);
    QCOMPARE(requests.at(1).first().toStringList(), two);
    // A launch with no files asks only that a window comes to the front.
    QVERIFY(AppService::forward(paths, {}));
    QTRY_COMPARE(requests.count(), 3);
    QVERIFY(requests.at(2).first().toStringList().isEmpty());
    // Only full paths are sent.
    QVERIFY(!AppService::forward(paths, {QStringLiteral("a.md")}));
    // The socket is in the runtime folder and only its owner can reach it.
    const QString socketPath = AppService::socketPath(paths);
    QCOMPARE(socketPath, paths.runtime + QStringLiteral("/parch.sock"));
    struct stat info {};
    QCOMPARE(::stat(QFile::encodeName(socketPath).constData(), &info), 0);
    QVERIFY(S_ISSOCK(info.st_mode));
    QCOMPARE(info.st_mode & 077, 0u);
    QTest::qWait(50);
    QCOMPARE(requests.count(), 3);
}

namespace {
// parch itself, as bin/build makes it.
const QString parchBinary = QStringLiteral(PARCH_SOURCE_DIR "/build/parch");

// What parch run as a process needs: offscreen, with a home, XDG folders,
// a runtime folder and a temporary folder of its own beneath `root`, and no
// Hyprland, display or session bus, so that it reaches nothing of the desktop.
QProcessEnvironment processEnvironment(const QString &root)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
                             "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "QT_LINUX_ACCESSIBILITY_ALWAYS_ON"})
        environment.remove(QString::fromLatin1(name));
    const QList<std::pair<const char *, const char *>> folders{
        {"HOME", "home"},           {"XDG_CONFIG_HOME", "config"}, {"XDG_DATA_HOME", "data"},
        {"XDG_STATE_HOME", "state"}, {"XDG_CACHE_HOME", "cache"},   {"XDG_RUNTIME_DIR", "runtime"},
        {"TMPDIR", "tmp"}};
    for (const auto &[name, folder] : folders) {
        const QString path = root + u'/' + QString::fromLatin1(folder);
        QDir().mkpath(path);
        environment.insert(QString::fromLatin1(name), path);
    }
    QFile::setPermissions(root + QStringLiteral("/runtime"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=") + root + QStringLiteral("/no-bus"));
    environment.insert(QStringLiteral("NO_AT_BRIDGE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QStringLiteral("generic"));
    environment.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    environment.insert(QStringLiteral("QML_DISABLE_DISK_CACHE"), QStringLiteral("1"));
    return environment;
}

// The places parch run with processEnvironment(root) keeps its files.
AppPaths processPaths(const QString &root)
{
    return AppPaths{root + QStringLiteral("/data/parch"), root + QStringLiteral("/state/parch"),
                    root + QStringLiteral("/config/parch"), root + QStringLiteral("/runtime")};
}

// Starts parch in that environment, its output merged for a failure to show.
void startProcess(QProcess &process, const QString &root, const QStringList &arguments)
{
    process.setProcessEnvironment(processEnvironment(root));
    process.setWorkingDirectory(root);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(parchBinary, arguments);
}

// Ends a process a test left running, so that none outlives it.
void stopProcess(QProcess &process)
{
    if (process.state() == QProcess::NotRunning) return;
    process.kill();
    process.waitForFinished();
}
} // namespace

void ParchTests::processForwardsAndEndsOnSignal()
{
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = processPaths(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    const QString file = root.filePath(QStringLiteral("notes.md"));
    writeFile(file, "notes\n");
    QProcess running;
    const auto stopRunning = qScopeGuard([&running] { stopProcess(running); });
    startProcess(running, root.path(), {});
    QVERIFY2(running.waitForStarted(), qPrintable(running.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(AppService::socketPath(paths)), 15000);

    // A second launch with a file hands it to the one running, and ends.
    QProcess second;
    const auto stopSecond = qScopeGuard([&second] { stopProcess(second); });
    startProcess(second, root.path(), {QStringLiteral("open"), file});
    QVERIFY(second.waitForFinished(15000));
    QCOMPARE(second.exitStatus(), QProcess::NormalExit);
    QVERIFY2(second.exitCode() == 0, second.readAll().constData());

    // SIGTERM, as at the end of the session: the running one writes its
    // session, closes its socket and exits as quitting does.
    running.terminate();
    QVERIFY(running.waitForFinished(15000));
    const QByteArray said = running.readAll();
    QCOMPARE(running.exitStatus(), QProcess::NormalExit);
    QVERIFY2(running.exitCode() == 0, said.constData());
    bool named = false;
    for (const WindowState &window : Session::load(paths).windows)
        for (const ViewState &tab : window.tabs) named = named || tab.path == QFileInfo(file).canonicalFilePath();
    QVERIFY2(named, said.constData());
    QVERIFY(!QFileInfo::exists(AppService::socketPath(paths)));
}

namespace {
// What the signal paths would have left the process with, for a test that
// must not leave it.
volatile std::sig_atomic_t recordedExit = -1;
void recordExit(int status) { recordedExit = status; }
} // namespace

void ParchTests::endingOnASignalRefusesAndLeavesInTime()
{
    // The first SIGTERM or SIGHUP reaches the event loop through a pipe; a
    // second, while parch is still ending, leaves at once with 128 plus its
    // number. The handling there was before is put back afterwards.
    struct sigaction before {};
    QVERIFY(::sigaction(SIGTERM, nullptr, &before) == 0);
    recordedExit = -1;
    const int heard = QuitSignals::install(recordExit);
    QVERIFY(heard >= 0);
    auto uninstall = qScopeGuard([] { QuitSignals::uninstall(); });
    QVERIFY(::raise(SIGTERM) == 0);
    unsigned char byte = 0;
    QCOMPARE(::read(heard, &byte, 1), ssize_t(1));
    QCOMPARE(int(byte), SIGTERM);
    QCOMPARE(int(recordedExit), -1);
    QVERIFY(::raise(SIGHUP) == 0);
    QCOMPARE(int(recordedExit), 128 + SIGHUP);
    uninstall.dismiss();
    QuitSignals::uninstall();
    struct sigaction after {};
    QVERIFY(::sigaction(SIGTERM, nullptr, &after) == 0);
    QVERIFY(after.sa_handler == before.sa_handler);

    // Ending: a launch meanwhile is refused with a reason, the socket goes
    // at the end, and a save still running at the bound leaves with
    // exit(0), its text in recovery; one that lands in time leaves through
    // the event loop.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    for (const bool saveLands : {false, true}) {
        const QString file = root.filePath(saveLands ? QStringLiteral("lands.md") : QStringLiteral("held.md"));
        writeFile(file, "start\n");
        recordedExit = -1;
        AppService service(paths);
        QVERIFY(service.listen());
        Backend backend(paths);
        Document *document = backend.store()->open(file);
        QVERIFY(document);
        document->buffer()->replace({0, 0}, QStringLiteral("edited "));
        bool taken = true;
        QString why;
        {
            std::optional<PoolBlock> block;
            if (!saveLands) block.emplace();
            std::thread launch;
            if (!saveLands) launch = std::thread([&] {
                QThread::msleep(100);
                taken = AppService::forward(paths, {root.filePath(QStringLiteral("other.md"))}, &why);
            });
            QElapsedTimer ending;
            ending.start();
            QCOMPARE(QuitSignals::end(backend, service, saveLands ? 5000 : 800, recordExit), saveLands);
            QVERIFY(ending.elapsed() < 3000);
            if (launch.joinable()) launch.join();
        }
        QVERIFY(!QFileInfo::exists(AppService::socketPath(paths)));
        if (saveLands) {
            QCOMPARE(int(recordedExit), -1);
            QCOMPARE(readFile(file), QByteArray("edited start\n"));
        } else {
            QVERIFY(!taken);
            QVERIFY2(why.contains(QStringLiteral("closing")), qPrintable(why));
            QCOMPARE(int(recordedExit), 0);
            bool recovered = false;
            for (const Recovery::Entry &entry : Recovery(paths).pending())
                recovered = recovered || entry.text == QStringLiteral("edited start\n");
            QVERIFY(recovered);
            backend.store()->waitForSaves();
        }
    }
}

void ParchTests::processRefusesAMissingTheme()
{
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    // parch listens on its socket before it looks for the theme.
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(processPaths(root.path())));
    const QString missing = root.filePath(QStringLiteral("no-such-theme"));
    QProcess process;
    const auto stop = qScopeGuard([&process] { stopProcess(process); });
    startProcess(process, root.path(), {QStringLiteral("--theme"), missing});
    QVERIFY(process.waitForFinished(15000));
    const QString said = QString::fromLocal8Bit(process.readAll());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 1, qPrintable(said));
    QVERIFY2(said.contains(QStringLiteral("parch: there is no theme ") + missing), qPrintable(said));
}

void ParchTests::processBenchmarkEndsByItself()
{
    // A whole measurement takes a minute and a half, so it runs only when asked for.
    if (qEnvironmentVariable("PARCH_SLOW_TESTS") != QLatin1String("1"))
        QSKIP("Set PARCH_SLOW_TESTS=1 to run the whole benchmark, which takes about 90 s");
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString document = root.filePath(QStringLiteral("measured.md"));
    QString text;
    for (int i = 0; i < 100; ++i) text += QStringLiteral("The quiet writer keeps each ordinary word and reads again.\n\n");
    writeFile(document, text.toUtf8());
    const QString output = root.filePath(QStringLiteral("result.json"));
    QProcess process;
    const auto stop = qScopeGuard([&process] { stopProcess(process); });
    // A full measurement, a minute of it idle: it ends by itself, as it quits.
    startProcess(process, root.path(), {QStringLiteral("open"), document, QStringLiteral("--benchmark"), output});
    QVERIFY(process.waitForFinished(240000));
    const QByteArray said = process.readAll();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, said.constData());
    const QJsonObject result = QJsonDocument::fromJson(readFile(output)).object();
    QVERIFY2(result.contains(QStringLiteral("launchToEditableMs")), said.constData());
    // It measured a copy: the document is as it was.
    QCOMPARE(readFile(document), text.toUtf8());
}

void ParchTests::tabsLoadWhenFirstShown()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QStringList files;
    for (const char *name : {"a.md", "b.md", "c.md"}) {
        files << root.filePath(QString::fromLatin1(name));
        writeFile(files.last(), QByteArray(name) + "\n");
    }
    const QString draftId = newUuid();
    writeFile(paths.drafts() + u'/' + draftId + QStringLiteral("/draft.md"), "Kept draft\nwith words");
    QVERIFY(Session::save(paths, SessionState{{WindowState{{{QString(), files.at(0), 0, 0, 0}, {QString(), files.at(1), 1, 1, 0},
                                                             {QString(), files.at(2), 0, 0, 0}, {draftId, QString(), 0, 0, 0, QStringLiteral("Kept draft")}},
                                                            1, true, {}}}}));
    Backend backend(paths);
    backend.start({});
    TabModel *tabs = backend.windows().first()->tabs();
    QCOMPARE(tabs->rowCount(), 4);
    QCOMPARE(tabs->activeIndex(), 1);
    // Only the document in front has been read.
    QCOMPARE(backend.store()->documents().size(), 1);
    QCOMPARE(tabs->activeDocument()->path(), canonical(files.at(1)));
    QCOMPARE(tabs->documentAt(0), nullptr);
    // A tab not read yet is named all the same.
    QCOMPARE(tabs->data(tabs->index(0), TabModel::TitleRole).toString(), QStringLiteral("a.md"));
    QCOMPARE(tabs->data(tabs->index(3), TabModel::TitleRole).toString(), QStringLiteral("Kept draft"));
    QVERIFY(tabs->data(tabs->index(0), TabModel::StateRole).toString().isEmpty());
    tabs->setActiveIndex(2);
    QCOMPARE(backend.store()->documents().size(), 2);
    QCOMPARE(tabs->activeDocument()->buffer()->text(), QStringLiteral("c.md\n"));
    tabs->setActiveIndex(3);
    QVERIFY(tabs->activeDocument() && tabs->activeDocument()->isDraft());
    QCOMPARE(tabs->activeDocument()->id(), draftId);
    QCOMPARE(tabs->activeDocument()->buffer()->text(), QStringLiteral("Kept draft\nwith words"));
    QCOMPARE(backend.store()->documents().size(), 3);
}

void ParchTests::damagedSessionStartsFreshAndIsKept()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QByteArray damaged = "{\"windows\": [ not json";
    writeFile(paths.sessionFile(), damaged);
    {
        Backend backend(paths);
        backend.start({});
        QCOMPARE(backend.windows().size(), 1);
        QCOMPARE(backend.windows().first()->tabs()->rowCount(), 1);
        QVERIFY(backend.document() && backend.document()->isDraft());
        // The damaged file is moved aside, never written over.
        QCOMPARE(readFile(paths.sessionFile() + QStringLiteral(".bad")), damaged);
        QVERIFY(!QFile::exists(paths.sessionFile()));
    }
    // A file of the wrong shape is damaged too, and the earlier copy is kept.
    writeFile(paths.sessionFile(), "[1, 2]");
    QVERIFY(Session::load(paths).windows.isEmpty());
    QCOMPARE(readFile(paths.sessionFile() + QStringLiteral(".bad")), damaged);
    QCOMPARE(readFile(paths.sessionFile() + QStringLiteral(".bad.2")), QByteArray("[1, 2]"));
    // Values out of range are mended and unusable tabs left out, without
    // giving up the rest of the file.
    writeFile(paths.sessionFile(), R"({"version": 1, "windows": [
        {"tabs": [{"id": "x", "path": "relative.md"}, {"path": "/abs/a.md", "cursor": -4, "anchor": 9, "top": "no"},
                  {"path": "/abs/\u0000.md"}, {}], "activeTab": 7, "libraryOpen": "yes", "width": -1, "height": 99999999},
        {"tabs": []}, 3]})");
    const SessionState mended = Session::load(paths);
    QCOMPARE(mended.windows.size(), 1);
    QCOMPARE(mended.windows.first().tabs.size(), 1);
    const ViewState &kept = mended.windows.first().tabs.first();
    QCOMPARE(kept.path, QStringLiteral("/abs/a.md"));
    QCOMPARE(kept.cursor, 0);
    QCOMPARE(kept.anchor, 9);
    QCOMPARE(kept.top, 0);
    QCOMPARE(mended.windows.first().activeTab, 0);
    QCOMPARE(mended.windows.first().libraryOpen, true);
    QVERIFY(!mended.windows.first().size.isValid());
    QVERIFY(!QFile::exists(paths.sessionFile() + QStringLiteral(".bad.3")));
}

void ParchTests::restoredDocumentsGoToTheirSessionWindow()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    Recovery recovery(paths);
    const QString inSession = newUuid(), elsewhere = newUuid();
    QVERIFY(recovery.write(inSession, {}, SourceSnapshot{1, QStringLiteral("kept in the second window")}, DiskVersion{}));
    QVERIFY(recovery.write(elsewhere, {}, SourceSnapshot{1, QStringLiteral("named by no window")}, DiskVersion{}));
    SessionState state;
    state.windows << WindowState{{{QString(), file, 0, 0, 0}}, 0, false, QSize(800, 600)}
                  << WindowState{{{QString(), file, 2, 2, 0}, {inSession, QString(), 3, 3, 0}}, 0, true, QSize(700, 500)};
    QVERIFY(Session::save(paths, state));
    Backend backend(paths);
    backend.start({});
    QCOMPARE(backend.windows().size(), 2);
    WindowContext *first = backend.windows().at(0), *second = backend.windows().at(1);
    // The session's window takes the text recovery kept, behind the tab in front.
    QCOMPARE(second->tabs()->rowCount(), 2);
    Document *kept = second->tabs()->documentAt(1);
    QVERIFY(kept);
    QCOMPARE(kept->id(), inSession);
    QCOMPARE(kept->buffer()->text(), QStringLiteral("kept in the second window"));
    QVERIFY(kept->state() != Document::State::Clean);
    QCOMPARE(second->tabs()->activeIndex(), 0);
    // Text no window named goes to the first window.
    QCOMPARE(first->tabs()->rowCount(), 2);
    QCOMPARE(first->tabs()->documentAt(1)->id(), elsewhere);
    QCOMPARE(first->tabs()->activeIndex(), 0);
    // Both windows show one document for the file, each with its own view.
    QCOMPARE(first->tabs()->documentAt(0), second->tabs()->documentAt(0));
    QCOMPARE(second->tabs()->viewState(0).cursor, 2);
    // Each window keeps its own library and size.
    QCOMPARE(first->libraryOpen(), false);
    QCOMPARE(second->libraryOpen(), true);
    QCOMPARE(first->initialSize(), QSize(800, 600));
    QCOMPARE(second->state().size, QSize(700, 500));
}

void ParchTests::recoveryOnlyTextReturnsAfterRelaunch()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QString id;
    {
        Backend backend(paths);
        backend.start({});
        Document *draft = backend.document();
        QVERIFY(draft);
        id = draft->id();
        // The draft's folder is taken, so only recovery can hold the text.
        writeFile(paths.drafts() + u'/' + id, "in the way");
        draft->buffer()->replace({0, 0}, QStringLiteral("only in recovery"));
        QSignalSpy quits(&backend, &Backend::quitRequested);
        backend.quit();
        QCOMPARE(quits.count(), 1);
        QCOMPARE(draft->state(), Document::State::SaveFailed);
    }
    {
        Backend again(paths);
        again.start({});
        QCOMPARE(again.windows().size(), 1);
        QCOMPARE(again.windows().first()->tabs()->rowCount(), 1);
        Document *back = again.document();
        QVERIFY(back);
        QCOMPARE(back->id(), id);
        QCOMPARE(back->buffer()->text(), QStringLiteral("only in recovery"));
        QVERIFY(back->state() != Document::State::Clean);
    }

    // After a crash, with no quit and no session, recovery still brings it back.
    QTemporaryDir crashed;
    QVERIFY(crashed.isValid());
    const AppPaths lost = AppPaths::under(crashed.path());
    {
        Backend backend(lost);
        backend.start({});
        Document *draft = backend.document();
        id = draft->id();
        writeFile(lost.drafts() + u'/' + id, "in the way");
        draft->buffer()->replace({0, 0}, QStringLiteral("typed before the crash"));
        QTRY_VERIFY(!Recovery(lost).pending().isEmpty());
    }
    QVERIFY(!QFile::exists(lost.sessionFile()));
    Backend again(lost);
    again.start({});
    QCOMPARE(again.windows().first()->tabs()->rowCount(), 1);
    QVERIFY(again.document());
    QCOMPARE(again.document()->id(), id);
    QCOMPARE(again.document()->buffer()->text(), QStringLiteral("typed before the crash"));
}

void ParchTests::launchSaysWhatCameBackFromRecovery()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("notes.md"));
    writeFile(file, "on disk\n");
    // A launch with nothing in recovery says nothing.
    {
        Backend backend(paths);
        backend.start({});
        QVERIFY(!backend.notice());
        QVERIFY(backend.problem().isEmpty());
    }
    QString named;
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Document *document = store.open(file);
        named = document->title();
        document->buffer()->replace({0, 0}, QStringLiteral("unsaved "));
        QVERIFY(recovery.write(document->id(), document->path(), *document->buffer()->snapshot(), document->diskVersion()));
        Document *draft = store.newDraft();
        draft->buffer()->replace({0, 0}, QStringLiteral("Lost draft\n"));
        QVERIFY(recovery.write(draft->id(), {}, *draft->buffer()->snapshot(), draft->diskVersion()));
    }
    Backend backend(paths);
    backend.start({file});
    // The text that came back is named, with Dismiss.
    QVERIFY(backend.notice());
    const QString said = backend.problem() + u'\n' + backend.problemDetail();
    QVERIFY2(said.contains(QStringLiteral("recovery")), qPrintable(said));
    QVERIFY2(said.contains(named) && said.contains(QStringLiteral("Lost draft")), qPrintable(said));
    backend.dismissNotice();
    QVERIFY(!backend.notice());
    QVERIFY(backend.problem().isEmpty());
}

void ParchTests::aLaunchThatOnlyKeepsTextInRecoverySaysSo()
{
    // A launch at which nothing came back from recovery, and text stayed
    // there because another document has the file its path leads to, is
    // titled for that, as a failure headed off and not as news. With text
    // that did come back beside it, the title says that, as before.
    QTemporaryDir root, files;
    QVERIFY(root.isValid() && files.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString folder = QFileInfo(files.path()).canonicalFilePath();
    const QString plan = folder + QStringLiteral("/plan.md"), other = folder + QStringLiteral("/other.md");
    writeFile(plan, "plan\n");
    writeFile(other, "other\n");
    {
        // other.md's entry holds what its file holds, as after a save that
        // landed just before parch ended; plan.md's holds unsaved text.
        DocumentStore store(paths);
        Recovery recovery(paths);
        Document *first = store.open(other);
        Document *second = store.open(plan);
        QVERIFY(first && second);
        second->buffer()->replace({0, 0}, QStringLiteral("mine "));
        QVERIFY(recovery.write(first->id(), other, *first->buffer()->snapshot(), first->diskVersion()));
        QTest::qWait(20);
        QVERIFY(recovery.write(second->id(), plan, *second->buffer()->snapshot(), second->diskVersion()));
    }
    QVERIFY(QFile::remove(plan));
    QVERIFY(QFile::link(other, plan));
    {
        Backend backend(paths);
        backend.start({});
        QVERIFY(backend.notice());
        QCOMPARE(backend.problem(), QStringLiteral("Text kept in recovery"));
        QCOMPARE(backend.problemDetail(),
                 QStringLiteral("Text from recovery stays there, since another document has the file its path leads to: plan.md"));
        QCOMPARE(backend.noticeTone(), Backend::Warning);
        QVERIFY(!backend.noticeInforms());
        backend.dismissNotice();
        QVERIFY(!backend.notice());
    }
    // Text that came back, with text that stayed.
    QTemporaryDir secondRoot, secondFiles;
    QVERIFY(secondRoot.isValid() && secondFiles.isValid());
    HeldEntry held;
    leaveAHeldEntry(AppPaths::under(secondRoot.path()), QFileInfo(secondFiles.path()).canonicalFilePath(), held);
    if (QTest::currentTestFailed()) return;
    Backend backend(AppPaths::under(secondRoot.path()));
    backend.start({});
    QCOMPARE(backend.problem(), QStringLiteral("Restored from recovery"));
    QVERIFY2(backend.problemDetail().contains(QStringLiteral("came back from recovery: other.md"))
                 && backend.problemDetail().contains(QStringLiteral("stays there")),
             qPrintable(backend.problemDetail()));
    QCOMPARE(backend.noticeTone(), Backend::News);
}

void ParchTests::sessionIsWrittenLaterAndOffTheUiThread()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SessionState state{{WindowState{{{QString(), QStringLiteral("/somewhere/a.md"), 0, 0, 0}}, 0, true, {}}}};
    {
        SessionWriter writer(paths);
        {
            // With every worker busy, writing returns at once and waits.
            PoolBlock block;
            for (int i = 0; i < 10; ++i) {
                state.windows[0].tabs[0].cursor = i;
                writer.write(state);
            }
            QVERIFY(!QFile::exists(paths.sessionFile()));
        }
        QTRY_COMPARE(Session::load(paths).windows.value(0).tabs.value(0).cursor, 9);
        // The first write and the newest: the eight between were never written.
        QTRY_COMPARE(writer.written(), 2);
        QTest::qWait(100);
        QCOMPARE(writer.written(), 2);
    }

    // The backend writes two seconds after a change, not at once.
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const AppPaths later = AppPaths::under(other.path());
    const QString file = other.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    Backend backend(later);
    backend.start({});
    QElapsedTimer clock;
    clock.start();
    backend.openFiles({file});
    QVERIFY(!sessionNames(later, canonical(file)));
    QTRY_VERIFY_WITH_TIMEOUT(sessionNames(later, canonical(file)), 5000);
    QVERIFY2(clock.elapsed() >= Backend::SessionDelayMs - 100, qPrintable(QString::number(clock.elapsed())));
}

void ParchTests::quitRefusalIgnoresRecoveryEvidenceFromBeforeIt()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    backend.start({});
    Document *draft = backend.document();
    const QString id = draft->id();
    // The draft cannot be saved, so its text lives in recovery.
    writeFile(paths.drafts() + u'/' + id, "in the way");
    draft->buffer()->replace({0, 0}, QStringLiteral("first"));
    QTRY_VERIFY(!Recovery(paths).pending().isEmpty());
    QTRY_COMPARE(Recovery(paths).pending().first().text, QStringLiteral("first"));
    // A failed save stays failed, so the quit below saves nothing and waits
    // on nothing that would deliver the report early.
    QTRY_COMPARE(draft->state(), Document::State::SaveFailed);
    // The report of that write arrives only after the next quit, as a queued
    // report can.
    Autosave *autosave = backend.autosave();
    const quint64 earlier = autosave->recoveryWrites();
    QVERIFY(earlier > 0);
    QMetaObject::invokeMethod(autosave, [autosave, id, earlier] { emit autosave->recoveryWritten(id, earlier); },
                              Qt::QueuedConnection);
    // Now recovery cannot be written either, and the newer text is nowhere.
    QVERIFY(QDir(paths.recovery()).removeRecursively());
    writeFile(paths.recovery(), "in the way");
    draft->buffer()->replace({5, 0}, QStringLiteral(" and more"));
    QSignalSpy quits(&backend, &Backend::quitRequested);
    backend.quit();
    QCOMPARE(quits.count(), 0);
    QVERIFY(!backend.problem().isEmpty());
    QCoreApplication::processEvents();
    QVERIFY2(!backend.problem().isEmpty(), "a report from before the refusal cleared it");
    // A write after the refusal is evidence, and clears it.
    QVERIFY(QFile::remove(paths.recovery()));
    draft->buffer()->replace({draft->buffer()->length(), 0}, QStringLiteral("."));
    QTRY_VERIFY(backend.problem().isEmpty());
}

void ParchTests::closingTabsAndWindowsKeepsText()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    Backend backend(paths);
    QSignalSpy quits(&backend, &Backend::quitRequested);
    backend.start({});
    WindowContext *window = backend.windows().first();
    TabModel *tabs = window->tabs();
    keepDraft(backend.document());

    // A clean unused draft leaves nothing behind.
    backend.newDraft();
    const QString unused = backend.document()->id();
    QCOMPARE(tabs->rowCount(), 2);
    backend.requestCloseTab(1);
    QCOMPARE(tabs->rowCount(), 1);
    QVERIFY(!QFileInfo::exists(paths.drafts() + u'/' + unused));

    // A modified file is saved as its tab is closed, and the tab goes once
    // the save has landed.
    backend.openFiles({file});
    Document *named = backend.document();
    QPointer<Document> watched = named;
    named->buffer()->replace({0, 0}, QStringLiteral("edited "));
    backend.requestCloseTab(tabs->activeIndex());
    // The save starts at once, not when autosave next comes round, and the
    // tab stays until it has landed.
    QCOMPARE(named->state(), Document::State::Saving);
    QCOMPARE(tabs->rowCount(), 2);
    QTRY_COMPARE(tabs->rowCount(), 1);
    QCOMPARE(readFile(file), QByteArray("edited alpha\n"));
    QTRY_VERIFY(!backend.store()->documents().contains(watched.data()));

    // So is a draft with words in it.
    backend.newDraft();
    Document *draft = backend.document();
    const QString draftId = draft->id();
    draft->buffer()->replace({0, 0}, QStringLiteral("some words"));
    backend.requestCloseTab(tabs->activeIndex());
    QTRY_COMPARE(readFile(paths.drafts() + u'/' + draftId + QStringLiteral("/draft.md")), QByteArray("some words"));

    // Text that cannot be saved is in recovery as its tab closes, and once
    // its save has failed it is back in a tab, where the failure shows.
    backend.newDraft();
    Document *blocked = backend.document();
    QPointer<Document> blockedWatched = blocked;
    writeFile(paths.drafts() + u'/' + blocked->id(), "in the way");
    blocked->buffer()->replace({0, 0}, QStringLiteral("only copy"));
    const QString blockedId = blocked->id();
    backend.requestCloseTab(tabs->activeIndex());
    QCOMPARE(tabs->indexOf(blocked), -1);
    const auto inRecovery = [&] {
        for (const Recovery::Entry &entry : Recovery(paths).pending())
            if (entry.documentId == blockedId && entry.text == QStringLiteral("only copy")) return true;
        return false;
    };
    QTRY_VERIFY(inRecovery());
    QVERIFY(backend.store()->documents().contains(blocked));
    QTRY_COMPARE(blocked->state(), Document::State::SaveFailed);
    QTRY_VERIFY(tabs->indexOf(blocked) >= 0);
    // Given up, it goes for good.
    tabs->setActiveIndex(tabs->indexOf(blocked));
    backend.discardText();
    QTRY_VERIFY(!blockedWatched);
    QCOMPARE(tabs->rowCount(), 1);

    // A second window closes by its last tab, and parch goes on.
    QSignalSpy removed(&backend, &Backend::windowRemoved);
    backend.newWindow();
    WindowContext *second = backend.windows().at(1);
    backend.activate(second);
    QCOMPARE(backend.activeWindow(), second);
    Document *secondDraft = second->tabs()->activeDocument();
    QPointer<Document> secondWatched = secondDraft;
    backend.requestCloseTab(0);
    QCOMPARE(removed.count(), 1);
    QCOMPARE(removed.first().first().value<WindowContext *>(), second);
    QCOMPARE(backend.windows().size(), 1);
    QCOMPARE(backend.activeWindow(), window);
    QTRY_VERIFY(!secondWatched);
    QCOMPARE(quits.count(), 0);
    // Closing the last tab of the last window is quitting.
    backend.requestCloseTab(0);
    QCOMPARE(quits.count(), 1);
    QCOMPARE(backend.windows().size(), 1);
    backend.closeWindow(window);
    QCOMPARE(quits.count(), 2);
}

void ParchTests::failedSaveOfAClosedTabComesBack()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    backend.start({});
    TabModel *tabs = backend.windows().first()->tabs();
    backend.newDraft();
    Document *draft = backend.document();
    QPointer<Document> watched = draft;
    draft->buffer()->replace({0, 0}, QStringLiteral("closed while saving"));
    // The draft's save cannot land, so its tab closes on the strength of recovery.
    writeFile(paths.drafts() + u'/' + draft->id(), "in the way");
    {
        PoolBlock block;
        backend.requestCloseTab(tabs->activeIndex());
        QCOMPARE(tabs->rowCount(), 1);
        QCOMPARE(tabs->indexOf(draft), -1);
        QCOMPARE(draft->state(), Document::State::Saving);
    }
    // The failure has a tab again, in the window in front, so that it can be seen.
    QTRY_COMPARE(draft->state(), Document::State::SaveFailed);
    QTRY_VERIFY(tabs->indexOf(draft) >= 0);
    QVERIFY(watched);
    QCOMPARE(tabs->rowCount(), 2);
    QVERIFY(backend.notice());
    QVERIFY2(backend.problem().contains(draft->title()) || backend.problemDetail().contains(draft->title()),
             qPrintable(backend.problem() + u'\n' + backend.problemDetail()));
    // It stays open until its text is somewhere safe or given up, as any failed save does.
    QVERIFY(backend.store()->documents().contains(draft));
}

void ParchTests::conflictCopyOpensInActiveWindow()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "base\n");
    Backend backend(paths);
    backend.start({});
    backend.openFiles({file});
    Document *document = backend.document();
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    backend.newWindow();
    WindowContext *second = backend.windows().at(1);
    backend.activate(second);
    backend.store()->resolveConflict(document, ConflictChoice::KeepBoth);
    // The copy, made on a worker, comes to the front of the window in front.
    QTRY_COMPARE(second->tabs()->rowCount(), 2);
    Document *copy = second->tabs()->activeDocument();
    QVERIFY(copy && copy != document);
    QVERIFY2(copy->path().contains(QStringLiteral("(conflict ")), qPrintable(copy->path()));
    QCOMPARE(copy->buffer()->text(), QStringLiteral("mine base\n"));
    QCOMPARE(second->tabs()->rowCount(), 2);
    // The file took the place of the first window's untouched draft.
    QCOMPARE(backend.windows().first()->tabs()->rowCount(), 1);
    QCOMPARE(document->buffer()->text(), QStringLiteral("theirs\n"));
}

void ParchTests::newFileBecomesItsFileOnFirstEdit()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    backend.start({});
    TabModel *tabs = backend.windows().first()->tabs();
    const QString target = root.filePath(QStringLiteral("new.md"));
    backend.openFiles({target});
    Document *document = backend.document();
    QVERIFY(document && document->isDraft());
    QVERIFY(document->buffer()->text().isEmpty());
    QCOMPARE(tabs->activeTitle(), QStringLiteral("new.md"));
    QCOMPARE(tabs->activePath(), target);
    QVERIFY(!QFile::exists(target));
    // Untouched, it creates nothing, through a quit as well.
    QSignalSpy quits(&backend, &Backend::quitRequested);
    backend.quit();
    QCOMPARE(quits.count(), 1);
    QVERIFY(!QFile::exists(target));
    QVERIFY(QDir(paths.drafts()).isEmpty());
    // Asked for again, the same tab comes to the front.
    backend.newDraft();
    const int rows = tabs->rowCount();
    backend.openFiles({target});
    QCOMPARE(tabs->rowCount(), rows);
    QCOMPARE(backend.document(), document);
    // The first edit makes it that file, once, keeping the document.
    document->buffer()->replace({0, 0}, QStringLiteral("fresh"));
    QTRY_COMPARE(readFile(target), QByteArray("fresh"));
    QTRY_VERIFY(!document->isDraft());
    QCOMPARE(document->path(), canonical(target));
    QCOMPARE(tabs->activeTitle(), QStringLiteral("new.md"));
    document->buffer()->replace({5, 0}, QStringLiteral(" words"));
    QTRY_COMPARE(readFile(target), QByteArray("fresh words"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QVERIFY(QDir(paths.drafts()).isEmpty());
    // Another one, closed untouched, creates nothing.
    const QString other = root.filePath(QStringLiteral("other.md"));
    backend.openFiles({other});
    backend.requestCloseTab(tabs->activeIndex());
    QVERIFY(!QFile::exists(other));

    // A path in a folder that does not exist is a tab that says so, and
    // nothing is created.
    const QString deep = root.filePath(QStringLiteral("missing/deeper/x.md"));
    backend.openFiles({deep});
    QCOMPARE(tabs->activeDocument(), nullptr);
    QCOMPARE(tabs->activeStatus(), QStringLiteral("File not found"));
    QCOMPARE(tabs->activeTitle(), QStringLiteral("x.md"));
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("missing"))));

    // Folders are not opened as documents: they join the library.
    QVERIFY(QDir(root.path()).mkdir(QStringLiteral("sub")));
    const int before = tabs->rowCount();
    backend.openFiles({root.path(), root.filePath(QStringLiteral("sub"))});
    QCOMPARE(tabs->rowCount(), before);
    QVERIFY(backend.problem().isEmpty());
    QCOMPARE(backend.settings()->libraryFolders(),
             (QStringList{canonical(root.path()), canonical(root.filePath(QStringLiteral("sub")))}));
}

void ParchTests::forwardedFilesOpenInTheMostRecentWindow()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    const QString a = root.filePath(QStringLiteral("a.md")), b = root.filePath(QStringLiteral("b.md"));
    writeFile(a, "alpha\n");
    writeFile(b, "beta\n");
    Backend backend(paths);
    backend.start({});
    backend.newWindow();
    WindowContext *first = backend.windows().at(0), *second = backend.windows().at(1);
    backend.activate(second);
    backend.activate(first);
    AppService service(paths);
    QVERIFY(service.listen());
    connect(&service, &AppService::openRequested, &backend, &Backend::openFiles);
    QSignalSpy raised(first, &WindowContext::raiseRequested);
    QVERIFY(AppService::forward(paths, {a}));
    // In place of the first window's untouched draft.
    QTRY_VERIFY(first->tabs()->activeDocument() && !first->tabs()->activeDocument()->isDraft());
    QCOMPARE(first->tabs()->rowCount(), 1);
    QCOMPARE(first->tabs()->activeDocument()->path(), canonical(a));
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(raised.count(), 1);
    backend.activate(second);
    QSignalSpy raisedSecond(second, &WindowContext::raiseRequested);
    QVERIFY(AppService::forward(paths, {b}));
    QTRY_VERIFY(second->tabs()->activeDocument() && !second->tabs()->activeDocument()->isDraft());
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(second->tabs()->activeDocument()->path(), canonical(b));
    QCOMPARE(raisedSecond.count(), 1);
}

void ParchTests::filesThatCannotBeReadAreNamedWhenOpened()
{
    if (::geteuid() == 0) QSKIP("Root reads a file whatever its mode.");
    QTemporaryDir root, files;
    QVERIFY(root.isValid() && files.isValid());
    const QString writing = QFileInfo(files.path()).canonicalFilePath() + QStringLiteral("/Writing");
    const QString locked = writing + QStringLiteral("/Locked away.md"), also = writing + QStringLiteral("/Also locked.md");
    writeFile(locked, "one\n");
    writeFile(also, "two\n");
    const auto readable = QFile::ReadOwner | QFile::WriteOwner;
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(locked, readable);
        QFile::setPermissions(also, readable);
    });
    QVERIFY(QFile::setPermissions(locked, {}));
    QVERIFY(QFile::setPermissions(also, {}));
    Backend backend(AppPaths::under(root.path()));
    backend.settings()->setLibraryFolders({writing});
    backend.start({});
    const QString denied = QString::fromLocal8Bit(std::strerror(EACCES));
    // One file is named, with its folder as the library names it, and why.
    backend.openFiles({locked});
    QCOMPARE(backend.problem(), QStringLiteral("“Locked away.md” in “Writing” cannot be read (%1)").arg(denied));
    QCOMPARE(backend.problemDetail(), QString());
    // Several are named one to a line.
    backend.openFiles({locked, also});
    QCOMPARE(backend.problem(), QStringLiteral("Some files could not be opened"));
    QCOMPARE(backend.problemDetail(), QStringLiteral("“Locked away.md” in “Writing” cannot be read (%1)\n"
                                                     "“Also locked.md” in “Writing” cannot be read (%1)")
                                          .arg(denied));
}

void ParchTests::eachRefusalToOpenSaysItsOwnReason()
{
    // The old copy of a draft named since is refused in words of its own,
    // that say where the draft is now, not as a file that cannot be read.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString copy = DocumentStore::draftFile(paths, id);
    const QString named = root.filePath(QStringLiteral("named.md"));
    writeFile(copy, "old words\n");
    writeFile(paths.registryFile(), QJsonDocument(QJsonObject{{named, id}}).toJson());
    Backend backend(paths);
    backend.start({});
    backend.openFiles({copy});
    QCOMPARE(backend.problem(), QStringLiteral("This is the old copy of a draft since named “named.md” in “%1”, so it is not opened")
                                    .arg(backend.folderLabel(named)));
    QCOMPARE(backend.problemDetail(), QString());
    // Nor is a file that is no regular file one that cannot be read.
    const QString pipe = root.filePath(QStringLiteral("pipe.md"));
    QCOMPARE(::mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
    backend.openFiles({pipe});
    QCOMPARE(backend.problem(), QStringLiteral("“pipe.md” in “%1” is not a regular file, so it is not opened")
                                    .arg(backend.folderLabel(pipe)));
}

void ParchTests::relativeLaunchPathsBecomeFull()
{
    QCOMPARE(absoluteFiles({QStringLiteral("a.md"), QStringLiteral("/x/b.md"), QStringLiteral("../c.md"), QString()},
                           QStringLiteral("/work/dir")),
             (QStringList{QStringLiteral("/work/dir/a.md"), QStringLiteral("/x/b.md"), QStringLiteral("/work/dir/../c.md")}));
    QCOMPARE(absoluteFiles({QStringLiteral("a.md")}, QStringLiteral("/")), QStringList{QStringLiteral("/a.md")});
    // One rule for a full path everywhere a path comes from outside.
    QVERIFY(PathText::isFullPath(QStringLiteral("/a/b.md")));
    QVERIFY(!PathText::isFullPath(QStringLiteral("b.md")));
    QVERIFY(!PathText::isFullPath(QString()));
    QVERIFY(!PathText::isFullPath(QStringLiteral("/a") + QChar(0) + QStringLiteral("b")));
    QVERIFY(!PathText::isFullPath(u'/' + QString(PathText::MaxLength, u'a')));
    QCOMPARE(PathText::folderOf(QStringLiteral("/a/b/c.md")), QStringLiteral("/a/b"));
    QCOMPARE(PathText::folderOf(QStringLiteral("/c.md")), QStringLiteral("/"));
    QCOMPARE(PathText::fileName(QStringLiteral("/a/b/c.md")), QStringLiteral("c.md"));
}

void ParchTests::staleSocketIsReplacedAndBadRequestsDropped()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    QVERIFY(QDir().mkpath(paths.runtime));
    QVERIFY(QFile::setPermissions(paths.runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString file = root.filePath(QStringLiteral("a.md"));
    const QString socketPath = AppService::socketPath(paths);
    // A crash left its socket behind: nobody answers there, and the next
    // instance takes the name.
    QVERIFY(leaveStaleSocket(socketPath));
    QVERIFY(!AppService::forward(paths, {file}));
    AppService service(paths);
    QVERIFY2(service.listen(), qPrintable(service.error()));
    QSignalSpy requests(&service, &AppService::openRequested);

    // Requests that are too large, malformed, or name anything but full
    // paths are dropped whole and their connection closed.
    const auto request = [](const QByteArray &files, const QByteArray &version = "1") {
        return frame("{\"version\":" + version + ",\"files\":" + files + "}");
    };
    QByteArray tooMany = "[";
    for (int i = 0; i <= AppService::MaxFiles; ++i) tooMany += QByteArray(i ? "," : "") + "\"/a.md\"";
    tooMany += "]";
    const QList<QByteArray> refused{
        frame({}, quint32(AppService::MaxRequestBytes) + 1),
        frame("not json"),
        // A bare list is not a request.
        frame(R"(["/a.md"])"),
        frame(R"({"files": ["/a.md"]})"),
        request(R"("/a.md")"),
        request(R"(["relative.md"])"),
        request(R"(["/a\u0000b.md"])"),
        request(R"(["/a.md", 1])"),
        request("[\"/" + QByteArray(PathText::MaxLength, 'a') + "\"]"),
        request(tooMany),
        frame(R"({"version": 1, "files": ["/a.md"], "activationToken": 5})"),
    };
    for (const QByteArray &bytes : refused)
        QVERIFY2(closedAfterSending(socketPath, bytes, 2000), bytes.left(40).constData());
    // One that stops halfway is closed after the time limit.
    QVERIFY(closedAfterSending(socketPath, request(R"(["/a.md"])").left(6), AppService::TimeoutMs + 2000));
    // A request of a version this instance does not know is refused in so many words.
    const QJsonObject answer = exchange(socketPath, request(R"(["/a.md"])", "2"));
    QCOMPARE(answer.value(QStringLiteral("accepted")).toBool(true), false);
    QVERIFY2(answer.value(QStringLiteral("reason")).toString().contains(QStringLiteral("version")),
             QJsonDocument(answer).toJson().constData());
    QCOMPARE(requests.count(), 0);
    // A good one is answered once it has been taken, with the activation token kept.
    const QJsonObject taken = exchange(socketPath, frame(R"({"version": 1, "files": ["/b.md"], "activationToken": "token-1"})"));
    QCOMPARE(taken.value(QStringLiteral("accepted")).toBool(), true);
    QCOMPARE(requests.count(), 1);
    QCOMPARE(service.activationToken(), QStringLiteral("token-1"));
    // The instance still takes a request from a launch afterwards.
    QVERIFY(AppService::forward(paths, {file}));
    QTRY_COMPARE(requests.count(), 2);
    QCOMPARE(requests.at(1).first().toStringList(), QStringList{file});
}

void ParchTests::forwardedOpenRaisesWithItsToken()
{
    // The window that shows a forwarded file is raised with the activation
    // token of the launch that forwarded it, as the Wayland platform reads
    // it, and nothing is left for a later raise; a launch without a token
    // leaves none. The raise is heard here, and no window is activated.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    QVERIFY(QDir().mkpath(paths.runtime));
    QVERIFY(QFile::setPermissions(paths.runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString a = root.filePath(QStringLiteral("a.md")), b = root.filePath(QStringLiteral("b.md"));
    writeFile(a, "alpha\n");
    writeFile(b, "beta\n");
    const QByteArray before = qgetenv("XDG_ACTIVATION_TOKEN");
    const auto restore = qScopeGuard([&] {
        if (before.isNull()) qunsetenv("XDG_ACTIVATION_TOKEN");
        else qputenv("XDG_ACTIVATION_TOKEN", before);
    });
    qunsetenv("XDG_ACTIVATION_TOKEN");
    Backend backend(paths);
    backend.start({});
    WindowContext *window = backend.activeWindow();
    AppService service(paths);
    QVERIFY2(service.listen(), qPrintable(service.error()));
    connect(&service, &AppService::openRequested, &backend, &Backend::openForwarded);
    QList<std::optional<QString>> raisedWith;
    connect(window, &WindowContext::raiseRequested, this, [&] {
        raisedWith.append(qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN")
                              ? std::optional(qEnvironmentVariable("XDG_ACTIVATION_TOKEN"))
                              : std::nullopt);
    });
    const QJsonObject taken = exchange(AppService::socketPath(paths),
                                       frame("{\"version\": 1, \"files\": [\"" + a.toUtf8()
                                             + "\"], \"activationToken\": \"token-1\"}"));
    QCOMPARE(taken.value(QStringLiteral("accepted")).toBool(), true);
    QTRY_COMPARE(raisedWith.size(), 1);
    QCOMPARE(raisedWith.first(), std::optional(QStringLiteral("token-1")));
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
    // A launch that was given no token raises the window without one.
    QVERIFY(AppService::forward(paths, {b}));
    QTRY_COMPARE(raisedWith.size(), 2);
    QCOMPARE(raisedWith.last(), std::nullopt);
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
}

void ParchTests::runtimeFolderMustBeTheAccountsOwn()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    const QByteArray runtime = QFile::encodeName(paths.runtime);
    // A folder other accounts can write to is refused by both sides.
    QVERIFY(QDir().mkpath(paths.runtime));
    QCOMPARE(::chmod(runtime.constData(), 0777), 0);
    AppService open(paths);
    QVERIFY(!open.listen());
    QVERIFY(!open.error().isEmpty());
    QVERIFY(!QFileInfo::exists(AppService::socketPath(paths)));
    QVERIFY(!AppService::forward(paths, {}));
    // So is a link standing in for the folder, even to a private one.
    QVERIFY(QDir(paths.runtime).removeRecursively());
    const QString elsewhere = root.filePath(QStringLiteral("private"));
    QVERIFY(QDir().mkpath(elsewhere));
    QCOMPARE(::chmod(QFile::encodeName(elsewhere).constData(), 0700), 0);
    QVERIFY(QFile::link(elsewhere, paths.runtime));
    AppService linked(paths);
    QVERIFY(!linked.listen());
    QVERIFY(!linked.error().isEmpty());
    QVERIFY(!QFileInfo::exists(elsewhere + QStringLiteral("/parch.sock")));
    // A missing folder is made, private to its owner.
    QVERIFY(QFile::remove(paths.runtime));
    AppService fresh(paths);
    QVERIFY2(fresh.listen(), qPrintable(fresh.error()));
    struct stat info {};
    QCOMPARE(::lstat(runtime.constData(), &info), 0);
    QVERIFY(S_ISDIR(info.st_mode));
    QCOMPARE(info.st_mode & 077, 0u);
}

void ParchTests::secondLaunchWaitsForOneStillStarting()
{
    // Two launches at once take turns: one that is starting holds the lock
    // until it listens, and the other then finds it alive instead of taking
    // the socket from it.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    QVERIFY(QDir().mkpath(paths.runtime));
    QVERIFY(QFile::setPermissions(paths.runtime, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    auto starting = std::make_unique<QLockFile>(AppService::socketPath(paths) + QStringLiteral(".lock"));
    QVERIFY(starting->tryLock(0));
    AppService first(paths);
    std::thread finisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        starting->unlock();
    });
    QElapsedTimer clock;
    clock.start();
    // The lock is free again after a moment, and nobody listens yet.
    const bool listening = first.listen();
    const qint64 waited = clock.elapsed();
    finisher.join();
    QVERIFY2(listening, qPrintable(first.error()));
    QVERIFY2(waited >= 250, qPrintable(QString::number(waited)));
    AppService second(paths);
    QVERIFY(!second.listen());
    QVERIFY(second.error().isEmpty());
    QSignalSpy requests(&first, &AppService::openRequested);
    QVERIFY(AppService::forward(paths, {QStringLiteral("/a.md")}));
    QTRY_COMPARE(requests.count(), 1);
}

void ParchTests::viewsComeBackWhereTheyWere()
{
    QTemporaryDir shared;
    QVERIFY(shared.isValid());
    const QString notes = shared.filePath(QStringLiteral("notes.md"));
    const QString other = shared.filePath(QStringLiteral("other.md"));
    writeFile(notes, "First line\nSecond line here\nThird\n");
    writeFile(other, "Other text\n");
    {
        ShellFixture shell({.root = shared.path()});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        activate(shell.window.get());
        // The first launch's untouched draft gives its tab to the first file.
        shell.backend->openFiles({notes, other});
        TabModel *tabs = shell.context()->tabs();
        QCOMPARE(tabs->rowCount(), 2);
        EditorBinding *binding = shell.binding();
        tabs->setActiveIndex(0);
        QCOMPARE(binding->document()->path(), canonical(notes));
        selectRange(shell.item(QStringLiteral("editor")), 6, 15);
        QCOMPARE(binding->anchorSource(), 6);
        QCOMPARE(binding->cursorSource(), 15);
        // Another tab and back: the view is where it was.
        tabs->setActiveIndex(1);
        QCOMPARE(binding->document()->path(), canonical(other));
        QCOMPARE(binding->cursorSource(), 0);
        tabs->setActiveIndex(0);
        QCOMPARE(binding->anchorSource(), 6);
        QCOMPARE(binding->cursorSource(), 15);
        // A second view of it starts there, and moves on its own.
        const int view = tabs->newView(0);
        QCOMPARE(binding->cursorSource(), 15);
        selectRange(shell.item(QStringLiteral("editor")), 2, 2);
        tabs->setActiveIndex(0);
        QCOMPARE(binding->cursorSource(), 15);
        tabs->closeTab(view);
        QCOMPARE(tabs->activeIndex(), 0);
        shell.backend->quit();
    }
    bool kept = false;
    for (const ViewState &view : Session::load(AppPaths::under(shared.path())).windows.value(0).tabs)
        kept = kept || (view.path == canonical(notes) && view.anchor == 6 && view.cursor == 15);
    QVERIFY(kept);
    {
        ShellFixture again({.root = shared.path()});
        QVERIFY2(again.ok(), qPrintable(again.error));
        TabModel *tabs = again.context()->tabs();
        QCOMPARE(tabs->rowCount(), 2);
        QCOMPARE(tabs->activeIndex(), 0);
        EditorBinding *binding = again.binding();
        QCOMPARE(binding->document()->path(), canonical(notes));
        QTRY_COMPARE(binding->cursorSource(), 15);
        QCOMPARE(binding->anchorSource(), 6);
        again.backend->quit();
    }
    // Positions from a longer text are kept inside the shorter one.
    writeFile(notes, "Hi\n");
    ShellFixture shorter({.root = shared.path()});
    QVERIFY2(shorter.ok(), qPrintable(shorter.error));
    QTRY_COMPARE(shorter.binding()->cursorSource(), 3);
    QCOMPARE(shorter.binding()->anchorSource(), 3);
}

void ParchTests::newWindowHasItsOwnTabsAndKeys()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *first = shell.window.get();
    activate(first);
    WindowContext *firstContext = shell.context();
    QTest::keyClick(first, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(shell.backend->windows().size(), 2);
    WindowContext *secondContext = shell.backend->windows().at(1);
    QQuickWindow *second = shell.windowFor(secondContext);
    QVERIFY(second);
    QCOMPARE(secondContext->tabs()->rowCount(), 1);
    QVERIFY(secondContext->tabs()->activeDocument()->isDraft());
    QVERIFY(secondContext->tabs()->activeDocument()->buffer()->text().isEmpty());
    // Keys reach the window in front, and the backend acts on it.
    activate(second);
    QCOMPARE(shell.backend->activeWindow(), secondContext);
    QTest::keyClick(second, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(secondContext->tabs()->rowCount(), 2);
    QCOMPARE(firstContext->tabs()->rowCount(), 1);
    typeText(second, QStringLiteral("second"));
    QCOMPARE(secondContext->tabs()->activeDocument()->buffer()->text(), QStringLiteral("second"));
    activate(first);
    QCOMPARE(shell.backend->activeWindow(), firstContext);
    QTest::keyClick(first, Qt::Key_N, Qt::ControlModifier);
    QCOMPARE(firstContext->tabs()->rowCount(), 2);
    QCOMPARE(secondContext->tabs()->rowCount(), 2);
    // Each window has its own library and size.
    QTest::keyClick(first, Qt::Key_B, Qt::ControlModifier);
    QCOMPARE(firstContext->libraryOpen(), !secondContext->libraryOpen());
    second->resize(700, 500);
    QTRY_COMPARE(secondContext->size(), QSize(700, 500));
    QVERIFY(firstContext->size() != QSize(700, 500));
    // Closing the second window keeps its text and leaves parch running.
    Document *typed = secondContext->tabs()->activeDocument();
    QSignalSpy quits(shell.backend.get(), &Backend::quitRequested);
    second->close();
    QTRY_COMPARE(shell.backend->windows().size(), 1);
    QTRY_VERIFY(!shell.windowFor(secondContext));
    QCOMPARE(quits.count(), 0);
    QTRY_COMPARE(readFile(shell.paths().drafts() + u'/' + typed->id() + QStringLiteral("/draft.md")), QByteArray("second"));
    // The window menu offers a new window and a new view.
    QStringList entries;
    QObject *menu = shell.object(QStringLiteral("windowMenu"));
    QVERIFY(menu);
    for (int i = 0; i < menu->property("count").toInt(); ++i) {
        QQuickItem *entry = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, entry), Q_ARG(int, i));
        if (entry) entries << entry->property("text").toString();
    }
    QVERIFY2(entries.contains(QStringLiteral("New window")), qPrintable(entries.join(", ")));
    QVERIFY2(entries.contains(QStringLiteral("New view")), qPrintable(entries.join(", ")));
}

void ParchTests::pageKeysSwitchTabsAsTabKeysDo()
{
    // Ctrl+PageDown and Ctrl+PageUp step to the next and previous tab, round
    // the ends, as Ctrl+Tab and Ctrl+Shift+Tab do, wherever the keys are.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    const QString a = shell.root.filePath(QStringLiteral("a.md")), b = shell.root.filePath(QStringLiteral("b.md"));
    writeFile(a, "alpha\n");
    writeFile(b, "beta\n");
    keepDraft(shell.document());
    shell.backend->openFiles({a, b});
    TabModel *tabs = shell.context()->tabs();
    QCOMPARE(tabs->rowCount(), 3);
    QCOMPARE(tabs->activeIndex(), 2);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_PageDown, Qt::ControlModifier);
    QCOMPARE(tabs->activeIndex(), 0);
    QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
    QCOMPARE(tabs->activeIndex(), 2);
    QTest::keyClick(window, Qt::Key_PageUp, Qt::ControlModifier);
    QCOMPARE(tabs->activeIndex(), 1);
    QCOMPARE(shell.document()->path(), canonical(a));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("alpha\n"));
    // From the library too.
    QQuickItem *library = shell.item(QStringLiteral("libraryTree"));
    QVERIFY(library);
    QTRY_VERIFY(library->isVisible());
    library->forceActiveFocus();
    QVERIFY(library->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_PageDown, Qt::ControlModifier);
    QCOMPARE(tabs->activeIndex(), 2);
    // The window's menu lists no tab switching, so it shows neither key.
    QVERIFY(!shell.window->property("keys").toMap().contains(QStringLiteral("nextTab")));
}

void ParchTests::tabKeysCycleAndCloseInTheWindow()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    const QString a = shell.root.filePath(QStringLiteral("a.md")), b = shell.root.filePath(QStringLiteral("b.md"));
    writeFile(a, "alpha\n");
    writeFile(b, "beta\n");
    keepDraft(shell.document());
    shell.backend->openFiles({a, b});
    TabModel *tabs = shell.context()->tabs();
    QCOMPARE(tabs->rowCount(), 3);
    QCOMPARE(tabs->activeIndex(), 2);
    QQuickItem *strip = shell.item(QStringLiteral("tabStrip"));
    QTRY_VERIFY(strip->isVisible());
    QCOMPARE(strip->property("count").toInt(), 3);
    QCOMPARE(strip->property("currentIndex").toInt(), 2);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(tabs->activeIndex(), 0);
    QCOMPARE(strip->property("currentIndex").toInt(), 0);
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(tabs->activeIndex(), 2);
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(tabs->activeIndex(), 1);
    QCOMPARE(shell.document()->path(), canonical(a));
    // A tab is chosen by clicking it too.
    QQuickItem *list = shell.item(QStringLiteral("tabs"));
    QQuickItem *third = nullptr;
    QMetaObject::invokeMethod(list, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, third), Q_ARG(int, 2));
    QVERIFY(third);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(third) - QPoint(int(third->width() / 4), 0));
    QCOMPARE(tabs->activeIndex(), 2);
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    // Ctrl+W saves the edited file and closes its tab once it is saved; its
    // neighbour comes in front.
    editor->forceActiveFocus();
    typeText(window, QStringLiteral("more "));
    QTest::keyClick(window, Qt::Key_W, Qt::ControlModifier);
    QTRY_COMPARE(tabs->rowCount(), 2);
    QCOMPARE(tabs->activeIndex(), 1);
    QCOMPARE(shell.document()->path(), canonical(b));
    QCOMPARE(readFile(a), QByteArray("more alpha\n"));
    QCOMPARE(strip->property("count").toInt(), 2);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    // A tab whose file is gone shows that in place of the editor.
    const QString gone = shell.root.filePath(QStringLiteral("gone/c.md"));
    shell.backend->openFiles({gone});
    QQuickItem *missing = shell.item(QStringLiteral("missingFile"));
    QVERIFY(missing && missing->isVisible());
    QVERIFY(!shell.item(QStringLiteral("editorPane"))->isVisible());
    QCOMPARE(shell.item(QStringLiteral("saveState"))->property("text").toString(), QStringLiteral("File not found"));
    QCOMPARE(shell.item(QStringLiteral("documentTitle"))->property("text").toString(), QStringLiteral("c.md"));
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY(!QFileInfo::exists(shell.root.filePath(QStringLiteral("gone"))));
    // Closing the last tab of the only window asks to quit.
    QSignalSpy quits(shell.backend.get(), &Backend::quitRequested);
    while (tabs->rowCount() > 1) QTest::keyClick(window, Qt::Key_W, Qt::ControlModifier);
    QCOMPARE(quits.count(), 0);
    QTest::keyClick(window, Qt::Key_W, Qt::ControlModifier);
    QCOMPARE(quits.count(), 1);
}

void ParchTests::newFileThatAppearsIsNeverReplaced()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QString keptId;
    {
        Backend backend(paths);
        backend.start({});
        TabModel *tabs = backend.windows().first()->tabs();
        const QString target = root.filePath(QStringLiteral("new.md"));
        backend.openFiles({target});
        Document *draft = backend.document();
        QVERIFY(draft && draft->isDraft());
        keptId = draft->id();
        // Another program writes the file before the first keystroke.
        writeFile(target, "theirs\n");
        draft->buffer()->replace({0, 0}, QStringLiteral("mine"));
        QTRY_VERIFY(!backend.problem().isEmpty());
        QVERIFY2(backend.problem().contains(QStringLiteral("new.md")), qPrintable(backend.problem()));
        QCOMPARE(readFile(target), QByteArray("theirs\n"));
        QVERIFY(draft->isDraft());
        QCOMPARE(draft->buffer()->text(), QStringLiteral("mine"));
        // The text is kept as a draft, and the tab says so rather than naming the file.
        QTRY_COMPARE(readFile(paths.drafts() + u'/' + keptId + QStringLiteral("/draft.md")), QByteArray("mine"));
        QCOMPARE(tabs->activeTitle(), QStringLiteral("mine"));
        QVERIFY(tabs->activePath().isEmpty());
        QCOMPARE(readFile(target), QByteArray("theirs\n"));
        // The file that appeared opens beside it.
        backend.openFiles({target});
        QVERIFY(backend.document() != draft);
        QCOMPARE(backend.document()->buffer()->text(), QStringLiteral("theirs\n"));

        // A file that appears while the naming is being written is not replaced either.
        const QString late = root.filePath(QStringLiteral("late.md"));
        backend.openFiles({late});
        Document *second = backend.document();
        QVERIFY(second->isDraft());
        {
            PoolBlock block;
            second->buffer()->replace({0, 0}, QStringLiteral("also mine"));
            QTRY_COMPARE(second->state(), Document::State::Saving);
            writeFile(late, "arrived meanwhile\n");
        }
        QTRY_VERIFY(backend.problem().contains(QStringLiteral("late.md")));
        QCOMPARE(readFile(late), QByteArray("arrived meanwhile\n"));
        QVERIFY(second->isDraft());
        QTRY_COMPARE(readFile(paths.drafts() + u'/' + second->id() + QStringLiteral("/draft.md")), QByteArray("also mine"));
        QSignalSpy quits(&backend, &Backend::quitRequested);
        backend.quit();
        QCOMPARE(quits.count(), 1);
    }
    // After a restart the kept text is a plain draft, never the file.
    Backend again(paths);
    again.start({});
    TabModel *tabs = again.windows().first()->tabs();
    int found = -1;
    for (int i = 0; i < tabs->rowCount(); ++i)
        if (tabs->data(tabs->index(i), TabModel::DocumentIdRole).toString() == keptId) found = i;
    QVERIFY(found >= 0);
    tabs->setActiveIndex(found);
    QVERIFY(tabs->activeDocument() && tabs->activeDocument()->isDraft());
    QCOMPARE(tabs->activeTitle(), QStringLiteral("mine"));
    QVERIFY(tabs->activePath().isEmpty());
}

void ParchTests::forwardWaitsUntilTheInstanceTakesTheFiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    SKIP_UNLESS_SOCKET_FITS(AppService::socketPath(paths));
    AppService service(paths);
    QVERIFY(service.listen());
    QSignalSpy requests(&service, &AppService::openRequested);
    // A launch is another process: it waits while the instance takes the files.
    std::atomic<int> taken{-1};
    int seenWhenAnswered = -1;
    std::thread launch([&] { taken = AppService::forward(paths, {QStringLiteral("/a.md")}) ? 1 : 0; });
    QTRY_VERIFY(taken != -1);
    seenWhenAnswered = int(requests.count());
    launch.join();
    QCOMPARE(taken.load(), 1);
    QCOMPARE(seenWhenAnswered, 1);

    // Many launches in a row are all taken, though the connections already
    // finished have not been deleted yet.
    std::atomic<int> done{0}, accepted{0};
    std::thread burst([&] {
        for (int i = 0; i < 24; ++i) {
            if (AppService::forward(paths, {QStringLiteral("/b.md")})) ++accepted;
            ++done;
        }
    });
    QElapsedTimer clock;
    clock.start();
    while (done < 24 && clock.elapsed() < 60000) QCoreApplication::processEvents();
    burst.join();
    QCOMPARE(accepted.load(), 24);
    QCOMPARE(requests.count(), 25);

    // An instance that has stopped listening takes nothing, and says so.
    service.close();
    QVERIFY(!QFileInfo::exists(AppService::socketPath(paths)));
    QString why;
    QVERIFY(!AppService::forward(paths, {QStringLiteral("/a.md")}, &why));
    // Nor does one that closes without answering.
    QLocalServer silent;
    silent.setSocketOptions(QLocalServer::UserAccessOption);
    QVERIFY(silent.listen(AppService::socketPath(paths)));
    connect(&silent, &QLocalServer::newConnection, &silent, [&silent] {
        while (QLocalSocket *socket = silent.nextPendingConnection()) {
            socket->abort();
            socket->deleteLater();
        }
    });
    std::atomic<int> unanswered{-1};
    std::thread lost([&] { unanswered = AppService::forward(paths, {QStringLiteral("/a.md")}) ? 1 : 0; });
    QTRY_VERIFY_WITH_TIMEOUT(unanswered != -1, AppService::TimeoutMs + 3000);
    lost.join();
    QCOMPARE(unanswered.load(), 0);
    QCOMPARE(requests.count(), 25);
}

void ParchTests::alreadyOpenFileComesToFrontInItsWindow()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "alpha\n");
    const QString link = root.filePath(QStringLiteral("link.md"));
    QVERIFY(QFile::link(file, link));
    Backend backend(paths);
    backend.start({});
    WindowContext *first = backend.windows().first();
    keepDraft(backend.document());
    backend.openFiles({file});
    Document *document = backend.document();
    backend.newWindow();
    WindowContext *second = backend.windows().at(1);
    for (const QString &path : {file, link}) {
        // The file is behind another tab in the first window, and the second is in front.
        first->tabs()->setActiveIndex(0);
        backend.activate(second);
        QSignalSpy raisedFirst(first, &WindowContext::raiseRequested);
        QSignalSpy raisedSecond(second, &WindowContext::raiseRequested);
        backend.openFiles({path});
        QCOMPARE(second->tabs()->rowCount(), 1);
        QCOMPARE(first->tabs()->rowCount(), 2);
        QCOMPARE(first->tabs()->activeDocument(), document);
        QCOMPARE(raisedFirst.count(), 1);
        QCOMPARE(raisedSecond.count(), 0);
    }
    // A second view is still there for the asking.
    backend.activate(first);
    backend.newView();
    QCOMPARE(first->tabs()->rowCount(), 3);
    QCOMPARE(first->tabs()->activeDocument(), document);
}

void ParchTests::backgroundViewsFollowEditsElsewhere()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "0123456789\n");
    TabModel tabs(&store), elsewhere(&store);
    Document *document = store.open(file);
    const int first = tabs.openDocument(document);
    tabs.newView(first);
    elsewhere.openDocument(document);
    tabs.setViewState(first, {document->id(), file, 8, 2, 4});
    elsewhere.setViewState(0, {document->id(), file, 8, 2, 4});
    SourceBuffer *buffer = document->buffer();
    const auto at = [&](TabModel &model, int index) {
        const ViewState view = model.viewState(index);
        return QList<int>{view.cursor, view.anchor, view.top};
    };
    // Before every position: all move by its length.
    buffer->replace({0, 0}, QStringLiteral("ab"));
    QCOMPARE(at(tabs, first), (QList<int>{10, 4, 6}));
    // Between them: those before stay.
    buffer->replace({5, 0}, QStringLiteral("xyz"));
    QCOMPARE(at(tabs, first), (QList<int>{13, 4, 9}));
    // Over one: it goes to the end of what replaced it, and those after move back.
    buffer->replace({8, 3}, QStringLiteral("Q"));
    QCOMPARE(at(tabs, first), (QList<int>{11, 4, 9}));
    // At a position: it stays where it is.
    buffer->replace({4, 0}, QStringLiteral("--"));
    QCOMPARE(at(tabs, first), (QList<int>{13, 4, 11}));
    // Undo is an edit like any other, and a view in another window follows too.
    QCOMPARE(at(elsewhere, 0), (QList<int>{13, 4, 11}));
    buffer->undo();
    QCOMPARE(at(elsewhere, 0), at(tabs, first));
}

void ParchTests::closingUnsavableTextKeepsItsTab()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    backend.start({});
    WindowContext *window = backend.windows().first();
    TabModel *tabs = window->tabs();
    backend.newDraft();
    Document *draft = backend.document();
    // Neither the draft's folder nor recovery can be written.
    writeFile(paths.drafts() + u'/' + draft->id(), "in the way");
    writeFile(paths.recovery(), "in the way");
    draft->buffer()->replace({0, 0}, QStringLiteral("only copy"));
    backend.requestCloseTab(1);
    QCOMPARE(tabs->rowCount(), 2);
    QCOMPARE(backend.document(), draft);
    QVERIFY(!backend.problem().isEmpty());
    QVERIFY2(backend.problemDetail().contains(QStringLiteral("only copy")), qPrintable(backend.problemDetail()));
    QCOMPARE(draft->buffer()->text(), QStringLiteral("only copy"));
    // Nor does the window that shows it close, while another window is open.
    backend.newWindow();
    QSignalSpy removed(&backend, &Backend::windowRemoved);
    backend.closeWindow(window);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(backend.windows().size(), 2);
    QCOMPARE(tabs->rowCount(), 2);
    // Its save has failed by now, so closing it again asks first, and the tab stays.
    QCOMPARE(draft->state(), Document::State::SaveFailed);
    QSignalSpy asked(window, &WindowContext::closeConfirmationRequested);
    backend.activate(window);
    backend.requestCloseTab(1);
    QCOMPARE(asked.count(), 1);
    QCOMPARE(tabs->rowCount(), 2);
    // Once the draft's own file can be written, trying again saves it and closes the tab.
    QVERIFY(QFile::remove(paths.drafts() + u'/' + draft->id()));
    backend.closeTryingAgain();
    QTRY_COMPARE(tabs->rowCount(), 1);
    QCOMPARE(readFile(DocumentStore::draftFile(paths, draft->id())), QByteArray("only copy"));
}

void ParchTests::windowsReachTabsOnlyThroughTheBackend()
{
    // Closing or opening a tab from QML would pass by what the backend does
    // to keep text, so QML can only ask the backend.
    const QMetaObject &meta = TabModel::staticMetaObject;
    for (const char *method : {"closeTab(int)", "openDocument(Document*)", "newView(int)"})
        QVERIFY2(meta.indexOfMethod(method) < 0, method);
    QVERIFY(Backend::staticMetaObject.indexOfMethod("newView()") >= 0);
    QVERIFY(Backend::staticMetaObject.indexOfMethod("requestCloseTab(int)") >= 0);
}

namespace {
// Marks every text it is asked for, so that a test sees which texts go through translation.
class MarkingTranslator : public QTranslator {
public:
    bool isEmpty() const override { return false; }
    QString translate(const char *, const char *sourceText, const char *, int) const override
    {
        return QStringLiteral("<%1>").arg(QString::fromUtf8(sourceText));
    }
};
} // namespace

void ParchTests::errorsShownCanBeTranslated()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    MarkingTranslator marking;
    QCoreApplication::installTranslator(&marking);
    const auto removed = qScopeGuard([&] { QCoreApplication::removeTranslator(&marking); });
    // A folder opened as a file, as a tab would show the reason.
    const LoadedFile folder = DiskFile::load(root.path());
    QVERIFY2(folder.error.startsWith(u'<'), qPrintable(folder.error));
    const LoadedFile missing = DiskFile::load(root.filePath(QStringLiteral("none.md")));
    QVERIFY2(missing.error.startsWith(u'<'), qPrintable(missing.error));
    // A recovery write that fails, as a refused quit would explain it.
    QString error;
    QVERIFY(!Recovery(AppPaths::under(root.path())).write(QStringLiteral("not an id"), {}, SourceSnapshot{1, "x"}, {}, &error));
    QVERIFY2(error.startsWith(u'<'), qPrintable(error));
    // A save over a changed file.
    const QString file = root.filePath(QStringLiteral("a.md"));
    writeFile(file, "a\n");
    const DiskVersion before = DiskFile::fingerprint(file);
    writeFile(file, "b\n");
    const SaveResult saved = DiskFile::save(file, "c\n", before);
    QVERIFY2(saved.error.startsWith(u'<'), qPrintable(saved.error));
}

void ParchTests::openingFindsATabNotYetReadInAnyWindow()
{
    // A restored tab waits unread behind the one in front of its window.
    // Opening its file from elsewhere brings that tab to the front, read,
    // rather than a second tab on the same file.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString a = canonical(root.path()) + QStringLiteral("/a.md"), b = canonical(root.path()) + QStringLiteral("/b.md"),
                  c = canonical(root.path()) + QStringLiteral("/c.md");
    writeFile(a, "alpha\n");
    writeFile(b, "beta\n");
    writeFile(c, "gamma\n");
    QVERIFY(Session::save(paths, SessionState{{WindowState{{{QString(), a, 0, 0, 0}, {QString(), b, 0, 0, 0}}, 1, true, {}},
                                               WindowState{{{QString(), c, 0, 0, 0}}, 0, true, {}}}}));
    Backend backend(paths);
    backend.start({});
    QCOMPARE(backend.windows().size(), 2);
    WindowContext *first = backend.windows().at(0), *second = backend.windows().at(1);
    QCOMPARE(first->tabs()->documentAt(0), nullptr);
    backend.activate(second);
    QSignalSpy raisedFirst(first, &WindowContext::raiseRequested);
    backend.openFiles({a});
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(first->tabs()->rowCount(), 2);
    QCOMPARE(first->tabs()->activeIndex(), 0);
    QVERIFY(first->tabs()->activeDocument());
    QCOMPARE(first->tabs()->activeDocument()->path(), a);
    QCOMPARE(first->tabs()->activeDocument()->buffer()->text(), QStringLiteral("alpha\n"));
    QCOMPARE(raisedFirst.count(), 1);
    // A window that shows the document is chosen over one whose tab only waits for it.
    QVERIFY(Session::save(paths, SessionState{{WindowState{{{QString(), c, 0, 0, 0}, {QString(), b, 0, 0, 0}}, 0, true, {}},
                                               WindowState{{{QString(), b, 0, 0, 0}}, 0, true, {}},
                                               WindowState{{{QString(), a, 0, 0, 0}}, 0, true, {}}}}));
    {
        Backend three(paths);
        three.start({});
        WindowContext *waiting = three.windows().at(0), *showing = three.windows().at(1);
        QCOMPARE(waiting->tabs()->documentAt(1), nullptr);
        QSignalSpy raisedShowing(showing, &WindowContext::raiseRequested);
        three.activate(three.windows().at(2));
        three.openFiles({b});
        QCOMPARE(raisedShowing.count(), 1);
        QCOMPARE(waiting->tabs()->documentAt(1), nullptr);
        QCOMPARE(waiting->tabs()->activeIndex(), 0);
    }
    // The tab in front of its window that says its file is not found takes
    // the file once it is back.
    QVERIFY(QFile::remove(c));
    QVERIFY(Session::save(paths, SessionState{{WindowState{{{QString(), c, 0, 0, 0}}, 0, true, {}}}}));
    Backend restored(paths);
    restored.start({});
    TabModel *tabs = restored.windows().first()->tabs();
    QCOMPARE(tabs->activeStatus(), QStringLiteral("File not found"));
    writeFile(c, "back\n");
    restored.openFiles({c});
    QCOMPARE(tabs->rowCount(), 1);
    QVERIFY(tabs->activeDocument());
    QCOMPARE(tabs->activeDocument()->buffer()->text(), QStringLiteral("back\n"));
}

namespace {
// What one window's library is made from, on temporary paths.
struct LibraryFixture {
    QTemporaryDir root;
    AppPaths paths = AppPaths::under(root.path());
    DocumentStore store{paths};
    RecentFiles recent{paths};
    Settings settings{paths.settingsFile()};
    // The library's own pool, which outlives the model.
    QThreadPool pool;
    LibraryModel model{paths, &store, &recent, &settings, &pool};

    // A folder beneath the root, by the kernel's name for it.
    QString folder(const QString &name) const { return canonical(root.path()) + u'/' + name; }
};

QStringList namesUnder(const QAbstractItemModel &model, const QModelIndex &parent = {})
{
    QStringList names;
    for (int row = 0; row < model.rowCount(parent); ++row)
        names << model.index(row, 0, parent).data(LibraryModel::NameRole).toString();
    return names;
}

QStringList kindsUnder(const QAbstractItemModel &model, const QModelIndex &parent = {})
{
    QStringList kinds;
    for (int row = 0; row < model.rowCount(parent); ++row)
        kinds << model.index(row, 0, parent).data(LibraryModel::KindRole).toString();
    return kinds;
}

QModelIndex rowNamed(const QAbstractItemModel &model, const QString &name, const QModelIndex &parent = {})
{
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data(LibraryModel::NameRole).toString() == name) return index;
    }
    return {};
}

QStringList topLevelNames(const LibraryModel &model)
{
    return namesUnder(model);
}

// Expands a row as a view does, and waits until its folder is listed.
bool listed(LibraryModel &model, const QModelIndex &row)
{
    if (!row.isValid()) return false;
    const QPersistentModelIndex kept(row);
    if (model.canFetchMore(row)) model.fetchMore(row);
    model.setExpanded(row, true);
    return QTest::qWaitFor([&] {
        return kept.isValid() && !kept.data(LibraryModel::LoadingRole).toBool() && !model.canFetchMore(kept);
    }, 5000);
}

QStringList childNames(LibraryModel &model, const QString &topLevel)
{
    const QModelIndex row = rowNamed(model, topLevel);
    if (!listed(model, row)) return {QStringLiteral("<%1 not listed>").arg(topLevel)};
    return namesUnder(model, row);
}

QStringList childKinds(LibraryModel &model, const QString &topLevel)
{
    const QModelIndex row = rowNamed(model, topLevel);
    if (!listed(model, row)) return {QStringLiteral("<%1 not listed>").arg(topLevel)};
    return kindsUnder(model, row);
}

// The documents the library shows anywhere, as far as it has listed them.
QStringList visibleDocumentNames(const LibraryModel &model, const QModelIndex &parent = {})
{
    QStringList names;
    for (int row = 0; row < model.rowCount(parent); ++row) {
        const QModelIndex index = model.index(row, 0, parent);
        if (index.data(LibraryModel::KindRole).toString() == QStringLiteral("document"))
            names << index.data(LibraryModel::NameRole).toString();
        names += visibleDocumentNames(model, index);
    }
    return names;
}

// Lets the work queued on a pool finish and its results come back.
void drainPool(QThreadPool *pool)
{
    pool->waitForDone();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
} // namespace

void ParchTests::libraryListsDraftsRecentAndFolders()
{
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    QVERIFY(QDir().mkpath(f.folder(QStringLiteral("Essays"))));
    f.model.addFolder(f.folder(QStringLiteral("Essays")));
    QCOMPARE(topLevelNames(f.model), (QStringList{"Drafts", "Recent", "Essays"}));
    QCOMPARE(kindsUnder(f.model), (QStringList{"section", "section", "folder"}));
    const QStringList roles = [&] {
        QStringList names;
        for (const QByteArray &name : f.model.roleNames()) names << QString::fromLatin1(name);
        return names;
    }();
    for (const char *role : {"name", "path", "kind", "missing"}) QVERIFY2(roles.contains(QLatin1String(role)), role);
    // A folder is not looked in until it is first expanded.
    const QModelIndex essays = rowNamed(f.model, QStringLiteral("Essays"));
    QCOMPARE(essays.data(LibraryModel::PathRole).toString(), f.folder(QStringLiteral("Essays")));
    QCOMPARE(f.model.rowCount(essays), 0);
    QVERIFY(f.model.canFetchMore(essays));
    QVERIFY(f.model.hasChildren(essays));
    // An open draft is in Drafts, and a noted file in Recent.
    Document *draft = f.store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("# Opening lines"));
    const QString file = f.folder(QStringLiteral("Essays/a.md"));
    writeFile(file, "a\n");
    f.recent.note(file);
    QCOMPARE(namesUnder(f.model, f.model.index(0, 0)), QStringList{"Opening lines"});
    QCOMPARE(kindsUnder(f.model, f.model.index(0, 0)), QStringList{"draft"});
    QCOMPARE(f.model.index(0, 0, f.model.index(0, 0)).data(LibraryModel::DocumentIdRole).toString(), draft->id());
    QCOMPARE(namesUnder(f.model, f.model.index(1, 0)), QStringList{"a.md"});
    QCOMPARE(f.model.index(0, 0, f.model.index(1, 0)).data(LibraryModel::PathRole).toString(), file);
    // An index of another model, as QML could pass one, names nothing here.
    QStandardItemModel other;
    other.appendRow(new QStandardItem(QStringLiteral("other")));
    const QModelIndex foreign = other.index(0, 0);
    f.model.setExpanded(foreign, true);
    f.model.setExpanded(foreign, false);
    f.model.fetchMore(foreign);
    QVERIFY(!f.model.data(foreign, LibraryModel::NameRole).isValid());
    QCOMPARE(f.model.rowCount(foreign), 0);
    QVERIFY(!f.model.hasChildren(foreign));
    QVERIFY(!f.model.canFetchMore(foreign));
    QVERIFY(!f.model.parent(foreign).isValid());
}

void ParchTests::folderShowsOnlyTextDocumentsAndSubfolders()
{
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString essays = f.folder(QStringLiteral("Essays"));
    for (const char *name : {"a.md", "b.markdown", "c.txt", "d.png", ".hidden.md", "Sub/Zed.md", "Sub/apple.md",
                             "Sub/Banana.md", "Sub/chapter 10.md", "Sub/chapter 2.md", ".git/config.md"})
        writeFile(essays + u'/' + QString::fromLatin1(name), "x\n");
    f.model.addFolder(essays);
    QCOMPARE(childNames(f.model, "Essays"), (QStringList{"Sub", "a.md", "b.markdown", "c.txt"}));
    const QModelIndex folder = rowNamed(f.model, QStringLiteral("Essays"));
    QCOMPARE(kindsUnder(f.model, folder), (QStringList{"folder", "document", "document", "document"}));
    QCOMPARE(f.model.index(1, 0, folder).data(LibraryModel::PathRole).toString(), essays + QStringLiteral("/a.md"));
    // Ignoring case, and numbers in order.
    const QModelIndex sub = rowNamed(f.model, QStringLiteral("Sub"), folder);
    QVERIFY(listed(f.model, sub));
    QCOMPARE(namesUnder(f.model, sub),
             (QStringList{"apple.md", "Banana.md", "chapter 2.md", "chapter 10.md", "Zed.md"}));
}

void ParchTests::filterNarrowsByNameIgnoringCase()
{
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString essays = f.folder(QStringLiteral("Essays")), research = f.folder(QStringLiteral("Research"));
    writeFile(essays + QStringLiteral("/A room for thought.md"), "x\n");
    writeFile(essays + QStringLiteral("/Ordinary things.md"), "x\n");
    // Not listed yet: the filter lists it, in the background.
    writeFile(essays + QStringLiteral("/Notes/Roomy.md"), "x\n");
    writeFile(research + QStringLiteral("/Sources.md"), "x\n");
    f.model.addFolder(essays);
    f.model.addFolder(research);
    QVERIFY(listed(f.model, rowNamed(f.model, QStringLiteral("Essays"))));
    QVERIFY(listed(f.model, rowNamed(f.model, QStringLiteral("Research"))));
    f.recent.note(research + QStringLiteral("/Sources.md"));
    Document *draft = f.store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("Roommates and others\n"));
    f.store.newDraft();
    QSignalSpy resets(&f.model, &QAbstractItemModel::modelReset);
    // Every row that comes and goes is announced as Qt's models must.
    QAbstractItemModelTester tester(&f.model, QAbstractItemModelTester::FailureReportingMode::QtTest);

    f.model.setFilter("ROOM");
    QCOMPARE(visibleDocumentNames(f.model), QStringList{"A room for thought.md"});
    // The sections stay, with what matches in them, and so does the folder
    // that holds a match; the other folder goes.
    QCOMPARE(topLevelNames(f.model), (QStringList{"Drafts", "Recent", "Essays"}));
    QCOMPARE(namesUnder(f.model, f.model.index(0, 0)), QStringList{"Roommates and others"});
    QCOMPARE(namesUnder(f.model, f.model.index(1, 0)), QStringList());
    QCOMPARE(namesUnder(f.model, rowNamed(f.model, QStringLiteral("Essays"))), QStringList{"A room for thought.md"});
    QCOMPARE(f.model.matches().size(), 2);
    QTRY_COMPARE(visibleDocumentNames(f.model), (QStringList{"Roomy.md", "A room for thought.md"}));
    QCOMPARE(f.model.matches().size(), 3);
    // A draft retitled into the filter, or out of it, follows.
    draft->buffer()->replace({0, 4}, QStringLiteral("Bed"));
    QCOMPARE(namesUnder(f.model, f.model.index(0, 0)), QStringList());
    // A folder whose own name matches keeps what it holds.
    f.model.setFilter(QStringLiteral("search"));
    QCOMPARE(topLevelNames(f.model), (QStringList{"Drafts", "Recent", "Research"}));
    QCOMPARE(namesUnder(f.model, rowNamed(f.model, QStringLiteral("Research"))), QStringList{"Sources.md"});
    // No filter shows everything again.
    f.model.setFilter({});
    QCOMPARE(topLevelNames(f.model), (QStringList{"Drafts", "Recent", "Essays", "Research"}));
    QCOMPARE(namesUnder(f.model, rowNamed(f.model, QStringLiteral("Essays"))),
             (QStringList{"Notes", "A room for thought.md", "Ordinary things.md"}));
    QCOMPARE(namesUnder(f.model, f.model.index(0, 0)).size(), 2);
    QCOMPARE(namesUnder(f.model, f.model.index(1, 0)), QStringList{"Sources.md"});
    QVERIFY(f.model.matches().isEmpty());
    // Rows come and go one by one, so a view keeps what it has open.
    QCOMPARE(resets.count(), 0);
}

void ParchTests::searchListsFoldersNotListedYet()
{
    // With a filter, the folders beneath the library's folders that are not
    // listed yet are listed in the background, and what they hold matches.
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString writing = f.folder(QStringLiteral("Writing"));
    writeFile(writing + QStringLiteral("/Essays/Deep/A room for thought.md"), "x\n");
    writeFile(writing + QStringLiteral("/Essays/Other.md"), "x\n");
    writeFile(writing + QStringLiteral("/.hidden/Roomy secret.md"), "x\n");
    // A link that leads back into a folder listed already is not listed again.
    QVERIFY(QFile::link(writing, writing + QStringLiteral("/Essays/back")));
    f.model.addFolder(writing);
    QAbstractItemModelTester tester(&f.model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QCOMPARE(namesUnder(f.model, rowNamed(f.model, QStringLiteral("Writing"))), QStringList());
    f.model.setFilter(QStringLiteral("room"));
    QVERIFY(f.model.property("searching").toBool());
    QTRY_COMPARE(visibleDocumentNames(f.model), QStringList{QStringLiteral("A room for thought.md")});
    QTRY_VERIFY(!f.model.property("searching").toBool());
    QCOMPARE(visibleDocumentNames(f.model), QStringList{QStringLiteral("A room for thought.md")});
    QCOMPARE(f.model.matches().size(), 1);
    QCOMPARE(f.model.matchCount(), 1);
    // Listed only for the filter, nothing is watched.
    QVERIFY(f.model.watchedFolders().isEmpty());
    // A new filter takes up from there, and stops what the old one had left.
    f.model.setFilter(QStringLiteral("other"));
    QTRY_VERIFY(!f.model.property("searching").toBool());
    QCOMPARE(visibleDocumentNames(f.model), QStringList{QStringLiteral("Other.md")});
    QVERIFY(f.model.watchedFolders().isEmpty());
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
}

void ParchTests::searchListingIsBounded()
{
    // At most eight folders deep below a library folder, and at most 2,000
    // folders for one filter, whichever library folder they are in.
    {
        LibraryFixture f;
        QVERIFY(f.root.isValid());
        const QString deep = f.folder(QStringLiteral("Deep"));
        QString folder = deep;
        for (int depth = 1; depth <= 10; ++depth) {
            folder += QStringLiteral("/d%1").arg(depth);
            writeFile(folder + QStringLiteral("/room at %1.md").arg(depth), "x\n");
        }
        f.model.addFolder(deep);
        f.model.setFilter(QStringLiteral("room"));
        QTRY_VERIFY(!f.model.property("searching").toBool());
        const QStringList found = visibleDocumentNames(f.model);
        for (int depth = 1; depth <= 8; ++depth)
            QVERIFY2(found.contains(QStringLiteral("room at %1.md").arg(depth)), qPrintable(found.join(", ")));
        QVERIFY2(!found.contains(QStringLiteral("room at 9.md")), qPrintable(found.join(", ")));
    }
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString wide = f.folder(QStringLiteral("Wide"));
    for (int i = 1; i <= 2100; ++i) QVERIFY(QDir().mkpath(wide + QStringLiteral("/f%1").arg(i, 4, 10, QLatin1Char('0'))));
    writeFile(wide + QStringLiteral("/f0001/room early.md"), "x\n");
    writeFile(wide + QStringLiteral("/f2100/room late.md"), "x\n");
    f.model.addFolder(wide);
    f.model.setFilter(QStringLiteral("room"));
    QTRY_VERIFY_WITH_TIMEOUT(!f.model.property("searching").toBool(), 30000);
    const QStringList found = visibleDocumentNames(f.model);
    QVERIFY(found.contains(QStringLiteral("room early.md")));
    QVERIFY(!found.contains(QStringLiteral("room late.md")));
    QCOMPARE(f.model.property("searchListings").toInt(), 2000);
}

void ParchTests::searchListsAgainAfterTheFieldIsEmptied()
{
    // A folder listed only for a search is unlisted again once the field is
    // emptied, so that a later search sees what has appeared since. A folder
    // the view has open then stays listed, and is watched.
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString writing = f.folder(QStringLiteral("Writing"));
    writeFile(writing + QStringLiteral("/Essays/A room.md"), "x\n");
    writeFile(writing + QStringLiteral("/Notes/Plain.md"), "x\n");
    f.model.addFolder(writing);
    f.model.setFilter(QStringLiteral("room"));
    QTRY_VERIFY(!f.model.property("searching").toBool());
    QCOMPARE(visibleDocumentNames(f.model), QStringList{QStringLiteral("A room.md")});
    // The view opens Writing, as it does to show a match.
    const QPersistentModelIndex top = rowNamed(f.model, QStringLiteral("Writing"));
    f.model.setExpanded(top, true);
    f.model.setFilter({});
    QTRY_VERIFY(f.model.watchedFolders().contains(writing));
    const QModelIndex notes = rowNamed(f.model, QStringLiteral("Notes"), top);
    QVERIFY(notes.isValid());
    QTRY_VERIFY(f.model.canFetchMore(rowNamed(f.model, QStringLiteral("Notes"), top)));
    QVERIFY(f.model.canFetchMore(rowNamed(f.model, QStringLiteral("Essays"), top)));
    QVERIFY(!f.model.watchedFolders().contains(writing + QStringLiteral("/Notes")));
    f.model.setExpanded(top, false);
    // What appears afterwards, in a folder listed before or in a new one, is found.
    writeFile(writing + QStringLiteral("/Notes/New idea.md"), "x\n");
    writeFile(writing + QStringLiteral("/Later/Idea two.md"), "x\n");
    f.model.setFilter(QStringLiteral("idea"));
    QVERIFY(f.model.property("searching").toBool());
    QTRY_VERIFY(!f.model.property("searching").toBool());
    QStringList found = visibleDocumentNames(f.model);
    found.sort();
    QCOMPARE(found, (QStringList{QStringLiteral("Idea two.md"), QStringLiteral("New idea.md")}));
}

void ParchTests::anOlderSearchsListingsHoldNothingUp()
{
    // Listings an older filter started that are still out, as a folder that
    // never answers leaves them, count against none of the jobs a newer
    // filter may run: the folders they are for are passed over. They count
    // as searching, since what they list is used.
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    for (const char *name : {"A", "B", "C"}) {
        const QString root = f.folder(QString::fromLatin1(name));
        writeFile(root + QStringLiteral("/note.md"), "x\n");
        f.model.addFolder(root);
    }
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
    {
        // Every listing waits, as on folders that never answer.
        PoolBlock block(&f.pool);
        f.model.setFilter(QStringLiteral("n"));
        QCOMPARE(f.model.jobsInFlight(), LibraryModel::SearchJobs);
        // A and B are still being listed for "n": "no" passes them over and lists C.
        f.model.setFilter(QStringLiteral("no"));
        QCOMPARE(f.model.jobsInFlight(), LibraryModel::SearchJobs + 1);
        QVERIFY(f.model.searching());
        // Every folder is out for an older filter: "not" lists nothing of
        // its own, and waits for those.
        f.model.setFilter(QStringLiteral("not"));
        QVERIFY(f.model.searching());
        QCOMPARE(f.model.jobsInFlight(), LibraryModel::SearchJobs + 1);
    }
    // The older listings, answered at last, are used all the same, since the
    // field kept its text.
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
    QVERIFY(!f.model.searching());
    QStringList found = visibleDocumentNames(f.model);
    QCOMPARE(found, (QStringList{"note.md", "note.md", "note.md"}));
}

void ParchTests::aListingFromBeforeTheFieldWasEmptiedIsUsed()
{
    // A listing that lands while the field has text is used, whatever filter
    // asked for it, even one from before the field was emptied: the folder
    // it is for is then listed.
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    for (const char *name : {"A", "B", "C"}) {
        const QString root = f.folder(QString::fromLatin1(name));
        writeFile(root + QStringLiteral("/note.md"), "x\n");
        f.model.addFolder(root);
    }
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
    {
        PoolBlock block(&f.pool);
        f.model.setFilter(QStringLiteral("n"));
        f.model.setFilter({});
        f.model.setFilter(QStringLiteral("note"));
    }
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
    QTRY_VERIFY(!f.model.searching());
    QCOMPARE(visibleDocumentNames(f.model), (QStringList{"note.md", "note.md", "note.md"}));
}

void ParchTests::aProbeThatNeverAnswersStopsSearchingInTime()
{
    // A library folder whose look never answers, as an unreachable mount's,
    // keeps the field searching for SearchingMs at most; what lands later
    // is still used.
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString root = f.folder(QStringLiteral("Mount"));
    writeFile(root + QStringLiteral("/note.md"), "x\n");
    QElapsedTimer clock;
    {
        PoolBlock block(&f.pool);
        f.model.addFolder(root);
        f.model.setFilter(QStringLiteral("note"));
        clock.start();
        QVERIFY(f.model.searching());
        QTest::qWait(LibraryModel::SearchingMs / 2);
        QVERIFY(f.model.searching());
        QTRY_VERIFY_WITH_TIMEOUT(!f.model.searching(), LibraryModel::SearchingMs);
        QVERIFY2(clock.elapsed() >= LibraryModel::SearchingMs - 100, qPrintable(QString::number(clock.elapsed())));
    }
    QTRY_COMPARE(visibleDocumentNames(f.model), QStringList{QStringLiteral("note.md")});
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
}

void ParchTests::searchFollowsFoldersAddedAndRemoved()
{
    // While the field has text, a folder added to the library is searched,
    // and what waited to be listed beneath a folder taken out goes.
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    QStringList roots;
    for (const char *name : {"A", "B", "C", "D"}) {
        roots << f.folder(QString::fromLatin1(name));
        writeFile(roots.last() + QStringLiteral("/plain.md"), "x\n");
        f.model.addFolder(roots.last());
    }
    QTRY_COMPARE(f.model.jobsInFlight(), 0);
    {
        PoolBlock block(&f.pool);
        f.model.setFilter(QStringLiteral("idea"));
        // A and B are being listed; C and D wait.
        QCOMPARE(f.model.searchQueued(), 2);
        f.model.removeFolder(roots.at(2));
        QCOMPARE(f.model.searchQueued(), 1);
    }
    QTRY_VERIFY(!f.model.searching());
    const QString added = f.folder(QStringLiteral("Added"));
    writeFile(added + QStringLiteral("/deep/An idea.md"), "x\n");
    f.model.addFolder(added);
    QVERIFY(f.model.searching());
    QTRY_COMPARE(visibleDocumentNames(f.model), QStringList{QStringLiteral("An idea.md")});
    QTRY_VERIFY(!f.model.searching());
}

void ParchTests::unreadableFolderShowsErrorRow()
{
    if (::geteuid() == 0) QSKIP("root reads every folder");
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString locked = f.folder(QStringLiteral("Locked"));
    writeFile(locked + QStringLiteral("/secret.md"), "x\n");
    QVERIFY(QFile::setPermissions(locked, QFileDevice::Permissions()));
    const auto restore = qScopeGuard([&] {
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    });
    f.model.addFolder(locked);
    QCOMPARE(childKinds(f.model, "Locked"), QStringList{"error"});
    const QString reason = childNames(f.model, QStringLiteral("Locked")).value(0);
    QVERIFY2(reason.startsWith(QStringLiteral("Cannot be read: ")), qPrintable(reason));
    QVERIFY(!rowNamed(f.model, QStringLiteral("Locked")).data(LibraryModel::MissingRole).toBool());
}

void ParchTests::recentPersistsAndFlagsMissingFiles()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString fileA = root.filePath(QStringLiteral("a.md")), fileB = root.filePath(QStringLiteral("b.md"));
    writeFile(fileA, "a\n");
    writeFile(fileB, "b\n");
    {
        RecentFiles recent(paths);
        recent.note(fileA);  recent.note(fileB);  QFile::remove(fileA);
    }
    RecentFiles reloaded(paths);
    QCOMPARE(reloaded.paths(), (QStringList{fileB, fileA}));
    QVERIFY(reloaded.missing(fileA));
    QVERIFY(!reloaded.missing(fileB));
    // Noted again, a file moves to the front, once; only full paths are taken.
    QSignalSpy changed(&reloaded, &RecentFiles::changed);
    reloaded.note(fileB);
    reloaded.note(QStringLiteral("relative.md"));
    QCOMPARE(changed.count(), 0);
    reloaded.note(fileA);
    QCOMPARE(reloaded.paths(), (QStringList{fileA, fileB}));
    // Fifty at most, the newest first.
    for (int i = 0; i < 60; ++i) reloaded.note(root.filePath(QStringLiteral("n%1.md").arg(i)));
    QCOMPARE(reloaded.paths().size(), RecentFiles::MaxPaths);
    QCOMPARE(reloaded.paths().first(), root.filePath(QStringLiteral("n59.md")));
    QCOMPARE(reloaded.paths().last(), root.filePath(QStringLiteral("n10.md")));
    // The file is written off the UI thread: noting returns before it changes.
    QTRY_COMPARE(RecentFiles(paths).paths(), reloaded.paths());
    {
        PoolBlock block;
        RecentFiles idle(paths);
        idle.note(fileB);
        QCOMPARE(idle.paths().first(), fileB);
        QCOMPARE(RecentFiles(paths).paths().first(), root.filePath(QStringLiteral("n59.md")));
        block.release();
    }
    QCOMPARE(RecentFiles(paths).paths().first(), fileB);

    // A damaged file is moved aside, never written over, and costs only the list.
    writeFile(paths.recentFile(), "{ not json");
    {
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("parch: .*recent files")));
        RecentFiles damaged(paths);
        QVERIFY(damaged.paths().isEmpty());
        QCOMPARE(readFile(paths.recentFile() + QStringLiteral(".bad")), QByteArray("{ not json"));
        damaged.note(fileB);
    }
    QCOMPARE(RecentFiles(paths).paths(), QStringList{fileB});
}

void ParchTests::libraryFoldersPersistAndGoneOnesStay()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString base = canonical(root.path());
    const QString kept = base + QStringLiteral("/Kept"), gone = base + QStringLiteral("/Gone"),
                  dropped = base + QStringLiteral("/Dropped");
    for (const QString &folder : {kept, gone, dropped}) QVERIFY(QDir().mkpath(folder));
    QVERIFY(QFile::link(kept, base + QStringLiteral("/link")));
    {
        DocumentStore store(paths);
        RecentFiles recent(paths);
        Settings settings(paths.settingsFile());
        QThreadPool pool;
        LibraryModel model(paths, &store, &recent, &settings, &pool);
        model.addFolder(kept);
        model.addFolder(gone);
        model.addFolder(dropped);
        // A folder is listed once, by whatever path it is added.
        model.addFolder(kept);
        model.addFolder(base + QStringLiteral("/link"));
        model.addFolder(QStringLiteral("relative"));
        QCOMPARE(topLevelNames(model), (QStringList{"Drafts", "Recent", "Kept", "Gone", "Dropped"}));
        model.removeFolder(dropped);
        QCOMPARE(topLevelNames(model), (QStringList{"Drafts", "Recent", "Kept", "Gone"}));
    }
    const QJsonObject stored = QJsonDocument::fromJson(readFile(paths.settingsFile())).object();
    QCOMPARE(stored.value(QStringLiteral("libraryFolders")).toVariant().toStringList(), (QStringList{kept, gone}));
    QVERIFY(QDir(gone).removeRecursively());
    {
        DocumentStore store(paths);
        RecentFiles recent(paths);
        Settings settings(paths.settingsFile());
        QThreadPool pool;
        LibraryModel model(paths, &store, &recent, &settings, &pool);
        QCOMPARE(topLevelNames(model), (QStringList{"Drafts", "Recent", "Kept", "Gone"}));
        // A folder that has gone stays, and says so before it is expanded.
        QTRY_VERIFY(rowNamed(model, QStringLiteral("Gone")).data(LibraryModel::MissingRole).toBool());
        QVERIFY(!rowNamed(model, QStringLiteral("Kept")).data(LibraryModel::MissingRole).toBool());
        QCOMPARE(childKinds(model, "Gone"), QStringList{"error"});
        QCOMPARE(childNames(model, QStringLiteral("Gone")), QStringList{"Folder not found"});
        // Back again, it is listed when expanded.
        writeFile(gone + QStringLiteral("/back.md"), "x\n");
        QCOMPARE(childNames(model, QStringLiteral("Gone")), QStringList{"back.md"});
        QVERIFY(!rowNamed(model, QStringLiteral("Gone")).data(LibraryModel::MissingRole).toBool());
        // Every window's library follows one change.
        LibraryModel other(paths, &store, &recent, &settings, &pool);
        QCOMPARE(topLevelNames(other), (QStringList{"Drafts", "Recent", "Kept", "Gone"}));
        model.removeFolder(gone);
        QCOMPARE(topLevelNames(other), (QStringList{"Drafts", "Recent", "Kept"}));
    }
    // Folders written by hand that are no full paths are left out, and said to be.
    writeFile(paths.settingsFile(), QJsonDocument(QJsonObject{{"libraryFolders", QJsonArray{"relative", 3, kept}}}).toJson());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("parch: .*libraryFolders")));
    QCOMPARE(Settings(paths.settingsFile()).libraryFolders(), QStringList{kept});
}

void ParchTests::expandedFoldersFollowTheDisk()
{
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString essays = f.folder(QStringLiteral("Essays"));
    writeFile(essays + QStringLiteral("/a.md"), "a\n");
    writeFile(essays + QStringLiteral("/b.md"), "b\n");
    writeFile(essays + QStringLiteral("/Deeper/d.md"), "d\n");
    f.model.addFolder(essays);
    const QPersistentModelIndex folder = rowNamed(f.model, QStringLiteral("Essays"));
    QVERIFY(listed(f.model, folder));
    QCOMPARE(namesUnder(f.model, folder), (QStringList{"Deeper", "a.md", "b.md"}));
    QCOMPARE(f.model.watchedFolders(), QStringList{essays});
    QAbstractItemModelTester tester(&f.model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QPersistentModelIndex b = rowNamed(f.model, QStringLiteral("b.md"), folder);
    // Files that appear, go or are renamed by others show as they are.
    writeFile(essays + QStringLiteral("/c.md"), "c\n");
    QTRY_COMPARE(namesUnder(f.model, folder), (QStringList{"Deeper", "a.md", "b.md", "c.md"}));
    QVERIFY(QFile::remove(essays + QStringLiteral("/a.md")));
    QTRY_COMPARE(namesUnder(f.model, folder), (QStringList{"Deeper", "b.md", "c.md"}));
    QVERIFY(QFile::rename(essays + QStringLiteral("/c.md"), essays + QStringLiteral("/Renamed.md")));
    QTRY_COMPARE(namesUnder(f.model, folder), (QStringList{"Deeper", "b.md", "Renamed.md"}));
    // A row that stays is the same row throughout.
    QVERIFY(b.isValid());
    QCOMPARE(b.data(LibraryModel::NameRole).toString(), QStringLiteral("b.md"));
    // A burst of changes ends in a listing with all of it.
    for (int i = 0; i < 100; ++i) writeFile(essays + QStringLiteral("/burst %1.md").arg(i), "x\n");
    QTRY_COMPARE(f.model.rowCount(folder), 103);
    // What parch itself names into the folder shows too.
    Document *draft = f.store.newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("named"));
    f.store.saveAs(draft, essays + QStringLiteral("/Named.md"));
    QTRY_VERIFY(namesUnder(f.model, folder).contains(QStringLiteral("Named.md")));
    // A folder expanded within is watched too, until the one around it is collapsed.
    const QModelIndex deeper = rowNamed(f.model, QStringLiteral("Deeper"), folder);
    QVERIFY(listed(f.model, deeper));
    QCOMPARE(namesUnder(f.model, deeper), QStringList{"d.md"});
    const QStringList watched = f.model.watchedFolders();
    QCOMPARE(QSet<QString>(watched.begin(), watched.end()), (QSet<QString>{essays, essays + QStringLiteral("/Deeper")}));
    f.model.setExpanded(folder, false);
    QVERIFY(f.model.watchedFolders().isEmpty());
    // Expanded again, it is listed afresh.
    writeFile(essays + QStringLiteral("/later.md"), "x\n");
    QVERIFY(listed(f.model, folder));
    QVERIFY(namesUnder(f.model, folder).contains(QStringLiteral("later.md")));
    QCOMPARE(f.model.watchedFolders(), QStringList{essays});
    // A folder removed from the library is no longer watched.
    f.model.removeFolder(essays);
    QVERIFY(f.model.watchedFolders().isEmpty());
}

void ParchTests::listingStaysOffTheUiThread()
{
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString big = f.folder(QStringLiteral("Big"));
    QVERIFY(QDir().mkpath(big));
    for (int i = 0; i < 5000; ++i) {
        QFile file(big + QStringLiteral("/note %1.md").arg(i));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }
    f.model.addFolder(big);
    const QPersistentModelIndex folder = rowNamed(f.model, QStringLiteral("Big"));
    QTRY_VERIFY(!folder.data(LibraryModel::LoadingRole).toBool());
    {
        // With no worker free, nothing is listed: the listing never happens on this thread.
        PoolBlock block(&f.pool);
        f.model.fetchMore(folder);
        f.model.setExpanded(folder, true);
        QCoreApplication::processEvents();
        QCOMPARE(f.model.rowCount(folder), 0);
        QVERIFY(folder.data(LibraryModel::LoadingRole).toBool());
        // Collapsed before its listing ends, the folder takes none of it.
        f.model.setExpanded(folder, false);
        QVERIFY(!folder.data(LibraryModel::LoadingRole).toBool());
        block.release();
        drainPool(&f.pool);
        QCOMPARE(f.model.rowCount(folder), 0);
        QVERIFY(f.model.canFetchMore(folder));
    }
    // Thousands of entries arrive as one insertion.
    QSignalSpy inserted(&f.model, &QAbstractItemModel::rowsInserted);
    QVERIFY(listed(f.model, folder));
    QCOMPARE(f.model.rowCount(folder), 5000);
    int intoFolder = 0;
    for (const QList<QVariant> &signal : std::as_const(inserted))
        intoFolder += signal.at(0).value<QModelIndex>() == folder ? 1 : 0;
    QCOMPARE(intoFolder, 1);
    {
        // Removed while it is listed again, the folder takes nothing either.
        PoolBlock block(&f.pool);
        f.model.setExpanded(folder, false);
        f.model.setExpanded(folder, true);
        f.model.removeFolder(big);
        block.release();
        drainPool(&f.pool);
        QCOMPARE(topLevelNames(f.model), (QStringList{"Drafts", "Recent"}));
    }
}

void ParchTests::draftsFollowCreationNamingAndRemoval()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString kept = newUuid();
    writeFile(DocumentStore::draftFile(paths, kept), "\n# A kept draft\n\nwith words");
    DocumentStore store(paths);
    RecentFiles recent(paths);
    Settings settings(paths.settingsFile());
    QThreadPool pool;
    LibraryModel model(paths, &store, &recent, &settings, &pool);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QModelIndex drafts = model.index(0, 0);
    // Titled as its document would be.
    QTRY_COMPARE(namesUnder(model, drafts), QStringList{"A kept draft"});
    // An open draft is listed at once, the newest first, and follows its title.
    Document *draft = store.newDraft();
    QCOMPARE(namesUnder(model, drafts), (QStringList{"Untitled", "A kept draft"}));
    draft->buffer()->replace({0, 0}, QStringLiteral("## Fresh words\nmore"));
    QCOMPARE(namesUnder(model, drafts), (QStringList{"Fresh words", "A kept draft"}));
    QCOMPARE(model.index(0, 0, drafts).data(LibraryModel::DocumentIdRole).toString(), draft->id());
    store.save(draft);
    QTRY_VERIFY(QFile::exists(DocumentStore::draftFile(paths, draft->id())));
    // Named, it is a draft no more.
    store.saveAs(draft, root.filePath(QStringLiteral("Fresh.md")));
    QTRY_COMPARE(namesUnder(model, drafts), QStringList{"A kept draft"});
    // A draft folder a named document left behind is no draft, a draft
    // removed from the disk goes, and one that appears comes.
    writeFile(DocumentStore::draftFile(paths, draft->id()), "Left behind");
    QVERIFY(QDir(paths.drafts() + u'/' + kept).removeRecursively());
    const QString later = newUuid();
    writeFile(DocumentStore::draftFile(paths, later), "Later");
    QTRY_COMPARE(namesUnder(model, drafts), QStringList{"Later"});
    // A draft opened and closed again stays, since its file is there.
    Document *opened = store.openDraft(later);
    QVERIFY(opened);
    store.retain(opened);
    store.release(opened);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!store.documents().contains(opened));
    QTRY_COMPARE(namesUnder(model, drafts), QStringList{"Later"});
}

void ParchTests::recentIsNotedOnOpenSaveAndName()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString base = canonical(root.path());
    const QString a = base + QStringLiteral("/a.md"), b = base + QStringLiteral("/b.md"),
                  named = base + QStringLiteral("/named.md");
    writeFile(a, "a\n");
    writeFile(b, "b\n");
    {
        Backend backend(paths);
        backend.start({});
        RecentFiles *recent = backend.recentFiles();
        QVERIFY(recent->paths().isEmpty());
        backend.openFiles({a});
        QCOMPARE(recent->paths(), QStringList{a});
        backend.openFiles({b});
        QCOMPARE(recent->paths(), (QStringList{b, a}));
        // Saved, a file is the newest again.
        Document *first = backend.store()->open(a);
        first->buffer()->replace({0, 0}, QStringLiteral("more "));
        backend.store()->save(first);
        QTRY_COMPARE(recent->paths(), (QStringList{a, b}));
        // A draft that is named is noted by its name.
        backend.newDraft();
        Document *draft = backend.document();
        draft->buffer()->replace({0, 0}, QStringLiteral("words"));
        backend.store()->saveAs(draft, named);
        QTRY_COMPARE(recent->paths(), (QStringList{named, a, b}));
        // The window's library shows them, the newest first.
        LibraryModel *library = backend.windows().first()->library();
        QCOMPARE(namesUnder(*library, library->index(1, 0)), (QStringList{"named.md", "a.md", "b.md"}));
    }
    QCOMPARE(RecentFiles(paths).paths(), (QStringList{named, a, b}));
}

void ParchTests::libraryRowsOpenThroughTheBackend()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString base = canonical(root.path());
    const QString a = base + QStringLiteral("/a.md"), b = base + QStringLiteral("/b.md"),
                  c = base + QStringLiteral("/c.md"), gone = base + QStringLiteral("/gone.md");
    writeFile(a, "alpha\n");
    writeFile(b, "beta\n");
    writeFile(c, "gamma\n");
    const QString waiting = newUuid(), kept = newUuid();
    writeFile(DocumentStore::draftFile(paths, waiting), "Waiting draft");
    writeFile(DocumentStore::draftFile(paths, kept), "Kept draft");
    // The first window shows b in front of a file and a draft not read yet;
    // the second shows c.
    QVERIFY(Session::save(paths, SessionState{{WindowState{{{QString(), b, 0, 0, 0}, {QString(), a, 0, 0, 0},
                                                             {waiting, QString(), 0, 0, 0}}, 0, true, {}},
                                               WindowState{{{QString(), c, 0, 0, 0}}, 0, true, {}}}}));
    Backend backend(paths);
    backend.start({});
    WindowContext *first = backend.windows().at(0), *second = backend.windows().at(1);
    QCOMPARE(first->tabs()->documentAt(1), nullptr);
    QCOMPARE(first->tabs()->documentAt(2), nullptr);
    // A file with a tab comes to the front there, read now.
    backend.activate(second);
    QSignalSpy raisedFirst(first, &WindowContext::raiseRequested);
    backend.openFromLibrary(a);
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(first->tabs()->rowCount(), 3);
    QCOMPARE(first->tabs()->activeIndex(), 1);
    QCOMPARE(first->tabs()->activeDocument()->buffer()->text(), QStringLiteral("alpha\n"));
    QCOMPARE(raisedFirst.count(), 1);
    // So does a draft, by its id.
    backend.activate(second);
    backend.openDraft(waiting);
    QCOMPARE(first->tabs()->rowCount(), 3);
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(first->tabs()->activeIndex(), 2);
    QCOMPARE(first->tabs()->activeDocument()->id(), waiting);
    QCOMPARE(first->tabs()->activeDocument()->buffer()->text(), QStringLiteral("Waiting draft"));
    QCOMPARE(raisedFirst.count(), 2);
    // A draft no tab shows opens in the window in front.
    backend.activate(second);
    backend.openDraft(kept);
    QCOMPARE(second->tabs()->rowCount(), 2);
    QCOMPARE(second->tabs()->activeDocument()->buffer()->text(), QStringLiteral("Kept draft"));
    // A listed file that has gone is a tab that says so, and nothing is made,
    // though its folder is there.
    backend.openFromLibrary(gone);
    QCOMPARE(second->tabs()->activeDocument(), nullptr);
    QCOMPARE(second->tabs()->activeStatus(), QStringLiteral("File not found"));
    QCOMPARE(second->tabs()->activeTitle(), QStringLiteral("gone.md"));
    QVERIFY(!QFileInfo::exists(gone));
    // A draft that is no longer there says so.
    backend.openDraft(newUuid());
    QCOMPARE(backend.problem(), QStringLiteral("Draft not found"));
}

namespace {
// The row a window's library shows by this name, if it shows one. A row
// the tree has put aside for reuse is hidden from drawing but not marked
// invisible, and the tree lays its rows out again only before it draws, so
// a row counts only when the tree holds it at its place and the tree's
// index, which follows the model at once, has the same name there.
QQuickItem *libraryRow(QQuickWindow *window, const QString &name)
{
    for (QQuickItem *item : visibleItems(window)) {
        if (item->objectName() != QStringLiteral("libraryRow") || item->property("name").toString() != name) continue;
        QObject *tree = item->property("treeView").value<QObject *>();
        if (!tree) continue;
        const int row = item->property("row").toInt();
        QQuickItem *held = nullptr;
        QMetaObject::invokeMethod(tree, "itemAtCell", Q_RETURN_ARG(QQuickItem *, held), Q_ARG(QPoint, QPoint(0, row)));
        QModelIndex index;
        QMetaObject::invokeMethod(tree, "index", Q_RETURN_ARG(QModelIndex, index), Q_ARG(int, row), Q_ARG(int, 0));
        if (held == item && index.data(LibraryModel::NameRole).toString() == name) return item;
    }
    return nullptr;
}

// Where the tree has a row by this name, as its index finds it at once,
// before its items are laid out again; -1 for none.
int treeRow(QObject *tree, const QString &name)
{
    for (int row = 0;; ++row) {
        QModelIndex index;
        QMetaObject::invokeMethod(tree, "index", Q_RETURN_ARG(QModelIndex, index), Q_ARG(int, row), Q_ARG(int, 0));
        if (!index.isValid()) return -1;
        if (index.data(LibraryModel::NameRole).toString() == name) return row;
    }
}

// Whether the row by this name is shown and has the property set.
bool rowHas(QQuickWindow *window, const QString &name, const char *property)
{
    QQuickItem *row = libraryRow(window, name);
    return row && row->property(property).toBool();
}

// Whether any row by this name has the property set, where the library
// shows one name in more than one place, as in Recent and in its folder.
bool someRowHas(QQuickWindow *window, const QString &name, const char *property)
{
    for (QQuickItem *item : visibleItems(window))
        if (item->objectName() == QStringLiteral("libraryRow") && item->property("name").toString() == name
            && libraryRow(window, name) && item->property(property).toBool()) {
            QObject *tree = item->property("treeView").value<QObject *>();
            QQuickItem *held = nullptr;
            QMetaObject::invokeMethod(tree, "itemAtCell", Q_RETURN_ARG(QQuickItem *, held),
                                      Q_ARG(QPoint, QPoint(0, item->property("row").toInt())));
            if (held == item) return true;
        }
    return false;
}

// Settings that hold one library folder.
QByteArray librarySettings(const QString &folder)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("libraryFolders"), QJsonArray{folder}}}).toJson();
}
} // namespace

void ParchTests::libraryKeyboardFindsAndOpens()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString essays = canonical(root.path()) + QStringLiteral("/Essays");
    const QString room = essays + QStringLiteral("/A room for thought.md"), ordinary = essays + QStringLiteral("/Ordinary things.md");
    const QString research = canonical(root.path()) + QStringLiteral("/Research");
    writeFile(room, "# A room for thought\n");
    writeFile(ordinary, "Ordinary\n");
    writeFile(research + QStringLiteral("/Sources.md"), "Sources\n");
    ShellFixture shell({.settings = QJsonDocument(QJsonObject{{QStringLiteral("libraryFolders"), QJsonArray{research, essays}}}).toJson(),
                        .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QQuickItem *tree = shell.item(QStringLiteral("libraryTree"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QVERIFY(search && tree && editor);
    TabModel *tabs = shell.context()->tabs();
    LibraryModel *library = shell.context()->library();
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Essays")));

    // Tab goes from the field into the tree; End goes to the last row, Right
    // opens the folder, Down moves into it and Enter opens the document there.
    search->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(window->activeFocusItem(), tree);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "keyboard"));
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("A room for thought.md")));
    QCOMPARE(library->watchedFolders(), QStringList{essays});
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("A room for thought.md"), "keyboard"));
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(tabs->activePath(), room);
    QCOMPARE(window->activeFocusItem(), editor);
    // Its row is marked as the document in front.
    QTRY_VERIFY(rowHas(window, QStringLiteral("A room for thought.md"), "active"));
    QVERIFY(!rowHas(window, QStringLiteral("Ordinary things.md"), "active"));
    // Recent, empty until now, has opened to show it.
    QTRY_VERIFY(rowHas(window, QStringLiteral("Recent"), "expanded"));
    // The editor has focus, so plain keys type.
    typeText(window, QStringLiteral("Q ?"));
    QVERIFY2(shell.document()->buffer()->text().startsWith(QStringLiteral("Q ?")), qPrintable(shell.document()->buffer()->text()));

    // The field narrows the library as it is typed in; Down goes to the
    // first match and Enter opens it.
    QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, treeRow(tree, QStringLiteral("Research"))));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Sources.md")));
    QMetaObject::invokeMethod(tree, "collapseRecursively", Q_ARG(int, treeRow(tree, QStringLiteral("Essays"))));
    QTRY_VERIFY(!rowHas(window, QStringLiteral("Essays"), "expanded"));
    QCOMPARE(library->watchedFolders(), QStringList{research});
    search->forceActiveFocus(Qt::TabFocusReason);
    typeText(window, QStringLiteral("ordinary"));
    QCOMPARE(library->filter(), QStringLiteral("ordinary"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Ordinary things.md")));
    QVERIFY(!libraryRow(window, QStringLiteral("A room for thought.md")));
    QVERIFY(!libraryRow(window, QStringLiteral("Research")));
    // The folder the search opened to show its match is watched like any other.
    QCOMPARE(library->watchedFolders(), QStringList{essays});
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(window->activeFocusItem(), tree);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Ordinary things.md"), "keyboard"));
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(tabs->activePath(), ordinary);
    QCOMPARE(window->activeFocusItem(), editor);

    // Escape empties the field, which still holds the search, and goes back to the text.
    search->forceActiveFocus(Qt::TabFocusReason);
    QCOMPARE(search->property("text").toString(), QStringLiteral("ordinary"));
    QTest::keyClick(window, Qt::Key_Escape);
    QCOMPARE(search->property("text").toString(), QString());
    QCOMPARE(library->filter(), QString());
    QCOMPARE(window->activeFocusItem(), editor);
    // The folder the search hid is open again, and watched again, and the
    // one it opened is closed again, as before the search.
    QTRY_VERIFY(rowHas(window, QStringLiteral("Research"), "expanded"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Sources.md")));
    QTRY_VERIFY(library->watchedFolders().contains(research));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Essays")) && !rowHas(window, QStringLiteral("Essays"), "expanded"));
    QTRY_COMPARE(library->watchedFolders(), QStringList{research});

    // Left goes from a document out to its folder, and then closes it. The
    // search's listing of Essays went with it, so Essays is listed again.
    QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, treeRow(tree, QStringLiteral("Essays"))));
    QTRY_COMPARE(library->rowCount(rowNamed(*library, QStringLiteral("Essays"))), 2);
    tree->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Ordinary things.md"), "keyboard"));
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "keyboard"));
    QVERIFY(rowHas(window, QStringLiteral("Essays"), "expanded"));
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Essays")) && !rowHas(window, QStringLiteral("Essays"), "expanded"));
    QCOMPARE(library->watchedFolders(), QStringList{research});
    // Shift+F10 on a folder at the top offers to take it out of the library.
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "keyboard"));
    QTest::keyClick(window, Qt::Key_F10, Qt::ShiftModifier);
    QObject *menu = shell.object(QStringLiteral("libraryFolderMenu"));
    QVERIFY(menu);
    QTRY_VERIFY(menu->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!menu->property("visible").toBool());
    QCOMPARE(shell.settings()->libraryFolders(), QStringList{research});
    QTRY_VERIFY(!libraryRow(window, QStringLiteral("Essays")));
    // A section closed by hand stays closed as it fills.
    QMetaObject::invokeMethod(tree, "collapseRecursively", Q_ARG(int, treeRow(tree, QStringLiteral("Recent"))));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Recent")) && !rowHas(window, QStringLiteral("Recent"), "expanded"));
    shell.backend->openFiles({research + QStringLiteral("/Sources.md")});
    QCOMPARE(namesUnder(*library, library->index(1, 0)).value(0), QStringLiteral("Sources.md"));
    // What the window does about the new rows is done once its posted calls have run.
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Recent")));
    QVERIFY(!rowHas(window, QStringLiteral("Recent"), "expanded"));
    // Up from the first row goes back to the field.
    tree->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Home);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(window->activeFocusItem(), search);
}

void ParchTests::libraryDrawerClosesOnOpening()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString essays = canonical(root.path()) + QStringLiteral("/Essays");
    const QString room = essays + QStringLiteral("/A room for thought.md"), ordinary = essays + QStringLiteral("/Ordinary things.md");
    const QString longName = QStringLiteral("A title much longer than a narrow library can show on one line.md");
    writeFile(room, "# A room for thought\n");
    writeFile(ordinary, "Ordinary\n");
    writeFile(essays + u'/' + longName, "Long\n");
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell({.settings = librarySettings(essays), .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 360);
    activate(window);
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QVERIFY(drawer && editor);
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QQuickItem *tree = shell.item(QStringLiteral("libraryTree"));
    QVERIFY(search && tree);
    // Opened to find something, the library gives its field focus.
    QTRY_COMPARE(window->activeFocusItem(), search);
    // Escape empties the field and closes the drawer, giving the text focus.
    typeText(window, QStringLiteral("zz"));
    QTest::keyClick(window, Qt::Key_Escape);
    QCOMPARE(search->property("text").toString(), QString());
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTRY_COMPARE(window->activeFocusItem(), search);
    // A document opened from the drawer closes it, and the text has focus.
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(window->activeFocusItem(), tree);
    QTest::keyClick(window, Qt::Key_End);
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("A room for thought.md")));
    // A name too long for the drawer is cut short, and given whole to
    // assistive technology and in a tooltip.
    QQuickItem *longRow = nullptr;
    QTRY_VERIFY((longRow = libraryRow(window, longName)));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QCOMPARE(QAccessible::queryAccessibleInterface(longRow)->text(QAccessible::Name), longName);
    QTest::mouseMove(window, centreOf(longRow));
    QObject *tip = nullptr;
    for (QObject *object : longRow->findChildren<QObject *>())
        if (object->objectName() == QStringLiteral("libraryRowTip")) tip = object;
    QVERIFY(tip);
    QTRY_VERIFY(tip->property("visible").toBool());
    QCOMPARE(tip->property("text").toString(), longName);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(shell.context()->tabs()->activePath(), room);
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_COMPARE(window->activeFocusItem(), editor);
    // A click does the same.
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = libraryRow(window, QStringLiteral("Ordinary things.md"))));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(row));
    QCOMPARE(shell.context()->tabs()->activePath(), ordinary);
    QTRY_VERIFY(!drawer->property("visible").toBool());
    QTRY_COMPARE(window->activeFocusItem(), editor);
}

void ParchTests::searchOpensAndWatchesDeepFolders()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString essays = canonical(root.path()) + QStringLiteral("/Essays"), old = essays + QStringLiteral("/Old");
    writeFile(essays + QStringLiteral("/Room.md"), "Room\n");
    writeFile(old + QStringLiteral("/Earlier.md"), "Earlier\n");
    ShellFixture shell({.settings = librarySettings(essays), .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QObject *tree = shell.object(QStringLiteral("libraryTree"));
    QVERIFY(search && tree);
    LibraryModel *library = shell.context()->library();
    const auto watched = [&] {
        const QStringList folders = library->watchedFolders();
        return QSet<QString>(folders.begin(), folders.end());
    };
    // Essays and Old inside it are open.
    QTRY_VERIFY(treeRow(tree, QStringLiteral("Essays")) >= 0);
    QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, treeRow(tree, QStringLiteral("Essays"))));
    QTRY_VERIFY(treeRow(tree, QStringLiteral("Old")) >= 0);
    QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, treeRow(tree, QStringLiteral("Old"))));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Earlier.md")));
    QTRY_COMPARE(watched(), (QSet<QString>{essays, old}));
    // A search that matches nothing hides Essays, which closes both.
    search->forceActiveFocus(Qt::TabFocusReason);
    typeText(window, QStringLiteral("earlierq"));
    QTRY_VERIFY(watched().isEmpty());
    // Meanwhile, unwatched, Old gains a file.
    writeFile(old + QStringLiteral("/Earlier, unseen.md"), "Unseen\n");
    // Matching two levels down opens Essays and Old again, each watched and
    // listed afresh.
    QTest::keyClick(window, Qt::Key_Backspace);
    QCOMPARE(library->filter(), QStringLiteral("earlier"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Earlier.md")));
    QTRY_COMPARE(watched(), (QSet<QString>{essays, old}));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Earlier, unseen.md")));
    writeFile(old + QStringLiteral("/Earlier still.md"), "Still\n");
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Earlier still.md")));
}

void ParchTests::searchSaysSearchingAndPutsExpansionBack()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString writing = canonical(root.path()) + QStringLiteral("/Writing");
    writeFile(writing + QStringLiteral("/Essays/A room for thought.md"), "x\n");
    writeFile(writing + QStringLiteral("/Notes/Plain.md"), "x\n");
    ShellFixture shell({.settings = librarySettings(writing), .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QObject *tree = shell.object(QStringLiteral("libraryTree"));
    QQuickItem *noMatches = shell.item(QStringLiteral("libraryNoMatches"));
    QVERIFY(search && tree && noMatches);
    QTRY_VERIFY(treeRow(tree, QStringLiteral("Writing")) >= 0);
    const auto expanded = [&](const QString &name) {
        bool open = false;
        const int row = treeRow(tree, name);
        if (row >= 0) QMetaObject::invokeMethod(tree, "isExpanded", Q_RETURN_ARG(bool, open), Q_ARG(int, row));
        return open;
    };
    QVERIFY(!expanded(QStringLiteral("Writing")));
    search->forceActiveFocus(Qt::TabFocusReason);
    {
        // While a listing for the filter is held, the library says it is
        // searching. The filter comes whole, as from a paste: a filter typed
        // a letter at a time leaves what it had out to the letter before.
        PoolBlock block(shell.backend->libraryPool());
        search->setProperty("text", QStringLiteral("room"));
        QTRY_VERIFY(noMatches->isVisible());
        QCOMPARE(noMatches->property("text").toString(), QStringLiteral("Searching…"));
    }
    // The match appears as its folders are listed, opened to show it.
    QTRY_VERIFY(libraryRow(window, QStringLiteral("A room for thought.md")));
    QVERIFY(!noMatches->isVisible());
    QVERIFY(expanded(QStringLiteral("Writing")));
    QVERIFY(expanded(QStringLiteral("Essays")));
    // Cleared, the library is opened as it was before the search.
    for (int i = 0; i < 4; ++i) QTest::keyClick(window, Qt::Key_Backspace);
    QTRY_COMPARE(shell.context()->library()->filter(), QString());
    QTRY_VERIFY(!expanded(QStringLiteral("Writing")));
    QTRY_VERIFY(shell.context()->library()->watchedFolders().isEmpty());
}

namespace {
// The longest the event loop went without turning, while it is measured: a
// timer that asks to fire every millisecond notes each gap between firings.
class LoopGaps : public QObject {
public:
    LoopGaps()
    {
        m_timer.setInterval(1);
        m_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.elapsed();
            m_longest = std::max(m_longest, now - m_last);
            m_last = now;
        });
        restart();
        m_timer.start();
    }
    void restart()
    {
        m_clock.start();
        m_last = 0;
        m_longest = 0;
    }
    qint64 longest() const { return std::max(m_longest, m_clock.elapsed() - m_last); }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    qint64 m_longest = 0;
};
} // namespace

void ParchTests::searchSaysSearchingWhileItsListingsAreOut()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString writing = canonical(root.path()) + QStringLiteral("/Writing");
    writeFile(writing + QStringLiteral("/Essays/A room for thought.md"), "x\n");
    ShellFixture shell({.settings = librarySettings(writing), .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QQuickItem *noMatches = shell.item(QStringLiteral("libraryNoMatches"));
    QVERIFY(search && noMatches);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Writing")));
    search->forceActiveFocus(Qt::TabFocusReason);
    const auto says = [&] { return noMatches->isVisible() ? noMatches->property("text").toString() : QString(); };
    QElapsedTimer clock;
    {
        // Typed a letter at a time while the listing is held, the field
        // says it is searching, and never that nothing matches.
        PoolBlock block(shell.backend->libraryPool());
        for (const QChar letter : QStringLiteral("room")) {
            typeText(window, QString(letter));
            QCoreApplication::processEvents();
            QVERIFY2(says() != QStringLiteral("No matches in the library"), qPrintable(search->property("text").toString()));
        }
        QCOMPARE(says(), QStringLiteral("Searching…"));
        clock.start();
        QTest::qWait(LibraryModel::SearchingMs / 2);
        QCOMPARE(says(), QStringLiteral("Searching…"));
        // Held past the time a filter may say so, it says nothing matches.
        QTRY_COMPARE_WITH_TIMEOUT(says(), QStringLiteral("No matches in the library"), LibraryModel::SearchingMs);
        QVERIFY2(clock.elapsed() >= LibraryModel::SearchingMs - 100, qPrintable(QString::number(clock.elapsed())));
    }
    // Released, the listing lands, and the match appears all the same.
    QTRY_VERIFY(libraryRow(window, QStringLiteral("A room for thought.md")));
    QTRY_VERIFY(!noMatches->isVisible());
}

void ParchTests::searchKeepsTheWindowResponsiveAtTheLibraryWorkload()
{
    // A large library, 1,000 folders of 10
    // documents, in a window 1440 pixels wide. Typing a one-letter search,
    // and then the next letter, leaves no gap of over 33 ms in the event
    // loop, and the search ends within a second.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString writing = canonical(root.path()) + QStringLiteral("/Writing");
    for (int folder = 1; folder <= 1000; ++folder) {
        const QString path = writing + QStringLiteral("/Folder %1").arg(folder, 4, 10, QLatin1Char('0'));
        QVERIFY(QDir().mkpath(path));
        for (int note = 1; note <= 10; ++note) {
            QFile file(path + QStringLiteral("/Note %1.md").arg(note, 2, 10, QLatin1Char('0')));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x\n");
        }
    }
    ShellFixture shell({.settings = librarySettings(writing), .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    LibraryModel *library = shell.context()->library();
    QObject *tree = shell.object(QStringLiteral("libraryTree"));
    QTRY_VERIFY(treeRow(tree, QStringLiteral("Writing")) >= 0);
    search->forceActiveFocus(Qt::TabFocusReason);
    QTest::qWait(200);

    LoopGaps gaps;
    QElapsedTimer searched;
    searched.start();
    QTest::keyClick(window, Qt::Key_N);
    QTRY_VERIFY_WITH_TIMEOUT(!library->searching(), 10000);
    const qint64 took = searched.elapsed();
    const qint64 first = gaps.longest();
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Note 01.md")));
    QTest::qWait(200);
    const qint64 settling = gaps.longest();
    // The search opened the library to its nearest hundred matches only:
    // Writing and ten of its folders, watched as open folders are.
    QVERIFY2(library->watchedFolders().size() <= 11, qPrintable(QString::number(library->watchedFolders().size())));
    QVERIFY(library->watchedFolders().contains(writing));
    gaps.restart();
    QTest::keyClick(window, Qt::Key_O);
    QTest::qWait(300);
    const qint64 next = gaps.longest();
    qInfo("search %lld ms; longest gaps: first key %lld ms, settling %lld ms, next key %lld ms", took, first,
          settling, next);
    QVERIFY2(took <= 1000, qPrintable(QStringLiteral("the search took %1 ms").arg(took)));
    QVERIFY2(settling <= 33, qPrintable(QStringLiteral("a gap of %1 ms after the first key").arg(settling)));
    QVERIFY2(next <= 33, qPrintable(QStringLiteral("a gap of %1 ms after the next key").arg(next)));
    QCOMPARE(library->filter(), QStringLiteral("no"));
}

void ParchTests::libraryWorkNeverHoldsUpSaving()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString base = canonical(root.path()), folder = base + QStringLiteral("/Folder");
    writeFile(folder + QStringLiteral("/inside.md"), "inside\n");
    QStringList files;
    for (int i = 0; i < 12; ++i) {
        files << base + QStringLiteral("/file %1.md").arg(i);
        writeFile(files.last(), "words\n");
    }
    Backend backend(paths);
    backend.start({});
    LibraryModel *library = backend.windows().first()->library();
    library->addFolder(folder);
    const QPersistentModelIndex row = rowNamed(*library, QStringLiteral("Folder"));
    QVERIFY(row.isValid());
    {
        // Every worker the library has waits, as on a folder that never answers.
        PoolBlock block(backend.libraryPool());
        library->setExpanded(row, true);
        for (const QString &file : std::as_const(files)) backend.openFiles({file});
        for (int i = 0; i < 10; ++i) {
            library->setExpanded(row, false);
            library->setExpanded(row, true);
        }
        // Nothing piles up behind it: one job for the folder, one look at
        // the recent files and one at the drafts, at most.
        QVERIFY2(library->jobsInFlight() <= 3, qPrintable(QString::number(library->jobsInFlight())));
        QCOMPARE(library->rowCount(row), 0);
        // A document is saved all the same.
        Document *document = backend.document();
        document->buffer()->replace({0, 0}, QStringLiteral("saved "));
        backend.save();
        QTRY_COMPARE(document->state(), Document::State::Clean);
        QCOMPARE(readFile(files.last()), QByteArray("saved words\n"));
        // While the library still waits.
        QCOMPARE(library->rowCount(row), 0);
        QVERIFY(row.data(LibraryModel::LoadingRole).toBool());
        block.release();
    }
    // Once the workers are free, the folder is listed.
    QTRY_COMPARE(namesUnder(*library, row), QStringList{"inside.md"});
}

void ParchTests::libraryKeyGoesToTheFieldAndBack()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QQuickItem *library = shell.item(QStringLiteral("library"));
    QVERIFY(editor && search && library);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(editor));
    QCOMPARE(window->activeFocusItem(), editor);
    // Docked and shown, Ctrl+B hides the library and the text keeps the keys.
    QVERIFY(library->isVisible());
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(!library->isVisible());
    QCOMPARE(window->activeFocusItem(), editor);
    // Shown again, the library takes the keys in its field.
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(library->isVisible());
    QCOMPARE(window->activeFocusItem(), search);
    typeText(window, QStringLiteral("draft"));
    QCOMPARE(shell.context()->library()->filter(), QStringLiteral("draft"));
    // Escape goes back to the text, which types again.
    QTest::keyClick(window, Qt::Key_Escape);
    QCOMPARE(window->activeFocusItem(), editor);
    QCOMPARE(shell.context()->library()->filter(), QString());
    typeText(window, QStringLiteral("Q"));
    QVERIFY2(shell.document()->buffer()->text().contains(u'Q'), qPrintable(shell.document()->buffer()->text()));
    // So does Escape from the tree.
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(!library->isVisible());
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_COMPARE(window->activeFocusItem(), search);
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(window->activeFocusItem(), shell.item(QStringLiteral("libraryTree")));
    QTest::keyClick(window, Qt::Key_Escape);
    QCOMPARE(window->activeFocusItem(), editor);
    QVERIFY(library->isVisible());
}

void ParchTests::librarySearchEnterSpaceAndCursor()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString essays = canonical(root.path()) + QStringLiteral("/Essays");
    writeFile(essays + QStringLiteral("/Alpha.md"), "Alpha\n");
    writeFile(essays + QStringLiteral("/Beta.md"), "Beta\n");
    writeFile(essays + QStringLiteral("/Gamma/inside.md"), "inside\n");
    QVERIFY(QDir().mkpath(essays + QStringLiteral("/Empty")));
    ShellFixture shell({.settings = librarySettings(essays), .root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    resizeWindow(window, 1440);
    activate(window);
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QQuickItem *tree = shell.item(QStringLiteral("libraryTree"));
    QQuickItem *noMatches = shell.item(QStringLiteral("libraryNoMatches"));
    QVERIFY(search && tree && noMatches);
    TabModel *tabs = shell.context()->tabs();
    QTRY_VERIFY(treeRow(tree, QStringLiteral("Essays")) >= 0);
    QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, treeRow(tree, QStringLiteral("Essays"))));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Beta.md")));
    // A folder not looked in yet can be opened; found empty, it has nothing to open.
    QVERIFY(rowHas(window, QStringLiteral("Empty"), "opens"));
    QMetaObject::invokeMethod(tree, "expand", Q_ARG(int, treeRow(tree, QStringLiteral("Empty"))));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Empty")) && !rowHas(window, QStringLiteral("Empty"), "opens"));
    const QString before = tabs->activeTitle();

    // Enter opens only a document or a draft: with only a folder matching it does nothing.
    search->forceActiveFocus(Qt::TabFocusReason);
    typeText(window, QStringLiteral("gamma"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Gamma")));
    const auto gammaOpen = [&] {
        bool open = false;
        QMetaObject::invokeMethod(tree, "isExpanded", Q_RETURN_ARG(bool, open), Q_ARG(int, treeRow(tree, QStringLiteral("Gamma"))));
        return open;
    };
    QVERIFY(!gammaOpen());
    QTest::keyClick(window, Qt::Key_Return);
    QVERIFY(!gammaOpen());
    QCOMPARE(tabs->activeTitle(), before);
    QCOMPARE(window->activeFocusItem(), search);
    QVERIFY(rowHas(window, QStringLiteral("Drafts"), "expanded"));
    QVERIFY(!noMatches->isVisible());
    // A search that matches nothing says so, quietly, and Enter does nothing.
    for (int i = 0; i < 5; ++i) QTest::keyClick(window, Qt::Key_Backspace);
    typeText(window, QStringLiteral("zzz"));
    QTRY_VERIFY(noMatches->isVisible());
    QTRY_COMPARE(noMatches->property("text").toString(), QStringLiteral("No matches in the library"));
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(tabs->activeTitle(), before);
    QVERIFY(rowHas(window, QStringLiteral("Drafts"), "expanded"));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    for (int i = 0; i < 3; ++i) QTest::keyClick(window, Qt::Key_Backspace);
    QVERIFY(!noMatches->isVisible());

    // Space opens the row the keyboard is on, as Enter does.
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(window->activeFocusItem(), tree);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Beta.md"), "keyboard"));
    QTest::keyClick(window, Qt::Key_Space);
    QCOMPARE(tabs->activeTitle(), QStringLiteral("Beta.md"));

    // The keyboard stays on its row as rows come above it: Beta.md in
    // Recent, and a new file in Essays.
    tree->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Beta.md"), "keyboard"));
    writeFile(essays + QStringLiteral("/Aardvark.md"), "Aardvark\n");
    QTRY_VERIFY(libraryRow(window, QStringLiteral("Aardvark.md")));
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Beta.md"), "keyboard"));
    QVERIFY(!someRowHas(window, QStringLiteral("Alpha.md"), "keyboard"));
    QTest::keyClick(window, Qt::Key_Up);
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Alpha.md"), "keyboard"));
}

void ParchTests::listedEmptyFolderShowsNoChevronWhileWatched()
{
    LibraryFixture f;
    QVERIFY(f.root.isValid());
    const QString essays = f.folder(QStringLiteral("Essays")), empty = essays + QStringLiteral("/Empty");
    QVERIFY(QDir().mkpath(empty));
    f.model.addFolder(essays);
    QVERIFY(listed(f.model, rowNamed(f.model, QStringLiteral("Essays"))));
    const QPersistentModelIndex folder = rowNamed(f.model, QStringLiteral("Empty"), rowNamed(f.model, QStringLiteral("Essays")));
    // Not looked in yet, it may hold something.
    QVERIFY(f.model.hasChildren(folder));
    // Found empty, it has nothing to open, while it is watched.
    QVERIFY(listed(f.model, folder));
    QVERIFY(!f.model.hasChildren(folder));
    QVERIFY(f.model.watchedFolders().contains(empty));
    writeFile(empty + QStringLiteral("/new.md"), "x\n");
    QTRY_COMPARE(f.model.rowCount(folder), 1);
    QVERIFY(f.model.hasChildren(folder));
    QVERIFY(QFile::remove(empty + QStringLiteral("/new.md")));
    QTRY_COMPARE(f.model.rowCount(folder), 0);
    QVERIFY(!f.model.hasChildren(folder));
    // No longer watched, it may hold something again, and is looked in when next opened.
    f.model.setExpanded(folder, false);
    QVERIFY(f.model.hasChildren(folder));
    QVERIFY(f.model.canFetchMore(folder));
}

namespace {
// Stands in for the desktop's dialogs: it counts what it is asked and
// answers only when a test emits one of its signals, as a person would.
class FakePicker : public FilePicker {
public:
    int opens = 0;
    int folders = 0;
    int saves = 0;
    QUrl lastSuggestion;

    void openDocuments() override { ++opens; }
    void openFolder() override { ++folders; }
    void saveDocument(const QUrl &suggested) override
    {
        ++saves;
        lastSuggestion = suggested;
    }
};

// Lays out, now, whatever is waiting to place the item and its parents,
// as the window would before it next draws.
void settle(QQuickItem *item)
{
    for (QList<QQuickItem *> pending{item}; !pending.isEmpty();) {
        QQuickItem *next = pending.takeLast();
        next->ensurePolished();
        pending += next->childItems();
    }
}

// The buttons along a dialog's foot, in reading order.
QList<QQuickItem *> footButtons(QObject *dialog)
{
    QList<QQuickItem *> buttons;
    auto *footer = dialog->property("footer").value<QQuickItem *>();
    if (!footer) return buttons;
    settle(footer);
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible()) return;
        if (item->inherits("QQuickAbstractButton")) {
            buttons << item;
            return;
        }
        for (QQuickItem *child : item->childItems()) visit(child);
    };
    visit(footer);
    std::stable_sort(buttons.begin(), buttons.end(), [](QQuickItem *a, QQuickItem *b) {
        const QPointF first = a->mapToScene(QPointF(0, 0)), second = b->mapToScene(QPointF(0, 0));
        return qAbs(first.y() - second.y()) > 1 ? first.y() < second.y() : first.x() < second.x();
    });
    return buttons;
}

QStringList buttonTexts(QObject *dialog)
{
    QStringList texts;
    for (QQuickItem *button : footButtons(dialog)) texts << button->property("text").toString();
    return texts;
}

QQuickItem *footButton(QObject *dialog, const QString &text)
{
    for (QQuickItem *button : footButtons(dialog))
        if (button->property("text").toString() == text) return button;
    return nullptr;
}
} // namespace

void ParchTests::saveOnDraftAsksForNameThenPromotes()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString place = canonical(folder.path());
    shell.settings()->setLastFolder(place);
    QQuickWindow *window = shell.window.get();
    activate(window);
    typeText(window, QStringLiteral("# Notes"));
    sendKey(window, Qt::Key_Return);
    typeText(window, QStringLiteral("body"));
    Document *draft = shell.document();
    QCOMPARE(draft->buffer()->text(), QStringLiteral("# Notes\nbody"));
    shell.backend->save();
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.lastSuggestion.fileName(), QStringLiteral("Notes.md"));
    QCOMPARE(picker.lastSuggestion.toLocalFile(), place + QStringLiteral("/Notes.md"));
    const QString target = place + QStringLiteral("/Notes.md");
    emit picker.saveSelected(QUrl::fromLocalFile(target));
    QTRY_COMPARE(readFile(target), QByteArray("# Notes\nbody"));
    QTRY_VERIFY(!shell.document()->isDraft());
    // The same document, with its text and identity, now has a name.
    QCOMPARE(shell.document(), draft);
    QCOMPARE(draft->path(), target);
    // Named, it saves to its file without asking again.
    shell.backend->save();
    QCOMPARE(picker.saves, 1);
}

void ParchTests::anUntouchedDraftGivesWayToTheDocumentOpened()
{
    // A document opened into a window whose front tab is an empty draft
    // never written in takes that tab's place, and the draft goes as an
    // empty draft goes when it is closed. A draft with text, or with undo
    // history, stays.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    QStringList files;
    for (const char *name : {"a.md", "b.md", "c.md", "d.md", "e.md"}) {
        files << root.filePath(QString::fromLatin1(name));
        writeFile(files.last(), QByteArray(name) + "\n");
    }
    Backend backend(paths);
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.start({});
    TabModel *tabs = backend.windows().first()->tabs();
    QSignalSpy closed(backend.store(), &DocumentStore::documentClosed);
    const auto paths_ = [&] {
        QStringList shown;
        for (int i = 0; i < tabs->rowCount(); ++i)
            shown << (tabs->documentAt(i) ? QFileInfo(tabs->documentAt(i)->path()).fileName() : QStringLiteral("?"));
        return shown;
    };
    // A forwarded launch, over the first launch's draft.
    Document *first = backend.document();
    QVERIFY(first && first->isDraft());
    backend.openFiles({files.at(0)});
    QCOMPARE(paths_(), QStringList{"a.md"});
    QCOMPARE(closed.size(), 1);
    QCOMPARE(closed.first().first().toString(), first->id());
    // From the library, in the draft's place.
    backend.newDraft();
    QCOMPARE(tabs->rowCount(), 2);
    backend.openFromLibrary(files.at(1));
    QCOMPARE(paths_(), (QStringList{"a.md", "b.md"}));
    QCOMPARE(tabs->activeIndex(), 1);
    QCOMPARE(closed.size(), 2);
    // From the file dialog.
    backend.newDraft();
    backend.open();
    emit picker.documentsSelected({QUrl::fromLocalFile(files.at(2))});
    QCOMPARE(paths_(), (QStringList{"a.md", "b.md", "c.md"}));
    QCOMPARE(closed.size(), 3);
    // A draft with text stays.
    backend.newDraft();
    Document *written = backend.document();
    written->buffer()->replace({0, 0}, QStringLiteral("x"));
    backend.openFiles({files.at(3)});
    QCOMPARE(paths_(), (QStringList{"a.md", "b.md", "c.md", "", "d.md"}));
    // So does an empty one with undo history.
    backend.newDraft();
    Document *emptied = backend.document();
    emptied->buffer()->replace({0, 0}, QStringLiteral("y"));
    emptied->buffer()->replace({0, 1}, QString());
    QCOMPARE(emptied->buffer()->length(), 0);
    backend.openFiles({files.at(4)});
    QCOMPARE(paths_(), (QStringList{"a.md", "b.md", "c.md", "", "d.md", "", "e.md"}));
    QCOMPARE(closed.size(), 3);
    QVERIFY(backend.store()->documents().contains(written));
    QVERIFY(backend.store()->documents().contains(emptied));
    // An empty draft that stands for a file a launch named, not there yet,
    // keeps its tab: that file would be dropped without a word.
    const QString wanted = root.filePath(QStringLiteral("wanted.md"));
    backend.openFiles({wanted});
    Document *standing = backend.document();
    QVERIFY(standing && standing->isDraft() && standing->buffer()->length() == 0);
    QCOMPARE(tabs->activePath(), wanted);
    const int count = tabs->rowCount();
    backend.openFiles({files.at(0)});
    QCOMPARE(tabs->rowCount(), count);
    backend.openFromLibrary(root.filePath(QStringLiteral("x.md")));
    QCOMPARE(tabs->rowCount(), count + 1);
    tabs->setActiveIndex(tabs->indexOf(standing));
    writeFile(root.filePath(QStringLiteral("f.md")), "f\n");
    backend.openFiles({root.filePath(QStringLiteral("f.md"))});
    QCOMPARE(tabs->rowCount(), count + 2);
    QVERIFY(tabs->indexOf(standing) >= 0);
    QCOMPARE(closed.size(), 3);
}

void ParchTests::cancelledPickerChangesNothing()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    shell.settings()->setLastFolder(canonical(folder.path()));
    QQuickWindow *window = shell.window.get();
    activate(window);
    typeText(window, QStringLiteral("words"));
    Document *draft = shell.document();
    const QString text = draft->buffer()->text();
    // The picker emits nothing, as when its dialog is cancelled.
    shell.backend->save();
    QCOMPARE(picker.saves, 1);
    QTest::qWait(50);
    QVERIFY(draft->isDraft());
    QCOMPARE(draft->buffer()->text(), text);
    QCOMPARE(shell.document(), draft);
    QVERIFY(QDir(folder.path()).isEmpty());
}

void ParchTests::closingFailedSaveAsksFirst()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    TabModel *tabs = shell.context()->tabs();
    Document *draft = shell.document();
    blockDrafts(shell.paths());
    typeText(window, QStringLiteral("only copy"));
    QTRY_COMPARE(draft->state(), Document::State::SaveFailed);
    shell.backend->requestCloseTab(0);
    QObject *confirmation = window->findChild<QObject *>(QStringLiteral("closeConfirmation"));
    QVERIFY(confirmation);
    QVERIFY(confirmation->property("visible").toBool());
    QCOMPARE(tabs->rowCount(), 1);
    QCOMPARE(draft->buffer()->text(), QStringLiteral("only copy"));
}

void ParchTests::conflictDialogOffersAllChoices()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QQuickItem *notice = shell.item(QStringLiteral("conflictNotice"));
    QVERIFY(notice);
    QQuickItem *review = findItem(notice, QStringLiteral("noticeAction"));
    QVERIFY(review);
    QTRY_VERIFY(review->isVisible());
    QCOMPARE(review->property("text").toString(), QStringLiteral("Review"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(review));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("conflictDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(buttonTexts(dialog), (QStringList{"Reload", "Keep both", "Overwrite", "Cancel"}));
    QCOMPARE(dialog->property("localText").toString(), QStringLiteral("mine\n"));
    QTRY_COMPARE(dialog->property("diskText").toString(), QStringLiteral("theirs\n"));
}

void ParchTests::conflictDifferenceIsMarkedByLines()
{
    // The common start and end are trimmed by whole lines, and the middle
    // left on each side is what differs, as source offsets and as positions
    // in a text view, which shows CRLF as one separator.
    const auto ranges = [](const QString &mine, const QString &theirs) {
        const QVariantMap found = DocumentReview::difference(mine, theirs);
        return QList<int>{found.value("localFrom", -1).toInt(), found.value("localTo", -1).toInt(),
                          found.value("diskFrom", -1).toInt(), found.value("diskTo", -1).toInt()};
    };
    const QString base = QStringLiteral("one\ntwo\nthree\n");
    // A change at the start, in the middle and at the end.
    QCOMPARE(ranges(QStringLiteral("ONE\ntwo\nthree\n"), base), (QList<int>{0, 4, 0, 4}));
    QCOMPARE(ranges(QStringLiteral("one\nTWO!\nthree\n"), base), (QList<int>{4, 9, 4, 8}));
    QCOMPARE(ranges(QStringLiteral("one\ntwo\nthree!\n"), base), (QList<int>{8, 15, 8, 14}));
    // Lines inserted on one side only: the other side marks where they go.
    QCOMPARE(ranges(QStringLiteral("one\ntwo\nnew\nthree\n"), base), (QList<int>{8, 12, 8, 8}));
    QCOMPARE(ranges(base, QStringLiteral("zero\none\ntwo\nthree\n")), (QList<int>{0, 0, 0, 5}));
    // Identical texts mark nothing.
    QVERIFY(!DocumentReview::difference(base, base).value("differs").toBool());
    QCOMPARE(ranges(base, base), (QList<int>{-1, -1, -1, -1}));
    QVERIFY(DocumentReview::difference(QStringLiteral("a\n"), QStringLiteral("b\n")).value("differs").toBool());
    // A missing final newline is a difference of the last line.
    QCOMPARE(ranges(QStringLiteral("one\ntwo"), QStringLiteral("one\ntwo\n")), (QList<int>{4, 7, 4, 8}));
    // CRLF counts as one separator in a view, so positions there lag behind the source.
    const QVariantMap crlf = DocumentReview::difference(QStringLiteral("one\r\ntwo\r\nthree\r\n"),
                                                        QStringLiteral("one\r\nTWO\r\nthree\r\n"));
    QCOMPARE(crlf.value("localFrom").toInt(), 5);
    QCOMPARE(crlf.value("localTo").toInt(), 10);
    QCOMPARE(crlf.value("localStart").toInt(), 4);
    QCOMPARE(crlf.value("localEnd").toInt(), 8);
    // A change of line endings alone is a difference too.
    QVERIFY(DocumentReview::difference(QStringLiteral("a\r\n"), QStringLiteral("a\n")).value("differs").toBool());
}

void ParchTests::historyDialogRestoresAsUndoableEdit()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    writeFile(file, "older");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    const QString olderId = shell.backend->history()->capture(document->id(), "older");
    QVERIFY(!olderId.isEmpty());
    selectRange(shell.item(QStringLiteral("editor")), 0, 5);
    typeText(window, QStringLiteral("newer"));
    QCOMPARE(document->buffer()->text(), QStringLiteral("newer"));
    shell.backend->restoreVersion(olderId);
    QCOMPARE(document->buffer()->text(), QStringLiteral("older"));
    shell.binding()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("newer"));
    // A version that cannot be restored says why, and changes nothing.
    shell.backend->restoreVersion(QStringLiteral("20200101T000000000-000000000000"));
    QCOMPARE(document->buffer()->text(), QStringLiteral("newer"));
    QCOMPARE(shell.backend->problem(), QStringLiteral("Not restored"));
    QCOMPARE(shell.backend->problemDetail(), QStringLiteral("That version is no longer in history"));
}

namespace {
// Clicks a control as a pointer would, at its centre.
void click(QQuickWindow *window, QQuickItem *control)
{
    QVERIFY2(control, "no control to click");
    settle(window->contentItem());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(control));
}

QObject *dialogIn(QQuickWindow *window, const char *name)
{
    return window->findChild<QObject *>(QString::fromLatin1(name));
}

bool isOpen(QObject *dialog)
{
    return dialog && dialog->property("opened").toBool();
}

// The notice bar's rows, by what each is about.
QStringList noticeKinds(const ShellFixture &shell)
{
    return shell.object(QStringLiteral("noticeBar"))->property("kinds").toStringList();
}

QQuickItem *noticeButton(const ShellFixture &shell, const QString &kind)
{
    QQuickItem *row = shell.item(kind + QStringLiteral("Notice"));
    if (!row) return nullptr;
    QQuickItem *button = findItem(row, QStringLiteral("noticeAction"));
    return button ? button : findItem(row, QStringLiteral("noticeDismiss"));
}

// A history version that says it holds more than the budget, under a name,
// so that history is paused until it goes.
QString fillHistory(const AppPaths &paths, const QString &documentId, qint64 bytes = qint64(2) << 30)
{
    const QString id = QStringLiteral("20200101T000000000-0123456789ab");
    const QString folder = paths.history() + u'/' + documentId;
    writeFile(folder + u'/' + id + QStringLiteral(".md"), "big");
    writeFile(folder + u'/' + id + QStringLiteral(".name"), "Kept");
    QJsonObject entry{{QStringLiteral("id"), id},
                      {QStringLiteral("taken"), QStringLiteral("2020-01-01T00:00:00.000Z")},
                      {QStringLiteral("name"), QStringLiteral("Kept")},
                      {QStringLiteral("bytes"), double(bytes)}};
    writeFile(folder + QStringLiteral("/index.json"), QJsonDocument(QJsonArray{entry}).toJson());
    return id;
}
} // namespace

void ParchTests::fileDialogClosedByItsWindowIsACancel()
{
    // The portal's answers, as the desktop's dialog gives them: a choice (0),
    // the dialog cancelled (1), or the dialog ended some other way (2), as
    // when its window is closed with the compositor's own key. Only a choice
    // chooses, and only an answer that cannot be used is a failure.
    using Kind = PortalFilePicker::Answer::Kind;
    const QVariantMap chosen{{QStringLiteral("uris"), QStringList{QStringLiteral("file:///notes/a.md")}}};
    const PortalFilePicker::Answer answer = PortalFilePicker::read(0, chosen);
    QCOMPARE(answer.kind, Kind::Chosen);
    QCOMPARE(answer.urls, QList<QUrl>{QUrl::fromLocalFile(QStringLiteral("/notes/a.md"))});
    for (const uint response : {1u, 2u}) {
        const PortalFilePicker::Answer nothing = PortalFilePicker::read(response, {});
        QCOMPARE(nothing.kind, Kind::Nothing);
        QVERIFY(nothing.failure.isEmpty());
        QCOMPARE(PortalFilePicker::read(response, chosen).kind, Kind::Nothing);
    }
    QCOMPARE(PortalFilePicker::read(3, {}).kind, Kind::Failed);
    QVERIFY(!PortalFilePicker::read(3, {}).failure.isEmpty());
    QCOMPARE(PortalFilePicker::read(0, {}).kind, Kind::Failed);
    const QVariantMap remote{{QStringLiteral("uris"), QStringList{QStringLiteral("https://example.invalid/a.md")}}};
    QCOMPARE(PortalFilePicker::read(0, remote).kind, Kind::Failed);
    // What the window does with a picker that answers nothing, or that
    // fails, is cancelledPickerChangesNothing's and pickerAnswersGoWhereAsked's.
}

void ParchTests::typingGoesOnInAConflict()
{
    // A conflict pauses saving, not writing: what is typed meanwhile is the
    // text each choice acts on, and recovery keeps it on the old base.
    QTemporaryDir files, app;
    QVERIFY(files.isValid() && app.isValid());
    const QString base = canonical(files.path());
    const AppPaths paths = AppPaths::under(app.path());
    const QByteArray baseBytes("base\n");
    {
        ShellFixture shell({.root = app.path()});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        QQuickWindow *window = shell.window.get();
        activate(window);
        QQuickItem *editor = shell.item(QStringLiteral("editor"));
        // Typed before the file changes, and typed after it is found changed.
        Document *document = nullptr;
        const auto conflicted = [&](const QString &name) {
            const QString file = base + u'/' + name;
            writeFile(file, baseBytes);
            shell.backend->openFiles({file});
            document = shell.document();
            QVERIFY(document);
            QTRY_COMPARE_WITH_TIMEOUT(window->activeFocusItem(), editor, 1000);
            typeText(window, QStringLiteral("mine "));
            writeFile(file, "theirs\n");
            QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
            QVERIFY(!editor->property("readOnly").toBool());
            typeText(window, QStringLiteral("late "));
        };

        conflicted(QStringLiteral("a.md"));
        if (QTest::currentTestFailed()) return;
        Document *a = document;
        QCOMPARE(a->buffer()->text(), QStringLiteral("mine late base\n"));
        QCOMPARE(a->state(), Document::State::Conflict);
        // Autosave waits for the choice, and recovery keeps the newest text
        // on the version the edits were made on.
        QTest::qWait(Autosave::SaveMaxMs + 500);
        QCOMPARE(readFile(base + QStringLiteral("/a.md")), QByteArray("theirs\n"));
        bool kept = false;
        for (const Recovery::Entry &entry : Recovery(paths).pending())
            if (entry.documentId == a->id())
                kept = entry.text == QStringLiteral("mine late base\n")
                       && entry.base.sha256 == QCryptographicHash::hash(baseBytes, QCryptographicHash::Sha256);
        QVERIFY(kept);
        // The dialog shows the text as it is when it opens.
        QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
        QObject *dialog = dialogIn(window, "conflictDialog");
        QTRY_VERIFY(isOpen(dialog));
        QCOMPARE(dialog->property("localText").toString(), QStringLiteral("mine late base\n"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(dialog));
        QTRY_COMPARE(window->activeFocusItem(), editor);
        typeText(window, QStringLiteral("more "));
        // Overwrite saves the newest text.
        shell.backend->review()->resolveConflict(a, DocumentReview::Overwrite);
        QTRY_COMPARE(a->state(), Document::State::Clean);
        QCOMPARE(readFile(base + QStringLiteral("/a.md")), QByteArray("mine late more base\n"));

        // Keep both saves it as the copy.
        conflicted(QStringLiteral("b.md"));
        if (QTest::currentTestFailed()) return;
        Document *b = document;
        shell.backend->review()->resolveConflict(b, DocumentReview::KeepBoth);
        QTRY_COMPARE(b->state(), Document::State::Clean);
        const QStringList copies = QDir(base).entryList({QStringLiteral("b (conflict *).md")});
        QCOMPARE(copies.size(), 1);
        QCOMPARE(readFile(base + u'/' + copies.first()), QByteArray("mine late base\n"));
        QCOMPARE(b->buffer()->text(), QStringLiteral("theirs\n"));

        // Reload records it in history before taking the file's text.
        conflicted(QStringLiteral("c.md"));
        if (QTest::currentTestFailed()) return;
        Document *c = document;
        shell.backend->review()->resolveConflict(c, DocumentReview::Reload);
        QTRY_COMPARE(c->state(), Document::State::Clean);
        QCOMPARE(c->buffer()->text(), QStringLiteral("theirs\n"));
        History *history = shell.backend->history();
        QTRY_VERIFY(!history->versions(c->id()).isEmpty());
        QCOMPARE(history->read(c->id(), history->versions(c->id()).first().id), QByteArray("mine late base\n"));

        // Left in conflict when parch ends at once.
        conflicted(QStringLiteral("d.md"));
        if (QTest::currentTestFailed()) return;
        Document *d = document;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const Recovery::Entry &entry : Recovery(paths).pending())
                    if (entry.documentId == d->id()) return entry.text == QStringLiteral("mine late base\n");
                return false;
            }(),
            3000);
    }
    // A restart brings it back in conflict, with what was typed.
    ShellFixture again({.root = app.path()});
    QVERIFY2(again.ok(), qPrintable(again.error));
    Document *restored = nullptr;
    for (Document *document : again.backend->store()->documents())
        if (document->path() == base + QStringLiteral("/d.md")) restored = document;
    QVERIFY(restored);
    QCOMPARE(restored->state(), Document::State::Conflict);
    QCOMPARE(restored->buffer()->text(), QStringLiteral("mine late base\n"));
    QCOMPARE(readFile(base + QStringLiteral("/d.md")), QByteArray("theirs\n"));
}

void ParchTests::keepBothOpensTheCopyWhereTheWriterWas()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    QByteArray text;
    for (int line = 1; line <= 200; ++line) text += "Line " + QByteArray::number(line) + " of the text\n";
    writeFile(file, text);
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    // A selection half way down, scrolled to where the writer was.
    QObject *pane = shell.object(QStringLiteral("editorPane"));
    const int anchor = document->buffer()->text().indexOf(QStringLiteral("Line 120"));
    const int top = document->buffer()->text().indexOf(QStringLiteral("Line 100"));
    QVERIFY(QMetaObject::invokeMethod(pane, "restoreView", Q_ARG(QVariant, anchor + 8), Q_ARG(QVariant, anchor),
                                      Q_ARG(QVariant, top)));
    EditorBinding *binding = shell.binding();
    QCOMPARE(binding->cursorSource(), anchor + 8);
    QCOMPARE(binding->topSourcePosition(), top);

    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    click(window, footButton(dialog, QStringLiteral("Keep both")));
    QTRY_VERIFY(!isOpen(dialog));
    // The copy is made on a worker, and comes to the front once it is.
    QTRY_VERIFY(shell.document() && shell.document() != document);
    Document *copy = shell.document();
    QCOMPARE(copy->buffer()->text(), QStringLiteral("mine ") + QString::fromUtf8(text));
    // The copy holds the text the writer was in, and opens where they were in it.
    QTRY_COMPARE(binding->cursorSource(), anchor + 8);
    QCOMPARE(binding->anchorSource(), anchor);
    QCOMPARE(binding->topSourcePosition(), top);
    QTRY_COMPARE(window->activeFocusItem(), shell.item(QStringLiteral("editor")));
    QCOMPARE(document->buffer()->text(), QStringLiteral("theirs\n"));
}

void ParchTests::textFieldsRestTheirCaretToo()
{
    // The library's field, a version's name and a dialog's text rest their
    // caret as the editor does: it blinks at the flash time, rests lit after
    // a pause, and then nothing is drawn; it is steady with a flash time of
    // 0 or reduced motion. Qt's own blink timer never runs.
    QStyleHints *hints = QGuiApplication::styleHints();
    const int flash = hints->cursorFlashTime();
    const auto restoreFlash = qScopeGuard([hints, flash] { hints->setCursorFlashTime(flash); });
    hints->setCursorFlashTime(200);
    QTemporaryDir sockets;
    QVERIFY(sockets.isValid());
    SKIP_UNLESS_SOCKET_FITS(sockets.path() + QStringLiteral("/.socket2.sock"));
    FakeHyprland hyprland(sockets.path());
    hyprland.setInt(QStringLiteral("animations:enabled"), 1);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "some text\n");
    ShellFixture shell({.hyprland = sockets.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    QTRY_VERIFY(hyprland.listeners() == 1);
    int frames = 0;
    const QMetaObject::Connection counting =
        connect(window, &QQuickWindow::frameSwapped, this, [&frames] { ++frames; }, Qt::DirectConnection);
    const auto stopCounting = qScopeGuard([counting] { disconnect(counting); });
    // Qt's own blink: a TextInput's timer is its own, a TextEdit's its control's.
    const auto qtTimers = [](QQuickItem *field) {
        QObject *control = textControlOf(field);
        return QAbstractEventDispatcher::instance()->timersForObject(control ? control : field).size();
    };
    const auto rests = [&](QQuickItem *field, const char *what) {
        QVERIFY2(field, what);
        field->forceActiveFocus(Qt::TabFocusReason);
        QObject *rest = field->findChild<QObject *>(QStringLiteral("caretRest"));
        if (rest) rest->setProperty("restMs", 1000);
        QTest::keyClick(window, Qt::Key_Right);
        QQuickItem *caret = findItem(field, QStringLiteral("fieldCaret"));
        int changes = 0;
        QMetaObject::Connection counting;
        if (caret) counting = connect(caret, &QQuickItem::visibleChanged, this, [&changes] { ++changes; });
        const auto stopCounting = qScopeGuard([&counting] { disconnect(counting); });
        // After a pause it rests lit, and an idle window draws nothing.
        QTest::qWait(1300);
        frames = changes = 0;
        QTest::qWait(1500);
        QVERIFY2(frames == 0, qPrintable(QStringLiteral("%1: %2 frames while idle").arg(QLatin1String(what)).arg(frames)));
        QVERIFY2(qtTimers(field) == 0, what);
        QVERIFY2(rest && caret, what);
        QVERIFY2(caret->isVisible() && changes == 0, what);
        QCOMPARE(caret->property("color").value<QColor>(), shell.theme->accent());
        // A key wakes it, and it blinks at half the flash time.
        QTest::keyClick(window, Qt::Key_Left);
        QVERIFY2(caret->isVisible(), what);
        QVERIFY2(rest->property("blinking").toBool(), what);
        QTRY_VERIFY2_WITH_TIMEOUT(changes >= 4, what, 900);
        QVERIFY2(qtTimers(field) == 0, what);
        // Steady with a flash time of 0.
        hints->setCursorFlashTime(0);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::qWait(50);
        changes = 0;
        QTest::qWait(400);
        QVERIFY2(caret->isVisible() && changes == 0 && !rest->property("blinking").toBool(), what);
        hints->setCursorFlashTime(200);
    };

    rests(shell.item(QStringLiteral("librarySearch")), "the library's field");
    if (QTest::currentTestFailed()) return;
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *history = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(history));
    QQuickItem *preview = shell.item(QStringLiteral("versionPreview"));
    QVERIFY(preview);
    rests(findItem(preview, QStringLiteral("paneText")), "a dialog's text");
    if (QTest::currentTestFailed()) return;
    click(window, footButton(history, QStringLiteral("Name this version")));
    QObject *prompt = dialogIn(window, "versionNamePrompt");
    QTRY_VERIFY(isOpen(prompt));
    typeText(window, QStringLiteral("Draft"));
    QQuickItem *name = qobject_cast<QQuickItem *>(window->activeFocusItem());
    QCOMPARE(name->objectName(), QStringLiteral("versionName"));
    rests(name, "a version's name");
    if (QTest::currentTestFailed()) return;

    // Reduced motion makes it steady too.
    hyprland.setInt(QStringLiteral("animations:enabled"), 0);
    hyprland.send("configreloaded>>");
    QTRY_VERIFY(shell.theme->reduceMotion());
    QQuickItem *caret = findItem(name, QStringLiteral("fieldCaret"));
    QTest::keyClick(window, Qt::Key_Left);
    QSignalSpy changes(caret, &QQuickItem::visibleChanged);
    QTest::qWait(400);
    QVERIFY(caret->isVisible());
    QCOMPARE(changes.count(), 0);
    QVERIFY(!name->findChild<QObject *>(QStringLiteral("caretRest"))->property("blinking").toBool());
}

void ParchTests::conflictDifferenceFindsEachRun()
{
    // Between the trimmed start and end, a line diff finds each run of lines
    // that differ, as source offsets and view positions on both sides; the
    // lines the two share between runs are not part of any.
    const auto runsOf = [](const QString &mine, const QString &theirs) {
        QList<QList<int>> runs;
        for (const QVariant &run : DocumentReview::difference(mine, theirs).value("runs").toList()) {
            const QVariantMap at = run.toMap();
            runs.append({at.value("localFrom").toInt(), at.value("localTo").toInt(), at.value("diskFrom").toInt(),
                         at.value("diskTo").toInt()});
        }
        return runs;
    };
    QString base;
    for (int line = 1; line <= 10; ++line) base += QStringLiteral("line %1\n").arg(line);
    const auto offset = [&](const QString &text, int line) {
        return int(text.indexOf(QStringLiteral("line %1\n").arg(line)));
    };
    // Two separate changes: two runs, and lines 4 to 7 between them unmarked.
    QString mine = base;
    mine.replace(QStringLiteral("line 3\n"), QStringLiteral("LINE 3\n"));
    QString theirs = base;
    theirs.replace(QStringLiteral("line 8\n"), QStringLiteral("LINE 8\n"));
    QCOMPARE(runsOf(mine, theirs), (QList<QList<int>>{{offset(base, 3), offset(base, 4), offset(base, 3), offset(base, 4)},
                                                      {offset(base, 8), offset(base, 9), offset(base, 8), offset(base, 9)}}));
    // An insertion on one side: a band there, and a bar where it goes on the other.
    QString inserted = base;
    inserted.insert(offset(base, 6), QStringLiteral("new line\n"));
    QCOMPARE(runsOf(inserted, base), (QList<QList<int>>{{offset(base, 6), offset(base, 6) + 9, offset(base, 6), offset(base, 6)}}));
    QCOMPARE(runsOf(base, inserted), (QList<QList<int>>{{offset(base, 6), offset(base, 6), offset(base, 6), offset(base, 6) + 9}}));
    // Identical texts mark nothing; a change of line endings alone is found.
    QVERIFY(runsOf(base, base).isEmpty());
    QString crlf = base;
    crlf.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    QCOMPARE(runsOf(crlf, base).size(), 1);
    const QVariantMap endings = DocumentReview::difference(crlf, base).value("runs").toList().first().toMap();
    QCOMPARE(endings.value("localStart").toInt(), 0);
    QCOMPARE(endings.value("localEnd").toInt(), int(base.size()));
    QCOMPARE(endings.value("diskEnd").toInt(), int(base.size()));
    // With CRLF, view positions lag behind the source by one for each pair before.
    const QVariantMap second = DocumentReview::difference(QString(crlf).replace(QStringLiteral("line 8"), QStringLiteral("LINE 8")),
                                                          crlf).value("runs").toList().first().toMap();
    QCOMPARE(second.value("localFrom").toInt(), int(crlf.indexOf(QStringLiteral("line 8"))));
    QCOMPARE(second.value("localStart").toInt(), offset(base, 8));

    // Beyond the bound, the one band of round 1: the whole trimmed middle.
    QString wide, other;
    for (int line = 0; line < 3000; ++line) {
        wide += QStringLiteral("a%1\n").arg(line);
        other += QStringLiteral("b%1\n").arg(line);
    }
    const QVariantMap edits = DocumentReview::difference(wide, other);
    QVERIFY(edits.value("bounded").toBool());
    QCOMPARE(runsOf(wide, other), (QList<QList<int>>{{0, int(wide.size()), 0, int(other.size())}}));
    QString longer = QStringLiteral("x\n").repeated(20001), shorter = QStringLiteral("x\n").repeated(20001);
    longer.prepend(QStringLiteral("first\n"));
    longer.append(QStringLiteral("last\n"));
    shorter.prepend(QStringLiteral("FIRST\n"));
    shorter.append(QStringLiteral("LAST\n"));
    QVERIFY(DocumentReview::difference(longer, shorter).value("bounded").toBool());
    QCOMPARE(runsOf(longer, shorter).size(), 1);
}

void ParchTests::conflictReviewMarksAndShowsTheDifference()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    QString text;
    for (int line = 1; line <= 200; ++line) text += QStringLiteral("Line %1 of the text\n").arg(line);
    writeFile(file, text.toUtf8());
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    const int at = text.indexOf(QStringLiteral("Line 150 "));
    document->buffer()->replace({at, 4}, QStringLiteral("LINE"));
    writeFile(file, QString(text).replace(QStringLiteral("Line 160 "), QStringLiteral("Line one-sixty ")).toUtf8());
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    QTRY_VERIFY(!dialog->property("diskText").toString().isEmpty());
    // Each pane marks its two runs, line 150 and line 160, in the theme's
    // selection colour, with lines 151 to 159 unmarked, and opens at the first.
    for (const char *name : {"localPane", "diskPane"}) {
        QQuickItem *pane = shell.item(QString::fromLatin1(name));
        QVERIFY(pane);
        const auto marks = [&] {
            QList<QQuickItem *> found;
            for (QQuickItem *item : visibleItems(window))
                if (item->objectName() == QStringLiteral("differenceMark") && pane->isAncestorOf(item)) found << item;
            std::sort(found.begin(), found.end(), [](QQuickItem *a, QQuickItem *b) { return a->y() < b->y(); });
            return found;
        };
        QTRY_COMPARE_WITH_TIMEOUT(marks().size(), 2, 3000);
        QQuickItem *body = findItem(pane, QStringLiteral("paneText"));
        const QString shown = dialog->property(qstrcmp(name, "localPane") == 0 ? "localText" : "diskText").toString();
        // Where the view shows a line, counted from 1.
        const auto lineTop = [&](int line) {
            qsizetype at = 0;
            for (int passed = 1; passed < line; ++passed) at = shown.indexOf(u'\n', at) + 1;
            QRectF rect;
            QMetaObject::invokeMethod(body, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, int(at)));
            return rect.y();
        };
        const QList<QQuickItem *> found = marks();
        for (QQuickItem *mark : found)
            QCOMPARE(mark->property("color").value<QColor>().rgba(), shell.theme->selection().rgba());
        QCOMPARE(found.at(0)->y(), lineTop(150));
        QCOMPARE(found.at(0)->y() + found.at(0)->height(), lineTop(151));
        QCOMPARE(found.at(1)->y(), lineTop(160));
        QCOMPARE(found.at(1)->y() + found.at(1)->height(), lineTop(161));
        QQuickItem *flick = body->parentItem()->parentItem();
        QTRY_VERIFY2(flick->property("contentY").toReal() > 0, name);
        const qreal top = flick->property("contentY").toReal();
        QVERIFY2(found.at(0)->y() >= top && found.at(0)->y() < top + flick->height(),
                 qPrintable(QStringLiteral("%1: first run at %2, view from %3").arg(QLatin1String(name)).arg(found.at(0)->y()).arg(top)));
    }
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));

    // With nothing that differs, nothing is marked.
    document->buffer()->replace({at, 4}, QStringLiteral("Line"));
    writeFile(file, text.toUtf8());
    QTest::qWait(300);
    // A conflict stays until it is chosen, though the texts are now the same.
    QCOMPARE(document->state(), Document::State::Conflict);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QTRY_VERIFY(isOpen(dialog));
    QTRY_COMPARE(dialog->property("diskText").toString(), text);
    QVERIFY(!dialog->property("difference").toMap().value(QStringLiteral("differs")).toBool());
    for (QQuickItem *item : visibleItems(window)) QVERIFY(item->objectName() != QStringLiteral("differenceMark"));
}

void ParchTests::foldersJoinTheLibraryAndShowThere()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString essays = canonical(root.path()) + QStringLiteral("/Essays"),
                  research = canonical(root.path()) + QStringLiteral("/Research"),
                  letters = canonical(root.path()) + QStringLiteral("/Letters");
    writeFile(essays + QStringLiteral("/a.md"), "a\n");
    writeFile(research + QStringLiteral("/b.md"), "b\n");
    writeFile(letters + QStringLiteral("/c.md"), "c\n");
    const AppPaths paths = AppPaths::under(root.path() + QStringLiteral("/app"));
    {
        // A folder on the command line joins the library, which opens on it.
        Backend backend(paths);
        // A dialog with no folder used last starts in the test's own folder.
        backend.fileRequests()->setFallbackFolders({paths.config});
        backend.start({essays});
        WindowContext *window = backend.activeWindow();
        QCOMPARE(backend.settings()->libraryFolders(), QStringList{essays});
        QVERIFY(window->libraryOpen());
        QCOMPARE(window->folderToShow(), essays);
        QVERIFY(backend.problem().isEmpty());
        // So does one a later launch forwards, with no tab for it.
        window->setLibraryOpen(false);
        window->setFolderToShow({});
        const int tabs = window->tabs()->rowCount();
        backend.openFiles({research});
        QCOMPARE(backend.settings()->libraryFolders(), (QStringList{essays, research}));
        QVERIFY(window->libraryOpen());
        QCOMPARE(window->folderToShow(), research);
        QCOMPARE(window->tabs()->rowCount(), tabs);
        QVERIFY(backend.problem().isEmpty());
        // And one chosen in the file dialog, in the window that asked.
        FakePicker picker;
        backend.setFilePicker(&picker);
        backend.newWindow();
        WindowContext *second = backend.windows().at(1);
        backend.activate(window);
        backend.openFolder();
        QCOMPARE(picker.folders, 1);
        backend.activate(second);
        emit picker.folderSelected(QUrl::fromLocalFile(letters));
        QCOMPARE(backend.settings()->libraryFolders(), (QStringList{essays, research, letters}));
        QCOMPARE(window->folderToShow(), letters);
        QVERIFY(second->folderToShow().isEmpty());
        QCOMPARE(backend.settings()->lastFolder(), letters);
    }
    // In the window, the library shows the folder with the keys on its row:
    // beside the editor in a wide window, over it in a narrow one.
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({essays});
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "keyboard"));
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "expanded"));
    QCOMPARE(window->activeFocusItem()->objectName(), QStringLiteral("libraryTree"));
    QVERIFY(shell.context()->folderToShow().isEmpty());
    resizeWindow(window, 360);
    shell.item(QStringLiteral("editor"))->forceActiveFocus();
    shell.backend->openFiles({research});
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Research"), "keyboard"));
    QCOMPARE(window->activeFocusItem()->objectName(), QStringLiteral("libraryTree"));
}

void ParchTests::aFileOpenedWithFoldersKeepsTheKeys()
{
    // Opened with a file, folders join the library and show there, but the
    // keys go to the file's text, whatever had them and in whichever order
    // the two came. Folders on their own put the keys on the folder's row.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString base = canonical(root.path());
    QStringList folders, files;
    for (const char *name : {"Essays", "Notes", "Letters", "Poems"}) {
        folders << base + u'/' + QString::fromLatin1(name);
        writeFile(folders.last() + QStringLiteral("/inside.md"), "inside\n");
    }
    for (const char *name : {"one.md", "two.md", "three.md"}) {
        files << base + u'/' + QString::fromLatin1(name);
        writeFile(files.last(), QByteArray(name) + "\n");
    }
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    QQuickItem *tree = shell.item(QStringLiteral("libraryTree"));
    QVERIFY(tree);
    const auto focused = [window] {
        return window->activeFocusItem() ? window->activeFocusItem()->objectName() : QString();
    };
    const auto keepsTheKeys = [&](const QStringList &launch, const QString &file, const QString &folder) {
        tree->forceActiveFocus();
        QCOMPARE(focused(), QStringLiteral("libraryTree"));
        shell.backend->openFiles(launch);
        QTRY_COMPARE(shell.document()->path(), file);
        QTRY_COMPARE(focused(), QStringLiteral("editor"));
        QTRY_VERIFY(someRowHas(window, QFileInfo(folder).fileName(), "expanded"));
        // Nothing takes them afterwards.
        QTest::qWait(100);
        QCOMPARE(focused(), QStringLiteral("editor"));
    };
    keepsTheKeys({folders.at(0), files.at(0)}, files.at(0), folders.at(0));
    if (QTest::currentTestFailed()) return;
    keepsTheKeys({files.at(1), folders.at(1)}, files.at(1), folders.at(1));
    if (QTest::currentTestFailed()) return;
    // The first keys typed are the document's.
    typeText(window, QStringLiteral("typed "));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("typed two.md\n"));

    // A folder alone has the keys on its row.
    shell.item(QStringLiteral("editor"))->forceActiveFocus();
    shell.backend->openFiles({folders.at(2)});
    QTRY_COMPARE(focused(), QStringLiteral("libraryTree"));
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Letters"), "keyboard"));

    // So does a folder with a file whose folder does not exist: its tab
    // shows no text, so it takes no keys.
    shell.item(QStringLiteral("editor"))->forceActiveFocus();
    const QString nowhere = base + QStringLiteral("/no/such/folder/a.md");
    shell.backend->openFiles({folders.at(1), nowhere});
    QTRY_COMPARE(shell.context()->tabs()->activePath(), nowhere);
    QVERIFY(!shell.item(QStringLiteral("editorPane"))->isVisible());
    QTRY_COMPARE(focused(), QStringLiteral("libraryTree"));
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Notes"), "keyboard"));
    QTest::qWait(100);
    QCOMPARE(focused(), QStringLiteral("libraryTree"));
    shell.backend->requestCloseTab(shell.context()->tabs()->activeIndex());

    // Narrow, the library over the editor stays shut while the file has the keys.
    resizeWindow(window, 360);
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QVERIFY(drawer);
    shell.item(QStringLiteral("editor"))->forceActiveFocus();
    shell.backend->openFiles({folders.at(3), files.at(2)});
    QTRY_COMPARE(shell.document()->path(), files.at(2));
    QTest::qWait(200);
    QVERIFY(!drawer->property("opened").toBool());
    QCOMPARE(focused(), QStringLiteral("editor"));
    QVERIFY(shell.backend->settings()->libraryFolders().contains(folders.at(3)));
}

void ParchTests::firstLaunchGivesTheKeysToWhatItOpens()
{
    // A first launch opens its files and folders before any window shows
    // them: a file opened with a folder keeps the keys in its text, and a
    // folder alone has them on its row.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString base = canonical(root.path());
    const QString folder = base + QStringLiteral("/Essays"), file = base + QStringLiteral("/one.md");
    writeFile(folder + QStringLiteral("/inside.md"), "inside\n");
    writeFile(file, "one\n");
    const auto focused = [](QQuickWindow *window) {
        return window->activeFocusItem() ? window->activeFocusItem()->objectName() : QString();
    };
    {
        QTemporaryDir app;
        QVERIFY(app.isValid());
        ShellFixture shell({.root = app.path(), .files = {folder, file}});
        QVERIFY2(shell.ok(), qPrintable(shell.error));
        QQuickWindow *window = shell.window.get();
        activate(window);
        QCOMPARE(shell.document()->path(), file);
        QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "expanded"));
        QTRY_COMPARE(focused(window), QStringLiteral("editor"));
        QTest::qWait(100);
        QCOMPARE(focused(window), QStringLiteral("editor"));
        typeText(window, QStringLiteral("typed "));
        QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("typed one\n"));
    }
    QTemporaryDir app;
    QVERIFY(app.isValid());
    ShellFixture shell({.root = app.path(), .files = {folder}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QTRY_COMPARE(focused(window), QStringLiteral("libraryTree"));
    QTRY_VERIFY(someRowHas(window, QStringLiteral("Essays"), "keyboard"));
}

void ParchTests::historyNamesAndRemovesVersions()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = newUuid();
    History history(paths, HistoryLimits{100, 30, 10});
    QSignalSpy paused(&history, &History::pausedChanged);
    const QString first = history.capture(id, "one");
    QVERIFY(!first.isEmpty());
    QCOMPARE(paused.count(), 0);
    // A version is named after the fact, and the name survives a lost index.
    QVERIFY(history.rename(id, first, QStringLiteral("Opening")));
    QCOMPARE(history.versions(id).first().name, QStringLiteral("Opening"));
    QVERIFY(QFile::remove(paths.history() + u'/' + id + QStringLiteral("/index.json")));
    QCOMPARE(history.versions(id).first().name, QStringLiteral("Opening"));
    QVERIFY(!history.rename(id, first, QString()));
    QVERIFY(!history.rename(id, QStringLiteral("20200101T000000000-0123456789ab"), QStringLiteral("Gone")));
    // Named versions that fill the budget pause history, and say so once.
    const QString second = history.capture(id, "a longer text", QStringLiteral("Big"));
    QVERIFY(!second.isEmpty());
    QVERIFY(history.paused());
    QCOMPARE(paused.count(), 1);
    QCOMPARE(paused.first().first().toBool(), true);
    QCOMPARE(history.capture(id, "x"), QString());
    QCOMPARE(paused.count(), 1);
    // Removing one makes room, and history goes on.
    QVERIFY(history.remove(id, second));
    QVERIFY(!history.paused());
    QCOMPARE(paused.count(), 2);
    QCOMPARE(paused.last().first().toBool(), false);
    QVERIFY(!QFile::exists(paths.history() + u'/' + id + u'/' + second + QStringLiteral(".md")));
    QVERIFY(!QFile::exists(paths.history() + u'/' + id + u'/' + second + QStringLiteral(".name")));
    QCOMPARE(history.versions(id).size(), 1);
    QVERIFY(!history.remove(id, second));
    QVERIFY(!history.capture(id, "x").isEmpty());
    // A capture that did not fit leaves a pause that removing a version ends.
    const QString big = history.capture(id, "twelve bytes");
    QVERIFY(big.isEmpty());
    QVERIFY(history.paused());
    QCOMPARE(paused.count(), 3);
    QVERIFY(history.remove(id, history.versions(id).first().id));
    QVERIFY(!history.paused());
    QCOMPARE(paused.count(), 4);
    // Asked afresh, it says nothing new.
    history.checkPaused();
    QCOMPARE(paused.count(), 4);
}

void ParchTests::historyDialogNamesDeletesAndGuardsRestore()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    writeFile(file, "now");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    History *history = shell.backend->history();
    const QString earlier = history->capture(document->id(), "earlier");
    QVERIFY(!earlier.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    // The text as it is now, then the version, newest first.
    QTRY_COMPARE(dialog->property("rows").toList().size(), 2);
    QCOMPARE(dialog->property("currentIndex").toInt(), 0);
    QQuickItem *list = shell.item(QStringLiteral("versionList"));
    QVERIFY(list && list->hasActiveFocus());
    QObject *preview = shell.object(QStringLiteral("versionPreview"));
    QCOMPARE(preview->property("text").toString(), QStringLiteral("now"));
    // Restore and Delete version do nothing for the text as it is now.
    QCOMPARE(buttonTexts(dialog), (QStringList{"Name this version", "Restore", "Delete version", "Close"}));
    QVERIFY(!footButton(dialog, QStringLiteral("Restore"))->isEnabled());
    QVERIFY(!footButton(dialog, QStringLiteral("Delete version"))->isEnabled());
    QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(dialog->property("currentIndex").toInt(), 1);
    QTRY_COMPARE(preview->property("text").toString(), QStringLiteral("earlier"));
    QTRY_VERIFY(footButton(dialog, QStringLiteral("Restore"))->isEnabled());
    QVERIFY(footButton(dialog, QStringLiteral("Delete version"))->isEnabled());

    // Naming the text as it is now makes a new version of it.
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(dialog->property("currentIndex").toInt(), 0);
    QVERIFY(!footButton(dialog, QStringLiteral("Restore"))->isEnabled());
    click(window, footButton(dialog, QStringLiteral("Name this version")));
    QObject *prompt = dialogIn(window, "versionNamePrompt");
    QTRY_VERIFY(isOpen(prompt));
    QCOMPARE(window->activeFocusItem()->objectName(), QStringLiteral("versionName"));
    typeText(window, QStringLiteral("Draft one"));
    sendKey(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(prompt));
    QTRY_COMPARE(dialog->property("rows").toList().size(), 3);
    const History::Version named = history->versions(document->id()).first();
    QCOMPARE(named.name, QStringLiteral("Draft one"));
    QCOMPARE(history->read(document->id(), named.id), QByteArray("now"));
    QCOMPARE(dialog->property("rows").toList().at(1).toMap().value(QStringLiteral("name")).toString(),
             QStringLiteral("Draft one"));

    // Naming an older row names that version.
    dialog->setProperty("currentIndex", 2);
    QTRY_COMPARE(preview->property("text").toString(), QStringLiteral("earlier"));
    click(window, footButton(dialog, QStringLiteral("Name this version")));
    QTRY_VERIFY(isOpen(prompt));
    typeText(window, QStringLiteral("First"));
    click(window, footButton(prompt, QStringLiteral("Name")));
    QTRY_VERIFY(!isOpen(prompt));
    QTRY_COMPARE(history->versions(document->id()).last().name, QStringLiteral("First"));
    QCOMPARE(history->versions(document->id()).last().id, earlier);
    QTRY_COMPARE(dialog->property("rows").toList().at(2).toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("First"));
    QCOMPARE(dialog->property("currentIndex").toInt(), 2);

    // Deleting asks first, with Cancel in front; Escape keeps the version.
    click(window, footButton(dialog, QStringLiteral("Delete version")));
    QObject *ask = dialogIn(window, "versionDeletePrompt");
    QTRY_VERIFY(isOpen(ask));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(ask));
    QVERIFY(isOpen(dialog));
    QCOMPARE(history->versions(document->id()).size(), 2);
    // Cancelled, the keys go back to the button that asked.
    QTRY_COMPARE(window->activeFocusItem(), footButton(dialog, QStringLiteral("Delete version")));
    click(window, footButton(dialog, QStringLiteral("Delete version")));
    QTRY_VERIFY(isOpen(ask));
    click(window, footButton(ask, QStringLiteral("Delete")));
    QTRY_COMPARE(history->versions(document->id()).size(), 1);
    QCOMPARE(history->versions(document->id()).first().name, QStringLiteral("Draft one"));
    QTRY_COMPARE(dialog->property("rows").toList().size(), 2);
    QCOMPARE(document->buffer()->text(), QStringLiteral("now"));

    // Restoring from the dialog is one edit that undo takes back.
    dialog->setProperty("currentIndex", 1);
    QTRY_VERIFY(footButton(dialog, QStringLiteral("Restore"))->isEnabled());
    document->buffer()->replace({0, 3}, QStringLiteral("later"));
    click(window, footButton(dialog, QStringLiteral("Restore")));
    QCOMPARE(document->buffer()->text(), QStringLiteral("now"));
    QTRY_VERIFY(!isOpen(dialog));
    QTRY_COMPARE(window->activeFocusItem(), shell.item(QStringLiteral("editor")));
    shell.binding()->undo();
    QCOMPARE(document->buffer()->text(), QStringLiteral("later"));
}

void ParchTests::historyDialogSaysWhyAVersionWasKept()
{
    // The list says, quietly after the time, why history kept a version by
    // itself, and the preview's header and the delete prompt say the same.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    writeFile(file, "opened\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    document->buffer()->replace({0, 6}, QStringLiteral("edited"));
    shell.backend->store()->save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    QTRY_COMPARE(dialog->property("rows").toList().size(), 3);
    const QVariantList rows = dialog->property("rows").toList();
    QCOMPARE(rows.at(1).toMap().value(QStringLiteral("reason")).toString(), QString());
    QCOMPARE(rows.at(2).toMap().value(QStringLiteral("reason")).toString(), QStringLiteral("as opened"));
    QStringList shown;
    for (QQuickItem *item : visibleItems(window))
        if (item->objectName() == QStringLiteral("versionReason")) {
            shown << item->property("text").toString();
            QCOMPARE(item->property("color").value<QColor>().rgba(), shell.theme->dim().rgba());
        }
    QCOMPARE(shown, QStringList{QStringLiteral(" · as opened")});
    dialog->setProperty("currentIndex", 2);
    QObject *preview = shell.object(QStringLiteral("versionPreview"));
    const QString when = rows.at(2).toMap().value(QStringLiteral("when")).toString();
    QTRY_COMPARE(preview->property("caption").toString(), QStringLiteral("%1 · as opened").arg(when));
    QTRY_COMPARE(preview->property("text").toString(), QStringLiteral("opened\n"));
    click(window, footButton(dialog, QStringLiteral("Delete version")));
    QObject *ask = dialogIn(window, "versionDeletePrompt");
    QTRY_VERIFY(isOpen(ask));
    QCOMPARE(ask->property("contentItem").value<QQuickItem *>()->property("text").toString(),
             QStringLiteral("The version from %1 · as opened leaves history for good.").arg(when));
    QTest::keyClick(window, Qt::Key_Escape);
}

void ParchTests::cancelledPromptsGiveTheKeysBackToTheirButton()
{
    // A prompt that a dialog's button opened, cancelled by the keys, gives
    // them back to that button; one whose action is done gives them to the list.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    writeFile(file, "now");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QVERIFY(!shell.backend->history()->capture(document->id(), "earlier").isEmpty());
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    QQuickItem *list = shell.item(QStringLiteral("versionList"));
    QTRY_VERIFY(list->hasActiveFocus());
    QTRY_COMPARE(dialog->property("rows").toList().size(), 2);
    const auto press = [&](const QString &text) {
        QQuickItem *button = footButton(dialog, text);
        button->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(window, Qt::Key_Space);
        return button;
    };
    QObject *namePrompt = dialogIn(window, "versionNamePrompt");
    QQuickItem *name = press(QStringLiteral("Name this version"));
    QTRY_VERIFY(isOpen(namePrompt));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(namePrompt));
    QTRY_COMPARE(window->activeFocusItem(), name);
    // Cancel by its button does the same.
    press(QStringLiteral("Name this version"));
    QTRY_VERIFY(isOpen(namePrompt));
    click(window, footButton(namePrompt, QStringLiteral("Cancel")));
    QTRY_VERIFY(!isOpen(namePrompt));
    QTRY_COMPARE(window->activeFocusItem(), name);
    // Naming is done, and the list has the keys. History keeps the name,
    // and the list reads its versions afresh, only once the pool is let go.
    auto block = std::make_unique<PoolBlock>();
    press(QStringLiteral("Name this version"));
    QTRY_VERIFY(isOpen(namePrompt));
    typeText(window, QStringLiteral("First"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(namePrompt));
    QTRY_VERIFY(list->hasActiveFocus());

    // A version is chosen, and Delete pressed as the versions read afresh
    // land: the press still opens its prompt.
    dialog->setProperty("currentIndex", 1);
    QObject *deletePrompt = dialogIn(window, "versionDeletePrompt");
    QQuickItem *remove = footButton(dialog, QStringLiteral("Delete version"));
    QVERIFY(remove->isEnabled());
    remove->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyPress(window, Qt::Key_Space);
    block.reset();
    QTRY_COMPARE(dialog->property("rows").toList().size(), 3);
    QVERIFY(remove->isEnabled());
    QTest::keyRelease(window, Qt::Key_Space);
    QTRY_VERIFY(isOpen(deletePrompt));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(deletePrompt));
    QTRY_COMPARE(window->activeFocusItem(), remove);
}

void ParchTests::restorePressedAsVersionsAreReadAgain()
{
    // Restore pressed as the list reads its versions afresh still restores:
    // the version chosen stays chosen, and its preview is not read again.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = files.filePath(QStringLiteral("a.md"));
    writeFile(file, "now");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QVERIFY(!shell.backend->history()->capture(document->id(), "earlier").isEmpty());
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    QTRY_COMPARE(dialog->property("rows").toList().size(), 2);
    dialog->setProperty("currentIndex", 1);
    QQuickItem *restore = footButton(dialog, QStringLiteral("Restore"));
    QTRY_VERIFY(restore->isEnabled());
    // A newer version arrives, read while the workers are held.
    auto block = std::make_unique<PoolBlock>();
    QVERIFY(!shell.backend->history()->capture(document->id(), "newer").isEmpty());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reload"));
    restore->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyPress(window, Qt::Key_Space);
    block.reset();
    QTRY_COMPARE(dialog->property("rows").toList().size(), 3);
    QVERIFY(restore->isEnabled());
    QTest::keyRelease(window, Qt::Key_Space);
    QTRY_COMPARE(document->buffer()->text(), QStringLiteral("earlier"));
}

void ParchTests::deletedFileConflictOffersSaveAgainOrElsewhere()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString base = canonical(files.path());
    const QString file = base + QStringLiteral("/a.md"), other = base + QStringLiteral("/b.md");
    writeFile(file, "base\n");
    writeFile(other, "other\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(document->fileMissing());
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")) && noticeButton(shell, QStringLiteral("conflict"))->isVisible());
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    // Nothing to reload or keep: this text can be saved again, or elsewhere.
    QCOMPARE(buttonTexts(dialog), (QStringList{"Save again", "Save as…", "Cancel"}));
    QCOMPARE(shell.object(QStringLiteral("diskPane"))->property("note").toString(), QStringLiteral("The file no longer exists."));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    // A file that comes back has text to compare and keep again.
    writeFile(file, "back\n");
    QTRY_COMPARE(buttonTexts(dialog), (QStringList{"Reload", "Keep both", "Overwrite", "Cancel"}));
    QTRY_COMPARE(dialog->property("diskText").toString(), QStringLiteral("back\n"));
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE(buttonTexts(dialog), (QStringList{"Save again", "Save as…", "Cancel"}));
    click(window, footButton(dialog, QStringLiteral("Save again")));
    QTRY_COMPARE(readFile(file), QByteArray("mine\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QTRY_VERIFY(!isOpen(dialog));

    // Save as… asks where, from the file it was, and the conflict ends there.
    shell.backend->openFiles({other});
    Document *second = shell.document();
    second->buffer()->replace({0, 5}, QStringLiteral("kept"));
    QVERIFY(QFile::remove(other));
    QTRY_COMPARE_WITH_TIMEOUT(second->state(), Document::State::Conflict, 3000);
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")));
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QTRY_VERIFY(isOpen(dialog));
    click(window, footButton(dialog, QStringLiteral("Save as…")));
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.lastSuggestion.toLocalFile(), other);
    QTRY_VERIFY(!isOpen(dialog));
    const QString elsewhere = base + QStringLiteral("/elsewhere.md");
    emit picker.saveSelected(QUrl::fromLocalFile(elsewhere));
    QTRY_COMPARE(readFile(elsewhere), QByteArray("kept\n"));
    QTRY_COMPARE(second->state(), Document::State::Clean);
    QCOMPARE(second->path(), elsewhere);
    QVERIFY(!QFile::exists(other));
    QCOMPARE(shell.settings()->lastFolder(), base);

    // A file that is no longer valid UTF-8 is described, not shown as garbage.
    second->buffer()->replace({0, 0}, QStringLiteral("more "));
    writeFile(elsewhere, QByteArray("caf\xe9\n"));
    QTRY_COMPARE_WITH_TIMEOUT(second->state(), Document::State::Conflict, 3000);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(buttonTexts(dialog), (QStringList{"Reload", "Keep both", "Overwrite", "Cancel"}));
    QTRY_COMPARE(dialog->property("diskProblem").toString(),
                 QStringLiteral("The file is not valid UTF-8 text, so it is not shown."));
    QCOMPARE(dialog->property("diskText").toString(), QString());
    QCOMPARE(shell.object(QStringLiteral("diskPane"))->property("note").toString(), dialog->property("diskProblem").toString());
}

void ParchTests::aLinkInTheFilesPlaceOffersToFollowIt()
{
    QTemporaryDir files, elsewhere;
    QVERIFY(files.isValid() && elsewhere.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/plan.md");
    const QString target = canonical(elsewhere.path()) + QStringLiteral("/precious.txt");
    writeFile(file, "base\n");
    writeFile(target, "precious\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(target, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    // The bar names where the path leads now.
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")));
    QQuickItem *row = shell.item(QStringLiteral("conflictNotice"));
    const QString words = findItem(row, QStringLiteral("noticeText"))->property("text").toString();
    QVERIFY2(words.contains(QStringLiteral("“plan.md” now leads to “%1”").arg(target)), qPrintable(words));
    // Its review offers to follow the link, keep both, put this text at
    // the document's name or save elsewhere, never to overwrite.
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(buttonTexts(dialog),
             (QStringList{"Follow the link", "Keep both", "Put this text at “plan.md”", "Save as…", "Cancel"}));
    // Putting the text there replaces the link, and leaves its file alone.
    click(window, footButton(dialog, QStringLiteral("Put this text at “plan.md”")));
    QTRY_VERIFY(!isOpen(dialog));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), file);
    QVERIFY(!QFileInfo(file).isSymLink());
    QCOMPARE(readFile(file), QByteArray("base\n"));
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    // Following the link is the other way out.
    QTest::qWait(300);
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(target, file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")));
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QTRY_VERIFY(isOpen(dialog));
    click(window, footButton(dialog, QStringLiteral("Follow the link")));
    QTRY_VERIFY(!isOpen(dialog));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), target);
    QCOMPARE(document->buffer()->text(), QStringLiteral("precious\n"));
}

void ParchTests::aDocumentWithoutTextOffersItsHistory()
{
    // Opened again at a path that leads to another file now, a document
    // with no text of its own takes no typing, and its review offers to
    // follow the link or to look through its history, and nothing to save.
    QTemporaryDir files, elsewhere;
    QVERIFY(files.isValid() && elsewhere.isValid());
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    const QString file = canonical(files.path()) + QStringLiteral("/plan.md");
    const QString target = canonical(elsewhere.path()) + QStringLiteral("/precious.txt");
    writeFile(file, "base\n");
    writeFile(target, "precious\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    keepDraft(shell.document());
    shell.backend->openFiles({file});
    TabModel *tabs = shell.context()->tabs();
    QCOMPARE(tabs->rowCount(), 2);
    shell.backend->requestCloseTab(1);
    QTRY_COMPARE(tabs->rowCount(), 1);
    QVERIFY(QFile::remove(file));
    QVERIFY(QFile::link(target, file));
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QCOMPARE(document->path(), file);
    QVERIFY(document->isTextless());
    shell.item(QStringLiteral("editor"))->forceActiveFocus();
    typeText(window, QStringLiteral("typed"));
    QCOMPARE(document->buffer()->text(), QString());
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")));
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(buttonTexts(dialog), (QStringList{"Follow the link", "Version history…", "Cancel"}));
    // What a choice is said to do never speaks of a text the document has not.
    const auto described = [](QQuickItem *button) {
        QAccessibleInterface *access = button ? QAccessible::queryAccessibleInterface(button) : nullptr;
        return access ? access->text(QAccessible::Description) : QStringLiteral("(none)");
    };
    QCOMPARE(described(footButton(dialog, QStringLiteral("Follow the link"))),
             QStringLiteral("Take the text of “%1”, and save there from now on.").arg(target));
    click(window, footButton(dialog, QStringLiteral("Version history…")));
    QTRY_VERIFY(!isOpen(dialog));
    QObject *history = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(history));
    QCOMPARE(history->property("document").value<Document *>(), document);
    QCOMPARE(readFile(target), QByteArray("precious\n"));
    QMetaObject::invokeMethod(history, "close");
    QTRY_VERIFY(!isOpen(history));
    // The link gone, and then a file back at its name, it is offered only
    // what can be done without a text of its own.
    QVERIFY(QFile::remove(file));
    QTRY_VERIFY_WITH_TIMEOUT(document->fileMissing(), 3000);
    writeFile(file, "back\n");
    QTRY_VERIFY_WITH_TIMEOUT(!document->fileMissing(), 3000);
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")));
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(buttonTexts(dialog), (QStringList{"Reload", "Version history…", "Cancel"}));
    QCOMPARE(described(footButton(dialog, QStringLiteral("Reload"))), QStringLiteral("Take the file's text."));
    click(window, footButton(dialog, QStringLiteral("Reload")));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->buffer()->text(), QStringLiteral("back\n"));
}

void ParchTests::aHeldTabNamesWhereItsPathLeadsFromHome()
{
    // A tab whose text recovery holds says where its path leads now as a
    // notice shows a path, from ~: in the editor's place, in the status
    // line, and to assistive technology.
    QTemporaryDir root;
    // The documents are in the test's own home.
    QTemporaryDir home(QDir::homePath() + QStringLiteral("/parch-XXXXXX"));
    QVERIFY(root.isValid() && home.isValid());
    HeldEntry held;
    leaveAHeldEntry(AppPaths::under(root.path()), canonical(home.path()), held);
    if (QTest::currentTestFailed()) return;
    ShellFixture shell({.root = root.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    TabModel *tabs = shell.context()->tabs();
    const int tab = tabs->find(held.plan);
    QVERIFY(tab >= 0);
    tabs->setActiveIndex(tab);
    QVERIFY(!tabs->documentAt(tab));
    const QString folder = QStringLiteral("~/") + QFileInfo(canonical(home.path())).fileName();
    const QString said =
        QStringLiteral("The text of “plan.md” is kept in recovery, and its path now leads to “%1/other.md”, so it is not opened")
            .arg(folder);
    QQuickItem *missing = shell.item(QStringLiteral("missingFile"));
    QTRY_VERIFY(missing && missing->isVisible());
    QCOMPARE(shell.item(QStringLiteral("missingFileState"))->property("text").toString(), said);
    QCOMPARE(shell.item(QStringLiteral("missingFilePath"))->property("text").toString(), folder + QStringLiteral("/plan.md"));
    QCOMPARE(shell.item(QStringLiteral("saveState"))->property("text").toString(), said);
    QCOMPARE(tabs->data(tabs->index(tab), TabModel::StateRole).toString(), said);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(shell.window.get(), &problems), qPrintable(problems.join(QStringLiteral("; "))));
}

void ParchTests::saveAgainSaysWhatTookTheName()
{
    // The notice says in one sentence what stopped saving again, naming the
    // folder as the header does, and adds nothing.
    QTemporaryDir files, elsewhere;
    QVERIFY(files.isValid() && elsewhere.isValid());
    const QString shared = canonical(files.path()) + QStringLiteral("/shared");
    const QString file = shared + QStringLiteral("/plan.md");
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    shell.settings()->setLibraryFolders({shared});
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    document->buffer()->replace({0, 0}, QStringLiteral("mine "));
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(QFile::link(elsewhere.filePath(QStringLiteral("planted")), file));
    QTest::qWait(200);
    shell.backend->resolveConflict(DocumentReview::Overwrite);
    QTRY_VERIFY(document->nameTaken());
    QQuickItem *row = shell.item(QStringLiteral("conflictNotice"));
    QVERIFY(row);
    QTRY_COMPARE(findItem(row, QStringLiteral("noticeText"))->property("text").toString(),
                 QStringLiteral("Something else is now at “plan.md” in “shared”, so parch did not save over it."));
    QVERIFY(!QFileInfo::exists(elsewhere.filePath(QStringLiteral("planted"))));
}

void ParchTests::unreadableFileOffersRetryOrSaveAs()
{
    if (::geteuid() == 0) QSKIP("Root reads a file whatever its mode.");
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    const auto readable = QFile::ReadOwner | QFile::WriteOwner;
    const auto restoreMode = qScopeGuard([&] { QFile::setPermissions(file, readable); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QVERIFY(QFile::setPermissions(file, {}));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(document->fileUnreadable());
    QVERIFY(!document->fileMissing());
    // The bar says why, and offers to read it again or to save the text elsewhere.
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")));
    QQuickItem *row = shell.item(QStringLiteral("conflictNotice"));
    QVERIFY(row);
    const QString denied = QString::fromLocal8Bit(std::strerror(EACCES));
    QCOMPARE(findItem(row, QStringLiteral("noticeText"))->property("text").toString(),
             QStringLiteral("“a.md” in “%1” cannot be read (%2). Nothing is saved over it until it can be read.")
                 .arg(shell.backend->folderLabel(file), denied));
    QCOMPARE(noticeButton(shell, QStringLiteral("conflict"))->property("text").toString(), QStringLiteral("Retry"));
    QVERIFY(findItem(row, QStringLiteral("noticeSaveAs"))->isVisible());
    // Its review offers the same, and never to save over a file parch cannot read.
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    QCOMPARE(buttonTexts(dialog), (QStringList{"Retry", "Save as…", "Cancel"}));
    QVERIFY2(shell.object(QStringLiteral("diskPane"))->property("note").toString().contains(denied),
             qPrintable(shell.object(QStringLiteral("diskPane"))->property("note").toString()));
    // Retry while it still cannot be read leaves it waiting.
    click(window, footButton(dialog, QStringLiteral("Retry")));
    QTRY_VERIFY(!isOpen(dialog));
    QTest::qWait(200);
    QCOMPARE(document->state(), Document::State::Conflict);
    QVERIFY(document->fileUnreadable());
    // Once it can be read, it is the file as it was, and the wait is over.
    QVERIFY(QFile::setPermissions(file, readable));
    if (QQuickItem *retry = noticeButton(shell, QStringLiteral("conflict"))) click(window, retry);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->buffer()->text(), QStringLiteral("base\n"));
}

void ParchTests::draftConflictDialogShowsItsOwnFile()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    Document *draft = shell.document();
    DocumentStore *store = shell.backend->store();
    draft->buffer()->replace({0, 0}, QStringLiteral("mine\n"));
    store->save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    writeFile(DocumentStore::draftFile(shell.paths(), draft->id()), "theirs\n");
    draft->buffer()->replace({0, 0}, QStringLiteral("more "));
    store->save(draft);
    QTRY_COMPARE(draft->state(), Document::State::Conflict);
    QTRY_VERIFY(noticeButton(shell, QStringLiteral("conflict")) && noticeButton(shell, QStringLiteral("conflict"))->isVisible());
    click(window, noticeButton(shell, QStringLiteral("conflict")));
    QObject *dialog = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(dialog));
    // The draft's own file is read and shown, never waited for; a copy has no place beside it.
    QCOMPARE(buttonTexts(dialog), (QStringList{"Reload", "Overwrite", "Cancel"}));
    QTRY_COMPARE(dialog->property("diskText").toString(), QStringLiteral("theirs\n"));
    QVERIFY(!dialog->property("loading").toBool());
    click(window, footButton(dialog, QStringLiteral("Reload")));
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QCOMPARE(draft->buffer()->text(), QStringLiteral("theirs\n"));
    QTRY_VERIFY(!isOpen(dialog));
}

void ParchTests::noticeBarShowsEachStateUntilResolved()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *bar = shell.item(QStringLiteral("noticeBar"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
    QVERIFY(bar && !bar->isVisible());
    QCOMPARE(noticeKinds(shell), QStringList());

    // A notice with no state behind it goes with Dismiss, and the status line leaves it to the bar.
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QCOMPARE(noticeKinds(shell), QStringList{"app"});
    QVERIFY(bar->isVisible());
    QVERIFY(!shell.item(QStringLiteral("problem"))->isVisible());
    // The bar is above the editor, never over it.
    QVERIFY(pane->mapToScene(QPointF(0, 0)).y() >= bar->mapToScene(QPointF(0, bar->height())).y() - 0.5);
    QCOMPARE(noticeButton(shell, QStringLiteral("app"))->property("text").toString(), QStringLiteral("Dismiss"));
    click(window, noticeButton(shell, QStringLiteral("app")));
    QVERIFY(shell.backend->problem().isEmpty());
    QTRY_VERIFY(!bar->isVisible());
    QCOMPARE(window->activeFocusItem(), editor);

    // A failed save and a failed recovery write each have a row and Retry.
    Document *draft = shell.document();
    const AppPaths paths = shell.paths();
    writeFile(paths.drafts() + u'/' + draft->id(), "in the way");
    writeFile(paths.recovery(), "in the way");
    typeText(window, QStringLiteral("words"));
    QTRY_COMPARE(noticeKinds(shell), (QStringList{"saveFailed", "recovery"}));
    QCOMPARE(noticeButton(shell, QStringLiteral("saveFailed"))->property("text").toString(), QStringLiteral("Retry"));
    QCOMPARE(noticeButton(shell, QStringLiteral("recovery"))->property("text").toString(), QStringLiteral("Retry"));
    QVERIFY(findItem(shell.item(QStringLiteral("saveFailedNotice")), QStringLiteral("noticeText"))
                ->property("text").toString().contains(draft->lastError()));
    // Neither goes by itself: only resolving its cause takes it away.
    QTest::qWait(Autosave::RecoveryMaxMs);
    QCOMPARE(noticeKinds(shell), (QStringList{"saveFailed", "recovery"}));
    QVERIFY(QFile::remove(paths.recovery()));
    click(window, noticeButton(shell, QStringLiteral("recovery")));
    QTRY_COMPARE(noticeKinds(shell), QStringList{"saveFailed"});
    bool kept = false;
    for (const Recovery::Entry &entry : Recovery(paths).pending()) kept = kept || entry.text == QStringLiteral("words");
    QVERIFY(kept);
    QVERIFY(QFile::remove(paths.drafts() + u'/' + draft->id()));
    click(window, noticeButton(shell, QStringLiteral("saveFailed")));
    QTRY_COMPARE(draft->state(), Document::State::Clean);
    QTRY_VERIFY(!bar->isVisible());
    QCOMPARE(readFile(DocumentStore::draftFile(paths, draft->id())), QByteArray("words"));

    // A read-only file says why, and has nothing to press.
    const QString broken = files.filePath(QStringLiteral("broken.md"));
    writeFile(broken, QByteArray("caf\xe9\n"));
    shell.backend->openFiles({broken});
    QTRY_COMPARE(noticeKinds(shell), QStringList{"readOnly"});
    QVERIFY(!noticeButton(shell, QStringLiteral("readOnly"))->isVisible());
    // The bar follows the document in front.
    shell.context()->tabs()->setActiveIndex(0);
    QTRY_COMPARE(noticeKinds(shell), QStringList());

    // History says from its own thread that it paused, and the bar shows it
    // until a version goes.
    const QString kept1 = fillHistory(paths, draft->id());
    shell.backend->checkpoint(QStringLiteral("Another"));
    QTRY_VERIFY(shell.backend->historyPaused());
    QTRY_COMPARE(noticeKinds(shell), (QStringList{"history", "app"}));
    QCOMPARE(noticeButton(shell, QStringLiteral("history"))->property("text").toString(), QStringLiteral("Review history"));
    click(window, noticeButton(shell, QStringLiteral("history")));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    shell.backend->removeVersion(kept1);
    QTRY_VERIFY(!shell.backend->historyPaused());
    QTRY_VERIFY(!noticeKinds(shell).contains(QStringLiteral("history")));
}

void ParchTests::noticesReadAsShownToAssistiveTechnology()
{
    // The home folder is one of the test's own, so that it can be named.
    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QByteArray realHome = qgetenv("HOME");
    const auto restoreHome = qScopeGuard([&realHome] { qputenv("HOME", realHome); });
    qputenv("HOME", QFile::encodeName(home.path()));
    // A file that is no regular file cannot be opened, and the notice names it.
    const QString pipe = home.filePath(QStringLiteral("pipe.md"));
    QCOMPARE(::mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    activate(shell.window.get());
    shell.backend->openFiles({pipe});
    QQuickItem *row = shell.item(QStringLiteral("appNotice"));
    QVERIFY(row);
    QTRY_VERIFY(row->isVisible());
    // Assistive technology hears what the screen shows: the home folder as ~.
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(row);
    QVERIFY(access);
    const QString description = access->text(QAccessible::Description);
    QVERIFY2(description.contains(QStringLiteral("“pipe.md” in “~”")), qPrintable(description));
    QVERIFY2(!description.contains(home.path()), qPrintable(description));
}

void ParchTests::damagedRegistryIsKeptAside()
{
    // The registry names every document's identity, and so its history: a
    // damaged one is moved aside, never written over.
    for (const QByteArray &damaged : {QByteArray("{ not json"), QByteArray("[\"an array\"]")}) {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const AppPaths paths = AppPaths::under(root.path());
        writeFile(paths.registryFile(), damaged);
        const QString file = root.filePath(QStringLiteral("a.md"));
        writeFile(file, "alpha\n");
        DocumentStore store(paths);
        QVERIFY(store.open(file));
        store.waitForSaves();
        QCOMPARE(readFile(paths.registryFile() + QStringLiteral(".bad")), damaged);
        QVERIFY(QJsonDocument::fromJson(readFile(paths.registryFile())).isObject());
    }
}

void ParchTests::suggestedNamesAreSafeFileNames()
{
    QCOMPARE(Document::suggestedFileName(QStringLiteral("# Notes\nbody")), QStringLiteral("Notes.md"));
    // The first heading, wherever it is, before the first words.
    QCOMPARE(Document::suggestedFileName(QStringLiteral("Some words first\n\n## Plans ##\n")), QStringLiteral("Plans.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("```\n# a comment\n```\n\n# Real\r\n")), QStringLiteral("Real.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("#hashtag only\n")), QStringLiteral("hashtag only.md"));
    // Nothing reaches another folder or hides the file.
    QCOMPARE(Document::suggestedFileName(QStringLiteral("# Notes/Ideas\n")), QStringLiteral("Notes-Ideas.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("# ../../etc\n")), QStringLiteral("-..-etc.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("# .hidden notes\n")), QStringLiteral("hidden notes.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral(". . .dots\n")), QStringLiteral("dots.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("# tab\there\x01 \u202Egpj.exe\n")), QStringLiteral("tab here gpj.exe.md"));
    // Nothing left is Untitled.
    QCOMPARE(Document::suggestedFileName(QString()), QStringLiteral("Untitled.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("  \n\n#\n")), QStringLiteral("Untitled.md"));
    QCOMPARE(Document::suggestedFileName(QStringLiteral("...\n")), QStringLiteral("Untitled.md"));
    // A long first line is cut between words, within 60 characters.
    const QString words = QStringLiteral("A long opening sentence that keeps going on and on past where any name should stop");
    const QString cut = Document::suggestedFileName(words);
    QVERIFY2(cut.size() <= 63, qPrintable(cut));
    QVERIFY2(words.startsWith(cut.chopped(3) + u' '), qPrintable(cut));
    QCOMPARE(cut, QStringLiteral("A long opening sentence that keeps going on and on past.md"));
    // One long word is cut between characters.
    QCOMPARE(Document::suggestedFileName(QString(80, u'x')), QString(60, u'x') + QStringLiteral(".md"));
    QCOMPARE(Document::suggestedFileName(QString(59, u'x') + QStringLiteral("\U0001F600 more")),
             QString(59, u'x') + QStringLiteral(".md"));

    // The save dialog starts in the folder last used, else Documents, else home.
    QTemporaryDir last, documents, home;
    QVERIFY(last.isValid() && documents.isValid() && home.isValid());
    const QString missing = last.filePath(QStringLiteral("gone"));
    QCOMPARE(Backend::firstFolder({last.path(), documents.path()}, home.path()), last.path());
    QCOMPARE(Backend::firstFolder({missing, documents.path()}, home.path()), documents.path());
    QCOMPARE(Backend::firstFolder({QString(), missing}, home.path()), home.path());
    QCOMPARE(Backend::firstFolder({QStringLiteral("relative"), documents.path()}, home.path()), documents.path());
}

namespace {
// A draft in its own tab whose save fails, since a file stands where its
// folder would go; the tab is in front.
Document *failingDraft(ShellFixture &shell, const QString &text)
{
    shell.backend->newDraft();
    Document *draft = shell.document();
    writeFile(shell.paths().drafts() + u'/' + draft->id(), "in the way");
    draft->buffer()->replace({0, 0}, text);
    shell.backend->store()->save(draft);
    if (!QTest::qWaitFor([&] { return draft->state() == Document::State::SaveFailed; }, 5000)) return nullptr;
    return draft;
}

bool inRecovery(const AppPaths &paths, const QString &documentId)
{
    for (const Recovery::Entry &entry : Recovery(paths).pending())
        if (entry.documentId == documentId) return true;
    return false;
}

// A draft saved once, in its own tab in front, whose next save fails, since
// a folder now stands where its file goes. Its text ends in a full stop.
Document *failingSavedDraft(ShellFixture &shell, const QString &text)
{
    shell.backend->newDraft();
    Document *draft = shell.document();
    draft->buffer()->replace({0, 0}, text);
    shell.backend->store()->save(draft);
    if (!QTest::qWaitFor([&] { return draft->state() == Document::State::Clean; }, 5000)) return nullptr;
    const QString file = DocumentStore::draftFile(shell.paths(), draft->id());
    QFile::remove(file);
    QDir().mkpath(file);
    draft->buffer()->replace({draft->buffer()->length(), 0}, QStringLiteral("."));
    shell.backend->store()->save(draft);
    if (!QTest::qWaitFor([&] { return draft->state() == Document::State::SaveFailed; }, 5000)) return nullptr;
    return draft;
}

bool historyHolds(const ShellFixture &shell, const QString &documentId, const QByteArray &bytes)
{
    History *history = shell.backend->history();
    for (const History::Version &version : history->versions(documentId))
        if (history->read(documentId, version.id) == bytes) return true;
    return false;
}
} // namespace

void ParchTests::closeConfirmationOffersEveryChoice()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString base = canonical(files.path());
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    shell.settings()->setLastFolder(base);
    QQuickWindow *window = shell.window.get();
    activate(window);
    TabModel *tabs = shell.context()->tabs();
    QObject *confirmation = dialogIn(window, "closeConfirmation");
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    const AppPaths paths = shell.paths();
    keepDraft(shell.document());

    // A failed save asks, with Cancel in front; Cancel keeps the tab. A
    // draft never saved has nothing on disk to lead back to its text, so
    // closing it is plainly discarding it.
    Document *first = failingDraft(shell, QStringLiteral("first words"));
    QVERIFY(first);
    QCOMPARE(tabs->rowCount(), 2);
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    QCOMPARE(buttonTexts(confirmation), (QStringList{"Try again", "Save as…", "Discard text", "Cancel"}));
    QVERIFY(confirmation->property("contentItem").value<QQuickItem *>()->property("text").toString().contains(
        QStringLiteral("never been saved")));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    QVERIFY(confirmation->property("contentItem").value<QQuickItem *>()->property("text").toString().contains(first->lastError()));
    click(window, footButton(confirmation, QStringLiteral("Cancel")));
    QTRY_VERIFY(!isOpen(confirmation));
    QCOMPARE(tabs->rowCount(), 2);
    QCOMPARE(window->activeFocusItem(), editor);

    // Try again saves it, once its folder can be made, and then the tab closes.
    const QString firstId = first->id();
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    QVERIFY(QFile::remove(paths.drafts() + u'/' + firstId));
    click(window, footButton(confirmation, QStringLiteral("Try again")));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QCOMPARE(readFile(DocumentStore::draftFile(paths, firstId)), QByteArray("first words"));

    // Save as… asks where, saves it there, and then the tab closes.
    Document *second = failingDraft(shell, QStringLiteral("# Second\nwords"));
    QVERIFY(second);
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    click(window, footButton(confirmation, QStringLiteral("Save as…")));
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.lastSuggestion.toLocalFile(), base + QStringLiteral("/Second.md"));
    QCOMPARE(tabs->rowCount(), 2);
    emit picker.saveSelected(QUrl::fromLocalFile(base + QStringLiteral("/Second.md")));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QCOMPARE(readFile(base + QStringLiteral("/Second.md")), QByteArray("# Second\nwords"));

    // Closing without saving a text that was saved before keeps it in
    // history, gives up its recovery entry, and closes the tab.
    Document *third = failingSavedDraft(shell, QStringLiteral("only here"));
    QVERIFY(third);
    const QString thirdId = third->id();
    QTRY_VERIFY(inRecovery(paths, thirdId));
    QPointer<Document> gone = third;
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    QCOMPARE(buttonTexts(confirmation), (QStringList{"Try again", "Save as…", "Close without saving", "Cancel"}));
    QVERIFY(confirmation->property("contentItem").value<QQuickItem *>()->property("text").toString().contains(
        QStringLiteral("keeps this text in history")));
    click(window, footButton(confirmation, QStringLiteral("Close without saving")));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QTRY_VERIFY(!gone);
    QTRY_VERIFY(!inRecovery(paths, thirdId));
    QVERIFY(historyHolds(shell, thirdId, "only here."));

    // A draft never saved is discarded, though history still takes a copy if it can.
    Document *fourth = failingDraft(shell, QStringLiteral("never kept"));
    QVERIFY(fourth);
    const QString fourthId = fourth->id();
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    click(window, footButton(confirmation, QStringLiteral("Discard text")));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QTRY_VERIFY(!inRecovery(paths, fourthId));
    QVERIFY(historyHolds(shell, fourthId, "never kept"));

    // A conflict offers to review it first.
    const QString file = base + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    shell.backend->openFiles({file});
    Document *named = shell.document();
    named->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(named->state(), Document::State::Conflict, 3000);
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    QCOMPARE(buttonTexts(confirmation), (QStringList{"Review", "Close without saving", "Cancel"}));
    click(window, footButton(confirmation, QStringLiteral("Review")));
    QObject *conflict = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(conflict));
    QTRY_VERIFY(!isOpen(confirmation));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    QTRY_COMPARE(conflict->property("diskText").toString(), QStringLiteral("theirs\n"));
    QCOMPARE(tabs->rowCount(), 2);
}

void ParchTests::closingAWindowAsksAboutItsUnsavedText()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    shell.backend->newWindow();
    WindowContext *second = shell.backend->windows().at(1);
    QQuickWindow *window = shell.windowFor(second);
    QVERIFY(window);
    activate(window);
    shell.backend->activate(second);
    Document *failing = failingDraft(shell, QStringLiteral("unsaved"));
    QVERIFY(failing);
    shell.backend->newDraft();
    TabModel *tabs = second->tabs();
    QCOMPARE(tabs->rowCount(), 3);
    QCOMPARE(tabs->activeIndex(), 2);
    // The first window is in front when the second is asked to close.
    shell.backend->activate(shell.context());
    QSignalSpy removed(shell.backend.get(), &Backend::windowRemoved);
    shell.backend->closeWindow(second);
    QCOMPARE(removed.count(), 0);
    // It stays, with the unsaved document in front and its question open.
    QCOMPARE(tabs->activeIndex(), 1);
    QCOMPARE(tabs->activeDocument(), failing);
    QCOMPARE(shell.backend->activeWindow(), second);
    QTRY_VERIFY(isOpen(dialogIn(window, "closeConfirmation")));
    QVERIFY(!isOpen(dialogIn(shell.window.get(), "closeConfirmation")));
    // Discarded, it goes, and the window can close.
    click(window, footButton(dialogIn(window, "closeConfirmation"), QStringLiteral("Discard text")));
    QTRY_COMPARE(tabs->rowCount(), 2);
    shell.backend->closeWindow(second);
    QCOMPARE(removed.count(), 1);
}

void ParchTests::failedSaveOnClosingAsksInstead()
{
    // Root writes into a read-only folder, so there is nothing to see as root.
    if (::geteuid() == 0) return;
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString folder = canonical(files.path()) + QStringLiteral("/locked"), file = folder + QStringLiteral("/a.md");
    writeFile(file, "alpha\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    keepDraft(shell.document());
    shell.backend->openFiles({file});
    TabModel *tabs = shell.context()->tabs();
    QCOMPARE(tabs->rowCount(), 2);
    Document *document = shell.document();
    const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
    QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
    typeText(window, QStringLiteral("more "));
    QSignalSpy asked(shell.context(), &WindowContext::closeConfirmationRequested);
    // Ctrl+W saves the file first; the save fails, so the tab asks instead of closing.
    QTest::keyClick(window, Qt::Key_W, Qt::ControlModifier);
    QTRY_COMPARE(asked.count(), 1);
    QCOMPARE(document->state(), Document::State::SaveFailed);
    QCOMPARE(tabs->rowCount(), 2);
    QTRY_VERIFY(isOpen(dialogIn(window, "closeConfirmation")));
    QCOMPARE(readFile(file), QByteArray("alpha\n"));
    // Once the folder can be written, Try again saves and closes.
    QVERIFY(QFile::setPermissions(folder, writable));
    click(window, footButton(dialogIn(window, "closeConfirmation"), QStringLiteral("Try again")));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QCOMPARE(readFile(file), QByteArray("more alpha\n"));
}

void ParchTests::pickerAnswersGoWhereAsked()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString base = canonical(files.path());
    const QString a = base + QStringLiteral("/a.md");
    writeFile(a, "alpha\n");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Backend backend(AppPaths::under(root.path()));
    // A dialog with no folder used last starts in the test's own folder.
    backend.fileRequests()->setFallbackFolders({root.path()});
    backend.start({});
    WindowContext *first = backend.activeWindow();
    TabModel *tabs = first->tabs();
    Document *draft = backend.document();
    draft->buffer()->replace({0, 0}, QStringLiteral("words"));

    // Without a file dialog, each action says so and changes nothing.
    for (const auto &action : {&Backend::open, &Backend::openFolder, &Backend::save, &Backend::saveAs}) {
        (backend.*action)();
        QCOMPARE(backend.problem(), QStringLiteral("No file dialog"));
        QVERIFY(backend.notice());
        backend.dismissNotice();
        QVERIFY(backend.problem().isEmpty());
    }
    QVERIFY(draft->isDraft());

    // A dialog that is cancelled answers nothing, and nothing changes.
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.settings()->setLastFolder(base);
    backend.open();
    backend.openFolder();
    backend.saveAs();
    backend.save();
    QCOMPARE(picker.opens, 1);
    QCOMPARE(picker.folders, 1);
    QCOMPARE(picker.saves, 2);
    QCOMPARE(picker.lastSuggestion.toLocalFile(), base + QStringLiteral("/words.md"));
    QCoreApplication::processEvents();
    QCOMPARE(tabs->rowCount(), 1);
    QVERIFY(backend.settings()->libraryFolders().isEmpty());
    QVERIFY(draft->isDraft());
    QCOMPARE(draft->buffer()->text(), QStringLiteral("words"));
    QVERIFY(backend.problem().isEmpty());
    // An answer to a question no longer asked, or to another question, is ignored.
    emit picker.documentsSelected({QUrl::fromLocalFile(a)});
    emit picker.folderSelected(QUrl::fromLocalFile(base));
    QCOMPARE(tabs->rowCount(), 1);
    QVERIFY(backend.settings()->libraryFolders().isEmpty());

    // A dialog that fails says why.
    backend.open();
    emit picker.failed(QStringLiteral("The portal went away"));
    QCOMPARE(backend.problem(), QStringLiteral("The file dialog failed"));
    QCOMPARE(backend.problemDetail(), QStringLiteral("The portal went away"));
    QVERIFY(backend.notice());
    emit picker.documentsSelected({QUrl::fromLocalFile(a)});
    QCOMPARE(tabs->rowCount(), 1);

    // An answer goes to the window that asked, though another is in front now.
    backend.open();
    backend.newWindow();
    WindowContext *second = backend.windows().at(1);
    backend.activate(second);
    QSignalSpy raised(first, &WindowContext::raiseRequested);
    emit picker.documentsSelected({QUrl::fromLocalFile(a)});
    QCOMPARE(tabs->rowCount(), 2);
    QCOMPARE(tabs->activeDocument()->path(), a);
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(raised.count(), 1);
    QCOMPARE(backend.settings()->lastFolder(), base);
    // Only local files are opened.
    backend.open();
    emit picker.documentsSelected({QUrl(QStringLiteral("https://example.invalid/b.md"))});
    QCOMPARE(backend.problem(), QStringLiteral("Some files could not be opened"));
    QVERIFY2(backend.problemDetail().contains(QStringLiteral("https://example.invalid/b.md")), qPrintable(backend.problemDetail()));
    QCOMPARE(second->tabs()->rowCount(), 1);

    // A draft standing for a file not made yet is offered that file.
    const QString fresh = base + QStringLiteral("/fresh.md");
    backend.activate(second);
    backend.openFiles({fresh});
    backend.save();
    QCOMPARE(picker.lastSuggestion.toLocalFile(), fresh);
    // Saved somewhere else, that folder is where the next dialog starts,
    // after a restart too.
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QString target = canonical(other.path()) + QStringLiteral("/kept.md");
    backend.newDraft();
    backend.document()->buffer()->replace({0, 0}, QStringLiteral("kept"));
    backend.save();
    emit picker.saveSelected(QUrl::fromLocalFile(target));
    QTRY_COMPARE(readFile(target), QByteArray("kept"));
    QTRY_COMPARE(backend.settings()->lastFolder(), canonical(other.path()));
    QCOMPARE(Settings(AppPaths::under(root.path()).settingsFile()).lastFolder(), canonical(other.path()));
}

void ParchTests::failedNamingIsTriedAgainThere()
{
    // Root writes into a read-only folder, so there is nothing to see as root.
    if (::geteuid() == 0) return;
    QTemporaryDir files, root;
    QVERIFY(files.isValid() && root.isValid());
    const QString folder = canonical(files.path()) + QStringLiteral("/locked");
    QVERIFY(QDir().mkpath(folder));
    const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
    QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    // A dialog with no folder used last starts in the test's own folder.
    backend.fileRequests()->setFallbackFolders({paths.config});
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.start({});
    Document *draft = backend.document();
    draft->buffer()->replace({0, 0}, QStringLiteral("words"));
    backend.saveAs();
    const QString target = folder + QStringLiteral("/words.md");
    emit picker.saveSelected(QUrl::fromLocalFile(target));
    QTRY_COMPARE(draft->state(), Document::State::SaveFailed);
    QVERIFY(draft->isDraft());
    // Trying again names it where it was asked to go, not merely as the draft it still is.
    QVERIFY(QFile::setPermissions(folder, writable));
    backend.retrySave();
    QTRY_COMPARE(readFile(target), QByteArray("words"));
    QTRY_VERIFY(!draft->isDraft());
    QCOMPARE(draft->path(), target);
    QCOMPARE(draft->state(), Document::State::Clean);
}

void ParchTests::pickedFileAlreadyOpenComesToItsTab()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString a = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(a, "alpha\n");
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Backend backend(AppPaths::under(root.path()));
    // A dialog with no folder used last starts in the test's own folder.
    backend.fileRequests()->setFallbackFolders({root.path()});
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.start({});
    keepDraft(backend.document());
    backend.openFiles({a});
    WindowContext *first = backend.activeWindow();
    Document *document = backend.document();
    backend.newDraft();
    QCOMPARE(first->tabs()->activeIndex(), 2);
    backend.newWindow();
    WindowContext *second = backend.windows().at(1);
    backend.activate(second);
    QSignalSpy raised(first, &WindowContext::raiseRequested);
    // The second window asks for a file the first already shows.
    backend.open();
    emit picker.documentsSelected({QUrl::fromLocalFile(a)});
    QCOMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(first->tabs()->rowCount(), 3);
    QCOMPARE(first->tabs()->activeIndex(), 1);
    QCOMPARE(first->tabs()->activeDocument(), document);
    QCOMPARE(raised.count(), 1);
}

void ParchTests::dialogsTakeTheKeysAndGiveThemBack()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString base = canonical(files.path());
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    shell.settings()->setLastFolder(base);
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));

    // Ctrl+S on a draft, as keys, reaches the file dialog with its name.
    typeText(window, QStringLiteral("# Plans"));
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.lastSuggestion.fileName(), QStringLiteral("Plans.md"));
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(picker.saves, 2);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QCOMPARE(picker.opens, 1);
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("# Plans"));

    // While the bar shows, keys still type in the text, and Tab reaches its button.
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QTRY_VERIFY(shell.item(QStringLiteral("noticeBar"))->isVisible());
    QCOMPARE(window->activeFocusItem(), editor);
    typeText(window, QStringLiteral(" now"));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("# Plans now"));
    QCOMPARE(window->activeFocusItem(), editor);
    QQuickItem *dismiss = noticeButton(shell, QStringLiteral("app"));
    QVERIFY(dismiss->activeFocusOnTab());
    dismiss->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QVERIFY(shell.backend->problem().isEmpty());
    QTRY_COMPARE(window->activeFocusItem(), editor);

    // Each dialog takes the keys on its safe choice, keeps Tab inside it,
    // cancels on Escape, and gives the keys back to the text.
    const QString file = base + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    shell.backend->openFiles({file});
    Document *document = shell.document();
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    editor->forceActiveFocus();
    const auto tabsAround = [&](QObject *dialog) {
        auto *inside = dialog->property("contentItem").value<QQuickItem *>()->parentItem();
        QList<QQuickItem *> reached;
        for (int i = 0; i < 12; ++i) {
            QQuickItem *now = window->activeFocusItem();
            if (!now || reached.contains(now)) break;
            reached << now;
            QTest::keyClick(window, Qt::Key_Tab);
        }
        for (QQuickItem *item : std::as_const(reached))
            if (!inside->isAncestorOf(item)) return QStringLiteral("Tab left the dialog for %1").arg(describe(item));
        return reached.size() >= 2 ? QString() : QStringLiteral("Tab reached %1 controls").arg(reached.size());
    };

    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *conflict = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(conflict));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    QVERIFY(window->activeFocusItem()->property("visualFocus").toBool());
    // Enter on the safe choice cancels: no text is lost.
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(conflict));
    QCOMPARE(document->state(), Document::State::Conflict);
    QCOMPARE(document->buffer()->text(), QStringLiteral("mine\n"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    // Opened from the notice bar's button by the keys, it gives them to the text, not the button.
    QQuickItem *review = noticeButton(shell, QStringLiteral("conflict"));
    QVERIFY(review);
    review->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(isOpen(conflict));
    const QString conflictTabs = tabsAround(conflict);
    QVERIFY2(conflictTabs.isEmpty(), qPrintable(conflictTabs));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(conflict));
    QCOMPARE(document->state(), Document::State::Conflict);
    QTRY_COMPARE(window->activeFocusItem(), editor);

    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *history = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(history));
    const QString historyTabs = tabsAround(history);
    QVERIFY2(historyTabs.isEmpty(), qPrintable(historyTabs));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(history));
    QTRY_COMPARE(window->activeFocusItem(), editor);

    const int tabs = shell.context()->tabs()->rowCount();
    shell.backend->requestCloseTab(shell.context()->tabs()->activeIndex());
    QObject *confirmation = dialogIn(window, "closeConfirmation");
    QTRY_VERIFY(isOpen(confirmation));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    const QString confirmationTabs = tabsAround(confirmation);
    QVERIFY2(confirmationTabs.isEmpty(), qPrintable(confirmationTabs));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(confirmation));
    QCOMPARE(shell.context()->tabs()->rowCount(), tabs);
    QTRY_COMPARE(window->activeFocusItem(), editor);

    // Another document coming to the front ends a dialog about the one before.
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QTRY_VERIFY(isOpen(history));
    shell.context()->tabs()->setActiveIndex(0);
    QTRY_VERIFY(!isOpen(history));
}

void ParchTests::dialogsKeepTheKeysUntilTheyClose()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    const auto inside = [window](QObject *dialog) {
        auto *popup = dialog->property("contentItem").value<QQuickItem *>()->parentItem();
        QQuickItem *focused = window->activeFocusItem();
        return focused && popup->isAncestorOf(focused);
    };
    // Typed keys and Tab stay in the dialog, Escape closes it, and the keys
    // are then back in the text, or where else they are to go.
    const auto keepsThem = [&](QObject *dialog, const QString &text, const char *what, QQuickItem *then = nullptr) {
        QTest::qWait(100);
        QVERIFY2(inside(dialog), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(what), describe(window->activeFocusItem()))));
        QTest::keyClick(window, Qt::Key_Z);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Z);
        QCOMPARE(document->buffer()->text(), text);
        QVERIFY(isOpen(dialog));
        QVERIFY2(inside(dialog), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(what), describe(window->activeFocusItem()))));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!isOpen(dialog));
        QTRY_COMPARE(window->activeFocusItem(), then ? then : editor);
        QCOMPARE(document->buffer()->text(), text);
    };

    // Version history, chosen in the window's menu by the keys: the menu
    // closes after the dialog has opened, and leaves the keys with it.
    QQuickItem *more = shell.item(QStringLiteral("moreButton"));
    more->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QObject *menu = shell.object(QStringLiteral("windowMenu"));
    QTRY_VERIFY(isOpen(menu));
    for (int i = 0; i < 20; ++i) {
        QQuickItem *entry = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, entry),
                                  Q_ARG(int, menu->property("currentIndex").toInt()));
        if (entry && entry->objectName() == QStringLiteral("historyEntry")) break;
        QTest::keyClick(window, Qt::Key_Down);
    }
    QTest::keyClick(window, Qt::Key_Return);
    QObject *history = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(history));
    QTRY_VERIFY(!isOpen(menu));
    keepsThem(history, QStringLiteral("base\n"), "history from the menu");
    if (QTest::currentTestFailed()) return;

    // A forwarded launch of the file in front leaves each dialog the keys.
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QTRY_VERIFY(isOpen(history));
    shell.backend->openFiles({file});
    keepsThem(history, QStringLiteral("base\n"), "history, forwarded");
    if (QTest::currentTestFailed()) return;

    // So does a forwarded folder, alone or with the file in front, beside the
    // editor or over it: the folder joins the library without the keys.
    const QString folder = canonical(files.path()) + QStringLiteral("/Essays");
    writeFile(folder + QStringLiteral("/inside.md"), "inside\n");
    for (const int width : {1440, 360}) {
        resizeWindow(window, width);
        for (const bool withFile : {false, true}) {
            QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
            QTRY_VERIFY(isOpen(history));
            shell.backend->openFiles(withFile ? QStringList{folder, file} : QStringList{folder});
            const QByteArray what = QStringLiteral("history, forwarded %1 at %2")
                                        .arg(withFile ? QStringLiteral("folder and file") : QStringLiteral("folder"))
                                        .arg(width)
                                        .toUtf8();
            keepsThem(history, QStringLiteral("base\n"), what.constData());
            if (QTest::currentTestFailed()) return;
            QVERIFY(!shell.object(QStringLiteral("libraryDrawer"))->property("visible").toBool());
        }
    }
    QVERIFY(shell.settings()->libraryFolders().contains(folder));
    // A dialog that opens while the library slides in over the editor keeps
    // the keys once the slide is over; when it closes, the library, still
    // open over the text, has them, in its field.
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QVERIFY(!drawer->property("visible").toBool());
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QVERIFY(drawer->property("visible").toBool() && !drawer->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QTRY_VERIFY(isOpen(history));
    QTRY_VERIFY(drawer->property("opened").toBool());
    // The library's items are in the window while it is shown over the text.
    QQuickItem *librarySearch = shell.item(QStringLiteral("librarySearch"));
    QQuickItem *libraryTree = shell.item(QStringLiteral("libraryTree"));
    QVERIFY(librarySearch && libraryTree);
    keepsThem(history, QStringLiteral("base\n"), "history, opened as the library slid in", librarySearch);
    if (QTest::currentTestFailed()) return;
    QMetaObject::invokeMethod(drawer, "close");
    QTRY_VERIFY(!drawer->property("visible").toBool());
    // A folder forwarded on the slide is shown once the library is open,
    // without the keys, which its row has once the dialog closes.
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QVERIFY(drawer->property("visible").toBool() && !drawer->property("opened").toBool());
    shell.backend->openFiles({folder});
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QTRY_VERIFY(isOpen(history));
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTRY_VERIFY(libraryRow(window, QStringLiteral("inside.md")));
    keepsThem(history, QStringLiteral("base\n"), "history, opened as the library slid in to a folder", libraryTree);
    if (QTest::currentTestFailed()) return;
    QMetaObject::invokeMethod(drawer, "close");
    QTRY_VERIFY(!drawer->property("visible").toBool());
    resizeWindow(window, 960);

    // While a dialog is open the editor takes no keys, even if it is given them.
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QTRY_VERIFY(isOpen(history));
    editor->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_Z);
    QCOMPARE(document->buffer()->text(), QStringLiteral("base\n"));
    QVERIFY(QMetaObject::invokeMethod(history, "close"));
    QTRY_VERIFY(!isOpen(history));

    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *conflict = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(conflict));
    shell.backend->openFiles({file});
    keepsThem(conflict, QStringLiteral("mine\n"), "conflict, forwarded");
    if (QTest::currentTestFailed()) return;

    shell.backend->requestCloseTab(shell.context()->tabs()->activeIndex());
    QObject *confirmation = dialogIn(window, "closeConfirmation");
    QTRY_VERIFY(isOpen(confirmation));
    shell.backend->openFiles({file});
    keepsThem(confirmation, QStringLiteral("mine\n"), "close confirmation, forwarded");
}

void ParchTests::keysReachEveryRegionAndNotice()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QVERIFY(shell.item(QStringLiteral("library"))->isVisible());
    const auto focused = [window] { return window->activeFocusItem(); };
    // F6 and Shift+F6 go round the window's regions, as GTK applications do.
    const auto step = [&](bool back, QQuickItem *expected, const char *what) {
        QTest::keyClick(window, Qt::Key_F6, back ? Qt::ShiftModifier : Qt::NoModifier);
        QTRY_VERIFY2(focused() == expected, qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(what), describe(focused()))));
    };
    step(false, search, "library from the text");
    step(false, editor, "text from the library, with no notice");
    step(true, search, "back to the library");
    step(true, editor, "back to the text");
    if (QTest::currentTestFailed()) return;

    // With a notice, its first button is a region of its own.
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QTRY_COMPARE(noticeKinds(shell), QStringList{QStringLiteral("app")});
    QQuickItem *dismiss = noticeButton(shell, QStringLiteral("app"));
    step(false, search, "library before the notice");
    step(false, dismiss, "the notice's button");
    step(false, editor, "the text after the notice");
    step(true, dismiss, "back to the notice");
    step(true, search, "back to the library");
    step(true, editor, "back round to the text");
    if (QTest::currentTestFailed()) return;
    // A hidden library is passed over.
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(!shell.item(QStringLiteral("library"))->isVisible());
    QTRY_COMPARE(focused(), editor);
    step(false, dismiss, "the notice, with the library hidden");
    step(false, editor, "the text, with the library hidden");
    if (QTest::currentTestFailed()) return;

    // Escape in the text dismisses a notice with nothing behind it, and
    // otherwise does nothing.
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(shell.backend->problem().isEmpty());
    QTRY_VERIFY(noticeKinds(shell).isEmpty());
    QCOMPARE(focused(), editor);
    QTest::keyClick(window, Qt::Key_Escape);
    QCOMPARE(focused(), editor);
    QCOMPARE(document->buffer()->text(), QStringLiteral("base\n"));
    step(false, editor, "the text alone");
    if (QTest::currentTestFailed()) return;

    // A conflict's notice stays, and its Review is reached by F6; Ctrl+S opens it too.
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QTRY_COMPARE(noticeKinds(shell), (QStringList{QStringLiteral("conflict"), QStringLiteral("app")}));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE(noticeKinds(shell), QStringList{QStringLiteral("conflict")});
    step(false, noticeButton(shell, QStringLiteral("conflict")), "the conflict's Review");
    step(false, editor, "the text after the conflict");
    if (QTest::currentTestFailed()) return;
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QObject *conflict = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(conflict));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(conflict));
    QTRY_COMPARE(focused(), editor);
    QCOMPARE(readFile(file), QByteArray("theirs\n"));

    // Narrow, the library over the editor counts only while it is open, and
    // leaving it closes it.
    resizeWindow(window, 360);
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    step(false, noticeButton(shell, QStringLiteral("conflict")), "the conflict, narrow");
    step(false, editor, "the text, narrow");
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTRY_COMPARE(focused(), search);
    step(false, noticeButton(shell, QStringLiteral("conflict")), "the conflict, from the drawer");
    QTRY_VERIFY(!drawer->property("visible").toBool());
}

void ParchTests::windowMenuOpensFromTheKeyboard()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    TabModel *tabs = shell.context()->tabs();
    for (const char *title : {"First", "Second"}) {
        Document *document = shell.backend->store()->newDraft();
        document->buffer()->replace({0, 0}, QString::fromLatin1(title));
        tabs->openDocument(document);
    }
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    editor->forceActiveFocus();
    QObject *menu = shell.object(QStringLiteral("windowMenu"));
    QVERIFY(menu);
    const auto entry = [menu](int index) {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, index));
        return item;
    };
    const auto current = [&] { return entry(menu->property("currentIndex").toInt()); };
    const auto moveTo = [&](const QString &text) {
        for (int i = 0; i < 30 && current() && current()->property("text").toString() != text; ++i)
            QTest::keyClick(window, Qt::Key_Down);
        return current() && current()->property("text").toString() == text;
    };

    // F10 opens the menu with its first entry highlighted, and the keys on it.
    QTest::keyClick(window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QTRY_COMPARE(menu->property("currentIndex").toInt(), 0);
    QVERIFY(entry(0)->property("highlighted").toBool());
    QTRY_VERIFY(entry(0)->hasActiveFocus());
    // Every entry with keys names them from the shortcut's own text.
    QHash<QString, QString> keysOf;
    for (int i = 0; i < menu->property("count").toInt(); ++i)
        if (QQuickItem *item = entry(i)) keysOf.insert(item->property("text").toString(), item->property("keys").toString());
    QVERIFY(keysOf.contains(QStringLiteral("Close tab")));
    QCOMPARE(keysOf.value(QStringLiteral("Close tab")),
             shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+W")))->property("nativeText").toString());
    QVERIFY(keysOf.contains(QStringLiteral("Find a document…")));
    QCOMPARE(keysOf.value(QStringLiteral("Find a document…")),
             shortcutFor(window, QKeySequence(QStringLiteral("Ctrl+Shift+O")))->property("nativeText").toString());
    // The menu button says which key opens the menu.
    QCOMPARE(shell.item(QStringLiteral("moreButton"))->property("keys").toString(),
             shortcutFor(window, QKeySequence(Qt::Key_F10))->property("nativeText").toString());
    // The arrows and Enter choose: Close tab closes the tab in front.
    const int count = tabs->rowCount();
    QVERIFY(moveTo(QStringLiteral("Close tab")));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(menu));
    QTRY_COMPARE(tabs->rowCount(), count - 1);
    QTRY_COMPARE(window->activeFocusItem(), editor);

    // Find a document shows the library, hidden or not, with the keys in its field.
    QQuickItem *library = shell.item(QStringLiteral("library"));
    QQuickItem *search = shell.item(QStringLiteral("librarySearch"));
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY(!library->isVisible());
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(library->isVisible());
    QTRY_COMPARE(window->activeFocusItem(), search);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QTest::keyClick(window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QVERIFY(moveTo(QStringLiteral("Find a document…")));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(menu));
    QTRY_COMPARE(window->activeFocusItem(), search);
    // Narrow, the library over the editor opens on its field.
    resizeWindow(window, 360);
    editor->forceActiveFocus();
    QObject *drawer = shell.object(QStringLiteral("libraryDrawer"));
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(drawer->property("opened").toBool());
    QTRY_COMPARE(window->activeFocusItem(), search);
}

void ParchTests::windowMenuActsOnItsOwnWindow()
{
    // A click in a window's menu can come before the backend has heard that
    // the window came to the front: what it asks for is done in that window.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/b.md");
    writeFile(file, "beta\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    WindowContext *first = shell.context();
    first->tabs()->activeDocument()->buffer()->replace({0, 0}, QStringLiteral("a draft"));
    first->tabs()->openDocument(shell.backend->store()->newDraft());
    shell.backend->newWindow();
    WindowContext *second = shell.backend->windows().at(1);
    QQuickWindow *secondWindow = shell.windowFor(second);
    QVERIFY(secondWindow);
    keepDraft(second->tabs()->activeDocument());
    shell.backend->activate(second);
    shell.backend->openFiles({file});
    QCOMPARE(second->tabs()->rowCount(), 2);
    Document *named = second->tabs()->activeDocument();
    QCOMPARE(named->path(), file);
    named->buffer()->replace({0, 0}, QStringLiteral("more "));
    QObject *menu = findObject(secondWindow, QStringLiteral("windowMenu"));
    QVERIFY(menu);
    // The first window is the one the backend has in front.
    shell.backend->activate(first);
    QVERIFY(QMetaObject::invokeMethod(menu, "save"));
    QCOMPARE(picker.saves, 0);
    QVERIFY(named->isSaving());
    QTRY_COMPARE(readFile(file), QByteArray("more beta\n"));
    shell.backend->activate(first);
    QVERIFY(QMetaObject::invokeMethod(menu, "closeTab"));
    QTRY_COMPARE(second->tabs()->rowCount(), 1);
    QCOMPARE(first->tabs()->rowCount(), 2);
}

void ParchTests::failedSaveNamesItsFolderAndOffersSaveAs()
{
    if (::geteuid() == 0) QSKIP("Permissions do not hold back root");
    // A folder in the test's own home, so that the message names it from ~.
    QTemporaryDir home(QDir::homePath() + QStringLiteral("/parch-XXXXXX"));
    QVERIFY(home.isValid());
    const QString base = canonical(home.path());
    const QString folder = base + QStringLiteral("/Saved");
    QVERIFY(QDir().mkpath(folder));
    const QString file = folder + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    FakePicker picker;
    shell.backend->setFilePicker(&picker);
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const auto restore = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
    QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE(document->state(), Document::State::SaveFailed);
    // The folder that could not be written is named, from ~.
    QTRY_COMPARE(noticeKinds(shell), QStringList{QStringLiteral("saveFailed")});
    QQuickItem *row = shell.item(QStringLiteral("saveFailedNotice"));
    QVERIFY(row);
    const QString shown = QStringLiteral("~/") + QFileInfo(base).fileName() + QStringLiteral("/Saved");
    QCOMPARE(findItem(row, QStringLiteral("noticeText"))->property("text").toString(),
             QStringLiteral("Not saved: parch cannot write in “%1” (Permission denied)").arg(shown));
    // Save as… sits beside Retry, and asks where, as the conflict dialog's does.
    QQuickItem *retry = findItem(row, QStringLiteral("noticeAction"));
    QQuickItem *saveAs = findItem(row, QStringLiteral("noticeSaveAs"));
    QVERIFY(retry && saveAs);
    QCOMPARE(retry->property("text").toString(), QStringLiteral("Retry"));
    QCOMPARE(saveAs->property("text").toString(), QStringLiteral("Save as…"));
    QVERIFY(saveAs->isVisible());
    QVERIFY(saveAs->activeFocusOnTab());
    saveAs->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.lastSuggestion.toLocalFile(), file);
    const QString elsewhere = base + QStringLiteral("/b.md");
    emit picker.saveSelected(QUrl::fromLocalFile(elsewhere));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), elsewhere);
    QCOMPARE(readFile(elsewhere), QByteArray("mine\n"));
    QCOMPARE(readFile(file), QByteArray("base\n"));
    QTRY_VERIFY(noticeKinds(shell).isEmpty());
}

void ParchTests::noticesMarkFailuresApartFromNews()
{
    // Text that came back from recovery is good news, and its mark is a calm
    // one; a notice of something that failed keeps the danger mark.
    QTemporaryDir app;
    QVERIFY(app.isValid());
    const AppPaths paths = AppPaths::under(app.path());
    {
        DocumentStore store(paths);
        Recovery recovery(paths);
        Document *draft = store.newDraft();
        draft->buffer()->replace({0, 0}, QStringLiteral("Lost draft\n"));
        QVERIFY(recovery.write(draft->id(), {}, *draft->buffer()->snapshot(), draft->diskVersion()));
    }
    ShellFixture shell({.root = app.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    activate(shell.window.get());
    QTRY_COMPARE(noticeKinds(shell), QStringList{QStringLiteral("app")});
    QCOMPARE(shell.backend->problem(), QStringLiteral("Restored from recovery"));
    const auto markColour = [&] {
        QQuickItem *row = shell.item(QStringLiteral("appNotice"));
        QQuickItem *mark = row ? findItem(row, QStringLiteral("noticeMark")) : nullptr;
        return mark ? mark->property("color").value<QColor>().rgba() : QRgb(0);
    };
    QCOMPARE(markColour(), shell.theme->dim().rgba());
    QVERIFY(markColour() != shell.theme->danger().rgba());
    // The news is cleared once and the failure said once, its kind with it.
    QSignalSpy changes(shell.backend.get(), &Backend::problemChanged);
    shell.backend->openFiles({QStringLiteral("relative.md")});
    QCOMPARE(shell.backend->problem(), QStringLiteral("Some files could not be opened"));
    QVERIFY(!shell.backend->noticeInforms());
    QCOMPARE(changes.count(), 2);
    QTRY_COMPARE(markColour(), shell.theme->danger().rgba());
    // Dismissed, nothing of it stays behind.
    changes.clear();
    shell.backend->dismissNotice();
    QCOMPARE(changes.count(), 1);
    QVERIFY(!shell.backend->notice() && !shell.backend->noticeInforms());
}

void ParchTests::noticeBarAndDialogsFitEveryWidth()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a rather long name for a document.md");
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    shell.backend->history()->capture(document->id(), "older\n", QStringLiteral("A named version"));
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    // Two rows: the conflict and a notice with a long detail.
    shell.backend->openFiles({QStringLiteral("relative.md"), QStringLiteral("another/relative/path.md")});
    QTRY_COMPARE(noticeKinds(shell), (QStringList{"conflict", "app"}));
    QQuickItem *bar = shell.item(QStringLiteral("noticeBar"));
    QQuickItem *pane = shell.item(QStringLiteral("editorPane"));
    const auto check = [&](int width, const QString &what) {
        QStringList problems;
        QVERIFY2(everyVisibleItemFitsWithin(window, &problems),
                 qPrintable(QStringLiteral("%1 at %2: %3").arg(what).arg(width).arg(problems.join("; "))));
        const QStringList unnamed = unnamedAccessibleItems(window);
        QVERIFY2(unnamed.isEmpty(), qPrintable(QStringLiteral("%1 at %2: %3").arg(what).arg(width).arg(unnamed.join(", "))));
    };
    for (int width : {360, 640, 960, 1440}) {
        resizeWindow(window, width);
        QTest::qWait(50);
        check(width, QStringLiteral("the notice bar"));
        // Tab reaches the bar's buttons, before the text.
        QStringList missed;
        QVERIFY2(tabKeyReachesEveryControl(window, &missed), qPrintable(missed.join(", ")));
        // The bar takes room from the editor and never covers it.
        QVERIFY(pane->mapToScene(QPointF(0, 0)).y() >= bar->mapToScene(QPointF(0, bar->height())).y() - 0.5);
        QVERIFY2(pane->height() > 4 * bar->height() || width == 360, qPrintable(QString::number(pane->height())));
        for (const char *name : {"conflictDialog", "historyDialog", "closeConfirmation"}) {
            const QByteArray which(name);
            if (which == "conflictDialog") QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
            else if (which == "historyDialog") QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
            else shell.backend->requestCloseTab(shell.context()->tabs()->activeIndex());
            QObject *dialog = dialogIn(window, name);
            QTRY_VERIFY(isOpen(dialog));
            QTest::qWait(50);
            check(width, QString::fromLatin1(name));
            QTest::keyClick(window, Qt::Key_Escape);
            QTRY_VERIFY(!isOpen(dialog));
        }
    }
    // A narrow window puts the two texts one above the other.
    resizeWindow(window, 360);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *conflict = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(conflict));
    QVERIFY(conflict->property("stacked").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(conflict));
    resizeWindow(window, 1440);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QTRY_VERIFY(isOpen(conflict));
    QVERIFY(!conflict->property("stacked").toBool());
}


void ParchTests::namedVersionsSurviveAnUnfinishedRename()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    const QString id = newUuid();
    const QString folder = paths.history() + u'/' + id, index = folder + QStringLiteral("/index.json");
    // A clock a second on at each capture, so that the versions have an order.
    QDateTime clock(QDate(2026, 11, 1), QTime(9, 0), QTimeZone::UTC);
    History history(paths, HistoryLimits{1, 30, qint64(1) << 30}, [&clock] { return clock = clock.addSecs(1); });
    const QString kept = history.capture(id, "first");
    QVERIFY(!kept.isEmpty());
    // A crash between a name's file and the index leaves the index listing
    // the version unnamed; the name file still counts.
    writeFile(folder + u'/' + kept + QStringLiteral(".name"), "Chapter one");
    QCOMPARE(history.versions(id).first().name, QStringLiteral("Chapter one"));
    QVERIFY(!history.capture(id, "second").isEmpty());
    QVERIFY(!history.capture(id, "third").isEmpty());
    // Pruning to one unnamed version leaves it.
    const auto ids = [&] {
        QStringList found;
        for (const History::Version &version : history.versions(id)) found << version.id;
        return found;
    };
    QCOMPARE(ids().size(), 2);
    QVERIFY(ids().contains(kept));
    QVERIFY(QFile::exists(folder + u'/' + kept + QStringLiteral(".md")));
    // A rename whose index cannot be written keeps its name as well.
    const QString later = history.versions(id).first().id;
    const QByteArray before = readFile(index);
    QVERIFY(QFile::remove(index));
    QVERIFY(QDir().mkpath(index));
    QVERIFY(!history.rename(id, later, QStringLiteral("Chapter two")));
    QVERIFY(QDir(index).removeRecursively());
    writeFile(index, before);
    QCOMPARE(history.versions(id).first().name, QStringLiteral("Chapter two"));
    QVERIFY(!history.capture(id, "fourth").isEmpty());
    QVERIFY(!history.capture(id, "fifth").isEmpty());
    QVERIFY(ids().contains(kept));
    QVERIFY(ids().contains(later));
    QCOMPARE(ids().size(), 3);
    // Removing one takes its name with it, so it does not come back.
    QVERIFY(history.remove(id, later));
    QVERIFY(!QFile::exists(folder + u'/' + later + QStringLiteral(".name")));
    QVERIFY(!ids().contains(later));
    QVERIFY(ids().contains(kept));
}

void ParchTests::closingWhenHistoryPausedAsksToDiscard()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    TabModel *tabs = shell.context()->tabs();
    QObject *confirmation = dialogIn(window, "closeConfirmation");
    const auto said = [&] { return confirmation->property("contentItem").value<QQuickItem *>()->property("text").toString(); };
    Document *draft = failingSavedDraft(shell, QStringLiteral("kept nowhere"));
    QVERIFY(draft);
    const QString id = draft->id();
    QTRY_VERIFY(inRecovery(shell.paths(), id));
    // History fills, and nothing has said so yet.
    fillHistory(shell.paths(), id);
    QVERIFY(!shell.backend->historyPaused());
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    QCOMPARE(buttonTexts(confirmation), (QStringList{"Try again", "Save as…", "Close without saving", "Cancel"}));
    click(window, footButton(confirmation, QStringLiteral("Close without saving")));
    // History cannot keep it after all, so the window asks again, about discarding.
    QTRY_VERIFY(confirmation->property("discarding").toBool());
    QTRY_VERIFY(isOpen(confirmation));
    QCOMPARE(buttonTexts(confirmation), (QStringList{"Discard text", "Cancel"}));
    QCOMPARE(window->activeFocusItem()->property("text").toString(), QStringLiteral("Cancel"));
    QVERIFY2(said().contains(QStringLiteral("keeps the text nowhere")), qPrintable(said()));
    QVERIFY(!said().contains(QStringLiteral("in history only")));
    QCOMPARE(tabs->rowCount(), 2);
    click(window, footButton(confirmation, QStringLiteral("Cancel")));
    QTRY_VERIFY(!isOpen(confirmation));
    QCOMPARE(tabs->rowCount(), 2);
    QVERIFY(inRecovery(shell.paths(), id));
    // Asked again, the window knows history is paused and says so at once.
    QTRY_VERIFY(shell.backend->historyPaused());
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    QVERIFY(!confirmation->property("discarding").toBool());
    QCOMPARE(buttonTexts(confirmation), (QStringList{"Try again", "Save as…", "Discard text", "Cancel"}));
    QVERIFY2(said().contains(QStringLiteral("History is paused")), qPrintable(said()));
    click(window, footButton(confirmation, QStringLiteral("Discard text")));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QTRY_VERIFY(!inRecovery(shell.paths(), id));
}

void ParchTests::textTypedWhileClosingIsKeptInHistory()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    TabModel *tabs = shell.context()->tabs();
    QObject *confirmation = dialogIn(window, "closeConfirmation");
    Document *draft = failingSavedDraft(shell, QStringLiteral("first"));
    QVERIFY(draft);
    const QString id = draft->id();
    shell.backend->requestCloseTab(1);
    QTRY_VERIFY(isOpen(confirmation));
    {
        // History takes the text only once the pool is free; meanwhile more is written.
        PoolBlock block;
        click(window, footButton(confirmation, QStringLiteral("Close without saving")));
        draft->buffer()->replace({draft->buffer()->length(), 0}, QStringLiteral(" and more"));
        QCOMPARE(tabs->rowCount(), 2);
    }
    QTRY_COMPARE(tabs->rowCount(), 1);
    QVERIFY(historyHolds(shell, id, "first. and more"));
}

void ParchTests::restoreRefusalsAlwaysSayWhy()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    Document *draft = failingSavedDraft(shell, QStringLiteral("words"));
    QVERIFY(draft);
    const QString saveError = draft->lastError();
    QVERIFY(!saveError.isEmpty());
    DocumentStore *store = shell.backend->store();
    const QString missing = QStringLiteral("20200101T000000000-000000000000");
    QString why;
    QVERIFY(store->restoreVersion(draft, missing, &why) == DocumentStore::Restore::Refused);
    QCOMPARE(why, QStringLiteral("That version is no longer in history"));
    // The failed save stays what the document shows.
    QCOMPARE(draft->lastError(), saveError);
    // Every refusal is said, the same one again too.
    for (int time = 0; time < 2; ++time) {
        QVERIFY(!shell.backend->review()->restoreVersion(draft, missing));
        QCOMPARE(shell.backend->problem(), QStringLiteral("Not restored"));
        QCOMPARE(shell.backend->problemDetail(), why);
        shell.backend->dismissNotice();
        QVERIFY(shell.backend->problem().isEmpty());
    }
    QCOMPARE(draft->lastError(), saveError);
    // A restore says whether it changed the text.
    const QString older = shell.backend->history()->capture(draft->id(), "older");
    QVERIFY(store->restoreVersion(draft, older) == DocumentStore::Restore::Restored);
    QCOMPARE(draft->buffer()->text(), QStringLiteral("older"));
    QVERIFY(store->restoreVersion(draft, older) == DocumentStore::Restore::Unchanged);
}

void ParchTests::staleReadsNeverReplaceNewerOnes()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({file});
    Document *document = shell.document();
    DocumentReview *review = shell.backend->review();
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(QMetaObject::invokeMethod(window, "reviewConflict"));
    QObject *conflict = dialogIn(window, "conflictDialog");
    QTRY_VERIFY(isOpen(conflict));
    QTRY_COMPARE(conflict->property("diskText").toString(), QStringLiteral("theirs\n"));
    const int waited = conflict->property("diskRequest").toInt();
    QVERIFY(waited > 0);
    // A late answer to an older read is not shown.
    emit review->diskTextRead(waited - 1, document->id(), QStringLiteral("stale"), QString());
    QCOMPARE(conflict->property("diskText").toString(), QStringLiteral("theirs\n"));
    // The file changing again while the dialog is open is read again.
    writeFile(file, "theirs again\n");
    QTRY_COMPARE(conflict->property("diskText").toString(), QStringLiteral("theirs again\n"));
    QVERIFY(conflict->property("diskRequest").toInt() > waited);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(conflict));

    // The same in the history dialog, for the list and for the preview.
    History *history = shell.backend->history();
    const QString first = history->capture(document->id(), "first");
    QVERIFY(!first.isEmpty());
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    QTRY_COMPARE(dialog->property("rows").toList().size(), 2);
    dialog->setProperty("currentIndex", 1);
    QObject *preview = shell.object(QStringLiteral("versionPreview"));
    QTRY_COMPARE(preview->property("text").toString(), QStringLiteral("first"));
    emit review->versionRead(dialog->property("previewRequest").toInt() - 1, document->id(), first,
                             QStringLiteral("stale"), QString());
    emit review->versionsRead(dialog->property("versionsRequest").toInt() - 1, document->id(), QVariantList());
    QCOMPARE(preview->property("text").toString(), QStringLiteral("first"));
    QCOMPARE(dialog->property("rows").toList().size(), 2);
}

void ParchTests::saveAsOverItsOwnConflictedFileOverwrites()
{
    QTemporaryDir files, root;
    QVERIFY(files.isValid() && root.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    Backend backend(AppPaths::under(root.path()));
    backend.fileRequests()->setFallbackFolders({root.path()});
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.start({});
    backend.openFiles({file});
    Document *document = backend.document();
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    writeFile(file, "theirs\n");
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    // The dialog asked to replace the file, so its own path is Overwrite.
    backend.saveAs();
    QCOMPARE(picker.lastSuggestion.toLocalFile(), file);
    emit picker.saveSelected(QUrl::fromLocalFile(file));
    QTRY_COMPARE(readFile(file), QByteArray("mine\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    // For a file that is gone, it saves the text there again.
    document->buffer()->replace({0, 0}, QStringLiteral("more "));
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(document->fileMissing());
    backend.saveAs();
    emit picker.saveSelected(QUrl::fromLocalFile(file));
    QTRY_COMPARE(readFile(file), QByteArray("more mine\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), file);
}

void ParchTests::saveAsFromAConflictWaitsForItsOutcome()
{
    // Root writes into a read-only folder, so there is nothing to see as root.
    if (::geteuid() == 0) return;
    QTemporaryDir files, other, root;
    QVERIFY(files.isValid() && other.isValid() && root.isValid());
    const QString file = canonical(files.path()) + QStringLiteral("/a.md");
    writeFile(file, "base\n");
    const QString folder = canonical(files.path()) + QStringLiteral("/locked");
    QVERIFY(QDir().mkpath(folder));
    const auto writable = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    const auto unlock = qScopeGuard([&] { QFile::setPermissions(folder, writable); });
    QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
    Backend backend(AppPaths::under(root.path()));
    backend.fileRequests()->setFallbackFolders({root.path()});
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.start({});
    backend.openFiles({file});
    Document *document = backend.document();
    document->buffer()->replace({0, 4}, QStringLiteral("mine"));
    QVERIFY(QFile::remove(file));
    QTRY_COMPARE_WITH_TIMEOUT(document->state(), Document::State::Conflict, 3000);
    QVERIFY(document->fileMissing());
    const QString lastFolder = canonical(other.path());
    backend.settings()->setLastFolder(lastFolder);
    QSignalSpy named(backend.fileRequests(), &FileRequests::named);

    // A name whose folder cannot be written in: nothing is said to be saved
    // while the save runs, nor after it fails.
    backend.saveAs();
    const QString target = folder + QStringLiteral("/b.md");
    emit picker.saveSelected(QUrl::fromLocalFile(target));
    QCOMPARE(named.count(), 0);
    QCOMPARE(backend.settings()->lastFolder(), lastFolder);
    QTRY_COMPARE(document->state(), Document::State::SaveFailed);
    QCOMPARE(named.count(), 0);
    QCOMPARE(backend.settings()->lastFolder(), lastFolder);
    QCOMPARE(document->path(), file);
    QVERIFY(!QFile::exists(file));

    // Trying again names it where it was asked to go.
    QVERIFY(QFile::setPermissions(folder, writable));
    backend.retrySave();
    QTRY_COMPARE(readFile(target), QByteArray("mine\n"));
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(document->path(), target);
    QVERIFY(!QFile::exists(file));
    QCOMPARE(named.count(), 1);
    QCOMPARE(backend.settings()->lastFolder(), folder);
}

void ParchTests::dialogsStartInTheFolderLastUsed()
{
    QTemporaryDir root, documents;
    auto last = std::make_unique<QTemporaryDir>();
    QVERIFY(root.isValid() && documents.isValid() && last->isValid());
    Backend backend(AppPaths::under(root.path()));
    FileRequests *files = backend.fileRequests();
    files->setFallbackFolders({documents.path() + QStringLiteral("/gone"), documents.path(), root.path()});
    FakePicker picker;
    backend.setFilePicker(&picker);
    backend.start({});
    // Nothing used yet: the first fallback that is a folder.
    QCOMPARE(files->startFolder(), documents.path());
    backend.open();
    QCOMPARE(picker.opens, 1);
    QCOMPARE(picker.startFolder(), documents.path());
    // Then the folder last used, for opening files and folders and for saving.
    const QString lastPath = last->path();
    backend.settings()->setLastFolder(lastPath);
    backend.openFolder();
    QCOMPARE(picker.startFolder(), lastPath);
    backend.open();
    QCOMPARE(picker.startFolder(), lastPath);
    backend.document()->buffer()->replace({0, 0}, QStringLiteral("# Plan"));
    backend.save();
    QCOMPARE(picker.lastSuggestion.toLocalFile(), lastPath + QStringLiteral("/Plan.md"));
    // One that has gone falls back, and with nothing there the last is taken as it is.
    last.reset();
    QCOMPARE(files->startFolder(), documents.path());
    files->setFallbackFolders({root.path() + QStringLiteral("/none"), root.path() + QStringLiteral("/also-none")});
    QCOMPARE(files->startFolder(), root.path() + QStringLiteral("/also-none"));
}

void ParchTests::pausedHistorySaysWhy()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    Document *document = shell.document();
    History *history = shell.backend->history();
    const auto said = [&] {
        QQuickItem *row = shell.item(QStringLiteral("historyNotice"));
        return row ? findItem(row, QStringLiteral("noticeText"))->property("text").toString() : QString();
    };
    // Named versions that fill history's space, written behind history's
    // back: it learns of them as it next reads the document's versions.
    const QString named = fillHistory(shell.paths(), document->id());
    QCOMPARE(history->versions(document->id()).size(), 1);
    history->checkPaused();
    QTRY_VERIFY(shell.backend->historyPaused());
    QVERIFY(shell.backend->historyFull());
    QVERIFY(history->pause() == History::Pause::NamedVersionsFill);
    QTRY_VERIFY2(said().contains(QStringLiteral("named versions fill")), qPrintable(said()));
    QVERIFY(history->remove(document->id(), named));
    QTRY_VERIFY(!shell.backend->historyPaused());
    QTRY_VERIFY(!noticeKinds(shell).contains(QStringLiteral("history")));
    // Named versions that nearly fill it, and a version that does not fit beside them.
    fillHistory(shell.paths(), document->id(), (qint64(1) << 30) - 1);
    QVERIFY(history->capture(document->id(), "xy").isEmpty());
    QVERIFY(history->pause() == History::Pause::VersionDidNotFit);
    QTRY_VERIFY(shell.backend->historyPaused());
    QVERIFY(!shell.backend->historyFull());
    QTRY_VERIFY2(said().contains(QStringLiteral("did not fit")), qPrintable(said()));
    QVERIFY(!said().contains(QStringLiteral("named versions fill")));
}

void ParchTests::messagesNameParchsFoldersPlainly()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const AppPaths paths = AppPaths::under(root.path());
    Backend backend(paths);
    const QString id = newUuid();
    QCOMPARE(backend.shownText(QStringLiteral("Cannot write ") + DocumentStore::draftFile(paths, id)),
             QStringLiteral("Cannot write the draft"));
    QCOMPARE(backend.shownText(QStringLiteral("In ") + paths.drafts() + u'/' + id + QStringLiteral(": full")),
             QStringLiteral("In the drafts folder: full"));
    QCOMPARE(backend.shownText(paths.recovery() + QStringLiteral(" is full")), QStringLiteral("the recovery folder is full"));
    const QString home = QDir::homePath();
    QCOMPARE(backend.shownText(home + QStringLiteral("/notes/a.md: Permission denied")),
             QStringLiteral("~/notes/a.md: Permission denied"));
    QCOMPARE(backend.shownText(home + QStringLiteral("other/a.md")), home + QStringLiteral("other/a.md"));
    QCOMPARE(backend.shownText(QStringLiteral("/elsewhere/a.md")), QStringLiteral("/elsewhere/a.md"));
    // A draft that cannot be saved, and recovery that cannot be written,
    // say so by what the folder is for, with the system's reason.
    blockDrafts(paths);
    writeFile(paths.recovery(), "in the way");
    Document *draft = backend.store()->newDraft();
    draft->buffer()->replace({0, 0}, QStringLiteral("words"));
    backend.store()->save(draft);
    QTRY_COMPARE(draft->state(), Document::State::SaveFailed);
    QVERIFY2(draft->lastError().startsWith(QStringLiteral("The drafts folder can't be written (")), qPrintable(draft->lastError()));
    QVERIFY(!draft->lastError().contains(paths.drafts()));
    QString error;
    QVERIFY(!Recovery(paths).write(id, {}, SourceSnapshot{1, QStringLiteral("x")}, {}, &error));
    QVERIFY2(error.startsWith(QStringLiteral("The recovery folder can't be written (")), qPrintable(error));
    QVERIFY(!error.contains(paths.recovery()));
}

void ParchTests::versionTimesReadNaturally()
{
    const QLocale before;
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    const auto restore = qScopeGuard([&] { QLocale::setDefault(before); });
    const QDateTime now(QDate(2026, 11, 15), QTime(16, 20));
    QCOMPARE(DocumentReview::when(QDateTime(QDate(2026, 11, 15), QTime(4, 50)), now), QStringLiteral("Today, 4:50 AM"));
    QCOMPARE(DocumentReview::when(QDateTime(QDate(2026, 11, 14), QTime(21, 5)), now), QStringLiteral("Yesterday, 9:05 PM"));
    QCOMPARE(DocumentReview::when(QDateTime(QDate(2026, 11, 3), QTime(4, 50)), now), QStringLiteral("Nov 3, 4:50 AM"));
    QCOMPARE(DocumentReview::when(QDateTime(QDate(2025, 12, 31), QTime(4, 50)), now),
             QStringLiteral("Dec 31, 2025, 4:50 AM"));
    // A time kept in UTC reads in local time.
    const QDateTime utc = QDateTime(QDate(2026, 11, 15), QTime(12, 0)).toUTC();
    QCOMPARE(DocumentReview::when(utc, now), QStringLiteral("Today, 12:00 PM"));
    // Versions that share their minute with a neighbour in the list, newest
    // first, show their seconds; the others do not.
    const QList<QDateTime> taken{QDateTime(QDate(2026, 11, 15), QTime(12, 58, 5)),
                                 QDateTime(QDate(2026, 11, 15), QTime(12, 57, 40)),
                                 QDateTime(QDate(2026, 11, 15), QTime(12, 57, 10)),
                                 QDateTime(QDate(2026, 11, 14), QTime(12, 57, 10))};
    QCOMPARE(DocumentReview::whenAll(taken, now),
             (QStringList{"Today, 12:58 PM", "Today, 12:57:40 PM", "Today, 12:57:10 PM", "Yesterday, 12:57 PM"}));
}

void ParchTests::historyActionsActOnTheirDocument()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString a = canonical(files.path()) + QStringLiteral("/a.md"), b = canonical(files.path()) + QStringLiteral("/b.md");
    writeFile(a, "alpha");
    writeFile(b, "beta");
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    shell.backend->openFiles({a});
    Document *first = shell.document();
    shell.backend->openFiles({b});
    Document *second = shell.document();
    DocumentReview *review = shell.backend->review();
    History *history = shell.backend->history();
    // With the second document in front, each action still acts on the first.
    review->checkpoint(first, QStringLiteral("For a"));
    QTRY_COMPARE(history->versions(first->id()).size(), 1);
    QVERIFY(history->versions(second->id()).isEmpty());
    const QString version = history->versions(first->id()).first().id;
    QCOMPARE(history->read(first->id(), version), QByteArray("alpha"));
    review->nameVersion(first, version, QStringLiteral("Renamed"));
    QTRY_COMPARE(history->versions(first->id()).first().name, QStringLiteral("Renamed"));
    first->buffer()->replace({0, 5}, QStringLiteral("later"));
    QVERIFY(review->restoreVersion(first, version));
    QCOMPARE(first->buffer()->text(), QStringLiteral("alpha"));
    QCOMPARE(second->buffer()->text(), QStringLiteral("beta"));
    review->removeVersion(first, version);
    const auto gone = [&] {
        for (const History::Version &kept : history->versions(first->id()))
            if (kept.id == version) return false;
        return true;
    };
    QTRY_VERIFY(gone());
    QVERIFY(history->versions(second->id()).isEmpty());

    // A prompt goes with its dialog, which goes when another document comes to the front.
    QVERIFY(QMetaObject::invokeMethod(window, "showHistory"));
    QObject *dialog = dialogIn(window, "historyDialog");
    QTRY_VERIFY(isOpen(dialog));
    click(window, footButton(dialog, QStringLiteral("Name this version")));
    QObject *prompt = dialogIn(window, "versionNamePrompt");
    QTRY_VERIFY(isOpen(prompt));
    shell.context()->tabs()->setActiveIndex(shell.context()->tabs()->indexOf(first));
    QTRY_VERIFY(!isOpen(dialog));
    QTRY_VERIFY(!isOpen(prompt));
}

void ParchTests::benchmarkReportsEveryMeasure()
{
    QTemporaryDir documents;
    const QString original = documents.filePath(QStringLiteral("measured.md"));
    QString text;
    for (int i = 0; i < 300; ++i) text += QStringLiteral("The quiet writer keeps each ordinary word and reads again.\n\n");
    writeFile(original, text.toUtf8());

    // It measures a copy under the same name, and refuses a file that is not there.
    QString error;
    const QString copy = Benchmark::copyIn(original, documents.filePath(QStringLiteral("run")), &error);
    QCOMPARE(copy, documents.filePath(QStringLiteral("run/measured.md")));
    QCOMPARE(readFile(copy), text.toUtf8());
    QVERIFY(Benchmark::copyIn(documents.filePath(QStringLiteral("none.md")), documents.filePath(QStringLiteral("run2")), &error)
                .isEmpty());
    QVERIFY(error.contains(QStringLiteral("none.md")));

    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    const QString output = documents.filePath(QStringLiteral("result.json"));
    Benchmark benchmark(output, QStringLiteral("measured"),
                        Benchmark::Timing{.settleMs = 200, .placeMs = 50, .keyMs = 10, .scrollMs = 300, .jumpMs = 20,
                                          .rewrapMs = 300, .quietMs = 100, .idleMs = 300, .waitMs = 5000});
    QSignalSpy finished(&benchmark, &Benchmark::finished);
    benchmark.setLaunchTime(Benchmark::now());
    benchmark.opening();
    shell.backend->openFiles({copy});
    activate(shell.window.get());
    benchmark.start(shell.window.get());
    QVERIFY(finished.wait(60000));
    QCOMPARE(finished.first().at(1).toString(), QString());
    QCOMPARE(finished.first().at(0).toInt(), 0);

    const QJsonObject result = QJsonDocument::fromJson(readFile(output)).object();
    // The whole-document measurement's fields, and the application's own.
    for (const char *key : {"label", "utf16Units", "qt", "platform", "refreshHz", "devicePixelRatio", "pssEmptyMiB",
                            "loadSetTextMs", "loadToFirstFrameMs", "pssLoadedMiB", "windowWidth", "windowHeight",
                            "editorHeightPx", "lineCount", "typing", "lengthAfterTyping", "expectedLength",
                            "scrollFrameMs", "scrolledToPx", "jumpToFrameMs", "rewrapFrameMs", "pssEndMiB",
                            "launchToEditableMs", "mainToEditableMs", "idleFrames", "idleCaretChanges", "cursorFlashTimeMs", "idleCpuTicks", "control",
                            "rewrapSettle", "peakRssMiB", "cpuTimeMs", "helperProcesses", "ownedProcessesEnd",
                            "pssEndWithOwnedMiB", "graphicsApi"})
        QVERIFY2(result.contains(QLatin1String(key)), key);
    QCOMPARE(result["label"].toString(), QStringLiteral("measured"));
    QCOMPARE(result["utf16Units"].toInt(), int(text.size()));
    QCOMPARE(result["expectedLength"].toInt(), int(text.size()) + 60);
    QCOMPARE(result["lengthAfterTyping"].toInt(), result["expectedLength"].toInt());
    for (const char *place : {"start", "middle", "end"}) {
        const QJsonObject typing = result["typing"].toObject()[QLatin1String(place)].toObject();
        QCOMPARE(typing["dispatchMs"].toObject()["n"].toInt(), 20);
        QVERIFY(typing["keyToFrameMs"].toObject()["n"].toInt() > 0);
    }
    QCOMPARE(result["jumpToFrameMs"].toObject()["n"].toInt(), 10);
    const QJsonObject control = result["control"].toObject();
    QVERIFY(control["scrollFrameMs"].toObject()["n"].toInt() > 0);
    QVERIFY(control["rewrapFrameMs"].toObject()["n"].toInt() > 0);
    QVERIFY(result["scrollFrameMs"].toObject()["n"].toInt() > 0);
    // A short document re-wraps as the column moves; nothing waits.
    const QJsonObject settle = result["rewrapSettle"].toObject();
    QCOMPARE(settle["deferred"].toBool(), false);
    QVERIFY(settle["widthSteps"].toInt() > 0);
    QVERIFY(settle["layoutsWhileMoving"].toInt() > 0);
    QVERIFY(result["peakRssMiB"].toDouble() > 0);
    // The window is left as it was found, with the text back in its place.
    QVERIFY(shell.item(QStringLiteral("editorPane"))->isVisible());
    QVERIFY(!findItem(shell.window->contentItem(), QStringLiteral("controlFlickable")));
    // What main() does next: quit as parch always does, its text safe.
    QSignalSpy quitting(shell.backend.get(), &Backend::quitRequested);
    shell.backend->quit();
    QVERIFY2(quitting.count() == 1, qPrintable(shell.backend->problem() + u' ' + shell.backend->problemDetail()));
}

void ParchTests::benchmarkThatCannotStartEndsTheLoop()
{
    // main() starts the benchmark before its event loop runs and leaves the
    // loop with the code the benchmark finishes with. One that fails at
    // once must still end that loop, with an error, rather than leave the
    // process waiting for ever.
    QTemporaryDir folder;
    QQuickWindow empty;
    empty.resize(200, 100);
    empty.show();
    Benchmark benchmark(folder.filePath(QStringLiteral("result.json")), QStringLiteral("none"),
                        Benchmark::Timing{.waitMs = 1000});
    // An event loop that is told to end before it runs forgets it, as
    // QCoreApplication::exec() does.
    QEventLoop loop;
    QString reason;
    QObject::connect(&benchmark, &Benchmark::finished, &loop, [&](int code, const QString &error) {
        reason = error;
        loop.exit(code);
    });
    benchmark.start(&empty);
    QTimer::singleShot(5000, &loop, [&loop] { loop.exit(99); });
    QCOMPARE(loop.exec(), 1);
    QVERIFY(reason.contains(QStringLiteral("no editor")));
    QVERIFY(!QFile::exists(folder.filePath(QStringLiteral("result.json"))));
}

QTEST_MAIN(ParchTests)
#include "parch_tests.moc"

