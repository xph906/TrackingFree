// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/child/webcrypto/shared_crypto.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "content/child/webcrypto/crypto_data.h"
#include "content/child/webcrypto/status.h"
#include "content/child/webcrypto/webcrypto_util.h"
#include "content/public/common/content_paths.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/WebKit/public/platform/WebCryptoAlgorithm.h"
#include "third_party/WebKit/public/platform/WebCryptoAlgorithmParams.h"
#include "third_party/WebKit/public/platform/WebCryptoKey.h"
#include "third_party/WebKit/public/platform/WebCryptoKeyAlgorithm.h"
#include "third_party/re2/re2/re2.h"

// The OpenSSL implementation of WebCrypto is less complete, so don't run all of
// the tests: http://crbug.com/267888
#if defined(USE_OPENSSL)
#define MAYBE(test_name) DISABLED_##test_name
#else
#define MAYBE(test_name) test_name
#endif

#define EXPECT_BYTES_EQ(expected, actual) \
  EXPECT_EQ(CryptoData(expected), CryptoData(actual))

#define EXPECT_BYTES_EQ_HEX(expected_hex, actual_bytes) \
  EXPECT_BYTES_EQ(HexStringToBytes(expected_hex), actual_bytes)

namespace content {

namespace webcrypto {

// These functions are used by GTEST to support EXPECT_EQ() for
// webcrypto::Status and webcrypto::CryptoData

void PrintTo(const Status& status, ::std::ostream* os) {
  if (status.IsSuccess())
    *os << "Success";
  else
    *os << "Error type: " << status.error_type()
        << " Error details: " << status.error_details();
}

bool operator==(const content::webcrypto::Status& a,
                const content::webcrypto::Status& b) {
  if (a.IsSuccess() != b.IsSuccess())
    return false;
  if (a.IsSuccess())
    return true;
  return a.error_type() == b.error_type() &&
         a.error_details() == b.error_details();
}

bool operator!=(const content::webcrypto::Status& a,
                const content::webcrypto::Status& b) {
  return !(a == b);
}

void PrintTo(const CryptoData& data, ::std::ostream* os) {
  *os << "[" << base::HexEncode(data.bytes(), data.byte_length()) << "]";
}

bool operator==(const content::webcrypto::CryptoData& a,
                const content::webcrypto::CryptoData& b) {
  return a.byte_length() == b.byte_length() &&
         memcmp(a.bytes(), b.bytes(), a.byte_length()) == 0;
}

namespace {

// -----------------------------------------------------------------------------

// TODO(eroman): For Linux builds using system NSS, AES-GCM support is a
// runtime dependency. Test it by trying to import a key.
// TODO(padolph): Consider caching the result of the import key test.
bool SupportsAesGcm() {
  std::vector<uint8> key_raw(16, 0);

  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  Status status = ImportKey(blink::WebCryptoKeyFormatRaw,
                            CryptoData(key_raw),
                            CreateAlgorithm(blink::WebCryptoAlgorithmIdAesGcm),
                            true,
                            blink::WebCryptoKeyUsageEncrypt,
                            &key);

  if (status.IsError())
    EXPECT_EQ(Status::ErrorUnsupported(), status);
  return status.IsSuccess();
}

blink::WebCryptoAlgorithm CreateRsaKeyGenAlgorithm(
    blink::WebCryptoAlgorithmId algorithm_id,
    unsigned int modulus_length,
    const std::vector<uint8>& public_exponent) {
  DCHECK_EQ(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5, algorithm_id);
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      algorithm_id,
      new blink::WebCryptoRsaKeyGenParams(
          modulus_length,
          webcrypto::Uint8VectorStart(public_exponent),
          public_exponent.size()));
}

blink::WebCryptoAlgorithm CreateRsaHashedKeyGenAlgorithm(
    blink::WebCryptoAlgorithmId algorithm_id,
    const blink::WebCryptoAlgorithmId hash_id,
    unsigned int modulus_length,
    const std::vector<uint8>& public_exponent) {
  DCHECK(algorithm_id == blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5 ||
         algorithm_id == blink::WebCryptoAlgorithmIdRsaOaep);
  DCHECK(blink::WebCryptoAlgorithm::isHash(hash_id));
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      algorithm_id,
      new blink::WebCryptoRsaHashedKeyGenParams(
          CreateAlgorithm(hash_id),
          modulus_length,
          webcrypto::Uint8VectorStart(public_exponent),
          public_exponent.size()));
}

// Creates an AES-CBC algorithm.
blink::WebCryptoAlgorithm CreateAesCbcAlgorithm(const std::vector<uint8>& iv) {
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      blink::WebCryptoAlgorithmIdAesCbc,
      new blink::WebCryptoAesCbcParams(Uint8VectorStart(iv), iv.size()));
}

// Creates and AES-GCM algorithm.
blink::WebCryptoAlgorithm CreateAesGcmAlgorithm(
    const std::vector<uint8>& iv,
    const std::vector<uint8>& additional_data,
    unsigned int tag_length_bits) {
  EXPECT_TRUE(SupportsAesGcm());
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      blink::WebCryptoAlgorithmIdAesGcm,
      new blink::WebCryptoAesGcmParams(Uint8VectorStart(iv),
                                       iv.size(),
                                       true,
                                       Uint8VectorStart(additional_data),
                                       additional_data.size(),
                                       true,
                                       tag_length_bits));
}

// Creates an HMAC algorithm whose parameters struct is compatible with key
// generation. It is an error to call this with a hash_id that is not a SHA*.
// The key_length_bits parameter is optional, with zero meaning unspecified.
blink::WebCryptoAlgorithm CreateHmacKeyGenAlgorithm(
    blink::WebCryptoAlgorithmId hash_id,
    unsigned int key_length_bits) {
  DCHECK(blink::WebCryptoAlgorithm::isHash(hash_id));
  // key_length_bytes == 0 means unspecified
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      blink::WebCryptoAlgorithmIdHmac,
      new blink::WebCryptoHmacKeyGenParams(
          CreateAlgorithm(hash_id), (key_length_bits != 0), key_length_bits));
}

// Returns a slightly modified version of the input vector.
//
//  - For non-empty inputs a single bit is inverted.
//  - For empty inputs, a byte is added.
std::vector<uint8> Corrupted(const std::vector<uint8>& input) {
  std::vector<uint8> corrupted_data(input);
  if (corrupted_data.empty())
    corrupted_data.push_back(0);
  corrupted_data[corrupted_data.size() / 2] ^= 0x01;
  return corrupted_data;
}

std::vector<uint8> HexStringToBytes(const std::string& hex) {
  std::vector<uint8> bytes;
  base::HexStringToBytes(hex, &bytes);
  return bytes;
}

std::vector<uint8> MakeJsonVector(const std::string& json_string) {
  return std::vector<uint8>(json_string.begin(), json_string.end());
}

std::vector<uint8> MakeJsonVector(const base::DictionaryValue& dict) {
  std::string json;
  base::JSONWriter::Write(&dict, &json);
  return MakeJsonVector(json);
}

// ----------------------------------------------------------------
// Helpers for working with JSON data files for test expectations.
// ----------------------------------------------------------------

// Reads a file in "src/content/test/data/webcrypto" to a base::Value.
// The file must be JSON, however it can also include C++ style comments.
::testing::AssertionResult ReadJsonTestFile(const char* test_file_name,
                                            scoped_ptr<base::Value>* value) {
  base::FilePath test_data_dir;
  if (!PathService::Get(DIR_TEST_DATA, &test_data_dir))
    return ::testing::AssertionFailure() << "Couldn't retrieve test dir";

  base::FilePath file_path =
      test_data_dir.AppendASCII("webcrypto").AppendASCII(test_file_name);

  std::string file_contents;
  if (!base::ReadFileToString(file_path, &file_contents)) {
    return ::testing::AssertionFailure()
           << "Couldn't read test file: " << file_path.value();
  }

  // Strip C++ style comments out of the "json" file, otherwise it cannot be
  // parsed.
  re2::RE2::GlobalReplace(&file_contents, re2::RE2("\\s*//.*"), "");

  // Parse the JSON to a dictionary.
  value->reset(base::JSONReader::Read(file_contents));
  if (!value->get()) {
    return ::testing::AssertionFailure()
           << "Couldn't parse test file JSON: " << file_path.value();
  }

  return ::testing::AssertionSuccess();
}

// Same as ReadJsonTestFile(), but return the value as a List.
::testing::AssertionResult ReadJsonTestFileToList(
    const char* test_file_name,
    scoped_ptr<base::ListValue>* list) {
  // Read the JSON.
  scoped_ptr<base::Value> json;
  ::testing::AssertionResult result = ReadJsonTestFile(test_file_name, &json);
  if (!result)
    return result;

  // Cast to an ListValue.
  base::ListValue* list_value = NULL;
  if (!json->GetAsList(&list_value) || !list_value)
    return ::testing::AssertionFailure() << "The JSON was not a list";

  list->reset(list_value);
  ignore_result(json.release());

  return ::testing::AssertionSuccess();
}

// Read a string property from the dictionary with path |property_name|
// (which can include periods for nested dictionaries). Interprets the
// string as a hex encoded string and converts it to a bytes list.
//
// Returns empty vector on failure.
std::vector<uint8> GetBytesFromHexString(base::DictionaryValue* dict,
                                         const char* property_name) {
  std::string hex_string;
  if (!dict->GetString(property_name, &hex_string)) {
    EXPECT_TRUE(false) << "Couldn't get string property: " << property_name;
    return std::vector<uint8>();
  }

  return HexStringToBytes(hex_string);
}

// Reads a string property with path "property_name" and converts it to a
// WebCryptoAlgorith. Returns null algorithm on failure.
blink::WebCryptoAlgorithm GetDigestAlgorithm(base::DictionaryValue* dict,
                                             const char* property_name) {
  std::string algorithm_name;
  if (!dict->GetString(property_name, &algorithm_name)) {
    EXPECT_TRUE(false) << "Couldn't get string property: " << property_name;
    return blink::WebCryptoAlgorithm::createNull();
  }

  struct {
    const char* name;
    blink::WebCryptoAlgorithmId id;
  } kDigestNameToId[] = {
        {"sha-1", blink::WebCryptoAlgorithmIdSha1},
        {"sha-256", blink::WebCryptoAlgorithmIdSha256},
        {"sha-384", blink::WebCryptoAlgorithmIdSha384},
        {"sha-512", blink::WebCryptoAlgorithmIdSha512},
    };

  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(kDigestNameToId); ++i) {
    if (kDigestNameToId[i].name == algorithm_name)
      return CreateAlgorithm(kDigestNameToId[i].id);
  }

  return blink::WebCryptoAlgorithm::createNull();
}

// Helper for ImportJwkFailures and ImportJwkOctFailures. Restores the JWK JSON
// dictionary to a good state
void RestoreJwkOctDictionary(base::DictionaryValue* dict) {
  dict->Clear();
  dict->SetString("kty", "oct");
  dict->SetString("alg", "A128CBC");
  dict->SetString("use", "enc");
  dict->SetBoolean("ext", false);
  dict->SetString("k", "GADWrMRHwQfoNaXU5fZvTg==");
}

// Helper for ImportJwkRsaFailures. Restores the JWK JSON
// dictionary to a good state
void RestoreJwkRsaDictionary(base::DictionaryValue* dict) {
  dict->Clear();
  dict->SetString("kty", "RSA");
  dict->SetString("alg", "RSA1_5");
  dict->SetString("use", "enc");
  dict->SetBoolean("ext", false);
  dict->SetString(
      "n",
      "qLOyhK-OtQs4cDSoYPFGxJGfMYdjzWxVmMiuSBGh4KvEx-CwgtaTpef87Wdc9GaFEncsDLxk"
      "p0LGxjD1M8jMcvYq6DPEC_JYQumEu3i9v5fAEH1VvbZi9cTg-rmEXLUUjvc5LdOq_5OuHmtm"
      "e7PUJHYW1PW6ENTP0ibeiNOfFvs");
  dict->SetString("e", "AQAB");
}

// Returns true if any of the vectors in the input list have identical content.
// Dumb O(n^2) implementation but should be fast enough for the input sizes that
// are used.
bool CopiesExist(const std::vector<std::vector<uint8> >& bufs) {
  for (size_t i = 0; i < bufs.size(); ++i) {
    for (size_t j = i + 1; j < bufs.size(); ++j) {
      if (CryptoData(bufs[i]) == CryptoData(bufs[j]))
        return true;
    }
  }
  return false;
}

blink::WebCryptoAlgorithm CreateAesKeyGenAlgorithm(
    blink::WebCryptoAlgorithmId aes_alg_id,
    unsigned short length) {
  return blink::WebCryptoAlgorithm::adoptParamsAndCreate(
      aes_alg_id, new blink::WebCryptoAesKeyGenParams(length));
}

blink::WebCryptoAlgorithm CreateAesCbcKeyGenAlgorithm(
    unsigned short key_length_bits) {
  return CreateAesKeyGenAlgorithm(blink::WebCryptoAlgorithmIdAesCbc,
                                  key_length_bits);
}

blink::WebCryptoAlgorithm CreateAesGcmKeyGenAlgorithm(
    unsigned short key_length_bits) {
  EXPECT_TRUE(SupportsAesGcm());
  return CreateAesKeyGenAlgorithm(blink::WebCryptoAlgorithmIdAesGcm,
                                  key_length_bits);
}

blink::WebCryptoAlgorithm CreateAesKwKeyGenAlgorithm(
    unsigned short key_length_bits) {
  return CreateAesKeyGenAlgorithm(blink::WebCryptoAlgorithmIdAesKw,
                                  key_length_bits);
}

// The following key pair is comprised of the SPKI (public key) and PKCS#8
// (private key) representations of the key pair provided in Example 1 of the
// NIST test vectors at
// ftp://ftp.rsa.com/pub/rsalabs/tmp/pkcs1v15sign-vectors.txt
const unsigned int kModulusLengthBits = 1024;
const char* const kPublicKeySpkiDerHex =
    "30819f300d06092a864886f70d010101050003818d0030818902818100a5"
    "6e4a0e701017589a5187dc7ea841d156f2ec0e36ad52a44dfeb1e61f7ad9"
    "91d8c51056ffedb162b4c0f283a12a88a394dff526ab7291cbb307ceabfc"
    "e0b1dfd5cd9508096d5b2b8b6df5d671ef6377c0921cb23c270a70e2598e"
    "6ff89d19f105acc2d3f0cb35f29280e1386b6f64c4ef22e1e1f20d0ce8cf"
    "fb2249bd9a21370203010001";
const char* const kPrivateKeyPkcs8DerHex =
    "30820275020100300d06092a864886f70d01010105000482025f3082025b"
    "02010002818100a56e4a0e701017589a5187dc7ea841d156f2ec0e36ad52"
    "a44dfeb1e61f7ad991d8c51056ffedb162b4c0f283a12a88a394dff526ab"
    "7291cbb307ceabfce0b1dfd5cd9508096d5b2b8b6df5d671ef6377c0921c"
    "b23c270a70e2598e6ff89d19f105acc2d3f0cb35f29280e1386b6f64c4ef"
    "22e1e1f20d0ce8cffb2249bd9a2137020301000102818033a5042a90b27d"
    "4f5451ca9bbbd0b44771a101af884340aef9885f2a4bbe92e894a724ac3c"
    "568c8f97853ad07c0266c8c6a3ca0929f1e8f11231884429fc4d9ae55fee"
    "896a10ce707c3ed7e734e44727a39574501a532683109c2abacaba283c31"
    "b4bd2f53c3ee37e352cee34f9e503bd80c0622ad79c6dcee883547c6a3b3"
    "25024100e7e8942720a877517273a356053ea2a1bc0c94aa72d55c6e8629"
    "6b2dfc967948c0a72cbccca7eacb35706e09a1df55a1535bd9b3cc34160b"
    "3b6dcd3eda8e6443024100b69dca1cf7d4d7ec81e75b90fcca874abcde12"
    "3fd2700180aa90479b6e48de8d67ed24f9f19d85ba275874f542cd20dc72"
    "3e6963364a1f9425452b269a6799fd024028fa13938655be1f8a159cbaca"
    "5a72ea190c30089e19cd274a556f36c4f6e19f554b34c077790427bbdd8d"
    "d3ede2448328f385d81b30e8e43b2fffa02786197902401a8b38f398fa71"
    "2049898d7fb79ee0a77668791299cdfa09efc0e507acb21ed74301ef5bfd"
    "48be455eaeb6e1678255827580a8e4e8e14151d1510a82a3f2e729024027"
    "156aba4126d24a81f3a528cbfb27f56886f840a9f6e86e17a44b94fe9319"
    "584b8e22fdde1e5a2e3bd8aa5ba8d8584194eb2190acf832b847f13a3d24"
    "a79f4d";

