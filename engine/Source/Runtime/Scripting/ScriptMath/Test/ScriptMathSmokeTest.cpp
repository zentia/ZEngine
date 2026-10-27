// =====================================================================
// ScriptMathSmokeTest
// ---------------------------------------------------------------------
// Verifies the script math API (VectorMath / QuaternionMath / Mathf) on the
// CPU side: every method is static, so this test calls them directly without
// going through the script runtime at all.
//
// Why this test exists: these methods are the **only** way a script can do
// arithmetic. Vector3 / Quaternion are not Object-derived, so ZCLASS cannot
// bind them (zht/parser.py: "ZCLASS must derive from an Object-based class")
// and a script's Vector3 is just the `{x, y, z}` plain object the Converter
// builds -- no methods. Before ScriptMath, a script could read
// transform.position but could not add two vectors, normalise one, or measure
// a distance: "move toward the target" was not expressible.
//
// The cases below concentrate on what is easy to get silently wrong:
//   * the behaviour this file defines itself (MoveTowards' negative
//     max_distance, ClampMagnitude's boundary, Sign's zero case);
//   * the engine behaviours ScriptMath *assumes* -- most importantly that
//     Normalize of a zero vector returns zero rather than NaN. If
//     Vector3::normalise ever changes, V5 fails instead of scripts getting
//     NaN positions.
//
// Deliberately NOT asserted: LookRotation's and ToEuler's axis conventions.
// The engine has no Euler->Quaternion constructor, and GetPitch/GetYaw/GetRoll
// carry Ogre (Y-up) comments that contradict each other, so which axis each
// component means in this Z-up engine cannot be read from the code. Asserting
// a guessed convention would lock in a wrong answer; only the identity case
// (all zeros) is checked.
//
// Own main(); excluded from ZRuntime in Runtime/CMakeLists.txt.
// Pure CPU -- no project, no window, no GPU, no script runtime.
// =====================================================================

#include "Runtime/Scripting/ScriptMath/ScriptMath.h"

#include <cmath>
#include <cstdio>

namespace
{
    int g_Checks = 0;
    int g_Failures = 0;

    void Check(bool condition, const char* name)
    {
        ++g_Checks;
        if (!condition)
        {
            std::fprintf(stderr, "  [FAIL] %s\n", name);
            ++g_Failures;
        }
        else
        {
            std::fprintf(stdout, "  [ ok ] %s\n", name);
        }
    }

    bool Nearly(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) < tolerance; }

    bool Nearly(const Vector3& a, const Vector3& b, float tolerance = 1e-4f)
    {
        return Nearly(a.x, b.x, tolerance) && Nearly(a.y, b.y, tolerance) && Nearly(a.z, b.z, tolerance);
    }

    bool Nearly(const Quaternion& a, const Quaternion& b, float tolerance = 1e-4f)
    {
        return Nearly(a.w, b.w, tolerance) && Nearly(a.x, b.x, tolerance) && Nearly(a.y, b.y, tolerance) &&
               Nearly(a.z, b.z, tolerance);
    }
}  // namespace

