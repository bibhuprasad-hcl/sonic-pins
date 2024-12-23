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

#include "tests/sflow/sflow_test.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>  // NOLINT: Need threads (instead of fiber) for upstream code.
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "glog/logging.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "gutil/collections.h"
#include "gutil/status.h"
#include "gutil/status_matchers.h"
#include "gutil/testing.h"
#include "lib/gnmi/gnmi_helper.h"
#include "lib/ixia_helper.h"
#include "lib/utils/json_utils.h"
#include "lib/validator/validator_lib.h"
#include "p4_pdpi/p4_runtime_session.h"
#include "p4_pdpi/packetlib/packetlib.h"
#include "p4_pdpi/pd.h"
#include "sai_p4/instantiations/google/sai_pd.pb.h"
#include "tests/forwarding/group_programming_util.h"
#include "tests/forwarding/util.h"
#include "tests/lib/p4rt_fixed_table_programming_helper.h"
#include "tests/lib/switch_test_setup_helpers.h"
#include "tests/qos/gnmi_parsers.h"
#include "tests/sflow/sflow_util.h"
#include "thinkit/generic_testbed.h"
#include "thinkit/mirror_testbed.h"
#include "thinkit/ssh_client.h"

namespace pins {

namespace {

using ::gutil::IsOkAndHolds;

// Number of packets sent to one port.
constexpr int kPacketsNum = 4000000;

// Number of packets sent per second.
constexpr int kPacketsPerSecond = 16000;

// The maximum number of bytes that should be copied from a sampled packet to
// the sFlow datagram.
constexpr int kSampleSize = 512;

// Once accumulated data reaches kMaxPacketSize, sFlow would generate an sFlow
// datagram.
constexpr int kMaxPacketSize = 1400;

// Sflowtool binary name in the collector.
constexpr absl::string_view kSflowToolName = "sflowtool";

constexpr absl::string_view kSflowtoolLineFormatTemplate =
    "/etc/init.d/sflow-container exec '$0 -l -p 6343 &"
    " pid=$$!; sleep $1; kill -9 $$pid;'";

constexpr absl::string_view kSflowtoolFullFormatTemplate =
    "/etc/init.d/sflow-container exec '$0 -p 6343 &"
    " pid=$$!; sleep $1; kill -9 $$pid;'";

// IpV4 address for filtering the sFlow packet.
constexpr uint32_t kIpV4Src = 0x01020304;  // 1.2.3.4
// Ixia flow details.
constexpr auto kDstMac = netaddr::MacAddress(02, 02, 02, 02, 02, 03);
constexpr auto kSourceMac = netaddr::MacAddress(00, 01, 02, 03, 04, 05);
constexpr auto kIpV4Dst = netaddr::Ipv4Address(192, 168, 10, 1);

constexpr int kSamplingRateInterval = 4000;

// Buffering and software bottlenecks can cause some amount of variance in rate
// measured end to end.
// Level of tolerance for packet rate verification.
// This could be parameterized in future if this is platform dependent.
constexpr double kTolerance = 0.15;

// Vrf prefix used in the test.
constexpr absl::string_view kVrfIdPrefix = "vrf-";

// Returns IP address in dot-decimal notation, e.g. "192.168.2.1".
std::string GetSrcIpv4AddrByPortId(const int port_id) {
  return netaddr::Ipv4Address(std::bitset<32>(kIpV4Src + port_id)).ToString();
}

// Sets ACL punt rule according to `port_id`.
absl::Status SetUpAclPunt(pdpi::P4RuntimeSession& p4_session,
                          const pdpi::IrP4Info& ir_p4info, int port_id) {
  ASSIGN_OR_RETURN(
      p4::v1::TableEntry pi_entry,
      pdpi::PartialPdTableEntryToPiTableEntry(
          ir_p4info, gutil::ParseProtoOrDie<sai::TableEntry>(absl::Substitute(
                         R"pb(
                           acl_ingress_table_entry {
                             match {
                               dst_mac { value: "$0" mask: "ff:ff:ff:ff:ff:ff" }
                               is_ipv4 { value: "0x1" }
                               src_ip { value: "$1" mask: "255.255.255.255" }
                               dst_ip { value: "$2" mask: "255.255.255.255" }
                             }
                             action { acl_trap { qos_queue: "0x7" } }
                             priority: 1
                           }
                         )pb",
                         kDstMac.ToString(), GetSrcIpv4AddrByPortId(port_id),
                         kIpV4Dst.ToString()))));
  return pdpi::InstallPiTableEntry(&p4_session, pi_entry);
}

// Sets ACL drop rule according to `port_id`.
absl::Status SetUpAclDrop(pdpi::P4RuntimeSession& p4_session,
                          const pdpi::IrP4Info& ir_p4info, int port_id) {
  ASSIGN_OR_RETURN(
      p4::v1::TableEntry pi_entry,
      pdpi::PartialPdTableEntryToPiTableEntry(
          ir_p4info, gutil::ParseProtoOrDie<sai::TableEntry>(absl::Substitute(
                         R"pb(
                           acl_ingress_table_entry {
                             match {
                               dst_mac { value: "$0" mask: "ff:ff:ff:ff:ff:ff" }
                               is_ipv4 { value: "0x1" }
                               src_ip { value: "$1" mask: "255.255.255.255" }
                               dst_ip { value: "$2" mask: "255.255.255.255" }
                             }
                             action { acl_drop {} }
                             priority: 1
                           }
                         )pb",
                         kDstMac.ToString(), GetSrcIpv4AddrByPortId(port_id),
                         kIpV4Dst.ToString()))));
  return pdpi::InstallPiTableEntry(&p4_session, pi_entry);
}

// Sets VRF according to port number. The pattern would be vrf-x (x=port id).
absl::Status SetSutVrf(pdpi::P4RuntimeSession& p4_session,
                       const p4::config::v1::P4Info& p4info,
                       const pdpi::IrP4Info& ir_p4info,
                       absl::Span<const int> port_ids) {
  for (int i = 0; i < port_ids.size(); ++i) {
    // Create default VRF for test.
    ASSIGN_OR_RETURN(
        p4::v1::TableEntry pi_entry,
        pdpi::PartialPdTableEntryToPiTableEntry(
            ir_p4info, gutil::ParseProtoOrDie<sai::TableEntry>(absl::Substitute(
                           R"pb(
                             vrf_table_entry {
                               match { vrf_id: "$0" }
                               action { no_action {} }
                             })pb",
                           absl::StrCat(kVrfIdPrefix, port_ids[i])))));
    RETURN_IF_ERROR(pdpi::InstallPiTableEntry(&p4_session, pi_entry));

    ASSIGN_OR_RETURN(
        pi_entry,
        pdpi::PartialPdTableEntryToPiTableEntry(
            ir_p4info,
            gutil::ParseProtoOrDie<sai::TableEntry>(absl::Substitute(
                R"pb(
                  acl_pre_ingress_table_entry {
                    match { in_port { value: "$0" } }  # Match in port
                    action { set_vrf { vrf_id: "$1" } }
                    priority: 1
                  })pb",
                port_ids[i], absl::StrCat(kVrfIdPrefix, port_ids[i])))));
    RETURN_IF_ERROR(pdpi::InstallPiTableEntry(&p4_session, pi_entry));
  }

  return absl::OkStatus();
}

// Creates members by filling in the controller port ids.
absl::StatusOr<std::vector<GroupMember>> CreateGroupMembers(
    int group_size, absl::Span<const int> controller_port_ids) {
  if (group_size > controller_port_ids.size()) {
    return absl::InvalidArgumentError(
        absl::StrCat("Not enough members: ", controller_port_ids.size(),
                     " to create the group with size: ", group_size));
  }
  std::vector<GroupMember> members(group_size);
  for (int i = 0; i < group_size; i++) {
    // Skip weight since we don't need it in this test.
    members[i] = pins::GroupMember{.port = controller_port_ids[i]};
    LOG(INFO) << "member-" << i << " port: " << members[i].port;
  }
  return members;
}

