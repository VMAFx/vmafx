<!-- markdownlint-disable MD013 -->
# MATLAB Usage

## Prerequisites

Install and activate [MATLAB](https://www.mathworks.com/). Then set
`MATLAB_PATH` in [`compat/python-vmaf/config.py`](../../compat/python-vmaf/config.py) to
point at your MATLAB binary:

```python
MATLAB_PATH = "/path/to/matlab"
```

For example on macOS:

```python
MATLAB_PATH = "/Applications/MATLAB_R2017a.app/bin/matlab"
```

## Available Algorithms

The available algorithms are ST-MAD [1], ST-RRED [2], SpEED-QA [3] and BRISQUE [4].

Example usage for ST-MAD, ST-RRED and SpEED-QA with the `run_testing` script:

```bash
python -m vmaf.script.run_testing <quality_type> <dataset_file>
```

where `<quality_type>` is `STMAD` (ST-MAD), `STRRED` (ST-RRED), `SpEED_Matlab`
(SpEED-QA), or `STRREDOpt` for a computationally efficient ST-RRED variant
that produces numerically identical results.

Example usage for BRISQUE:

```bash
python -m vmaf.script.run_vmaf yuv_420p 1920 1080 \
    NFLX_dataset_public/ref/OldTownCross_25fps.yuv \
    NFLX_dataset_public/dis/OldTownCross_90_1080_4300.yuv \
    --model model/vmaf_brisque_all_v0.0rc.pkl
```

## References

[1] P. V. Vu, C. T. Vu, and D. M. Chandler, "A spatiotemporal mostapparent-distortion model for video quality assessment," IEEE Int’l Conf. Image Process., pp. 2505–2508, 2011.

[2] R. Soundararajan and A. C. Bovik, "Video quality assessment by reduced reference spatio-temporal entropic differencing," IEEE Trans. Circ. Syst. Video Technol., vol. 23, no. 4, pp. 684–694, Apr. 2013.

[3] C. G. Bampis, P. Gupta, R. Soundararajan, and A. C. Bovik, "SpEEDQA: Spatial efficient entropic differencing for image and video quality," IEEE Signal Process. Lett., vol. 24, no. 9, pp. 1333–1337, 2017.

[4] A. Mittal, A. K. Moorthy, and A. C. Bovik, "No-reference image quality assessment in the spatial domain," IEEE Trans. Image Process., vol. 21, no. 12, pp. 4695–4708, Dec. 2012.
