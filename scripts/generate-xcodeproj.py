#!/usr/bin/env python3
"""Generates App/ChiptuneTracker.xcodeproj.

The app target compiles App/Sources/*.swift and depends on the TrackerAudio
product of the repo's Swift package (a local package reference to ".."), so all
engine code lives in the package and the project stays tiny. Object IDs are
derived from stable names, so regenerating produces identical output.

Run after adding or removing files in App/Sources:
    scripts/generate-xcodeproj.py
"""
from pathlib import Path
import hashlib

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "App"
PROJECT = APP / "ChiptuneTracker.xcodeproj"
NAME = "ChiptuneTracker"
BUNDLE_ID = "com.joryshilmover.ChiptuneTracker"
PACKAGE_PRODUCT = "TrackerAudio"
DEPLOYMENT_TARGET = "18.0"


def ident(key: str) -> str:
    return hashlib.sha1(key.encode()).hexdigest()[:24].upper()


def q(value) -> str:
    s = str(value)
    if s and all(c.isalnum() or c in "._/" for c in s):
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def fmt(value, indent: int) -> str:
    pad = "\t" * indent
    if isinstance(value, dict):
        inner = "".join(f"{pad}\t{q(k)} = {fmt(v, indent + 1)};\n" for k, v in value.items())
        return "{\n" + inner + pad + "}"
    if isinstance(value, list):
        inner = "".join(f"{pad}\t{fmt(v, indent + 1)},\n" for v in value)
        return "(\n" + inner + pad + ")"
    return q(value)


objects: dict[str, dict] = {}


def add(key: str, body: dict) -> str:
    objects[ident(key)] = body
    return ident(key)


sources = sorted(p for p in (APP / "Sources").glob("*.swift"))
file_refs, build_files = [], []
for path in sources:
    rel = path.relative_to(APP).as_posix()
    ref = add("file:" + rel, {"isa": "PBXFileReference", "lastKnownFileType": "sourcecode.swift",
                              "path": path.name, "sourceTree": "<group>"})
    file_refs.append(ref)
    build_files.append(add("build:" + rel, {"isa": "PBXBuildFile", "fileRef": ref}))

package_ref = add("package", {"isa": "XCLocalSwiftPackageReference", "relativePath": ".."})
product_dep = add("product-dep", {"isa": "XCSwiftPackageProductDependency", "package": package_ref,
                                  "productName": PACKAGE_PRODUCT})
product_build = add("product-build", {"isa": "PBXBuildFile", "productRef": product_dep})

app_product = add("app-product", {"isa": "PBXFileReference", "explicitFileType": "wrapper.application",
                                   "includeInIndex": "0", "path": f"{NAME}.app", "sourceTree": "BUILT_PRODUCTS_DIR"})
sources_group = add("group:sources", {"isa": "PBXGroup", "children": file_refs, "path": "Sources",
                                      "sourceTree": "<group>"})
products_group = add("group:products", {"isa": "PBXGroup", "children": [app_product], "name": "Products",
                                        "sourceTree": "<group>"})
main_group = add("group:main", {"isa": "PBXGroup", "children": [sources_group, products_group],
                                "sourceTree": "<group>"})

sources_phase = add("phase:sources", {"isa": "PBXSourcesBuildPhase", "buildActionMask": "2147483647",
                                      "files": build_files, "runOnlyForDeploymentPostprocessing": "0"})
frameworks_phase = add("phase:frameworks", {"isa": "PBXFrameworksBuildPhase", "buildActionMask": "2147483647",
                                            "files": [product_build], "runOnlyForDeploymentPostprocessing": "0"})
resources_phase = add("phase:resources", {"isa": "PBXResourcesBuildPhase", "buildActionMask": "2147483647",
                                          "files": [], "runOnlyForDeploymentPostprocessing": "0"})

project_common = {
    "ALWAYS_SEARCH_USER_PATHS": "NO",
    "CLANG_ENABLE_MODULES": "YES",
    "ENABLE_USER_SCRIPT_SANDBOXING": "YES",
    "IPHONEOS_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
    "SDKROOT": "iphoneos",
    "SWIFT_VERSION": "6.0",
}
project_debug = add("config:project:debug", {"isa": "XCBuildConfiguration", "name": "Debug", "buildSettings": {
    **project_common,
    "DEBUG_INFORMATION_FORMAT": "dwarf",
    "ENABLE_TESTABILITY": "YES",
    "GCC_OPTIMIZATION_LEVEL": "0",
    "ONLY_ACTIVE_ARCH": "YES",
    "SWIFT_ACTIVE_COMPILATION_CONDITIONS": "DEBUG $(inherited)",
    "SWIFT_OPTIMIZATION_LEVEL": "-Onone",
}})
project_release = add("config:project:release", {"isa": "XCBuildConfiguration", "name": "Release", "buildSettings": {
    **project_common,
    "DEBUG_INFORMATION_FORMAT": "dwarf-with-dsym",
    "SWIFT_COMPILATION_MODE": "wholemodule",
    "VALIDATE_PRODUCT": "YES",
}})

