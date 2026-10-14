#pragma once
// Spring-based animation values. They only advance while a surface is animating; once settled
// the frame loop stops completely (no idle redraws).
#include <cmath>

namespace nb::gfx {

// Global motion preference: Windows "Show animations" off, or the app's reduce-motion setting.
bool ReducedMotion();
void SetReducedMotion(bool reduced);

struct Spring {
    float value = 0, target = 0, velocity = 0;
    float stiffness = 420;  // higher = faster
    float damping = 34;     // ~0.83 damping ratio at mass 1 -> a hint of overshoot ("island" feel)

    Spring() = default;
    Spring(float v, float k = 420, float d = 34) : value(v), target(v), stiffness(k), damping(d) {}

    void Snap(float v) {
        value = target = v;
        velocity = 0;
    }
    void To(float t) {
        target = t;
        if (ReducedMotion()) Snap(t);
    }
    bool Settled() const { return std::fabs(value - target) < 0.01f && std::fabs(velocity) < 0.05f; }
    // Returns true while still moving.
    bool Step(float dt) {
        if (Settled()) {
            value = target;
            velocity = 0;
            return false;
        }
        // semi-implicit Euler in small sub-steps for stability at low frame rates
        float remaining = dt > 0.1f ? 0.1f : dt;
        while (remaining > 0) {
            float h = remaining > 1.0f / 240 ? 1.0f / 240 : remaining;
            float force = -stiffness * (value - target) - damping * velocity;
            velocity += force * h;
            value += velocity * h;
            remaining -= h;
        }
        return !Settled();
    }
};

// Presets
inline Spring SoftSpring(float v) { return Spring(v, 320, 32); }  // panels
inline Spring SnappySpring(float v) { return Spring(v, 520, 44); } // icons, hover
inline Spring FadeSpring(float v) { return Spring(v, 380, 40); }   // opacity (no overshoot)

} // namespace nb::gfx
