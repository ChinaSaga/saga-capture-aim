"""Train the existing 2 -> 64 -> 32 -> 20 network and export mouse.bin."""

import argparse
from pathlib import Path
import sys


def load_data(csv_path, pd, torch):
    if not csv_path.is_file() or csv_path.stat().st_size == 0:
        raise ValueError(f"没有采集数据：{csv_path}\n请先点击红球、蓝球完成记录。")
    data = pd.read_csv(csv_path, header=None, dtype=str, keep_default_na=False)
    if data.empty or data.shape[1] != 11:
        raise ValueError("数据格式错误：每行必须包含目标位移和 10 个轨迹点（共 11 列）。")

    def point(value):
        pair = value.split(',')
        if len(pair) != 2:
            raise ValueError(f"无效的坐标：{value!r}")
        return [int(pair[0]), int(pair[1])]

    inputs, labels = [], []
    for line, row in enumerate(data.itertuples(index=False, name=None), 1):
        try:
            inputs.append(point(row[0]))
            labels.append([point(value) for value in row[1:]])
        except ValueError as error:
            raise ValueError(f"第 {line} 行数据错误：{error}") from error
    inputs = torch.tensor(inputs, dtype=torch.float32).unsqueeze(1)
    labels = torch.tensor(labels, dtype=torch.float32)
    if not torch.isfinite(inputs).all() or not torch.isfinite(labels).all():
        raise ValueError("数据中包含超出范围的坐标。")
    return inputs, labels


def export_bin(model, output_path):
    # Same order and little-endian float32 layout as src/Mouse.cpp::mc_create.
    # Replace only a complete model; cancellation cannot truncate an old model.
    temporary = output_path.with_suffix('.bin.tmp')
    try:
        with temporary.open('wb') as output:
            for layer in (model.fc1, model.fc2, model.fc3):
                output.write(layer.weight.detach().cpu().numpy().astype('<f4').tobytes())
                output.write(layer.bias.detach().cpu().numpy().astype('<f4').tobytes())
        if temporary.stat().st_size != 11728:
            raise ValueError("模型大小不正确，未替换已有模型。")
        temporary.replace(output_path)
    finally:
        temporary.unlink(missing_ok=True)


def train(data_dir, epochs):
    try:
        import numpy  # PyTorch's binary export needs NumPy.
        import pandas as pd
        import torch
        from torch import nn, optim
        from torch.utils.data import DataLoader, TensorDataset
    except ImportError as error:
        raise RuntimeError(
            f"当前 Python 缺少训练依赖：{error}\n请在 CMD 中执行：\n"
            f'"{sys.executable}" -m pip install numpy pandas torch'
        ) from error

    class SimpleNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.fc1 = nn.Linear(2, 64)
            self.fc2 = nn.Linear(64, 32)
            self.fc3 = nn.Linear(32, 20)

        def forward(self, values):
            values = torch.flatten(values, start_dim=1)
            values = torch.relu(self.fc1(values))
            values = torch.relu(self.fc2(values))
            return self.fc3(values).view(-1, 10, 2)

    data_path = data_dir / '人手数据.txt'
    if not data_path.exists():
        data_path = data_dir / 'mouse_data.csv'
    inputs, labels = load_data(data_path, pd, torch)
    dataset = TensorDataset(inputs, labels)
    batches = DataLoader(dataset, batch_size=64, shuffle=True)
    model = SimpleNet()
    criterion = nn.MSELoss()
    optimizer = optim.Adam(model.parameters(), lr=0.001)
    scheduler = optim.lr_scheduler.StepLR(optimizer, step_size=50, gamma=0.9)
    print(f"Python：{sys.executable}", flush=True)
    print(f"数据目录：{data_dir}\n样本数：{len(dataset)}，训练轮数：{epochs}", flush=True)
    print("关闭本窗口或按 Ctrl+C 可中止训练；成功后请重启主程序。", flush=True)
    for epoch in range(epochs):
        total = 0.0
        for batch_inputs, batch_labels in batches:
            optimizer.zero_grad()
            loss = criterion(model(batch_inputs), batch_labels)
            if not torch.isfinite(loss):
                raise ValueError("训练损失出现无效数值，未替换已有模型。")
            loss.backward()
            optimizer.step()
            # Preserve the original script's per-batch learning-rate schedule.
            scheduler.step()
            total += loss.item() * len(batch_inputs)
        print(f"Epoch {epoch + 1}/{epochs}  loss={total / len(dataset):.6f}", flush=True)

    model.eval()
    with torch.no_grad():
        total = 0.0
        for batch_inputs, batch_labels in batches:
            total += criterion(model(batch_inputs), batch_labels).item() * len(batch_inputs)
        print(f"训练集复测 loss={total / len(dataset):.6f}（使用同一份采集数据）", flush=True)
    if not all(torch.isfinite(value).all() for value in model.parameters()):
        raise ValueError("模型参数出现无效数值，未替换已有模型。")
    output = data_dir / 'mouse.bin'
    export_bin(model, output)
    print(f"\n训练完成！模型已保存：{output}\n重启主程序后自动加载。可以关闭本窗口。", flush=True)


def main():
    parser = argparse.ArgumentParser(description="训练人手轨迹并导出 mouse.bin")
    parser.add_argument('--data-dir', type=Path, default=Path(__file__).resolve().parent,
                        help="人手数据.txt 和 mouse.bin 所在目录（兼容旧 mouse_data.csv）")
    parser.add_argument('--epochs', type=int, default=500, help="训练轮数，默认 500")
    args = parser.parse_args()
    if args.epochs < 1:
        parser.error('--epochs 必须大于 0')
    try:
        train(args.data_dir.resolve(), args.epochs)
    except KeyboardInterrupt:
        print("\n训练已中止，已有模型保持不变。", flush=True)
        return 130
    except Exception as error:
        print(f"\n训练失败：{error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
