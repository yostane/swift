//===--- ParseableInterfaceModuleLoader.h - Loads .swiftinterface files ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
/// \file This implements the logic for loading and building parseable module
/// interfaces.
///
/// === Loading Parseable Modules ===
///
/// If there is a .swiftinterface file corresponding to a given module name
/// present in the frontend's search paths, then this module loader will look in
/// the following places for a module:
///
/// - First, look in the module cache (specified by -module-cache-path)
///   - We check here first because an existing .swiftmodule might be
///     out-of-date, necessitating a rebuild. If a cached module is out-of-date,
///     it's simply rebuilt.
/// - Next, look adjacent to the .swiftinterface. If we find a module that's
///   either loadable by this compiler, valid, and up-to-date, or totally
///   unreadable, then delegate to the serialized module loader to either load
///   or diagnose it.
/// - Finally, look in the prebuilt module cache (specified
///   by -prebuilt-module-cache-path)
///
/// If we can't find an appropriate module to load, we can always fall back and
/// recompile the .swiftinterface file.
///
/// === Dependency Checking ===
///
/// Cached modules keep track of their dependencies' last modification time and
/// file size. This means that checking if a module is up-to-date requires
/// `stat`ing the dependencies and comparing the results from the filesystem
/// with the results in the module.
///
/// Prebuilt modules, on the other hand, won't have a reliable modification
/// time, as their dependencies live in the SDK. Prebuilt modules will instead
/// keep track of the size and content hash of their dependencies.
/// In order to avoid constantly re-hashing the dependencies, however, we will
/// install a "forwarding module" in the regular cache.
/// This "forwarding module"
///  - Points to the prebuilt module on disk, and
///  - Lists modification times from the last time we verified the content
///
/// So, to recap, there are 4 kinds of modules:
/// ┌───────────────────────────────┐
/// │     ┌───┐           ┌───┐     │
/// │     │ I │           │ M │     │
/// │     └───┘           └───┘     │
/// │ .swiftinterface  .swiftmodule │
/// │     ┌───┐           ┌───┐     │
/// │     │ P │           │ F │     │
/// │     └───┘           └───┘     │
/// │   Prebuilt        Forwarding  │
/// │ .swiftmodule     .swiftmodule │
/// └───────────────────────────────┘
///
/// - Prebuilt modules have hash-based dependencies, cached modules have
///   mod-time-based dependencies
/// - Forwarding modules point to prebuilt modules and augment them with
///   modification times
///
/// === Example Cache ===
///
/// Here's an example of what's in a prebuilt cache or module cache.
///
/// Say there are 4 frameworks, each exposing a .swiftinterface file.
/// Then, we pre-build 2 of those frameworks and put them in the prebuilt cache.
/// Finally, we import all 4 of those frameworks while building a project.
///
/// For the 2 frameworks with modules in the prebuilt cache, we'll have
/// installed 2 forwarding modules. For the other 2 frameworks, we'll have
/// compiled the interfaces and put them in the module cache.
///
///                                ┌─────┐
///  ┌─────────────────────────────┤ SDK ├─────────────────────────┐
///  │          ┌────────────────┐ └─────┘     ┌────────────────┐  │
///  │  ┌───────┤ Framework Dirs ├────────┐   ┌┤ Prebuilt Cache ├┐ │
///  │  │       └────────────────┘        │   │└────────────────┘│ │
///  │  │  ┌───┐   ┌───┐   ┌───┐   ┌───┐  │   │   ┌───┐  ┌───┐   │ │
///  │  │  │ I │   │ I │   │ I │   │ I │◀─┼───┼───│ P │  │ P │◀═╗│ │
///  │  │  └───┘   └───┘   └───┘   └───┘  │   │   └───┘  └───┘  ║│ │
///  │  │    ▲       ▲       ▲            │   │     ▲      │    ║│ │
///  │  └────┼───────┼───────┼────────────┘   └─────╫──────┼────╫┘ │
///  │       │       │       └──────────────────────╫──────┘    ║  │
///  └───────┼───────┼──────────────────────────────╫───────────╫──┘
///          │       │   ┌───────────────┐          ║           ║
///          │  ┌────┼───┤ Module Cache  ├────────┐ ║           ║
///          │  │    │   └───────────────┘        │ ║           ║
///          │  │  ┌───┐   ┌───┐   ┌───┐   ┌───┐  │ ║           ║
///          │  │  │ M │   │ M │   │ F │   │ F │  │ ║           ║
///          │  │  └───┘   └───┘   └───┘   └───┘  │ ║           ║
///          │  │            │       ║       ╚════╪═╝           ║
///          │  └────────────┼───────╫────────────┘             ║
///          └───────────────┘       ╚══════════════════════════╝
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/LLVM.h"
#include "swift/Frontend/ParseableInterfaceSupport.h"
#include "swift/Serialization/SerializedModuleLoader.h"

namespace clang {
  class CompilerInstance;
}

namespace unittest {
  class ParseableInterfaceModuleLoaderTest;
}

namespace swift {

/// A ModuleLoader that runs a subordinate \c CompilerInvocation and
/// \c CompilerInstance to convert .swiftinterface files to .swiftmodule
/// files on the fly, caching the resulting .swiftmodules in the module cache
/// directory, and loading the serialized .swiftmodules from there.
class ParseableInterfaceModuleLoader : public SerializedModuleLoaderBase {
  friend class unittest::ParseableInterfaceModuleLoaderTest;
  explicit ParseableInterfaceModuleLoader(ASTContext &ctx, StringRef cacheDir,
                                          StringRef prebuiltCacheDir,
                                          DependencyTracker *tracker,
                                          ModuleLoadingMode loadMode)
  : SerializedModuleLoaderBase(ctx, tracker, loadMode),
  CacheDir(cacheDir), PrebuiltCacheDir(prebuiltCacheDir)
  {}

  std::string CacheDir;
  std::string PrebuiltCacheDir;

  std::error_code findModuleFilesInDirectory(
    AccessPathElem ModuleID, StringRef DirPath, StringRef ModuleFilename,
    StringRef ModuleDocFilename,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer) override;

  bool isCached(StringRef DepPath) override;

public:
  static std::unique_ptr<ParseableInterfaceModuleLoader>
  create(ASTContext &ctx, StringRef cacheDir, StringRef prebuiltCacheDir,
         DependencyTracker *tracker, ModuleLoadingMode loadMode) {
    return std::unique_ptr<ParseableInterfaceModuleLoader>(
      new ParseableInterfaceModuleLoader(ctx, cacheDir, prebuiltCacheDir,
                                         tracker, loadMode));
  }

  /// Unconditionally build \p InPath (a swiftinterface file) to \p OutPath (as
  /// a swiftmodule file).
  ///
  /// A simplified version of the core logic in #openModuleFiles, mostly for
  /// testing purposes.
  static bool buildSwiftModuleFromSwiftInterface(
    ASTContext &Ctx, StringRef CacheDir, StringRef PrebuiltCacheDir,
    StringRef ModuleName, StringRef InPath, StringRef OutPath,
    bool SerializeDependencyHashes, bool TrackSystemDependencies);
};

/// Extract the specified-or-defaulted -module-cache-path that winds up in
/// the clang importer, for reuse as the .swiftmodule cache path when
/// building a ParseableInterfaceModuleLoader.
std::string
getModuleCachePathFromClang(const clang::CompilerInstance &Instance);

}