// Program route entries using vrf_id.
absl::Status ProgramRoutes(pdpi::P4RuntimeSession& p4_session,
                           const pdpi::IrP4Info& ir_p4info, const int port_id,
                           absl::string_view next_hop_id) {
  const std::string vrf_id = absl::StrCat(kVrfIdPrefix, port_id);
  // Add set of flows to allow forwarding.
  auto ipv4_entry = gutil::ParseProtoOrDie<sai::Update>(absl::Substitute(
      R"pb(
        type: INSERT
        table_entry {
          ipv4_table_entry {
            match { vrf_id: "$0" }
            action { set_nexthop_id { nexthop_id: "$1" } }
          }
        })pb",
      vrf_id, next_hop_id));
  p4::v1::WriteRequest write_request;
  ASSIGN_OR_RETURN(
      p4::v1::Update pi_entry, pdpi::PdUpdateToPi(ir_p4info, ipv4_entry),
      _.SetPrepend() << "Failed in PD table conversion to PI, entry: "
                     << ipv4_entry.DebugString() << " error: ");
  *write_request.add_updates() = pi_entry;
  return pdpi::SetMetadataAndSendPiWriteRequest(&p4_session, write_request);
}

// Program L3 Admit table for the given mac-address.
absl::Status ProgramL3Admit(pdpi::P4RuntimeSession& p4_session,
                            const pdpi::IrP4Info& ir_p4info,
                            const L3AdmitOptions& options) {
  p4::v1::WriteRequest write_request;
  ASSIGN_OR_RETURN(
      *write_request.add_updates(),
      L3AdmitTableUpdate(ir_p4info, p4::v1::Update::INSERT, options));
  return pdpi::SetMetadataAndSendPiWriteRequest(&p4_session, write_request);
}

// These are the counters we track in these tests.
struct Counters {
  uint64_t in_pkts;
  uint64_t out_pkts;
  uint64_t in_octets;
  uint64_t out_octets;
};

absl::StatusOr<uint64_t> GetGnmiStat(std::string stat_name,
                                     absl::string_view iface,
                                     gnmi::gNMI::StubInterface* gnmi_stub) {
  std::string ops_state_path;
  std::string ops_parse_str;

  if (absl::StartsWith(stat_name, "ipv4-")) {
    std::string name = stat_name.substr(5);
    ops_state_path = absl::StrCat(
        "interfaces/interface[name=", iface,
        "]subinterfaces/subinterface[index=0]/ipv4/state/counters/", name);
    ops_parse_str = "openconfig-if-ip:" + name;
  } else if (absl::StartsWith(stat_name, "ipv6-")) {
    std::string name = stat_name.substr(5);
    ops_state_path = absl::StrCat(
        "interfaces/interface[name=", iface,
        "]subinterfaces/subinterface[index=0]/ipv6/state/counters/", name);
    ops_parse_str = "openconfig-if-ip:" + name;
  } else {
    ops_state_path = absl::StrCat("interfaces/interface[name=", iface,
                                  "]/state/counters/", stat_name);
    ops_parse_str = "openconfig-interfaces:" + stat_name;
  }

  ASSIGN_OR_RETURN(std::string ops_response,
                   pins_test::GetGnmiStatePathInfo(gnmi_stub, ops_state_path,
                                                   ops_parse_str));

  uint64_t stat;
  // skip over the initial quote '"'
  (void)absl::SimpleAtoi(ops_response.substr(1), &stat);
  return stat;
}

void ShowCounters(const Counters& cnt) {
  LOG(INFO) << "in-pkts " << cnt.in_pkts;
  LOG(INFO) << "out-pkts " << cnt.out_pkts;
  LOG(INFO) << "in-octets " << cnt.in_octets;
  LOG(INFO) << "out-octets " << cnt.out_octets;
}

// DeltaCounters - computer delta as change from initial to final counters
Counters DeltaCounters(const Counters& initial, const Counters& final) {
  Counters delta = {};

  delta.in_pkts = final.in_pkts - initial.in_pkts;
  delta.out_pkts = final.out_pkts - initial.out_pkts;
  delta.in_octets = final.in_octets - initial.in_octets;
  delta.out_octets = final.out_octets - initial.out_octets;
  return delta;
}

absl::StatusOr<Counters> ReadCounters(std::string iface,
                                      gnmi::gNMI::StubInterface* gnmi_stub) {
  Counters cnt = {};

  ASSIGN_OR_RETURN(cnt.in_pkts, GetGnmiStat("in-pkts", iface, gnmi_stub));
  ASSIGN_OR_RETURN(cnt.out_pkts, GetGnmiStat("out-pkts", iface, gnmi_stub));
  ASSIGN_OR_RETURN(cnt.in_octets, GetGnmiStat("in-octets", iface, gnmi_stub));
  ASSIGN_OR_RETURN(cnt.out_octets, GetGnmiStat("out-octets", iface, gnmi_stub));
  return cnt;
}

// The packets are all same for one port. Use port_id as the index for
// generating packets.
absl::Status SendNPacketsToSut(absl::Span<const std::string> traffic_ref,
                               absl::string_view topology_ref,
                               absl::Duration runtime,
                               thinkit::GenericTestbed& testbed) {
  // Send Ixia traffic.
  RETURN_IF_ERROR(
      pins_test::ixia::StartTraffic(traffic_ref, topology_ref, testbed));

  // Wait for Traffic to be sent.
  absl::SleepFor(runtime);

  // Stop Ixia traffic.
  RETURN_IF_ERROR(pins_test::ixia::StopTraffic(traffic_ref, testbed));

  return absl::OkStatus();
}

// Set up Ixia traffic with given parameters and return the traffic ref and
// topology ref string.
absl::StatusOr<std::pair<std::vector<std::string>, std::string>>
SetUpIxiaTraffic(absl::Span<const IxiaLink> ixia_links,
                 thinkit::GenericTestbed& testbed, const int pkt_count,
                 const int pkt_rate, const int frame_size = 1000) {
  std::vector<std::string> traffic_refs;
  std::string topology_ref;
  for (const IxiaLink& ixia_link : ixia_links) {
    LOG(INFO) << __func__ << " Ixia if:" << ixia_link.ixia_interface
              << " sut if:" << ixia_link.sut_interface
              << " port id:" << ixia_link.port_id;

    std::string ixia_interface = ixia_link.ixia_interface;
    std::string sut_interface = ixia_link.sut_interface;

    // Set up Ixia traffic.
    ASSIGN_OR_RETURN(pins_test::ixia::IxiaPortInfo ixia_port,
                     pins_test::ixia::ExtractPortInfo(ixia_interface));
    ASSIGN_OR_RETURN(std::string topology_ref_tmp,
                     pins_test::ixia::IxiaConnect(ixia_port.hostname, testbed));
    if (topology_ref.empty()) {
      topology_ref = topology_ref_tmp;
    } else {
      EXPECT_EQ(topology_ref, topology_ref_tmp);
    }

    ASSIGN_OR_RETURN(std::string vport_ref,
                     pins_test::ixia::IxiaVport(topology_ref, ixia_port.card,
                                                ixia_port.port, testbed));

    ASSIGN_OR_RETURN(std::string traffic_ref,
                     pins_test::ixia::IxiaSession(vport_ref, testbed));

    RETURN_IF_ERROR(
        pins_test::ixia::SetFrameRate(traffic_ref, pkt_rate, testbed));

    RETURN_IF_ERROR(
        pins_test::ixia::SetFrameCount(traffic_ref, pkt_count, testbed));

    RETURN_IF_ERROR(
        pins_test::ixia::SetFrameSize(traffic_ref, frame_size, testbed));

    RETURN_IF_ERROR(pins_test::ixia::SetSrcMac(traffic_ref,
                                               kSourceMac.ToString(), testbed));

    RETURN_IF_ERROR(
        pins_test::ixia::SetDestMac(traffic_ref, kDstMac.ToString(), testbed));

    RETURN_IF_ERROR(pins_test::ixia::AppendIPv4(traffic_ref, testbed));

    // Use Ipv4 source address to differentiate different ports.
    RETURN_IF_ERROR(pins_test::ixia::SetSrcIPv4(
        traffic_ref, GetSrcIpv4AddrByPortId(ixia_link.port_id), testbed));

    RETURN_IF_ERROR(pins_test::ixia::SetDestIPv4(traffic_ref,
                                                 kIpV4Dst.ToString(), testbed));
    traffic_refs.push_back(traffic_ref);
  }
  return std::make_pair(traffic_refs, topology_ref);
}

