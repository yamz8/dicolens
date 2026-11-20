#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QVector>

// Tags are written as 0xGGGGEEEE, group then element, as the standard prints them.
namespace Tag {
constexpr quint32 TransferSyntax = 0x00020010;
constexpr quint32 SpecificCharacterSet = 0x00080005;
constexpr quint32 ImageType = 0x00080008;
constexpr quint32 SopClass = 0x00080016;
constexpr quint32 SopInstance = 0x00080018;
constexpr quint32 StudyDate = 0x00080020;
constexpr quint32 SeriesDate = 0x00080021;
constexpr quint32 AcquisitionDate = 0x00080022;
constexpr quint32 ContentDate = 0x00080023;
constexpr quint32 StudyTime = 0x00080030;
constexpr quint32 Modality = 0x00080060;
constexpr quint32 Manufacturer = 0x00080070;
constexpr quint32 Institution = 0x00080080;
constexpr quint32 StudyDescription = 0x00081030;
constexpr quint32 SeriesDescription = 0x0008103E;
constexpr quint32 ManufacturerModel = 0x00081090;
constexpr quint32 PatientName = 0x00100010;
constexpr quint32 PatientId = 0x00100020;
constexpr quint32 PatientBirthDate = 0x00100030;
constexpr quint32 PatientSex = 0x00100040;
constexpr quint32 PatientAge = 0x00101010;
constexpr quint32 BodyPart = 0x00180015;
constexpr quint32 SliceThickness = 0x00180050;
constexpr quint32 FrameTime = 0x00181063;
constexpr quint32 CineRate = 0x00180040;
constexpr quint32 RecommendedFrameRate = 0x00082144;
constexpr quint32 StudyInstance = 0x0020000D;
constexpr quint32 SeriesInstance = 0x0020000E;
constexpr quint32 SeriesNumber = 0x00200011;
constexpr quint32 AcquisitionNumber = 0x00200012;
constexpr quint32 InstanceNumber = 0x00200013;
constexpr quint32 ImagePosition = 0x00200032;
constexpr quint32 ImageOrientation = 0x00200037;
constexpr quint32 SliceLocation = 0x00201041;
constexpr quint32 SamplesPerPixel = 0x00280002;
constexpr quint32 Photometric = 0x00280004;
constexpr quint32 PlanarConfiguration = 0x00280006;
constexpr quint32 NumberOfFrames = 0x00280008;
constexpr quint32 Rows = 0x00280010;
constexpr quint32 Columns = 0x00280011;
constexpr quint32 PixelSpacing = 0x00280030;
constexpr quint32 ImagerPixelSpacing = 0x00181164;
constexpr quint32 BitsAllocated = 0x00280100;
constexpr quint32 BitsStored = 0x00280101;
constexpr quint32 HighBit = 0x00280102;
constexpr quint32 PixelRepresentation = 0x00280103;
constexpr quint32 WindowCenter = 0x00281050;
constexpr quint32 WindowWidth = 0x00281051;
constexpr quint32 RescaleIntercept = 0x00281052;
constexpr quint32 RescaleSlope = 0x00281053;
constexpr quint32 RescaleType = 0x00281054;
constexpr quint32 RedPaletteDescriptor = 0x00281101;
constexpr quint32 GreenPaletteDescriptor = 0x00281102;
constexpr quint32 BluePaletteDescriptor = 0x00281103;
constexpr quint32 RedPaletteData = 0x00281201;
constexpr quint32 GreenPaletteData = 0x00281202;
constexpr quint32 BluePaletteData = 0x00281203;
constexpr quint32 PixelPaddingValue = 0x00280120;
constexpr quint32 VoiLutSequence = 0x00283010;
constexpr quint32 LutDescriptor = 0x00283002;
constexpr quint32 PresentationLutShape = 0x20500020;
constexpr quint32 ReferencedSeries = 0x00081115;
constexpr quint32 ReferencedImage = 0x00081140;
constexpr quint32 ReferencedSopInstance = 0x00081155;
constexpr quint32 ReferencedFrameNumber = 0x00081160;
constexpr quint32 CodeMeaning = 0x00080104;
constexpr quint32 UltrasoundRegions = 0x00186011;
constexpr quint32 RegionSpatialFormat = 0x00186012;
constexpr quint32 RegionMinX = 0x00186018;
constexpr quint32 RegionMinY = 0x0018601A;
constexpr quint32 RegionMaxX = 0x0018601C;
constexpr quint32 RegionMaxY = 0x0018601E;
constexpr quint32 PhysicalUnitsX = 0x00186024;
constexpr quint32 PhysicalUnitsY = 0x00186026;
constexpr quint32 PhysicalDeltaX = 0x0018602C;
constexpr quint32 PhysicalDeltaY = 0x0018602E;
constexpr quint32 ValueType = 0x0040A040;
constexpr quint32 ConceptName = 0x0040A043;
constexpr quint32 ContentDateTime = 0x0040A120;
constexpr quint32 DateValue = 0x0040A121;
constexpr quint32 TimeValue = 0x0040A122;
constexpr quint32 PersonName = 0x0040A123;
constexpr quint32 TextValue = 0x0040A160;
constexpr quint32 ConceptCode = 0x0040A168;
constexpr quint32 MeasuredValue = 0x0040A300;
constexpr quint32 MeasurementUnits = 0x004008EA;
constexpr quint32 NumericValue = 0x0040A30A;
constexpr quint32 ContentSequence = 0x0040A730;
constexpr quint32 DocumentTitle = 0x00420010;
constexpr quint32 EncapsulatedDocument = 0x00420011;
constexpr quint32 DocumentMimeType = 0x00420012;
constexpr quint32 GraphicAnnotations = 0x00700001;
constexpr quint32 BoundingBoxUnits = 0x00700003;
constexpr quint32 AnchorPointUnits = 0x00700004;
constexpr quint32 GraphicUnits = 0x00700005;
constexpr quint32 UnformattedText = 0x00700006;
constexpr quint32 TextObjects = 0x00700008;
constexpr quint32 GraphicObjects = 0x00700009;
constexpr quint32 BoundingBoxTopLeft = 0x00700010;
constexpr quint32 BoundingBoxBottomRight = 0x00700011;
constexpr quint32 AnchorPoint = 0x00700014;
constexpr quint32 AnchorPointVisibility = 0x00700015;
constexpr quint32 GraphicData = 0x00700022;
constexpr quint32 GraphicType = 0x00700023;
constexpr quint32 GraphicFilled = 0x00700024;
// Overlay planes repeat across the even groups 6000 to 601E.
constexpr quint32 OverlayRows = 0x60000010;
constexpr quint32 OverlayColumns = 0x60000011;
constexpr quint32 OverlayFrames = 0x60000015;
constexpr quint32 OverlayOrigin = 0x60000050;
constexpr quint32 OverlayFrameOrigin = 0x60000051;
constexpr quint32 OverlayBitsAllocated = 0x60000100;
constexpr quint32 OverlayData = 0x60003000;
// Enhanced multi-frame files group what varies frame by frame, and what
// every frame shares.
constexpr quint32 SharedGroups = 0x52009229;
constexpr quint32 PerFrameGroups = 0x52009230;
constexpr quint32 PixelData = 0x7FE00010;
constexpr quint32 Item = 0xFFFEE000;
constexpr quint32 ItemEnd = 0xFFFEE00D;
constexpr quint32 SequenceEnd = 0xFFFEE0DD;
}

