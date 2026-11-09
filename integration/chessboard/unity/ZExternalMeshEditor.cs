// 一次性的编辑期操作：把外部网格消费端挂到场景的 ExternalMeshRoot 上。
//
// 为什么不直接在离线场景里写 m_Script：
//   trunk 的 Program/GameScripts/** 由外部编译器编成
//   Assets/Plugins/GameScripts/Scripts.dll，DLL 里的 MonoBehaviour 用的是
//   Scripts.dll.meta 的 GUID + 类型 fileID。脚本自己的 .cs.meta GUID 在
//   资产库里根本不存在，写进场景必然 Missing (Mono Script)。
//   所以这里让 Unity 自己 AddComponent 并保存场景——序列化出来的引用天然正确，
//   之后 guid / fileID 就稳定了，也不需要运行时代码去找对象。
//
// 用法：GameScriptBuilder/ForceRebuildAll 让 Scripts.dll 收录
//       ZEngine.Chessboard.ZExternalMeshBinder，然后
//       ZEngine/棋盘/挂载外部网格消费端。

using System;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace ZEngine.Chessboard.EditorTools
{
    public static class ZExternalMeshEditor
    {
        private const string MarkerName = "ExternalMeshRoot";
        private const string BinderTypeName = "ZEngine.Chessboard.ZExternalMeshBinder";

        [MenuItem("ZEngine/棋盘/挂载外部网格消费端", false, 10)]
        public static void AttachBinder()
        {
            Scene scene = SceneManager.GetActiveScene();
            if (!scene.IsValid() || string.IsNullOrEmpty(scene.path))
            {
                EditorUtility.DisplayDialog("ZEngine 棋盘",
                    "请先打开 ART_OS_AC90_HD.unity 场景。", "知道了");
                return;
            }

            GameObject marker = FindMarker(scene);
            if (marker == null)
            {
                EditorUtility.DisplayDialog("ZEngine 棋盘",
                    "当前场景没有 " + MarkerName + " 节点。\n" +
                    "先跑 deploy_to_trunk.py --apply 覆盖场景。", "知道了");
                return;
            }

            Type binder = FindBinderType();
            if (binder == null)
            {
                EditorUtility.DisplayDialog("ZEngine 棋盘",
                    "Scripts.dll 里没有 " + BinderTypeName + "。\n\n" +
                    "先执行菜单 GameScriptBuilder/ForceRebuildAll（Ctrl+Shift+T），\n" +
                    "确认 Assets/Plugins/GameScripts/Scripts.dll 时间戳更新后再点本菜单。",
                    "知道了");
                return;
            }

            Component existing = marker.GetComponent(binder);
            if (existing != null)
            {
                LogReference(existing);
                EditorUtility.DisplayDialog("ZEngine 棋盘",
                    MarkerName + " 已经挂好了，无需重复操作。", "好");
                return;
            }

            Component component = Undo.AddComponent(marker, binder);
            if (component == null)
            {
                Debug.LogError("[ZEngine] AddComponent failed: " + BinderTypeName);
                return;
            }

            EditorUtility.SetDirty(marker);
            EditorSceneManager.MarkSceneDirty(scene);
            EditorSceneManager.SaveScene(scene);
            LogReference(component);
            Debug.Log("[ZEngine] 已挂载 " + BinderTypeName + " 并保存场景：" + scene.path);
        }

        [MenuItem("ZEngine/棋盘/挂载外部网格消费端", true)]
        private static bool AttachBinderValidate()
        {
            return !EditorApplication.isPlayingOrWillChangePlaymode;
        }

        [MenuItem("ZEngine/棋盘/打印外部网格脚本引用", false, 11)]
        public static void PrintReference()
        {
            Type binder = FindBinderType();
            if (binder == null)
            {
                Debug.LogError("[ZEngine] Scripts.dll 里没有 " + BinderTypeName);
                return;
            }
            Scene scene = SceneManager.GetActiveScene();
            GameObject marker = scene.IsValid() ? FindMarker(scene) : null;
            Component existing = marker != null ? marker.GetComponent(binder) : null;
            if (existing == null)
            {
                Debug.LogWarning("[ZEngine] 先执行「挂载外部网格消费端」再打印引用");
                return;
            }
            LogReference(MonoScript.FromMonoBehaviour((MonoBehaviour)existing));
        }

        private static GameObject FindMarker(Scene scene)
        {
            foreach (GameObject root in scene.GetRootGameObjects())
            {
                Transform found = FindDeep(root.transform, MarkerName);
                if (found != null) return found.gameObject;
            }
            return null;
        }

        private static Transform FindDeep(Transform parent, string name)
        {
            if (parent.name == name) return parent;
            for (int i = 0; i < parent.childCount; i++)
            {
                Transform found = FindDeep(parent.GetChild(i), name);
                if (found != null) return found;
            }
            return null;
        }

        private static Type FindBinderType()
        {
            Type direct = Type.GetType(BinderTypeName + ", Scripts");
            if (direct != null) return direct;

            foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
            {
                Type type = assembly.GetType(BinderTypeName, false);
                if (type != null && typeof(Component).IsAssignableFrom(type))
                {
                    return type;
                }
            }
            return null;
        }

        private static void LogReference(UnityEngine.Object target)
        {
            if (target == null) return;
            string guid;
            long fileId;
            if (AssetDatabase.TryGetGUIDAndLocalFileIdentifier(target, out guid, out fileId))
            {
                Debug.Log("[ZEngine] m_Script: {fileID: " + fileId +
                          ", guid: " + guid + ", type: 3}  (" + target.name + ")");
            }
            else
            {
                Debug.LogWarning("[ZEngine] 无法解析 " + target.name + " 的脚本引用");
            }
        }
    }
}
