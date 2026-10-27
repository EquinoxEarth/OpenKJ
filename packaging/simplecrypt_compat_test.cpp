#include "simplecrypt.h"

#include <QByteArray>
#include <cstdio>
#include <stdexcept>

namespace {
constexpr quint64 fixtureKey = 0x0123456789abcdefULL;

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Inspect the serialized payload independently of SimpleCrypt's reader.
// The first decoded byte is random; the checksum and content must stay identical.
QByteArray payload(const QByteArray &cipher)
{
    require(cipher.size() >= 5 && cipher[0] == char(3), "Unexpected saved-data format");
    auto body = cipher.mid(2);
    unsigned char previous = 0;
    for (qsizetype i = 0; i < body.size(); ++i) {
        const auto encrypted = static_cast<unsigned char>(body[i]);
        const auto keyByte = static_cast<unsigned char>(fixtureKey >> ((i % 8) * 8));
        body[i] = char(encrypted ^ keyByte ^ previous);
        previous = encrypted;
    }
    return body.mid(1);
}
}

int main()
{
    try {
        struct Fixture {
            QByteArray plaintext;
            const char *cipher;
            SimpleCrypt::CompressionMode compression;
        };
        // Generated with the pre-migration implementation and a synthetic key.
        const Fixture fixtures[] = {
            {QByteArray(), "AwLWG7A=", SimpleCrypt::CompressionNever},
            {QByteArray("OpenKJ saved setting"), "AwK7f1eRhqbroQTpMdnI6K+OErplmJa9+Q==", SimpleCrypt::CompressionNever},
            {QByteArray::fromHex("00010200ff804f70656e4b4a"), "AwJMH1/WsPfUKkXHHPD5954=", SimpleCrypt::CompressionNever},
            {QByteArray(512, 'K'), "AwMj2C6nwIek3ejWixx+GCIjzFQ1Kkw=", SimpleCrypt::CompressionAlways},
        };
        SimpleCrypt crypt(fixtureKey);
        for (const auto &fixture : fixtures) {
            const auto legacy = QByteArray::fromBase64(fixture.cipher);
            require(crypt.decryptToByteArray(legacy) == fixture.plaintext &&
                    crypt.lastError() == SimpleCrypt::ErrorNoError,
                    "Previously saved data must still decrypt");
            crypt.setCompressionMode(fixture.compression);
            const auto current = crypt.encryptToByteArray(fixture.plaintext);
            require(current.left(2) == legacy.left(2) && payload(current) == payload(legacy),
                    "New saved data must preserve the legacy checksum and payload format");
            require(crypt.decryptToByteArray(current) == fixture.plaintext &&
                    crypt.lastError() == SimpleCrypt::ErrorNoError, "New saved data must round-trip");
            auto damaged = legacy;
            damaged[damaged.size() - 1] ^= 1;
            require(crypt.decryptToByteArray(damaged).isEmpty() &&
                    crypt.lastError() == SimpleCrypt::ErrorIntegrityFailed,
                    "Corrupted saved data must still fail integrity checking");
        }
        std::puts("Legacy SimpleCrypt compatibility passed");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
