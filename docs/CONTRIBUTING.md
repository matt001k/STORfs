# Contributing Guidelines

Thank you for your interest in contributing to STORfs!
This document provides guidelines and instructions for contributing.

## Table of Contents

- [Bug Reporting](#bug-reporting)
- [Feature Requests](#feature-requests)
- [Development Setup](#development-setup)
- [Coding Standards](#coding-standards)
- [Pull Request Process](#pull-request-process)
- [Project Vision](#project-vision)

## Bug Reporting

- Report bugs using [GitHub Issues](../../issues).
- Include a clear description of the problem and steps to reproduce it.
- Provide details about your environment (OS, compiler version, target architecture).
- If possible, include a minimal code example that demonstrates the issue.

## Feature Requests

- Suggest new features using [GitHub Issues](../../issues).
- Describe the use case and expected behavior.
- Explain why the feature would be beneficial to the project.

## Development Setup

1. Fork and clone the repository.
2. The following tools are needed to contribute:
  - [Docker](https://www.docker.com/)
  - [ClangFormat](https://clang.llvm.org/docs/ClangFormat.html)
    - Most Linux distributions offer this as a standalone package
  - [Make](https://www.gnu.org/software/make/)
3. Build and run unit test:
   ```bash
   cd test
   make
   ```
   The unit tests are built around [ceedling](https://www.throwtheswitch.org/ceedling)'s test framework.

## Coding Standards

- Follow the formatting rules defined in `.clang-format`.
  - View [ClangFormat](https://clang.llvm.org/docs/ClangFormat.html) for usage,
  can also be added as a plugin for most IDEs.
- Keep functions focused and well-documented.
- Use descriptive variable and function names.
- Ensure all new code compiles without warnings.
- Write unit tests for new functionality.

## Pull Request Process

1. Create a feature branch from `develop`:
   ```bash
   git checkout -b feat/your-feature develop
   ```
2. Make your changes and ensure they compile cleanly.
3. Run the test suite and confirm all tests pass.
4. Commit your changes with clear, descriptive commit messages.
5. Push your branch and open a pull request against `develop`.
6. Describe your changes in the PR description and link any related issues.
7. Address any review feedback promptly.

## Project Vision

STORfs aims to continually improve as a file system.
Contributions that fix bugs, improve performance,
add tests, or introduce well-motivated features are all welcome.
Planned improvements are opened as issues in the repository.

## Contact

If you would like to contribute as a maintainer, please reach out via [Email](mailto:kraus2mj@gmail.com).
Compiled library size data for different processor architectures is also appreciated.
