import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Dicolens

// One slice view with all that goes over it: what's shown and where, the
// value under the pointer, the ruler, and the scrubber. Side by side, the
// window shows two, each with a backend of its own.
Item {
    id: pane

    property var source
    property bool second: false
    // Side by side, the pane the keys go to has the accent around it.
    property bool inUse: true
    readonly property alias view: view

    function activate() { win.rightActive = pane.second }

    Rectangle {
        anchors.fill: parent
        color: "black"
        visible: pane.source.hasImages
    }

    // The image isn't mirrored; only the layout around it is.
    SliceView {
        id: view
        objectName: pane.second ? "rightView" : "view"
        anchors.fill: parent
        source: pane.source
        accent: theme.accent
        LayoutMirroring.enabled: false
    }

    property string hoverText: ""
    property point hoverPoint: Qt.point(-1, -1)

    function refreshHover() {
        if (hoverPoint.x < 0) { hoverText = ""; return }
        const p = view.toImage(hoverPoint.x, hoverPoint.y)
        hoverText = pane.source.valueAt(p.x, p.y)
    }

    Connections {
        target: pane.source
        function onImageChanged() { pane.refreshHover() }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: true
        enabled: pane.source.hasImages
        cursorShape: win.measuring ? Qt.CrossCursor : drag === "pan" ? Qt.ClosedHandCursor : Qt.ArrowCursor
        LayoutMirroring.enabled: false

        property string drag: ""
        property point start
        property point last
        property real wheelRest: 0

        onPressed: (event) => {
            pane.activate()
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
                pane.source.beginWindowDrag()
            }
        }
        onPositionChanged: (event) => {
            pane.hoverPoint = Qt.point(event.x, event.y)
            if (drag === "pan")
                view.pan(event.x - last.x, event.y - last.y)
            else if (drag === "zoom")
                view.zoomAt(start.x, start.y, Math.pow(1.01, last.y - event.y))
            else if (drag === "ruler")
                view.moveRuler(event.x, event.y)
            else if (drag === "window")
                pane.source.dragWindow(event.x - start.x, event.y - start.y)
            last = Qt.point(event.x, event.y)
            pane.refreshHover()
        }
        onReleased: drag = ""
        onExited: { pane.hoverPoint = Qt.point(-1, -1); pane.refreshHover() }
        onDoubleClicked: (event) => { if (event.button === Qt.LeftButton && !win.measuring) view.fit() }
        onWheel: (event) => {
            pane.activate()
            if (event.modifiers & Qt.ControlModifier) {
                view.zoomAt(event.x, event.y, Math.pow(1.0015, event.angleDelta.y))
                return
            }
            // Touchpads send small steps; a slice for every notch's worth.
            wheelRest += event.angleDelta.y
            while (Math.abs(wheelRest) >= 120) {
                pane.source.step(wheelRest > 0 ? -1 : 1)
                wheelRest -= wheelRest > 0 ? 120 : -120
            }
        }
    }

    Label {
        anchors.centerIn: parent
        width: Math.min(parent.width - 80, 520)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: pane.source.hasImages && pane.source.frameError !== ""
        text: pane.source.frameError
        color: "#ff8a80"  // On the black canvas.
        font.pixelSize: 15
    }

    BusyIndicator {
        anchors.centerIn: parent
        width: 48
        height: 48
        running: pane.source.hasImages && !pane.source.frameReady && pane.source.frameError === "" && view.width > 0
        visible: running && pane.source.imageWidth === 0
    }

    InfoOverlay {
        anchors.fill: parent
        anchors.margins: 16
        source: pane.source
        visible: pane.source.hasImages && !win.infoHidden
        hoverText: pane.hoverText
        zoom: view.zoom
        measuring: win.measuring && pane.inUse
    }

    // Where in the series the shown slice is, and how much of it is
    // decoded. Click or drag to go anywhere in it.
    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 14
        visible: pane.source.sliceCount > 1
        LayoutMirroring.enabled: false
        LayoutMirroring.childrenInherit: true

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: scrubArea.containsMouse || scrubArea.pressed ? 6 : 3
            color: "#1f1f24"
            Rectangle {
                width: parent.width * pane.source.decodedCount / Math.max(1, pane.source.sliceCount)
                height: parent.height
                color: "#34343c"
            }
            Rectangle {
                x: parent.width * pane.source.sliceIndex / pane.source.sliceCount
                width: Math.max(3, parent.width / pane.source.sliceCount)
                height: parent.height
                color: theme.accent
            }
        }
        MouseArea {
            id: scrubArea
            anchors.fill: parent
            hoverEnabled: true
            function seek(x) { pane.source.sliceIndex = Math.floor(Math.max(0, Math.min(0.9999, x / width)) * pane.source.sliceCount) }
            onPressed: (event) => { pane.activate(); seek(event.x) }
            onPositionChanged: (event) => { if (pressed) seek(event.x) }
        }
    }

    Notice {
        source: pane.source
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 48
    }

    // Which pane the keys go to, side by side.
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: 2
        border.color: theme.accent
        visible: win.comparing && pane.inUse
    }
}
