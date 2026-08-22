# ac3forge

Python bindings for [ac3forge](https://github.com/iainchesworthlabs/ac3forge), a clean-room
AC-3/E-AC-3 (Dolby Digital/Digital Plus) encoder and decoder written in C++23, including the
Atmos-in-DD+ object layer (OAMD + JOC).

```python
import numpy as np
import ac3forge as ac3

encoder = ac3.FrameEncoder(ac3.EncoderConfig(acmod=ac3.Acmod.k2_0, bitrate_kbps=192))
tone = 0.2 * np.sin(2 * np.pi * 440 * np.arange(ac3.SAMPLES_PER_FRAME) / 48000).astype(np.float32)
frame = encoder.encode_frame([tone, tone])

decoder = ac3.FrameDecoder()
decoded = decoder.decode_frame(frame)
print(decoded.channels[0].shape)  # (1536,)
```

See [docs/library/python-api.md](https://iainchesworthlabs.github.io/ac3forge/library/python-api/)
for the full surface (E-AC-3 and Atmos object encode/decode included) and
[the main project README](https://github.com/iainchesworthlabs/ac3forge) for what the codec
itself covers. Licensed GPL-3.0-only, same as the rest of the project — see
[LICENSE](https://github.com/iainchesworthlabs/ac3forge/blob/main/LICENSE).
