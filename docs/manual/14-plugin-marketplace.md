# Plugin registry and marketplace format

Plugins can contain native modules, so a registry entry is executable code. A package must be inspected and its
publisher trusted before it is installed or enabled. The engine does not sandbox native plugin code.

## Local registry

`aether::plugin::LocalPluginRegistry` provides a small, offline package store. The chosen directory contains one
folder per plugin and one immutable package per version:

```text
<registry>/
  Coins/
    1.2.0.apkg
    1.3.0.apkg
  Weather/
    2.0.apkg
```

Each package remains the source of truth for its name, version, ABI, engine requirement and dependencies. Registry
paths must agree with the package descriptor. Versions use numeric dotted form, matching the engine's current
version comparison (`1.10` is newer than `1.9`). Existing versions cannot be overwritten through the API.

```cpp
#include "aether/plugin/registry.h"

aether::plugin::LocalPluginRegistry registry(".aether/plugin-registry");
std::string error;

// Add a previously inspected package; publishing an existing name/version fails.
if (!registry.Publish("build/Coins.apkg", &error)) {
    // Show error to the user.
}

std::vector<std::string> warnings;
for (const auto& package : registry.List(&warnings)) {
    // package.descriptor has the verified package metadata.
}

// Install one explicit version. Dependencies are still inspected and installed separately.
if (!registry.Install("Coins", "1.2.0", "MyGame/Plugins", false, &error)) {
    // Show error to the user.
}
```

The registry copies packages without extracting them. Installation uses the staged `InstallPluginPackage` path,
which validates archive paths, the descriptor, ABI and unpacked size before replacing a plugin folder. Required
dependencies are not installed automatically; check `UnmetDependencies` and choose versions explicitly.

## Interchange metadata

A hosted registry can provide a versioned JSON catalog alongside `.apkg` files. The catalog is a discovery hint;
the descriptor inside each archive remains authoritative. A catalog entry should include:

- schema version, plugin name and version;
- package filename, byte length and SHA-256 digest;
- plugin ABI and minimum engine version;
- required and optional dependencies with minimum versions;
- license identifier, publisher identity, source repository and release notes;
- review state and the timestamp of the last review.

Consumers must validate catalog shape and size, verify the digest before inspection, then validate the archive
descriptor against the catalog. Installations should record exact plugin versions and digests in a project lock
file. Dependency resolution must report missing or conflicting requirements and must not download or enable a
dependency silently.

## Trust, review and signing

The eventual marketplace should define publisher identity, signature format and key rotation, revocation,
catalog rollback protection, license policy, review criteria and an appeal path before it accepts public uploads.
Signatures should cover canonical catalog metadata and the package digest. A client should show publisher,
permissions or capabilities, dependencies, license and review status before installation. A successful signature
proves who signed bytes; it does not prove the plugin is safe.

Hosting and signing authority are still maintainer decisions. The local registry intentionally has no network
protocol, accounts, signature trust roots, automatic dependency downloads or global catalog. A local folder is
trusted only to the extent that its owner and the packages placed there are trusted.
