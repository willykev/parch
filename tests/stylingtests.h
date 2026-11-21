#pragma once

// The tests of live formatting: the styling of Markdown source in the
// editor, from the engine's tree, through the decoration layer. ParchTests
// runs each of them as a test function of its own.
namespace StylingTests {

void decorationRangesCanGrowAtTheirEdges();
void constructsGetTheirStylesOnTheirMarkersAndContent();
void stylesFollowCrlfAndUnicodeAndStayFew();
void editorStylesMarkdownAsItIsWritten();
void stylingFollowsEditsWithoutStaleResults();
void themeChangeRecoloursTheStyling();
void stylingKeepsEveryGlyphWhereItWas();
void liveFormattingTurnsOff();
void liveFormattingIsAPreferenceAndACommand();
void resettingStylingStatsLeavesAnIdleViewSettled();
void typingWithLiveFormattingStaysWithinAFrame();

} // namespace StylingTests
