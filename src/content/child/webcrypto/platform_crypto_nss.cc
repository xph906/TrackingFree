// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/child/webcrypto/platform_crypto.h"

#include <cryptohi.h>
#include <pk11pub.h>
#include <secerr.h>
#include <sechash.h>

#include <vector>

#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "content/child/webcrypto/crypto_data.h"
#include "content/child/webcrypto/status.h"
#include "content/child/webcrypto/webcrypto_util.h"
#include "crypto/nss_util.h"
#include "crypto/scoped_nss_types.h"
#include "third_party/WebKit/public/platform/WebCryptoAlgorithm.h"
#include "third_party/WebKit/public/platform/WebCryptoAlgorithmParams.h"
#include "third_party/WebKit/public/platform/WebCryptoKeyAlgorithm.h"

#if defined(USE_NSS)
#include <dlfcn.h>
#include <secoid.h>
#endif

// At the time of this writing:
//   * Windows and Mac builds ship with their own copy of NSS (3.15+)
//   * Linux builds use the system's libnss, which is 3.14 on Debian (but 3.15+
//     on other distros).
//
// Since NSS provides AES-GCM support starting in version 3.15, it may be
// unavailable for Linux Chrome users.
//
//  * !defined(CKM_AES_GCM)
//
//      This means that at build time, the NSS header pkcs11t.h is older than
//      3.15. However at runtime support may be present.
//
//  * !defined(USE_NSS)
//
//      This means that Chrome is being built with an embedded copy of NSS,
//      which can be assumed to be >= 3.15. On the other hand if USE_NSS is
//      defined, it also implies running on Linux.
//
// TODO(eroman): Simplify this once 3.15+ is required by Linux builds.
#if !defined(CKM_AES_GCM)
#define CKM_AES_GCM 0x00001087

struct CK_GCM_PARAMS {
  CK_BYTE_PTR pIv;
  CK_ULONG ulIvLen;
  CK_BYTE_PTR pAAD;
  CK_ULONG ulAADLen;
  CK_ULONG ulTagBits;
};
#endif  // !defined(CKM_AES_GCM)

// Signature for PK11_Encrypt and PK11_Decrypt.
typedef SECStatus (*PK11_EncryptDecryptFunction)(PK11SymKey*,
                                                 CK_MECHANISM_TYPE,
                                                 SECItem*,
                                                 unsigned char*,
                                                 unsigned int*,
                                                 unsigned int,
                                                 const unsigned char*,
                                                 unsigned int);

// Singleton to abstract away dynamically loading libnss3.so
class AesGcmSupport {
 public:
  bool IsSupported() const { return pk11_encrypt_func_ && pk11_decrypt_func_; }

  // Returns NULL if unsupported.
  PK11_EncryptDecryptFunction pk11_encrypt_func() const {
    return pk11_encrypt_func_;
  }

  // Returns NULL if unsupported.
  PK11_EncryptDecryptFunction pk11_decrypt_func() const {
    return pk11_decrypt_func_;
  }

 private:
  friend struct base::DefaultLazyInstanceTraits<AesGcmSupport>;

  AesGcmSupport() {
#if !defined(USE_NSS)
    // Using a bundled version of NSS that is guaranteed to have this symbol.
    pk11_encrypt_func_ = PK11_Encrypt;
    pk11_decrypt_func_ = PK11_Decrypt;
#else
    // Using system NSS libraries and PCKS #11 modules, which may not have the
    // necessary function (PK11_Encrypt) or mechanism support (CKM_AES_GCM).

    // If PK11_Encrypt() was successfully resolved, then NSS will support
    // AES-GCM directly. This was introduced in NSS 3.15.
    pk11_encrypt_func_ = reinterpret_cast<PK11_EncryptDecryptFunction>(
        dlsym(RTLD_DEFAULT, "PK11_Encrypt"));
    pk11_decrypt_func_ = reinterpret_cast<PK11_EncryptDecryptFunction>(
        dlsym(RTLD_DEFAULT, "PK11_Decrypt"));
#endif
  }

  PK11_EncryptDecryptFunction pk11_encrypt_func_;
  PK11_EncryptDecryptFunction pk11_decrypt_func_;
};

base::LazyInstance<AesGcmSupport>::Leaky g_aes_gcm_support =
    LAZY_INSTANCE_INITIALIZER;

namespace content {

namespace webcrypto {

namespace platform {

// Each key maintains a copy of its serialized form
// in either 'raw', 'pkcs8', or 'spki' format. This is to allow
// structured cloning of keys synchronously from the target Blink
// thread without having to lock access to the key.
//
// TODO(eroman): Take advantage of this for implementing exportKey(): no need
//               to call into NSS if the serialized form already exists.
//               http://crubg.com/366836
class SymKey : public Key {
 public:
  static Status Create(crypto::ScopedPK11SymKey key, scoped_ptr<SymKey>* out) {
    out->reset(new SymKey(key.Pass()));
    return ExportKeyRaw(out->get(), &(*out)->serialized_key_);
  }

  PK11SymKey* key() { return key_.get(); }

  virtual SymKey* AsSymKey() OVERRIDE { return this; }
  virtual PublicKey* AsPublicKey() OVERRIDE { return NULL; }
  virtual PrivateKey* AsPrivateKey() OVERRIDE { return NULL; }

  virtual bool ThreadSafeSerializeForClone(
      blink::WebVector<uint8>* key_data) OVERRIDE {
    key_data->assign(Uint8VectorStart(serialized_key_), serialized_key_.size());
    return true;
  }

 private:
  explicit SymKey(crypto::ScopedPK11SymKey key) : key_(key.Pass()) {}

  crypto::ScopedPK11SymKey key_;
  std::vector<uint8> serialized_key_;

  DISALLOW_COPY_AND_ASSIGN(SymKey);
};

class PublicKey : public Key {
 public:
  static Status Create(crypto::ScopedSECKEYPublicKey key,
                       scoped_ptr<PublicKey>* out) {
    out->reset(new PublicKey(key.Pass()));
    return ExportKeySpki(out->get(), &(*out)->serialized_key_);
  }

  SECKEYPublicKey* key() { return key_.get(); }

  virtual SymKey* AsSymKey() OVERRIDE { return NULL; }
  virtual PublicKey* AsPublicKey() OVERRIDE { return this; }
  virtual PrivateKey* AsPrivateKey() OVERRIDE { return NULL; }

  virtual bool ThreadSafeSerializeForClone(
      blink::WebVector<uint8>* key_data) OVERRIDE {
    key_data->assign(Uint8VectorStart(serialized_key_), serialized_key_.size());
    return true;
  }

 private:
  explicit PublicKey(crypto::ScopedSECKEYPublicKey key) : key_(key.Pass()) {}

  crypto::ScopedSECKEYPublicKey key_;
  std::vector<uint8> serialized_key_;

  DISALLOW_COPY_AND_ASSIGN(PublicKey);
};

class PrivateKey : public Key {
 public:
  static Status Create(crypto::ScopedSECKEYPrivateKey key,
                       const blink::WebCryptoKeyAlgorithm& algorithm,
                       scoped_ptr<PrivateKey>* out) {
    out->reset(new PrivateKey(key.Pass()));
    return ExportKeyPkcs8(out->get(), algorithm, &(*out)->serialized_key_);
  }

  SECKEYPrivateKey* key() { return key_.get(); }

  virtual SymKey* AsSymKey() OVERRIDE { return NULL; }
  virtual PublicKey* AsPublicKey() OVERRIDE { return NULL; }
  virtual PrivateKey* AsPrivateKey() OVERRIDE { return this; }

  virtual bool ThreadSafeSerializeForClone(
      blink::WebVector<uint8>* key_data) OVERRIDE {
    key_data->assign(Uint8VectorStart(serialized_key_), serialized_key_.size());
    return true;
  }

 private:
  explicit PrivateKey(crypto::ScopedSECKEYPrivateKey key) : key_(key.Pass()) {}