int main()
{
    const Vector3 x_axis = VectorMath::UnitX();
    const Vector3 y_axis = VectorMath::UnitY();
    const Vector3 z_axis = VectorMath::UnitZ();

    // ---- V1: constants ----
    Check(VectorMath::Create(1.f, 2.f, 3.f) == Vector3(1.f, 2.f, 3.f), "V1 Create(x, y, z)");
    Check(VectorMath::Zero().isZero(), "V1b Zero()");
    Check(Nearly(VectorMath::One(), Vector3(1.f, 1.f, 1.f)), "V1c One()");
    Check(Nearly(x_axis, Vector3(1.f, 0.f, 0.f)), "V1d UnitX()");
    Check(Nearly(y_axis, Vector3(0.f, 1.f, 0.f)), "V1e UnitY()");
    Check(Nearly(z_axis, Vector3(0.f, 0.f, 1.f)), "V1f UnitZ() -- Z-up: this is the vertical axis");

    // ---- V2: arithmetic ----
    Check(Nearly(VectorMath::Add(x_axis, y_axis), Vector3(1.f, 1.f, 0.f)), "V2 Add");
    Check(Nearly(VectorMath::Sub(x_axis, y_axis), Vector3(1.f, -1.f, 0.f)), "V2b Sub");
    Check(Nearly(VectorMath::Mul(Vector3(2.f, 3.f, 4.f), Vector3(5.f, 6.f, 7.f)), Vector3(10.f, 18.f, 28.f)),
          "V2c Mul is component-wise (not dot, not cross)");
    Check(Nearly(VectorMath::Scale(x_axis, 3.f), Vector3(3.f, 0.f, 0.f)), "V2d Scale");
    Check(Nearly(VectorMath::Negate(Vector3(1.f, -2.f, 3.f)), Vector3(-1.f, 2.f, -3.f)), "V2e Negate");

    // ---- V3: measure ----
    Check(Nearly(VectorMath::Magnitude(Vector3(3.f, 4.f, 0.f)), 5.f), "V3 Magnitude");
    Check(Nearly(VectorMath::SqrMagnitude(Vector3(3.f, 4.f, 0.f)), 25.f), "V3b SqrMagnitude");
    Check(Nearly(VectorMath::Distance(Vector3(1.f, 0.f, 0.f), Vector3(4.f, 4.f, 0.f)), 5.f), "V3c Distance");
    Check(Nearly(VectorMath::SqrDistance(Vector3(1.f, 0.f, 0.f), Vector3(4.f, 4.f, 0.f)), 25.f),
          "V3d SqrDistance");

    // ---- V4: dot / cross ----
    Check(Nearly(VectorMath::Dot(x_axis, y_axis), 0.f), "V4 Dot of orthogonal axes == 0");
    Check(Nearly(VectorMath::Dot(x_axis, x_axis), 1.f), "V4b Dot of a unit axis with itself == 1");
    Check(Nearly(VectorMath::Cross(x_axis, y_axis), z_axis), "V4c Cross(X, Y) == Z (right-handed)");

    // ---- V5: normalize, including the zero case ScriptMath relies on ----
    Check(Nearly(VectorMath::Magnitude(VectorMath::Normalize(Vector3(3.f, 4.f, 0.f))), 1.f),
          "V5 Normalize gives unit length");
    {
        const Vector3 n = VectorMath::Normalize(VectorMath::Zero());
        Check(!n.isNaN() && n.isZero(), "V5b Normalize(zero) == zero, NOT NaN (scripts would poison a position)");
    }

    // ---- V6: angle ----
    Check(Nearly(VectorMath::Angle(x_axis, y_axis), 90.f), "V6 Angle(X, Y) == 90 degrees");
    Check(Nearly(VectorMath::AngleRad(x_axis, y_axis), Math_PI * 0.5f), "V6b AngleRad(X, Y) == pi/2");
    Check(Nearly(VectorMath::Angle(x_axis, x_axis), 0.f), "V6c Angle of a vector with itself == 0");

    // ---- V7: lerp ----
    {
        const Vector3 a = VectorMath::Zero();
        const Vector3 b = VectorMath::Create(10.f, 0.f, 0.f);
        Check(Nearly(VectorMath::Lerp(a, b, 0.f), a), "V7 Lerp(t=0) == a");
        Check(Nearly(VectorMath::Lerp(a, b, 1.f), b), "V7b Lerp(t=1) == b");
        Check(Nearly(VectorMath::Lerp(a, b, 0.5f), Vector3(5.f, 0.f, 0.f)), "V7c Lerp(t=0.5) == midpoint");
        // 不夹取：与 Unity 的 LerpUnclamped 同，想夹取先自己 Clamp01。
        Check(Nearly(VectorMath::Lerp(a, b, 2.f), Vector3(20.f, 0.f, 0.f)), "V7d Lerp does NOT clamp alpha");
    }

    // ---- V8: move towards (defined here, so worth pinning down) ----
    {
        const Vector3 a = VectorMath::Zero();
        const Vector3 b = VectorMath::Create(10.f, 0.f, 0.f);

        Check(Nearly(VectorMath::MoveTowards(a, b, 3.f), Vector3(3.f, 0.f, 0.f)), "V8 MoveTowards stops at the step");
        Check(Nearly(VectorMath::MoveTowards(a, b, 100.f), b), "V8b MoveTowards returns b when the step overshoots");
        Check(Nearly(VectorMath::MoveTowards(a, a, 5.f), a), "V8c MoveTowards from a to a == a (no divide by zero)");
        Check(Nearly(VectorMath::MoveTowards(a, b, -4.f), Vector3(-4.f, 0.f, 0.f)),
              "V8d MoveTowards with a negative step walks BACKWARDS (matches Unity, not a bug)");
    }

    // ---- V9: clamp magnitude ----
    {
        const Vector3 v = VectorMath::Create(0.f, 10.f, 0.f);
        Check(Nearly(VectorMath::Magnitude(VectorMath::ClampMagnitude(v, 3.f)), 3.f),
              "V9 ClampMagnitude caps the length");
        Check(Nearly(VectorMath::ClampMagnitude(v, 3.f), Vector3(0.f, 3.f, 0.f)),
              "V9b ClampMagnitude keeps the direction");
        Check(Nearly(VectorMath::ClampMagnitude(Vector3(1.f, 0.f, 0.f), 5.f), Vector3(1.f, 0.f, 0.f)),
              "V9c ClampMagnitude leaves a short enough vector alone");
        Check(VectorMath::ClampMagnitude(VectorMath::Zero(), 5.f).isZero(),
              "V9d ClampMagnitude(zero) == zero");
    }

    // ---- V10: clamp / min / max ----
    {
        const Vector3 v = VectorMath::Create(-5.f, 5.f, 0.f);
        const Vector3 lo = VectorMath::Create(-1.f, -1.f, -1.f);
        const Vector3 hi = VectorMath::Create(1.f, 1.f, 1.f);
        Check(Nearly(VectorMath::Clamp(v, lo, hi), Vector3(-1.f, 1.f, 0.f)), "V10 Clamp is per-component");
        Check(Nearly(VectorMath::Min(v, VectorMath::Zero()), Vector3(-5.f, 0.f, 0.f)), "V10b Min");
        Check(Nearly(VectorMath::Max(v, VectorMath::Zero()), Vector3(0.f, 5.f, 0.f)), "V10c Max");
    }

    // ---- V11: reflect / project ----
    {
        // 竖直向下打到水平面（法线 = +Z，Z-up）→ 反射回竖直向上。
        const Vector3 down = VectorMath::Create(0.f, 0.f, -1.f);
        Check(Nearly(VectorMath::Reflect(down, z_axis), z_axis), "V11 Reflect off a +Z plane");
        // 斜向量投影到水平面上 → 竖直分量归零。
        const Vector3 slant = VectorMath::Create(3.f, 4.f, 5.f);
        Check(Nearly(VectorMath::Project(slant, z_axis), Vector3(3.f, 4.f, 0.f)), "V11b Project onto a +Z plane");
    }

    Check(VectorMath::IsZero(VectorMath::Zero()) && !VectorMath::IsZero(x_axis), "V12 IsZero");
    Check(Nearly(VectorMath::FromToRotation(x_axis, y_axis) * x_axis, y_axis),
          "V13 FromToRotation(X, Y) rotates X onto Y");

    // ---- Q1: angle axis / rotate ----
    {
        const Quaternion q = QuaternionMath::AngleAxis(90.f, z_axis);
        // 右手系绕 +Z 转 90 度：X -> Y。
        Check(Nearly(QuaternionMath::Rotate(q, x_axis), y_axis), "Q1 AngleAxis(90, +Z) rotates X onto Y");
        Check(Nearly(QuaternionMath::Rotate(QuaternionMath::Identity(), x_axis), x_axis),
              "Q1b Rotate by Identity is a no-op");
        Check(Nearly(QuaternionMath::Angle(QuaternionMath::Identity(), q), 90.f),
              "Q1c Angle(identity, q) == 90 degrees");
    }

    // ---- Q2: composition / inverse ----
    {
        const Quaternion q = QuaternionMath::AngleAxis(30.f, z_axis);
        Check(Nearly(QuaternionMath::Mul(QuaternionMath::Identity(), q), q), "Q2 Mul(identity, q) == q");
        Check(Nearly(QuaternionMath::Mul(q, q.inverse()), QuaternionMath::Identity()),
              "Q2b q * q.inverse() == identity");
        Check(Nearly(QuaternionMath::Inverse(QuaternionMath::Inverse(q)), q), "Q2c Inverse is involutive");
    }

    // ---- Q3: slerp ----
    {
        const Quaternion a = QuaternionMath::Identity();
        const Quaternion b = QuaternionMath::AngleAxis(90.f, z_axis);
        Check(Nearly(QuaternionMath::Slerp(a, b, 0.f), a), "Q3 Slerp(t=0) == a");
        Check(Nearly(QuaternionMath::Slerp(a, b, 1.f), b), "Q3b Slerp(t=1) == b");
        Check(Nearly(QuaternionMath::Angle(a, QuaternionMath::Slerp(a, b, 0.5f)), 45.f),
              "Q3c Slerp(t=0.5) is halfway in angle");
        Check(Nearly(QuaternionMath::Nlerp(a, b, 0.f), a), "Q3d Nlerp(t=0) == a");
    }

    // ---- Q4: to euler (identity only -- see the header note) ----
    {
        const Vector3 e = QuaternionMath::ToEuler(QuaternionMath::Identity());
        Check(Nearly(e, VectorMath::Zero()), "Q4 ToEuler(identity) == (0, 0, 0)");
    }

    // ---- M1: scalars ----
    Check(Nearly(Mathf::Clamp(5.f, 0.f, 1.f), 1.f), "M1 Clamp above the range");
    Check(Nearly(Mathf::Clamp(-5.f, 0.f, 1.f), 0.f), "M1b Clamp below the range");
    Check(Nearly(Mathf::Clamp01(0.25f), 0.25f), "M1c Clamp01 inside");
    Check(Nearly(Mathf::Lerp(0.f, 10.f, 0.5f), 5.f), "M1d Lerp");
    Check(Nearly(Mathf::MoveTowards(0.f, 10.f, 3.f), 3.f), "M1e MoveTowards stops at the step");
    Check(Nearly(Mathf::MoveTowards(0.f, 10.f, 100.f), 10.f), "M1f MoveTowards does not overshoot");
    Check(Nearly(Mathf::Sign(-2.f), -1.f) && Nearly(Mathf::Sign(2.f), 1.f) && Nearly(Mathf::Sign(0.f), 0.f),
          "M1g Sign(-)/(+)/(0)");
    Check(Nearly(Mathf::Deg2Rad(180.f), Math_PI), "M1h Deg2Rad(180) == pi");
    Check(Nearly(Mathf::Rad2Deg(Math_PI), 180.f), "M1i Rad2Deg(pi) == 180");
    Check(Nearly(Mathf::Pi(), Math_PI), "M1j Pi()");

    std::fprintf(stdout, "\nScriptMath: %d checks, %d failed\n", g_Checks, g_Failures);
    return g_Failures == 0 ? 0 : 1;
}
