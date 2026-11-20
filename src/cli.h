#pragma once

#include <QStringList>

// Commands that need no display, for agents and scripts. Returns the exit
// code, or -1 when the arguments ask for the window instead; then path
// holds what to open, if anything.
int runCli(const QStringList &arguments, QStringList &open);

// Whether the arguments ask for a command rather than the window. main asks
// before there is an application, which aborts when it finds no display, and
// runs these on the offscreen platform.
bool needsNoWindow(int argc, char *argv[]);
