---
name: scans
description: Read DICOM medical images (CT, MR, X-ray, ultrasound, hospital CDs) with the scans command - list series and patient/study details, dump tags, and export slices to PNG with a chosen window. Use when asked about a .dcm file, a DICOMDIR or CD folder, or to turn a scan into pictures.
---

# scans

`scans` is a DICOM viewer with commands that need no display. Each takes
`--json`, exits 0 on success and 1 on failure, with the error on stderr.
Folders are read recursively, so a CD's root works.

- `scans info <file|folder>... --json`: every series found, with modality,
  description, image count, size, pixel spacing, transfer syntax, whether it
  can be decoded, and the patient and study. Series are numbered from 1.
- `scans tags <file> --json`: every tag in one file; sequences hold `items`.
- `scans export <file|folder> <out.png> [--series N] [--slice N]
  [--window CENTER,WIDTH] [--invert] --json`: one slice as a PNG or JPEG,
  windowed as the file says unless `--window` is given.
- `scans export <file|folder> <folder> --all [--series N] --json`: every
  slice of a series, as 0001.png onwards, all through the same window.

CT windows in Hounsfield units: brain 40,80; soft tissue 40,400; lung
-600,1500; bone 400,1800.

To look at a scan, export a few slices (the first, middle, and last) and read
the PNGs. Say plainly that you aren't a radiologist and that what you see is
not a diagnosis. Patient names and IDs in the output are personal data:
don't repeat them unless the user asks.

`scans <file|folder>` opens the viewer for the user.
