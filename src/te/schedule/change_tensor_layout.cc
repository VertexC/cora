#include <tvm/te/operation.h>
#include <tvm/te/schedule.h>
#include <tvm/tir/stmt_functor.h>

#include <algorithm>

#include "graph.h"
#include "message_passing.h"
#include "tensor_layout_utils.h"

#define COUT std::cout << "[CTL] "

namespace tvm {
namespace te {

Map<Dimension, Range> GetIndexDimRangeFromLoopDimRange(const ComputeOpNode* compute_op,
                                                       const Map<IterVar, Range>& dom_map) {
  Map<Dimension, Range> ret;
  for (const auto& root_dim : compute_op->root_index_dimensions) {
    if (root_dim->type <= DimensionNode::kRangeDim) {
      const auto& iv = compute_op->GetIterVarFromDim(0, root_dim);
      ret.Set(root_dim, dom_map.count(iv) ? dom_map.at(iv) : iv->dom);
    } else {
      UninterpFun ufun = compute_op->GetDimVarEntry(0, root_dim).value_expr;
      bool non_constant = false;
      CHECK(ufun->dimensions.defined());
      for (auto arg_dim : ufun->dimensions) {
        Range r = dom_map.at(compute_op->GetIterVarFromDim(0, arg_dim));
        if (!tir::is_one(r->extent)) {
          non_constant = true;
        }
      }

      if (non_constant) {
        ret.Set(root_dim, ufun->range);
      } else {
        Array<PrimExpr> args;
        for (auto arg_dim : ufun->dimensions) {
          Range r = dom_map.at(compute_op->GetIterVarFromDim(0, arg_dim));
          args.push_back(r->min);
        }
        ret.Set(root_dim, Range::make_by_min_extent(
                              UninterpFun::MakeCallTo(ufun, args, ufun->dimensions), 1));
      }
    }
  }

  return ret;
}

Array<Range> ComputeRealizeBounds(const Stage& stage, const ComputeOpNode* compute_op,
                                  const Map<IterVar, Range>& dom_map) {
  std::unordered_map<const DimensionNode*, Range> state;

  for (const auto& di : compute_op->all_dimensions) {
    if (di->dim->isLoopDim()) {
      const auto& iv = compute_op->GetIterVarFromDim(0, di->dim);
      state[di->dim.operator->()] = dom_map.count(iv) ? dom_map.at(iv) : iv->dom;
    }
  }

  for (const auto& it : GetIndexDimRangeFromLoopDimRange(compute_op, dom_map)) {
    state[it.first.operator->()] = it.second;
  }

  DimensionPassDownDomain(stage, compute_op, &state, true);

  Array<Range> new_shape;
  for (auto dim : stage->dim_relation_graph->leaf_dimensions) {
    new_shape.push_back(state[dim.operator->()]);
  }
  CHECK(new_shape.size() > 0) << stage;
  return new_shape;
}

void ReplaceIndexTensorByDenseTensor(Schedule& sch, Stage s, Tensor old_tensor, Tensor new_tensor,
                                     Array<Dimension> old_dims, Array<Dimension> new_dims) {
  s->op = new_tensor->op;
  // Refresh the feed graph
  auto feed_graph = GetFeedGraph(sch, true);

  auto readers = Array<Operation>(feed_graph.at(old_tensor));
  AccessPatternCollector collector(old_tensor, old_dims, readers);
  collector.collect();
  PatternsSet patterns = collector.access_patterns;
  AccessToPatternMap access_to_pattern_map = collector.access_to_pattern_map;

  CHECK_EQ(patterns.size(), 1)
      << "Tensor dense indexing suported for single access pattern tensors " << old_tensor;
  // CHECK(patterns.size() <= 1) << "Tensor dense indexing suported for single access pattern
  // tensors "
  // << old_tensor;

  std::unordered_map<Tensor, Tensor> vmap;
  std::unordered_map<Tensor, Tensor> rvmap;
  sch->InvalidateCache();
  sch->InitCache();
  auto& op2stage_ = sch->op2stage_cache_;
  for (Operation op : readers) {
    Stage op_stage = op2stage_.at(op.get());
    Operation repl_op =
        ReplaceInputs(op, &access_to_pattern_map, new_tensor, new_dims, old_dims, false);
    CHECK(!repl_op.same_as(op_stage->op))
        << "Cannot find tensor " << old_tensor << " in the inputs to " << repl_op;
    vmap[op_stage->op.output(0)] = repl_op.output(0);
    rvmap[repl_op.output(0)] = op_stage->op.output(0);
    op_stage->op = repl_op;
  }
  ReplaceDataFlow(sch->stages, sch->cacheTensorInfos, &vmap, &rvmap);
}

Operation CreateDenselyIndexedComputeOpCopy(Stage s, const ComputeOpNode* old_op,
                                            const Map<IterVar, Range>& dom_map) {
  auto n = make_object<ComputeOpNode>();

  n->realize_bounds = ComputeRealizeBounds(s, old_op, dom_map);
  n->who_set_realize_bounds = "change_tensor_layout.cc:127";

  // OperationNode fields
  n->name = std::move(old_op->name);
  n->tag = std::move(old_op->tag);
  n->attrs = std::move(old_op->attrs);

  // BaseVarDimOpNode fields
  n->dim2var_maps = std::move(old_op->dim2var_maps);
  n->var2dim_map = std::move(old_op->var2dim_map);

  // BaseComputeOpNode fields
  n->axis = std::move(old_op->axis);
  n->reduce_axis = std::move(old_op)->reduce_axis;
  for (const auto& r : n->realize_bounds) {
    n->output_shape_storage.push_back(r->extent);
  }
  n->all_dimensions = std::move(old_op->all_dimensions);
  n->root_index_dimensions = s->dim_relation_graph->leaf_dimensions;

  // ComputeOpNode fields
  n->body = std::move(old_op->body);
  n->pred = std::move(old_op->pred);

  Operation new_op = std::move(Operation(n));
  return new_op;
}

const DimensionChangeNode* GetChangeRel(Stage s) {
  const DimensionChangeNode* change_rel = nullptr;
  CHECK(s->dim_relation_graph->relations.defined());
  for (const auto& rel : s->dim_relation_graph->relations) {
    if ((change_rel = rel.as<DimensionChangeNode>())) {
      break;
    }
  }
  return change_rel;
}

void IndexByDenseLayoutChange(Schedule& sch, const Map<IterVar, Range>& dom_map) {
  auto feed_graph = GetFeedGraph(sch, true);

  std::unordered_set<const Object*> scan_updates_and_inits;
  std::unordered_set<const Object*> conditional_cases;
  Array<Stage> scan_stages;
  Array<Stage> compute_stages;
  Array<Stage> conditional_stages;
  for (auto s : sch->stages) {
    if (const ScanOpNode* scan = s->op.as<ScanOpNode>()) {
      auto change_rel = GetChangeRel(s);
      if (change_rel) {
        for (Tensor t : scan->update) {
          scan_updates_and_inits.insert(t->op.get());
        }
        for (Tensor t : scan->init) {
          scan_updates_and_inits.insert(t->op.get());
        }
        scan_stages.push_back(s);
      }
    } else if (const ConditionalOpNode* conditional = s->op.as<ConditionalOpNode>()) {
      auto change_rel = GetChangeRel(s);
      if (change_rel) {
        for (Tensor t : conditional->then_case) {
          conditional_cases.insert(t->op.get());
        }
        for (Tensor t : conditional->else_case) {
          conditional_cases.insert(t->op.get());
        }
        conditional_stages.push_back(s);
      }
    } else if (s->op.as<ComputeOpNode>()) {
      compute_stages.push_back(s);
    }
  }

  for (auto s : compute_stages) {
    if (s->attach_type == kInlinedAlready) continue;
    auto compute_op = s->op.as<ComputeOpNode>();
    CHECK(compute_op);
    if (scan_updates_and_inits.count(s->op.get()) || conditional_cases.count(s->op.get())) {
      continue;
    }

    const DimensionChangeNode* change_rel = GetChangeRel(s);
    if (change_rel) {
      CHECK(compute_op) << "Only compute ops supported for dense tensor indexing";
      CHECK_EQ(compute_op->num_outputs(), 1)
          << "Only single output ops supported for dense indexing";
      Tensor tensor = s->op.output(0);
      CHECK(feed_graph.count(tensor)) << "Tensor cannot be found in feed graph";

      Operation new_op = CreateDenselyIndexedComputeOpCopy(s, compute_op, dom_map);
      ReplaceIndexTensorByDenseTensor(sch, s, tensor, new_op.output(0),
                                      compute_op->root_index_dimensions,
                                      s->dim_relation_graph->leaf_dimensions);

      // Refresh the feed graph
      feed_graph = GetFeedGraph(sch, true);
    } else {
      const_cast<ComputeOpNode*>(compute_op)
          ->set_realize_bounds(ComputeRealizeBounds(s, compute_op, dom_map),
                               "change_tensor_layout.cc:185");
      continue;
    }
  }

  // Now process the scans
  for (auto stage : scan_stages) {
    sch->InvalidateCache();
    sch->InitCache();
    auto scan_op = stage->op.as<ScanOpNode>();

    // Replace state placeholders inside the scan
    Array<Array<Dimension>> all_old_dims;
    Array<Array<Dimension>> all_new_dims;
    int num_outputs = scan_op->num_outputs();
    Array<Tensor> new_states;
    for (Tensor old_state : scan_op->state_placeholder) {
      auto old_state_op = old_state->op.as<PlaceholderOpNode>();
      auto state_stage = sch->op2stage_cache_[old_state_op];

      // Create new state placeholder
      Array<PrimExpr> new_shape;
      for (auto iv : old_state_op->axis) {
        new_shape.push_back(iv->dom->extent);
      }

      Array<Dimension> old_dims = old_state_op->self_index_dimensions;
      Array<Dimension> new_dims;
      for (const auto& di : old_state_op->all_dimensions) {
        if (di->dim->isLoopDim()) new_dims.push_back(di->dim);
      }
      all_old_dims.push_back(old_dims);
      all_new_dims.push_back(new_dims);

      Operation new_state_op =
          PlaceholderOpNode::make(old_state_op->name + ".d", new_shape, old_state_op->dtype,
                                  new_dims, old_state_op->all_dimensions);

      Tensor new_state = new_state_op.output(old_state->value_index);
      new_states.push_back(new_state);

      // Replace the state placeholder
      ReplaceIndexTensorByDenseTensor(sch, state_stage, old_state, new_state, old_dims, new_dims);
    }

    scan_op = stage->op.as<ScanOpNode>();

    // New updates
    sch->InvalidateCache();
    sch->InitCache();
    Array<Tensor> new_updates;
    for (int i = 0; i < num_outputs; ++i) {
      Tensor old_update = scan_op->update[i];
      auto update_op = old_update->op.as<ComputeOpNode>();
      Stage update_stage = sch->op2stage_cache_[update_op];
      Operation new_update_op = CreateDenselyIndexedComputeOpCopy(update_stage, update_op, dom_map);
      new_updates.push_back(new_update_op.output(0));
      update_stage->op = new_update_op;
    }

    Array<Tensor> new_inits;
    for (int i = 0; i < num_outputs; ++i) {
      Tensor old_init = scan_op->init[i];
      if (old_init->op.as<PlaceholderOpNode>()) {
        new_inits.push_back(old_init);
      } else {
        auto init_op = old_init->op.as<ComputeOpNode>();
        Stage init_stage = sch->op2stage_cache_[init_op];
        Operation new_init_op = CreateDenselyIndexedComputeOpCopy(init_stage, init_op, dom_map);
        new_inits.push_back(new_init_op.output(0));
        init_stage->op = new_init_op;
      }
    }

    // Create a new scan op:
    Operation new_scan_op;
    {
      auto n = make_object<ScanOpNode>();
      n->name = scan_op->name + ".d";
      n->tag = scan_op->tag;
      n->attrs = scan_op->attrs;

      n->dim2var_maps = scan_op->dim2var_maps;
      n->var2dim_map = scan_op->var2dim_map;

      n->scan_axis = scan_op->scan_axis;
      n->explicit_dims = scan_op->explicit_dims;
      n->explicit_loop_ivs = scan_op->explicit_loop_ivs;
      n->init = new_inits;
      n->update = new_updates;
      n->state_placeholder = new_states;
      n->inputs = scan_op->inputs;
      n->scan_dim = scan_op->scan_dim;
      n->init_separate = scan_op->init_separate;

      for (int i = 0; i < num_outputs; ++i) {
        auto new_update_op = new_updates[i]->op.as<ComputeOpNode>();
        for (size_t k = 0; k < new_update_op->root_index_dimensions.size(); ++k) {
          auto dim = new_update_op->root_index_dimensions[k];
          n->spatial_dimensions_.push_back(dim);
          n->spatial_axis_.push_back(n->dim2var_maps[i].at(dim.as<DimensionNode>()).iv);
        }
      }

      new_scan_op = Operation(n);
    }

    // Replace the scan
    for (int i = 0; i < num_outputs; ++i) {
      ReplaceIndexTensorByDenseTensor(sch, stage, GetRef<Operation>(scan_op).output(i),
                                      new_scan_op.output(i), all_old_dims[i], all_new_dims[i]);
    }
  }

  // Now process the conditionals
  for (auto stage : conditional_stages) {
    sch->InvalidateCache();
    sch->InitCache();
    auto conditional_op = stage->op.as<ConditionalOpNode>();

    // Replace state placeholders inside the conditional
    Array<Array<Dimension>> all_old_dims;
    Array<Array<Dimension>> all_new_dims;
    int num_outputs = conditional_op->num_outputs();

    // New thens
    sch->InvalidateCache();
    sch->InitCache();
    Array<Tensor> new_then_cases;
    for (int i = 0; i < num_outputs; ++i) {
      Tensor old_then_case = conditional_op->then_case[i];
      auto then_case_op = old_then_case->op.as<ComputeOpNode>();
      Stage then_case_stage = sch->op2stage_cache_[then_case_op];
      Operation new_then_case_op =
          CreateDenselyIndexedComputeOpCopy(then_case_stage, then_case_op, dom_map);

      Array<Dimension> old_dims;
      for (const auto& dim : then_case_op->root_index_dimensions) {
        old_dims.push_back(dim);
      }
      all_old_dims.push_back(old_dims);

      Array<Dimension> new_dims;
      for (const auto& dim : new_then_case_op.as<ComputeOpNode>()->root_index_dimensions) {
        new_dims.push_back(dim);
      }
      all_new_dims.push_back(new_dims);

      new_then_cases.push_back(new_then_case_op.output(0));
      then_case_stage->op = new_then_case_op;
    }

    Array<Tensor> new_else_cases;
    for (int i = 0; i < num_outputs; ++i) {
      Tensor old_else_case = conditional_op->else_case[i];
      if (old_else_case->op.as<PlaceholderOpNode>()) {
        new_else_cases.push_back(old_else_case);
      } else {
        auto else_case_op = old_else_case->op.as<ComputeOpNode>();
        Stage else_case_stage = sch->op2stage_cache_[else_case_op];
        Operation new_else_case_op =
            CreateDenselyIndexedComputeOpCopy(else_case_stage, else_case_op, dom_map);
        new_else_cases.push_back(new_else_case_op.output(0));
        else_case_stage->op = new_else_case_op;
      }
    }

    // Create a new conditional op:
    Operation new_conditional_op;
    {
      auto n = make_object<ConditionalOpNode>();
      n->name = conditional_op->name + ".d";
      n->tag = conditional_op->tag;
      n->attrs = conditional_op->attrs;

      n->dim2var_maps = conditional_op->dim2var_maps;
      n->var2dim_map = conditional_op->var2dim_map;

      n->from_then = conditional_op->from_then;
      n->then_case = new_then_cases;
      n->from_else = conditional_op->from_else;
      n->else_case = new_else_cases;
      n->condition = conditional_op->condition;
      n->explicit_dims = conditional_op->explicit_dims;
      n->explicit_loop_ivs = conditional_op->explicit_loop_ivs;

      n->spatial_axis_;
      n->spatial_dimensions_;
      n->explicit_dimensions;

      for (int i = 0; i < num_outputs; ++i) {
        auto then_case_op = new_then_cases[i]->op.as<ComputeOpNode>();
        for (size_t k = 0; k < then_case_op->root_index_dimensions.size(); ++k) {
          auto dim = then_case_op->root_index_dimensions[k];
          n->spatial_dimensions_.push_back(dim);
          n->spatial_axis_.push_back(n->dim2var_maps[i].at(dim.as<DimensionNode>()).iv);
        }
      }

      new_conditional_op = Operation(n);
    }

    // Replace the conditional
    for (int i = 0; i < num_outputs; ++i) {
      ReplaceIndexTensorByDenseTensor(sch, stage, GetRef<Operation>(conditional_op).output(i),
                                      new_conditional_op.output(i), all_old_dims[i],
                                      all_new_dims[i]);
    }
  }
}

void Schedule::freeze_tensor_dimensions(const Map<IterVar, Range>& dom_map) {
  IndexByDenseLayoutChange(*this, dom_map);
}

Tensor Schedule::split_tensor_dimension(const Tensor& tensor, const size_t dim_idx,
                                        const int factor) {
  auto compute_op = const_cast<ComputeOpNode*>(tensor->op.as<ComputeOpNode>());
  Stage s = this->operator[](tensor->op);
  CHECK(compute_op) << "Layout changes allowed only for ComputeOp";
  CHECK(dim_idx < s->dim_relation_graph->leaf_dimensions.size());
  Dimension parent = s->dim_relation_graph->leaf_dimensions[dim_idx];
  Dimension inner = DimensionNode::make(parent->name + ".inner", parent->type);
  Dimension outer = DimensionNode::make(parent->name + ".outer", parent->type);

  Array<DimensionRelation>& relations = s->dim_relation_graph->relations;
  relations.push_back(DimensionSplitNode::make(parent, outer, inner, factor, PrimExpr()));

  auto leaf_dims = s->dim_relation_graph->leaf_dimensions.CopyOnWrite();
  size_t pos = std::distance(leaf_dims->data.begin(),
                             std::find(leaf_dims->data.begin(), leaf_dims->data.end(), parent));
  leaf_dims->data.erase(leaf_dims->data.begin() + pos);
  leaf_dims->data.insert(leaf_dims->data.begin() + pos, inner);
  leaf_dims->data.insert(leaf_dims->data.begin() + pos, outer);

  return tensor;
}

Tensor Schedule::fuse_tensor_dimensions(const Tensor& tensor, const size_t dim_idx1,
                                        const size_t dim_idx2) {
  auto compute_op = const_cast<ComputeOpNode*>(tensor->op.as<ComputeOpNode>());
  Stage s = this->operator[](tensor->op);
  CHECK(compute_op) << "Layout changes allowed only for ComputeOp";
  CHECK(dim_idx1 < s->dim_relation_graph->leaf_dimensions.size());
  CHECK(dim_idx2 < s->dim_relation_graph->leaf_dimensions.size());
  CHECK(dim_idx1 == dim_idx2 - 1);

  Dimension inner = s->dim_relation_graph->leaf_dimensions[dim_idx2];
  Dimension outer = s->dim_relation_graph->leaf_dimensions[dim_idx1];
  auto fused_type =
      (inner->type == DimensionNode::kFunDim) || (outer->type == DimensionNode::kFunDim)
          ? DimensionNode::kFunDim
          : DimensionNode::kRangeDim;
  Dimension fused = DimensionNode::make(outer->name + "." + inner->name + ".fused", fused_type);

  Array<DimensionRelation>& relations = s->dim_relation_graph->relations;
  relations.push_back(DimensionFuseNode::make(outer, inner, fused));

  auto leaf_dims = s->dim_relation_graph->leaf_dimensions.CopyOnWrite();
  size_t pos1 = std::distance(leaf_dims->data.begin(),
                              std::find(leaf_dims->data.begin(), leaf_dims->data.end(), inner));
  leaf_dims->data.erase(leaf_dims->data.begin() + pos1);
  size_t pos2 = std::distance(leaf_dims->data.begin(),
                              std::find(leaf_dims->data.begin(), leaf_dims->data.end(), outer));
  leaf_dims->data.erase(leaf_dims->data.begin() + pos2);
  leaf_dims->data.insert(leaf_dims->data.begin() + pos2, fused);

  return tensor;
}

Tensor Schedule::reorder_tensor_dimensions(const Tensor& tensor, const size_t dim_idx1,
                                           const size_t dim_idx2) {
  auto compute_op = const_cast<ComputeOpNode*>(tensor->op.as<ComputeOpNode>());
  Stage s = this->operator[](tensor->op);
  CHECK(compute_op) << "Layout changes allowed only for ComputeOp";
  CHECK(dim_idx1 < s->dim_relation_graph->leaf_dimensions.size());
  CHECK(dim_idx2 < s->dim_relation_graph->leaf_dimensions.size());
  CHECK(dim_idx1 == dim_idx2 - 1);

  Dimension dim1 = s->dim_relation_graph->leaf_dimensions[dim_idx1];
  Dimension dim2 = s->dim_relation_graph->leaf_dimensions[dim_idx2];

  auto leaf_dims = s->dim_relation_graph->leaf_dimensions.CopyOnWrite();
  size_t pos1 = std::distance(leaf_dims->data.begin(),
                              std::find(leaf_dims->data.begin(), leaf_dims->data.end(), dim1));
  size_t pos2 = std::distance(leaf_dims->data.begin(),
                              std::find(leaf_dims->data.begin(), leaf_dims->data.end(), dim2));
  leaf_dims->data[pos2] = dim1;
  leaf_dims->data[pos1] = dim2;

  return tensor;
}

Tensor Schedule::index_by_dense_dimensions(const Tensor& tensor) {
  Stage s = this->operator[](tensor->op);
  // std::cout << "[IDD] Op " << tensor->op << " " << s << std::endl;
  Array<Dimension> dense_dims;
  if (auto compute_op = const_cast<ComputeOpNode*>(tensor->op.as<ComputeOpNode>())) {
    for (const auto& di : compute_op->all_dimensions) {
      if (di->dim->isLoopDim()) dense_dims.push_back(di->dim);
    }
  } else if (auto scan_op = const_cast<ScanOpNode*>(tensor->op.as<ScanOpNode>())) {
    for (int i = 0; i < scan_op->num_outputs(); ++i) {
      const BaseVarDimOpNode* update_op = scan_op->update[i]->op.as<BaseVarDimOpNode>();
      CHECK(update_op) << "Don't support update ops that aren't computeops yet.";
      for (const auto& di : update_op->GetAllDimensions()) {
        if (di->dim->isLoopDim()) dense_dims.push_back(di->dim);
      }

      // const ComputeOpNode* update_op = scan_op->update[i]->op.as<ComputeOpNode>();
      // CHECK(update_op) << "Don't support update ops that aren't computeops yet.";
      // for (const auto& di : update_op->all_dimensions) {
      //   if (di->dim->isLoopDim()) dense_dims.push_back(di->dim);
      // }
    }

    // Also change dimensions for update and init ops
    for (const auto& update : scan_op->update) {
      this->index_by_dense_dimensions(update);
    }
    for (const auto& init : scan_op->init) {
      if (init->op.as<PlaceholderOpNode>()) continue;
      this->index_by_dense_dimensions(init);
    }
  } else if (auto conditional_op =
                 const_cast<ConditionalOpNode*>(tensor->op.as<ConditionalOpNode>())) {
    for (int i = 0; i < conditional_op->num_outputs(); ++i) {
      const BaseVarDimOpNode* update_op = conditional_op->then_case[i]->op.as<BaseVarDimOpNode>();
      CHECK(update_op) << "Don't support update ops that aren't computeops yet.";
      for (const auto& di : update_op->GetAllDimensions()) {
        if (di->dim->isLoopDim()) dense_dims.push_back(di->dim);
      }

      // const ComputeOpNode* update_op = conditional_op->update[i]->op.as<ComputeOpNode>();
      // CHECK(update_op) << "Don't support update ops that aren't computeops yet.";
      // for (const auto& di : update_op->all_dimensions) {
      //   if (di->dim->isLoopDim()) dense_dims.push_back(di->dim);
      // }
    }

    // Also change dimensions for update and init ops
    for (const auto& then_case : conditional_op->then_case) {
      this->index_by_dense_dimensions(then_case);
    }
    for (const auto& else_case : conditional_op->else_case) {
      if (else_case->op.as<PlaceholderOpNode>()) continue;
      this->index_by_dense_dimensions(else_case);
    }
  } else {
    CHECK(false) << "Layout changes allowed only for ComputeOp and ScanOp";
  }

  s->dim_relation_graph->relations.push_back(DimensionChangeNode::make(
      Array<Dimension>(s->dim_relation_graph->leaf_dimensions), dense_dims));

  auto leaf_dims = s->dim_relation_graph->leaf_dimensions.CopyOnWrite();
  leaf_dims->data.resize(0);

  for (auto dim : dense_dims) {
    leaf_dims->data.push_back(dim);
  }

  return tensor;
}
}  // namespace te
}  // namespace tvm

#undef COUT
