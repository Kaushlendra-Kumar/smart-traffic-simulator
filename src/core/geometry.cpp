#include "traffic/core/geometry.hpp"

#include <algorithm>

namespace traffic {

bool line_intersection(Vec2 p, Vec2 d, Vec2 q, Vec2 e, Vec2& out) {
  const double den = cross(d, e);
  if (std::abs(den) < 1e-9) return false;
  const double t = cross(q - p, e) / den;
  out = p + d * t;
  return true;
}

bool segments_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  const double d1 = cross(b - a, c - a);
  const double d2 = cross(b - a, d - a);
  const double d3 = cross(d - c, a - c);
  const double d4 = cross(d - c, b - c);
  constexpr double eps = 1e-9;
  return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) &&
         ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

Polyline::Polyline(std::vector<Vec2> pts) : pts_(std::move(pts)) {
  cum_.reserve(pts_.size());
  double acc = 0.0;
  for (std::size_t i = 0; i < pts_.size(); ++i) {
    if (i > 0) acc += (pts_[i] - pts_[i - 1]).length();
    cum_.push_back(acc);
  }
}

std::size_t Polyline::segment_for(double s) const {
  // index i such that cum_[i] <= s <= cum_[i+1]
  auto it = std::upper_bound(cum_.begin(), cum_.end(), s);
  std::size_t i = it == cum_.begin() ? 0 : static_cast<std::size_t>(it - cum_.begin()) - 1;
  return std::min(i, pts_.size() >= 2 ? pts_.size() - 2 : 0);
}

Vec2 Polyline::point_at(double s) const {
  if (pts_.empty()) return {};
  if (pts_.size() == 1) return pts_[0];
  s = std::clamp(s, 0.0, length());
  const std::size_t i = segment_for(s);
  const double seg = cum_[i + 1] - cum_[i];
  const double t = seg > 0 ? (s - cum_[i]) / seg : 0.0;
  return pts_[i] + (pts_[i + 1] - pts_[i]) * t;
}

Vec2 Polyline::tangent_at(double s) const {
  if (pts_.size() < 2) return {1, 0};
  s = std::clamp(s, 0.0, length());
  const std::size_t i = segment_for(s);
  return (pts_[i + 1] - pts_[i]).normalized();
}

bool Polyline::intersects(const Polyline& other) const {
  for (std::size_t i = 0; i + 1 < pts_.size(); ++i)
    for (std::size_t j = 0; j + 1 < other.pts_.size(); ++j)
      if (segments_intersect(pts_[i], pts_[i + 1], other.pts_[j], other.pts_[j + 1])) return true;
  return false;
}

Polyline quadratic_bezier(Vec2 p0, Vec2 p1, Vec2 p2, int samples) {
  std::vector<Vec2> pts;
  pts.reserve(static_cast<std::size_t>(samples) + 1);
  for (int k = 0; k <= samples; ++k) {
    const double t = static_cast<double>(k) / samples;
    const double u = 1.0 - t;
    pts.push_back(p0 * (u * u) + p1 * (2 * u * t) + p2 * (t * t));
  }
  return Polyline(std::move(pts));
}

}  // namespace traffic
