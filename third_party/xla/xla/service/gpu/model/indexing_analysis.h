/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_GPU_MODEL_INDEXING_ANALYSIS_H_
#define XLA_SERVICE_GPU_MODEL_INDEXING_ANALYSIS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "llvm/ADT/Hashing.h"
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/gpu/fusions/tiling_util.h"
#include "xla/service/gpu/hlo_traversal.h"
#include "xla/service/gpu/model/affine_map_printer.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/shape.h"

namespace xla {
namespace gpu {

// Contains indexing maps for all N-dimensional tensor input operands that
// correspond to a particular output.
struct HloInstructionIndexing {
  std::string ToString(
      const AffineMapPrinter& printer = AffineMapPrinter()) const;
  void Print(std::ostream& out, const AffineMapPrinter& printer) const;

  // Returns true if the indexing was simplified.
  bool Simplify();

  // Creates a HloInstructionIndexing from a list of indexing maps for all
  // operands and sorted w.r.t. operand index, i.e. indexing_maps[i] corresponds
  // to operand[i] of the instruction.
  static HloInstructionIndexing FromIndexingMaps(
      absl::Span<const IndexingMap> indexing_maps);

  // Maps input operand index to the indexing map for one particular output.
  std::vector<absl::flat_hash_set<std::optional<IndexingMap>>> indexing_maps;
};
std::ostream& operator<<(std::ostream& out,
                         const HloInstructionIndexing& instr_indexing);

std::string ToString(const mlir::AffineMap& affine_map);

// Computes indexing maps for all input operands necessary to compute an element
// of the `output_id` instruction output.
HloInstructionIndexing ComputeOutputToInputIndexing(const HloInstruction* instr,
                                                    int output_id,
                                                    mlir::MLIRContext* ctx);

// Computes indexing maps for all output operands that the element of the
// `input_id` instruction input will participate in.
HloInstructionIndexing ComputeInputToOutputIndexing(const HloInstruction* instr,
                                                    int input_id,
                                                    mlir::MLIRContext* ctx);

using IndexingMapSet = absl::flat_hash_set<std::optional<IndexingMap>>;
using GroupedByOpIndexingMap =
    absl::flat_hash_map<const HloInstruction*, IndexingMapSet>;

// Computes indexing for every instruction within a fusion cluster.
std::optional<GroupedByOpIndexingMap> ComputeGroupedOutputToInputIndexing(
    const HloFusionAdaptor& fusion_adaptor, int output_id,
    mlir::MLIRContext* ctx);

// Groups indexing maps by instructions.
absl::flat_hash_map<const HloInstruction*, IndexingMapSet>
GroupIndexingMapsByProducers(const HloInstructionIndexing& indexing,
                             const HloInstruction* instr);

// Creates an indexing map for bitcasting from `input_shape` to `output_shape`.
// Equivalent to linearizing the input_shape index and then delinearizing it
// to output_shape.
IndexingMap GetBitcastMap(const Shape& input_shape, const Shape& output_shape,
                          mlir::MLIRContext* ctx);

// Creates an indexing map from the physical layout of the tensor to its logical
// layout.
IndexingMap GetIndexingMapFromPhysicalLayoutToLogical(const Shape& shape,
                                                      mlir::MLIRContext* ctx);

// Creates an indexing map from the logical layout of the tensor to its physical
// layout.
IndexingMap GetIndexingMapFromLogicalToPhysicalLayout(const Shape& shape,
                                                      mlir::MLIRContext* ctx);

// Creates an indexing map from thread and block IDs to elements of the tiled
// shape. Uses the same convention as KernelFusionInterface: dimensions 0 to 2
// are thread indices (currently only 0 is used), dimensions 3 to 5 are block
// indices (currently only 3 is used).
mlir::AffineMap GetBlockOffsetsForTiling(const Tiling& tiling,
                                         mlir::MLIRContext* ctx);
mlir::AffineMap GetThreadOffsetsForTiling(const Tiling& tiling,
                                          mlir::MLIRContext* ctx);

// Convenience functions for the two functions above
// (`GetBlockOffsestsForTiling` + `GetThreadOffsetsForTiling`). Also sets up
// the ranges of dimensions and symbols.
IndexingMap GetIndexingMapForTiling(const Tiling& tiling,
                                    mlir::MLIRContext* ctx);
IndexingMap GetIndexingMapForTiling(mlir::AffineMap block_offsets,
                                    mlir::AffineMap thread_offsets,
                                    const Tiling& tiling);

// Returns the shape of the output of the instruction.
const Shape& GetOutputShape(const HloInstruction* instr, int64_t output_id);

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_MODEL_INDEXING_ANALYSIS_H_
