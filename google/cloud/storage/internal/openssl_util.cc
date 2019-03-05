// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/storage/internal/openssl_util.h"
#include "google/cloud/internal/throw_delegate.h"
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#ifdef OPENSSL_IS_BORINGSSL
#include <openssl/base64.h>
#endif  // OPENSSL_IS_BORINGSSL
#include <memory>
#include <sstream>

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {

namespace {
// The name of the function to free an EVP_MD_CTX changed in OpenSSL 1.1.0.
#if (OPENSSL_VERSION_NUMBER < 0x10100000L)  // Older than version 1.1.0.
inline std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_destroy)>
GetDigestCtx() {
  return std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_destroy)>(
      EVP_MD_CTX_create(), &EVP_MD_CTX_destroy);
};
#else
inline std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> GetDigestCtx() {
  return std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
};
#endif

#ifndef OPENSSL_IS_BORINGSSL
std::unique_ptr<BIO, decltype(&BIO_free_all)>
MakeBioChainForBase64Transcoding() {
  auto base64_io = std::unique_ptr<BIO, decltype(&BIO_free)>(
      BIO_new(BIO_f_base64()), &BIO_free);
  auto mem_io = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()),
                                                          &BIO_free);
  if (!(base64_io && mem_io)) {
    std::ostringstream err_builder;
    err_builder << "Permanent error in " << __func__ << ": "
                << "Could not allocate BIO* for Base64 encoding.";
    google::cloud::internal::ThrowRuntimeError(err_builder.str());
  }
  auto bio_chain = std::unique_ptr<BIO, decltype(&BIO_free_all)>(
      // Output from a b64 encoder should go to an in-memory sink.
      BIO_push(static_cast<BIO*>(base64_io.release()),
               static_cast<BIO*>(mem_io.release())),
      // Make sure we free all resources in this chain upon destruction.
      &BIO_free_all);
  // Don't use newlines as a signal for when to flush buffers.
  BIO_set_flags(static_cast<BIO*>(bio_chain.get()), BIO_FLAGS_BASE64_NO_NL);
  return bio_chain;
}
#endif  // OPENSSL_IS_BORINGSSL

std::string Base64Encode(std::uint8_t const* bytes, std::size_t bytes_size) {
#ifdef OPENSSL_IS_BORINGSSL
  std::size_t encoded_size;
  EVP_EncodedLength(&encoded_size, bytes_size);
  std::vector<std::uint8_t> result(encoded_size);
  std::size_t out_size = EVP_EncodeBlock(result.data(), bytes, bytes_size);
  result.resize(out_size);
  return {result.begin(), result.end()};
#else
  auto bio_chain = MakeBioChainForBase64Transcoding();
  int retval = 0;

  while (true) {
    retval = BIO_write(static_cast<BIO*>(bio_chain.get()), bytes,
                       static_cast<int>(bytes_size));
    if (retval > 0) {
      break;  // Positive value == successful write.
    }
    if (!BIO_should_retry(static_cast<BIO*>(bio_chain.get()))) {
      std::ostringstream err_builder;
      err_builder << "Permanent error in " << __func__ << ": "
                  << "BIO_write returned non-retryable value of " << retval;
      google::cloud::internal::ThrowRuntimeError(err_builder.str());
    }
  }
  // Tell the b64 encoder that we're done writing data, thus prompting it to
  // add trailing '=' characters for padding if needed.
  while (true) {
    retval = BIO_flush(static_cast<BIO*>(bio_chain.get()));
    if (retval > 0) {
      break;  // Positive value == successful flush.
    }
    if (!BIO_should_retry(static_cast<BIO*>(bio_chain.get()))) {
      std::ostringstream err_builder;
      err_builder << "Permanent error in " << __func__ << ": "
                  << "BIO_flush returned non-retryable value of " << retval;
      google::cloud::internal::ThrowRuntimeError(err_builder.str());
    }
  }

  // This buffer belongs to the BIO chain and is freed upon its destruction.
  BUF_MEM* buf_mem;
  BIO_get_mem_ptr(static_cast<BIO*>(bio_chain.get()), &buf_mem);
  // Return a string copy of the buffer's bytes, as the buffer will be freed
  // upon this method's exit.
  return std::string(buf_mem->data, buf_mem->length);
#endif  // OPENSSL_IS_BORINGSSL
}
}  // namespace

