#include "cli.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

#include "pixels.h"
#include "study.h"

namespace {

const char *Usage = R"(Usage:
  scans [file | folder]...                 Open the viewer
  scans open [file | folder]...            The same
  scans info <file | folder>... [--json]   List the series found, with patient and study
  scans tags <file> [--frame <n>] [--json] Every tag in a file, sequences indented
  scans export <file | folder> <output.png> [--series <n>] [--slice <n>]
               [--window <center>,<width>] [--invert]
                                           Save one slice as a PNG or JPEG, windowed as given
                                           or as the file says; series and slice count from 1
  scans export <file | folder> <folder> --all [--series <n>] [same options]
                                           Save every slice of a series, 0001.png onwards
  scans skill install                      Install the agent skill to ~/.claude/skills/scans
  scans --help | --version

Folders are read recursively, so a hospital CD's root works. Commands exit
0 on success and 1 on failure, with errors on stderr.
)";

QTextStream &out() {
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err() {
    static QTextStream stream(stderr);
    return stream;
}

int fail(const QString &message, bool json) {
    if (json)
        err() << QJsonDocument(QJsonObject{{"error", message}}).toJson(QJsonDocument::Compact) << Qt::endl;
    else
        err() << "scans: " << message << Qt::endl;
    return 1;
}

QString option(QStringList &args, const QString &name) {
    const int i = args.indexOf(name);
    if (i < 0 || i + 1 >= args.size())
        return {};
    const QString value = args[i + 1];
    args.remove(i, 2);
    return value;
}

bool flag(QStringList &args, const QString &name) { return args.removeAll(name) > 0; }

Scan scanPaths(const QStringList &paths) {
    return scanFiles(listFiles(paths, true));
}

QJsonObject seriesJson(const Series &s, int index) {
    return {
        {"index", index},
        {"uid", s.uid},
        {"number", s.number},
        {"description", s.description},
        {"modality", s.modality},
        {"bodyPart", s.bodyPart},
        {"images", int(s.slices.size())},
        {"rows", s.rows},
        {"columns", s.columns},
        {"color", s.color},
        {"pixelSpacing", s.hasSpacing ? QJsonArray{s.spacing[0], s.spacing[1]} : QJsonValue()},
        {"sliceThickness", s.thickness > 0 ? QJsonValue(s.thickness) : QJsonValue()},
        {"transferSyntax", Dictionary::transferSyntaxName(s.transferSyntax).isEmpty()
                               ? s.transferSyntax : Dictionary::transferSyntaxName(s.transferSyntax)},
        {"decodable", canDecode(s.transferSyntax)},
        {"patient", QJsonObject{{"name", s.patientName}, {"id", s.patientId},
                                {"birthDate", DicomFile::formatDate(s.patientBirthDate)},
                                {"sex", s.patientSex}, {"age", s.patientAge}}},
        {"study", QJsonObject{{"uid", s.studyUid}, {"description", s.studyDescription},
                              {"date", DicomFile::formatDate(s.studyDate)}, {"institution", s.institution}}},
    };
}

int info(QStringList args) {
    const bool json = flag(args, "--json");
    if (args.isEmpty())
        return fail("info needs a file or folder", json);
    for (const auto &path : args)
        if (!QFileInfo::exists(path))
            return fail(QStringLiteral("%1 doesn't exist").arg(path), json);
    const Scan scan = scanPaths(args);
    if (json) {
        QJsonArray series;
        for (int i = 0; i < scan.series.size(); ++i)
            series << seriesJson(scan.series[i], i + 1);
        out() << QJsonDocument(QJsonObject{{"files", scan.files},
                                           {"skipped", scan.skipped},
                                           {"unreadable", QJsonArray::fromStringList(scan.unreadable)},
                                           {"series", series}})
                     .toJson(QJsonDocument::Indented);
    } else {
        QString study;
        for (int i = 0; i < scan.series.size(); ++i) {
            const Series &s = scan.series[i];
            const QString header = QStringLiteral("%1  %2  %3").arg(s.patientName, DicomFile::formatDate(s.studyDate), s.studyDescription).trimmed();
            if (header != study)
                out() << (i ? "\n" : "") << header << "\n";
            study = header;
            out() << QStringLiteral("  %1. %2 %3  %4 images  %5x%6%7\n")
                         .arg(i + 1)
                         .arg(s.modality, s.description.isEmpty() ? "(no description)" : s.description)
                         .arg(s.slices.size())
                         .arg(s.columns)
                         .arg(s.rows)
                         .arg(canDecode(s.transferSyntax) ? "" : "  (can't decode " + s.transferSyntax + ")");
        }
        out() << "\n" << scan.files << (scan.files == 1 ? " DICOM file" : " DICOM files") << " read, " << scan.skipped
              << (scan.skipped == 1 ? " other file" : " other files") << " skipped\n";
        for (const auto &problem : scan.unreadable)
            err() << "unreadable: " << problem << "\n";
    }
    out().flush();
    return scan.series.isEmpty() ? 1 : 0;
}

QJsonArray tagsJson(const DicomFile &file, const QList<DicomElement> &elements) {
    QJsonArray array;
    for (const auto &e : elements) {
        QJsonObject o{{"tag", tagText(e.tag)}, {"vr", QString::fromLatin1(e.vr)}, {"name", Dictionary::name(e.tag)}};
        if (!e.items.isEmpty() || e.vr == "SQ") {
            QJsonArray items;
            for (const auto &item : e.items)
                items << tagsJson(file, item);
            o["items"] = items;
        } else {
            o["value"] = file.display(e);
        }
        array << o;
    }
    return array;
}

void printTags(const DicomFile &file, const QList<DicomElement> &elements, int depth) {
    for (const auto &e : elements) {
        const QString indent(depth * 2, ' ');
        out() << indent << tagText(e.tag) << " " << e.vr << " " << Dictionary::name(e.tag).leftJustified(36 - depth * 2)
              << " " << (e.items.isEmpty() && e.vr != "SQ" ? file.display(e) : QString::number(e.items.size()) + " items")
              << "\n";
        for (int i = 0; i < e.items.size(); ++i) {
            out() << indent << "  > item " << i + 1 << "\n";
            printTags(file, e.items[i], depth + 2);
        }
    }
}

int tags(QStringList args) {
    const bool json = flag(args, "--json");
    if (args.size() != 1)
        return fail("tags needs one file", json);
    QString error;
    const DicomFile file = DicomFile::read(args.first(), &error);
    if (!file.isValid())
        return fail(QStringLiteral("%1: %2").arg(args.first(), error), json);
    if (json)
        out() << QJsonDocument(tagsJson(file, file.elements())).toJson(QJsonDocument::Indented);
    else
        printTags(file, file.elements(), 0);
    out().flush();
    return 0;
}

bool save(const QImage &image, const QString &path, QString *error) {
    // Written beside the target and renamed over it, so a failure never
    // leaves half a file.
    QSaveFile file(path);
    const QByteArray format = QFileInfo(path).suffix().toLower() == "jpg" || QFileInfo(path).suffix().toLower() == "jpeg"
                                  ? "JPG" : "PNG";
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, format) || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

int exportImages(QStringList args) {
    const bool json = flag(args, "--json");
    const bool all = flag(args, "--all");
    const bool invert = flag(args, "--invert");
    const QString seriesText = option(args, "--series");
    const QString sliceText = option(args, "--slice");
    const QString windowText = option(args, "--window");
    if (args.size() != 2)
        return fail("export needs a file or folder and where to save", json);
    const QString input = args[0], output = args[1];
    if (!QFileInfo::exists(input))
        return fail(QStringLiteral("%1 doesn't exist").arg(input), json);

    const Scan scan = scanPaths({input});
    if (scan.series.isEmpty())
        return fail(QStringLiteral("no DICOM images in %1").arg(input), json);
    const int seriesIndex = seriesText.isEmpty() ? 1 : seriesText.toInt();
    if (seriesIndex < 1 || seriesIndex > scan.series.size())
        return fail(QStringLiteral("there are %1 series; --series counts from 1").arg(scan.series.size()), json);
    const Series &series = scan.series[seriesIndex - 1];

    double center = 0, width = 0;
    bool fixedWindow = false;
    if (!windowText.isEmpty()) {
        const QStringList parts = windowText.split(',');
        bool okC = false, okW = false;
        if (parts.size() == 2) {
            center = parts[0].toDouble(&okC);
            width = parts[1].toDouble(&okW);
        }
        if (!okC || !okW || width < 1)
            return fail("--window takes center,width, such as 40,400", json);
        fixedWindow = true;
    }

    QList<int> indices;
    if (all) {
        for (int i = 0; i < series.slices.size(); ++i)
            indices << i;
        if (!QDir().mkpath(output))
            return fail(QStringLiteral("can't make the folder %1").arg(output), json);
    } else {
        const int slice = sliceText.isEmpty() ? 1 : sliceText.toInt();
        if (slice < 1 || slice > series.slices.size())
            return fail(QStringLiteral("the series has %1 slices; --slice counts from 1").arg(series.slices.size()), json);
        indices << slice - 1;
    }

    // Every slice of a series shares the first one's window, as on screen,
    // unless one is given.
    QJsonArray written;
    QList<Frame> video;
    for (int n = 0; n < indices.size(); ++n) {
        const Slice &slice = series.slices[indices[n]];
        QString error;
        const DicomFile file = DicomFile::read(slice.path, &error);
        if (!file.isValid())
            return fail(QStringLiteral("%1: %2").arg(slice.path, error), json);
        Frame frame;
        if (isVideo(file.transferSyntax())) {
            if (video.isEmpty())
                video = decodeVideo(file);
            frame = slice.frame < video.size() ? video[slice.frame] : video.value(0);
        } else {
            frame = decodeFrame(file, slice.frame);
        }
        if (!frame.error.isEmpty())
            return fail(QStringLiteral("%1: %2%3").arg(slice.path, frame.error, frame.detail.isEmpty() ? "" : " (" + frame.detail + ")"), json);
        if (!fixedWindow) {
            defaultWindow(file, frame, slice.frame, center, width);
            fixedWindow = true;
        }
        QImage image = render(frame, center, width, invert);
        // Pixels that aren't square are shown at their true shape.
        const double aspect = series.pixelAspect();
        if (std::abs(aspect - 1) > 0.01)
            image = image.scaled(qRound(image.width() * std::max(1.0, aspect)), qRound(image.height() * std::max(1.0, 1 / aspect)),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        const QString path = all ? QDir(output).filePath(QStringLiteral("%1.png").arg(n + 1, 4, 10, QLatin1Char('0'))) : output;
        if (!save(image, path, &error))
            return fail(QStringLiteral("can't save %1: %2").arg(path, error), json);
        written << path;
    }
    if (json)
        out() << QJsonDocument(QJsonObject{{"written", written}, {"window", QJsonArray{center, width}}})
                     .toJson(QJsonDocument::Compact)
              << "\n";
    else
        out() << "Saved " << written.size() << (written.size() == 1 ? " image" : " images") << " to " << output << "\n";
    out().flush();
    return 0;
}

int skill(QStringList args) {
    const bool json = flag(args, "--json");
    if (args != QStringList{"install"})
        return fail("try: scans skill install", json);
    QFile bundled(":/skill/SKILL.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return fail("the skill isn't bundled in this build", json);
    const QString directory = QDir::homePath() + "/.claude/skills/scans";
    QDir().mkpath(directory);
    QSaveFile file(directory + "/SKILL.md");
    if (!file.open(QIODevice::WriteOnly) || file.write(bundled.readAll()) < 0 || !file.commit())
        return fail(file.errorString(), json);
    if (json)
        out() << QJsonDocument(QJsonObject{{"installed", file.fileName()}}).toJson(QJsonDocument::Compact) << "\n";
    else
        out() << "Installed " << file.fileName() << "\n";
    out().flush();
    return 0;
}

}

int runCli(const QStringList &arguments, QStringList &open) {
    QStringList args = arguments.mid(1);
    const QString command = args.value(0);
    if (command == "--help" || command == "-h" || command == "help") {
        out() << Usage;
        out().flush();
        return 0;
    }
    if (command == "--version") {
        out() << "scans " << QCoreApplication::applicationVersion() << "\n";
        out().flush();
        return 0;
    }
    if (command == "info")
        return info(args.mid(1));
    if (command == "tags")
        return tags(args.mid(1));
    if (command == "export")
        return exportImages(args.mid(1));
    if (command == "skill")
        return skill(args.mid(1));
    if (command == "open")
        args.removeFirst();
    for (const auto &arg : args)
        if (arg.startsWith("-")) {
            err() << "scans: unknown option " << arg << "\n\n" << Usage;
            err().flush();
            return 1;
        }
    open = args;
    return -1;
}
