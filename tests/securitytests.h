#pragma once

// The tests of what parch refuses to reach or write: pictures fetched only
// from public addresses, exports that never write through a link, the
// permissions of parch's own files, the limits its helpers run with, and
// an assets folder that is a link. ParchTests runs each of them as a test
// function of its own.
namespace SecurityTests {

void webPictureRequestsKeepTheirHost();
void webPicturesComeOnlyFromPublicAddresses();
void exportsAskBeforeReplacingALink();
void parchKeepsItsFilesToItself();
void helpersRunWithinLimits();
void aLinkedAssetsFolderIsNotFollowed();
void bigPastesConvertOffTheWindowThread();
void aWordTooLongToBreakKeepsTypingQuick();
void aLongPdfStaysCancellableWhileItIsSet();

} // namespace SecurityTests