  crypto::ScopedSECKEYPrivateKey key_;
  std::vector<uint8> serialized_key_;

  DISALLOW_COPY_AND_ASSIGN(PrivateKey);
};

namespace {

// Creates a SECItem for the data in |buffer|. This does NOT make a copy, so
// |buffer| should outlive the SECItem.
SECItem MakeSECItemForBuffer(const CryptoData& buffer) {
  SECItem item = {
      siBuffer,
      // NSS requires non-const data even though it is just for input.
      const_cast<unsigned char*>(buffer.bytes()), buffer.byte_length()};
  return item;
}

HASH_HashType WebCryptoAlgorithmToNSSHashType(
    blink::WebCryptoAlgorithmId algorithm) {
  switch (algorithm) {
    case blink::WebCryptoAlgorithmIdSha1:
      return HASH_AlgSHA1;
    case blink::WebCryptoAlgorithmIdSha256:
      return HASH_AlgSHA256;
    case blink::WebCryptoAlgorithmIdSha384:
      return HASH_AlgSHA384;
    case blink::WebCryptoAlgorithmIdSha512:
      return HASH_AlgSHA512;
    default:
      // Not a digest algorithm.
      return HASH_AlgNULL;
  }
}

CK_MECHANISM_TYPE WebCryptoHashToHMACMechanism(
    const blink::WebCryptoAlgorithm& algorithm) {
  switch (algorithm.id()) {
    case blink::WebCryptoAlgorithmIdSha1:
      return CKM_SHA_1_HMAC;
    case blink::WebCryptoAlgorithmIdSha256:
      return CKM_SHA256_HMAC;
    case blink::WebCryptoAlgorithmIdSha384:
      return CKM_SHA384_HMAC;
    case blink::WebCryptoAlgorithmIdSha512:
      return CKM_SHA512_HMAC;
    default:
      // Not a supported algorithm.
      return CKM_INVALID_MECHANISM;
  }
}

Status AesCbcEncryptDecrypt(EncryptOrDecrypt mode,
                            SymKey* key,
                            const CryptoData& iv,
                            const CryptoData& data,
                            std::vector<uint8>* buffer) {
  CK_ATTRIBUTE_TYPE operation = (mode == ENCRYPT) ? CKA_ENCRYPT : CKA_DECRYPT;

  SECItem iv_item = MakeSECItemForBuffer(iv);

  crypto::ScopedSECItem param(PK11_ParamFromIV(CKM_AES_CBC_PAD, &iv_item));
  if (!param)
    return Status::OperationError();

  crypto::ScopedPK11Context context(PK11_CreateContextBySymKey(
      CKM_AES_CBC_PAD, operation, key->key(), param.get()));

  if (!context.get())
    return Status::OperationError();

  // Oddly PK11_CipherOp takes input and output lengths as "int" rather than
  // "unsigned int". Do some checks now to avoid integer overflowing.
  if (data.byte_length() >= INT_MAX - AES_BLOCK_SIZE) {
    // TODO(eroman): Handle this by chunking the input fed into NSS. Right now
    // it doesn't make much difference since the one-shot API would end up
    // blowing out the memory and crashing anyway.
    return Status::ErrorDataTooLarge();
  }

  // PK11_CipherOp does an invalid memory access when given empty decryption
  // input, or input which is not a multiple of the block size. See also
  // https://bugzilla.mozilla.com/show_bug.cgi?id=921687.
  if (operation == CKA_DECRYPT &&
      (data.byte_length() == 0 || (data.byte_length() % AES_BLOCK_SIZE != 0))) {
    return Status::OperationError();
  }

  // TODO(eroman): Refine the output buffer size. It can be computed exactly for
  //               encryption, and can be smaller for decryption.
  unsigned int output_max_len = data.byte_length() + AES_BLOCK_SIZE;
  CHECK_GT(output_max_len, data.byte_length());

  buffer->resize(output_max_len);

  unsigned char* buffer_data = Uint8VectorStart(buffer);

  int output_len;
  if (SECSuccess != PK11_CipherOp(context.get(),
                                  buffer_data,
                                  &output_len,
                                  buffer->size(),
                                  data.bytes(),
                                  data.byte_length())) {
    return Status::OperationError();
  }

  unsigned int final_output_chunk_len;
  if (SECSuccess != PK11_DigestFinal(context.get(),
                                     buffer_data + output_len,
                                     &final_output_chunk_len,
                                     output_max_len - output_len)) {
    return Status::OperationError();
  }

  buffer->resize(final_output_chunk_len + output_len);
  return Status::Success();
}

// Helper to either encrypt or decrypt for AES-GCM. The result of encryption is
// the concatenation of the ciphertext and the authentication tag. Similarly,
// this is the expectation for the input to decryption.
Status AesGcmEncryptDecrypt(EncryptOrDecrypt mode,
                            SymKey* key,
                            const CryptoData& data,
                            const CryptoData& iv,
                            const CryptoData& additional_data,
                            unsigned int tag_length_bits,
                            std::vector<uint8>* buffer) {
  if (!g_aes_gcm_support.Get().IsSupported())
    return Status::ErrorUnsupported();

  unsigned int tag_length_bytes = tag_length_bits / 8;

  CK_GCM_PARAMS gcm_params = {0};
  gcm_params.pIv = const_cast<unsigned char*>(iv.bytes());
  gcm_params.ulIvLen = iv.byte_length();

  gcm_params.pAAD = const_cast<unsigned char*>(additional_data.bytes());
  gcm_params.ulAADLen = additional_data.byte_length();

  gcm_params.ulTagBits = tag_length_bits;

  SECItem param;
  param.type = siBuffer;
  param.data = reinterpret_cast<unsigned char*>(&gcm_params);
  param.len = sizeof(gcm_params);

  unsigned int buffer_size = 0;

  // Calculate the output buffer size.
  if (mode == ENCRYPT) {
    // TODO(eroman): This is ugly, abstract away the safe integer arithmetic.
    if (data.byte_length() > (UINT_MAX - tag_length_bytes))
      return Status::ErrorDataTooLarge();
    buffer_size = data.byte_length() + tag_length_bytes;
  } else {
    // TODO(eroman): In theory the buffer allocated for the plain text should be
    // sized as |data.byte_length() - tag_length_bytes|.
    //
    // However NSS has a bug whereby it will fail if the output buffer size is
    // not at least as large as the ciphertext:
    //
    // https://bugzilla.mozilla.org/show_bug.cgi?id=%20853674
    //
    // From the analysis of that bug it looks like it might be safe to pass a
    // correctly sized buffer but lie about its size. Since resizing the
    // WebCryptoArrayBuffer is expensive that hack may be worth looking into.
    buffer_size = data.byte_length();
  }

  buffer->resize(buffer_size);
  unsigned char* buffer_data = Uint8VectorStart(buffer);

  PK11_EncryptDecryptFunction func =
      (mode == ENCRYPT) ? g_aes_gcm_support.Get().pk11_encrypt_func()
                        : g_aes_gcm_support.Get().pk11_decrypt_func();

  unsigned int output_len = 0;
  SECStatus result = func(key->key(),
                          CKM_AES_GCM,
                          &param,
                          buffer_data,
                          &output_len,
                          buffer->size(),
                          data.bytes(),
                          data.byte_length());

  if (result != SECSuccess)
    return Status::OperationError();

  // Unfortunately the buffer needs to be shrunk for decryption (see the NSS bug
  // above).
  buffer->resize(output_len);

  return Status::Success();
}

CK_MECHANISM_TYPE WebCryptoAlgorithmToGenMechanism(
    const blink::WebCryptoAlgorithm& algorithm) {
  switch (algorithm.id()) {
    case blink::WebCryptoAlgorithmIdAesCbc:
    case blink::WebCryptoAlgorithmIdAesGcm:
    case blink::WebCryptoAlgorithmIdAesKw:
      return CKM_AES_KEY_GEN;
    case blink::WebCryptoAlgorithmIdHmac:
      return WebCryptoHashToHMACMechanism(algorithm.hmacKeyGenParams()->hash());
    default:
      return CKM_INVALID_MECHANISM;
  }
}

// Converts a (big-endian) WebCrypto BigInteger, with or without leading zeros,
// to unsigned long.
bool BigIntegerToLong(const uint8* data,
                      unsigned int data_size,
                      unsigned long* result) {
  // TODO(padolph): Is it correct to say that empty data is an error, or does it
  // mean value 0? See https://www.w3.org/Bugs/Public/show_bug.cgi?id=23655
  if (data_size == 0)
    return false;

  *result = 0;
  for (size_t i = 0; i < data_size; ++i) {
    size_t reverse_i = data_size - i - 1;

    if (reverse_i >= sizeof(unsigned long) && data[i])
      return false;  // Too large for a long.

    *result |= data[i] << 8 * reverse_i;
  }
  return true;
}

bool IsAlgorithmRsa(const blink::WebCryptoAlgorithm& algorithm) {
  return algorithm.id() == blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5 ||
         algorithm.id() == blink::WebCryptoAlgorithmIdRsaOaep ||
         algorithm.id() == blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5;
}

bool CreatePublicKeyAlgorithm(const blink::WebCryptoAlgorithm& algorithm,
                              SECKEYPublicKey* key,
                              blink::WebCryptoKeyAlgorithm* key_algorithm) {
  // TODO(eroman): What about other key types rsaPss, rsaOaep.
  if (!key || key->keyType != rsaKey)
    return false;

  unsigned int modulus_length_bits = SECKEY_PublicKeyStrength(key) * 8;
  CryptoData public_exponent(key->u.rsa.publicExponent.data,
                             key->u.rsa.publicExponent.len);

  switch (algorithm.paramsType()) {
    case blink::WebCryptoAlgorithmParamsTypeRsaHashedImportParams:
    case blink::WebCryptoAlgorithmParamsTypeRsaHashedKeyGenParams:
      *key_algorithm = blink::WebCryptoKeyAlgorithm::createRsaHashed(
          algorithm.id(),
          modulus_length_bits,
          public_exponent.bytes(),
          public_exponent.byte_length(),
          GetInnerHashAlgorithm(algorithm).id());
      return true;
    case blink::WebCryptoAlgorithmParamsTypeRsaKeyGenParams:
    case blink::WebCryptoAlgorithmParamsTypeNone:
      *key_algorithm = blink::WebCryptoKeyAlgorithm::createRsa(
          algorithm.id(),
          modulus_length_bits,
          public_exponent.bytes(),
          public_exponent.byte_length());
      return true;
    default:
      return false;
  }
}

bool CreatePrivateKeyAlgorithm(const blink::WebCryptoAlgorithm& algorithm,
                               SECKEYPrivateKey* key,
                               blink::WebCryptoKeyAlgorithm* key_algorithm) {
  crypto::ScopedSECKEYPublicKey public_key(SECKEY_ConvertToPublicKey(key));
  return CreatePublicKeyAlgorithm(algorithm, public_key.get(), key_algorithm);
}

// The Default IV for AES-KW. See http://www.ietf.org/rfc/rfc3394.txt
// Section 2.2.3.1.
// TODO(padolph): Move to common place to be shared with OpenSSL implementation.
const unsigned char kAesIv[] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};

// Sets NSS CK_MECHANISM_TYPE and CK_FLAGS corresponding to the input Web Crypto
// algorithm ID.
Status WebCryptoAlgorithmToNssMechFlags(
    const blink::WebCryptoAlgorithm& algorithm,
    CK_MECHANISM_TYPE* mechanism,
    CK_FLAGS* flags) {
  // Flags are verified at the Blink layer; here the flags are set to all
  // possible operations of a key for the input algorithm type.
  switch (algorithm.id()) {
    case blink::WebCryptoAlgorithmIdHmac: {
      const blink::WebCryptoAlgorithm hash = GetInnerHashAlgorithm(algorithm);
      *mechanism = WebCryptoHashToHMACMechanism(hash);
      if (*mechanism == CKM_INVALID_MECHANISM)
        return Status::ErrorUnsupported();
      *flags = CKF_SIGN | CKF_VERIFY;
      break;
    }
    case blink::WebCryptoAlgorithmIdAesCbc: {
      *mechanism = CKM_AES_CBC;
      *flags = CKF_ENCRYPT | CKF_DECRYPT;
      break;
    }
    case blink::WebCryptoAlgorithmIdAesKw: {
      *mechanism = CKM_NSS_AES_KEY_WRAP;
      *flags = CKF_WRAP | CKF_WRAP;
      break;
    }
    case blink::WebCryptoAlgorithmIdAesGcm: {
      if (!g_aes_gcm_support.Get().IsSupported())
        return Status::ErrorUnsupported();
      *mechanism = CKM_AES_GCM;
      *flags = CKF_ENCRYPT | CKF_DECRYPT;
      break;
    }
    default:
      return Status::ErrorUnsupported();
  }
  return Status::Success();
}

Status DoUnwrapSymKeyAesKw(const CryptoData& wrapped_key_data,
                           SymKey* wrapping_key,
                           CK_MECHANISM_TYPE mechanism,
                           CK_FLAGS flags,
                           crypto::ScopedPK11SymKey* unwrapped_key) {
  DCHECK_GE(wrapped_key_data.byte_length(), 24u);
  DCHECK_EQ(wrapped_key_data.byte_length() % 8, 0u);

  SECItem iv_item = MakeSECItemForBuffer(CryptoData(kAesIv, sizeof(kAesIv)));
  crypto::ScopedSECItem param_item(
      PK11_ParamFromIV(CKM_NSS_AES_KEY_WRAP, &iv_item));
  if (!param_item)
    return Status::ErrorUnexpected();

  SECItem cipher_text = MakeSECItemForBuffer(wrapped_key_data);

  // The plaintext length is always 64 bits less than the data size.
  const unsigned int plaintext_length = wrapped_key_data.byte_length() - 8;

#if defined(USE_NSS)
  // Part of workaround for
  // https://bugzilla.mozilla.org/show_bug.cgi?id=981170. See the explanation
  // later in this function.
  PORT_SetError(0);
#endif

  crypto::ScopedPK11SymKey new_key(
      PK11_UnwrapSymKeyWithFlags(wrapping_key->key(),
                                 CKM_NSS_AES_KEY_WRAP,
                                 param_item.get(),
                                 &cipher_text,
                                 mechanism,
                                 CKA_FLAGS_ONLY,
                                 plaintext_length,
                                 flags));

  // TODO(padolph): Use NSS PORT_GetError() and friends to report a more
  // accurate error, providing if doesn't leak any information to web pages
  // about other web crypto users, key details, etc.
  if (!new_key)
    return Status::OperationError();

#if defined(USE_NSS)
  // Workaround for https://bugzilla.mozilla.org/show_bug.cgi?id=981170
  // which was fixed in NSS 3.16.0.
  // If unwrap fails, NSS nevertheless returns a valid-looking PK11SymKey,
  // with a reasonable length but with key data pointing to uninitialized
  // memory.
  // To understand this workaround see the fix for 981170:
  // https://hg.mozilla.org/projects/nss/rev/753bb69e543c
  if (!NSS_VersionCheck("3.16") && PORT_GetError() == SEC_ERROR_BAD_DATA)
    return Status::OperationError();
#endif

  *unwrapped_key = new_key.Pass();
  return Status::Success();
}

void CopySECItemToVector(const SECItem& item, std::vector<uint8>* out) {
  out->assign(item.data, item.data + item.len);
}

// The system NSS library doesn't have the new PK11_ExportDERPrivateKeyInfo
// function yet (https://bugzilla.mozilla.org/show_bug.cgi?id=519255). So we
// provide a fallback implementation.
#if defined(USE_NSS)
// From PKCS#1 [http://tools.ietf.org/html/rfc3447]:
//
//    RSAPrivateKey ::= SEQUENCE {
//      version           Version,
//      modulus           INTEGER,  -- n
//      publicExponent    INTEGER,  -- e
//      privateExponent   INTEGER,  -- d
//      prime1            INTEGER,  -- p
//      prime2            INTEGER,  -- q
//      exponent1         INTEGER,  -- d mod (p-1)
//      exponent2         INTEGER,  -- d mod (q-1)
//      coefficient       INTEGER,  -- (inverse of q) mod p
//      otherPrimeInfos   OtherPrimeInfos OPTIONAL
//    }
//
// Note that otherPrimeInfos is only applicable for version=1. Since NSS
// doesn't use multi-prime can safely use version=0.
struct RSAPrivateKey {
  SECItem version;
  SECItem modulus;
  SECItem public_exponent;
  SECItem private_exponent;
  SECItem prime1;
  SECItem prime2;
  SECItem exponent1;
  SECItem exponent2;
  SECItem coefficient;
};

const SEC_ASN1Template RSAPrivateKeyTemplate[] = {
    {SEC_ASN1_SEQUENCE, 0, NULL, sizeof(RSAPrivateKey)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, version)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, modulus)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, public_exponent)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, private_exponent)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, prime1)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, prime2)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, exponent1)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, exponent2)},
    {SEC_ASN1_INTEGER, offsetof(RSAPrivateKey, coefficient)},
    {0}};

