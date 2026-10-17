import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every series found, grouped under its study, with a thumbnail of its
// middle slice.
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
        currentIndex: backend.seriesIndex
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool current: index === backend.seriesIndex
            width: list.width
            height: column.implicitHeight

            ColumnLayout {
                id: column
                width: parent.width
                spacing: 0

                // The study's heading, above its first series.
                ColumnLayout {
                    visible: row.modelData.study !== ""
                    Layout.fillWidth: true
                    Layout.leftMargin: 14
                    Layout.rightMargin: 14
                    Layout.topMargin: row.index === 0 ? 14 : 22
                    Layout.bottomMargin: 6
                    spacing: 1
                    Label {
                        text: row.modelData.study
                        color: win.text
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Label {
                        text: row.modelData.studyDate
                        color: win.muted
                        font.pixelSize: 12
                        visible: text !== ""
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 76
                    color: row.current ? "#202027" : hover.hovered ? "#1a1a1f" : "transparent"

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
                                source: "image://thumbnails/" + row.index + "/" + backend.thumbnailsVersion
                            }
                            Label {
                                anchors.centerIn: parent
                                text: row.modelData.modality
                                color: win.muted
                                font.pixelSize: 12
                                visible: thumb.status !== Image.Ready || thumb.implicitWidth <= 1
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                text: row.modelData.title
                                color: row.current ? win.text : "#c8c8d0"
                                font.bold: row.current
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                            Label {
                                text: (translator.language, [row.modelData.modality,
                                       qsTr("%n images", "", row.modelData.count)].filter(x => x).join("  ·  "))
                                color: win.muted
                                font.pixelSize: 12
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("Can't be decoded")
                                color: "#ff8a80"
                                font.pixelSize: 12
                                visible: !row.modelData.decodable
                            }
                        }
                    }

                    HoverHandler { id: hover }
                    TapHandler { onTapped: backend.seriesIndex = row.index }
                }
            }
        }
    }
}
