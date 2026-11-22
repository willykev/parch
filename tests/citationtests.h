#pragma once

// Tests of citations and bibliographies: the readers of bibliography
// databases, the CSL style reader and processor, the bundled styles and the
// citation formatter. The test class's slots call these.
namespace CitationTests {
void sharedYamlReferencesAreReadOnce();
void yamlFieldsCanArriveInAnyOrder();
void longAuthorListsDisambiguatePromptly();
void citationMarkupKeepsUnclosedTagsAsText();
void citationLocatorsWithWhitespaceFormatPromptly();
void aRepeatedBibliographyIsSearchedOnce();

void deeplyNestedCitationQuotesAreText();
void aTitleOfManyTagsFormatsQuickly();
void aStyleThatExpandsTooFarIsRefused();
void crossrefInheritanceCountsAgainstTheBudget();
void manyFieldsAndChildrenParseQuickly();


void richTextOutput();
void bibtexSyntax();
void bibtexLatex();
void bibtexNames();
void bibtexDatesTypesAndFields();
void diagnosticsKeepPercentNumbers();
void citationYearsSortAndLabelAtTheirLimits();
void bibliographyDiagnostics();
void cslJsonAndYaml();
void styleErrors();
void bundledStylesLoad();
void localeTermsAndFallbacks();
void apaStyle();
void chicagoAuthorDateStyle();
void chicagoNotesStyle();
void ieeeStyle();
void mlaStyle();
void harvardStyle();
void vancouverStyle();
void locatorParsing();
void notePositions();
void keysAndScopes();
void writerDefinitions();
void formattingOffTheUiThread();
void cslTestSuite();

} // namespace CitationTests
