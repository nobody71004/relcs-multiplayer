#!/usr/bin/env python3
"""agent_harness.py — control a reLCS multiplayer server with a real-world LLM.

Pipeline:  LLM (OpenAI-compatible tool calling)  ->  reLCS-agentd (HTTP/JSON)
           ->  game server RCON / bot spawner    ->  live game + players

Modes:
  --mock                run a deterministic agent plan against agentd and
                        verify every tool result (used by the e2e run book)
  (default)             run a tool-calling loop against any OpenAI-compatible
                        chat/completions endpoint (env: OPENAI_API_BASE,
                        OPENAI_API_KEY, OPENAI_MODEL)

Usage:
  python agent_harness.py --mock --base http://127.0.0.1:7801 --rcon e2epw
  OPENAI_API_BASE=https://api.openai.com/v1 OPENAI_API_KEY=... \
      python agent_harness.py --base http://127.0.0.1:7801 --rcon e2epw \
      --goal "welcome everyone and spawn a roaming bot called Scar"
"""
import argparse
import json
import os
import sys
import urllib.request

BASE = "http://127.0.0.1:7801"
RCON = ""


def call(path, payload=None, method=None):
    """One JSON call to the agent bridge. Returns parsed JSON."""
    url = BASE + path
    data = None
    headers = {"X-Rcon": RCON, "Content-Type": "application/json"}
    if payload is not None:
        data = json.dumps(payload).encode()
        method = method or "POST"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


# ---- tools exposed to the LLM (mirrors GET /v1/tools) ----------------------
def tool_server_status(_args):
    return call("/v1/status")


def tool_server_rcon(args):
    return call("/v1/rcon", {"command": args["command"]})


def tool_server_say(args):
    return call("/v1/say", {"text": args["text"]})


def tool_server_give(args):
    return call("/v1/give", {
        "target": str(args.get("target", "")),
        "kind": args.get("kind", "money"),
        "value": int(args.get("value", 0)),
        "arg": int(args.get("arg", 200)),
    })


def tool_server_spawn_bot(args):
    return call("/v1/bot", {
        "name": args.get("name", "AgentBot"),
        "scenario": args.get("scenario", "roam"),
        "duration": int(args.get("duration", 600)),
    })


TOOLS = {
    "server_status": tool_server_status,
    "server_rcon": tool_server_rcon,
    "server_say": tool_server_say,
    "server_give": tool_server_give,
    "server_spawn_bot": tool_server_spawn_bot,
}


def run_tool(name, args):
    fn = TOOLS.get(name)
    if not fn:
        return {"ok": False, "error": "unknown tool " + name}
    try:
        return fn(args)
    except Exception as e:  # noqa: BLE001 - report to the model
        return {"ok": False, "error": str(e)}


# ---- mock mode: deterministic end-to-end pipeline verification -------------
def run_mock():
    checks = []

    def expect(desc, ok, detail=None):
        checks.append((desc, bool(ok)))
        print(("  ok  " if ok else " FAIL ") + desc + ("  " + str(detail)[:160] if (detail and not ok) else ""))

    # 1. tool schema available
    tools = call("/v1/tools", method="GET")
    expect("tools schema has 5 tools", len(tools.get("tools", [])) == 5)

    # 2. live status
    st = run_tool("server_status", {})
    expect("server_status ok", st.get("ok") is True and len(st.get("status", [])) > 0)

    # 3. broadcast
    say = run_tool("server_say", {"text": "Agent harness online"})
    expect("server_say ok", say.get("ok") is True, say)

    # 4. find a player target (if any) and give money + godmode
    target = None
    for line in st.get("inventory", []):
        # "#id name money=.. hp=.. armour=.. weapon=.. ammo=.."
        parts = line.replace("#", "").split()
        if parts and parts[0].isdigit():
            target = parts[0]
            break
    if target:
        g1 = run_tool("server_give", {"target": target, "kind": "money", "value": 5000})
        expect("give money ok", g1.get("ok") is True, g1)
        g2 = run_tool("server_give", {"target": target, "kind": "god", "value": 1})
        expect("give godmode ok", g2.get("ok") is True, g2)
    else:
        print("  ..  no players online; give tests skipped")

    # 5. spawn an NPC bot player
    bot = run_tool("server_spawn_bot", {"name": "AgentMock", "scenario": "roam", "duration": 60})
    expect("spawn_bot ok", bot.get("ok") is True, bot)

    # 6. raw rcon still works
    ver = run_tool("server_rcon", {"command": "version"})
    expect("rcon version ok", ver.get("ok") is True, ver)

    passed = all(ok for _, ok in checks)
    print("[HARNESS] RESULT: %s" % ("PASS" if passed else "FAIL"))
    return 0 if passed else 1


# ---- real LLM mode: OpenAI-compatible tool-calling loop --------------------
def run_llm(goal):
    api = os.environ.get("OPENAI_API_BASE", "https://api.openai.com/v1").rstrip("/")
    key = os.environ.get("OPENAI_API_KEY", "")
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    tools = call("/v1/tools", method="GET")["tools"]

    messages = [
        {"role": "system", "content":
         "You operate a Liberty City Stories multiplayer server. Use the tools "
         "to inspect and change live game state. Be brief. After finishing, "
         "reply with DONE."},
        {"role": "user", "content": goal},
    ]

    for step in range(12):
        req = urllib.request.Request(
            api + "/chat/completions",
            data=json.dumps({"model": model, "messages": messages, "tools": tools}).encode(),
            headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=60) as r:
            resp = json.loads(r.read().decode())
        msg = resp["choices"][0]["message"]
        messages.append(msg)
        calls = msg.get("tool_calls") or []
        if not calls:
            print("[HARNESS] RESULT: PASS (goal answered)")
            return 0
        for tc in calls:
            name = tc["function"]["name"]
            args = json.loads(tc["function"].get("arguments") or "{}")
            print("  tool: %s %s" % (name, json.dumps(args)[:120]))
            result = run_tool(name, args)
            messages.append({
                "role": "tool",
                "tool_call_id": tc.get("id", ""),
                "content": json.dumps(result)[:4000],
            })
    print("[HARNESS] RESULT: FAIL (loop exhausted)")
    return 1


def main():
    global BASE, RCON
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE, help="agent bridge base URL")
    ap.add_argument("--rcon", default=os.environ.get("RCON_PW", ""), help="rcon password")
    ap.add_argument("--mock", action="store_true", help="deterministic pipeline test")
    ap.add_argument("--goal", default="", help="LLM goal (real mode)")
    a = ap.parse_args()
    BASE, RCON = a.base.rstrip("/"), a.rcon
    if a.mock:
        return run_mock()
    return run_llm(a.goal or "Say hi to everyone and report the server status.")


if __name__ == "__main__":
    sys.exit(main())
