#pragma once

#include "Serializer.h"

struct ClientHelloMessage {
    std::string random;
    std::string kexKeyShare;
    std::string nonce;

    template <class X>
    void visit(X& x) {
        x(1, random);
        x(2, kexKeyShare);
        x(3, nonce);
    }
};

struct SignatureMessage {
    std::string pubkey;
    std::string signature;

    template <class X>
    void visit(X& x) {
        x(1, pubkey);
        x(2, signature);
    }
};

struct ServerHelloFinishedMessage {
    std::string random;
    std::string kexKeyShare;
    std::string nonce;
    std::string encryptedSignatureMessage;

    template <class X>
    void visit(X& x) {
        x(1, random);
        x(2, kexKeyShare);
        x(3, nonce);
        x(4, encryptedSignatureMessage);
    }
};

struct ClientFinishedMessage {
    std::string encryptedSignatureMessage;

    template <class X>
    void visit(X& x) {
        x(1, encryptedSignatureMessage);
    }

};
