#include "envoy/config/core/v3/base.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"
#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.validate.h"
#include "envoy/type/v3/percent.pb.h"

#include "common/buffer/buffer_impl.h"
#include "common/http/date_provider_impl.h"

#include "extensions/filters/network/http_connection_manager/config.h"

#include "test/mocks/config/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/server/mocks.h"
#include "test/test_common/printers.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::An;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace HttpConnectionManager {

envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager
parseHttpConnectionManagerFromV2Yaml(const std::string& yaml) {
  envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager
      http_connection_manager;
  TestUtility::loadFromYamlAndValidate(yaml, http_connection_manager);
  return http_connection_manager;
}

class HttpConnectionManagerConfigTest : public testing::Test {
public:
  HttpConnectionManagerConfigTest() {
    ON_CALL(context_, getServerFactoryContext()).WillByDefault(ReturnRef(server_context_));
  }
  NiceMock<Server::Configuration::MockFactoryContext> context_;
  NiceMock<Server::Configuration::MockServerFactoryContext> server_context_;
  Http::SlowDateProviderImpl date_provider_{context_.dispatcher().timeSource()};
  NiceMock<Router::MockRouteConfigProviderManager> route_config_provider_manager_;
  NiceMock<Config::MockConfigProviderManager> scoped_routes_config_provider_manager_;
};

TEST_F(HttpConnectionManagerConfigTest, ValidateFail) {
  EXPECT_THROW(
      HttpConnectionManagerFilterConfigFactory().createFilterFactoryFromProto(
          envoy::extensions::filters::network::http_connection_manager::v3::HttpConnectionManager(),
          context_),
      ProtoValidationException);
}

TEST_F(HttpConnectionManagerConfigTest, InvalidFilterName) {
  const std::string yaml_string = R"EOF(
codec_type: http1
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
http_filters:
- name: foo
  )EOF";

  EXPECT_THROW_WITH_MESSAGE(
      HttpConnectionManagerConfig(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                  date_provider_, route_config_provider_manager_,
                                  scoped_routes_config_provider_manager_),
      EnvoyException, "Didn't find a registered implementation for name: 'foo'");
}

TEST_F(HttpConnectionManagerConfigTest, RouterInverted) {
  const std::string yaml_string = R"EOF(
codec_type: http1
server_name: foo
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
http_filters:
- name: envoy.filters.http.router
- name: envoy.filters.http.health_check
  typed_config:
    "@type": type.googleapis.com/envoy.config.filter.http.health_check.v2.HealthCheck
    pass_through_mode: false
  )EOF";

  EXPECT_THROW_WITH_MESSAGE(
      HttpConnectionManagerConfig(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                  date_provider_, route_config_provider_manager_,
                                  scoped_routes_config_provider_manager_),
      EnvoyException, "Error: envoy.filters.http.router must be the terminal http filter.");
}

TEST_F(HttpConnectionManagerConfigTest, NonTerminalFilter) {
  const std::string yaml_string = R"EOF(
codec_type: http1
server_name: foo
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
http_filters:
- name: envoy.filters.http.health_check
  typed_config:
    "@type": type.googleapis.com/envoy.config.filter.http.health_check.v2.HealthCheck
    pass_through_mode: false
  )EOF";

  EXPECT_THROW_WITH_MESSAGE(
      HttpConnectionManagerConfig(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                  date_provider_, route_config_provider_manager_,
                                  scoped_routes_config_provider_manager_),
      EnvoyException,
      "Error: non-terminal filter envoy.filters.http.health_check is the last filter in a http "
      "filter chain.");
}