// On success |value| will be filled with data which must be freed by
// SECITEM_FreeItem(value, PR_FALSE);
bool ReadUint(SECKEYPrivateKey* key,
              CK_ATTRIBUTE_TYPE attribute,
              SECItem* value) {
  SECStatus rv = PK11_ReadRawAttribute(PK11_TypePrivKey, key, attribute, value);

  // PK11_ReadRawAttribute() returns items of type siBuffer. However in order
  // for the ASN.1 encoding to be correct, the items must be of type
  // siUnsignedInteger.
  value->type = siUnsignedInteger;

  return rv == SECSuccess;
}

// Fills |out| with the RSA private key properties. Returns true on success.
// Regardless of the return value, the caller must invoke FreeRSAPrivateKey()
// to free up any allocated memory.
//
// The passed in RSAPrivateKey must be zero-initialized.
bool InitRSAPrivateKey(SECKEYPrivateKey* key, RSAPrivateKey* out) {
  if (key->keyType != rsaKey)
    return false;

  // Everything should be zero-ed out. These are just some spot checks.
  DCHECK(!out->version.data);
  DCHECK(!out->version.len);
  DCHECK(!out->modulus.data);
  DCHECK(!out->modulus.len);

  // Always use version=0 since not using multi-prime.
  if (!SEC_ASN1EncodeInteger(NULL, &out->version, 0))
    return false;

  if (!ReadUint(key, CKA_MODULUS, &out->modulus))
    return false;
  if (!ReadUint(key, CKA_PUBLIC_EXPONENT, &out->public_exponent))
    return false;
  if (!ReadUint(key, CKA_PRIVATE_EXPONENT, &out->private_exponent))
    return false;
  if (!ReadUint(key, CKA_PRIME_1, &out->prime1))
    return false;
  if (!ReadUint(key, CKA_PRIME_2, &out->prime2))
    return false;
  if (!ReadUint(key, CKA_EXPONENT_1, &out->exponent1))
    return false;
  if (!ReadUint(key, CKA_EXPONENT_2, &out->exponent2))
    return false;
  if (!ReadUint(key, CKA_COEFFICIENT, &out->coefficient))
    return false;

  return true;
}

