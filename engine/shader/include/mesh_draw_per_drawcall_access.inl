// Include after declaring per-drawcall SSBO bindings:
//   MeshDrawPerDrawcall per_drawcall;              (binding 1)
//   MeshDrawPerDrawcallVertexBlending vertex_blending; (binding 2)
// Keeps skinning code on legacy bare field names.

#define mesh_instances per_drawcall.mesh_instances
#define joint_matrices vertex_blending.joint_matrices
