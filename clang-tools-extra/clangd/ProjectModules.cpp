//===------------------ ProjectModules.h -------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ProjectModules.h"

#include "support/Logger.h"

#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"

namespace clang::clangd {
namespace {
/// A scanner to query the dependency information for C++20 Modules.
///
/// The scanner can scan a single file with `scan(PathRef)` member function
/// or scan the whole project with `globalScan(vector<PathRef>)` member
/// function. See the comments of `globalScan` to see the details.
///
/// The ModuleDependencyScanner can get the directly required module names for a
/// specific source file. Also the ModuleDependencyScanner can get the source
/// file declaring the primary module interface for a specific module name.
///
/// IMPORTANT NOTE: we assume that every module unit is only declared once in a
/// source file in the project. But the assumption is not strictly true even
/// besides the invalid projects. The language specification requires that every
/// module unit should be unique in a valid program. But a project can contain
/// multiple programs. Then it is valid that we can have multiple source files
/// declaring the same module in a project as long as these source files don't
/// interfere with each other.
class ModuleDependencyScanner {
public:
  ModuleDependencyScanner()
      : Service(tooling::dependencies::ScanningMode::CanonicalPreprocessing,
                tooling::dependencies::ScanningOutputFormat::P1689) {}

  /// The scanned modules dependency information for a specific source file.
  struct ModuleDependencyInfo {
    /// The name of the module if the file is a module unit.
    std::optional<std::string> ModuleName;
    /// A list of names for the modules that the file directly depends.
    std::vector<std::string> RequiredModules;
  };

  /// Scanning the single file specified by \param FilePath.
  std::optional<ModuleDependencyInfo> scan(PathRef FilePath);

  /// Get the source file from the module name. Note that the language
  /// guarantees all the module names are unique in a valid program.
  /// This function should only be called after globalScan.
  ///
  /// TODO: We should handle the case that there are multiple source files
  /// declaring the same module.
  PathRef getSourceForModuleNameSlow(llvm::StringRef ModuleName);

  /// Return the direct required modules. Indirect required modules are not
  /// included.
  std::vector<std::string> getRequiredModules(PathRef File);

  void update(std::shared_ptr<const clang::tooling::CompilationDatabase> CDB,
              const ThreadsafeFS &TFS) {
    this->CDB = CDB;
    this->TFS = &TFS;
  }

private:
  std::shared_ptr<const clang::tooling::CompilationDatabase> CDB;
  const ThreadsafeFS *TFS;

  clang::tooling::dependencies::DependencyScanningService Service;
};

std::optional<ModuleDependencyScanner::ModuleDependencyInfo>
ModuleDependencyScanner::scan(PathRef FilePath) {
  if (!CDB) {
    elog("We need to assign Compilation to Database before scanning.");
    return std::nullopt;
  }

  auto Candidates = CDB->getCompileCommands(FilePath);
  if (Candidates.empty())
    return std::nullopt;

  // Choose the first candidates as the compile commands as the file.
  // Following the same logic with
  // DirectoryBasedGlobalCompilationDatabase::getCompileCommand.
  tooling::CompileCommand Cmd = std::move(Candidates.front());

  using namespace clang::tooling::dependencies;

  llvm::SmallString<128> FilePathDir(FilePath);
  llvm::sys::path::remove_filename(FilePathDir);
  DependencyScanningTool ScanningTool(Service, TFS->view(FilePathDir));

  llvm::Expected<P1689Rule> ScanningResult =
      ScanningTool.getP1689ModuleDependencyFile(Cmd, Cmd.Directory);

  if (auto E = ScanningResult.takeError()) {
    elog("Scanning modules dependencies for {0} failed: {1}", FilePath,
         llvm::toString(std::move(E)));
    return std::nullopt;
  }

  ModuleDependencyInfo Result;

  if (ScanningResult->Provides)
    Result.ModuleName = ScanningResult->Provides->ModuleName;

  for (auto &Required : ScanningResult->Requires)
    Result.RequiredModules.push_back(Required.ModuleName);

  return Result;
}

PathRef ModuleDependencyScanner::getSourceForModuleNameSlow(
    llvm::StringRef ModuleName) {
  if (!CDB) {
    elog("We need to assign Compilation to Database before scanning.");
    return {};
  }

  for (auto &File : CDB->getAllFiles())
    if (auto ScanningResult = scan(File))
      if (ScanningResult->ModuleName == ModuleName)
        return File;

  return {};
}

std::vector<std::string>
ModuleDependencyScanner::getRequiredModules(PathRef File) {
  auto ScanningResult = scan(File);
  if (!ScanningResult)
    return {};

  return ScanningResult->RequiredModules;
}
} // namespace

class ProjectModulesImpl : public ProjectModules {
public:
  ProjectModulesImpl() = default;

  ~ProjectModulesImpl() override = default;

  std::vector<std::string> getRequiredModules(PathRef File) override {
    return Scanner.getRequiredModules(File);
  }

  std::string getSourceForModuleName(llvm::StringRef ModuleName) override {
    auto Iter = ModuleNameSourcesCache.find(ModuleName);
    if (Iter != ModuleNameSourcesCache.end()) {
      // We need to verify it since the cache is not stable - it'll
      // be meaningless if the users change its content.
      if (verifyModuleUnit(Iter->second, ModuleName))
        return Iter->second;

      // If it is not valid, we need to clear it.
      ModuleNameSourcesCache.erase(Iter);
    }

    // Fallback to slow case.
    PathRef ResultPath = Scanner.getSourceForModuleNameSlow(ModuleName);
    if (!ResultPath.empty())
      ModuleNameSourcesCache.insert_or_assign(ModuleName, ResultPath);

    return ResultPath.str();
  }

  void update(std::shared_ptr<const clang::tooling::CompilationDatabase> CDB,
              const ThreadsafeFS &TFS) override {
    Scanner.update(CDB, TFS);
  }

private:
  /// Check if @param File declares a module with name @param ModuleName.
  bool verifyModuleUnit(PathRef File, llvm::StringRef ModuleName) {
    auto ScanningResult = Scanner.scan(File);
    if (!ScanningResult)
      return false;

    return ScanningResult->ModuleName == ModuleName;
  }

  ModuleDependencyScanner Scanner;

  llvm::StringMap<std::string> ModuleNameSourcesCache;
};

std::unique_ptr<ProjectModules> ProjectModules::getProjectModules() {
  return std::make_unique<ProjectModulesImpl>();
}

} // namespace clang::clangd
