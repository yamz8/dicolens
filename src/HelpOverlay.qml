import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, grouped. The README lists the same ones.
Rectangle {
    id: help
    color: "#f40e0e10"

    MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

    readonly property var groups: [
        [qsTr("Open"), [["O", qsTr("Open files")], ["Shift+O", qsTr("Open a folder or a CD")],
                        [qsTr("Drop"), qsTr("Drop files or folders on the window")],
                        ["Ctrl+E", qsTr("Save the image as a PNG")], ["Ctrl+C", qsTr("Copy the image")],
                        ["?", qsTr("This list")], ["Q", qsTr("Quit")]]],
        [qsTr("Move through"), [[qsTr("Wheel"), qsTr("Previous / next slice")], ["↑ ↓", qsTr("Previous / next slice")],
                                ["PgUp PgDn", qsTr("Ten slices back / on")], ["Home End", qsTr("First / last slice")],
                                ["← →", qsTr("Previous / next series")], ["Space", qsTr("Play / pause the series as a loop")]]],
        [qsTr("Look"), [[qsTr("Drag"), qsTr("Contrast across, brightness down")],
                        ["1 2 3 4", qsTr("CT windows: brain, soft tissue, lung, bone")],
                        ["0", qsTr("The file's own window")], ["A", qsTr("The image's full range")],
                        ["I", qsTr("Invert")], ["Ctrl+Z", qsTr("Undo a window change")], ["Ctrl+Shift+Z", qsTr("Redo")]]],
        [qsTr("View"), [[qsTr("Ctrl+wheel"), qsTr("Zoom about the pointer")], [qsTr("Right-drag"), qsTr("Zoom")],
                        [qsTr("Middle-drag"), qsTr("Move the image (or Shift+drag)")], ["F", qsTr("Fit to the window")],
                        ["R", qsTr("Reset the view and window")], ["M", qsTr("Measure: drag a ruler")],
                        ["S", qsTr("Show or hide the series")], ["T", qsTr("Show or hide the tags")],
                        ["Ctrl+F", qsTr("Search the tags")], ["H", qsTr("Hide or show the patient's details")]]],
    ]

    Flickable {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        contentHeight: grid.implicitHeight + 60
        clip: true

        GridLayout {
            id: grid
            readonly property int count: help.width > 1100 ? 2 : 1
            readonly property real columnWidth: (help.width - 64 - (count - 1) * columnSpacing) / count
            x: 32
            y: 40
            width: help.width - 64
            columns: count
            columnSpacing: 48
            rowSpacing: 28

            Repeater {
                model: help.groups
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: grid.columnWidth
                    Layout.maximumWidth: grid.columnWidth
                    spacing: 7
                    Label {
                        text: modelData[0]
                        color: theme.accent
                        font.bold: true
                        font.pixelSize: 16
                        Layout.bottomMargin: 4
                    }
                    Repeater {
                        model: modelData[1]
                        delegate: RowLayout {
                            required property var modelData
                            spacing: 14
                            Label {
                                text: modelData[0]
                                color: win.text
                                font.family: "monospace"
                                font.bold: true
                                Layout.preferredWidth: 150
                                Layout.alignment: Qt.AlignTop
                                wrapMode: Text.Wrap
                            }
                            Label {
                                text: modelData[1]
                                color: "#b8b8c0"
                                wrapMode: Text.Wrap
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
    }

    Label {
        id: footer
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 20
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        text: qsTr("For looking at your own scans. Not for diagnosis.") + "   ·   " + qsTr("Esc closes")
        color: win.muted
    }
}
