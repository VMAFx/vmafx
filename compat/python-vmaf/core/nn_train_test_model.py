import pickle
import sys
from contextlib import suppress
from pathlib import Path

import numpy as np
import scipy.stats
from sklearn.metrics import f1_score

from vmaf.tools.decorator import override

try:
    import tensorflow.compat.v1 as tf

    tf.disable_v2_behavior()
except ImportError:
    tf = None

from vmaf.core.h5py_mixin import H5pyMixin
from vmaf.core.train_test_model import ClassifierMixin, RawVideoTrainTestModelMixin, TrainTestModel
from vmaf.tools.misc import get_dir_without_last_slash
from vmaf.tools.safe_pickle import load_pickle
from vmaf.tools.sigproc import as_one_hot, create_hp_yuv_4channel, dstack_y_u_v

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


class NeuralNetTrainTestModel(
    RawVideoTrainTestModelMixin,
    TrainTestModel,
    # order affects whose _assert_dimension
    # gets called
    H5pyMixin,
):

    DEFAULT_N_EPOCHS = 30
    DEFAULT_LEARNING_RATE = 1e-3
    DEFAULT_PATCH_WIDTH = 50
    DEFAULT_PATCH_HEIGHT = 50
    DEFAULT_PATCHES_PER_FRAME = 1
    DEFAULT_BATCH_SIZE = 20
    DEFAULT_SEED = None

    @property
    def n_epochs(self):
        if self.param_dict is not None and "n_epochs" in self.param_dict:
            return self.param_dict["n_epochs"]
        return self.DEFAULT_N_EPOCHS

    @property
    def learning_rate(self):
        if self.param_dict is not None and "learning_rate" in self.param_dict:
            return self.param_dict["learning_rate"]
        return self.DEFAULT_LEARNING_RATE

    @property
    def patch_width(self):
        if self.param_dict is not None and "patch_width" in self.param_dict:
            return self.param_dict["patch_width"]
        return self.DEFAULT_PATCH_WIDTH

    @property
    def patch_height(self):
        if self.param_dict is not None and "patch_height" in self.param_dict:
            return self.param_dict["patch_height"]
        return self.DEFAULT_PATCH_HEIGHT

    @property
    def patches_per_frame(self):
        if self.param_dict is not None and "patches_per_frame" in self.param_dict:
            return self.param_dict["patches_per_frame"]
        return self.DEFAULT_PATCHES_PER_FRAME

    @property
    def batch_size(self):
        if self.param_dict is not None and "batch_size" in self.param_dict:
            return self.param_dict["batch_size"]
        return self.DEFAULT_BATCH_SIZE

    @property
    def seed(self):
        if self.param_dict is not None and "seed" in self.param_dict:
            return self.param_dict["seed"]
        return self.DEFAULT_SEED

    @property
    def checkpoints_dir(self):
        # if None, won't output checkpoints
        if self.optional_dict2 is not None and "checkpoints_dir" in self.optional_dict2:
            return self.optional_dict2["checkpoints_dir"]
        return None

    def _assert_args(self):
        super(NeuralNetTrainTestModel, self)._assert_args()
        self.assert_h5py_file()  # assert h5py_file in self.optional_dict2

    @staticmethod
    def _assert_xs(xs):
        # for now, force input xys or xs having 'dis_y', 'dis_u' and 'dis_v'
        assert "dis_y" in xs
        assert "dis_u" in xs
        assert "dis_v" in xs

    @override(TrainTestModel)
    def train(self, xys):
        self._assert_xs(xys)

        self.model_type = self.TYPE

        assert "label" in xys
        assert "content_id" in xys

        # this makes sure the order of features are normalized, and each
        # dimension of xys_2d is consistent with feature_names
        feature_names = sorted(xys.keys())
        feature_names.remove("label")
        feature_names.remove("content_id")
        self.feature_names = feature_names

        self.norm_type = "none"  # no conventional data normalization

        patches_cache, labels_cache = self._populate_patches_and_labels(feature_names, xys)

        model = self._train(patches_cache, labels_cache)
        self.model = model

    @override(TrainTestModel)
    def predict(self, xs):

        self._assert_xs(xs)
        self._assert_trained()
        for name in self.feature_names:
            assert name in xs

        feature_names = self.feature_names

        # loop through xs
        len_xs = len(xs[feature_names[0]])
        ys_label_pred = []
        for i in range(len_xs):

            sys.stdout.write("Evaluating test data: %d / %d\r" % (i, len_xs))
            sys.stdout.flush()

            # create single x
            x = {}
            for feature_name in feature_names:
                x[feature_name] = [xs[feature_name][i]]

            # extract patches
            patches_cache = self._populate_patches(feature_names, x)

            # predict
            y_label_pred = self._predict(self.model, patches_cache)
            ys_label_pred.append(y_label_pred)

        return {"ys_label_pred": ys_label_pred}

    def _create_patch_and_label_dataset(self, total_frames, overwrite=True):

        patches_dims = (
            total_frames * self.patches_per_frame,
            self.patch_height,
            self.patch_width,
            self.n_channels,
        )
        if overwrite:
            with suppress(KeyError):
                del self.h5py_file["patches"]
        patches_cache = self.h5py_file.create_dataset("patches", patches_dims, dtype="float")
        patches_cache.dims[0].label = "batch"
        patches_cache.dims[1].label = "height"
        patches_cache.dims[2].label = "width"
        patches_cache.dims[3].label = "channel"

        labels_dims = (total_frames * self.patches_per_frame,)
        if overwrite:
            with suppress(KeyError):
                del self.h5py_file["labels"]
        labels_cache = self.h5py_file.create_dataset("labels", labels_dims, dtype="uint8")

        return patches_cache, labels_cache

    def _populate_patches_and_labels(self, xkeys, xys, mode="train"):
        np.random.seed(self.seed)
        total_frames = self._get_total_frames(xys)
        patches_cache, labels_cache = self._create_patch_and_label_dataset(total_frames)
        assert "dis_y" in xkeys
        assert "dis_u" in xkeys
        assert "dis_v" in xkeys
        yss = xys["dis_y"]  # yss: Y * frames * videos
        uss = xys["dis_u"]
        vss = xys["dis_v"]
        labels = self._labels_for_mode(xys, yss, mode)
        assert len(yss) == len(uss) == len(vss) == len(labels)
        patch_idx = 0
        for ys, us, vs, label in zip(yss, uss, vss, labels, strict=False):  # iterate videos
            assert len(ys) == len(us) == len(vs)
            for y, u, v in zip(ys, us, vs, strict=False):  # iterate frames
                image = create_hp_yuv_4channel(dstack_y_u_v(y, u, v))
                patch_idx = self._write_frame_patches(
                    image, label, mode, patches_cache, labels_cache, patch_idx
                )
        return patches_cache, labels_cache

    @staticmethod
    def _labels_for_mode(xys, yss, mode):
        if mode == "train":
            assert "label" in xys
            return xys["label"]
        if mode == "test":
            return [None for _ in range(len(yss))]
        raise AssertionError()

    def _write_frame_patches(self, image, label, mode, patches_cache, labels_cache, patch_idx):
        height, width, _channels = image.shape
        adjusted_height = height - self.patch_height
        adjusted_width = width - self.patch_width
        rows, columns = np.meshgrid(
            np.arange(adjusted_height),
            np.arange(adjusted_width),
            sparse=False,
            indexing="ij",
        )
        order = np.random.permutation(adjusted_height * adjusted_width)
        positions = zip(rows.reshape(-1)[order], columns.reshape(-1)[order], strict=False)
        for patches_found, (row, column) in enumerate(positions, start=1):
            patches_cache[patch_idx] = image[
                row : row + self.patch_height,
                column : column + self.patch_width,
            ]
            if mode == "train":
                labels_cache[patch_idx] = label
            patch_idx += 1
            if patches_found >= self.patches_per_frame:
                break
        return patch_idx

    def _populate_patches(self, xkeys, xs):
        # reuse _populate_patches_and_labels to do the job
        patches_cache, _ = self._populate_patches_and_labels(xkeys, xys=xs, mode="test")
        return patches_cache

    def _get_total_frames(self, xys):
        yss = xys["dis_y"]  # yss
        return np.sum(list(map(len, yss)))

    @override(TrainTestModel)
    def to_file(self, filename, **more):

        self._assert_trained()

        # special handling for tensorflow: save .model differently
        model_dict_copy = self.model_dict.copy()
        model_dict_copy["model"] = None
        info_to_save = {"param_dict": self.param_dict, "model_dict": model_dict_copy}

        saver = tf.train.Saver()
        sess = self.model["sess"]
        saver.save(sess, filename + ".model")

        with Path(filename).open("wb") as file:
            pickle.dump(info_to_save, file)

    @classmethod
    @override(TrainTestModel)
    def from_file(cls, filename, logger=None, optional_dict2=None, **more):
        fmt = more.get("format", "pkl")
        assert fmt in ["pkl"], f"format must be pkl but got {fmt}"

        assert Path(filename).exists(), "File name {} does not exist.".format(filename)
        with Path(filename).open("rb") as file:
            info_loaded = load_pickle(file)

        train_test_model = cls(param_dict={}, logger=logger, optional_dict2=optional_dict2)
        train_test_model.param_dict = info_loaded["param_dict"]
        train_test_model.model_dict = info_loaded["model_dict"]

        # == special handling of tensorflow: load .model differently ==

        input_image_batch, _logits, _y, y_p, _W_conv0, _W_conv1, _loss, _train_step = (
            cls.create_tf_variables(train_test_model.param_dict)
        )

        saver = tf.train.Saver()
        sess = tf.Session()
        checkpoint_dir = get_dir_without_last_slash(filename)
        ckpt = tf.train.get_checkpoint_state(checkpoint_dir)
        if ckpt and ckpt.model_checkpoint_path:
            saver.restore(sess, ckpt.model_checkpoint_path)
        else:
            raise AssertionError()
        model = {
            "sess": sess,
            "y_p": y_p,
            "input_image_batch": input_image_batch,
        }
        train_test_model.model_dict["model"] = model

        return train_test_model

    @staticmethod
    @override(TrainTestModel)
    def delete(filename, **more):
        fmt = more.get("format", "pkl")
        assert fmt in ["pkl"], f"format must be pkl but got {fmt}"

        if Path(filename).exists():
            Path(filename).unlink()
        if Path(filename + ".model").exists():
            Path(filename + ".model").unlink()
        if Path(filename + ".model.meta").exists():
            Path(filename + ".model.meta").unlink()
        filedir = get_dir_without_last_slash(filename)
        if Path(filedir + "/checkpoint").exists():
            Path(filedir + "/checkpoint").unlink()

    @classmethod
    def reset(cls):
        super(NeuralNetTrainTestModel, cls).reset()

        # reset tensorflow to avoid any memory
        tf.reset_default_graph()


