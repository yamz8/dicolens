#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtTest>
#include <atomic>
#include <thread>

#include "dicom.h"

#include "support.h"

// The registry of tags and UIDs, which comes from the standard's own tables
// (src/data, written by tools/standard.py): that it's all there, that it
// answers as the list kept by hand did, and what it does where the standard
// leaves a choice.
class DictionaryTests : public QObject {
    Q_OBJECT

private slots:
    // First, so the threads are the ones that find the tables unread:
    // decoding asks from worker threads, and whichever asks first reads
    // them while the others wait.
    void answersFromManyThreadsAtOnce() {
        std::atomic<int> wrong = 0;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&wrong, t] {
                for (int i = 0; i < 2000; ++i) {
                    if (Dictionary::name(Tag::PatientName) != "Patient's Name" || Dictionary::vr(Tag::Rows) != "US")
                        ++wrong;
                    if (Dictionary::vr(0x60000010 + quint32((i + t) % 16) * 0x20000) != "US")
                        ++wrong;
                    if (Dictionary::transferSyntaxName("1.2.840.10008.1.2.5") != "RLE Lossless")
                        ++wrong;
                    if (Dictionary::name(0x00090010 + quint32(i)) != "Private")
                        ++wrong;
                }
            });
        for (auto &thread : threads)
            thread.join();
        QCOMPARE(wrong.load(), 0);
    }

    void holdsTheWholeRegistry() {
        // Part 6 has grown with every edition; a table cut short would
        // read as tags without names.
        QVERIFY2(Dictionary::count() >= 5300, qPrintable(QString::number(Dictionary::count())));
        QVERIFY2(QRegularExpression("^20[0-9]{2}[a-e]$").match(Dictionary::edition()).hasMatch(),
                 qPrintable(Dictionary::edition()));
    }

    void namesTagsAsTheStandardDoes_data() {
        QTest::addColumn<quint32>("tag");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("vr");
        // What the app reads by tag, and what its own list once named.
        const struct { quint32 tag; const char *name; const char *vr; } known[] = {
            {0x00020010, "Transfer Syntax UID", "UI"},
            {0x00080005, "Specific Character Set", "CS"},
            {0x00080018, "SOP Instance UID", "UI"},
            {0x00080020, "Study Date", "DA"},
            {0x00080060, "Modality", "CS"},
            {0x00080080, "Institution Name", "LO"},
            {0x00081030, "Study Description", "LO"},
            {0x0008103E, "Series Description", "LO"},
            {0x00081140, "Referenced Image Sequence", "SQ"},
            {0x00082228, "Primary Anatomic Structure Sequence", "SQ"},
            {0x00100010, "Patient's Name", "PN"},
            {0x00100020, "Patient ID", "LO"},
            {0x00100030, "Patient's Birth Date", "DA"},
            {0x00101010, "Patient's Age", "AS"},
            {0x00181030, "Protocol Name", "LO"},
            {0x0018602C, "Physical Delta X", "FD"},
            {0x0020000D, "Study Instance UID", "UI"},
            {0x00200032, "Image Position (Patient)", "DS"},
            {0x00280009, "Frame Increment Pointer", "AT"},
            {0x00280010, "Rows", "US"},
            {0x00281052, "Rescale Intercept", "DS"},
            {0x0040A730, "Content Sequence", "SQ"},
            {0x00420011, "Encapsulated Document", "OB"},
            {0x00700022, "Graphic Data", "FL"},
            {0x20500020, "Presentation LUT Shape", "CS"},
            {0x52009229, "Shared Functional Groups Sequence", "SQ"},
            {0x52009230, "Per-Frame Functional Groups Sequence", "SQ"},
            {0x7FE00001, "Extended Offset Table", "OV"},
            {0xFFFAFFFA, "Digital Signatures Sequence", "SQ"},
            // Beyond what the hand list had: retired, and a name with a
            // letter Latin-1 has and ASCII doesn't.
            {0x00080001, "Length to End", "UL"},
            {0x00181153, "Exposure in µAs", "IS"},
        };
        for (const auto &k : known)
            QTest::newRow(k.name) << k.tag << QString::fromUtf8(k.name) << QByteArray(k.vr);
    }

    void namesTagsAsTheStandardDoes() {
        QFETCH(quint32, tag);
        QFETCH(QString, name);
        QFETCH(QByteArray, vr);
        QCOMPARE(Dictionary::name(tag), name);
        QCOMPARE(Dictionary::vr(tag), vr);
    }

    void knowsWhatIsRetired() {
        QVERIFY(Dictionary::retired(0x00080001));     // Length to End
        QVERIFY(Dictionary::retired(0x00101000));     // Other Patient IDs
        QVERIFY(Dictionary::retired(0x50003000));     // Curve Data
        QVERIFY(!Dictionary::retired(Tag::PatientName));
        QVERIFY(!Dictionary::retired(0x12345678));
    }

    void namesGroupLengthsAndPrivateTags() {
        // Element 0000 of any group is its length, though the standard no
        // longer lists it; the file meta group's has a name of its own.
        QCOMPARE(Dictionary::name(0x00080000), QString("Group Length"));
        QCOMPARE(Dictionary::vr(0x00080000), QByteArray("UL"));
        QCOMPARE(Dictionary::name(0x00090000), QString("Group Length"));
        QCOMPARE(Dictionary::vr(0x00090000), QByteArray("UL"));
        QCOMPARE(Dictionary::name(0x00020000), QString("File Meta Information Group Length"));
        // Odd groups are a vendor's own: named as private, with no VR to read by.
        QCOMPARE(Dictionary::name(0x00091001), QString("Private"));
        QVERIFY(Dictionary::vr(0x00091001).isEmpty());
        // A tag nobody has assigned has neither.
        QVERIFY(Dictionary::name(0x00082299).isEmpty());
        QVERIFY(Dictionary::vr(0x00082299).isEmpty());
    }

    void repeatsGroupsAndElements() {
        // Overlay planes are in every even group from 6000 to 601E, and
        // retired curves from 5000 on.
        QCOMPARE(Dictionary::name(0x60000010), QString("Overlay Rows"));
        QCOMPARE(Dictionary::name(0x60023000), QString("Overlay Data"));
        QCOMPARE(Dictionary::vr(0x601E0050), QByteArray("SS"));
        QCOMPARE(Dictionary::name(0x50103000), QString("Curve Data"));
        QCOMPARE(Dictionary::name(0x7F000010), QString("Variable Pixel Data"));
        // The tag one of them repeats beside is still its own.
        QCOMPARE(Dictionary::name(Tag::PixelData), QString("Pixel Data"));
        // An odd group among them is private like any other.
        QCOMPARE(Dictionary::name(0x60013000), QString("Private"));
        QVERIFY(Dictionary::vr(0x60013000).isEmpty());
        // And the few that repeat in the element: (0020,31xx), (0028,04x0), (1000,xxx0).
        QCOMPARE(Dictionary::name(0x002031AB), QString("Source Image IDs"));
        QCOMPARE(Dictionary::name(0x00280410), QString("Rows For Nth Order Coefficients"));
        QCOMPARE(Dictionary::name(0x00280431), QString("Columns For Nth Order Coefficients"));
        // (0028,0400) is one of those by its digits, and a tag of its own.
        QCOMPARE(Dictionary::name(0x00280400), QString("Transform Label"));
        QCOMPARE(Dictionary::name(0x10000120), QString("Escape Triplet"));
    }

    void choosesAmongSeveralVrs() {
        // Where the standard allows several, an implicit file is read by
        // one: OW when that's among them, and US for "US or SS".
        QCOMPARE(Dictionary::vrs(Tag::PixelData), QString("OB or OW"));
        QCOMPARE(Dictionary::vr(Tag::PixelData), QByteArray("OW"));
        QCOMPARE(Dictionary::vrs(0x60003000), QString("OB or OW"));
        QCOMPARE(Dictionary::vr(0x60003000), QByteArray("OW"));
        QCOMPARE(Dictionary::vrs(Tag::PixelPaddingValue), QString("US or SS"));
        QCOMPARE(Dictionary::vr(Tag::PixelPaddingValue), QByteArray("US"));
        QCOMPARE(Dictionary::vr(Tag::LutDescriptor), QByteArray("US"));
        QCOMPARE(Dictionary::vr(Tag::RedPaletteDescriptor), QByteArray("US"));
        QCOMPARE(Dictionary::vrs(0x00283006), QString("US or OW"));
        QCOMPARE(Dictionary::vr(0x00283006), QByteArray("OW"));
        QCOMPARE(Dictionary::vrs(0x00281200), QString("US or SS or OW"));
        QCOMPARE(Dictionary::vr(0x00281200), QByteArray("OW"));
        QCOMPARE(Dictionary::vrs(Tag::Rows), QString("US"));
        // Item delimiters have none, and no row of the table is read as one.
        QVERIFY(Dictionary::vr(Tag::Item).isEmpty());
        QCOMPARE(Dictionary::name(Tag::Item), QString("Item"));

        // The padding of signed pixels reads as US, and is made signed by
        // who uses it.
        Writer w;
        w.explicitVr = false;
        w.image(1, 2, 16, 16, true);
        w.add(Tag::PixelPaddingValue, "SS", le16({0x8000}));
        w.pixels(le16({0x8000, 5}));
        const DicomFile file = DicomFile::parse(w.file("1.2.840.10008.1.2"));
        QVERIFY(file.isValid());
        QCOMPARE(file.element(Tag::PixelPaddingValue)->vr, QByteArray("US"));
        QCOMPARE(file.integer(Tag::PixelPaddingValue), 0x8000);
    }

    void namesUids() {
        QCOMPARE(Dictionary::transferSyntaxName("1.2.840.10008.1.2"), QString("Implicit VR Little Endian"));
        QCOMPARE(Dictionary::transferSyntaxName("1.2.840.10008.1.2.1"), QString("Explicit VR Little Endian"));
        QCOMPARE(Dictionary::transferSyntaxName("1.2.840.10008.1.2.4.50"), QString("JPEG Baseline (Process 1)"));
        QCOMPARE(Dictionary::transferSyntaxName("1.2.840.10008.1.2.4.90"), QString("JPEG 2000 Image Compression (Lossless Only)"));
        QCOMPARE(Dictionary::transferSyntaxName("1.2.840.10008.1.2.5"), QString("RLE Lossless"));
        QCOMPARE(Dictionary::sopClassName("1.2.840.10008.5.1.4.1.1.2"), QString("CT Image Storage"));
        QCOMPARE(Dictionary::sopClassName("1.2.840.10008.5.1.4.1.1.88.11"), QString("Basic Text SR Storage"));
        // Each answers for its own kind only, as a value's meaning is told
        // by asking both.
        QVERIFY(Dictionary::sopClassName("1.2.840.10008.1.2.5").isEmpty());
        QVERIFY(Dictionary::transferSyntaxName("1.2.840.10008.5.1.4.1.1.2").isEmpty());
        QVERIFY(Dictionary::transferSyntaxName("1.2.3").isEmpty());
        QCOMPARE(Dictionary::uidName("1.2.840.10008.5.1.4.1.1.2"), QString("CT Image Storage"));
        QCOMPARE(Dictionary::uidName("1.2.840.10008.1.5.1"), QString("Hot Iron Color Palette SOP Instance"));
        QVERIFY(Dictionary::uidName("1.2.3").isEmpty());

        // A tag's UI value shows with what it names.
        Writer w;
        w.add(Tag::SopClass, "UI", "1.2.840.10008.5.1.4.1.1.2");
        const DicomFile file = DicomFile::parse(w.file("1.2.840.10008.1.2.4.50"));
        QCOMPARE(file.display(*file.element(Tag::SopClass)), QString("1.2.840.10008.5.1.4.1.1.2 (CT Image Storage)"));
        QCOMPARE(file.display(*file.element(Tag::TransferSyntax)), QString("1.2.840.10008.1.2.4.50 (JPEG Baseline (Process 1))"));
    }

    void readsImplicitFilesByIt() {
        // An implicit file says no value's kind: names, numbers, floats and
        // sequences all come out by what the registry says of their tags,
        // the ones the hand list never had among them.
        Writer inner;
        inner.explicitVr = false;
        inner.add(0x00080100, "SH", "T-D1100").add(Tag::CodeMeaning, "LO", "Head");
        Writer w;
        w.explicitVr = false;
        w.add(Tag::Modality, "CS", "MR")
            .sequence(0x00082228, {inner})
            .add(0x00101005, "PN", "Roe^Jane")
            .fd(0x00189087, 1000.5)   // Diffusion b-value
            .us(Tag::Rows, 2);
        const DicomFile file = DicomFile::parse(w.file("1.2.840.10008.1.2"));
        QVERIFY(file.isValid());
        QCOMPARE(file.element(0x00082228)->vr, QByteArray("SQ"));
        QCOMPARE(file.element(0x00082228)->items.size(), 1);
        QCOMPARE(file.element(0x00082228)->items.first().last().vr, QByteArray("LO"));
        QCOMPARE(file.element(0x00101005)->vr, QByteArray("PN"));
        QCOMPARE(file.element(0x00189087)->vr, QByteArray("FD"));
        QCOMPARE(file.number(0x00189087), 1000.5);
        QCOMPARE(file.integer(Tag::Rows), 2);
        // A private tag has no kind to be read by, and stays bytes.
        Writer p;
        p.explicitVr = false;
        p.add(0x00091001, "LO", "secret");
        QCOMPARE(DicomFile::parse(p.add(Tag::Modality, "CS", "CT").file("1.2.840.10008.1.2")).element(0x00091001)->vr,
                 QByteArray("UN"));
    }

    void readsTheTablesQuickly() {
        // The tables were read by the first test; reading them again from
        // nothing says what the first tag of an implicit file waits for.
        // Far under a frame is the point, not the number.
        QElapsedTimer timer;
        timer.start();
        int rows = 0;
        Dictionary::table(":/data/elements.tsv", nullptr, [&](const QList<QByteArrayView> &) { ++rows; });
        Dictionary::table(":/data/uids.tsv", nullptr, [&](const QList<QByteArrayView> &) { ++rows; });
        Dictionary::table(":/data/confidentiality.tsv", nullptr, [&](const QList<QByteArrayView> &) { ++rows; });
        QVERIFY2(rows > 6000, qPrintable(QString::number(rows)));
        QVERIFY2(timer.elapsed() < 250, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));
        // A lookup is a search of a sorted list.
        timer.restart();
        int named = 0;
        for (quint32 tag = 0x00080000; tag < 0x00090000; ++tag)
            named += Dictionary::vr(tag).isEmpty() ? 0 : 1;
        QVERIFY2(named > 150, qPrintable(QString::number(named)));
        QVERIFY2(timer.elapsed() < 500, qPrintable(QStringLiteral("%1 ms").arg(timer.elapsed())));
    }

    void tablesAreSortedAndWhole() {
        // The tables are data kept in the repository; one edited by hand,
        // or cut by a merge, shows here rather than as a tag gone missing.
        QString edition;
        QList<QByteArray> tags;
        Dictionary::table(":/data/elements.tsv", &edition, [&](const QList<QByteArrayView> &cells) {
            quint32 mask = 0, digits = 0;
            QVERIFY2(cells.size() == 4, cells.value(0).toByteArray().constData());
            QVERIFY2(Dictionary::pattern(cells[0], &mask, &digits), cells[0].toByteArray().constData());
            tags << cells[0].toByteArray();
        });
        QCOMPARE(edition, Dictionary::edition());
        QCOMPARE(tags.size(), Dictionary::count());
        QVERIFY(std::is_sorted(tags.begin(), tags.end()));
        QVERIFY(std::adjacent_find(tags.begin(), tags.end()) == tags.end());

        int rows = 0;
        Dictionary::table(":/data/confidentiality.tsv", nullptr, [&](const QList<QByteArrayView> &cells) {
            QVERIFY2(cells.size() == 15, cells.value(0).toByteArray().constData());
            // The basic profile says something of every row.
            QVERIFY2(!cells[1].isEmpty(), cells[0].toByteArray().constData());
            ++rows;
        });
        QVERIFY2(rows >= 650, qPrintable(QString::number(rows)));
        quint32 mask = 0, digits = 0;
        QVERIFY(Dictionary::pattern("60xx3000", &mask, &digits));
        QCOMPARE(mask, 0xFF00FFFFu);
        QCOMPARE(digits, 0x60003000u);
        QVERIFY(!Dictionary::pattern("private", &mask, &digits));
        QVERIFY(!Dictionary::pattern("0008005G", &mask, &digits));
    }
};

QTEST_GUILESS_MAIN(DictionaryTests)
#include "dictionary_tests.moc"
