// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/iam/iam_credentials_client.gcpcxx.pb.h"
#include "google/cloud/iam/internal/iam_credentials_stub_factory.gcpcxx.pb.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/log.h"
#include "google/cloud/testing_util/assert_ok.h"
#include "google/cloud/testing_util/capture_log_lines_backend.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
inline namespace GOOGLE_CLOUD_CPP_NS {
namespace iam {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;

class IamCredentialsIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    backend_ =
        std::make_shared<google::cloud::testing_util::CaptureLogLinesBackend>();
    logger_id_ = google::cloud::LogSink::Instance().AddBackend(backend_);
    rpc_tracing_options_.enable_tracing("rpc");

    iam_service_account_ = google::cloud::internal::GetEnv(
                               "GOOGLE_CLOUD_CPP_IAM_TEST_SERVICE_ACCOUNT")
                               .value_or("");
    invalid_iam_service_account_ =
        google::cloud::internal::GetEnv(
            "GOOGLE_CLOUD_CPP_IAM_INVALID_TEST_SERVICE_ACCOUNT")
            .value_or("");

    ASSERT_FALSE(iam_service_account_.empty());
    ASSERT_FALSE(invalid_iam_service_account_.empty());
  }
  void TearDown() override {
    google::cloud::LogSink::Instance().RemoveBackend(logger_id_);
    logger_id_ = 0;
  }
  std::vector<std::string> ClearLogLines() { return backend_->ClearLogLines(); }
  IAMCredentialsConnectionOptions rpc_tracing_options_;
  std::string iam_service_account_;
  std::string invalid_iam_service_account_;

 private:
  std::shared_ptr<google::cloud::testing_util::CaptureLogLinesBackend> backend_;
  long logger_id_ = 0;  // NOLINT
};

TEST_F(IamCredentialsIntegrationTest, GenerateAccessTokenSuccess) {
  google::protobuf::Duration lifetime;
  lifetime.set_seconds(3600);
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection({}));
  auto response = client.GenerateAccessToken(
      "projects/-/serviceAccounts/" + iam_service_account_, {},
      {"https://www.googleapis.com/auth/spanner.admin"}, lifetime);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->access_token().empty());
}

TEST_F(IamCredentialsIntegrationTest, GenerateAccessTokenFailure) {
  google::protobuf::Duration lifetime;
  lifetime.set_seconds(3600);
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.GenerateAccessToken(
      "projects/-/serviceAccounts/" + invalid_iam_service_account_, {},
      {"https://www.googleapis.com/auth/spanner.admin"}, lifetime);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("GenerateAccessToken")));
}

TEST_F(IamCredentialsIntegrationTest, GenerateIdTokenSuccess) {
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection(
      IAMCredentialsConnectionOptions(),
      std::unique_ptr<IAMCredentialsRetryPolicy>(
          new IAMCredentialsLimitedTimeRetryPolicy(std::chrono::minutes(30))),
      std::unique_ptr<BackoffPolicy>(new ExponentialBackoffPolicy(
          std::chrono::seconds(1), std::chrono::minutes(5), 2.0)),
      MakeDefaultIAMCredentialsConnectionIdempotencyPolicy()));
  auto response = client.GenerateIdToken(
      "projects/-/serviceAccounts/" + iam_service_account_, {},
      {"https://www.googleapis.com/auth/spanner.admin"}, true);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->token().empty());
}

TEST_F(IamCredentialsIntegrationTest, GenerateIdTokenFailure) {
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.GenerateIdToken(
      "projects/-/serviceAccounts/" + iam_service_account_, {}, {""}, false);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("GenerateIdToken")));
}

TEST_F(IamCredentialsIntegrationTest, SignBlobSuccess) {
  std::string payload = "somebytes";
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection(
      iam_internal::CreateDefaultIAMCredentialsStub({}),
      std::unique_ptr<IAMCredentialsRetryPolicy>(
          new IAMCredentialsLimitedTimeRetryPolicy(std::chrono::minutes(30))),
      std::unique_ptr<BackoffPolicy>(new ExponentialBackoffPolicy(
          std::chrono::seconds(1), std::chrono::minutes(5), 2.0)),
      MakeDefaultIAMCredentialsConnectionIdempotencyPolicy()));
  auto response = client.SignBlob(
      "projects/-/serviceAccounts/" + iam_service_account_, {}, payload);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->key_id().empty());
  EXPECT_FALSE(response->signed_blob().empty());
}

