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
#include "xla/service/gpu/fusions/transpose.h"

#include <array>
#include <cstdint>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/AtomicOrdering.h"
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/permutation_util.h"
#include "xla/service/gpu/elemental_ir_emitter.h"
#include "xla/service/gpu/fusions/tiling_util.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/ir_emitter_context.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/model/indexing_analysis.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/service/gpu/target_util.h"
#include "xla/service/llvm_ir/fused_ir_emitter.h"
#include "xla/service/llvm_ir/ir_array.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/status.h"
#include "xla/util.h"

namespace xla {
namespace gpu {
namespace {

Tiling ComputeTransposeTiling(const TransposeDescription& tiled_transpose) {
  constexpr int kNumRows = 4;
  static_assert(WarpSize() % kNumRows == 0);

  // 3D view over the output shape.
  Vector3 transposed_dims = tiled_transpose.dimensions;
  Vector3 permutation = tiled_transpose.permutation;

  // Note: the supported permutations are their own inverses. Therefore we
  // always use the permutation, even when we want the inverse.
  CHECK((permutation == Vector3{0, 2, 1}) || (permutation == Vector3{2, 1, 0}));

  absl::InlinedVector<int64_t, 4> input_dims{transposed_dims[permutation[0]],
                                             transposed_dims[permutation[1]],
                                             transposed_dims[permutation[2]]};

  // We tile along the minor dimensions pre- and post-transpose.
  absl::InlinedVector<int64_t, 4> tile_sizes{1, 1, 1};
  tile_sizes[permutation[2]] = WarpSize() / kNumRows;
  absl::InlinedVector<int64_t, 4> num_threads{1, 1, WarpSize()};
  num_threads[permutation[2]] = kNumRows;

  return Tiling(input_dims, tile_sizes, num_threads);
}

void MaybeEmitFenceForAMDGPU(llvm::IRBuilder<>* builder,
                             IrEmitterContext& ir_emitter_context) {
  auto* module = builder->GetInsertBlock()->getModule();
  if (IsAMDGPU(module) &&
      ir_emitter_context.rocm_compute_capability().fence_before_barrier()) {
    builder->CreateFence(
        llvm::AtomicOrdering::SequentiallyConsistent,
        builder->getContext().getOrInsertSyncScopeID("workgroup"));
  }
}

void EmitSyncThreads(llvm::IRBuilder<>* builder,
                     IrEmitterContext& ir_emitter_context) {
  MaybeEmitFenceForAMDGPU(builder, ir_emitter_context);
  EmitCallToTargetIntrinsic(TargetIntrinsicID::kBarrierId, {}, {}, builder);
}

llvm_ir::IrArray::Index PermuteIndex(const llvm_ir::IrArray::Index& index,
                                     absl::Span<const int64_t> permutation) {
  return llvm_ir::IrArray::Index{Permute(index.multidim(), permutation),
                                 Permute(index.dims(), permutation),
                                 index.GetType()};
}

mlir::AffineMap PermuteAffineMapResults(mlir::AffineMap map,
                                        absl::Span<const int64_t> permutation) {
  return map.getSubMap(
      std::vector<unsigned>(permutation.begin(), permutation.end()));
}

}  // namespace

TransposeFusion::TransposeFusion(const HloFusionAnalysis& analysis)
    : analysis_(analysis),
      tiling_(ComputeTransposeTiling(analysis.tiled_transpose())) {
  for (auto [root, hero] :
       llvm::zip(analysis_.fusion_roots(), analysis_.fusion_heroes())) {
    if (auto transpose = GetDescriptionForTiledTransposeEmitter(*root, *hero)) {
      permutation_ = transpose->permutation;
      break;
    }
  }
}

absl::Status TransposeFusion::EmitKernel(IrEmitterContext& ir_emitter_context,
                                         const HloFusionInstruction& fusion,
                                         const LaunchDimensions& launch_dims,
                                         std::vector<llvm_ir::IrArray> inputs,
                                         std::vector<llvm_ir::IrArray> outputs,
                                         llvm::IRBuilder<>* builder) const {
  const auto& hlo_roots = analysis_.fusion_roots();
  GpuElementalIrEmitter elemental_emitter(ir_emitter_context, builder);
  FusedIrEmitter fused_emitter(elemental_emitter);
  for (auto [i, input] : llvm::enumerate(inputs)) {
    HloInstruction* fused_operand = fusion.fused_parameter(i);
    fused_emitter.BindGenerator(
        *fused_operand, [input = input, builder,
                         fused_operand](const llvm_ir::IrArray::Index& index) {
          return input.EmitReadArrayElement(index, builder,
                                            fused_operand->name());
        });
  }

  absl::flat_hash_map<const HloInstruction*,
                      std::vector<std::pair<int64_t, const HloInstruction*>>>
      transposes_to_roots;
  // Keep a list of deduplicated transpose heroes separate from
  // transposes_to_roots to make the CodeGen deterministic.
  std::vector<TransposeDescription> transposes;
  transposes.reserve(hlo_roots.size());
  std::vector<std::pair<int64_t, const HloInstruction*>> extra_outputs;

  for (const auto& [output_idx, root] : llvm::enumerate(hlo_roots)) {
    const auto& hero = *analysis_.fusion_heroes()[output_idx];
    auto transpose_descr = GetDescriptionForTiledTransposeEmitter(*root, hero);
    if (transpose_descr.has_value()) {
      auto iterator_inserted = transposes_to_roots.insert(std::make_pair(
          &hero, std::vector<std::pair<int64_t, const HloInstruction*>>{
                     {output_idx, root}}));
      if (iterator_inserted.second) {
        transposes.push_back(*transpose_descr);
      } else {
        iterator_inserted.first->second.push_back({output_idx, root});
      }
    } else {
      extra_outputs.push_back({output_idx, root});
    }
  }

  absl::flat_hash_map<const HloInstruction*, llvm_ir::SharedMemoryTile> tiles;
  Vector3 permutation;
  for (const auto& [tile_idx, tr] : llvm::enumerate(transposes)) {
    permutation = tr.permutation;
    auto tile_size = tiling_.GetBlockTileSize();
    ++tile_size.back();  // Prevent bank conflicts.
    auto* module = ir_emitter_context.llvm_module();
    tiles[tr.instr] = llvm_ir::AllocateSharedMemoryTile(
        module,
        llvm_ir::PrimitiveTypeToIrType(tr.instr->shape().element_type(),
                                       module),
        tile_size, absl::StrCat("tr_tile_", tile_idx));
  }

  auto tile_generator = [&](const TilingThreadIdInfo& thread_id_info,
                            const llvm_ir::IrArray::Index& tile_start_index,
                            absl::Span<llvm::Value* const> tile_dimensions) {
    // Copy input parameter values to shared memory buffers:
    // tile[thread_id_y, thread_id_x] = input[index]
    EmitTile(builder, tiling_, thread_id_info, tile_dimensions,
             [&](absl::Span<llvm::Value* const> index_in_tile) {
               auto index = tile_start_index.AddOffset(index_in_tile, builder);
               for (const auto& tr : transposes) {
                 auto input_gen =
                     *fused_emitter.GetGenerator(*tr.instr->operand(0));
                 auto input_index = index.SourceIndexOfBitcast(
                     tr.instr->operand(0)->shape(), builder);
                 llvm::Value* value = *input_gen(input_index);
                 tiles[tr.instr].Store(value, index_in_tile, builder);
               }

               // Compute all extra output values before writing them. This
               // avoids overwriting aliased input/output values before all
               // reads occurred.
               std::vector<std::tuple<llvm_ir::IrArray, llvm_ir::IrArray::Index,
                                      llvm::Value*>>
                   scheduled_writes;
               for (const auto& [output_idx, root] : extra_outputs) {
                 auto extra_output_index =
                     index.SourceIndexOfBitcast(root->shape(), builder);
                 auto output_gen = *fused_emitter.GetGenerator(*root);
                 llvm::Value* output_value = *output_gen(extra_output_index);
                 scheduled_writes.emplace_back(
                     outputs[output_idx], extra_output_index, output_value);
               }

               for (const auto& [output, idx, value] : scheduled_writes) {
                 output.EmitWriteArrayElement(idx, value, builder);
               }
             });

    EmitSyncThreads(builder, ir_emitter_context);

    auto output_tile_index = PermuteIndex(tile_start_index, permutation);
    auto transposed_tile_dimensions = Permute(tile_dimensions, permutation);

    EmitTile(
        builder, tiling_, thread_id_info, transposed_tile_dimensions,
        /*emit_elem_function=*/
        [&](absl::Span<llvm::Value* const> index_in_tile) {
          auto index = output_tile_index.AddOffset(index_in_tile, builder);
          for (const auto& tr : transposes) {
            llvm::Value* loaded = tiles[tr.instr].Load(
                Permute(index_in_tile, permutation), builder);

            FusedIrEmitter fused_emitter(elemental_emitter);
            fused_emitter.BindGenerator(
                *tr.instr,
                [&](const llvm_ir::IrArray::Index&) { return loaded; });
            for (int64_t i = 0;
                 i < fusion.fused_instructions_computation()->num_parameters();
                 ++i) {
              llvm_ir::IrArray ir_array = inputs[i];
              HloInstruction* fused_operand = fusion.fused_parameter(i);
              fused_emitter.BindGenerator(
                  *fused_operand, [=](const llvm_ir::IrArray::Index& index) {
                    return ir_array.EmitReadArrayElement(index, builder,
                                                         fused_operand->name());
                  });
            }

            // Apply code generation for the code after the real hero.
            // Compute all output values before writing them. This avoids
            // overwriting aliased input/output values before all reads
            // occurred.
            std::vector<std::tuple<llvm_ir::IrArray, llvm_ir::IrArray::Index,
                                   llvm::Value*>>
                scheduled_writes;
            for (const auto& [output_idx, root] :
                 transposes_to_roots[tr.instr]) {
              TF_ASSIGN_OR_RETURN(llvm_ir::ElementGenerator gen,
                                  fused_emitter.GetGenerator(*root));

              // Both for emission and writing it should be
              // index-as-transformed by the computation.
              auto untiled_index =
                  index.SourceIndexOfBitcast(root->shape(), builder);
              TF_ASSIGN_OR_RETURN(llvm::Value * generated, gen(untiled_index));
              scheduled_writes.emplace_back(outputs[output_idx], untiled_index,
                                            generated);
            }
            for (const auto& [output, idx, value] : scheduled_writes) {
              output.EmitWriteArrayElement(idx, value, builder);
            }
          }
          return absl::OkStatus();
        });
  };

  llvm::Type* index_type =
      GetIndexTypeForKernel(&fusion, launch_dims.launch_bound(), builder);
  return EmitTilingKernel(builder, tiling_, index_type, tile_generator)
      .status();
}

LaunchDimensions TransposeFusion::launch_dimensions() const {
  return LaunchDimensions(tiling_.GetNumBlocks(),
                          tiling_.GetNumThreadsPerBlock());
}

std::optional<IndexingMap> TransposeFusion::ComputeThreadIdToOutputIndexing(
    int64_t root_index, mlir::MLIRContext* ctx) const {
  const auto& hero = *analysis_.fusion_heroes()[root_index];
  const auto& root = *analysis_.fusion_roots()[root_index];
  if (!GetDescriptionForTiledTransposeEmitter(root, hero)) {
    // Non-transpose roots are elementwise by definition.
    return ComputeThreadIdToInputIndexing(root_index, 0, ctx);
  }

  // The block offsets are permuted, but the thread offsets remain the same.
  auto block_offset = GetBlockOffsetsForTiling(tiling_, ctx)
                          .getSubMap(std::vector<unsigned>{permutation_.begin(),
                                                           permutation_.end()});
  auto thread_offset = GetThreadOffsetsForTiling(tiling_, ctx);
  auto permuted_tiled_shape =
      ShapeUtil::MakeShape(U8, Permute(tiling_.GetShape(), permutation_));

  return ComposeIndexingMaps(
      GetIndexingMapForTiling(block_offset, thread_offset, tiling_),
      GetBitcastMap(permuted_tiled_shape, hero.shape(), ctx),
      /*simplify=*/false);
}

std::optional<IndexingMap> TransposeFusion::ComputeThreadIdToInputIndexing(
    int64_t root_index, int64_t hero_operand_index,
    mlir::MLIRContext* ctx) const {
  const auto& hero = *analysis_.fusion_heroes()[root_index];

  return ComposeIndexingMaps(
      GetIndexingMapForTiling(tiling_, ctx),
      GetBitcastMap(tiling_.GetXlaShape(), hero.operand(0)->shape(), ctx),
      /*simplify=*/false);
}

}  // namespace gpu
}  // namespace xla
