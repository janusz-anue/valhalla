#include "meili/transition_cost_model.h"
#include "meili/routing.h"

namespace {
inline float
GreatCircleDistance(const valhalla::meili::Measurement& left,
                    const valhalla::meili::Measurement& right)
{ return left.lnglat().Distance(right.lnglat()); }

inline float ClockDistance(const valhalla::meili::Measurement& left,
                           const valhalla::meili::Measurement& right)
{ return right.epoch_time() - left.epoch_time(); }
}

namespace valhalla {
namespace meili {

TransitionCostModel::TransitionCostModel(
    baldr::GraphReader& graphreader,
    const IViterbiSearch& vs,
    const ColumnGetter& get_column,
    const MeasurementGetter& get_measurement,
    const sif::cost_ptr_t* mode_costing,
    const sif::TravelMode mode,
    float beta,
    float breakage_distance,
    float max_route_distance_factor,
    float max_route_time_factor,
    float turn_penalty_factor)
    : graphreader_(graphreader),
      vs_(vs),
      get_column_(get_column),
      get_measurement_(get_measurement),
      mode_costing_(mode_costing),
      mode_(mode),
      beta_(beta),
      inv_beta_(1.f / beta_),
      breakage_distance_(breakage_distance),
      max_route_distance_factor_(max_route_distance_factor),
      max_route_time_factor_(max_route_time_factor),
      turn_penalty_factor_(turn_penalty_factor),
      turn_cost_table_{0.f}
{
  if (beta_ <= 0.f) {
    throw std::invalid_argument("Expect beta to be positive");
  }

  if (turn_penalty_factor_ < 0.f) {
    throw std::invalid_argument("Expect turn penalty factor to be nonnegative");
  }

  if (0.f < turn_penalty_factor_) {
    for (int i = 0; i <= 180; ++i) {
      turn_cost_table_[i] = turn_penalty_factor_ * std::exp(-i/45.f);
    }
  }
}

TransitionCostModel::TransitionCostModel(
    baldr::GraphReader& graphreader,
    const IViterbiSearch& vs,
    const ColumnGetter& get_column,
    const MeasurementGetter& get_measurement,
    const sif::cost_ptr_t* mode_costing,
    const sif::TravelMode mode,
    const boost::property_tree::ptree& config)
    : TransitionCostModel(
          graphreader,
          vs,
          get_column,
          get_measurement,
          mode_costing,
          mode,
          config.get<float>("beta"),
          config.get<float>("breakage_distance"),
          config.get<float>("max_route_distance_factor"),
          config.get<float>("max_route_time_factor"),
          config.get<float>("turn_penalty_factor")) {}

float
TransitionCostModel::operator()(const StateId& lhs, const StateId& rhs) const
{
  const auto& left = get_column_(lhs.time())[lhs.id()];
  const auto& right = get_column_(rhs.time())[lhs.id()];

  if (!left.routed()) {
    UpdateRoute(lhs, rhs);
  }

  // Compute the transition cost if we found a path
  const auto label = left.last_label(right);
  if (label) {
    // Get some basic info about difference between the two measurements
    const auto& left_measurement = get_measurement_(lhs.time());
    const auto& right_measurement = get_measurement_(rhs.time());
    return CalculateTransitionCost(
        label->turn_cost(),
        label->cost().cost,
        GreatCircleDistance(left_measurement, right_measurement),
        label->cost().secs,
        ClockDistance(left_measurement, right_measurement));
  }

  // No path found
  return -1.f;
}

void
TransitionCostModel::UpdateRoute(const StateId& lhs, const StateId& rhs) const
{
  const auto& left = get_column_(lhs.time())[lhs.id()];
  const auto& right = get_column_(rhs.time())[lhs.id()];

  // Prepare edgelabel
  const Label* edgelabel = nullptr;
  const auto& prev_stateid = vs_.Predecessor(left.stateid());
  if (prev_stateid.IsValid()) {
    const auto& prev_state = get_column_(prev_stateid.time())[prev_stateid.id()];
    if (!prev_state.routed()) {
      // When ViterbiSearch calls this method, the left state is
      // guaranteed to be optimal, its predecessor is therefore
      // guaranteed to be expanded (and routed). When
      // NaiveViterbiSearch calls this method, the previous column,
      // where the predecessor of the left state stays, are
      // guaranteed to be all expanded (and routed).
      throw std::logic_error(
          "The predecessor of current state must have been routed."
          " Check if you have misused the TransitionCost method");
    }
    edgelabel = prev_state.last_label(left);
  }

  // Prepare locations and stateids
  const auto& right_column = get_column_(right.stateid().time());
  std::vector<baldr::PathLocation> locations;
  locations.reserve(1 + right_column.size());
  locations.push_back(left.candidate());
  std::vector<StateId> unreached_stateids;
  unreached_stateids.reserve(right_column.size());
  for (const auto& state : right_column) {
    if (!vs_.Predecessor(state.stateid()).IsValid()) {
      locations.push_back(state.candidate());
      unreached_stateids.push_back(state.stateid());
    }
  }

  const auto& left_measurement = get_measurement_(lhs.time());
  const auto& right_measurement = get_measurement_(rhs.time());

  const auto gc_dist = GreatCircleDistance(left_measurement, right_measurement);
  const auto max_route_distance = std::min(
      gc_dist * max_route_distance_factor_,
      breakage_distance_);

  const auto clk_dist = ClockDistance(left_measurement, right_measurement);
  const auto max_route_time = clk_dist * max_route_time_factor_;

  const midgard::DistanceApproximator approximator(right_measurement.lnglat());

  // Route, we have to make sure that the max distance is greater
  // than 0 otherwise we wont be able to get any labels into the
  // labelset
  labelset_ptr_t labelset = std::make_shared<LabelSet>(std::max(std::ceil(max_route_distance), 1.f));

  const auto& results = find_shortest_path(
      graphreader_,
      locations,
      0,
      labelset,
      approximator,
      right_measurement.search_radius(),
      mode_costing_[static_cast<size_t>(mode_)],
      edgelabel,
      turn_cost_table_,
      std::ceil(max_route_distance),
      std::ceil(max_route_time));

  left.SetRoute(unreached_stateids, results, labelset);
}

}
}
