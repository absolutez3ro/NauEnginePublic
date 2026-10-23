"""Validate actual Nau D3D12 captures and write images for local review (numpy, Pillow)."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def verify(prefix, mode):
    scene = Image.open(str(prefix) + "-scene.ppm")
    width, height = scene.size
    captures = {}
    for name, components in [("raw", 1), ("filtered", 1), ("depth", 1), ("normals", 4)]:
        values = np.fromfile(str(prefix) + "-" + name + ".f32", dtype="<f4")
        assert values.size == width * height * components, (name, values.size, width, height)
        assert np.isfinite(values).all(), name + " contains NaN/Inf"
        captures[name] = values.reshape(height, width, components).squeeze()
    raw, filtered, depth = (captures[name] for name in ["raw", "filtered", "depth"])
    presentation = np.asarray(scene, dtype=np.float32) / 255.0
    if mode in ["ao", "raw", "zero-radius"]:
        assert np.max(np.abs(presentation - filtered[..., None])) <= 2.0 / 255.0, "AO view does not display the filtered map"
    else:
        assert np.count_nonzero(presentation > 0.05) > width * height, "Scene presentation is almost black"
    assert depth.min() >= 0 and depth.max() <= 1, "Invalid reversed-Z depth"
    for name, values in [("raw", raw), ("filtered", filtered)]:
        assert values.min() >= -1e-6 and values.max() <= 1.000001, name + " outside [0,1]"
        assert np.allclose(values[depth == 0], 1.0, atol=1e-6), name + " occludes the background"
        if mode in ["off", "zero-radius", "switch"]:
            assert np.allclose(values, 1.0, atol=1e-6), "Neutral mode changed AO"
    if mode in ["lit", "ao", "raw"]:
        assert np.count_nonzero(raw < 0.95) > 10, "AO did not darken any geometry"
        assert np.count_nonzero(depth > 0) > 100, "Scene geometry missing"
    if mode == "raw":
        assert np.allclose(raw, filtered, atol=2e-6), "Disabled blur changed AO"
    if mode in ["lit", "ao"]:
        assert np.count_nonzero(np.abs(raw - filtered) > 1e-4) > 10, "Blur did not execute"
    result = {
        "mode": mode, "width": width, "height": height,
        "geometry_pixels": int(np.count_nonzero(depth > 0)),
        "occluded_pixels": int(np.count_nonzero(filtered < 0.95)),
        "raw_min": float(raw.min()), "filtered_min": float(filtered.min()),
        "raw_mean": float(raw.mean()), "filtered_mean": float(filtered.mean()),
        "blur_changed_pixels": int(np.count_nonzero(np.abs(raw - filtered) > 1e-4)),
    }
    reference_path = Path(str(prefix) + "-reference.f32")
    if mode in ["lit", "ao", "raw"] and reference_path.exists():
        reference = np.fromfile(reference_path, dtype="<f4").reshape(height, width)
        error = np.abs(reference - filtered)
        assert np.isfinite(reference).all(), "Reference contains NaN/Inf"
        # Hardware and WARP can choose adjacent sample pixels near a projection boundary due to float rounding.
        # Bound both the worst pixel and the distribution so a stale frame or broken copy cannot pass by averaging.
        max_tolerance = 0.02 if mode == "raw" else 0.01
        assert error.max() <= max_tolerance and error.mean() <= 0.00001 and np.quantile(error, 0.999) <= 0.001, "GPU 1 disagrees with the GPU 0 result of the same frame"
        result["secondary_reference_max_error"] = float(error.max())
        result["secondary_reference_mean_error"] = float(error.mean())
    for name in ["raw", "filtered"]:
        image = np.round(np.clip(captures[name], 0, 1) * 255).astype(np.uint8)
        Image.fromarray(image).save(str(prefix) + "-" + name + ".png")
    scene.save(str(prefix) + "-scene.png")
    Path(str(prefix) + "-verification.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--mode", choices=["off", "lit", "ao", "raw", "zero-radius", "switch"], required=True)
    args = parser.parse_args()
    verify(args.prefix, args.mode)
