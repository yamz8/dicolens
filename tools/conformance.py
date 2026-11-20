#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = [
#     "numpy",
#     "pydicom>=3.0,<4",
#     "pylibjpeg>=2.0",
#     "pylibjpeg-libjpeg>=2.1",
#     "pylibjpeg-openjpeg>=2.3",
#     "pyjpegls>=1.2",
#     "python-gdcm>=3.0.24",
# ]
# ///
"""Holds what Dicolens decodes against a reference decoder, frame by frame.

A development tool: run it with bin/conformance (which builds the pixel
dumper first) or `uv run tools/conformance.py`. Nothing here is shipped, and
nothing it downloads is kept in the repository.

The reference is pydicom with its pylibjpeg (libjpeg, OpenJPEG) and CharLS
plugins. The files are pydicom's own test files, the pydicom-data repository,
and NEMA's WG04 compressed samples when their server answers, cached under
~/.cache/dicolens-conformance/. Every frame both sides decode is compared:

  exact       every value the same. Required of the lossless transfer
              syntaxes and of uncompressed files.
  tolerance   a lossy syntax (two decoders may round a lossy picture
              differently), within LOSSY_MEAN and LOSSY_MAX below; a lossy
              JPEG whose colour is kept at half resolution, which decoders
              bring back differently (Dicolens repeats each sample, the
              reference blends neighbours), with its brightness within those
              and its colour within CHROMA_MEAN; a lossy picture the
              reference can't decode (12-bit JPEG) whose uncompressed
              original is among the files, within ORIGINAL_MEAN of that; or
              32-bit values, which Dicolens keeps in 16 bits, within one
              step of that scale.
  mismatch    anything else, including a frame the reference decodes and
              Dicolens doesn't. Each is listed after the table.
  by design   Dicolens says it doesn't read this kind of picture, or ffmpeg
              says it hasn't the decoder for it.
  no ref.     the reference couldn't decode it either way (damaged test
              files, mostly), so there is nothing to compare with.

Grey frames are compared as stored values: the bits the file says are
stored, sign-extended. Colour frames are compared as the 8-bit RGB a viewer
shows; where a lossless file stores YCbCr, the reference's samples are
converted with the same full-range BT.601 arithmetic Dicolens uses, so a
lossless colour file must still match exactly.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import warnings
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pydicom
from pydicom.encaps import generate_frames
from pydicom.pixels import apply_color_lut
from pydicom.uid import UID

ROOT = Path(__file__).resolve().parent.parent
CACHE = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "dicolens-conformance"

PYDICOM_DATA = "https://codeload.github.com/pydicom/pydicom-data/tar.gz/refs/heads/master"
# NEMA's working group 4 samples: the same pictures in every compressed
# transfer syntax, and the uncompressed ones they were made from.
WG04 = "ftp://medical.nema.org/medical/dicom/DataSets/WG04/"
WG04_ARCHIVES = ["compsamples_refanddir.tar.bz2", "compsamples_rle_20040210.tar.bz2", "compsamples_jpeg.tar",
                 "compsamples_jpegls.tar", "compsamples_j2k.tar"]

# A lossy frame is within tolerance when, on a scale of 0 to 255 (grey
# values are scaled from their stored bits), the two decoders differ by no
# more than LOSSY_MEAN on average and LOSSY_MAX at any one sample.
LOSSY_MEAN = 1.0
LOSSY_MAX = 16.0
# Where a JPEG keeps colour at half resolution, that holds for brightness
# (Y), and the colour (Cb, Cr) may differ by this on average: at an edge
# between two colours, a repeated sample and a blended one are far apart.
CHROMA_MEAN = 4.0
# The reference's decoders don't read 12-bit lossy JPEG. NEMA's samples come
# with the uncompressed pictures they were made from (X_JPLY from X_UNC), so
# those are held against their originals instead: what was lost in
# compressing is in the difference too, so only its average is bounded.
ORIGINAL_MEAN = 2.0

LOSSY = {
    "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.81", "1.2.840.10008.1.2.4.91",
    "1.2.840.10008.1.2.4.93", "1.2.840.10008.1.2.4.203",
}
# Syntaxes whose codec turns colour back to RGB itself, so the reference is
# asked for RGB rather than for the samples as stored.
CODEC_COLOUR = {
    "1.2.840.10008.1.2.4.50", "1.2.840.10008.1.2.4.51", "1.2.840.10008.1.2.4.90", "1.2.840.10008.1.2.4.91",
    "1.2.840.10008.1.2.4.92", "1.2.840.10008.1.2.4.93", "1.2.840.10008.1.2.4.201", "1.2.840.10008.1.2.4.202",
    "1.2.840.10008.1.2.4.203",
}
BY_DESIGN = (
    "This image's pixel format isn't supported",
    "This image is compressed in a way Dicolens can't read",
    "This JPEG image's layout isn't supported",
    "This file has no image",
)
# What ffmpeg says of a stream it has no decoder for (JPEG-LS with its
# colours interleaved sample by sample).
FFMPEG_LACKS = "Not yet implemented in FFmpeg"
# A palette written as segments (runs and ramps) rather than entries, which
# Dicolens doesn't expand.
SEGMENTED_PALETTE = 0x00281221


@dataclass
class Tally:
    files: int = 0
    frames: int = 0
    exact: int = 0
    tolerance: int = 0
    mismatch: int = 0
    by_design: int = 0
    no_reference: int = 0


@dataclass
class Result:
    path: Path
    syntax: str = ""
    photometric: str = ""
    tally: Tally = field(default_factory=Tally)
    notes: list[str] = field(default_factory=list)   # One for each mismatch.
    unread: set[str] = field(default_factory=set)    # Why frames are "by design".
    skipped: str = ""                                 # Why the file isn't in the table at all.


def fetch(url: str, to: Path, timeout: int) -> bool:
    """Downloads once; a failed download leaves nothing behind."""
    if to.exists():
        return True
    to.parent.mkdir(parents=True, exist_ok=True)
    partial = to.with_suffix(to.suffix + ".part")
    try:
        print(f"Fetching {url}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=timeout) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out)
        partial.rename(to)
        return True
    except Exception as error:  # noqa: BLE001 - any failure means "not reachable today"
        print(f"  not fetched: {error}", file=sys.stderr)
        partial.unlink(missing_ok=True)
        return False


def unpack(archive: Path, to: Path) -> None:
    if to.exists():
        return
    work = to.with_name(to.name + ".part")
    shutil.rmtree(work, ignore_errors=True)
    with tarfile.open(archive) as tar:
        # The "data" filter refuses paths that leave the folder, links out
        # of it, and device files.
        tar.extractall(work, filter="data")
    work.rename(to)


def corpora(with_wg04: bool) -> dict[str, list[Path]]:
    """The test files, by where they came from."""
    found: dict[str, list[Path]] = {}
    own = Path(pydicom.__file__).parent / "data" / "test_files"
    found["pydicom"] = sorted(p for p in own.rglob("*") if p.is_file())

    archive = CACHE / "downloads" / "pydicom-data.tar.gz"
    if fetch(PYDICOM_DATA, archive, timeout=120):
        unpack(archive, CACHE / "pydicom-data")
        found["pydicom-data"] = sorted(p for p in (CACHE / "pydicom-data").rglob("*") if p.is_file())

    if with_wg04:
        files: list[Path] = []
        for name in WG04_ARCHIVES:
            archive = CACHE / "downloads" / name
            if fetch(WG04 + name, archive, timeout=60):
                folder = CACHE / "wg04" / name.split(".")[0]
                unpack(archive, folder)
                files += sorted(p for p in folder.rglob("*") if p.is_file())
        if files:
            found["nema-wg04"] = files
    return found


def stored_values(array: np.ndarray, ds: pydicom.Dataset, compressed: bool, lossy: bool) -> np.ndarray:
    """Grey samples as the values the file says are stored: BitsStored of
    them, from HighBit down in uncompressed data, sign-extended. A lossy
    decoder may overshoot the stored bits (1023 comes back as 1024), and a
    viewer holds such a value at the top rather than masking it to 0."""
    allocated = int(ds.BitsAllocated)
    stored = min(max(int(getattr(ds, "BitsStored", allocated)), 1), allocated)
    high = int(getattr(ds, "HighBit", stored - 1))
    is_signed = int(getattr(ds, "PixelRepresentation", 0)) == 1
    values = array.astype(np.int64) & ((1 << allocated) - 1)
    if not compressed:
        values >>= min(max(high + 1 - stored, 0), 31)
    if lossy and not is_signed:
        values = np.minimum(values, (1 << stored) - 1)
    values &= (1 << stored) - 1
    if is_signed:
        values = np.where(values & (1 << (stored - 1)), values - (1 << stored), values)
    return values


def ybr_to_rgb(ybr: np.ndarray) -> np.ndarray:
    """Full-range BT.601, in the arithmetic of ybrToRgb in src/pixels.cpp."""
    y, cb, cr = (ybr[..., i].astype(np.float64) for i in range(3))
    rgb = np.stack([y + 1.402 * (cr - 128), y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128), y + 1.772 * (cb - 128)], axis=-1)
    return (np.clip(rgb, 0.0, 255.0) + 0.5).astype(np.uint8)


def transfer_syntax(ds: pydicom.Dataset) -> str:
    """The file's transfer syntax; for a file from before Part 10, which has
    no header to say, the one it was read as (and pydicom is told so)."""
    if "TransferSyntaxUID" not in ds.file_meta:
        implicit, little = ds.original_encoding
        ds.file_meta.TransferSyntaxUID = UID("1.2.840.10008.1.2" if implicit else
                                             "1.2.840.10008.1.2.1" if little else "1.2.840.10008.1.2.2")
    return str(ds.file_meta.TransferSyntaxUID)


def chroma_subsampled(ds: pydicom.Dataset) -> bool:
    """Whether a JPEG's start-of-frame gives its components different
    sampling factors: colour kept at a lower resolution than brightness."""
    try:
        stream = next(generate_frames(ds.PixelData, number_of_frames=int(getattr(ds, "NumberOfFrames", 1) or 1)))
    except Exception:  # noqa: BLE001 - a stream that can't be found has no factors
        return False
    at = 2
    while at + 4 <= len(stream):
        if stream[at] != 0xFF or stream[at + 1] in (0xFF, 0x01, 0xD8) or 0xD0 <= stream[at + 1] <= 0xD7:
            at += 1
            continue
        marker, length = stream[at + 1], int.from_bytes(stream[at + 2:at + 4], "big")
        if marker in (0xC0, 0xC1, 0xC2):
            factors = {stream[at + 11 + 3 * c] for c in range(stream[at + 9]) if at + 11 + 3 * c < len(stream)}
            return len(factors) > 1
        if marker in (0xDA, 0xD9):
            break
        at += 2 + length
    return False


def reference(path: Path) -> tuple[pydicom.Dataset, list[tuple[str, np.ndarray]]]:
    """The file's frames from pydicom: ("grey", stored values) or ("rgb",
    height x width x 3 bytes). Raises when it can't decode them."""
    ds = pydicom.dcmread(path, force=True)
    syntax = transfer_syntax(ds)
    photometric = str(getattr(ds, "PhotometricInterpretation", "")).upper()
    samples = int(getattr(ds, "SamplesPerPixel", 1))
    frames = int(getattr(ds, "NumberOfFrames", 1) or 1)
    compressed = UID(syntax).is_compressed

    codec_colour = syntax in CODEC_COLOUR
    ds.pixel_array_options(raw=not codec_colour)
    array = ds.pixel_array
    if frames == 1:
        array = array[np.newaxis, ...]

    out: list[tuple[str, np.ndarray]] = []
    for frame in array:
        if samples == 1 and photometric == "PALETTE COLOR":
            rgb = apply_color_lut(frame, ds)
            if rgb.dtype != np.uint8:
                rgb = (rgb >> 8).astype(np.uint8)
            out.append(("rgb", rgb[..., :3]))
        elif samples == 1:
            out.append(("grey", stored_values(frame, ds, compressed, syntax in LOSSY)))
        else:
            allocated = int(ds.BitsAllocated)
            if allocated > 8:
                stored = min(max(int(getattr(ds, "BitsStored", allocated)), 8), 16)
                frame = np.minimum(frame.astype(np.int64) >> (stored - 8), 255)
            # RLE keeps the colour it was given, like uncompressed data.
            if not codec_colour and photometric.startswith("YBR") and allocated == 8:
                frame = ybr_to_rgb(frame)
            out.append(("rgb", frame.astype(np.uint8)))
    return ds, out


