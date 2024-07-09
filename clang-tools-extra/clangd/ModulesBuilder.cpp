//===----------------- ModulesBuilder.cpp ------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ModulesBuilder.h"

#include "Compiler.h"
#include "support/Logger.h"

#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendActions.h"

#include "clang/Serialization/ASTReader.h"

namespace clang {
namespace clangd {

namespace {

// Create a path to store module files. Generally it should be:
//
//   {TEMP_DIRS}/clangd/module_files/{file-name}/.
//
// {TEMP_DIRS} is the temporary directory for the system, e.g., "/var/tmp"
// or "C:/TEMP".
//
// TODO: Move these module fils out of the temporary directory if the module
// files are persistent.
llvm::SmallString<256> getModuleFilesPath(PathRef MainFile) {
  llvm::SmallString<256> Result;

  llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/true, Result);

  llvm::sys::path::append(Result, "clangd");
  llvm::sys::path::append(Result, "module_files");
  llvm::sys::path::append(Result, MainFile);

  llvm::sys::fs::create_directories(Result);
  return Result;
}

// Get a unique module file path under \param ModuleFilesPrefix.
std::string getUniqueModuleFilePath(llvm::StringRef ModuleName,
                                    PathRef ModuleFilesPrefix) {
  llvm::SmallString<256> ModuleFilePathPattern(ModuleFilesPrefix);
  auto [PrimaryModuleName, PartitionName] = ModuleName.split(':');
  llvm::sys::path::append(ModuleFilePathPattern, PrimaryModuleName);
  if (!PartitionName.empty()) {
    ModuleFilePathPattern.append("-");
    ModuleFilePathPattern.append(PartitionName);
  }

  ModuleFilePathPattern.append("-%%-%%-%%-%%-%%-%%");

  llvm::SmallString<256> ModuleFilePath;
  llvm::sys::fs::createUniquePath(ModuleFilePathPattern, ModuleFilePath,
                                  /*MakeAbsolute=*/false);

  ModuleFilePath.append(".pcm");
  return std::string(ModuleFilePath);
}

// FailedPrerequisiteModules - stands for the PrerequisiteModules which has
// errors happened during the building process.
class FailedPrerequisiteModules : public PrerequisiteModules {
public:
  ~FailedPrerequisiteModules() override = default;

  // We shouldn't adjust the compilation commands based on
  // FailedPrerequisiteModules.
  void adjustHeaderSearchOptions(HeaderSearchOptions &Options) const override {
  }

  // FailedPrerequisiteModules can never be reused.
  bool
  canReuse(const CompilerInvocation &CI,
           llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>) const override {
    return false;
  }
};

struct ModuleFile {
  ModuleFile(StringRef ModuleName, PathRef ModuleFilePath)
      : ModuleName(ModuleName.str()), ModuleFilePath(ModuleFilePath.str()) {}

  ModuleFile() = delete;

  ModuleFile(const ModuleFile &) = delete;
  ModuleFile operator=(const ModuleFile &) = delete;

  // The move constructor is needed for llvm::SmallVector.
  ModuleFile(ModuleFile &&Other)
      : ModuleName(std::move(Other.ModuleName)),
        ModuleFilePath(std::move(Other.ModuleFilePath)) {
    Other.ModuleName.clear();
    Other.ModuleFilePath.clear();
  }

  ModuleFile &operator=(ModuleFile &&Other) {
    if (this == &Other)
      return *this;

    this->~ModuleFile();
    new (this) ModuleFile(std::move(Other));
    return *this;
  }

  ~ModuleFile() {
    if (!ModuleFilePath.empty())
      llvm::sys::fs::remove(ModuleFilePath);
  }

