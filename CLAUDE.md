# Kaptchi Flutter — Claude Instructions

## Before declaring any task complete

1. **Always run `flutter analyze`** after making Dart/Flutter changes.
   - Fix all errors and warnings before finishing.
   - Run it from the project root: `flutter analyze`

2. **For C++ changes** (`windows/runner/*.cpp` / `.h`), attempt a build check:
   - Run `flutter build windows` or check the build manually.
   - Pay special attention to: string concatenation operator precedence, missing `+` between `std::string` and adjacent string literals, and MSVC-specific syntax quirks.

3. **Never say "done"** until the above checks pass without errors.
