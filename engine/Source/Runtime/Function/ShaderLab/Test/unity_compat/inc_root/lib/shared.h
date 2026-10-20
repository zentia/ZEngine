// Shared header for the include-root regression test.
//
// It deliberately lives OUTSIDE the .shader's own directory, so it can only be
// found when the compiler is given a project include root
// (ShaderLabDx12Compiler::SetIncludeDirectory). That is Unity's layout: a
// shader under <ShadersRoot>/Foo/Bar.shader includes headers relative to
// <ShadersRoot>, not to Foo/.

#ifndef ZENGINE_TEST_INC_ROOT_SHARED_H
#define ZENGINE_TEST_INC_ROOT_SHARED_H

float4 DoubleIt(float4 v)
{
    return v * float4(2.0, 2.0, 2.0, 2.0);
}

#endif  // ZENGINE_TEST_INC_ROOT_SHARED_H
