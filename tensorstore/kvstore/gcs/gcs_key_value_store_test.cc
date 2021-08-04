// Copyright 2020 The TensorStore Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stddef.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include <nlohmann/json.hpp>
#include "tensorstore/context.h"
#include "tensorstore/internal/http/curl_handle.h"
#include "tensorstore/internal/http/curl_transport.h"
#include "tensorstore/internal/http/http_request.h"
#include "tensorstore/internal/http/http_response.h"
#include "tensorstore/internal/intrusive_ptr.h"
#include "tensorstore/internal/json_gtest.h"
#include "tensorstore/internal/logging.h"
#include "tensorstore/internal/mutex.h"
#include "tensorstore/internal/path.h"
#include "tensorstore/kvstore/gcs/gcs_mock.h"
#include "tensorstore/kvstore/generation.h"
#include "tensorstore/kvstore/key_value_store.h"
#include "tensorstore/kvstore/key_value_store_testutil.h"
#include "tensorstore/util/execution.h"
#include "tensorstore/util/executor.h"
#include "tensorstore/util/future.h"
#include "tensorstore/util/sender_testutil.h"
#include "tensorstore/util/status.h"
#include "tensorstore/util/status_testutil.h"

using tensorstore::CompletionNotifyingReceiver;
using tensorstore::Context;
using tensorstore::Future;
using tensorstore::GCSMockStorageBucket;
using tensorstore::KeyRange;
using tensorstore::KeyValueStore;
using tensorstore::MatchesJson;
using tensorstore::MatchesStatus;
using tensorstore::internal_http::HttpRequest;
using tensorstore::internal_http::HttpResponse;
using tensorstore::internal_http::HttpTransport;
using tensorstore::internal_http::SetDefaultHttpTransport;

namespace {

// Responds to a "metadata.google.internal" request.
class MetadataMockTransport : public HttpTransport {
 public:
  Future<HttpResponse> IssueRequest(const HttpRequest& request,
                                    absl::Cord payload,
                                    absl::Duration request_timeout,
                                    absl::Duration connect_timeout) override {
    std::string_view scheme, host, path;
    tensorstore::internal::ParseURI(request.url(), &scheme, &host, &path);

    if (host != "metadata.google.internal") {
      return absl::UnimplementedError("Mock cannot satisfy the request.");
    }

    // Respond with the GCE OAuth2 token
    if (path == std::string_view("/computeMetadata/v1/instance/"
                                 "service-accounts/user@nowhere.com/token")) {
      return HttpResponse{
          200,
          absl::Cord(
              R"({ "token_type" : "refresh", "access_token": "abc", "expires_in": 3600 })")};
    }

    // Respond with the GCE context metadata.
    if (absl::StartsWith(path,
                         "/computeMetadata/v1/instance/service-accounts/"
                         "default/?recursive=true")) {
      return HttpResponse{
          200, absl::Cord(
                   R"({ "email": "user@nowhere.com", "scopes": [ "test" ] })")};
    }

    // Pretend to run on GCE.
    return HttpResponse{200, absl::Cord()};
  }
};

class MyMockTransport : public HttpTransport {
 public:
  Future<HttpResponse> IssueRequest(const HttpRequest& request,
                                    absl::Cord payload,
                                    absl::Duration request_timeout,
                                    absl::Duration connect_timeout) override {
    auto future = metadata_mock_.IssueRequest(request, payload, request_timeout,
                                              connect_timeout);
    if (future.result().ok()) return future;

    // Next, try each bucket until there is a success.
    for (auto* bucket : buckets_) {
      future = bucket->IssueRequest(request, payload, request_timeout,
                                    connect_timeout);
      if (future.result().ok()) return future;
    }

    return future;
  }

