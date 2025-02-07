// Copyright 2024 Google LLC
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

// This file defines the API for transforming a bmv2 protobuf (representing
// the input bmv2 json file) together with a pdpi protobuf (representing the
// p4info file) into our IR protobuf for consumption.

#ifndef P4_SYMBOLIC_IR_IR_H_
#define P4_SYMBOLIC_IR_IR_H_

#include <string>

#include "absl/status/statusor.h"
#include "glog/logging.h"
#include "google/protobuf/repeated_ptr_field.h"
#include "p4_pdpi/ir.pb.h"
#include "p4_symbolic/bmv2/bmv2.pb.h"
#include "p4_symbolic/ir/ir.pb.h"
#include "p4_symbolic/ir/table_entries.h"

namespace p4_symbolic::ir {

// The dataplane configuration of the switch.
// Used as input to our symbolic pipeline.
struct Dataplane {
  P4Program program;
  // Maps the full name of a table to a list of its entries.
  TableEntries entries;
};

// A special control name indicating the end of execution in a pipeline
// in P4-Symbolic's IR.
inline std::string EndOfPipeline() { return "__END_OF_PIPELINE__"; }

// A special parse state name indicating the end of execution in a parser in
// P4-Symbolic's IR. We decided to keep the parser CFG separated from the
// pipeline CFG for the symbolic evaluation.
inline std::string EndOfParser() { return "__END_OF_PARSER__"; }

// For any table application, BMv2 JSON (and P4-Symbolic IR) use a map from
// actions that may be executed as a result of table application to the next
// table/control that must be evaluated if the corresponding action is executed.
// This encodes conditionals on the result of table applications in P4 code.
// There are also two special action names that refer to whether any table
// entry was hit (table hit) or no table entry was hit (table miss). The
// following two functions return those special action names.
inline std::string TableHitAction() { return "__HIT__"; }
inline std::string TableMissAction() { return "__MISS__"; }

// Transforms bmv2 protobuf and pdpi protobuf into our IR protobuf.
absl::StatusOr<P4Program> Bmv2AndP4infoToIr(const bmv2::P4Program& bmv2,
                                            const pdpi::IrP4Info& pdpi);

// Returns a reference to the `ir::TableEntry` contained in the given `entry`.
const pdpi::IrTableEntry& GetPdpiIrEntryOrSketch(const ir::TableEntry& entry);

int GetIndex(const TableEntry& entry);

const std::string& GetTableName(const TableEntry& entry);
const std::string& GetTableName(const ConcreteTableEntry& entry);
const std::string& GetTableName(const SymbolicTableEntry& entry);

int GetPriority(const TableEntry& entry);
int GetPriority(const ConcreteTableEntry& entry);
int GetPriority(const SymbolicTableEntry& entry);

const google::protobuf::RepeatedPtrField<pdpi::IrMatch>& GetMatches(
    const TableEntry& entry);
const google::protobuf::RepeatedPtrField<pdpi::IrMatch>& GetMatches(
    const ConcreteTableEntry& entry);
const google::protobuf::RepeatedPtrField<pdpi::IrMatch>& GetMatches(
    const SymbolicTableEntry& entry);

}  // namespace p4_symbolic::ir

#endif  // P4_SYMBOLIC_IR_IR_H_
