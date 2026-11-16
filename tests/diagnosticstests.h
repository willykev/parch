#pragma once

// The tests of what parch says is wrong in a document and what it says it
// supports: include limits as settings and options, formulas and diagrams
// checked through their helpers, the Check document dialog and the
// preview's marks for what cannot be resolved, and the conformance figures
// the user guide states. ParchTests runs each of them as a test function of
// its own.
namespace DiagnosticsTests {

void includeLimitsAreConfigurable();
void checkDrawsFormulasAndDiagrams();
void checkDocumentListsEveryProblem();
void previewMarksWhatLeadsNowhere();
void katexCorpusDrawsAsRecorded();
void guideListsWhatMathCannotDraw();
void guideNamesEveryDiagramFamily();
void guideConformanceFiguresHold();
void guideStatesWhatCitationsCannotDo();

} // namespace DiagnosticsTests
