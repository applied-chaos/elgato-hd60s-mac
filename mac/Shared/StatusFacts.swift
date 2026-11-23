import Foundation

func factsData(_ st: hd60s_status) -> Data {
    var st = st
    var buffer: UnsafeMutablePointer<CChar>?
    var size = 0
    guard let file = open_memstream(&buffer, &size) else { return Data() }
    hd60s_print_json(&st, file)
    fclose(file)
    defer { free(buffer) }
    return buffer.map { Data(bytes: $0, count: size) } ?? Data()
}
