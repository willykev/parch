#pragma once

// The tests of what a frame holds: whatever the document's size, the editor
// and the preview give the scene graph only what is in view or near it, and
// keys typed beside the preview reach the screen without waiting for it, and
// a new width of the column moves each view at most once.
// ParchTests runs each of them as a test function of its own.
namespace FrameTests {

void adversarialEditsDrawOnlyNearTheView();
void textIsDrawnWhereverTheViewGoes();
void keysBesideThePreviewDoNotWaitForIt();
void newWidthsMoveEachViewOnce();

} // namespace FrameTests