  MetadataMockTransport metadata_mock_;
  std::vector<GCSMockStorageBucket*> buckets_;
};

struct DefaultHttpTransportSetter {
  DefaultHttpTransportSetter(std::shared_ptr<HttpTransport> transport) {
    SetDefaultHttpTransport(transport);
  }
  ~DefaultHttpTransportSetter() { SetDefaultHttpTransport(nullptr); }
};

TEST(GcsKeyValueStoreTest, BadBucketNames) {
  auto context = Context::Default();
  for (auto bucket :
       {"a", "_abc", "abc_", "ABC", "a..b", "a.-.b",
        "a."
        "0123456789123456789012345678912345678901234567891234567890"
        "1234567891234567890123456789123456789012345678912345678901"
        "23456789123456789.b"}) {
    EXPECT_FALSE(
        KeyValueStore::Open({{"driver", "gcs"}, {"bucket", bucket}}, context)
            .result())
        << "bucket: " << bucket;
  }
  for (auto bucket : {"abc", "abc.1-2_3.abc"}) {
    EXPECT_TRUE(
        KeyValueStore::Open({{"driver", "gcs"}, {"bucket", bucket}}, context)
            .result())
        << "bucket: " << bucket;
  }
}

TEST(GcsKeyValueStoreTest, BadObjectNames) {
  using tensorstore::StorageGeneration;

  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();

  // https://www.googleapis.com/kvstore/v1/b/my-project/o/test
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());
  EXPECT_THAT(store->Read(".").result(),
              MatchesStatus(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(store->Read("..").result(),
              MatchesStatus(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(store->Read(".well-known/acme-challenge").result(),
              MatchesStatus(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(store->Read("foo\nbar").result(),
              MatchesStatus(absl::StatusCode::kInvalidArgument));
  EXPECT_THAT(store->Read("foo\rbar").result(),
              MatchesStatus(absl::StatusCode::kInvalidArgument));

  {
    KeyValueStore::ReadOptions options;
    options.if_not_equal = StorageGeneration::FromString("abc123");
    EXPECT_THAT(store->Read("abc", options).result(),
                MatchesStatus(absl::StatusCode::kInvalidArgument));
  }
}

TEST(GcsKeyValueStoreTest, Basic) {
  // Setup mocks for:
  // https://www.googleapis.com/kvstore/v1/b/my-bucket/o/test
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(auto spec, store->spec());
  EXPECT_THAT(spec.ToJson(tensorstore::IncludeDefaults{false}),
              ::testing::Optional(
                  MatchesJson({{"driver", "gcs"}, {"bucket", "my-bucket"}})));
  tensorstore::internal::TestKeyValueStoreBasicFunctionality(store);
}

TEST(GcsKeyValueStoreTest, Retry) {
  for (int max_retries : {2, 3, 4}) {
    for (bool fail : {false, true}) {
      // Setup mocks for:
      // https://www.googleapis.com/kvstore/v1/b/my-bucket/o/test
      auto mock_transport = std::make_shared<MyMockTransport>();
      DefaultHttpTransportSetter mock_transport_setter{mock_transport};

      GCSMockStorageBucket bucket("my-bucket");
      mock_transport->buckets_.push_back(&bucket);

      auto context = Context::Default();
      TENSORSTORE_ASSERT_OK_AND_ASSIGN(
          auto store,
          KeyValueStore::Open(
              {{"driver", "gcs"},
               {"bucket", "my-bucket"},
               {"context",
                {{"gcs_request_retries", {{"max_retries", max_retries}}}}}},
              context)
              .result());
      if (fail) {
        bucket.TriggerErrors(max_retries);
        EXPECT_THAT(store->Read("x").result(),
                    MatchesStatus(absl::StatusCode::kAborted));
      } else {
        bucket.TriggerErrors(max_retries - 2);
        TENSORSTORE_EXPECT_OK(store->Read("x").result());
      }
    }
  }
}

TEST(GcsKeyValueStoreTest, List) {
  // Setup mocks for:
  // https://www.googleapis.com/kvstore/v1/b/my-bucket/o/test
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());

  // Listing an empty bucket via `List` works.
  {
    absl::Notification notification;
    std::vector<std::string> log;
    tensorstore::execution::submit(
        store->List({}),
        CompletionNotifyingReceiver{&notification,
                                    tensorstore::LoggingReceiver{&log}});
    notification.WaitForNotification();
    EXPECT_THAT(log, ::testing::ElementsAre("set_starting", "set_done",
                                            "set_stopping"));
  }

  // Listing an empty bucket via `ListFuture` works.
  EXPECT_THAT(ListFuture(store.get(), {}).result(),
              ::testing::Optional(::testing::ElementsAre()));

  TENSORSTORE_EXPECT_OK(store->Write("a/b", absl::Cord("xyz")));
  TENSORSTORE_EXPECT_OK(store->Write("a/d", absl::Cord("xyz")));
  TENSORSTORE_EXPECT_OK(store->Write("a/c/x", absl::Cord("xyz")));
  TENSORSTORE_EXPECT_OK(store->Write("a/c/y", absl::Cord("xyz")));
  TENSORSTORE_EXPECT_OK(store->Write("a/c/z/e", absl::Cord("xyz")));
  TENSORSTORE_EXPECT_OK(store->Write("a/c/z/f", absl::Cord("xyz")));

  // Listing the entire stream via `List` works.
  {
    absl::Notification notification;
    std::vector<std::string> log;
    tensorstore::execution::submit(
        store->List({}),
        CompletionNotifyingReceiver{&notification,
                                    tensorstore::LoggingReceiver{&log}});
    notification.WaitForNotification();
    EXPECT_THAT(
        log, ::testing::UnorderedElementsAre(
                 "set_starting", "set_value: a/d", "set_value: a/c/z/f",
                 "set_value: a/c/y", "set_value: a/c/z/e", "set_value: a/c/x",
                 "set_value: a/b", "set_done", "set_stopping"));
  }

  // Listing the entire stream via `ListFuture` works.
  EXPECT_THAT(ListFuture(store.get(), {}).result(),
              ::testing::Optional(::testing::UnorderedElementsAre(
                  "a/d", "a/c/z/f", "a/c/y", "a/c/z/e", "a/c/x", "a/b")));

  // Listing a subset of the stream via `List` works.
  {
    absl::Notification notification;
    std::vector<std::string> log;
    tensorstore::execution::submit(
        store->List({KeyRange::Prefix("a/c/")}),
        CompletionNotifyingReceiver{&notification,
                                    tensorstore::LoggingReceiver{&log}});
    notification.WaitForNotification();
    EXPECT_THAT(log, ::testing::UnorderedElementsAre(
                         "set_starting", "set_value: a/c/z/f",
                         "set_value: a/c/y", "set_value: a/c/z/e",
                         "set_value: a/c/x", "set_done", "set_stopping"));
  }

  // Cancellation immediately after starting yields nothing..
  struct CancelOnStarting : public tensorstore::LoggingReceiver {
    void set_starting(tensorstore::AnyCancelReceiver do_cancel) {
      this->tensorstore::LoggingReceiver::set_starting({});
      do_cancel();
    }
  };

  {
    absl::Notification notification;
    std::vector<std::string> log;
    tensorstore::execution::submit(
        store->List({}),
        CompletionNotifyingReceiver{&notification, CancelOnStarting{{&log}}});
    notification.WaitForNotification();
    EXPECT_THAT(log, ::testing::ElementsAre("set_starting", "set_done",
                                            "set_stopping"));
  }

  // Cancellation in the middle of the stream stops the stream.
  // However it may not do it immediately.
  struct CancelAfter2 : public tensorstore::LoggingReceiver {
    using Key = tensorstore::KeyValueStore::Key;
    tensorstore::AnyCancelReceiver cancel;

    void set_starting(tensorstore::AnyCancelReceiver do_cancel) {
      this->cancel = std::move(do_cancel);
      this->tensorstore::LoggingReceiver::set_starting({});
    }

    void set_value(Key k) {
      this->tensorstore::LoggingReceiver::set_value(std::move(k));
      if (this->log->size() == 2) {
        this->cancel();
      }
    }
  };

  {
    absl::Notification notification;
    std::vector<std::string> log;
    tensorstore::execution::submit(
        store->List({}),
        CompletionNotifyingReceiver{&notification, CancelAfter2{{&log}}});
    notification.WaitForNotification();
    EXPECT_THAT(log, ::testing::Contains("set_starting"));
    EXPECT_THAT(log, ::testing::Contains("set_done"));
    EXPECT_THAT(log, ::testing::Contains("set_stopping"));
    // We cannot guarantee that a single value is returned, so just verify
    // that any of the following exists and the size.
    EXPECT_LE(4, log.size());
    EXPECT_THAT(
        log, ::testing::Contains(::testing::AnyOf(
                 "set_value: a/d", "set_value: a/c/z/f", "set_value: a/c/y",
                 "set_value: a/c/z/e", "set_value: a/c/x", "set_value: a/b")));
  }

  // Listing a subset of the stream via `ListFuture` works.
  EXPECT_THAT(ListFuture(store.get(), {KeyRange::Prefix("a/c/")}).result(),
              ::testing::Optional(::testing::UnorderedElementsAre(
                  "a/c/z/f", "a/c/y", "a/c/z/e", "a/c/x")));
}

TEST(GcsKeyValueStoreTest, SpecRoundtrip) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  tensorstore::internal::TestKeyValueStoreSpecRoundtrip(
      {{"driver", "gcs"}, {"bucket", "my-bucket"}});
}

TEST(GcsKeyValueStoreTest, InvalidSpec) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  auto context = tensorstore::Context::Default();

  // Test with extra key.
  EXPECT_THAT(
      KeyValueStore::Open(
          {{"driver", "gcs"}, {"bucket", "my-bucket"}, {"extra", "key"}},
          context)
          .result(),
      MatchesStatus(absl::StatusCode::kInvalidArgument));

  // Test with missing `"bucket"` key.
  EXPECT_THAT(KeyValueStore::Open({{"driver", "gcs"}}, context).result(),
              MatchesStatus(absl::StatusCode::kInvalidArgument));

  // Test with invalid `"bucket"` key.
  EXPECT_THAT(
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", 5}}, context).result(),
      MatchesStatus(absl::StatusCode::kInvalidArgument));
}

TEST(GcsKeyValueStoreTest, RequestorPays) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket1("my-bucket1");
  GCSMockStorageBucket bucket2("my-bucket2", "myproject");
  mock_transport->buckets_.push_back(&bucket1);
  mock_transport->buckets_.push_back(&bucket2);

  const auto TestWrite = [&](Context context, auto bucket2_status_matcher) {
    TENSORSTORE_ASSERT_OK_AND_ASSIGN(
        auto store1, KeyValueStore::Open(
                         {{"driver", "gcs"}, {"bucket", "my-bucket1"}}, context)
                         .result());
    TENSORSTORE_ASSERT_OK_AND_ASSIGN(
        auto store2, KeyValueStore::Open(
                         {{"driver", "gcs"}, {"bucket", "my-bucket2"}}, context)
                         .result());
    TENSORSTORE_EXPECT_OK(store1->Write("abc", absl::Cord("xyz")));
    EXPECT_THAT(GetStatus(store2->Write("abc", absl::Cord("xyz"))),
                bucket2_status_matcher);
  };

  TestWrite(Context::Default(),
            MatchesStatus(absl::StatusCode::kInvalidArgument));
  TestWrite(Context(Context::Spec::FromJson(
                        {{"gcs_user_project", {{"project_id", "badproject"}}}})
                        .value()),
            MatchesStatus(absl::StatusCode::kInvalidArgument));
  TestWrite(Context(Context::Spec::FromJson(
                        {{"gcs_user_project", {{"project_id", "myproject"}}}})
                        .value()),
            absl::OkStatus());
}

class MyConcurrentMockTransport : public MyMockTransport {
 public:
  void reset(std::size_t limit) {
    absl::MutexLock lock(&concurrent_request_mutex_);
    expected_concurrent_requests_ = limit;
    cur_concurrent_requests_ = 0;
    max_concurrent_requests_ = 0;
  }

  Future<HttpResponse> IssueRequest(const HttpRequest& request,
                                    absl::Cord payload,
                                    absl::Duration request_timeout,
                                    absl::Duration connect_timeout) override {
    std::string_view scheme, host, path;
    tensorstore::internal::ParseURI(request.url(), &scheme, &host, &path);

    // Don't do concurrency test on auth requests, as those don't happen
    // concurrently.
    if (host != "metadata.google.internal") {
      {
        absl::MutexLock lock(&concurrent_request_mutex_);
        ++cur_concurrent_requests_;
        max_concurrent_requests_ =
            std::max(max_concurrent_requests_, cur_concurrent_requests_);
        concurrent_request_mutex_.Await(absl::Condition(
            +[](MyConcurrentMockTransport* self) {
              return self->max_concurrent_requests_ ==
                     self->expected_concurrent_requests_;
            },
            this));
      }
      absl::SleepFor(kSleepAmount);
      {
        absl::MutexLock lock(&concurrent_request_mutex_);
        --cur_concurrent_requests_;
      }
    }

    return MyMockTransport::IssueRequest(request, payload, request_timeout,
                                         connect_timeout);
  }

  constexpr static auto kSleepAmount = absl::Milliseconds(10);

  std::size_t expected_concurrent_requests_ = 0;
  std::size_t cur_concurrent_requests_ = 0;
  std::size_t max_concurrent_requests_ = 0;
  absl::Mutex concurrent_request_mutex_;
};

TEST(GcsKeyValueStoreTest, Concurrency) {
  auto mock_transport = std::make_shared<MyConcurrentMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  const auto TestConcurrency = [&](size_t limit) {
    mock_transport->reset(limit);
    Context context{Context::Spec::FromJson(
                        {{"gcs_request_concurrency", {{"limit", limit}}}})
                        .value()};
    TENSORSTORE_ASSERT_OK_AND_ASSIGN(
        auto store, KeyValueStore::Open(
                        {{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
                        .result());

    std::vector<tensorstore::Future<KeyValueStore::ReadResult>> futures;
    for (size_t i = 0; i < 10 * limit; ++i) {
      futures.push_back(store->Read("abc"));
    }
    for (const auto& future : futures) {
      future.Wait();
    }
    EXPECT_EQ(limit, mock_transport->max_concurrent_requests_);
  };

  TestConcurrency(1);
  TestConcurrency(2);
  TestConcurrency(3);
}

TEST(GcsKeyValueStoreTest, DeletePrefix) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());
  tensorstore::internal::TestKeyValueStoreDeletePrefix(store);
}

TEST(GcsKeyValueStoreTest, DeleteRange) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());
  tensorstore::internal::TestKeyValueStoreDeleteRange(store);
}

TEST(GcsKeyValueStoreTest, DeleteRangeToEnd) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());
  tensorstore::internal::TestKeyValueStoreDeleteRangeToEnd(store);
}

