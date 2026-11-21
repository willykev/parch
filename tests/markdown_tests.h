#pragma once

// The tests of the Markdown engine (src/md*.{h,cpp}, src/frontmatter.*).
// They are plain functions so that they can live in a file of their own;
// each is run by a slot of the same name, prefixed `markdown`, in
// parch_tests.cpp.
namespace MarkdownTests {
void aWideTableWithShortRowsStaysSmall();
void aTablePastItsCellLimitBecomesText();
void manyTableOfContentsShareOneList();
void manyAnchorsBeforeAValueDoNotRecurse();
void anUnclosedFlowListInFrontMatterIsAnError();
void aLongUnclosedFrontMatterScalarParsesQuickly();


void commonMarkSpec();
void gfmExtensions();
void profilesAndOptions();
void inlineFormatting();
void superscriptAndSubscript();
void headingsAndBreaks();
void lists();
void codeBlocks();
void linksAndImages();
void headingLabelsAndSlugs();
void tableOfContents();
void tables();
void definitionLists();
void footnotes();
void citations();
void citationPrefixes();
void comments();
void rawHtmlPolicy();
void math();
void mermaid();
void frontMatter();
void variables();
void includes();
void includeGuards();
void includeScopes();
void includesFromDisk();
void sourceRanges();
void markerRanges();
void randomDocuments();
void outlineAndPlainText();
void positionsAndProse();
void htmlRenderer();
void diagnostics();
void parsesOnAnyThread();
void performance();
void pathologicalInput();

} // namespace MarkdownTests
