#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "framecache.h"
#include "library.h"
#include "reportreader.h"
#include "seriesmodel.h"
#include "tagmodel.h"

#include "support.h"

// What the panes share and what each is made of: the cache of decoded
// slices and its budget, the library that every pane opens scans into, and
// the lists the window shows.

namespace {

// What one of the test series' slices takes in memory: 16 by 16 pixels of
// 16 bits.
constexpr qint64 SliceBytes = 16 * 16 * 2;

// A series of 16 × 16 slices, each pixel its slice's number.
bool writeStack(const QString &folder, const QByteArray &series, int slices) {
    return writeSeries(folder, "CT", series, "1.2.77.100", 16, slices, 1, 1, [](int, int, int k) { return k; });
}

QList<Series> seriesIn(const QString &folder) {
    return scanFiles(listFiles({folder}, true)).series;
}

// What a tag's row says, or nothing when the list has no such row.
QString tagValue(const TagModel &tags, const QString &tag) {
    for (int i = 0; i < tags.rowCount(); ++i)
        if (tags.data(tags.index(i), TagModel::Tag).toString() == tag)
            return tags.data(tags.index(i), TagModel::Value).toString();
    return {};
}

}

class LibraryTests : public QObject {
    Q_OBJECT

private slots:
    void readsASliceOnceForEveryoneLooking() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 12));
        FrameCache cache;
        cache.reset(seriesIn(dir.path()));
        // Two panes on one series, looking at different slices of it.
        QObject left, right;
        cache.setInterest(&left, {0, 2, false, 0});
        cache.setInterest(&right, {0, 9, false, 0});
        QTRY_VERIFY(cache.complete(0) && cache.idle());
        QCOMPARE(cache.decodes(), 12);
        for (int i = 0; i < 12; ++i)
            QCOMPARE(cache.frame(0, i)->value(0, 0), double(i - 1024));
        // Moving about, or wanting it whole, reads nothing again.
        cache.setInterest(&left, {0, 7, true, 0});
        cache.setInterest(&right, {0, 0, false, 0});
        QTest::qWait(50);
        QVERIFY(cache.idle());
        QCOMPARE(cache.decodes(), 12);
        // A pane that's gone wants nothing: its slices can be let go of.
        {
            QObject passing;
            cache.setInterest(&passing, {0, 11, false, 0});
            QVERIFY(cache.pinned(0, 11));
        }
        cache.dropInterest(&left);
        QVERIFY(!cache.pinned(0, 11));
        QVERIFY(cache.pinned(0, 2));
    }

    void keepsWithinItsBudget() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 20));
        FrameCache cache;
        cache.setBudget(8 * SliceBytes);
        cache.reset(seriesIn(dir.path()));
        QObject pane;
        // Read outward from the slice looked at, and no further than fits.
        cache.setInterest(&pane, {0, 0, false, 0});
        QTRY_VERIFY(cache.idle() && cache.count(0) == 8);
        for (int i = 0; i < 8; ++i)
            QVERIFY(cache.has(0, i));
        QCOMPARE(cache.evictions(), 0);
        QVERIFY(cache.used() <= cache.budget());

        // Looking elsewhere reads the slice there and its neighbours, and
        // lets go of the slices used longest ago: never of those.
        for (int i = 5; i < 8; ++i)
            cache.touch(0, i);
        cache.setInterest(&pane, {0, 15, false, 0});
        QTRY_VERIFY(cache.idle() && cache.has(0, 13) && cache.has(0, 14) && cache.has(0, 15) && cache.has(0, 16)
                    && cache.has(0, 17));
        for (int i = 13; i <= 17; ++i)
            QVERIFY(cache.pinned(0, i));
        QCOMPARE(cache.evictions(), 5);
        QCOMPARE(cache.count(0), 8);
        for (int i = 0; i < 5; ++i)
            QVERIFY(!cache.has(0, i));
        for (int i = 5; i < 8; ++i)
            QVERIFY(cache.has(0, i));
        QVERIFY(cache.used() <= cache.budget());

        // Full, it reads nothing more: a series bigger than the budget
        // isn't read round and round.
        const int read = cache.decodes();
        QCOMPARE(read, 13);
        QTest::qWait(100);
        QCOMPARE(cache.decodes(), read);

        // A slice let go of comes back when it's looked at again.
        cache.setInterest(&pane, {0, 0, false, 0});
        QTRY_VERIFY(cache.idle() && cache.has(0, 0) && cache.has(0, 1) && cache.has(0, 2));
        QCOMPARE(cache.frame(0, 0)->value(0, 0), -1024.0);
        QCOMPARE(cache.decodes(), read + 3);
        QVERIFY(cache.used() <= cache.budget());

        // What's looked at is kept even when the budget has no room for it.
        cache.setBudget(SliceBytes);
        QTRY_VERIFY(cache.idle());
        QCOMPARE(cache.count(0), 3);
        for (int i = 0; i < 3; ++i)
            QVERIFY(cache.has(0, i));
    }

    void grantsAWholeSeriesOnlyWhenItFits() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 20));
        QVERIFY(writeStack(dir.path(), "1.2.77.2", 20));
        FrameCache cache;
        cache.setBudget(10 * SliceBytes);
        cache.reset(seriesIn(dir.path()));
        QCOMPARE(cache.wholeBytes(0), 20 * SliceBytes);
        QObject pane, other;
        // Too big: refused before a slice is read, and the interest stands
        // as the slice looked at, so nothing is read only to be let go of.
        QVERIFY(!cache.fits(&pane, {0, 0, true, 0}));
        QVERIFY(!cache.setInterest(&pane, {0, 0, true, 0}));
        QTRY_VERIFY(cache.idle() && cache.count(0) == 10);
        QTest::qWait(100);
        QCOMPARE(cache.decodes(), 10);
        QCOMPARE(cache.evictions(), 0);

        // With room for it, it's read whole and none of it is let go of.
        cache.setBudget(20 * SliceBytes);
        QVERIFY(cache.setInterest(&pane, {0, 0, true, 0}));
        QTRY_VERIFY(cache.complete(0) && cache.idle());
        QCOMPARE(cache.evictions(), 0);
        // There's no room for a second beside it, or for anything made
        // from it that takes memory of its own.
        QVERIFY(!cache.setInterest(&other, {1, 0, true, 0}));
        QVERIFY(!cache.fits(&pane, {0, 0, true, 1}));
        QTRY_VERIFY(cache.idle());
        QVERIFY(cache.complete(0));
        // The second pane still has the slice it looks at.
        QVERIFY(cache.has(1, 0));
    }

    void readsTagsOffTheGuiThread() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        const QString first = dir.filePath("1.2.9.1-0"), second = dir.filePath("1.2.9.1-1");
        TagModel tags;
        QSignalSpy resets(&tags, &QAbstractItemModel::modelReset);
        tags.setWanted(true);
        tags.show(first, 0);
        // Nothing was read on this thread: the rows come later.
        QCOMPARE(tags.rowCount(), 0);
        QVERIFY(tags.reading());
        // Another slice asked for before the first came back: the first's
        // tags are dropped, never shown.
        tags.show(second, 0);
        QTRY_VERIFY(!tags.reading() && tags.rowCount() > 0);
        QCOMPARE(tags.dropped(), 1);
        QCOMPARE(tags.reads(), 2);
        QCOMPARE(resets.count(), 1);
        QCOMPARE(tagValue(tags, "(0020,0013)"), QString("2"));
        const QHash<int, QByteArray> roles = tags.roleNames();
        for (const char *role : {"depth", "tag", "name", "vr", "value", "item"})
            QVERIFY2(roles.values().contains(role), role);
        QCOMPARE(tags.data(tags.index(0), TagModel::Depth).toInt(), 0);
        QCOMPARE(tags.data(tags.index(0), TagModel::Item).toBool(), false);

        // The next slice has the same tags with other values: those rows
        // change, and the list isn't made again.
        const int rows = tags.rowCount();
        QSignalSpy changes(&tags, &QAbstractItemModel::dataChanged);
        tags.show(first, 0);
        QTRY_COMPARE(tagValue(tags, "(0020,0013)"), QString("1"));
        QCOMPARE(resets.count(), 1);
        QCOMPARE(tags.rowCount(), rows);
        QVERIFY(changes.count() > 0 && changes.count() < rows);
        // Shown again, nothing changes at all.
        changes.clear();
        tags.show(first, 0);
        QTest::qWait(50);
        QCOMPARE(changes.count(), 0);
        QCOMPARE(resets.count(), 1);

        // A search goes through the rows it has, at once, without the file.
        const int reads = tags.reads();
        tags.setFilter("instance number");
        QCOMPARE(tags.rowCount(), 1);
        QCOMPARE(tags.data(tags.index(0), TagModel::Tag).toString(), QString("(0020,0013)"));
        tags.setFilter("no tag says this");
        QCOMPARE(tags.rowCount(), 0);
        tags.setFilter("");
        QCOMPARE(tags.rowCount(), rows);
        QCOMPARE(tags.reads(), reads);
        QVERIFY(!tags.reading());

        // Closed, it holds nothing and reads nothing.
        tags.setWanted(false);
        QCOMPARE(tags.rowCount(), 0);
        tags.show(second, 0);
        QVERIFY(!tags.reading());
    }

    void stepsThroughFramesWithoutReadingTheFileAgain() {
        // An enhanced file's per-frame groups hold an item a frame.
        QTemporaryDir dir;
        QList<Writer> frames;
        for (const char *position : {"0\\0\\0", "0\\0\\5", "0\\0\\10"}) {
            Writer plane, group;
            plane.add(Tag::ImagePosition, "DS", position);
            group.sequence(0x00209113, {plane});
            frames << group;
        }
        Writer w;
        w.image(2, 2, 16, 12, false)
            .add(Tag::NumberOfFrames, "IS", "3")
            .sequence(Tag::PerFrameGroups, frames)
            .pixels(le16({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
        const QString path = dir.filePath("ENHANCED");
        QVERIFY(write(path, w.file(ExplicitLE)));

        TagModel tags;
        QSignalSpy resets(&tags, &QAbstractItemModel::modelReset);
        tags.setWanted(true);
        tags.show(path, 0);
        QTRY_VERIFY(!tags.reading() && tags.rowCount() > 0);
        QVERIFY(tagValue(tags, "(0020,0032)").endsWith("0"));
        auto heading = [&tags] {
            for (int i = 0; i < tags.rowCount(); ++i)
                if (tags.data(tags.index(i), TagModel::Item).toBool())
                    return tags.data(tags.index(i), TagModel::Name).toString();
            return QString();
        };
        QCOMPARE(heading(), QString("Frame 1"));
        // Held down, the key asks for frame after frame: the file is read
        // once, the frames between are skipped, and the last is shown.
        tags.show(path, 1);
        tags.show(path, 2);
        QTRY_VERIFY(!tags.reading() && tagValue(tags, "(0020,0032)").endsWith("10"));
        QCOMPARE(heading(), QString("Frame 3"));
        QCOMPARE(tags.reads(), 1);
        QCOMPARE(resets.count(), 1);
    }

    void listsSeriesAndChangesARowAtATime() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path(), "1.2.9.1"));
        QVERIFY(writeVolume(dir.path(), "1.2.9.2"));
        Library library;
        SeriesModel *series = library.seriesModel();
        QSignalSpy resets(series, &QAbstractItemModel::modelReset);
        QSignalSpy changes(series, &QAbstractItemModel::dataChanged);
        QSignalSpy opened(&library, &Library::opened);
        QObject asker;
        library.open({dir.path()}, &asker);
        QVERIFY(library.scanning());
        QTRY_COMPARE(opened.count(), 1);
        QVERIFY(!library.scanning());
        QCOMPARE(opened.first().at(0).value<QObject *>(), &asker);
        QCOMPARE(library.serial(), 1);

        QCOMPARE(series->rowCount(), 2);
        QCOMPARE(series->count(), 2);
        const QHash<int, QByteArray> roles = series->roleNames();
        for (const char *role : {"title", "modality", "images", "number", "study", "studyDate", "decodable", "thumbnail"})
            QVERIFY2(roles.values().contains(role), role);
        QCOMPARE(series->data(series->index(0), SeriesModel::Title).toString(), QString("Untitled series"));
        QCOMPARE(series->title(1), QString("Untitled series"));
        QCOMPARE(series->data(series->index(0), SeriesModel::Modality).toString(), QString("CT"));
        QCOMPARE(series->data(series->index(1), SeriesModel::Images).toInt(), 5);
        QVERIFY(series->data(series->index(0), SeriesModel::Decodable).toBool());
        // The study's heading is on its first series only.
        QCOMPARE(series->data(series->index(0), SeriesModel::Study).toString(), QString("Study"));
        QCOMPARE(series->data(series->index(1), SeriesModel::Study).toString(), QString());

        // Each thumbnail changes its own row, and the list isn't made again.
        QTRY_VERIFY(!series->data(series->index(0), SeriesModel::Thumbnail).toString().isEmpty()
                    && !series->data(series->index(1), SeriesModel::Thumbnail).toString().isEmpty());
        QCOMPARE(resets.count(), 1);
        QCOMPARE(changes.count(), 2);
        for (const auto &change : changes) {
            QCOMPARE(change.at(0).toModelIndex(), change.at(1).toModelIndex());
            QCOMPARE(change.at(2).value<QList<int>>(), QList<int>{SeriesModel::Thumbnail});
        }
        QVERIFY(!library.thumbnail(0).isNull() && !library.thumbnail(1).isNull());
        QVERIFY(library.thumbnail(2).isNull());
    }

    void marksTheReportsOfTheStudyShown() {
        Report pdf, structured;
        pdf.kind = Report::Kind::Pdf;
        pdf.studyUid = "1.2.1";
        pdf.studyDescription = "Chest";
        structured.kind = Report::Kind::Structured;
        structured.title = "Findings";
        structured.studyUid = "1.2.2";
        ReportModel reports;
        reports.reset({pdf, structured});
        QCOMPARE(reports.rowCount(), 2);
        const QHash<int, QByteArray> roles = reports.roleNames();
        for (const char *role : {"title", "date", "pdf", "study", "current"})
            QVERIFY2(roles.values().contains(role), role);
        QCOMPARE(reports.data(reports.index(0), ReportModel::Title).toString(), QString("Report"));
        QCOMPARE(reports.data(reports.index(1), ReportModel::Title).toString(), QString("Findings"));
        QVERIFY(reports.data(reports.index(0), ReportModel::Pdf).toBool());
        QCOMPARE(reports.data(reports.index(0), ReportModel::Study).toString(), QString("Chest"));
        QVERIFY(!reports.data(reports.index(0), ReportModel::Current).toBool());

        // Another series shown changes which are its study's, in place.
        QSignalSpy resets(&reports, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&reports, &QAbstractItemModel::dataChanged);
        reports.setStudy("1.2.2", true);
        QVERIFY(!reports.data(reports.index(0), ReportModel::Current).toBool());
        QVERIFY(reports.data(reports.index(1), ReportModel::Current).toBool());
        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.first().at(2).value<QList<int>>(), QList<int>{ReportModel::Current});
        reports.setStudy("1.2.2", true);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(resets.count(), 0);
    }
};

QTEST_MAIN(LibraryTests)
#include "library_tests.moc"