class SharedCryptoTest : public testing::Test {
 protected:
  virtual void SetUp() OVERRIDE { Init(); }
};

blink::WebCryptoKey ImportSecretKeyFromRaw(
    const std::vector<uint8>& key_raw,
    const blink::WebCryptoAlgorithm& algorithm,
    blink::WebCryptoKeyUsageMask usage) {
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  bool extractable = true;
  EXPECT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(key_raw),
                      algorithm,
                      extractable,
                      usage,
                      &key));

  EXPECT_FALSE(key.isNull());
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(algorithm.id(), key.algorithm().id());
  EXPECT_EQ(extractable, key.extractable());
  EXPECT_EQ(usage, key.usages());
  return key;
}

void ImportRsaKeyPair(const std::vector<uint8>& spki_der,
                      const std::vector<uint8>& pkcs8_der,
                      const blink::WebCryptoAlgorithm& algorithm,
                      bool extractable,
                      blink::WebCryptoKeyUsageMask usage_mask,
                      blink::WebCryptoKey* public_key,
                      blink::WebCryptoKey* private_key) {
  EXPECT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatSpki,
                      CryptoData(spki_der),
                      algorithm,
                      true,
                      usage_mask,
                      public_key));
  EXPECT_FALSE(public_key->isNull());
  EXPECT_TRUE(public_key->handle());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key->type());
  EXPECT_EQ(algorithm.id(), public_key->algorithm().id());
  EXPECT_EQ(extractable, extractable);
  EXPECT_EQ(usage_mask, public_key->usages());

  EXPECT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(pkcs8_der),
                      algorithm,
                      extractable,
                      usage_mask,
                      private_key));
  EXPECT_FALSE(private_key->isNull());
  EXPECT_TRUE(private_key->handle());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, private_key->type());
  EXPECT_EQ(algorithm.id(), private_key->algorithm().id());
  EXPECT_EQ(extractable, extractable);
  EXPECT_EQ(usage_mask, private_key->usages());
}

Status AesGcmEncrypt(const blink::WebCryptoKey& key,
                     const std::vector<uint8>& iv,
                     const std::vector<uint8>& additional_data,
                     unsigned int tag_length_bits,
                     const std::vector<uint8>& plain_text,
                     std::vector<uint8>* cipher_text,
                     std::vector<uint8>* authentication_tag) {
  EXPECT_TRUE(SupportsAesGcm());
  blink::WebCryptoAlgorithm algorithm =
      CreateAesGcmAlgorithm(iv, additional_data, tag_length_bits);

  std::vector<uint8> output;
  Status status = Encrypt(algorithm, key, CryptoData(plain_text), &output);
  if (status.IsError())
    return status;

  if ((tag_length_bits % 8) != 0) {
    EXPECT_TRUE(false) << "Encrypt should have failed.";
    return Status::OperationError();
  }

  size_t tag_length_bytes = tag_length_bits / 8;

  if (tag_length_bytes > output.size()) {
    EXPECT_TRUE(false) << "tag length is larger than output";
    return Status::OperationError();
  }

  // The encryption result is cipher text with authentication tag appended.
  cipher_text->assign(output.begin(),
                      output.begin() + (output.size() - tag_length_bytes));
  authentication_tag->assign(output.begin() + cipher_text->size(),
                             output.end());

  return Status::Success();
}

Status AesGcmDecrypt(const blink::WebCryptoKey& key,
                     const std::vector<uint8>& iv,
                     const std::vector<uint8>& additional_data,
                     unsigned int tag_length_bits,
                     const std::vector<uint8>& cipher_text,
                     const std::vector<uint8>& authentication_tag,
                     std::vector<uint8>* plain_text) {
  EXPECT_TRUE(SupportsAesGcm());
  blink::WebCryptoAlgorithm algorithm =
      CreateAesGcmAlgorithm(iv, additional_data, tag_length_bits);

  // Join cipher text and authentication tag.
  std::vector<uint8> cipher_text_with_tag;
  cipher_text_with_tag.reserve(cipher_text.size() + authentication_tag.size());
  cipher_text_with_tag.insert(
      cipher_text_with_tag.end(), cipher_text.begin(), cipher_text.end());
  cipher_text_with_tag.insert(cipher_text_with_tag.end(),
                              authentication_tag.begin(),
                              authentication_tag.end());

  return Decrypt(algorithm, key, CryptoData(cipher_text_with_tag), plain_text);
}

Status ImportKeyJwk(const CryptoData& key_data,
                    const blink::WebCryptoAlgorithm& algorithm,
                    bool extractable,
                    blink::WebCryptoKeyUsageMask usage_mask,
                    blink::WebCryptoKey* key) {
  return ImportKey(blink::WebCryptoKeyFormatJwk,
                   key_data,
                   algorithm,
                   extractable,
                   usage_mask,
                   key);
}

Status ImportKeyJwkFromDict(const base::DictionaryValue& dict,
                            const blink::WebCryptoAlgorithm& algorithm,
                            bool extractable,
                            blink::WebCryptoKeyUsageMask usage_mask,
                            blink::WebCryptoKey* key) {
  return ImportKeyJwk(CryptoData(MakeJsonVector(dict)),
                      algorithm,
                      extractable,
                      usage_mask,
                      key);
}

// Parses a vector of JSON into a dictionary.
scoped_ptr<base::DictionaryValue> GetJwkDictionary(
    const std::vector<uint8>& json) {
  base::StringPiece json_string(
      reinterpret_cast<const char*>(Uint8VectorStart(json)), json.size());
  base::Value* value = base::JSONReader::Read(json_string);
  EXPECT_TRUE(value);
  base::DictionaryValue* dict_value = NULL;
  value->GetAsDictionary(&dict_value);
  return scoped_ptr<base::DictionaryValue>(dict_value);
}

// Verifies the input dictionary contains the expected values. Exact matches are
// required on the fields examined.
::testing::AssertionResult VerifyJwk(
    const scoped_ptr<base::DictionaryValue>& dict,
    const std::string& kty_expected,
    const std::string& alg_expected,
    blink::WebCryptoKeyUsageMask use_mask_expected) {
  // ---- kty
  std::string value_string;
  if (!dict->GetString("kty", &value_string))
    return ::testing::AssertionFailure() << "Missing 'kty'";
  if (value_string != kty_expected)
    return ::testing::AssertionFailure() << "Expected 'kty' to be "
                                         << kty_expected << "but found "
                                         << value_string;

  // ---- alg
  if (!dict->GetString("alg", &value_string))
    return ::testing::AssertionFailure() << "Missing 'alg'";
  if (value_string != alg_expected)
    return ::testing::AssertionFailure() << "Expected 'alg' to be "
                                         << alg_expected << " but found "
                                         << value_string;

  // ---- ext
  // always expect ext == true in this case
  bool ext_value;
  if (!dict->GetBoolean("ext", &ext_value))
    return ::testing::AssertionFailure() << "Missing 'ext'";
  if (!ext_value)
    return ::testing::AssertionFailure()
           << "Expected 'ext' to be true but found false";

  // ---- key_ops
  base::ListValue* key_ops;
  if (!dict->GetList("key_ops", &key_ops))
    return ::testing::AssertionFailure() << "Missing 'key_ops'";
  blink::WebCryptoKeyUsageMask key_ops_mask = 0;
  Status status = GetWebCryptoUsagesFromJwkKeyOps(key_ops, &key_ops_mask);
  if (status.IsError())
    return ::testing::AssertionFailure() << "Failure extracting 'key_ops'";
  if (key_ops_mask != use_mask_expected)
    return ::testing::AssertionFailure()
           << "Expected 'key_ops' mask to be " << use_mask_expected
           << " but found " << key_ops_mask << " (" << value_string << ")";

  return ::testing::AssertionSuccess();
}

// Verifies that the JSON in the input vector contains the provided
// expected values. Exact matches are required on the fields examined.
::testing::AssertionResult VerifySecretJwk(
    const std::vector<uint8>& json,
    const std::string& alg_expected,
    const std::string& k_expected_hex,
    blink::WebCryptoKeyUsageMask use_mask_expected) {
  scoped_ptr<base::DictionaryValue> dict = GetJwkDictionary(json);
  if (!dict.get() || dict->empty())
    return ::testing::AssertionFailure() << "JSON parsing failed";

  // ---- k
  std::string value_string;
  if (!dict->GetString("k", &value_string))
    return ::testing::AssertionFailure() << "Missing 'k'";
  std::string k_value;
  if (!webcrypto::Base64DecodeUrlSafe(value_string, &k_value))
    return ::testing::AssertionFailure() << "Base64DecodeUrlSafe(k) failed";
  if (!LowerCaseEqualsASCII(base::HexEncode(k_value.data(), k_value.size()),
                            k_expected_hex.c_str())) {
    return ::testing::AssertionFailure() << "Expected 'k' to be "
                                         << k_expected_hex
                                         << " but found something different";
  }

  return VerifyJwk(dict, "oct", alg_expected, use_mask_expected);
}

// Verifies that the JSON in the input vector contains the provided
// expected values. Exact matches are required on the fields examined.
::testing::AssertionResult VerifyPublicJwk(
    const std::vector<uint8>& json,
    const std::string& alg_expected,
    const std::string& n_expected_hex,
    const std::string& e_expected_hex,
    blink::WebCryptoKeyUsageMask use_mask_expected) {
  scoped_ptr<base::DictionaryValue> dict = GetJwkDictionary(json);
  if (!dict.get() || dict->empty())
    return ::testing::AssertionFailure() << "JSON parsing failed";

  // ---- n
  std::string value_string;
  if (!dict->GetString("n", &value_string))
    return ::testing::AssertionFailure() << "Missing 'n'";
  std::string n_value;
  if (!webcrypto::Base64DecodeUrlSafe(value_string, &n_value))
    return ::testing::AssertionFailure() << "Base64DecodeUrlSafe(n) failed";
  if (base::HexEncode(n_value.data(), n_value.size()) != n_expected_hex) {
    return ::testing::AssertionFailure() << "'n' does not match the expected "
                                            "value";
  }
  // TODO(padolph): LowerCaseEqualsASCII() does not work for above!

  // ---- e
  if (!dict->GetString("e", &value_string))
    return ::testing::AssertionFailure() << "Missing 'e'";
  std::string e_value;
  if (!webcrypto::Base64DecodeUrlSafe(value_string, &e_value))
    return ::testing::AssertionFailure() << "Base64DecodeUrlSafe(e) failed";
  if (!LowerCaseEqualsASCII(base::HexEncode(e_value.data(), e_value.size()),
                            e_expected_hex.c_str())) {
    return ::testing::AssertionFailure() << "Expected 'e' to be "
                                         << e_expected_hex
                                         << " but found something different";
  }

  return VerifyJwk(dict, "RSA", alg_expected, use_mask_expected);
}

}  // namespace

TEST_F(SharedCryptoTest, CheckAesGcm) {
  if (!SupportsAesGcm()) {
    LOG(WARNING) << "AES GCM not supported on this platform, so some tests "
                    "will be skipped. Consider upgrading local NSS libraries";
    return;
  }
}

// Tests several Status objects against their expected hard coded values, as
// well as ensuring that comparison of Status objects works.
// Comparison should take into account both the error details, as well as the
// error type.
TEST_F(SharedCryptoTest, Status) {
  // Even though the error message is the same, these should not be considered
  // the same by the tests because the error type is different.
  EXPECT_NE(Status::DataError(), Status::OperationError());
  EXPECT_NE(Status::Success(), Status::OperationError());

  EXPECT_EQ(Status::Success(), Status::Success());
  EXPECT_EQ(Status::ErrorJwkPropertyWrongType("kty", "string"),
            Status::ErrorJwkPropertyWrongType("kty", "string"));

  Status status = Status::Success();

  EXPECT_FALSE(status.IsError());
  EXPECT_EQ("", status.error_details());

  status = Status::OperationError();
  EXPECT_TRUE(status.IsError());
  EXPECT_EQ("", status.error_details());
  EXPECT_EQ(blink::WebCryptoErrorTypeOperation, status.error_type());

  status = Status::DataError();
  EXPECT_TRUE(status.IsError());
  EXPECT_EQ("", status.error_details());
  EXPECT_EQ(blink::WebCryptoErrorTypeData, status.error_type());

  status = Status::ErrorUnsupported();
  EXPECT_TRUE(status.IsError());
  EXPECT_EQ("The requested operation is unsupported", status.error_details());
  EXPECT_EQ(blink::WebCryptoErrorTypeNotSupported, status.error_type());

  status = Status::ErrorJwkPropertyMissing("kty");
  EXPECT_TRUE(status.IsError());
  EXPECT_EQ("The required JWK property \"kty\" was missing",
            status.error_details());
  EXPECT_EQ(blink::WebCryptoErrorTypeData, status.error_type());

  status = Status::ErrorJwkPropertyWrongType("kty", "string");
  EXPECT_TRUE(status.IsError());
  EXPECT_EQ("The JWK property \"kty\" must be a string",
            status.error_details());
  EXPECT_EQ(blink::WebCryptoErrorTypeData, status.error_type());

  status = Status::ErrorJwkBase64Decode("n");
  EXPECT_TRUE(status.IsError());
  EXPECT_EQ("The JWK property \"n\" could not be base64 decoded",
            status.error_details());
  EXPECT_EQ(blink::WebCryptoErrorTypeData, status.error_type());
}

TEST_F(SharedCryptoTest, DigestSampleSets) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("digest.json", &tests));

  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);
    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));

    blink::WebCryptoAlgorithm test_algorithm =
        GetDigestAlgorithm(test, "algorithm");
    std::vector<uint8> test_input = GetBytesFromHexString(test, "input");
    std::vector<uint8> test_output = GetBytesFromHexString(test, "output");

    std::vector<uint8> output;
    ASSERT_EQ(Status::Success(),
              Digest(test_algorithm, CryptoData(test_input), &output));
    EXPECT_BYTES_EQ(test_output, output);
  }
}

TEST_F(SharedCryptoTest, DigestSampleSetsInChunks) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("digest.json", &tests));

  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);
    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));

    blink::WebCryptoAlgorithm test_algorithm =
        GetDigestAlgorithm(test, "algorithm");
    std::vector<uint8> test_input = GetBytesFromHexString(test, "input");
    std::vector<uint8> test_output = GetBytesFromHexString(test, "output");

    // Test the chunk version of the digest functions. Test with 129 byte chunks
    // because the SHA-512 chunk size is 128 bytes.
    unsigned char* output;
    unsigned int output_length;
    static const size_t kChunkSizeBytes = 129;
    size_t length = test_input.size();
    scoped_ptr<blink::WebCryptoDigestor> digestor(
        CreateDigestor(test_algorithm.id()));
    std::vector<uint8>::iterator begin = test_input.begin();
    size_t chunk_index = 0;
    while (begin != test_input.end()) {
      size_t chunk_length = std::min(kChunkSizeBytes, length - chunk_index);
      std::vector<uint8> chunk(begin, begin + chunk_length);
      ASSERT_TRUE(chunk.size() > 0);
      EXPECT_TRUE(digestor->consume(&chunk.front(), chunk.size()));
      chunk_index = chunk_index + chunk_length;
      begin = begin + chunk_length;
    }
    EXPECT_TRUE(digestor->finish(output, output_length));
    EXPECT_BYTES_EQ(test_output, CryptoData(output, output_length));
  }
}

