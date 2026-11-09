// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <limits>
#include <memory>
#include <string>
#include <type_traits>

#include <Security/Security.h>
#include <httplib.h>

namespace Libraries::Http {

// cpp-httplib's Mbed TLS backend returns the entire presented chain. Mbed TLS
// owns these certificates for the duration of the verifier callback.
inline bool VerifyIosHttpsPeer(httplib::tls::session_t session, const std::string& host) {
    if (host.empty() || host.find('\0') != std::string::npos) {
        return false;
    }
    const std::unique_ptr<std::remove_pointer_t<CFStringRef>, decltype(&CFRelease)> hostname(
        CFStringCreateWithCString(kCFAllocatorDefault, host.c_str(), kCFStringEncodingUTF8),
        CFRelease);
    const std::unique_ptr<std::remove_pointer_t<CFMutableArrayRef>, decltype(&CFRelease)> chain(
        CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks), CFRelease);
    if (!hostname || !chain) {
        return false;
    }
    for (auto* cert = static_cast<const mbedtls_x509_crt*>(httplib::tls::get_peer_cert(session));
         cert; cert = cert->next) {
        if (!cert->raw.p || cert->raw.len == 0 ||
            cert->raw.len > static_cast<size_t>(std::numeric_limits<CFIndex>::max())) {
            return false;
        }
        const std::unique_ptr<std::remove_pointer_t<CFDataRef>, decltype(&CFRelease)> data(
            CFDataCreate(kCFAllocatorDefault, cert->raw.p, static_cast<CFIndex>(cert->raw.len)),
            CFRelease);
        if (!data) {
            return false;
        }
        const std::unique_ptr<std::remove_pointer_t<SecCertificateRef>, decltype(&CFRelease)> peer(
            SecCertificateCreateWithData(kCFAllocatorDefault, data.get()), CFRelease);
        if (!peer) {
            return false;
        }
        CFArrayAppendValue(chain.get(), peer.get());
    }
    if (CFArrayGetCount(chain.get()) == 0) {
        return false;
    }
    const std::unique_ptr<std::remove_pointer_t<SecPolicyRef>, decltype(&CFRelease)> policy(
        SecPolicyCreateSSL(true, hostname.get()), CFRelease);
    SecTrustRef trust_ref = nullptr;
    if (!policy || SecTrustCreateWithCertificates(chain.get(), policy.get(), &trust_ref) != errSecSuccess) {
        return false;
    }
    const std::unique_ptr<std::remove_pointer_t<SecTrustRef>, decltype(&CFRelease)> trust(
        trust_ref, CFRelease);
    return trust && SecTrustSetNetworkFetchAllowed(trust.get(), false) == errSecSuccess &&
           SecTrustEvaluateWithError(trust.get(), nullptr);
}

inline void ConfigureIosHttpsVerification(httplib::Client& client, const std::string& host,
                                          bool verify_server) {
    if (verify_server) {
        // Run Apple's trust and hostname checks after the handshake and before
        // cpp-httplib can send any HTTP headers or body.
        client.set_session_verifier([host](httplib::tls::session_t session) {
            return VerifyIosHttpsPeer(session, host)
                       ? httplib::SSLVerifierResponse::CertificateAccepted
                       : httplib::SSLVerifierResponse::CertificateRejected;
        });
    }
    // The backend has no iOS trust store of its own.
    client.enable_server_certificate_verification(false);
}

} // namespace Libraries::Http