class ToddNoiseClassifierTrainTestModel(NeuralNetTrainTestModel, ClassifierMixin):

    TYPE = "TODDNOISECLASSIFIER"
    VERSION = "0.1"

    # override NeuralNetTrainTestModel.DEFAULT_PATCHES_PER_FRAME
    DEFAULT_PATCHES_PER_FRAME = 10

    n_channels = 4
    n_filters = 5
    fsize0 = 5
    fsize1 = 3

    @staticmethod
    def _partition_training_data(labels):
        indices = np.random.permutation(len(labels))
        midpoint = len(labels) // 2
        train_indices = indices[:midpoint]
        validate_indices = indices[midpoint:]
        positive = [index for index in train_indices if labels[index] == 1]
        negative = [index for index in train_indices if labels[index] == 0]
        return train_indices, validate_indices, positive, negative

    def _checkpoint(self, session, saver, epoch):
        if not self.checkpoints_dir:
            return saver
        if saver is None:
            saver = tf.train.Saver(max_to_keep=0)
        output_file = "%s/model_epoch_%d.ckpt" % (self.checkpoints_dir, epoch)
        print("Checkpointing -> %s" % (output_file,))
        saver.save(session, output_file)
        return saver

    def _train_balanced_batches(
        self, patches, labels, input_image_batch, y_, train_step, session, positive, negative
    ):
        half_batch = self.batch_size // 2
        iterations = min(len(positive) // half_batch, len(negative) // half_batch)
        for iteration in range(iterations):
            start = iteration * half_batch
            end = (iteration + 1) * half_batch
            positive_batch = np.sort(positive[start:end]).tolist()
            negative_batch = np.sort(negative[start:end]).tolist()
            x_batch = np.vstack((patches[positive_batch], patches[negative_batch]))
            y_batch = np.vstack(
                (as_one_hot(labels[positive_batch]), as_one_hot(labels[negative_batch]))
            )
            sys.stdout.write("Training: %d / %d\r" % (iteration, iterations))
            sys.stdout.flush()
            session.run(train_step, feed_dict={input_image_batch: x_batch, y_: y_batch})
        np.random.shuffle(positive)
        np.random.shuffle(negative)

    def _evaluate_epoch(
        self,
        patches,
        labels,
        input_image_batch,
        loss,
        session,
        train_indices,
        validate_indices,
        y_,
        y_p,
        epoch,
    ):
        print("")
        print("******************** EPOCH %d / %d ********************" % (epoch, self.n_epochs))
        train_loss, train_score = self._evaluate_on_patches(
            patches, labels, input_image_batch, loss, session, train_indices, y_, y_p, "train"
        )
        print("")
        validate_loss, validate_score = self._evaluate_on_patches(
            patches,
            labels,
            input_image_batch,
            loss,
            session,
            validate_indices,
            y_,
            y_p,
            "validate",
        )
        print("")
        print(
            "f1 train %g, f1 validate %g, loss train %g, loss validate %g"
            % (train_score, validate_score, train_loss, validate_loss)
        )

    def _train(self, patches, labels):
        assert len(patches) == len(labels)
        train_indices, validate_indices, positive, negative = self._partition_training_data(labels)
        input_image_batch, _logits, y_, y_p, _W_conv0, _W_conv1, loss, train_step = (
            self.create_tf_variables(self.param_dict)
        )
        init = tf.initialize_all_variables()
        sess = tf.Session()
        sess.run(init)
        saver = None
        for epoch in range(self.n_epochs):
            self._evaluate_epoch(
                patches,
                labels,
                input_image_batch,
                loss,
                sess,
                train_indices,
                validate_indices,
                y_,
                y_p,
                epoch,
            )
            saver = self._checkpoint(sess, saver, epoch)
            self._train_balanced_batches(
                patches, labels, input_image_batch, y_, train_step, sess, positive, negative
            )
            print("")
        return {
            "sess": sess,
            "y_p": y_p,
            "input_image_batch": input_image_batch,
        }

    def _evaluate_on_patches(
        self,
        patches_in,
        labels_in,
        input_image_batch,
        loss_in,
        sess,
        indices,
        y_,
        y_p,
        type_="train",
    ):
        ys_pred = []
        ys_true = []
        loss_cum = 0.0
        n_steps = len(indices) // self.batch_size
        for M in range(n_steps):
            sys.stdout.write(
                "Evaluating {type} data: {M} / {nstep}\r".format(type=type_, M=M, nstep=n_steps)
            )
            sys.stdout.flush()
            curr_indices = indices[M * self.batch_size : (M + 1) * self.batch_size].tolist()
            patches = [patches_in[idx] for idx in curr_indices]
            labels_ = [labels_in[idx] for idx in curr_indices]
            labels = as_one_hot(labels_)
            y_pred, loss = sess.run(
                [y_p, loss_in], feed_dict={input_image_batch: patches, y_: labels}
            )
            ys_pred = np.hstack((ys_pred, y_pred))
            ys_true = np.hstack((ys_true, labels_))
            loss_cum += loss
        loss_cum /= n_steps
        score = f1_score(ys_true, ys_pred)
        return loss_cum, score

    @classmethod
    def _predict(cls, model, patches):

        sess = model["sess"]
        y_p = model["y_p"]
        input_image_batch = model["input_image_batch"]

        ys_patche_pred = sess.run(y_p, feed_dict={input_image_batch: patches})

        # predict by majority voting on all patches
        return scipy.stats.mode(ys_patche_pred)[0][0]

    @classmethod
    def create_tf_variables(cls, param_dict):
        def weight_variable(shape, name="test", seed=None):
            return tf.Variable(tf.random_normal(shape, stddev=0.1, seed=seed), name=name)

        def conv2d(x, W):
            return tf.nn.conv2d(x, W, strides=[1, 1, 1, 1], padding="VALID")

        if param_dict is not None and "patch_width" in param_dict:
            patch_width = param_dict["patch_width"]
        else:
            patch_width = cls.DEFAULT_PATCH_WIDTH

        if param_dict is not None and "patch_height" in param_dict:
            patch_height = param_dict["patch_height"]
        else:
            patch_height = cls.DEFAULT_PATCH_HEIGHT

        if param_dict is not None and "learning_rate" in param_dict:
            learning_rate = param_dict["learning_rate"]
        else:
            learning_rate = cls.DEFAULT_LEARNING_RATE

        if param_dict is not None and "seed" in param_dict:
            seed = param_dict["seed"]
        else:
            seed = cls.DEFAULT_SEED

        tf.set_random_seed(seed)

        n_channels = cls.n_channels
        response_map_size_width = patch_width - (cls.fsize0 // 2) * 2
        response_map_size_width -= (cls.fsize1 // 2) * 2
        response_map_size_height = patch_height - (cls.fsize0 // 2) * 2
        response_map_size_height -= (cls.fsize1 // 2) * 2
        response_map_size = response_map_size_height * response_map_size_width
        input_image_batch = tf.placeholder(
            tf.float32, shape=[None, patch_height, patch_width, n_channels]
        )
        y_ = tf.placeholder(tf.float32, shape=[None, 2])
        W_conv0 = weight_variable([cls.fsize0, cls.fsize0, n_channels, cls.n_filters], "W_conv0")
        W_conv1 = weight_variable([cls.fsize1, cls.fsize1, cls.n_filters, 2], "W_conv1")

        # convolutional layer 1
        h_conv0 = conv2d(input_image_batch, W_conv0)
        h_conv0_elu = tf.nn.elu(h_conv0)

        # layer 2, which is just an output layer
        h_conv1 = conv2d(h_conv0_elu, W_conv1)
        h_conv1_elu = tf.nn.elu(h_conv1)
        h_conv1_elu_flat = tf.reshape(h_conv1_elu, [-1, response_map_size, 2])

        logits = tf.reduce_mean(h_conv1_elu_flat, 1)
        logits_norm = tf.nn.softmax(logits)

        y_p = tf.argmax(logits_norm, 1)
        loss = tf.reduce_mean(tf.nn.softmax_cross_entropy_with_logits(logits=logits, labels=y_))
        train_step = tf.train.AdamOptimizer(learning_rate).minimize(loss)

        return input_image_batch, logits, y_, y_p, W_conv0, W_conv1, loss, train_step
