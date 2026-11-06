"""Create an optional FP16 model without modifying the production model.

Dependencies may be installed into an isolated directory and supplied with
--dependency-dir; this script does not install packages globally.
"""
import argparse
import copy
import json
from pathlib import Path
import sys


def clear_internal_type_annotations(graph, onnx):
    removed = len(graph.value_info)
    del graph.value_info[:]
    for node in graph.node:
        for attribute in node.attribute:
            if attribute.type == onnx.AttributeProto.GRAPH:
                removed += clear_internal_type_annotations(attribute.g, onnx)
            elif attribute.type == onnx.AttributeProto.GRAPHS:
                for nested in attribute.graphs:
                    removed += clear_internal_type_annotations(nested, onnx)
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--dependency-dir', type=Path)
    parser.add_argument('--overwrite-output', action='store_true')
    parser.add_argument('--keep-fp32-prefix', action='append', default=[],
                        help='Keep nodes whose names start with this prefix in FP32; repeatable')
    args = parser.parse_args()
    source, target = args.input.resolve(), args.output.resolve()
    if source == target:
        parser.error('Input and output must differ; input overwrite is never permitted')
    if target.exists() and not args.overwrite_output:
        parser.error('Output already exists; use --overwrite-output to replace this optional artifact')
    if args.dependency_dir:
        sys.path.insert(0, str(args.dependency_dir.resolve()))
    import onnx
    from onnxconverter_common import float16

    model = onnx.load(str(source))
    onnx.checker.check_model(model)
    original_io = [(v.name, v.type.SerializeToString()) for v in (*model.graph.input, *model.graph.output)]
    metadata = copy.deepcopy(model.metadata_props)
    blocked = [node.name for node in model.graph.node
               if any(node.name.startswith(prefix) for prefix in args.keep_fp32_prefix)]
    if args.keep_fp32_prefix and not blocked:
        parser.error('No nodes matched --keep-fp32-prefix; check exported node names')
    converted = float16.convert_float_to_float16(model, keep_io_types=True, node_block_list=blocked)
    # Converter 1.16 can leave FLOAT annotations on newly generated Resize
    # cast outputs that actually hold FLOAT16. Internal value_info is optional:
    # remove it and let the runtime infer types from actual nodes/initializers.
    # Public I/O types and model metadata remain intact.
    removed = clear_internal_type_annotations(converted.graph, onnx)
    del converted.metadata_props[:]
    converted.metadata_props.extend(metadata)
    actual_io = [(v.name, v.type.SerializeToString()) for v in (*converted.graph.input, *converted.graph.output)]
    if actual_io != original_io:
        raise ValueError('Conversion changed public model I/O types or shapes')
    onnx.checker.check_model(converted, full_check=True)
    target.parent.mkdir(parents=True, exist_ok=True)
    onnx.save_model(converted, str(target))
    onnx.checker.check_model(str(target), full_check=True)
    report = dict(input=str(source), output=str(target), bytes=target.stat().st_size,
                  onnx_version=onnx.__version__, external_io_preserved=True,
                  metadata_properties=len(metadata), removed_internal_annotations=removed,
                  fp32_node_prefixes=args.keep_fp32_prefix, fp32_blocked_nodes=len(blocked),
                  nodes=len(converted.graph.node), checker='passed; full_check=True',
                  note='Optional candidate: validate changing-image output and accuracy before activation')
    print(json.dumps(report, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
