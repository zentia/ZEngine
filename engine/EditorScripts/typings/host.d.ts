// Host-environment declarations for the editor script VM.
//
// Hand-written and checked in, unlike the sibling `zengine.d.ts` /
// `zengine-editor.d.ts`, which EditorScriptingManager::WriteTypings()
// regenerates from the real C++ bindings on every editor start.
//
// `console` is not part of the ECMAScript standard library, so tsconfig's
// `"lib": ["es2020"]` does not declare it and `"types": []` keeps @types/node
// out on purpose (this VM is not Node). The puerts backend installs a `console`
// global into the VM, so the API exists at runtime -- it just needs a
// declaration for tsc. Only the members the backend actually provides are
// declared here; adding one that the backend lacks would turn a compile-time
// error into a runtime crash.
declare const console: {
    log(...args: unknown[]): void;
    info(...args: unknown[]): void;
    warn(...args: unknown[]): void;
    error(...args: unknown[]): void;
};
