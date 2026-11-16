#pragma once

// The tests of `parch --screenshot` and the options that set the window up
// for a picture (src/cli.*, src/screenshot.*, main()). ParchTests runs each
// of them as a test function of its own, prefixed `screenshot`.
namespace ScreenshotTests {

void optionsAreParsed();
void malformedOptionsAreRefused();
void helpNamesEveryOption();
void windowIsSetUpAsAsked();
void pictureWaitsForEverythingDrawn();
void exportDialogShowsItsPages();
void drawsOffScreenAtTheSizeAsked();
void writesNothingOfThePersons();
void namesWhatCannotBeDone();

} // namespace ScreenshotTests