TEST_F(SharedCryptoTest, HMACSampleSets) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("hmac.json", &tests));
  // TODO(padolph): Missing known answer tests for HMAC SHA384, and SHA512.
  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);
    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));

    blink::WebCryptoAlgorithm test_hash = GetDigestAlgorithm(test, "hash");
    const std::vector<uint8> test_key = GetBytesFromHexString(test, "key");
    const std::vector<uint8> test_message =
        GetBytesFromHexString(test, "message");
    const std::vector<uint8> test_mac = GetBytesFromHexString(test, "mac");

    blink::WebCryptoAlgorithm algorithm =
        CreateAlgorithm(blink::WebCryptoAlgorithmIdHmac);

    blink::WebCryptoAlgorithm importAlgorithm =
        CreateHmacImportAlgorithm(test_hash.id());

    blink::WebCryptoKey key = ImportSecretKeyFromRaw(
        test_key,
        importAlgorithm,
        blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify);

    EXPECT_EQ(test_hash.id(), key.algorithm().hmacParams()->hash().id());
    EXPECT_EQ(test_key.size() * 8, key.algorithm().hmacParams()->lengthBits());

    // Verify exported raw key is identical to the imported data
    std::vector<uint8> raw_key;
    EXPECT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));
    EXPECT_BYTES_EQ(test_key, raw_key);

    std::vector<uint8> output;

    ASSERT_EQ(Status::Success(),
              Sign(algorithm, key, CryptoData(test_message), &output));

    EXPECT_BYTES_EQ(test_mac, output);

    bool signature_match = false;
    EXPECT_EQ(Status::Success(),
              VerifySignature(algorithm,
                              key,
                              CryptoData(output),
                              CryptoData(test_message),
                              &signature_match));
    EXPECT_TRUE(signature_match);

    // Ensure truncated signature does not verify by passing one less byte.
    EXPECT_EQ(
        Status::Success(),
        VerifySignature(algorithm,
                        key,
                        CryptoData(Uint8VectorStart(output), output.size() - 1),
                        CryptoData(test_message),
                        &signature_match));
    EXPECT_FALSE(signature_match);

    // Ensure truncated signature does not verify by passing no bytes.
    EXPECT_EQ(Status::Success(),
              VerifySignature(algorithm,
                              key,
                              CryptoData(),
                              CryptoData(test_message),
                              &signature_match));
    EXPECT_FALSE(signature_match);

    // Ensure extra long signature does not cause issues and fails.
    const unsigned char kLongSignature[1024] = {0};
    EXPECT_EQ(
        Status::Success(),
        VerifySignature(algorithm,
                        key,
                        CryptoData(kLongSignature, sizeof(kLongSignature)),
                        CryptoData(test_message),
                        &signature_match));
    EXPECT_FALSE(signature_match);
  }
}

TEST_F(SharedCryptoTest, AesCbcFailures) {
  const std::string key_hex = "2b7e151628aed2a6abf7158809cf4f3c";
  blink::WebCryptoKey key = ImportSecretKeyFromRaw(
      HexStringToBytes(key_hex),
      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
      blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt);

  // Verify exported raw key is identical to the imported data
  std::vector<uint8> raw_key;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));
  EXPECT_BYTES_EQ_HEX(key_hex, raw_key);

  std::vector<uint8> output;

  // Use an invalid |iv| (fewer than 16 bytes)
  {
    std::vector<uint8> input(32);
    std::vector<uint8> iv;
    EXPECT_EQ(Status::ErrorIncorrectSizeAesCbcIv(),
              Encrypt(webcrypto::CreateAesCbcAlgorithm(iv),
                      key,
                      CryptoData(input),
                      &output));
    EXPECT_EQ(Status::ErrorIncorrectSizeAesCbcIv(),
              Decrypt(webcrypto::CreateAesCbcAlgorithm(iv),
                      key,
                      CryptoData(input),
                      &output));
  }

  // Use an invalid |iv| (more than 16 bytes)
  {
    std::vector<uint8> input(32);
    std::vector<uint8> iv(17);
    EXPECT_EQ(Status::ErrorIncorrectSizeAesCbcIv(),
              Encrypt(webcrypto::CreateAesCbcAlgorithm(iv),
                      key,
                      CryptoData(input),
                      &output));
    EXPECT_EQ(Status::ErrorIncorrectSizeAesCbcIv(),
              Decrypt(webcrypto::CreateAesCbcAlgorithm(iv),
                      key,
                      CryptoData(input),
                      &output));
  }

  // Give an input that is too large (would cause integer overflow when
  // narrowing to an int).
  {
    std::vector<uint8> iv(16);

    // Pretend the input is large. Don't pass data pointer as NULL in case that
    // is special cased; the implementation shouldn't actually dereference the
    // data.
    CryptoData input(&iv[0], INT_MAX - 3);

    EXPECT_EQ(Status::ErrorDataTooLarge(),
              Encrypt(CreateAesCbcAlgorithm(iv), key, input, &output));
    EXPECT_EQ(Status::ErrorDataTooLarge(),
              Decrypt(CreateAesCbcAlgorithm(iv), key, input, &output));
  }

  // Fail importing the key (too few bytes specified)
  {
    std::vector<uint8> key_raw(1);
    std::vector<uint8> iv(16);

    blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
    EXPECT_EQ(Status::ErrorImportAesKeyLength(),
              ImportKey(blink::WebCryptoKeyFormatRaw,
                        CryptoData(key_raw),
                        CreateAesCbcAlgorithm(iv),
                        true,
                        blink::WebCryptoKeyUsageEncrypt,
                        &key));
  }

  // Fail exporting the key in SPKI and PKCS#8 formats (not allowed for secret
  // keys).
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            ExportKey(blink::WebCryptoKeyFormatSpki, key, &output));
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            ExportKey(blink::WebCryptoKeyFormatPkcs8, key, &output));
}

TEST_F(SharedCryptoTest, MAYBE(AesCbcSampleSets)) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_cbc.json", &tests));

  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);
    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));

    std::vector<uint8> test_key = GetBytesFromHexString(test, "key");
    std::vector<uint8> test_iv = GetBytesFromHexString(test, "iv");
    std::vector<uint8> test_plain_text =
        GetBytesFromHexString(test, "plain_text");
    std::vector<uint8> test_cipher_text =
        GetBytesFromHexString(test, "cipher_text");

    blink::WebCryptoKey key = ImportSecretKeyFromRaw(
        test_key,
        CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
        blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt);

    EXPECT_EQ(test_key.size() * 8, key.algorithm().aesParams()->lengthBits());

    // Verify exported raw key is identical to the imported data
    std::vector<uint8> raw_key;
    EXPECT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));
    EXPECT_BYTES_EQ(test_key, raw_key);

    std::vector<uint8> output;

    // Test encryption.
    EXPECT_EQ(Status::Success(),
              Encrypt(webcrypto::CreateAesCbcAlgorithm(test_iv),
                      key,
                      CryptoData(test_plain_text),
                      &output));
    EXPECT_BYTES_EQ(test_cipher_text, output);

    // Test decryption.
    EXPECT_EQ(Status::Success(),
              Decrypt(webcrypto::CreateAesCbcAlgorithm(test_iv),
                      key,
                      CryptoData(test_cipher_text),
                      &output));
    EXPECT_BYTES_EQ(test_plain_text, output);

    const unsigned int kAesCbcBlockSize = 16;

    // Decrypt with a padding error by stripping the last block. This also ends
    // up testing decryption over empty cipher text.
    if (test_cipher_text.size() >= kAesCbcBlockSize) {
      EXPECT_EQ(Status::OperationError(),
                Decrypt(CreateAesCbcAlgorithm(test_iv),
                        key,
                        CryptoData(&test_cipher_text[0],
                                   test_cipher_text.size() - kAesCbcBlockSize),
                        &output));
    }

    // Decrypt cipher text which is not a multiple of block size by stripping
    // a few bytes off the cipher text.
    if (test_cipher_text.size() > 3) {
      EXPECT_EQ(
          Status::OperationError(),
          Decrypt(CreateAesCbcAlgorithm(test_iv),
                  key,
                  CryptoData(&test_cipher_text[0], test_cipher_text.size() - 3),
                  &output));
    }
  }
}

TEST_F(SharedCryptoTest, MAYBE(GenerateKeyAes)) {
  // Check key generation for each of AES-CBC, AES-GCM, and AES-KW, and for each
  // allowed key length.
  std::vector<blink::WebCryptoAlgorithm> algorithm;
  const unsigned short kKeyLength[] = {128, 192, 256};
  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(kKeyLength); ++i) {
    algorithm.push_back(CreateAesCbcKeyGenAlgorithm(kKeyLength[i]));
    algorithm.push_back(CreateAesKwKeyGenAlgorithm(kKeyLength[i]));
    if (SupportsAesGcm())
      algorithm.push_back(CreateAesGcmKeyGenAlgorithm(kKeyLength[i]));
  }
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  std::vector<std::vector<uint8> > keys;
  std::vector<uint8> key_bytes;
  for (size_t i = 0; i < algorithm.size(); ++i) {
    SCOPED_TRACE(i);
    // Generate a small sample of keys.
    keys.clear();
    for (int j = 0; j < 16; ++j) {
      ASSERT_EQ(Status::Success(),
                GenerateSecretKey(algorithm[i], true, 0, &key));
      EXPECT_TRUE(key.handle());
      EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
      ASSERT_EQ(Status::Success(),
                ExportKey(blink::WebCryptoKeyFormatRaw, key, &key_bytes));
      EXPECT_EQ(key_bytes.size() * 8,
                key.algorithm().aesParams()->lengthBits());
      keys.push_back(key_bytes);
    }
    // Ensure all entries in the key sample set are unique. This is a simplistic
    // estimate of whether the generated keys appear random.
    EXPECT_FALSE(CopiesExist(keys));
  }
}

TEST_F(SharedCryptoTest, MAYBE(GenerateKeyAesBadLength)) {
  const unsigned short kKeyLen[] = {0, 127, 257};
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(kKeyLen); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(Status::ErrorGenerateKeyLength(),
              GenerateSecretKey(
                  CreateAesCbcKeyGenAlgorithm(kKeyLen[i]), true, 0, &key));
    EXPECT_EQ(Status::ErrorGenerateKeyLength(),
              GenerateSecretKey(
                  CreateAesKwKeyGenAlgorithm(kKeyLen[i]), true, 0, &key));
    if (SupportsAesGcm()) {
      EXPECT_EQ(Status::ErrorGenerateKeyLength(),
                GenerateSecretKey(
                    CreateAesGcmKeyGenAlgorithm(kKeyLen[i]), true, 0, &key));
    }
  }
}

TEST_F(SharedCryptoTest, MAYBE(GenerateKeyHmac)) {
  // Generate a small sample of HMAC keys.
  std::vector<std::vector<uint8> > keys;
  for (int i = 0; i < 16; ++i) {
    std::vector<uint8> key_bytes;
    blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
    blink::WebCryptoAlgorithm algorithm =
        CreateHmacKeyGenAlgorithm(blink::WebCryptoAlgorithmIdSha1, 512);
    ASSERT_EQ(Status::Success(), GenerateSecretKey(algorithm, true, 0, &key));
    EXPECT_FALSE(key.isNull());
    EXPECT_TRUE(key.handle());
    EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
    EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, key.algorithm().id());
    EXPECT_EQ(blink::WebCryptoAlgorithmIdSha1,
              key.algorithm().hmacParams()->hash().id());
    EXPECT_EQ(512u, key.algorithm().hmacParams()->lengthBits());

    std::vector<uint8> raw_key;
    ASSERT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));
    EXPECT_EQ(64U, raw_key.size());
    keys.push_back(raw_key);
  }
  // Ensure all entries in the key sample set are unique. This is a simplistic
  // estimate of whether the generated keys appear random.
  EXPECT_FALSE(CopiesExist(keys));
}

// If the key length is not provided, then the block size is used.
TEST_F(SharedCryptoTest, MAYBE(GenerateKeyHmacNoLength)) {
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  blink::WebCryptoAlgorithm algorithm =
      CreateHmacKeyGenAlgorithm(blink::WebCryptoAlgorithmIdSha1, 0);
  ASSERT_EQ(Status::Success(), GenerateSecretKey(algorithm, true, 0, &key));
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, key.algorithm().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha1,
            key.algorithm().hmacParams()->hash().id());
  EXPECT_EQ(512u, key.algorithm().hmacParams()->lengthBits());
  std::vector<uint8> raw_key;
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));
  EXPECT_EQ(64U, raw_key.size());

  // The block size for HMAC SHA-512 is larger.
  algorithm = CreateHmacKeyGenAlgorithm(blink::WebCryptoAlgorithmIdSha512, 0);
  ASSERT_EQ(Status::Success(), GenerateSecretKey(algorithm, true, 0, &key));
  EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, key.algorithm().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha512,
            key.algorithm().hmacParams()->hash().id());
  EXPECT_EQ(1024u, key.algorithm().hmacParams()->lengthBits());
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));
  EXPECT_EQ(128U, raw_key.size());
}

TEST_F(SharedCryptoTest, ImportJwkKeyUsage) {
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  base::DictionaryValue dict;
  dict.SetString("kty", "oct");
  dict.SetBoolean("ext", false);
  dict.SetString("k", "GADWrMRHwQfoNaXU5fZvTg==");
  const blink::WebCryptoAlgorithm aes_cbc_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc);
  const blink::WebCryptoAlgorithm hmac_algorithm =
      webcrypto::CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256);
  const blink::WebCryptoAlgorithm aes_kw_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);

  // Test null usage.
  base::ListValue* key_ops = new base::ListValue;
  // Note: the following call makes dict assume ownership of key_ops.
  dict.Set("key_ops", key_ops);
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict, aes_cbc_algorithm, false, 0, &key));
  EXPECT_EQ(0, key.usages());

  // Test each key_ops value translates to the correct Web Crypto value.
  struct TestCase {
    const char* jwk_key_op;
    const char* jwk_alg;
    const blink::WebCryptoAlgorithm algorithm;
    const blink::WebCryptoKeyUsage usage;
  };
  // TODO(padolph): Add 'deriveBits' key_ops value once it is supported.
  const TestCase test_case[] = {
      {"encrypt", "A128CBC", aes_cbc_algorithm,
       blink::WebCryptoKeyUsageEncrypt},
      {"decrypt", "A128CBC", aes_cbc_algorithm,
       blink::WebCryptoKeyUsageDecrypt},
      {"sign", "HS256", hmac_algorithm, blink::WebCryptoKeyUsageSign},
      {"verify", "HS256", hmac_algorithm, blink::WebCryptoKeyUsageVerify},
      {"wrapKey", "A128KW", aes_kw_algorithm, blink::WebCryptoKeyUsageWrapKey},
      {"unwrapKey", "A128KW", aes_kw_algorithm,
       blink::WebCryptoKeyUsageUnwrapKey},
      {"deriveKey", "HS256", hmac_algorithm,
       blink::WebCryptoKeyUsageDeriveKey}};
  for (size_t test_index = 0; test_index < ARRAYSIZE_UNSAFE(test_case);
       ++test_index) {
    SCOPED_TRACE(test_index);
    dict.SetString("alg", test_case[test_index].jwk_alg);
    key_ops->Clear();
    key_ops->AppendString(test_case[test_index].jwk_key_op);
    EXPECT_EQ(Status::Success(),
              ImportKeyJwkFromDict(dict,
                                   test_case[test_index].algorithm,
                                   false,
                                   test_case[test_index].usage,
                                   &key));
    EXPECT_EQ(test_case[test_index].usage, key.usages());
  }

  // Test discrete multiple usages.
  dict.SetString("alg", "A128CBC");
  key_ops->Clear();
  key_ops->AppendString("encrypt");
  key_ops->AppendString("decrypt");
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict,
                                 aes_cbc_algorithm,
                                 false,
                                 blink::WebCryptoKeyUsageDecrypt |
                                     blink::WebCryptoKeyUsageEncrypt,
                                 &key));
  EXPECT_EQ(blink::WebCryptoKeyUsageDecrypt | blink::WebCryptoKeyUsageEncrypt,
            key.usages());

  // Test constrained key usage (input usage is a subset of JWK usage).
  key_ops->Clear();
  key_ops->AppendString("encrypt");
  key_ops->AppendString("decrypt");
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict,
                                 aes_cbc_algorithm,
                                 false,
                                 blink::WebCryptoKeyUsageDecrypt,
                                 &key));
  EXPECT_EQ(blink::WebCryptoKeyUsageDecrypt, key.usages());

  // Test failure if input usage is NOT a strict subset of the JWK usage.
  key_ops->Clear();
  key_ops->AppendString("encrypt");
  EXPECT_EQ(Status::ErrorJwkKeyopsInconsistent(),
            ImportKeyJwkFromDict(dict,
                                 aes_cbc_algorithm,
                                 false,
                                 blink::WebCryptoKeyUsageEncrypt |
                                     blink::WebCryptoKeyUsageDecrypt,
                                 &key));

  // Test 'use' inconsistent with 'key_ops'.
  dict.SetString("alg", "HS256");
  dict.SetString("use", "sig");
  key_ops->AppendString("sign");
  key_ops->AppendString("verify");
  key_ops->AppendString("encrypt");
  EXPECT_EQ(Status::ErrorJwkUseAndKeyopsInconsistent(),
            ImportKeyJwkFromDict(
                dict,
                hmac_algorithm,
                false,
                blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify,
                &key));

  // Test JWK composite 'sig' use
  dict.Remove("key_ops", NULL);
  dict.SetString("use", "sig");
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(
                dict,
                hmac_algorithm,
                false,
                blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify,
                &key));
  EXPECT_EQ(blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify,
            key.usages());

  // Test JWK composite use 'enc' usage
  dict.SetString("alg", "A128CBC");
  dict.SetString("use", "enc");
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict,
                                 aes_cbc_algorithm,
                                 false,
                                 blink::WebCryptoKeyUsageDecrypt |
                                     blink::WebCryptoKeyUsageEncrypt |
                                     blink::WebCryptoKeyUsageWrapKey |
                                     blink::WebCryptoKeyUsageUnwrapKey |
                                     blink::WebCryptoKeyUsageDeriveKey,
                                 &key));
  EXPECT_EQ(blink::WebCryptoKeyUsageDecrypt | blink::WebCryptoKeyUsageEncrypt |
                blink::WebCryptoKeyUsageWrapKey |
                blink::WebCryptoKeyUsageUnwrapKey |
                blink::WebCryptoKeyUsageDeriveKey,
            key.usages());
}

