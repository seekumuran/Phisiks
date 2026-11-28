#!/usr/bin/env python3
"""Example PAI client: drives the PHISIKS editor through the PHISIKS Agent Interface (docs/PAI.md).

PAI is newline-delimited JSON-RPC 2.0 on the editor's stdin / stdout. This client starts the editor
itself, so no port is opened and nothing but this process can talk to it. Standard library only.

    python pai_client.py --exe Build/bin/Release/Phisiks.exe            # demo session
    python pai_client.py --exe ... --check                               # protocol / permission checks (ctest PaiProtocol)

Use PaiClient in your own tool (an agent's tool layer, a script, a test):

    with PaiClient(exe, write=True, simulate=True) as pai:
        print(pai.call("scene.list"))
        cube = pai.call("scene.create", name="Box", components={"meshRenderer": {"mesh": "Cube"}})
        pai.call("scene.set_transform", id=cube["id"], position=[0, 2, 0])
"""
import argparse
import json
import os
import queue
import subprocess
import sys
import threading
import time


class PaiError(Exception):
    def __init__(self, code, message):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class PaiClient:
    """One editor process, one PAI session. Permissions are fixed when the editor starts."""

    default_args = ()

    def __init__(self, exe, write=False, simulate=False, extra_args=(), timeout=60.0):
        exe = os.path.abspath(exe)
        args = [exe, "--pai", "--hidden"]
        if write:
            args.append("--pai-write")
        if simulate:
            args.append("--pai-simulate")
        args += list(self.default_args) + list(extra_args)
        self.timeout = timeout
        self.next_id = 1
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            self.lines.put(raw.decode("utf-8", "replace").rstrip("\r\n"))
        self.lines.put(None)

    def send_raw(self, text):
        """Send one line as-is and return the parsed response (for malformed-input tests)."""
        self.proc.stdin.write(text.encode("utf-8") + b"\n")
        self.proc.stdin.flush()
        line = self.lines.get(timeout=self.timeout)
        if line is None:
            raise RuntimeError("the editor closed its output")
        return json.loads(line)

    def request(self, method, **params):
        """The full JSON-RPC response object."""
        rid = self.next_id
        self.next_id += 1
        response = self.send_raw(json.dumps({"jsonrpc": "2.0", "id": rid, "method": method, "params": params}))
        if response.get("id") != rid:
            raise RuntimeError(f"response id {response.get('id')} does not match request {rid}")
        return response

    def call(self, method, **params):
        """The result, or PaiError."""
        response = self.request(method, **params)
        if "error" in response:
            raise PaiError(response["error"]["code"], response["error"]["message"])
        return response["result"]

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.terminate()   # the editor would otherwise keep running after its input closes
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def wait_steps(pai, timeout=30.0):
    end = time.time() + timeout
    while time.time() < end:
        status = pai.call("sim.status")
        if status["stepping"] == 0:
            return status
        time.sleep(0.05)
    raise RuntimeError("sim.step did not finish")


def demo(exe):
    with PaiClient(exe, write=True, simulate=True) as pai:
        hello = pai.call("initialize", client="pai_client.py")
        print(f"connected: {hello['engine']} {hello['engineVersion']}, {hello['protocol']} {hello['protocolVersion']}, permissions {hello['permissions']}")
        manifest = pai.call("pai.describe")
        print(f"{len(manifest['methods'])} methods:", ", ".join(sorted(manifest["methods"])))
        print("project:", json.dumps(pai.call("project.info")))
        for e in pai.call("scene.list")["entities"]:
            print(f"  {e['id']}  {e['name']:<20} {', '.join(e['components'])}")
        box = pai.call("scene.create", name="PAI box", position=[0, 4, 0],
                       components={"meshRenderer": {"mesh": "Cube"}, "collider": {"shape": "Box"}, "rigidbody": {"mass": 2.0}})
        print("created", box["id"], "- undo label:", pai.call("history.get")["undoLabel"])
        pai.call("sim.step", frames=60)
        status = wait_steps(pai)
        metrics = pai.call("sim.metrics", ids=[box["id"]])
        print(f"after 60 frames (t = {status['simulationTime']:.2f} s): box at {metrics['entities'][0]['position']}")
        pai.call("sim.stop")
        print("stopped: box back at", pai.call("scene.get", id=box["id"])["transform"]["position"])
        print("undo:", pai.call("history.undo")["undone"], "- entities now", pai.call("scene.list")["total"])


