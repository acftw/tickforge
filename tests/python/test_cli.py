import json

from tickforge.cli import main


def test_quick_end_to_end(tmp_path):
    out, img = tmp_path / "res", tmp_path / "img"
    assert main(["all", "--quick", "--paths", "200", "--out", str(out), "--img", str(img)]) == 0
    study = json.loads((out / "study_quick.json").read_text())
    assert set(study["comparison"]) == {"symmetric", "glft", "glft+imbalance", "glft+lead"}
    assert len(list(img.glob("*.png"))) >= 5