TEST_F(SharedCryptoTest, ImportJwkFailures) {
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc);
  blink::WebCryptoKeyUsageMask usage_mask = blink::WebCryptoKeyUsageEncrypt;

  // Baseline pass: each test below breaks a single item, so we start with a
  // passing case to make sure each failure is caused by the isolated break.
  // Each breaking subtest below resets the dictionary to this passing case when
  // complete.
  base::DictionaryValue dict;
  RestoreJwkOctDictionary(&dict);
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));

  // Fail on empty JSON.
  EXPECT_EQ(
      Status::ErrorImportEmptyKeyData(),
      ImportKeyJwk(
          CryptoData(MakeJsonVector("")), algorithm, false, usage_mask, &key));

  // Fail on invalid JSON.
  const std::vector<uint8> bad_json_vec = MakeJsonVector(
      "{"
      "\"kty\"         : \"oct\","
      "\"alg\"         : \"HS256\","
      "\"use\"         : ");
  EXPECT_EQ(Status::ErrorJwkNotDictionary(),
            ImportKeyJwk(
                CryptoData(bad_json_vec), algorithm, false, usage_mask, &key));

  // Fail on JWK alg present but unrecognized.
  dict.SetString("alg", "A127CBC");
  EXPECT_EQ(Status::ErrorJwkUnrecognizedAlgorithm(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on invalid kty.
  dict.SetString("kty", "foo");
  EXPECT_EQ(Status::ErrorJwkUnrecognizedKty(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on missing kty.
  dict.Remove("kty", NULL);
  EXPECT_EQ(Status::ErrorJwkPropertyMissing("kty"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on kty wrong type.
  dict.SetDouble("kty", 0.1);
  EXPECT_EQ(Status::ErrorJwkPropertyWrongType("kty", "string"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on invalid use.
  dict.SetString("use", "foo");
  EXPECT_EQ(Status::ErrorJwkUnrecognizedUse(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on invalid use (wrong type).
  dict.SetBoolean("use", true);
  EXPECT_EQ(Status::ErrorJwkPropertyWrongType("use", "string"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on invalid extractable (wrong type).
  dict.SetInteger("ext", 0);
  EXPECT_EQ(Status::ErrorJwkPropertyWrongType("ext", "boolean"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on invalid key_ops (wrong type).
  dict.SetBoolean("key_ops", true);
  EXPECT_EQ(Status::ErrorJwkPropertyWrongType("key_ops", "list"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on invalid key_ops (wrong element value).
  base::ListValue* key_ops = new base::ListValue;
  // Note: the following call makes dict assume ownership of key_ops.
  dict.Set("key_ops", key_ops);
  key_ops->AppendString("foo");
  EXPECT_EQ(Status::ErrorJwkUnrecognizedKeyop(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);
}

TEST_F(SharedCryptoTest, ImportJwkOctFailures) {
  base::DictionaryValue dict;
  RestoreJwkOctDictionary(&dict);
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc);
  blink::WebCryptoKeyUsageMask usage_mask = blink::WebCryptoKeyUsageEncrypt;
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();

  // Baseline pass.
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  EXPECT_EQ(algorithm.id(), key.algorithm().id());
  EXPECT_FALSE(key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageEncrypt, key.usages());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());

  // The following are specific failure cases for when kty = "oct".

  // Fail on missing k.
  dict.Remove("k", NULL);
  EXPECT_EQ(Status::ErrorJwkPropertyMissing("k"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on bad b64 encoding for k.
  dict.SetString("k", "Qk3f0DsytU8lfza2au #$% Htaw2xpop9GYyTuH0p5GghxTI=");
  EXPECT_EQ(Status::ErrorJwkBase64Decode("k"),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on empty k.
  dict.SetString("k", "");
  EXPECT_EQ(Status::ErrorJwkIncorrectKeyLength(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on k actual length (120 bits) inconsistent with the embedded JWK alg
  // value (128) for an AES key.
  dict.SetString("k", "AVj42h0Y5aqGtE3yluKL");
  EXPECT_EQ(Status::ErrorJwkIncorrectKeyLength(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);

  // Fail on k actual length (192 bits) inconsistent with the embedded JWK alg
  // value (128) for an AES key.
  dict.SetString("k", "dGhpcyAgaXMgIDI0ICBieXRlcyBsb25n");
  EXPECT_EQ(Status::ErrorJwkIncorrectKeyLength(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkOctDictionary(&dict);
}

TEST_F(SharedCryptoTest, MAYBE(ImportExportJwkRsaPublicKey)) {
  // This test uses kPublicKeySpkiDerHex as the RSA key. The data below
  // represents the modulus and public exponent extracted from this SPKI blob.
  // These values appear explicitly in the JWK rendering of the key.
  const std::string n_hex =
      "A56E4A0E701017589A5187DC7EA841D156F2EC0E36AD52A44DFEB1E61F7AD991D8C51056"
      "FFEDB162B4C0F283A12A88A394DFF526AB7291CBB307CEABFCE0B1DFD5CD9508096D5B2B"
      "8B6DF5D671EF6377C0921CB23C270A70E2598E6FF89D19F105ACC2D3F0CB35F29280E138"
      "6B6F64C4EF22E1E1F20D0CE8CFFB2249BD9A2137";
  const std::string e_hex = "010001";

  struct TestCase {
    const blink::WebCryptoAlgorithm algorithm;
    const blink::WebCryptoKeyUsageMask usage;
    const char* const jwk_alg;
  };
  const TestCase kTests[] = {
      // RSAES-PKCS1-v1_5
      {CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
       blink::WebCryptoKeyUsageEncrypt, "RSA1_5"},
      // RSASSA-PKCS1-v1_5 SHA-1
      {CreateRsaHashedImportAlgorithm(
           blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
           blink::WebCryptoAlgorithmIdSha1),
       blink::WebCryptoKeyUsageSign, "RS1"},
      // RSASSA-PKCS1-v1_5 SHA-256
      {CreateRsaHashedImportAlgorithm(
           blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
           blink::WebCryptoAlgorithmIdSha256),
       blink::WebCryptoKeyUsageSign, "RS256"},
      // RSASSA-PKCS1-v1_5 SHA-384
      {CreateRsaHashedImportAlgorithm(
           blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
           blink::WebCryptoAlgorithmIdSha384),
       blink::WebCryptoKeyUsageSign, "RS384"},
      // RSASSA-PKCS1-v1_5 SHA-512
      {CreateRsaHashedImportAlgorithm(
           blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
           blink::WebCryptoAlgorithmIdSha512),
       blink::WebCryptoKeyUsageSign, "RS512"}};

  for (size_t test_index = 0; test_index < ARRAYSIZE_UNSAFE(kTests);
       ++test_index) {
    SCOPED_TRACE(test_index);
    const TestCase& test = kTests[test_index];

    // Import the spki to create a public key
    blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
    ASSERT_EQ(Status::Success(),
              ImportKey(blink::WebCryptoKeyFormatSpki,
                        CryptoData(HexStringToBytes(kPublicKeySpkiDerHex)),
                        test.algorithm,
                        true,
                        test.usage,
                        &public_key));

    // Export the public key as JWK and verify its contents
    std::vector<uint8> jwk;
    ASSERT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatJwk, public_key, &jwk));
    EXPECT_TRUE(VerifyPublicJwk(jwk, test.jwk_alg, n_hex, e_hex, test.usage));

    // Import the JWK back in to create a new key
    blink::WebCryptoKey public_key2 = blink::WebCryptoKey::createNull();
    EXPECT_EQ(
        Status::Success(),
        ImportKeyJwk(
            CryptoData(jwk), test.algorithm, true, test.usage, &public_key2));
    EXPECT_TRUE(public_key2.handle());
    EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key2.type());
    EXPECT_EQ(true, public_key2.extractable());
    EXPECT_EQ(test.algorithm.id(), public_key2.algorithm().id());

    // Export the new key as spki and compare to the original.
    std::vector<uint8> spki;
    ASSERT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatSpki, public_key2, &spki));
    EXPECT_BYTES_EQ_HEX(kPublicKeySpkiDerHex, CryptoData(spki));
  }
}

TEST_F(SharedCryptoTest, MAYBE(ImportJwkRsaFailures)) {
  base::DictionaryValue dict;
  RestoreJwkRsaDictionary(&dict);
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKeyUsageMask usage_mask = blink::WebCryptoKeyUsageEncrypt;
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();

  // An RSA public key JWK _must_ have an "n" (modulus) and an "e" (exponent)
  // entry, while an RSA private key must have those plus at least a "d"
  // (private exponent) entry.
  // See http://tools.ietf.org/html/draft-ietf-jose-json-web-algorithms-18,
  // section 6.3.

  // Baseline pass.
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  EXPECT_EQ(algorithm.id(), key.algorithm().id());
  EXPECT_FALSE(key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageEncrypt, key.usages());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, key.type());

  // The following are specific failure cases for when kty = "RSA".

  // Fail if either "n" or "e" is not present or malformed.
  const std::string kKtyParmName[] = {"n", "e"};
  for (size_t idx = 0; idx < ARRAYSIZE_UNSAFE(kKtyParmName); ++idx) {
    // Fail on missing parameter.
    dict.Remove(kKtyParmName[idx], NULL);
    EXPECT_NE(Status::Success(),
              ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
    RestoreJwkRsaDictionary(&dict);

    // Fail on bad b64 parameter encoding.
    dict.SetString(kKtyParmName[idx], "Qk3f0DsytU8lfza2au #$% Htaw2xpop9yTuH0");
    EXPECT_NE(Status::Success(),
              ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
    RestoreJwkRsaDictionary(&dict);

    // Fail on empty parameter.
    dict.SetString(kKtyParmName[idx], "");
    EXPECT_NE(Status::Success(),
              ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
    RestoreJwkRsaDictionary(&dict);
  }

  // Fail if "d" parameter is present, implying the JWK is a private key, which
  // is not supported.
  dict.SetString("d", "Qk3f0Dsyt");
  EXPECT_EQ(Status::ErrorJwkRsaPrivateKeyUnsupported(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  RestoreJwkRsaDictionary(&dict);
}

TEST_F(SharedCryptoTest, MAYBE(ImportJwkInputConsistency)) {
  // The Web Crypto spec says that if a JWK value is present, but is
  // inconsistent with the input value, the operation must fail.

  // Consistency rules when JWK value is not present: Inputs should be used.
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  bool extractable = false;
  blink::WebCryptoAlgorithm algorithm =
      CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256);
  blink::WebCryptoKeyUsageMask usage_mask = blink::WebCryptoKeyUsageVerify;
  base::DictionaryValue dict;
  dict.SetString("kty", "oct");
  dict.SetString("k", "l3nZEgZCeX8XRwJdWyK3rGB8qwjhdY8vOkbIvh4lxTuMao9Y_--hdg");
  std::vector<uint8> json_vec = MakeJsonVector(dict);
  EXPECT_EQ(
      Status::Success(),
      ImportKeyJwk(
          CryptoData(json_vec), algorithm, extractable, usage_mask, &key));
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(extractable, key.extractable());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, key.algorithm().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha256,
            key.algorithm().hmacParams()->hash().id());
  EXPECT_EQ(320u, key.algorithm().hmacParams()->lengthBits());
  EXPECT_EQ(blink::WebCryptoKeyUsageVerify, key.usages());
  key = blink::WebCryptoKey::createNull();

  // Consistency rules when JWK value exists: Fail if inconsistency is found.

  // Pass: All input values are consistent with the JWK values.
  dict.Clear();
  dict.SetString("kty", "oct");
  dict.SetString("alg", "HS256");
  dict.SetString("use", "sig");
  dict.SetBoolean("ext", false);
  dict.SetString("k", "l3nZEgZCeX8XRwJdWyK3rGB8qwjhdY8vOkbIvh4lxTuMao9Y_--hdg");
  json_vec = MakeJsonVector(dict);
  EXPECT_EQ(
      Status::Success(),
      ImportKeyJwk(
          CryptoData(json_vec), algorithm, extractable, usage_mask, &key));

  // Extractable cases:
  // 1. input=T, JWK=F ==> fail (inconsistent)
  // 4. input=F, JWK=F ==> pass, result extractable is F
  // 2. input=T, JWK=T ==> pass, result extractable is T
  // 3. input=F, JWK=T ==> pass, result extractable is F
  EXPECT_EQ(
      Status::ErrorJwkExtInconsistent(),
      ImportKeyJwk(CryptoData(json_vec), algorithm, true, usage_mask, &key));
  EXPECT_EQ(
      Status::Success(),
      ImportKeyJwk(CryptoData(json_vec), algorithm, false, usage_mask, &key));
  EXPECT_FALSE(key.extractable());
  dict.SetBoolean("ext", true);
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict, algorithm, true, usage_mask, &key));
  EXPECT_TRUE(key.extractable());
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(dict, algorithm, false, usage_mask, &key));
  EXPECT_FALSE(key.extractable());
  dict.SetBoolean("ext", true);  // restore previous value

  // Fail: Input algorithm (AES-CBC) is inconsistent with JWK value
  // (HMAC SHA256).
  EXPECT_EQ(Status::ErrorJwkAlgorithmInconsistent(),
            ImportKeyJwk(CryptoData(json_vec),
                         CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                         extractable,
                         usage_mask,
                         &key));

  // Fail: Input algorithm (HMAC SHA1) is inconsistent with JWK value
  // (HMAC SHA256).
  EXPECT_EQ(
      Status::ErrorJwkAlgorithmInconsistent(),
      ImportKeyJwk(CryptoData(json_vec),
                   CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha1),
                   extractable,
                   usage_mask,
                   &key));

  // Pass: JWK alg missing but input algorithm specified: use input value
  dict.Remove("alg", NULL);
  EXPECT_EQ(Status::Success(),
            ImportKeyJwkFromDict(
                dict,
                CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256),
                extractable,
                usage_mask,
                &key));
  EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, algorithm.id());
  dict.SetString("alg", "HS256");

  // Fail: Input usage_mask (encrypt) is not a subset of the JWK value
  // (sign|verify)
  EXPECT_EQ(Status::ErrorJwkUseInconsistent(),
            ImportKeyJwk(CryptoData(json_vec),
                         algorithm,
                         extractable,
                         blink::WebCryptoKeyUsageEncrypt,
                         &key));

  // Fail: Input usage_mask (encrypt|sign|verify) is not a subset of the JWK
  // value (sign|verify)
  usage_mask = blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageSign |
               blink::WebCryptoKeyUsageVerify;
  EXPECT_EQ(
      Status::ErrorJwkUseInconsistent(),
      ImportKeyJwk(
          CryptoData(json_vec), algorithm, extractable, usage_mask, &key));

  // TODO(padolph): kty vs alg consistency tests: Depending on the kty value,
  // only certain alg values are permitted. For example, when kty = "RSA" alg
  // must be of the RSA family, or when kty = "oct" alg must be symmetric
  // algorithm.

  // TODO(padolph): key_ops consistency tests
}

TEST_F(SharedCryptoTest, MAYBE(ImportJwkHappy)) {
  // This test verifies the happy path of JWK import, including the application
  // of the imported key material.

  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  bool extractable = false;
  blink::WebCryptoAlgorithm algorithm =
      CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256);
  blink::WebCryptoKeyUsageMask usage_mask = blink::WebCryptoKeyUsageSign;

  // Import a symmetric key JWK and HMAC-SHA256 sign()
  // Uses the first SHA256 test vector from the HMAC sample set above.

  base::DictionaryValue dict;
  dict.SetString("kty", "oct");
  dict.SetString("alg", "HS256");
  dict.SetString("use", "sig");
  dict.SetBoolean("ext", false);
  dict.SetString("k", "l3nZEgZCeX8XRwJdWyK3rGB8qwjhdY8vOkbIvh4lxTuMao9Y_--hdg");

  ASSERT_EQ(
      Status::Success(),
      ImportKeyJwkFromDict(dict, algorithm, extractable, usage_mask, &key));

  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha256,
            key.algorithm().hmacParams()->hash().id());

  const std::vector<uint8> message_raw = HexStringToBytes(
      "b1689c2591eaf3c9e66070f8a77954ffb81749f1b00346f9dfe0b2ee905dcc288baf4a"
      "92de3f4001dd9f44c468c3d07d6c6ee82faceafc97c2fc0fc0601719d2dcd0aa2aec92"
      "d1b0ae933c65eb06a03c9c935c2bad0459810241347ab87e9f11adb30415424c6c7f5f"
      "22a003b8ab8de54f6ded0e3ab9245fa79568451dfa258e");

  std::vector<uint8> output;

  ASSERT_EQ(Status::Success(),
            Sign(CreateAlgorithm(blink::WebCryptoAlgorithmIdHmac),
                 key,
                 CryptoData(message_raw),
                 &output));

  const std::string mac_raw =
      "769f00d3e6a6cc1fb426a14a4f76c6462e6149726e0dee0ec0cf97a16605ac8b";

  EXPECT_BYTES_EQ_HEX(mac_raw, output);

  // TODO(padolph): Import an RSA public key JWK and use it
}

TEST_F(SharedCryptoTest, MAYBE(ImportExportJwkSymmetricKey)) {
  // Raw keys are generated by openssl:
  // % openssl rand -hex <key length bytes>
  const char* const key_hex_128 = "3f1e7cd4f6f8543f6b1e16002e688623";
  const char* const key_hex_192 =
      "ed91f916dc034eba68a0f9e7f34ddd48b98bd2848109e243";
  const char* const key_hex_256 =
      "bd08286b81a74783fd1ccf46b7e05af84ee25ae021210074159e0c4d9d907692";
  const char* const key_hex_384 =
      "a22c5441c8b185602283d64c7221de1d0951e706bfc09539435ec0e0ed614e1d406623f2"
      "b31d31819fec30993380dd82";
  const char* const key_hex_512 =
      "5834f639000d4cf82de124fbfd26fb88d463e99f839a76ba41ac88967c80a3f61e1239a4"
      "52e573dba0750e988152988576efd75b8d0229b7aca2ada2afd392ee";
  const blink::WebCryptoAlgorithm aes_cbc_alg =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc);
  const blink::WebCryptoAlgorithm aes_gcm_alg =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesGcm);
  const blink::WebCryptoAlgorithm aes_kw_alg =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);
  const blink::WebCryptoAlgorithm hmac_sha_1_alg =
      webcrypto::CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha1);
  const blink::WebCryptoAlgorithm hmac_sha_256_alg =
      webcrypto::CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256);
  const blink::WebCryptoAlgorithm hmac_sha_384_alg =
      webcrypto::CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha384);
  const blink::WebCryptoAlgorithm hmac_sha_512_alg =
      webcrypto::CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha512);

  struct TestCase {
    const char* const key_hex;
    const blink::WebCryptoAlgorithm algorithm;
    const blink::WebCryptoKeyUsageMask usage;
    const char* const jwk_alg;
  };

  // TODO(padolph): Test AES-CTR JWK export, once AES-CTR import works.
  const TestCase kTests[] = {
      // AES-CBC 128
      {key_hex_128, aes_cbc_alg,
       blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt,
       "A128CBC"},
      // AES-CBC 192
      {key_hex_192, aes_cbc_alg, blink::WebCryptoKeyUsageEncrypt, "A192CBC"},
      // AES-CBC 256
      {key_hex_256, aes_cbc_alg, blink::WebCryptoKeyUsageDecrypt, "A256CBC"},
      // AES-GCM 128
      {key_hex_128, aes_gcm_alg,
       blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt,
       "A128GCM"},
      // AES-CGM 192
      {key_hex_192, aes_gcm_alg, blink::WebCryptoKeyUsageEncrypt, "A192GCM"},
      // AES-GCM 256
      {key_hex_256, aes_gcm_alg, blink::WebCryptoKeyUsageDecrypt, "A256GCM"},
      // AES-KW 128
      {key_hex_128, aes_kw_alg,
       blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
       "A128KW"},
      // AES-KW 192
      {key_hex_192, aes_kw_alg,
       blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
       "A192KW"},
      // AES-KW 256
      {key_hex_256, aes_kw_alg,
       blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
       "A256KW"},
      // HMAC SHA-1
      {key_hex_256, hmac_sha_1_alg,
       blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify, "HS1"},
      // HMAC SHA-384
      {key_hex_384, hmac_sha_384_alg, blink::WebCryptoKeyUsageSign, "HS384"},
      // HMAC SHA-512
      {key_hex_512, hmac_sha_512_alg, blink::WebCryptoKeyUsageVerify, "HS512"},
      // Large usage value
      {key_hex_256, aes_cbc_alg,
       blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt |
           blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
       "A256CBC"},
      // Zero usage value
      {key_hex_512, hmac_sha_512_alg, 0, "HS512"},
  };

  // Round-trip import/export each key.

  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  std::vector<uint8> json;
  for (size_t test_index = 0; test_index < ARRAYSIZE_UNSAFE(kTests);
       ++test_index) {
    SCOPED_TRACE(test_index);
    const TestCase& test = kTests[test_index];

    // Skip AES-GCM tests where not supported.
    if (test.algorithm.id() == blink::WebCryptoAlgorithmIdAesGcm &&
        !SupportsAesGcm()) {
      continue;
    }

    // Import a raw key.
    key = ImportSecretKeyFromRaw(
        HexStringToBytes(test.key_hex), test.algorithm, test.usage);

    // Export the key in JWK format and validate.
    ASSERT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatJwk, key, &json));
    EXPECT_TRUE(VerifySecretJwk(json, test.jwk_alg, test.key_hex, test.usage));

    // Import the JWK-formatted key.
    ASSERT_EQ(
        Status::Success(),
        ImportKeyJwk(CryptoData(json), test.algorithm, true, test.usage, &key));
    EXPECT_TRUE(key.handle());
    EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
    EXPECT_EQ(test.algorithm.id(), key.algorithm().id());
    EXPECT_EQ(true, key.extractable());
    EXPECT_EQ(test.usage, key.usages());

    // Export the key in raw format and compare to the original.
    std::vector<uint8> key_raw_out;
    ASSERT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatRaw, key, &key_raw_out));
    EXPECT_BYTES_EQ_HEX(test.key_hex, key_raw_out);
  }
}

