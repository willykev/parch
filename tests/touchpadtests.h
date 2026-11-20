#pragma once

// The tests of scrolling with a touchpad: a view follows the fingers from
// their first move, and glides on after they lift, at the speed the swipe
// had as they lifted, slowing down to rest; not at all after a mouse
// wheel's click or fingers held still. A glide stops at the end of the
// view, when the fingers scroll again, at a click or a key, and when
// something else moves the view; a short swipe along a glide adds to its
// speed. The window's editor and preview both glide.
// ParchTests runs each of them as a test function of its own.
namespace TouchpadTests {

void viewsFollowTheFingersFromTheirFirstMove();
void aSwipeGlidesOnAfterTheFingersLift();
void sidewaysSwipesGlideSideways();
void fingersHeldStillLeaveNoGlide();
void aMouseWheelDoesNotGlide();
void aGlideStopsAtTheEndOfTheView();
void scrollingClicksAndKeysStopAGlide();
void aViewMovedElsewhereStopsItsGlide();
void aShortSwipeAlongAGlideAddsToIt();
void theEditorAndThePreviewGlide();

} // namespace TouchpadTests
