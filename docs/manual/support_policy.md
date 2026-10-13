# Support policy (proposal)

**Status: a proposal for the maintainer to accept, change or reject.** Nothing here is promised until it is
agreed and this note is removed. It builds on [API stability](api_stability.md) and
[Upgrading](13-upgrading.md).

## Versions

Releases are `0.MINOR.PATCH` while the engine is pre-1.0 and `MAJOR.MINOR.PATCH` after.

- **Patch** releases fix bugs and never remove or change a **stable** API.
- **Minor** releases add features and may change **experimental** APIs. A **stable** API is deprecated in one
  minor release and removed no earlier than the next.
- Pre-1.0, a minor release may also change file formats; the loaders then refuse the older files with a clear
  message, or migrate them, and `docs/design/format_versions.txt` says which.

## What is supported

Proposed, for the maintainer to set:

| Line | Gets | For how long |
| --- | --- | --- |
| Latest minor | Everything | Until the next minor ships |
| Previous minor | Security and crash fixes | To be decided (suggested: 3 months) |
| A long-term-support minor | Security and crash fixes | To be decided (suggested: 12 months), chosen at 1.0 |

## What counts as a breaking change

- Removing or changing the signature of a name in a **stable** header. `tools/check_api_stability.py` blocks a
  removal in CI; a changed signature is caught in review.
- A saved file the previous release wrote that the new one cannot read.
- A plugin ABI bump (`kPluginAbi`). Plugins built for the old ABI are refused with a message, and the bump is
  listed in the release notes.

## Open questions

1. How long is "previous minor" supported, and is there a long-term-support line before 1.0?
2. Are platform backends (Vulkan, D3D12, mobile) covered by the same promise, or only the engine library?
3. Who decides a security fix is back-ported, and where is it announced?
