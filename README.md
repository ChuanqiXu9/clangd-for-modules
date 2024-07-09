A preview for C++20 Named Modules Support in clangd.

This repo is created for users who would like to use modules with clangd
in the early days. Feedbacks and contributions are welcomed.

We still wish to contribute this to upstream.

Following the instructions here to [build clangd from source](https://github.com/llvm/llvm-project/tree/main/clang-tools-extra/clangd#building-and-testing-clangd).

A compile commands file is neceesary to use this with modules.

An important assumption for this to work correctly is that there is no
multiple module interface units declare the same module unit name. The standard forbids the case where duplicated module units occurs in a program.
But it is technically fine to have duplicated module units in a project as
long as they won't be linked together.

To boost the process of scanning, users can provide a module map file to the clangd via the `--modules-map-path=<path-to-module-map-file>`. This is
not necessary, but the clangd may scan all of the projects to figure out
what is the source file to provide one specific module.

The syntax for the module map file is:

```
# Comment lines starting with #
# '#' not starting at the head of the line doesn't work
# Users should provide one <module-name, path-to-source-file> pair one line,
# separated by a space.
# If the provided path is not an absolute path, it will be concatenated with
# the location of the module map file.
module_name path_to_source_file
```
