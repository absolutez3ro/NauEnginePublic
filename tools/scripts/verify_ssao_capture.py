"""Validate actual Nau D3D12 captures and write images for local review (numpy, Pillow)."""
import argparse
import json
import re
from pathlib import Path

import numpy as np
from PIL import Image


def verify(prefix, mode, independent_normals=False):
    scene = Image.open(str(prefix) + "-scene.ppm")
    display_width, display_height = scene.size
    result_path = Path(str(prefix) + ".result.json")
    runtime = json.loads(result_path.read_text(encoding="utf-8-sig")) if result_path.exists() else {}
    width = runtime.get("aoWidth", display_width)
    height = runtime.get("aoHeight", display_height)
    captures = {}
    names = [("filtered", 1), ("depth", 1), ("normals", 4), ("history" if mode == "busy" else "raw", 1)]
    for name, components in names:
        values = np.fromfile(str(prefix) + "-" + name + ".f32", dtype="<f4")
        assert values.size == width * height * components, (name, values.size, width, height)
        assert np.isfinite(values).all(), name + " contains NaN/Inf"
        captures[name] = values.reshape(height, width, components).squeeze()
    if mode == "busy":
        captures["raw"] = captures["history"]
        assert np.array_equal(captures["history"], captures["filtered"]), "Busy frame did not preserve the last completed AO"
    raw, filtered, depth = (captures[name] for name in ["raw", "filtered", "depth"])
    presentation = np.asarray(scene, dtype=np.float32) / 255.0
    resolved_path = Path(str(prefix) + "-resolved.f32")
    resolved = np.fromfile(resolved_path, dtype="<f4").reshape(display_height, display_width) if resolved_path.exists() else filtered
    assert np.isfinite(resolved).all() and resolved.min() >= 0 and resolved.max() <= 1, "Invalid resolved AO"
    resolve_low_depth_path = Path(str(prefix) + "-resolve_low_depth.f32")
    history_depth_path = Path(str(prefix) + "-history_depth.f32")
    paired_depth = None
    if resolve_low_depth_path.exists():
        paired_depth = np.fromfile(resolve_low_depth_path, dtype="<f4").reshape(height, width)
        assert np.isfinite(paired_depth).all() and paired_depth.min() >= 0 and paired_depth.max() <= 1, "Invalid retained AO depth"
        source_depth = np.fromfile(history_depth_path, dtype="<f4").reshape(height, width) if history_depth_path.exists() else depth
        assert np.array_equal(paired_depth, source_depth), "Upsampling did not retain the depth paired with its AO map"
        if mode != "busy":
            assert np.array_equal(paired_depth, depth), "Retained depth differs from the submitted source frame"
        assert np.allclose(filtered[paired_depth == 0], 1.0, atol=1e-6), "History AO occludes its own source background"
    resolve_depth_path = Path(str(prefix) + "-resolve_depth.f32")
    if resolve_depth_path.exists():
        resolve_depth = np.fromfile(resolve_depth_path, dtype="<f4").reshape(display_height, display_width)
        assert np.allclose(resolved[resolve_depth == 0], 1.0, atol=1e-6), "Upsampling occludes the current background"
        source_depth_path = Path(str(prefix) + "-source_full_depth.f32")
        if source_depth_path.exists() and runtime.get("device") == "primary":
            source_depth = np.fromfile(source_depth_path, dtype="<f4").reshape(display_height, display_width)
            assert np.array_equal(source_depth, resolve_depth), "Upsampling lost the prepass depth snapshot"
    if mode in ["ao", "raw", "zero-radius"]:
        assert np.max(np.abs(presentation - resolved[..., None])) <= 2.0 / 255.0, "AO view does not display the resolved map"
    else:
        assert np.count_nonzero(presentation > 0.05) > width * height, "Scene presentation is almost black"
    assert depth.min() >= 0 and depth.max() <= 1, "Invalid reversed-Z depth"
    for name, values in [("raw", raw), ("filtered", filtered)]:
        assert values.min() >= -1e-6 and values.max() <= 1.000001, name + " outside [0,1]"
        if mode != "busy":
            assert np.allclose(values[depth == 0], 1.0, atol=1e-6), name + " occludes the background"
        if mode in ["off", "zero-radius", "switch"]:
            assert np.allclose(values, 1.0, atol=1e-6), "Neutral mode changed AO"
    if mode in ["lit", "ao", "raw", "busy"]:
        assert np.count_nonzero(raw < 0.95) > 10, "AO did not darken any geometry"
        assert np.count_nonzero(depth > 0) > 100, "Scene geometry missing"
    if mode == "raw":
        assert np.max(np.abs(raw - filtered)) <= 0.5 / 255.0 + 2e-6, "Disabled blur changed AO beyond R8 quantization"
    assert np.max(np.abs(filtered * 255.0 - np.round(filtered * 255.0))) <= 2e-5, "Output is not an R8 map"
    if mode in ["lit", "ao"]:
        assert np.count_nonzero(np.abs(raw - filtered) > 0.5 / 255.0 + 1e-4) > 10, "Blur did not change AO beyond output rounding"
    result = {
        "mode": mode, "width": width, "height": height,
        "display_width": display_width, "display_height": display_height,
        "resolved_occluded_pixels": int(np.count_nonzero(resolved < 0.95)),
        "geometry_pixels": int(np.count_nonzero(depth > 0)),
        "occluded_pixels": int(np.count_nonzero(filtered < 0.95)),
        "raw_min": float(raw.min()), "filtered_min": float(filtered.min()),
        "raw_mean": float(raw.mean()), "filtered_mean": float(filtered.mean()),
        "blur_changed_pixels": int(np.count_nonzero(np.abs(raw - filtered) > 0.5 / 255.0 + 1e-4)),
    }
    if paired_depth is not None:
        result["paired_depth_verified"] = True
        result["paired_depth_differs_from_captured_depth_pixels"] = int(np.count_nonzero(paired_depth != depth))
    if mode == "busy":
        # Reuse intentionally belongs to an older camera/geometry; current depth cannot validate its old background.
        result["cached_ao_over_current_background_pixels"] = int(np.count_nonzero((depth == 0) & (filtered < 1.0)))
    reference_path = Path(str(prefix) + "-reference.f32")
    if mode == "busy":
        assert runtime.get("reused", 0) > 0, "Busy did not reuse a completed map"
        log = Path(str(prefix) + ".stdout.log").read_text(encoding="utf-8-sig")
        reused = re.search(r"SSAO: reused capture frame=(\d+), source=(\d+)", log)
        assert reused and 0 < int(reused[2]) < int(reused[1]), "Cached AO did not come from an earlier completed frame"
        result["capture_frame"] = int(reused[1])
        result["source_frame"] = int(reused[2])
    if runtime.get("captureSourceFrame", 0) != runtime.get("captureDisplayFrame", 0):
        assert runtime["captureDisplayFrame"] >= runtime["captureSourceFrame"] > 0, "Pipeline displayed a future result"
        result["capture_frame"] = runtime["captureDisplayFrame"]
        result["source_frame"] = runtime["captureSourceFrame"]
    if mode in ["lit", "ao", "raw"] and reference_path.exists() and runtime.get("validateSecondary", True):
        reference = np.fromfile(reference_path, dtype="<f4").reshape(height, width)
        error = np.abs(reference - filtered)
        assert np.isfinite(reference).all(), "Reference contains NaN/Inf"
        # The reference is R32F before output conversion. Discount only the unavoidable half-step of R8 rounding;
        # keep the previous bounds on the remaining error, including its distribution and the worst pixel.
        excess = np.maximum(error - 0.5 / 255.0, 0.0)
        strict_tolerance = 0.02 if mode == "raw" else 0.01
        max_tolerance = (0.05 if mode == "raw" else 0.02) if independent_normals else strict_tolerance
        outlier_fraction = float(np.count_nonzero(error > strict_tolerance) / error.size)
        # Independently reconstructed normals can change a discrete SSAO sample at a surface boundary.
        # Keep the distribution bounds, cap absolute error, and allow strict-bound outliers in <=0.01% of pixels.
        assert error.max() <= max_tolerance and excess.mean() <= 0.00001 and np.quantile(excess, 0.999) <= 0.001, "GPU 1 disagrees with the GPU 0 result of the same frame beyond R8 rounding"
        assert not independent_normals or outlier_fraction <= 0.0001, "Independent normal reconstruction changed too many pixels beyond the strict bound"
        result["independent_normals"] = independent_normals
        result["secondary_reference_strict_outlier_pixels"] = int(np.count_nonzero(error > strict_tolerance))
        result["secondary_reference_strict_outlier_fraction"] = outlier_fraction
        result["secondary_reference_max_error"] = float(error.max())
        result["secondary_reference_mean_error"] = float(error.mean())
        result["secondary_reference_excess_mean_error"] = float(excess.mean())
        result["secondary_reference_excess_p999"] = float(np.quantile(excess, 0.999))
    for name in ["raw", "filtered"]:
        image = np.round(np.clip(captures[name], 0, 1) * 255).astype(np.uint8)
        Image.fromarray(image).save(str(prefix) + "-" + name + ".png")
    scene.save(str(prefix) + "-scene.png")
    Path(str(prefix) + "-verification.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--mode", choices=["off", "lit", "ao", "raw", "zero-radius", "switch", "busy"], required=True)
    parser.add_argument("--independent-normals", action="store_true", help="Allow bounded sparse outliers when each GPU reconstructs its own normals from depth")
    args = parser.parse_args()
    verify(args.prefix, args.mode, args.independent_normals)
