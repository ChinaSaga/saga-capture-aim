"""Synthetic-data checks; never read or overwrite the user's recordings/models."""
import csv
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / '人手轨迹'


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


collector = load_script('collect_mouse_data')
trainer = load_script('train')


class TrajectoryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='人手测试 %PATH% & ')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.csv_path = self.directory / '人手数据.txt'

    def record(self):
        return collector.save_to_csv([(100 + i * 3, 100 + i) for i in range(25)], self.csv_path)

    def test_append_and_training_contract(self):
        import pandas as pd
        import torch
        self.assertTrue(self.record())
        self.assertTrue(self.record())
        inputs, labels = trainer.load_data(self.csv_path, pd, torch)
        self.assertEqual(tuple(inputs.shape), (4, 1, 2))
        self.assertEqual(tuple(labels.shape), (4, 10, 2))
        self.assertEqual(inputs[0, 0].tolist(), [54.0, -18.0])
        self.assertEqual(inputs[1, 0].tolist(), [-54.0, -18.0])
        self.assertEqual(labels[0, 0].tolist(), [0.0, 0.0])
        self.assertEqual(labels[0, -1].tolist(), inputs[0, 0].tolist())

    def test_short_path_resets_blue_hit_target_without_saving(self):
        instance = collector.Collector.__new__(collector.Collector)
        instance.center = (0, 0)
        instance.blue_center = (200, 0)
        instance.ball_radius = 40
        instance.recording = True
        instance.mouse_path = [(i, 0) for i in range(5)]
        instance.csv_path = self.csv_path
        instance.n = 0
        instance.canvas = Mock()
        instance.label = Mock()
        instance.spawn_blue_ball = Mock(return_value=(0, 200))
        instance.click(SimpleNamespace(x=200, y=0))
        self.assertEqual(instance.blue_center, (0, 200))
        self.assertFalse(instance.recording)
        self.assertEqual(instance.n, 0)
        self.assertFalse(self.csv_path.exists())
        instance.mouse_path = [(i, i) for i in range(12)]
        instance.recording = True
        instance.click(SimpleNamespace(x=0, y=200))
        self.assertEqual(instance.n, 1)
        self.assertTrue(self.csv_path.is_file())

    def test_cli_default_directory_and_binary_export(self):
        self.record()
        script = self.directory / 'train.py'
        shutil.copyfile(SCRIPTS / 'train.py', script)
        result = subprocess.run([sys.executable, str(script), '--epochs', '2'],
                                cwd=ROOT, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        model = self.directory / 'mouse.bin'
        self.assertEqual(model.stat().st_size, 11728)
        import numpy as np
        values = np.fromfile(model, dtype='<f4')
        self.assertTrue(np.isfinite(values).all())
        self.assertFalse(model.with_suffix('.bin.tmp').exists())

    def test_bad_or_missing_data_preserves_model(self):
        import pandas as pd
        import torch
        with self.assertRaises(ValueError):
            trainer.load_data(self.csv_path, pd, torch)
        self.csv_path.write_text('bad,data\n', encoding='utf-8')
        model = self.directory / 'mouse.bin'
        model.write_bytes(b'existing model must survive')
        before = model.read_bytes()
        result = subprocess.run([sys.executable, str(SCRIPTS / 'train.py'),
                                 '--data-dir', str(self.directory), '--epochs', '1'],
                                cwd=ROOT, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(model.read_bytes(), before)
        with self.csv_path.open('w', newline='', encoding='utf-8') as output:
            csv.writer(output).writerow(['1,2'] * 10 + ['bad coordinate'])
        with self.assertRaisesRegex(ValueError, '1'):
            trainer.load_data(self.csv_path, pd, torch)

    def test_export_layer_order(self):
        import numpy as np
        import torch
        from torch import nn
        model = SimpleNamespace(fc1=nn.Linear(2, 64), fc2=nn.Linear(64, 32), fc3=nn.Linear(32, 20))
        with torch.no_grad():
            for index, layer in enumerate((model.fc1, model.fc2, model.fc3)):
                layer.weight.fill_(index * 2 + 1)
                layer.bias.fill_(index * 2 + 2)
        output = self.directory / 'mouse.bin'
        trainer.export_bin(model, output)
        actual = np.fromfile(output, dtype='<f4')
        expected = np.concatenate([np.full(size, value, dtype='<f4') for value, size in
                                   enumerate((128, 64, 2048, 32, 640, 20), 1)])
        np.testing.assert_array_equal(actual, expected)

    @unittest.skipUnless(os.environ.get('SAGA_TEST_TK') == '1', 'Set SAGA_TEST_TK=1 for the fullscreen Tk smoke test')
    def test_real_tk_geometry_recording_and_escape(self):
        import tkinter as tk
        root = tk.Tk()
        app = collector.Collector(root, self.csv_path)
        failures = []

        def exercise():
            try:
                self.assertGreater(app.width, 500)
                self.assertGreater(app.height, 300)
                x, y = app.center
                bx, by = app.blue_center
                app.canvas.event_generate('<Button-1>', x=x, y=y)
                for i in range(1, 25):
                    app.canvas.event_generate('<Motion>', x=round(x + (bx - x) * i / 24),
                                              y=round(y + (by - y) * i / 24))
                app.canvas.event_generate('<Button-1>', x=bx, y=by)
                self.assertEqual(app.n, 1)
                self.assertFalse(app.recording)
                self.assertEqual(len(self.csv_path.read_text().splitlines()), 2)
                root.event_generate('<Escape>')
            except BaseException as error:
                failures.append(error)
                root.destroy()

        root.after(150, exercise)
        root.mainloop()
        if failures:
            raise failures[0]


if __name__ == '__main__':
    unittest.main(verbosity=2)
