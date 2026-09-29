"""Click the red ball, move naturally, then click the blue ball to save."""

import argparse
import csv
import math
from pathlib import Path
import random
import sys


def path_to_rows(path):
    if len(path) < 10:
        return []
    start_x, start_y = path[0]
    # Keep the original ten-point sampling and horizontal mirror augmentation.
    step = max(1, len(path) // 10)
    points = [(path[i][0] - start_x, -(path[i][1] - start_y))
              for i in list(range(0, len(path), step))[:10]]
    rows = []
    for flip_x in (1, -1):
        values = [f'{x * flip_x},{y}' for x, y in points]
        rows.append([values[-1]] + values)
    return rows


def save_to_csv(path, csv_path):
    rows = path_to_rows(path)
    if not rows:
        return False
    with csv_path.open('a', newline='', encoding='utf-8') as output:
        csv.writer(output).writerows(rows)
    return True


class Collector:
    def __init__(self, root, csv_path):
        import tkinter as tk
        self.root, self.csv_path = root, csv_path
        self.n, self.recording, self.mouse_path = 0, False, []
        self.ball_radius = 40
        root.title('人手数据记录')
        root.attributes('-fullscreen', True)
        root.configure(bg='black')
        self.label = tk.Label(root, text='本次记录：0', font=('Microsoft YaHei', 24),
                              bg='black', fg='white')
        self.label.pack(side='top', pady=20)
        self.canvas = tk.Canvas(root, bg='black', highlightthickness=0)
        self.canvas.pack(fill='both', expand=True)
        # On Windows fullscreen geometry arrives as window events. Processing
        # only idle tasks can still report a 1x1 canvas during initial startup.
        root.update()
        self.width, self.height = self.canvas.winfo_width(), self.canvas.winfo_height()
        self.center = (self.width // 2, self.height // 2)
        x, y = self.center
        self.canvas.create_oval(x - 40, y - 40, x + 40, y + 40,
                                fill='red', outline='white', width=3)
        self.blue_center = self.spawn_blue_ball()
        self.canvas.create_text(self.width // 2, 35, text='点击红球开始 → 移动到蓝球，再点击结束',
                                fill='white', font=('Microsoft YaHei', 24))
        self.canvas.create_text(self.width // 2, 80, text='按 Esc 或 Q 退出，已完成的记录自动保存',
                                fill='gray', font=('Microsoft YaHei', 18))
        self.canvas.bind('<Motion>', self.motion)
        self.canvas.bind('<Button-1>', self.click)
        root.bind('<Escape>', lambda event: root.destroy())
        root.bind('<q>', lambda event: root.destroy())
        root.bind('<Q>', lambda event: root.destroy())
        root.focus_force()

    def spawn_blue_ball(self):
        # Sample within the canvas (the counter above it is not canvas space).
        x, y = self.center
        max_x = max(1, self.width // 2 - 55)
        max_y = max(1, self.height // 2 - 110)
        min_distance = min(150, max(max_x, max_y))
        while True:
            dx, dy = random.randint(-max_x, max_x), random.randint(-max_y, max_y)
            if min_distance <= math.hypot(dx, dy) <= 400:
                break
        x, y = x + dx, y + dy
        self.canvas.delete('blueball')
        self.canvas.create_oval(x - 40, y - 40, x + 40, y + 40,
                                fill='blue', outline='cyan', width=3, tags='blueball')
        return x, y

    def motion(self, event):
        if self.recording:
            self.mouse_path.append((event.x, event.y))

    def click(self, event):
        position = (event.x, event.y)
        if math.dist(position, self.center) <= self.ball_radius + 10:
            self.recording = True
            self.mouse_path = [position]
            self.canvas.delete('status')
            self.canvas.create_text(self.center[0], self.center[1] - 100,
                                    text='RECORDING...', fill='lime',
                                    font=('Helvetica', 40), tags='status')
        elif self.recording and math.dist(position, self.blue_center) <= self.ball_radius + 10:
            self.recording = False
            self.canvas.delete('status')
            try:
                if save_to_csv(self.mouse_path, self.csv_path):
                    self.n += 1
                    self.label.config(text=f'本次记录：{self.n}')
            except OSError as error:
                from tkinter import messagebox
                messagebox.showerror('保存失败', f'无法保存到 {self.csv_path}\n{error}', parent=self.root)
            self.mouse_path = []
            # Update the hit-test position even when a short path was rejected.
            self.blue_center = self.spawn_blue_ball()


def main():
    parser = argparse.ArgumentParser(description='记录红球到蓝球的人手鼠标轨迹')
    parser.add_argument('--data-dir', type=Path,
                        default=Path(sys.executable if getattr(sys, 'frozen', False) else __file__).resolve().parent,
                        help='人手数据.txt 保存目录')
    args = parser.parse_args()
    try:
        import tkinter as tk
        data_dir = args.data_dir.resolve()
        data_dir.mkdir(parents=True, exist_ok=True)
        csv_path = data_dir / '人手数据.txt'
        legacy_path = data_dir / 'mouse_data.csv'
        if not csv_path.exists() and legacy_path.exists():
            legacy_path.rename(csv_path)
        # Check write access before the user spends time recording.
        with csv_path.open('a', encoding='utf-8'):
            pass
        print(f'人手数据保存位置：{csv_path}', flush=True)
        root = tk.Tk()
        Collector(root, csv_path)
        root.mainloop()
    except Exception as error:
        print(f'记录工具启动失败：{error}\n请使用包含 Tkinter 的 Python。', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
