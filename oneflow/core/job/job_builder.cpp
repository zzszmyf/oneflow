#include "oneflow/core/job/job_builder.h"
#include "oneflow/core/common/util.h"
#include "oneflow/core/operator/operator.h"

namespace oneflow {

std::function<const ParallelConf*(const std::string&)> MakeGetterParallelConf4OpName(
    const Placement& placement) {
  auto op_name2parallel_conf = std::make_shared<HashMap<std::string, const ParallelConf*>>();
  for (const auto& placement_group : placement.placement_group()) {
    for (const std::string& op_name : placement_group.op_set().op_name()) {
      const ParallelConf* parallel_conf = &placement_group.parallel_conf();
      CHECK(op_name2parallel_conf->emplace(op_name, parallel_conf).second);
    }
  }
  return [op_name2parallel_conf](const std::string& op_name) {
    return op_name2parallel_conf->at(op_name);
  };
}

JobBuilder::JobBuilder(Job* job) : job_(job) {
  FOR_RANGE(int32_t, i, 0, job->net().op_size()) {
    CHECK(op_name2op_conf_.emplace(job->net().op(i).name(), job->mutable_net()->mutable_op(i))
              .second);
  }
  FOR_RANGE(int32_t, i, 0, job->placement().placement_group_size()) {
    auto* placemnt_group = job->mutable_placement()->mutable_placement_group(i);
    for (const auto& op_name : placemnt_group->op_set().op_name()) {
      CHECK(
          op_name2parallel_conf_.emplace(op_name, placemnt_group->mutable_parallel_conf()).second);
    }
  }
  auto* sbp_conf = job->mutable_sbp_conf();
  for (auto& pair : *(sbp_conf->mutable_op_name2sbp_signature_conf())) {
    op_name2sbp_signature_conf_.emplace(pair.first, &pair.second);
  }
  for (auto& pair : *(job->mutable_helper()->mutable_lbn2batch_axis())) {
    lbn2batch_axis_.emplace(pair.first, &pair.second);
  }
  auto* helper_conf = job->mutable_helper();
  for (auto& pair : *(helper_conf->mutable_op_name2op_time_shape())) {
    op_name2time_shapes_.emplace(pair.first, &pair.second);
  }
}

OperatorConf* JobBuilder::MutableOpConf4OpName(const std::string& op_name) {
  const auto& it = op_name2op_conf_.find(op_name);
  CHECK(it != op_name2op_conf_.end());
  return it->second;
}

const OperatorConf& JobBuilder::OpConf4OpName(const std::string& op_name) const {
  return *op_name2op_conf_.at(op_name);
}

void JobBuilder::AddOps(const ParallelConf& parallel_conf,
                        const std::vector<OperatorConf>& op_confs) {
  auto* placemnt_group = job_->mutable_placement()->add_placement_group();
  *placemnt_group->mutable_parallel_conf() = parallel_conf;
  for (const auto& op_conf : op_confs) {
    CHECK(op_name2op_conf_.find(op_conf.name()) == op_name2op_conf_.end());
    OperatorConf* mut_op_conf = job_->mutable_net()->add_op();
    *mut_op_conf = op_conf;
    CHECK(op_name2op_conf_.emplace(op_conf.name(), mut_op_conf).second);
    placemnt_group->mutable_op_set()->add_op_name(op_conf.name());
    CHECK(op_name2parallel_conf_.emplace(op_conf.name(), placemnt_group->mutable_parallel_conf())
              .second);
  }
}

PlacementGroup* JobBuilder::FindPlacementGroup(const std::string& op_name) const {
  FOR_RANGE(int32_t, i, 0, job_->mutable_placement()->placement_group_size()) {
    PlacementGroup* const cur_pg = job_->mutable_placement()->mutable_placement_group(i);
    const auto& op_names = cur_pg->op_set().op_name();
    if (std::find(op_names.begin(), op_names.end(), op_name) != op_names.end()) { return cur_pg; }
  }
  UNIMPLEMENTED();
  return nullptr;
}

void JobBuilder::MutParallelConfOnlyOnce(const std::string& op_name,
                                         const ParallelConf& parallel_conf) {
  CHECK(modified_parallel_conf_op_names_.emplace(op_name).second);
  PlacementGroup* placement_group = FindPlacementGroup(op_name);
  {
    auto* const op_names = placement_group->mutable_op_set()->mutable_op_name();
    Erase<PbRpf<std::string>>(*op_names, [&](const std::string& x) { return x == op_name; });
    Placement* const placement = job_->mutable_placement();
    if (op_names->size() > 0) { placement_group = placement->mutable_placement_group()->Add(); }
  }
  placement_group->mutable_op_set()->add_op_name(op_name);
  *placement_group->mutable_parallel_conf() = parallel_conf;
}

void JobBuilder::RemoveOpByName(const std::string& op_name) {
  RemoveOpByName(std::unordered_set<std::string>{op_name});
}

void JobBuilder::RemoveOpByName(const std::unordered_set<std::string>& removing_names) {
  // Update net
  DLNetConf net = job_->net();
  job_->mutable_net()->clear_op();
  for (const OperatorConf& op_conf : net.op()) {
    if (removing_names.count(op_conf.name()) == 0) { *(job_->mutable_net()->add_op()) = op_conf; }
  }
  // Update placement
  auto placement_group = job_->placement().placement_group();
  job_->mutable_placement()->clear_placement_group();
  for (const PlacementGroup& place : placement_group) {
    PlacementGroup p;
    OpNameSet* op_set = p.mutable_op_set();
    for (const std::string& name : place.op_set().op_name()) {
      if (removing_names.count(name) == 0) { op_set->add_op_name(name); }
    }

    *(p.mutable_parallel_conf()) = place.parallel_conf();
    if (op_set->op_name().size() > 0) { *(job_->mutable_placement()->add_placement_group()) = p; }
  }

  auto* sbp_conf = job_->mutable_sbp_conf()->mutable_op_name2sbp_signature_conf();
  auto* time_shape_conf = job_->mutable_helper()->mutable_op_name2op_time_shape();
  for (const std::string& op_name : removing_names) {
    // Update Sbp
    if (sbp_conf->count(op_name) > 0) { sbp_conf->erase(op_name); }
    // Update time shape
    if (time_shape_conf->count(op_name) > 0) { time_shape_conf->erase(op_name); }
  }
  // Update batch dim lbis
  // Update builder
  JobBuilder builder(job_);
  op_name2op_conf_.swap(builder.op_name2op_conf_);
  op_name2parallel_conf_.swap(builder.op_name2parallel_conf_);
  op_name2sbp_signature_conf_.swap(builder.op_name2sbp_signature_conf_);
  lbn2batch_axis_.swap(builder.lbn2batch_axis_);
}

void JobBuilder::DelOps(const std::vector<std::string>& op_names) {
  std::unordered_set<std::string> removing_names;
  for (const auto& op_name : op_names) { removing_names.insert(op_name); }
  RemoveOpByName(removing_names);
}

void JobBuilder::DelOps(const std::vector<OperatorConf>& op_confs) {
  std::unordered_set<std::string> removing_names;
  for (const auto& op_conf : op_confs) { removing_names.insert(op_conf.name()); }
  RemoveOpByName(removing_names);
}

void JobBuilder::MutOpsOnlyOnce(const std::vector<OperatorConf>& op_confs) {
  for (const auto& op_conf : op_confs) {
    CHECK(modified_op_conf_op_names_.emplace(op_conf.name()).second);
    op_name2op_conf_.at(op_conf.name())->CopyFrom(op_conf);
  }
}

void JobBuilder::AddOrMutOpsOnlyOnce(const ParallelConf& parallel_conf,
                                     const std::vector<OperatorConf>& op_confs) {
  std::vector<OperatorConf> add_ops;
  std::vector<OperatorConf> mut_ops;
  for (const auto& op_conf : op_confs) {
    if (op_name2op_conf_.find(op_conf.name()) == op_name2op_conf_.end()) {
      add_ops.push_back(op_conf);
    } else {
      mut_ops.push_back(op_conf);
    }
  }
  AddOps(parallel_conf, add_ops);
  MutOpsOnlyOnce(mut_ops);
}

void JobBuilder::ForEachOperator(const std::function<void(const Operator&)>& Handler) const {
  for (const auto& pair : op_name2op_conf_) {
    DeviceType device_type = ParallelDesc(*op_name2parallel_conf_.at(pair.first)).device_type();
    std::shared_ptr<Operator> op = ConstructOp(*pair.second, device_type, &GlobalJobDesc());
    Handler(*op);
  }
}

const ParallelConf& JobBuilder::ParallelConf4OpName(const std::string& op_name) const {
  return *op_name2parallel_conf_.at(op_name);
}

void JobBuilder::AddParallelConf4OpName(const std::string& op_name,
                                        const ParallelConf& parallel_conf) {
  bool update = (op_name2parallel_conf_.count(op_name) == 0);
  if (update) {
    // update `op_name2parallel_conf_`
    PlacementGroup* group = job_->mutable_placement()->add_placement_group();
    group->mutable_op_set()->add_op_name(op_name);
    *(group->mutable_parallel_conf()) = parallel_conf;
    op_name2parallel_conf_[op_name] = group->mutable_parallel_conf();
  }
}

SbpParallel* JobBuilder::MutSbpParallel4Oba(const OpBlobArg& oba) const {
  auto* sbp_sig = &(*job_->mutable_sbp_conf()->mutable_op_name2sbp_signature_conf())[oba.op_name()];
  return &(*sbp_sig->mutable_bn_in_op2sbp_parallel())[oba.bn_in_op()];
}

void JobBuilder::BindIdenticalSbpOpBlobArgPair(const OpBlobArg& first, const OpBlobArg& second) {
  auto* pair = job_->mutable_helper()->mutable_identical_sbp_oba_pairs()->mutable_pair()->Add();
  *pair->mutable_first() = first;
  *pair->mutable_second() = second;
}

const SbpSignature& JobBuilder::SbpSignature4OpName(const std::string& op_name) const {
  const auto& it = op_name2sbp_signature_conf_.find(op_name);
  CHECK(it != op_name2sbp_signature_conf_.end());
  return *(it->second);
}

void JobBuilder::AddSbpSignature4OpName(const std::string& op_name,
                                        const SbpSignature& sbp_signature) {
  const auto& it = op_name2sbp_signature_conf_.find(op_name);
  if (it != op_name2sbp_signature_conf_.end()) {
    *(it->second) = sbp_signature;
    return;
  }

  auto* op_name2sbp_signature_conf = job_->mutable_sbp_conf()->mutable_op_name2sbp_signature_conf();
  (*op_name2sbp_signature_conf)[op_name] = sbp_signature;
  op_name2sbp_signature_conf_.emplace(op_name, &(*op_name2sbp_signature_conf)[op_name]);
}

const OpTimeShape& JobBuilder::TimeShape4OpName(const std::string& op_name) const {
  const auto& it = op_name2time_shapes_.find(op_name);
  CHECK(it != op_name2time_shapes_.end());
  return *(it->second);
}

void JobBuilder::AddTimeShape4OpName(const std::string& op_name, const OpTimeShape& time_shape) {
  bool update = (op_name2time_shapes_.count(op_name) == 0);
  if (update) {
    auto* time_shape_conf = job_->mutable_helper()->mutable_op_name2op_time_shape();
    (*time_shape_conf)[op_name] = time_shape;
    op_name2time_shapes_[op_name] = &((*time_shape_conf)[op_name]);
  }
}

const OptInt64& JobBuilder::BatchAxis4Lbn(const std::string& lbn) const {
  const auto& it = lbn2batch_axis_.find(lbn);
  CHECK(it != lbn2batch_axis_.end());
  return *(it->second);
}

void JobBuilder::AddBatchAxis4Lbn(const std::string& lbn, const OptInt64& axis) {
  bool update =
      (lbn2batch_axis_.count(lbn) == 0) || (lbn2batch_axis_[lbn]->value() != axis.value());
  if (update) {
    auto* batch_axis = job_->mutable_helper()->mutable_lbn2batch_axis();
    (*batch_axis)[lbn] = axis;
    lbn2batch_axis_[lbn] = &((*batch_axis)[lbn]);
  }
}

}  // namespace oneflow
