"""Run the SceneBaseSample MGPU benchmark and plot its per-frame timings."""

import argparse
import csv
from datetime import datetime
import math
import os
from pathlib import Path
import statistics
import subprocess


REPO = Path(__file__).resolve().parents[3]
DEFAULT_EXE = REPO / "build/win_vs2022_x64_mgpu/bin/Release/SceneBaseSample.exe"
COLORS = ("#205493", "#a34428")
SCENES = {"jungle": "Jungle Ruins", "default": "Стандартная сцена"}


def read_capture(path):
    phases = [[], [], [], []]
    gpu_counts = [0, 0, 0, 0]
    scene = None
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        for row in reader:
            try:
                phase = int(row["phase"])
                gpu_count = int(row["gpu_count"])
                frame_index = int(row["frame"])
                elapsed = float(row["elapsed_s"])
                frame_time = float(row["frame_time_ms"])
                cascades = int(row["cascades_from_second_gpu"])
                omni_shadows = int(row["omni_shadows_from_second_gpu"])
                atlas_age = int(row["cascade_atlas_age_frames"])
                overflow = int(row["capture_overflow"])
                row_scene = row["scene"]
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError(f"Malformed capture at CSV line {reader.line_num}") from error

            scene = scene or row_scene
            if row_scene not in SCENES or row_scene != scene:
                raise ValueError(f"Unexpected scene at CSV line {reader.line_num}")
            if not 1 <= phase <= len(phases):
                raise ValueError(f"Unexpected phase at CSV line {reader.line_num}")
            samples = phases[phase - 1]
            gpu_counts[phase - 1] = gpu_counts[phase - 1] or gpu_count
            if gpu_count not in (1, 2) or gpu_count != gpu_counts[phase - 1] or frame_index != len(samples) + 1:
                raise ValueError(f"Unexpected GPU count or frame index at CSV line {reader.line_num}")
            if overflow or cascades != gpu_count - 1 or omni_shadows not in (0, 1) or (gpu_count == 1 and omni_shadows):
                raise ValueError(f"Capture overflow or unexpected shadow atlas source in phase {phase}, frame {frame_index}")
            previous_elapsed = samples[-1][0] if samples else 0.0
            if (not math.isfinite(frame_time) or not math.isfinite(elapsed) or frame_time <= 0
                    or not math.isclose(elapsed - previous_elapsed, frame_time / 1000, abs_tol=1.1e-6)
                    or atlas_age < 0):
                raise ValueError(f"Invalid timing or atlas age at CSV line {reader.line_num}")
            samples.append((elapsed, frame_time, atlas_age, omni_shadows))

    for phase, samples in enumerate(phases, 1):
        if len(samples) < 2 or samples[-1][0] < 15.0:
            raise ValueError(f"Incomplete capture in phase {phase}")
    if any(set(gpu_counts[index:index + 2]) != {1, 2} for index in (0, 2)):
        raise ValueError("Expected one phase per GPU mode in each pair")
    return scene, list(zip(gpu_counts, phases))


def percentile(sorted_values, fraction):
    index = fraction * (len(sorted_values) - 1)
    lower = int(index)
    weight = index - lower
    return sorted_values[lower] * (1 - weight) + sorted_values[min(lower + 1, len(sorted_values) - 1)] * weight


def summarize(samples):
    frame_times = sorted(frame_time for _, frame_time, _, _ in samples)
    mean = statistics.mean(frame_times)
    return mean, statistics.median(frame_times), percentile(frame_times, 0.95), percentile(frame_times, 0.99), 1000 / mean