TEST_F(SharedCryptoTest, MAYBE(ExportJwkEmptySymmetricKey)) {
  const blink::WebCryptoAlgorithm import_algorithm =
      webcrypto::CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha1);

  blink::WebCryptoKeyUsageMask usages = blink::WebCryptoKeyUsageSign;
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();

  // Import a zero-byte HMAC key.
  const char key_data_hex[] = "";
  key = ImportSecretKeyFromRaw(
      HexStringToBytes(key_data_hex), import_algorithm, usages);
  EXPECT_EQ(0u, key.algorithm().hmacParams()->lengthBits());

  // Export the key in JWK format and validate.
  std::vector<uint8> json;
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatJwk, key, &json));
  EXPECT_TRUE(VerifySecretJwk(json, "HS1", key_data_hex, usages));

  // Now try re-importing the JWK key.
  key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatJwk,
                      CryptoData(json),
                      import_algorithm,
                      true,
                      usages,
                      &key));

  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(0u, key.algorithm().hmacParams()->lengthBits());

  std::vector<uint8> exported_key_data;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &exported_key_data));

  EXPECT_EQ(0u, exported_key_data.size());
}

TEST_F(SharedCryptoTest, MAYBE(ImportExportSpki)) {
  // Passing case: Import a valid RSA key in SPKI format.
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(
      Status::Success(),
      ImportKey(blink::WebCryptoKeyFormatSpki,
                CryptoData(HexStringToBytes(kPublicKeySpkiDerHex)),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
                true,
                blink::WebCryptoKeyUsageEncrypt,
                &key));
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, key.type());
  EXPECT_TRUE(key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageEncrypt, key.usages());
  EXPECT_EQ(kModulusLengthBits,
            key.algorithm().rsaParams()->modulusLengthBits());
  EXPECT_BYTES_EQ_HEX(
      "010001", CryptoData(key.algorithm().rsaParams()->publicExponent()));

  // Failing case: Empty SPKI data
  EXPECT_EQ(
      Status::ErrorImportEmptyKeyData(),
      ImportKey(blink::WebCryptoKeyFormatSpki,
                CryptoData(std::vector<uint8>()),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
                true,
                blink::WebCryptoKeyUsageEncrypt,
                &key));

  // Failing case: Bad DER encoding.
  EXPECT_EQ(
      Status::DataError(),
      ImportKey(blink::WebCryptoKeyFormatSpki,
                CryptoData(HexStringToBytes("618333c4cb")),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
                true,
                blink::WebCryptoKeyUsageEncrypt,
                &key));

  // Failing case: Import RSA key but provide an inconsistent input algorithm.
  EXPECT_EQ(Status::DataError(),
            ImportKey(blink::WebCryptoKeyFormatSpki,
                      CryptoData(HexStringToBytes(kPublicKeySpkiDerHex)),
                      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                      true,
                      blink::WebCryptoKeyUsageEncrypt,
                      &key));

  // Passing case: Export a previously imported RSA public key in SPKI format
  // and compare to original data.
  std::vector<uint8> output;
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatSpki, key, &output));
  EXPECT_BYTES_EQ_HEX(kPublicKeySpkiDerHex, output);

  // Failing case: Try to export a previously imported RSA public key in raw
  // format (not allowed for a public key).
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &output));

  // Failing case: Try to export a non-extractable key
  ASSERT_EQ(
      Status::Success(),
      ImportKey(blink::WebCryptoKeyFormatSpki,
                CryptoData(HexStringToBytes(kPublicKeySpkiDerHex)),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
                false,
                blink::WebCryptoKeyUsageEncrypt,
                &key));
  EXPECT_TRUE(key.handle());
  EXPECT_FALSE(key.extractable());
  EXPECT_EQ(Status::ErrorKeyNotExtractable(),
            ExportKey(blink::WebCryptoKeyFormatSpki, key, &output));
}

TEST_F(SharedCryptoTest, MAYBE(ImportExportPkcs8)) {
  // Passing case: Import a valid RSA key in PKCS#8 format.
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(HexStringToBytes(kPrivateKeyPkcs8DerHex)),
                      CreateRsaHashedImportAlgorithm(
                          blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                          blink::WebCryptoAlgorithmIdSha1),
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &key));
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, key.type());
  EXPECT_TRUE(key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageSign, key.usages());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha1,
            key.algorithm().rsaHashedParams()->hash().id());
  EXPECT_EQ(kModulusLengthBits,
            key.algorithm().rsaHashedParams()->modulusLengthBits());
  EXPECT_BYTES_EQ_HEX(
      "010001",
      CryptoData(key.algorithm().rsaHashedParams()->publicExponent()));

  std::vector<uint8> exported_key;
  ASSERT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatPkcs8, key, &exported_key));
  EXPECT_BYTES_EQ_HEX(kPrivateKeyPkcs8DerHex, exported_key);

  // Failing case: Empty PKCS#8 data
  EXPECT_EQ(Status::ErrorImportEmptyKeyData(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(std::vector<uint8>()),
                      CreateRsaHashedImportAlgorithm(
                          blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                          blink::WebCryptoAlgorithmIdSha1),
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &key));

  // Failing case: Bad DER encoding.
  EXPECT_EQ(
      Status::DataError(),
      ImportKey(blink::WebCryptoKeyFormatPkcs8,
                CryptoData(HexStringToBytes("618333c4cb")),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5),
                true,
                blink::WebCryptoKeyUsageSign,
                &key));

  // Failing case: Import RSA key but provide an inconsistent input algorithm.
  EXPECT_EQ(Status::DataError(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(HexStringToBytes(kPrivateKeyPkcs8DerHex)),
                      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &key));
}