struct DicomElement {
    quint32 tag = 0;
    QByteArray vr;
    QByteArray value;                      // Empty for sequences and pixel data.
    QList<QList<DicomElement>> items;      // A sequence's items, each a list of elements.
    qint64 length = 0;                     // As written; -1 for undefined.
};

// One DICOM file's data set. Pixel data stays on disk: reading records only
// where it is, so scanning a whole CD touches a few kilobytes a file.
class DicomFile {
public:
    // Reads a Part 10 file (with the "DICM" preamble), or a bare data set as
    // older systems write them. Fails with a reason in English.
    static DicomFile read(const QString &path, QString *error = nullptr);
    static DicomFile parse(const QByteArray &bytes, QString *error = nullptr);
    static DicomFile parse(const uchar *data, qint64 size, QString *error = nullptr);

    bool isValid() const { return m_valid; }
    QString path() const { return m_path; }
    QString transferSyntax() const { return m_transferSyntax; }
    bool littleEndian() const { return m_littleEndian; }

    bool has(quint32 tag) const { return m_index.contains(tag); }
    const DicomElement *element(quint32 tag) const;
    const QList<DicomElement> &elements() const { return m_elements; }

    // A tag as it applies to one frame: enhanced multi-frame files put
    // spacing, position, rescale, and window in functional groups, per frame
    // or shared, rather than at the top level.
    const DicomElement *find(quint32 tag, int frame) const;

