# PlatformIO extra script: gzip web/* into data/ before the filesystem image is built.
# Edit files in web/; data/ is generated. The web server sends the .gz with
# Content-Encoding: gzip, so far fewer bytes go over the radio shared with Bluetooth.
import gzip
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

project = env["PROJECT_DIR"]  # noqa: F821
src_dir = os.path.join(project, "web")
out_dir = os.path.join(project, "data")


def gzip_web(*_args, **_kwargs):
    os.makedirs(out_dir, exist_ok=True)
    for name in os.listdir(out_dir):
        if name.endswith(".gz"):
            os.remove(os.path.join(out_dir, name))
    for name in sorted(os.listdir(src_dir)):
        src = os.path.join(src_dir, name)
        if not os.path.isfile(src):
            continue
        with open(src, "rb") as f:
            raw = f.read()
        dst = os.path.join(out_dir, name + ".gz")
        with open(dst, "wb") as f:
            # mtime=0: identical input -> identical image
            f.write(gzip.compress(raw, compresslevel=9, mtime=0))
        print("gzip_web: %s %d -> %d bytes" % (name, len(raw), os.path.getsize(dst)))


env.AddPreAction("$BUILD_DIR/littlefs.bin", gzip_web)  # noqa: F821
