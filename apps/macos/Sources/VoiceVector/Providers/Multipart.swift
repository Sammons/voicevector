import Foundation

/// Hand-rolled multipart/form-data encoder (RFC 2388) — enough for STT uploads.
struct Multipart {
    let boundary: String
    private var body = Data()

    init(boundary: String = "voicevector-\(UUID().uuidString)") {
        self.boundary = boundary
    }

    var contentType: String { "multipart/form-data; boundary=\(boundary)" }

    mutating func addField(name: String, value: String) {
        body.append(contentsOf: "--\(boundary)\r\n".utf8)
        body.append(contentsOf: "Content-Disposition: form-data; name=\"\(name)\"\r\n\r\n".utf8)
        body.append(contentsOf: value.utf8)
        body.append(contentsOf: "\r\n".utf8)
    }

    mutating func addFile(name: String, filename: String, contentType: String, data: Data) {
        body.append(contentsOf: "--\(boundary)\r\n".utf8)
        body.append(contentsOf:
            "Content-Disposition: form-data; name=\"\(name)\"; filename=\"\(filename)\"\r\n".utf8)
        body.append(contentsOf: "Content-Type: \(contentType)\r\n\r\n".utf8)
        body.append(data)
        body.append(contentsOf: "\r\n".utf8)
    }

    func encoded() -> Data {
        var data = body
        data.append(contentsOf: "--\(boundary)--\r\n".utf8)
        return data
    }
}
