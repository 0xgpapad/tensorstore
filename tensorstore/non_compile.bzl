# Copyright 2020 The TensorStore Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("//:utils.bzl", "constraint_values_config_setting")
load("//tensorstore:tensorstore.bzl", "tensorstore_cc_test")

def cc_with_non_compile_test(
        name,
        srcs,
        deps = None,
        nc_test_shard_count = 10,
        copts = None,
        **kwargs):
    # Non-compile tests are not supported

    if copts == None:
        copts = []

    if deps == None:
        deps = []

    # This should really be detecting the compiler rather than the platform, but
    # it is difficult to reliably detect the compiler.
    msvc_config_setting = constraint_values_config_setting(["@platforms//os:windows"])

    # This just turns it into a regular test.
    tensorstore_cc_test(
        name = name,
        srcs = srcs,
        local_defines = select({
            msvc_config_setting: [],
            "//conditions:default": ["EXPECT_NON_COMPILE(...)="],
        }),
        copts = copts + select({
            msvc_config_setting: ["/FI", "tensorstore/internal/non_compile_bypass.h"],
            "//conditions:default": [],
        }),
        deps = deps + select({
            msvc_config_setting: ["@com_google_tensorstore//tensorstore/internal:non_compile_bypass"],
            "//conditions:default": [],
        }),
        **kwargs
    )
