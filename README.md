# Toolchain Registry

Toolchain Registry is a repository that serves as a central registry for maintaining and versioning toolchains used in our software development projects, such as [Yatool](https://github.com/yandex/yatool).

This repository contains packages with essential patches, sources and other scripts in order to reproduce the build of our toolchains.

## Usage
We mainly use [stal-ix/ix](https://stal-ix.github.io) as a package manager.

[Yatool](https://github.com/yandex/yatool) is a toolkit designed for monorepositories. Almost every Yandex open source repository contains its `.mapping.conf.json` file, which stores information necessary to use a particular toolchain.

The `ynd` contains a folders set, each package corresponds to one folder. An `ix.sh` script is included in each folder, as it is required by the package manager.

## Releases
The release page is synchronized with our internal release process. For each release, there is a detailed description of how to reproduce the build of particular toolchain. 

Additionally, every release is related to a commit, where you can find the changelog of the current toolchain. This ensures transparency and accountability in the development process, allowing users to track changes and understand the evolution of the toolchains available on the registry. 

Stay informed, stay updated, and make the most of each release by checking out the detailed information provided for each toolchain release.
