#!/usr/bin/env pwsh
#
# Platform-isolation guard.
#
# ac3forge branches on the operating system in CMake, never in the preprocessor:
# src/audio/CMakeLists.txt picks one src/audio/src/platform/<backend>/ directory
# (alsa/android/macos/pipewire/posix/windows) for the target OS, so exactly one
# audio_backend.cpp/capture.cpp/monitor.cpp/passthrough.cpp set is ever
# compiled. That only stays true if nobody reaches for an #ifdef, and an #ifdef
# is the path of least resistance the moment a second platform misbehaves --
# hence this check.
#
# The rule here is stricter than "no OS macros": NO preprocessor conditional of
# any kind is allowed in src/. The codebase has none today, so the check costs
# nothing to keep at zero, and zero is a far easier line to hold than "only the
# justified ones". Header-configuration defines that a platform header genuinely
# requires (WIN32_LEAN_AND_MEAN, NOMINMAX) belong in target_compile_definitions
# -- see the WIN32 block in src/audio/CMakeLists.txt for the worked example.
#
# Include guards are not affected: the codebase uses #pragma once.
#
# One other narrow exception, added for src/capi/include/ac3forge_c/ac3forge.h
# (roadmap F1): `#ifdef __cplusplus` / `extern "C" {` / `#endif` is the
# standard idiom that lets one header be included from both a C and a C++
# translation unit, which a C-callable public header genuinely needs -
# `extern "C"` is not even legal syntax outside `#ifdef __cplusplus`, since a
# .c file's compiler does not know the token. This is a LANGUAGE-DIALECT
# marker, not a platform or feature branch - the thing this check exists to
# forbid - so a bare `#ifdef __cplusplus ... #endif` pair (no #else/#elif
# inside it) is tracked separately below and excluded from violations; every
# other conditional, including one that merely mentions __cplusplus in an
# #if/#elif expression alongside something else, is still flagged.
#
# Usage:  ./scripts/check-platform-macros.ps1 [-Root <repo-root>]
# Exit:   0 = clean, 1 = violation(s) found, 2 = bad invocation.

param(
    [string]$Root = $PWD
)

$ErrorActionPreference = 'Stop'

# Any conditional-compilation directive. Deliberately broad: #if 0 to comment a
# block out, or a feature-flag #ifdef, are just as unwelcome as a platform one.
# `#define` is NOT matched -- constants and macros are ordinary C++ -- and
# neither is #include or #pragma.
$directivePattern = '^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b'
$cplusplusGuardPattern = '^\s*#\s*ifdef\s+__cplusplus\b'

$srcRoot = Join-Path $Root 'src'
if (-not (Test-Path $srcRoot)) {
    Write-Error "No src/ directory under '$Root'. Pass -Root <repo-root>."
    exit 2
}

# apps/ (the runnable-application tree - ac3cli, ac3gui, the Android and WASM
# demos) carries the same rule and is scanned alongside src/ once it exists.
# Optional rather than required: a repo state mid-way through the src/->apps/
# consolidation (or a checkout of an older tag, before apps/ existed at all)
# still has a valid src/ to scan even with no apps/ yet.
$scanRoots = @($srcRoot)
$appsRoot = Join-Path $Root 'apps'
if (Test-Path $appsRoot) {
    $scanRoots += $appsRoot
}

$files = Get-ChildItem -Path $scanRoots -Recurse -File -Include '*.h', '*.hpp', '*.cpp', '*.cc', '*.cxx', '*.inl'

$violations = @()
foreach ($file in $files) {
    # Per-file stack tracking ONLY whether each currently-open conditional is
    # exactly a plain `#ifdef __cplusplus` guard, so its matching `#endif` can
    # be recognised too - a bare regex match on `#endif` alone cannot tell
    # which opening directive it closes.
    $guardStack = New-Object System.Collections.Generic.Stack[bool]
    foreach ($m in (Select-String -Path $file.FullName -Pattern $directivePattern -AllMatches -CaseSensitive)) {
        $line = $m.Line.Trim()
        $isOpen = $line -match '^\s*#\s*(if|ifdef|ifndef)\b'
        $isEndif = $line -match '^\s*#\s*endif\b'

        if ($isOpen) {
            $isCplusplusGuard = $line -match $cplusplusGuardPattern
            $guardStack.Push($isCplusplusGuard)
            if ($isCplusplusGuard) {
                continue
            }
        } elseif ($isEndif -and $guardStack.Count -gt 0) {
            if ($guardStack.Pop()) {
                continue
            }
        }

        $violations += [pscustomobject]@{
            Path = [System.IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
            Line = $m.LineNumber
            Text = $line
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host ''
    Write-Host 'Platform-isolation violation: preprocessor conditional in src/ or apps/.' -ForegroundColor Red
    Write-Host 'Per-OS code is selected by CMake (see the WIN32 block in src/audio/CMakeLists.txt),'
    Write-Host 'so it belongs in its own translation unit, not behind an #ifdef.'
    Write-Host ''
    foreach ($v in $violations) {
        Write-Host ('  {0}:{1}: {2}' -f $v.Path, $v.Line, $v.Text)
        # GitHub Actions annotation; prints harmlessly when run locally.
        Write-Host ('::error file={0},line={1}::Preprocessor conditional in src/ or apps/ - select the platform in CMake instead' -f $v.Path, $v.Line)
    }
    Write-Host ''
    Write-Host ('{0} violation(s) found.' -f $violations.Count) -ForegroundColor Red
    exit 1
}

Write-Host "OK: no preprocessor conditionals in src/ or apps/ ($($files.Count) files scanned)." -ForegroundColor Green
exit 0
