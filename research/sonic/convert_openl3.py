"""Convert the OpenL3 mel256 model to the production post-frontend ONNX graph.

The `musicpack-sonic-openl3-v1` profile fixes OpenL3 0.4.0 (weights SHA-256
`624ee7b1...`). The kapre mel frontend (STFT/mel/decibel) is deterministic
DSP and is reimplemented in C (see sonic/frontend.c, whose numpy reference is
research/sonic/frontend.py). Only the learned network after the frontend is
converted to ONNX, giving onnxruntime the standard conv/bn/maxpool/fc ops.

Usage (inside the research venv):

    research/sonic/.venv/bin/python research/sonic/convert_openl3.py \
        <openl3_audio_mel256_music.h5> <out.onnx>

The H5 is loaded through openl3's own loader (kapre custom objects); the
script verifies the loaded weights against the profile's pinned SHA-256 and
checks the ONNX graph reproduces the Keras post-frontend network on random
mel spectrograms (cosine ~1.0). Only the SHA-256-pinned weights may be used;
the produced ONNX is a derived artifact of exactly those weights.
"""

import hashlib
import os
import sys

import numpy as np


WEIGHTS_SHA256 = "624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    h5_path, out_path = sys.argv[1], sys.argv[2]
    if not os.path.isfile(h5_path):
        print(f"error: model file not found: {h5_path}", file=sys.stderr)
        return 1

    with open(h5_path, "rb") as f:
        actual = hashlib.sha256(f.read()).hexdigest()
    if actual != WEIGHTS_SHA256:
        print(f"error: weights SHA-256 mismatch\n  got {actual}\n  want "
              f"{WEIGHTS_SHA256}", file=sys.stderr)
        return 1

    # openl3's loader needs OPENL3_MODEL_DIR set and the model at the pinned
    # filename inside it.
    model_dir = os.path.dirname(os.path.abspath(h5_path))
    os.environ["OPENL3_MODEL_DIR"] = model_dir
    from openl3.models import load_audio_embedding_model

    model = load_audio_embedding_model(input_repr="mel256", content_type="music",
                                       embedding_size=512)
    for i, l in enumerate(model.layers[:3]):
        print(f"layer {i}: {l.name} {type(l).__name__} {l.output_shape}")
    if model.layers[1].name != "melspectrogram":
        print("error: unexpected model layout", file=sys.stderr)
        return 1

    import tensorflow as tf
    from tensorflow import keras

    x = keras.Input(shape=(256, 199, 1), dtype="float32")
    y = x
    for l in model.layers[2:]:
        y = l(y)
    post = keras.Model(x, y)
    post._set_inputs(x)

    import tf2onnx
    spec = tf.TensorSpec((None, 256, 199, 1), tf.float32, name="mel")
    onnx_model, _ = tf2onnx.convert.from_keras(post, input_signature=(spec,), opset=13)
    with open(out_path, "wb") as f:
        f.write(onnx_model.SerializeToString())

    import onnxruntime as ort
    rng = np.random.RandomState(1)
    mel = rng.rand(8, 256, 199, 1).astype(np.float32)
    y_tf = post.predict(mel, verbose=0)
    sess = ort.InferenceSession(out_path, providers=["CPUExecutionProvider"])
    y_onnx = sess.run(None, {"mel": mel})[0]
    diff = np.abs(y_tf - y_onnx)
    cos = float(np.mean(np.sum(y_tf * y_onnx, axis=1) /
                        (np.linalg.norm(y_tf, axis=1) * np.linalg.norm(y_onnx, axis=1))))
    print(f"verification: cosine={cos:.7f} maxdiff={diff.max():.3e} "
          f"meandiff={diff.mean():.3e}")
    if cos < 0.9999:
        print("error: ONNX conversion does not reproduce the Keras network",
              file=sys.stderr)
        return 1

    print("sha256:", hashlib.sha256(open(out_path, "rb").read()).hexdigest())
    print("wrote:", out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