TEST_F(IamCredentialsIntegrationTest, SignBlobFailure) {
  std::string payload = "somebytes";
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.SignBlob(
      "projects/-/serviceAccounts/" + invalid_iam_service_account_, {},
      payload);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("SignBlob")));
}

TEST_F(IamCredentialsIntegrationTest, SignJwtSuccess) {
  std::string payload = R"({"some": "json"})";
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection({}));
  auto response = client.SignJwt(
      "projects/-/serviceAccounts/" + iam_service_account_, {}, payload);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->key_id().empty());
  EXPECT_FALSE(response->signed_jwt().empty());
}

TEST_F(IamCredentialsIntegrationTest, SignJwtFailure) {
  std::string payload = R"({"some": "json"})";
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.SignJwt(
      "projects/-/serviceAccounts/" + invalid_iam_service_account_, {},
      payload);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("SignJwt")));
}

TEST_F(IamCredentialsIntegrationTest, GenerateAccessTokenProtoRequestSuccess) {
  ::google::iam::credentials::v1::GenerateAccessTokenRequest request;
  request.set_name("projects/-/serviceAccounts/" + iam_service_account_);
  *request.add_scope() = "https://www.googleapis.com/auth/spanner.admin";
  google::protobuf::Duration lifetime;
  lifetime.set_seconds(3600);
  *request.mutable_lifetime() = lifetime;
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection({}));
  auto response = client.GenerateAccessToken(request);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->access_token().empty());
}

TEST_F(IamCredentialsIntegrationTest, GenerateAccessTokenProtoRequestFailure) {
  ::google::iam::credentials::v1::GenerateAccessTokenRequest request;
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.GenerateAccessToken(request);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("GenerateAccessToken")));
}

TEST_F(IamCredentialsIntegrationTest, GenerateIdTokenProtoRequestSuccess) {
  ::google::iam::credentials::v1::GenerateIdTokenRequest request;
  request.set_name("projects/-/serviceAccounts/" + iam_service_account_);
  request.set_audience("https://www.googleapis.com/auth/spanner.admin");
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection({}));
  auto response = client.GenerateIdToken(request);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->token().empty());
}

TEST_F(IamCredentialsIntegrationTest, GenerateIdTokenProtoRequestFailure) {
  ::google::iam::credentials::v1::GenerateIdTokenRequest request;
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.GenerateIdToken(request);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("GenerateIdToken")));
}

TEST_F(IamCredentialsIntegrationTest, SignBlobProtoRequestSuccess) {
  ::google::iam::credentials::v1::SignBlobRequest request;
  request.set_name("projects/-/serviceAccounts/" + iam_service_account_);
  request.set_payload("somebytes");
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection());
  auto response = client.SignBlob(request);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->key_id().empty());
  EXPECT_FALSE(response->signed_blob().empty());
}

TEST_F(IamCredentialsIntegrationTest, SignBlobProtoRequestFailure) {
  ::google::iam::credentials::v1::SignBlobRequest request;
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.SignBlob(request);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("SignBlob")));
}

TEST_F(IamCredentialsIntegrationTest, SignJwtProtoRequestSuccess) {
  ::google::iam::credentials::v1::SignJwtRequest request;
  request.set_name("projects/-/serviceAccounts/" + iam_service_account_);
  request.set_payload(R"({"some": "json"})");
  auto client = IAMCredentialsClient(MakeIAMCredentialsConnection({}));
  auto response = client.SignJwt(request);
  EXPECT_STATUS_OK(response);
  EXPECT_FALSE(response->key_id().empty());
  EXPECT_FALSE(response->signed_jwt().empty());
}

TEST_F(IamCredentialsIntegrationTest, SignJwtProtoRequestFailure) {
  ::google::iam::credentials::v1::SignJwtRequest request;
  auto client =
      IAMCredentialsClient(MakeIAMCredentialsConnection(rpc_tracing_options_));
  auto response = client.SignJwt(request);
  EXPECT_FALSE(response.status().ok());
  auto const log_lines = ClearLogLines();
  EXPECT_THAT(log_lines, Contains(HasSubstr("SignJwt")));
}

}  // namespace
}  // namespace iam
}  // namespace GOOGLE_CLOUD_CPP_NS
}  // namespace cloud
}  // namespace google