// Get the packet counters on SUT interface connected to Ixia.
absl::StatusOr<std::vector<Counters>> GetIxiaInterfaceCounters(
    absl::Span<const IxiaLink> ixia_links,
    gnmi::gNMI::StubInterface* gnmi_stub) {
  std::vector<Counters> counters;
  for (const IxiaLink& ixia_link : ixia_links) {
    ASSIGN_OR_RETURN(auto initial_in_counter,
                     ReadCounters(ixia_link.sut_interface, gnmi_stub));
    LOG(INFO) << "Ingress Counters (" << ixia_link.sut_interface << "):\n";
    ShowCounters(initial_in_counter);
    LOG(INFO) << "\n";
    counters.push_back(initial_in_counter);
  }
  // Reads CPU counter.
  ASSIGN_OR_RETURN(auto initial_in_counter, ReadCounters("CPU", gnmi_stub));
  LOG(INFO) << "Ingress Counters (\"CPU\"):\n";
  ShowCounters(initial_in_counter);
  LOG(INFO) << "\n";
  counters.push_back(initial_in_counter);
  return counters;
}

// Run sflowtool on SUT in a new thread. Returns the thread to let caller to
// wait for the finish.
absl::StatusOr<std::thread> StartSflowCollector(
    thinkit::SSHClient* ssh_client, absl::string_view device_name,
    absl::string_view sflow_template, const int sflowtool_runtime,
    std::string& sflow_tool_result) {
  std::thread sflow_tool_thread = std::thread([&sflow_tool_result, ssh_client,
                                               device_name, sflow_template,
                                               sflowtool_runtime]() {
    const std::string ssh_command =
        absl::Substitute(sflow_template, kSflowToolName, sflowtool_runtime);
    LOG(INFO) << "ssh command:" << ssh_command;
    ASSERT_OK_AND_ASSIGN(sflow_tool_result,
                         ssh_client->RunCommand(
                             device_name, ssh_command,
                             /*timeout=*/absl::Seconds(sflowtool_runtime + 2)));
  });
  // Sleep to wait sflowtool to start.
  absl::SleepFor(absl::Seconds(5));
  return sflow_tool_thread;
}

// Send packets to SUT and validate packet counters via gNMI.
absl::Status SendSflowTraffic(absl::Span<const std::string> traffic_refs,
                              absl::string_view topology_ref,
                              absl::Span<const IxiaLink> ixia_links,
                              thinkit::GenericTestbed& testbed,
                              gnmi::gNMI::StubInterface* gnmi_stub,
                              const int pkt_count, const int pkt_rate) {
  // Read initial counters via GNMI from the SUT
  LOG(INFO) << "Read initial packet counters.";
  ASSIGN_OR_RETURN(std::vector<Counters> initial_in_counters,
                   GetIxiaInterfaceCounters(ixia_links, gnmi_stub));

  RETURN_IF_ERROR(SendNPacketsToSut(
      traffic_refs, topology_ref,
      /*runtime=*/absl::Seconds(std::ceil(1.0f * pkt_count / pkt_rate)),
      testbed));

  LOG(INFO) << "Read final packet counters.";
  // Read final counters via GNMI from the SUT
  ASSIGN_OR_RETURN(std::vector<Counters> final_in_counters,
                   GetIxiaInterfaceCounters(ixia_links, gnmi_stub));
  for (size_t i = 0; i < ixia_links.size(); ++i) {
    auto delta = DeltaCounters(initial_in_counters[i], final_in_counters[i]);
    // Display the difference in the counters for now (during test dev)
    LOG(INFO) << "\nIngress Deltas (" << ixia_links[i].sut_interface << "):\n";
    ShowCounters(delta);
    EXPECT_EQ(delta.in_pkts, pkt_count)
        << "Received packets count is not equal to sent packets count: "
        << ". Interface: " << ixia_links[i].sut_interface << ". Sent "
        << pkt_count << ". Received " << delta.in_pkts << ".";
  }
  // Show CPU counter data.
  auto delta =
      DeltaCounters(initial_in_counters.back(), final_in_counters.back());
  LOG(INFO) << "\nIngress Deltas (\"CPU\"):\n";
  ShowCounters(delta);
  return absl::OkStatus();
}

int GetSflowSamplesOnSut(const std::string& sflowtool_output,
                         const int port_id) {
  constexpr int kFieldSize = 20, kSrcIpIdx = 9;
  int count = 0;
  // Each line indicates one sFlow sample.
  for (absl::string_view sflow : absl::StrSplit(sflowtool_output, '\n')) {
    // Split by column.
    std::vector<absl::string_view> fields = absl::StrSplit(sflow, ',');
    if (fields.size() < kFieldSize) {
      continue;
    }
    // Filter source ip.
    if (fields[kSrcIpIdx] == GetSrcIpv4AddrByPortId(port_id)) {
      count++;
    }
  }
  return count;
}

// Get port speed by reading interface/ethernet/state/port-speed path.
absl::StatusOr<std::string> GetPortSpeed(absl::string_view iface,
                                         gnmi::gNMI::StubInterface* gnmi_stub) {
  std::string ops_state_path = absl::StrCat("interfaces/interface[name=", iface,
                                            "]/ethernet/state/port-speed");

  std::string ops_parse_str = "openconfig-if-ethernet:port-speed";
  return pins_test::GetGnmiStatePathInfo(gnmi_stub, ops_state_path,
                                         ops_parse_str);
}

// Check interface/state/oper-status value to validate if link is up.
absl::StatusOr<bool> CheckLinkUp(absl::string_view interface,
                                 gnmi::gNMI::StubInterface& gnmi_stub) {
  std::string oper_status_state_path = absl::StrCat(
      "interfaces/interface[name=", interface, "]/state/oper-status");

  std::string parse_str = "openconfig-interfaces:oper-status";
  ASSIGN_OR_RETURN(std::string ops_response,
                   pins_test::GetGnmiStatePathInfo(
                       &gnmi_stub, oper_status_state_path, parse_str));

  return ops_response == "\"UP\"";
}

// Returns an available port which is UP and different from `used_port`.
// Returns an error if failed.
absl::StatusOr<Port> GetUnusedUpPort(gnmi::gNMI::StubInterface& gnmi_stub,
                                     absl::string_view used_port) {
  absl::flat_hash_map<std::string, std::string> port_id_per_port_name;
  ASSIGN_OR_RETURN(port_id_per_port_name,
                   pins_test::GetAllUpInterfacePortIdsByName(gnmi_stub));
  for (const auto& [interface, port_id_str] : port_id_per_port_name) {
    int port_id;
    if (interface != used_port && absl::SimpleAtoi(port_id_str, &port_id)) {
      return Port{
          .interface_name = interface,
          .port_id = port_id,
      };
    }
  }
  return absl::FailedPreconditionError("No more usable port ids.");
}

