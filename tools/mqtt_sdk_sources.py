"""Verify the vendored MQTT source and reconstruct its reviewed patch lineage.

Every upstream input remains pinned by UPSTREAM_SHA256.json. Modified and new
inputs have separate PATCHED_SHA256.json pins; reversing the complete ordered
patch series must recover every upstream byte. All reconstruction is temporary.
"""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "firmware-ESP32-LTE-M/components/espressif__mqtt"
UPSTREAM_CLIENT_SHA = "4b24720b34c2bd44b0857a5251f5392663225c618595229540b35f1529663a9a"
RESEND_CLIENT_SHA = "1a120957d6f8078a0cd27f4febac54389c5dce7f025069cad493e945d005f361"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_current(sdk=SDK):
    upstream = json.loads((sdk / "UPSTREAM_SHA256.json").read_text())
    patched = json.loads((sdk / "PATCHED_SHA256.json").read_text())
    assert upstream["mqtt_client.c"] == UPSTREAM_CLIENT_SHA
    for name, expected in {**upstream, **patched}.items():
        path = Path(name)
        assert not path.is_absolute() and ".." not in path.parts, name
        assert digest(sdk / name) == expected, f"Vendored MQTT input changed: {name}"
    return upstream, patched


def reconstruct(build, sdk=SDK):
    """Return upstream, resend-fixed and current MQTT source after verification."""
    upstream, patched = verify_current(sdk)
    patches = sorted(sdk.glob("[0-9][0-9][0-9][0-9]-*.patch"))
    assert patches and patches[0].name == "0001-stop-after-resend-abort.patch"
    original = build / "sdk-reconstructed"
    original.mkdir()
    for name in upstream.keys() | patched.keys():
        target = original / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(sdk / name, target)
    resend_source = None
    for patch in reversed(patches):
        if patch == patches[0]:
            resend_source = (original / "mqtt_client.c").read_text()
            assert hashlib.sha256(resend_source.encode()).hexdigest() == RESEND_CLIENT_SHA
        subprocess.run(["patch", "--batch", "--reverse", "-p1", "-i", str(patch)],
                       cwd=original, check=True, capture_output=True, text=True, timeout=30)
    for name, expected in upstream.items():
        assert digest(original / name) == expected, f"Patch lineage does not restore upstream: {name}"
    for name in patched.keys() - upstream.keys():
        assert not (original / name).exists(), f"Patch lineage does not account for new input: {name}"
    return (original / "mqtt_client.c").read_text(), resend_source, (sdk / "mqtt_client.c").read_text()