def check(exe):
    """Protocol, permission and safety checks. Exit code 0 when all pass."""
    failures = []

    def expect(name, condition, detail=""):
        print(("PASS " if condition else "FAIL ") + name + ("" if condition else f"  ({detail})"))
        if not condition:
            failures.append(name)

    def error_code(fn):
        try:
            fn()
        except PaiError as e:
            return e.code
        return None

    # --- read-only session (the default permission set)
    with PaiClient(exe) as pai:
        hello = pai.call("initialize")
        expect("initialize: protocol", hello["protocol"] == "phisiks-pai" and hello["protocolVersion"] == "1.0", hello)
        expect("initialize: read only", hello["permissions"] == {"read": True, "write": False, "simulate": False}, hello["permissions"])
        expect("initialize: only granted methods listed", "scene.create" not in hello["methods"] and "scene.list" in hello["methods"])
        manifest = pai.call("pai.describe")
        expect("manifest: every method has permission and description",
               all({"permission", "granted", "params", "description"} <= set(m) for m in manifest["methods"].values()))
        expect("manifest: write methods marked not granted", manifest["methods"]["scene.create"]["granted"] is False)
        listed = pai.call("scene.list")
        expect("scene.list", listed["total"] >= 1 and all(len(e["id"]) == 16 for e in listed["entities"]), listed)
        first = listed["entities"][0]
        got = pai.call("scene.get", id=first["id"])
        expect("scene.get", got["id"] == first["id"] and "transform" in got, got)
        expect("engine.state", "version" in pai.call("engine.state"))
        expect("project.info: edit mode", pai.call("project.info")["mode"] == "edit")
        expect("permission: write refused", error_code(lambda: pai.call("scene.create", name="x")) == -32001)
        expect("permission: simulate refused", error_code(lambda: pai.call("sim.play")) == -32001)
        expect("permission: undo refused", error_code(lambda: pai.call("history.undo")) == -32001)
        expect("unknown method", error_code(lambda: pai.call("shell.exec", command="dir")) == -32601)
        expect("unknown entity", error_code(lambda: pai.call("scene.get", id="00000000deadbeef")) == -32003)
        expect("bad id type", error_code(lambda: pai.call("scene.get", id=42)) == -32602)
        r = pai.send_raw("{not json")
        expect("malformed JSON -> parse error", r.get("error", {}).get("code") == -32700 and r.get("id") is None, r)
        r = pai.send_raw(json.dumps({"id": 7, "method": "scene.list"}))
        expect("missing jsonrpc -> invalid request", r.get("error", {}).get("code") == -32600 and r.get("id") == 7, r)
        r = pai.send_raw(json.dumps({"jsonrpc": "2.0", "id": 8, "method": "scene.list", "params": [1, 2]}))
        expect("positional params refused", r.get("error", {}).get("code") == -32602, r)
        r = pai.send_raw(json.dumps([{"jsonrpc": "2.0", "id": 9, "method": "engine.state"}, {"jsonrpc": "2.0", "id": 10, "method": "nope"}]))
        expect("batch", isinstance(r, list) and len(r) == 2 and "result" in r[0] and r[1]["error"]["code"] == -32601, r)
        r = pai.send_raw(json.dumps({"jsonrpc": "2.0", "id": 11, "method": "scene.list", "params": {"name": "x" * 300000}}))
        expect("oversized request refused", r.get("error", {}).get("code") == -32600, r)
        expect("still responsive", pai.call("engine.state")["version"] == hello["engineVersion"])

    # --- write + simulate session
    with PaiClient(exe, write=True, simulate=True) as pai:
        before = pai.call("scene.list")["total"]
        box = pai.call("scene.create", name="Box", position=[0, 3, 0],
                       components={"meshRenderer": {"mesh": "Cube"}, "collider": {"shape": "Box"}, "rigidbody": {"mass": 1.0}})
        expect("scene.create", pai.call("scene.list")["total"] == before + 1 and "rigidbody" in box["components"], box)
        expect("create is one undo step", pai.call("history.get")["undoLabel"] == "PAI: create 'Box'", pai.call("history.get"))
        pai.call("scene.set_transform", id=box["id"], position=[1, 5, 2])
        expect("set_transform", pai.call("scene.get", id=box["id"])["transform"]["position"] == [1, 5, 2])
        pai.call("history.undo")
        expect("undo restores the transform", pai.call("scene.get", id=box["id"])["transform"]["position"] == [0, 3, 0])
        pai.call("history.redo")
        expect("redo", pai.call("scene.get", id=box["id"])["transform"]["position"] == [1, 5, 2])
        pai.call("scene.set_component", id=box["id"], component="rigidbody", value={"mass": 4.5})
        expect("set_component", pai.call("scene.get", id=box["id"])["components"]["rigidbody"]["mass"] == 4.5)
        expect("invalid component value refused",
               error_code(lambda: pai.call("scene.set_component", id=box["id"], component="rigidbody", value={"mass": "heavy"})) == -32602)
        expect("unknown component refused",
               error_code(lambda: pai.call("scene.set_component", id=box["id"], component="nope", value={})) == -32602)
        expect("file path in mesh refused",
               error_code(lambda: pai.call("scene.set_component", id=box["id"], component="meshRenderer", value={"mesh": "C:/Windows/win.ini"})) == -32001)
        expect("relative path in mesh refused",
               error_code(lambda: pai.call("scene.set_component", id=box["id"], component="meshRenderer", value={"mesh": "../../secret.obj"})) == -32001)
        expect("script refused",
               error_code(lambda: pai.call("scene.set_component", id=box["id"], component="script", value={"source": "os.execute('x')"})) == -32001)
        r = pai.send_raw('{"jsonrpc":"2.0","id":99,"method":"scene.set_transform","params":{"id":"%s","position":[NaN,0,0]}}' % box["id"])
        expect("non-finite position refused (not JSON)", r.get("error", {}).get("code") == -32700, r)
        expect("huge position refused", error_code(lambda: pai.call("scene.set_transform", id=box["id"], position=[1e9, 0, 0])) == -32602)
        child = pai.call("scene.create", name="Child", parent=box["id"])
        expect("create with parent", child["parent"] == box["id"], child)
        expect("cycle refused", error_code(lambda: pai.call("scene.set_parent", id=box["id"], parent=child["id"])) == -32602)
        expect("save without file refused", error_code(lambda: pai.call("scene.save")) == -32002)

        pai.call("sim.step", frames=30)
        wait_steps(pai)
        expect("sim.step enters play, then pauses", pai.call("sim.status")["mode"] == "play" and pai.call("sim.status")["state"] == "Paused")
        y = pai.call("sim.metrics", ids=[box["id"]])["entities"][0]["position"][1]
        expect("simulation ran (box fell)", y < 5.0, y)
        expect("edit refused while playing", error_code(lambda: pai.call("scene.rename", id=box["id"], newName="X")) == -32002)
        expect("too many frames refused", error_code(lambda: pai.call("sim.step", frames=10**6)) == -32004)
        pai.call("sim.stop")
        expect("stop restores the edit state", pai.call("scene.get", id=box["id"])["transform"]["position"] == [1, 5, 2])
        expect("back in edit mode", pai.call("sim.status")["mode"] == "edit")
        pai.call("scene.delete", id=box["id"])
        expect("delete (with children)", pai.call("scene.list")["total"] == before)
        pai.call("history.undo")
        expect("undo delete restores both", pai.call("scene.list")["total"] == before + 2)

    # --- CAD parts
    with PaiClient(exe, write=True) as pai:
        box_doc = {"format": "phisiks-cad", "version": 1, "unit": "mm", "parameters": [{"name": "w", "expression": "30"}],
                   "features": [{"id": 1, "type": "box", "name": "Block", "values": {"x": "w", "y": "20", "z": "10"}, "position": [0, 0, 0], "op": "new"}],
                   "nextId": 2}
        part = pai.call("cad.create", name="Block", document=box_doc)
        kernel = part["kernel"] != "none"
        expect("cad.create (parametric part)", len(part["id"]) == 16 and part["unit"] == "mm" and len(part["features"]) == 1, part)
        if kernel:
            expect("cad.create builds the solid", part["ok"] and abs(part["volume"] - 6000.0) < 1e-6 and part["valid"], part)
        updated = pai.call("cad.set_parameter", id=part["id"], parameter="w", expression="45")
        if kernel:
            expect("cad.set_parameter rebuilds", abs(updated["volume"] - 9000.0) < 1e-6, updated)
        expect("cad.set_parameter is one undo step", pai.call("history.get")["undoLabel"].startswith("Edit CAD part"), pai.call("history.get"))
        expect("invalid parameter expression refused", error_code(lambda: pai.call("cad.set_parameter", id=part["id"], parameter="w", expression="nope +")) == -32602)
        got = pai.call("cad.get", id=part["id"], document=True)
        expect("the refused expression was not kept", any(p["name"] == "w" and p["expression"] == "45" for p in got["parameters"]), got["parameters"])
        expect("cad.get returns the document", got["document"].get("format") == "phisiks-cad")
        expect("invalid document refused", error_code(lambda: pai.call("cad.set_document", id=part["id"], document={"format": "x"})) == -32602)
        expect("future document version refused", error_code(lambda: pai.call("cad.set_document", id=part["id"], document={"format": "phisiks-cad", "version": 99})) == -32602)
        expect("unknown export format refused", error_code(lambda: pai.call("cad.export", id=part["id"], format="../../evil")) == -32602)
        expect("cad.get of a non-CAD entity", error_code(lambda: pai.call("cad.get", name="Sun")) == -32003)
        pai.call("history.undo")
        pai.call("history.undo")
        expect("undo removes the part", error_code(lambda: pai.call("cad.get", id=part["id"])) == -32003)

    print(f"{'ALL PASS' if not failures else str(len(failures)) + ' FAILED'}")
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--exe", required=True, help="path to the Phisiks editor executable")
    parser.add_argument("--check", action="store_true", help="run the protocol / permission checks")
    parser.add_argument("--gl41", action="store_true", help="start the editor on the OpenGL 4.1 tier")
    args = parser.parse_args()
    if args.gl41:
        PaiClient.default_args = ("--gl41",)
    if not os.path.isfile(args.exe):
        sys.exit(f"no editor at {args.exe}")
    sys.exit(check(args.exe) if args.check else demo(args.exe) or 0)


if __name__ == "__main__":
    main()