// Returns a vector of SUT interfaces that are connected to Ixia and up.
absl::StatusOr<std::vector<IxiaLink>> GetIxiaConnectedUpLinks(
    thinkit::GenericTestbed& generic_testbed,
    gnmi::gNMI::StubInterface& gnmi_stub) {
  std::vector<IxiaLink> ixia_links;

  absl::flat_hash_map<std::string, thinkit::InterfaceInfo> interface_info =
      generic_testbed.GetSutInterfaceInfo();
  absl::flat_hash_map<std::string, std::string> port_id_per_port_name;
  ASSIGN_OR_RETURN(port_id_per_port_name,
                   pins_test::GetAllInterfaceNameToPortId(gnmi_stub));
  // Loop through the interface_info looking for Ixia/SUT interface pairs,
  // checking if the link is up.  Add the pair to connections.
  for (const auto& [interface, info] : interface_info) {
    if (info.interface_modes.contains(thinkit::TRAFFIC_GENERATOR)) {
      ASSIGN_OR_RETURN(bool sut_link_up, CheckLinkUp(interface, gnmi_stub));
      auto port_id = gutil::FindOrNull(port_id_per_port_name, interface);
      EXPECT_NE(port_id, nullptr) << absl::Substitute(
          "No corresponding p4rt id for interface $0", interface);
      if (sut_link_up) {
        LOG(INFO) << "Ixia interface:" << info.peer_interface_name
                  << ". Sut interface:" << interface << ". Port id:"
                  << *port_id;
        ixia_links.push_back(IxiaLink{
            .ixia_interface = info.peer_interface_name,
            .sut_interface = interface,
            .port_id = std::stoi(*port_id),
        });
      }
    }
  }

  return ixia_links;
}

// Used for printing result.
struct SflowResult {
  std::string rule;
  std::string sut_interface;
  int packets;
  int sampling_rate;
  int expected_samples;
  int actual_samples;

  std::string DebugString() {
    return absl::Substitute(
        "Rule: $0\n"
        "Ingress interface: $1\n"
        "Total packets input: $2\n"
        "Sampling rate: 1 in $3\n"
        "Expected samples: $4\n"
        "Actual samples: $5",
        rule, sut_interface, packets, sampling_rate, expected_samples,
        actual_samples);
  }
};

// Populates `agent_addr_ipv6` from gNMI config, `sflow_interface_names` from
// testbed.
absl::Status GetSflowInfoFromSut(
    thinkit::GenericTestbed* testbed, const std::string& gnmi_config,
    std::string* agent_addr_ipv6,
    absl::flat_hash_set<std::string>* sflow_interface_names) {
  ASSIGN_OR_RETURN(auto loopback_ipv6,
                   pins_test::ParseLoopbackIpv6s(gnmi_config));

  if (loopback_ipv6.empty()) {
    return absl::FailedPreconditionError(absl::Substitute(
        "No loopback IP found for $0 testbed.", testbed->Sut().ChassisName()));
  }
  *agent_addr_ipv6 = loopback_ipv6[0].ToString();

  absl::flat_hash_map<std::string, thinkit::InterfaceInfo> interface_info =
      testbed->GetSutInterfaceInfo();
  sflow_interface_names->clear();
  for (const auto& [interface, info] : interface_info) {
    if (info.interface_modes.contains(thinkit::CONTROL_INTERFACE) ||
        info.interface_modes.contains(thinkit::TRAFFIC_GENERATOR)) {
      sflow_interface_names->insert(interface);
    }
  }
  return absl::OkStatus();
}

// Returns the value of `key` in `sflow_datagram`. Returns an error if not
// found.
absl::StatusOr<absl::string_view> ExtractValueByKey(
    absl::string_view sflow_datagram, absl::string_view key) {
  int idx = sflow_datagram.find(key);
  if (idx == std::string::npos) {
    return absl::NotFoundError(
        absl::Substitute("Cannot find $0 in $1", key, sflow_datagram));
  }
  return absl::StripAsciiWhitespace(sflow_datagram.substr(
      idx + key.size(), sflow_datagram.find('\n', idx) - idx - key.size()));
}

// Returns the headerLen from `sflowtool_output`. Also stores datagram into test
// artifact with `file_name`. Returns -1 if any error occurs.
absl::StatusOr<int> GetHeaderLenFromSflowOutput(
    absl::string_view sflowtool_output, int port_id,
    absl::string_view file_name, thinkit::TestEnvironment& test_environment) {
  // Every "startDatagram" indicates an sFlow datagram.
  constexpr char kPattern[] = "startDatagram";
  size_t pos = sflowtool_output.rfind(kPattern, 0);
  if (pos == std::string::npos) {
    return absl::NotFoundError(
        absl::Substitute("Cound not find $0 in sflowtool_output", kPattern));
  }
  absl::string_view sflow_datagram = sflowtool_output.substr(pos);
  LOG(INFO) << "sFlow datagram:\n" << sflow_datagram;

  // Example dump:
  // startDatagram =================================
  // ...
  // startSample ----------------------
  // ...
  // headerLen 128
  // ...
  // srcIP 1.2.5.12
  // ...
  // endSample   ----------------------
  // endDatagram   =================================
  EXPECT_OK(test_environment.StoreTestArtifact(file_name, sflow_datagram));

  // Verifies this datagram is generated from specific traffic. We use srcIP
  // value for specific in port so we use it to validate as well.
  ASSIGN_OR_RETURN(absl::string_view src_ip,
                   ExtractValueByKey(sflow_datagram, "srcIP"));
  if (src_ip != GetSrcIpv4AddrByPortId(port_id)) {
    return absl::FailedPreconditionError(
        absl::Substitute("srcIp field in sflow sample is not as expected: "
                         "expected: $0, actual: $1.",
                         GetSrcIpv4AddrByPortId(port_id), src_ip));
  }
  // Header length would be after `headerLen` field.
  ASSIGN_OR_RETURN(absl::string_view header_len_str,
                   ExtractValueByKey(sflow_datagram, "headerLen"));
  int header_len;
  (void)absl::SimpleAtoi(header_len_str, &header_len);
  return header_len;
}

}  // namespace

void SflowTestFixture::SetUp() {
  // Pick a testbed with an Ixia Traffic Generator.
  auto requirements =
      gutil::ParseProtoOrDie<thinkit::TestRequirements>(
          R"pb(interface_requirements {
                 count: 1
                 interface_modes: TRAFFIC_GENERATOR
               })pb");

  ASSERT_OK_AND_ASSIGN(
      testbed_,
      GetParam().testbed_interface->GetTestbedWithRequirements(requirements));

  std::string gnmi_config = GetParam().gnmi_config;
  ASSERT_OK_AND_ASSIGN(nlohmann::json gnmi_config_json,
                       json_yang::ParseJson(gnmi_config));
  if (gnmi_config_json.find("openconfig-sampling:sampling") !=
          gnmi_config_json.end() &&
      gnmi_config_json["openconfig-sampling:sampling"]
                      ["openconfig-sampling-sflow:sflow"]["config"]
                      ["enabled"]) {
    sflow_enabled_by_config_ = true;
  }
  int sampling_size, sampling_rate;
  std::string agent_addr_ipv6;
  absl::flat_hash_set<std::string> sflow_enabled_interfaces;
  std::vector<std::pair<std::string, int>> collector_address_and_port;
  ASSERT_OK(
      testbed_->Environment().StoreTestArtifact("gnmi_config", gnmi_config));
  sampling_size = kSampleSize;
  sampling_rate = kSamplingRateInterval;

  // Set to loopback ip by default.
  collector_address_and_port.push_back({"127.0.0.1", 6343});
  ASSERT_OK(GetSflowInfoFromSut(testbed_.get(), gnmi_config, &agent_addr_ipv6,
                                &sflow_enabled_interfaces));
  ASSERT_OK_AND_ASSIGN(
      gnmi_config, UpdateSflowConfig(
                       gnmi_config, agent_addr_ipv6, collector_address_and_port,
                       sflow_enabled_interfaces, sampling_rate, sampling_size));
  ASSERT_OK(testbed_->Environment().StoreTestArtifact(
      "gnmi_config_with_sflow.txt",
      json_yang::FormatJsonBestEffort(gnmi_config)));
  ASSERT_OK(testbed_->Environment().StoreTestArtifact(
      "p4info.pb.txt", GetP4Info().DebugString()));
  ASSERT_OK_AND_ASSIGN(sut_p4_session_,
                       pins_test::ConfigureSwitchAndReturnP4RuntimeSession(
                           testbed_->Sut(), gnmi_config, GetP4Info()));
  ASSERT_OK_AND_ASSIGN(ir_p4_info_, pdpi::CreateIrP4Info(GetP4Info()));
  ASSERT_OK_AND_ASSIGN(gnmi_stub_, testbed_->Sut().CreateGnmiStub());

  // Wait until all sFLow gNMI states are converged.
  ASSERT_OK(pins_test::WaitForCondition(
      VerifySflowStatesConverged, absl::Seconds(30), gnmi_stub_.get(),
      agent_addr_ipv6, sampling_rate, sampling_size, collector_address_and_port,
      sflow_enabled_interfaces));

  ASSERT_OK_AND_ASSIGN(ready_links_,
                       GetIxiaConnectedUpLinks(*testbed_, *gnmi_stub_));
  ASSERT_FALSE(ready_links_.empty()) << "Ixia links are not ready";
}

