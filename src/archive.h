#pragma once

#include <QString>

// Scans that come as a zip, as hospital portals hand them out, are unpacked
// into a folder of this run's own and read from there.
namespace Archive {

bool isZip(const QString &path);

// The folder the archive was unpacked into, or empty, with a reason in
// English. bsdtar does the unpacking, which keeps every file inside it.
QString unpack(const QString &path, QString *error = nullptr);

// Removes this run's unpacked files, and any left by runs that crashed.
void cleanUp(bool includingThisRun);

}
