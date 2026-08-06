#pragma once
#include <cmath>

namespace PlatformerPhysics2D{

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float x_, float y_) : x(x_), y(y_) {}

    Vec2 operator+(const Vec2& o) const { return { x + o.x, y + o.y }; }
    Vec2 operator-(const Vec2& o) const { return { x - o.x, y - o.y }; }
    Vec2 operator*(float s) const { return { x * s, y * s }; }
    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }

    static float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
    static float Length(const Vec2& v) { return std::sqrt(Dot(v, v)); }
};

} // namespace PlatformerPhysics2D
