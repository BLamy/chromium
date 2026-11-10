# Replay's changes to dependencies

Replay's changes to a dependency go in a fork of it, which `DEPS` names:

| Dependency | Fork | Branch |
| --- | --- | --- |
| `v8` | `BLamy/chromium-v8` | `chromium-158` |
| `third_party/boringssl/src` | `BLamy/boringssl` | `chromium-158` |

One doesn't have a fork yet, and is a patch here until it does. After `gclient sync`:

```sh
git -C third_party/libc++abi/src apply ../../../replay_patches/libc++abi.patch
```

| Patch | Against | What |
| --- | --- | --- |
| `libc++abi.patch` | `440f88852530ac197479edb1463dda1916692cce` of `chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxxabi` | The mutex that decides which thread runs a static initializer is one the driver orders. New here: on Chromium 108 the guard was unordered, and statics that raced were constructed early, one by one |
