// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PINS_TESTS_SFLOW_SFLOW_UTIL_H_
#define PINS_TESTS_SFLOW_SFLOW_UTIL_H_

#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "proto/gnmi/gnmi.grpc.pb.h"

namespace pins {

// Reads value from `state_path` and verifies it is the same with
// `expected_value`. Returns a FailedPreconditionError if not matched.
absl::Status VerifyGnmiStateConverged(gnmi::gNMI::StubInterface* gnmi_stub,
                                      absl::string_view state_path,
                                      absl::string_view expected_value);

// Sets sFLow sampling size to `sampling_size` and checks if it's applied to
// corresponding state path in `timeout`. Returns error if failed.
absl::Status SetSflowSamplingSize(gnmi::gNMI::StubInterface* gnmi_stub,
                                  int sampling_size,
                                  absl::Duration timeout = absl::Seconds(5));

// Sets sFlow config to `enabled` and checks if it's applied to state path in
// `timeout`. Returns error if failed.
absl::Status SetSflowConfigEnabled(gnmi::gNMI::StubInterface* gnmi_stub,
                                   bool enabled,
                                   absl::Duration timeout = absl::Seconds(5));

// Sets sFlow ingress sampling rate of `interface` to `sampling_rate` and checks
// if it's applied to corresponding state path in `timeout`. Returns error if
// failed.
absl::Status SetSflowIngressSamplingRate(
    gnmi::gNMI::StubInterface* gnmi_stub, absl::string_view interface,
    int sampling_rate, absl::Duration timeout = absl::Seconds(5));

// Verifies all sFlow-related config is consumed by switch by reading
// corresponding gNMI state paths. Returns an FailedPreconditionError if
// `agent_addr_ipv6` or `sflow_enabled_interfaces` is empty.
absl::Status VerifySflowStatesConverged(
    gnmi::gNMI::StubInterface* gnmi_stub, absl::string_view agent_addr_ipv6,
    const int sampling_rate, const int sampling_header_size,
    const std::vector<std::pair<std::string, int>>& collector_address_and_port,
    const absl::flat_hash_set<std::string>& sflow_enabled_interfaces);

// Updates `gnmi_config` with sFlow-related config and returns modified config
// if success. The modified config would sort collector IPs and interface names
// by string order. Returns an FailedPreconditionError if `agent_addr_ipv6` or
// `sflow_enabled_interfaces` is empty.
absl::StatusOr<std::string> UpdateSflowConfig(

    absl::string_view gnmi_config, absl::string_view agent_addr_ipv6,
    const std::vector<std::pair<std::string, int>>& collector_address_and_port,
    const absl::flat_hash_set<std::string>& sflow_enabled_interfaces,
    const int sampling_rate, const int sampling_header_size);

}  // namespace pins
#endif  // PINS_TESTS_SFLOW_SFLOW_UTIL_H_
