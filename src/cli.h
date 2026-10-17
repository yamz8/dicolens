#pragma once

#include <QStringList>

// Commands that need no display, for agents and scripts. Returns the exit
// code, or -1 when the arguments ask for the window instead; then path
// holds what to open, if anything.
int runCli(const QStringList &arguments, QStringList &open);
