#pragma once

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <atomic>
#include <memory>

#include "anonymize.h"
#include "filepicker.h"
#include "library.h"
#include "notices.h"

// An anonymous copy as the window offers it. Before anything is written it
// works out what a copy would do, with the function that makes one: how
// many files, what becomes of each attribute, what's left out, which
// pictures may show the patient, which regions are blacked out, and what
// the check would refuse. It has one choice, whether the descriptions
// stay. Once a folder is chosen the copy is made, and how it went stays
// here until it's dismissed. The plan and the copy are both a worker's.
class AnonymousCopy : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The anonymous copy comes from the backend")
    Q_PROPERTY(bool shown READ shown NOTIFY changed)
    Q_PROPERTY(Stage stage READ stage NOTIFY changed)
    Q_PROPERTY(bool keepDescriptions READ keepDescriptions WRITE setKeepDescriptions NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(int files READ files NOTIFY changed)
    Q_PROPERTY(QVariantList removed READ removed NOTIFY changed)
    Q_PROPERTY(QVariantList emptied READ emptied NOTIFY changed)
    Q_PROPERTY(QVariantList replaced READ replaced NOTIFY changed)
    Q_PROPERTY(QVariantList kept READ kept NOTIFY changed)
    Q_PROPERTY(int reports READ reports NOTIFY changed)
    Q_PROPERTY(int annotations READ annotations NOTIFY changed)
    Q_PROPERTY(QVariantList pictures READ pictures NOTIFY changed)
    Q_PROPERTY(int regions READ regions NOTIFY changed)
    Q_PROPERTY(int blackedOut READ blackedOut NOTIFY changed)
    Q_PROPERTY(int unhidden READ unhidden NOTIFY changed)
    Q_PROPERTY(QVariantList refused READ refused NOTIFY changed)
    Q_PROPERTY(int refusedFiles READ refusedFiles NOTIFY changed)
    Q_PROPERTY(QVariantList uncopied READ uncopied NOTIFY changed)
    Q_PROPERTY(int uncopiedFiles READ uncopiedFiles NOTIFY changed)
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
    Q_PROPERTY(QString folderName READ folderName NOTIFY changed)
    Q_PROPERTY(QString failure READ failure NOTIFY changed)
    Q_PROPERTY(QString edition READ edition CONSTANT)

public:
    // Nothing asked for; a plan being worked out; the plan, ready to look
    // through; the copy being written; and how it went.
    enum Stage { Closed, Planning, Planned, Saving, Saved };
    Q_ENUM(Stage)

    // Without a chooser there's nowhere to ask, and nothing is saved.
    explicit AnonymousCopy(FilePicker *picker, QObject *parent = nullptr);
    ~AnonymousCopy() override;

    // Whose scan is copied, with the regions marked to black out of it.
    void setLibrary(Library *library);

    bool shown() const { return m_shown; }
    Stage stage() const { return m_stage; }
    bool keepDescriptions() const { return m_options.keepDescriptions; }
    // Works the plan out again: what it says depends on it.
    void setKeepDescriptions(bool keep);
    // Only a plan that's ready, with something in it to copy, can be saved.
    bool canSave() const { return m_picker && m_stage == Planned && m_result.written > 0; }

    // What the plan or the copy comes to. The files that would be copied,
    // or were.
    int files() const { return m_result.written; }
    // What becomes of each attribute, as rows of name, tag, and files: how
    // many files have it.
    QVariantList removed() const { return counts(m_result.summary.removed); }
    QVariantList emptied() const { return counts(m_result.summary.emptied); }
    QVariantList replaced() const { return counts(m_result.summary.replaced); }
    QVariantList kept() const { return counts(m_result.summary.kept); }
    // Left out whole, as their text names the patient.
    int reports() const { return m_result.skipped; }
    int annotations() const { return m_result.annotations; }
    // The series whose pictures may show the patient's details, and those
    // with regions to black out, as rows of name, modality, images (how
    // many its series has), flagged (how many of them may show details),
    // and regions.
    QVariantList pictures() const;
    // Regions marked in all, and the images one was blacked out of.
    int regions() const;
    int blackedOut() const { return m_result.blackedOut; }
    // Images that may show the patient's details, in series with no region.
    int unhidden() const;
    // Copies the check refuses, by what it found and where, as rows of
    // name, tag, what, found, files, and series; and how many files those are.
    QVariantList refused() const;
    int refusedFiles() const;
    // Files not copied for any other reason, as rows of reason, files, and
    // series.
    QVariantList uncopied() const;
    int uncopiedFiles() const { return int(m_result.uncopied.size()) - refusedFiles(); }
    // Where the copy went.
    QString folder() const { return m_result.folder; }
    QString folderName() const;
    // Why nothing could be written there at all, when that's how it went.
    QString failure() const;
    QString edition() const { return Anonymize::edition(); }

    // Ctrl+Shift+S: shows what a copy of the open scan would do.
    Q_INVOKABLE void begin();
    // Asks where to save, and saves what was planned once that's answered.
    Q_INVOKABLE void choose();
    // Puts it away. A plan is dropped and nothing is written; a copy being
    // written goes on, and comes back if there's something to say of it.
    Q_INVOKABLE void close();

    void save(const QString &folder);
    void retranslate() { emit changed(); }

signals:
    void changed();
    // A moment's word that it worked, or what went wrong with asking.
    void said(const Words &words);
    void failed(const Words &words);

private:
    void plan();
    void finished(const Anonymize::Result &result);
    QVariantList counts(const QList<Anonymize::Count> &counts) const;
    // A series of the scan being copied, as the series list calls it.
    QString seriesName(int series) const;
    QString seriesNames(const QList<int> &series) const;

    FilePicker *m_picker;
    Library *m_library = nullptr;
    bool m_shown = false;
    Stage m_stage = Closed;
    // What's planned and then copied: the scan and the regions as they
    // were when the plan was asked for.
    Scan m_scan;
    Anonymize::Options m_options;
    Anonymize::Result m_result;
    // A plan still being worked out for a choice since changed is stopped,
    // and what it comes to dropped.
    std::shared_ptr<std::atomic<bool>> m_cancel;
    int m_generation = 0;
};
