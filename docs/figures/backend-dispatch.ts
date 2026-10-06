// Backend dispatch of docs/backends/index.md, drawn from the code at the evidence anchors: the
// CLI's --backend and --no_<backend> options (cli_parse.cpp), the model's features registered with
// vmaf_use_features_from_model, the registry lookup by feature name and backend flag, the twin
// lookup through provided_features (vmaf_get_feature_extractor_twin, ADR-1359) with its geometry
// verdict (vmaf_feature_backend_twin), and the CPU's runtime SIMD selection (vmaf_init_cpu).
import type { PraetorFigure } from '../../tools/figures/types.ts';

export default {
  title: 'Backend dispatch: from a feature to the code that computes it',
  alt: 'A requested feature runs on the device twin of the chosen backend when one fits, else on the CPU with its best SIMD path.',
  evidence: [
    'core/tools/cli_parse.cpp:ARG_BACKEND',
    'core/tools/cli_parse.cpp:ARG_NO_CUDA',
    'core/src/libvmaf.c:vmaf_engine_use_features_from_model',
    'core/src/libvmaf.c:vmaf_use_feature',
    'core/src/feature/feature_extractor.cpp:vmaf_get_feature_extractor_by_feature_name',
    'core/src/feature/feature_extractor.cpp:vmaf_get_feature_extractor_twin',
    'core/src/libvmaf.c:vmaf_engine_feature_backend_twin',
    'core/src/feature/feature_extractor.h:VMAF_FEATURE_EXTRACTOR_METAL',
    'core/src/libvmaf.c:vmaf_init_cpu',
  ],
  describe: [
    'Disabled backends (--no_cuda, --no_sycl, ...) leave the candidate list first.',
    'With --backend NAME a missing or failing backend is an error, never a silent fall back to the CPU.',
  ],
  props: {
    layout: {
      gap: 44,
      children: [
        {
          direction: 'column',
          gap: 30,
          children: [
            { id: 'options', label: 'Options', sub: '--backend, --model, --feature', width: 270 },
            { id: 'features', label: 'Features to compute', sub: 'named by the model or the CLI', width: 270 },
          ],
        },
        {
          direction: 'column',
          gap: 30,
          children: [
            { id: 'registry', label: 'Extractor registry', sub: 'by feature name and backend flag', width: 250 },
            { id: 'twin', label: 'Device twin?', sub: 'provided_features, geometry', shape: 'decision', width: 200 },
          ],
        },
        {
          id: 'paths',
          label: 'Runs on',
          direction: 'column',
          gap: 14,
          children: [
            { id: 'gpu', label: 'GPU twin', sub: 'CUDA, SYCL, HIP or Metal', width: 230 },
            { id: 'cpu', label: 'CPU extractor', sub: 'AVX-512, AVX2, NEON, SVE2 or scalar C', width: 230 },
          ],
        },
      ],
    },
    edges: [
      { from: 'options', to: 'features' },
      { from: 'features', to: 'registry', label: 'vmaf_use_feature' },
      { from: 'registry', to: 'twin' },
      { from: 'twin', to: 'gpu', label: 'twin fits' },
      { from: 'twin', to: 'cpu', label: 'no twin', around: 'below' },
    ],
    steps: [
      {
        label: 'GPU twin',
        caption: 'With a GPU backend compiled in and allowed, a feature with a twin runs on the device.',
        flow: [
          { edges: 'options->features', say: 'The model names its features; --backend cuda allows CUDA.' },
          { edges: 'features->registry', say: 'Each feature is looked up in the registry.' },
          { edges: 'registry->twin', say: 'The CPU extractor names a CUDA twin through provided_features.' },
          { edges: 'twin->gpu', say: 'The twin accepts the frame geometry and options, so it runs.' },
        ],
      },
      {
        label: 'CPU',
        caption: 'Without a fitting twin, the CPU extractor runs its best SIMD path.',
        flow: [
          { edges: 'options->features', say: '--backend cpu, or no GPU backend compiled in.' },
          { edges: 'features->registry', say: 'The feature is looked up.' },
          { edges: 'registry->twin', say: 'No twin is eligible.' },
          { edges: 'twin->cpu', say: 'The CPU extractor runs; vmaf_init_cpu picked AVX-512, AVX2, NEON or scalar C.' },
        ],
      },
    ],
  },
} satisfies PraetorFigure;