TEST(GcsKeyValueStoreTest, DeleteRangeFromBeginning) {
  auto mock_transport = std::make_shared<MyMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open({{"driver", "gcs"}, {"bucket", "my-bucket"}}, context)
          .result());
  tensorstore::internal::TestKeyValueStoreDeleteRangeFromBeginning(store);
}

class MyDeleteRangeCancellationMockTransport : public MyMockTransport {
 public:
  Future<HttpResponse> IssueRequest(const HttpRequest& request,
                                    absl::Cord payload,
                                    absl::Duration request_timeout,
                                    absl::Duration connect_timeout) override {
    if (request.method() == "DELETE") {
      cancellation_notification_.WaitForNotification();
      ++total_delete_requests_;
    }
    return MyMockTransport::IssueRequest(request, payload, request_timeout,
                                         connect_timeout);
  }

  std::atomic<std::size_t> total_delete_requests_{0};
  absl::Notification cancellation_notification_;
};

TEST(GcsKeyValueStoreTest, DeleteRangeCancellation) {
  auto mock_transport =
      std::make_shared<MyDeleteRangeCancellationMockTransport>();
  DefaultHttpTransportSetter mock_transport_setter{mock_transport};

  GCSMockStorageBucket bucket("my-bucket");
  mock_transport->buckets_.push_back(&bucket);

  auto context = Context::Default();
  TENSORSTORE_ASSERT_OK_AND_ASSIGN(
      auto store,
      KeyValueStore::Open(
          {
              {"driver", "gcs"},
              {"bucket", "my-bucket"},
              {"context", {{"gcs_request_concurrency", {{"limit", 1}}}}},
          },
          context)
          .result());
  for (std::string key : {"a/b", "a/c/a", "a/c/b", "a/c/d", "a/d"}) {
    TENSORSTORE_ASSERT_OK(store->Write(key, absl::Cord()));
  }

  // Issue DeleteRange request and then immediately cancel it by dropping the
  // future.
  {
    [[maybe_unused]] auto future =
        store->DeleteRange(tensorstore::KeyRange{"a/ba", "a/ca"});
  }
  mock_transport->cancellation_notification_.Notify();
  // FIXME(jbms): Unfortunately our cancellation mechanism does not permit
  // waiting for the cancellation to complete, so we must use a delay as a hack.
  // Note that if the delay is not long enough, the test may pass spuriously,
  // but it won't fail spuriously.
  absl::SleepFor(absl::Milliseconds(100));
  EXPECT_GE(1, mock_transport->total_delete_requests_.load());
  EXPECT_THAT(ListFuture(store.get()).result(),
              ::testing::Optional(::testing::SizeIs(::testing::Ge(4))));
}

}  // namespace