void SflowTestFixture::TearDown() {
  LOG(INFO) << "\n------ TearDown START ------\n";

  // Clear table entries, restore gNMI config and stop RPC sessions.
  ASSERT_OK(pdpi::ClearTableEntries(sut_p4_session_.get()));
  if (!sflow_enabled_by_config_) {
    ASSERT_OK(SetSflowConfigEnabled(gnmi_stub_.get(), /*enabled=*/false));
  }
  ASSERT_OK(pins_test::PushGnmiConfig(testbed_->Sut(), GetParam().gnmi_config));
  if (sut_p4_session_ != nullptr) {
    EXPECT_OK(sut_p4_session_->Finish());
  }
  GetParam().testbed_interface->TearDown();
  if (ssh_client_ != nullptr) {
    delete ssh_client_;
    ssh_client_ = nullptr;
  }
  if (GetParam().testbed_interface != nullptr) {
    delete GetParam().testbed_interface;
  }
  LOG(INFO) << "\n------ TearDown END ------\n";
}

// This test checks sFlow works as expected with no rules.
// 1. Set up Ixia traffic and send packets to SUT via Ixia.
// 2. Collect sFlow samples via sflowtool on SUT.
// 3. Validate the result is as expected.
TEST_P(SflowTestFixture, VerifyIngressSamplingForNoMatchPackets) {
  const IxiaLink& ingress_link = ready_links_[0];
  Port ingress_port = Port{
      .interface_name = ingress_link.sut_interface,
      .port_id = ingress_link.port_id,
  };

  // ixia_ref_pair would include the traffic reference and topology reference
  // which could be used to send traffic later.
  std::pair<std::vector<std::string>, std::string> ixia_ref_pair;
  // Set up Ixia traffic.
  ASSERT_OK_AND_ASSIGN(ixia_ref_pair,
                       SetUpIxiaTraffic({ingress_link}, *testbed_, kPacketsNum,
                                        kPacketsPerSecond));

  // Start sflowtool on SUT.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(
          ssh_client_, testbed_->Sut().ChassisName(),
          kSflowtoolLineFormatTemplate,
          /*sflowtool_runtime=*/kPacketsNum / kPacketsPerSecond + 30,
          sflow_result));

  // Send packets from Ixia to SUT.
  ASSERT_OK(SendSflowTraffic(ixia_ref_pair.first, ixia_ref_pair.second,
                             {ingress_link}, *testbed_, gnmi_stub_.get(),
                             kPacketsNum, kPacketsPerSecond));

  // Wait for sflowtool to finish.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  LOG(INFO) << "sFlow samples:\n" << sflow_result;

  // Verify sflowtool result. Since we use port id to generate packets, we use
  // port id to filter sFlow packets.
  const int sample_count =
      GetSflowSamplesOnSut(sflow_result, ingress_port.port_id);
  const double expected_count = 1.0 * kPacketsNum / kSamplingRateInterval;
  SflowResult result = SflowResult{
      .sut_interface = ingress_port.interface_name,
      .packets = kPacketsNum,
      .sampling_rate = kSamplingRateInterval,
      .expected_samples = static_cast<int>(expected_count),
      .actual_samples = sample_count,
  };
  LOG(INFO) << "------ Test result ------\n" << result.DebugString();
  EXPECT_GE(sample_count, expected_count * (1 - kTolerance));
  EXPECT_LE(sample_count, expected_count * (1 + kTolerance));
}

// Verifies ingress sampling could work when forwarding traffic.
TEST_P(SflowTestFixture, VerifyIngressSamplingForForwardedPackets) {
  const IxiaLink& ingress_link = ready_links_[0];
  Port ingress_port = Port{
      .interface_name = ingress_link.sut_interface,
      .port_id = ingress_link.port_id,
  };
  ASSERT_OK_AND_ASSIGN(
      Port egress_port,
      GetUnusedUpPort(*gnmi_stub_, ingress_port.interface_name));
  // Programs forwarding rule.
  ASSERT_OK_AND_ASSIGN(std::vector<GroupMember> members,
                       CreateGroupMembers(1, {egress_port.port_id}));
  ASSERT_OK(pins::ProgramNextHops(testbed_->Environment(), *sut_p4_session_,
                                   GetIrP4Info(), members));
  const std::string& egress_next_hop_id = members[0].nexthop;
  ASSERT_OK(SetSutVrf(*sut_p4_session_, GetP4Info(), GetIrP4Info(),
                      {ingress_port.port_id}));

  // Allow the destination mac address to L3.
  ASSERT_OK(ProgramL3Admit(
      *sut_p4_session_, ir_p4_info_,
      L3AdmitOptions{
          .priority = 2070,
          .dst_mac = std::make_pair(kDstMac.ToString(), "FF:FF:FF:FF:FF:FF"),
      }));
  ASSERT_OK(ProgramRoutes(*sut_p4_session_, GetIrP4Info(), ingress_port.port_id,
                          egress_next_hop_id));

  // Set up Ixia traffic. ixia_ref_pair would include the traffic reference and
  // topology reference which could be used to send traffic later.
  std::pair<std::vector<std::string>, std::string> ixia_ref_pair;
  ASSERT_OK_AND_ASSIGN(ixia_ref_pair,
                       SetUpIxiaTraffic({ingress_link}, *testbed_, kPacketsNum,
                                        kPacketsPerSecond));

  // Start sflowtool on SUT.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(
          ssh_client_, testbed_->Sut().ChassisName(),
          kSflowtoolLineFormatTemplate,
          /*sflowtool_runtime=*/kPacketsNum / kPacketsPerSecond + 10,
          sflow_result));

  // Send packets via Ixia.
  ASSERT_OK(SendSflowTraffic(ixia_ref_pair.first, ixia_ref_pair.second,
                             {ingress_link}, *testbed_, gnmi_stub_.get(),
                             kPacketsNum, kPacketsPerSecond));

  // Wait for sflowtool to finish.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  LOG(INFO) << "sFlow samples:\n" << sflow_result;

  // Verify sflowtool result. Since we use port id to generate packets, we use
  // port id to filter sFlow packets.
  const int sample_count =
      GetSflowSamplesOnSut(sflow_result, ingress_port.port_id);
  const double expected_count = 1.0 * kPacketsNum / kSamplingRateInterval;
  EXPECT_GE(sample_count, expected_count * (1 - kTolerance));
  EXPECT_LE(sample_count, expected_count * (1 + kTolerance));
  SflowResult result = SflowResult{
      .rule = "Forward traffic",
      .sut_interface = ingress_port.interface_name,
      .packets = kPacketsNum,
      .sampling_rate = kSamplingRateInterval,
      .expected_samples = static_cast<int>(expected_count),
      .actual_samples = sample_count,
  };
  LOG(INFO) << "------ Test result ------\n" << result.DebugString();
}

