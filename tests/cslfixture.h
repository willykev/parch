#pragma once

#include <QString>

// Runs one fixture of the CSL test suite (citation-style-language/
// test-suite, processor-tests/humans): its style, items and citations go
// through CslEngine, and its expected HTML is compared with the output,
// both read into formatted runs so that equal formatting written with
// different tags compares equal.
namespace CslFixture {

struct Outcome {
    bool passed = false;
    // Set when the fixture could not be run at all.
    QString error;
    QString expected;
    QString actual;
};

Outcome run(const QString &path);

} // namespace CslFixture