def dicolens(dumper: Path, path: Path, raw: Path) -> tuple[dict, list[dict]]:
    """What the pixel dumper printed: the file's line, then one a frame."""
    run = subprocess.run([str(dumper), "--raw", str(raw), str(path)], capture_output=True, timeout=600)
    lines = [json.loads(line) for line in run.stdout.decode("utf-8", "replace").splitlines() if line.startswith("{")]
    if not lines or "frames" not in lines[0]:
        problem = lines[0].get("error") if lines else f"exit {run.returncode}: {run.stderr.decode('utf-8', 'replace')[-300:]}"
        return {"error": problem or "no output"}, []
    return lines[0], lines[1:]


def luma_chroma(rgb: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Brightness, and the two colour differences side by side (BT.601)."""
    r, g, b = (rgb[..., i].astype(np.float64) for i in range(3))
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return y, np.stack([(b - y) * 0.564, (r - y) * 0.713], axis=-1)


def compare(kind: str, want: np.ndarray, got: dict, ds: pydicom.Dataset, lossy: bool, subsampled: bool,
            original: bool = False) -> tuple[str, str]:
    """("exact" | "tolerance" | "mismatch", why)."""
    if got["kind"] != kind:
        return "mismatch", f"decoded as {got['kind']}, the reference as {kind}"
    height, width = want.shape[:2]
    if (got["height"], got["width"]) != (height, width):
        return "mismatch", f"{got['width']}x{got['height']}, the reference {width}x{height}"
    data = np.fromfile(got["raw"], dtype=np.uint8)
    if kind == "rgb":
        mine = data.reshape(height, width, 3).astype(np.int64)
        theirs = want.astype(np.int64)
        scale = 1.0
    else:
        mine = data.view("<u2").reshape(height, width)
        mine = (mine.view("<i2") if got["signed"] else mine).astype(np.int64)
        theirs = want
        allocated = int(ds.BitsAllocated)
        if allocated > 16:
            # Kept in 16 bits with a scale of its own; compared as the
            # values shown, to within a step of that scale.
            slope = float(getattr(ds, "RescaleSlope", 1) or 1)
            intercept = float(getattr(ds, "RescaleIntercept", 0) or 0)
            shown = mine * got["slope"] + got["intercept"]
            expected = theirs * slope + intercept
            worst = float(np.max(np.abs(shown - expected)))
            step = abs(got["slope"])
            if worst <= step * 0.5 + 1e-6 * max(1.0, float(np.max(np.abs(expected)))):
                return ("exact" if worst == 0 else "tolerance"), ""
            return "mismatch", f"32-bit values differ by up to {worst:g} (one step is {step:g})"
        stored = min(max(int(getattr(ds, "BitsStored", allocated)), 1), 16)
        scale = 255.0 / ((1 << stored) - 1)
    if np.array_equal(mine, theirs):
        return "exact", ""
    difference = np.abs(mine - theirs) * scale
    worst, mean = float(difference.max()), float(difference.mean())
    differing = int(np.count_nonzero(difference))
    why = f"{differing} of {difference.size} samples differ, by {mean:.3f} on average and {worst:.1f} at most (of 255)"
    if original:
        return ("tolerance" if mean <= ORIGINAL_MEAN else "mismatch"), why + ", against the uncompressed original"
    if lossy and mean <= LOSSY_MEAN and worst <= LOSSY_MAX:
        return "tolerance", why
    if lossy and subsampled and kind == "rgb":
        (y1, c1), (y2, c2) = luma_chroma(mine), luma_chroma(theirs)
        luma, chroma = np.abs(y1 - y2), np.abs(c1 - c2)
        why += f"; brightness by {luma.mean():.3f} and {luma.max():.1f}, colour by {chroma.mean():.3f} on average"
        if luma.mean() <= LOSSY_MEAN and luma.max() <= LOSSY_MAX and chroma.mean() <= CHROMA_MEAN:
            return "tolerance", why
    return "mismatch", why


def check(dumper: Path, path: Path, work: Path, originals: dict[str, Path]) -> Result:
    result = Result(path)
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            header = pydicom.dcmread(path, force=True, stop_before_pixels=True)
            syntax = transfer_syntax(header)
    except Exception as error:  # noqa: BLE001
        result.skipped = f"not read by the reference: {type(error).__name__}"
        return result
    if not any(tag in header for tag in ("Rows",)) or "Columns" not in header:
        result.skipped = "no image"
        return result
    result.syntax = syntax
    result.photometric = str(getattr(header, "PhotometricInterpretation", "?")).upper()

    raw = Path(tempfile.mkdtemp(dir=work))
    try:
        try:
            info, frames = dicolens(dumper, path, raw)
        except subprocess.TimeoutExpired:
            info, frames = {"error": "Dicolens took more than ten minutes"}, []
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                ds, wanted = reference(path)
            problem = ""
        except Exception as error:  # noqa: BLE001 - pydicom raises many kinds
            ds, wanted, problem = header, [], f"{type(error).__name__}: {str(error)[:120]}"
        # A lossy picture only Dicolens decodes, held against its original.
        against_original = False
        source = originals.get(path.name.rsplit("_", 1)[0] + "_UNC")
        if not wanted and syntax in LOSSY and source:
            try:
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore")
                    ds, wanted = reference(source)
                against_original, problem = True, ""
            except Exception:  # noqa: BLE001 - then there is still nothing to compare with
                pass

        tally = result.tally
        tally.files = 1
        if info.get("video"):
            # One stream for all the frames, which pydicom doesn't decode.
            tally.frames = 1
            tally.no_reference = 1
            return result
        if "error" in info and not wanted:
            result.skipped = f"read by neither ({info['error']}; {problem})"
            tally.files = 0
            return result
        if "error" in info:
            tally.frames = len(wanted)
            tally.mismatch = len(wanted)
            result.notes.append(f"Dicolens doesn't read the file: {info['error']}")
            return result

        tally.frames = max(len(frames), len(wanted))
        subsampled = syntax in LOSSY and bool(wanted) and wanted[0][0] == "rgb" and chroma_subsampled(ds)
        for index in range(tally.frames):
            got = frames[index] if index < len(frames) else {"error": "No such frame"}
            if "error" in got and (any(got["error"].startswith(reason) for reason in BY_DESIGN)
                                   or FFMPEG_LACKS in got.get("detail", "")
                                   or (SEGMENTED_PALETTE in header and "no palette" in got["error"])):
                tally.by_design += 1
                result.unread.add(f"{got['error']} ({got.get('detail') or 'no more said'})"[:160])
            elif index >= len(wanted):
                tally.no_reference += 1
                if "error" not in got and problem and index == 0:
                    result.notes.append(f"decoded by Dicolens, not by the reference ({problem})")
            elif "error" in got:
                tally.mismatch += 1
                result.notes.append(f"frame {index}: Dicolens says \"{got['error']}\" {got.get('detail', '')}".rstrip())
            else:
                verdict, why = compare(*wanted[index], got, ds, syntax in LOSSY, subsampled, against_original)
                setattr(tally, verdict, getattr(tally, verdict) + 1)
                if verdict == "mismatch":
                    result.notes.append(f"frame {index}: {why}")
        return result
    finally:
        shutil.rmtree(raw, ignore_errors=True)


def syntax_name(uid: str) -> str:
    name = UID(uid).name
    return uid if name == uid else name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--dumper", type=Path, default=ROOT / "build" / "dicolens_pixeldump",
                        help="the dicolens_pixeldump program (bin/conformance builds it)")
    parser.add_argument("--no-wg04", action="store_true", help="leave out NEMA's WG04 samples (a large download)")
    parser.add_argument("--only", metavar="TEXT", help="only files whose path contains this")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("files", nargs="*", type=Path, help="compare these files instead of the downloaded ones")
    arguments = parser.parse_args()
    if not arguments.dumper.exists():
        print(f"{arguments.dumper} isn't built: run bin/conformance, or build the dicolens_pixeldump target", file=sys.stderr)
        return 2

    sources = {"given": arguments.files} if arguments.files else corpora(not arguments.no_wg04)
    paths = [p for files in sources.values() for p in files if not arguments.only or arguments.only in str(p)]
    for source, files in sources.items():
        print(f"{source}: {len(files)} files", file=sys.stderr)

    CACHE.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=CACHE) as work, concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        originals = {p.name: p for files in sources.values() for p in files if p.name.endswith("_UNC")}
        results = list(pool.map(lambda p: check(arguments.dumper, p, Path(work), originals), paths))

    table: dict[tuple[str, str], Tally] = defaultdict(Tally)
    for result in results:
        if result.skipped:
            continue
        row = table[(syntax_name(result.syntax), result.photometric)]
        for name in vars(row):
            setattr(row, name, getattr(row, name) + getattr(result.tally, name))

    columns = ["Transfer syntax", "Photometric", "files", "frames", "exact", "tolerance", "mismatch", "by design", "no ref."]
    rows = [[syntax, photometric, *map(str, vars(tally).values())] for (syntax, photometric), tally in sorted(table.items())]
    total = Tally()
    for tally in table.values():
        for name in vars(total):
            setattr(total, name, getattr(total, name) + getattr(tally, name))
    rows.append(["Total", "", *map(str, vars(total).values())])
    widths = [max(len(row[i]) for row in [columns, *rows]) for i in range(len(columns))]
    for row in [columns, ["-" * w for w in widths], *rows]:
        print("  ".join(cell.ljust(w) if i < 2 else cell.rjust(w) for i, (cell, w) in enumerate(zip(row, widths))))

    noted = [r for r in results if r.notes]
    if noted:
        print("\nWhat differs:")
        for result in noted:
            print(f"  {result.path.name} ({syntax_name(result.syntax)}, {result.photometric})")
            for note in result.notes[:4]:
                print(f"    {note}")
            if len(result.notes) > 4:
                print(f"    and {len(result.notes) - 4} more frames")
    unread = [r for r in results if r.unread]
    if unread:
        print("\nNot read, by design:")
        for result in unread:
            print(f"  {result.path.name} ({syntax_name(result.syntax)}, {result.photometric}): {'; '.join(sorted(result.unread))}")
    left_out = defaultdict(int)
    for result in results:
        if result.skipped:
            left_out[result.skipped.split(" (")[0]] += 1
    if left_out:
        print("\nNot in the table: " + ", ".join(f"{count} {why}" for why, count in sorted(left_out.items())))
    return 1 if total.mismatch else 0


if __name__ == "__main__":
    sys.exit(main())
