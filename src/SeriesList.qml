import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every series found, grouped under its study, with a thumbnail of its
// middle slice, and after them the reports that came with them.
Rectangle {
    id: panel
    color: win.panel

    Rectangle {
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: 1
        color: win.line
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.rightMargin: 1
        clip: true
        model: backend.series
        currentIndex: win.inUse.seriesIndex
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

        footer: ColumnLayout {
            width: list.width
            spacing: 0
            visible: backend.reportCount > 0

            Label {
                text: qsTr("Reports")
                color: win.text
                font.bold: true
                Layout.leftMargin: 14
                Layout.topMargin: 22
                Layout.bottomMargin: 6
            }
            Repeater {
                model: backend.reports
                delegate: Rectangle {
                    id: reportRow
                    required property string title
                    required property string date
                    required property string study
                    required property bool pdf
                    required property int index
                    readonly property bool current: index === backend.reportIndex
                    Layout.fillWidth: true
                    Layout.preferredHeight: reportColumn.implicitHeight + 16
                    color: current ? win.selected : reportHover.hovered ? win.hover : "transparent"

                    Rectangle {
                        anchors.left: parent.left
                        width: 3
                        height: parent.height
                        color: theme.accent
                        visible: reportRow.current
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 12
                        Rectangle {
                            Layout.preferredWidth: 60
                            Layout.preferredHeight: 28
                            radius: Math.min(4, theme.radius)
                            color: "transparent"
                            border.color: reportRow.current ? theme.accent : win.line
                            Label {
                                anchors.centerIn: parent
                                // Kinds of file, kept as they are in every language.
                                text: reportRow.pdf ? "PDF" : "SR"
                                color: win.muted
                                font.pixelSize: win.px(12)
                                font.bold: true
                            }
                        }
                        ColumnLayout {
                            id: reportColumn
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: reportRow.title
                                color: reportRow.current ? win.text : win.secondary
                                font.bold: reportRow.current
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Label {
                                text: [reportRow.study, reportRow.date].filter(x => x).map(x => "\u2068" + x + "\u2069").join("  ·  ")
                                color: win.muted
                                font.pixelSize: win.px(12)
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                visible: text !== ""
                            }
                        }
                    }
                    HoverHandler { id: reportHover }
                    TapHandler { onTapped: backend.openReport(reportRow.index) }
                }
            }
            Item { Layout.preferredHeight: 14 }
        }

        delegate: Item {
            id: row
            required property string title
            required property string modality
            required property int images
            required property string study
            required property string studyDate
            required property bool decodable
            // Where its thumbnail is, once it's made: a new one, a new address.
            required property string thumbnail
            required property int index
            readonly property bool current: index === win.inUse.seriesIndex
            width: list.width
            height: column.implicitHeight

            ColumnLayout {
                id: column
                width: parent.width
                spacing: 0

                // The study's heading, above its first series.
                ColumnLayout {
                    visible: row.study !== ""
                    Layout.fillWidth: true
                    Layout.leftMargin: 14
                    Layout.rightMargin: 14
                    Layout.topMargin: row.index === 0 ? 14 : 22
                    Layout.bottomMargin: 6
                    spacing: 1
                    Label {
                        text: row.study
                        color: win.text
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: row.studyDate
                        color: win.muted
                        font.pixelSize: win.px(12)
                        visible: text !== ""
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    // Room for a title on two lines at any text size.
                    Layout.preferredHeight: Math.max(76, seriesText.implicitHeight + 16)
                    color: row.current ? win.selected : hover.hovered ? win.hover : "transparent"

                    Rectangle {
                        anchors.left: parent.left
                        width: 3
                        height: parent.height
                        color: theme.accent
                        visible: row.current
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 60
                            Layout.preferredHeight: 60
                            color: "#000"
                            radius: Math.min(4, theme.radius)
                            border.color: row.current ? theme.accent : win.line
                            Image {
                                id: thumb
                                anchors.fill: parent
                                anchors.margins: 1
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                cache: false
                                source: row.thumbnail
                            }
                            Label {
                                anchors.centerIn: parent
                                text: row.modality
                                color: win.muted
                                font.pixelSize: win.px(12)
                                visible: thumb.status !== Image.Ready || thumb.implicitWidth <= 1
                            }
                        }

                        ColumnLayout {
                            id: seriesText
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: row.title
                                color: row.current ? win.text : win.secondary
                                font.bold: row.current
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                            Label {
                                text: [row.modality, qsTr("%Ln images", "", row.images)].filter(x => x).join("  ·  ")
                                color: win.muted
                                font.pixelSize: win.px(12)
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("Can't be decoded")
                                color: win.error
                                font.pixelSize: win.px(12)
                                visible: !row.decodable
                            }
                        }
                    }

                    HoverHandler { id: hover }
                    TapHandler { onTapped: win.inUse.seriesIndex = row.index }
                }
            }
        }
    }
}
