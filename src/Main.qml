import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Dicolens

ApplicationWindow {
    id: win
    width: 1280
    height: 820
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: backend.hasImages && !win.infoHidden && backend.info.patient ? backend.info.patient + " — Dicolens" : "Dicolens"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    // The window's shades, mixed from the theme so they read on light
    // themes as well as dark ones.
    function mix(base, over, amount) { return Qt.tint(base, Qt.rgba(over.r, over.g, over.b, amount)) }
    readonly property color text: theme.foreground
    readonly property color secondary: mix(theme.foreground, theme.background, 0.2)
    readonly property color muted: mix(theme.foreground, theme.background, 0.45)
    readonly property color panel: mix(theme.background, theme.foreground, 0.03)
    readonly property color hover: mix(theme.background, theme.foreground, 0.06)
    readonly property color selected: mix(theme.background, theme.foreground, 0.1)
    readonly property color line: mix(theme.background, theme.foreground, 0.12)
    readonly property color error: theme.colors.red

    // Scans are read on black, whatever the theme, as every radiology viewer
    // shows them, so what's drawn over the image keeps light colors of its own.
    readonly property color canvasText: "#e8e8ec"
    readonly property color canvasDim: "#c8c8d0"

    property bool helpVisible: false
    property bool seriesVisible: true
    property bool tagsVisible: false
    property bool infoHidden: false
    // 0 looks; 1, 2, and 3 measure lengths, areas, and angles.
    property int tool: 0
    function useTool(t) { win.tool = win.tool === t ? 0 : t }
    // Side by side: two panes on the same scans, the keys going to one,
    // the other following it from slice to slice while linked.
    property bool comparing: false
    property bool rightActive: false
    property bool linked: true
    readonly property var inUse: comparing && rightActive ? partner : backend
    readonly property var other: inUse === backend ? partner : backend
    readonly property var inUseView: comparing && rightActive ? rightPane.view : leftPane.view
    readonly property bool reading: backend.reportIndex >= 0
    // Letter keys go to the tag search while it's being typed in.
    readonly property bool typing: tagPanel.searching
    readonly property bool keys: !typing && !reading

    onTagsVisibleChanged: { backend.tagsWanted = tagsVisible; partner.tagsWanted = tagsVisible }

    function compare() {
        if (!backend.hasImages)
            return
        comparing = !comparing
        if (comparing) {
            partner.share(backend)
            if (linked)
                partner.follow(backend)
            rightActive = true
        } else {
            rightActive = false
        }
    }

    // While linked, the pane not in use follows the one that is; a series
    // just picked on either side starts where the other side is instead.
    property int leftSeries: -1
    property int rightSeries: -1
    Connections {
        target: backend
        function onSliceChanged() {
            const picked = backend.seriesIndex !== win.leftSeries
            win.leftSeries = backend.seriesIndex
            if (!win.comparing || !win.linked)
                return
            if (picked)
                backend.follow(partner)
            else if (!win.rightActive)
                partner.follow(backend)
        }
        function onOpened() {
            if (!win.comparing)
                return
            partner.share(backend)
            if (win.linked)
                partner.follow(backend)
        }
    }
    Connections {
        target: partner
        function onSliceChanged() {
            const picked = partner.seriesIndex !== win.rightSeries
            win.rightSeries = partner.seriesIndex
            if (!win.comparing || !win.linked)
                return
            if (picked)
                partner.follow(backend)
            else if (win.rightActive)
                backend.follow(partner)
        }
    }

    Shortcut { sequence: "?"; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "O"; enabled: win.keys; onActivated: backend.open() }
    Shortcut { sequence: "Shift+O"; enabled: win.keys; onActivated: backend.openFolder() }
    Shortcut { sequence: "Space"; enabled: win.keys; onActivated: win.inUse.togglePlay() }
    Shortcut { sequences: ["Up", "K"]; enabled: win.keys; onActivated: win.inUse.step(-1) }
    Shortcut { sequences: ["Down", "J"]; enabled: win.keys; onActivated: win.inUse.step(1) }
    Shortcut { sequence: "PgUp"; enabled: win.keys; onActivated: win.inUse.step(-10) }
    Shortcut { sequence: "PgDown"; enabled: win.keys; onActivated: win.inUse.step(10) }
    Shortcut { sequence: "Home"; enabled: win.keys; onActivated: win.inUse.sliceIndex = 0 }
    Shortcut { sequence: "End"; enabled: win.keys; onActivated: win.inUse.sliceIndex = win.inUse.sliceCount - 1 }
    Shortcut { sequence: "Left"; enabled: win.keys; onActivated: win.inUse.stepSeries(-1) }
    Shortcut { sequence: "Right"; enabled: win.keys; onActivated: win.inUse.stepSeries(1) }
    Shortcut { sequence: "1"; enabled: win.keys; onActivated: win.inUse.preset(1) }
    Shortcut { sequence: "2"; enabled: win.keys; onActivated: win.inUse.preset(2) }
    Shortcut { sequence: "3"; enabled: win.keys; onActivated: win.inUse.preset(3) }
    Shortcut { sequence: "4"; enabled: win.keys; onActivated: win.inUse.preset(4) }
    Shortcut { sequence: "0"; enabled: win.keys; onActivated: win.inUse.fileWindow() }
    Shortcut { sequence: "A"; enabled: win.keys; onActivated: win.inUse.fullRange() }
    Shortcut { sequence: "I"; enabled: win.keys; onActivated: win.inUse.toggleInvert() }
    Shortcut { sequence: "M"; enabled: win.keys; onActivated: win.useTool(1) }
    Shortcut { sequence: "E"; enabled: win.keys; onActivated: win.useTool(2) }
    Shortcut { sequence: "Shift+M"; enabled: win.keys; onActivated: win.useTool(3) }
    Shortcut { sequence: "Shift+C"; enabled: win.keys; onActivated: win.useTool(4) }
    Shortcut { sequence: "Ctrl+Left"; enabled: win.keys; onActivated: win.inUse.tilt(0, -5) }
    Shortcut { sequence: "Ctrl+Right"; enabled: win.keys; onActivated: win.inUse.tilt(0, 5) }
    Shortcut { sequence: "Ctrl+Up"; enabled: win.keys; onActivated: win.inUse.tilt(1, -5) }
    Shortcut { sequence: "Ctrl+Down"; enabled: win.keys; onActivated: win.inUse.tilt(1, 5) }
    Shortcut { sequences: ["Delete", "Backspace"]; enabled: win.keys; onActivated: win.inUseView.removeLast() }
    Shortcut { sequence: "Shift+Delete"; enabled: win.keys; onActivated: win.inUseView.clearMeasures() }
    Shortcut { sequence: "G"; enabled: win.keys; onActivated: win.inUse.nextColorMap() }
    Shortcut { sequence: "F"; enabled: win.keys; onActivated: win.inUseView.fit() }
    Shortcut { sequence: "R"; enabled: win.keys; onActivated: { win.inUseView.fit(); win.inUse.resetOrientation(); win.inUse.fileWindow() } }
    Shortcut { sequence: "]"; enabled: win.keys; onActivated: win.inUse.rotate(1) }
    Shortcut { sequence: "["; enabled: win.keys; onActivated: win.inUse.rotate(-1) }
    Shortcut { sequence: "X"; enabled: win.keys; onActivated: win.inUse.flipHorizontal() }
    Shortcut { sequence: "Y"; enabled: win.keys; onActivated: win.inUse.flipVertical() }
    Shortcut { sequence: "N"; enabled: win.keys; onActivated: { backend.toggleMarks(); if (partner.marksShown !== backend.marksShown) partner.toggleMarks() } }
    Shortcut { sequence: "C"; enabled: win.keys; onActivated: win.compare() }
    Shortcut { sequence: "V"; enabled: win.keys; onActivated: win.inUse.nextPlane() }
    Shortcut { sequence: "B"; enabled: win.keys; onActivated: win.inUse.nextSlab() }
    Shortcut { sequence: "Shift+B"; enabled: win.keys; onActivated: win.inUse.nextSlabThickness() }
    Shortcut { sequence: "Tab"; enabled: win.keys && win.comparing; onActivated: win.rightActive = !win.rightActive }
    Shortcut {
        sequence: "L"
        enabled: win.keys && win.comparing
        onActivated: {
            win.linked = !win.linked
            if (win.linked) win.other.follow(win.inUse)
            win.inUse.announce(win.linked ? qsTr("Scrolling together") : qsTr("Scrolling apart"))
        }
    }
    Shortcut { sequence: "P"; enabled: !win.typing; onActivated: win.reading ? backend.closeReport() : backend.openStudyReport() }
    Shortcut { sequences: ["Left", "Shift+Tab"]; enabled: win.reading; onActivated: backend.stepReport(-1) }
    Shortcut { sequences: ["Right", "Tab"]; enabled: win.reading; onActivated: backend.stepReport(1) }
    Shortcut { sequences: ["Up", "K"]; enabled: win.reading; onActivated: reportPanel.scroll(-60) }
    Shortcut { sequences: ["Down", "J"]; enabled: win.reading; onActivated: reportPanel.scroll(60) }
    Shortcut { sequence: "PgUp"; enabled: win.reading; onActivated: reportPanel.scroll(-reportPanel.height * 0.8) }
    Shortcut { sequences: ["PgDown", "Space"]; enabled: win.reading; onActivated: reportPanel.scroll(reportPanel.height * 0.8) }
    Shortcut { sequences: ["Return", "Enter"]; enabled: win.reading && backend.reportIsPdf; onActivated: backend.openReport(backend.reportIndex) }
    Shortcut { sequence: "S"; enabled: win.keys; onActivated: win.seriesVisible = !win.seriesVisible }
    Shortcut { sequence: "T"; enabled: win.keys; onActivated: win.tagsVisible = !win.tagsVisible }
    Shortcut { sequence: "H"; enabled: win.keys; onActivated: win.infoHidden = !win.infoHidden }
    Shortcut { sequence: "Ctrl+F"; enabled: !win.reading; onActivated: { win.tagsVisible = true; tagPanel.focusSearch() } }
    Shortcut { sequence: "Ctrl+E"; onActivated: win.inUse.exportImage() }
    Shortcut { sequence: "Ctrl+Shift+E"; enabled: win.keys; onActivated: win.inUse.exportVideo() }
    Shortcut { sequence: "Ctrl+Shift+S"; enabled: win.keys; onActivated: backend.exportAnonymous() }
    Shortcut { sequence: "Ctrl+C"; enabled: win.keys; onActivated: win.inUse.copyImage() }
    Shortcut { sequences: [StandardKey.Undo]; enabled: win.keys; onActivated: win.inUse.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", StandardKey.Redo]; enabled: win.keys; onActivated: win.inUse.redo() }
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (win.helpVisible) win.helpVisible = false
            else if (win.reading) backend.closeReport()
            else if (win.typing) tagPanel.leaveSearch()
            else if (win.inUseView.drawing) win.inUseView.removeLast()
            else if (win.tool !== 0) win.tool = 0
            else if (win.inUse.playing) win.inUse.stop()
            else if (backend.problem || partner.problem) { backend.dismissProblem(); partner.dismissProblem() }
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
            objectName: "seriesList"
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
            objectName: "viewer"
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.left: seriesList.right
            anchors.right: tagPanel.left
            clip: true

            // Side by side, the panes split the width; the image in each
            // isn't mirrored, but which pane is first follows the language.
            ScanPane {
                id: leftPane
                objectName: "leftPane"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                width: win.comparing ? (parent.width - 2) / 2 : parent.width
                source: backend
                inUse: !win.rightActive
            }

            Rectangle {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: leftPane.right
                width: 2
                color: win.line
                visible: win.comparing
            }

            ScanPane {
                id: rightPane
                objectName: "rightPane"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: (parent.width - 2) / 2
                visible: win.comparing
                source: partner
                second: true
                inUse: win.rightActive
            }

            DropArea {
                anchors.fill: parent
                onDropped: (drop) => { if (drop.hasUrls) backend.load(drop.urls) }
            }

            EmptyState {
                objectName: "emptyState"
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

            ReportPanel {
                id: reportPanel
                objectName: "reportPanel"
                anchors.fill: parent
                visible: win.reading
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
            color: win.mix(theme.background, win.error, 0.16)
            border.color: win.mix(theme.background, win.error, 0.5)
            visible: backend.problem !== "" || partner.problem !== ""
            RowLayout {
                id: problemRow
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 640
                    text: backend.problem || partner.problem
                    color: win.text
                    wrapMode: Text.Wrap
                }
                ToolButton {
                    text: "✕"
                    Material.roundedScale: theme.radius
                    onClicked: { backend.dismissProblem(); partner.dismissProblem() }
                }
            }
        }
    }

    HelpOverlay {
        objectName: "help"
        anchors.fill: parent
        visible: win.helpVisible
    }
}