TEST_F(HttpConnectionManagerConfigTest, MiscConfig) {
  const std::string yaml_string = R"EOF(
codec_type: http1
server_name: foo
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
tracing:
  operation_name: ingress
  max_path_tag_length: 128
http_filters:
- name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  EXPECT_EQ(128, config.tracingConfig()->max_path_tag_length_);
  EXPECT_EQ(*context_.local_info_.address_, config.localAddress());
  EXPECT_EQ("foo", config.serverName());
  EXPECT_EQ(HttpConnectionManagerConfig::HttpConnectionManagerProto::OVERWRITE,
            config.serverHeaderTransformation());
  EXPECT_EQ(5 * 60 * 1000, config.streamIdleTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, TracingCustomTagsConfig) {
  const std::string yaml_string = R"EOF(
stat_prefix: router
route_config:
  name: local_route
tracing:
  custom_tags:
  - tag: ltag
    literal:
      value: lvalue
  - tag: etag
    environment:
      name: E_TAG
  - tag: rtag
    request_header:
      name: X-Tag
  - tag: mtag
    metadata:
      kind: { request: {} }
      metadata_key:
        key: com.bar.foo
        path: [ { key: xx }, { key: yy } ]
  )EOF";
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  std::vector<std::string> custom_tags{"ltag", "etag", "rtag", "mtag"};
  const Tracing::CustomTagMap& custom_tag_map = config.tracingConfig()->custom_tags_;
  for (const std::string& custom_tag : custom_tags) {
    EXPECT_NE(custom_tag_map.find(custom_tag), custom_tag_map.end());
  }
}

TEST_F(HttpConnectionManagerConfigTest, DEPRECATED_FEATURE_TEST(RequestHeaderForTagsConfig)) {
  const std::string yaml_string = R"EOF(
stat_prefix: router
route_config:
  name: local_route
tracing:
  request_headers_for_tags:
  - foo
  )EOF";
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  const Tracing::CustomTagMap& custom_tag_map = config.tracingConfig()->custom_tags_;
  const Tracing::RequestHeaderCustomTag* foo = dynamic_cast<const Tracing::RequestHeaderCustomTag*>(
      custom_tag_map.find("foo")->second.get());
  EXPECT_NE(foo, nullptr);
  EXPECT_EQ(foo->tag(), "foo");
}

TEST_F(HttpConnectionManagerConfigTest,
       DEPRECATED_FEATURE_TEST(ListenerDirectionOutboundOverride)) {
  const std::string yaml_string = R"EOF(
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
tracing:
  operation_name: ingress
http_filters:
- name: envoy.filters.http.router
  )EOF";

  ON_CALL(context_, direction()).WillByDefault(Return(envoy::config::core::v3::OUTBOUND));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(Tracing::OperationName::Egress, config.tracingConfig()->operation_name_);
}

TEST_F(HttpConnectionManagerConfigTest, DEPRECATED_FEATURE_TEST(ListenerDirectionInboundOverride)) {
  const std::string yaml_string = R"EOF(
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
tracing:
  operation_name: egress
http_filters:
- name: envoy.filters.http.router
  )EOF";

  ON_CALL(context_, direction()).WillByDefault(Return(envoy::config::core::v3::INBOUND));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(Tracing::OperationName::Ingress, config.tracingConfig()->operation_name_);
}

TEST_F(HttpConnectionManagerConfigTest, SamplingDefault) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  internal_address_config:
    unix_sockets: true
  route_config:
    name: local_route
  tracing:
    operation_name: ingress
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  EXPECT_EQ(100, config.tracingConfig()->client_sampling_.numerator());
  EXPECT_EQ(Tracing::DefaultMaxPathTagLength, config.tracingConfig()->max_path_tag_length_);
  EXPECT_EQ(envoy::type::v3::FractionalPercent::HUNDRED,
            config.tracingConfig()->client_sampling_.denominator());
  EXPECT_EQ(10000, config.tracingConfig()->random_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::TEN_THOUSAND,
            config.tracingConfig()->random_sampling_.denominator());
  EXPECT_EQ(100, config.tracingConfig()->overall_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::HUNDRED,
            config.tracingConfig()->overall_sampling_.denominator());
}

