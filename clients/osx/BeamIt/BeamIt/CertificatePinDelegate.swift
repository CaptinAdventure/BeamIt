import CryptoKit
import Foundation
import Security

final class CertificatePinDelegate: NSObject, URLSessionDelegate {
    private let fingerprint: String?
    private let trustAnyCertificate: Bool

    init(fingerprint: String?, trustAnyCertificate: Bool) {
        self.fingerprint = fingerprint
        self.trustAnyCertificate = trustAnyCertificate
    }

    func urlSession(
        _ session: URLSession,
        didReceive challenge: URLAuthenticationChallenge,
        completionHandler: @escaping (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
    ) {
        guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
              let trust = challenge.protectionSpace.serverTrust,
              let certificate = (SecTrustCopyCertificateChain(trust) as? [SecCertificate])?.first
        else {
            completionHandler(.cancelAuthenticationChallenge, nil)
            return
        }

        if !trustAnyCertificate {
            let certificateData = SecCertificateCopyData(certificate) as Data
            let presentedFingerprint = SHA256.hash(data: certificateData)
                .map { String(format: "%02X", $0) }
                .joined()
            guard presentedFingerprint == fingerprint else {
                completionHandler(.cancelAuthenticationChallenge, nil)
                return
            }
        }
        completionHandler(.useCredential, URLCredential(trust: trust))
    }
}
