#include "seriesmodel.h"

#include "pixels.h"
#include "readout.h"

QHash<int, QByteArray> SeriesModel::roleNames() const {
    return {{Title, "title"}, {Modality, "modality"}, {Images, "images"}, {Number, "number"},
            {Study, "study"}, {StudyDate, "studyDate"}, {Decodable, "decodable"}, {Thumbnail, "thumbnail"}};
}

QVariant SeriesModel::data(const QModelIndex &index, int role) const {
    const int row = index.row();
    if (!index.isValid() || row < 0 || row >= m_series.size())
        return {};
    const Series &s = m_series[row];
    // The first series of each study carries its heading.
    const bool heads = row == 0 || s.studyUid + s.patientName != m_series[row - 1].studyUid + m_series[row - 1].patientName;
    switch (role) {
    case Title: return titleOf(s);
    case Modality: return s.modality;
    case Images: return int(s.slices.size());
    case Number: return s.number;
    case Study: return !heads ? QString() : s.studyDescription.isEmpty() ? tr("Study") : s.studyDescription;
    case StudyDate: return heads ? Readout::date(s.studyDate) : QString();
    case Decodable: return canDecode(s.transferSyntax);
    case Thumbnail:
        return m_thumbnails.value(row) == 0
                   ? QString()
                   : QStringLiteral("image://thumbnails/%1/%2-%3").arg(row).arg(m_scans).arg(m_thumbnails[row]);
    }
    return {};
}

QString SeriesModel::titleOf(const Series &series) {
    if (!series.description.isEmpty())
        return series.description;
    return series.number.isEmpty() ? tr("Untitled series") : tr("Series %1").arg(series.number);
}

void SeriesModel::reset(const QList<Series> &series) {
    beginResetModel();
    m_series = series;
    m_thumbnails.fill(0, series.size());
    ++m_scans;
    endResetModel();
    emit countChanged();
}

void SeriesModel::thumbnailArrived(int row) {
    if (row < 0 || row >= m_thumbnails.size())
        return;
    ++m_thumbnails[row];
    emit dataChanged(index(row), index(row), {Thumbnail});
}

void SeriesModel::retranslate() {
    if (!m_series.isEmpty())
        emit dataChanged(index(0), index(m_series.size() - 1), {Title, Study, StudyDate});
}
