# Copyright 2022 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("//bazel:grpc_build_system.bzl", "grpc_cc_test", "grpc_package")
load("//test/core/util:grpc_fuzzer.bzl", "grpc_proto_fuzzer")

licenses(["notice"])

grpc_package(
    name = "test/core/event_engine/work_queue",
    visibility = "tests",
)

grpc_cc_test(
    name = "work_queue_test",
    srcs = ["work_queue_test.cc"],
    external_deps = ["gtest"],
    deps = [
        "//:common_event_engine_closures",
        "//:event_engine_work_queue",
        "//:exec_ctx",
        "//:gpr_platform",
        "//test/core/util:grpc_test_util_unsecure",
    ],
)

grpc_proto_fuzzer(
    name = "work_queue_fuzzer",
    srcs = ["work_queue_fuzzer.cc"],
    corpus = "corpora",
    language = "C++",
    proto = "work_queue_fuzzer.proto",
    tags = ["no_windows"],
    uses_event_engine = False,
    uses_polling = False,
    deps = [
        "//:event_engine_work_queue",
        "//test/core/util:grpc_test_util",
    ],
)