// Verifies ingress sampling could work when dropping packets.
TEST_P(SflowTestFixture, VerifyIngressSamplesForDropPackets) {
  const IxiaLink& ingress_link = ready_links_[0];
  Port ingress_port = Port{
      .interface_name = ingress_link.sut_interface,
      .port_id = ingress_link.port_id,
  };
  ASSERT_OK(
      SetUpAclDrop(*sut_p4_session_, GetIrP4Info(), ingress_port.port_id));

  // Set up Ixia traffic. ixia_ref_pair would include the traffic reference and
  // topology reference which could be used to send traffic later.
  std::pair<std::vector<std::string>, std::string> ixia_ref_pair;
  ASSERT_OK_AND_ASSIGN(ixia_ref_pair,
                       SetUpIxiaTraffic({ingress_link}, *testbed_, kPacketsNum,
                                        kPacketsPerSecond));

  // Start sflowtool on SUT.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(
          ssh_client_, testbed_->Sut().ChassisName(),
          kSflowtoolLineFormatTemplate,
          /*sflowtool_runtime=*/kPacketsNum / kPacketsPerSecond + 10,
          sflow_result));

  // Send packets via Ixia.
  ASSERT_OK(SendSflowTraffic(ixia_ref_pair.first, ixia_ref_pair.second,
                             {ingress_link}, *testbed_, gnmi_stub_.get(),
                             kPacketsNum, kPacketsPerSecond));

  // Wait for sflowtool to finish.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  LOG(INFO) << "sFlow samples:\n" << sflow_result;

  // Verify sflowtool result. Since we use port id to generate packets, we use
  // port id to filter sFlow packets.
  const int sample_count =
      GetSflowSamplesOnSut(sflow_result, ingress_port.port_id);
  const double expected_count = 1.0 * kPacketsNum / kSamplingRateInterval;
  EXPECT_GE(sample_count, expected_count * (1 - kTolerance));
  EXPECT_LE(sample_count, expected_count * (1 + kTolerance));
  SflowResult result = SflowResult{
      .rule = "Drop traffic",
      .sut_interface = ingress_port.interface_name,
      .packets = kPacketsNum,
      .sampling_rate = kSamplingRateInterval,
      .expected_samples = static_cast<int>(expected_count),
      .actual_samples = sample_count,
  };
  LOG(INFO) << "------ Test result ------\n" << result.DebugString();
}

// TODO: Add a punt test case for cpu bound punt traffic like
// ssh/scp traffic: generate traffic from Ixia destined to Loopback0 ip addr and
// tcp port matching scp/ssh.
// Verifies ingress sampling could work when punting traffic.
TEST_P(SflowTestFixture, DISABLED_VerifyIngressSamplesForP4rtPuntTraffic) {
  const IxiaLink& ingress_link = ready_links_[0];
  Port ingress_port = Port{
      .interface_name = ingress_link.sut_interface,
      .port_id = ingress_link.port_id,
  };
  ASSERT_OK(
      SetUpAclPunt(*sut_p4_session_, GetIrP4Info(), ingress_port.port_id));

  // Set up Ixia traffic. ixia_ref_pair would include the traffic reference and
  // topology reference which could be used to send traffic later.
  std::pair<std::vector<std::string>, std::string> ixia_ref_pair;
  ASSERT_OK_AND_ASSIGN(ixia_ref_pair,
                       SetUpIxiaTraffic({ingress_link}, *testbed_, kPacketsNum,
                                        kPacketsPerSecond));

  // Start sflowtool on SUT.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(
          ssh_client_, testbed_->Sut().ChassisName(),
          kSflowtoolLineFormatTemplate,
          /*sflowtool_runtime=*/kPacketsNum / kPacketsPerSecond + 10,
          sflow_result));

  // Send packets via Ixia.
  ASSERT_OK(SendSflowTraffic(ixia_ref_pair.first, ixia_ref_pair.second,
                             {ingress_link}, *testbed_, gnmi_stub_.get(),
                             kPacketsNum, kPacketsPerSecond));
  // Wait for sflowtool to finish.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  LOG(INFO) << "sFlow samples:\n" << sflow_result;

  // Verify sflowtool result. Since we use port id to generate packets, we use
  // port id to filter sFlow packets.
  const int sample_count =
      GetSflowSamplesOnSut(sflow_result, ingress_port.port_id);
  const double expected_count = 1.0 * kPacketsNum / kSamplingRateInterval;
  EXPECT_GE(sample_count, expected_count * (1 - kTolerance));
  EXPECT_LE(sample_count, expected_count * (1 + kTolerance));

  SflowResult result = SflowResult{
      .rule = "Punt traffic",
      .sut_interface = ingress_port.interface_name,
      .packets = kPacketsNum,
      .sampling_rate = kSamplingRateInterval,
      .expected_samples = static_cast<int>(expected_count),
      .actual_samples = sample_count,
  };
  LOG(INFO) << "------ Test result ------\n" << result.DebugString();
}

// Verifies sampling size could work:
// Traffic packet size size_a, sFlow sampling size size_b: expects sample header
// size equals to min(size_a, size_b).
TEST_P(SampleSizeTest, VerifySamplingSizeWorks) {
  const int packet_size = GetParam().packet_size,
            sample_size = GetParam().sample_size;
  ASSERT_NE(packet_size, 0);
  ASSERT_NE(sample_size, 0);
  // ixia_ref_pair would include the traffic reference and topology reference
  // which could be used to send traffic later.
  std::pair<std::vector<std::string>, std::string> ixia_ref_pair;
  ASSERT_OK(SetSflowSamplingSize(gnmi_stub_.get(), sample_size));
  const IxiaLink& ingress_link = ready_links_[0];

  // Set up Ixia traffic.
  ASSERT_OK_AND_ASSIGN(ixia_ref_pair,
                       SetUpIxiaTraffic({ingress_link}, *testbed_, kPacketsNum,
                                        kPacketsPerSecond, packet_size));

  // Start sflowtool on SUT.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(
          ssh_client_, testbed_->Sut().ChassisName(),
          kSflowtoolFullFormatTemplate,
          /*sflowtool_runtime=*/kPacketsNum / kPacketsPerSecond + 10,
          sflow_result));

  // Send packets with kPacketSize from Ixia to SUT.
  ASSERT_OK(SendSflowTraffic(ixia_ref_pair.first, ixia_ref_pair.second,
                             {ingress_link}, *testbed_, gnmi_stub_.get(),
                             kPacketsNum, kPacketsPerSecond));

  // Wait for sflowtool to finish.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  LOG(INFO) << "sFlow samples with sampling size " << sample_size << ":\n"
            << sflow_result;
  EXPECT_OK(testbed_->Environment().StoreTestArtifact("sflow_result.txt",
                                                      sflow_result));
  EXPECT_THAT(
      GetHeaderLenFromSflowOutput(
          sflow_result, ingress_link.port_id,
          absl::Substitute("sFLow_datagram_packet_size_$0_sampling_size_$1.txt",
                           packet_size, sample_size),
          testbed_->Environment()),
      IsOkAndHolds(std::min(
          sample_size,
          packet_size - 4)));  // sFlow would strip some bytes from each packet.
}

