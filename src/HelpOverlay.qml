import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Every shortcut, grouped. The README lists the same ones.
Rectangle {
    id: help
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.98)

    MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

    readonly property var groups: [
        [qsTr("Open"), [["O", qsTr("Open files")], ["Shift+O", qsTr("Open a folder or a CD")],
                        [qsTr("Drop"), qsTr("Drop files or folders on the window")],
                        ["Ctrl+E", qsTr("Save the image as a PNG")], ["Ctrl+Shift+E", qsTr("Save the series as a video")],
                        ["Ctrl+C", qsTr("Copy the image")], ["Ctrl+Shift+S", qsTr("Save an anonymous copy of the scans")],
                        ["?", qsTr("This list")], ["Q", qsTr("Quit")]]],
        [qsTr("Move through"), [[qsTr("Wheel"), qsTr("Previous / next slice")], ["↑ ↓", qsTr("Previous / next slice")],
                                ["PgUp PgDn", qsTr("Ten slices back / on")], ["Home End", qsTr("First / last slice")],
                                ["← →", qsTr("Previous / next series")], ["Space", qsTr("Play / pause the series as a loop")]]],
        [qsTr("Look"), [[qsTr("Drag"), qsTr("Contrast across, brightness down")],
                        ["1 2 3 4", qsTr("CT windows: brain, soft tissue, lung, bone")],
                        ["0", qsTr("The file's own window")], ["A", qsTr("The image's full range")],
                        ["I", qsTr("Invert")], ["G", qsTr("Colour scales, for PET")], ["Ctrl+Z", qsTr("Undo a window change")], ["Ctrl+Shift+Z", qsTr("Redo")]]],
        [qsTr("View"), [[qsTr("Ctrl+wheel"), qsTr("Zoom about the pointer")], [qsTr("Right-drag"), qsTr("Zoom")],
                        [qsTr("Middle-drag"), qsTr("Move the image (or Shift+drag)")], ["F", qsTr("Fit to the window")],
                        ["] [", qsTr("Turn right / left")], ["X Y", qsTr("Flip left to right / top to bottom")],
                        ["R", qsTr("Reset the view and window")],
                        ["N", qsTr("Show or hide marks left on the image")],
                        ["S", qsTr("Show or hide the series")], ["T", qsTr("Show or hide the tags")],
                        ["Ctrl+F", qsTr("Search the tags")], ["H", qsTr("Hide or show the patient's details")]]],
        [qsTr("Compare and read"), [["C", qsTr("Two series side by side")], ["Tab", qsTr("The other side")],
                                    ["L", qsTr("Scroll the sides together or apart")],
                                    ["Shift+V", qsTr("Four views: three planes and 3D")],
                                    ["U", qsTr("A PET over the CT or MR taken with it")],
                                    ["Shift+U", qsTr("How strongly the PET shows")],
                                    ["P", qsTr("Read the reports that came with the scans")],
                                    ["← →", qsTr("Other reports, while reading")]]],
        [qsTr("Measure and cut"), [["M", qsTr("Ruler")], ["E", qsTr("Area, with its values")], ["Shift+M", qsTr("Angle")], ["Shift+C", qsTr("Cobb angle, between two lines")],
                                   ["Delete", qsTr("Remove the last measurement")], ["Shift+Delete", qsTr("Remove this slice's measurements")],
                                   ["V", qsTr("Cut the series another way")], ["Ctrl+← → ↑ ↓", qsTr("Tilt the cut")],
                                   [qsTr("Click"), qsTr("Where the planes meet")],
                                   [qsTr("Drag the line"), qsTr("Turn the other side's plane")],
                                   ["B", qsTr("Slabs: MIP, MinIP, average")],
                                   ["Shift+B", qsTr("Slab thickness")]]],
        [qsTr("In 3D"), [["D", qsTr("The series in 3D, and back")], [qsTr("Drag"), qsTr("Turn it")],
                         [qsTr("Ctrl+drag"), qsTr("Show more or less of it")],
                         [qsTr("Wheel") + "  ↑ ↓", qsTr("Cut into it from the front")],
                         ["B", qsTr("Bone, vessels, skin, lungs, MIP")],
                         ["V", qsTr("From the front, the side, the back, above")],
                         ["Space", qsTr("Spin it")]]],
    ]

    Flickable {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        contentHeight: grid.implicitHeight + 60
        clip: true

        RowLayout {
            id: grid
            readonly property int count: help.width > 1180 ? 3 : help.width > 760 ? 2 : 1
            readonly property real columnWidth: (help.width - 64 - (count - 1) * spacing) / count
            x: 32
            y: 40
            width: help.width - 64
            spacing: count === 3 ? 36 : 48

            // The groups balanced across the columns there's room for.
            Repeater {
                model: grid.count === 3 ? [[0, 5], [1, 2, 6], [3, 4]]
                     : grid.count === 2 ? [[0, 2, 5], [1, 3, 4, 6]] : [[0, 1, 2, 3, 4, 5, 6]]
                delegate: ColumnLayout {
                    id: column
                    required property var modelData
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: grid.columnWidth
                    Layout.maximumWidth: grid.columnWidth
                    spacing: 28

                    Repeater {
                        model: column.modelData
                        delegate: ColumnLayout {
                            id: section
                            required property var modelData
                            readonly property var group: help.groups[modelData]
                            Layout.alignment: Qt.AlignTop
                            Layout.fillWidth: true
                            spacing: 7
                            Label {
                                text: section.group[0]
                                color: theme.accent
                                font.bold: true
                                font.pixelSize: 16
                                Layout.bottomMargin: 4
                            }
                            Repeater {
                                model: section.group[1]
                                delegate: RowLayout {
                                    required property var modelData
                                    spacing: 14
                                    Label {
                                        text: modelData[0]
                                        color: win.text
                                        font.family: "monospace"
                                        font.bold: true
                                        Layout.preferredWidth: grid.count === 3 ? 118 : 150
                                        Layout.alignment: Qt.AlignTop
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        text: modelData[1]
                                        color: win.secondary
                                        wrapMode: Text.Wrap
                                        Layout.fillWidth: true
                                    }
                                }
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