  std::string ModuleName;
  std::string ModuleFilePath;
};

bool IsModuleFileUpToDate(
    PathRef ModuleFilePath,
    const PrerequisiteModules &RequisiteModules) {
IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(new DiagnosticOptions());

  auto HSOpts = std::make_shared<HeaderSearchOptions>();
  RequisiteModules.adjustHeaderSearchOptions(*HSOpts);
  HSOpts->ForceCheckCXX20ModulesInputFiles = true;
  HSOpts->ValidateASTInputFilesContent = true;

  PCHContainerOperations PCHOperations;
  std::unique_ptr<ASTUnit> Unit = ASTUnit::LoadFromASTFile(
      ModuleFilePath.str(), PCHOperations.getRawReader(), ASTUnit::LoadASTOnly,
      Diags, FileSystemOptions(), std::move(HSOpts));

  if (!Unit)
    return false;

  auto Reader = Unit->getASTReader();
  if (!Reader)
    return false;

  bool UpToDate = true;
  Reader->getModuleManager().visit([&](serialization::ModuleFile &MF) -> bool {
    Reader->visitInputFiles(
        MF, /*IncludeSystem=*/false, /*Complain=*/false,
        [&](const serialization::InputFile &IF, bool isSystem) {
          if (!IF.getFile() || IF.isOutOfDate())
            UpToDate = false;
        });

    return !UpToDate;
  });

  return UpToDate;
}

bool IsModuleFilesUpToDate(
    llvm::SmallVector<PathRef> ModuleFilePaths,
    const PrerequisiteModules &RequisiteModules) {
  return llvm::all_of(ModuleFilePaths, [&RequisiteModules](auto ModuleFilePath) {
    return IsModuleFileUpToDate(ModuleFilePath, RequisiteModules);
  });
}

// ReusablePrerequisiteModules - stands for PrerequisiteModules for which all
// the required modules are built successfully. All the module files
// are owned by the modules builder.
class ReusablePrerequisiteModules : public PrerequisiteModules {
public:
  ReusablePrerequisiteModules() = default;

  ReusablePrerequisiteModules(const ReusablePrerequisiteModules &) = delete;
  ReusablePrerequisiteModules
  operator=(const ReusablePrerequisiteModules &) = delete;
  ReusablePrerequisiteModules(ReusablePrerequisiteModules &&) = delete;
  ReusablePrerequisiteModules
  operator=(ReusablePrerequisiteModules &&) = delete;

  ~ReusablePrerequisiteModules() override = default;

  void adjustHeaderSearchOptions(HeaderSearchOptions &Options) const override {
    // Appending all built module files.
    for (auto &RequiredModule : RequiredModules)
      Options.PrebuiltModuleFiles.insert_or_assign(
          RequiredModule->ModuleName, RequiredModule->ModuleFilePath);
  }

  bool
  canReuse(const CompilerInvocation &CI,
           llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>) const override {
    if (RequiredModules.empty())
      return true;

    SmallVector<StringRef> BMIPaths;
    for (auto &MF : RequiredModules)
      BMIPaths.push_back(MF->ModuleFilePath);
    return IsModuleFilesUpToDate(BMIPaths, *this);
  }

  bool isModuleUnitBuilt(llvm::StringRef ModuleName) const {
    return BuiltModuleNames.contains(ModuleName);
  }

  void addModuleFile(std::shared_ptr<ModuleFile> BMI) {
    BuiltModuleNames.insert(BMI->ModuleName);
    RequiredModules.emplace_back(std::move(BMI));
  }

  mutable llvm::SmallVector<std::shared_ptr<ModuleFile>, 8> RequiredModules;
  /// A helper class to speedup the query if a module is built.
  llvm::StringSet<> BuiltModuleNames;
};

// Build a module file for module with `ModuleName`. The information of built
// module file are stored in \param BuiltModuleFiles.
std::optional<ModuleFile>
buildModuleFile(llvm::StringRef ModuleName, PathRef ModuleUnitFileName,
                const GlobalCompilationDatabase &CDB, const ThreadsafeFS &TFS,
                PathRef ModuleFilesPrefix,
                ReusablePrerequisiteModules &BuiltModuleFiles) {
  // Try cheap operation earlier to boil-out cheaply if there are problems.
  auto Cmd = CDB.getCompileCommand(ModuleUnitFileName);
  if (!Cmd) {
    elog("Failed to build '{0}': No compile command for {1}", ModuleName,
         ModuleUnitFileName);
    return std::nullopt;
  }

  Cmd->Output = getUniqueModuleFilePath(ModuleName, ModuleFilesPrefix);

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(*Cmd);

  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  if (!CI) {
    elog("Failed to build '{0}': Failed to build compiler invocation for {1}",
         ModuleName, ModuleUnitFileName);
    return std::nullopt;
  }

  auto FS = Inputs.TFS->view(Inputs.CompileCommand.Directory);
  auto Buf = FS->getBufferForFile(Inputs.CompileCommand.Filename);
  if (!Buf) {
    elog("Failed to build '{0}': Failed to create buffer for {1}", ModuleName,
         Inputs.CompileCommand.Filename);
    return std::nullopt;
  }

  // Hash the contents of input files and store the hash value to the BMI files.
  // So that we can check if the files are still valid when we want to reuse the
  // BMI files.
  CI->getHeaderSearchOpts().ValidateASTInputFilesContent = true;

  BuiltModuleFiles.adjustHeaderSearchOptions(CI->getHeaderSearchOpts());

  CI->getFrontendOpts().OutputFile = Inputs.CompileCommand.Output;
  auto Clang =
      prepareCompilerInstance(std::move(CI), /*Preamble=*/nullptr,
                              std::move(*Buf), std::move(FS), IgnoreDiags);
  if (!Clang) {
    elog("Failed to build '{0}': Failed to prepare compiler instance for {0}",
         ModuleName, ModuleUnitFileName);
    return std::nullopt;
  }

  GenerateReducedModuleInterfaceAction Action;
  Clang->ExecuteAction(Action);

  if (Clang->getDiagnostics().hasErrorOccurred()) {
    elog("Failed to build '{0}': Compilation for {1} failed", ModuleName,
         ModuleUnitFileName);
    return std::nullopt;
  }

  return ModuleFile{ModuleName, Inputs.CompileCommand.Output};
}

class ReusableModulesBuilder : public ModulesBuilder {
public:
  ReusableModulesBuilder(const GlobalCompilationDatabase &CDB) : CDB(CDB) {}