struct FreeRsaPrivateKey {
  void operator()(RSAPrivateKey* out) {
    SECITEM_FreeItem(&out->version, PR_FALSE);
    SECITEM_FreeItem(&out->modulus, PR_FALSE);
    SECITEM_FreeItem(&out->public_exponent, PR_FALSE);
    SECITEM_FreeItem(&out->private_exponent, PR_FALSE);
    SECITEM_FreeItem(&out->prime1, PR_FALSE);
    SECITEM_FreeItem(&out->prime2, PR_FALSE);
    SECITEM_FreeItem(&out->exponent1, PR_FALSE);
    SECITEM_FreeItem(&out->exponent2, PR_FALSE);
    SECITEM_FreeItem(&out->coefficient, PR_FALSE);
  }
};
#endif  // defined(USE_NSS)

}  // namespace

class DigestorNSS : public blink::WebCryptoDigestor {
 public:
  explicit DigestorNSS(blink::WebCryptoAlgorithmId algorithm_id)
      : hash_context_(NULL), algorithm_id_(algorithm_id) {}

  virtual ~DigestorNSS() {
    if (!hash_context_)
      return;

    HASH_Destroy(hash_context_);
    hash_context_ = NULL;
  }

  virtual bool consume(const unsigned char* data, unsigned int size) {
    return ConsumeWithStatus(data, size).IsSuccess();
  }

  Status ConsumeWithStatus(const unsigned char* data, unsigned int size) {
    // Initialize everything if the object hasn't been initialized yet.
    if (!hash_context_) {
      Status error = Init();
      if (!error.IsSuccess())
        return error;
    }

    HASH_Update(hash_context_, data, size);

    return Status::Success();
  }

  virtual bool finish(unsigned char*& result_data,
                      unsigned int& result_data_size) {
    Status error = FinishInternal(result_, &result_data_size);
    if (!error.IsSuccess())
      return false;
    result_data = result_;
    return true;
  }

  Status FinishWithVectorAndStatus(std::vector<uint8>* result) {
    if (!hash_context_)
      return Status::ErrorUnexpected();

    unsigned int result_length = HASH_ResultLenContext(hash_context_);
    result->resize(result_length);
    unsigned char* digest = Uint8VectorStart(result);
    unsigned int digest_size;  // ignored
    return FinishInternal(digest, &digest_size);
  }

 private:
  Status Init() {
    HASH_HashType hash_type = WebCryptoAlgorithmToNSSHashType(algorithm_id_);

    if (hash_type == HASH_AlgNULL)
      return Status::ErrorUnsupported();

    hash_context_ = HASH_Create(hash_type);
    if (!hash_context_)
      return Status::OperationError();

    HASH_Begin(hash_context_);

    return Status::Success();
  }

  Status FinishInternal(unsigned char* result, unsigned int* result_size) {
    if (!hash_context_) {
      Status error = Init();
      if (!error.IsSuccess())
        return error;
    }

    unsigned int hash_result_length = HASH_ResultLenContext(hash_context_);
    DCHECK_LE(hash_result_length, static_cast<size_t>(HASH_LENGTH_MAX));

    HASH_End(hash_context_, result, result_size, hash_result_length);

    if (*result_size != hash_result_length)
      return Status::ErrorUnexpected();
    return Status::Success();
  }

  HASHContext* hash_context_;
  blink::WebCryptoAlgorithmId algorithm_id_;
  unsigned char result_[HASH_LENGTH_MAX];
};

