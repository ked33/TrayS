# Repository Instructions

- Do not run MSBuild, Visual Studio builds, or any other local compilation or linking command.
- Validate changes with non-building static checks, then push them to the remote branch.
- Treat the GitHub Actions `Build x64` workflow and its `Release|x64` job as the source of truth for compilation.
- Preserve unrelated user changes in a dirty worktree.
