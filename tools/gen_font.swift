// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// src/mm_font.c: Michroma's letters as coverage bitmaps for the plug-in's screen (src/mm_screen.c), the panel lettering
// of its faceplates, at 11, 13, 15 and 18 pixels to the em. macOS only (CoreText); the file is kept in the repository,
// so a build never needs this.
//
//   xcrun swift tools/gen_font.swift DIR > src/mm_font.c      # DIR: Michroma-Regular.ttf (OFL, Google Fonts' build)
import CoreGraphics
import CoreText
import Foundation

let dir = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "."
// name, file, em (px)
let faces: [(String, String, CGFloat)] = [
    ("m11", "Michroma-Regular.ttf", 11), ("m13", "Michroma-Regular.ttf", 13), ("m15", "Michroma-Regular.ttf", 15),
    ("m18", "Michroma-Regular.ttf", 18),
]
// ASCII 32..126, then the middle dot
let chars: [UniChar] = Array(32...126).map { UniChar($0) } + [0x00B7]

func font(_ file: String, _ em: CGFloat) -> CTFont {
    let url = URL(fileURLWithPath: dir).appendingPathComponent(file)
    guard let provider = CGDataProvider(url: url as CFURL), let cg = CGFont(provider) else {
        fputs("cannot read \(url.path)\n", stderr)
        exit(1)
    }
    return CTFontCreateWithGraphicsFont(cg, em, nil, nil)
}

var out = """
/* SPDX-License-Identifier: OFL-1.1
 * Michroma, Copyright 2011 The Michroma Project Authors (https://github.com/googlefonts/Michroma-font), SIL Open Font
 * License 1.1 (LICENSES/OFL-1.1.txt): its letters drawn as coverage bitmaps (mm_font.h). Written by
 * tools/gen_font.swift: change the script, not this file. */
#include "mm_font.h"


"""

var faceLines: [String] = []
for (name, file, em) in faces {
    let f = font(file, em)
    var glyphs = [CGGlyph](repeating: 0, count: chars.count)
    _ = CTFontGetGlyphsForCharacters(f, chars, &glyphs, chars.count)
    var bits: [UInt8] = []
    var table: [String] = []
    for (i, g) in glyphs.enumerated() {
        var glyph = g
        var adv = CGSize.zero
        CTFontGetAdvancesForGlyphs(f, .horizontal, &glyph, &adv, 1)
        var rect = CGRect.zero
        CTFontGetBoundingRectsForGlyphs(f, .horizontal, &glyph, &rect, 1)
        let a64 = Int((adv.width * 64).rounded())
        if g == 0 || rect.isEmpty || chars[i] == 32 {
            table.append("{0, 0, 0, 0, \(a64), 0}")
            continue
        }
        let x0 = Int(floor(rect.minX)) - 1, x1 = Int(ceil(rect.maxX)) + 1
        let y0 = Int(floor(rect.minY)) - 1, y1 = Int(ceil(rect.maxY)) + 1
        let w = x1 - x0, h = y1 - y0
        let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                            space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
        ctx.setAllowsFontSmoothing(false)
        ctx.setShouldSmoothFonts(false)
        ctx.setAllowsFontSubpixelPositioning(false)
        ctx.setAllowsFontSubpixelQuantization(false)
        ctx.setShouldAntialias(true)
        ctx.setFillColor(gray: 0, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.setFillColor(gray: 1, alpha: 1)
        var pos = CGPoint(x: CGFloat(-x0), y: CGFloat(-y0))
        CTFontDrawGlyphs(f, &glyph, &pos, 1, ctx)
        let data = ctx.data!.bindMemory(to: UInt8.self, capacity: w * h)
        let at = bits.count
        for row in 0..<h {                           // (CoreGraphics' first row is the top one)
            for col in 0..<w { bits.append(data[row * w + col]) }
        }
        table.append("{\(x0), \(-y1), \(w), \(h), \(a64), \(at)}")
    }
    out += "static const mm_glyph MM_G_\(name.uppercased())[] = {\n"
    for i in stride(from: 0, to: table.count, by: 6) {
        out += "    " + table[i..<min(i + 6, table.count)].joined(separator: ", ") + ",\n"
    }
    out += "};\nstatic const uint8_t MM_B_\(name.uppercased())[] = {\n"
    for i in stride(from: 0, to: bits.count, by: 24) {
        out += "    " + bits[i..<min(i + 24, bits.count)].map { String($0) }.joined(separator: ",") + ",\n"
    }
    out += "};\n"
    let ascent = Int(CTFontGetAscent(f).rounded()), descent = Int(CTFontGetDescent(f).rounded())
    let cap = Int(CTFontGetCapHeight(f).rounded())
    faceLines.append("const mm_face MM_FACE_\(name.uppercased()) = {\(Int(em)), \(ascent), \(descent), \(cap), MM_G_\(name.uppercased()), MM_B_\(name.uppercased())};")
}
out += "\n" + faceLines.joined(separator: "\n") + "\n"
print(out, terminator: "")
