// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libheif contributors
// Independent public Apple-framework consumer and producer for synthetic tmaps.
import Foundation
import CoreGraphics
import CoreImage
import ImageIO

func require(_ condition: Bool, _ message: String) {
    if !condition {
        FileHandle.standardError.write(Data((message + "\n").utf8))
        exit(1)
    }
}

guard CommandLine.arguments.count == 2 else {
    print("usage: swift scripts/check-tmap-apple.swift SYNTHETIC_FIXTURE_DIRECTORY")
    exit(64)
}
let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let linearSpace = CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!
let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!
let context = CIContext(options: [.workingColorSpace: linearSpace,
                                  .outputColorSpace: linearSpace,
                                  .cacheIntermediates: false])
let signal = 192.0 / 255.0
let baselineLinear = pow((signal + 0.055) / 1.055, 2.4)
let bounds = CGRect(x: 0, y: 0, width: 64, height: 64)
let hdrOptions = [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR,
                  kCGImageSourceShouldCache: true,
                  kCGImageSourceShouldAllowFloat: true,
                  // Check the full alternate, without a generated display curve.
                  kCGImageSourceDecodeRequestOptions:
                    [kCGImageSourceGenerateImageSpecificLumaScaling: false]] as CFDictionary
var failures = [String]()

for rgb in [false, true] {
    let name = rgb ? "libheif-rgb-pq.heic" : "libheif-mono-pq.heic"
    let url = directory.appendingPathComponent(name)
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
        require(false, "ImageIO cannot open \(name)")
        fatalError()
    }
    require(CGImageSourceCopyAuxiliaryDataInfoAtIndex(source, 0,
            kCGImageAuxiliaryDataTypeISOGainMap) != nil, "Missing ISO auxiliary data in \(name)")
    // Auxiliary discovery may populate the source's default SDR decode cache.
    // Use a fresh source with the same explicit HDR options at both entry points.
    guard let hdrSource = CGImageSourceCreateWithURL(url as CFURL, hdrOptions),
          let decoded = CGImageSourceCreateImageAtIndex(hdrSource, 0, hdrOptions) else {
        require(false, "ImageIO HDR decode failed for \(name)")
        fatalError()
    }
    require(decoded.width == 64 && decoded.height == 64, "Incorrect output geometry")
    let image = CIImage(cgImage: decoded)
    var pixels = [Float](repeating: 0, count: 64 * 64 * 4)
    pixels.withUnsafeMutableBytes { bytes in
        context.render(image, toBitmap: bytes.baseAddress!, rowBytes: 64 * 16,
                       bounds: bounds, format: .RGBAf, colorSpace: linearSpace)
    }
    let gains = rgb ? [4.0, exp2(128.0 / 255.0), exp2(64.0 / 255.0)] : [4.0, 4.0, 4.0]
    var maximumError = 0.0
    var maximumLinearValue = 0.0
    for i in 0..<(64 * 64) {
        for c in 0..<3 {
            let value = Double(pixels[i * 4 + c])
            require(value.isFinite, "Non-finite Apple HDR pixel")
            maximumLinearValue = max(maximumLinearValue, value)
            maximumError = max(maximumError, abs(value - baselineLinear * gains[c]))
        }
    }
    // Independently checked extended-linear pixels establish that DecodeToHDR
    // applied the gain map; contentHeadroom remains diagnostic only.
    print("CHECK Apple consumer \(name) contentHeadroom=\(decoded.contentHeadroom) maxLinear=\(maximumLinearValue) maxLinearError=\(maximumError) bits=\(decoded.bitsPerComponent) space=\(String(describing: decoded.colorSpace?.name))")
    // An independent public Core Image load helps distinguish the ImageIO
    // decode-request path from file discovery/reconstruction on hosted Macs.
    // It does not replace or relax the ImageIO pixel gate below.
    if let direct = CIImage(contentsOf: url, options: [.expandToHDR: true]) {
        var directPixels = [Float](repeating: 0, count: 64 * 64 * 4)
        directPixels.withUnsafeMutableBytes { bytes in
            context.render(direct, toBitmap: bytes.baseAddress!, rowBytes: 64 * 16,
                           bounds: bounds, format: .RGBAf, colorSpace: linearSpace)
        }
        var directError = 0.0
        var directMaximum = 0.0
        for i in 0..<(64 * 64) {
            for c in 0..<3 {
                let value = Double(directPixels[i * 4 + c])
                require(value.isFinite, "Non-finite Core Image HDR pixel")
                directMaximum = max(directMaximum, value)
                directError = max(directError, abs(value - baselineLinear * gains[c]))
            }
        }
        print("CHECK Core Image consumer \(name) maxLinear=\(directMaximum) maxLinearError=\(directError) space=\(String(describing: direct.colorSpace?.name))")
    }
    if maximumLinearValue <= 1.0 {
        failures.append("ImageIO HDR decode stayed in SDR range for \(name)")
    }
    // Includes 8-bit YCbCr codec/matrix rounding and Apple colour management.
    if maximumError >= 0.025 {
        failures.append("Apple HDR pixel error \(maximumError) for \(name)")
    }
    guard let sdr = CGImageSourceCreateImageAtIndex(source, 0,
            [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToSDR] as CFDictionary) else {
        fatalError("Missing SDR fallback")
    }
    var sdrPixels = [Float](repeating: 0, count: 64 * 64 * 4)
    sdrPixels.withUnsafeMutableBytes { bytes in
        context.render(CIImage(cgImage: sdr), toBitmap: bytes.baseAddress!, rowBytes: 64 * 16,
                       bounds: bounds, format: .RGBAf, colorSpace: linearSpace)
    }
    require(sdrPixels.enumerated().filter { $0.offset % 4 != 3 }.allSatisfy {
        $0.element.isFinite && abs(Double($0.element) - baselineLinear) < 0.01
    }, "Apple SDR fallback differs from baseline")
    if maximumLinearValue > 1.0 && maximumError < 0.025 {
        print("PASS Apple consumer \(name)")
    }
}

