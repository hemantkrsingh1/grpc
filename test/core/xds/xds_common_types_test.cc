//
// Copyright 2022 gRPC authors.
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
//

#include "src/core/ext/xds/xds_common_types.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <google/protobuf/wrappers.pb.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "envoy/extensions/transport_sockets/tls/v3/tls.upb.h"
#include "gmock/gmock.h"
#include "google/protobuf/duration.upb.h"
#include "gtest/gtest.h"
#include "re2/re2.h"
#include "upb/def.hpp"
#include "upb/upb.hpp"

#include <grpc/grpc.h>
#include <grpc/support/log.h>

#include "src/core/ext/xds/xds_bootstrap.h"
#include "src/core/ext/xds/xds_bootstrap_grpc.h"
#include "src/core/ext/xds/xds_client.h"
#include "src/core/ext/xds/xds_resource_type.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/time.h"
#include "src/core/lib/gprpp/validation_errors.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/matchers/matchers.h"
#include "src/proto/grpc/testing/xds/v3/regex.pb.h"
#include "src/proto/grpc/testing/xds/v3/string.pb.h"
#include "src/proto/grpc/testing/xds/v3/tls.pb.h"
#include "test/core/util/test_config.h"

using CommonTlsContextProto =
    envoy::extensions::transport_sockets::tls::v3::CommonTlsContext;

namespace grpc_core {
namespace testing {
namespace {

TraceFlag xds_common_types_test_trace(true, "xds_common_types_test");

class XdsCommonTypesTest : public ::testing::Test {
 protected:
  XdsCommonTypesTest()
      : xds_client_(MakeXdsClient()),
        decode_context_{xds_client_.get(), xds_client_->bootstrap().server(),
                        &xds_common_types_test_trace, upb_def_pool_.ptr(),
                        upb_arena_.ptr()} {}

  static RefCountedPtr<XdsClient> MakeXdsClient() {
    grpc_error_handle error;
    auto bootstrap = GrpcXdsBootstrap::Create(
        "{\n"
        "  \"xds_servers\": [\n"
        "    {\n"
        "      \"server_uri\": \"xds.example.com\",\n"
        "      \"channel_creds\": [\n"
        "        {\"type\": \"google_default\"}\n"
        "      ]\n"
        "    }\n"
        "  ],\n"
        "  \"certificate_providers\": {\n"
        "    \"provider1\": {\n"
        "      \"plugin_name\": \"file_watcher\",\n"
        "      \"config\": {\n"
        "        \"certificate_file\": \"/path/to/cert\",\n"
        "        \"private_key_file\": \"/path/to/key\"\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}");
    if (!bootstrap.ok()) {
      gpr_log(GPR_ERROR, "Error parsing bootstrap: %s",
              bootstrap.status().ToString().c_str());
      GPR_ASSERT(false);
    }
    return MakeRefCounted<XdsClient>(std::move(*bootstrap),
                                     /*transport_factory=*/nullptr);
  }