  ReusableModulesBuilder(const ReusableModulesBuilder &) = delete;
  ReusableModulesBuilder(ReusableModulesBuilder &&) = delete;

  ReusableModulesBuilder &operator=(const ReusableModulesBuilder &) = delete;
  ReusableModulesBuilder &operator=(ReusableModulesBuilder &&) = delete;

  std::unique_ptr<PrerequisiteModules>
  buildPrerequisiteModulesFor(PathRef File, const ThreadsafeFS &TFS) override;

private:
  bool getOrBuildModuleFile(StringRef ModuleName, const ThreadsafeFS &TFS,
                            ProjectModules &MDB,
                            ReusablePrerequisiteModules &RequiredModules);

  std::shared_ptr<ModuleFile>
  getValidModuleFile(StringRef ModuleName, ProjectModules &MDB,
                     const ThreadsafeFS &TFS,
                     PrerequisiteModules &BuiltModuleFiles);
  /// This should only be called by getValidModuleFile. This is unlocked version
  /// of getValidModuleFile. This is extracted to avoid dead locks when
  /// recursing.
  std::shared_ptr<ModuleFile>
  isValidModuleFileUnlocked(StringRef ModuleName, ProjectModules &MDB,
                            const ThreadsafeFS &TFS,
                            PrerequisiteModules &BuiltModuleFiles);

  llvm::StringMap<std::shared_ptr<ModuleFile>> ModuleFiles;
  std::mutex ModuleFilesMutex;

  // We should only build a unique module at most at the same time.
  // When we want to build a module
  llvm::StringMap<std::shared_ptr<std::mutex>> BuildingModuleMutexes;
  llvm::StringMap<std::shared_ptr<std::condition_variable>> BuildingModuleCVs;
  // The building modules set. A successed built module or a failed module or
  // a unbuilt module shouldn't be in this set.
  // This set is helpful to control the behavior of the condition variables.
  llvm::StringSet<> BuildingModules;
  // Lock when we accessing ModuleBuildingCVs and ModuleBuildingMutexes.
  std::mutex ModulesBuildingMutex;

  void startBuildingModule(StringRef ModuleName) {
    std::lock_guard<std::mutex> _(ModulesBuildingMutex);
    BuildingModules.insert(ModuleName);
  }
  void endBuildingModule(StringRef ModuleName) {
    std::lock_guard<std::mutex> _(ModulesBuildingMutex);
    BuildingModules.erase(ModuleName);
  }
  bool isBuildingModule(StringRef ModuleName) {
    std::lock_guard<std::mutex> _(ModulesBuildingMutex);
    return BuildingModules.contains(ModuleName);
  }

  /// An RAII object to guard the process to build a specific module.
  struct ModuleBuildingSharedOwner {
  public:
    ModuleBuildingSharedOwner(StringRef ModuleName,
                              std::shared_ptr<std::mutex> &Mutex,
                              std::shared_ptr<std::condition_variable> &CV,
                              ReusableModulesBuilder &Builder)
        : ModuleName(ModuleName), Mutex(Mutex), CV(CV), Builder(Builder) {
      IsFirstTask = (Mutex.use_count() == 2);
    }

    ~ModuleBuildingSharedOwner();

    bool isUniqueBuildingOwner() { return IsFirstTask; }

    std::mutex &getMutex() { return *Mutex; }

    std::condition_variable &getCV() { return *CV; }

