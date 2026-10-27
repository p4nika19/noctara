import QtQuick
import Noctara

// The shared backdrop blur. The backdrop is captured once at a reduced resolution and blurred once per frame
// in which it changes; every glass panel then samples the result behind itself (see GlassPanel.qml), so the
// cost does not grow with the number of panels. When the backdrop is static, nothing is re-rendered.
//
//   kawase, liquid  capture -> Dual Kawase down x N -> up x N (N = Theme.glass.blurIterations, max 5)
//   gaussian        capture -> horizontal pass -> vertical pass
//
// Pass parameters come from Theme.glass (blurPlan() in C++). GPU only: the owner loads this item only when
// Theme.glass.blurred is set, which never happens in the software renderer.
Item {
    id: root

    required property Item source
    // Texture provider with the blurred backdrop, sampled by the glass panels.
    readonly property Item output: gaussian ? vertical : upSources[0]

    readonly property bool gaussian: Theme.glass.style === "gaussian"
    readonly property int iterations: gaussian ? 0 : Math.max(1, Math.min(5, Theme.glass.blurIterations))
    readonly property size captureSize: Qt.size(Math.max(1, Math.ceil(width / Theme.glass.blurDownsample)),
                                                Math.max(1, Math.ceil(height / Theme.glass.blurDownsample)))
    readonly property var downSources: [down1, down2, down3, down4, down5]
    readonly property var upSources: [up0, up1, up2, up3, up4]

    function levelSize(level) {
        const f = Math.pow(2, level)
        return Qt.size(Math.max(1, Math.ceil(captureSize.width / f)), Math.max(1, Math.ceil(captureSize.height / f)))
    }

    // Texture providers must stay visible to be updated; a zero size keeps them from drawing anything.
    ShaderEffectSource {
        id: capture
        width: 0
        height: 0
        sourceItem: root.source
        textureSize: root.captureSize
        smooth: true
    }

    // One blur pass: a hidden shader item rendered into its own texture.
    component Pass: ShaderEffectSource {
        id: pass
        property bool used: true
        property Item input
        property size inputSize
        property size size
        property string shader
        property vector2d direction  // gaussian: one texel along the blur axis
        property real amount         // kawase: tap offset in texels; gaussian: sigma in texels
        width: 0
        height: 0
        smooth: true
        hideSource: true
        textureSize: size
        sourceItem: used ? effect : null
        readonly property ShaderEffect effect: ShaderEffect {
            parent: root
            width: root.width
            height: root.height
            visible: false
            property Item source: pass.input
            property vector2d texel: Qt.vector2d(1 / Math.max(1, pass.inputSize.width), 1 / Math.max(1, pass.inputSize.height))
            property real offset: pass.amount
            property vector2d direction: pass.direction
            property real sigma: pass.amount
            fragmentShader: pass.shader
        }
    }

    // --- Dual Kawase --------------------------------------------------------------------------------------
    Pass {
        id: down1
        used: !root.gaussian
        input: capture
        inputSize: root.captureSize
        size: root.levelSize(1)
        shader: "qrc:/shaders/kawase_down.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: down2
        used: !root.gaussian && root.iterations >= 2
        input: down1
        inputSize: down1.size
        size: root.levelSize(2)
        shader: "qrc:/shaders/kawase_down.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: down3
        used: !root.gaussian && root.iterations >= 3
        input: down2
        inputSize: down2.size
        size: root.levelSize(3)
        shader: "qrc:/shaders/kawase_down.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: down4
        used: !root.gaussian && root.iterations >= 4
        input: down3
        inputSize: down3.size
        size: root.levelSize(4)
        shader: "qrc:/shaders/kawase_down.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: down5
        used: !root.gaussian && root.iterations >= 5
        input: down4
        inputSize: down4.size
        size: root.levelSize(5)
        shader: "qrc:/shaders/kawase_down.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    // Up pass k reads level k + 1: the deepest down pass for the first one, the previous up pass otherwise.
    Pass {
        id: up4
        used: !root.gaussian && root.iterations >= 5
        input: down5
        inputSize: down5.size
        size: root.levelSize(4)
        shader: "qrc:/shaders/kawase_up.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: up3
        used: !root.gaussian && root.iterations >= 4
        input: root.iterations === 4 ? down4 : up4
        inputSize: root.levelSize(4)
        size: root.levelSize(3)
        shader: "qrc:/shaders/kawase_up.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: up2
        used: !root.gaussian && root.iterations >= 3
        input: root.iterations === 3 ? down3 : up3
        inputSize: root.levelSize(3)
        size: root.levelSize(2)
        shader: "qrc:/shaders/kawase_up.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: up1
        used: !root.gaussian && root.iterations >= 2
        input: root.iterations === 2 ? down2 : up2
        inputSize: root.levelSize(2)
        size: root.levelSize(1)
        shader: "qrc:/shaders/kawase_up.frag.qsb"
        amount: Theme.glass.blurOffset
    }
    Pass {
        id: up0
        used: !root.gaussian
        input: root.iterations === 1 ? down1 : up1
        inputSize: root.levelSize(1)
        size: root.captureSize
        shader: "qrc:/shaders/kawase_up.frag.qsb"
        amount: Theme.glass.blurOffset
    }

    // --- Gaussian -----------------------------------------------------------------------------------------
    Pass {
        id: horizontal
        used: root.gaussian
        input: capture
        inputSize: root.captureSize
        size: root.captureSize
        shader: "qrc:/shaders/gaussian.frag.qsb"
        direction: Qt.vector2d(1 / root.captureSize.width, 0)
        amount: Theme.glass.blurSigma
    }
    Pass {
        id: vertical
        used: root.gaussian
        input: horizontal
        inputSize: root.captureSize
        size: root.captureSize
        shader: "qrc:/shaders/gaussian.frag.qsb"
        direction: Qt.vector2d(0, 1 / root.captureSize.height)
        amount: Theme.glass.blurSigma
    }
}
