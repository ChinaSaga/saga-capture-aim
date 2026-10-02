"""Compare benchmark detections and optional full-image YOLO ground truth.

Reports fixed-threshold metrics, not mAP: the benchmark uses conf=0.25,
NMS IoU=0.45 and maxDetections=99. Ground truth must not be used with crops.
"""
import argparse
import json
import math
from pathlib import Path
import struct


def read(prefix):
    names = Path(str(prefix) + '.manifest.txt').read_text(encoding='utf-8-sig').splitlines()
    data = Path(str(prefix) + '.detections.bin').read_bytes()
    offset, frames = 0, []
    for name in names:
        if offset + 4 > len(data):
            raise ValueError('Truncated frame count: ' + name)
        count, = struct.unpack_from('<I', data, offset)
        offset += 4
        if offset + count * 24 > len(data):
            raise ValueError('Truncated detections: ' + name)
        frame = [struct.unpack_from('<iiiiif', data, offset + i * 24) for i in range(count)]
        offset += count * 24
        frames.append(frame)
    if offset != len(data):
        raise ValueError('Unexpected trailing bytes')
    return names, frames


def iou(a, b):
    inter = max(0, min(a[0]+a[2], b[0]+b[2])-max(a[0], b[0])) * max(0, min(a[1]+a[3], b[1]+b[3])-max(a[1], b[1]))
    union = a[2]*a[3] + b[2]*b[3] - inter
    return inter/union if union > 0 else 0.0


def match(a, b, threshold):
    # Highest-IoU greedy correspondence; classes must agree. This comparison
    # describes output drift and does not establish ground-truth correctness.
    pairs = sorted(((iou(x, y), i, j) for i, x in enumerate(a) for j, y in enumerate(b)
                    if x[4] == y[4] and iou(x, y) >= threshold), reverse=True)
    used_a, used_b, result = set(), set(), []
    for overlap, i, j in pairs:
        if i not in used_a and j not in used_b:
            used_a.add(i)
            used_b.add(j)
            result.append((i, j, overlap))
    return result


def distribution(values):
    if not values:
        return {'count': 0}
    values = sorted(values)
    return {'count': len(values), 'min': values[0], 'mean': sum(values)/len(values),
            'p05': values[max(0, math.ceil(len(values)*.05)-1)],
            'p95': values[min(len(values)-1, math.ceil(len(values)*.95)-1)], 'max': values[-1]}


def compare(a, b, threshold):
    result = {'frames': len(a), 'exact_changed_frames': 0, 'count_changed_frames': 0,
              'class_count_changed_frames': 0, 'unmatched_frames': 0,
              'baseline_detections': sum(map(len, a)), 'candidate_detections': sum(map(len, b)),
              'unmatched_baseline': 0, 'unmatched_candidate': 0, 'match_iou_threshold': threshold}
    overlaps, coordinates, confidences = [], [], []
    for x, y in zip(a, b):
        result['exact_changed_frames'] += x != y
        result['count_changed_frames'] += len(x) != len(y)
        result['class_count_changed_frames'] += sorted(d[4] for d in x) != sorted(d[4] for d in y)
        pairs = match(x, y, threshold)
        ua, ub = len(x)-len(pairs), len(y)-len(pairs)
        result['unmatched_baseline'] += ua
        result['unmatched_candidate'] += ub
        result['unmatched_frames'] += bool(ua or ub)
        for i, j, overlap in pairs:
            overlaps.append(overlap)
            coordinates.append(max(abs(x[i][k]-y[j][k]) for k in range(4)))
            confidences.append(abs(x[i][5]-y[j][5]))
    result.update(matched_iou=distribution(overlaps), max_box_component_drift_pixels=distribution(coordinates),
                  absolute_confidence_drift=distribution(confidences))
    return result


def ground_truth(names, frames, labels):
    from PIL import Image
    tp = fp = fn = evaluated = missing = 0
    classes = {}
    for name, detections in zip(names, frames):
        label = labels / (Path(name).stem + '.txt')
        if not label.exists():
            missing += 1
            continue  # Missing annotations cannot safely be treated as negatives.
        with Image.open(name) as image:
            width, height = image.size
        truth = []
        for line in label.read_text(encoding='utf-8-sig').splitlines():
            if not line.strip():
                continue
            fields = line.split()
            if len(fields) != 5:
                raise ValueError('Expected YOLO detection labels: ' + str(label))
            c, cx, cy, w, h = map(float, fields)
            truth.append(((cx-w/2)*width, (cy-h/2)*height, w*width, h*height, int(c)))
        used = set()
        evaluated += 1
        # Standard fixed-confidence detection matching: predictions ordered by
        # score, each assigned the highest-IoU unclaimed truth of its class.
        for detection in sorted(detections, key=lambda d: d[5], reverse=True):
            stats = classes.setdefault(detection[4], [0, 0, 0])
            candidates = [(iou(detection, t), i) for i, t in enumerate(truth)
                          if i not in used and t[4] == detection[4]]
            best = max(candidates, default=(0, -1))
            if best[0] >= .5:
                used.add(best[1]); tp += 1; stats[0] += 1
            else:
                fp += 1; stats[1] += 1
        for i, t in enumerate(truth):
            if i not in used:
                fn += 1; classes.setdefault(t[4], [0, 0, 0])[2] += 1
    def metrics(t, p, n):
        return dict(tp=t, fp=p, fn=n, precision=t/(t+p) if t+p else None, recall=t/(t+n) if t+n else None)
    return dict(evaluated_frames=evaluated, missing_label_frames=missing, iou_threshold=.5,
                confidence_threshold=.25, max_detections=99, **metrics(tp, fp, fn),
                by_class={str(c): metrics(*v) for c, v in sorted(classes.items())})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--match-iou', type=float, default=.5)
    parser.add_argument('--labels', type=Path)
    parser.add_argument('--full-image', action='store_true', help='Required assertion when using ground truth')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.labels and not args.full_image:
        parser.error('--labels requires --full-image; uncropped labels are invalid for crop benchmarks')
    names, a = read(args.baseline)
    candidate_names, b = read(args.candidate)
    if names != candidate_names:
        raise ValueError('Manifests differ; compare identical sampled images in identical order')
    result = compare(a, b, args.match_iou)
    if args.labels:
        result['baseline_ground_truth'] = ground_truth(names, a, args.labels)
        result['candidate_ground_truth'] = ground_truth(names, b, args.labels)
    rendered = json.dumps(result, indent=2, ensure_ascii=False)
    print(rendered)
    if args.output:
        args.output.write_text(rendered + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
