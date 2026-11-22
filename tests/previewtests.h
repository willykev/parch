#pragma once

// The tests of the preview: the three views and what each view keeps, the
// document the preview builds from the Markdown engine's tree, how it
// follows edits and the theme, and how it and the editor find each other.
// ParchTests runs each of them as a test function of its own.
namespace PreviewTests {
void aWebSvgIsDrawnWithoutItsOutsidePictures();
void helperSvgWithAnOutsideReferenceIsNotPaintedByQt();


void viewModesAreKeptByEachView();
void previewBuildsTextAndInlineFormatting();
void previewBuildsListsQuotesAndRules();
void previewBuildsCodeAndTablesAsIslands();
void previewBuildsNotesCitationsAndContents();
void previewBuildsPicturesAndTheirStates();
void previewShowsRawHtmlUnderThePolicy();
void previewPartsKeepTheirKeysAndSources();
void previewBuildsInPlaceForPaper();
void previewStyleIsReadableInAnyTheme();
void codeIsColouredByLanguage();
void viewsShowTheTextThePreviewOrBoth();
void previewFollowsEditsWithoutRebuilding();
void staleResultsNeverReplaceNewerText();
void typingQuickerThanReadsIsFollowed();
void previewSaysWhenItIsBehind();
void previewAndSourceFindEachOther();
void includedTextLeadsToItsFile();
void textAndPreviewScrollTogether();
void linksGoWhereTheyLead();
void linksOfOtherKindsAskFirst();
void tasksAreTickedInTheSource();
void notesGoThereAndBack();
void themeChangeRecoloursWithoutRebuilding();
void narrowWindowsShowOnePaneOfASplit();
void dividerMovesAndIsRemembered();
void aBookIsFollowedWithinAFrame();
void wideBlocksScrollByThemselves();
void previewIsReachedAndReadWithoutAMouse();
void copyingFromThePreviewGivesTextAndHtml();
void picturesArriveAndFailInTheirPlace();
void theRealHelpersDrawInThePreview();
void outlineAndPreferencesReachThePreview();
void vimKeysAreTheTextsAlone();
void focusModeDimsTheTextAlone();
void distractionFreeWritingKeepsEachView();
void citationsReadAsTheirStyleWordsThem();
void documentSettingsReachThePreview();
void previewFollowsFilesOnDisk();
void webPicturesWaitUntilAsked();
void pathologicalBlocksStayCheapToLayOut();
void drawnPicturesAreDecodedOffTheUiThreadAndKeptToScreenSize();
void headingLinksAreFoundInTheSharedParse();

void aLinkHintShowsTheHostItOpens();
void aWebLinkWithUserInfoAsksFirst();
void theLinkDialogNeverElidesTheHost();
void aMailLinkWithAnAttachmentAsksFirst();
void showingWebPicturesApprovesTheirHosts();
void aPictureFromANewHostAsksAgain();
void anOlderShowThemSettingStillWorks();

} // namespace PreviewTests
