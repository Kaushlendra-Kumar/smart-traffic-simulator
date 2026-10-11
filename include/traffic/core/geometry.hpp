#pragma once
#include <cmath>
#include <vector>

namespace traffic {

struct Vec2 {
  double x = 0.0;
  double y = 0.0;

  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(double k) const { return {x * k, y * k}; }
  constexpr bool operator==(const Vec2&) const = default;
  [[nodiscard]] double length() const { return std::hypot(x, y); }
  [[nodiscard]] Vec2 normalized() const {
    const double l = length();
    return l > 0 ? Vec2{x / l, y / l} : Vec2{};
  }
  /// Unit vector rotated 90 degrees clockwise: the "right-hand side" of a heading.
  [[nodiscard]] constexpr Vec2 right_normal() const { return {y, -x}; }
};

constexpr double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

/// Intersection point of two infinite lines (p + t*d). Returns false if parallel.
bool line_intersection(Vec2 p, Vec2 d, Vec2 q, Vec2 e, Vec2& out);

/// True if closed segments [a,b] and [c,d] properly intersect (touching endpoints excluded).
bool segments_intersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d);

/// A polyline with cumulative arc length, used for intersection connectors.
class Polyline {
 public:
  Polyline() = default;
  explicit Polyline(std::vector<Vec2> pts);

  [[nodiscard]] double length() const { return cum_.empty() ? 0.0 : cum_.back(); }
  /// Point and unit tangent at arc length s (clamped to [0, length]).
  [[nodiscard]] Vec2 point_at(double s) const;
  [[nodiscard]] Vec2 tangent_at(double s) const;
  [[nodiscard]] const std::vector<Vec2>& points() const { return pts_; }
  [[nodiscard]] bool intersects(const Polyline& other) const;

 private:
  [[nodiscard]] std::size_t segment_for(double s) const;
  std::vector<Vec2> pts_;
  std::vector<double> cum_;
};

/// Samples a quadratic Bezier curve into a polyline.
Polyline quadratic_bezier(Vec2 p0, Vec2 p1, Vec2 p2, int samples = 16);

}  // namespace traffic
