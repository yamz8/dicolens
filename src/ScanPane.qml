import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Dicolens

// One slice view with all that goes over it: what's shown and where, the
// value under the pointer, the ruler, and the scrubber; or the series in
// 3D. Side by side, the window shows two, each with a backend of its own,
// and four views show three planes and the 3D.
Item {
    id: pane

    property var source
    property bool second: false
    // Of the four views, which this is; -1 outside them.
    property int quadIndex: -1
    // Side by side, the pane the keys go to has the accent around it.
    property bool inUse: true
    // Showing the series in 3D rather than a slice of it.
    property bool threeD: false
    property string viewName: second ? "rightView" : "view"
    // The views whose planes this one's lines show.
    property var reference: null
    property var secondReference: null
    readonly property alias view: view
    readonly property alias volume: volumeView

    function activate() {
        if (win.quad)
            win.quadActive = pane.quadIndex
        else
            win.rightActive = pane.second
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        visible: pane.source.hasImages
    }

    // The image isn't mirrored; only the layout around it is.
    SliceView {
        id: view
        objectName: pane.viewName
        anchors.fill: parent
        visible: !pane.threeD
        source: pane.source
        tool: win.tool
        // Each view shows where the others cut it.
        reference: pane.reference
        secondReference: pane.secondReference
        accent: theme.accent
        textScale: textSize.scale
        LayoutMirroring.enabled: false
    }

    VolumeView {
        id: volumeView
        objectName: pane.viewName + "3d"
        anchors.fill: parent
        visible: pane.threeD
        active: pane.threeD && pane.visible && pane.width > 0
        source: pane.source
        accent: theme.accent
        LayoutMirroring.enabled: false
    }

    property string hoverText: ""
    property point hoverPoint: Qt.point(-1, -1)

    function refreshHover() {
        if (hoverPoint.x < 0 || pane.threeD) { hoverText = ""; return }
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
        cursorShape: pane.threeD ? (drag === "pan" || drag === "turn3d" ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                   : win.tool !== 0 ? Qt.CrossCursor : drag === "pan" ? Qt.ClosedHandCursor
                   : drag === "turn" || overLine ? Qt.SizeAllCursor : Qt.ArrowCursor
        LayoutMirroring.enabled: false

        property string drag: ""
        property bool overLine: false
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
            else if (pane.threeD) {
                volumeView.stopSpin()
                if (event.modifiers & Qt.ControlModifier) {
                    // What values show, as a drag sets the window on a slice.
                    drag = "shift3d"
                    volumeView.beginShift()
                } else {
                    // A drag turns it; a click, where the planes meet.
                    drag = "click3d"
                }
            } else if (win.tool !== 0 || view.drawing) {
                drag = "measure"
                view.press(event.x, event.y)
            } else if (view.nearReference(event.x, event.y)) {
                // The other side's plane turns about where they meet.
                drag = "turn"
                view.beginTurn(event.x, event.y)
            } else {
                // A drag sets the window; a click, where the planes meet.
                drag = "click"
            }
        }
        onPositionChanged: (event) => {
            pane.hoverPoint = Qt.point(event.x, event.y)
            if (drag === "pan" && pane.threeD)
                volumeView.pan(event.x - last.x, event.y - last.y)
            else if (drag === "zoom" && pane.threeD)
                volumeView.zoomBy(Math.pow(1.01, last.y - event.y))
            else if (drag === "click3d" && Math.hypot(event.x - start.x, event.y - start.y) >= 3)
                drag = "turn3d"
            else if (drag === "shift3d")
                volumeView.shiftLook(event.x - start.x)
            else if (drag === "pan")
                view.pan(event.x - last.x, event.y - last.y)
            else if (drag === "zoom")
                view.zoomAt(start.x, start.y, Math.pow(1.01, last.y - event.y))
            else if (drag === "measure")
                view.drag(event.x, event.y)
            else if (drag === "turn")
                view.turnTo(event.x, event.y)
            else if (drag === "click" && Math.hypot(event.x - start.x, event.y - start.y) >= 3) {
                drag = "window"
                pane.source.beginWindowDrag()
            }
            if (drag === "turn3d")
                volumeView.turn(event.x - last.x, event.y - last.y)
            else if (drag === "window")
                pane.source.dragWindow(event.x - start.x, event.y - start.y)
            else if (drag === "" && !pane.threeD)
                view.hover(event.x, event.y)
            overLine = drag === "" && win.tool === 0 && !pane.threeD && view.nearReference(event.x, event.y)
            last = Qt.point(event.x, event.y)
            pane.refreshHover()
        }
        onReleased: (event) => {
            if (drag === "measure")
                view.release(event.x, event.y)
            else if (drag === "click") {
                // A place in the patient counts from the middle of the first
                // pixel; the picture from its corner.
                const p = view.toImage(event.x, event.y)
                win.goThroughAll(pane.source.pointAt(p.x - 0.5, p.y - 0.5))
            } else if (drag === "click3d") {
                win.goThroughAll(volumeView.pointAt(event.x, event.y))
            }
            drag = ""
        }
        onExited: { pane.hoverPoint = Qt.point(-1, -1); pane.refreshHover() }
        onDoubleClicked: (event) => {
            if (event.button === Qt.LeftButton && pane.threeD)
                volumeView.fit()
            else if (event.button === Qt.LeftButton && win.tool === 0)
                view.fit()
        }
        onWheel: (event) => {
            pane.activate()
            if (pane.threeD) {
                // The wheel cuts into it from the front; with Ctrl, zooms.
                if (event.modifiers & Qt.ControlModifier)
                    volumeView.zoomBy(Math.pow(1.0015, event.angleDelta.y))
                else
                    volumeView.cutDeeper(-event.angleDelta.y / 120 * 5)
                return
            }
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
        visible: pane.source.hasImages && text !== ""
        text: pane.threeD ? (volumeView.busy ? "" : volumeView.status) : pane.source.frameError
        color: "#ff8a80"  // On the black canvas.
        font.pixelSize: win.px(15)
    }

    BusyIndicator {
        id: busy
        anchors.centerIn: parent
        width: 48
        height: 48
        running: pane.threeD ? volumeView.busy
                             : pane.source.hasImages && !pane.source.frameReady && pane.source.frameError === "" && view.width > 0
        visible: running && (pane.threeD || pane.source.imageWidth === 0)
    }

    Label {
        anchors.top: busy.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        visible: busy.visible && pane.threeD
        text: volumeView.status
        color: win.canvasText
    }

    InfoOverlay {
        anchors.fill: parent
        anchors.margins: 16
        source: pane.source
        volume: pane.threeD ? volumeView : null
        compact: win.quad && pane.quadIndex > 0
        visible: pane.source.hasImages && !win.infoHidden
        hoverText: pane.hoverText
        zoom: pane.threeD ? volumeView.zoom : view.zoom
        tool: pane.inUse ? win.tool : 0
    }

    // Where in the series the shown slice is, and how much of it is
    // decoded. Click or drag to go anywhere in it.
    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 14
        visible: pane.source.sliceCount > 1 && !pane.threeD
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
                x: parent.width * pane.source.sliceIndex / Math.max(1, pane.source.sliceCount)
                width: Math.max(3, parent.width / Math.max(1, pane.source.sliceCount))
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
        visible: (win.comparing || win.quad) && pane.inUse
    }
}
