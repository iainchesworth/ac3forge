#!/usr/bin/env pwsh
#
# Platform-isolation guard.
#
# ac3forge branches on the operating system in CMake, never in the preprocessor:
# src/lib/CMakeLists.txt picks capture_wasapi.cpp or capture_stub.cpp (and the
# passthrough pair) for the target OS, so exactly one implementation is ever
# compiled. That only stays true if nobody reaches for an #ifdef, and an #ifdef
# is the path of least resistance the moment a second platform misbehaves --
# hence this check.
#
# The rule here is stricter than "no OS macros": NO preprocessor conditional of
# any kind is allowed in src/. The codebase has none today, so the check costs
# nothing to keep at zero, and zero is a far easier line to hold than "only the
# justified ones". Header-configuration defines that a platform header genuinely
# requires (WIN32_LEAN_AND_MEAN, NOMINMAX) belong in target_compile_definitions
# -- see the WIN32 block in src/lib/CMakeLists.txt for the worked example.
#
# Include guards are not affected: the codebase uses #pragma once.
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
$pattern = '^\s*#\s*(if|ifdef|ifndef|elif|elifdef|elifndef|else|endif)\b'

$srcRoot = Join-Path $Root 'src'
if (-not (Test-Path $srcRoot)) {
    Write-Error "No src/ directory under '$Root'. Pass -Root <repo-root>."
    exit 2
}

$files = Get-ChildItem -Path $srcRoot -Recurse -File -Include '*.h', '*.hpp', '*.cpp', '*.cc', '*.cxx', '*.inl'

$violations = @()
foreach ($file in $files) {
    foreach ($m in (Select-String -Path $file.FullName -Pattern $pattern -AllMatches -CaseSensitive)) {
        $violations += [pscustomobject]@{
            Path = [System.IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
            Line = $m.LineNumber
            Text = $m.Line.Trim()
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host ''
    Write-Host 'Platform-isolation violation: preprocessor conditional in src/.' -ForegroundColor Red
    Write-Host 'Per-OS code is selected by CMake (see the WIN32 block in src/lib/CMakeLists.txt),'
    Write-Host 'so it belongs in its own translation unit, not behind an #ifdef.'
    Write-Host ''
    foreach ($v in $violations) {
        Write-Host ('  {0}:{1}: {2}' -f $v.Path, $v.Line, $v.Text)
        # GitHub Actions annotation; prints harmlessly when run locally.
        Write-Host ('::error file={0},line={1}::Preprocessor conditional in src/ - select the platform in CMake instead' -f $v.Path, $v.Line)
    }
    Write-Host ''
    Write-Host ('{0} violation(s) found.' -f $violations.Count) -ForegroundColor Red
    exit 1
}

Write-Host "OK: no preprocessor conditionals in src/ ($($files.Count) files scanned)." -ForegroundColor Green
exit 0
