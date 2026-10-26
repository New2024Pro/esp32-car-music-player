# PlatformIO extra script: gzip web/* into data/ before the filesystem image is built.
# Edit files in web/; data/ is generated. The web server sends the .gz with
# Content-Encoding: gzip, so far fewer bytes go over the radio shared with Bluetooth.
#
# Runs on every pio invocation (not as a pre-action of littlefs.bin): the image is only
# rebuilt when data/ changes, so data/ must be refreshed before that check happens.
import gzip
import os

Import("env")  # noqa: F821 (provided by PlatformIO)

project = env["PROJECT_DIR"]  # noqa: F821
src_dir = os.path.join(project, "web")
out_dir = os.path.join(project, "data")


def gzip_web():
    os.makedirs(out_dir, exist_ok=True)
    wanted = set()
    for name in sorted(os.listdir(src_dir)):
        src = os.path.join(src_dir, name)
        if not os.path.isfile(src):
            continue
        with open(src, "rb") as f:
            raw = f.read()
        dst = os.path.join(out_dir, name + ".gz")
        wanted.add(name + ".gz")
        # mtime=0: identical input -> identical bytes, so unchanged pages leave data/ untouched
        packed = gzip.compress(raw, compresslevel=9, mtime=0)
        if os.path.isfile(dst):
            with open(dst, "rb") as f:
                if f.read() == packed:
                    continue
        with open(dst, "wb") as f:
            f.write(packed)
        print("gzip_web: %s %d -> %d bytes" % (name, len(raw), len(packed)))
    for name in os.listdir(out_dir):
        if name.endswith(".gz") and name not in wanted:
            os.remove(os.path.join(out_dir, name))


gzip_web()
