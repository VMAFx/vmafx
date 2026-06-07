from __future__ import absolute_import

import os
import tempfile
import urllib.error
import urllib.request

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

PYTHON_ROOT = os.path.dirname(os.path.realpath(__file__))
ROOT = os.path.abspath(
    os.path.join(
        PYTHON_ROOT,
        "..",
        "..",
    )
)
# Fork change: the Python training/eval harness writes its dataset, model,
# encode, output, and per-run scratch trees under a workspace directory. Upstream
# keeps that at ROOT/workspace (repo root), which is noise for users who only
# consume libvmaf or the vmaf CLI. This fork moves it next to the code that
# actually uses it (see docs/architecture/workspace.md). Override with the
# VMAF_WORKSPACE env var if you need the harness to read/write elsewhere
# (useful for CI caches or read-only checkouts).
WORKSPACE = os.environ.get("VMAF_WORKSPACE", os.path.join(PYTHON_ROOT, "workspace"))
# Fork change: the training/eval harness reads example datasets, param files,
# model params, and tutorial images from a resource tree. Upstream keeps that at
# ROOT/resource (repo root). This fork moves it next to the code that uses it
# (see docs/architecture/index.md). Override with the VMAF_RESOURCE env var.
RESOURCE = os.environ.get("VMAF_RESOURCE", os.path.join(PYTHON_ROOT, "resource"))
VMAF_RESOURCE_ROOT = "https://github.com/Netflix/vmaf_resource/raw/master"


def download_reactively(local_path, remote_path):
    if not os.path.exists(local_path):
        os.makedirs(os.path.dirname(local_path), exist_ok=True)
        print(f"download {local_path} from {remote_path}")
        try:
            # Use the system trust store (default since Python 3.6). The
            # previous version of this function clobbered the *process-global*
            # SSL default with `ssl._create_unverified_context`, which masked
            # legitimate TLS failures (corporate MITM proxy, expired root,
            # active attacker) and leaked the bypass to every later SSL call
            # in the same process. GitHub's `vmaf_resource` URL serves a
            # valid public-CA chain, so the bypass was unjustified
            # defence-in-depth disabling. If a CI environment can't validate,
            # operators set `SSL_CERT_FILE` to the right bundle — that's the
            # documented escape hatch and stays per-process scoped. See
            # docs/research/0090-semgrep-warnings-audit-2026-05-09.md (F1).
            # Download to a sibling tempfile then atomically rename, so
            # concurrent readers (e.g. pytest-xdist workers sharing the
            # cache) cannot observe a partially-written local_path.
            # Port of upstream Netflix/vmaf commit 32780bd9b.
            fd, tmp_path = tempfile.mkstemp(
                dir=os.path.dirname(local_path),
                prefix=os.path.basename(local_path) + ".",
                suffix=".part",
            )
            os.close(fd)
            try:
                urllib.request.urlretrieve(remote_path, tmp_path)
                os.replace(tmp_path, local_path)
            except Exception:
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
                raise
        except urllib.error.HTTPError as e:
            print(f"error downloading from {remote_path}")
            raise e


