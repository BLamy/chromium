# Replay's changes to dependencies

Chromium takes these from their own repositories, at the commits `DEPS` names, so Replay's changes to them are
patches here rather than forks. After `gclient sync`:

```sh
git -C third_party/boringssl/src apply ../../../replay_patches/boringssl.patch
git -C third_party/libc++abi/src apply ../../../replay_patches/libc++abi.patch
```

| Patch | Against | What |
| --- | --- | --- |
| `boringssl.patch` | `ea9f3a34c3977144c45d272be81842fd891a0bdb` | Random bytes come straight from the system when recording or replaying. Which thread first seeds the DRBG, and which first sets up fork detection, isn't the same twice. From `replayio/boringssl`'s `rand.c` change |
| `libc++abi.patch` | `440f88852530ac197479edb1463dda1916692cce` | The mutex that decides which thread runs a static initializer is one the driver orders. New here: on Chromium 108 the guard was unordered, and statics that raced were constructed early, one by one |

V8 is `BLamy/chromium-v8`, branch `chromium-158`.
