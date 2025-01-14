load("//tensorstore:tensorstore.bzl", "tensorstore_cc_library", "tensorstore_cc_test")

package(default_visibility = [":__subpackages__"])

licenses(["notice"])

tensorstore_cc_library(
    name = "driver_impl",
    hdrs = ["driver_impl.h"],
    deps = [
        "//tensorstore:array",
        "//tensorstore:chunk_layout",
        "//tensorstore:codec_spec",
        "//tensorstore:context",
        "//tensorstore:data_type",
        "//tensorstore:index",
        "//tensorstore:open_mode",
        "//tensorstore:resize_options",
        "//tensorstore:schema",
        "//tensorstore:staleness_bound",
        "//tensorstore:transaction",
        "//tensorstore/driver",
        "//tensorstore/driver:chunk",
        "//tensorstore/index_space:index_transform",
        "//tensorstore/internal:arena",
        "//tensorstore/internal:concurrency_resource",
        "//tensorstore/internal:data_copy_concurrency_resource",
        "//tensorstore/internal:intrusive_ptr",
        "//tensorstore/internal:json",
        "//tensorstore/internal:lock_collection",
        "//tensorstore/internal:logging",
        "//tensorstore/internal:memory",
        "//tensorstore/internal:mutex",
        "//tensorstore/internal:nditerable",
        "//tensorstore/internal:nditerable_transformed_array",
        "//tensorstore/internal:poly",
        "//tensorstore/internal:staleness_bound_json_binder",
        "//tensorstore/internal:type_traits",
        "//tensorstore/internal/cache",
        "//tensorstore/internal/cache:async_cache",
        "//tensorstore/internal/cache:async_initialized_cache_mixin",
        "//tensorstore/internal/cache:cache_key",
        "//tensorstore/internal/cache:cache_pool_resource",
        "//tensorstore/internal/cache:kvs_backed_cache",
        "//tensorstore/internal/compression:jpeg",
        "//tensorstore/internal/compression:png",
        "//tensorstore/kvstore",
        "//tensorstore/serialization:absl_time",
        "//tensorstore/util:execution",
        "//tensorstore/util:executor",
        "//tensorstore/util:future",
        "//tensorstore/util:result",
        "//tensorstore/util:sender",
        "//tensorstore/util:span",
        "//tensorstore/util:status",
        "//tensorstore/util:str_cat",
        "//tensorstore/util/garbage_collection",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings:cord",
        "@com_google_absl//absl/time",
    ],
    alwayslink = True,
)

tensorstore_cc_test(
    name = "image_test",
    size = "small",
    srcs = ["driver_test.cc"],
    deps = [
        "//tensorstore",
        "//tensorstore:context",
        "//tensorstore:index",
        "//tensorstore:open",
        "//tensorstore:progress",
        "//tensorstore:strided_layout",
        "//tensorstore/driver/image/jpeg",  # build_cleaner: keep
        "//tensorstore/driver/image/png",  # build_cleaner: keep
        "//tensorstore/index_space:dim_expression",
        "//tensorstore/internal:elementwise_function",
        "//tensorstore/internal:json_gtest",
        "//tensorstore/internal:logging",
        "//tensorstore/internal:memory",
        "//tensorstore/internal:poly",
        "//tensorstore/internal/cache",
        "//tensorstore/internal/cache:async_cache",
        "//tensorstore/internal/cache:kvs_backed_cache",
        "//tensorstore/internal/compression:jpeg",
        "//tensorstore/internal/compression:png",
        "//tensorstore/kvstore",
        "//tensorstore/kvstore/memory",  # build_cleaner: keep
        "//tensorstore/util:future",
        "//tensorstore/util:result",
        "//tensorstore/util:status",
        "//tensorstore/util:status_testutil",
        "@com_github_nlohmann_json//:nlohmann_json",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:cord",
        "@com_google_googletest//:gtest_main",
    ],
)