def write_chart(path, pairs, scene):
    width, height = 1080, 630
    plot_left, plot_width, plot_height = 82, 953, 195
    time_limit = math.ceil(max(samples[-1][0] for pair in pairs for samples in pair))
    max_frame_time = max(sample[1] for pair in pairs for samples in pair for sample in samples)
    scale = 10 ** math.floor(math.log10(max_frame_time / 5))
    grid_step = next(step * scale for step in (1, 2, 5, 10) if step * scale >= max_frame_time / 5)
    grid_rows = math.ceil(max_frame_time / grid_step)
    frame_time_limit = grid_step * grid_rows

    svg = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" role="img">',
        f"<title>Время кадра · {SCENES[scene]}</title>",
        '<rect width="100%" height="100%" fill="#fff"/>',
        '<g font-family="Segoe UI, Arial, sans-serif" fill="#213043" font-size="13">',
        f'<text x="32" y="42" font-size="25" font-weight="600">Время кадра · {SCENES[scene]}</text>',
    ]
    for index, color in enumerate(COLORS):
        x = 830 + index * 98
        svg.append(f'<path d="M{x} 35h20" stroke="{color}" stroke-width="3"/>')
        svg.append(f'<text x="{x + 26}" y="40">{index + 1} GPU</text>')

    for pair_index, pair in enumerate(pairs):
        top = 94 + pair_index * 260
        bottom = top + plot_height
        svg.append(f'<text x="{plot_left}" y="{top - 14}" font-size="15" font-weight="600">Прогон {pair_index + 1}</text>')
        svg.append(f'<text x="34" y="{top + 4}">мс</text>')
        for tick in range(grid_rows + 1):
            value = tick * grid_step
            y = bottom - value / frame_time_limit * plot_height
            svg.append(f'<path d="M{plot_left} {y:.2f}h{plot_width}" stroke="#dce3ea"/>')
            svg.append(f'<text x="{plot_left - 9}" y="{y + 4:.2f}" text-anchor="end">{value:g}</text>')
        for seconds in range(0, time_limit + 1, 4):
            x = plot_left + plot_width * seconds / time_limit
            svg.append(f'<path d="M{x:.2f} {top}v{plot_height}" stroke="#dce3ea"/>')
            svg.append(f'<text x="{x:.2f}" y="{bottom + 21}" text-anchor="middle">{seconds}</text>')
        for color, samples in zip(COLORS, pair):
            points = " ".join(
                f"{plot_left + elapsed / time_limit * plot_width:.2f},{bottom - frame_time / frame_time_limit * plot_height:.2f}"
                for elapsed, frame_time, _, _ in samples
            )
            svg.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="1.15" opacity="0.78"/>')
    svg.append('<text x="558" y="615" text-anchor="middle">Время от начала замера, с</text>')
    svg.append("</g></svg>")
    path.write_text("\n".join(svg), encoding="utf-8")


