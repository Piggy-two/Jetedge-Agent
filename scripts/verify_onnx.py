from pathlib import Path
import hashlib
import platform

import numpy as np
import onnx
import onnxruntime as ort


def calculate_sha256(path: Path) -> str:
    digest = hashlib.sha256()

    with path.open("rb") as file:
        while chunk := file.read(1024 * 1024):
            digest.update(chunk)

    return digest.hexdigest()


def node_description(node) -> str:
    return (
        f"name={node.name}, "
        f"shape={node.shape}, "
        f"dtype={node.type}"
    )


def main() -> None:
    project_root = Path(__file__).resolve().parent.parent
    model_path = project_root / "models" / "yolo11s.onnx"
    info_path = project_root / "models" / "model_info.txt"

    if not model_path.exists():
        raise FileNotFoundError(f"Model does not exist: {model_path}")

    print("1. Loading ONNX model...")
    model = onnx.load(str(model_path))

    print("2. Running ONNX Checker...")
    onnx.checker.check_model(model)
    print("ONNX Checker: PASSED")

    print("3. Creating ONNX Runtime session...")
    session = ort.InferenceSession(
        str(model_path),
        providers=["CPUExecutionProvider"],
    )

    inputs = session.get_inputs()
    outputs = session.get_outputs()

    if len(inputs) != 1:
        raise RuntimeError(
            f"Expected one input node, but found {len(inputs)}"
        )

    input_node = inputs[0]

    print("4. Running random-input inference...")
    random_input = np.random.rand(
        1, 3, 384, 640
    ).astype(np.float32)

    inference_outputs = session.run(
        None,
        {input_node.name: random_input},
    )

    for index, output in enumerate(inference_outputs):
        if not np.all(np.isfinite(output)):
            raise RuntimeError(
                f"Output {index} contains NaN or Inf"
            )

        print(
            f"Output {index}: "
            f"shape={output.shape}, "
            f"dtype={output.dtype}"
        )

    sha256 = calculate_sha256(model_path)
    size_mb = model_path.stat().st_size / 1024 / 1024

    lines = [
        "YOLO11s ONNX Model Information",
        "=" * 50,
        "",
        "Model: YOLO11s",
        "Source: Ultralytics pretrained model",
        "Input size: 1x3x384x640",
        "Batch: 1",
        "ONNX opset: 12",
        "Dynamic shape: false",
        "Simplify: true",
        "",
        f"Python: {platform.python_version()}",
        f"ONNX: {onnx.__version__}",
        f"ONNX Runtime: {ort.__version__}",
        "",
        "Input nodes:",
    ]

    lines.extend(
        f"- {node_description(node)}"
        for node in inputs
    )

    lines.append("")
    lines.append("Output nodes:")

    lines.extend(
        f"- {node_description(node)}"
        for node in outputs
    )

    lines.extend(
        [
            "",
            f"Model size: {size_mb:.2f} MB",
            f"SHA256: {sha256}",
            "ONNX Checker: PASSED",
            "ONNX Runtime inference: PASSED",
            "NaN/Inf check: PASSED",
        ]
    )

    info_path.write_text(
        "\n".join(lines),
        encoding="utf-8",
    )

    print()
    print("Verification completed successfully.")
    print(f"SHA256: {sha256}")
    print(f"Report: {info_path}")


if __name__ == "__main__":
    main()
