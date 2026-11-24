import json

from tickforge.cli import main


def test_quick_end_to_end(tmp_path):
    out, img = tmp_path / "res", tmp_path / "img"
    assert main(["all", "--quick", "--paths", "200", "--out", str(out), "--img", str(img)]) == 0
    study = json.loads((out / "study_quick.json").read_text())
    assert set(study["comparison"]) == {"symmetric", "glft", "glft+imbalance", "glft+lead"}
    assert len(list(img.glob("*.png"))) >= 5


def test_site_data(tmp_path):
    from pathlib import Path

    from tickforge.site import build

    results = Path(__file__).resolve().parents[2] / "results"
    landscape, summary = build(results, tmp_path)
    L = json.loads(landscape.read_text())
    assert set(L["runs"]) == {"symmetric", "glft+lead"}
    m = L["meta"]
    width = m["price_hi"] - m["price_lo"] + 1
    for r in L["runs"].values():
        assert len(r["depth"]) == m["samples"] * width
        assert len(r["fundamental"]) == m["samples"]
    S = json.loads(summary.read_text())
    assert set(S["comparison"]) == {"symmetric", "glft", "glft+imbalance", "glft+lead"}