def write_report(csv_path, expected_scene=None, metadata=()):
    scene, phases = read_capture(csv_path)
    if expected_scene and scene != expected_scene:
        raise ValueError(f"Expected scene {expected_scene}, received {scene}")

    pairs = [[samples for _, samples in sorted(phases[index:index + 2])] for index in (0, 2)]
    chart_path = csv_path.with_name("frame_times.svg")
    write_chart(chart_path, pairs, scene)
    pooled = [pairs[0][mode] + pairs[1][mode] for mode in range(2)]
    single, dual = (summarize(samples) for samples in pooled)
    relative_change = (dual[1] / single[1] - 1) * 100
    paired_medians = [[statistics.median(sample[1] for sample in samples) for samples in pair] for pair in pairs]
    faster = [dual_median < single_median for single_median, dual_median in paired_medians]
    if all(faster):
        verdict = "Две GPU снижают медиану в обоих прогонах."
    elif any(faster):
        verdict = "Две GPU снижают медиану в одном из двух прогонов."
    else:
        verdict = "Медиана времени кадра выше или равна в режиме двух GPU в обоих прогонах."
    report = [
        f"# Тени на двух GPU · {SCENES[scene]}",
        "",
        "При вращении направленного света каскады теней приходится регулярно пересчитывать. Проверяем, сокращается ли время кадра после переноса части этой работы на вторую GPU.",
        "",
        "Вторая GPU строит дальние каскады и, если в сцене есть подходящие источники, карты теней точечных источников.",
        "Основная GPU рисует сцену, ближние каскады и читает атлас со второй GPU.",
        "Время кадра измерено между последовательными завершениями вывода кадра на потоке рендера. Кадры прогрева исключены.",
        "",
        *metadata,
        "",
        "## Результаты",
        "",
        f"{verdict} По всем кадрам изменение медианы составило {relative_change:+.1f}%.",
        "",
        "| Режим | Кадров | Среднее, мс | Медиана, мс | p95, мс | p99, мс | Средний FPS |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for mode, samples in enumerate(pooled, 1):
        values = " | ".join(f"{value:.2f}" for value in summarize(samples))
        report.append(f"| {mode} GPU | {len(samples)} | {values} |")
    report.extend([
        "",
        "| Фаза | Режим | Кадров | Длительность, с | Среднее, мс | Медиана, мс | p95, мс |",
        "|---|---|---:|---:|---:|---:|---:|",
    ])
    for phase, (gpu_count, samples) in enumerate(phases, 1):
        mean, median, p95, _, _ = summarize(samples)
        report.append(f"| {phase} | {gpu_count} GPU | {len(samples)} | {samples[-1][0]:.2f} | {mean:.2f} | {median:.2f} | {p95:.2f} |")
    ages = sorted(age for _, _, age, _ in pooled[1])
    omni_frames = sum(omni for _, _, _, omni in pooled[1])
    report.extend([
        "",
        f"Возраст атласа дальних каскадов в фазах 2 GPU, в кадрах — медиана {statistics.median(ages):g}, p95 {percentile(ages, 0.95):g}.",
        f"Атлас точечных источников со второй GPU использовался в {omni_frames} из {len(ages)} кадров.",
        "",
        f"![Время кадра в двух прогонах]({chart_path.name})",
        "",
        f"[Покадровые данные]({csv_path.name})",
        "",
    ])
    report_path = csv_path.with_name("report.md")
    report_path.write_text("\n".join(report), encoding="utf-8")
    print(f"{SCENES[scene]}: 1 GPU median {single[1]:.2f} ms, p95 {single[2]:.2f} ms")
    print(f"{SCENES[scene]}: 2 GPUs median {dual[1]:.2f} ms, p95 {dual[2]:.2f} ms ({relative_change:+.1f}% median)")
    print(report_path)


def run_benchmark(args):
    executable = args.exe.resolve(strict=True)
    output_dir = args.output_dir or REPO / "build/mgpu_benchmark" / datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "frame_times.csv"
    if csv_path.exists():
        raise ValueError(f"Output already exists: {csv_path}")

    env = os.environ.copy()
    env.update({
        "NAU_SAMPLE_BENCHMARK": args.scene,
        "NAU_SAMPLE_BENCHMARK_CSV": str(csv_path),
        "NAU_SAMPLE_BENCHMARK_MOTION": "0",
        "NAU_SAMPLE_SUN_ROTATE": "0" if args.static_sun else "1",
        "NAU_SAMPLE_VSYNC": "0",
        "NAU_MGPU": "0",
        "NAU_MGPU_AUTO": "0",
    })
    print(f"Running {SCENES[args.scene]} with {executable}", flush=True)
    process = subprocess.run(
        [str(executable)], cwd=executable.parent, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", timeout=420,
    )
    engine_log = process.stdout.splitlines()
    log_lines = [line for line in engine_log if "benchmark:" in line or "mgpu settings:" in line]
    if process.returncode or not csv_path.is_file() or not any("benchmark: adapters " in line for line in log_lines):
        raise ValueError(f"Benchmark failed (exit {process.returncode})\n" + "\n".join((log_lines or engine_log)[-20:]))

    metadata = [f"- Направленный свет {'статичен' if args.static_sun else 'вращается со скоростью 20°/с'}."]
    for line in log_lines:
        if "benchmark: adapters " in line:
            metadata.append(f"- GPU сцены / GPU теней — {line.split('benchmark: adapters ', 1)[1]}.")
        if "benchmark: viewport " in line:
            metadata.append(f"- Разрешение — {line.split('benchmark: viewport ', 1)[1]}.")
        if "mgpu settings:" in line:
            settings = line.split("mgpu settings:", 1)[1].strip().split(", ", 2)[-1]
            metadata.append(f"- Параметры каскадов и передачи атласа — {settings}.")
    metadata.append("- VSync и автоматический выбор GPU выключены. В каждой фазе 8 с прогрева и около 16 с замера.")
    write_report(csv_path, args.scene, metadata)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="run the Release sample and generate the report")
    run.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    run.add_argument("--scene", choices=SCENES, default="jungle")
    run.add_argument("--static-sun", action="store_true")
    run.add_argument("--output-dir", type=Path)
    report = commands.add_parser("report", help="plot and summarize an existing benchmark CSV")
    report.add_argument("csv", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "run":
            run_benchmark(args)
        else:
            write_report(args.csv.resolve())
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()

