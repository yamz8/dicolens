#include "cli.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

#include "anonymize.h"
#include "archive.h"
#include "measurestore.h"
#include "pixels.h"
#include "reformat.h"
#include "reports.h"
#include "study.h"
#include "volumerender.h"

namespace {

const char *Usage = R"(Usage:
  dicolens [file | folder]...                 Open the viewer
  dicolens open [file | folder]...            The same
  dicolens info <file | folder>... [--json]   List the series found, with patient and study
  dicolens tags <file> [--frame <n>] [--json] Every tag in a file, sequences indented
  dicolens export <file | folder> <output.png> [--series <n>] [--slice <n>]
                  [--window <center>,<width>] [--invert]
                                              Save one slice as a PNG or JPEG, windowed as given
                                              or as the file says; series and slice count from 1
  dicolens export <file | folder> <folder> --all [--series <n>] [same options]
                                              Save every slice of a series, 0001.png onwards
  dicolens export <file | folder> <output.png> --3d [--series <n>]
                  [--look bone|vessels|skin|lungs|surface|mip]
                  [--from front|back|left|right|above] [--turn <across>,<down>]
                  [--cut <mm>] [--size <width>x<height>] [--window <center>,<width>]
                                              Save the series in 3D, as the viewer draws it:
                                              --turn in degrees, --cut into it from the front
  dicolens export ... --pet                   Put the PET taken with a CT or MR over it, in
                                              colour, on a slice or in 3D
  dicolens measurements <file | folder> [--series <n>] [--json]
                                              The measurements kept with the scans, drawn in
                                              the viewer, with what each comes to
  dicolens report <file | folder> [--report <n>] [--out <file.pdf>] [--json]
                                              Read a report that came with the scans: a
                                              structured report as text, or a PDF saved with --out
  dicolens anonymize <file | folder>... --out <folder> [--json]
                                              Copy the images without the patient's name, IDs,
                                              birth date, or institution, into a new folder
                                              "Anonymous scans" inside the one given; dates, age,
                                              sex, and weight stay, and reports are left out
  dicolens skill install                      Install the agent skill to ~/.claude/skills/dicolens
  dicolens --help | --version

Folders are read recursively, so a hospital CD's root works, and a zip file
is read as the folder inside it. Commands exit
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
        err() << "dicolens: " << message << Qt::endl;
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

Scan scanPaths(QStringList paths) {
    // A zip is read as the folder it holds.
    for (QString &path : paths)
        if (QFileInfo(path).isFile() && Archive::isZip(path)) {
            QString error;
            const QString dir = Archive::unpack(path, &error);
            if (dir.isEmpty()) {
                QTextStream(stderr) << path << ": " << error << "\n";
                continue;
            }
            path = dir;
        }
    Scan scan = scanFiles(listFiles(paths, true));
    return scan;
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

QJsonObject reportJson(const Report &r, int index) {
    return {
        {"index", index},
        {"kind", r.kind == Report::Kind::Pdf ? "pdf" : "structured"},
        {"title", r.title},
        {"date", DicomFile::formatDate(r.date)},
        {"study", QJsonObject{{"uid", r.studyUid}, {"description", r.studyDescription},
                              {"date", DicomFile::formatDate(r.studyDate)}}},
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
        QJsonArray reports;
        for (int i = 0; i < scan.reports.size(); ++i)
            reports << reportJson(scan.reports[i], i + 1);
        out() << QJsonDocument(QJsonObject{{"files", scan.files},
                                           {"skipped", scan.skipped},
                                           {"unreadable", QJsonArray::fromStringList(scan.unreadable)},
                                           {"series", series},
                                           {"reports", reports},
                                           {"annotations", int(scan.marks.size())}})
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
        if (!scan.reports.isEmpty())
            out() << "\nReports\n";
        for (int i = 0; i < scan.reports.size(); ++i) {
            const Report &r = scan.reports[i];
            out() << QStringLiteral("  %1. %2 %3  %4\n")
                         .arg(i + 1)
                         .arg(r.kind == Report::Kind::Pdf ? "PDF" : "SR", r.title.isEmpty() ? "(no title)" : r.title,
                              DicomFile::formatDate(r.date));
        }
        out() << "\n" << scan.files << (scan.files == 1 ? " DICOM file" : " DICOM files") << " read, " << scan.skipped
              << (scan.skipped == 1 ? " other file" : " other files") << " skipped\n";
        for (const auto &problem : scan.unreadable)
            err() << "unreadable: " << problem << "\n";
    }
    out().flush();
    return scan.series.isEmpty() ? 1 : 0;
}

// Per-frame functional groups hold one item a frame: with a frame chosen
// (from 0), only that frame's is given, as the viewer shows them.
bool otherFrame(const DicomElement &e, int item, int frame) {
    return e.tag == Tag::PerFrameGroups && frame >= 0 && item != frame;
}

QJsonArray tagsJson(const DicomFile &file, const QList<DicomElement> &elements, int frame) {
    QJsonArray array;
    for (const auto &e : elements) {
        QJsonObject o{{"tag", tagText(e.tag)}, {"vr", QString::fromLatin1(e.vr)}, {"name", Dictionary::name(e.tag)}};
        if (!e.items.isEmpty() || e.vr == "SQ") {
            QJsonArray items;
            for (int i = 0; i < e.items.size(); ++i)
                if (!otherFrame(e, i, frame))
                    items << tagsJson(file, e.items[i], frame);
            o["items"] = items;
        } else {
            o["value"] = file.display(e);
        }
        array << o;
    }
    return array;
}

void printTags(const DicomFile &file, const QList<DicomElement> &elements, int depth, int frame) {
    for (const auto &e : elements) {
        const QString indent(depth * 2, ' ');
        out() << indent << tagText(e.tag) << " " << e.vr << " " << Dictionary::name(e.tag).leftJustified(36 - depth * 2)
              << " " << (e.items.isEmpty() && e.vr != "SQ" ? file.display(e) : QString::number(e.items.size()) + " items")
              << "\n";
        for (int i = 0; i < e.items.size(); ++i) {
            if (otherFrame(e, i, frame))
                continue;
            out() << indent << "  > item " << i + 1 << "\n";
            printTags(file, e.items[i], depth + 2, frame);
        }
    }
}

int tags(QStringList args) {
    const bool json = flag(args, "--json");
    const QString frameText = option(args, "--frame");
    if (args.size() != 1)
        return fail("tags needs one file", json);
    QString error;
    const DicomFile file = DicomFile::read(args.first(), &error);
    if (!file.isValid())
        return fail(QStringLiteral("%1: %2").arg(args.first(), error), json);
    int frame = -1;
    if (!frameText.isEmpty()) {
        const int frames = std::max(1, file.integer(Tag::NumberOfFrames, 1));
        frame = frameText.toInt() - 1;
        if (frame < 0 || frame >= frames)
            return fail(QStringLiteral("the file has %1 frames; --frame counts from 1").arg(frames), json);
    }
    if (json)
        out() << QJsonDocument(tagsJson(file, file.elements(), frame)).toJson(QJsonDocument::Indented);
    else
        printTags(file, file.elements(), 0, frame);
    out().flush();
    return 0;
}

bool save(const QImage &image, const QString &path, QString *error) {
    // Written beside the target and renamed over it, so a failure never
    // leaves half a file.
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray format = QFileInfo(path).suffix().toLower() == "jpg" || QFileInfo(path).suffix().toLower() == "jpeg"
                                  ? "JPG" : "PNG";
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, format) || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

// One slice decoded, or why it can't be.
Frame decodeSlice(const Series &series, int index, QString *error) {
    const Slice &slice = series.slices[index];
    const DicomFile file = DicomFile::read(slice.path, error);
    if (!file.isValid()) {
        *error = QStringLiteral("%1: %2").arg(slice.path, *error);
        return {};
    }
    Frame frame = decodeFrame(file, slice.frame);
    if (!frame.error.isEmpty()) {
        *error = QStringLiteral("%1: %2%3").arg(slice.path, frame.error, frame.detail.isEmpty() ? "" : " (" + frame.detail + ")");
        return {};
    }
    return frame;
}

bool decodeAll(const Series &series, QVector<Frame> &frames, QString *error) {
    frames.clear();
    for (int i = 0; i < series.slices.size(); ++i) {
        frames << decodeSlice(series, i, error);
        if (frames.last().isNull())
            return false;
    }
    return true;
}

bool parseWindow(const QString &text, double &center, double &width) {
    const QStringList parts = text.split(',');
    bool okC = false, okW = false;
    if (parts.size() == 2) {
        center = parts[0].toDouble(&okC);
        width = parts[1].toDouble(&okW);
    }
    return okC && okW && width >= 1;
}

// The PET to put over a series, packed and ranged as the viewer does.
struct PetOverlay {
    VoxelGrid grid;
    Overlay3D look;
};

bool loadPet(const Series &pet, PetOverlay &out, QString *error) {
    QVector<Frame> frames;
    if (!decodeAll(pet, frames, error))
        return false;
    out.grid = packVolume(makeVolume(pet), frames, pet.suv > 0 ? pet.suv : 1);
    if (!out.grid.valid) {
        *error = "the PET can't be read whole, so it can't be put over this";
        return false;
    }
    petRange(out.grid, pet.suv > 0, out.look.low, out.look.high);
    out.look.grid = &out.grid;
    out.look.colorMap = 1;
    out.look.opacity = 0.5;
    return true;
}

QString noPartner(const Series &s) {
    return s.modality == "PT" ? QStringLiteral("there's no CT or MR taken with this PET to put it over")
                              : QStringLiteral("there's no PET taken with this series to put over it");
}

int exportVolume(const Scan &scan, int index, const QString &output, QStringList &args, bool json) {
    const bool invert = flag(args, "--invert");
    const bool withPet = flag(args, "--pet");
    const QString lookText = option(args, "--look").toLower(), fromText = option(args, "--from").toLower();
    const QString turnText = option(args, "--turn"), cutText = option(args, "--cut"), sizeText = option(args, "--size");
    const QString windowText = option(args, "--window");
    if (!option(args, "--slice").isEmpty() || flag(args, "--all"))
        return fail("--3d draws the whole series; it takes no --slice or --all", json);

    // Over a PET, the CT or MR it was taken with is the one drawn.
    const Series *series = &scan.series[index];
    const Series *pet = nullptr;
    if (withPet) {
        const int partner = fusionPartner(scan.series, index);
        if (partner < 0)
            return fail(noPartner(*series), json);
        pet = series->modality == "PT" ? series : &scan.series[partner];
        series = series->modality == "PT" ? &scan.series[partner] : series;
    }

    View3D view;
    const QStringList looks{"bone", "vessels", "skin", "lungs", "surface", "mip"};
    if (lookText.isEmpty()) {
        view.look = defaultLook(series->modality);
    } else {
        const int look = looks.indexOf(lookText);
        if (look < 0)
            return fail("--look is one of bone, vessels, skin, lungs, surface, mip", json);
        if (look < 4 && series->modality != "CT")
            return fail("bone, vessels, skin, and lungs are looks for CT; this is " + series->modality +
                        ", so use surface or mip", json);
        view.look = Look(look);
    }
    const QStringList sides{"front", "back", "left", "right", "above"};
    if (!fromText.isEmpty()) {
        if (!sides.contains(fromText))
            return fail("--from is one of front, back, left, right, above", json);
        view.face(sides.indexOf(fromText));
    }
    if (!turnText.isEmpty()) {
        const QStringList parts = turnText.split(',');
        bool okA = false, okD = false;
        const double across = parts.value(0).toDouble(&okA), down = parts.size() == 2 ? parts[1].toDouble(&okD) : 0;
        if (!okA || (parts.size() == 2 && !okD) || parts.size() > 2)
            return fail("--turn takes degrees across, and optionally down, such as 30 or 30,-15", json);
        view.turn(across * M_PI / 180, down * M_PI / 180);
    }
    if (!cutText.isEmpty()) {
        bool ok = false;
        view.cut = cutText.toDouble(&ok);
        if (!ok || view.cut < 0)
            return fail("--cut takes millimetres, such as 40", json);
    }
    QSize size(1024, 1024);
    if (!sizeText.isEmpty()) {
        const QStringList parts = sizeText.toLower().split('x');
        bool okW = false, okH = false;
        size = QSize(parts.value(0).toInt(&okW), parts.size() == 2 ? parts[1].toInt(&okH) : parts.value(0).toInt(&okH));
        if (!okW || !okH || parts.size() > 2 || size.width() < 16 || size.height() < 16 || size.width() > 8192 ||
            size.height() > 8192)
            return fail("--size takes pixels, such as 1024x768, from 16 to 8192", json);
    }

    const Volume volume = makeVolume(*series);
    if (!volume.valid)
        return fail("this series can't be shown in 3D: it needs a stack of parallel, evenly spaced grey slices", json);
    QString error;
    QVector<Frame> frames;
    if (!decodeAll(*series, frames, &error))
        return fail(error, json);
    const VoxelGrid grid = packVolume(volume, frames);
    if (!grid.valid)
        return fail("this series can't be shown in 3D: its slices aren't all the same size and kind", json);

    // A surface and a MIP follow the window, the file's own unless given.
    double center = 0, width = 0;
    if (!windowText.isEmpty()) {
        if (!parseWindow(windowText, center, width))
            return fail("--window takes center,width, such as 40,400", json);
    } else {
        const int middle = series->slices.size() / 2;
        const DicomFile file = DicomFile::read(series->slices[middle].path, &error);
        defaultWindow(file, frames[middle], series->slices[middle].frame, center, width);
    }
    view.center = center;
    view.width = std::max(1.0, width);
    view.inverted = invert;
    view.colorMap = series->modality == "PT" ? 2 : 0;

    PetOverlay overlay;
    if (pet && !loadPet(*pet, overlay, &error))
        return fail(error, json);
    const QImage image = renderVolume(grid, view, size, 0.75, pet ? &overlay.look : nullptr);
    if (image.isNull())
        return fail("the 3D picture couldn't be drawn", json);
    if (!save(image, output, &error))
        return fail(QStringLiteral("can't save %1: %2").arg(output, error), json);

    const int drawn = int(series - scan.series.constData()) + 1;
    if (json) {
        QJsonObject o{{"written", QJsonArray{output}},
                      {"series", drawn},
                      {"look", looks[int(view.look)]},
                      {"from", fromText.isEmpty() ? "front" : fromText},
                      {"cut", view.cut},
                      {"size", QJsonArray{image.width(), image.height()}},
                      {"window", QJsonArray{center, width}}};
        o["pet"] = pet ? QJsonValue(int(pet - scan.series.constData()) + 1) : QJsonValue();
        out() << QJsonDocument(o).toJson(QJsonDocument::Compact) << "\n";
    } else {
        out() << "Saved series " << drawn << " in 3D (" << looks[int(view.look)] << (pet ? ", with the PET over it" : "")
              << ") to " << output << "\n";
    }
    out().flush();
    return 0;
}

int measurements(QStringList args) {
    const bool json = flag(args, "--json");
    const QString seriesText = option(args, "--series");
    if (args.size() != 1)
        return fail("measurements needs a file or folder", json);
    if (!QFileInfo::exists(args[0]))
        return fail(QStringLiteral("%1 doesn't exist").arg(args[0]), json);
    const Scan scan = scanPaths({args[0]});
    if (scan.series.isEmpty())
        return fail(QStringLiteral("no DICOM images in %1").arg(args[0]), json);
    QList<int> chosen;
    if (!seriesText.isEmpty()) {
        const int n = seriesText.toInt();
        if (n < 1 || n > scan.series.size())
            return fail(QStringLiteral("there are %1 series; --series counts from 1").arg(scan.series.size()), json);
        chosen << n - 1;
    } else {
        for (int i = 0; i < scan.series.size(); ++i)
            chosen << i;
    }

    static const char *kinds[] = {"", "ruler", "area", "angle", "cobb"};
    static const char *planes[] = {"scanned", "coronal", "sagittal"};
    QJsonArray list;
    QString text;
    QTextStream lines(&text);
    for (int index : chosen) {
        const Series &s = scan.series[index];
        const auto kept = MeasureStore::instance().all(s.uid);
        if (kept.isEmpty() && seriesText.isEmpty())
            continue;
        const QString valueUnit = s.suv > 0 ? "SUV" : s.modality == "CT" ? "HU" : "";
        const Volume volume = makeVolume(s);
        QVector<Frame> frames;   // Every slice, read once a cut plane's area needs them.
        QJsonArray measures;
        lines << "Series " << index + 1 << "  " << s.description << "\n";
        for (const auto &placed : kept) {
            const Measure &m = placed.measure;
            if (m.kind < 1 || m.kind > 4 || placed.plane < 0 || placed.plane > 2)
                continue;
            MeasureScale scale;
            Frame picture;
            QString error;
            if (placed.plane == 0) {
                if (placed.slice < 0 || placed.slice >= s.slices.size())
                    continue;
                if (s.hasSpacing) {
                    scale.even = true;
                    scale.across = s.spacing[1];
                    scale.down = s.spacing[0];
                } else {
                    scale.regions = s.regions;
                }
                scale.aspect = s.pixelAspect();
                if (m.kind == 2)
                    picture = decodeSlice(s, placed.slice, &error);
            } else {
                if (!volume.valid)
                    continue;
                const Cut cut = cutAcross(volume, placed.plane == 1 ? 1 : 0);
                if (placed.slice < 0 || placed.slice >= cut.count(volume))
                    continue;
                scale.even = true;
                scale.across = volume.spacing[cut.horizontal];
                scale.down = volume.spacing[cut.vertical];
                scale.aspect = scale.across / scale.down;
                if (m.kind == 2 && (!frames.isEmpty() || decodeAll(s, frames, &error)))
                    picture = reformat(volume, cut, placed.slice, frames);
            }
            const MeasureValues v = measureValues(m, scale, picture.isNull() ? nullptr : &picture, s.suv > 0 ? s.suv : 1);
            if (!v.valid)
                continue;
            QJsonArray points{QJsonArray{m.a.x(), m.a.y()}, QJsonArray{m.b.x(), m.b.y()}};
            if (m.kind >= 3)
                points << QJsonArray{m.c.x(), m.c.y()};
            if (m.kind == 4)
                points << QJsonArray{m.d.x(), m.d.y()};
            QJsonObject o{{"kind", kinds[m.kind]}, {"plane", planes[placed.plane]}, {"slice", placed.slice + 1},
                          {"points", points}};
            QString what;
            auto fixed = [](double x, int decimals) { return QString::number(x, 'f', decimals); };
            if (m.kind == 1) {
                o["length"] = v.length;
                o["unit"] = v.millimetres ? "mm" : "px";
                what = "ruler  " + fixed(v.length, 1) + (v.millimetres ? " mm" : " px");
            } else if (m.kind == 2) {
                o["area"] = v.area;
                o["unit"] = v.millimetres ? "mm²" : "px²";
                what = "area  " + fixed(v.area, 1) + (v.millimetres ? " mm²" : " px²");
                if (v.count > 0) {
                    o["pixels"] = v.count;
                    o["mean"] = v.mean;
                    o["sd"] = v.sd;
                    o["min"] = v.low;
                    o["max"] = v.high;
                    o["valueUnit"] = valueUnit;
                    const QString u = valueUnit.isEmpty() ? "" : " " + valueUnit;
                    what += "  mean " + fixed(v.mean, 1) + u + ", SD " + fixed(v.sd, 1) + ", " + fixed(v.low, 1) + " to " +
                            fixed(v.high, 1) + u;
                } else if (!error.isEmpty()) {
                    o["valuesError"] = error;
                }
            } else {
                o["degrees"] = v.angle;
                what = QString(m.kind == 4 ? "Cobb angle  " : "angle  ") + fixed(v.angle, 1) + "°";
            }
            measures << o;
            lines << "  slice " << placed.slice + 1 << (placed.plane ? QString(" (%1)").arg(planes[placed.plane]) : QString())
                  << "  " << what << "\n";
        }
        if (measures.isEmpty())
            lines << "  no measurements\n";
        list << QJsonObject{{"index", index + 1}, {"uid", s.uid}, {"description", s.description},
                            {"measurements", measures}};
    }
    if (json)
        out() << QJsonDocument(QJsonObject{{"series", list}}).toJson(QJsonDocument::Compact) << "\n";
    else if (list.isEmpty())
        out() << "No measurements are kept with these scans. Measurements drawn in the viewer are.\n";
    else
        out() << text;
    out().flush();
    return 0;
}

int exportImages(QStringList args) {
    const bool json = flag(args, "--json");
    const bool threeD = flag(args, "--3d");
    const QString seriesText = option(args, "--series");
    if (threeD) {
        // The 3D options are taken out first; what's left is input and output.
        QStringList rest = args;
        for (const char *name : {"--look", "--from", "--turn", "--cut", "--size", "--window", "--slice"}) {
            const int i = rest.indexOf(name);
            if (i >= 0)
                rest.remove(i, std::min(2, int(rest.size()) - i));
        }
        for (const char *name : {"--invert", "--pet", "--all"})
            rest.removeAll(name);
        if (rest.size() != 2)
            return fail("export needs a file or folder and where to save", json);
        if (!QFileInfo::exists(rest[0]))
            return fail(QStringLiteral("%1 doesn't exist").arg(rest[0]), json);
        const Scan scan = scanPaths({rest[0]});
        if (scan.series.isEmpty())
            return fail(QStringLiteral("no DICOM images in %1").arg(rest[0]), json);
        const int seriesIndex = seriesText.isEmpty() ? 1 : seriesText.toInt();
        if (seriesIndex < 1 || seriesIndex > scan.series.size())
            return fail(QStringLiteral("there are %1 series; --series counts from 1").arg(scan.series.size()), json);
        return exportVolume(scan, seriesIndex - 1, rest[1], args, json);
    }
    const bool all = flag(args, "--all");
    const bool invert = flag(args, "--invert");
    const bool withPet = flag(args, "--pet");
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
        if (!parseWindow(windowText, center, width))
            return fail("--window takes center,width, such as 40,400", json);
        fixedWindow = true;
    }

    // A PET goes over the CT or MR's own slices, so the series given is that one.
    PetOverlay pet;
    if (withPet) {
        const int partner = fusionPartner(scan.series, seriesIndex - 1);
        if (partner < 0)
            return fail(noPartner(series), json);
        if (series.modality == "PT")
            return fail(QStringLiteral("--pet goes over the CT or MR's slices: choose that series, --series %1").arg(partner + 1), json);
        QString error;
        if (!loadPet(scan.series[partner], pet, &error))
            return fail(error, json);
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
        if (withPet && !frame.isColor())
            image = fuseSlice(image, slicePlane(series, indices[n]), pet.grid, pet.look);
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

int report(QStringList args) {
    const bool json = flag(args, "--json");
    const QString number = option(args, "--report"), output = option(args, "--out");
    if (args.isEmpty())
        return fail("report needs a file or folder", json);
    for (const auto &path : args)
        if (!QFileInfo::exists(path))
            return fail(QStringLiteral("%1 doesn't exist").arg(path), json);
    const Scan scan = scanPaths(args);
    if (scan.reports.isEmpty())
        return fail("no reports were found there", json);
    const int index = number.isEmpty() ? 1 : number.toInt();
    if (index < 1 || index > scan.reports.size())
        return fail(QStringLiteral("there are %1 reports; --report counts from 1").arg(scan.reports.size()), json);
    const Report &r = scan.reports[index - 1];
    const DicomFile file = DicomFile::read(r.path);
    QJsonObject result = reportJson(r, index);

    if (r.kind == Report::Kind::Pdf) {
        if (output.isEmpty())
            return fail("this report is a PDF; save it with --out report.pdf", json);
        const QByteArray pdf = Reports::pdf(file);
        QSaveFile saved(output);
        saved.setDirectWriteFallback(false);
        if (!saved.open(QIODevice::WriteOnly) || saved.write(pdf) != pdf.size() || !saved.commit())
            return fail(QStringLiteral("%1 couldn't be saved: %2").arg(output, saved.errorString()), json);
        result["saved"] = QFileInfo(output).absoluteFilePath();
        if (json)
            out() << QJsonDocument(result).toJson(QJsonDocument::Indented);
        else
            out() << "Saved " << output << "\n";
        out().flush();
        return 0;
    }

    const QList<ReportLine> lines = Reports::lines(file);
    if (json) {
        QJsonArray content;
        for (const auto &line : lines)
            content << QJsonObject{{"depth", line.depth}, {"heading", line.heading}, {"name", line.name}, {"value", line.value}};
        result["content"] = content;
        out() << QJsonDocument(result).toJson(QJsonDocument::Indented);
    } else {
        out() << r.title << "\n\n" << Reports::text(lines) << "\n";
    }
    out().flush();
    return 0;
}

int anonymize(QStringList args) {
    const bool json = flag(args, "--json");
    const QString output = option(args, "--out");
    if (args.isEmpty() || output.isEmpty())
        return fail("anonymize needs files or folders, and --out <folder>", json);
    for (const auto &path : args)
        if (!QFileInfo::exists(path))
            return fail(QStringLiteral("%1 doesn't exist").arg(path), json);
    const Scan scan = scanPaths(args);
    if (scan.series.isEmpty())
        return fail("no DICOM images were found there", json);
    if (!QDir().mkpath(output))
        return fail(QStringLiteral("%1 can't be created").arg(output), json);
    const Anonymize::Result r = Anonymize::copyScan(scan, output);
    for (const auto &e : r.errors)
        err() << "dicolens: " << e << "\n";
    if (r.written == 0)
        return fail("nothing could be copied", json);
    if (json) {
        out() << QJsonDocument(QJsonObject{{"folder", QFileInfo(r.folder).absoluteFilePath()},
                                           {"written", r.written},
                                           {"reportsLeftOut", r.skipped},
                                           {"mayShowDetailsInPicture", r.showDetails},
                                           {"failed", int(r.errors.size())}})
                     .toJson(QJsonDocument::Indented);
    } else {
        out() << "Copied " << r.written << " files into " << r.folder << "\n";
        if (r.skipped)
            out() << "Left out " << r.skipped << " reports, as their text names the patient\n";
        if (r.showDetails)
            out() << r.showDetails << " images may show the patient's details in the picture itself; "
                  << "look at them before sharing\n";
    }
    out().flush();
    return r.errors.isEmpty() ? 0 : 1;
}

int skill(QStringList args) {
    const bool json = flag(args, "--json");
    if (args != QStringList{"install"})
        return fail("try: dicolens skill install", json);
    QFile bundled(":/skill/SKILL.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return fail("the skill isn't bundled in this build", json);
    const QString directory = QDir::homePath() + "/.claude/skills/dicolens";
    QDir().mkpath(directory);
    QSaveFile file(directory + "/SKILL.md");
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bundled.readAll()) < 0 || !file.commit())
        return fail(file.errorString(), json);
    if (json)
        out() << QJsonDocument(QJsonObject{{"installed", file.fileName()}}).toJson(QJsonDocument::Compact) << "\n";
    else
        out() << "Installed " << file.fileName() << "\n";
    out().flush();
    return 0;
}

// Every command by name: the one list runCli runs from and needsNoWindow
// answers from, so a new command can't be left out of either.
const QHash<QString, int (*)(QStringList)> &commands() {
    static const QHash<QString, int (*)(QStringList)> table{
        {"info", info},     {"tags", tags},                 {"export", exportImages}, {"skill", skill},
        {"report", report}, {"measurements", measurements}, {"anonymize", anonymize},
    };
    return table;
}

}

bool needsNoWindow(int argc, char *argv[]) {
    if (argc < 2)
        return false;
    // An option is --help, --version, or a mistake the usage answers.
    const QString first = QString::fromLocal8Bit(argv[1]);
    return first.startsWith('-') || first == "help" || commands().contains(first);
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
        out() << "dicolens " << QCoreApplication::applicationVersion() << "\n";
        out().flush();
        return 0;
    }
    if (const auto run = commands().value(command))
        return run(args.mid(1));
    if (command == "open")
        args.removeFirst();
    for (const auto &arg : args)
        if (arg.startsWith("-")) {
            err() << "dicolens: unknown option " << arg << "\n\n" << Usage;
            err().flush();
            return 1;
        }
    open = args;
    return -1;
}