  RefCountedPtr<XdsClient> xds_client_;
  upb::DefPool upb_def_pool_;
  upb::Arena upb_arena_;
  XdsResourceType::DecodeContext decode_context_;
};

//
// ParseDuration() tests
//

using DurationTest = XdsCommonTypesTest;

TEST_F(DurationTest, Basic) {
  google_protobuf_Duration* duration_proto =
      google_protobuf_Duration_new(upb_arena_.ptr());
  google_protobuf_Duration_set_seconds(duration_proto, 1);
  google_protobuf_Duration_set_nanos(duration_proto, 2000000);
  ValidationErrors errors;
  Duration duration = ParseDuration(duration_proto, &errors);
  EXPECT_TRUE(errors.ok()) << errors.status("unexpected errors");
  EXPECT_EQ(duration, Duration::Milliseconds(1002));
}

TEST_F(DurationTest, NegativeNumbers) {
  google_protobuf_Duration* duration_proto =
      google_protobuf_Duration_new(upb_arena_.ptr());
  google_protobuf_Duration_set_seconds(duration_proto, -1);
  google_protobuf_Duration_set_nanos(duration_proto, -2);
  ValidationErrors errors;
  ParseDuration(duration_proto, &errors);
  absl::Status status = errors.status("validation failed");
  EXPECT_EQ(status.message(),
            "validation failed: ["
            "field:nanos error:value must be in the range [0, 999999999]; "
            "field:seconds error:value must be in the range [0, 315576000000]]")
      << status;
}

TEST_F(DurationTest, ValuesTooHigh) {
  google_protobuf_Duration* duration_proto =
      google_protobuf_Duration_new(upb_arena_.ptr());
  google_protobuf_Duration_set_seconds(duration_proto, 315576000001);
  google_protobuf_Duration_set_nanos(duration_proto, 1000000000);
  ValidationErrors errors;
  ParseDuration(duration_proto, &errors);
  absl::Status status = errors.status("validation failed");
  EXPECT_EQ(status.message(),
            "validation failed: ["
            "field:nanos error:value must be in the range [0, 999999999]; "
            "field:seconds error:value must be in the range [0, 315576000000]]")
      << status;
}

//
// CommonTlsContext tests
//

class CommonTlsConfigTest : public XdsCommonTypesTest {
 protected:
  // For convenience, tests build protos using the protobuf API and then
  // use this function to convert it to a upb object, which can be
  // passed to CommonTlsConfig::Parse() for validation.
  const envoy_extensions_transport_sockets_tls_v3_CommonTlsContext*
  ConvertToUpb(CommonTlsContextProto proto) {
    // Serialize the protobuf proto.
    std::string serialized_proto;
    if (!proto.SerializeToString(&serialized_proto)) {
      EXPECT_TRUE(false) << "protobuf serialization failed";
      return nullptr;
    }
    // Deserialize as upb proto.
    const auto* upb_proto =
        envoy_extensions_transport_sockets_tls_v3_CommonTlsContext_parse(
            serialized_proto.data(), serialized_proto.size(), upb_arena_.ptr());
    if (upb_proto == nullptr) {
      EXPECT_TRUE(false) << "upb parsing failed";
      return nullptr;
    }
    return upb_proto;
  }

  absl::StatusOr<CommonTlsContext> Parse(
      const envoy_extensions_transport_sockets_tls_v3_CommonTlsContext*
          upb_proto) {
    ValidationErrors errors;
    CommonTlsContext common_tls_context =
        CommonTlsContext::Parse(decode_context_, upb_proto, &errors);
    if (!errors.ok()) return errors.status("validation failed");
    return common_tls_context;
  }
};

TEST_F(CommonTlsConfigTest, CaCertProviderInCombinedValidationContext) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* cert_provider =
      common_tls_context_proto.mutable_combined_validation_context()
          ->mutable_default_validation_context()
          ->mutable_ca_certificate_provider_instance();
  cert_provider->set_instance_name("provider1");
  cert_provider->set_certificate_name("cert_name");
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_TRUE(common_tls_context.ok()) << common_tls_context.status();
  EXPECT_EQ(common_tls_context->certificate_validation_context
                .ca_certificate_provider_instance.instance_name,
            "provider1");
  EXPECT_EQ(common_tls_context->certificate_validation_context
                .ca_certificate_provider_instance.certificate_name,
            "cert_name");
  EXPECT_THAT(common_tls_context->certificate_validation_context
                  .match_subject_alt_names,
              ::testing::ElementsAre());
  EXPECT_TRUE(common_tls_context->tls_certificate_provider_instance.Empty())
      << common_tls_context->tls_certificate_provider_instance.ToString();
}

TEST_F(CommonTlsConfigTest, CaCertProviderInValidationContext) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* cert_provider = common_tls_context_proto.mutable_validation_context()
                            ->mutable_ca_certificate_provider_instance();
  cert_provider->set_instance_name("provider1");
  cert_provider->set_certificate_name("cert_name");
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_TRUE(common_tls_context.ok()) << common_tls_context.status();
  EXPECT_EQ(common_tls_context->certificate_validation_context
                .ca_certificate_provider_instance.instance_name,
            "provider1");
  EXPECT_EQ(common_tls_context->certificate_validation_context
                .ca_certificate_provider_instance.certificate_name,
            "cert_name");
  EXPECT_THAT(common_tls_context->certificate_validation_context
                  .match_subject_alt_names,
              ::testing::ElementsAre());
  EXPECT_TRUE(common_tls_context->tls_certificate_provider_instance.Empty())
      << common_tls_context->tls_certificate_provider_instance.ToString();
}

TEST_F(CommonTlsConfigTest, ValidationSdsConfigUnsupported) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  common_tls_context_proto.mutable_validation_context_sds_secret_config();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:validation_context_sds_secret_config "
            "error:feature unsupported]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, TlsCertProvider) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* cert_provider =
      common_tls_context_proto.mutable_tls_certificate_provider_instance();
  cert_provider->set_instance_name("provider1");
  cert_provider->set_certificate_name("cert_name");
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_TRUE(common_tls_context.ok()) << common_tls_context.status();
  EXPECT_TRUE(common_tls_context->certificate_validation_context.Empty())
      << common_tls_context->certificate_validation_context.ToString();
  EXPECT_EQ(common_tls_context->tls_certificate_provider_instance.instance_name,
            "provider1");
  EXPECT_EQ(
      common_tls_context->tls_certificate_provider_instance.certificate_name,
      "cert_name");
}

