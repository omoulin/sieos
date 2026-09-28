#!/usr/bin/env python3
"""
mksiaconfig.py AI_CONFIG ROOTFS PERMS_OUT

Pre-register the model for sia: read the model connection from AI_CONFIG and
write ~/.sia/config for root and user inside ROOTFS, plus the matching
ownership/mode lines (0700 directory, 0600 file) to PERMS_OUT.

AI_CONFIG may be key=value lines, "key: value" lines (optionally with
"export" and quotes), or a JSON object. Recognised names, case-insensitive:
  endpoint  endpoint, url, base_url, api_base, target_uri, *_endpoint
  model     model, model_name, deployment, deployment_name, *_deployment
  api_key   api_key, apikey, key, api-key, *_api_key, *_key
Values are never printed.
"""
import json
import os
import re
import sys

ACCOUNTS = [("root", "/root", 0, 0), ("user", "/home/user", 100, 10)]


def classify(name):
    k = name.strip().lower().replace("-", "_").replace(".", "_")
    if k in ("api_version", "version"):
        return None
    if "endpoint" in k or k in ("url", "base_url", "api_base", "target_uri", "uri", "host"):
        return "endpoint"
    if "deployment" in k or "model" in k:
        return "model"
    if k in ("key", "apikey", "token") or k.endswith("_key") or k.endswith("apikey") or k == "api_key":
        return "api_key"
    return None


def parse(text):
    found = {}
    stripped = text.strip()
    pairs = []
    if stripped.startswith("{"):
        def walk(obj):
            for k, v in obj.items():
                if isinstance(v, dict):
                    walk(v)
                elif isinstance(v, (str, int, float)):
                    pairs.append((k, str(v)))
        walk(json.loads(stripped))
    else:
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith(("#", ";", "[")):
                continue
            m = re.match(r"^(?:export\s+)?([A-Za-z0-9_.\-]+)\s*[=:]\s*(.*)$", line)
            if m:
                value = m.group(2).strip()
                if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                    value = value[1:-1]
                pairs.append((m.group(1), value))
    for k, v in pairs:
        field = classify(k)
        if field and v and field not in found:
            found[field] = v
    return found


def main():
    src, rootfs, perms_out = sys.argv[1], sys.argv[2], sys.argv[3]
    open(perms_out, "w").close()
    if not os.path.exists(src):
        return
    try:
        found = parse(open(src).read())
    except (ValueError, OSError) as e:
        print(f"mksiaconfig: {src}: cannot parse ({type(e).__name__}); sia will ask on first use", file=sys.stderr)
        return
    missing = [f for f in ("endpoint", "model", "api_key") if f not in found]
    if missing:
        print(f"mksiaconfig: {src}: missing {', '.join(missing)}; sia will ask on first use", file=sys.stderr)
        return
    endpoint = found["endpoint"]
    if not endpoint.startswith(("http://", "https://")):
        endpoint = "https://" + endpoint
    for bad in ("\n", "\r"):
        if any(bad in v for v in found.values()):
            print(f"mksiaconfig: {src}: values must be single lines", file=sys.stderr)
            return
    body = ("# sia model connection (Azure AI Foundry), from ai.config at build time. Keep this file private.\n"
            f"endpoint={endpoint}\nmodel={found['model']}\napi_key={found['api_key']}\nauto_approve=no\n")
    with open(perms_out, "w") as perms:
        for name, home, uid, gid in ACCOUNTS:
            d = os.path.join(rootfs + home, ".sia")
            os.makedirs(d, exist_ok=True)
            path = os.path.join(d, "config")
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(body)
            perms.write(f"{home}/.sia            0700     {uid:<6} {gid}\n")
            perms.write(f"{home}/.sia/config     0600     {uid:<6} {gid}\n")
    print(f"mksiaconfig: model registered from {src} for {', '.join(a[0] for a in ACCOUNTS)} "
          f"(endpoint, model and key found; values not shown)")


if __name__ == "__main__":
    main()
