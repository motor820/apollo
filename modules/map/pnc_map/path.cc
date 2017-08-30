/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/map/pnc_map/path.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <unordered_map>

#include "modules/common/math/line_segment2d.h"
#include "modules/common/math/math_utils.h"
#include "modules/common/math/polygon2d.h"
#include "modules/common/math/vec2d.h"
#include "modules/common/util/string_util.h"

namespace apollo {
namespace hdmap {

using common::math::LineSegment2d;
using common::math::Polygon2d;
using common::math::Vec2d;
using common::math::Box2d;
using common::math::kMathEpsilon;
using common::math::Sqr;
using std::placeholders::_1;

namespace {

const double kSampleDistance = 0.25;

bool find_lane_segment(const MapPathPoint& p1, const MapPathPoint& p2,
                       LaneSegment* const lane_segment) {
  for (const auto& wp1 : p1.lane_waypoints()) {
    for (const auto& wp2 : p2.lane_waypoints()) {
      if (wp1.lane->id().id() == wp2.lane->id().id() && wp1.s < wp2.s) {
        *lane_segment = LaneSegment(wp1.lane, wp1.s, wp2.s);
        return true;
      }
    }
  }
  return false;
}

}  // namespace

std::string LaneWaypoint::DebugString() const {
  if (lane == nullptr) {
    return "(lane is null)";
  }
  return common::util::StrCat("id = ", lane->id().id(), "  s = ", s);
}

std::string LaneSegment::DebugString() const {
  if (lane == nullptr) {
    return "(lane is null)";
  }
  return common::util::StrCat("id = ", lane->id().id(),
                              "  "
                              "start_s = ",
                              start_s,
                              "  "
                              "end_s = ",
                              end_s);
}

std::string MapPathPoint::DebugString() const {
  return common::util::StrCat(
      "x = ", x_, "  y = ", y_, "  heading = ", _heading,
      "  lwp = "
      "{(",
      common::util::PrintDebugStringIter(_lane_waypoints, "), ("), ")}");
}

std::string Path::DebugString() const {
  return common::util::StrCat(
      "num_points = ", num_points_,
      "  points = "
      "{(",
      common::util::PrintDebugStringIter(path_points_, "), ("),
      ")}  "
      "numlane_segments_ = ",
      lane_segments_.size(),
      "  lane_segments = "
      "{(",
      common::util::PrintDebugStringIter(lane_segments_, "), ("), ")}");
}

std::string PathOverlap::DebugString() const {
  return common::util::StrCat(object_id, " ", start_s, " ", end_s);
}

Path::Path(std::vector<MapPathPoint> path_points)
    : path_points_(std::move(path_points)) {
  Init();
}

Path::Path(std::vector<MapPathPoint> path_points,
           std::vector<LaneSegment> lane_segments)
    : path_points_(std::move(path_points)),
      lane_segments_(std::move(lane_segments)) {
  Init();
}

Path::Path(std::vector<MapPathPoint> path_points,
           std::vector<LaneSegment> lane_segments,
           const double max_approximation_error)
    : path_points_(std::move(path_points)),
      lane_segments_(std::move(lane_segments)) {
  Init();
  if (max_approximation_error > 0.0) {
    _use_path_approximation = true;
    _approximation = PathApproximation(*this, max_approximation_error);
  }
}

void Path::Init() {
  InitPoints();
  InitLaneSegments();
  InitPointIndex();
  InitWidth();
  //  InitOverlaps();
}

void Path::InitPoints() {
  num_points_ = static_cast<int>(path_points_.size());
  CHECK_GE(num_points_, 2);

  accumulated_s_.clear();
  accumulated_s_.reserve(num_points_);
  segments_.clear();
  segments_.reserve(num_points_);
  unit_directions_.clear();
  unit_directions_.reserve(num_points_);
  double s = 0.0;
  for (int i = 0; i < num_points_; ++i) {
    accumulated_s_.push_back(s);
    Vec2d heading;
    if (i + 1 >= num_points_) {
      heading = path_points_[i] - path_points_[i - 1];
    } else {
      segments_.emplace_back(path_points_[i], path_points_[i + 1]);
      heading = path_points_[i + 1] - path_points_[i];
      // TODO(lianglia_apollo):
      // use heading.length when all adjacent lanes are guarantee to be
      // connected.
      s += heading.Length();
    }
    heading.Normalize();
    unit_directions_.push_back(heading);
  }
  length_ = s;
  _num_sample_points = static_cast<int>(length_ / kSampleDistance) + 1;
  num_segments_ = num_points_ - 1;

  CHECK_EQ(accumulated_s_.size(), num_points_);
  CHECK_EQ(unit_directions_.size(), num_points_);
  CHECK_EQ(segments_.size(), num_segments_);
}

void Path::InitLaneSegments() {
  if (lane_segments_.empty()) {
    lane_segments_.reserve(num_points_);
    for (int i = 0; i + 1 < num_points_; ++i) {
      LaneSegment lane_segment;
      if (find_lane_segment(path_points_[i], path_points_[i + 1],
                            &lane_segment)) {
        lane_segments_.push_back(lane_segment);
      }
    }
  }

  lane_segments_to_next_point_.clear();
  lane_segments_to_next_point_.reserve(num_points_);
  for (int i = 0; i + 1 < num_points_; ++i) {
    LaneSegment lane_segment;
    if (find_lane_segment(path_points_[i], path_points_[i + 1],
                          &lane_segment)) {
      lane_segments_to_next_point_.push_back(lane_segment);
    } else {
      lane_segments_to_next_point_.push_back(LaneSegment());
    }
  }
  CHECK_EQ(lane_segments_to_next_point_.size(), num_segments_);
}

void Path::InitWidth() {
  _left_width.clear();
  _left_width.reserve(_num_sample_points);
  _right_width.clear();
  _right_width.reserve(_num_sample_points);

  double s = 0;
  for (int i = 0; i < _num_sample_points; ++i) {
    const MapPathPoint point = GetSmoothPoint(s);
    if (point.lane_waypoints().empty()) {
      _left_width.push_back(0.0);
      _right_width.push_back(0.0);
      AERROR << "path point:" << point.DebugString() << " has invalid width.";
    } else {
      const LaneWaypoint waypoint = point.lane_waypoints()[0];
      CHECK_NOTNULL(waypoint.lane);
      double left_width = 0.0;
      double right_width = 0.0;
      waypoint.lane->GetWidth(waypoint.s, &left_width, &right_width);
      _left_width.push_back(left_width);
      _right_width.push_back(right_width);
    }
    s += kSampleDistance;
  }
  CHECK_EQ(_left_width.size(), _num_sample_points);
  CHECK_EQ(_right_width.size(), _num_sample_points);
}

void Path::InitPointIndex() {
  _last_point_index.clear();
  _last_point_index.reserve(_num_sample_points);
  double s = 0.0;
  int last_index = 0;
  for (int i = 0; i < _num_sample_points; ++i) {
    while (last_index + 1 < num_points_ &&
           accumulated_s_[last_index + 1] <= s) {
      ++last_index;
    }
    _last_point_index.push_back(last_index);
    s += kSampleDistance;
  }
  CHECK_EQ(_last_point_index.size(), _num_sample_points);
}

void Path::GetAllOverlaps(GetOverlapFromLaneFunc get_overlaps_from_lane,
                          std::vector<PathOverlap>* const overlaps) const {
  if (overlaps == nullptr) {
    return;
  }
  overlaps->clear();
  std::unordered_map<std::string, std::vector<std::pair<double, double>>>
      overlaps_by_id;
  double s = 0.0;
  for (const auto& lane_segment : lane_segments_) {
    if (lane_segment.lane == nullptr) {
      continue;
    }
    for (const auto& overlap : get_overlaps_from_lane(*(lane_segment.lane))) {
      const auto& overlap_info =
          overlap->get_object_overlap_info(lane_segment.lane->id());
      if (overlap_info == nullptr) {
        continue;
      }

      const auto& lane_overlap_info = overlap_info->lane_overlap_info();
      if (lane_overlap_info.start_s() < lane_segment.end_s &&
          lane_overlap_info.end_s() > lane_segment.start_s) {
        const double ref_s = s - lane_segment.start_s;
        const double adjusted_start_s =
            std::max(lane_overlap_info.start_s(), lane_segment.start_s) + ref_s;
        const double adjusted_end_s =
            std::min(lane_overlap_info.end_s(), lane_segment.end_s) + ref_s;
        for (const auto& object : overlap->overlap().object()) {
          if (object.id().id() != lane_segment.lane->id().id()) {
            overlaps_by_id[object.id().id()].emplace_back(adjusted_start_s,
                                                          adjusted_end_s);
          }
        }
      }
    }
    s += lane_segment.end_s - lane_segment.start_s;
  }
  for (auto& overlaps_one_object : overlaps_by_id) {
    const std::string& object_id = overlaps_one_object.first;
    auto& segments = overlaps_one_object.second;
    std::sort(segments.begin(), segments.end());

    const double kMinOverlapDistanceGap = 1.5;  // in meters.
    for (const auto& segment : segments) {
      if (!overlaps->empty() && overlaps->back().object_id == object_id &&
          segment.first - overlaps->back().end_s <= kMinOverlapDistanceGap) {
        overlaps->back().end_s =
            std::max(overlaps->back().end_s, segment.second);
      } else {
        overlaps->emplace_back(object_id, segment.first, segment.second);
      }
    }
  }
  std::sort(overlaps->begin(), overlaps->end(),
            [](const PathOverlap& overlap1, const PathOverlap& overlap2) {
              return overlap1.start_s < overlap2.start_s;
            });
}

void Path::InitOverlaps() {
  GetAllOverlaps(std::bind(&LaneInfo::cross_lanes, _1), &_lane_overlaps);
  GetAllOverlaps(std::bind(&LaneInfo::signals, _1), &_signal_overlaps);
  GetAllOverlaps(std::bind(&LaneInfo::yield_signs, _1), &_yield_sign_overlaps);
  GetAllOverlaps(std::bind(&LaneInfo::stop_signs, _1), &_stop_sign_overlaps);
  GetAllOverlaps(std::bind(&LaneInfo::crosswalks, _1), &_crosswalk_overlaps);
  GetAllOverlaps(std::bind(&LaneInfo::junctions, _1), &_junction_overlaps);

  // TODO(all): add support for parking and speed bumps.
  /*
  GetAllOverlaps(std::bind(&LaneInfo::parking_spaces, _1),
                   &_parking_space_overlaps);
  GetAllOverlaps(std::bind(&LaneInfo::speed_bumps, _1),
                   &_speed_bump_overlaps);
  */
}

MapPathPoint Path::GetSmoothPoint(const InterpolatedIndex& index) const {
  CHECK_GE(index.id, 0);
  CHECK_LT(index.id, num_points_);

  const MapPathPoint& ref_point = path_points_[index.id];
  if (std::abs(index.offset) > kMathEpsilon) {
    const Vec2d delta = unit_directions_[index.id] * index.offset;
    MapPathPoint point({ref_point.x() + delta.x(), ref_point.y() + delta.y()},
                       ref_point.heading());
    if (index.id < num_segments_) {
      const LaneSegment& lane_segment = lane_segments_to_next_point_[index.id];
      if (lane_segment.lane != nullptr) {
        point.add_lane_waypoint(LaneWaypoint(
            lane_segment.lane, lane_segment.start_s + index.offset));
      }
    }
    if (point.lane_waypoints().empty() && !ref_point.lane_waypoints().empty()) {
      point.add_lane_waypoint(ref_point.lane_waypoints()[0]);
    }
    return point;
  } else {
    return ref_point;
  }
}

MapPathPoint Path::GetSmoothPoint(double s) const {
  return GetSmoothPoint(GetIndexFromS(s));
}

double Path::GetSFromIndex(const InterpolatedIndex& index) const {
  if (index.id < 0) {
    return 0.0;
  }
  if (index.id >= num_points_) {
    return length_;
  }
  return accumulated_s_[index.id] + index.offset;
}

InterpolatedIndex Path::GetIndexFromS(double s) const {
  if (s <= 0.0) {
    return {0, 0.0};
  }
  CHECK_GT(num_points_, 0);
  if (s >= length_) {
    return {num_points_ - 1, 0.0};
  }
  const int sample_id = static_cast<int>(s / kSampleDistance);
  if (sample_id >= _num_sample_points) {
    return {num_points_ - 1, 0.0};
  }
  const int next_sample_id = sample_id + 1;
  int low = _last_point_index[sample_id];
  int high = (next_sample_id < _num_sample_points
                  ? std::min(num_points_, _last_point_index[next_sample_id] + 1)
                  : num_points_);
  while (low + 1 < high) {
    const int mid = (low + high) / 2;
    if (accumulated_s_[mid] <= s) {
      low = mid;
    } else {
      high = mid;
    }
  }
  return {low, s - accumulated_s_[low]};
}

bool Path::GetNearestPoint(const Vec2d& point, double* accumulate_s,
                             double* lateral) const {
  double distance = 0.0;
  return GetNearestPoint(point, accumulate_s, lateral, &distance);
}

bool Path::GetNearestPoint(const Vec2d& point, double* accumulate_s,
                             double* lateral, double* min_distance) const {
  if (!GetProjection(point, accumulate_s, lateral, min_distance)) {
    return false;
  }
  if (*accumulate_s < 0.0) {
    *accumulate_s = 0.0;
    *min_distance = point.DistanceTo(path_points_[0]);
  } else if (*accumulate_s > length_) {
    *accumulate_s = length_;
    *min_distance = point.DistanceTo(path_points_.back());
  }
  return true;
}

bool Path::GetProjection(const common::math::Vec2d& point,
                          double* accumulate_s, double* lateral) const {
  double distance = 0.0;
  return GetProjection(point, accumulate_s, lateral, &distance);
}

bool Path::GetProjection(const Vec2d& point, double* accumulate_s,
                          double* lateral, double* min_distance) const {
  if (segments_.empty()) {
    return false;
  }
  if (accumulate_s == nullptr || lateral == nullptr ||
      min_distance == nullptr) {
    return false;
  }
  if (_use_path_approximation) {
    return _approximation.GetProjection(*this, point, accumulate_s, lateral,
                                         min_distance);
  }
  CHECK_GE(num_points_, 2);
  *min_distance = std::numeric_limits<double>::infinity();

  for (int i = 0; i < num_segments_; ++i) {
    const auto& segment = segments_[i];
    const double distance = segment.DistanceTo(point);
    if (distance < *min_distance) {
      const double proj = segment.ProjectOntoUnit(point);
      if (proj < 0.0 && i > 0) {
        continue;
      }
      if (proj > segment.length() && i + 1 < num_segments_) {
        const auto& next_segment = segments_[i + 1];
        if ((point - next_segment.start())
                .InnerProd(next_segment.unit_direction()) >= 0.0) {
          continue;
        }
      }
      *min_distance = distance;
      if (i + 1 >= num_segments_) {
        *accumulate_s = accumulated_s_[i] + proj;
      } else {
        *accumulate_s = accumulated_s_[i] + std::min(proj, segment.length());
      }
      const double prod = segment.ProductOntoUnit(point);
      if ((i == 0 && proj < 0.0) ||
          (i + 1 == num_segments_ && proj > segment.length())) {
        *lateral = prod;
      } else {
        *lateral = (prod > 0.0 ? distance : -distance);
      }
    }
  }
  return true;
}

bool Path::GetHeadingAlongPath(const Vec2d& point, double* heading) const {
  if (heading == nullptr) {
    return false;
  }
  double s = 0;
  double l = 0;
  if (GetProjection(point, &s, &l)) {
    *heading = GetSmoothPoint(s).heading();
    return true;
  }
  return false;
}

double Path::get_left_width(const double s) const {
  return GetSample(_left_width, s);
}

double Path::get_right_width(const double s) const {
  return GetSample(_right_width, s);
}

bool Path::get_width(const double s, double* left_width,
                     double* right_width) const {
  CHECK_NOTNULL(left_width);
  CHECK_NOTNULL(right_width);

  if (s < 0.0 || s > length_) {
    return false;
  }
  *left_width = GetSample(_left_width, s);
  *right_width = GetSample(_right_width, s);
  return true;
}

double Path::GetSample(const std::vector<double>& samples,
                       const double s) const {
  if (samples.empty()) {
    return 0.0;
  }
  if (s <= 0.0) {
    return samples[0];
  }
  const int idx = static_cast<int>(s / kSampleDistance);
  if (idx >= _num_sample_points - 1) {
    return samples.back();
  }
  const double ratio = (s - idx * kSampleDistance) / kSampleDistance;
  return samples[idx] * (1.0 - ratio) + samples[idx + 1] * ratio;
}

bool Path::is_on_path(const Vec2d& point) const {
  double accumulate_s = 0.0;
  double lateral = 0.0;
  if (!GetProjection(point, &accumulate_s, &lateral)) {
    return false;
  }
  double left_width = 0.0;
  double right_width = 0.0;
  if (!get_width(accumulate_s, &left_width, &right_width)) {
    return false;
  }
  if (lateral < left_width && lateral > -right_width) {
    return true;
  }
  return false;
}

bool Path::overlap_with(const common::math::Box2d& box, double width) const {
  if (_use_path_approximation) {
    return _approximation.overlap_with(*this, box, width);
  }
  const Vec2d center = box.center();
  const double radius_sqr = Sqr(box.diagonal() / 2.0 + width) + kMathEpsilon;
  for (const auto& segment : segments_) {
    if (segment.DistanceSquareTo(center) > radius_sqr) {
      continue;
    }
    if (box.DistanceTo(segment) <= width + kMathEpsilon) {
      return true;
    }
  }
  return false;
}

double PathApproximation::compute_max_error(const Path& path, const int s,
                                            const int t) {
  if (s + 1 >= t) {
    return 0.0;
  }
  const auto& points = path.path_points();
  const LineSegment2d segment(points[s], points[t]);
  double max_distance_sqr = 0.0;
  for (int i = s + 1; i < t; ++i) {
    max_distance_sqr =
        std::max(max_distance_sqr, segment.DistanceSquareTo(points[i]));
  }
  return sqrt(max_distance_sqr);
}

bool PathApproximation::is_within_max_error(const Path& path, const int s,
                                            const int t) {
  if (s + 1 >= t) {
    return true;
  }
  const auto& points = path.path_points();
  const LineSegment2d segment(points[s], points[t]);
  for (int i = s + 1; i < t; ++i) {
    if (segment.DistanceSquareTo(points[i]) > _max_sqr_error) {
      return false;
    }
  }
  return true;
}

void PathApproximation::Init(const Path& path) {
  init_dilute(path);
  init_projections(path);
}

void PathApproximation::init_dilute(const Path& path) {
  const int num_original_points = path.num_points();
  _original_ids.clear();
  int last_idx = 0;
  while (last_idx < num_original_points - 1) {
    _original_ids.push_back(last_idx);
    int next_idx = last_idx + 1;
    int delta = 2;
    for (; last_idx + delta < num_original_points; delta *= 2) {
      if (!is_within_max_error(path, last_idx, last_idx + delta)) {
        break;
      }
      next_idx = last_idx + delta;
    }
    for (; delta > 0; delta /= 2) {
      if (next_idx + delta < num_original_points &&
          is_within_max_error(path, last_idx, next_idx + delta)) {
        next_idx += delta;
      }
    }
    last_idx = next_idx;
  }
  _original_ids.push_back(last_idx);
  num_points_ = static_cast<int>(_original_ids.size());
  if (num_points_ == 0) {
    return;
  }

  segments_.clear();
  segments_.reserve(num_points_ - 1);
  for (int i = 0; i < num_points_ - 1; ++i) {
    segments_.emplace_back(path.path_points()[_original_ids[i]],
                           path.path_points()[_original_ids[i + 1]]);
  }
  _max_error_per_segment.clear();
  _max_error_per_segment.reserve(num_points_ - 1);
  for (int i = 0; i < num_points_ - 1; ++i) {
    _max_error_per_segment.push_back(
        compute_max_error(path, _original_ids[i], _original_ids[i + 1]));
  }
}

void PathApproximation::init_projections(const Path& path) {
  if (num_points_ == 0) {
    return;
  }
  _projections.clear();
  _projections.reserve(segments_.size() + 1);
  double s = 0.0;
  _projections.push_back(0);
  for (const auto& segment : segments_) {
    s += segment.length();
    _projections.push_back(s);
  }
  const auto& original_points = path.path_points();
  const int num_original_points = original_points.size();
  _original_projections.clear();
  _original_projections.reserve(num_original_points);
  for (size_t i = 0; i < _projections.size(); ++i) {
    _original_projections.push_back(_projections[i]);
    if (i + 1 < _projections.size()) {
      const auto& segment = segments_[i];
      for (int idx = _original_ids[i] + 1; idx < _original_ids[i + 1]; ++idx) {
        const double proj = segment.ProjectOntoUnit(original_points[idx]);
        _original_projections.push_back(
            _projections[i] + std::max(0.0, std::min(proj, segment.length())));
      }
    }
  }

  // max_p_to_left[i] = max(p[0], p[1], ... p[i]).
  _max_original_projections_to_left.resize(num_original_points);
  double last_projection = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < num_original_points; ++i) {
    last_projection = std::max(last_projection, _original_projections[i]);
    _max_original_projections_to_left[i] = last_projection;
  }
  for (int i = 0; i + 1 < num_original_points; ++i) {
    CHECK_LE(_max_original_projections_to_left[i],
             _max_original_projections_to_left[i + 1] + kMathEpsilon);
  }

