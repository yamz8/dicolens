import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Dicolens

// An anonymous copy, before it's made and after: what a copy would do to
// every file, worked out without writing one; the one choice it has; and,
// once a folder is chosen, how it went, which stays up until it's
// dismissed. The keys come here while it's open, as Main.qml sends them.
Rectangle {
    id: panel
    color: Qt.rgba(theme.background.r, theme.background.g, theme.background.b, 0.98)

    readonly property AnonymousCopy copy: backend.anonymous
    readonly property bool planning: copy.stage === AnonymousCopy.Planning
    readonly property bool planned: copy.stage === AnonymousCopy.Planned
    readonly property bool saving: copy.stage === AnonymousCopy.Saving
    readonly property bool saved: copy.stage === AnonymousCopy.Saved
    // The series whose pictures may show the patient, with nothing blacked
    // out of them.
    readonly property var unhidden: copy.pictures.filter(row => row.flagged > 0 && row.regions === 0)
    // Whether there's something to look into: before, what wouldn't be
    // copied; after, that and all the copy leaves to the reader.
    readonly property bool attention: copy.refusedFiles > 0 || copy.uncopiedFiles > 0
                                      || (saved && (copy.unhidden > 0 || copy.reports > 0 || copy.annotations > 0))

    function scroll(amount) {
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + amount))
    }
    // Enter: the main action of whichever stage it's in.
    function accept() {
        if (saved)
            copy.close()
        else
            copy.choose()
    }
    function toggleDescriptions() {
        copy.keepDescriptions = !copy.keepDescriptions
    }
    function n(value) {
        // Reads the language so the numbers re-render when it changes.
        return translator.language !== undefined ? Number(value).toLocaleString(Qt.locale(), 'f', 0) : ""
    }
    // A name that stays as it's written inside a line of another script.
    function isolated(text) { return "⁨" + text + "⁩" }
    function seriesLine(row) {
        return [row.modality, qsTr("%Ln images", "", row.images)].filter(x => x).map(isolated).join("  ·  ")
    }
    onVisibleChanged: flick.contentY = 0

    // Clicks and the wheel stay in the panel rather than reaching the
    // scans under it.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: (wheel) => wheel.accepted = true
    }

    component Heading: Label {
        color: theme.accent
        font.bold: true
        font.pixelSize: win.px(16)
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    component Para: Label {
        color: win.secondary
        horizontalAlignment: Text.AlignLeft
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    // One of the four things that become of an attribute, and the
    // attributes it becomes of, each with how many files have it.
    component Counts: ColumnLayout {
        id: group
        property string title
        property string caption
        property var rows: []
        spacing: 5
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignTop

        Label {
            text: group.title
            color: win.text
            font.bold: true
            font.pixelSize: win.px(15)
            horizontalAlignment: Text.AlignLeft
            Layout.fillWidth: true
        }
        Label {
            text: group.caption
            color: win.muted
            font.pixelSize: win.px(13)
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.bottomMargin: 3
        }
        Repeater {
            model: group.rows
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 14
                Label {
                    text: modelData.name
                    color: win.secondary
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    text: panel.n(modelData.files)
                    color: win.muted
                    Layout.alignment: Qt.AlignTop
                }
            }
        }
        Label {
            visible: group.rows.length === 0
            text: qsTr("Nothing")
            color: win.muted
            horizontalAlignment: Text.AlignLeft
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        id: header
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 32
        width: Math.min(parent.width - 64, 860)
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                text: qsTr("Save an Anonymous Copy")
                color: win.text
                font.bold: true
                font.pixelSize: win.px(20)
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignLeft
                Layout.fillWidth: true
            }
            Label {
                // The edition of the standard it goes by, as the standard
                // writes it in every language.
                text: "DICOM PS3.15 " + panel.copy.edition
                color: theme.accent
                font.bold: true
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.preferredHeight: 1
            color: win.line
        }
    }

    // A plan being worked out, or the copy being written.
    ColumnLayout {
        anchors.centerIn: parent
        visible: panel.planning || panel.saving
        spacing: 14
        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: panel.visible && (panel.planning || panel.saving)
        }
        Label {
            objectName: "anonymousWorking"
            Layout.alignment: Qt.AlignHCenter
            text: panel.saving ? qsTr("Saving an anonymous copy…") : qsTr("Working out what a copy would do…")
            color: win.text
            font.pixelSize: win.px(16)
        }
    }

    Flickable {
        id: flick
        objectName: "anonymousFlick"
        anchors.top: header.bottom
        anchors.bottom: footer.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 16
        anchors.bottomMargin: 12
        contentHeight: content.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        visible: panel.planned || panel.saved

        ColumnLayout {
            id: content
            x: (flick.width - width) / 2
            width: header.width
            spacing: 22

            // How many files, and where they go.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Label {
                    objectName: "anonymousHeadline"
                    text: panel.saved ? (panel.copy.failure !== "" ? panel.copy.failure
                                         : panel.copy.files > 0 ? qsTr("Saved %Ln files in %1", "", panel.copy.files).arg(panel.isolated(panel.copy.folderName))
                                         : qsTr("Nothing was copied"))
                        : panel.copy.files > 0 ? qsTr("%Ln files would be copied", "", panel.copy.files)
                        : qsTr("Nothing here can be copied")
                    color: win.text
                    font.bold: true
                    font.pixelSize: win.px(22)
                    horizontalAlignment: Text.AlignLeft
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Para {
                    visible: text !== ""
                    text: panel.saved ? panel.copy.folder
                        : qsTr("Into a new folder, “Anonymous scans”, inside the one you choose. Nothing is written until then, and the scans you opened stay as they are.")
                    // A path keeps its end, which is where the copy is.
                    wrapMode: panel.saved ? Text.NoWrap : Text.Wrap
                    elide: panel.saved ? Text.ElideMiddle : Text.ElideNone
                    color: win.muted
                }
            }

            // What needs a look: before, what wouldn't be copied; after,
            // everything the copy leaves to whoever shares it.
            Rectangle {
                objectName: "anonymousAttention"
                visible: panel.attention
                Layout.fillWidth: true
                implicitHeight: issues.implicitHeight + 28
                radius: Math.min(4, theme.radius)
                color: win.mix(theme.background, win.error, 0.1)
                border.color: win.mix(theme.background, win.error, 0.45)

                ColumnLayout {
                    id: issues
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 14
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    spacing: 10

                    Label {
                        text: panel.saved ? qsTr("Before you share it") : qsTr("Not everything would be copied")
                        color: win.text
                        font.bold: true
                        font.pixelSize: win.px(16)
                        horizontalAlignment: Text.AlignLeft
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    // Copies the check refuses, by what it found and where.
                    ColumnLayout {
                        visible: panel.copy.refusedFiles > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Para {
                            objectName: "anonymousRefused"
                            color: win.text
                            text: panel.saved ? qsTr("%Ln copies still named the patient, so they weren't kept:", "", panel.copy.refusedFiles)
                                              : qsTr("%Ln copies would still name the patient, so they wouldn't be kept:", "", panel.copy.refusedFiles)
                        }
                        Repeater {
                            model: panel.copy.refused
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: 14
                                spacing: 1
                                Para {
                                    color: win.text
                                    font.bold: true
                                    text: panel.isolated(modelData.name) + "  " + panel.isolated(modelData.tag)
                                }
                                // What was found is the patient's: it's
                                // said once, and not at all while the
                                // patient's details are hidden.
                                Para {
                                    text: win.infoHidden ? modelData.what : modelData.what + ": " + panel.isolated(modelData.found)
                                }
                                Para {
                                    color: win.muted
                                    text: [qsTr("%Ln files", "", modelData.files), modelData.series].filter(x => x).map(panel.isolated).join("  ·  ")
                                }
                            }
                        }
                        Para {
                            objectName: "anonymousCaution"
                            visible: panel.saved
                            color: win.text
                            text: qsTr("So don't take this folder for an anonymous copy until you know why.")
                        }
                    }

                    // Files not copied for any other reason.
                    ColumnLayout {
                        visible: panel.copy.uncopiedFiles > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Para {
                            color: win.text
                            text: panel.saved ? qsTr("%Ln files couldn't be copied.", "", panel.copy.uncopiedFiles)
                                              : qsTr("%Ln files can't be copied:", "", panel.copy.uncopiedFiles)
                        }
                        Repeater {
                            model: panel.copy.uncopied
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: 14
                                spacing: 1
                                Para { text: modelData.reason }
                                Para {
                                    color: win.muted
                                    text: [qsTr("%Ln files", "", modelData.files), modelData.series].filter(x => x).map(panel.isolated).join("  ·  ")
                                }
                            }
                        }
                    }

                    // Pictures that may still show the patient.
                    ColumnLayout {
                        visible: panel.saved && panel.copy.unhidden > 0
                        Layout.fillWidth: true
                        spacing: 6
                        Para {
                            color: win.text
                            text: qsTr("%Ln of the images may show the patient's details in the picture itself, which a copy can't take out. Look at them before sharing.", "", panel.copy.unhidden)
                        }
                        Repeater {
                            model: panel.unhidden
                            delegate: Para {
                                required property var modelData
                                Layout.leftMargin: 14
                                text: panel.isolated(modelData.name) + "  ·  " + panel.seriesLine(modelData)
                            }
                        }
                    }

                    // What was left out whole.
                    Para {
                        visible: panel.saved && panel.copy.reports > 0
                        color: win.text
                        text: qsTr("Reports were left out, as their text names the patient.")
                    }
                    Para {
                        visible: panel.saved && panel.copy.annotations > 0
                        color: win.text
                        text: qsTr("Annotation files were left out, as their text can name the patient.")
                    }
                }
            }
            Para {
                visible: panel.saved && !panel.attention
                text: qsTr("Every copy passed the check, and nothing was left out.")
            }

            // The one choice, with what each way costs.
            ColumnLayout {
                visible: panel.planned
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Switch {
                        id: keep
                        objectName: "anonymousKeep"
                        focusPolicy: Qt.NoFocus
                        Material.accent: theme.accent
                        onToggled: panel.copy.keepDescriptions = checked
                        // A click sets it as much as Space does, so it's
                        // told what the choice is rather than bound to it.
                        Binding on checked { value: panel.copy.keepDescriptions }
                    }
                    Label {
                        text: qsTr("Keep the study and series descriptions")
                        color: win.text
                        font.bold: true
                        horizontalAlignment: Text.AlignLeft
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        TapHandler { onTapped: panel.toggleDescriptions() }
                    }
                    Label {
                        text: "Space"
                        color: win.muted
                        font.family: "monospace"
                        font.bold: true
                    }
                }
                Para {
                    text: qsTr("Off, the copy's series have no names. On, their text is copied as it is, and a copy that names the patient there is refused.")
                }
            }

            // What becomes of each attribute.
            ColumnLayout {
                visible: panel.copy.files > 0
                Layout.fillWidth: true
                spacing: 10
                Heading { text: qsTr("What becomes of each attribute") }
                Para { text: qsTr("By the name the standard gives it, with how many of the files have it.") }
                GridLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    columns: content.width > 620 ? 2 : 1
                    columnSpacing: 44
                    rowSpacing: 22
                    // Equal shares of the width, whatever is in them.
                    uniformCellWidths: true
                    Counts {
                        objectName: "anonymousRemoved"
                        title: qsTr("Removed")
                        caption: qsTr("Gone from the copy")
                        rows: panel.copy.removed
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 22
                        Counts {
                            title: qsTr("Emptied")
                            caption: qsTr("Still there, with no value")
                            rows: panel.copy.emptied
                        }
                        Counts {
                            title: qsTr("Replaced")
                            caption: qsTr("By a stand-in, or a new UID")
                            rows: panel.copy.replaced
                        }
                        Counts {
                            objectName: "anonymousKept"
                            title: qsTr("Kept")
                            caption: qsTr("Named by the profile, and kept on purpose")
                            rows: panel.copy.kept
                        }
                    }
                }
            }

            // What isn't copied at all.
            ColumnLayout {
                visible: panel.planned && (panel.copy.reports > 0 || panel.copy.annotations > 0)
                Layout.fillWidth: true
                spacing: 6
                Heading { text: qsTr("Left out") }
                Para {
                    visible: panel.copy.reports > 0
                    text: qsTr("%Ln reports would be left out, as their text names the patient.", "", panel.copy.reports)
                }
                Para {
                    visible: panel.copy.annotations > 0
                    text: qsTr("%Ln annotation files would be left out, as their text can name the patient.", "", panel.copy.annotations)
                }
            }

            // What no change to the attributes reaches: the picture.
            ColumnLayout {
                visible: panel.planned
                Layout.fillWidth: true
                spacing: 6
                Heading { text: qsTr("In the picture itself") }
                Para {
                    objectName: "anonymousRegions"
                    text: panel.copy.regions > 0 ? qsTr("%Ln regions will be blacked out, each in every image of its series.", "", panel.copy.regions)
                                                 : qsTr("No region is marked to black out.")
                }
                Para {
                    visible: panel.copy.pictures.some(row => row.flagged > 0)
                    text: qsTr("These series may show the patient's details in the picture, as ultrasound and screen captures do, which no change to the attributes takes out:")
                }
                Repeater {
                    model: panel.copy.pictures
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.leftMargin: 14
                        spacing: 14
                        Label {
                            text: modelData.name
                            color: win.text
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignLeft
                            Layout.fillWidth: true
                        }
                        Label {
                            text: panel.seriesLine(modelData)
                            color: win.muted
                        }
                        Label {
                            text: modelData.regions > 0 ? qsTr("%Ln regions blacked out", "", modelData.regions) : qsTr("Nothing blacked out")
                            color: modelData.regions > 0 ? theme.accent : win.error
                            font.bold: true
                        }
                    }
                }
                Para {
                    text: qsTr("To black a region out, close this, show the series, and press Shift+H: a box drawn on one image is blacked out of them all. Regions are kept until Dicolens closes.")
                }
            }

            // That it's checked, and what it follows.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Heading { text: qsTr("Checked, and by the standard") }
                Para {
                    text: qsTr("Each copy is read back and searched for the names, IDs, birth date, addresses, and serial numbers the scans held. A copy that still holds any of them isn't kept.")
                }
                Para {
                    objectName: "anonymousMethod"
                    text: (panel.copy.keepDescriptions
                           ? qsTr("It follows the table of DICOM PS3.15 %1's Basic Profile, but keeps the study, series, and protocol descriptions as they are, so the copy doesn't claim the profile. Dates and the patient's age, sex, size, and weight stay too.")
                           : qsTr("It follows DICOM PS3.15 %1, the Basic Application Level Confidentiality Profile. Dates and the patient's age, sex, size, and weight stay, as reading the scans needs them."))
                          .arg(panel.copy.edition)
                }
            }
        }
    }

    RowLayout {
        id: footer
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 20
        width: header.width
        spacing: 12

        Button {
            id: main
            objectName: "anonymousMain"
            visible: !panel.saving
            enabled: panel.saved || panel.copy.canSave
            // Keys go through the window's shortcuts, so Enter and Space
            // mean the same wherever the pointer last was.
            focusPolicy: Qt.NoFocus
            text: (panel.saved ? qsTr("Done") : qsTr("Choose a Folder…")) + "   Enter"
            highlighted: true
            Material.roundedScale: theme.radius
            Material.accent: theme.accent
            onClicked: panel.accept()
            // A highlighted button colors its caption by Material's own
            // rule, not Material.foreground, so it can't read on every accent.
            contentItem: Label {
                text: main.text
                font: main.font
                color: theme.accentForeground
                opacity: main.enabled ? 1 : 0.5
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
        Button {
            visible: panel.planning || panel.planned
            focusPolicy: Qt.NoFocus
            text: qsTr("Cancel") + "   Esc"
            flat: true
            Material.roundedScale: theme.radius
            onClicked: panel.copy.close()
        }
        Label {
            visible: panel.saving
            text: qsTr("Esc puts this away; the copy goes on.")
            color: win.muted
            horizontalAlignment: Text.AlignLeft
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Item { Layout.fillWidth: true; visible: !panel.saving }
    }
}