// Apple producer, with separately constructed SDR and HDR intentions.
let baseline = CIImage(color: CIColor(red: signal, green: signal, blue: signal,
                                     colorSpace: sRGB)!).cropped(to: bounds)
let hdrValue = baselineLinear * 4.0
let hdr = CIImage(color: CIColor(red: hdrValue, green: hdrValue, blue: hdrValue,
                               colorSpace: linearSpace)!).cropped(to: bounds)
// A generic linear RGB output carries an explicit ICC matrix/TRC description.
// On macOS 15 Intel, named sRGB output omitted CP/TC from both colr and VUI,
// leaving use_base_colour_space without defined application primaries.
let producerSpace = CGColorSpace(name: CGColorSpace.genericRGBLinear)!
let output = directory.appendingPathComponent("apple-mono.heic")
require(!FileManager.default.fileExists(atPath: output.path), "Refusing to overwrite Apple producer fixture")
try context.writeHEIFRepresentation(of: baseline, to: output, format: .RGBA8,
                                   colorSpace: producerSpace, options: [.hdrImage: hdr])
guard let source = CGImageSourceCreateWithURL(output as CFURL, nil) else { fatalError("Missing Apple output") }
require(CGImageSourceCopyAuxiliaryDataInfoAtIndex(source, 0,
        kCGImageAuxiliaryDataTypeISOGainMap) != nil, "Apple producer omitted ISO gain map")
guard let hdrSource = CGImageSourceCreateWithURL(output as CFURL, hdrOptions),
      let produced = CGImageSourceCreateImageAtIndex(hdrSource, 0, hdrOptions) else {
    fatalError("Apple producer HDR decode failed")
}
var reference = [Float](repeating: 0, count: 64 * 64 * 4)
reference.withUnsafeMutableBytes { bytes in
    context.render(CIImage(cgImage: produced), toBitmap: bytes.baseAddress!, rowBytes: 64 * 16,
                   bounds: bounds, format: .RGBAf, colorSpace: linearSpace)
}
let producerError = reference.enumerated().filter { $0.offset % 4 != 3 }.map {
    abs(Double($0.element) - hdrValue)
}.max()!
if producerError >= 0.06 {
    failures.append("Apple lossy producer differs from HDR intention: \(producerError)")
}
try reference.withUnsafeBytes { bytes in
    try Data(bytes).write(to: URL(fileURLWithPath: output.path + ".rgba32f"), options: .withoutOverwriting)
}
if producerError < 0.06 {
    print("PASS Apple producer apple-mono.heic (canonical libheif decode checked separately)")
}
require(failures.isEmpty, failures.joined(separator: "\n"))