Status ImportKeyRaw(const blink::WebCryptoAlgorithm& algorithm,
                    const CryptoData& key_data,
                    bool extractable,
                    blink::WebCryptoKeyUsageMask usage_mask,
                    blink::WebCryptoKey* key) {
  DCHECK(!algorithm.isNull());

  CK_MECHANISM_TYPE mechanism;
  CK_FLAGS flags;
  Status status =
      WebCryptoAlgorithmToNssMechFlags(algorithm, &mechanism, &flags);
  if (status.IsError())
    return status;

  SECItem key_item = MakeSECItemForBuffer(key_data);

  crypto::ScopedPK11Slot slot(PK11_GetInternalSlot());
  crypto::ScopedPK11SymKey pk11_sym_key(
      PK11_ImportSymKeyWithFlags(slot.get(),
                                 mechanism,
                                 PK11_OriginUnwrap,
                                 CKA_FLAGS_ONLY,
                                 &key_item,
                                 flags,
                                 false,
                                 NULL));
  if (!pk11_sym_key.get())
    return Status::OperationError();

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreateSecretKeyAlgorithm(
          algorithm, key_data.byte_length(), &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<SymKey> key_handle;
  status = SymKey::Create(pk11_sym_key.Pass(), &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(key_handle.release(),
                                     blink::WebCryptoKeyTypeSecret,
                                     extractable,
                                     key_algorithm,
                                     usage_mask);
  return Status::Success();
}

Status ExportKeyRaw(SymKey* key, std::vector<uint8>* buffer) {
  if (PK11_ExtractKeyValue(key->key()) != SECSuccess)
    return Status::OperationError();

  // http://crbug.com/366427: the spec does not define any other failures for
  // exporting, so none of the subsequent errors are spec compliant.
  const SECItem* key_data = PK11_GetKeyData(key->key());
  if (!key_data)
    return Status::OperationError();

  buffer->assign(key_data->data, key_data->data + key_data->len);

  return Status::Success();
}

namespace {

typedef scoped_ptr<CERTSubjectPublicKeyInfo,
                   crypto::NSSDestroyer<CERTSubjectPublicKeyInfo,
                                        SECKEY_DestroySubjectPublicKeyInfo> >
    ScopedCERTSubjectPublicKeyInfo;

// Validates an NSS KeyType against a WebCrypto import algorithm.
bool ValidateNssKeyTypeAgainstInputAlgorithm(
    KeyType key_type,
    const blink::WebCryptoAlgorithm& algorithm) {
  switch (key_type) {
    case rsaKey:
      return IsAlgorithmRsa(algorithm);
    case dsaKey:
    case ecKey:
    case rsaPssKey:
    case rsaOaepKey:
      // TODO(padolph): Handle other key types.
      break;
    default:
      break;
  }
  return false;
}

}  // namespace

Status ImportKeySpki(const blink::WebCryptoAlgorithm& algorithm,
                     const CryptoData& key_data,
                     bool extractable,
                     blink::WebCryptoKeyUsageMask usage_mask,
                     blink::WebCryptoKey* key) {
  DCHECK(key);

  if (!key_data.byte_length())
    return Status::ErrorImportEmptyKeyData();
  DCHECK(key_data.bytes());

  // The binary blob 'key_data' is expected to be a DER-encoded ASN.1 Subject
  // Public Key Info. Decode this to a CERTSubjectPublicKeyInfo.
  SECItem spki_item = MakeSECItemForBuffer(key_data);
  const ScopedCERTSubjectPublicKeyInfo spki(
      SECKEY_DecodeDERSubjectPublicKeyInfo(&spki_item));
  if (!spki)
    return Status::DataError();

  crypto::ScopedSECKEYPublicKey sec_public_key(
      SECKEY_ExtractPublicKey(spki.get()));
  if (!sec_public_key)
    return Status::DataError();

  const KeyType sec_key_type = SECKEY_GetPublicKeyType(sec_public_key.get());
  if (!ValidateNssKeyTypeAgainstInputAlgorithm(sec_key_type, algorithm))
    return Status::DataError();

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreatePublicKeyAlgorithm(
          algorithm, sec_public_key.get(), &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<PublicKey> key_handle;
  Status status = PublicKey::Create(sec_public_key.Pass(), &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(key_handle.release(),
                                     blink::WebCryptoKeyTypePublic,
                                     extractable,
                                     key_algorithm,
                                     usage_mask);

  return Status::Success();
}

Status ExportKeySpki(PublicKey* key, std::vector<uint8>* buffer) {
  const crypto::ScopedSECItem spki_der(
      SECKEY_EncodeDERSubjectPublicKeyInfo(key->key()));
  // http://crbug.com/366427: the spec does not define any other failures for
  // exporting, so none of the subsequent errors are spec compliant.
  if (!spki_der)
    return Status::OperationError();

  DCHECK(spki_der->data);
  DCHECK(spki_der->len);

  buffer->assign(spki_der->data, spki_der->data + spki_der->len);

  return Status::Success();
}

Status ExportRsaPublicKey(PublicKey* key,
                          std::vector<uint8>* modulus,
                          std::vector<uint8>* public_exponent) {
  DCHECK(key);
  DCHECK(key->key());
  if (key->key()->keyType != rsaKey)
    return Status::ErrorUnsupported();
  CopySECItemToVector(key->key()->u.rsa.modulus, modulus);
  CopySECItemToVector(key->key()->u.rsa.publicExponent, public_exponent);
  if (modulus->empty() || public_exponent->empty())
    return Status::ErrorUnexpected();
  return Status::Success();
}

Status ExportKeyPkcs8(PrivateKey* key,
                      const blink::WebCryptoKeyAlgorithm& key_algorithm,
                      std::vector<uint8>* buffer) {
  // TODO(eroman): Support other RSA key types as they are added to Blink.
  if (key_algorithm.id() != blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5 &&
      key_algorithm.id() != blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5)
    return Status::ErrorUnsupported();

#if defined(USE_NSS)
  // PK11_ExportDERPrivateKeyInfo isn't available. Use our fallback code.
  const SECOidTag algorithm = SEC_OID_PKCS1_RSA_ENCRYPTION;
  const int kPrivateKeyInfoVersion = 0;

  SECKEYPrivateKeyInfo private_key_info = {};
  RSAPrivateKey rsa_private_key = {};
  scoped_ptr<RSAPrivateKey, FreeRsaPrivateKey> free_private_key(
      &rsa_private_key);

  // http://crbug.com/366427: the spec does not define any other failures for
  // exporting, so none of the subsequent errors are spec compliant.
  if (!InitRSAPrivateKey(key->key(), &rsa_private_key))
    return Status::OperationError();

  crypto::ScopedPLArenaPool arena(PORT_NewArena(DER_DEFAULT_CHUNKSIZE));
  if (!arena.get())
    return Status::OperationError();

  if (!SEC_ASN1EncodeItem(arena.get(),
                          &private_key_info.privateKey,
                          &rsa_private_key,
                          RSAPrivateKeyTemplate))
    return Status::OperationError();

  if (SECSuccess !=
      SECOID_SetAlgorithmID(
          arena.get(), &private_key_info.algorithm, algorithm, NULL))
    return Status::OperationError();

  if (!SEC_ASN1EncodeInteger(
          arena.get(), &private_key_info.version, kPrivateKeyInfoVersion))
    return Status::OperationError();

  crypto::ScopedSECItem encoded_key(
      SEC_ASN1EncodeItem(NULL,
                         NULL,
                         &private_key_info,
                         SEC_ASN1_GET(SECKEY_PrivateKeyInfoTemplate)));
#else   // defined(USE_NSS)
  crypto::ScopedSECItem encoded_key(
      PK11_ExportDERPrivateKeyInfo(key->key(), NULL));
#endif  // defined(USE_NSS)

  if (!encoded_key.get())
    return Status::OperationError();

  buffer->assign(encoded_key->data, encoded_key->data + encoded_key->len);
  return Status::Success();
}

Status ImportKeyPkcs8(const blink::WebCryptoAlgorithm& algorithm,
                      const CryptoData& key_data,
                      bool extractable,
                      blink::WebCryptoKeyUsageMask usage_mask,
                      blink::WebCryptoKey* key) {
  DCHECK(key);

  if (!key_data.byte_length())
    return Status::ErrorImportEmptyKeyData();
  DCHECK(key_data.bytes());

  // The binary blob 'key_data' is expected to be a DER-encoded ASN.1 PKCS#8
  // private key info object.
  SECItem pki_der = MakeSECItemForBuffer(key_data);

  SECKEYPrivateKey* seckey_private_key = NULL;
  crypto::ScopedPK11Slot slot(PK11_GetInternalSlot());
  if (PK11_ImportDERPrivateKeyInfoAndReturnKey(slot.get(),
                                               &pki_der,
                                               NULL,    // nickname
                                               NULL,    // publicValue
                                               false,   // isPerm
                                               false,   // isPrivate
                                               KU_ALL,  // usage
                                               &seckey_private_key,
                                               NULL) != SECSuccess) {
    return Status::DataError();
  }
  DCHECK(seckey_private_key);
  crypto::ScopedSECKEYPrivateKey private_key(seckey_private_key);

  const KeyType sec_key_type = SECKEY_GetPrivateKeyType(private_key.get());
  if (!ValidateNssKeyTypeAgainstInputAlgorithm(sec_key_type, algorithm))
    return Status::DataError();

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreatePrivateKeyAlgorithm(algorithm, private_key.get(), &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<PrivateKey> key_handle;
  Status status =
      PrivateKey::Create(private_key.Pass(), key_algorithm, &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(key_handle.release(),
                                     blink::WebCryptoKeyTypePrivate,
                                     extractable,
                                     key_algorithm,
                                     usage_mask);

  return Status::Success();
}

// -----------------------------------
// Hmac
// -----------------------------------

Status SignHmac(SymKey* key,
                const blink::WebCryptoAlgorithm& hash,
                const CryptoData& data,
                std::vector<uint8>* buffer) {
  DCHECK_EQ(PK11_GetMechanism(key->key()), WebCryptoHashToHMACMechanism(hash));

  SECItem param_item = {siBuffer, NULL, 0};
  SECItem data_item = MakeSECItemForBuffer(data);
  // First call is to figure out the length.
  SECItem signature_item = {siBuffer, NULL, 0};

  if (PK11_SignWithSymKey(key->key(),
                          PK11_GetMechanism(key->key()),
                          &param_item,
                          &signature_item,
                          &data_item) != SECSuccess) {
    return Status::OperationError();
  }

  DCHECK_NE(0u, signature_item.len);

  buffer->resize(signature_item.len);
  signature_item.data = Uint8VectorStart(buffer);

  if (PK11_SignWithSymKey(key->key(),
                          PK11_GetMechanism(key->key()),
                          &param_item,
                          &signature_item,
                          &data_item) != SECSuccess) {
    return Status::OperationError();
  }

  DCHECK_EQ(buffer->size(), signature_item.len);
  return Status::Success();
}

// -----------------------------------
// RsaEsPkcs1v1_5
// -----------------------------------

Status EncryptRsaEsPkcs1v1_5(PublicKey* key,
                             const CryptoData& data,
                             std::vector<uint8>* buffer) {
  const unsigned int encrypted_length_bytes =
      SECKEY_PublicKeyStrength(key->key());

  // RSAES can operate on messages up to a length of k - 11, where k is the
  // octet length of the RSA modulus.
  if (encrypted_length_bytes < 11 ||
      encrypted_length_bytes - 11 < data.byte_length())
    return Status::ErrorDataTooLarge();

  buffer->resize(encrypted_length_bytes);
  unsigned char* const buffer_data = Uint8VectorStart(buffer);

  if (PK11_PubEncryptPKCS1(key->key(),
                           buffer_data,
                           const_cast<unsigned char*>(data.bytes()),
                           data.byte_length(),
                           NULL) != SECSuccess) {
    return Status::OperationError();
  }
  return Status::Success();
}

Status DecryptRsaEsPkcs1v1_5(PrivateKey* key,
                             const CryptoData& data,
                             std::vector<uint8>* buffer) {
  const int modulus_length_bytes = PK11_GetPrivateModulusLen(key->key());
  if (modulus_length_bytes <= 0)
    return Status::ErrorUnexpected();
  const unsigned int max_output_length_bytes = modulus_length_bytes;

  buffer->resize(max_output_length_bytes);
  unsigned char* const buffer_data = Uint8VectorStart(buffer);

  unsigned int output_length_bytes = 0;
  if (PK11_PrivDecryptPKCS1(key->key(),
                            buffer_data,
                            &output_length_bytes,
                            max_output_length_bytes,
                            const_cast<unsigned char*>(data.bytes()),
                            data.byte_length()) != SECSuccess) {
    return Status::OperationError();
  }
  DCHECK_LE(output_length_bytes, max_output_length_bytes);
  buffer->resize(output_length_bytes);
  return Status::Success();
}

// -----------------------------------
// RsaSsaPkcs1v1_5
// -----------------------------------

Status SignRsaSsaPkcs1v1_5(PrivateKey* key,
                           const blink::WebCryptoAlgorithm& hash,
                           const CryptoData& data,
                           std::vector<uint8>* buffer) {
  // Pick the NSS signing algorithm by combining RSA-SSA (RSA PKCS1) and the
  // inner hash of the input Web Crypto algorithm.
  SECOidTag sign_alg_tag;
  switch (hash.id()) {
    case blink::WebCryptoAlgorithmIdSha1:
      sign_alg_tag = SEC_OID_PKCS1_SHA1_WITH_RSA_ENCRYPTION;
      break;
    case blink::WebCryptoAlgorithmIdSha256:
      sign_alg_tag = SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION;
      break;
    case blink::WebCryptoAlgorithmIdSha384:
      sign_alg_tag = SEC_OID_PKCS1_SHA384_WITH_RSA_ENCRYPTION;
      break;
    case blink::WebCryptoAlgorithmIdSha512:
      sign_alg_tag = SEC_OID_PKCS1_SHA512_WITH_RSA_ENCRYPTION;
      break;
    default:
      return Status::ErrorUnsupported();
  }

  crypto::ScopedSECItem signature_item(SECITEM_AllocItem(NULL, NULL, 0));
  if (SEC_SignData(signature_item.get(),
                   data.bytes(),
                   data.byte_length(),
                   key->key(),
                   sign_alg_tag) != SECSuccess) {
    return Status::OperationError();
  }

  buffer->assign(signature_item->data,
                 signature_item->data + signature_item->len);
  return Status::Success();
}

Status VerifyRsaSsaPkcs1v1_5(PublicKey* key,
                             const blink::WebCryptoAlgorithm& hash,
                             const CryptoData& signature,
                             const CryptoData& data,
                             bool* signature_match) {
  const SECItem signature_item = MakeSECItemForBuffer(signature);

  SECOidTag hash_alg_tag;
  switch (hash.id()) {
    case blink::WebCryptoAlgorithmIdSha1:
      hash_alg_tag = SEC_OID_SHA1;
      break;
    case blink::WebCryptoAlgorithmIdSha256:
      hash_alg_tag = SEC_OID_SHA256;
      break;
    case blink::WebCryptoAlgorithmIdSha384:
      hash_alg_tag = SEC_OID_SHA384;
      break;
    case blink::WebCryptoAlgorithmIdSha512:
      hash_alg_tag = SEC_OID_SHA512;
      break;
    default:
      return Status::ErrorUnsupported();
  }

  *signature_match =
      SECSuccess == VFY_VerifyDataDirect(data.bytes(),
                                         data.byte_length(),
                                         key->key(),
                                         &signature_item,
                                         SEC_OID_PKCS1_RSA_ENCRYPTION,
                                         hash_alg_tag,
                                         NULL,
                                         NULL);
  return Status::Success();
}

Status EncryptDecryptAesCbc(EncryptOrDecrypt mode,
                            SymKey* key,
                            const CryptoData& data,
                            const CryptoData& iv,
                            std::vector<uint8>* buffer) {
  // TODO(eroman): Inline.
  return AesCbcEncryptDecrypt(mode, key, iv, data, buffer);
}

Status EncryptDecryptAesGcm(EncryptOrDecrypt mode,
                            SymKey* key,
                            const CryptoData& data,
                            const CryptoData& iv,
                            const CryptoData& additional_data,
                            unsigned int tag_length_bits,
                            std::vector<uint8>* buffer) {
  // TODO(eroman): Inline.
  return AesGcmEncryptDecrypt(
      mode, key, data, iv, additional_data, tag_length_bits, buffer);
}

// -----------------------------------
// Key generation
// -----------------------------------

Status GenerateRsaKeyPair(const blink::WebCryptoAlgorithm& algorithm,
                          bool extractable,
                          blink::WebCryptoKeyUsageMask usage_mask,
                          unsigned int modulus_length_bits,
                          const CryptoData& public_exponent,
                          const blink::WebCryptoAlgorithm& hash_or_null,
                          blink::WebCryptoKey* public_key,
                          blink::WebCryptoKey* private_key) {
  crypto::ScopedPK11Slot slot(PK11_GetInternalKeySlot());
  if (!slot)
    return Status::OperationError();

  unsigned long public_exponent_long;
  if (!BigIntegerToLong(public_exponent.bytes(),
                        public_exponent.byte_length(),
                        &public_exponent_long) ||
      !public_exponent_long) {
    return Status::ErrorGenerateKeyPublicExponent();
  }

  PK11RSAGenParams rsa_gen_params;
  rsa_gen_params.keySizeInBits = modulus_length_bits;
  rsa_gen_params.pe = public_exponent_long;

  // Flags are verified at the Blink layer; here the flags are set to all
  // possible operations for the given key type.
  CK_FLAGS operation_flags;
  switch (algorithm.id()) {
    case blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5:
    case blink::WebCryptoAlgorithmIdRsaOaep:
      operation_flags = CKF_ENCRYPT | CKF_DECRYPT | CKF_WRAP | CKF_UNWRAP;
      break;
    case blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5:
      operation_flags = CKF_SIGN | CKF_VERIFY;
      break;
    default:
      NOTREACHED();
      return Status::ErrorUnexpected();
  }
  const CK_FLAGS operation_flags_mask =
      CKF_ENCRYPT | CKF_DECRYPT | CKF_SIGN | CKF_VERIFY | CKF_WRAP | CKF_UNWRAP;

  // The private key must be marked as insensitive and extractable, otherwise it
  // cannot later be exported in unencrypted form or structured-cloned.
  const PK11AttrFlags attribute_flags =
      PK11_ATTR_INSENSITIVE | PK11_ATTR_EXTRACTABLE;

  // Note: NSS does not generate an sec_public_key if the call below fails,
  // so there is no danger of a leaked sec_public_key.
  SECKEYPublicKey* sec_public_key;
  crypto::ScopedSECKEYPrivateKey scoped_sec_private_key(
      PK11_GenerateKeyPairWithOpFlags(slot.get(),
                                      CKM_RSA_PKCS_KEY_PAIR_GEN,
                                      &rsa_gen_params,
                                      &sec_public_key,
                                      attribute_flags,
                                      operation_flags,
                                      operation_flags_mask,
                                      NULL));
  if (!private_key)
    return Status::OperationError();

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreatePublicKeyAlgorithm(algorithm, sec_public_key, &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<PublicKey> public_key_handle;
  Status status = PublicKey::Create(
      crypto::ScopedSECKEYPublicKey(sec_public_key), &public_key_handle);
  if (status.IsError())
    return status;

  scoped_ptr<PrivateKey> private_key_handle;
  status = PrivateKey::Create(
      scoped_sec_private_key.Pass(), key_algorithm, &private_key_handle);
  if (status.IsError())
    return status;

  *public_key = blink::WebCryptoKey::create(public_key_handle.release(),
                                            blink::WebCryptoKeyTypePublic,
                                            true,
                                            key_algorithm,
                                            usage_mask);
  *private_key = blink::WebCryptoKey::create(private_key_handle.release(),
                                             blink::WebCryptoKeyTypePrivate,
                                             extractable,
                                             key_algorithm,
                                             usage_mask);

  return Status::Success();
}

void Init() {
  crypto::EnsureNSSInit();
}

Status DigestSha(blink::WebCryptoAlgorithmId algorithm,
                 const CryptoData& data,
                 std::vector<uint8>* buffer) {
  DigestorNSS digestor(algorithm);
  Status error = digestor.ConsumeWithStatus(data.bytes(), data.byte_length());
  // http://crbug.com/366427: the spec does not define any other failures for
  // digest, so none of the subsequent errors are spec compliant.
  if (!error.IsSuccess())
    return error;
  return digestor.FinishWithVectorAndStatus(buffer);
}

scoped_ptr<blink::WebCryptoDigestor> CreateDigestor(
    blink::WebCryptoAlgorithmId algorithm_id) {
  return scoped_ptr<blink::WebCryptoDigestor>(new DigestorNSS(algorithm_id));
}

Status GenerateSecretKey(const blink::WebCryptoAlgorithm& algorithm,
                         bool extractable,
                         blink::WebCryptoKeyUsageMask usage_mask,
                         unsigned keylen_bytes,
                         blink::WebCryptoKey* key) {
  CK_MECHANISM_TYPE mech = WebCryptoAlgorithmToGenMechanism(algorithm);
  blink::WebCryptoKeyType key_type = blink::WebCryptoKeyTypeSecret;

  if (mech == CKM_INVALID_MECHANISM)
    return Status::ErrorUnsupported();

  crypto::ScopedPK11Slot slot(PK11_GetInternalKeySlot());
  if (!slot)
    return Status::OperationError();

  crypto::ScopedPK11SymKey pk11_key(
      PK11_KeyGen(slot.get(), mech, NULL, keylen_bytes, NULL));

  if (!pk11_key)
    return Status::OperationError();

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreateSecretKeyAlgorithm(algorithm, keylen_bytes, &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<SymKey> key_handle;
  Status status = SymKey::Create(pk11_key.Pass(), &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(
      key_handle.release(), key_type, extractable, key_algorithm, usage_mask);
  return Status::Success();
}

Status ImportRsaPublicKey(const blink::WebCryptoAlgorithm& algorithm,
                          bool extractable,
                          blink::WebCryptoKeyUsageMask usage_mask,
                          const CryptoData& modulus_data,
                          const CryptoData& exponent_data,
                          blink::WebCryptoKey* key) {
  if (!modulus_data.byte_length())
    return Status::ErrorImportRsaEmptyModulus();

  if (!exponent_data.byte_length())
    return Status::ErrorImportRsaEmptyExponent();

  DCHECK(modulus_data.bytes());
  DCHECK(exponent_data.bytes());

  // NSS does not provide a way to create an RSA public key directly from the
  // modulus and exponent values, but it can import an DER-encoded ASN.1 blob
  // with these values and create the public key from that. The code below
  // follows the recommendation described in
  // https://developer.mozilla.org/en-US/docs/NSS/NSS_Tech_Notes/nss_tech_note7

  // Pack the input values into a struct compatible with NSS ASN.1 encoding, and
  // set up an ASN.1 encoder template for it.
  struct RsaPublicKeyData {
    SECItem modulus;
    SECItem exponent;
  };
  const RsaPublicKeyData pubkey_in = {
      {siUnsignedInteger, const_cast<unsigned char*>(modulus_data.bytes()),
       modulus_data.byte_length()},
      {siUnsignedInteger, const_cast<unsigned char*>(exponent_data.bytes()),
       exponent_data.byte_length()}};
  const SEC_ASN1Template rsa_public_key_template[] = {
      {SEC_ASN1_SEQUENCE, 0, NULL, sizeof(RsaPublicKeyData)},
      {SEC_ASN1_INTEGER, offsetof(RsaPublicKeyData, modulus), },
      {SEC_ASN1_INTEGER, offsetof(RsaPublicKeyData, exponent), },
      {0, }};

  // DER-encode the public key.
  crypto::ScopedSECItem pubkey_der(
      SEC_ASN1EncodeItem(NULL, NULL, &pubkey_in, rsa_public_key_template));
  if (!pubkey_der)
    return Status::OperationError();

  // Import the DER-encoded public key to create an RSA SECKEYPublicKey.
  crypto::ScopedSECKEYPublicKey pubkey(
      SECKEY_ImportDERPublicKey(pubkey_der.get(), CKK_RSA));
  if (!pubkey)
    return Status::OperationError();

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreatePublicKeyAlgorithm(algorithm, pubkey.get(), &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<PublicKey> key_handle;
  Status status = PublicKey::Create(pubkey.Pass(), &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(key_handle.release(),
                                     blink::WebCryptoKeyTypePublic,
                                     extractable,
                                     key_algorithm,
                                     usage_mask);
  return Status::Success();
}

Status WrapSymKeyAesKw(SymKey* key,
                       SymKey* wrapping_key,
                       std::vector<uint8>* buffer) {
  // The data size must be at least 16 bytes and a multiple of 8 bytes.
  // RFC 3394 does not specify a maximum allowed data length, but since only
  // keys are being wrapped in this application (which are small), a reasonable
  // max limit is whatever will fit into an unsigned. For the max size test,
  // note that AES Key Wrap always adds 8 bytes to the input data size.
  const unsigned int input_length = PK11_GetKeyLength(key->key());
  if (input_length < 16)
    return Status::ErrorDataTooSmall();
  if (input_length > UINT_MAX - 8)
    return Status::ErrorDataTooLarge();
  if (input_length % 8)
    return Status::ErrorInvalidAesKwDataLength();

  SECItem iv_item = MakeSECItemForBuffer(CryptoData(kAesIv, sizeof(kAesIv)));
  crypto::ScopedSECItem param_item(
      PK11_ParamFromIV(CKM_NSS_AES_KEY_WRAP, &iv_item));
  if (!param_item)
    return Status::ErrorUnexpected();

  const unsigned int output_length = input_length + 8;
  buffer->resize(output_length);
  SECItem wrapped_key_item = MakeSECItemForBuffer(CryptoData(*buffer));

  if (SECSuccess != PK11_WrapSymKey(CKM_NSS_AES_KEY_WRAP,
                                    param_item.get(),
                                    wrapping_key->key(),
                                    key->key(),
                                    &wrapped_key_item)) {
    return Status::OperationError();
  }
  if (output_length != wrapped_key_item.len)
    return Status::ErrorUnexpected();

  return Status::Success();
}

Status UnwrapSymKeyAesKw(const CryptoData& wrapped_key_data,
                         SymKey* wrapping_key,
                         const blink::WebCryptoAlgorithm& algorithm,
                         bool extractable,
                         blink::WebCryptoKeyUsageMask usage_mask,
                         blink::WebCryptoKey* key) {
  // Determine the proper NSS key properties from the input algorithm.
  CK_MECHANISM_TYPE mechanism;
  CK_FLAGS flags;
  Status status =
      WebCryptoAlgorithmToNssMechFlags(algorithm, &mechanism, &flags);
  if (status.IsError())
    return status;

  crypto::ScopedPK11SymKey unwrapped_key;
  status = DoUnwrapSymKeyAesKw(
      wrapped_key_data, wrapping_key, mechanism, flags, &unwrapped_key);
  if (status.IsError())
    return status;

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreateSecretKeyAlgorithm(
          algorithm, PK11_GetKeyLength(unwrapped_key.get()), &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<SymKey> key_handle;
  status = SymKey::Create(unwrapped_key.Pass(), &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(key_handle.release(),
                                     blink::WebCryptoKeyTypeSecret,
                                     extractable,
                                     key_algorithm,
                                     usage_mask);
  return Status::Success();
}

Status DecryptAesKw(SymKey* wrapping_key,
                    const CryptoData& data,
                    std::vector<uint8>* buffer) {
  // Due to limitations in the NSS API for the AES-KW algorithm, |data| must be
  // temporarily viewed as a symmetric key to be unwrapped (decrypted).
  crypto::ScopedPK11SymKey decrypted;
  Status status = DoUnwrapSymKeyAesKw(
      data, wrapping_key, CKK_GENERIC_SECRET, 0, &decrypted);
  if (status.IsError())
    return status;

  // Once the decrypt is complete, extract the resultant raw bytes from NSS and
  // return them to the caller.
  if (PK11_ExtractKeyValue(decrypted.get()) != SECSuccess)
    return Status::OperationError();
  const SECItem* const key_data = PK11_GetKeyData(decrypted.get());
  if (!key_data)
    return Status::OperationError();
  buffer->assign(key_data->data, key_data->data + key_data->len);

  return Status::Success();
}

Status WrapSymKeyRsaEs(SymKey* key,
                       PublicKey* wrapping_key,
                       std::vector<uint8>* buffer) {
  // Check the raw length of the key to be wrapped against the max size allowed
  // by the RSA wrapping key. With PKCS#1 v1.5 padding used in this function,
  // the maximum data length that can be encrypted is the wrapping_key's modulus
  // byte length minus eleven bytes.
  const unsigned int input_length_bytes = PK11_GetKeyLength(key->key());
  const unsigned int modulus_length_bytes =
      SECKEY_PublicKeyStrength(wrapping_key->key());
  if (modulus_length_bytes < 11 ||
      modulus_length_bytes - 11 < input_length_bytes)
    return Status::ErrorDataTooLarge();

  buffer->resize(modulus_length_bytes);
  SECItem wrapped_key_item = MakeSECItemForBuffer(CryptoData(*buffer));

  if (SECSuccess !=
      PK11_PubWrapSymKey(
          CKM_RSA_PKCS, wrapping_key->key(), key->key(), &wrapped_key_item)) {
    return Status::OperationError();
  }
  if (wrapped_key_item.len != modulus_length_bytes)
    return Status::ErrorUnexpected();

  return Status::Success();
}

Status UnwrapSymKeyRsaEs(const CryptoData& wrapped_key_data,
                         PrivateKey* wrapping_key,
                         const blink::WebCryptoAlgorithm& algorithm,
                         bool extractable,
                         blink::WebCryptoKeyUsageMask usage_mask,
                         blink::WebCryptoKey* key) {
  // Verify wrapped_key_data size does not exceed the modulus of the RSA key.
  const int modulus_length_bytes =
      PK11_GetPrivateModulusLen(wrapping_key->key());
  if (modulus_length_bytes <= 0)
    return Status::ErrorUnexpected();
  if (wrapped_key_data.byte_length() >
      static_cast<unsigned int>(modulus_length_bytes))
    return Status::ErrorDataTooLarge();

  // Determine the proper NSS key properties from the input algorithm.
  CK_MECHANISM_TYPE mechanism;
  CK_FLAGS flags;
  Status status =
      WebCryptoAlgorithmToNssMechFlags(algorithm, &mechanism, &flags);
  if (status.IsError())
    return status;

  SECItem wrapped_key_item = MakeSECItemForBuffer(wrapped_key_data);

  crypto::ScopedPK11SymKey unwrapped_key(
      PK11_PubUnwrapSymKeyWithFlagsPerm(wrapping_key->key(),
                                        &wrapped_key_item,
                                        mechanism,
                                        CKA_DECRYPT,
                                        0,
                                        flags,
                                        false));
  if (!unwrapped_key)
    return Status::OperationError();

  const unsigned int key_length = PK11_GetKeyLength(unwrapped_key.get());

  blink::WebCryptoKeyAlgorithm key_algorithm;
  if (!CreateSecretKeyAlgorithm(algorithm, key_length, &key_algorithm))
    return Status::ErrorUnexpected();

  scoped_ptr<SymKey> key_handle;
  status = SymKey::Create(unwrapped_key.Pass(), &key_handle);
  if (status.IsError())
    return status;

  *key = blink::WebCryptoKey::create(key_handle.release(),
                                     blink::WebCryptoKeyTypeSecret,
                                     extractable,
                                     key_algorithm,
                                     usage_mask);
  return Status::Success();
}

}  // namespace platform

}  // namespace webcrypto

}  // namespace content