target_settings = {
    "ASSETCATALOG_COMPILER_APPICON_NAME": "",
    "CODE_SIGN_STYLE": "Automatic",
    "CURRENT_PROJECT_VERSION": "1",
    "ENABLE_PREVIEWS": "YES",
    "GENERATE_INFOPLIST_FILE": "YES",
    "INFOPLIST_KEY_CFBundleDisplayName": "Chiptune Tracker",
    "INFOPLIST_KEY_UIApplicationSceneManifest_Generation": "YES",
    "INFOPLIST_KEY_UIApplicationSupportsIndirectInputEvents": "YES",
    "INFOPLIST_KEY_UILaunchScreen_Generation": "YES",
    "INFOPLIST_KEY_UISupportedInterfaceOrientations_iPad":
        "UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown "
        "UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight",
    "INFOPLIST_KEY_UISupportedInterfaceOrientations_iPhone":
        "UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight",
    "LD_RUNPATH_SEARCH_PATHS": "$(inherited) @executable_path/Frameworks",
    "MARKETING_VERSION": "0.1",
    "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
    "PRODUCT_NAME": "$(TARGET_NAME)",
    "SWIFT_EMIT_LOC_STRINGS": "YES",
    "TARGETED_DEVICE_FAMILY": "1,2",
}
target_debug = add("config:target:debug", {"isa": "XCBuildConfiguration", "name": "Debug",
                                           "buildSettings": target_settings})
target_release = add("config:target:release", {"isa": "XCBuildConfiguration", "name": "Release",
                                               "buildSettings": target_settings})
project_configs = add("configs:project", {"isa": "XCConfigurationList",
                                          "buildConfigurations": [project_debug, project_release],
                                          "defaultConfigurationIsVisible": "0",
                                          "defaultConfigurationName": "Release"})
target_configs = add("configs:target", {"isa": "XCConfigurationList",
                                        "buildConfigurations": [target_debug, target_release],
                                        "defaultConfigurationIsVisible": "0",
                                        "defaultConfigurationName": "Release"})

target = add("target", {
    "isa": "PBXNativeTarget",
    "buildConfigurationList": target_configs,
    "buildPhases": [sources_phase, frameworks_phase, resources_phase],
    "buildRules": [],
    "dependencies": [],
    "name": NAME,
    "packageProductDependencies": [product_dep],
    "productName": NAME,
    "productReference": app_product,
    "productType": "com.apple.product-type.application",
})
root = add("project", {
    "isa": "PBXProject",
    "attributes": {"BuildIndependentTargetsInParallel": "1", "LastSwiftUpdateCheck": "2700",
                   "LastUpgradeCheck": "2700"},
    "buildConfigurationList": project_configs,
    "compatibilityVersion": "Xcode 15.0",
    "developmentRegion": "en",
    "hasScannedForEncodings": "0",
    "knownRegions": ["en", "Base"],
    "mainGroup": main_group,
    "packageReferences": [package_ref],
    "productRefGroup": products_group,
    "projectDirPath": "",
    "projectRoot": "",
    "targets": [target],
})

pbxproj = "// !$*UTF8*$!\n" + fmt({
    "archiveVersion": "1",
    "classes": {},
    "objectVersion": "60",
    "objects": dict(sorted(objects.items())),
    "rootObject": root,
}, 0) + "\n"

scheme = f"""<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "2700" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "YES" buildForRunning = "YES" buildForProfiling = "YES" buildForArchiving = "YES" buildForAnalyzing = "YES">
            <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{target}" BuildableName = "{NAME}.app" BlueprintName = "{NAME}" ReferencedContainer = "container:{NAME}.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" shouldUseLaunchSchemeArgsEnv = "YES">
   </TestAction>
   <LaunchAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" debugServiceExtension = "internal" allowLocationSimulation = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{target}" BuildableName = "{NAME}.app" BlueprintName = "{NAME}" ReferencedContainer = "container:{NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction buildConfiguration = "Release" shouldUseLaunchSchemeArgsEnv = "YES" savedToolIdentifier = "" useCustomWorkingDirectory = "NO" debugDocumentVersioning = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{target}" BuildableName = "{NAME}.app" BlueprintName = "{NAME}" ReferencedContainer = "container:{NAME}.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </ProfileAction>
   <AnalyzeAction buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
"""

(PROJECT / "xcshareddata" / "xcschemes").mkdir(parents=True, exist_ok=True)
(PROJECT / "project.pbxproj").write_text(pbxproj)
(PROJECT / "xcshareddata" / "xcschemes" / f"{NAME}.xcscheme").write_text(scheme)
print(f"wrote {PROJECT.relative_to(ROOT)} ({len(sources)} sources)")
