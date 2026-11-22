#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTranslator>
#include <QtTest>

#include "backend.h"
#include "framecache.h"
#include "library.h"
#include "notices.h"
#include "reportreader.h"
#include "seriesmodel.h"
#include "tagmodel.h"
#include "translator.h"
#include "volumeview.h"

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

    void readsNearestWhatEachPaneLooksAt() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 20));
        FrameCache cache;
        cache.setBudget(10 * SliceBytes);
        cache.reset(seriesIn(dir.path()));
        // Two panes far apart in one series, with room for ten slices:
        // each has its own slice and the two either side, not the first
        // ten of the series, nor the ten round whoever asked first.
        QObject left, right;
        cache.setInterest(&left, {0, 3, false, 0});
        cache.setInterest(&right, {0, 16, false, 0});
        QTRY_VERIFY(cache.idle() && cache.has(0, 14) && cache.has(0, 18) && cache.count(0) == 10);
        for (int i = 0; i < 20; ++i)
            QCOMPARE(cache.has(0, i), (i >= 1 && i <= 5) || (i >= 14 && i <= 18));
        QVERIFY(cache.used() <= cache.budget());
    }

    void takesItsBudgetFromTheMachineOrTheEnvironment() {
        // A third of the machine's memory, and never less than 512 MB.
        const QByteArray before = qgetenv("DICOLENS_CACHE_MB");
        qunsetenv("DICOLENS_CACHE_MB");
        QVERIFY(FrameCache::defaultBudget() >= (512ll << 20));
        qputenv("DICOLENS_CACHE_MB", "64");
        QCOMPARE(FrameCache::defaultBudget(), 64ll << 20);
        QCOMPARE(FrameCache().budget(), 64ll << 20);
        // Nonsense is no budget at all, so it's the machine's again.
        qputenv("DICOLENS_CACHE_MB", "lots");
        QVERIFY(FrameCache::defaultBudget() >= (512ll << 20));
        before.isNull() ? qunsetenv("DICOLENS_CACHE_MB") : qputenv("DICOLENS_CACHE_MB", before);
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

    void panesOnOneSeriesReadItOnce() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 12));
        QVERIFY(writeStack(dir.path(), "1.2.77.2", 6));
        const auto library = std::make_shared<Library>();
        FrameCache *cache = library->cache();
        Backend left(nullptr, library), right(nullptr, library);
        // Opened from one pane, it's open in both; the other shows none of
        // it, and reads none of it, until it's given a series.
        left.loadPaths({dir.path()});
        QVERIFY(left.waitForSlice(10000));
        QVERIFY(right.hasImages());
        QCOMPARE(right.seriesIndex(), -1);
        QCOMPARE(left.seriesIndex(), 0);
        QCOMPARE(left.sliceCount(), 12);
        // Both on one series, at different slices of it.
        right.selectSeries(0);
        right.setSliceIndex(9);
        QVERIFY(right.waitForSlice(10000));
        QTRY_VERIFY(cache->idle() && cache->complete(0));
        QCOMPARE(cache->decodes(), 12);
        QCOMPARE(left.decodedCount(), 12);
        QCOMPARE(right.decodedCount(), 12);
        QCOMPARE(left.picture().frame.value(0, 0), -1024.0);
        QCOMPARE(right.picture().frame.value(0, 0), 9 - 1024.0);
        // Put away, a pane keeps nothing in memory; shown beside the first
        // again, it keeps what it showed.
        QVERIFY(cache->pinned(0, 9));
        right.rest();
        QVERIFY(!cache->pinned(0, 9));
        right.share(&left);
        QCOMPARE(right.sliceIndex(), 9);
        QVERIFY(cache->pinned(0, 9));
        QVERIFY(right.frameReady());

        // A pane with a library of its own joins this one to go beside the
        // first, on the next series, which is the only one read for it.
        Backend third(nullptr);
        QVERIFY(third.library() != library.get());
        third.share(&left);
        QCOMPARE(third.library(), library.get());
        QCOMPARE(third.seriesIndex(), 1);
        QVERIFY(third.waitForSlice(10000));
        QTRY_VERIFY(cache->idle() && cache->complete(1));
        QCOMPARE(cache->decodes(), 18);
        // Shared again, it keeps what it shows.
        third.setSliceIndex(4);
        third.share(&left);
        QCOMPARE(third.seriesIndex(), 1);
        QCOMPARE(third.sliceIndex(), 4);

        // Cut another way for four views, it takes the first pane's
        // slices as they are: its picture is there at once.
        third.mirror(&left, 1);
        QCOMPARE(third.seriesIndex(), 0);
        QCOMPARE(third.plane(), 1);
        QVERIFY(third.frameReady());
        QCOMPARE(third.imageHeight(), 12);
        QCOMPARE(cache->decodes(), 18);
        QVERIFY(cache->idle());
    }

    void showsASeriesAlreadyReadAtOnce() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 8));
        QVERIFY(writeStack(dir.path(), "1.2.77.2", 8));
        const auto library = std::make_shared<Library>();
        FrameCache *cache = library->cache();
        Backend left(nullptr, library), right(nullptr, library);
        left.loadPaths({dir.path()});
        QVERIFY(left.waitForSlice(10000));
        QTRY_VERIFY(cache->idle() && cache->complete(0));
        QCOMPARE(cache->decodes(), 8);

        // Another pane picking it has its picture before any event is
        // handled, so before anything could have been read for it.
        right.selectSeries(0);
        QVERIFY(right.frameReady());
        QCOMPARE(right.imageWidth(), 16);
        QCOMPARE(right.decodedCount(), 8);
        QVERIFY(!right.image().isNull());
        QCOMPARE(cache->decodes(), 8);

        // And so has a pane coming back to a series it left.
        left.selectSeries(1);
        QVERIFY(!left.frameReady());
        QVERIFY(left.waitForSlice(10000));
        QTRY_VERIFY(cache->idle() && cache->complete(1));
        left.selectSeries(0);
        QVERIFY(left.frameReady());
        left.setSliceIndex(7);
        QVERIFY(left.frameReady());
        QCOMPARE(left.picture().frame.value(0, 0), 7 - 1024.0);
        QCOMPARE(cache->decodes(), 16);
    }

    void keepsTheLastPictureUpWhileASliceComesBack() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 20));
        const auto library = std::make_shared<Library>();
        FrameCache *cache = library->cache();
        cache->setBudget(8 * SliceBytes);
        Backend backend(nullptr, library);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QTRY_VERIFY(cache->idle() && cache->count(0) == 8);
        QCOMPARE(backend.decodedCount(), 8);
        // Far enough on that the first slice is let go of.
        backend.setSliceIndex(10);
        QVERIFY(backend.waitForSlice(10000));
        backend.setSliceIndex(19);
        QVERIFY(backend.waitForSlice(10000));
        QTRY_VERIFY(cache->idle());
        QVERIFY(!cache->has(0, 0));
        QVERIFY(cache->used() <= cache->budget());
        QCOMPARE(backend.decodedCount(), 8);

        // Back at it: slice 19's picture stays until slice 0 is read again.
        const int read = cache->decodes();
        backend.setSliceIndex(0);
        QVERIFY(!backend.frameReady());
        QCOMPARE(backend.imageWidth(), 16);
        QCOMPARE(backend.picture().frame.value(0, 0), 19 - 1024.0);
        QVERIFY(!backend.image().isNull());
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.picture().frame.value(0, 0), -1024.0);
        QVERIFY(cache->decodes() > read);
        // What the pane shows, and the slices either side, are kept.
        for (int i = 0; i <= FrameCache::Neighbours; ++i)
            QVERIFY(cache->pinned(0, i));
        QVERIFY(!cache->pinned(0, FrameCache::Neighbours + 1));
    }

    void refusesAVolumeTooBigForTheBudget() {
        QTemporaryDir dir;
        QVERIFY(writeStack(dir.path(), "1.2.77.1", 20));
        const auto library = std::make_shared<Library>();
        FrameCache *cache = library->cache();
        cache->setBudget(8 * SliceBytes);
        Backend backend(nullptr, library);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(backend.canReformat());
        const QString tooBig = "This series is too big to cut another way or show in 3D";

        // Cut another way, a slab, a tilt, and 3D each need all twenty
        // slices at once, and there's room for eight: each is refused
        // before a slice is read for it, and says why.
        QVERIFY(!backend.volumeFits());
        QVERIFY2(backend.problem().startsWith(tooBig), qPrintable(backend.problem()));
        backend.dismissProblem();
        backend.nextPlane();
        QCOMPARE(backend.plane(), 0);
        QVERIFY(backend.problem().startsWith(tooBig));
        backend.dismissProblem();
        backend.nextSlab();
        QCOMPARE(backend.slabName(), QString());
        QVERIFY(backend.problem().startsWith(tooBig));
        backend.dismissProblem();
        backend.tilt(0, 20);
        QCOMPARE(backend.plane(), 0);
        QVERIFY(backend.problem().startsWith(tooBig));
        backend.dismissProblem();
        VolumeView view;
        view.setSize({100, 100});
        view.setBackend(&backend);
        view.setActive(true);
        QVERIFY2(view.status().startsWith(tooBig), qPrintable(view.status()));
        QVERIFY(!view.busy());
        QVERIFY(!view.ready());

        // Refused, nothing is read round and round or let go of: the
        // slices that fit are read once, and the pane goes on showing them.
        QTRY_VERIFY(cache->idle() && cache->count(0) == 8);
        QTest::qWait(100);
        QCOMPARE(cache->decodes(), 8);
        QCOMPARE(cache->evictions(), 0);
        backend.setSliceIndex(19);
        QVERIFY(backend.waitForSlice(10000));
        QCOMPARE(backend.picture().frame.value(0, 0), 19 - 1024.0);
        QTRY_VERIFY(cache->idle());
        QVERIFY(cache->used() <= cache->budget());

        // With room for the series and the 3D view's volume beside it,
        // both are granted.
        cache->setBudget(64 * SliceBytes);
        backend.nextPlane();
        QCOMPARE(backend.plane(), 1);
        QVERIFY(backend.waitForSlice(10000));
        QVERIFY(backend.problem().isEmpty());
        QCOMPARE(backend.imageHeight(), 20);
        QVERIFY(view.waitForPicture(10000));
        QCOMPARE(view.status(), QString());
        QVERIFY(cache->complete(0));

        // And when the room goes from under a plane already cut, the pane
        // goes back to the slices as scanned and says why.
        cache->setBudget(8 * SliceBytes);
        backend.step(1);
        QCOMPARE(backend.plane(), 0);
        QVERIFY(backend.problem().startsWith(tooBig));
        QVERIFY(backend.waitForSlice(10000));
        QTRY_VERIFY(cache->idle());
        QVERIFY(cache->used() <= cache->budget());
        QVERIFY(view.status().startsWith(tooBig));
    }

    void makesThePictureOnlyWhenAsked() {
        QTemporaryDir dir;
        QVERIFY(writeVolume(dir.path()));
        Backend backend(nullptr);
        backend.loadPaths({dir.path()});
        QVERIFY(backend.waitForSlice(10000));
        // Its size, the slice, and the window are all known and changed
        // without the windowed picture being made.
        QCOMPARE(backend.imageWidth(), 4);
        QCOMPARE(backend.imageHeight(), 3);
        backend.preset(3);
        backend.step(1);
        QVERIFY(backend.waitForSlice(10000));
        backend.toggleInvert();
        QCOMPARE(backend.renders(), 0);

        // What a shader would draw from: the stored values and how to read
        // them, the window, and where the picture lies.
        const Picture picture = backend.picture();
        QVERIFY(!picture.isNull());
        QCOMPARE(picture.size(), QSize(4, 3));
        QCOMPARE(picture.frame.mono.size(), 12);
        QCOMPARE(picture.frame.stored(1, 2), 100 + 20 + 1);
        QVERIFY(!picture.frame.isSigned);
        QCOMPARE(picture.frame.slope, 1.0);
        QCOMPARE(picture.frame.intercept, 0.0);
        QCOMPARE(picture.center, -600.0);
        QCOMPARE(picture.width, 1500.0);
        QVERIFY(picture.inverted);
        QCOMPARE(picture.colorMap, 0);
        QVERIFY(!picture.pet);
        QVERIFY(picture.plane.valid);
        QCOMPARE(picture.plane.origin[2], 3.0);

        // Asked for, it's made once, and again only after something it's
        // made from has changed.
        const QImage image = backend.image();
        QCOMPARE(image.size(), QSize(4, 3));
        QCOMPARE(image, picture.render());
        QCOMPARE(backend.renders(), 1);
        backend.image();
        QCOMPARE(backend.renders(), 1);
        backend.toggleInvert();
        QCOMPARE(backend.renders(), 1);
        QVERIFY(backend.image() != image);
        QCOMPARE(backend.renders(), 2);
    }

    void saysANoticeAgainInTheLanguageSwitchedTo() {
        // A translator switched by hand, as the notices only ever see
        // QCoreApplication's.
        struct Danish : QTranslator {
            bool on = false;
            bool isEmpty() const override { return false; }
            QString translate(const char *, const char *source, const char *, int) const override {
                return on && qstrcmp(source, "PET off") == 0 ? QStringLiteral("PET fra") : QString();
            }
        } danish;
        QCoreApplication::installTranslator(&danish);
        Notices notices;
        QSignalSpy said(&notices, &Notices::noticeChanged);
        notices.flash([] { return QCoreApplication::translate("dicolens", "PET off"); });
        notices.setProblem([] { return QCoreApplication::translate("dicolens", "PET off") + "?"; });
        QCOMPARE(notices.notice(), QString("PET off"));
        danish.on = true;
        notices.retranslate();
        QCOMPARE(notices.notice(), QString("PET fra"));
        QCOMPARE(notices.problem(), QString("PET fra?"));
        QCOMPARE(said.count(), 2);
        // Words made elsewhere stay as they were given.
        notices.flashText("Scrolling together");
        notices.retranslate();
        QCOMPARE(notices.notice(), QString("Scrolling together"));
        // Dismissed, there's nothing to say again.
        notices.setProblem({});
        notices.retranslate();
        QCOMPARE(notices.problem(), QString());
        QCoreApplication::removeTranslator(&danish);
    }

    // Last, as it changes the language the numbers after it are written in.
    void saysAProblemInTheLanguageSwitchedTo() {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("language"), "en\n"));
        const QString missing = "%1 doesn't exist", none = "No DICOM images were found there";
        const QJsonObject english{{missing, missing}, {none, none}};
        const QJsonObject danish{{missing, "%1 findes ikke"}, {none, "Der blev ikke fundet DICOM-billeder der"}};
        QVERIFY(write(dir.filePath("en.json"), QJsonDocument(english).toJson()));
        QVERIFY(write(dir.filePath("da.json"), QJsonDocument(danish).toJson()));
        Translator translator(dir.filePath("language"), dir.path());
        QCoreApplication::installTranslator(&translator);
        // As main.cpp wires them.
        Backend backend(nullptr), other(nullptr);
        connect(&translator, &Translator::changed, &backend, [&] {
            backend.retranslate();
            other.retranslate();
        });

        // A problem made of English and what it's about, and one that is
        // only words.
        backend.loadPaths({"/no/such/scan"});
        QTRY_COMPARE(backend.problem(), QString("/no/such/scan doesn't exist"));
        QTemporaryDir empty;
        other.loadPaths({empty.path()});
        QTRY_COMPARE(other.problem(), QString("No DICOM images were found there"));

        QSignalSpy changed(&backend, &Backend::problemChanged);
        QVERIFY(write(dir.filePath("language"), "da\n"));
        QTRY_COMPARE(backend.problem(), QString("/no/such/scan findes ikke"));
        QCOMPARE(other.problem(), QString("Der blev ikke fundet DICOM-billeder der"));
        QVERIFY(changed.count() >= 1);
        // And back.
        QVERIFY(write(dir.filePath("language"), "en\n"));
        QTRY_COMPARE(backend.problem(), QString("/no/such/scan doesn't exist"));
        // Dismissed, a switch brings nothing back.
        backend.dismissProblem();
        QVERIFY(write(dir.filePath("language"), "da\n"));
        QTRY_COMPARE(other.problem(), QString("Der blev ikke fundet DICOM-billeder der"));
        QCOMPARE(backend.problem(), QString());
        QCoreApplication::removeTranslator(&translator);
        QLocale::setDefault(QLocale::system());
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    }
};

QTEST_MAIN(LibraryTests)
#include "library_tests.moc"