TEST_F(SharedCryptoTest, MAYBE(GenerateKeyPairRsa)) {
  // Note: using unrealistic short key lengths here to avoid bogging down tests.

  // Successful WebCryptoAlgorithmIdRsaEsPkcs1v1_5 key generation.
  const unsigned int modulus_length = 256;
  const std::vector<uint8> public_exponent = HexStringToBytes("010001");
  blink::WebCryptoAlgorithm algorithm =
      CreateRsaKeyGenAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5,
                               modulus_length,
                               public_exponent);
  bool extractable = true;
  const blink::WebCryptoKeyUsageMask usage_mask = 0;
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));
  EXPECT_FALSE(public_key.isNull());
  EXPECT_FALSE(private_key.isNull());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key.type());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, private_key.type());
  EXPECT_TRUE(public_key.extractable());
  EXPECT_EQ(extractable, private_key.extractable());
  EXPECT_EQ(usage_mask, public_key.usages());
  EXPECT_EQ(usage_mask, private_key.usages());

  // Try exporting the generated key pair, and then re-importing to verify that
  // the exported data was valid.
  std::vector<uint8> public_key_spki;
  EXPECT_EQ(
      Status::Success(),
      ExportKey(blink::WebCryptoKeyFormatSpki, public_key, &public_key_spki));
  public_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(
      Status::Success(),
      ImportKey(blink::WebCryptoKeyFormatSpki,
                CryptoData(public_key_spki),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
                true,
                usage_mask,
                &public_key));
  EXPECT_EQ(modulus_length,
            public_key.algorithm().rsaParams()->modulusLengthBits());

  std::vector<uint8> private_key_pkcs8;
  EXPECT_EQ(
      Status::Success(),
      ExportKey(
          blink::WebCryptoKeyFormatPkcs8, private_key, &private_key_pkcs8));
  private_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(
      Status::Success(),
      ImportKey(blink::WebCryptoKeyFormatPkcs8,
                CryptoData(private_key_pkcs8),
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5),
                true,
                usage_mask,
                &private_key));
  EXPECT_EQ(modulus_length,
            private_key.algorithm().rsaParams()->modulusLengthBits());

  // Fail with bad modulus.
  algorithm = CreateRsaKeyGenAlgorithm(
      blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5, 0, public_exponent);
  EXPECT_EQ(Status::ErrorGenerateRsaZeroModulus(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));

  // Fail with bad exponent: larger than unsigned long.
  unsigned int exponent_length = sizeof(unsigned long) + 1;  // NOLINT
  const std::vector<uint8> long_exponent(exponent_length, 0x01);
  algorithm = CreateRsaKeyGenAlgorithm(
      blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5, modulus_length, long_exponent);
  EXPECT_EQ(Status::ErrorGenerateKeyPublicExponent(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));

  // Fail with bad exponent: empty.
  const std::vector<uint8> empty_exponent;
  algorithm =
      CreateRsaKeyGenAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5,
                               modulus_length,
                               empty_exponent);
  EXPECT_EQ(Status::ErrorGenerateKeyPublicExponent(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));

  // Fail with bad exponent: all zeros.
  std::vector<uint8> exponent_with_leading_zeros(15, 0x00);
  algorithm =
      CreateRsaKeyGenAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5,
                               modulus_length,
                               exponent_with_leading_zeros);
  EXPECT_EQ(Status::ErrorGenerateKeyPublicExponent(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));

  // Key generation success using exponent with leading zeros.
  exponent_with_leading_zeros.insert(exponent_with_leading_zeros.end(),
                                     public_exponent.begin(),
                                     public_exponent.end());
  algorithm =
      CreateRsaKeyGenAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5,
                               modulus_length,
                               exponent_with_leading_zeros);
  EXPECT_EQ(Status::Success(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));
  EXPECT_FALSE(public_key.isNull());
  EXPECT_FALSE(private_key.isNull());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key.type());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, private_key.type());
  EXPECT_TRUE(public_key.extractable());
  EXPECT_EQ(extractable, private_key.extractable());
  EXPECT_EQ(usage_mask, public_key.usages());
  EXPECT_EQ(usage_mask, private_key.usages());

  // Successful WebCryptoAlgorithmIdRsaSsaPkcs1v1_5 key generation (sha256)
  algorithm =
      CreateRsaHashedKeyGenAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                                     blink::WebCryptoAlgorithmIdSha256,
                                     modulus_length,
                                     public_exponent);
  EXPECT_EQ(Status::Success(),
            GenerateKeyPair(
                algorithm, extractable, usage_mask, &public_key, &private_key));
  EXPECT_FALSE(public_key.isNull());
  EXPECT_FALSE(private_key.isNull());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key.type());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, private_key.type());
  EXPECT_EQ(modulus_length,
            public_key.algorithm().rsaHashedParams()->modulusLengthBits());
  EXPECT_EQ(modulus_length,
            private_key.algorithm().rsaHashedParams()->modulusLengthBits());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha256,
            public_key.algorithm().rsaHashedParams()->hash().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha256,
            private_key.algorithm().rsaHashedParams()->hash().id());
  EXPECT_TRUE(public_key.extractable());
  EXPECT_EQ(extractable, private_key.extractable());
  EXPECT_EQ(usage_mask, public_key.usages());
  EXPECT_EQ(usage_mask, private_key.usages());

  // Successful WebCryptoAlgorithmIdRsaSsaPkcs1v1_5 key generation.
  algorithm =
      CreateRsaHashedKeyGenAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                                     blink::WebCryptoAlgorithmIdSha1,
                                     modulus_length,
                                     public_exponent);
  EXPECT_EQ(
      Status::Success(),
      GenerateKeyPair(algorithm, false, usage_mask, &public_key, &private_key));
  EXPECT_FALSE(public_key.isNull());
  EXPECT_FALSE(private_key.isNull());
  EXPECT_EQ(blink::WebCryptoKeyTypePublic, public_key.type());
  EXPECT_EQ(blink::WebCryptoKeyTypePrivate, private_key.type());
  EXPECT_EQ(modulus_length,
            public_key.algorithm().rsaHashedParams()->modulusLengthBits());
  EXPECT_EQ(modulus_length,
            private_key.algorithm().rsaHashedParams()->modulusLengthBits());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha1,
            public_key.algorithm().rsaHashedParams()->hash().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha1,
            private_key.algorithm().rsaHashedParams()->hash().id());
  // Even though "extractable" was set to false, the public key remains
  // extractable.
  EXPECT_TRUE(public_key.extractable());
  EXPECT_FALSE(private_key.extractable());
  EXPECT_EQ(usage_mask, public_key.usages());
  EXPECT_EQ(usage_mask, private_key.usages());

  // Exporting a private key as SPKI format doesn't make sense. However this
  // will first fail because the key is not extractable.
  std::vector<uint8> output;
  EXPECT_EQ(Status::ErrorKeyNotExtractable(),
            ExportKey(blink::WebCryptoKeyFormatSpki, private_key, &output));

  // Re-generate an extractable private_key and try to export it as SPKI format.
  // This should fail since spki is for public keys.
  EXPECT_EQ(
      Status::Success(),
      GenerateKeyPair(algorithm, true, usage_mask, &public_key, &private_key));
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            ExportKey(blink::WebCryptoKeyFormatSpki, private_key, &output));
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsRoundTrip)) {
  // Import a key pair.
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      HexStringToBytes(kPublicKeySpkiDerHex),
      HexStringToBytes(kPrivateKeyPkcs8DerHex),
      algorithm,
      false,
      blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt,
      &public_key,
      &private_key);

  // Make a maximum-length data message. RSAES can operate on messages up to
  // length of k - 11 bytes, where k is the octet length of the RSA modulus.
  const unsigned int kMaxMsgSizeBytes = kModulusLengthBits / 8 - 11;
  // There are two hex chars for each byte.
  const unsigned int kMsgHexSize = kMaxMsgSizeBytes * 2;
  char max_data_hex[kMsgHexSize + 1];
  std::fill(&max_data_hex[0], &max_data_hex[0] + kMsgHexSize, 'a');
  max_data_hex[kMsgHexSize] = '\0';

  // Verify encrypt / decrypt round trip on a few messages. Note that RSA
  // encryption does not support empty input.
  algorithm = CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  const char* const kTestDataHex[] = {"ff", "0102030405060708090a0b0c0d0e0f",
                                      max_data_hex};
  std::vector<uint8> encrypted_data;
  std::vector<uint8> decrypted_data;
  for (size_t i = 0; i < ARRAYSIZE_UNSAFE(kTestDataHex); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(Status::Success(),
              Encrypt(algorithm,
                      public_key,
                      CryptoData(HexStringToBytes(kTestDataHex[i])),
                      &encrypted_data));
    EXPECT_EQ(kModulusLengthBits / 8, encrypted_data.size());
    ASSERT_EQ(Status::Success(),
              Decrypt(algorithm,
                      private_key,
                      CryptoData(encrypted_data),
                      &decrypted_data));
    EXPECT_BYTES_EQ_HEX(kTestDataHex[i], decrypted_data);
  }
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsKnownAnswer)) {
  scoped_ptr<base::Value> json;
  ASSERT_TRUE(ReadJsonTestFile("rsa_es.json", &json));
  base::DictionaryValue* test = NULL;
  ASSERT_TRUE(json->GetAsDictionary(&test));

  // Because the random data in PKCS1.5 padding makes the encryption output non-
  // deterministic, we cannot easily do a typical known-answer test for RSA
  // encryption / decryption. Instead we will take a known-good encrypted
  // message, decrypt it, re-encrypt it, then decrypt again, verifying that the
  // original known cleartext is the result.

  const std::vector<uint8> rsa_spki_der =
      GetBytesFromHexString(test, "rsa_spki_der");

  const std::vector<uint8> rsa_pkcs8_der =
      GetBytesFromHexString(test, "rsa_pkcs8_der");
  const std::vector<uint8> ciphertext =
      GetBytesFromHexString(test, "ciphertext");
  const std::vector<uint8> cleartext = GetBytesFromHexString(test, "cleartext");

  // Import the key pair.
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      rsa_spki_der,
      rsa_pkcs8_der,
      algorithm,
      false,
      blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt,
      &public_key,
      &private_key);

  // Decrypt the known-good ciphertext with the private key. As a check we must
  // get the known original cleartext.
  std::vector<uint8> decrypted_data;
  ASSERT_EQ(
      Status::Success(),
      Decrypt(algorithm, private_key, CryptoData(ciphertext), &decrypted_data));
  EXPECT_BYTES_EQ(cleartext, decrypted_data);

  // Encrypt this decrypted data with the public key.
  std::vector<uint8> encrypted_data;
  ASSERT_EQ(
      Status::Success(),
      Encrypt(
          algorithm, public_key, CryptoData(decrypted_data), &encrypted_data));
  EXPECT_EQ(128u, encrypted_data.size());

  // Finally, decrypt the newly encrypted result with the private key, and
  // compare to the known original cleartext.
  decrypted_data.clear();
  ASSERT_EQ(
      Status::Success(),
      Decrypt(
          algorithm, private_key, CryptoData(encrypted_data), &decrypted_data));
  EXPECT_EQ(cleartext, decrypted_data);
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsFailures)) {
  // Import a key pair.
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      HexStringToBytes(kPublicKeySpkiDerHex),
      HexStringToBytes(kPrivateKeyPkcs8DerHex),
      algorithm,
      false,
      blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt,
      &public_key,
      &private_key);

  // Fail encrypt with a private key.
  std::vector<uint8> encrypted_data;
  const std::string message_hex_str("0102030405060708090a0b0c0d0e0f");
  const std::vector<uint8> message_hex(HexStringToBytes(message_hex_str));
  EXPECT_EQ(
      Status::ErrorUnexpectedKeyType(),
      Encrypt(
          algorithm, private_key, CryptoData(message_hex), &encrypted_data));

  // Fail encrypt with empty message.
  EXPECT_EQ(Status::ErrorDataTooSmall(),
            Encrypt(algorithm,
                    public_key,
                    CryptoData(std::vector<uint8>()),
                    &encrypted_data));

  // Fail encrypt with message too large. RSAES can operate on messages up to
  // length of k - 11 bytes, where k is the octet length of the RSA modulus.
  const unsigned int kMaxMsgSizeBytes = kModulusLengthBits / 8 - 11;
  EXPECT_EQ(Status::ErrorDataTooLarge(),
            Encrypt(algorithm,
                    public_key,
                    CryptoData(std::vector<uint8>(kMaxMsgSizeBytes + 1, '0')),
                    &encrypted_data));

  // Generate encrypted data.
  EXPECT_EQ(
      Status::Success(),
      Encrypt(algorithm, public_key, CryptoData(message_hex), &encrypted_data));

  // Fail decrypt with a public key.
  std::vector<uint8> decrypted_data;
  EXPECT_EQ(
      Status::ErrorUnexpectedKeyType(),
      Decrypt(
          algorithm, public_key, CryptoData(encrypted_data), &decrypted_data));

  // Corrupt encrypted data; ensure decrypt fails because padding was disrupted.
  EXPECT_EQ(Status::OperationError(),
            Decrypt(algorithm,
                    private_key,
                    CryptoData(Corrupted(encrypted_data)),
                    &decrypted_data));

  // TODO(padolph): Are there other specific data corruption scenarios to
  // consider?

  // Do a successful decrypt with good data just for confirmation.
  EXPECT_EQ(
      Status::Success(),
      Decrypt(
          algorithm, private_key, CryptoData(encrypted_data), &decrypted_data));
  EXPECT_BYTES_EQ_HEX(message_hex_str, decrypted_data);
}

TEST_F(SharedCryptoTest, MAYBE(RsaSsaSignVerifyFailures)) {
  // Import a key pair.
  blink::WebCryptoKeyUsageMask usage_mask =
      blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify;
  blink::WebCryptoAlgorithm importAlgorithm =
      CreateRsaHashedImportAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                                     blink::WebCryptoAlgorithmIdSha1);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(HexStringToBytes(kPublicKeySpkiDerHex),
                   HexStringToBytes(kPrivateKeyPkcs8DerHex),
                   importAlgorithm,
                   false,
                   usage_mask,
                   &public_key,
                   &private_key);

  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5);

  std::vector<uint8> signature;
  bool signature_match;

  // Compute a signature.
  const std::vector<uint8> data = HexStringToBytes("010203040506070809");
  ASSERT_EQ(Status::Success(),
            Sign(algorithm, private_key, CryptoData(data), &signature));

  // Ensure truncated signature does not verify by passing one less byte.
  EXPECT_EQ(Status::Success(),
            VerifySignature(
                algorithm,
                public_key,
                CryptoData(Uint8VectorStart(signature), signature.size() - 1),
                CryptoData(data),
                &signature_match));
  EXPECT_FALSE(signature_match);

  // Ensure truncated signature does not verify by passing no bytes.
  EXPECT_EQ(Status::Success(),
            VerifySignature(algorithm,
                            public_key,
                            CryptoData(),
                            CryptoData(data),
                            &signature_match));
  EXPECT_FALSE(signature_match);

  // Ensure corrupted signature does not verify.
  std::vector<uint8> corrupt_sig = signature;
  corrupt_sig[corrupt_sig.size() / 2] ^= 0x1;
  EXPECT_EQ(Status::Success(),
            VerifySignature(algorithm,
                            public_key,
                            CryptoData(corrupt_sig),
                            CryptoData(data),
                            &signature_match));
  EXPECT_FALSE(signature_match);

  // Ensure signatures that are greater than the modulus size fail.
  const unsigned int long_message_size_bytes = 1024;
  DCHECK_GT(long_message_size_bytes, kModulusLengthBits / 8);
  const unsigned char kLongSignature[long_message_size_bytes] = {0};
  EXPECT_EQ(Status::Success(),
            VerifySignature(algorithm,
                            public_key,
                            CryptoData(kLongSignature, sizeof(kLongSignature)),
                            CryptoData(data),
                            &signature_match));
  EXPECT_FALSE(signature_match);

  // Ensure that verifying using a private key, rather than a public key, fails.
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            VerifySignature(algorithm,
                            private_key,
                            CryptoData(signature),
                            CryptoData(data),
                            &signature_match));

  // Ensure that signing using a public key, rather than a private key, fails.
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            Sign(algorithm, public_key, CryptoData(data), &signature));

  // Ensure that signing and verifying with an incompatible algorithm fails.
  algorithm = CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);

  EXPECT_EQ(Status::ErrorUnexpected(),
            Sign(algorithm, private_key, CryptoData(data), &signature));
  EXPECT_EQ(Status::ErrorUnexpected(),
            VerifySignature(algorithm,
                            public_key,
                            CryptoData(signature),
                            CryptoData(data),
                            &signature_match));

  // Some crypto libraries (NSS) can automatically select the RSA SSA inner hash
  // based solely on the contents of the input signature data. In the Web Crypto
  // implementation, the inner hash should be specified uniquely by the key
  // algorithm parameter. To validate this behavior, call Verify with a computed
  // signature that used one hash type (SHA-1), but pass in a key with a
  // different inner hash type (SHA-256). If the hash type is determined by the
  // signature itself (undesired), the verify will pass, while if the hash type
  // is specified by the key algorithm (desired), the verify will fail.

  // Compute a signature using SHA-1 as the inner hash.
  EXPECT_EQ(Status::Success(),
            Sign(CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5),
                 private_key,
                 CryptoData(data),
                 &signature));

  blink::WebCryptoKey public_key_256 = blink::WebCryptoKey::createNull();
  EXPECT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatSpki,
                      CryptoData(HexStringToBytes(kPublicKeySpkiDerHex)),
                      CreateRsaHashedImportAlgorithm(
                          blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                          blink::WebCryptoAlgorithmIdSha256),
                      true,
                      usage_mask,
                      &public_key_256));

  // Now verify using an algorithm whose inner hash is SHA-256, not SHA-1. The
  // signature should not verify.
  // NOTE: public_key was produced by generateKey, and so its associated
  // algorithm has WebCryptoRsaKeyGenParams and not WebCryptoRsaSsaParams. Thus
  // it has no inner hash to conflict with the input algorithm.
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha1,
            private_key.algorithm().rsaHashedParams()->hash().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha256,
            public_key_256.algorithm().rsaHashedParams()->hash().id());

  bool is_match;
  EXPECT_EQ(Status::Success(),
            VerifySignature(
                CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5),
                public_key_256,
                CryptoData(signature),
                CryptoData(data),
                &is_match));
  EXPECT_FALSE(is_match);
}