TEST_F(CommonTlsConfigTest, TlsCertificatesUnuspported) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  common_tls_context_proto.add_tls_certificates();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:tls_certificates error:feature unsupported]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, TlsCertificatesSdsConfigUnuspported) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  common_tls_context_proto.add_tls_certificate_sds_secret_configs();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:tls_certificate_sds_secret_configs "
            "error:feature unsupported]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, TlsParamsUnuspported) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  common_tls_context_proto.mutable_tls_params();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:tls_params error:feature unsupported]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, CustomHandshakerUnuspported) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  common_tls_context_proto.mutable_custom_handshaker();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:custom_handshaker error:feature unsupported]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, UnknownCertificateProviderInstance) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* cert_provider = common_tls_context_proto.mutable_validation_context()
                            ->mutable_ca_certificate_provider_instance();
  cert_provider->set_instance_name("fake");
  cert_provider->set_certificate_name("cert_name");
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:validation_context.ca_certificate_provider_instance"
            ".instance_name "
            "error:unrecognized certificate provider instance name: fake]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, MatchSubjectAltNames) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* validation_context =
      common_tls_context_proto.mutable_validation_context();
  auto* string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_exact("exact");
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_prefix("prefix");
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_suffix("suffix");
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_contains("contains");
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->mutable_safe_regex()->set_regex("regex");
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_TRUE(common_tls_context.ok()) << common_tls_context.status();
  const auto& match_subject_alt_names =
      common_tls_context->certificate_validation_context
          .match_subject_alt_names;
  ASSERT_EQ(match_subject_alt_names.size(), 5);
  EXPECT_EQ(match_subject_alt_names[0].type(), StringMatcher::Type::kExact);
  EXPECT_EQ(match_subject_alt_names[0].string_matcher(), "exact");
  EXPECT_TRUE(match_subject_alt_names[0].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[1].type(), StringMatcher::Type::kPrefix);
  EXPECT_EQ(match_subject_alt_names[1].string_matcher(), "prefix");
  EXPECT_TRUE(match_subject_alt_names[1].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[2].type(), StringMatcher::Type::kSuffix);
  EXPECT_EQ(match_subject_alt_names[2].string_matcher(), "suffix");
  EXPECT_TRUE(match_subject_alt_names[2].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[3].type(), StringMatcher::Type::kContains);
  EXPECT_EQ(match_subject_alt_names[3].string_matcher(), "contains");
  EXPECT_TRUE(match_subject_alt_names[3].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[4].type(), StringMatcher::Type::kSafeRegex);
  EXPECT_EQ(match_subject_alt_names[4].regex_matcher()->pattern(), "regex");
  EXPECT_TRUE(match_subject_alt_names[4].case_sensitive());
  EXPECT_TRUE(common_tls_context->certificate_validation_context
                  .ca_certificate_provider_instance.Empty())
      << common_tls_context->certificate_validation_context
             .ca_certificate_provider_instance.ToString();
  EXPECT_TRUE(common_tls_context->tls_certificate_provider_instance.Empty())
      << common_tls_context->tls_certificate_provider_instance.ToString();
}