TEST_F(HttpConnectionManagerConfigTest, SamplingConfigured) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  internal_address_config:
    unix_sockets: true
  route_config:
    name: local_route
  tracing:
    operation_name: ingress
    client_sampling:
      value: 1
    random_sampling:
      value: 2
    overall_sampling:
      value: 3
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  EXPECT_EQ(1, config.tracingConfig()->client_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::HUNDRED,
            config.tracingConfig()->client_sampling_.denominator());
  EXPECT_EQ(200, config.tracingConfig()->random_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::TEN_THOUSAND,
            config.tracingConfig()->random_sampling_.denominator());
  EXPECT_EQ(3, config.tracingConfig()->overall_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::HUNDRED,
            config.tracingConfig()->overall_sampling_.denominator());
}

TEST_F(HttpConnectionManagerConfigTest, FractionalSamplingConfigured) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  internal_address_config:
    unix_sockets: true
  route_config:
    name: local_route
  tracing:
    operation_name: ingress
    client_sampling:
      value: 0.1
    random_sampling:
      value: 0.2
    overall_sampling:
      value: 0.3
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  EXPECT_EQ(0, config.tracingConfig()->client_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::HUNDRED,
            config.tracingConfig()->client_sampling_.denominator());
  EXPECT_EQ(20, config.tracingConfig()->random_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::TEN_THOUSAND,
            config.tracingConfig()->random_sampling_.denominator());
  EXPECT_EQ(0, config.tracingConfig()->overall_sampling_.numerator());
  EXPECT_EQ(envoy::type::v3::FractionalPercent::HUNDRED,
            config.tracingConfig()->overall_sampling_.denominator());
}

TEST_F(HttpConnectionManagerConfigTest, UnixSocketInternalAddress) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  internal_address_config:
    unix_sockets: true
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  Network::Address::PipeInstance unixAddress{"/foo"};
  Network::Address::Ipv4Instance internalIpAddress{"127.0.0.1", 0};
  Network::Address::Ipv4Instance externalIpAddress{"12.0.0.1", 0};
  EXPECT_TRUE(config.internalAddressConfig().isInternalAddress(unixAddress));
  EXPECT_TRUE(config.internalAddressConfig().isInternalAddress(internalIpAddress));
  EXPECT_FALSE(config.internalAddressConfig().isInternalAddress(externalIpAddress));
}

TEST_F(HttpConnectionManagerConfigTest, MaxRequestHeadersKbDefault) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(60, config.maxRequestHeadersKb());
}

TEST_F(HttpConnectionManagerConfigTest, MaxRequestHeadersKbConfigured) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  max_request_headers_kb: 16
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(16, config.maxRequestHeadersKb());
}

TEST_F(HttpConnectionManagerConfigTest, MaxRequestHeadersKbMaxConfigurable) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  max_request_headers_kb: 96
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(96, config.maxRequestHeadersKb());
}

// Validated that an explicit zero stream idle timeout disables.
TEST_F(HttpConnectionManagerConfigTest, DisabledStreamIdleTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  stream_idle_timeout: 0s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(0, config.streamIdleTimeout().count());
}

// Validate that deprecated idle_timeout is still ingested.
TEST_F(HttpConnectionManagerConfigTest, DEPRECATED_FEATURE_TEST(IdleTimeout)) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  idle_timeout: 1s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(1000, config.idleTimeout().value().count());
}

// Validate that idle_timeout set in common_http_protocol_options is used.
TEST_F(HttpConnectionManagerConfigTest, CommonHttpProtocolIdleTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  common_http_protocol_options:
    idle_timeout: 1s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(1000, config.idleTimeout().value().count());
}

// Validate that idle_timeout defaults to 1h
TEST_F(HttpConnectionManagerConfigTest, CommonHttpProtocolIdleTimeoutDefault) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(std::chrono::hours(1), config.idleTimeout().value());
}

// Validate that idle_timeouts can be turned off
TEST_F(HttpConnectionManagerConfigTest, CommonHttpProtocolIdleTimeoutOff) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  common_http_protocol_options:
    idle_timeout: 0s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_FALSE(config.idleTimeout().has_value());
}

// Check that the default max request header count is 100.
TEST_F(HttpConnectionManagerConfigTest, DefaultMaxRequestHeaderCount) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(100, config.maxRequestHeadersCount());
}

