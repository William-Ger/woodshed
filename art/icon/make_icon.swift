// Draws the Woodshed app icon (1024x1024 PNG): an amber guitar pick with a shed cut into it,
// its door lit, on a dark walnut tile. Usage: swift art/icon/make_icon.swift <out.png>
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let size = 1024
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                    space: cs, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
// Work top-down like a drawing program.
ctx.translateBy(x: 0, y: CGFloat(size))
ctx.scaleBy(x: 1, y: -1)

func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255,
            blue: CGFloat(hex & 0xff) / 255, alpha: a)
}
func gradient(_ stops: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: cs, colors: stops.map { $0.1 } as CFArray, locations: stops.map { $0.0 })!
}

// --- Tile (Apple's icon grid: 824 pt rounded square inset 100) --------------------------
let tile = CGRect(x: 100, y: 100, width: 824, height: 824)
let tilePath = CGPath(roundedRect: tile, cornerWidth: 185, cornerHeight: 185, transform: nil)

ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: 18), blur: 40, color: rgb(0x000000, 0.45))
ctx.addPath(tilePath); ctx.setFillColor(rgb(0x1a110b)); ctx.fillPath()
ctx.restoreGState()

ctx.saveGState()
ctx.addPath(tilePath); ctx.clip()
ctx.drawLinearGradient(gradient([(0, rgb(0x4a2f1c)), (0.55, rgb(0x2a1b11)), (1, rgb(0x150e09))]),
                       start: CGPoint(x: 512, y: 100), end: CGPoint(x: 512, y: 924), options: [])

// Walnut grain: long, gently wavering lines.
var seed: UInt64 = 7
func rnd() -> CGFloat { seed = seed &* 6364136223846793005 &+ 1442695040888963407; return CGFloat((seed >> 33) % 10000) / 10000 }
for i in 0..<70 {
    let y0 = 90 + CGFloat(i) * 12.5 + rnd() * 8
    let p = CGMutablePath()
    p.move(to: CGPoint(x: 80, y: y0))
    var x: CGFloat = 80
    while x < 960 {
        let nx = x + 120 + rnd() * 80
        p.addQuadCurve(to: CGPoint(x: nx, y: y0 + (rnd() - 0.5) * 14), control: CGPoint(x: (x + nx) / 2, y: y0 + (rnd() - 0.5) * 26))
        x = nx
    }
    ctx.addPath(p)
    ctx.setStrokeColor(i % 3 == 0 ? rgb(0x000000, 0.16) : rgb(0xffd9a0, 0.035))
    ctx.setLineWidth(i % 3 == 0 ? 2.5 : 1.5)
    ctx.strokePath()
}

// A pool of warm light behind the pick.
ctx.drawRadialGradient(gradient([(0, rgb(0xffb04a, 0.22)), (1, rgb(0xffb04a, 0))]),
                       startCenter: CGPoint(x: 512, y: 470), startRadius: 0,
                       endCenter: CGPoint(x: 512, y: 470), endRadius: 420, options: [])
ctx.restoreGState()

// Bevel: a faint light edge on top, dark at the bottom.
ctx.saveGState()
ctx.addPath(tilePath); ctx.clip()
ctx.addPath(CGPath(roundedRect: tile.insetBy(dx: 2, dy: 2), cornerWidth: 183, cornerHeight: 183, transform: nil))
ctx.setStrokeColor(rgb(0xffffff, 0.10)); ctx.setLineWidth(4); ctx.strokePath()
ctx.restoreGState()

// --- The pick --------------------------------------------------------------------------
let pick = CGMutablePath()
pick.move(to: CGPoint(x: 512, y: 236))
pick.addCurve(to: CGPoint(x: 772, y: 372), control1: CGPoint(x: 650, y: 236), control2: CGPoint(x: 772, y: 262))
pick.addCurve(to: CGPoint(x: 548, y: 800), control1: CGPoint(x: 772, y: 520), control2: CGPoint(x: 630, y: 700))
pick.addQuadCurve(to: CGPoint(x: 476, y: 800), control: CGPoint(x: 512, y: 842))
pick.addCurve(to: CGPoint(x: 252, y: 372), control1: CGPoint(x: 394, y: 700), control2: CGPoint(x: 252, y: 520))
pick.addCurve(to: CGPoint(x: 512, y: 236), control1: CGPoint(x: 252, y: 262), control2: CGPoint(x: 374, y: 236))
pick.closeSubpath()

ctx.saveGState()
ctx.setShadow(offset: CGSize(width: 0, height: 22), blur: 34, color: rgb(0x000000, 0.55))
ctx.addPath(pick); ctx.setFillColor(rgb(0xd88a2c)); ctx.fillPath()
ctx.restoreGState()

