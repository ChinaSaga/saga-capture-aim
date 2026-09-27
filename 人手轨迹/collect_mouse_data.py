import math
import random
import tkinter as tk
import csv

root = tk.Tk()
root.attributes('-fullscreen', True)
root.configure(bg='black')

# UI
label_n = tk.Label(root, text="n: 0", font=("Helvetica", 24), bg="black", fg="white")
label_n.pack(side="top", pady=20)

csv_file_path = "mouse_data.csv"

screen_width = root.winfo_screenwidth()
screen_height = root.winfo_screenheight()

ball_radius = 40  # 加大一点更好点
min_distance = 150  # 蓝球和红球最小距离，防止太近找不到

center_x = screen_width // 2
center_y = screen_height // 2

canvas = tk.Canvas(root, width=screen_width, height=screen_height, bg="black", highlightthickness=0)
canvas.pack(fill="both", expand=True)

n = 0
recording = False
mouse_path = []

# 固定的红球（起点）
ball1 = canvas.create_oval(
    center_x - ball_radius, center_y - ball_radius,
    center_x + ball_radius, center_y + ball_radius,
    fill="red", outline="white", width=3
)

# 生成一个合理的蓝球位置（确保距离够远且不贴边）
def spawn_blue_ball():
    while True:
        dx = random.randint(-screen_width//2 + 100, screen_width//2 - 100)
        dy = random.randint(-screen_height//2 + 100, screen_height//2 - 100)
        dist = math.hypot(dx, dy)
        if min_distance <= dist <= 400:  # 不太近也不太远
            x = center_x + dx
            y = center_y + dy
            # 删除旧蓝球（如果存在）
            canvas.delete("blueball")
            canvas.create_oval(
                x - ball_radius, y - ball_radius,
                x + ball_radius, y + ball_radius,
                fill="blue", outline="cyan", width=3, tags="blueball"
            )
            return x, y

ball2_pos = spawn_blue_ball()  # 初始蓝球

def distance(p1, p2):
    return math.hypot(p1[0] - p2[0], p1[1] - p2[1])

def motion(event):
    if recording:
        mouse_path.append((event.x, event.y))

def click(event):
    global recording, mouse_path, n, ball2_pos

    # 防止快速双击或其他误操作
    if canvas.find_withtag("blueball") == ():  # 蓝球不存在时不响应任何点击
        return

    mx, my = event.x, event.y
    red_center = (center_x, center_y)
    blue_center = ball2_pos

    # 点击红球 → 开始记录或重新记录（仅在未录制时生效）
    if distance((mx, my), red_center) <= ball_radius + 10:
      recording = True
      mouse_path = [(mx, my)]
      canvas.delete("status")
      canvas.create_text(center_x, center_y - 100,
                       text="RECORDING...",
                       fill="lime",
                       font=("Helvetica", 40),
                       tags="status")

    # 点击蓝球 → 结束记录
    elif recording and distance((mx, my), blue_center) <= ball_radius + 10:
        recording = False
        canvas.delete("status")

        # 防止轨迹太短（误触）
        if len(mouse_path) < 10:
            mouse_path = []
            spawn_blue_ball()  # 重新生成蓝球，等下一次有效操作
            return

        save_to_csv(mouse_path)
        n += 1
        label_n.config(text=f"n: {n}")

        mouse_path = []

        if n >= 99999999:
            root.destroy()
            return

        # 关键：重新生成蓝球
        ball2_pos = spawn_blue_ball()

def transform_path(x_rel, y_rel, flip_x, flip_y):
    if flip_x:
        x_rel = [-x for x in x_rel]
    if flip_y:
        y_rel = [-y for y in y_rel]
    return x_rel, y_rel
def save_to_csv(path):
    if len(path) < 2:
        return
    start_x, start_y = path[0]
    x_rel = [x - start_x for x, y in path]
    y_rel = [-(y - start_y) for x, y in path]
    step = max(1, len(path) // 10)
    indices = list(range(0, len(path), step))[:10]
    key_x = [x_rel[i] for i in indices]
    key_y = [y_rel[i] for i in indices]
    transforms = [(1, 1), (-1, 1)]   # ← 只改这一行
    rows = []
    for fx, fy in transforms:
        tx, ty = transform_path(key_x, key_y, fx == -1, fy == -1)
        row = [f"{tx[-1]},{ty[-1]}"] + \
              [f"{tx[i]},{ty[i]}" for i in range(len(tx))]
        rows.append(row)
    with open(csv_file_path, 'a', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerows(rows)

def key_press(event):
    if event.keysym in ["Escape", "q", "Q"]:
        root.destroy()

# 绑定事件
canvas.bind('<Motion>', motion)
canvas.bind('<Button-1>', click)
root.bind('<Key>', key_press)

# 开头提示
canvas.create_text(screen_width//2, 200, text="点击红色球开始 → 鼠标移动到蓝色球,再次点击结束", fill="white", font=("宋体", 32), tags="intro")
canvas.create_text(screen_width//2, 260, text="按 Esc 随时退出", fill="gray", font=("宋体", 20))

root.mainloop()