class VmafExternalConfig(object):

    _MISSING_EXTERNAL_MESSAGE = """
    Must install {name} and set {key} in %s/externals.py, e.g. add a line like
    {key} = "[path to exec]/{name}"
    """ % PYTHON_ROOT

    @staticmethod
    def _path_from_external(name):
        """
        :param str name: Name of external configuration to look up
        :return str: Configured path, if any
        """
        try:
            from . import externals

            path = getattr(externals, name, None)
            if path and os.path.exists(path):
                return path
        except ImportError:
            print("ImportError")

        return None

    @staticmethod
    def _attr_from_external(name):
        """
        :param str name: Name of external configuration to look up
        :return str: Configured attribute (not path), if any
        """
        try:
            from . import externals

            attr = getattr(externals, name, None)
            return attr
        except ImportError:
            print("ImportError")
        return None

    @classmethod
    def ffmpeg_path(cls):
        """
        :return str: Path to ffmpeg, if installed and configured via `externals` module
        """
        return cls._path_from_external("FFMPEG_PATH")

    @classmethod
    def ffmpeg_env(cls):
        """
        :return dict: Dictionary of environmental variables when running ffmpeg, if installed and configured via
        'externals` module
        """
        return cls._attr_from_external("FFMPEG_ENV")

    @classmethod
    def matlab_path(cls):
        """
        :return str: Path to matlab, if installed and configured via `externals` module
        """
        return cls._path_from_external("MATLAB_PATH")

    @classmethod
    def matlab_runtime_path(cls):
        """
        :return str: Path to matlab runtime, if installed and configured via `externals` module
        """
        return cls._path_from_external("MATLAB_RUNTIME_PATH")

    @classmethod
    def cvx_path(cls):
        """
        :return str: Path to cvx, if installed and configured via `externals` module
        """
        return cls._path_from_external("CVX_PATH")

    @classmethod
    def psnr_path(cls):
        """
        :return str: Path to external psnr executable, if installed and configured via `externals` module
        """
        return cls._path_from_external("PSNR_PATH")

    @classmethod
    def moment_path(cls):
        """
        :return str: Path to external moment executable, if installed and configured via `externals` module
        """
        return cls._path_from_external("MOMENT_PATH")

    @classmethod
    def ssim_path(cls):
        """
        :return str: Path to external ssim executable, if installed and configured via `externals` module
        """
        return cls._path_from_external("SSIM_PATH")

    @classmethod
    def ms_ssim_path(cls):
        """
        :return str: Path to external ms_ssim executable, if installed and configured via `externals` module
        """
        return cls._path_from_external("MS_SSIM_PATH")

    @classmethod
    def vmaf_path(cls):
        """
        :return str: Path to external vmaf executable, if installed and configured via `externals` module
        """
        return cls._path_from_external("VMAF_PATH")

    @classmethod
    def vmafexec_path(cls):
        """
        :return str: Path to external vmafexec executable, if installed and configured via `externals` module
        """
        return cls._path_from_external("VMAFEXEC_PATH")

    @classmethod
    def get_and_assert_ffmpeg(cls):
        path = cls.ffmpeg_path()
        assert path is not None, cls._MISSING_EXTERNAL_MESSAGE.format(
            name="ffmpeg", key="FFMPEG_PATH"
        )
        return path

    @classmethod
    def get_and_assert_matlab(cls):
        path = cls.matlab_path()
        assert path is not None, cls._MISSING_EXTERNAL_MESSAGE.format(
            name="matlab", key="MATLAB_PATH"
        )
        return path

    @classmethod
    def get_and_assert_matlab_runtime(cls):
        path = cls.matlab_runtime_path()
        assert (
            path is not None
        ), """Must install matlab runtime (v9.1) and set {key} in {root}/externals.py, e.g. add a line like {key} = "[path to matlab runtime]/v91"
            """.format(
            root=PYTHON_ROOT, key="MATLAB_RUNTIME_PATH"
        )
        return path

    @classmethod
    def get_and_assert_cvx(cls):
        path = cls.cvx_path()
        assert path is not None, cls._MISSING_EXTERNAL_MESSAGE.format(name="cvx", key="CVX_PATH")
        return path


class VmafConfig(object):

    @classmethod
    def root_path(cls, *components):
        return os.path.join(ROOT, *components)

    @classmethod
    def file_result_store_path(cls, *components):
        return os.path.join(WORKSPACE, "result_store_dir", "file_result_store", *components)

    @classmethod
    def encode_store_path(cls, *components):
        return os.path.join(WORKSPACE, "result_store_dir", "encode_store", *components)

    @classmethod
    def workspace_path(cls, *components):
        """Root of the Python harness workspace (datasets, models, encodes,
        outputs, per-run scratch). Fork moved this from ROOT/workspace to
        compat/python-vmaf/workspace; override via VMAF_WORKSPACE env var. See
        docs/architecture/workspace.md for the subdirectory contract.
        """
        return os.path.join(WORKSPACE, *components)

    @classmethod
    def workdir_path(cls, *components):
        return os.path.join(WORKSPACE, "workdir", *components)

    @classmethod
    def model_path(cls, *components):
        return cls.root_path("model", *components)

    @classmethod
    def resource_path(cls, *components):
        """Root of the static resource tree (example datasets, param files,
        model-training params, tutorial images). Fork moved this from
        ROOT/resource to compat/python-vmaf/resource; override via VMAF_RESOURCE env
        var. See docs/architecture/index.md for the rationale.
        """
        return os.path.join(RESOURCE, *components)

    @classmethod
    def test_resource_path(cls, *components, bypass_download=False):
        local_path = cls.root_path("python", "test", "resource", *components)
        if bypass_download:
            pass
        else:
            remote_path = os.path.join(
                VMAF_RESOURCE_ROOT, "python", "test", "resource", *components
            )
            download_reactively(local_path, remote_path)
        return local_path

    @classmethod
    def tools_resource_path(cls, *components):
        return cls.root_path("python", "vmaf", "tools", "resource", *components)

    @classmethod
    def encode_path(cls, *components):
        return os.path.join(WORKSPACE, "encode", *components)


class DisplayConfig(object):

    @staticmethod
    def show(**kwargs):
        from vmaf import plt

        if "write_to_dir" in kwargs:
            fmt = kwargs["format"] if "format" in kwargs else "png"
            filedir = (
                kwargs["write_to_dir"]
                if kwargs["write_to_dir"] is not None
                else VmafConfig.workspace_path("output")
            )
            os.makedirs(filedir, exist_ok=True)
            for fignum in plt.get_fignums():
                fig = plt.figure(fignum)
                fig.savefig(os.path.join(filedir, str(fignum) + "." + fmt), format=fmt)
        else:
            plt.show()
