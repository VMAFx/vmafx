// The tiny-AI pipeline of docs/ai/overview.md, drawn from the code at the evidence anchors:
// training and ONNX export in ai/ (export_to_onnx), the committed models and their registry in
// model/tiny/, the one ONNX Runtime entry point in core/src/dnn/ (vmaf_use_tiny_model), and the
// two surfaces that call it: the vmaf CLI (--tiny-model) and the FFmpeg patches (tiny_model,
// vf_vmafx_pre).
import type { PraetorFigure } from '../../tools/figures/types.ts';

export default {
  title: 'Tiny-AI pipeline: from training to the runtime',
  alt: 'Models are trained and exported to ONNX in ai/, committed under model/tiny/ and run by one runtime in core/src/dnn/.',
  evidence: [
    'ai/src/vmaf_train/cli.py:export_cmd',
    'ai/src/vmaf_train/models/exports.py:export_to_onnx',
    'model/tiny/registry.json:models',
    'core/src/dnn/dnn_attach_api.c:vmaf_use_tiny_model',
    'core/include/libvmaf/dnn.h:vmaf_use_tiny_model',
    'core/tools/cli_parse.cpp:ARG_TINY_MODEL',
    'ffmpeg-patches/src/vf_vmafx.c:tiny_model',
    'ffmpeg-patches/src/vf_vmafx_pre.c:vmafx_pre',
  ],
  describe: [
    'Training depends on PyTorch and Lightning; the runtime depends only on ONNX Runtime.',
    'The boundary between them is the .onnx file and its sidecar JSON on disk.',
  ],
  props: {
    layout: {
      direction: 'column',
      gap: 36,
      children: [
        { id: 'train', label: 'ai/', sub: 'train, export, register (PyTorch, Lightning)', width: 300 },
        { id: 'models', label: 'model/tiny/', sub: '.onnx + sidecar .json, registry.json', shape: 'store', width: 300 },
        { id: 'runtime', label: 'core/src/dnn/', sub: 'vmaf_use_tiny_model(), ONNX Runtime', width: 300 },
        {
          id: 'surfaces',
          label: 'Surfaces',
          direction: 'row',
          gap: 28,
          children: [
            { id: 'cli', label: 'vmaf CLI', sub: '--tiny-model' },
            { id: 'ffmpeg', label: 'FFmpeg', sub: 'vf_vmafx, vf_vmafx_pre' },
          ],
        },
      ],
    },
    edges: [
      { from: 'train', to: 'models', label: 'export_to_onnx' },
      { from: 'models', to: 'runtime', label: 'load' },
      { from: 'runtime', to: 'cli' },
      { from: 'runtime', to: 'ffmpeg' },
    ],
    steps: [
      {
        label: 'Train and ship',
        caption: 'A model leaves Python as an ONNX file and is scored by the C runtime.',
        flow: [
          { edges: 'train->models', say: 'ai/ trains the model and exports it to ONNX with its sidecar.' },
          { edges: 'models->runtime', say: 'vmaf_use_tiny_model() opens the committed model through ONNX Runtime.' },
          { edges: ['runtime->cli', 'runtime->ffmpeg'], say: 'The CLI and the FFmpeg filters share that one runtime.' },
        ],
      },
    ],
  },
} satisfies PraetorFigure;
