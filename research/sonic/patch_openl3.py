#!/usr/bin/env python3
"""Produce a Python 3.12-installable openl3==0.4.0 source tree.

openl3 0.4.0 (last release, unmaintained):

  * reads its version in setup.py via ``imp.load_source`` — ``imp`` was
    removed in Python 3.12;
  * downloads all 12 model-weight files into the package during a source
    build (hundreds of MB) even though only one model is needed.

This script downloads the exact pinned sdist (SHA-256 verified), applies the
small patches below, and writes the patched source tree to
<out-dir>/openl3-0.4.0/. Install with:

    python research/sonic/patch_openl3.py wheels
    uv pip install --python research/sonic/.venv/bin/python wheels/openl3-0.4.0

Patches (nothing else is changed; the model code is untouched):

  1. setup.py: drop ``import imp`` and read the version via exec();
  2. setup.py: build with no bundled weight files (``weight_files = []``) —
     weights are fetched at runtime by our adapter into a gitignored cache
     with recorded SHA-256 (see analyzers/openl3.py);
  3. models.py: ``get_audio_embedding_model_path`` / ``get_image_*`` honour an
     ``OPENL3_MODEL_DIR`` environment variable so the cached weights are used
     instead of the package directory.

The wheel/artifact dir is gitignored; this script is the committed,
reproducible way to regenerate the patched tree.
"""

import hashlib
import os
import sys
import tarfile
import tempfile
import urllib.request

URL = "https://files.pythonhosted.org/packages/source/o/openl3/openl3-0.4.0.tar.gz"
SHA256 = "91fc142ecca4d39c8bba0fa7412210bab9625ce2b1a6265b266e768b561d14d5"

PATCHES = [
    # 1a. remove the Python 3.12-incompatible import
    ("setup.py", "import imp\n", ""),
    # 1b. read version without imp.load_source
    ("setup.py",
     "version = imp.load_source('openl3.version', "
     "os.path.join('openl3', 'version.py'))",
     "_version_ns = {}\n"
     "exec(open(os.path.join('openl3', 'version.py')).read(), _version_ns)\n"
     "version = _version_ns['version']"),
    # 1c. setup() consumes the version string, not a module attribute
    ("setup.py", "    version=version.version,\n", "    version=version,\n"),
    # 2. never bundle weight files at build time
    ("setup.py",
     "weight_files = ['openl3_{}_{}_{}.h5'.format(*tup)\n"
     "                for tup in product(modalities, input_reprs, content_type)]",
     "weight_files = []  # weights are fetched at runtime by our adapter"),
    # 3. audio model path honours OPENL3_MODEL_DIR
    ("openl3/models.py",
     "    return os.path.join(os.path.dirname(__file__),\n"
     "                        'openl3_audio_{}_{}.h5'.format(input_repr, content_type))",
     "    model_dir = os.environ.get('OPENL3_MODEL_DIR') or os.path.dirname(__file__)\n"
     "    return os.path.join(model_dir,\n"
     "                        'openl3_audio_{}_{}.h5'.format(input_repr, content_type))"),
    # 3b. image model path honours OPENL3_MODEL_DIR (consistency)
    ("openl3/models.py",
     "    return os.path.join(os.path.dirname(__file__),\n"
     "                        'openl3_image_{}_{}.h5'.format(input_repr, content_type))",
     "    model_dir = os.environ.get('OPENL3_MODEL_DIR') or os.path.dirname(__file__)\n"
     "    return os.path.join(model_dir,\n"
     "                        'openl3_image_{}_{}.h5'.format(input_repr, content_type))"),
    # 4. relax stale dependency pins so the audio path installs on a modern
    #    TF2 / Keras2 stack (h5py must be 3.x for TF 2.15; resampy must be
    #    0.4.x for librosa 0.10; moviepy/scikit-image are only used by the
    #    image/video embedding path, which the MusicPack benchmark never uses).
    ("setup.py",
     "    install_requires=[\n"
     "        'tensorflow>=2.0.0',\n"
     "        'numpy>=1.13.0',\n"
     "        'scipy>=0.19.1',\n"
     "        'kapre>=0.3.5',\n"
     "        'soundfile>=0.9.0.post1',\n"
     "        'resampy>=0.2.1,<0.3.0',\n"
     "        'h5py>=2.7.0,<3.0.0',\n"
     "        'moviepy>=1.0.0',\n"
     "        'scikit-image>=0.14.3,<0.15.0',\n"
     "        'librosa>=0.7.2',  # version limit from kapre\n"
     "    ],",
     "    install_requires=[\n"
     "        'tensorflow>=2.0.0',\n"
     "        'numpy>=1.13.0',\n"
     "        'scipy>=0.19.1',\n"
     "        'kapre>=0.3.5',\n"
     "        'soundfile>=0.9.0.post1',\n"
     "        'resampy>=0.4.2',\n"
     "        'h5py>=3.10,<4',\n"
     "        'librosa>=0.7.2',\n"
     "    ],"),
]


def main():
    out_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "wheels")
    os.makedirs(out_dir, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tarball = os.path.join(tmp, "openl3-0.4.0.tar.gz")
        print("downloading", URL)
        urllib.request.urlretrieve(URL, tarball)

        digest = hashlib.sha256(open(tarball, "rb").read()).hexdigest()
        if digest != SHA256:
            print("ERROR: sdist SHA-256 mismatch", file=sys.stderr)
            sys.exit(1)
        print("sdist sha256 verified:", digest)

        with tarfile.open(tarball) as tf:
            tf.extractall(tmp)
        src = os.path.join(tmp, "openl3-0.4.0")

        for rel, old, new in PATCHES:
            path = os.path.join(src, rel)
            text = open(path).read()
            if old not in text:
                print("ERROR: patch target not found in %s:\n%s" % (rel, old),
                      file=sys.stderr)
                sys.exit(1)
            open(path, "w").write(text.replace(old, new))
            print("patched %s" % rel)

        dest = os.path.join(out_dir, "openl3-0.4.0")
        if os.path.isdir(dest):
            import shutil
            shutil.rmtree(dest)
        os.rename(src, dest)
        print("patched source written to", dest)


if __name__ == "__main__":
    main()
