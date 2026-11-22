#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include "anonymize.h"
#include "dicom.h"
#include "pixels.h"
#include "study.h"

#include "support.h"

// Anonymous copies: that each attribute gets what PS3.15 Table E.1-1 says,
// that a copy still naming the patient isn't kept, that a plan says what a
// copy would do, and that a region blacked out of a picture changes those
// pixels and no others.

namespace {

const QString ImplicitLE = "1.2.840.10008.1.2";
const char *CtImage = "1.2.840.10008.5.1.4.1.1.2";

// An element of an item, by tag.
const DicomElement *in(const QList<DicomElement> &item, quint32 tag) {
    for (const auto &e : item)
        if (e.tag == tag)
            return &e;
    return nullptr;
}

// A value as text, without its padding.
QString valueOf(const DicomElement *e) {
    return e ? QString::fromLatin1(e->value).remove(QChar('\0')).trimmed() : QStringLiteral("(absent)");
}

QByteArray bytesOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The image pixel module, with a series of its own.
Writer &module(Writer &w, const QByteArray &series, int rows, int columns, int bits, int stored, bool isSigned,
               const QByteArray &photometric = "MONOCHROME2") {
    return w.add(Tag::SeriesInstance, "UI", series)
        .us(Tag::SamplesPerPixel, photometric.startsWith("MONO") || photometric.startsWith("PALETTE") ? 1 : 3)
        .add(Tag::Photometric, "CS", photometric)
        .us(Tag::Rows, rows)
        .us(Tag::Columns, columns)
        .us(Tag::BitsAllocated, bits)
        .us(Tag::BitsStored, stored)
        .us(Tag::HighBit, stored - 1)
        .us(Tag::PixelRepresentation, isSigned ? 1 : 0);
}

// A CT image of Jane Doe's, 2 by 2, with what a hospital's files say of a
// patient around it.
Writer janeDoe(bool explicitVr, const QByteArray &instance, const QByteArray &series = "1.2.7.3") {
    Writer w;
    w.explicitVr = explicitVr;
    w.add(Tag::SopClass, "UI", CtImage)
        .add(Tag::SopInstance, "UI", instance)
        .add(Tag::StudyDate, "DA", "20250101")
        .add(0x00080050, "SH", "ACC123")
        .add(Tag::Modality, "CS", "CT")
        .add(Tag::Institution, "LO", "Saint Elsewhere")
        .add(0x00080090, "PN", "House^Gregory")
        .add(Tag::StudyDescription, "LO", "Head CT")
        .add(Tag::SeriesDescription, "LO", "Axial 5 mm")
        .add(Tag::PatientName, "PN", "Doe^Jane")
        .add(Tag::PatientId, "LO", "MRN42")
        .add(Tag::PatientBirthDate, "DA", "19700101")
        .add(Tag::PatientSex, "CS", "F")
        .add(Tag::PatientAge, "AS", "055Y")
        .add(0x00181030, "LO", "Head routine")
        .add(Tag::StudyInstance, "UI", "1.2.7");
    module(w, series, 2, 2, 16, 12, false);
    return w.add(Tag::InstanceNumber, "IS", instance.mid(instance.lastIndexOf('.') + 1));
}

// PS3.5 annex G with every byte a literal: nothing is saved, and all a
// decoder needs is there. One segment a byte of each sample, the most
// significant first.
QByteArray rle(const QList<QByteArray> &segments) {
    QByteArray header(64, 0), body;
    qToLittleEndian<quint32>(quint32(segments.size()), header.data());
    for (int i = 0; i < segments.size(); ++i) {
        qToLittleEndian<quint32>(quint32(64 + body.size()), header.data() + 4 + 4 * i);
        for (qsizetype at = 0; at < segments[i].size(); at += 128) {
            const QByteArray run = segments[i].mid(at, 128);
            body += char(run.size() - 1);
            body += run;
        }
        if (body.size() % 2)
            body += char(0);
    }
    return header + body;
}

// A colour picture as a baseline JPEG, by ffmpeg, or nothing when it can't.
QByteArray jpeg(const QByteArray &rgb, int width, int height) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
        return {};
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", QString("%1x%2").arg(width).arg(height),
                            "-i", "-", "-c:v", "mjpeg", "-pix_fmt", "yuvj444p", "-q:v", "2", "-f", "image2", "-update", "1", "-"});
    ffmpeg.write(rgb);
    ffmpeg.closeWriteChannel();
    ffmpeg.waitForFinished();
    return ffmpeg.exitCode() == 0 ? ffmpeg.readAllStandardOutput() : QByteArray();
}

QStringList names(const QList<Anonymize::Count> &counts) {
    QStringList list;
    for (const auto &c : counts)
        list << c.name;
    return list;
}

int filesOf(const QList<Anonymize::Count> &counts, const QString &name) {
    for (const auto &c : counts)
        if (c.name == name)
            return c.files;
    return 0;
}

}

Q_DECLARE_METATYPE(QList<QRect>)

class AnonymizeTests : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    int m_files = 0;

    // A new path of the tests' own.
    QString fresh(const QString &suffix = {}) { return m_dir.filePath(QStringLiteral("%1%2").arg(++m_files).arg(suffix)); }

    DicomFile onDisk(const QByteArray &bytes) {
        const QString path = fresh(".src");
        return write(path, bytes) ? DicomFile::read(path) : DicomFile();
    }

    // One file anonymized on its own.
    struct Made {
        bool written = false;
        QString error, path;
        DicomFile copy;
        Anonymize::Outcome outcome;
    };
    Made anonymized(const QByteArray &source, const Anonymize::Options &options = {}, const QList<QRect> &blackout = {},
                    Anonymize::Session *session = nullptr) {
        Anonymize::Session own;
        Made made;
        made.path = fresh(".dcm");
        made.written = Anonymize::write(onDisk(source), made.path, session ? *session : own, &made.error, options,
                                        blackout, &made.outcome);
        if (made.written)
            made.copy = DicomFile::read(made.path);
        return made;
    }

    QProcess *run(const QStringList &arguments) {
        auto *p = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", "offscreen");
        p->setProcessEnvironment(env);
        p->start(QCoreApplication::applicationDirPath() + "/dicolens", arguments);
        return p->waitForFinished(60000) ? p : nullptr;
    }

