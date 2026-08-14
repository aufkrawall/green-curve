// SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
// SPDX-License-Identifier: MIT
//
// The updater's cryptographic gate: does this manifest carry a signature from a
// key we compiled in, and are these bytes on disk the ones it names?
//
// Everything here runs inside the LocalSystem service, and the thing on the far
// side of a successful verification is an installer that will be launched with
// SYSTEM rights.  That makes this the highest-consequence code in the
// application, so it is deliberately small and refuses by default: every
// function returns false unless a positive proof succeeded, and no failure path
// leaves a caller able to mistake "could not check" for "checked and fine".
//
// ## Why CNG and not a vendored crypto library
//
// bcrypt.dll is an OS service, the same argument that lets the installer use
// cabinet.dll for decompression: no third-party code ships to users and no
// hand-written primitive sits in the trust path.  Signatures are raw r||s,
// which is exactly the layout `BCryptVerifySignature` wants for ECDSA_P256, so
// there is no DER decoder here -- one less untrusted-input parser in the one
// place that could least afford a bug in one.
//
// ## What is verified, in order
//
//   1. The manifest's signature, over its exact bytes, against each compiled-in
//      public key.  Only if one verifies is the manifest parsed at all.
//   2. The staged file's byte length against the manifest.
//   3. The staged file's SHA-256 against the manifest.
//
// Length before digest is not redundant: it fails a truncated or padded
// download immediately and bounds how much is read before the expensive check.

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

// Strict decoder: canonical padding, no whitespace beyond a single trailing
// newline, no alternate alphabet.  A permissive decoder would accept several
// spellings of one signature, and "which bytes were actually signed" must have
// exactly one answer.
static int gc_update_base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool gc_update_base64_decode(const char* text, size_t textLen,
                                    unsigned char* out, size_t outSize,
                                    size_t* outLen) {
    if (!text || !out || !outLen) return false;
    *outLen = 0;

    // Tolerate exactly one trailing CR/LF pair, because the signature is a text
    // file and a text-mode round trip is the one benign mutation it can suffer.
    while (textLen > 0 && (text[textLen - 1] == '\n' || text[textLen - 1] == '\r')) {
        textLen--;
    }
    if (textLen == 0 || (textLen % 4) != 0) return false;

    size_t produced = 0;
    for (size_t i = 0; i < textLen; i += 4) {
        int quad[4];
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            char c = text[i + j];
            if (c == '=') {
                // Padding is only ever the last one or two characters.
                if (i + 4 != textLen || j < 2) return false;
                quad[j] = 0;
                padding++;
            } else {
                if (padding) return false;   // data after padding
                quad[j] = gc_update_base64_value(c);
                if (quad[j] < 0) return false;
            }
        }
        unsigned int triple = ((unsigned int)quad[0] << 18) |
                              ((unsigned int)quad[1] << 12) |
                              ((unsigned int)quad[2] << 6) |
                              (unsigned int)quad[3];
        int bytes = 3 - padding;
        for (int j = 0; j < bytes; ++j) {
            if (produced >= outSize) return false;
            out[produced++] = (unsigned char)((triple >> (16 - 8 * j)) & 0xFF);
        }
    }
    *outLen = produced;
    return produced > 0;
}

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

static void gc_update_hex_lower(const unsigned char* bytes, size_t count,
                                char* out, size_t outSize) {
    static const char kDigits[] = "0123456789abcdef";
    if (!out || outSize == 0) return;
    out[0] = 0;
    if (!bytes || outSize < count * 2 + 1) return;
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = kDigits[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
    }
    out[count * 2] = 0;
}

