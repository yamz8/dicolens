#include "tagmodel.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>
#include <functional>

QHash<int, QByteArray> TagModel::roleNames() const {
    return {{Depth, "depth"}, {Tag, "tag"}, {Name, "name"}, {Vr, "vr"}, {Value, "value"}, {Item, "item"}};
}

QVariant TagModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows[index.row()];
    switch (role) {
    case Depth: return row.depth;
    case Tag: return row.tag;
    case Name:
        if (!row.item)
            return row.name;
        return row.perFrame ? tr("Frame %1").arg(row.number) : tr("Item %1").arg(row.number);
    case Vr: return row.vr;
    case Value: return row.value;
    case Item: return row.item;
    }
    return {};
}

QList<TagModel::Row> TagModel::read(const DicomFile &file, int frame) {
    QList<Row> rows;
    std::function<void(const QList<DicomElement> &, int)> add = [&](const QList<DicomElement> &elements, int depth) {
        for (const auto &e : elements) {
            if (rows.size() >= MostRead)
                return;
            Row row;
            row.depth = depth;
            row.tag = tagText(e.tag);
            row.name = Dictionary::name(e.tag);
            row.vr = QString::fromLatin1(e.vr);
            row.value = file.display(e);
            rows << row;
            // Per-frame groups hold one item a frame; only the shown
            // frame's is worth reading.
            const bool perFrame = e.tag == ::Tag::PerFrameGroups;
            for (int i = 0; i < e.items.size(); ++i) {
                if (perFrame && i != frame)
                    continue;
                Row heading;
                heading.depth = depth + 1;
                heading.item = true;
                heading.perFrame = perFrame;
                heading.number = i + 1;
                rows << heading;
                add(e.items[i], depth + 2);
            }
        }
    };
    add(file.elements(), 0);
    return rows;
}

void TagModel::setWanted(bool wanted) {
    if (wanted == m_wanted)
        return;
    m_wanted = wanted;
    refresh();
}

void TagModel::setFilter(const QString &filter) {
    if (filter == m_filter)
        return;
    m_filter = filter;
    filterRows();
}

void TagModel::show(const QString &path, int frame) {
    if (path == m_path && frame == m_frame)
        return;
    m_path = path;
    m_frame = frame;
    refresh();
}

void TagModel::refresh() {
    // Whatever is being read is no longer what's wanted.
    ++m_generation;
    if (!m_wanted || m_path.isEmpty()) {
        m_all.clear();
        m_file.reset();
        m_filePath.clear();
        setRows({});
        return;
    }
    // One read at a time: the one under way starts the latest when it ends.
    if (!m_reading)
        start();
}

void TagModel::start() {
    m_reading = true;
    const auto kept = m_filePath == m_path ? m_file : nullptr;
    if (!kept)
        ++m_reads;
    QPointer<TagModel> self(this);
    QThreadPool::globalInstance()->start([self, kept, path = m_path, frame = m_frame, generation = m_generation] {
        const auto file = kept ? kept : std::make_shared<const DicomFile>(DicomFile::read(path));
        const QList<Row> rows = read(*file, frame);
        QMetaObject::invokeMethod(qApp, [self, file, path, rows, generation] {
            if (!self)
                return;
            self->m_reading = false;
            if (self->m_wanted && path == self->m_path) {
                self->m_file = file;
                self->m_filePath = path;
            }
            if (generation != self->m_generation) {
                // Another slice was asked for while this one was read.
                ++self->m_dropped;
                if (self->m_wanted && !self->m_path.isEmpty())
                    self->start();
                return;
            }
            self->m_all = rows;
            self->filterRows();
        }, Qt::QueuedConnection);
    });
}

void TagModel::filterRows() {
    const QString filter = m_filter.trimmed();
    QList<Row> shown;
    for (const Row &row : std::as_const(m_all)) {
        if (shown.size() >= MostShown)
            break;
        // A search lists what matches, without the items' headings.
        if (filter.isEmpty()
            || (!row.item && (row.tag.contains(filter, Qt::CaseInsensitive) || row.name.contains(filter, Qt::CaseInsensitive)
                              || row.value.contains(filter, Qt::CaseInsensitive))))
            shown << row;
    }
    setRows(shown);
}

void TagModel::setRows(const QList<Row> &rows) {
    // Slices of one series mostly differ in a few values: those rows
    // change, and the list stays where it was scrolled to.
    bool sameShape = rows.size() == m_rows.size();
    for (int i = 0; sameShape && i < rows.size(); ++i) {
        const Row &a = rows[i], &b = m_rows[i];
        sameShape = a.depth == b.depth && a.tag == b.tag && a.name == b.name && a.vr == b.vr && a.item == b.item
                    && a.perFrame == b.perFrame;
    }
    if (sameShape) {
        // The next frame's heading has the next number, and is the same row.
        for (int i = 0; i < rows.size(); ++i)
            if (rows[i].value != m_rows[i].value || rows[i].number != m_rows[i].number) {
                m_rows[i] = rows[i];
                emit dataChanged(index(i), index(i), {Name, Value});
            }
        return;
    }
    const bool resized = rows.size() != m_rows.size();
    beginResetModel();
    m_rows = rows;
    endResetModel();
    if (resized)
        emit countChanged();
}

void TagModel::retranslate() {
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {Name});
}