TEST_F(SharedCryptoTest, MAYBE(RsaSignVerifyKnownAnswer)) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("pkcs1v15_sign.json", &tests));

  // Import the key pair.
  blink::WebCryptoAlgorithm importAlgorithm =
      CreateRsaHashedImportAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5,
                                     blink::WebCryptoAlgorithmIdSha1);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      HexStringToBytes(kPublicKeySpkiDerHex),
      HexStringToBytes(kPrivateKeyPkcs8DerHex),
      importAlgorithm,
      false,
      blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify,
      &public_key,
      &private_key);

  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaSsaPkcs1v1_5);

  // Validate the signatures are computed and verified as expected.
  std::vector<uint8> signature;
  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);

    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));

    std::vector<uint8> test_message =
        GetBytesFromHexString(test, "message_hex");
    std::vector<uint8> test_signature =
        GetBytesFromHexString(test, "signature_hex");

    signature.clear();
    ASSERT_EQ(
        Status::Success(),
        Sign(algorithm, private_key, CryptoData(test_message), &signature));
    EXPECT_BYTES_EQ(test_signature, signature);

    bool is_match = false;
    ASSERT_EQ(Status::Success(),
              VerifySignature(algorithm,
                              public_key,
                              CryptoData(test_signature),
                              CryptoData(test_message),
                              &is_match));
    EXPECT_TRUE(is_match);
  }
}

TEST_F(SharedCryptoTest, MAYBE(AesKwKeyImport)) {
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);

  // Import a 128-bit Key Encryption Key (KEK)
  std::string key_raw_hex_in = "025a8cf3f08b4f6c5f33bbc76a471939";
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes(key_raw_hex_in)),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));
  std::vector<uint8> key_raw_out;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &key_raw_out));
  EXPECT_BYTES_EQ_HEX(key_raw_hex_in, key_raw_out);

  // Import a 192-bit KEK
  key_raw_hex_in = "c0192c6466b2370decbb62b2cfef4384544ffeb4d2fbc103";
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes(key_raw_hex_in)),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &key_raw_out));
  EXPECT_BYTES_EQ_HEX(key_raw_hex_in, key_raw_out);

  // Import a 256-bit Key Encryption Key (KEK)
  key_raw_hex_in =
      "e11fe66380d90fa9ebefb74e0478e78f95664d0c67ca20ce4a0b5842863ac46f";
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes(key_raw_hex_in)),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key, &key_raw_out));
  EXPECT_BYTES_EQ_HEX(key_raw_hex_in, key_raw_out);

  // Fail import of 0 length key
  EXPECT_EQ(Status::ErrorImportAesKeyLength(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes("")),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));

  // Fail import of 124-bit KEK
  key_raw_hex_in = "3e4566a2bdaa10cb68134fa66c15ddb";
  EXPECT_EQ(Status::ErrorImportAesKeyLength(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes(key_raw_hex_in)),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));

  // Fail import of 200-bit KEK
  key_raw_hex_in = "0a1d88608a5ad9fec64f1ada269ebab4baa2feeb8d95638c0e";
  EXPECT_EQ(Status::ErrorImportAesKeyLength(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes(key_raw_hex_in)),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));

  // Fail import of 260-bit KEK
  key_raw_hex_in =
      "72d4e475ff34215416c9ad9c8281247a4d730c5f275ac23f376e73e3bce8d7d5a";
  EXPECT_EQ(Status::ErrorImportAesKeyLength(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(HexStringToBytes(key_raw_hex_in)),
                      algorithm,
                      true,
                      blink::WebCryptoKeyUsageWrapKey,
                      &key));
}

TEST_F(SharedCryptoTest, MAYBE(UnwrapFailures)) {
  // This test exercises the code path common to all unwrap operations.
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_kw.json", &tests));
  base::DictionaryValue* test;
  ASSERT_TRUE(tests->GetDictionary(0, &test));
  const std::vector<uint8> test_kek = GetBytesFromHexString(test, "kek");
  const std::vector<uint8> test_ciphertext =
      GetBytesFromHexString(test, "ciphertext");

  // Using a key that does not have unwrapKey usage should fail.
  blink::WebCryptoKey bad_wrapping_key = ImportSecretKeyFromRaw(
      test_kek,
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw),
      blink::WebCryptoKeyUsageDecrypt);  // <-- should be UnwrapKey
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(
      Status::ErrorUnexpected(),
      UnwrapKey(blink::WebCryptoKeyFormatRaw,
                CryptoData(test_ciphertext),
                bad_wrapping_key,
                webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw),
                webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                true,
                blink::WebCryptoKeyUsageEncrypt,
                &unwrapped_key));

  // Using a wrapping algorithm that does not match the wrapping key algorithm
  // should fail.
  blink::WebCryptoKey wrapping_key = ImportSecretKeyFromRaw(
      test_kek,
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw),
      blink::WebCryptoKeyUsageUnwrapKey);
  EXPECT_EQ(
      Status::ErrorUnexpected(),
      UnwrapKey(blink::WebCryptoKeyFormatRaw,
                CryptoData(test_ciphertext),
                wrapping_key,
                webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                true,
                blink::WebCryptoKeyUsageEncrypt,
                &unwrapped_key));
}

TEST_F(SharedCryptoTest, MAYBE(AesKwRawSymkeyWrapUnwrapKnownAnswer)) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_kw.json", &tests));

  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);
    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));
    const std::vector<uint8> test_kek = GetBytesFromHexString(test, "kek");
    const std::vector<uint8> test_key = GetBytesFromHexString(test, "key");
    const std::vector<uint8> test_ciphertext =
        GetBytesFromHexString(test, "ciphertext");
    const blink::WebCryptoAlgorithm wrapping_algorithm =
        webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);

    // Import the wrapping key.
    blink::WebCryptoKey wrapping_key = ImportSecretKeyFromRaw(
        test_kek,
        wrapping_algorithm,
        blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey);

    // Import the key to be wrapped.
    blink::WebCryptoKey key = ImportSecretKeyFromRaw(
        test_key,
        webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
        blink::WebCryptoKeyUsageEncrypt);

    // Wrap the key and verify the ciphertext result against the known answer.
    std::vector<uint8> wrapped_key;
    ASSERT_EQ(Status::Success(),
              WrapKey(blink::WebCryptoKeyFormatRaw,
                      key,
                      wrapping_key,
                      wrapping_algorithm,
                      &wrapped_key));
    EXPECT_BYTES_EQ(test_ciphertext, wrapped_key);

    // Unwrap the known ciphertext to get a new test_key.
    blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
    ASSERT_EQ(
        Status::Success(),
        UnwrapKey(blink::WebCryptoKeyFormatRaw,
                  CryptoData(test_ciphertext),
                  wrapping_key,
                  wrapping_algorithm,
                  webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                  true,
                  blink::WebCryptoKeyUsageEncrypt,
                  &unwrapped_key));
    EXPECT_FALSE(key.isNull());
    EXPECT_TRUE(key.handle());
    EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
    EXPECT_EQ(blink::WebCryptoAlgorithmIdAesCbc, key.algorithm().id());
    EXPECT_EQ(true, key.extractable());
    EXPECT_EQ(blink::WebCryptoKeyUsageEncrypt, key.usages());

    // Export the new key and compare its raw bytes with the original known key.
    std::vector<uint8> raw_key;
    EXPECT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatRaw, unwrapped_key, &raw_key));
    EXPECT_BYTES_EQ(test_key, raw_key);
  }
}

// Unwrap a HMAC key using AES-KW, and then try doing a sign/verify with the
// unwrapped key
TEST_F(SharedCryptoTest, MAYBE(AesKwRawSymkeyUnwrapSignVerifyHmac)) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_kw.json", &tests));

  base::DictionaryValue* test;
  ASSERT_TRUE(tests->GetDictionary(0, &test));
  const std::vector<uint8> test_kek = GetBytesFromHexString(test, "kek");
  const std::vector<uint8> test_ciphertext =
      GetBytesFromHexString(test, "ciphertext");
  const blink::WebCryptoAlgorithm wrapping_algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);

  // Import the wrapping key.
  blink::WebCryptoKey wrapping_key = ImportSecretKeyFromRaw(
      test_kek, wrapping_algorithm, blink::WebCryptoKeyUsageUnwrapKey);

  // Unwrap the known ciphertext.
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(
      Status::Success(),
      UnwrapKey(blink::WebCryptoKeyFormatRaw,
                CryptoData(test_ciphertext),
                wrapping_key,
                wrapping_algorithm,
                CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha1),
                false,
                blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify,
                &key));

  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, key.algorithm().id());
  EXPECT_FALSE(key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageSign | blink::WebCryptoKeyUsageVerify,
            key.usages());

  // Sign an empty message and ensure it is verified.
  std::vector<uint8> test_message;
  std::vector<uint8> signature;

  ASSERT_EQ(Status::Success(),
            Sign(CreateAlgorithm(blink::WebCryptoAlgorithmIdHmac),
                 key,
                 CryptoData(test_message),
                 &signature));

  EXPECT_GT(signature.size(), 0u);

  bool verify_result;
  ASSERT_EQ(Status::Success(),
            VerifySignature(CreateAlgorithm(blink::WebCryptoAlgorithmIdHmac),
                            key,
                            CryptoData(signature),
                            CryptoData(test_message),
                            &verify_result));
}

TEST_F(SharedCryptoTest, MAYBE(AesKwRawSymkeyWrapUnwrapErrors)) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_kw.json", &tests));
  base::DictionaryValue* test;
  // Use 256 bits of data with a 256-bit KEK
  ASSERT_TRUE(tests->GetDictionary(5, &test));
  const std::vector<uint8> test_kek = GetBytesFromHexString(test, "kek");
  const std::vector<uint8> test_key = GetBytesFromHexString(test, "key");
  const std::vector<uint8> test_ciphertext =
      GetBytesFromHexString(test, "ciphertext");
  const blink::WebCryptoAlgorithm wrapping_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);
  const blink::WebCryptoAlgorithm key_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc);
  // Import the wrapping key.
  blink::WebCryptoKey wrapping_key = ImportSecretKeyFromRaw(
      test_kek,
      wrapping_algorithm,
      blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey);
  // Import the key to be wrapped.
  blink::WebCryptoKey key = ImportSecretKeyFromRaw(
      test_key,
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
      blink::WebCryptoKeyUsageEncrypt);

  // Unwrap with wrapped data too small must fail.
  const std::vector<uint8> small_data(test_ciphertext.begin(),
                                      test_ciphertext.begin() + 23);
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(Status::ErrorDataTooSmall(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(small_data),
                      wrapping_key,
                      wrapping_algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageEncrypt,
                      &unwrapped_key));

  // Unwrap with wrapped data size not a multiple of 8 bytes must fail.
  const std::vector<uint8> unaligned_data(test_ciphertext.begin(),
                                          test_ciphertext.end() - 2);
  EXPECT_EQ(Status::ErrorInvalidAesKwDataLength(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(unaligned_data),
                      wrapping_key,
                      wrapping_algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageEncrypt,
                      &unwrapped_key));
}

TEST_F(SharedCryptoTest, MAYBE(AesKwRawSymkeyUnwrapCorruptData)) {
  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_kw.json", &tests));
  base::DictionaryValue* test;
  // Use 256 bits of data with a 256-bit KEK
  ASSERT_TRUE(tests->GetDictionary(5, &test));
  const std::vector<uint8> test_kek = GetBytesFromHexString(test, "kek");
  const std::vector<uint8> test_key = GetBytesFromHexString(test, "key");
  const std::vector<uint8> test_ciphertext =
      GetBytesFromHexString(test, "ciphertext");
  const blink::WebCryptoAlgorithm wrapping_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);

  // Import the wrapping key.
  blink::WebCryptoKey wrapping_key = ImportSecretKeyFromRaw(
      test_kek,
      wrapping_algorithm,
      blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey);

  // Unwrap of a corrupted version of the known ciphertext should fail, due to
  // AES-KW's built-in integrity check.
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(
      Status::OperationError(),
      UnwrapKey(blink::WebCryptoKeyFormatRaw,
                CryptoData(Corrupted(test_ciphertext)),
                wrapping_key,
                wrapping_algorithm,
                webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                true,
                blink::WebCryptoKeyUsageEncrypt,
                &unwrapped_key));
}

TEST_F(SharedCryptoTest, MAYBE(AesKwJwkSymkeyUnwrapKnownData)) {
  // The following data lists a known HMAC SHA-256 key, then a JWK
  // representation of this key which was encrypted ("wrapped") using AES-KW and
  // the following wrapping key.
  // For reference, the intermediate clear JWK is
  // {"alg":"HS256","ext":true,"k":<b64urlKey>,"key_ops":["verify"],"kty":"oct"}
  // (Not shown is space padding to ensure the cleartext meets the size
  // requirements of the AES-KW algorithm.)
  const std::vector<uint8> key_data = HexStringToBytes(
      "000102030405060708090A0B0C0D0E0F000102030405060708090A0B0C0D0E0F");
  const std::vector<uint8> wrapped_key_data = HexStringToBytes(
      "14E6380B35FDC5B72E1994764B6CB7BFDD64E7832894356AAEE6C3768FC3D0F115E6B0"
      "6729756225F999AA99FDF81FD6A359F1576D3D23DE6CB69C3937054EB497AC1E8C38D5"
      "5E01B9783A20C8D930020932CF25926103002213D0FC37279888154FEBCEDF31832158"
      "97938C5CFE5B10B4254D0C399F39D0");
  const std::vector<uint8> wrapping_key_data =
      HexStringToBytes("000102030405060708090A0B0C0D0E0F");
  const blink::WebCryptoAlgorithm wrapping_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdAesKw);

  // Import the wrapping key.
  blink::WebCryptoKey wrapping_key = ImportSecretKeyFromRaw(
      wrapping_key_data, wrapping_algorithm, blink::WebCryptoKeyUsageUnwrapKey);

  // Unwrap the known wrapped key data to produce a new key
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(
      Status::Success(),
      UnwrapKey(blink::WebCryptoKeyFormatJwk,
                CryptoData(wrapped_key_data),
                wrapping_key,
                wrapping_algorithm,
                CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256),
                true,
                blink::WebCryptoKeyUsageVerify,
                &unwrapped_key));

  // Validate the new key's attributes.
  EXPECT_FALSE(unwrapped_key.isNull());
  EXPECT_TRUE(unwrapped_key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, unwrapped_key.type());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdHmac, unwrapped_key.algorithm().id());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdSha256,
            unwrapped_key.algorithm().hmacParams()->hash().id());
  EXPECT_EQ(256u, unwrapped_key.algorithm().hmacParams()->lengthBits());
  EXPECT_EQ(true, unwrapped_key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageVerify, unwrapped_key.usages());

  // Export the new key's raw data and compare to the known original.
  std::vector<uint8> raw_key;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, unwrapped_key, &raw_key));
  EXPECT_BYTES_EQ(key_data, raw_key);
}