// Check that max request header count is configured.
TEST_F(HttpConnectionManagerConfigTest, MaxRequestHeaderCountConfigurable) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  common_http_protocol_options:
    max_headers_count: 200
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(200, config.maxRequestHeadersCount());
}

TEST_F(HttpConnectionManagerConfigTest, ServerOverwrite) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  server_header_transformation: OVERWRITE
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_, featureEnabled(_, An<uint64_t>()))
      .WillOnce(Invoke(&context_.runtime_loader_.snapshot_,
                       &Runtime::MockSnapshot::featureEnabledDefault));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(HttpConnectionManagerConfig::HttpConnectionManagerProto::OVERWRITE,
            config.serverHeaderTransformation());
}

TEST_F(HttpConnectionManagerConfigTest, ServerAppendIfAbsent) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  server_header_transformation: APPEND_IF_ABSENT
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_, featureEnabled(_, An<uint64_t>()))
      .WillOnce(Invoke(&context_.runtime_loader_.snapshot_,
                       &Runtime::MockSnapshot::featureEnabledDefault));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(HttpConnectionManagerConfig::HttpConnectionManagerProto::APPEND_IF_ABSENT,
            config.serverHeaderTransformation());
}

TEST_F(HttpConnectionManagerConfigTest, ServerPassThrough) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  server_header_transformation: PASS_THROUGH
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_, featureEnabled(_, An<uint64_t>()))
      .WillOnce(Invoke(&context_.runtime_loader_.snapshot_,
                       &Runtime::MockSnapshot::featureEnabledDefault));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(HttpConnectionManagerConfig::HttpConnectionManagerProto::PASS_THROUGH,
            config.serverHeaderTransformation());
}

// Validated that by default we don't normalize paths
// unless set build flag path_normalization_by_default=true
TEST_F(HttpConnectionManagerConfigTest, NormalizePathDefault) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_, featureEnabled(_, An<uint64_t>()))
      .WillOnce(Invoke(&context_.runtime_loader_.snapshot_,
                       &Runtime::MockSnapshot::featureEnabledDefault));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
#ifdef ENVOY_NORMALIZE_PATH_BY_DEFAULT
  EXPECT_TRUE(config.shouldNormalizePath());
#else
  EXPECT_FALSE(config.shouldNormalizePath());
#endif
}

// Validated that we normalize paths with runtime override when not specified.
TEST_F(HttpConnectionManagerConfigTest, NormalizePathRuntime) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_,
              featureEnabled("http_connection_manager.normalize_path", An<uint64_t>()))
      .WillOnce(Return(true));
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_TRUE(config.shouldNormalizePath());
}

// Validated that when configured, we normalize paths, ignoring runtime.
TEST_F(HttpConnectionManagerConfigTest, NormalizePathTrue) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  normalize_path: true
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_,
              featureEnabled("http_connection_manager.normalize_path", An<uint64_t>()))
      .Times(0);
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_TRUE(config.shouldNormalizePath());
}

// Validated that when explicitly set false, we don't normalize, ignoring runtime.
TEST_F(HttpConnectionManagerConfigTest, NormalizePathFalse) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  normalize_path: false
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  EXPECT_CALL(context_.runtime_loader_.snapshot_,
              featureEnabled("http_connection_manager.normalize_path", An<uint64_t>()))
      .Times(0);
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_FALSE(config.shouldNormalizePath());
}

// Validated that by default we don't merge slashes.
TEST_F(HttpConnectionManagerConfigTest, MergeSlashesDefault) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_FALSE(config.shouldMergeSlashes());
}

// Validated that when configured, we merge slashes.
TEST_F(HttpConnectionManagerConfigTest, MergeSlashesTrue) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  merge_slashes: true
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_TRUE(config.shouldMergeSlashes());
}

// Validated that when explicitly set false, we don't merge slashes.
TEST_F(HttpConnectionManagerConfigTest, MergeSlashesFalse) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  merge_slashes: false
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_FALSE(config.shouldMergeSlashes());
}

