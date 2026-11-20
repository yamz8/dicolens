#pragma once

#include <QHash>
#include <QList>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <atomic>

#include "dicom.h"
#include "study.h"

// Copies of scans without what identifies the patient, to send for a
// second opinion or to a study.
//
// What happens to each attribute is the standard's own table: PS3.15 Table
// E.1-1, the Basic Application Level Confidentiality Profile, with two of
// its options, which reading the scans needs: Retain Longitudinal Temporal
// Information with Full Dates (dates and times stay) and Retain Patient
// Characteristics (age, sex, size, and weight stay). Private attributes are
// removed and UIDs replaced. The table is src/data/confidentiality.tsv.
//
// Where the table leaves a choice to what the file's IOD requires ("X/Z/D":
// remove, unless the attribute must be there, or must have a value),
// Dicolens takes the one that's valid whatever the IOD: empty for X/Z, a
// dummy value for X/D, Z/D and X/Z/D, and for X/Z/U* the sequence with the
// UIDs in it replaced. Where an option says to clean a text rather than
// keep it, nothing here can know the text is clean, so the profile's own
// action stands. And whatever the table says, a person's name goes, a UID
// that isn't the standard's own is replaced, and a value that looks like a
// sequence nothing could read into is dropped.
namespace Anonymize {

struct Options {
    // The profile removes what people typed to describe a study, a series,
    // and a protocol, as it says whatever they knew. Telling series apart
    // needs those three, so this keeps them as they are (Study Description,
    // Series Description, Protocol Name; no others). Nothing cleans them,
    // so the copy then says what was kept instead of calling itself the
    // Basic Profile.
    bool keepDescriptions = false;
    // Rectangles to black out in every image of a series, in image pixels,
    // by the series' place in the scan, from 0. Ultrasound and screen
    // captures carry the name in the picture.
    QHash<int, QList<QRect>> blackout;
};

// Something that identifies the patient or those around them, read from a
// source file, and looked for in every copy.
struct Identifier {
    enum class Kind { Name, Id, BirthDate, Place, Phone, Device };
    // A long one is looked for anywhere. A short one ("Lee", "A12") only as
    // a word of its own, and a number only where no digit continues it, or
    // every other value would hold them. A phone number is also looked for
    // through whatever punctuation it's written with.
    enum class Match { Anywhere, Word, Number, Squeezed };
    QString text;    // As it's looked for: case-folded.
    QString found;   // As the source wrote it.
    Kind kind = Kind::Name;
    Match match = Match::Anywhere;
};

// The UIDs and patient IDs given out so far, so the copies still refer to
// one another as the originals did, and what the sources said that no copy
// may.
struct Session {
    QHash<QString, QString> uids;
    QHash<QString, QString> patients;
    QList<Identifier> identifiers;
    QSet<QString> known;         // The identifiers' texts, so each is kept once.
    QSet<QString> sourceUids;    // Every UID of the sources that isn't the standard's own.
};

// One of the sets of tags the summary counts as one: every private
// attribute, every curve, every overlay plane, a file-set's directory.
namespace Family {
constexpr quint32 Private = 0x00010000;
constexpr quint32 Curves = 0x50000000;
constexpr quint32 Overlays = 0x60000000;
constexpr quint32 Directory = 0x00040000;
}

// What was done to one file's attributes, by tag, wherever in the file
// they were. An attribute inside a sequence that went as a whole isn't
// listed; the sequence is.
struct Changes {
    QSet<quint32> removed;
    QSet<quint32> emptied;
    QSet<quint32> replaced;   // By a dummy value, a stand-in, or a new UID.
    QSet<quint32> kept;       // Named by the profile, and kept by an option.
};

// A copy that still held something of the patient's, and so wasn't kept.
struct Failure {
    QString path;     // The source file.
    quint32 tag = 0;
    QString name;     // The tag's name.
    QString found;    // What was found there.
    QString what;     // What that is: "a person's name".
    QString text() const;
};

// What became of one file.
struct Outcome {
    Changes changes;
    QList<Failure> failures;
    bool blackedOut = false;
};

struct Count {
    QString tag;      // "(0008,0080)", or "(60xx,xxxx)" for a family; empty for private attributes.
    QString name;
    int files = 0;
    bool operator==(const Count &) const = default;
};

// What the copies were made without, and with, by attribute.
struct Summary {
    QList<Count> removed, emptied, replaced, kept;
    bool operator==(const Summary &) const = default;
};

// Reads what identifies the patient and those around them from a source
// file: every part of every person's name, IDs, the birth date, accession
// number, institutions, addresses, phone numbers, serial numbers, and UIDs.
void learn(const DicomFile &file, Session &session);

// Looks through a copy for anything learned: every value at any depth, as
// text and as bytes, but for the pixels themselves.
QList<Failure> verify(const DicomFile &copy, const Session &session);

// Writes a copy of a DICOM file, with the rectangles blacked out of its
// picture, and reads it back to look for what the source said of the
// patient; a copy that still holds any of it is deleted. A picture with
// rectangles is decoded and written uncompressed, and so is one that keeps
// an overlay in its pixels' spare bits, to clear them; one that can't be
// drawn again value for value (a video, 32-bit pixels) isn't copied. Fails
// with a reason in English.
bool write(const DicomFile &file, const QString &path, Session &session, QString *error = nullptr,
           const Options &options = {}, const QList<QRect> &blackout = {}, Outcome *outcome = nullptr);

// The same without writing anything: what would be done, and whether the
// copy would pass.
bool check(const DicomFile &file, Session &session, QString *error = nullptr, const Options &options = {},
           const QList<QRect> &blackout = {}, Outcome *outcome = nullptr);

// Whether the picture itself may show the patient's details, as ultrasound
// and screen captures often do; no change to the tags takes those out.
bool mayShowDetails(const DicomFile &file);

// The edition of the standard the profile is from, such as "2026d".
QString edition();

// What a copy says was done to it, as its De-identification Method reads.
QStringList method(const Options &options = {});

struct Result {
    QString folder;           // Where the copy went; empty for a plan.
    int written = 0;          // Copies kept; in a plan, those that would be.
    int skipped = 0;          // Reports, whose text names the patient.
    int annotations = 0;      // Presentation states left out, as their text can name the patient.
    int showDetails = 0;      // Images whose picture may show the patient's details.
    int blackedOut = 0;       // Images with a rectangle blacked out.
    QStringList errors;       // A file and why it wasn't copied.
    QList<Failure> failures;  // The errors that are something of the patient's found in a copy.
    Summary summary;
    QStringList method;
};

// Copies every image of a scan into a new folder inside folder, named
// "Anonymous scans", a series to a folder. Reports are left out, and so
// are presentation states: the profile blanks their text and graphics,
// and what's left draws nothing.
Result copyScan(const Scan &scan, const QString &folder, const std::atomic<bool> *cancel = nullptr,
                const Options &options = {});

// What copyScan would do, without writing anything.
Result plan(const Scan &scan, const Options &options = {}, const std::atomic<bool> *cancel = nullptr);

}