// TODO(eroman):
//   * Test decryption when the tag length exceeds input size
//   * Test decryption with empty input
//   * Test decryption with tag length of 0.
TEST_F(SharedCryptoTest, MAYBE(AesGcmSampleSets)) {
  // Some Linux test runners may not have a new enough version of NSS.
  if (!SupportsAesGcm()) {
    LOG(WARNING) << "AES GCM not supported, skipping tests";
    return;
  }

  scoped_ptr<base::ListValue> tests;
  ASSERT_TRUE(ReadJsonTestFileToList("aes_gcm.json", &tests));

  // Note that WebCrypto appends the authentication tag to the ciphertext.
  for (size_t test_index = 0; test_index < tests->GetSize(); ++test_index) {
    SCOPED_TRACE(test_index);
    base::DictionaryValue* test;
    ASSERT_TRUE(tests->GetDictionary(test_index, &test));

    const std::vector<uint8> test_key = GetBytesFromHexString(test, "key");
    const std::vector<uint8> test_iv = GetBytesFromHexString(test, "iv");
    const std::vector<uint8> test_additional_data =
        GetBytesFromHexString(test, "additional_data");
    const std::vector<uint8> test_plain_text =
        GetBytesFromHexString(test, "plain_text");
    const std::vector<uint8> test_authentication_tag =
        GetBytesFromHexString(test, "authentication_tag");
    const unsigned int test_tag_size_bits = test_authentication_tag.size() * 8;
    const std::vector<uint8> test_cipher_text =
        GetBytesFromHexString(test, "cipher_text");

    blink::WebCryptoKey key = ImportSecretKeyFromRaw(
        test_key,
        CreateAlgorithm(blink::WebCryptoAlgorithmIdAesGcm),
        blink::WebCryptoKeyUsageEncrypt | blink::WebCryptoKeyUsageDecrypt);

    // Verify exported raw key is identical to the imported data
    std::vector<uint8> raw_key;
    EXPECT_EQ(Status::Success(),
              ExportKey(blink::WebCryptoKeyFormatRaw, key, &raw_key));

    EXPECT_BYTES_EQ(test_key, raw_key);

    // Test encryption.
    std::vector<uint8> cipher_text;
    std::vector<uint8> authentication_tag;
    EXPECT_EQ(Status::Success(),
              AesGcmEncrypt(key,
                            test_iv,
                            test_additional_data,
                            test_tag_size_bits,
                            test_plain_text,
                            &cipher_text,
                            &authentication_tag));

    EXPECT_BYTES_EQ(test_cipher_text, cipher_text);
    EXPECT_BYTES_EQ(test_authentication_tag, authentication_tag);

    // Test decryption.
    std::vector<uint8> plain_text;
    EXPECT_EQ(Status::Success(),
              AesGcmDecrypt(key,
                            test_iv,
                            test_additional_data,
                            test_tag_size_bits,
                            test_cipher_text,
                            test_authentication_tag,
                            &plain_text));
    EXPECT_BYTES_EQ(test_plain_text, plain_text);

    // Decryption should fail if any of the inputs are tampered with.
    EXPECT_EQ(Status::OperationError(),
              AesGcmDecrypt(key,
                            Corrupted(test_iv),
                            test_additional_data,
                            test_tag_size_bits,
                            test_cipher_text,
                            test_authentication_tag,
                            &plain_text));
    EXPECT_EQ(Status::OperationError(),
              AesGcmDecrypt(key,
                            test_iv,
                            Corrupted(test_additional_data),
                            test_tag_size_bits,
                            test_cipher_text,
                            test_authentication_tag,
                            &plain_text));
    EXPECT_EQ(Status::OperationError(),
              AesGcmDecrypt(key,
                            test_iv,
                            test_additional_data,
                            test_tag_size_bits,
                            Corrupted(test_cipher_text),
                            test_authentication_tag,
                            &plain_text));
    EXPECT_EQ(Status::OperationError(),
              AesGcmDecrypt(key,
                            test_iv,
                            test_additional_data,
                            test_tag_size_bits,
                            test_cipher_text,
                            Corrupted(test_authentication_tag),
                            &plain_text));

    // Try different incorrect tag lengths
    uint8 kAlternateTagLengths[] = {0, 8, 96, 120, 128, 160, 255};
    for (size_t tag_i = 0; tag_i < arraysize(kAlternateTagLengths); ++tag_i) {
      unsigned int wrong_tag_size_bits = kAlternateTagLengths[tag_i];
      if (test_tag_size_bits == wrong_tag_size_bits)
        continue;
      EXPECT_NE(Status::Success(),
                AesGcmDecrypt(key,
                              test_iv,
                              test_additional_data,
                              wrong_tag_size_bits,
                              test_cipher_text,
                              test_authentication_tag,
                              &plain_text));
    }
  }
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsRawSymkeyWrapUnwrapKnownAnswer)) {
  scoped_ptr<base::Value> json;
  ASSERT_TRUE(ReadJsonTestFile("rsa_es.json", &json));
  base::DictionaryValue* test = NULL;
  ASSERT_TRUE(json->GetAsDictionary(&test));
  const std::vector<uint8> rsa_spki_der =
      GetBytesFromHexString(test, "rsa_spki_der");
  const std::vector<uint8> rsa_pkcs8_der =
      GetBytesFromHexString(test, "rsa_pkcs8_der");
  const std::vector<uint8> ciphertext =
      GetBytesFromHexString(test, "ciphertext");
  const std::vector<uint8> cleartext = GetBytesFromHexString(test, "cleartext");
  blink::WebCryptoAlgorithm key_algorithm =
      CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256);

  // Import the RSA key pair.
  blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      rsa_spki_der,
      rsa_pkcs8_der,
      algorithm,
      false,
      blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
      &public_key,
      &private_key);

  // Import the symmetric key.
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(cleartext),
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &key));

  // Wrap the symmetric key with raw format.
  std::vector<uint8> wrapped_key;
  ASSERT_EQ(Status::Success(),
            WrapKey(blink::WebCryptoKeyFormatRaw,
                    key,
                    public_key,
                    algorithm,
                    &wrapped_key));

  // Unwrap the wrapped key.
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(wrapped_key),
                      private_key,
                      algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &unwrapped_key));
  EXPECT_FALSE(key.isNull());
  EXPECT_TRUE(key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, key.type());
  EXPECT_EQ(key_algorithm.id(), key.algorithm().id());
  EXPECT_EQ(true, key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageSign, key.usages());

  // Export the new key and compare its raw bytes with the original known data.
  std::vector<uint8> raw_key;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, unwrapped_key, &raw_key));
  EXPECT_BYTES_EQ(cleartext, raw_key);

  // Unwrap the known wrapped key and compare to the known cleartext.
  ASSERT_EQ(Status::Success(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(ciphertext),
                      private_key,
                      algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &unwrapped_key));
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, unwrapped_key, &raw_key));
  EXPECT_BYTES_EQ(cleartext, raw_key);
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsRawSymkeyWrapUnwrapErrors)) {
  const std::vector<uint8> data(64, 0);
  blink::WebCryptoAlgorithm key_algorithm =
      CreateHmacImportAlgorithm(blink::WebCryptoAlgorithmIdSha256);

  // Import the RSA key pair.
  blink::WebCryptoAlgorithm wrapping_algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey public_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      HexStringToBytes(kPublicKeySpkiDerHex),
      HexStringToBytes(kPrivateKeyPkcs8DerHex),
      wrapping_algorithm,
      false,
      blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
      &public_key,
      &private_key);

  // Import the symmetric key.
  blink::WebCryptoKey key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(data),
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &key));

  // Wrapping with a private key should fail.
  std::vector<uint8> wrapped_key;
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            WrapKey(blink::WebCryptoKeyFormatRaw,
                    key,
                    private_key,
                    wrapping_algorithm,
                    &wrapped_key));

  // Wrapping a key whose raw keying material is too large for the wrapping key
  // should fail.
  // RSAES can encrypt data up to length of k - 11 bytes, where k is the octet
  // length of the RSA modulus, and can decrypt data up to length k. Fabricate a
  // big piece of data here that fails both of these criteria, so it can be used
  // for both wrap and unwrap negative tests below.
  const std::vector<uint8> big_data(kModulusLengthBits / 8 + 1, 0);
  blink::WebCryptoKey big_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(big_data),
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &big_key));
  EXPECT_EQ(Status::ErrorDataTooLarge(),
            WrapKey(blink::WebCryptoKeyFormatRaw,
                    big_key,
                    public_key,
                    wrapping_algorithm,
                    &wrapped_key));

  // Unwrapping with a public key should fail.
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(Status::ErrorUnexpectedKeyType(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(data),
                      public_key,
                      wrapping_algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &unwrapped_key));

  // Unwrapping empty data should fail.
  const std::vector<uint8> emtpy_data;
  EXPECT_EQ(Status::ErrorDataTooSmall(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(emtpy_data),
                      private_key,
                      wrapping_algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &unwrapped_key));

  // Unwrapping data too large for the wrapping key should fail.
  EXPECT_EQ(Status::ErrorDataTooLarge(),
            UnwrapKey(blink::WebCryptoKeyFormatRaw,
                      CryptoData(big_data),
                      private_key,
                      wrapping_algorithm,
                      key_algorithm,
                      true,
                      blink::WebCryptoKeyUsageSign,
                      &unwrapped_key));
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsJwkSymkeyUnwrapKnownAnswer)) {
  // The following data lists a known 128-bit AES-CBC key, then a JWK
  // representation of this key that was encrypted ("wrapped") using
  // RSAES-PKCS1-v1_5 and kPublicKeySpkiDerHex as the wrapping key.
  // For reference, the intermediate clear JWK is
  // {"alg":"A128CBC","ext":true,"k":<b64url>,"key_ops":["encrypt"],"kty":"oct"}
  const std::vector<uint8> key_data =
      HexStringToBytes("8f56a26e7e8b77dca15ed54339724bf5");
  const std::vector<uint8> wrapped_key_data = HexStringToBytes(
      "9debcabd9c731d6a779622dbef38635419c409b3077af67b3cf0601b2da7054f2ec26156"
      "06bb764e4986f45dd09ce660432a7abbac48b5249924f12dea52275b6d67d8b8a2f63525"
      "fbbf67d61244c1afa9e30857b87b7a48cdc0b3196dc1477738cbf9e42ea65d5e0edc3b05"
      "afafadc7d7400e26a51270d251040d51ce46cecc");
  const blink::WebCryptoAlgorithm wrapping_algorithm =
      webcrypto::CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);

  // Import the private wrapping key.
  blink::WebCryptoKey private_wrapping_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(HexStringToBytes(kPrivateKeyPkcs8DerHex)),
                      wrapping_algorithm,
                      false,
                      blink::WebCryptoKeyUsageDecrypt |
                          blink::WebCryptoKeyUsageUnwrapKey,
                      &private_wrapping_key));

  // Unwrap the key.
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(Status::Success(),
            UnwrapKey(blink::WebCryptoKeyFormatJwk,
                      CryptoData(wrapped_key_data),
                      private_wrapping_key,
                      wrapping_algorithm,
                      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                      true,
                      blink::WebCryptoKeyUsageEncrypt,
                      &unwrapped_key));
  EXPECT_FALSE(unwrapped_key.isNull());
  EXPECT_TRUE(unwrapped_key.handle());
  EXPECT_EQ(blink::WebCryptoKeyTypeSecret, unwrapped_key.type());
  EXPECT_EQ(blink::WebCryptoAlgorithmIdAesCbc, unwrapped_key.algorithm().id());
  EXPECT_EQ(true, unwrapped_key.extractable());
  EXPECT_EQ(blink::WebCryptoKeyUsageEncrypt, unwrapped_key.usages());

  // Export the unwrapped key and compare to the original.
  std::vector<uint8> raw_key;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, unwrapped_key, &raw_key));
  EXPECT_BYTES_EQ(key_data, raw_key);
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsJwkSymkeyWrapUnwrapRoundTrip)) {
  // Generate the symkey to be wrapped (256-bit AES-CBC key).
  const blink::WebCryptoAlgorithm gen_algorithm =
      CreateAesCbcKeyGenAlgorithm(256);
  blink::WebCryptoKey key_to_wrap = blink::WebCryptoKey::createNull();
  ASSERT_EQ(
      Status::Success(),
      GenerateSecretKey(
          gen_algorithm, true, blink::WebCryptoKeyUsageEncrypt, &key_to_wrap));

  // Import the wrapping key pair.
  const blink::WebCryptoAlgorithm wrapping_algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey public_wrapping_key = blink::WebCryptoKey::createNull();
  blink::WebCryptoKey private_wrapping_key = blink::WebCryptoKey::createNull();
  ImportRsaKeyPair(
      HexStringToBytes(kPublicKeySpkiDerHex),
      HexStringToBytes(kPrivateKeyPkcs8DerHex),
      wrapping_algorithm,
      false,
      blink::WebCryptoKeyUsageWrapKey | blink::WebCryptoKeyUsageUnwrapKey,
      &public_wrapping_key,
      &private_wrapping_key);

  // Wrap the symkey in JWK format, using the public wrapping key.
  std::vector<uint8> wrapped_data;
  ASSERT_EQ(Status::Success(),
            WrapKey(blink::WebCryptoKeyFormatJwk,
                    key_to_wrap,
                    public_wrapping_key,
                    wrapping_algorithm,
                    &wrapped_data));

  // Unwrap the key using the private wrapping key.
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            UnwrapKey(blink::WebCryptoKeyFormatJwk,
                      CryptoData(wrapped_data),
                      private_wrapping_key,
                      wrapping_algorithm,
                      CreateAlgorithm(blink::WebCryptoAlgorithmIdAesCbc),
                      true,
                      blink::WebCryptoKeyUsageEncrypt,
                      &unwrapped_key));

  // Export the original symkey and the unwrapped key and compare.
  std::vector<uint8> raw_key1, raw_key2;
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, key_to_wrap, &raw_key1));
  EXPECT_EQ(Status::Success(),
            ExportKey(blink::WebCryptoKeyFormatRaw, unwrapped_key, &raw_key2));
  EXPECT_BYTES_EQ(raw_key1, raw_key2);
}

TEST_F(SharedCryptoTest, MAYBE(RsaEsJwkSymkeyWrapUnwrapErrors)) {
  // Unwrap JWK-formatted data that can be successfully decrypted, but contains
  // an error in the plaintext JWK so it cannot be subsequently imported, and
  // ensure that a generic error is returned instead of some other more specific
  // error. This shows that information about the plaintext JWK inside the
  // encrypted data is not leaked.
  // Note that it is sufficient to consider just one JWK import failure mode
  // here; others are validated in the ImportJwkFailures Test. The specific
  // error in the cleartext data below is kty = "foo", which is an invalid kty
  // value.
  const std::string cleartext =
      "{\"alg\":\"A128CBC\",\"ext\":true,\"k\":"
      "\"j1aibn6Ld9yhXtVDOXJL9Q\",\"key_ops\":[\"encrypt\"],\"kty\":\"foo\"}";
  // ciphertext is the cleartext above encrypted with kPublicKeySpkiDerHex, and
  // can be decrypted with kPrivateKeyPkcs8DerHex
  const std::vector<uint8> ciphertext = HexStringToBytes(
      "93bc7bb2ca8502fcf3224e19b12ba455ac32d01695611022c76d3dbdd797c044de047d44"
      "6c5ed5de5b8f79147ffe1df8da9c894b58881b238d39bd24cecd5c1a98a7c0b07354aee6"
      "24791b2d549b7ecf1219c49513a1bcbb0fac5c6b59d350b564c44dc3678dadf84b4ea3d1"
      "32e576e88f8d4a2d27c173e033a97bbda7e47bb9");

  // Import the private decryption key.
  const blink::WebCryptoAlgorithm algorithm =
      CreateAlgorithm(blink::WebCryptoAlgorithmIdRsaEsPkcs1v1_5);
  blink::WebCryptoKey private_decryption_key =
      blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(HexStringToBytes(kPrivateKeyPkcs8DerHex)),
                      algorithm,
                      false,
                      blink::WebCryptoKeyUsageDecrypt,
                      &private_decryption_key));

  // Decrypt the ciphertext and validate the result, to prove that decryption is
  // successful.
  std::vector<uint8> decrypted_data;
  ASSERT_EQ(Status::Success(),
            Decrypt(algorithm,
                    private_decryption_key,
                    CryptoData(ciphertext),
                    &decrypted_data));
  EXPECT_BYTES_EQ(cleartext, decrypted_data);

  // Import the private wrapping key. Note this is the same underlying keying
  // material used for private_decryption_key above. The only difference is that
  // it has unwrap rather than decrypt usage.
  blink::WebCryptoKey private_wrapping_key = blink::WebCryptoKey::createNull();
  ASSERT_EQ(Status::Success(),
            ImportKey(blink::WebCryptoKeyFormatPkcs8,
                      CryptoData(HexStringToBytes(kPrivateKeyPkcs8DerHex)),
                      algorithm,
                      false,
                      blink::WebCryptoKeyUsageUnwrapKey,
                      &private_wrapping_key));

  // Treat the ciphertext as a wrapped key and try to unwrap it. Ensure a
  // generic error is received.
  blink::WebCryptoKey unwrapped_key = blink::WebCryptoKey::createNull();
  EXPECT_EQ(Status::OperationError(),
            UnwrapKey(blink::WebCryptoKeyFormatJwk,
                      CryptoData(ciphertext),
                      private_wrapping_key,
                      algorithm,
                      CreateAesCbcAlgorithm(std::vector<uint8>(0, 16)),
                      true,
                      blink::WebCryptoKeyUsageEncrypt,
                      &unwrapped_key));
}

}  // namespace webcrypto

}  // namespace content
