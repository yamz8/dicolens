#include "measurestore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

MeasureStore &MeasureStore::instance() {
    static MeasureStore store;
    return store;
}

MeasureStore::MeasureStore() {
    m_folder = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/dicolens/measurements";
}

void MeasureStore::setFolder(const QString &folder) {
    m_folder = folder;
    m_series.clear();
    m_loaded.clear();
    emit changed();
}

QString MeasureStore::path(const QString &series) const {
    // UIDs are digits and dots, but a file name shouldn't trust that.
    const QByteArray hash = QCryptographicHash::hash(series.toUtf8(), QCryptographicHash::Sha1).toHex();
    return m_folder + "/" + QString::fromLatin1(hash) + ".json";
}

void MeasureStore::load(const QString &series) {
    if (series.isEmpty() || m_loaded.contains(series))
        return;
    m_loaded.insert(series);
    QFile file(path(series));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value("series").toString() != series)
        return;
    auto &byPicture = m_series[series];
    for (const QJsonValue &v : root.value("measurements").toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonArray p = o.value("points").toArray();
        if (p.size() != 8)
            continue;
        Measure m;
        m.kind = o.value("kind").toInt(1);
        m.a = QPointF(p[0].toDouble(), p[1].toDouble());
        m.b = QPointF(p[2].toDouble(), p[3].toDouble());
        m.c = QPointF(p[4].toDouble(), p[5].toDouble());
        m.d = QPointF(p[6].toDouble(), p[7].toDouble());
        byPicture[slot(o.value("plane").toInt(), o.value("slice").toInt(), {})] << m;
    }
}

void MeasureStore::save(const QString &series) {
    if (series.isEmpty())
        return;
    QJsonArray list;
    const auto &byPicture = m_series.value(series);
    for (auto it = byPicture.begin(); it != byPicture.end(); ++it) {
        const int plane = it.key().section('/', 0, 0).toInt(), slice = it.key().section('/', 1, 1).toInt();
        if (plane == 3)
            continue;
        for (const Measure &m : it.value())
            list << QJsonObject{{"plane", plane},
                                {"slice", slice},
                                {"kind", m.kind},
                                {"points", QJsonArray{m.a.x(), m.a.y(), m.b.x(), m.b.y(), m.c.x(), m.c.y(), m.d.x(), m.d.y()}}};
    }
    const QString file = path(series);
    if (list.isEmpty()) {
        QFile::remove(file);
        return;
    }
    QDir().mkpath(m_folder);
    QSaveFile out(file);
    out.setDirectWriteFallback(false);
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(QJsonObject{{"series", series}, {"measurements", list}}).toJson(QJsonDocument::Compact));
        out.commit();
    }
}

QList<Measure> MeasureStore::at(const QString &series, int plane, int slice, const QString &cut) {
    load(series);
    return m_series.value(series).value(slot(plane, slice, cut));
}

void MeasureStore::add(const QString &series, int plane, int slice, const Measure &measure, const QString &cut) {
    load(series);
    m_series[series][slot(plane, slice, cut)] << measure;
    save(series);
    emit changed();
}

bool MeasureStore::removeLast(const QString &series, int plane, int slice, const QString &cut) {
    load(series);
    auto &list = m_series[series][slot(plane, slice, cut)];
    if (list.isEmpty())
        return false;
    list.removeLast();
    save(series);
    emit changed();
    return true;
}

QList<MeasureStore::Placed> MeasureStore::all(const QString &series) {
    load(series);
    QList<Placed> list;
    const auto &byPicture = m_series.value(series);
    for (auto it = byPicture.begin(); it != byPicture.end(); ++it)
        for (const Measure &m : it.value())
            list << Placed{it.key().section('/', 0, 0).toInt(), it.key().section('/', 1, 1).toInt(), m};
    // Within a picture the order is kept: the sort is stable.
    std::stable_sort(list.begin(), list.end(), [](const Placed &x, const Placed &y) {
        return x.plane != y.plane ? x.plane < y.plane : x.slice < y.slice;
    });
    return list;
}

void MeasureStore::clear(const QString &series, int plane, int slice, const QString &cut) {
    load(series);
    if (m_series[series].remove(slot(plane, slice, cut)) == 0)
        return;
    save(series);
    emit changed();
}
