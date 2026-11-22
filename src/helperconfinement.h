#pragma once

#include <QStringList>

struct HelperRun;

namespace HelperConfinement {

// The kernel ruleset ABI through its scope field.
struct Restrictions {
    quint64 handled_access_fs;
    quint64 handled_access_net;
    quint64 scoped;
};
Restrictions restrictionsFor(int version);

// Whether a child can enter a Landlock ruleset on this kernel.
bool available();
// Protections the running kernel cannot enforce for a drawing.
QStringList missingProtections();
// Make a ruleset for one drawing. The caller owns the descriptor.
int rulesFor(const HelperRun &run, const QString &home);
// Called between fork and exec, after PR_SET_NO_NEW_PRIVS.
bool enter(int rules);

} // namespace HelperConfinement
