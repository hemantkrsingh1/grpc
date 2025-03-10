//
// Copyright 2021 gRPC authors.
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

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/fault_injection/service_config_parser.h"

#include <algorithm>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"

#include "src/core/ext/filters/fault_injection/fault_injection_filter.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/status_util.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/json/json_util.h"

namespace grpc_core {

namespace {

std::vector<FaultInjectionMethodParsedConfig::FaultInjectionPolicy>
ParseFaultInjectionPolicy(const Json::Array& policies_json_array,
                          std::vector<grpc_error_handle>* error_list) {
  std::vector<FaultInjectionMethodParsedConfig::FaultInjectionPolicy> policies;
  for (size_t i = 0; i < policies_json_array.size(); i++) {
    FaultInjectionMethodParsedConfig::FaultInjectionPolicy
        fault_injection_policy;
    std::vector<grpc_error_handle> sub_error_list;
    if (policies_json_array[i].type() != Json::Type::OBJECT) {
      error_list->push_back(GRPC_ERROR_CREATE(absl::StrCat(
          "faultInjectionPolicy index ", i, " is not a JSON object")));
      continue;
    }
    const Json::Object& json_object = policies_json_array[i].object_value();
    // Parse abort_code
    std::string abort_code_string;
    if (ParseJsonObjectField(json_object, "abortCode", &abort_code_string,
                             &sub_error_list, false)) {
      if (!grpc_status_code_from_string(abort_code_string.c_str(),
                                        &(fault_injection_policy.abort_code))) {
        sub_error_list.push_back(GRPC_ERROR_CREATE(
            "field:abortCode error:failed to parse status code"));
      }
    }
    // Parse abort_message
    if (!ParseJsonObjectField(json_object, "abortMessage",
                              &fault_injection_policy.abort_message,
                              &sub_error_list, false)) {
      fault_injection_policy.abort_message = "Fault injected";
    }
    // Parse abort_code_header
    ParseJsonObjectField(json_object, "abortCodeHeader",
                         &fault_injection_policy.abort_code_header,
                         &sub_error_list, false);
    // Parse abort_percentage_header
    ParseJsonObjectField(json_object, "abortPercentageHeader",
                         &fault_injection_policy.abort_percentage_header,
                         &sub_error_list, false);
    // Parse abort_percentage_numerator
    ParseJsonObjectField(json_object, "abortPercentageNumerator",
                         &fault_injection_policy.abort_percentage_numerator,
                         &sub_error_list, false);
    // Parse abort_percentage_denominator
    if (ParseJsonObjectField(
            json_object, "abortPercentageDenominator",
            &fault_injection_policy.abort_percentage_denominator,
            &sub_error_list, false)) {
      if (fault_injection_policy.abort_percentage_denominator != 100 &&
          fault_injection_policy.abort_percentage_denominator != 10000 &&
          fault_injection_policy.abort_percentage_denominator != 1000000) {
        sub_error_list.push_back(GRPC_ERROR_CREATE(
            "field:abortPercentageDenominator error:Denominator can only be "
            "one of "
            "100, 10000, 1000000"));
      }
    }
    // Parse delay
    ParseJsonObjectFieldAsDuration(json_object, "delay",
                                   &fault_injection_policy.delay,
                                   &sub_error_list, false);
    // Parse delay_header
    ParseJsonObjectField(json_object, "delayHeader",
                         &fault_injection_policy.delay_header, &sub_error_list,
                         false);
    // Parse delay_percentage_header
    ParseJsonObjectField(json_object, "delayPercentageHeader",
                         &fault_injection_policy.delay_percentage_header,
                         &sub_error_list, false);
    // Parse delay_percentage_numerator
    ParseJsonObjectField(json_object, "delayPercentageNumerator",
                         &fault_injection_policy.delay_percentage_numerator,
                         &sub_error_list, false);
    // Parse delay_percentage_denominator
    if (ParseJsonObjectField(
            json_object, "delayPercentageDenominator",
            &fault_injection_policy.delay_percentage_denominator,
            &sub_error_list, false)) {
      if (fault_injection_policy.delay_percentage_denominator != 100 &&
          fault_injection_policy.delay_percentage_denominator != 10000 &&
          fault_injection_policy.delay_percentage_denominator != 1000000) {
        sub_error_list.push_back(GRPC_ERROR_CREATE(
            "field:delayPercentageDenominator error:Denominator can only be "
            "one of "
            "100, 10000, 1000000"));
      }
    }
    // Parse max_faults
    static_assert(
        std::is_unsigned<decltype(fault_injection_policy.max_faults)>::value,
        "maxFaults should be unsigned");
    ParseJsonObjectField(json_object, "maxFaults",
                         &fault_injection_policy.max_faults, &sub_error_list,
                         false);
    if (!sub_error_list.empty()) {
      error_list->push_back(GRPC_ERROR_CREATE_FROM_VECTOR(
          absl::StrCat("failed to parse faultInjectionPolicy index ", i),
          &sub_error_list));
    }
    policies.push_back(std::move(fault_injection_policy));
  }
  return policies;
}

}  // namespace

absl::StatusOr<std::unique_ptr<ServiceConfigParser::ParsedConfig>>
FaultInjectionServiceConfigParser::ParsePerMethodParams(const ChannelArgs& args,
                                                        const Json& json) {
  // Only parse fault injection policy if the following channel arg is present.
  if (!args.GetBool(GRPC_ARG_PARSE_FAULT_INJECTION_METHOD_CONFIG)
           .value_or(false)) {
    return nullptr;
  }
  // Parse fault injection policy from given Json
  std::vector<FaultInjectionMethodParsedConfig::FaultInjectionPolicy>
      fault_injection_policies;
  std::vector<grpc_error_handle> error_list;
  const Json::Array* policies_json_array;
  if (ParseJsonObjectField(json.object_value(), "faultInjectionPolicy",
                           &policies_json_array, &error_list)) {
    fault_injection_policies =
        ParseFaultInjectionPolicy(*policies_json_array, &error_list);
  }
  if (!error_list.empty()) {
    grpc_error_handle error =
        GRPC_ERROR_CREATE_FROM_VECTOR("Fault injection parser", &error_list);
    absl::Status status = absl::InvalidArgumentError(
        absl::StrCat("error parsing fault injection method parameters: ",
                     grpc_error_std_string(error)));
    return status;
  }
  if (fault_injection_policies.empty()) return nullptr;
  return std::make_unique<FaultInjectionMethodParsedConfig>(
      std::move(fault_injection_policies));
}

void FaultInjectionServiceConfigParser::Register(
    CoreConfiguration::Builder* builder) {
  builder->service_config_parser()->RegisterParser(
      std::make_unique<FaultInjectionServiceConfigParser>());
}

size_t FaultInjectionServiceConfigParser::ParserIndex() {
  return CoreConfiguration::Get().service_config_parser().GetParserIndex(
      parser_name());
}

}  // namespace grpc_core
