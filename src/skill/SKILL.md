---
name: dicolens
description: Read DICOM medical images (CT, MR, PET, X-ray, ultrasound, hospital CDs) with the dicolens command - list series and patient/study details, read the reports that came with them, dump tags, export slices or a 3D picture to PNG with a chosen window and a PET over the CT, list the measurements the user drew in the viewer, and make an anonymous copy to share. Use when asked about a .dcm file, a DICOMDIR or CD folder, or to turn a scan into pictures.
---

# Dicolens

`dicolens` is a DICOM viewer with commands that need no display. Each takes
`--json`, exits 0 on success and 1 on failure, with the error on stderr.
Folders are read recursively, so a CD's root works, and a zip file is read as
the folder inside it.

- `dicolens info <file|folder>... --json`: every series found, with modality,
  description, image count, size, pixel spacing, transfer syntax, whether it
  can be decoded, and the patient and study. Series are numbered from 1.
- `dicolens tags <file> [--frame N] --json`: every tag in one file; sequences
  hold `items`. A multi-frame file keeps an item a frame in (5200,9230);
  `--frame` gives only that frame's.
- `dicolens export <file|folder> <out.png> [--series N] [--slice N]
  [--window CENTER,WIDTH] [--invert] --json`: one slice as a PNG or JPEG,
  windowed as the file says unless `--window` is given.
- `dicolens export <file|folder> <folder> --all [--series N] --json`: every
  slice of a series, as 0001.png onwards, all through the same window.
- `dicolens export <file|folder> <out.png> --3d [--series N]
  [--look bone|vessels|skin|lungs|surface|mip] [--from front|back|left|right|above]
  [--turn ACROSS,DOWN] [--cut MM] [--size WxH] --json`: the series in 3D, as
  the viewer draws it. Bone, vessels, skin, and lungs are for CT; MR and PET
  use surface or mip, through `--window`. `--turn` is in degrees, `--cut`
  cuts that many millimetres away from the front to see inside. It needs a
  stack of evenly spaced slices: thin ones (2 mm or less) look best.
- Add `--pet` to either export to put the PET taken with a CT or MR over it,
  in colour (hot iron; SUV 1 to 6 when the PET has SUVs). Slices take the
  CT's `--series`; the error names it if given the PET's.
- `dicolens measurements <file|folder> [--series N] --json`: the rulers,
  areas, angles, and Cobb angles the user drew in the viewer, kept with each
  series: its slice (from 1) and plane (`scanned`, `coronal`, `sagittal`),
  points in image pixels, and what it comes to (`length` in mm, `area` in mm²
  with `mean`, `sd`, `min`, `max` in HU or SUV, or `degrees`). Pixels where
  the scan has no scale.
- `dicolens report <file|folder> [--report N] [--out report.pdf] --json`: a
  report that came with the scans (`info` lists them under `reports`). A
  structured report comes back as its findings (`content`, headings and
  name/value lines); a PDF report is saved to `--out`, to be read from there.
- `dicolens anonymize <file|folder|zip>... --out <folder> --json`: copies the
  images into a new folder "Anonymous scans" inside `--out`, without the
  patient's name, IDs, birth date, or institution, with new UIDs (PS3.15 Basic
  Profile, keeping dates and age, sex, and weight). Reports are left out.
  `mayShowDetailsInPicture` counts images, such as ultrasound, whose picture
  itself may show the name; tell the user to check those before sharing.

CT windows in Hounsfield units: brain 40,80; soft tissue 40,400; lung
-600,1500; bone 400,1800.

When a CD has a report, read it first: it's what the radiologist concluded.
When the user asks about something they measured, read `measurements` rather
than measuring again.
To look at a scan, export a few slices (the first, middle, and last) and read
the PNGs. Say plainly that you aren't a radiologist and that what you see is
not a diagnosis. Patient names and IDs in the output are personal data:
don't repeat them unless the user asks.

`dicolens <file|folder>` opens the viewer for the user.