TEST_F(HttpConnectionManagerConfigTest, ConfiguredRequestTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  request_timeout: 53s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(53 * 1000, config.requestTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, DisabledRequestTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  request_timeout: 0s
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(0, config.requestTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, UnconfiguredRequestTimeout) {
  const std::string yaml_string = R"EOF(
  stat_prefix: ingress_http
  route_config:
    name: local_route
  http_filters:
  - name: envoy.filters.http.router
  )EOF";

  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(yaml_string), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);
  EXPECT_EQ(0, config.requestTimeout().count());
}

TEST_F(HttpConnectionManagerConfigTest, SingleDateProvider) {
  const std::string yaml_string = R"EOF(
codec_type: http1
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
http_filters:
- name: envoy.filters.http.dynamo
  typed_config:
    "@type": type.googleapis.com/google.protobuf.Empty
- name: envoy.filters.http.router
  )EOF";

  auto proto_config = parseHttpConnectionManagerFromV2Yaml(yaml_string);
  HttpConnectionManagerFilterConfigFactory factory;
  // We expect a single slot allocation vs. multiple.
  EXPECT_CALL(context_.thread_local_, allocateSlot());
  Network::FilterFactoryCb cb1 = factory.createFilterFactoryFromProto(proto_config, context_);
  Network::FilterFactoryCb cb2 = factory.createFilterFactoryFromProto(proto_config, context_);
  EXPECT_TRUE(factory.isTerminalFilter());
}

TEST_F(HttpConnectionManagerConfigTest, BadHttpConnectionMangerConfig) {
  std::string yaml_string = R"EOF(
codec_type: http1
stat_prefix: my_stat_prefix
route_config:
  virtual_hosts:
  - name: default
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: fake_cluster
filter:
- {}
  )EOF";

  EXPECT_THROW(parseHttpConnectionManagerFromV2Yaml(yaml_string), EnvoyException);
}

TEST_F(HttpConnectionManagerConfigTest, BadAccessLogConfig) {
  std::string yaml_string = R"EOF(
codec_type: http1
stat_prefix: my_stat_prefix
route_config:
  virtual_hosts:
  - name: default
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: fake_cluster
http_filters:
- name: envoy.filters.http.dynamo
  config: {}
access_log:
- name: accesslog
  typed_config:
    "@type": type.googleapis.com/envoy.config.accesslog.v2.FileAccessLog
    path: "/dev/null"
  filter: []
  )EOF";

  EXPECT_THROW_WITH_REGEX(parseHttpConnectionManagerFromV2Yaml(yaml_string), EnvoyException,
                          "filter: Proto field is not repeating, cannot start list.");
}

TEST_F(HttpConnectionManagerConfigTest, BadAccessLogType) {
  std::string yaml_string = R"EOF(
codec_type: http1
stat_prefix: my_stat_prefix
route_config:
  virtual_hosts:
  - name: default
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: fake_cluster
http_filters:
- name: envoy.filters.http.dynamo
  typed_config: {}
access_log:
- name: accesslog
  typed_config:
    "@type": type.googleapis.com/envoy.config.accesslog.v2.FileAccessLog
    path: "/dev/null"
  filter:
    bad_type: {}
  )EOF";

  EXPECT_THROW_WITH_REGEX(parseHttpConnectionManagerFromV2Yaml(yaml_string), EnvoyException,
                          "bad_type: Cannot find field");
}

TEST_F(HttpConnectionManagerConfigTest, BadAccessLogNestedTypes) {
  std::string yaml_string = R"EOF(
codec_type: http1
stat_prefix: my_stat_prefix
route_config:
  virtual_hosts:
  - name: default
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: fake_cluster
http_filters:
- name: envoy.filters.http.dynamo
  typed_config: {}
access_log:
- name: accesslog
  typed_config:
    "@type": type.googleapis.com/envoy.config.accesslog.v2.FileAccessLog
    path: "/dev/null"
  filter:
    and_filter:
      filters:
      - or_filter:
          filters:
          - duration_filter:
              op: ">="
              value: 10000
          - bad_type: {}
      - not_health_check_filter: {}
  )EOF";

  EXPECT_THROW_WITH_REGEX(parseHttpConnectionManagerFromV2Yaml(yaml_string), EnvoyException,
                          "bad_type: Cannot find field");
}

