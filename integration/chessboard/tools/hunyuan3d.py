"""混元图生 3D 的最小封装。

官方 `buddy-cloud.py 3d` 只暴露了文生 3D（`{Prompt, Model}`），
但底层 `SubmitHunyuanTo3DProJob` 支持 `ImageBase64`。这里复用它的
签名/代理/轮询实现，只把 body 换成图生 3D，避免自己重写 TC3 签名。

用法：
    python tools/hunyuan3d.py --image assets/style_refs/star_pillar.jpg \
        --out assets/external_meshes/star_pillar.hunyuan.glb

注意：生成文件 URL 只有 24 小时有效，脚本会立刻下载并打印 sha256。
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import sys
import urllib.request

SKILL = os.path.join(
    os.environ.get("CODEBUDDY_HOME", r"C:\Users\zentiali\AppData\Local\Programs\CodeBuddy CN"),
    "resources", "app", "extensions", "genie", "out", "extension",
    "builtin", "buddy-multimodal-generation", "scripts", "buddy-cloud.py")


def load_cloud():
    spec = importlib.util.spec_from_file_location("buddy_cloud", SKILL)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load buddy-cloud.py from %s" % SKILL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def download(url: str, path: str) -> str:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with urllib.request.urlopen(url, timeout=300) as response:
        data = response.read()
    with open(path, "wb") as handle:
        handle.write(data)
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description="Hunyuan image-to-3D")
    parser.add_argument("--image", required=True)
    parser.add_argument("--out", required=True, help="落盘路径（.glb）")
    parser.add_argument("--prompt", default="")
    parser.add_argument("--model", default="3.1", choices=["3.0", "3.1"])
    parser.add_argument("--face-count", type=int, default=30000)
    parser.add_argument("--pbr", action="store_true", help="开启 PBR 材质")
    parser.add_argument("--token", default=os.environ.get("BUDDY_CLOUD_TOKEN", ""))
    parser.add_argument("--max-poll-time", type=int, default=900)
    args = parser.parse_args()

    if not args.token:
        print("ERROR: no token (pass --token or BUDDY_CLOUD_TOKEN)")
        return 2

    cloud = load_cloud()
    with open(args.image, "rb") as handle:
        encoded = base64.b64encode(handle.read()).decode("ascii")
    body = {
        "Model": args.model,
        "ImageBase64": encoded,
        "FaceCount": max(10000, min(1500000, args.face_count)),
    }
    if args.prompt:
        body["Prompt"] = args.prompt
    if args.pbr:
        body["EnablePBR"] = True

    cfg = cloud._PROVIDER_MAP["3d"]  # noqa: SLF001
    endpoint = cloud._resolve_default_endpoint()  # noqa: SLF001
    submitted = cloud._call_api(  # noqa: SLF001
        endpoint, cfg["provider"], cfg["service"], cfg["version"],
        cfg["submit_action"], body, args.token)
    job_id = submitted.get("JobId") or submitted.get("job_id")
    if not job_id:
        print("SUBMIT FAILED: %s" % json.dumps(submitted, ensure_ascii=False))
        return 1
    print("JOB %s" % job_id)

    result = cloud._poll_job(  # noqa: SLF001
        endpoint, cfg["provider"], cfg["service"], cfg["version"],
        cfg["query_action"], job_id, args.token, 5, args.max_poll_time)
    files = result.get("ResultFile3Ds") or result.get("result_files") or []
    print("FILES %s" % json.dumps(files, ensure_ascii=False))
    glb = next((f for f in files
                if str(f.get("Type", f.get("type", ""))).lower() == "glb"), None)
    if glb is None:
        print("NO GLB: %s" % json.dumps(result, ensure_ascii=False))
        return 1
    digest = download(glb.get("Url") or glb.get("url"), args.out)
    print("SAVED %s sha256=%s" % (args.out, digest))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
