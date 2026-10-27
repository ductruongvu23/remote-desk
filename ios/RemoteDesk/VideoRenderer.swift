import AVFoundation
import UIKit

/// Giải mã HEVC/H.264 bằng phần cứng như Moonlight: AVSampleBufferDisplayLayer, hiện ngay từng khung, không đệm.
final class VideoRenderer {
    let layer = AVSampleBufferDisplayLayer()
    var onNeedKey: (() -> Void)?
    private var format: CMVideoFormatDescription?
    private var params: [Data] = [], hevc = false
    private var waitingKey = true, askedKey = false
    private let lock = NSLock()
    private var frames = 0, bytes = 0
    private var size = CGSize(width: 16, height: 9)

    init() {
        layer.videoGravity = .resizeAspect
        layer.backgroundColor = UIColor.black.cgColor
    }

    var videoSize: CGSize { lock.withLock { size } }
    /// Số khung và số byte nhận được từ lần gọi trước.
    func takeCounters() -> (frames: Int, bytes: Int) { lock.withLock { defer { frames = 0; bytes = 0 }; return (frames, bytes) } }

    /// Gói từ PC: 1 byte cờ (bit 0 = khung khoá, bit 1 = HEVC) + Annex B. Gọi trên một hàng đợi nối tiếp.
    func enqueue(_ packet: Data) {
        let bytes = [UInt8](packet)
        guard bytes.count > 1 else { return }
        lock.withLock { self.bytes += bytes.count }
        let isKey = bytes[0] & 1 != 0, isHevc = bytes[0] & 2 != 0
        if isHevc != hevc { hevc = isHevc; params = []; format = nil }  // PC đổi codec: dựng lại bộ giải mã
        let (ps, avcc) = Self.split(bytes, from: 1, hevc: hevc)
        if let ps, ps != params { params = ps; format = nil }
        if format == nil, !params.isEmpty, let f = Self.makeFormat(params, hevc: hevc) {
            format = f
            let d = CMVideoFormatDescriptionGetDimensions(f)
            lock.withLock { size = CGSize(width: Int(d.width), height: Int(d.height)) }
        }
        let renderer = layer.sampleBufferRenderer
        if renderer.status == .failed || renderer.requiresFlushToResumeDecoding {
            renderer.flush()
            waitingKey = true
        }
        if waitingKey && !isKey {  // khung P thiếu khung gốc sẽ ra hình rác: bỏ, xin PC một khung khoá
            if !askedKey { askedKey = true; onNeedKey?() }
            return
        }
        guard let format, !avcc.isEmpty, let sample = Self.sample(avcc, format) else { return }
        waitingKey = false
        askedKey = false
        renderer.enqueue(sample)
        lock.withLock { frames += 1 }
    }

    /// Tách Annex B: lấy bộ tham số (H.264: SPS, PPS; HEVC: VPS, SPS, PPS) và gói các NAL còn lại sang dạng AVCC
    /// (độ dài 4 byte) cho VideoToolbox. Bộ tham số chỉ trả về khi có đủ.
    static func split(_ b: [UInt8], from start: Int, hevc: Bool = false) -> (params: [Data]?, avcc: Data) {
        var starts: [Int] = []
        var i = start
        while i + 2 < b.count {
            if b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 { starts.append(i + 3); i += 3 } else { i += 1 }
        }
        let want = hevc ? [32, 33, 34] : [7, 8], aud = hevc ? 35 : 9
        var ps: [Int: Data] = [:], avcc = Data()
        for (k, s) in starts.enumerated() {
            var e = k + 1 < starts.count ? starts[k + 1] - 3 : b.count
            while e > s && b[e - 1] == 0 { e -= 1 }
            guard e > s else { continue }
            let nal = b[s..<e]
            let t = hevc ? Int(nal.first! >> 1) & 0x3F : Int(nal.first! & 0x1F)
            if want.contains(t) {
                ps[t] = Data(nal)
            } else if t != aud {
                var len = UInt32(nal.count).bigEndian
                withUnsafeBytes(of: &len) { avcc.append(contentsOf: $0) }
                avcc.append(contentsOf: nal)
            }
        }
        let full = want.allSatisfy { ps[$0] != nil }
        return (full ? want.map { ps[$0]! } : nil, avcc)
    }

    static func makeFormat(_ ps: [Data], hevc: Bool) -> CMVideoFormatDescription? {
        var f: CMFormatDescription?
        let all = ps.reduce(Data(), +)  // một khối liền để các con trỏ còn sống suốt lúc gọi
        let status = all.withUnsafeBytes { raw -> OSStatus in
            let base = raw.bindMemory(to: UInt8.self).baseAddress!
            var off = 0
            let ptrs: [UnsafePointer<UInt8>] = ps.map { d in
                defer { off += d.count }
                return base + off
            }
            let sizes = ps.map(\.count)
            return hevc
                ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: ps.count, parameterSetPointers: ptrs,
                    parameterSetSizes: sizes, nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &f)
                : CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: ps.count, parameterSetPointers: ptrs,
                    parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &f)
        }
        return status == noErr ? f : nil
    }

    static func sample(_ data: Data, _ format: CMFormatDescription) -> CMSampleBuffer? {
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
                allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: data.count, blockAllocator: kCFAllocatorDefault,
                customBlockSource: nil, offsetToData: 0, dataLength: data.count, flags: kCMBlockBufferAssureMemoryNowFlag,
                blockBufferOut: &block) == noErr, let block,
              data.withUnsafeBytes({ CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block,
                                                                  offsetIntoDestination: 0, dataLength: data.count) }) == noErr
        else { return nil }
        var sample: CMSampleBuffer?
        var size = data.count
        guard CMSampleBufferCreateReady(
                allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format, sampleCount: 1,
                sampleTimingEntryCount: 0, sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size,
                sampleBufferOut: &sample) == noErr, let sample
        else { return nil }
        if let att = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true), CFArrayGetCount(att) > 0 {
            let dict = unsafeBitCast(CFArrayGetValueAtIndex(att, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dict, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                                 Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        return sample
    }
}