// Test that the deprecated extension name still functions.
TEST_F(HttpConnectionManagerConfigTest, DEPRECATED_FEATURE_TEST(DeprecatedExtensionFilterName)) {
  const std::string deprecated_name = "envoy.http_connection_manager";

  ASSERT_NE(
      nullptr,
      Registry::FactoryRegistry<Server::Configuration::NamedNetworkFilterConfigFactory>::getFactory(
          deprecated_name));
}

class FilterChainTest : public HttpConnectionManagerConfigTest {
public:
  const std::string basic_config_ = R"EOF(
codec_type: http1
server_name: foo
stat_prefix: router
route_config:
  virtual_hosts:
  - name: service
    domains:
    - "*"
    routes:
    - match:
        prefix: "/"
      route:
        cluster: cluster
http_filters:
- name: envoy.filters.http.dynamo
- name: envoy.filters.http.router

  )EOF";
};

TEST_F(FilterChainTest, CreateFilterChain) {
  HttpConnectionManagerConfig config(parseHttpConnectionManagerFromV2Yaml(basic_config_), context_,
                                     date_provider_, route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  Http::MockFilterChainFactoryCallbacks callbacks;
  EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
  EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
  config.createFilterChain(callbacks);
}

// Tests where upgrades are configured on via the HCM.
TEST_F(FilterChainTest, CreateUpgradeFilterChain) {
  auto hcm_config = parseHttpConnectionManagerFromV2Yaml(basic_config_);
  hcm_config.add_upgrade_configs()->set_upgrade_type("websocket");

  HttpConnectionManagerConfig config(hcm_config, context_, date_provider_,
                                     route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  NiceMock<Http::MockFilterChainFactoryCallbacks> callbacks;
  // Check the case where WebSockets are configured in the HCM, and no router
  // config is present. We should create an upgrade filter chain for
  // WebSockets.
  {
    EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    EXPECT_TRUE(config.createUpgradeFilterChain("WEBSOCKET", nullptr, callbacks));
  }

  // Check the case where WebSockets are configured in the HCM, and no router
  // config is present. We should not create an upgrade filter chain for Foo
  {
    EXPECT_CALL(callbacks, addStreamFilter(_)).Times(0);
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)).Times(0);
    EXPECT_FALSE(config.createUpgradeFilterChain("foo", nullptr, callbacks));
  }

  // Now override the HCM with a route-specific disabling of WebSocket to
  // verify route-specific disabling works.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", false));
    EXPECT_FALSE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }

  // For paranoia's sake make sure route-specific enabling doesn't break
  // anything.
  {
    EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", true));
    EXPECT_TRUE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }
}

// Tests where upgrades are configured off via the HCM.
TEST_F(FilterChainTest, CreateUpgradeFilterChainHCMDisabled) {
  auto hcm_config = parseHttpConnectionManagerFromV2Yaml(basic_config_);
  hcm_config.add_upgrade_configs()->set_upgrade_type("websocket");
  hcm_config.mutable_upgrade_configs(0)->mutable_enabled()->set_value(false);

  HttpConnectionManagerConfig config(hcm_config, context_, date_provider_,
                                     route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  NiceMock<Http::MockFilterChainFactoryCallbacks> callbacks;
  // Check the case where WebSockets are off in the HCM, and no router config is present.
  { EXPECT_FALSE(config.createUpgradeFilterChain("WEBSOCKET", nullptr, callbacks)); }

  // Check the case where WebSockets are off in the HCM and in router config.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", false));
    EXPECT_FALSE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }

  // With a route-specific enabling for WebSocket, WebSocket should work.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("WebSocket", true));
    EXPECT_TRUE(config.createUpgradeFilterChain("WEBSOCKET", &upgrade_map, callbacks));
  }

  // With only a route-config we should do what the route config says.
  {
    std::map<std::string, bool> upgrade_map;
    upgrade_map.emplace(std::make_pair("foo", true));
    upgrade_map.emplace(std::make_pair("bar", false));
    EXPECT_TRUE(config.createUpgradeFilterChain("foo", &upgrade_map, callbacks));
    EXPECT_FALSE(config.createUpgradeFilterChain("bar", &upgrade_map, callbacks));
    EXPECT_FALSE(config.createUpgradeFilterChain("eep", &upgrade_map, callbacks));
  }
}