// Verifies sampling rate could work:
// send traffic to two interfaces with different sampling rate and verifies
// samples count.
TEST_P(SampleRateTest, VerifySamplingRateWorks) {
  const IxiaLink& ingress_link = ready_links_[0];
  const int sample_rate = GetParam().sample_rate;
  ASSERT_GT(sample_rate, 0);
  const int traffic_rate = sample_rate * 4;
  const int packets_num = sample_rate * 40;

  ASSERT_OK(SetSflowIngressSamplingRate(
      gnmi_stub_.get(), ingress_link.sut_interface, sample_rate));

  // ixia_ref_pair would include the traffic reference and topology reference
  // which could be used to send traffic later.
  std::pair<std::vector<std::string>, std::string> ixia_ref_pair;
  // Set up Ixia traffic.
  ASSERT_OK_AND_ASSIGN(ixia_ref_pair,
                       SetUpIxiaTraffic({ingress_link}, *testbed_, packets_num,
                                        traffic_rate, /*frame_size=*/500));

  // Start sflowtool on SUT.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(ssh_client_, testbed_->Sut().ChassisName(),
                          kSflowtoolLineFormatTemplate,
                          /*sflowtool_runtime=*/packets_num / traffic_rate + 10,
                          sflow_result));

  // Send packets from Ixia to SUT.
  ASSERT_OK(SendSflowTraffic(ixia_ref_pair.first, ixia_ref_pair.second,
                             {ingress_link}, *testbed_, gnmi_stub_.get(),
                             packets_num, traffic_rate));

  // Wait for sflowtool to finish.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  EXPECT_OK(testbed_->Environment().StoreTestArtifact(
      absl::Substitute("sflow_result_sampling_rate_$0_result.txt", sample_rate),
      sflow_result));

  // Verify sflowtool result. Since we use port id to generate packets, we use
  // port id to filter sFlow packets.
  const int sample_count =
      GetSflowSamplesOnSut(sflow_result, ingress_link.port_id);
  const double expected_count =
      static_cast<double>(packets_num) / static_cast<double>(sample_rate);
  SflowResult result = SflowResult{
      .sut_interface = ingress_link.sut_interface,
      .packets = packets_num,
      .sampling_rate = sample_rate,
      .expected_samples = static_cast<int>(expected_count),
      .actual_samples = sample_count,
  };
  LOG(INFO) << "------ Test result ------\n" << result.DebugString();

  // TODO: tune the tolerance rate of sampling rate test
  // since sample count seems like to be more deviated when the sample rate
  // is high.
  EXPECT_GE(sample_count, expected_count * (1 - kTolerance));
  EXPECT_LE(sample_count, expected_count * (1 + kTolerance));
}
namespace {

constexpr int kInbandSamplingRate = 256;
constexpr int kInbandSamplingHeaderSize = 128;
constexpr int kInbandTrafficPps = 120;

// Sends N packets at a rate of kInbandTrafficPps.
absl::Status SendNPacketsFromSwitch(
    int num_packets, const std::string& port, const pdpi::IrP4Info& ir_p4info,
    pdpi::P4RuntimeSession& p4_session,
    thinkit::TestEnvironment& test_environment) {
  const absl::Time start_time = absl::Now();
  auto packet = gutil::ParseProtoOrDie<packetlib::Packet>(
      R"pb(
        headers {
          ethernet_header {
            ethernet_destination: "02:03:04:05:06:07"
            ethernet_source: "00:01:02:03:04:05"
            ethertype: "0x0800"
          }
        }
        headers {
          ipv4_header {
            version: "0x4"
            ihl: "0x5"
            dscp: "0x03"
            ecn: "0x0"
            identification: "0x0000"
            flags: "0x0"
            fragment_offset: "0x0000"
            ttl: "0x20"
            protocol: "0x05"
            ipv4_source: "1.2.3.4"
            ipv4_destination: "5.6.7.8"
          }
        }
        payload: "Test packet for Sflow Inband testing")pb");
  ASSIGN_OR_RETURN(std::string raw_packet, SerializePacket(packet));
  for (int i = 0; i < num_packets; i++) {
    // Rate limit to kInbandTrafficPps packets per second.
    RETURN_IF_ERROR(InjectEgressPacket(
        port, raw_packet, ir_p4info, &p4_session,
        /*packet_delay=*/absl::Milliseconds(1000 / kInbandTrafficPps)));
  }

  LOG(INFO) << "Sent " << num_packets << " packets in "
            << (absl::Now() - start_time) << ".";
  return absl::OkStatus();
}

// Sets VRF id for all packets.
absl::Status SetSwitchVrfForAllPackets(pdpi::P4RuntimeSession& p4_session,
                                       const pdpi::IrP4Info& ir_p4info,
                                       absl::string_view vrf_id) {
  // Create default VRF for test.
  ASSIGN_OR_RETURN(
      p4::v1::TableEntry pi_entry,
      pdpi::PartialPdTableEntryToPiTableEntry(
          ir_p4info, gutil::ParseProtoOrDie<sai::TableEntry>(absl::Substitute(
                         R"pb(
                           vrf_table_entry {
                             match { vrf_id: "$0" }
                             action { no_action {} }
                           })pb",
                         vrf_id))));
  RETURN_IF_ERROR(pdpi::InstallPiTableEntry(&p4_session, pi_entry));

  ASSIGN_OR_RETURN(
      pi_entry,
      pdpi::PartialPdTableEntryToPiTableEntry(
          ir_p4info, gutil::ParseProtoOrDie<sai::TableEntry>(absl::Substitute(
                         R"pb(
                           acl_pre_ingress_table_entry {
                             match {}  # Wildcard match
                             action { set_vrf { vrf_id: "$0" } }
                             priority: 1
                           })pb",
                         vrf_id))));
  return pdpi::InstallPiTableEntry(&p4_session, pi_entry);
}

// Programs ipv6_table_entry using `vrf_id`, `ip_address` and `next_hop_id`.
absl::Status ProgramRoutesForIpv6(pdpi::P4RuntimeSession& p4_session,
                                  const pdpi::IrP4Info& ir_p4info,
                                  absl::string_view vrf_id,
                                  absl::string_view ip_address,
                                  absl::string_view next_hop_id) {
  auto ipv4_entry = gutil::ParseProtoOrDie<sai::Update>(absl::Substitute(
      R"pb(
        type: INSERT
        table_entry {
          ipv6_table_entry {
            match {
              vrf_id: "$0"
              ipv6_dst { value: "$1" prefix_length: 128 }
            }
            action { set_nexthop_id { nexthop_id: "$2" } }
          }
        })pb",
      vrf_id, ip_address, next_hop_id));
  p4::v1::WriteRequest write_request;
  ASSIGN_OR_RETURN(
      p4::v1::Update pi_entry, pdpi::PdUpdateToPi(ir_p4info, ipv4_entry),
      _.SetPrepend() << "Failed in PD table conversion to PI, entry: "
                     << ipv4_entry.DebugString() << " error: ");
  *write_request.add_updates() = pi_entry;
  return pdpi::SetMetadataAndSendPiWriteRequest(&p4_session, write_request);
}

// Programs a next hop entry for `port_id` and returns nexthop id if successful.
absl::StatusOr<std::string> ProgramNextHops(pdpi::P4RuntimeSession& p4_session,
                                            const pdpi::IrP4Info& ir_p4info,
                                            absl::string_view port_id) {
  p4::v1::WriteRequest pi_request;
  const std::string rif_id = absl::StrCat("rif-", port_id);
  const std::string neighbor_id = "fe80::2";
  const std::string src_mac = "00:02:03:04:05:06";
  const std::string dst_mac = "00:1a:11:17:5f:80";
  ASSIGN_OR_RETURN(*pi_request.add_updates(),
                   RouterInterfaceTableUpdate(ir_p4info, p4::v1::Update::INSERT,
                                              rif_id, port_id, src_mac));
  ASSIGN_OR_RETURN(*pi_request.add_updates(),
                   NeighborTableUpdate(ir_p4info, p4::v1::Update::INSERT,
                                       rif_id, neighbor_id, dst_mac));
  const std::string nexthop_id = absl::StrCat("nexthop-", port_id);
  ASSIGN_OR_RETURN(*pi_request.add_updates(),
                   NexthopTableUpdate(ir_p4info, p4::v1::Update::INSERT,
                                      nexthop_id, rif_id, neighbor_id));
  RETURN_IF_ERROR(
      pdpi::SetMetadataAndSendPiWriteRequest(&p4_session, pi_request));
  return nexthop_id;
}

}  // namespace