TEST_F(CommonTlsConfigTest, MatchSubjectAltNamesCaseInsensitive) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* validation_context =
      common_tls_context_proto.mutable_validation_context();
  auto* string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_exact("exact");
  string_matcher->set_ignore_case(true);
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_prefix("prefix");
  string_matcher->set_ignore_case(true);
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_suffix("suffix");
  string_matcher->set_ignore_case(true);
  string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->set_contains("contains");
  string_matcher->set_ignore_case(true);
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_TRUE(common_tls_context.ok()) << common_tls_context.status();
  const auto& match_subject_alt_names =
      common_tls_context->certificate_validation_context
          .match_subject_alt_names;
  ASSERT_EQ(match_subject_alt_names.size(), 4);
  EXPECT_EQ(match_subject_alt_names[0].type(), StringMatcher::Type::kExact);
  EXPECT_EQ(match_subject_alt_names[0].string_matcher(), "exact");
  EXPECT_FALSE(match_subject_alt_names[0].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[1].type(), StringMatcher::Type::kPrefix);
  EXPECT_EQ(match_subject_alt_names[1].string_matcher(), "prefix");
  EXPECT_FALSE(match_subject_alt_names[1].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[2].type(), StringMatcher::Type::kSuffix);
  EXPECT_EQ(match_subject_alt_names[2].string_matcher(), "suffix");
  EXPECT_FALSE(match_subject_alt_names[2].case_sensitive());
  EXPECT_EQ(match_subject_alt_names[3].type(), StringMatcher::Type::kContains);
  EXPECT_EQ(match_subject_alt_names[3].string_matcher(), "contains");
  EXPECT_FALSE(match_subject_alt_names[3].case_sensitive());
  EXPECT_TRUE(common_tls_context->certificate_validation_context
                  .ca_certificate_provider_instance.Empty())
      << common_tls_context->certificate_validation_context
             .ca_certificate_provider_instance.ToString();
  EXPECT_TRUE(common_tls_context->tls_certificate_provider_instance.Empty())
      << common_tls_context->tls_certificate_provider_instance.ToString();
}

TEST_F(CommonTlsConfigTest, MatchSubjectAltNamesInvalid) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* validation_context =
      common_tls_context_proto.mutable_validation_context();
  auto* string_matcher = validation_context->add_match_subject_alt_names();
  string_matcher->mutable_safe_regex()->set_regex("regex");
  string_matcher->set_ignore_case(true);
  string_matcher = validation_context->add_match_subject_alt_names();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:validation_context.match_subject_alt_names[0].ignore_case "
            "error:not supported for regex matcher; "
            "field:validation_context.match_subject_alt_names[1] "
            "error:invalid StringMatcher specified]")
      << common_tls_context.status();
}

TEST_F(CommonTlsConfigTest, ValidationContextUnsupportedFields) {
  // Construct proto.
  CommonTlsContextProto common_tls_context_proto;
  auto* validation_context =
      common_tls_context_proto.mutable_validation_context();
  validation_context->add_verify_certificate_spki("foo");
  validation_context->add_verify_certificate_hash("bar");
  validation_context->mutable_require_signed_certificate_timestamp()->set_value(
      true);
  validation_context->mutable_crl();
  validation_context->mutable_custom_validator_config();
  // Convert to upb.
  const auto* upb_proto = ConvertToUpb(common_tls_context_proto);
  ASSERT_NE(upb_proto, nullptr);
  // Run test.
  auto common_tls_context = Parse(upb_proto);
  ASSERT_FALSE(common_tls_context.ok());
  EXPECT_EQ(common_tls_context.status().message(),
            "validation failed: ["
            "field:validation_context.crl "
            "error:feature unsupported; "
            "field:validation_context.custom_validator_config "
            "error:feature unsupported; "
            "field:validation_context.require_signed_certificate_timestamp "
            "error:feature unsupported; "
            "field:validation_context.verify_certificate_hash "
            "error:feature unsupported; "
            "field:validation_context.verify_certificate_spki "
            "error:feature unsupported]")
      << common_tls_context.status();
}

}  // namespace
}  // namespace testing
}  // namespace grpc_core

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  int ret = RUN_ALL_TESTS();
  grpc_shutdown();
  return ret;
}