TEST_F(FilterChainTest, CreateCustomUpgradeFilterChain) {
  auto hcm_config = parseHttpConnectionManagerFromV2Yaml(basic_config_);
  auto websocket_config = hcm_config.add_upgrade_configs();
  websocket_config->set_upgrade_type("websocket");

  ASSERT_TRUE(websocket_config->add_filters()->ParseFromString("\n"
                                                               "\x19"
                                                               "envoy.filters.http.router"));

  auto foo_config = hcm_config.add_upgrade_configs();
  foo_config->set_upgrade_type("foo");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x19"
                                             "envoy.filters.http.dynamo");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x19"
                                             "envoy.filters.http.dynamo");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x19"
                                             "envoy.filters.http.router");

  HttpConnectionManagerConfig config(hcm_config, context_, date_provider_,
                                     route_config_provider_manager_,
                                     scoped_routes_config_provider_manager_);

  {
    Http::MockFilterChainFactoryCallbacks callbacks;
    EXPECT_CALL(callbacks, addStreamFilter(_));        // Dynamo
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)); // Router
    config.createFilterChain(callbacks);
  }

  {
    Http::MockFilterChainFactoryCallbacks callbacks;
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)).Times(1);
    EXPECT_TRUE(config.createUpgradeFilterChain("websocket", nullptr, callbacks));
  }

  {
    Http::MockFilterChainFactoryCallbacks callbacks;
    EXPECT_CALL(callbacks, addStreamDecoderFilter(_)).Times(1);
    EXPECT_CALL(callbacks, addStreamFilter(_)).Times(2); // Dynamo
    EXPECT_TRUE(config.createUpgradeFilterChain("Foo", nullptr, callbacks));
  }
}

TEST_F(FilterChainTest, CreateCustomUpgradeFilterChainWithRouterNotLast) {
  auto hcm_config = parseHttpConnectionManagerFromV2Yaml(basic_config_);
  auto websocket_config = hcm_config.add_upgrade_configs();
  websocket_config->set_upgrade_type("websocket");

  ASSERT_TRUE(websocket_config->add_filters()->ParseFromString("\n"
                                                               "\x19"
                                                               "envoy.filters.http.router"));

  auto foo_config = hcm_config.add_upgrade_configs();
  foo_config->set_upgrade_type("foo");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x19"
                                             "envoy.filters.http.router");
  foo_config->add_filters()->ParseFromString("\n"
                                             "\x19"
                                             "envoy.filters.http.dynamo");

  EXPECT_THROW_WITH_MESSAGE(
      HttpConnectionManagerConfig(hcm_config, context_, date_provider_,
                                  route_config_provider_manager_,
                                  scoped_routes_config_provider_manager_),
      EnvoyException, "Error: envoy.filters.http.router must be the terminal http upgrade filter.");
}

TEST_F(FilterChainTest, InvalidConfig) {
  auto hcm_config = parseHttpConnectionManagerFromV2Yaml(basic_config_);
  hcm_config.add_upgrade_configs()->set_upgrade_type("WEBSOCKET");
  hcm_config.add_upgrade_configs()->set_upgrade_type("websocket");

  EXPECT_THROW_WITH_MESSAGE(HttpConnectionManagerConfig(hcm_config, context_, date_provider_,
                                                        route_config_provider_manager_,
                                                        scoped_routes_config_provider_manager_),
                            EnvoyException,
                            "Error: multiple upgrade configs with the same name: 'websocket'");
}

} // namespace HttpConnectionManager
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
