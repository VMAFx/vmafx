from pathlib import Path

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


import numpy as np

from vmaf.config import VmafConfig
from vmaf.core.executor import run_executors_in_parallel
from vmaf.core.nn_train_test_model import ToddNoiseClassifierTrainTestModel
from vmaf.core.raw_extractor import DisYUVRawVideoExtractor
from vmaf.routine import read_dataset
from vmaf.tools.misc import import_python_file


def _load_assets(seed, count):
    dataset_path = VmafConfig.resource_path("dataset", "BSDS500_noisy_dataset.py")
    dataset = import_python_file(dataset_path)
    assets = read_dataset(dataset)
    np.random.seed(seed)
    np.random.shuffle(assets)
    return assets[:count]


def _extract_raw_yuvs(assets, raw_video_h5py_file):
    print("======================== Extract raw YUVs ==============================")
    _, raw_yuvs = run_executors_in_parallel(
        DisYUVRawVideoExtractor,
        assets,
        fifo_mode=True,
        delete_workdir=True,
        parallelize=False,
        result_store=None,
        optional_dict=None,
        optional_dict2={"h5py_file": raw_video_h5py_file},
    )
    return raw_yuvs


def _train_and_evaluate(raw_yuvs, num_train, seed, n_epochs, patch_h5py_file):
    model = ToddNoiseClassifierTrainTestModel(
        param_dict={"seed": seed, "n_epochs": n_epochs},
        logger=None,
        optional_dict2={"h5py_file": patch_h5py_file},
    )
    print("============================ Train model ===============================")
    model.train(ToddNoiseClassifierTrainTestModel.get_xys_from_results(raw_yuvs[:num_train]))
    print("=========================== Evaluate model =============================")
    test_results = raw_yuvs[num_train:]
    xs = ToddNoiseClassifierTrainTestModel.get_xs_from_results(test_results)
    ys = ToddNoiseClassifierTrainTestModel.get_ys_from_results(test_results)
    return model.evaluate(xs, ys)


def main():
    num_train, num_test, n_epochs, seed = 500, 50, 30, 0
    assets = _load_assets(seed, num_train + num_test)
    raw_video_h5py_filepath = VmafConfig.workdir_path("rawvideo.hdf5")
    raw_video_h5py_file = DisYUVRawVideoExtractor.open_h5py_file(raw_video_h5py_filepath)
    patch_h5py_filepath = VmafConfig.workdir_path("patch.hdf5")
    patch_h5py_file = ToddNoiseClassifierTrainTestModel.open_h5py_file(patch_h5py_filepath)
    raw_yuvs = _extract_raw_yuvs(assets, raw_video_h5py_file)
    result = _train_and_evaluate(raw_yuvs, num_train, seed, n_epochs, patch_h5py_file)
    print("")
    print("f1 test %g, errorrate test %g" % (result["f1"], result["errorrate"]))
    DisYUVRawVideoExtractor.close_h5py_file(raw_video_h5py_file)
    ToddNoiseClassifierTrainTestModel.close_h5py_file(patch_h5py_file)
    Path(raw_video_h5py_filepath).unlink()
    Path(patch_h5py_filepath).unlink()
    print("Done.")


if __name__ == "__main__":
    main()