  private:
    StringRef ModuleName;
    std::shared_ptr<std::mutex> Mutex;
    std::shared_ptr<std::condition_variable> CV;
    ReusableModulesBuilder &Builder;
    bool IsFirstTask;
  };

  ModuleBuildingSharedOwner
  getOrCreateModuleBuildingCVAndLock(StringRef ModuleName);

  const GlobalCompilationDatabase &CDB;
};

ReusableModulesBuilder::ModuleBuildingSharedOwner::
    ~ModuleBuildingSharedOwner() {
  std::lock_guard<std::mutex> _(Builder.ModulesBuildingMutex);

  Mutex.reset();
  CV.reset();

  // Try to release the memory in builder if possible.
  if (auto Iter = Builder.BuildingModuleCVs.find(ModuleName);
      Iter != Builder.BuildingModuleCVs.end() &&
      Iter->getValue().use_count() == 1)
    Builder.BuildingModuleCVs.erase(Iter);

  if (auto Iter = Builder.BuildingModuleMutexes.find(ModuleName);
      Iter != Builder.BuildingModuleMutexes.end() &&
      Iter->getValue().use_count() == 1)
    Builder.BuildingModuleMutexes.erase(Iter);
}

std::shared_ptr<ModuleFile> ReusableModulesBuilder::isValidModuleFileUnlocked(
    StringRef ModuleName, ProjectModules &MDB, const ThreadsafeFS &TFS,
    PrerequisiteModules &BuiltModuleFiles) {
  auto Iter = ModuleFiles.find(ModuleName);
  if (Iter != ModuleFiles.end()) {
    if (!IsModuleFileUpToDate(Iter->second->ModuleFilePath, BuiltModuleFiles)) {
      log("Found not-up-date module file {0} for module {1} in cache",
          Iter->second->ModuleFilePath, ModuleName);
      ModuleFiles.erase(Iter);
      return nullptr;
    }

    if (llvm::any_of(
            MDB.getRequiredModules(MDB.getSourceForModuleName(ModuleName)),
            [&MDB, &TFS, &BuiltModuleFiles, this](auto &&RequiredModuleName) {
              return !isValidModuleFileUnlocked(RequiredModuleName, MDB, TFS,
                                                BuiltModuleFiles);
            })) {
      ModuleFiles.erase(Iter);
      return nullptr;
    }

    return Iter->second;
  }

  log("Don't find {0} in cache", ModuleName);

  return nullptr;
}

std::shared_ptr<ModuleFile> ReusableModulesBuilder::getValidModuleFile(
    StringRef ModuleName, ProjectModules &MDB, const ThreadsafeFS &TFS,
    PrerequisiteModules &BuiltModuleFiles) {
  std::lock_guard<std::mutex> _(ModuleFilesMutex);

  return isValidModuleFileUnlocked(ModuleName, MDB, TFS, BuiltModuleFiles);
}

std::unique_ptr<PrerequisiteModules>
ReusableModulesBuilder::buildPrerequisiteModulesFor(PathRef File,
                                                    const ThreadsafeFS &TFS) {
  std::unique_ptr<ProjectModules> MDB = CDB.getProjectModules(File);
  if (!MDB) {
    elog("Failed to get Project Modules information for {0}", File);
    return std::make_unique<FailedPrerequisiteModules>();
  }

  std::vector<std::string> RequiredModuleNames = MDB->getRequiredModules(File);
  if (RequiredModuleNames.empty())
    return std::make_unique<ReusablePrerequisiteModules>();

  log("Trying to build required modules for {0}", File);

  auto RequiredModules = std::make_unique<ReusablePrerequisiteModules>();

  for (const std::string &RequiredModuleName : RequiredModuleNames)
    // Return early if there is any error.
    if (!getOrBuildModuleFile(RequiredModuleName, TFS, *MDB.get(),
                              *RequiredModules.get())) {
      elog("Failed to build module {0};", RequiredModuleName);
      return std::make_unique<FailedPrerequisiteModules>();
    }

  log("Built required modules for {0}", File);

  return std::move(RequiredModules);
}

ReusableModulesBuilder::ModuleBuildingSharedOwner
ReusableModulesBuilder::getOrCreateModuleBuildingCVAndLock(
    StringRef ModuleName) {
  std::lock_guard<std::mutex> _(ModulesBuildingMutex);

  auto MutexIter = BuildingModuleMutexes.find(ModuleName);
  if (MutexIter == BuildingModuleMutexes.end())
    MutexIter = BuildingModuleMutexes
                    .try_emplace(ModuleName, std::make_shared<std::mutex>())
                    .first;

  auto CVIter = BuildingModuleCVs.find(ModuleName);
  if (CVIter == BuildingModuleCVs.end())
    CVIter = BuildingModuleCVs
                 .try_emplace(ModuleName,
                              std::make_shared<std::condition_variable>())
                 .first;

  return ModuleBuildingSharedOwner(ModuleName, MutexIter->getValue(),
                                   CVIter->getValue(), *this);
}

bool ReusableModulesBuilder::getOrBuildModuleFile(
    StringRef ModuleName, const ThreadsafeFS &TFS, ProjectModules &MDB,
    ReusablePrerequisiteModules &BuiltModuleFiles) {
  if (BuiltModuleFiles.isModuleUnitBuilt(ModuleName))
    return true;

  PathRef ModuleUnitFileName = MDB.getSourceForModuleName(ModuleName);
  /// It is possible that we're meeting third party modules (modules whose
  /// source are not in the project. e.g, the std module may be a third-party
  /// module for most project) or something wrong with the implementation of
  /// ProjectModules.
  /// FIXME: How should we treat third party modules here? If we want to ignore
  /// third party modules, we should return true instead of false here.
  /// Currently we simply bail out.
  if (ModuleUnitFileName.empty()) {
    log("Don't get the module unit for module {0}", ModuleName);
    return false;
  }

  for (auto &RequiredModuleName : MDB.getRequiredModules(ModuleUnitFileName)) {
    // Return early if there are errors building the module file.
    if (!getOrBuildModuleFile(RequiredModuleName, TFS, MDB, BuiltModuleFiles)) {
      log("Failed to build module {0}", RequiredModuleName);
      return false;
    }
  }

  if (std::shared_ptr<ModuleFile> Cached =
          getValidModuleFile(ModuleName, MDB, TFS, BuiltModuleFiles)) {
    log("Reusing module {0} from {1}", ModuleName, Cached->ModuleFilePath);
    BuiltModuleFiles.addModuleFile(Cached);
    return true;
  }

  ModuleBuildingSharedOwner ModuleBuildingOwner =
      getOrCreateModuleBuildingCVAndLock(ModuleName);

  std::condition_variable &CV = ModuleBuildingOwner.getCV();
  std::unique_lock<std::mutex> lk(ModuleBuildingOwner.getMutex());
  if (!ModuleBuildingOwner.isUniqueBuildingOwner()) {
    log("Waiting other task for module {0}", ModuleName);
    CV.wait(lk, [this, ModuleName] { return !isBuildingModule(ModuleName); });

    // Try to access the built module files from other threads manually.
    // We don't call getValidModuleFile here since it may be too heavy.
    std::lock_guard<std::mutex> _(ModuleFilesMutex);
    auto Iter = ModuleFiles.find(ModuleName);
    if (Iter != ModuleFiles.end()) {
      log("Got module file from other task building {0}", ModuleName);
      BuiltModuleFiles.addModuleFile(Iter->second);
      return true;
    }

    // If the module file is not in the cache, it indicates that the building
    // from other thread failed, so we give up earlier in this case to avoid
    // wasting time.
    return false;
  }

  log("Building module {0}", ModuleName);
  startBuildingModule(ModuleName);

  llvm::SmallString<256> ModuleFilesPrefix =
      getModuleFilesPath(ModuleUnitFileName);

  std::optional<ModuleFile> MF =
      buildModuleFile(ModuleName, ModuleUnitFileName, CDB, TFS,
                      ModuleFilesPrefix, BuiltModuleFiles);

  bool BuiltSuccessed = (bool)MF;
  if (MF) {
    log("Built module {0} to {1}", ModuleName, MF->ModuleFilePath);
    std::lock_guard<std::mutex> _(ModuleFilesMutex);
    auto BuiltModuleFile = std::make_shared<ModuleFile>(std::move(*MF));
    ModuleFiles.insert_or_assign(ModuleName, BuiltModuleFile);
    BuiltModuleFiles.addModuleFile(std::move(BuiltModuleFile));
  } else {
    log("Failed to build module {0}", ModuleName);
  }

  endBuildingModule(ModuleName);
  CV.notify_all();
  return BuiltSuccessed;
}

} // namespace

std::unique_ptr<ModulesBuilder>
ModulesBuilder::getModulesBuilder(const GlobalCompilationDatabase &CDB) {
  return std::make_unique<ReusableModulesBuilder>(CDB);
}

} // namespace clangd
} // namespace clang
