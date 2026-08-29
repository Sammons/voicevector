import Foundation

enum HTTPError: LocalizedError {
    case badURL(String)
    case status(Int, String)
    case badResponse(String)

    var errorDescription: String? {
        switch self {
        case .badURL(let url): return "Invalid URL: \(url)"
        case .status(let code, let body):
            let snippet = body.prefix(300)
            return "HTTP \(code)\(snippet.isEmpty ? "" : ": \(snippet)")"
        case .badResponse(let why): return "Unexpected response: \(why)"
        }
    }
}

/// Thin URLSession wrapper: JSON and multipart POST/GET with sane timeouts.
enum HTTP {
    static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 60
        config.timeoutIntervalForResource = 300
        return URLSession(configuration: config)
    }()

    static func url(base: String, path: String) throws -> URL {
        let trimmedBase = base.hasSuffix("/") ? String(base.dropLast()) : base
        guard let url = URL(string: trimmedBase + path) else { throw HTTPError.badURL(base + path) }
        return url
    }

    static func request(_ url: URL, method: String, headers: [String: String],
                        body: Data? = nil, contentType: String? = nil) -> URLRequest {
        var request = URLRequest(url: url)
        request.httpMethod = method
        for (key, value) in headers { request.setValue(value, forHTTPHeaderField: key) }
        if let contentType { request.setValue(contentType, forHTTPHeaderField: "Content-Type") }
        request.httpBody = body
        return request
    }

    /// Sends the request, throws on non-2xx (with body snippet for diagnostics).
    static func send(_ request: URLRequest) async throws -> Data {
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse else {
            throw HTTPError.badResponse("not an HTTP response")
        }
        guard (200..<300).contains(http.statusCode) else {
            throw HTTPError.status(http.statusCode, String(data: data, encoding: .utf8) ?? "")
        }
        return data
    }

    static func json(_ data: Data) throws -> [String: Any] {
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw HTTPError.badResponse("expected a JSON object")
        }
        return object
    }
}