  // min_p_to_right[i] = min(p[i], p[i + 1], ... p[size - 1]).
  _min_original_projections_to_right.resize(_original_projections.size());
  last_projection = std::numeric_limits<double>::infinity();
  for (int i = num_original_points - 1; i >= 0; --i) {
    last_projection = std::min(last_projection, _original_projections[i]);
    _min_original_projections_to_right[i] = last_projection;
  }
  for (int i = 0; i + 1 < num_original_points; ++i) {
    CHECK_LE(_min_original_projections_to_right[i],
             _min_original_projections_to_right[i + 1] + kMathEpsilon);
  }

  // Sample max_p_to_left by sample_distance.
  _max_projection = _projections.back();
  _num_projection_samples =
      static_cast<int>(_max_projection / kSampleDistance) + 1;
  _sampled_max_original_projections_to_left.clear();
  _sampled_max_original_projections_to_left.reserve(_num_projection_samples);
  double proj = 0.0;
  int last_index = 0;
  for (int i = 0; i < _num_projection_samples; ++i) {
    while (last_index + 1 < num_original_points &&
           _max_original_projections_to_left[last_index + 1] < proj) {
      ++last_index;
    }
    _sampled_max_original_projections_to_left.push_back(last_index);
    proj += kSampleDistance;
  }
  CHECK_EQ(_sampled_max_original_projections_to_left.size(),
           _num_projection_samples);
}

bool PathApproximation::GetProjection(const Path& path,
                                       const common::math::Vec2d& point,
                                       double* accumulate_s, double* lateral,
                                       double* min_distance) const {
  if (num_points_ == 0) {
    return false;
  }
  if (accumulate_s == nullptr || lateral == nullptr ||
      min_distance == nullptr) {
    return false;
  }
  double min_distance_sqr = std::numeric_limits<double>::infinity();
  int estimate_nearest_segment_idx = -1;
  std::vector<double> distance_sqr_to_segments;
  distance_sqr_to_segments.reserve(segments_.size());
  for (size_t i = 0; i < segments_.size(); ++i) {
    const double distance_sqr = segments_[i].DistanceSquareTo(point);
    distance_sqr_to_segments.push_back(distance_sqr);
    if (distance_sqr < min_distance_sqr) {
      min_distance_sqr = distance_sqr;
      estimate_nearest_segment_idx = i;
    }
  }
  if (estimate_nearest_segment_idx < 0) {
    return false;
  }
  const auto& original_segments = path.segments();
  const int num_original_segments = static_cast<int>(original_segments.size());
  const auto& original_accumulated_s = path.accumulated_s();
  double min_distance_sqr_with_error =
      Sqr(sqrt(min_distance_sqr) +
          _max_error_per_segment[estimate_nearest_segment_idx] + _max_error);
  *min_distance = std::numeric_limits<double>::infinity();
  int nearest_segment_idx = -1;
  for (size_t i = 0; i < segments_.size(); ++i) {
    if (distance_sqr_to_segments[i] >= min_distance_sqr_with_error) {
      continue;
    }
    int first_segment_idx = _original_ids[i];
    int last_segment_idx = _original_ids[i + 1] - 1;
    double max_original_projection = std::numeric_limits<double>::infinity();
    if (first_segment_idx < last_segment_idx) {
      const auto& segment = segments_[i];
      const double projection = segment.ProjectOntoUnit(point);
      const double prod_sqr = Sqr(segment.ProductOntoUnit(point));
      if (prod_sqr >= min_distance_sqr_with_error) {
        continue;
      }
      const double scan_distance = sqrt(min_distance_sqr_with_error - prod_sqr);
      const double min_projection = projection - scan_distance;
      max_original_projection = _projections[i] + projection + scan_distance;
      if (min_projection > 0.0) {
        const double limit = _projections[i] + min_projection;
        const int sample_index =
            std::max(0, static_cast<int>(limit / kSampleDistance));
        if (sample_index >= _num_projection_samples) {
          first_segment_idx = last_segment_idx;
        } else {
          first_segment_idx =
              std::max(first_segment_idx,
                       _sampled_max_original_projections_to_left[sample_index]);
          if (first_segment_idx >= last_segment_idx) {
            first_segment_idx = last_segment_idx;
          } else {
            while (first_segment_idx < last_segment_idx &&
                   _max_original_projections_to_left[first_segment_idx + 1] <
                       limit) {
              ++first_segment_idx;
            }
          }
        }
      }
    }
    bool min_distance_updated = false;
    bool is_within_end_point = false;
    for (int idx = first_segment_idx; idx <= last_segment_idx; ++idx) {
      if (_min_original_projections_to_right[idx] > max_original_projection) {
        break;
      }
      const auto& original_segment = original_segments[idx];
      const double x0 = point.x() - original_segment.start().x();
      const double y0 = point.y() - original_segment.start().y();
      const double ux = original_segment.unit_direction().x();
      const double uy = original_segment.unit_direction().y();
      double proj = x0 * ux + y0 * uy;
      double distance = 0.0;
      if (proj < 0.0) {
        if (is_within_end_point) {
          continue;
        }
        is_within_end_point = true;
        distance = hypot(x0, y0);
      } else if (proj <= original_segment.length()) {
        is_within_end_point = true;
        distance = std::abs(x0 * uy - y0 * ux);
      } else {
        is_within_end_point = false;
        if (idx != last_segment_idx) {
          continue;
        }
        distance = original_segment.end().DistanceTo(point);
      }
      if (distance < *min_distance) {
        min_distance_updated = true;
        *min_distance = distance;
        nearest_segment_idx = idx;
      }
    }
    if (min_distance_updated) {
      min_distance_sqr_with_error = Sqr(*min_distance + _max_error);
    }
  }
  if (nearest_segment_idx >= 0) {
    const auto& segment = original_segments[nearest_segment_idx];
    double proj = segment.ProjectOntoUnit(point);
    const double prod = segment.ProductOntoUnit(point);
    if (nearest_segment_idx > 0) {
      proj = std::max(0.0, proj);
    }
    if (nearest_segment_idx + 1 < num_original_segments) {
      proj = std::min(segment.length(), proj);
    }
    *accumulate_s = original_accumulated_s[nearest_segment_idx] + proj;
    if ((nearest_segment_idx == 0 && proj < 0.0) ||
        (nearest_segment_idx + 1 == num_original_segments &&
         proj > segment.length())) {
      *lateral = prod;
    } else {
      *lateral = (prod > 0 ? (*min_distance) : -(*min_distance));
    }
    return true;
  }
  return false;
}

bool PathApproximation::overlap_with(const Path& path, const Box2d& box,
                                     double width) const {
  if (num_points_ == 0) {
    return false;
  }
  const Vec2d center = box.center();
  const double radius = box.diagonal() / 2.0 + width;
  const double radius_sqr = Sqr(radius);
  const auto& original_segments = path.segments();
  for (size_t i = 0; i < segments_.size(); ++i) {
    const LineSegment2d& segment = segments_[i];
    const double max_error = _max_error_per_segment[i];
    const double radius_sqr_with_error = Sqr(radius + max_error);
    if (segment.DistanceSquareTo(center) > radius_sqr_with_error) {
      continue;
    }
    int first_segment_idx = _original_ids[i];
    int last_segment_idx = _original_ids[i + 1] - 1;
    double max_original_projection = std::numeric_limits<double>::infinity();
    if (first_segment_idx < last_segment_idx) {
      const auto& segment = segments_[i];
      const double projection = segment.ProjectOntoUnit(center);
      const double prod_sqr = Sqr(segment.ProductOntoUnit(center));
      if (prod_sqr >= radius_sqr_with_error) {
        continue;
      }
      const double scan_distance = sqrt(radius_sqr_with_error - prod_sqr);
      const double min_projection = projection - scan_distance;
      max_original_projection = _projections[i] + projection + scan_distance;
      if (min_projection > 0.0) {
        const double limit = _projections[i] + min_projection;
        const int sample_index =
            std::max(0, static_cast<int>(limit / kSampleDistance));
        if (sample_index >= _num_projection_samples) {
          first_segment_idx = last_segment_idx;
        } else {
          first_segment_idx =
              std::max(first_segment_idx,
                       _sampled_max_original_projections_to_left[sample_index]);
          if (first_segment_idx >= last_segment_idx) {
            first_segment_idx = last_segment_idx;
          } else {
            while (first_segment_idx < last_segment_idx &&
                   _max_original_projections_to_left[first_segment_idx + 1] <
                       limit) {
              ++first_segment_idx;
            }
          }
        }
      }
    }
    for (int idx = first_segment_idx; idx <= last_segment_idx; ++idx) {
      if (_min_original_projections_to_right[idx] > max_original_projection) {
        break;
      }
      const auto& original_segment = original_segments[idx];
      if (original_segment.DistanceSquareTo(center) > radius_sqr) {
        continue;
      }
      if (box.DistanceTo(original_segment) <= width) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace hdmap
}  // namespace apollo
