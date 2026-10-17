import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Scans

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: backend.hasImages && !win.infoHidden && backend.info.patient ? backend.info.patient + " — Scans" : "Scans"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    readonly property color text: "#e8e8ec"
    readonly property color muted: "#8a8a94"
    readonly property color panel: "#141417"
    readonly property color line: "#26262c"

    property bool helpVisible: false
    property bool seriesVisible: true
    property bool tagsVisible: false
    property bool infoHidden: false
    property bool measuring: false
    // Letter keys go to the tag search while it's being typed in.
    readonly property bool typing: tagPanel.searching

    onTagsVisibleChanged: backend.tagsWanted = tagsVisible

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "O"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.open() }
    Shortcut { sequence: "Shift+O"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.openFolder() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.togglePlay() }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.step(-1) }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.step(1) }
    Shortcut { sequence: "PgUp"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.step(-10) }
    Shortcut { sequence: "PgDown"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.step(10) }
    Shortcut { sequence: "Home"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.sliceIndex = 0 }
    Shortcut { sequence: "End"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.sliceIndex = backend.sliceCount - 1 }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.stepSeries(-1) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.stepSeries(1) }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.preset(1) }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.preset(2) }
    Shortcut { sequence: "3"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.preset(3) }
    Shortcut { sequence: "4"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.preset(4) }
    Shortcut { sequence: "0"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.fileWindow() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.fullRange() }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.toggleInvert() }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.measuring = !win.measuring }
    Shortcut { sequence: "F"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: view.fit() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: { view.fit(); backend.fileWindow() } }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.seriesVisible = !win.seriesVisible }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.tagsVisible = !win.tagsVisible }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.infoHidden = !win.infoHidden }
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; onActivated: { win.tagsVisible = true; tagPanel.focusSearch() } }
    Shortcut { sequence: "Ctrl+E"; context: Qt.ApplicationShortcut; onActivated: backend.exportImage() }
    Shortcut { sequence: "Ctrl+C"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.copyImage() }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", StandardKey.Redo]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.redo() }
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.helpVisible) win.helpVisible = false
            else if (win.typing) tagPanel.leaveSearch()
            else if (view.rulerVisible) view.clearRuler()
            else if (win.measuring) win.measuring = false
            else if (backend.playing) backend.stop()
            else if (backend.problem) backend.dismissProblem()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        SeriesList {
            id: seriesList
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            width: visible ? 248 : 0
            visible: win.seriesVisible && backend.hasImages
        }

        TagPanel {
            id: tagPanel
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            width: visible ? Math.min(480, parent.width * 0.42) : 0
            visible: win.tagsVisible && backend.hasImages
        }

        Item {
            id: viewer
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: seriesList.right
            anchors.right: tagPanel.left
            clip: true

            // The image isn't mirrored; only the layout around it is.
            SliceView {
                id: view
                anchors.fill: parent
                source: backend
                accent: theme.accent
                LayoutMirroring.enabled: false
            }

            property string hoverText: ""
            property point hoverPoint: Qt.point(-1, -1)

            function refreshHover() {
                if (hoverPoint.x < 0) { hoverText = ""; return }
                const p = view.toImage(hoverPoint.x, hoverPoint.y)
                hoverText = backend.valueAt(p.x, p.y)
            }

            Connections {
                target: backend
                function onImageChanged() { viewer.refreshHover() }
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                hoverEnabled: true
                enabled: backend.hasImages
                cursorShape: win.measuring ? Qt.CrossCursor : drag === "pan" ? Qt.ClosedHandCursor : Qt.ArrowCursor
                LayoutMirroring.enabled: false

                property string drag: ""
                property point start
                property point last
                property real wheelRest: 0

                onPressed: (event) => {
                    start = Qt.point(event.x, event.y)
                    last = start
                    if (event.button === Qt.MiddleButton || (event.button === Qt.LeftButton && (event.modifiers & Qt.ShiftModifier)))
                        drag = "pan"
                    else if (event.button === Qt.RightButton)
                        drag = "zoom"
                    else if (win.measuring) {
                        drag = "ruler"
                        view.startRuler(event.x, event.y)
                    } else {
                        drag = "window"
                        backend.beginWindowDrag()
                    }
                }
                onPositionChanged: (event) => {
                    viewer.hoverPoint = Qt.point(event.x, event.y)
                    if (drag === "pan")
                        view.pan(event.x - last.x, event.y - last.y)
                    else if (drag === "zoom")
                        view.zoomAt(start.x, start.y, Math.pow(1.01, last.y - event.y))
                    else if (drag === "ruler")
                        view.moveRuler(event.x, event.y)
                    else if (drag === "window")
                        backend.dragWindow(event.x - start.x, event.y - start.y)
                    last = Qt.point(event.x, event.y)
                    viewer.refreshHover()
                }
                onReleased: drag = ""
                onExited: { viewer.hoverPoint = Qt.point(-1, -1); viewer.refreshHover() }
                onDoubleClicked: (event) => { if (event.button === Qt.LeftButton && !win.measuring) view.fit() }
                onWheel: (event) => {
                    if (event.modifiers & Qt.ControlModifier) {
                        view.zoomAt(event.x, event.y, Math.pow(1.0015, event.angleDelta.y))
                        return
                    }
                    // Touchpads send small steps; a slice for every notch's worth.
                    wheelRest += event.angleDelta.y
                    while (Math.abs(wheelRest) >= 120) {
                        backend.step(wheelRest > 0 ? -1 : 1)
                        wheelRest -= wheelRest > 0 ? 120 : -120
                    }
                }
            }

            DropArea {
                anchors.fill: parent
                onDropped: (drop) => { if (drop.hasUrls) backend.load(drop.urls) }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: !backend.hasImages && !backend.scanning
            }

            // Reading a folder.
            ColumnLayout {
                anchors.centerIn: parent
                visible: backend.scanning && !backend.hasImages
                spacing: 14
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: backend.scanTotal > 0 ? qsTr("Reading %1 of %2 files").arg(backend.scanDone.toLocaleString(Qt.locale(), 'f', 0)).arg(backend.scanTotal.toLocaleString(Qt.locale(), 'f', 0))
                                                : qsTr("Looking for scans…")
                    color: win.text
                    font.pixelSize: 16
                }
                ProgressBar {
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignHCenter
                    from: 0
                    to: Math.max(1, backend.scanTotal)
                    value: backend.scanDone
                    indeterminate: backend.scanTotal === 0
                }
            }

            Label {
                anchors.centerIn: parent
                width: Math.min(parent.width - 80, 520)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: backend.hasImages && backend.frameError !== ""
                text: backend.frameError
                color: "#ff8a80"
                font.pixelSize: 15
            }

            BusyIndicator {
                anchors.centerIn: parent
                width: 48
                height: 48
                running: backend.hasImages && !backend.frameReady && backend.frameError === "" && view.width > 0
                visible: running && backend.imageWidth === 0
            }

            InfoOverlay {
                anchors.fill: parent
                anchors.margins: 16
                visible: backend.hasImages && !win.infoHidden
                hoverText: viewer.hoverText
                zoom: view.zoom
                measuring: win.measuring
            }

            // Where in the series the shown slice is, and how much of it
            // is decoded. Click or drag to go anywhere in it.
            Item {
                id: scrubber
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 14
                visible: backend.sliceCount > 1
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: scrubArea.containsMouse || scrubArea.pressed ? 6 : 3
                    color: "#1f1f24"
                    Rectangle {
                        width: parent.width * backend.decodedCount / Math.max(1, backend.sliceCount)
                        height: parent.height
                        color: "#34343c"
                    }
                    Rectangle {
                        x: parent.width * backend.sliceIndex / backend.sliceCount
                        width: Math.max(3, parent.width / backend.sliceCount)
                        height: parent.height
                        color: theme.accent
                    }
                }
                MouseArea {
                    id: scrubArea
                    anchors.fill: parent
                    hoverEnabled: true
                    function seek(x) { backend.sliceIndex = Math.floor(Math.max(0, Math.min(0.9999, x / width)) * backend.sliceCount) }
                    onPressed: (event) => seek(event.x)
                    onPositionChanged: (event) => { if (pressed) seek(event.x) }
                }
            }

            Notice {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 48
            }
        }

        // A problem stays until it's dismissed or the next scan opens.
        Rectangle {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 14
            width: Math.min(parent.width - 40, problemRow.implicitWidth + 32)
            height: problemRow.implicitHeight + 20
            radius: Math.min(4, theme.radius)
            color: "#3a1d1f"
            border.color: "#6b2c30"
            visible: backend.problem !== ""
            RowLayout {
                id: problemRow
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 640
                    text: backend.problem
                    color: "#ffd6d6"
                    wrapMode: Text.Wrap
                }
                ToolButton {
                    text: "✕"
                    onClicked: backend.dismissProblem()
                }
            }
        }
    }

    HelpOverlay {
        anchors.fill: parent
        visible: win.helpVisible
    }
}