void SflowInbandTestFixture::SetUp() {
  thinkit::MirrorTestbed& testbed =
      GetParam().testbed_interface->GetMirrorTestbed();

  // Push gNMI config to main switch.
  const std::string& main_gnmi_config = GetParam().main_gnmi_config;
  ASSERT_OK(testbed.Environment().StoreTestArtifact("main_gnmi_config.txt",
                                                    main_gnmi_config));
  ASSERT_OK_AND_ASSIGN(main_p4_session_,
                       pins_test::ConfigureSwitchAndReturnP4RuntimeSession(
                           GetMainSwitch(), main_gnmi_config, GetP4Info()));
  ASSERT_OK_AND_ASSIGN(ir_p4_info_, pdpi::CreateIrP4Info(GetP4Info()));

  // Push gNMI config to peer switch.
  const std::string& peer_gnmi_config = GetParam().peer_gnmi_config;
  ASSERT_OK(testbed.Environment().StoreTestArtifact("peer_gnmi_config.txt",
                                                    peer_gnmi_config));
  ASSERT_OK_AND_ASSIGN(peer_p4_session_,
                       pins_test::ConfigureSwitchAndReturnP4RuntimeSession(
                           GetPeerSwitch(), peer_gnmi_config, GetP4Info()));

  // Find the loopback0 address for main switch & peer switch.
  ASSERT_OK_AND_ASSIGN(auto main_loopback0_ipv6s,
                       pins_test::ParseLoopbackIpv6s(main_gnmi_config));
  ASSERT_GT(main_loopback0_ipv6s.size(), 0) << absl::Substitute(
      "No loopback IP found for $0 testbed.", GetMainSwitch().ChassisName());
  netaddr::Ipv6Address main_loopback0_ipv6 = main_loopback0_ipv6s[0];
  ASSERT_OK_AND_ASSIGN(auto peer_loopback0_ipv6s,
                       pins_test::ParseLoopbackIpv6s(peer_gnmi_config));
  ASSERT_GT(peer_loopback0_ipv6s.size(), 0) << absl::Substitute(
      "No loopback IP found for $0 testbed.", GetPeerSwitch().ChassisName());
  const std::string peer_loopback0_ipv6 = peer_loopback0_ipv6s[0].ToString();
  collector_ipv6_ = main_loopback0_ipv6.ToString();
  ASSERT_NE(collector_ipv6_, peer_loopback0_ipv6);

  // Create GNMI stub for admin operations.
  ASSERT_OK_AND_ASSIGN(peer_gnmi_stub_, GetPeerSwitch().CreateGnmiStub());
  ASSERT_OK_AND_ASSIGN(
      auto port_id_per_port_name,
      pins_test::GetAllUpInterfacePortIdsByName(*peer_gnmi_stub_));
  ASSERT_GE(port_id_per_port_name.size(), 2);

  // Set peer switch with sFlow gNMI config.
  ASSERT_OK_AND_ASSIGN(auto gnmi_config_json,
                       json_yang::ParseJson(peer_gnmi_config));
  if (gnmi_config_json.find("openconfig-sampling:sampling") !=
          gnmi_config_json.end() &&
      gnmi_config_json["openconfig-sampling:sampling"]
                      ["openconfig-sampling-sflow:sflow"]["config"]
                      ["enabled"]) {
    sflow_enabled_by_config_ = true;
  }
  std::vector<std::pair<std::string, int>> collector_address_and_port;

  // Use main switch Loopback0 Ip address as collector.
  collector_address_and_port.push_back({collector_ipv6_, 6343});

  ASSERT_OK_AND_ASSIGN(traffic_port_,
                       GetUnusedUpPort(*peer_gnmi_stub_, /*used_port=*/""));

  absl::flat_hash_set<std::string> sflow_enabled_interfaces{
      traffic_port_.interface_name};
  ASSERT_OK_AND_ASSIGN(
      auto peer_gnmi_config_with_sflow,
      UpdateSflowConfig(peer_gnmi_config, peer_loopback0_ipv6,
                        collector_address_and_port, sflow_enabled_interfaces,
                        kInbandSamplingRate, kInbandSamplingHeaderSize));
  ASSERT_OK(testbed.Environment().StoreTestArtifact(
      "peer_gnmi_config_with_sflow.txt",
      json_yang::FormatJsonBestEffort(peer_gnmi_config_with_sflow)));
  ASSERT_OK(
      pins_test::PushGnmiConfig(GetPeerSwitch(), peer_gnmi_config_with_sflow));

  // Wait until all sFLow gNMI states are converged.
  ASSERT_OK(pins_test::WaitForCondition(
      VerifySflowStatesConverged, absl::Seconds(30), peer_gnmi_stub_.get(),
      peer_loopback0_ipv6, kInbandSamplingRate, kInbandSamplingHeaderSize,
      collector_address_and_port, sflow_enabled_interfaces));
}

void SflowInbandTestFixture::TearDown() {
  // Restore sFlow config on peer switch.
  if (!sflow_enabled_by_config_) {
    ASSERT_OK(SetSflowConfigEnabled(peer_gnmi_stub_.get(),
                                    /*enabled=*/false));
  }

  if (main_p4_session_ != nullptr) {
    EXPECT_OK(main_p4_session_->Finish());
  }
  if (peer_p4_session_ != nullptr) {
    EXPECT_OK(pdpi::ClearTableEntries(peer_p4_session_.get()));
    EXPECT_OK(peer_p4_session_->Finish());
  }
  GetParam().testbed_interface->TearDown();
}

// 1. Pick an interface and let main switch send some traffic via this
// interface.
// 2. Set rules on peer switch to forward sample to main switch.
// 3. Verifies on main switch that samples are generated.
// |-----------| Traffic  | --------- |
// | Main(SUT) | -------> |  Peer(CS) |
// |-----------| Inband   | --------  |
// | Main(SUT) | <------- |  Peer(CS) |
TEST_P(SflowInbandTestFixture, TestInbandPathToSflowCollector) {
  thinkit::MirrorTestbed& testbed =
      GetParam().testbed_interface->GetMirrorTestbed();
  const int packets_num = 2000;

  ASSERT_OK_AND_ASSIGN(
      Port inband_port,
      GetUnusedUpPort(*peer_gnmi_stub_, traffic_port_.interface_name));

  // Program rules to forward sample pkts to main switch loopback0 IPv6 address.
  // 1. Set a default vrf for all packets
  // 2. Install a route entry matching on the default vrf and route prefix of
  // the main switch loopback0 ipv6.
  // 3. Define the nexthop and dependent objects for the route entry action.
  const std::string vrf_id = "vrf-50";
  ASSERT_OK(
      SetSwitchVrfForAllPackets(*peer_p4_session_, GetIrP4Info(), vrf_id));

  ASSERT_OK_AND_ASSIGN(std::string next_hop_id,
                       ProgramNextHops(*peer_p4_session_, GetIrP4Info(),
                                       absl::StrCat(inband_port.port_id)));
  ASSERT_OK(ProgramRoutesForIpv6(*peer_p4_session_, GetIrP4Info(), vrf_id,
                                 collector_ipv6_, next_hop_id));

  // Start sflowtool on main switch.
  std::string sflow_result;
  ASSERT_OK_AND_ASSIGN(
      std::thread sflow_tool_thread,
      StartSflowCollector(
          GetParam().main_ssh_client, GetMainSwitch().ChassisName(),
          kSflowtoolLineFormatTemplate,
          /*sflowtool_runtime=*/packets_num / kInbandTrafficPps + 20,
          sflow_result));

  // Send packets from main switch.
  ASSERT_OK(SendNPacketsFromSwitch(
      packets_num, absl::StrCat(traffic_port_.port_id), GetIrP4Info(),
      *main_p4_session_, testbed.Environment()));

  // Wait for sflowtool to finish and check sFlow result.
  if (sflow_tool_thread.joinable()) {
    sflow_tool_thread.join();
  }
  EXPECT_OK(testbed.Environment().StoreTestArtifact("sflow_result.txt",
                                                    sflow_result));
  ASSERT_FALSE(sflow_result.empty());
}

}  // namespace pins
