# PC/XT HDD Support (Archived)

Status: completed.

This proposal has been retired after implementation and validation.

## Outcome

- PC/XT boots from both floppy and HDD images.
- Stage2 and kernel UFS path use the 44BSD-compatible on-disk layout.
- PC/XT image generation defaults to 44BSD UFS.

## Permanent Documentation

- Target behavior and boot flow: [`../../targets/ia16.md`](../../targets/ia16.md)
- Kernel filesystem architecture: [`../../kernel/filesystems.md`](../../kernel/filesystems.md)
- UFS format and invariants: [`../../kernel/ufs.md`](../../kernel/ufs.md)

## Notes

The repository keeps this file only as a completion marker for historical
traceability.
