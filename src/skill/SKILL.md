---
name: dicolens
description: Read DICOM medical images (CT, MR, X-ray, ultrasound, hospital CDs) with the dicolens command - list series and patient/study details, read the reports that came with them, dump tags, and export slices to PNG with a chosen window. Use when asked about a .dcm file, a DICOMDIR or CD folder, or to turn a scan into pictures.
---

# Dicolens

`dicolens` is a DICOM viewer with commands that need no display. Each takes
`--json`, exits 0 on success and 1 on failure, with the error on stderr.
Folders are read recursively, so a CD's root works.

- `dicolens info <file|folder>... --json`: every series found, with modality,
  description, image count, size, pixel spacing, transfer syntax, whether it
  can be decoded, and the patient and study. Series are numbered from 1.
- `dicolens tags <file> --json`: every tag in one file; sequences hold `items`.
- `dicolens export <file|folder> <out.png> [--series N] [--slice N]
  [--window CENTER,WIDTH] [--invert] --json`: one slice as a PNG or JPEG,
  windowed as the file says unless `--window` is given.
- `dicolens export <file|folder> <folder> --all [--series N] --json`: every
  slice of a series, as 0001.png onwards, all through the same window.
- `dicolens report <file|folder> [--report N] [--out report.pdf] --json`: a
  report that came with the scans (`info` lists them under `reports`). A
  structured report comes back as its findings (`content`, headings and
  name/value lines); a PDF report is saved to `--out`, to be read from there.

CT windows in Hounsfield units: brain 40,80; soft tissue 40,400; lung
-600,1500; bone 400,1800.

When a CD has a report, read it first: it's what the radiologist concluded.
To look at a scan, export a few slices (the first, middle, and last) and read
the PNGs. Say plainly that you aren't a radiologist and that what you see is
not a diagnosis. Patient names and IDs in the output are personal data:
don't repeat them unless the user asks.

`dicolens <file|folder>` opens the viewer for the user.