    QString string(quint32 tag) const;               // Every value, trimmed, joined with "\".
    QString first(quint32 tag) const;                // The first of a multi-valued string.
    QList<double> numbers(quint32 tag) const;        // Decimal or integer strings, or binary numbers.
    QList<double> numbers(const DicomElement *element) const;
    double number(quint32 tag, double fallback = 0) const;
    int integer(quint32 tag, int fallback = 0) const;

    // A value as people read it: names without carets, dates with dashes,
    // binary numbers spelled out, long binary values summarised.
    QString display(const DicomElement &element) const;

    bool hasPixelData() const { return m_pixelOffset >= 0; }
    bool encapsulated() const { return m_encapsulated; }
    qint64 pixelOffset() const { return m_pixelOffset; }
    qint64 pixelLength() const { return m_pixelLength; }
    QByteArray pixelVr() const { return m_pixelVr; }
    // Bytes of the pixel data, from its start; from the file on disk, or
    // from memory when the file was deflated.
    QByteArray readPixels(qint64 offset, qint64 length) const;

    // The compressed fragments of each frame of encapsulated pixel data,
    // read from disk. frameCount is the file's Number of Frames.
    static QList<QByteArray> splitFrames(const QByteArray &encapsulated, int frameCount);

    QString decode(const QByteArray &text) const;
    static QString formatName(const QString &value);
    static QString formatDate(const QString &value);
    static QString formatTime(const QString &value);

private:
    bool parseDataSet(const uchar *data, qint64 size, qint64 &pos, qint64 end, bool explicitVr,
                      bool littleEndian, QList<DicomElement> &out, int depth, QString *error);
    // A sequence's items, up to its end or its delimiter.
    bool parseItems(const uchar *data, qint64 size, qint64 &pos, qint64 sequenceEnd, bool itemsExplicit,
                    bool littleEndian, QList<QList<DicomElement>> &items, int depth, QString *error);
    void index();

    QString m_path;
    QString m_transferSyntax = "1.2.840.10008.1.2";
    QByteArray m_charset;
    bool m_valid = false;
    bool m_littleEndian = true;
    QList<DicomElement> m_elements;
    QHash<quint32, int> m_index;
    qint64 m_pixelOffset = -1;
    qint64 m_pixelLength = 0;              // Bytes on disk, items and all when encapsulated.
    bool m_encapsulated = false;
    QByteArray m_pixelVr;
    QByteArray m_inflated;
};

namespace Dictionary {
// The standard name of a tag ("Patient's Name") and its VR, or empty when
// it's private or not in the bundled list.
QString name(quint32 tag);
QByteArray vr(quint32 tag);
QString transferSyntaxName(const QString &uid);
QString sopClassName(const QString &uid);
}

QString tagText(quint32 tag);

// A number a file gave, as an int: the nearest an int can hold, since a file
// may spell any number at all, and the fallback for one that isn't a number.
int toInt(double value, int fallback = 0);
