#pragma once

// The tests of the interactive Markdown guide: the command that opens it
// beside its preview, every part of it read by the real engine and built
// by the real preview, and the guide as a document of parch's own, which
// keeps nothing on disk until Save as writes a copy. ParchTests runs each
// of them as a test function of its own.
namespace GuideTests {

void markdownGuideOpensBesideItsPreview();
void markdownGuideRendersEveryPart();
void markdownGuideKeepsNothingOnDisk();
void markdownGuideClosesWithoutAsking();
void markdownGuideIsSavedAsACopy();

} // namespace GuideTests
