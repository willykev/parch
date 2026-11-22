#pragma once

#include <QString>

struct HelperRun;

namespace HelperConfinement {

// Whether a child can enter a Landlock ruleset on this kernel.
bool available();
// Make a ruleset for one drawing. The caller owns the descriptor.
int rulesFor(const HelperRun &run, const QString &home);
// Called between fork and exec, after PR_SET_NO_NEW_PRIVS.
bool enter(int rules);

} // namespace HelperConfinement
