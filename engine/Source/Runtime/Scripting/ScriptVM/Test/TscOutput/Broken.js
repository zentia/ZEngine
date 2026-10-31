"use strict";
// ==========================================================================
// 故意在模块求值阶段抛异常的脚本。
//
// 它钉住的是 ScriptEnv::LoadModule 的一个真实 bug：模块执行到一半抛了（例如
// `class X extends NotAConstructor` 在模块求值阶段就炸），但 LoadModuleInternal
// 的 exports 兜底分支仍然拿得到一个非空的 `module.exports`，于是照样把引用塞进
// 缓存。不丢掉这个半成品的话：
//
//   * IsModuleLoaded() 说"已加载"；
//   * 之后每次 LoadModule 直接命中缓存返回 true；
//   * 这段模块**永远不会再被执行一次** —— 改好源码再热重载，拿到的还是那次抛
//     异常留下的空壳。
//
// 这个 bug 是在给 ScriptTsChainSmokeTest 做负向验证（把运行时的 Behaviour shim
// 从 function 改成 {}）时撞出来的：那时 T1 失败但 T2（IsModuleLoaded）竟然是真。
// ==========================================================================
throw new Error("Broken module, on purpose");
