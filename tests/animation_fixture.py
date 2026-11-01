import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="cressida-animation-") as directory:
    root = pathlib.Path(directory)
    colors = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255),
              (255, 255, 0, 255), (0, 255, 255, 255), (255, 0, 255, 255)]
    frames = b"".join(bytes(color) * (32 * 24) for color in colors)
    for extension, codec in [("gif", "gif"), ("apng", "apng"), ("webp", "libwebp_anim"), ("mp4", "libx264"), ("webm", "libvpx")]:
        codec_options = ["-pix_fmt", "yuv420p"] if extension in ("mp4", "webm") else []
        subprocess.run([
            sys.argv[1], "-v", "error", "-nostdin", "-f", "rawvideo", "-pixel_format", "rgba",
            "-video_size", "32x24", "-framerate", "10", "-i", "pipe:0", "-an", "-c:v", codec,
            "-threads", "1", *codec_options, "-y", str(root / ("clip." + extension)),
        ], input=frames, check=True, timeout=15)
    # Full-range H.264 exposes range metadata. Preserve gray levels during playback.
    gray_row = b"".join(bytes((gray, gray, gray, 255)) * 8 for gray in (16, 64, 192, 240))
    subprocess.run([
        sys.argv[1], "-v", "error", "-nostdin", "-f", "rawvideo", "-pixel_format", "rgba",
        "-video_size", "32x24", "-framerate", "10", "-i", "pipe:0", "-an", "-c:v", "libx264",
        "-crf", "0", "-pix_fmt", "yuvj420p", "-color_range", "pc", "-threads", "1", "-y", str(root / "full-range.mp4"),
    ], input=gray_row * 24 * 6, check=True, timeout=15)
    # Portraits and non-vector-aligned rows exercise conversion buffer boundaries.
    # Include odd native widths and tall portrait canvases.
    for width, height in [(10, 2), (26, 2), (18, 30), (34, 50), (17, 23), (31, 47), (174, 512)]:
        for extension, codec in [("gif", "gif"), ("apng", "apng"), ("webp", "libwebp_anim"), ("mp4", "libx264"), ("webm", "libvpx")]:
            codec_options = ["-pix_fmt", "yuv444p" if width % 2 or height % 2 else "yuv420p"] if extension == "mp4" else []
            if extension == "webm":
                codec_options = ["-pix_fmt", "yuv420p"]
            subprocess.run([
                sys.argv[1], "-v", "error", "-nostdin", "-f", "rawvideo", "-pixel_format", "rgba",
                "-video_size", f"{width}x{height}", "-framerate", "10", "-i", "pipe:0", "-an",
                "-c:v", codec, "-threads", "1", *codec_options, "-y",
                str(root / f"unaligned-{width}x{height}.{extension}"),
            ], input=b"".join(bytes(color) * (width * height) for color in colors), check=True, timeout=15)
    # One-pixel details must survive decoding, including portraits displayed on HiDPI.
    detail_width, detail_height = 336, 600
    def detail_frame(index):
        row = b"".join(
            bytes((value, value, value, 255))
            for x in range(detail_width)
            for value in [255 if (x + index) % 2 else 0]
        )
        return row * detail_height
    details = [detail_frame(index) for index in range(2)]
    for extension, codec in [("gif", "gif"), ("apng", "apng"), ("webp", "libwebp_anim"),
                             ("mp4", "libx264rgb"), ("webm", "libvpx-vp9")]:
        lossless_options = ["-lossless", "1"] if extension in ("webp", "webm") else []
        if extension == "mp4":
            lossless_options = ["-crf", "0"]
        if extension == "webm":
            lossless_options += ["-pix_fmt", "gbrp"]
        if extension == "gif":
            lossless_options = ["-pix_fmt", "gray"]
        # Long clips exceed 64 MiB in raw RGBA: retain all fine detail under pressure.
        for stem, repeats in [("detail", 3), ("detail-long", 60)]:
            subprocess.run([
                sys.argv[1], "-v", "error", "-nostdin", "-f", "rawvideo", "-pixel_format", "rgba",
                "-video_size", f"{detail_width}x{detail_height}", "-framerate", "10", "-i", "pipe:0",
                "-an", "-c:v", codec, "-threads", "1", *lossless_options,
                "-y", str(root / f"{stem}.{extension}"),
            ], input=b"".join(details * repeats), check=True, timeout=15)
    # Encoded GIF exceeds the frame-count bound but is tiny on disk/in memory.
    subprocess.run([
        sys.argv[1], "-v", "error", "-nostdin", "-f", "lavfi", "-i",
        "testsrc2=size=8x8:rate=100:duration=12.1", "-c:v", "gif", "-threads", "1",
        "-y", str(root / "long.gif"),
    ], check=True, timeout=15)
    subprocess.run([
        sys.argv[1], "-v", "error", "-nostdin", "-f", "lavfi", "-i",
        "testsrc2=size=256x256:rate=30:duration=10", "-c:v", "gif", "-threads", "1",
        "-y", str(root / "large.gif"),
    ], check=True, timeout=15)
    subprocess.run([sys.argv[2], str(root)], check=True, timeout=30)
