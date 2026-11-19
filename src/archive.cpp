#include "archive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>

namespace {

QString base() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/dicolens/unpacked";
}

QString mine() {
    return base() + "/" + QString::number(QCoreApplication::applicationPid());
}

}

namespace Archive {

bool isZip(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray magic = file.read(4);
    return magic == QByteArray("PK\x03\x04", 4) || magic == QByteArray("PK\x05\x06", 4);
}

QString unpack(const QString &path, QString *error) {
    auto fail = [&](const char *reason) {
        if (error)
            *error = reason;
        return QString();
    };
    const QString tool = QStandardPaths::findExecutable("bsdtar");
    if (tool.isEmpty())
        return fail(QT_TRANSLATE_NOOP("dicolens", "Opening a zip file needs bsdtar, from libarchive"));
    // Each archive gets its own folder, so two open at once don't mix.
    const QString dir = mine() + "/" + QUuid::createUuid().toString(QUuid::Id128);
    if (!QDir().mkpath(dir))
        return fail(QT_TRANSLATE_NOOP("dicolens", "There's nowhere to unpack the zip file"));
    QFile::setPermissions(QFileInfo(dir).absolutePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QProcess process;
    // bsdtar won't write outside the folder without -P, nor follow links
    // out of it.
    process.start(tool, {"-x", "-f", path, "-C", dir});
    if (!process.waitForStarted() || !process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        QDir(dir).removeRecursively();
        return fail(QT_TRANSLATE_NOOP("dicolens", "The zip file can't be unpacked"));
    }
    return dir;
}

void cleanUp(bool includingThisRun) {
    QDir root(base());
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool number = false;
        const qint64 pid = name.toLongLong(&number);
        const bool ours = pid == QCoreApplication::applicationPid();
        const bool running = number && !ours && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));
        if ((ours && includingThisRun) || (!ours && !running))
            QDir(root.filePath(name)).removeRecursively();
    }
}

}