private slots:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    void anonymizesACopy() {
        QTemporaryDir dir;
        Writer reference;
        reference.add(0x00081150, "UI", "1.2.840.10008.5.1.4.1.1.2").add(0x00081155, "UI", "1.2.7.1");
        Writer w;
        w.add(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", "1.2.7.2")
            .add(Tag::StudyDate, "DA", "20250101")
            .add(0x00080050, "SH", "ACC123")
            .add(Tag::Modality, "CS", "CT")
            .add(0x00080080, "LO", "Saint Elsewhere")
            .add(0x00080090, "PN", "House^Gregory")
            .sequence(0x00081140, {reference})
            .add(0x00090010, "LO", "VENDOR")
            .add(0x00091001, "LO", "secret")
            .add(0x00100010, "PN", "Doe^Jane")
            .add(0x00100020, "LO", "MRN42")
            .add(0x00100030, "DA", "19700101")
            .add(0x00100040, "CS", "F")
            .add(0x00101030, "DS", "70")
            .add(0x0020000D, "UI", "1.2.7")
            .image(2, 2, 16, 12, false)
            .add(0x0020000E, "UI", "1.2.7.3")
            .pixels(le16({1, 2, 3, 4}));
        QVERIFY(write(dir.filePath("IM1"), w.file(ExplicitLE)));
        // The same patient's other image, written implicitly, as older
        // systems do.
        Writer implicit;
        implicit.explicitVr = false;
        implicit.add(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", "1.2.7.1")
            .add(Tag::Modality, "CS", "CT")
            .add(0x00100010, "PN", "Doe^Jane")
            .add(0x00100020, "LO", "MRN42")
            .add(0x0020000D, "UI", "1.2.7")
            .image(2, 2, 16, 12, false)
            .add(0x0020000E, "UI", "1.2.7.3")
            .add(Tag::InstanceNumber, "IS", "2")
            .pixels(le16({5, 6, 7, 8}));
        QVERIFY(write(dir.filePath("IM2"), implicit.file("1.2.840.10008.1.2")));

        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QTemporaryDir out;
        const Anonymize::Result result = Anonymize::copyScan(scan, out.path());
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n")));
        QCOMPARE(result.written, 2);
        QCOMPARE(result.folder, out.filePath("Anonymous scans"));

        const Scan copied = scanFiles(listFiles({result.folder}, true));
        QCOMPARE(copied.series.size(), 1);
        QCOMPARE(copied.series.first().slices.size(), 2);
        const DicomFile a = DicomFile::read(copied.series.first().slices[0].path);
        const DicomFile b = DicomFile::read(copied.series.first().slices[1].path);
        for (const DicomFile *f : {&a, &b}) {
            QVERIFY(f->isValid());
            QCOMPARE(f->first(0x00100010), QString("Anonymous"));
            QCOMPARE(f->first(0x00100020), QString("ANON1"));
            QVERIFY(!f->has(0x00091001) && !f->has(0x00090010));
            QCOMPARE(f->first(0x00120062), QString("YES"));
            QCOMPARE(f->first(0x00080016), QString("1.2.840.10008.5.1.4.1.1.2"));
            QVERIFY(f->first(0x0020000D).startsWith("2.25."));
            QCOMPARE(f->first(0x0020000D), a.first(0x0020000D));
        }
        // The institution's name is one the table lets an IOD require a
        // value of (X/Z/D), so a value that names nobody stands there.
        const DicomFile &explicitCopy = a.has(0x00081140) ? a : b;
        QCOMPARE(explicitCopy.first(0x00080080), QString("Anonymized"));
        QCOMPARE(explicitCopy.first(0x00080050), QString());
        QCOMPARE(explicitCopy.first(0x00080090), QString());
        QCOMPARE(explicitCopy.first(0x00100030), QString());
        QCOMPARE(explicitCopy.first(0x00100040), QString("F"));
        QCOMPARE(explicitCopy.first(Tag::StudyDate), QString("20250101"));
        // The reference to the other image still finds it, by its new UID.
        const DicomFile &withReference = a.has(0x00081140) ? a : b;
        const DicomFile &referenced = a.has(0x00081140) ? b : a;
        const DicomElement *refs = withReference.element(0x00081140);
        QVERIFY(refs && refs->items.size() == 1);
        QString uid;
        for (const auto &e : refs->items.first())
            if (e.tag == 0x00081155)
                uid = QString::fromLatin1(e.value).remove(QChar('\0')).trimmed();
        QCOMPARE(uid, referenced.first(0x00080018));
        // The pictures are untouched.
        const Frame pixels = decodeFrame(withReference, 0);
        QCOMPARE(pixels.mono, (QVector<quint16>{1, 2, 3, 4}));
        QCOMPARE(decodeFrame(referenced, 0).mono, (QVector<quint16>{5, 6, 7, 8}));
        QCOMPARE(referenced.transferSyntax(), QString("1.2.840.10008.1.2"));
        // Nothing unchecked is left beside the copies.
        for (const QString &path : listFiles({result.folder}, true))
            QVERIFY2(path.endsWith(".dcm"), qPrintable(path));

        // Ultrasound may have the name in the picture, which is said.
        Writer us;
        us.add(Tag::Modality, "CS", "US");
        QVERIFY(Anonymize::mayShowDetails(DicomFile::parse(us.file(ExplicitLE))));
        QVERIFY(!Anonymize::mayShowDetails(a));

        // And from the command line, with what it did as JSON.
        QProcess *p = run({"anonymize", dir.path(), "--out", out.path(), "--json"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 0);
        const QJsonObject json = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
        QCOMPARE(json.value("written").toInt(), 2);
        QCOMPARE(json.value("folder").toString(), QFileInfo(out.filePath("Anonymous scans 2")).absoluteFilePath());
    }

    void anonymizesWhatImplicitFilesDontName() {
        // An implicit file says no value's kind, so a name, a UID, or a
        // sequence under a tag the registry doesn't have arrives as bytes.
        // None of the patient may leave in them.
        QTemporaryDir dir;
        Writer was, modified, original;
        was.explicitVr = modified.explicitVr = original.explicitVr = false;
        was.add(0x00100010, "PN", "Doe^Jane").add(0x00100020, "LO", "MRN42");
        modified.sequence(0x04000550, {was});
        // A sequence nobody registered, holding a name.
        Writer unlisted;
        unlisted.explicitVr = false;
        unlisted.add(0x00100010, "PN", "Poe^Jane");
        QVERIFY(Dictionary::vr(0x00082299).isEmpty());
        Writer w;
        w.explicitVr = false;
        w.add(0x00080016, "UI", "1.2.840.10008.5.1.4.1.1.2")
            .add(0x00080018, "UI", "1.2.7.1")
            .add(Tag::Modality, "CS", "CT")
            .add(0x0008009C, "PN", "House^Gregory")
            .sequence(0x00082299, {unlisted})
            .add(0x00100010, "PN", "Doe^Jane")
            .add(0x00100020, "LO", "MRN42")
            .add(0x00101005, "PN", "Roe^Jane")
            .add(0x00181002, "UI", "1.2.7.99")
            .add(0x00184000, "LT", "Mrs Doe moved")
            .add(0x0020000D, "UI", "1.2.7")
            .image(2, 2, 16, 12, false)
            .add(0x0020000E, "UI", "1.2.7.3")
            .sequence(0x04000561, {modified})
            .pixels(le16({5, 6, 7, 8}));
        QVERIFY(write(dir.filePath("IM1"), w.file("1.2.840.10008.1.2")));

        // The sequence reads as one, though nothing said it was.
        const DicomFile source = DicomFile::read(dir.filePath("IM1"));
        QVERIFY(source.isValid());
        QVERIFY(source.element(0x00082299));
        QCOMPARE(source.element(0x00082299)->items.size(), 1);

        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QTemporaryDir out;
        const Anonymize::Result result = Anonymize::copyScan(scan, out.path());
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n")));
        QCOMPARE(result.written, 1);
        const QStringList copies = listFiles({result.folder}, true);
        QCOMPARE(copies.size(), 1);
        const QByteArray bytes = bytesOf(copies.first());
        for (const char *theirs : {"Doe", "Roe", "Poe", "House", "MRN42", "1.2.7.99"})
            QVERIFY2(!bytes.contains(theirs), theirs);
        // It still reads, and shows the same picture.
        const DicomFile read = DicomFile::read(copies.first());
        QVERIFY(read.isValid());
        QCOMPARE(read.first(0x00100010), QString("Anonymous"));
        QVERIFY(read.first(0x00181002).startsWith("2.25."));
        QCOMPARE(decodeFrame(read, 0).mono, (QVector<quint16>{5, 6, 7, 8}));
        // The name in the unregistered sequence was found there and replaced.
        QCOMPARE(read.element(0x00082299)->items.size(), 1);
        QCOMPARE(valueOf(in(read.element(0x00082299)->items.first(), Tag::PatientName)), QString("Anonymous"));
    }

    void followsEveryActionOfTheTable() {
        // One attribute for each action code of PS3.15 Table E.1-1, and for
        // what goes whatever the table says.
        Writer institution = Writer::code("H1", "Saint Elsewhere");
        Writer study, image, context = Writer::code("C1", "Supine");
        study.add(0x00081150, "UI", "1.2.840.10008.3.1.2.3.1").add(0x00081155, "UI", "1.2.7");
        image.add(0x00081150, "UI", CtImage).add(0x00081155, "UI", "1.2.7.1");
        Writer observer;
        observer.add(0x0040A027, "LO", "Princeton Plainsboro")
            .add(0x0040A030, "DT", "20250101120000")
            .add(0x0040A075, "PN", "Wilson^James")
            .sequence(0x0040A088, {Writer::code("W1", "Oncologist")});
        Writer w;
        w.ul(0x00080000, 4)
            .add(Tag::SopClass, "UI", CtImage)
            .add(Tag::SopInstance, "UI", "1.2.7.2")
            .add(Tag::StudyDate, "DA", "20250101")                      // Z, kept by the dates option
            .add(Tag::AcquisitionDate, "DA", "20250102")                // X/Z, kept by the dates option
            .add(0x00080050, "SH", "ACC123")                            // Z
            .add(0x00080058, "UI", "1.2.7.50\\1.2.840.10008.5.1.4.1.1.2")   // U, of several values
            .add(Tag::Modality, "CS", "CT")                             // Not in the table
            .add(Tag::Institution, "LO", "Saint Elsewhere")             // X/Z/D
            .add(0x00080081, "ST", "1 Infinite Loop, Springfield")      // X
            .sequence(0x00080082, {institution})                        // X/Z/D, a sequence
            .add(0x00080090, "PN", "House^Gregory")                     // Z
            .add(0x00081010, "SH", "")                                  // X/Z/D, and already empty
            .add(0x00081070, "PN", "Chase^Robert")                      // X/Z/D
            .sequence(0x00081110, {study})                              // X/Z
            .sequence(Tag::ReferencedImage, {image})                    // X/Z/U*
            .add(0x00082111, "ST", "Mrs Doe's scan, reformatted")       // X
            .add(0x00090010, "LO", "VENDOR")                            // Private
            .add(0x00091001, "LO", "secret")
            .add(Tag::PatientName, "PN", "Doe^Jane")                    // Z
            .add(Tag::PatientId, "LO", "MRN42")                         // Z/D
            .add(Tag::PatientBirthDate, "DA", "19700101")               // Z
            .add(Tag::PatientSex, "CS", "F")                            // Z, kept by the characteristics option
            .add(Tag::PatientAge, "AS", "055Y")                         // X, kept by the option
            .add(0x00101020, "DS", "1.70")
            .add(0x00101030, "DS", "70")
            .add(0x00102110, "LO", "Penicillin, her husband Tom says")  // X, and the option says to clean
            .add(0x00102160, "SH", "Martian")                           // X, kept by the option
            .add(0x00120040, "LO", "SUBJ-0007")                         // D
            .add(0x00120063, "LO", "Some other method")                 // What an earlier anonymizing said
            .add(0x00142006, "PN", "Cuddy^Lisa")                        // A name the table doesn't list
            .add(0x00180010, "LO", "Iodine")                            // Z/D
            .add(0x00181000, "LO", "SN-998877")                         // X/Z/D
            .add(0x00181030, "LO", "Head routine")                      // X/D
            .add(0x0018700A, "SH", "DET-5")                             // X/D
            .add(Tag::StudyInstance, "UI", "1.2.7")                     // U
            .add(0x00200010, "SH", "S77")                               // Z
            .add(0x00200052, "UI", "1.2.7.9")                           // U
            .add(0x00209312, "UI", "1.2.7.10");                         // A UID the table doesn't list
        module(w, "1.2.7.3", 2, 2, 16, 12, false);
        w.sequence(0x00400555, {context})                               // X/Z
            .sequence(0x0040A073, {observer})                           // D, a sequence
            .add(0x0040A123, "PN", "Foreman^Eric")                      // D
            .add(0x00700084, "PN", "Cameron^Allison")                   // Z/D
            .add(0x04000565, "CS", "COERCE")                            // D
            .add(0x21000140, "AE", "PACS_MAIN")                         // D
            .add(0x50103000, "OW", QByteArray(4, 'c'))                  // A curve
            .us(Tag::OverlayRows, 2)                                    // An overlay plane
            .add(Tag::OverlayData, "OW", QByteArray(2, 'o'))
            .pixels(le16({1, 2, 3, 4}));

        Anonymize::Session session;
        const Made made = anonymized(w.file(ExplicitLE), {}, {}, &session);
        QVERIFY2(made.written, qPrintable(made.error));
        const DicomFile &c = made.copy;
        const Anonymize::Changes &did = made.outcome.changes;

        // X: gone.
        for (const quint32 tag : {0x00080081u, 0x00082111u, 0x00102110u})
            QVERIFY2(!c.has(tag) && did.removed.contains(tag), qPrintable(tagText(tag)));
        // Z: there, and empty; a sequence without items.
        for (const quint32 tag : {0x00080050u, 0x00080090u, quint32(Tag::PatientBirthDate), 0x00200010u}) {
            QVERIFY2(c.has(tag) && c.element(tag)->value.isEmpty() && did.emptied.contains(tag), qPrintable(tagText(tag)));
        }
        // X/Z: as Z, which is valid whether or not the IOD wants the attribute.
        for (const quint32 tag : {0x00081110u, 0x00400555u}) {
            QVERIFY2(c.has(tag) && c.element(tag)->items.isEmpty() && did.emptied.contains(tag), qPrintable(tagText(tag)));
            QCOMPARE(c.element(tag)->vr, QByteArray("SQ"));
        }
        // D, and X/D, Z/D, X/Z/D: a dummy value of the attribute's own kind.
        QCOMPARE(c.first(0x00120040), QString("Anonymized"));
        QCOMPARE(c.first(0x0040A123), QString("Anonymous"));
        QCOMPARE(c.first(0x04000565), QString("ANONYMIZED"));
        QCOMPARE(c.first(0x21000140), QString("ANONYMIZED"));
        QCOMPARE(c.first(Tag::Institution), QString("Anonymized"));
        QCOMPARE(c.first(0x00081070), QString("Anonymous"));
        QCOMPARE(c.first(0x00180010), QString("Anonymized"));
        QCOMPARE(c.first(0x00181000), QString("Anonymized"));
        QCOMPARE(c.first(0x00181030), QString("Anonymized"));
        QCOMPARE(c.first(0x0018700A), QString("Anonymized"));
        QCOMPARE(c.first(0x00700084), QString("Anonymous"));
        for (const quint32 tag : {0x00120040u, 0x0040A123u, 0x04000565u, 0x21000140u, quint32(Tag::Institution), 0x00081070u,
                                  0x00180010u, 0x00181000u, 0x00181030u, 0x0018700Au, 0x00700084u, 0x00080082u, 0x0040A073u})
            QVERIFY2(did.replaced.contains(tag), qPrintable(tagText(tag)));
        // What was empty had nothing to replace.
        QVERIFY(c.has(0x00081010) && c.element(0x00081010)->value.isEmpty());
        QVERIFY(!did.replaced.contains(0x00081010) && !did.emptied.contains(0x00081010));
        // For a sequence, D is for all it holds: the items keep their shape.
        const DicomElement *codes = c.element(0x00080082);
        QVERIFY(codes && codes->items.size() == 1 && codes->items.first().size() == 3);
        for (const auto &e : codes->items.first())
            QCOMPARE(valueOf(&e), QString("Anonymized"));
        const DicomElement *observers = c.element(0x0040A073);
        QVERIFY(observers && observers->items.size() == 1);
        QCOMPARE(valueOf(in(observers->items.first(), 0x0040A027)), QString("Anonymized"));
        QCOMPARE(valueOf(in(observers->items.first(), 0x0040A030)), QString("19000101000000"));
        QCOMPARE(valueOf(in(observers->items.first(), 0x0040A075)), QString("Anonymous"));
        QCOMPARE(in(observers->items.first(), 0x0040A088)->items.size(), 1);
        QCOMPARE(valueOf(in(in(observers->items.first(), 0x0040A088)->items.first(), Tag::CodeMeaning)), QString("Anonymized"));
        // Z for the patient's name is a name that says what the copy is,
        // and the ID a stand-in.
        QCOMPARE(c.first(Tag::PatientName), QString("Anonymous"));
        QCOMPARE(c.first(Tag::PatientId), QString("ANON1"));
        QVERIFY(did.replaced.contains(Tag::PatientName) && did.replaced.contains(Tag::PatientId));
        // K, by the two options: dates, and what the patient is like.
        QCOMPARE(c.first(Tag::StudyDate), QString("20250101"));
        QCOMPARE(c.first(Tag::AcquisitionDate), QString("20250102"));
        QCOMPARE(c.first(Tag::PatientSex), QString("F"));
        QCOMPARE(c.first(Tag::PatientAge), QString("055Y"));
        QCOMPARE(c.first(0x00101020), QString("1.70"));
        QCOMPARE(c.first(0x00101030), QString("70"));
        QCOMPARE(c.first(0x00102160), QString("Martian"));
        for (const quint32 tag : {quint32(Tag::StudyDate), quint32(Tag::AcquisitionDate), quint32(Tag::PatientSex),
                                  quint32(Tag::PatientAge), 0x00101020u, 0x00101030u, 0x00102160u})
            QVERIFY2(did.kept.contains(tag), qPrintable(tagText(tag)));
        // What the table doesn't name is neither: it stays, and isn't listed.
        QCOMPARE(c.first(Tag::Modality), QString("CT"));
        QVERIFY(!did.kept.contains(Tag::Modality));
        // U: new UIDs, each value of several, and the standard's own left.
        for (const quint32 tag : {quint32(Tag::SopInstance), quint32(Tag::StudyInstance), quint32(Tag::SeriesInstance),
                                  0x00200052u, 0x00209312u}) {
            QVERIFY2(c.first(tag).startsWith("2.25.") && did.replaced.contains(tag), qPrintable(tagText(tag)));
        }
        const QStringList failed = c.string(0x00080058).split('\\');
        QCOMPARE(failed.size(), 2);
        QVERIFY(failed[0].startsWith("2.25."));
        QCOMPARE(failed[1], QString(CtImage));
        QCOMPARE(c.first(Tag::SopClass), QString(CtImage));
        QVERIFY(!did.replaced.contains(Tag::SopClass));
        // X/Z/U*: the sequence stays, for the UIDs in it, which are new.
        const DicomElement *references = c.element(Tag::ReferencedImage);
        QVERIFY(references && references->items.size() == 1);
        QCOMPARE(valueOf(in(references->items.first(), 0x00081150)), QString(CtImage));
        QVERIFY(valueOf(in(references->items.first(), 0x00081155)).startsWith("2.25."));
        QVERIFY(did.replaced.contains(Tag::ReferencedImage));
        // Whatever the table says: private attributes, curves, overlay
        // planes, group lengths, and a name the table doesn't list.
        for (const quint32 tag : {0x00080000u, 0x00090010u, 0x00091001u, 0x00142006u, 0x50103000u, quint32(Tag::OverlayRows),
                                  quint32(Tag::OverlayData)})
            QVERIFY2(!c.has(tag), qPrintable(tagText(tag)));
        QVERIFY(did.removed.contains(Anonymize::Family::Private) && did.removed.contains(Anonymize::Family::Curves)
                && did.removed.contains(Anonymize::Family::Overlays) && did.removed.contains(0x00142006));
        // The copy says what it is, in place of what an earlier one said.
        QCOMPARE(c.first(0x00120062), QString("YES"));
        QCOMPARE(c.string(0x00120063).split('\\'), Anonymize::method());
        QCOMPARE(c.first(0x00280303), QString("UNMODIFIED"));
        // The file's meta group is the copy's own.
        QCOMPARE(c.first(0x00020003), c.first(Tag::SopInstance));
        QCOMPARE(c.first(0x00020013), QString("DICOLENS"));
        QCOMPARE(decodeFrame(c, 0).mono, (QVector<quint16>{1, 2, 3, 4}));

        // The same UIDs and the same patient get the same stand-ins in the
        // next file, written implicitly; another patient gets another.
        Writer next;
        next.explicitVr = false;
        next.add(Tag::SopClass, "UI", CtImage).add(Tag::SopInstance, "UI", "1.2.7.1").add(Tag::PatientId, "LO", "MRN42")
            .add(Tag::StudyInstance, "UI", "1.2.7");
        const Made second = anonymized(next.file(ImplicitLE), {}, {}, &session);
        QVERIFY2(second.written, qPrintable(second.error));
        QCOMPARE(second.copy.first(Tag::StudyInstance), c.first(Tag::StudyInstance));
        QCOMPARE(second.copy.first(Tag::SopInstance), valueOf(in(references->items.first(), 0x00081155)));
        QCOMPARE(second.copy.first(Tag::PatientId), QString("ANON1"));
        Writer other;
        other.add(Tag::SopClass, "UI", CtImage).add(Tag::SopInstance, "UI", "1.2.8.1").add(Tag::PatientId, "LO", "MRN43");
        QCOMPARE(anonymized(other.file(ExplicitLE), {}, {}, &session).copy.first(Tag::PatientId), QString("ANON2"));
    }

    void cleansInsideSequences_data() {
        QTest::addColumn<bool>("explicitVr");
        QTest::newRow("explicit") << true;
        QTest::newRow("implicit") << false;
    }

    void cleansInsideSequences() {
        QFETCH(bool, explicitVr);
        // The profile applies wherever an attribute is: three sequences
        // down, in a frame's functional groups, in a sequence nobody
        // registered.
        auto writer = [explicitVr] {
            Writer w;
            w.explicitVr = explicitVr;
            return w;
        };
        Writer instance = writer(), series = writer(), frame = writer(), groups = writer(), unlisted = writer();
        instance.add(0x00081150, "UI", CtImage)
            .add(0x00081155, "UI", "1.2.7.44")
            .add(Tag::SeriesDescription, "LO", "Doe series")
            .add(0x00090010, "LO", "VENDOR")
            .add(0x00091001, "LO", "Doe")
            .add(Tag::PatientName, "PN", "Doe^Jane")
            .add(0x00209158, "LT", "Jane moved");
        series.sequence(0x00081199, {instance}).add(Tag::SeriesInstance, "UI", "1.2.7.3");
        frame.add(0x00189074, "DT", "20250101101010").add(0x00209158, "LT", "Mrs Doe coughed");
        groups.sequence(0x00209111, {frame});
        unlisted.sequence(Tag::ReferencedSeries, {series}).add(0x0040A123, "PN", "Roe^Richard");
        Writer w = writer();
        w.add(Tag::SopClass, "UI", CtImage)
            .add(Tag::SopInstance, "UI", "1.2.7.2")
            .sequence(Tag::ReferencedSeries, {series})
            .sequence(0x00082299, {unlisted})
            .add(Tag::PatientName, "PN", "Doe^Jane");
        module(w, "1.2.7.3", 2, 2, 16, 12, false);
        w.sequence(Tag::SharedGroups, {groups}).pixels(le16({1, 2, 3, 4}));

        const Made made = anonymized(w.file(explicitVr ? ExplicitLE : ImplicitLE));
        QVERIFY2(made.written, qPrintable(made.error));
        QCOMPARE(made.copy.transferSyntax(), explicitVr ? ExplicitLE : ImplicitLE);
        const QByteArray bytes = bytesOf(made.path);
        for (const char *theirs : {"Doe", "Jane", "Roe", "Richard", "VENDOR", "1.2.7.44", "1.2.7.3", "coughed"})
            QVERIFY2(!bytes.contains(theirs), theirs);

        auto check = [&](const DicomElement *referenced) {
            QVERIFY(referenced && referenced->items.size() == 1);
            const QList<DicomElement> &s = referenced->items.first();
            QCOMPARE(valueOf(in(s, Tag::SeriesInstance)), made.copy.first(Tag::SeriesInstance));
            QVERIFY(in(s, 0x00081199) && in(s, 0x00081199)->items.size() == 1);
            const QList<DicomElement> &i = in(s, 0x00081199)->items.first();
            QCOMPARE(valueOf(in(i, 0x00081150)), QString(CtImage));
            QVERIFY(valueOf(in(i, 0x00081155)).startsWith("2.25."));
            QCOMPARE(valueOf(in(i, Tag::PatientName)), QString("Anonymous"));
            QVERIFY(!in(i, Tag::SeriesDescription) && !in(i, 0x00090010) && !in(i, 0x00091001) && !in(i, 0x00209158));
        };
        check(made.copy.element(Tag::ReferencedSeries));
        const DicomElement *kept = made.copy.element(0x00082299);
        QVERIFY(kept && kept->items.size() == 1);
        check(in(kept->items.first(), Tag::ReferencedSeries));
        QCOMPARE(valueOf(in(kept->items.first(), 0x0040A123)), QString("Anonymous"));
        // A frame's own date stays, by the dates option; its comment doesn't.
        const DicomElement *shared = made.copy.element(Tag::SharedGroups);
        QVERIFY(shared && shared->items.size() == 1);
        const DicomElement *content = in(shared->items.first(), 0x00209111);
        QVERIFY(content && content->items.size() == 1);
        QCOMPARE(valueOf(in(content->items.first(), 0x00189074)), QString("20250101101010"));
        QVERIFY(!in(content->items.first(), 0x00209158));
        QVERIFY(made.outcome.changes.kept.contains(0x00189074));
        QVERIFY(made.outcome.changes.removed.contains(0x00209158));
        QCOMPARE(decodeFrame(made.copy, 0).mono, (QVector<quint16>{1, 2, 3, 4}));
    }

    void survivesHostileFiles() {
        // A file that lies about a value's kind: a name written as a long
        // string, a UID as a short one, and a name under a tag of nobody's.
        Writer w;
        w.add(Tag::SopClass, "UI", CtImage)
            .add(Tag::SopInstance, "SH", "1.2.7.2")
            .add(0x00081050, "LO", "Kutner^Lawrence")
            .add(0x00082299, "PN", "Taub^Chris")
            .add(0x00082297, "UI", "1.2.7.77");
        // A sequence written as bytes of no kind, which nothing can read
        // into: it's dropped rather than trusted.
        Writer hidden;
        hidden.explicitVr = false;
        hidden.add(Tag::PatientName, "PN", "Doe^Jane");
        QByteArray unread;
        w.u16(unread, 0xFFFE);
        w.u16(unread, 0xE000);
        w.u32(unread, quint32(hidden.body.size() + 2));   // A length that's wrong, so it doesn't read as items.
        unread += hidden.body;
        w.add(0x00082295, "OB", unread);
        const DicomFile source = onDisk(w.file(ExplicitLE));
        QVERIFY(source.isValid());
        QVERIFY(source.element(0x00082295)->items.isEmpty());

        const Made made = anonymized(w.file(ExplicitLE));
        QVERIFY2(made.written, qPrintable(made.error));
        const QByteArray bytes = bytesOf(made.path);
        for (const char *theirs : {"Kutner", "Taub", "Doe", "1.2.7.2", "1.2.7.77"})
            QVERIFY2(!bytes.contains(theirs), theirs);
        QVERIFY(made.copy.first(Tag::SopInstance).startsWith("2.25."));
        QVERIFY(made.copy.first(0x00082297).startsWith("2.25."));
        QVERIFY(!made.copy.has(0x00081050) && !made.copy.has(0x00082299) && !made.copy.has(0x00082295));
        QVERIFY(made.outcome.changes.removed.contains(0x00082295));

        // A deflated file is copied inflated; a big-endian one isn't copied.
        QString error;
        Anonymize::Session session;
        Writer big;
        big.bigEndian = true;
        big.add(Tag::PatientName, "PN", "Doe^Jane");
        QVERIFY(!Anonymize::write(onDisk(big.file("1.2.840.10008.1.2.2")), fresh(".dcm"), session, &error));
        QCOMPARE(error, QString("Big-endian files can't be copied"));
        QVERIFY(!Anonymize::write(DicomFile(), fresh(".dcm"), session, &error));
        QCOMPARE(error, QString("This file can't be read"));

        // A copy of a copy passes: the stand-ins aren't what's looked for,
        // and they're stood in for again without complaint.
        const Made again = anonymized(bytesOf(anonymized(janeDoe(true, "1.2.7.1").pixels(le16({1, 2, 3, 4})).file(ExplicitLE)).path));
        QVERIFY2(again.written, qPrintable(again.error));
        QCOMPARE(again.copy.first(Tag::PatientName), QString("Anonymous"));
        QCOMPARE(again.copy.first(Tag::PatientId), QString("ANON1"));

        // Cancelled before it starts, nothing is copied.
        QTemporaryDir dir, out;
        QVERIFY(write(dir.filePath("IM1"), janeDoe(true, "1.2.7.1").pixels(le16({1, 2, 3, 4})).file(ExplicitLE)));
        std::atomic<bool> cancel = true;
        QCOMPARE(Anonymize::copyScan(scanFiles(listFiles({dir.path()}, true)), out.path(), &cancel).written, 0);
    }

    void keepsNothingOfDamagedFiles_data() {
        QTest::addColumn<bool>("explicitVr");
        QTest::newRow("explicit") << true;
        QTest::newRow("implicit") << false;
    }

    void keepsNothingOfDamagedFiles() {
        QFETCH(bool, explicitVr);
        // A file with a byte or two wrong, or cut short, reads as something
        // nobody wrote: lengths that swallow the next element, names under
        // other tags, values of other kinds. Whatever it reads as, copying
        // it must neither fall over nor let the patient through. The other
        // files of a scan say who the patient is, as the intact one does here.
        auto writer = [explicitVr] {
            Writer w;
            w.explicitVr = explicitVr;
            return w;
        };
        Writer reference = writer(), request = writer();
        reference.add(0x00081150, "UI", CtImage).add(0x00081155, "UI", "1.2.7.44");
        request.add(0x00321032, "PN", "Zimmermann^Bartholomew").add(0x00400009, "SH", "STEP-778899");
        Writer w = writer();
        w.add(Tag::SopClass, "UI", CtImage)
            .add(Tag::SopInstance, "UI", "1.2.7.1")
            .add(Tag::StudyDate, "DA", "20250101")
            .add(0x00080050, "SH", "ACC-998877")
            .add(Tag::Modality, "CS", "CT")
            .add(Tag::Institution, "LO", "Saint Elsewhere")
            .add(0x00080090, "PN", "Hernandez^Guillermo")
            .add(Tag::SeriesDescription, "LO", "Axial 5 mm")
            .sequence(Tag::ReferencedImage, {reference})
            .add(0x00090010, "LO", "VENDOR")
            .add(0x00091001, "LO", "Montgomery")
            .add(Tag::PatientName, "PN", "Montgomery^Alexandra")
            .add(Tag::PatientId, "LO", "MRN-424242")
            .add(Tag::PatientBirthDate, "DA", "19700101")
            .add(Tag::StudyInstance, "UI", "1.2.7");
        module(w, "1.2.7.3", 2, 2, 16, 12, false);
        w.sequence(0x00400275, {request}).pixels(le16({1, 2, 3, 4}));
        const QByteArray intact = w.file(explicitVr ? ExplicitLE : ImplicitLE);
        Anonymize::Session learned;
        Anonymize::learn(DicomFile::parse(intact), learned);
        const QList<QByteArray> theirs{"montgomery", "alexandra", "hernandez", "guillermo", "zimmermann", "bartholomew",
                                       "mrn-424242", "acc-998877", "step-778899", "saint elsewhere"};

        quint32 state = explicitVr ? 12345 : 67890;
        auto random = [&state](int below) {
            state = state * 1664525u + 1013904223u;
            return int((state >> 8) % quint32(below));
        };
        Anonymize::Options keep;
        keep.keepDescriptions = true;
        const QString source = fresh(".src"), path = fresh(".dcm");
        int copied = 0, refused = 0;
        for (int round = 0; round < 3000; ++round) {
            QByteArray damaged = intact;
            for (int flips = 1 + random(3); flips > 0; --flips)
                damaged[132 + random(int(damaged.size()) - 132)] = char(random(256));
            if (random(8) == 0)
                damaged.truncate(140 + random(int(damaged.size()) - 140));
            QVERIFY(write(source, damaged));
            const DicomFile file = DicomFile::read(source);
            if (!file.isValid())
                continue;
            Anonymize::Session session = learned;
            QString error;
            const bool planned = Anonymize::check(file, session, &error, keep, round % 3 ? QList<QRect>() : QList<QRect>{QRect(0, 0, 1, 1)});
            Q_UNUSED(planned);
            session = learned;
            if (!Anonymize::write(file, path, session, &error, keep)) {
                ++refused;
                QVERIFY(!QFileInfo::exists(path) && !QFileInfo::exists(path + ".unchecked"));
                continue;
            }
            ++copied;
            const QByteArray bytes = bytesOf(path).toLower();
            for (const QByteArray &name : theirs)
                QVERIFY2(!bytes.contains(name), qPrintable(QStringLiteral("round %1: %2").arg(round).arg(QString::fromLatin1(name))));
            QVERIFY(DicomFile::read(path).isValid());
        }
        // Most damage leaves a file that can be copied; some of it moves a
        // name to where only the check afterwards finds it.
        QVERIFY2(copied > 1000 && refused > 10, qPrintable(QStringLiteral("%1 copied, %2 refused").arg(copied).arg(refused)));
    }

    void clearsAnOverlayKeptInThePixels() {
        // Files once kept an overlay plane in the bits of each pixel the
        // picture didn't use. Taking away what describes the plane leaves
        // the bits; drawing the picture again from its own twelve doesn't.
        Writer w = janeDoe(true, "1.2.7.1");
        w.us(Tag::OverlayRows, 2).us(Tag::OverlayColumns, 2).us(Tag::OverlayBitsAllocated, 16).us(0x60000102, 12)
            .pixels(le16({0x1001, 0x0002, 0x1003, 0x0004}));
        const Made made = anonymized(w.file(ExplicitLE));
        QVERIFY2(made.written, qPrintable(made.error));
        QCOMPARE(made.copy.readPixels(0, made.copy.pixelLength()), le16({1, 2, 3, 4}));
        QCOMPARE(decodeFrame(made.copy, 0).mono, (QVector<quint16>{1, 2, 3, 4}));
        QVERIFY(!made.copy.has(Tag::OverlayRows) && !made.copy.has(0x60000102));
        QVERIFY(made.outcome.changes.removed.contains(Anonymize::Family::Overlays));
        // Nothing was blacked out, and the copy doesn't say anything was.
        QVERIFY(!made.outcome.blackedOut);
        QVERIFY(!made.copy.string(0x00120063).contains("blacked out"));

        // A plane with data of its own leaves the pixels alone, compressed
        // as they came.
        QVector<quint16> samples{100, 200, 300, 400};
        Writer plane = janeDoe(true, "1.2.7.1");
        plane.us(Tag::OverlayRows, 2).us(Tag::OverlayColumns, 2).us(Tag::OverlayBitsAllocated, 1).us(0x60000102, 0)
            .add(Tag::OverlayData, "OW", QByteArray(2, '\x0F'))
            .encapsulated({losslessJpeg(samples, 2, 2, 1, 12, 1, 0)});
        const Made kept = anonymized(plane.file("1.2.840.10008.1.2.4.70"));
        QVERIFY2(kept.written, qPrintable(kept.error));
        QCOMPARE(kept.copy.transferSyntax(), QString("1.2.840.10008.1.2.4.70"));
        QVERIFY(!kept.copy.has(Tag::OverlayData) && !kept.copy.has(Tag::OverlayRows));

        // Where the picture can't be drawn again, the file isn't copied.
        Writer wide;
        wide.add(Tag::SopClass, "UI", CtImage).image(1, 2, 32, 32, false).us(Tag::OverlayBitsAllocated, 32).us(0x60000102, 31)
            .pixels(QByteArray(8, 1));
        const Made refused = anonymized(wide.file(ExplicitLE));
        QVERIFY(!refused.written);
        QCOMPARE(refused.error, QString("This image keeps an overlay in its pixels, which can't be taken out: "
                                        "its pixels are 32-bit, which aren't kept value for value"));
    }

    void doesNotMistakeItsOwnWords() {
        // What a copy says of itself has words a patient can be called by
        // and codes an accession number can be; they're Dicolens' own.
        Writer w = janeDoe(true, "1.2.7.1");
        w.body.replace("Doe^Jane", "Free^Tex");
        w.body.replace("ACC123", "113100");
        w.body.replace("Saint Elsewhere", "UIDs replaced  ");
        const Made made = anonymized(w.pixels(le16({1, 2, 3, 4})).file(ExplicitLE));
        QVERIFY2(made.written, qPrintable(made.error));
        QVERIFY(made.copy.string(0x00120063).contains("free text"));
        QVERIFY(made.copy.string(0x00120063).contains("UIDs replaced"));
        const DicomElement *codes = made.copy.element(0x00120064);
        QVERIFY(codes && valueOf(in(codes->items.first(), 0x00080100)) == "113100");
    }

    void removesDescriptionsUnlessAsked() {
        Writer w = janeDoe(true, "1.2.7.1");
        w.add(0x00321060, "LO", "CT of the head").add(0x00400254, "LO", "Head without contrast").pixels(le16({1, 2, 3, 4}));

        // As the Basic Profile says: descriptions are what people typed.
        const Made strict = anonymized(w.file(ExplicitLE));
        QVERIFY2(strict.written, qPrintable(strict.error));
        QVERIFY(!strict.copy.has(Tag::StudyDescription) && !strict.copy.has(Tag::SeriesDescription));
        QCOMPARE(strict.copy.first(0x00181030), QString("Anonymized"));
        QVERIFY(strict.copy.has(0x00321060) && strict.copy.first(0x00321060).isEmpty());   // X/Z
        QVERIFY(!strict.copy.has(0x00400254));
        QStringList said = strict.copy.string(0x00120063).split('\\');
        QCOMPARE(said, Anonymize::method());
        QVERIFY(said.first().endsWith("Basic Application Confidentiality Profile"));
        QVERIFY(said.first().contains(Anonymize::edition()));
        auto codes = [](const DicomFile &file) {
            QStringList values;
            for (const auto &item : file.element(0x00120064)->items)
                values << valueOf(in(item, 0x00080100));
            return values;
        };
        QCOMPARE(codes(strict.copy), (QStringList{"113100", "113106", "113108"}));

        // Asked to, the three that tell series apart stay as they are, and
        // no others; and the copy says so, claiming neither the profile
        // nor that anything cleaned them.
        Anonymize::Options options;
        options.keepDescriptions = true;
        const Made kept = anonymized(w.file(ExplicitLE), options);
        QVERIFY2(kept.written, qPrintable(kept.error));
        QCOMPARE(kept.copy.first(Tag::StudyDescription), QString("Head CT"));
        QCOMPARE(kept.copy.first(Tag::SeriesDescription), QString("Axial 5 mm"));
        QCOMPARE(kept.copy.first(0x00181030), QString("Head routine"));
        QVERIFY(kept.copy.first(0x00321060).isEmpty() && !kept.copy.has(0x00400254));
        for (const quint32 tag : {quint32(Tag::StudyDescription), quint32(Tag::SeriesDescription), 0x00181030u})
            QVERIFY(kept.outcome.changes.kept.contains(tag) && strict.outcome.changes.removed.contains(tag) != (tag == 0x00181030u));
        said = kept.copy.string(0x00120063).split('\\');
        QCOMPARE(said, Anonymize::method(options));
        QVERIFY(said.contains("Study Description, Series Description, Protocol Name kept as is"));
        for (const QString &value : said) {
            QVERIFY2(value.size() <= 64, qPrintable(value));
            QVERIFY2(!value.contains("Confidentiality Profile") && !value.contains("Clean"), qPrintable(value));
        }
        QCOMPARE(codes(kept.copy), (QStringList{"113106", "113108"}));
        QCOMPARE(kept.copy.first(0x00120062), QString("YES"));
        for (const QString &value : Anonymize::method())
            QVERIFY2(value.size() <= 64, qPrintable(value));
    }

    void learnsWhatIdentifies() {
        Writer other;
        other.add(Tag::PatientId, "LO", "ALT-5550");
        Writer w;
        w.add(Tag::Modality, "CS", "MR")
            .add(0x00080050, "SH", "1024")
            .add(Tag::Manufacturer, "LO", "ACME")
            .add(Tag::Institution, "LO", "Saint  Elsewhere")
            .add(0x00080081, "ST", "1 Infinite Loop, Springfield")
            .add(0x00081010, "SH", "Aquilion")
            .add(0x00081040, "LO", "MR")
            .add(0x00081070, "PN", "MR")
            .add(Tag::ManufacturerModel, "LO", "Aquilion ONE")
            .add(Tag::PatientName, "PN", "de la Cruz^Lee Ann^Q^Dr^Jr=Ideo^Graph")
            .add(Tag::PatientId, "LO", "42")
            .add(Tag::PatientBirthDate, "DA", "19700102")
            .add(0x00101001, "PN", "Colon^Head\\O'Brien-Smith")
            .sequence(0x00101002, {other})
            .add(0x00102154, "SH", "+1 (555) 123-4567")
            .add(0x00181000, "LO", "SN-998877")
            .add(Tag::StudyInstance, "UI", "1.2.7")
            .add(Tag::SopClass, "UI", CtImage);
        Anonymize::Session session;
        Anonymize::learn(DicomFile::parse(w.file(ExplicitLE)), session);
        QHash<QString, Anonymize::Identifier> learned;
        for (const auto &id : std::as_const(session.identifiers))
            learned.insert(id.text + (id.match == Anonymize::Identifier::Match::Squeezed ? "/squeezed" : ""), id);
        using Match = Anonymize::Identifier::Match;
        using Kind = Anonymize::Identifier::Kind;

        // Every part of every name, whole and word by word; a short one
        // only ever as a word of its own.
        for (const char *name : {"de la cruz", "cruz", "lee ann", "lee", "ann", "ideo", "graph", "colon", "head",
                                 "o'brien-smith", "o'brien", "smith"}) {
            QVERIFY2(learned.contains(name), name);
            QCOMPARE(learned[name].kind, Kind::Name);
            QCOMPARE(learned[name].match, qstrlen(name) <= 4 ? Match::Word : Match::Anywhere);
        }
        QCOMPARE(learned["lee"].found, QString("Lee"));
        // Not the small words of a name, initials, titles, or suffixes;
        // nor what only says a kind of exam or department.
        for (const char *none : {"de", "la", "q", "dr", "jr", "mr", "42", "aquilion", "acme"})
            QVERIFY2(!learned.contains(none), none);
        // IDs, at any depth; a number where no digit continues it.
        QCOMPARE(learned["1024"].match, Match::Number);
        QCOMPARE(learned["1024"].kind, Kind::Id);
        QCOMPARE(learned["alt-5550"].kind, Kind::Id);
        // The birth date as it's written here and elsewhere.
        for (const char *date : {"19700102", "1970-01-02", "1970.01.02", "02.01.1970", "02/01/1970", "01/02/1970"}) {
            QVERIFY2(learned.contains(date), date);
            QCOMPARE(learned[date].kind, Kind::BirthDate);
        }
        // The institution whole, an address also line by line, a phone
        // number also by its digits alone, and a serial number.
        QCOMPARE(learned["saint elsewhere"].kind, Kind::Place);
        QVERIFY(learned.contains("1 infinite loop, springfield") && learned.contains("1 infinite loop") && learned.contains("springfield"));
        QCOMPARE(learned["+1 (555) 123-4567"].kind, Kind::Phone);
        QCOMPARE(learned["15551234567/squeezed"].match, Match::Squeezed);
        QCOMPARE(learned["sn-998877"].kind, Kind::Device);
        // And the UIDs that are this patient's, not the standard's.
        QCOMPARE(session.sourceUids, (QSet<QString>{"1.2.7"}));

        // Learning the same file again adds nothing.
        const qsizetype count = session.identifiers.size();
        Anonymize::learn(DicomFile::parse(w.file(ExplicitLE)), session);
        QCOMPARE(session.identifiers.size(), count);
    }

    void findsWhatACopyStillHolds_data() {
        QTest::addColumn<quint32>("tag");
        QTest::addColumn<QByteArray>("vr");
        QTest::addColumn<QByteArray>("value");
        QTest::addColumn<QString>("found");   // Empty when nothing is to be found.

        auto row = [](const char *name, quint32 tag, const char *vr, const QByteArray &value, const char *found) {
            QTest::newRow(name) << tag << QByteArray(vr) << value << QString::fromUtf8(found);
        };
        const quint32 Description = Tag::SeriesDescription, Unregistered = 0x00082299;
        // Found.
        row("a name in a description", Description, "LO", "for LEE, see prior", "Lee");
        row("a name at the end", Description, "LO", "Follow-up of Cruz", "Cruz");
        row("a whole name", Description, "LO", "DE LA CRUZ follow-up", "de la Cruz");
        row("a name that is a body part, in text", Description, "LO", "Colon study", "Colon");
        row("an ID after letters", Description, "LO", "ID1024", "1024");
        row("an ID as a word", Description, "LO", "ref 1024.", "1024");
        row("a birth date with dashes", Description, "LO", "born 1970-01-02", "1970-01-02");
        row("a birth date day first", 0x00204000, "LT", "geb. 02.01.1970", "02.01.1970");
        row("a birth date in a date of its own", Tag::PatientBirthDate, "DA", "19700102", "19700102");
        row("a birth date in a date and time", Unregistered, "DT", "19700102103000", "19700102");
        row("a phone number written otherwise", Description, "LO", "tel 1-555-123-4567", "+1 (555) 123-4567");
        row("an institution", Description, "LO", "SAINT ELSEWHERE CT1", "Saint Elsewhere");
        row("a town of the address", 0x00081030, "LO", "Springfield protocol", "Springfield");
        row("an ID in a coded value", 0x00180015, "CS", "REF 1024", "1024");
        row("a serial number in what's written as a code", 0x00180015, "CS", "SN-998877", "SN-998877");
        row("a name in what's written as a code and isn't one", 0x00180015, "CS", "Colon, sigmoid", "Colon");
        row("a name in what's written as a number", Tag::SliceThickness, "DS", "5 (Cruz)", "Cruz");
        row("an ID in what's written as a number", Tag::SliceThickness, "DS", "5 mm, ID 1024", "1024");
        row("a name in what's written as a UID", 0x00200052, "UI", "1.2.840.10008.Cruz", "Cruz");
        row("a name in bytes that are text", Unregistered, "UN", "Lee", "Lee");
        row("a long name in bytes that aren't", 0x00282000, "OB", QByteArray("\x00\x01 Saint Elsewhere \x02\x00", 20), "Saint Elsewhere");
        row("one of the original's UIDs", 0x00200052, "UI", "1.2.7", "1.2.7");
        // Not found.
        row("a name that is a body part, as a code", 0x00180015, "CS", "COLON", "");
        row("another body part", 0x00180015, "CS", "HEAD", "");
        row("a short name inside a word", Description, "LO", "SLEEP STUDY, sleeve, Annual", "");
        row("the modality in a software version", 0x00181020, "LO", "syngo MR E11", "");
        row("an ID among a matrix's numbers", Tag::Rows, "US", QByteArray("\x00\x04", 2), "");
        row("an ID in a decimal string", Tag::SliceThickness, "DS", "1024", "");
        row("an ID in an integer string", Tag::InstanceNumber, "IS", "1024", "");
        row("an ID inside a longer number", Description, "LO", "A10245 and 21024", "");
        row("the small words of a name", Description, "LO", "IRM de la jambe", "");
        row("a study on the birthday", Tag::StudyDate, "DA", "19700102", "");
        row("a study on the birthday, as older files write it", Tag::StudyDate, "DA", "1970.01.02", "");
        row("a time that reads like the ID", Tag::StudyTime, "TM", "1024", "");
        row("a patient ID too short to mean anything", Description, "LO", "42 slices", "");
        row("the model a station is named after", Tag::ManufacturerModel, "LO", "Aquilion ONE", "");
        row("a short name in bytes that aren't text", 0x00283006, "OW", QByteArray("\x00\x01 Lee \x02\x00 Ann", 12), "");
        row("the standard's own UID", Tag::SopClass, "UI", CtImage, "");
        row("a stand-in", Tag::PatientName, "PN", "Anonymous", "");
    }

    void findsWhatACopyStillHolds() {
        QFETCH(quint32, tag);
        QFETCH(QByteArray, vr);
        QFETCH(QByteArray, value);
        QFETCH(QString, found);

        Writer source;
        source.add(Tag::Modality, "CS", "MR")
            .add(0x00080050, "SH", "1024")
            .add(Tag::Institution, "LO", "Saint Elsewhere")
            .add(0x00080081, "ST", "1 Infinite Loop, Springfield")
            .add(0x00081010, "SH", "Aquilion")
            .add(0x00081070, "PN", "MR")
            .add(Tag::ManufacturerModel, "LO", "Aquilion ONE")
            .add(Tag::PatientName, "PN", "de la Cruz^Lee Ann")
            .add(Tag::PatientId, "LO", "42")
            .add(Tag::PatientBirthDate, "DA", "19700102")
            .add(0x00101001, "PN", "Colon^Head")
            .add(0x00102154, "SH", "+1 (555) 123-4567")
            .add(0x00181000, "LO", "SN-998877")
            .add(Tag::StudyInstance, "UI", "1.2.7");
        Anonymize::Session session;
        Anonymize::learn(DicomFile::parse(source.file(ExplicitLE)), session);

        // The value alone, and the same three sequences down.
        Writer copy, item, outer;
        copy.add(Tag::Modality, "CS", "MR").add(tag, vr, value);
        item.add(tag, vr, value);
        outer.sequence(0x00081199, {item});
        Writer nested;
        nested.add(Tag::Modality, "CS", "MR").sequence(Tag::ReferencedSeries, {outer});
        for (const Writer *w : {&copy, &nested}) {
            const DicomFile file = DicomFile::parse(w->file(ExplicitLE));
            QVERIFY(file.isValid());
            const QList<Anonymize::Failure> failures = Anonymize::verify(file, session);
            if (found.isEmpty()) {
                QVERIFY2(failures.isEmpty(), failures.isEmpty() ? "" : qPrintable(failures.first().text()));
                continue;
            }
            QCOMPARE(failures.size(), 1);
            QCOMPARE(failures.first().tag, tag);
            QCOMPARE(failures.first().found, found);
            QVERIFY2(failures.first().text().startsWith(tagText(tag)) && failures.first().text().endsWith(found),
                     qPrintable(failures.first().text()));
        }
    }

    void findsNamesInOtherScripts() {
        // A script without spaces has no words to be whole; its names are
        // looked for anywhere.
        Writer source;
        source.add(Tag::SpecificCharacterSet, "CS", "ISO_IR 192").add(Tag::PatientName, "PN", QString("山田^太郎").toUtf8());
        Anonymize::Session session;
        Anonymize::learn(DicomFile::parse(source.file(ExplicitLE)), session);
        Writer copy;
        copy.add(Tag::SpecificCharacterSet, "CS", "ISO_IR 192").add(Tag::SeriesDescription, "LO", QString("山田太郎様の検査").toUtf8());
        const auto failures = Anonymize::verify(DicomFile::parse(copy.file(ExplicitLE)), session);
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures.first().found, QString("山田"));
        QCOMPARE(failures.first().what, QString("a person's name"));
    }

    void doesNotKeepACopyThatNamesThePatient() {
        // The patient's name in a description that was asked to be kept.
        Writer described = janeDoe(true, "1.2.7.1");
        described.body.replace("Axial 5 mm", "Mrs DOE CT");
        described.pixels(le16({1, 2, 3, 4}));
        Anonymize::Options keep;
        keep.keepDescriptions = true;
        Made made = anonymized(described.file(ExplicitLE), keep);
        QVERIFY(!made.written);
        QCOMPARE(made.error, QString("The copy wasn't kept: (0008,103E) Series Description still holds a person's name: Doe"));
        QCOMPARE(made.outcome.failures.size(), 1);
        QCOMPARE(made.outcome.failures.first().tag, quint32(Tag::SeriesDescription));
        QCOMPARE(made.outcome.failures.first().found, QString("Doe"));
        QCOMPARE(made.outcome.failures.first().what, QString("a person's name"));
        QVERIFY(made.outcome.failures.first().path.endsWith(".src"));
        // Neither the copy nor what it was before it was checked is left.
        QVERIFY(!QFileInfo::exists(made.path) && !QFileInfo::exists(made.path + ".unchecked"));
        // Without the description there's nothing to find.
        made = anonymized(described.file(ExplicitLE));
        QVERIFY2(made.written, qPrintable(made.error));

        // What's left under a tag nobody registered: the table has no row
        // for it, so it's kept, and then found.
        Writer leftover = janeDoe(true, "1.2.7.1");
        leftover.add(0x00082299, "LO", "Seen by Dr House").pixels(le16({1, 2, 3, 4}));
        made = anonymized(leftover.file(ExplicitLE));
        QVERIFY(!made.written);
        QCOMPARE(made.error, QString("The copy wasn't kept: (0008,2299) an unnamed attribute still holds a person's name: House"));
        // The same written implicitly, where it's bytes of no known kind.
        Writer implicit = janeDoe(false, "1.2.7.1");
        implicit.add(0x00082299, "LO", "Seen by Dr House").pixels(le16({1, 2, 3, 4}));
        made = anonymized(implicit.file(ImplicitLE));
        QVERIFY(!made.written);
        QCOMPARE(made.outcome.failures.first().tag, 0x00082299u);
        // And an ID in bytes the table keeps: a colour profile's.
        Writer profile = janeDoe(true, "1.2.7.1");
        profile.add(0x00282000, "OB", QByteArray("\x00\x00\x02\x10 patient MRN42 \x00\x01", 22)).pixels(le16({1, 2, 3, 4}));
        made = anonymized(profile.file(ExplicitLE));
        QVERIFY(!made.written);
        QCOMPARE(made.outcome.failures.first().what, QString("an ID"));

        // A name the images never spell out, only a report left out of the
        // copy, is still not to be in them.
        QTemporaryDir dir, out;
        Writer first = janeDoe(true, "1.2.7.1"), second = janeDoe(true, "1.2.8.1", "1.2.8");
        second.body.replace("Axial 5 mm", "Cuddy's CT");
        QVERIFY(write(dir.filePath("IM1"), first.pixels(le16({1, 2, 3, 4})).file(ExplicitLE)));
        QVERIFY(write(dir.filePath("IM2"), second.pixels(le16({1, 2, 3, 4})).file(ExplicitLE)));
        Writer text;
        text.add(Tag::ValueType, "CS", "TEXT").add(Tag::TextValue, "UT", "Unremarkable.");
        Writer report;
        report.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.88.11")
            .add(Tag::Modality, "CS", "SR")
            .add(Tag::StudyInstance, "UI", "1.2.7")
            .add(0x0040A075, "PN", "Cuddy^Lisa")
            .sequence(Tag::ContentSequence, {text});
        QVERIFY(write(dir.filePath("SR1"), report.file(ExplicitLE)));
        Writer series;
        series.add(Tag::SeriesInstance, "UI", "1.2.7.3");
        Writer state;
        state.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.11.1").add(Tag::Modality, "CS", "PR").sequence(Tag::ReferencedSeries, {series});
        QVERIFY(write(dir.filePath("PR1"), state.file(ExplicitLE)));

        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QCOMPARE(scan.series.size(), 2);
        const Anonymize::Result result = Anonymize::copyScan(scan, out.path(), nullptr, keep);
        QCOMPARE(result.written, 1);
        QCOMPARE(result.skipped, 1);        // The report.
        QCOMPARE(result.annotations, 1);    // The presentation state.
        QCOMPARE(result.errors.size(), 1);
        QVERIFY2(result.errors.first().startsWith(dir.filePath("IM2") + ": The copy wasn't kept: (0008,103E)"), qPrintable(result.errors.first()));
        QCOMPARE(result.failures.size(), 1);
        QCOMPARE(result.failures.first().path, dir.filePath("IM2"));
        QCOMPARE(result.failures.first().found, QString("Cuddy"));
        // And by series, for the window: which one lacks a file, and why.
        QCOMPARE(result.uncopied.size(), 1);
        QCOMPARE(result.uncopied.first().series, 1);
        QCOMPARE(result.uncopied.first().path, dir.filePath("IM2"));
        QVERIFY(result.uncopied.first().refused);
        QCOMPARE(dir.filePath("IM2") + ": " + result.uncopied.first().reason, result.errors.first());
        const QStringList copies = listFiles({result.folder}, true);
        QCOMPARE(copies.size(), 1);
        // What was kept counts only the copies that were.
        QCOMPARE(filesOf(result.summary.kept, "Series Description"), 1);

        // The command line says the same, and fails.
        QProcess *p = run({"anonymize", dir.path(), "--out", out.path(), "--keep-descriptions", "--json"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 1);
        const QJsonObject json = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
        QCOMPARE(json.value("written").toInt(), 1);
        QCOMPARE(json.value("failed").toInt(), 1);
        QCOMPARE(json.value("reportsLeftOut").toInt(), 1);
        QCOMPARE(json.value("annotationsLeftOut").toInt(), 1);
        const QJsonObject failure = json.value("verificationFailures").toArray().first().toObject();
        QCOMPARE(failure.value("tag").toString(), QString("(0008,103E)"));
        QCOMPARE(failure.value("name").toString(), QString("Series Description"));
        QCOMPARE(failure.value("found").toString(), QString("Cuddy"));
        QCOMPARE(failure.value("file").toString(), dir.filePath("IM2"));
        QVERIFY(p->readAllStandardError().contains("still holds a person's name: Cuddy"));
        // Without the descriptions both are copied.
        p = run({"anonymize", dir.path(), "--out", out.path()});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 0);
        const QString said = QString::fromUtf8(p->readAllStandardOutput());
        QVERIFY2(said.contains("Copied 2 files into ") && said.contains("Left out 1 report,") && said.contains("Left out 1 annotation file,"),
                 qPrintable(said));
    }

    void plansWhatACopyWouldDo() {
        // Two series, one of them written implicitly, with a file that
        // can't pass among them.
        QTemporaryDir dir, out;
        for (int i = 1; i <= 3; ++i) {
            Writer w = janeDoe(i != 2, "1.2.7." + QByteArray::number(i));
            w.add(0x00090010, "LO", "VENDOR").add(0x00080081, "ST", "1 Infinite Loop").pixels(le16({1, 2, 3, 4}));
            QVERIFY(write(dir.filePath(QString("A%1").arg(i)), w.file(i != 2 ? ExplicitLE : ImplicitLE)));
        }
        Writer b = janeDoe(true, "1.2.8.1", "1.2.8"), bad = janeDoe(true, "1.2.8.2", "1.2.8");
        bad.add(0x00082299, "LO", "Seen by Dr House");
        QVERIFY(write(dir.filePath("B1"), b.pixels(le16({1, 2, 3, 4})).file(ExplicitLE)));
        QVERIFY(write(dir.filePath("B2"), bad.pixels(le16({1, 2, 3, 4})).file(ExplicitLE)));
        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QCOMPARE(scan.series.size(), 2);

        for (const bool keepDescriptions : {false, true}) {
            Anonymize::Options options;
            options.keepDescriptions = keepDescriptions;
            options.blackout[1] << QRect(0, 0, 1, 1);
            const Anonymize::Result planned = Anonymize::plan(scan, options);
            // Nothing was written, anywhere.
            QVERIFY(planned.folder.isEmpty());
            QVERIFY(QDir(out.path()).isEmpty());
            QCOMPARE(listFiles({dir.path()}, true).size(), 5);

            const Anonymize::Result copied = Anonymize::copyScan(scan, out.path(), nullptr, options);
            QCOMPARE(copied.written, 4);
            QCOMPARE(planned.written, copied.written);
            QCOMPARE(planned.errors, copied.errors);
            QCOMPARE(planned.failures.size(), 1);
            QCOMPARE(copied.failures.size(), 1);
            QCOMPARE(planned.failures.first().text(), copied.failures.first().text());
            QCOMPARE(planned.blackedOut, 1);
            QCOMPARE(copied.blackedOut, 1);
            QCOMPARE(planned.showDetails, copied.showDetails);
            QCOMPARE(planned.method, copied.method);
            QVERIFY(planned.summary == copied.summary);

            // And the summary says what it should, by name, with how many files.
            const Anonymize::Summary &s = copied.summary;
            QCOMPARE(filesOf(s.removed, "Institution Address"), 3);
            QCOMPARE(filesOf(s.removed, "Private attributes"), 3);
            QCOMPARE(s.removed.last().name, QString("Private attributes"));
            QVERIFY(s.removed.last().tag.isEmpty());
            // Those are Dicolens' words, to translate; the rest the standard's.
            QVERIFY(!s.removed.last().standard && s.removed.first().standard);
            // B2 isn't copied, which is said by series too, and that it
            // wasn't for want of being readable.
            QCOMPARE(planned.uncopied.size(), 1);
            QCOMPARE(copied.uncopied.size(), 1);
            QCOMPARE(copied.uncopied.first().series, 1);
            QVERIFY(copied.uncopied.first().refused);
            QCOMPARE(filesOf(s.emptied, "Accession Number"), 4);
            QCOMPARE(filesOf(s.emptied, "Patient's Birth Date"), 4);
            QCOMPARE(filesOf(s.replaced, "Patient's Name"), 4);
            QCOMPARE(filesOf(s.replaced, "Patient ID"), 4);
            QCOMPARE(filesOf(s.replaced, "Study Instance UID"), 4);
            QCOMPARE(filesOf(s.replaced, "Institution Name"), 4);
            QCOMPARE(filesOf(s.kept, "Study Date"), 4);
            QCOMPARE(filesOf(s.kept, "Patient's Age"), 4);
            QCOMPARE(filesOf(s.removed, "Study Description"), keepDescriptions ? 0 : 4);
            QCOMPARE(filesOf(s.kept, "Study Description"), keepDescriptions ? 4 : 0);
            QCOMPARE(filesOf(s.replaced, "Protocol Name"), keepDescriptions ? 0 : 4);
            QCOMPARE(filesOf(s.kept, "Protocol Name"), keepDescriptions ? 4 : 0);
            QVERIFY(!names(s.kept).contains("Modality"));
            for (const auto &c : s.removed + s.emptied + s.replaced + s.kept)
                QVERIFY2(c.files > 0 && !c.name.isEmpty() && (c.tag.isEmpty() || c.tag.startsWith('(')), qPrintable(c.name));
            QVERIFY(std::is_sorted(s.kept.begin(), s.kept.end(), [](const auto &x, const auto &y) { return x.tag < y.tag; }));
            QVERIFY(QDir(copied.folder).removeRecursively());
        }

        // From the command line: a dry run needs nowhere to write, and
        // says which edition of the standard it went by.
        QProcess *p = run({"anonymize", dir.path(), "--dry-run", "--json"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 1);   // B2 wouldn't pass.
        QJsonObject json = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
        QCOMPARE(json.value("dryRun").toBool(), true);
        QVERIFY(json.value("folder").isNull() && !json.contains("written"));
        QCOMPARE(json.value("wouldWrite").toInt(), 4);
        QCOMPARE(json.value("edition").toString(), Anonymize::edition());
        QCOMPARE(json.value("standard").toString(), "DICOM PS3.15 " + Dictionary::edition());
        QCOMPARE(json.value("basicProfile").toBool(), true);
        QCOMPARE(json.value("method").toArray().size(), Anonymize::method().size());
        QCOMPARE(json.value("verificationFailures").toArray().size(), 1);
        const QJsonObject summary = json.value("summary").toObject();
        QStringList kept;
        for (const auto &entry : summary.value("kept").toArray()) {
            QCOMPARE(entry.toObject().value("files").toInt(), 4);
            kept << entry.toObject().value("name").toString();
        }
        QCOMPARE(kept, (QStringList{"Study Date", "Patient's Sex", "Patient's Age"}));
        QCOMPARE(summary.value("emptied").toArray().first().toObject().value("tag").toString(), QString("(0008,0050)"));
        QVERIFY(summary.value("removed").toArray().last().toObject().value("tag").isNull());
        QVERIFY(QDir(out.path()).isEmpty());

        // The same for one good series, for people.
        QTemporaryDir good;
        QVERIFY(QFile::copy(dir.filePath("A1"), good.filePath("A1")));
        p = run({"anonymize", good.path(), "--dry-run", "--keep-descriptions"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 0);
        const QString said = QString::fromUtf8(p->readAllStandardOutput());
        QVERIFY2(said.startsWith("Would copy 1 file; nothing was written\nBy DICOM PS3.15 " + Anonymize::edition()), qPrintable(said));
        QVERIFY2(said.contains("descriptions kept as they are") && !said.contains("Confidentiality Profile;"), qPrintable(said));
        QVERIFY2(said.contains("  Kept:     Study Date (1), Study Description (1), Series Description (1)"), qPrintable(said));
        QVERIFY2(said.contains("Private attributes (1)\n"), qPrintable(said));
        p = run({"anonymize", good.path(), "--json"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 1);   // Neither --out nor --dry-run.
    }

    void blacksOutARegion_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("black");            // The stored value that's black; -1 for colour.
        QTest::addColumn<int>("bitsAllocated");
        QTest::addColumn<int>("bitsStored");
        QTest::addColumn<QString>("photometric");
        QTest::addColumn<int>("frames");

        // 4 columns by 3 rows, each value its own.
        auto grey = [](int scale, int offset, int frames = 1) {
            QList<int> values;
            for (int i = 0; i < 12 * frames; ++i)
                values << offset + scale * (i + 1);
            return values;
        };
        QByteArray rgb;
        for (int i = 0; i < 36; ++i)
            rgb += char(20 + 6 * i);
        auto image = [](int bits, int stored, bool isSigned, const QByteArray &photometric = "MONOCHROME2") {
            Writer w;
            w.add(Tag::SopClass, "UI", CtImage).add(Tag::SopInstance, "UI", "1.2.7.1").add(Tag::PatientName, "PN", "Doe^Jane");
            return w.image(3, 4, bits, stored, isSigned, photometric);
        };
        auto bytes8 = [](const QList<int> &values) {
            QByteArray out;
            for (const int v : values)
                out += char(v);
            return out;
        };
        auto row = [](const char *name, const QByteArray &bytes, int black, int allocated, int stored,
                      const char *photometric = "MONOCHROME2", int frames = 1) {
            QTest::newRow(name) << bytes << black << allocated << stored << QString(photometric) << frames;
        };

        row("8-bit grey", image(8, 8, false).pixels(bytes8(grey(20, 0)), "OB").file(ExplicitLE), 0, 8, 8);
        row("12 bits of 16", image(16, 12, false).pixels(le16(grey(300, 400))).file(ExplicitLE), 0, 16, 12);
        row("16 bits, implicit", [&] {
            Writer w = image(16, 16, false);
            // Rewritten implicitly: the same elements without their VRs.
            Writer implicit;
            implicit.explicitVr = false;
            implicit.add(Tag::SopClass, "UI", CtImage).add(Tag::PatientName, "PN", "Doe^Jane").image(3, 4, 16, 16, false);
            return implicit.pixels(le16(grey(5000, 1000))).file(ImplicitLE);
        }(), 0, 16, 16);
        row("16 bits signed", image(16, 16, true).pixels(le16(grey(5000, -30001))).file(ExplicitLE), -32768, 16, 16);
        {
            // Twelve signed bits in sixteen, as two's complement of twelve.
            QList<int> values;
            for (const int v : grey(300, -1900))
                values << (v & 0x0FFF);
            row("12 bits signed", image(16, 12, true).pixels(le16(values)).file(ExplicitLE), -2048, 16, 12);
        }
        row("white is zero", image(16, 12, false, "MONOCHROME1").pixels(le16(grey(300, 400))).file(ExplicitLE), 4095, 16, 12,
            "MONOCHROME1");
        {
            // Stored in the top twelve bits, the bottom four another's.
            Writer w;
            w.add(Tag::SopClass, "UI", CtImage).us(Tag::SamplesPerPixel, 1).add(Tag::Photometric, "CS", "MONOCHROME2")
                .us(Tag::Rows, 3).us(Tag::Columns, 4).us(Tag::BitsAllocated, 16).us(Tag::BitsStored, 12).us(Tag::HighBit, 15)
                .us(Tag::PixelRepresentation, 0);
            QList<int> values;
            for (const int v : grey(300, 400))
                values << ((v << 4) | 0x9);
            row("values in the top bits", w.pixels(le16(values)).file(ExplicitLE), 0, 16, 12);
        }
        row("three frames", image(16, 12, false).add(Tag::NumberOfFrames, "IS", "3").pixels(le16(grey(100, 0, 3))).file(ExplicitLE),
            0, 16, 12, "MONOCHROME2", 3);
        row("colour", image(8, 8, false, "RGB").us(Tag::PlanarConfiguration, 0).pixels(rgb, "OB").file(ExplicitLE), -1, 8, 8, "RGB");
        row("colour by plane", image(8, 8, false, "RGB").us(Tag::PlanarConfiguration, 1).pixels(rgb, "OB").file(ExplicitLE), -1, 8, 8, "RGB");
        {
            // A palette of four colours, none of them black.
            Writer w;
            w.add(Tag::SopClass, "UI", CtImage).us(Tag::SamplesPerPixel, 1).add(Tag::Photometric, "CS", "PALETTE COLOR")
                .us(Tag::Rows, 3).us(Tag::Columns, 4).us(Tag::BitsAllocated, 8).us(Tag::BitsStored, 8).us(Tag::HighBit, 7)
                .us(Tag::PixelRepresentation, 0);
            for (const quint32 tag : {Tag::RedPaletteDescriptor, Tag::GreenPaletteDescriptor, Tag::BluePaletteDescriptor})
                w.add(tag, "US", le16({4, 0, 8}));
            w.add(Tag::RedPaletteData, "OW", QByteArray("\x10\x40\x80\xF0", 4))
                .add(Tag::GreenPaletteData, "OW", QByteArray("\x20\x50\x90\xE0", 4))
                .add(Tag::BluePaletteData, "OW", QByteArray("\x30\x60\xA0\xD0", 4));
            row("colours from a palette", w.pixels(QByteArray("\x00\x01\x02\x03\x03\x02\x01\x00\x01\x01\x02\x02", 12), "OB").file(ExplicitLE),
                -1, 8, 8, "RGB");
        }
        {
            // RLE: the high bytes of every pixel, then the low ones.
            QByteArray high, low;
            for (const int v : grey(300, 400)) {
                high += char(v >> 8);
                low += char(v & 0xFF);
            }
            row("RLE", image(16, 12, false).encapsulated({rle({high, low})}).file("1.2.840.10008.1.2.5"), 0, 16, 12);
            // And colour, two frames, a plane a segment.
            QList<QByteArray> frames;
            for (int f = 0; f < 2; ++f) {
                QList<QByteArray> planes{{}, {}, {}};
                for (int i = 0; i < 36; ++i)
                    planes[i % 3] += char(uchar(rgb[i]) + 40 * f);
                frames << rle(planes);
            }
            row("RLE colour, two frames",
                image(8, 8, false, "RGB").us(Tag::PlanarConfiguration, 1).add(Tag::NumberOfFrames, "IS", "2").encapsulated(frames).file("1.2.840.10008.1.2.5"),
                -1, 8, 8, "RGB", 2);
        }
        {
            QVector<quint16> samples;
            for (const int v : grey(300, 400))
                samples << quint16(v);
            row("lossless JPEG", image(16, 12, false).encapsulated({losslessJpeg(samples, 4, 3, 1, 12, 1, 0)}).file("1.2.840.10008.1.2.4.70"),
                0, 16, 12);
            QVector<quint16> colours;
            for (const char c : rgb)
                colours << uchar(c);
            row("lossless JPEG colour",
                image(8, 8, false, "RGB").us(Tag::PlanarConfiguration, 0).encapsulated({losslessJpeg(colours, 4, 3, 3, 8, 1, 0)}).file("1.2.840.10008.1.2.4.70"),
                -1, 8, 8, "RGB");
        }
    }

    void blacksOutARegion() {
        QFETCH(QByteArray, bytes);
        QFETCH(int, black);
        QFETCH(int, bitsAllocated);
        QFETCH(int, bitsStored);
        QFETCH(QString, photometric);
        QFETCH(int, frames);
        // Two columns of the top two rows, and a rectangle that hangs over
        // the corner, of which only the corner pixel is the picture's.
        const QList<QRect> rectangles{QRect(1, 0, 2, 2), QRect(3, 2, 5, 5)};
        auto inside = [&](int x, int y) {
            return std::any_of(rectangles.begin(), rectangles.end(), [&](const QRect &r) { return r.contains(x, y); });
        };

        const DicomFile source = onDisk(bytes);
        QVERIFY(source.isValid());
        Anonymize::Session session;
        Anonymize::Outcome outcome;
        QString error;
        const QString path = fresh(".dcm");
        QVERIFY2(Anonymize::write(source, path, session, &error, {}, rectangles, &outcome), qPrintable(error));
        QVERIFY(outcome.blackedOut);
        const DicomFile copy = DicomFile::read(path);
        QVERIFY(copy.isValid());

        // Uncompressed, with an image pixel module that's true of it.
        const bool colour = photometric == "RGB";
        QCOMPARE(copy.transferSyntax(), ExplicitLE);
        QVERIFY(!copy.encapsulated());
        QCOMPARE(copy.integer(Tag::Rows), 3);
        QCOMPARE(copy.integer(Tag::Columns), 4);
        QCOMPARE(copy.integer(Tag::SamplesPerPixel), colour ? 3 : 1);
        QCOMPARE(copy.first(Tag::Photometric), photometric);
        QCOMPARE(copy.integer(Tag::BitsAllocated), bitsAllocated);
        QCOMPARE(copy.integer(Tag::BitsStored), bitsStored);
        QCOMPARE(copy.integer(Tag::HighBit), bitsStored - 1);
        QCOMPARE(copy.integer(Tag::PixelRepresentation), source.integer(Tag::PixelRepresentation));
        QCOMPARE(copy.has(Tag::PlanarConfiguration), colour);
        QCOMPARE(copy.integer(Tag::PlanarConfiguration), 0);
        QCOMPARE(std::max(1, copy.integer(Tag::NumberOfFrames, 1)), frames);
        QCOMPARE(copy.pixelLength(), qint64(12) * (colour ? 3 : bitsAllocated / 8) * frames);
        QCOMPARE(copy.pixelVr(), QByteArray(!colour && bitsAllocated == 16 ? "OW" : "OB"));
        for (const quint32 tag : {Tag::RedPaletteDescriptor, Tag::RedPaletteData, Tag::BluePaletteData})
            QVERIFY(!copy.has(tag));
        QVERIFY(!copy.has(0x00282110));   // Nothing here was ever compressed lossily.
        QVERIFY(copy.string(0x00120063).split('\\').contains("Regions of the picture blacked out: 2"));

        // Every frame: black inside the rectangles, and outside them the
        // very values the original decodes to.
        for (int f = 0; f < frames; ++f) {
            const Frame before = decodeFrame(source, f), after = decodeFrame(copy, f);
            QVERIFY2(before.error.isEmpty(), qPrintable(before.error));
            QVERIFY2(after.error.isEmpty(), qPrintable(after.error));
            QCOMPARE(after.isColor(), colour);
            QCOMPARE(after.isSigned, before.isSigned);
            QCOMPARE(after.inverted, before.inverted);
            int changed = 0;
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 4; ++x) {
                    if (colour) {
                        QCOMPARE(after.color.pixel(x, y), inside(x, y) ? qRgb(0, 0, 0) : before.color.pixel(x, y));
                        changed += after.color.pixel(x, y) != before.color.pixel(x, y);
                    } else {
                        QCOMPARE(after.stored(x, y), inside(x, y) ? black : before.stored(x, y));
                        changed += after.stored(x, y) != before.stored(x, y);
                    }
                }
            QCOMPARE(changed, 5);
            // Black is black: the darkest the copy can show.
            if (!colour) {
                const QImage shown = render(after, after.windowCenter, after.windowWidth, false);
                for (int y = 0; y < 3; ++y)
                    for (int x = 0; x < 4; ++x)
                        QVERIFY(qGray(shown.pixel(1, 0)) <= qGray(shown.pixel(x, y)));
                QVERIFY2(qGray(shown.pixel(1, 0)) <= 1, qPrintable(QString::number(qGray(shown.pixel(1, 0)))));
            }
        }
        // The patient is out of the tags as in any copy.
        QCOMPARE(copy.first(Tag::PatientName), source.has(Tag::PatientName) ? QString("Anonymous") : QString());
    }

    void saysWhenLossyPixelsWereRedrawn() {
        QByteArray rgb;
        for (int i = 0; i < 16 * 16 * 3; ++i)
            rgb += char(40 + (i * 7) % 180);
        const QByteArray stream = jpeg(rgb, 16, 16);
        if (stream.isEmpty())
            QSKIP("ffmpeg can't encode JPEG here");
        Writer w;
        w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.7").us(Tag::SamplesPerPixel, 3).add(Tag::Photometric, "CS", "YBR_FULL")
            .us(Tag::PlanarConfiguration, 0).us(Tag::Rows, 16).us(Tag::Columns, 16).us(Tag::BitsAllocated, 8)
            .us(Tag::BitsStored, 8).us(Tag::HighBit, 7).us(Tag::PixelRepresentation, 0).add(0x00282112, "DS", "10")
            .encapsulated({stream});
        const DicomFile source = onDisk(w.file("1.2.840.10008.1.2.4.50"));
        const Frame before = decodeFrame(source, 0);
        QVERIFY2(before.error.isEmpty(), qPrintable(before.error));
        const Made made = anonymized(w.file("1.2.840.10008.1.2.4.50"), {}, {QRect(0, 0, 16, 4)});
        QVERIFY2(made.written, qPrintable(made.error));
        // What the compression took isn't back, and the copy goes on
        // saying it was compressed, though its pixels no longer are.
        QCOMPARE(made.copy.transferSyntax(), ExplicitLE);
        QCOMPARE(made.copy.first(0x00282110), QString("01"));
        QCOMPARE(made.copy.first(0x00282112), QString("10"));
        QCOMPARE(made.copy.first(Tag::Photometric), QString("RGB"));
        const Frame after = decodeFrame(made.copy, 0);
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 16; ++x)
                QCOMPARE(after.color.pixel(x, y), y < 4 ? qRgb(0, 0, 0) : before.color.pixel(x, y));
    }

    void refusesWhatItCantRedraw() {
        const QList<QRect> corner{QRect(0, 0, 1, 1)};
        auto refused = [&](const QByteArray &bytes, const QList<QRect> &rectangles) {
            const Made made = anonymized(bytes, {}, rectangles);
            // Refused, nothing is written, by the plan as by the copy.
            if (made.written || QFileInfo::exists(made.path) || QFileInfo::exists(made.path + ".unchecked"))
                return QStringLiteral("(it was written)");
            Anonymize::Session session;
            QString planned;
            if (Anonymize::check(onDisk(bytes), session, &planned, {}, rectangles) || planned != made.error)
                return QStringLiteral("(the plan said otherwise: %1)").arg(planned);
            return made.error;
        };
        const QString because = "A region can't be blacked out of this image: ";
        auto image = [](int bits, int stored, const QByteArray &photometric = "MONOCHROME2") {
            Writer w;
            w.add(Tag::SopClass, "UI", CtImage);
            return w.image(1, 2, bits, stored, false, photometric);
        };

        // A video, which is one stream and not a frame at a time.
        QCOMPARE(refused(image(8, 8, "YBR_PARTIAL_420").encapsulated({QByteArray(8, 'v')}).file("1.2.840.10008.1.2.4.102"), corner),
                 because + "it's a video, which can't be written back frame for frame");
        // 32-bit values, which the decoder brings into 16 to show.
        QCOMPARE(refused(image(32, 32).pixels(QByteArray(8, 1)).file(ExplicitLE), corner),
                 because + "its pixels are 32-bit, which aren't kept value for value");
        // A bit a pixel, which it spreads to a byte.
        QCOMPARE(refused(image(1, 1).pixels(QByteArray(2, 1), "OB").file(ExplicitLE), corner),
                 because + "its pixels are 1-bit, which aren't kept value for value");
        // Colour of 16 bits a sample, which it shows in 8.
        QCOMPARE(refused(image(16, 16, "RGB").pixels(QByteArray(12, 1)).file(ExplicitLE), corner),
                 because + "its colours are 16-bit, which are only shown in 8 bits");
        // A rectangle that misses the picture would hide nothing.
        QCOMPARE(refused(image(8, 8).pixels(QByteArray(2, 1), "OB").file(ExplicitLE), {QRect(2, 0, 4, 4)}),
                 because + "the rectangle 2,0,4,4 lies outside its 2x1 picture");
        QCOMPARE(refused(image(8, 8).pixels(QByteArray(2, 1), "OB").file(ExplicitLE), {QRect(0, 0, 0, 0)}),
                 because + "the rectangle 0,0,0,0 lies outside its 2x1 picture");
        // No picture at all.
        Writer none;
        none.add(Tag::SopClass, "UI", CtImage);
        QCOMPARE(refused(none.file(ExplicitLE), corner), because + "it has no picture to black out");
        // A compressed frame that doesn't decode: the decoder's own reason,
        // which only decoding finds, so a plan can't say it first.
        const Made damaged = anonymized(image(16, 12).encapsulated({QByteArray(4, 'x')}).file("1.2.840.10008.1.2.4.70"), {}, corner);
        QVERIFY(!damaged.written && !QFileInfo::exists(damaged.path) && !QFileInfo::exists(damaged.path + ".unchecked"));
        QVERIFY2(damaged.error.startsWith(because) && damaged.error.size() > because.size(), qPrintable(damaged.error));

        // A palette of 16-bit colours is shown through their top 8 bits:
        // refused when the bottom 8 say more, drawn when they don't.
        auto palette = [](const QByteArray &entries) {
            Writer w;
            w.add(Tag::SopClass, "UI", CtImage).us(Tag::SamplesPerPixel, 1).add(Tag::Photometric, "CS", "PALETTE COLOR")
                .us(Tag::Rows, 1).us(Tag::Columns, 2).us(Tag::BitsAllocated, 8).us(Tag::BitsStored, 8).us(Tag::HighBit, 7)
                .us(Tag::PixelRepresentation, 0);
            for (const quint32 tag : {Tag::RedPaletteDescriptor, Tag::GreenPaletteDescriptor, Tag::BluePaletteDescriptor})
                w.add(tag, "US", le16({2, 0, 16}));
            for (const quint32 tag : {Tag::RedPaletteData, Tag::GreenPaletteData, Tag::BluePaletteData})
                w.add(tag, "OW", entries);
            return w.pixels(QByteArray("\x00\x01", 2), "OB").file(ExplicitLE);
        };
        QCOMPARE(refused(palette(le16({0x1234, 0x8000})), corner),
                 because + "its colours come from a 16-bit palette, which is only shown in 8 bits");
        const Made drawn = anonymized(palette(le16({0x4040, 0x8000})), {}, {QRect(0, 0, 1, 1)});
        QVERIFY2(drawn.written, qPrintable(drawn.error));
        QCOMPARE(decodeFrame(drawn.copy, 0).color.pixel(0, 0), qRgb(0, 0, 0));
        QCOMPARE(decodeFrame(drawn.copy, 0).color.pixel(1, 0), qRgb(0x80, 0x80, 0x80));

        // The window asks the same before it lets a region be marked, and
        // is told which it is, to say in the user's language.
        using Cant = Anonymize::Redraw::Cant;
        auto asked = [&](const QByteArray &bytes) { return Anonymize::canRedraw(onDisk(bytes)); };
        QCOMPARE(asked(image(8, 8, "YBR_PARTIAL_420").encapsulated({QByteArray(8, 'v')}).file("1.2.840.10008.1.2.4.102")).cant, Cant::Video);
        QCOMPARE(asked(image(32, 32).pixels(QByteArray(8, 1)).file(ExplicitLE)).cant, Cant::GreyBits);
        QCOMPARE(asked(image(32, 32).pixels(QByteArray(8, 1)).file(ExplicitLE)).bits, 32);
        QCOMPARE(asked(image(1, 1).pixels(QByteArray(2, 1), "OB").file(ExplicitLE)).bits, 1);
        QCOMPARE(asked(image(16, 16, "RGB").pixels(QByteArray(12, 1)).file(ExplicitLE)).cant, Cant::ColourBits);
        QCOMPARE(asked(palette(le16({0x1234, 0x8000}))).cant, Cant::Palette);
        QCOMPARE(asked(none.file(ExplicitLE)).cant, Cant::NoPicture);
        QVERIFY(asked(palette(le16({0x4040, 0x8000}))).possible());
        QVERIFY(asked(image(16, 12).pixels(QByteArray(4, 1)).file(ExplicitLE)).possible());
        QVERIFY(asked(image(8, 8).pixels(QByteArray(2, 1), "OB").file(ExplicitLE)).possible());
    }

    void blacksOutOnlyTheSeriesAsked() {
        // Two series; a rectangle for the second. The first is copied as
        // it was, byte for byte of its picture, compressed as it came.
        QTemporaryDir dir, out;
        QVector<quint16> samples{100, 200, 300, 400};
        Writer compressed = janeDoe(true, "1.2.7.1");
        QVERIFY(write(dir.filePath("A1"), compressed.encapsulated({losslessJpeg(samples, 2, 2, 1, 12, 1, 0)}).file("1.2.840.10008.1.2.4.70")));
        for (int i = 1; i <= 2; ++i) {
            Writer w = janeDoe(true, "1.2.8." + QByteArray::number(i), "1.2.8");
            w.add(Tag::Modality, "CS", "US");
            QVERIFY(write(dir.filePath(QString("B%1").arg(i)), w.pixels(le16({10 * i, 20, 30, 40})).file(ExplicitLE)));
        }
        const Scan scan = scanFiles(listFiles({dir.path()}, true));
        QCOMPARE(scan.series.size(), 2);
        QCOMPARE(scan.series[1].slices.size(), 2);
        Anonymize::Options options;
        options.blackout[1] << QRect(0, 0, 2, 1);
        const Anonymize::Result result = Anonymize::copyScan(scan, out.path(), nullptr, options);
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join("\n")));
        QCOMPARE(result.written, 3);
        QCOMPARE(result.blackedOut, 2);
        QCOMPARE(result.showDetails, 2);   // Ultrasound may show more than the rectangle hid.
        QCOMPARE(result.showDetailsIn.value(1), 2);
        QVERIFY(!result.showDetailsIn.contains(0));
        const DicomFile first = DicomFile::read(result.folder + "/Series 001/0001.dcm");
        QCOMPARE(first.transferSyntax(), QString("1.2.840.10008.1.2.4.70"));
        const DicomFile original = DicomFile::read(dir.filePath("A1"));
        QCOMPARE(first.readPixels(0, first.pixelLength()), original.readPixels(0, original.pixelLength()));
        QVERIFY(!first.string(0x00120063).contains("blacked out"));
        for (int i = 1; i <= 2; ++i) {
            const DicomFile second = DicomFile::read(QStringLiteral("%1/Series 002/000%2.dcm").arg(result.folder).arg(i));
            QCOMPARE(decodeFrame(second, 0).mono, (QVector<quint16>{0, 0, 30, 40}));
            QVERIFY(second.string(0x00120063).contains("Regions of the picture blacked out: 1"));
        }

        // From the command line, series counted from 1 as info lists them.
        QProcess *p = run({"anonymize", dir.path(), "--out", out.path(), "--blackout", "2:0,0,2,1", "--blackout", "2:1,1,1,1", "--json"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 0);
        const QJsonObject json = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
        QCOMPARE(json.value("written").toInt(), 3);
        QCOMPARE(json.value("blackedOut").toInt(), 2);
        QCOMPARE(json.value("mayShowDetailsInPicture").toInt(), 2);
        const DicomFile twice = DicomFile::read(json.value("folder").toString() + "/Series 002/0001.dcm");
        QCOMPARE(decodeFrame(twice, 0).mono, (QVector<quint16>{0, 0, 30, 0}));
        p = run({"anonymize", dir.path(), "--out", out.path(), "--blackout", "2:0,0,2,1"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 0);
        QVERIFY(p->readAllStandardOutput().contains("Blacked out a region of 2 images\n"));

        // What isn't a rectangle of a series there is, is refused before
        // anything is written.
        const int before = QDir(out.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        const QList<QPair<QString, QString>> wrong{
            {"3:0,0,1,1", "there are 2 series; --blackout counts them from 1, as info lists them"},
            {"0:0,0,1,1", "there are 2 series"},
            {"2:5,5,1,1", "the rectangle 5,5,1,1 lies outside series 2's 2x2 picture"},
            {"2:0,0,0,1", "--blackout takes <series>:<x>,<y>,<width>,<height>"},
            {"2:0,0,1", "--blackout takes"},
            {"0,0,1,1", "--blackout takes"},
            {"two:0,0,1,1", "--blackout takes"},
            {"2:-1,0,1,1", "--blackout takes"},
            {"2:0,0,2000000000,2000000000", "--blackout takes"},
        };
        for (const auto &[region, message] : wrong) {
            p = run({"anonymize", dir.path(), "--out", out.path(), "--blackout", region});
            QVERIFY(p);
            QCOMPARE(p->exitCode(), 1);
            const QByteArray said = p->readAllStandardError();
            QVERIFY2(said.contains(message.toUtf8()), said.constData());
        }
        p = run({"anonymize", dir.path(), "--out", out.path(), "--blackout"});
        QVERIFY(p);
        QCOMPARE(p->exitCode(), 1);
        QCOMPARE(QDir(out.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), before);
    }
};

QTEST_GUILESS_MAIN(AnonymizeTests)
#include "anonymize_tests.moc"
