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

#include "tensorstore/internal/oauth2/gce_auth_provider.h"

#include <stdint.h>

#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include <nlohmann/json.hpp>
#include "tensorstore/internal/env.h"
#include "tensorstore/internal/http/curl_handle.h"
#include "tensorstore/internal/http/curl_transport.h"
#include "tensorstore/internal/http/http_request.h"
#include "tensorstore/internal/http/http_response.h"
#include "tensorstore/internal/json.h"
#include "tensorstore/internal/oauth2/oauth_utils.h"
#include "tensorstore/internal/path.h"
#include "tensorstore/internal/retry.h"
#include "tensorstore/util/result.h"
#include "tensorstore/util/status.h"

using tensorstore::internal_http::HttpRequestBuilder;
using tensorstore::internal_http::HttpResponse;

namespace jb = tensorstore::internal_json_binding;

namespace tensorstore {
namespace internal_oauth2 {
namespace {

// Using GCE-based credentials
// 1. Run the process on GCE.
// 2. Avoid setting GOOGLE_APPLICATION_CREDENTIALS,
//    and avoid credentials in the well-known location of
//    .config/gcloud/application_default_credentials.json
// 3. The GCE metadata service will return credentials for <self>.

// The ServiceAccountInfo is returned from a GCE metadata call to:
// "metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/",
struct ServiceAccountInfo {
  std::string email;
  std::vector<std::string> scopes;
};

constexpr static auto ServiceAccountInfoBinder = jb::Object(
    jb::Member("email", jb::Projection(&ServiceAccountInfo::email,
                                       jb::NonEmptyStringBinder)),

    // Note that the "scopes" attribute will always be present and contain a
    // JSON array. At minimum, for the request to succeed, the instance must
    // have been granted the scope that allows it to retrieve info from the
    // metadata server.
    jb::Member("scopes", jb::Projection(&ServiceAccountInfo::scopes)),
    jb::DiscardExtraMembers);

}  // namespace

std::string GceMetadataHostname() {
  auto maybe_hostname = internal::GetEnv("GCE_METADATA_ROOT");
  if (maybe_hostname.has_value()) {
    return std::move(*maybe_hostname);
  }
  return "metadata.google.internal";
}

GceAuthProvider::GceAuthProvider(
    std::shared_ptr<internal_http::HttpTransport> transport)
    : GceAuthProvider(std::move(transport), &absl::Now) {}

GceAuthProvider::GceAuthProvider(
    std::shared_ptr<internal_http::HttpTransport> transport,
    std::function<absl::Time()> clock)
    : service_account_email_("default"),
      expiration_(absl::InfinitePast()),
      transport_(std::move(transport)),
      clock_(std::move(clock)) {}

Result<GceAuthProvider::BearerTokenWithExpiration> GceAuthProvider::GetToken() {
  if (!IsValid()) {
    auto status = Refresh();
    TENSORSTORE_RETURN_IF_ERROR(status);
  }
  return BearerTokenWithExpiration{access_token_, expiration_};
}

Result<HttpResponse> GceAuthProvider::IssueRequest(std::string path,
                                                   bool recursive) {
  HttpRequestBuilder request_builder(
      "GET", internal::JoinPath("http://", GceMetadataHostname(), path));
  request_builder.AddHeader("Metadata-Flavor: Google");
  if (recursive) {
    request_builder.AddQueryParameter("recursive", "true");
  }
  return transport_->IssueRequest(request_builder.BuildRequest(), {}).result();
}

Status GceAuthProvider::RetrieveServiceAccountInfo() {
  TENSORSTORE_ASSIGN_OR_RETURN(
      auto response,
      IssueRequest(
          absl::StrCat("/computeMetadata/v1/instance/service-accounts/",
                       service_account_email_, "/"),
          true));
  TENSORSTORE_RETURN_IF_ERROR(HttpResponseCodeToStatus(response));

  auto info_response = internal::ParseJson(response.payload.Flatten());
  if (info_response.is_discarded()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Failed to parse service account response: ",
                     response.payload.Flatten()));
  }

  TENSORSTORE_ASSIGN_OR_RETURN(auto service_info,
                               jb::FromJson<ServiceAccountInfo>(
                                   info_response, ServiceAccountInfoBinder));

  service_account_email_ = std::move(service_info.email);
  scopes_ = std::set<std::string>(service_info.scopes.begin(),
                                  service_info.scopes.end());
  return absl::OkStatus();
}

Status GceAuthProvider::Refresh() {
  TENSORSTORE_RETURN_IF_ERROR(RetrieveServiceAccountInfo());

  const auto now = clock_();
  TENSORSTORE_ASSIGN_OR_RETURN(
      auto response,
      IssueRequest(
          absl::StrCat("/computeMetadata/v1/instance/service-accounts/",
                       service_account_email_, "/token"),
          false));
  TENSORSTORE_RETURN_IF_ERROR(HttpResponseCodeToStatus(response));
  TENSORSTORE_ASSIGN_OR_RETURN(auto result, internal_oauth2::ParseOAuthResponse(
                                                response.payload.Flatten()));
  expiration_ = now + absl::Seconds(result.expires_in);
  access_token_ = std::move(result.access_token);
  return absl::OkStatus();
}

}  // namespace internal_oauth2
}  // namespace tensorstore