std::string Base64Decode(std::string const& str) {
#ifdef OPENSSL_IS_BORINGSSL
  std::size_t decoded_size;
  EVP_DecodedLength(&decoded_size, str.size());
  std::string result(decoded_size, '\0');
  EVP_DecodeBase64(
      reinterpret_cast<std::uint8_t*>(&result[0]), &decoded_size, result.size(),
      reinterpret_cast<std::uint8_t const*>(str.data()), str.size());
  result.resize(decoded_size);
  return result;
#else
  if (str.empty()) {
    return std::string{};
  }

  // We could compute the exact buffer size by looking at the number of padding
  // characters (=) at the end of str, but we will get the exact length later,
  // so simply compute a buffer that is big enough.
  std::string result(str.size() * 3 / 4, ' ');

  using UniqueBioChainPtr = std::unique_ptr<BIO, decltype(&BIO_free_all)>;
  using UniqueBioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

  UniqueBioPtr source(BIO_new_mem_buf(const_cast<char*>(str.data()),
                                      static_cast<int>(str.size())),
                      &BIO_free);
  if (!source) {
    std::ostringstream os;
    os << __func__ << ": cannot create BIO for source string=<" << str << ">";
    google::cloud::internal::ThrowRuntimeError(os.str());
  }
  UniqueBioPtr filter(BIO_new(BIO_f_base64()), &BIO_free);
  if (!filter) {
    std::ostringstream os;
    os << __func__ << ": cannot create BIO for Base64 decoding";
    google::cloud::internal::ThrowRuntimeError(os.str());
  }

  // Based on the documentation this never fails, so we can transfer ownership
  // of `filter` and `source` and do not need to check the result.
  UniqueBioChainPtr bio(BIO_push(filter.release(), source.release()),
                        &BIO_free_all);
  BIO_set_flags(bio.get(), BIO_FLAGS_BASE64_NO_NL);

  // We do not retry, just make one call because the full stream is blocking.
  // Note that the number of bytes to read is the number of bytes we fetch from
  // the *source*, not the number of bytes that we have available in `result`.
  int len = BIO_read(bio.get(), &result[0], static_cast<int>(str.size()));
  if (len < 0) {
    std::ostringstream os;
    os << "Error parsing Base64 string [" << len << "], string=<" << str << ">";
    google::cloud::internal::ThrowRuntimeError(os.str());
  }

  result.resize(static_cast<std::size_t>(len));
  return result;
#endif  // OPENSSL_IS_BORINGSSL
}

std::string Base64Encode(std::string const& str) {
  return Base64Encode(reinterpret_cast<unsigned char const*>(str.data()),
                      str.size());
}

std::string Base64Encode(std::vector<std::uint8_t> const& bytes) {
  return Base64Encode(bytes.data(), bytes.size());
}

std::vector<std::uint8_t> SignStringWithPem(
    std::string const& str, std::string const& pem_contents,
    storage::oauth2::JwtSigningAlgorithms alg) {
  using ::google::cloud::storage::oauth2::JwtSigningAlgorithms;

  // We check for failures several times, so we shorten this into a lambda
  // to avoid bloating the code with alloc/init checks.
  const char* func_name = __func__;  // Avoid using the lambda name instead.
  auto handle_openssl_failure = [&func_name](const char* error_msg) -> void {
    std::ostringstream err_builder;
    err_builder << "Permanent error in " << func_name
                << " (failed to sign string with PEM key):\n"
                << error_msg;
    google::cloud::internal::ThrowRuntimeError(err_builder.str());
  };

  auto digest_ctx = GetDigestCtx();
  if (!digest_ctx) {
    handle_openssl_failure("Could not create context for OpenSSL digest.");
  }

  EVP_MD const* digest_type = nullptr;
  switch (alg) {
    case JwtSigningAlgorithms::RS256:
      digest_type = EVP_sha256();
      break;
  }
  if (digest_type == nullptr) {
    handle_openssl_failure("Could not find specified digest in OpenSSL.");
  }

  auto pem_buffer = std::unique_ptr<BIO, decltype(&BIO_free)>(
      BIO_new_mem_buf(const_cast<char*>(pem_contents.c_str()),
                      static_cast<int>(pem_contents.length())),
      &BIO_free);
  if (!pem_buffer) {
    handle_openssl_failure("Could not create PEM buffer.");
  }

  auto private_key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
      PEM_read_bio_PrivateKey(
          static_cast<BIO*>(pem_buffer.get()),
          nullptr,  // EVP_PKEY **x
          nullptr,  // pem_password_cb *cb -- a custom callback.
          // void *u -- this represents the password for the PEM (only
          // applicable for formats such as PKCS12 (.p12 files) that use
          // a password, which we don't currently support.
          nullptr),
      &EVP_PKEY_free);
  if (!private_key) {
    handle_openssl_failure("Could not parse PEM to get private key.");
  }

  int const digest_sign_success_code = 1;
  if (digest_sign_success_code !=
      EVP_DigestSignInit(static_cast<EVP_MD_CTX*>(digest_ctx.get()),
                         nullptr,  // EVP_PKEY_CTX **pctx
                         digest_type,
                         nullptr,  // ENGINE *e
                         static_cast<EVP_PKEY*>(private_key.get()))) {
    handle_openssl_failure("Could not initialize PEM digest.");
  }

  if (digest_sign_success_code !=
      EVP_DigestSignUpdate(static_cast<EVP_MD_CTX*>(digest_ctx.get()),
                           str.c_str(), str.length())) {
    handle_openssl_failure("Could not update PEM digest.");
  }

  std::size_t signed_str_size = 0;
  // Calling this method with a nullptr buffer will populate our size var
  // with the resulting buffer's size. This allows us to then call it again,
  // with the correct buffer and size, which actually populates the buffer.
  if (digest_sign_success_code !=
      EVP_DigestSignFinal(static_cast<EVP_MD_CTX*>(digest_ctx.get()),
                          nullptr,  // unsigned char *sig
                          &signed_str_size)) {
    handle_openssl_failure("Could not finalize PEM digest (1/2).");
  }

  std::vector<unsigned char> signed_str(signed_str_size);
  if (digest_sign_success_code !=
      EVP_DigestSignFinal(static_cast<EVP_MD_CTX*>(digest_ctx.get()),
                          signed_str.data(), &signed_str_size)) {
    handle_openssl_failure("Could not finalize PEM digest (2/2).");
  }

  return {signed_str.begin(), signed_str.end()};
}

}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google