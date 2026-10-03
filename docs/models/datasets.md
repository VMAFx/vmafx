<!-- markdownlint-disable MD013 MD059 -->
# Datasets

Two sample datasets, each a Python dataset file plus a video collection, are
available for training and testing custom VMAF models. Point the Python
harness at the dataset file, and put the downloaded videos where the file's
`ref_dir` and `dis_dir` say.

| Dataset | Dataset file | Reference / distorted videos | Access |
| --- | --- | --- | --- |
| [Netflix Public Dataset](#netflix-public-dataset) | [`NFLX_dataset_public.py`](https://github.com/VMAFx/vmafx/blob/master/compat/python-vmaf/resource/dataset/NFLX_dataset_public.py) | 9 / 79 | Public download, access granted on request |
| [VQEG HD3 Dataset](#vqeg-hd3-dataset) | [`VQEGHD3_dataset.py`](https://github.com/VMAFx/vmafx/blob/master/compat/python-vmaf/resource/dataset/VQEGHD3_dataset.py) | 8 / 63 | CDVL account, then convert to YUV420P |

## Use a dataset

The dataset files are consumed by the Python harness:

- To validate a model on a dataset, see
  [Validate a Dataset](../usage/python.md#validate-a-dataset).
- To train a model on a dataset, see
  [Train a New Model](../usage/python.md#train-a-new-model).

## Netflix Public Dataset

The dataset is publicly available to the community for training, testing and
verification of results. The videos are in YUV420P format and can be
downloaded
[from this folder](https://drive.google.com/folderview?id=0B3YWNICYMBIweGdJbERlUG9zc0k&usp=sharing)
(access is granted on request).

### File naming

- Each file name has the form
  `{content name}_{expert score}_{height in pixels}_{bitrate in Kbps}.yuv`.
- For example, `BirdsInCage_85_720_1050.yuv` is a decompressed video of the
  content `BirdInCage`, with an expert opinion score of 85 out of 100,
  compressed at 720p and 1050 Kbps.
- The expert opinion score differs from the MOS offered in
  `NFLX_dataset_public.py`, which comes from a panel of non-expert subjects.

## VQEG HD3 Dataset

The dataset file contains the file names of VQEG (Video Quality Experts Group)
HD3 videos. The videos themselves are available from
[CDVL](http://www.cdvl.org/).

### Download the videos

1. Log in to CDVL.
2. Choose "Advanced Search".
3. Select "VQEG Subjective Tests" in the dataset dropdown and search for the
   keyword `vqeghd3`. The result is a single combined ZIP file of about 84 GB,
   "VQEG HDTV Test, vqeghd3", because the individual sequences are no longer
   available for download.
4. Convert the videos to YUV420P format.

### Selection

The dataset file includes `src01` to `src09` except `src04`, which overlaps
with the Netflix Public Dataset. It includes `hrc04`, `hrc07`, `hrc16`,
`hrc17`, `hrc18`, `hrc19`, `hrc20` and `hrc21`, which are the distortion types
most relevant to adaptive streaming.