ctx.saveGState()
ctx.addPath(pick); ctx.clip()
ctx.drawLinearGradient(gradient([(0, rgb(0xffcf73)), (0.45, rgb(0xeea040)), (1, rgb(0xb5641c))]),
                       start: CGPoint(x: 400, y: 236), end: CGPoint(x: 600, y: 820), options: [])
// Celluloid sheen across the top.
let sheen = CGMutablePath()
sheen.addEllipse(in: CGRect(x: 250, y: 160, width: 520, height: 230))
ctx.addPath(sheen); ctx.clip()
ctx.drawLinearGradient(gradient([(0, rgb(0xffffff, 0.40)), (1, rgb(0xffffff, 0.0))]),
                       start: CGPoint(x: 512, y: 230), end: CGPoint(x: 512, y: 390), options: [])
ctx.restoreGState()

ctx.addPath(pick); ctx.setStrokeColor(rgb(0x7a3e0e, 0.55)); ctx.setLineWidth(3); ctx.strokePath()

// --- The shed, cut into the pick ---------------------------------------------------------
let shed = CGMutablePath()
// Roof with overhanging eaves, then the walls.
shed.move(to: CGPoint(x: 512, y: 366))
shed.addLine(to: CGPoint(x: 662, y: 486))
shed.addLine(to: CGPoint(x: 640, y: 486))
shed.addLine(to: CGPoint(x: 640, y: 632))
shed.addLine(to: CGPoint(x: 384, y: 632))
shed.addLine(to: CGPoint(x: 384, y: 486))
shed.addLine(to: CGPoint(x: 362, y: 486))
shed.closeSubpath()

ctx.saveGState()
ctx.addPath(pick); ctx.clip()
// Inset shadow so the shed reads as cut in, not stuck on.
ctx.setShadow(offset: CGSize(width: 0, height: -4), blur: 6, color: rgb(0xffe2a8, 0.6))
ctx.addPath(shed); ctx.setFillColor(rgb(0x21150c)); ctx.fillPath()
ctx.restoreGState()

// Board lines on the walls.
ctx.saveGState()
ctx.addPath(shed); ctx.clip()
for i in 0..<9 {
    let x = 384 + CGFloat(i) * 32
    ctx.move(to: CGPoint(x: x, y: 486)); ctx.addLine(to: CGPoint(x: x, y: 632))
}
ctx.setStrokeColor(rgb(0x000000, 0.35)); ctx.setLineWidth(3); ctx.strokePath()
ctx.restoreGState()

// The lit doorway, with light spilling out onto the pick below it.
let door = CGRect(x: 480, y: 540, width: 64, height: 92)
ctx.saveGState()
ctx.addPath(pick); ctx.clip()
ctx.drawRadialGradient(gradient([(0, rgb(0xfff1c4, 0.75)), (1, rgb(0xfff1c4, 0))]),
                       startCenter: CGPoint(x: 512, y: 640), startRadius: 0,
                       endCenter: CGPoint(x: 512, y: 660), endRadius: 120, options: [])
ctx.restoreGState()

ctx.saveGState()
ctx.addPath(CGPath(roundedRect: door, cornerWidth: 4, cornerHeight: 4, transform: nil)); ctx.clip()
ctx.drawLinearGradient(gradient([(0, rgb(0xfff4cf)), (1, rgb(0xffbe55))]),
                       start: CGPoint(x: 512, y: 540), end: CGPoint(x: 512, y: 632), options: [])
ctx.restoreGState()

// A small lit window in the gable.
let window = CGRect(x: 496, y: 436, width: 32, height: 32)
ctx.saveGState()
ctx.addPath(CGPath(roundedRect: window, cornerWidth: 3, cornerHeight: 3, transform: nil)); ctx.clip()
ctx.drawLinearGradient(gradient([(0, rgb(0xfff4cf)), (1, rgb(0xffbe55))]),
                       start: CGPoint(x: 512, y: 436), end: CGPoint(x: 512, y: 468), options: [])
ctx.restoreGState()
ctx.move(to: CGPoint(x: 512, y: 436)); ctx.addLine(to: CGPoint(x: 512, y: 468))
ctx.move(to: CGPoint(x: 496, y: 452)); ctx.addLine(to: CGPoint(x: 528, y: 452))
ctx.setStrokeColor(rgb(0x21150c)); ctx.setLineWidth(4); ctx.strokePath()

// --- Write -------------------------------------------------------------------------------
let out = URL(fileURLWithPath: CommandLine.arguments[1])
let dest = CGImageDestinationCreateWithURL(out as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
CGImageDestinationFinalize(dest)
print("wrote \(out.path)")