static bool gc_update_sha256_buffer(const void* data, size_t length,
                                    unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        return false;
    }
    bool ok = false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        if (BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data, (ULONG)length, 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, 32, 0))) {
            ok = true;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Hash a file THROUGH AN ALREADY-OPEN HANDLE rather than by path.
//
// This is the TOCTOU closure and it is the reason the signature is
// deliberately awkward.  Hashing by path and then launching by path leaves a
// window in which the bytes that were measured and the bytes that get executed
// are not required to be the same file.  The caller opens the staged installer
// once, with a share mode that denies writes and deletes, hashes through that
// handle, and holds it open across CreateProcess -- so there is no moment at
// which the verified file can be swapped for another.
static bool gc_update_sha256_handle(HANDLE file, char* hexOut, size_t hexOutSize,
                                    unsigned long long* sizeOut,
                                    char* err, size_t errSize) {
    if (hexOut && hexOutSize) hexOut[0] = 0;
    if (sizeOut) *sizeOut = 0;
    if (!file || file == INVALID_HANDLE_VALUE) {
        StringCchCopyA(err, errSize, "no file handle");
        return false;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0) {
        StringCchPrintfA(err, errSize, "cannot size the staged file (error %lu)",
                         GetLastError());
        return false;
    }
    if ((unsigned long long)fileSize.QuadPart > GC_UPDATE_ASSET_MAX_BYTES) {
        StringCchCopyA(err, errSize, "staged file exceeds the maximum asset size");
        return false;
    }
    if (sizeOut) *sizeOut = (unsigned long long)fileSize.QuadPart;

    LARGE_INTEGER origin = {};
    if (!SetFilePointerEx(file, origin, nullptr, FILE_BEGIN)) {
        StringCchPrintfA(err, errSize, "cannot rewind the staged file (error %lu)",
                         GetLastError());
        return false;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        StringCchCopyA(err, errSize, "SHA-256 provider unavailable");
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        StringCchCopyA(err, errSize, "cannot create a SHA-256 hash object");
        return false;
    }

    bool ok = true;
    unsigned long long remaining = (unsigned long long)fileSize.QuadPart;
    static const DWORD kChunk = 256 * 1024;
    unsigned char* buffer = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, kChunk);
    if (!buffer) {
        StringCchCopyA(err, errSize, "out of memory hashing the staged file");
        ok = false;
    }
    while (ok && remaining > 0) {
        DWORD want = remaining > kChunk ? kChunk : (DWORD)remaining;
        DWORD got = 0;
        if (!ReadFile(file, buffer, want, &got, nullptr) || got == 0) {
            StringCchPrintfA(err, errSize, "read failed while hashing (error %lu)",
                             GetLastError());
            ok = false;
            break;
        }
        if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer, got, 0))) {
            StringCchCopyA(err, errSize, "SHA-256 update failed");
            ok = false;
            break;
        }
        remaining -= got;
    }
    unsigned char digest[32] = {};
    if (ok && !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
        StringCchCopyA(err, errSize, "SHA-256 finalization failed");
        ok = false;
    }
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (ok) gc_update_hex_lower(digest, sizeof(digest), hexOut, hexOutSize);
    return ok;
}

// ---------------------------------------------------------------------------
// ECDSA P-256 signature verification
// ---------------------------------------------------------------------------

// CNG's public-key blob for P-256: a fixed header followed by X||Y.  The
// embedded keys are stored in exactly that tail layout (see
// update_verify_keys.h), so building the blob is a header plus a copy.
#ifndef BCRYPT_ECDSA_PUBLIC_P256_MAGIC
#define BCRYPT_ECDSA_PUBLIC_P256_MAGIC 0x31534345
#endif

// P-256 order / 2.  Signing normalizes s to the lower half; verification must
// enforce the same canonical spelling, otherwise both s and N-s verify.
static const unsigned char GC_UPDATE_P256_LOW_S_CEILING[32] = {
    0x7F, 0xFF, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00,
    0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xDE, 0x73, 0x7D, 0x56, 0xD3, 0x8B, 0xCF, 0x42,
    0x79, 0xDC, 0xE5, 0x61, 0x7E, 0x31, 0x92, 0xA8,
};

