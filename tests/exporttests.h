#pragma once

// The tests of exports and printing: each format's structure and content
// for a document holding every construct, what each format says it leaves
// out, cancellation, failures, atomic output and the source left alone, the
// window's Export dialog and its background jobs, and printing through a
// stand-in for the desktop's print portal. ParchTests runs each of them as a
// test function of its own.
namespace ExportTests {
void paperPaginatesWrappedHeadingsTogether();

void paperIndexesUnresolvedReferencesOnce();

void consecutiveHeadingsKeepTheirPagePositions();
void paperSetsManyShortParagraphsPromptly();
void paperWrapsLongTextRunsPromptly_data();
void paperWrapsLongTextRunsPromptly();
void docxReusesTheBytesOfOnePicture();
void aDocumentWithManyFormulasStopsAtTheHelperBudget();

void aHugeHtmlTableIsShownAsText();
void aDeeplyQuotedHtmlPasteFallsBackToText();
void aTagWithManyAttributesPastesQuickly();

void onlyReferenceFilesTravelWithMarkdown();
void aBibliographyThatIsNotOneStaysBehind();
void copiedCitationFilesAreListed();
void aReferenceListMustBeAnArrayOfObjects();
void checkReportsABibliographyThatCannotBeRead();
void anEmptyBibliographyIsAWarning_data();
void anEmptyBibliographyIsAWarning();
void epubMetadataWithControlCharactersIsWellFormed();
void aRepeatedHtmlAttributeIsKeptOnce();
void docxBodyTextWithPercentNumbersIsKept();
void anExportOverSomeoneElsesFileTakesTheUsualMode();
void anExportNeverCarriesSetIdBits();
void anOrderedListStartingAtTheLargestNumber();
void docxBookmarksAndRelationshipsStayUnique_data();
void docxBookmarksAndRelationshipsStayUnique();
void svgPicturesUseTheirDrawingInEveryFormat();
void aPdfIsNotAPicture();
void picturesAreReadByTheirOwnFormat_data();
void picturesAreReadByTheirOwnFormat();
void exportedSvgPicturesHoldNoScript_data();
void svgExportKeepsViewportAndNamedView();
void exportedSvgPicturesHoldNoScript();
void embeddedPicturesKeepTheirViewFragments();
void inlineFormulaSvgKeepsOnlyDrawingElements();
void hugeSvgSizesAreDrawnWithinBounds();
void onePictureNamedManyWaysIsDecodedOnce();
void picturesPastTheBudgetAreLeftOutWithANote();

void exportLinksKeepQueryAndFragment();

void pdfSetsPagesWithSearchableText();
void pdfLocalLinksLeadFromTheOutputFolder();
void epubReportsLinksToFilesOutsideTheBook();
void pdfKeepsHeadingsWithTheirText();
void pdfTallTableRowsRunOntoTheNextPage();
void pdfSetsALongDocumentWhole();
void pdfOutlineIsAnIncrementalUpdate();
void paperFormulasSitOnTheBaseline();
void paperDiagramsAreVectorsWhereQtDrawsThem();
void docxIsWordprocessingMarkup();
void bibliographyPrecedesCollectedNotes_data();
void bibliographyPrecedesCollectedNotes();
void docxDeclaresFixedWidthCodeFonts();
void frontMatterTitleIsMetadataAcrossFormats_data();
void frontMatterTitleIsMetadataAcrossFormats();
void docxOpensInLibreOffice();
void epubFollowsTheContainerRules();
void epubSplitsIntoChapters();
void htmlCarriesItsPictures();
void exportFoldersKeepDocuments();
void exportsReadPicturesWithinTheirFolders();
void exportCreationIsExclusive();
void markdownExportsAssembledOrAsIs();
void exportsListLimitationsBeforeWriting();
void exportsStopWhenCancelled();
void exportsFailWithAReasonAndKeepTheOldFile();
void exportsNeverTouchTheSource();
void exportsLinkIncludedFilesWithinThemselves();
void htmlPicturesKeepTheirViewsAndOddFolderNames();
void aReplacedExportKeepsWhoMayReadIt();
void exportDialogListsLimitationsAndExportsInTheBackground();
void exportsAskBeforeReplacingWhatTheSuffixNames();
void codeIsColouredWithoutAPreviewShownFirst();
void exportAgainRepeatsTheRecipe();
void printingGoesThroughThePortal();
void printPreviewShowsThePagesThePdfHas();
void exportDialogShowsItsOptionsBesideThePages();
void cliExportsEveryFormat();

} // namespace ExportTests
