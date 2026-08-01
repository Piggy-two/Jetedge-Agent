from pathlib import Path
import shutil

from ultralytics import YOLO


def main() -> None:
    project_root = Path(__file__).resolve().parent.parent
    model_dir = project_root / "models"
    model_dir.mkdir(parents=True, exist_ok=True)

    print("Loading YOLO11s...")
    model = YOLO("yolo11s.pt")

    print("Exporting ONNX...")
    exported_path = Path(
        model.export(
            format="onnx",
            imgsz=(384, 640),
            batch=1,
            opset=12,
            simplify=True,
            dynamic=False,
        )
    )

    target_path = model_dir / "yolo11s.onnx"

    if exported_path.resolve() != target_path.resolve():
        if target_path.exists():
            target_path.unlink()
        shutil.move(str(exported_path), str(target_path))

    print(f"ONNX exported successfully: {target_path}")


if __name__ == "__main__":
    main()