static bool gc_update_verify_with_key(const unsigned char* publicKey,
                                      const unsigned char* digest,
                                      const unsigned char* signature,
                                      size_t signatureLen) {
    if (!gc_update_key_is_populated(publicKey)) return false;
    if (signatureLen != GC_UPDATE_SIGNATURE_BYTES) return false;
    unsigned char highS = 0;
    for (size_t i = 32; i < signatureLen; ++i) {
        const unsigned char ceiling = GC_UPDATE_P256_LOW_S_CEILING[i - 32];
        if (signature[i] != ceiling) {
            highS = signature[i] > ceiling ? 1 : 0;
            break;
        }
    }
    if (highS) {
        debug_log("update verify: rejected a non-canonical high-S signature\n");
        return false;
    }

    unsigned char blob[sizeof(BCRYPT_ECCKEY_BLOB) + GC_UPDATE_PUBLIC_KEY_BYTES] = {};
    BCRYPT_ECCKEY_BLOB* header = (BCRYPT_ECCKEY_BLOB*)blob;
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = GC_UPDATE_PUBLIC_KEY_BYTES / 2;
    memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), publicKey, GC_UPDATE_PUBLIC_KEY_BYTES);

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM,
                                                    nullptr, 0))) {
        return false;
    }
    bool ok = false;
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS imported = BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                            &key, blob, (ULONG)sizeof(blob), 0);
    if (BCRYPT_SUCCESS(imported)) {
        NTSTATUS verified = BCryptVerifySignature(
            key, nullptr, (PUCHAR)digest, 32,
            (PUCHAR)signature, (ULONG)signatureLen, 0);
        ok = BCRYPT_SUCCESS(verified);
        BCryptDestroyKey(key);
    } else {
        debug_log("update verify: public key import failed status=0x%08lX\n",
                  (unsigned long)imported);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// Verify a manifest's detached signature over its exact bytes.
//
// A signature is accepted when ANY compiled-in key verifies it, which is what
// makes key rotation survivable: a build that knows both the retiring and the
// incoming key can read manifests signed with either, so retiring a key never
// strands clients on a version they can no longer update out of.
//
// Every key is tried even after one fails, and the loop does not short-circuit
// on the *reason* a key failed -- an import error and a bad signature are both
// simply "not this key".
static bool gc_update_verify_manifest_signature(const void* manifestBytes,
                                                size_t manifestLength,
                                                const char* signatureText,
                                                size_t signatureTextLength,
                                                char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!manifestBytes || manifestLength == 0) {
        StringCchCopyA(err, errSize, "no manifest bytes to verify");
        return false;
    }
    if (manifestLength > GC_UPDATE_MANIFEST_MAX_BYTES) {
        StringCchCopyA(err, errSize, "manifest is larger than the format allows");
        return false;
    }
    if (!signatureText || signatureTextLength == 0 ||
        signatureTextLength > GC_UPDATE_SIGNATURE_MAX_BYTES) {
        StringCchCopyA(err, errSize, "signature is missing or malformed");
        return false;
    }

    unsigned char signature[GC_UPDATE_SIGNATURE_BYTES] = {};
    size_t signatureLen = 0;
    if (!gc_update_base64_decode(signatureText, signatureTextLength,
                                 signature, sizeof(signature), &signatureLen) ||
        signatureLen != GC_UPDATE_SIGNATURE_BYTES) {
        StringCchCopyA(err, errSize,
                       "signature is not a 64-byte base64 ECDSA P-256 value");
        return false;
    }

    unsigned char digest[32] = {};
    if (!gc_update_sha256_buffer(manifestBytes, manifestLength, digest)) {
        StringCchCopyA(err, errSize, "could not hash the manifest");
        return false;
    }

    for (size_t i = 0; i < GC_UPDATE_PUBLIC_KEY_COUNT; ++i) {
        if (gc_update_verify_with_key(GC_UPDATE_PUBLIC_KEYS[i], digest,
                                      signature, signatureLen)) {
            debug_log("update verify: manifest signature accepted by key slot %u\n",
                      (unsigned)i);
            return true;
        }
    }
    // Deliberately does not say which key came closest or why each failed: the
    // useful fact for a user report is that no trusted key signed this, and the
    // rest is noise that invites treating a refusal as a configuration problem.
    StringCchCopyA(err, errSize,
                   "manifest is not signed by a trusted Green Curve release key");
    debug_log("update verify: manifest signature REJECTED by all %u key slot(s)\n",
              (unsigned)GC_UPDATE_PUBLIC_KEY_COUNT);
    return false;
}

// The full gate a staged installer must pass: exact byte length, then exact
// SHA-256, both against a manifest that has already had its signature checked.
//
// The digest comparison is a plain byte compare -- the digest is not a secret,
// so there is nothing for a timing side channel to learn -- but it compares the
// WHOLE string including the terminator, so a prefix match cannot pass.
static bool gc_update_staged_file_matches(HANDLE file, const GcUpdateAsset* asset,
                                          char* err, size_t errSize) {
    if (err && errSize) err[0] = 0;
    if (!asset || !asset->present) {
        StringCchCopyA(err, errSize, "no manifest entry for this architecture");
        return false;
    }

    char actualHex[GC_UPDATE_SHA256_HEX_CHARS + 1] = {};
    unsigned long long actualSize = 0;
    if (!gc_update_sha256_handle(file, actualHex, sizeof(actualHex), &actualSize,
                                 err, errSize)) {
        return false;
    }
    if (actualSize != asset->size) {
        StringCchPrintfA(err, errSize,
                         "staged file is %llu bytes; the signed manifest says %llu",
                         actualSize, asset->size);
        debug_log("update verify: size mismatch actual=%llu expected=%llu\n",
                  actualSize, asset->size);
        return false;
    }
    if (strcmp(actualHex, asset->sha256) != 0) {
        StringCchCopyA(err, errSize,
                       "staged file does not match the digest in the signed manifest");
        debug_log("update verify: digest mismatch actual=%s expected=%s\n",
                  actualHex, asset->sha256);
        return false;
    }
    debug_log("update verify: staged file matches the signed manifest "
              "(%llu bytes, sha256=%s)\n", actualSize, actualHex);
    return true;
}
